# DotNetBuild.cmake
# CMake module for building .NET projects as proper CMake targets
#
# This module provides functions to integrate .NET/C# projects into CMake builds
# using the dotnet CLI.
#
# Functions:
#   add_dotnet_library(target_name) - Add a .NET library target
#   add_dotnet_dependency(target dependent_target) - Make a target depend on a .NET target
#
# The following variables should be set before including this module:
#   DOTNET_EXE - Path to the dotnet executable (found via find_program)

if(NOT DEFINED DOTNET_EXE)
    find_program(DOTNET_EXE NAMES dotnet)
    if(NOT DOTNET_EXE)
        message(FATAL_ERROR "DotNetBuild: Could not find 'dotnet' executable")
    endif()
endif()

#[=============================================================================[
add_dotnet_library

Creates a CMake target that builds a .NET library using the dotnet CLI.

Usage:
    add_dotnet_library(<target_name>
        CSPROJ <path_to_csproj>
        [OUTPUT_DIR <output_directory>]
        [DEPENDS <dependency_targets>...]
        [SOURCES <source_files>...]
    )

Arguments:
    target_name     - Name of the CMake target to create
    CSPROJ          - Path to the .csproj file
    OUTPUT_DIR      - Output directory for built assemblies (default: ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
    DEPENDS         - Other targets this .NET project depends on
    SOURCES         - Source files to track for rebuild detection (auto-detected if not specified)

The function creates:
    - A custom target named <target_name> that builds the .NET project
    - Sets target properties for output paths
    - Configures proper rebuild detection based on source files

Example:
    add_dotnet_library(Coral.Managed
        CSPROJ ${CMAKE_CURRENT_SOURCE_DIR}/Coral.Managed/Coral.Managed.csproj
        OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin/$<CONFIG>
    )
#]=============================================================================]
function(add_dotnet_library TARGET_NAME)
    set(options "")
    set(oneValueArgs CSPROJ OUTPUT_DIR)
    set(multiValueArgs DEPENDS SOURCES)
    cmake_parse_arguments(DOTNET "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Validate required arguments
    if(NOT DOTNET_CSPROJ)
        message(FATAL_ERROR "add_dotnet_library: CSPROJ is required")
    endif()

    if(NOT EXISTS "${DOTNET_CSPROJ}")
        message(FATAL_ERROR "add_dotnet_library: CSPROJ file does not exist: ${DOTNET_CSPROJ}")
    endif()

    # Get the project directory and name
    get_filename_component(CSPROJ_DIR "${DOTNET_CSPROJ}" DIRECTORY)
    get_filename_component(CSPROJ_NAME "${DOTNET_CSPROJ}" NAME_WE)

    # Set default output directory
    if(NOT DOTNET_OUTPUT_DIR)
        set(DOTNET_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    endif()

    # Auto-detect sources if not provided
    if(NOT DOTNET_SOURCES)
        file(GLOB_RECURSE DOTNET_SOURCES CONFIGURE_DEPENDS
            "${CSPROJ_DIR}/*.cs"
        )
    endif()

    # Create stamp file path (configuration-independent for the target)
    set(STAMP_FILE "${CMAKE_BINARY_DIR}/${TARGET_NAME}.stamp")

    # Create the custom command that builds the .NET project
    add_custom_command(
        OUTPUT "${STAMP_FILE}"
        DEPENDS
            ${DOTNET_SOURCES}
            "${DOTNET_CSPROJ}"
        WORKING_DIRECTORY "${CSPROJ_DIR}"
        COMMENT "Building ${TARGET_NAME} (.NET)"
        VERBATIM
        COMMAND "${DOTNET_EXE}" build "${DOTNET_CSPROJ}"
                --configuration $<CONFIG>
                --output "${DOTNET_OUTPUT_DIR}"
                --nologo
                -v quiet
        COMMAND "${CMAKE_COMMAND}" -E touch "${STAMP_FILE}"
    )

    # Create the target
    add_custom_target(${TARGET_NAME} ALL
        DEPENDS "${STAMP_FILE}"
    )

    # Add dependencies if specified
    if(DOTNET_DEPENDS)
        add_dependencies(${TARGET_NAME} ${DOTNET_DEPENDS})
    endif()

    # Set target properties for use by other targets
    set_target_properties(${TARGET_NAME} PROPERTIES
        DOTNET_OUTPUT_DIR "${DOTNET_OUTPUT_DIR}"
        DOTNET_CSPROJ "${DOTNET_CSPROJ}"
        DOTNET_ASSEMBLY_NAME "${CSPROJ_NAME}"
    )

    # Create variables in parent scope for convenience
    set(${TARGET_NAME}_OUTPUT_DIR "${DOTNET_OUTPUT_DIR}" PARENT_SCOPE)
    set(${TARGET_NAME}_DLL "${DOTNET_OUTPUT_DIR}/${CSPROJ_NAME}.dll" PARENT_SCOPE)
    set(${TARGET_NAME}_DEPS_JSON "${DOTNET_OUTPUT_DIR}/${CSPROJ_NAME}.deps.json" PARENT_SCOPE)
    set(${TARGET_NAME}_RUNTIMECONFIG "${DOTNET_OUTPUT_DIR}/${CSPROJ_NAME}.runtimeconfig.json" PARENT_SCOPE)

endfunction()

#[=============================================================================[
dotnet_copy_to_target

Copies .NET assembly outputs to another target's output directory.

Usage:
    dotnet_copy_to_target(<dotnet_target> <native_target>)

Arguments:
    dotnet_target   - The .NET target created by add_dotnet_library
    native_target   - The native target to copy assemblies to

This adds a post-build command to native_target that copies the .NET
assembly and its supporting files to the native target's output directory.
#]=============================================================================]
function(dotnet_copy_to_target DOTNET_TARGET NATIVE_TARGET)
    get_target_property(DOTNET_OUTPUT ${DOTNET_TARGET} DOTNET_OUTPUT_DIR)
    get_target_property(ASSEMBLY_NAME ${DOTNET_TARGET} DOTNET_ASSEMBLY_NAME)

    if(NOT DOTNET_OUTPUT OR NOT ASSEMBLY_NAME)
        message(FATAL_ERROR "dotnet_copy_to_target: ${DOTNET_TARGET} is not a valid .NET target")
    endif()

    add_custom_command(TARGET ${NATIVE_TARGET} POST_BUILD
        COMMENT "Copying ${DOTNET_TARGET} assemblies to ${NATIVE_TARGET} output"
        VERBATIM
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DOTNET_OUTPUT}/${ASSEMBLY_NAME}.dll"
            "$<TARGET_FILE_DIR:${NATIVE_TARGET}>/${ASSEMBLY_NAME}.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DOTNET_OUTPUT}/${ASSEMBLY_NAME}.deps.json"
            "$<TARGET_FILE_DIR:${NATIVE_TARGET}>/${ASSEMBLY_NAME}.deps.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DOTNET_OUTPUT}/${ASSEMBLY_NAME}.runtimeconfig.json"
            "$<TARGET_FILE_DIR:${NATIVE_TARGET}>/${ASSEMBLY_NAME}.runtimeconfig.json"
    )
endfunction()

#[=============================================================================[
dotnet_add_publish

Creates a publish target for a .NET project (self-contained or framework-dependent).

Usage:
    dotnet_add_publish(<target_name>
        CSPROJ <path_to_csproj>
        [OUTPUT_DIR <output_directory>]
        [RUNTIME <runtime_identifier>]
        [SELF_CONTAINED <ON|OFF>]
        [DEPENDS <dependency_targets>...]
    )

Arguments:
    target_name     - Name of the CMake target to create
    CSPROJ          - Path to the .csproj file
    OUTPUT_DIR      - Output directory for published files
    RUNTIME         - .NET Runtime Identifier (e.g., win-x64, linux-x64, osx-x64)
    SELF_CONTAINED  - Whether to publish as self-contained (default: OFF)
    DEPENDS         - Other targets this publish depends on
#]=============================================================================]
function(dotnet_add_publish TARGET_NAME)
    set(options "")
    set(oneValueArgs CSPROJ OUTPUT_DIR RUNTIME SELF_CONTAINED)
    set(multiValueArgs DEPENDS)
    cmake_parse_arguments(DOTNET "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Validate required arguments
    if(NOT DOTNET_CSPROJ)
        message(FATAL_ERROR "dotnet_add_publish: CSPROJ is required")
    endif()

    if(NOT EXISTS "${DOTNET_CSPROJ}")
        message(FATAL_ERROR "dotnet_add_publish: CSPROJ file does not exist: ${DOTNET_CSPROJ}")
    endif()

    # Get the project directory
    get_filename_component(CSPROJ_DIR "${DOTNET_CSPROJ}" DIRECTORY)
    get_filename_component(CSPROJ_NAME "${DOTNET_CSPROJ}" NAME_WE)

    # Set defaults
    if(NOT DOTNET_OUTPUT_DIR)
        set(DOTNET_OUTPUT_DIR "${CMAKE_BINARY_DIR}/publish/${TARGET_NAME}/$<CONFIG>")
    endif()

    if(NOT DEFINED DOTNET_SELF_CONTAINED)
        set(DOTNET_SELF_CONTAINED OFF)
    endif()

    # Determine runtime identifier if not specified
    if(NOT DOTNET_RUNTIME)
        if(WIN32)
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(DOTNET_RUNTIME "win-x64")
            else()
                set(DOTNET_RUNTIME "win-x86")
            endif()
        elseif(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
                set(DOTNET_RUNTIME "osx-arm64")
            else()
                set(DOTNET_RUNTIME "osx-x64")
            endif()
        elseif(UNIX)
            if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
                set(DOTNET_RUNTIME "linux-arm64")
            else()
                set(DOTNET_RUNTIME "linux-x64")
            endif()
        endif()
    endif()

    # Build the publish command
    set(PUBLISH_ARGS
        "${DOTNET_CSPROJ}"
        --configuration $<CONFIG>
        --output "${DOTNET_OUTPUT_DIR}"
        --runtime "${DOTNET_RUNTIME}"
        --nologo
    )

    if(DOTNET_SELF_CONTAINED)
        list(APPEND PUBLISH_ARGS --self-contained true)
    else()
        list(APPEND PUBLISH_ARGS --self-contained false)
    endif()

    # Stamp file
    set(STAMP_FILE "${CMAKE_BINARY_DIR}/${TARGET_NAME}.publish.stamp")

    add_custom_command(
        OUTPUT "${STAMP_FILE}"
        DEPENDS "${DOTNET_CSPROJ}"
        WORKING_DIRECTORY "${CSPROJ_DIR}"
        COMMENT "Publishing ${TARGET_NAME} (.NET) for ${DOTNET_RUNTIME}"
        VERBATIM
        COMMAND "${DOTNET_EXE}" publish ${PUBLISH_ARGS}
        COMMAND "${CMAKE_COMMAND}" -E touch "${STAMP_FILE}"
    )

    add_custom_target(${TARGET_NAME}
        DEPENDS "${STAMP_FILE}"
    )

    if(DOTNET_DEPENDS)
        add_dependencies(${TARGET_NAME} ${DOTNET_DEPENDS})
    endif()

    # Set properties
    set_target_properties(${TARGET_NAME} PROPERTIES
        DOTNET_OUTPUT_DIR "${DOTNET_OUTPUT_DIR}"
        DOTNET_RUNTIME "${DOTNET_RUNTIME}"
    )

endfunction()
