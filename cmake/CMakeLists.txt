cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
project(Coral
    VERSION 1.0.0
    DESCRIPTION "A .NET Core hosting library for native C++ applications"
    LANGUAGES CXX
)

#------------------------------------------------------------------------------
# Options
#------------------------------------------------------------------------------
option(CORAL_EXAMPLE "Build the example programs" OFF)
option(CORAL_TESTING "Build the tests" ON)

#------------------------------------------------------------------------------
# CMake Module Path
#------------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/modules")

#------------------------------------------------------------------------------
# Global Configuration (matches premake workspace settings)
#------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match premake's output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Build/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Build/$<CONFIG>)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Build/$<CONFIG>)

# Global defines (from premake5-native.lua workspace)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

#------------------------------------------------------------------------------
# .NET SDK Detection
#------------------------------------------------------------------------------
find_program(DOTNET_EXE NAMES dotnet REQUIRED)

execute_process(
    COMMAND ${DOTNET_EXE} --version
    OUTPUT_VARIABLE DOTNET_SDKVER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT DOTNET_SDKVER)
    message(FATAL_ERROR "Failed to check .NET version!")
endif()
if(DOTNET_SDKVER VERSION_LESS "9.0.0")
    message(FATAL_ERROR "No suitable .NET SDK found, .NET 9+ is required.")
endif()
message(STATUS "Found a suitable .NET SDK, version ${DOTNET_SDKVER}")

execute_process(
    COMMAND ${DOTNET_EXE} --list-runtimes
    OUTPUT_VARIABLE DOTNET_RUNTIMES
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT DOTNET_RUNTIMES)
    message(FATAL_ERROR "No .NET runtimes found, .NET 9+ is required.")
endif()

set(DOTNET_RTVER "")
string(REPLACE "\r\n" "\n" DOTNET_RUNTIMES "${DOTNET_RUNTIMES}")
string(REPLACE "\r" "\n" DOTNET_RUNTIMES "${DOTNET_RUNTIMES}")
string(REPLACE "\n" ";" DOTNET_RTLINES "${DOTNET_RUNTIMES}")
foreach(line IN LISTS DOTNET_RTLINES)
    string(REGEX REPLACE " +" ";" parts "${line}")
    list(LENGTH parts len)
    if(len GREATER 1)
        list(GET parts 0 name)
        list(GET parts 1 ver)
        if(name STREQUAL "Microsoft.NETCore.App" AND ver VERSION_GREATER "9.0.0")
            if(DOTNET_RTVER STREQUAL "" OR ver VERSION_GREATER DOTNET_RTVER)
                set(DOTNET_RTVER "${ver}")
            endif()
        endif()
    endif()
endforeach()
if(NOT DOTNET_RTVER)
    message(FATAL_ERROR "No suitable .NET runtimes found, .NET 9+ is required.")
endif()
message(STATUS "Found a suitable .NET runtime, version ${DOTNET_RTVER}")

#------------------------------------------------------------------------------
# Include DotNet Build Module
#------------------------------------------------------------------------------
include(DotNetBuild)

#------------------------------------------------------------------------------
# Paths
#------------------------------------------------------------------------------
set(CORAL_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(CORAL_ROOT "${CORAL_ROOT}" ABSOLUTE)

# Common output directory for managed assemblies
set(CORAL_MANAGED_OUTPUT_DIR ${CMAKE_BINARY_DIR}/Build/$<CONFIG>)

#------------------------------------------------------------------------------
# Coral.Managed Target
#------------------------------------------------------------------------------
add_dotnet_library(Coral.Managed
    CSPROJ ${CORAL_ROOT}/Coral.Managed/Coral.Managed-Static.csproj
    OUTPUT_DIR ${CORAL_MANAGED_OUTPUT_DIR}
)

#------------------------------------------------------------------------------
# Coral.Native Static Library
#------------------------------------------------------------------------------
add_library(Coral.Native STATIC)

# Create alias for use with add_subdirectory / FetchContent
add_library(Coral::Native ALIAS Coral.Native)

# Depend on managed assembly
add_dependencies(Coral.Native Coral.Managed)

# C++17 standard
target_compile_features(Coral.Native PUBLIC cxx_std_17)

# Static runtime off (shared runtime) - CMake default is usually fine
# For MSVC, this controls /MD vs /MT
if(MSVC)
    set_property(TARGET Coral.Native PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Source files
file(GLOB_RECURSE CORAL_NATIVE_SOURCES
    ${CORAL_ROOT}/Coral.Native/Source/*.cpp
    ${CORAL_ROOT}/Coral.Native/Source/*.hpp
)

file(GLOB CORAL_NATIVE_HEADERS
    ${CORAL_ROOT}/Coral.Native/Include/Coral/*.hpp
)

target_sources(Coral.Native
    PRIVATE ${CORAL_NATIVE_SOURCES}
    PRIVATE ${CORAL_NATIVE_HEADERS}
)

# Include directories - use generator expressions for proper install/build separation
target_include_directories(Coral.Native
    PUBLIC
        $<BUILD_INTERFACE:${CORAL_ROOT}/Coral.Native/Include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CORAL_ROOT}/Coral.Native/Source
)

# External include directories (system includes to suppress warnings)
target_include_directories(Coral.Native SYSTEM
    PRIVATE ${CORAL_ROOT}/NetCore
)

# Precompiled header (CMake 3.16+)
target_precompile_headers(Coral.Native
    PRIVATE ${CORAL_ROOT}/Coral.Native/Source/CoralPCH.hpp
)

# Platform-specific defines (from premake)
if(WIN32)
    target_compile_definitions(Coral.Native PRIVATE CORAL_WINDOWS)
elseif(APPLE)
    target_compile_definitions(Coral.Native PRIVATE CORAL_MACOSX)
elseif(UNIX)
    target_compile_definitions(Coral.Native PRIVATE CORAL_LINUX)
endif()

# Configuration-specific settings
target_compile_definitions(Coral.Native
    PRIVATE
        $<$<CONFIG:Debug>:CORAL_DEBUG>
        $<$<CONFIG:Release>:CORAL_RELEASE>
        $<$<CONFIG:RelWithDebInfo>:CORAL_RELEASE>
        $<$<CONFIG:MinSizeRel>:CORAL_RELEASE>
)

# Debug symbols and optimization
if(MSVC)
    target_compile_options(Coral.Native PRIVATE
        $<$<CONFIG:Debug>:/Zi /Od>
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/Zi /O2>
        $<$<CONFIG:MinSizeRel>:/O1>
    )
else()
    target_compile_options(Coral.Native PRIVATE
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-g -O2>
        $<$<CONFIG:MinSizeRel>:-Os>
    )
endif()

#------------------------------------------------------------------------------
# Example.Native Executable
#------------------------------------------------------------------------------
if(CORAL_EXAMPLE)
    set(EXAMPLE_ROOT ${CORAL_ROOT}/Example)
    set(EXAMPLE_OUTPUT_DIR ${CMAKE_BINARY_DIR}/Build/$<CONFIG>/Example)

    # Example.Managed
    add_dotnet_library(Example.Managed
        CSPROJ ${EXAMPLE_ROOT}/Example.Managed/Example.Managed-Static.csproj
        OUTPUT_DIR ${EXAMPLE_OUTPUT_DIR}
        DEPENDS Coral.Managed
    )

    # Example.Native executable
    add_executable(Example.Native)

    add_dependencies(Example.Native Example.Managed)

    # Output directory
    set_target_properties(Example.Native PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${EXAMPLE_OUTPUT_DIR}
    )

    # MSVC runtime library
    if(MSVC)
        set_property(TARGET Example.Native PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    # Source files
    file(GLOB EXAMPLE_NATIVE_SOURCES
        ${EXAMPLE_ROOT}/Example.Native/Source/*.cpp
        ${EXAMPLE_ROOT}/Example.Native/Source/*.hpp
    )
    target_sources(Example.Native PRIVATE ${EXAMPLE_NATIVE_SOURCES})

    # Link to Coral.Native
    target_link_libraries(Example.Native PRIVATE Coral.Native)

    # Configuration-specific defines (from premake)
    target_compile_definitions(Example.Native PRIVATE
        $<$<CONFIG:Debug>:CORAL_EXAMPLE_DEBUG>
        $<$<CONFIG:Release>:CORAL_EXAMPLE_RELEASE>
        $<$<CONFIG:RelWithDebInfo>:CORAL_EXAMPLE_RELEASE>
        $<$<CONFIG:MinSizeRel>:CORAL_EXAMPLE_RELEASE>
    )

    # Debug symbols and optimization
    if(MSVC)
        target_compile_options(Example.Native PRIVATE
            $<$<CONFIG:Debug>:/Zi /Od>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:RelWithDebInfo>:/Zi /O2>
            $<$<CONFIG:MinSizeRel>:/O1>
        )
    else()
        target_compile_options(Example.Native PRIVATE
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-g -O2>
            $<$<CONFIG:MinSizeRel>:-Os>
        )
    endif()

    # Copy Coral.Managed to Example output directory
    dotnet_copy_to_target(Coral.Managed Example.Native)
endif()

#------------------------------------------------------------------------------
# Testing.Native Executable
#------------------------------------------------------------------------------
if(CORAL_TESTING)
    set(TESTING_ROOT ${CORAL_ROOT}/Tests)
    set(TESTING_OUTPUT_DIR ${CMAKE_BINARY_DIR}/Build/$<CONFIG>/Tests)

    # Testing.Managed
    add_dotnet_library(Testing.Managed
        CSPROJ ${TESTING_ROOT}/Testing.Managed/Testing.Managed-Static.csproj
        OUTPUT_DIR ${TESTING_OUTPUT_DIR}
        DEPENDS Coral.Managed
    )

    # Testing.Native executable
    add_executable(Testing.Native)

    add_dependencies(Testing.Native Testing.Managed)

    # Output directory
    set_target_properties(Testing.Native PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${TESTING_OUTPUT_DIR}
    )

    # MSVC runtime library
    if(MSVC)
        set_property(TARGET Testing.Native PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    # Source files
    file(GLOB TESTING_NATIVE_SOURCES
        ${TESTING_ROOT}/Testing.Native/Source/*.cpp
        ${TESTING_ROOT}/Testing.Native/Source/*.hpp
    )
    target_sources(Testing.Native PRIVATE ${TESTING_NATIVE_SOURCES})

    # Link to Coral.Native
    target_link_libraries(Testing.Native PRIVATE Coral.Native)

    # Configuration-specific defines (from premake)
    target_compile_definitions(Testing.Native PRIVATE
        $<$<CONFIG:Debug>:CORAL_TESTING_DEBUG>
        $<$<CONFIG:Release>:CORAL_TESTING_RELEASE>
        $<$<CONFIG:RelWithDebInfo>:CORAL_TESTING_RELEASE>
        $<$<CONFIG:MinSizeRel>:CORAL_TESTING_RELEASE>
    )

    # Debug symbols and optimization
    if(MSVC)
        target_compile_options(Testing.Native PRIVATE
            $<$<CONFIG:Debug>:/Zi /Od>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:RelWithDebInfo>:/Zi /O2>
            $<$<CONFIG:MinSizeRel>:/O1>
        )
    else()
        target_compile_options(Testing.Native PRIVATE
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-g -O2>
            $<$<CONFIG:MinSizeRel>:-Os>
        )
    endif()

    # Copy Coral.Managed to Testing output directory
    dotnet_copy_to_target(Coral.Managed Testing.Native)

    # Enable CTest
    enable_testing()
    add_test(NAME CoralTests COMMAND Testing.Native WORKING_DIRECTORY ${TESTING_OUTPUT_DIR})
endif()

#------------------------------------------------------------------------------
# IDE Organization (for Visual Studio, Xcode, etc.)
#------------------------------------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Group managed targets
if(TARGET Coral.Managed)
    set_target_properties(Coral.Managed PROPERTIES FOLDER "Managed")
endif()
if(TARGET Example.Managed)
    set_target_properties(Example.Managed PROPERTIES FOLDER "Example")
endif()
if(TARGET Testing.Managed)
    set_target_properties(Testing.Managed PROPERTIES FOLDER "Tests")
endif()

# Group native targets
if(TARGET Example.Native)
    set_target_properties(Example.Native PROPERTIES FOLDER "Example")
endif()
if(TARGET Testing.Native)
    set_target_properties(Testing.Native PROPERTIES FOLDER "Tests")
endif()

#------------------------------------------------------------------------------
# Installation and Package Export
#------------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Set installation directories
set(CORAL_INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/Coral)
set(CORAL_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
set(CORAL_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})

# Install the native library
install(TARGETS Coral.Native
    EXPORT CoralTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install headers
install(DIRECTORY ${CORAL_ROOT}/Coral.Native/Include/Coral
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# Install NetCore headers (required for hostfxr)
install(DIRECTORY ${CORAL_ROOT}/NetCore/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Coral/NetCore
    FILES_MATCHING PATTERN "*.h"
)

# Export targets
install(EXPORT CoralTargets
    FILE CoralTargets.cmake
    NAMESPACE Coral::
    DESTINATION ${CORAL_INSTALL_CONFIGDIR}
)

# Create version file
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CoralConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Configure package config file
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/CoralConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CoralConfig.cmake
    INSTALL_DESTINATION ${CORAL_INSTALL_CONFIGDIR}
    PATH_VARS CORAL_INSTALL_INCLUDEDIR CORAL_INSTALL_LIBDIR
)

# Install config files
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/CoralConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/CoralConfigVersion.cmake
    DESTINATION ${CORAL_INSTALL_CONFIGDIR}
)

# Export build tree targets (for use without installing)
export(EXPORT CoralTargets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/CoralTargets.cmake
    NAMESPACE Coral::
)

# Register package in user's package registry (optional)
export(PACKAGE Coral)

#------------------------------------------------------------------------------
# Summary
#------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Coral Configuration Summary:")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build Type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  .NET SDK:       ${DOTNET_SDKVER}")
message(STATUS "  .NET Runtime:   ${DOTNET_RTVER}")
message(STATUS "  Build Example:  ${CORAL_EXAMPLE}")
message(STATUS "  Build Tests:    ${CORAL_TESTING}")
message(STATUS "")
