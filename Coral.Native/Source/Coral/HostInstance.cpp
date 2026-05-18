#include "Coral/HostInstance.hpp"
#include "Coral/StringHelper.hpp"
#include "Coral/TypeCache.hpp"

#include "Verify.hpp"
#include "HostFXRErrorCodes.hpp"
#include "CoralManagedFunctions.hpp"

#include <cstdlib>   // setenv / _putenv_s
#include <string>
#include <thread>

#ifdef CORAL_WINDOWS
	#include <ShlObj_core.h>  // pulls in <windows.h> for SetEnvironmentVariableA, WaitNamedPipeA, GetCurrentProcessId
#else
	#include <dlfcn.h>
	#include <sys/types.h>
	#include <unistd.h>       // getpid
#endif

namespace Coral {

	// ---------------------------------------------------------------
	// Helpers for the external-debugger-attach support added to
	// HostSettings. Kept in this TU's anonymous namespace because they
	// only matter at Initialize time and have no public surface.
	// ---------------------------------------------------------------
	namespace {

		// Set a process-level env var WITHOUT overwriting an existing
		// value. We want this behavior so a developer who explicitly
		// disabled diagnostics for a perf-test run (DOTNET_EnableDiagnostics=0
		// in their shell) still wins; we only fill in the unset case.
		void SetEnvIfUnset(const char* name, const char* value)
		{
#ifdef CORAL_WINDOWS
			// GetEnvironmentVariableA returns 0 + sets ERROR_ENVVAR_NOT_FOUND
			// when the variable doesn't exist. _putenv_s + SetEnvironmentVariableA
			// both work; we use SetEnvironmentVariableA so the value is
			// visible to subsequent SDK / hostfxr / runtime queries via
			// GetEnvironmentVariableA, which is what they use internally.
			char buf[8];
			DWORD got = GetEnvironmentVariableA(name, buf, sizeof(buf));
			if (got == 0)  // not set
			{
				SetEnvironmentVariableA(name, value);
			}
#else
			setenv(name, value, /*overwrite=*/0);
#endif
		}

		// Force-enable the .NET diagnostics IPC + debugger + profiler
		// subsystems. Called before hostfxr loads the runtime, because
		// the CLR reads these during Debugger::Startup which fires
		// during hostfxr_initialize_for_runtime_config - setting them
		// after that is a no-op.
		//
		// All four default to "1" in a stock runtime, but some hosting
		// contexts (corporate-policy GPOs, IIS-style hosts, certain CI
		// runners that proxy through a launcher) inherit "0" from their
		// parent process. Setting them here makes the embed's
		// debuggability invariant under the parent's environment.
		void EnableDotnetDiagnosticsEnv()
		{
			SetEnvIfUnset("DOTNET_EnableDiagnostics",          "1");
			SetEnvIfUnset("DOTNET_EnableDiagnostics_IPC",      "1");
			SetEnvIfUnset("DOTNET_EnableDiagnostics_Debugger", "1");
			SetEnvIfUnset("DOTNET_EnableDiagnostics_Profiler", "1");
		}

		// Poll for the existence of the .NET diagnostic IPC pipe.
		// Returns true if the pipe came up within `timeout`, false on
		// timeout (silent - caller decides whether to log).
		//
		// The pipe naming convention is documented in
		// dotnet/diagnostics:
		//   Windows: \\.\pipe\dotnet-diagnostic-<pid>
		//   POSIX:   ${TMPDIR:-/tmp}/dotnet-diagnostic-<pid>-<start_time>-socket
		//
		// On Windows we use WaitNamedPipeA with a 0ms wait per probe so
		// we get an immediate "exists?" answer without consuming a
		// server instance. ERROR_PIPE_BUSY and ERROR_SEM_TIMEOUT both
		// mean "pipe is up, just couldn't open a new client" - those
		// count as success for our purposes.
		bool WaitForDiagnosticPipeUp(std::chrono::milliseconds timeout)
		{
#ifdef CORAL_WINDOWS
			const std::string pipeName =
				std::string("\\\\.\\pipe\\dotnet-diagnostic-") +
				std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));

			const auto deadline = std::chrono::steady_clock::now() + timeout;
			for (;;)
			{
				if (WaitNamedPipeA(pipeName.c_str(), 0))
				{
					return true;  // a free server instance exists
				}
				const DWORD err = GetLastError();
				if (err == ERROR_PIPE_BUSY || err == ERROR_SEM_TIMEOUT)
				{
					return true;  // pipe exists, just busy with someone else
				}
				if (std::chrono::steady_clock::now() >= deadline)
				{
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
#else
			// POSIX: the runtime drops a Unix domain socket at
			// $TMPDIR/dotnet-diagnostic-<pid>-<starttime>-socket. We
			// just look for any path matching the prefix; we don't
			// connect to it.
			const char* tmpdir = std::getenv("TMPDIR");
			if (tmpdir == nullptr || *tmpdir == '\0') tmpdir = "/tmp";
			const std::string prefix =
				std::string("dotnet-diagnostic-") +
				std::to_string(static_cast<long>(getpid())) + "-";

			const auto deadline = std::chrono::steady_clock::now() + timeout;
			for (;;)
			{
				std::error_code ec;
				for (auto& entry : std::filesystem::directory_iterator(tmpdir, ec))
				{
					if (!entry.is_socket(ec)) continue;
					const auto& name = entry.path().filename().string();
					if (name.rfind(prefix, 0) == 0) return true;
				}
				if (std::chrono::steady_clock::now() >= deadline)
				{
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
#endif
		}

	} // anonymous namespace


	struct CoreCLRFunctions
	{
		hostfxr_set_error_writer_fn SetHostFXRErrorWriter = nullptr;
		hostfxr_set_runtime_property_value_fn SetRuntimePropertyValue = nullptr;
		hostfxr_initialize_for_runtime_config_fn InitHostFXRForRuntimeConfig = nullptr;
		hostfxr_get_runtime_delegate_fn GetRuntimeDelegate = nullptr;
		hostfxr_close_fn CloseHostFXR = nullptr;
		load_assembly_and_get_function_pointer_fn GetManagedFunctionPtr = nullptr;
	};
	static CoreCLRFunctions s_CoreCLRFunctions;

	static MessageCallbackFn MessageCallback = nullptr;
	static MessageLevel MessageFilter;
	static ExceptionCallbackFn ExceptionCallback = nullptr;

	static void DefaultMessageCallback(std::string_view InMessage, MessageLevel InLevel)
	{
		const char* level = "";

		switch (InLevel)
		{
		default: break;
		case MessageLevel::Trace:
			level = "Trace";
			break;
		case MessageLevel::Info:
			level = "Info";
			break;
		case MessageLevel::Warning:
			level = "Warn";
			break;
		case MessageLevel::Error:
			level = "Error";
			break;
		}

		std::cout << "[Coral](" << level << "): " << InMessage << std::endl;
	}

	CoralInitStatus HostInstance::Initialize(HostSettings InSettings)
	{
		CORAL_VERIFY(!m_Initialized);

		// Setup settings (moved up so EnableManagedDebugger is consulted
		// BEFORE LoadHostFXR - the CLR reads DOTNET_EnableDiagnostics*
		// during Debugger::Startup which runs inside
		// hostfxr_initialize_for_runtime_config; setting them after
		// LoadHostFXR is too late.)
		m_Settings = std::move(InSettings);

		if (!m_Settings.MessageCallback)
			m_Settings.MessageCallback = DefaultMessageCallback;
		MessageCallback = m_Settings.MessageCallback;
		MessageFilter = m_Settings.MessageFilter;

		if (m_Settings.EnableManagedDebugger)
		{
			EnableDotnetDiagnosticsEnv();
		}

		if (!LoadHostFXR())
		{
			return CoralInitStatus::DotNetNotFound;
		}

		s_CoreCLRFunctions.SetHostFXRErrorWriter([](const UCChar* InMessage)
		{
			auto message = StringHelper::ConvertWideToUtf8(InMessage);
			MessageCallback(message, MessageLevel::Error);
		});

		m_CoralManagedAssemblyPath = std::filesystem::path(m_Settings.CoralDirectory) / "Coral.Managed.dll";

		if (!std::filesystem::exists(m_CoralManagedAssemblyPath))
		{
			MessageCallback("Failed to find Coral.Managed.dll", MessageLevel::Error);
			return CoralInitStatus::CoralManagedNotFound;
		}

		if (!InitializeCoralManaged())
		{
			return CoralInitStatus::CoralManagedInitError;
		}

		// After the runtime is up and the first managed call has fired
		// (inside InitializeCoralManaged), the diagnostic IPC server
		// thread still needs a moment to publish its named pipe / Unix
		// socket. If we return from Initialize before that publish, any
		// debugger attach probed during the gap gets E_INVALIDARG. The
		// wait is bounded + silent so we never deadlock startup.
		if (m_Settings.EnableManagedDebugger && m_Settings.WaitForDiagnosticPipe)
		{
			if (!WaitForDiagnosticPipeUp(m_Settings.DiagnosticPipeWaitTimeout))
			{
				// Not fatal - debugger attach often still works without
				// the IPC pipe (it's used by dotnet-counters/-trace/-dump,
				// not by vsdbg's ICorDebug attach itself), but log so
				// debug-attach failures correlate with the missing pipe
				// in the user's editor log.
				MessageCallback(
					"[Coral] diagnostic IPC pipe didn't appear within timeout; "
					"external managed-debugger attach may need a retry.",
					MessageLevel::Warning);
			}
		}

		return CoralInitStatus::Success;
	}

	void HostInstance::Shutdown()
	{
		s_CoreCLRFunctions.CloseHostFXR(m_HostFXRContext);
	}
	
	AssemblyLoadContext HostInstance::CreateAssemblyLoadContext(std::string_view InName)
	{
		ScopedString name = String::New(InName);
		ScopedString dllPath = String::New("");
		AssemblyLoadContext alc;
		alc.m_ContextId = s_ManagedFunctions.CreateAssemblyLoadContextFptr(name, dllPath);
		alc.m_Host = this;
		return alc;
	}

	AssemblyLoadContext HostInstance::CreateAssemblyLoadContext(std::string_view InName, std::string_view InDllPath)
	{
		ScopedString name = String::New(InName);
		ScopedString dllPath = String::New(InDllPath);
		AssemblyLoadContext alc;
		alc.m_ContextId = s_ManagedFunctions.CreateAssemblyLoadContextFptr(name, dllPath);
		alc.m_Host = this;
		return alc;
	}

	void HostInstance::UnloadAssemblyLoadContext(AssemblyLoadContext& InLoadContext)
	{
		s_ManagedFunctions.UnloadAssemblyLoadContextFptr(InLoadContext.m_ContextId);
		InLoadContext.m_ContextId = -1;
		InLoadContext.m_LoadedAssemblies.Clear();
	}

#ifdef CORAL_WINDOWS
	template <typename TFunc>
	TFunc LoadFunctionPtr(void* InLibraryHandle, const char* InFunctionName)
	{
		auto result = (TFunc)GetProcAddress((HMODULE)InLibraryHandle, InFunctionName);
		CORAL_VERIFY(result);
		return result;
	}
#else
	template <typename TFunc>
	TFunc LoadFunctionPtr(void* InLibraryHandle, const char* InFunctionName)
	{
		auto result = (TFunc)dlsym(InLibraryHandle, InFunctionName);
		CORAL_VERIFY(result);
		return result;
	}
#endif

	static std::filesystem::path GetHostFXRPath()
	{
#ifdef CORAL_WINDOWS
		std::filesystem::path basePath = "";
		
		// Find the Program Files folder
		TCHAR pf[MAX_PATH];
		SHGetSpecialFolderPath(
		nullptr,
		pf,
		CSIDL_PROGRAM_FILES,
		FALSE);

		basePath = pf;
		basePath /= "dotnet/host/fxr/";

		auto searchPaths = std::array
		{
			basePath
		};
#elif defined(CORAL_APPLE)
		auto searchPaths = std::array
		{
			std::filesystem::path("/usr/local/share/dotnet/host/fxr/"),
			std::filesystem::path("/usr/share/dotnet/host/fxr/")
		};
#else
		auto searchPaths = std::array
		{
			std::filesystem::path("/usr/local/lib/dotnet/host/fxr/"),
			std::filesystem::path("/usr/local/lib64/dotnet/host/fxr/"),
			std::filesystem::path("/usr/local/share/dotnet/host/fxr/"),

			std::filesystem::path("/usr/lib/dotnet/host/fxr/"),
			std::filesystem::path("/usr/lib64/dotnet/host/fxr/"),
			std::filesystem::path("/usr/share/dotnet/host/fxr/")
		};
#endif

		for (const auto& path : searchPaths)
		{
			if (!std::filesystem::exists(path))
				continue;

			for (const auto& dir : std::filesystem::recursive_directory_iterator(path))
			{
				if (!dir.is_directory())
					continue;

				auto dirPath = dir.path().filename();
				char version = dirPath.string()[0];

				if (version != '9')
					continue;

				auto res = dir / std::filesystem::path(CORAL_HOSTFXR_NAME);
				CORAL_VERIFY(std::filesystem::exists(res));
				return res;
			}
		}

		return "";
	}

	bool HostInstance::LoadHostFXR() const
	{
		// Retrieve the file path to the CoreCLR library
		auto hostfxrPath = GetHostFXRPath();

		if (hostfxrPath.empty())
		{
			return false;
		}

		// Load the CoreCLR library
		void* libraryHandle = nullptr;

#ifdef CORAL_WINDOWS
	#ifdef CORAL_WIDE_CHARS
		libraryHandle = LoadLibraryW(hostfxrPath.c_str());
	#else
		libraryHandle = LoadLibraryA(hostfxrPath.string().c_str());
	#endif
#else
		libraryHandle = dlopen(hostfxrPath.string().data(), RTLD_NOW | RTLD_GLOBAL);
#endif

		if (libraryHandle == nullptr)
		{
			return false;
		}

		// Load CoreCLR functions
		s_CoreCLRFunctions.SetHostFXRErrorWriter = LoadFunctionPtr<hostfxr_set_error_writer_fn>(libraryHandle, "hostfxr_set_error_writer");
		s_CoreCLRFunctions.SetRuntimePropertyValue = LoadFunctionPtr<hostfxr_set_runtime_property_value_fn>(libraryHandle, "hostfxr_set_runtime_property_value");
		s_CoreCLRFunctions.InitHostFXRForRuntimeConfig = LoadFunctionPtr<hostfxr_initialize_for_runtime_config_fn>(libraryHandle, "hostfxr_initialize_for_runtime_config");
		s_CoreCLRFunctions.GetRuntimeDelegate = LoadFunctionPtr<hostfxr_get_runtime_delegate_fn>(libraryHandle, "hostfxr_get_runtime_delegate");
		s_CoreCLRFunctions.CloseHostFXR = LoadFunctionPtr<hostfxr_close_fn>(libraryHandle, "hostfxr_close");

		return true;
	}
	
	bool HostInstance::InitializeCoralManaged()
	{
		// Fetch load_assembly_and_get_function_pointer_fn from CoreCLR
		{
			auto runtimeConfigPath = std::filesystem::path(m_Settings.CoralDirectory) / "Coral.Managed.runtimeconfig.json";

			if (!std::filesystem::exists(runtimeConfigPath))
			{
				MessageCallback("Failed to find Coral.Managed.runtimeconfig.json", MessageLevel::Error);
				return false;
			}

			int status = s_CoreCLRFunctions.InitHostFXRForRuntimeConfig(runtimeConfigPath.c_str(), nullptr, &m_HostFXRContext);
			CORAL_VERIFY(status == StatusCode::Success || status == StatusCode::Success_HostAlreadyInitialized || status == StatusCode::Success_DifferentRuntimeProperties);
			CORAL_VERIFY(m_HostFXRContext != nullptr);

			std::filesystem::path coralDirectoryPath = m_Settings.CoralDirectory;
			s_CoreCLRFunctions.SetRuntimePropertyValue(m_HostFXRContext, CORAL_STR("APP_CONTEXT_BASE_DIRECTORY"), coralDirectoryPath.c_str());

			if (m_Settings.EnableManagedDebugger)
			{
				// Force-enable MetadataUpdater regardless of what the
				// runtimeconfig.json says. The SDK defaults this to false
				// for Release/Profile builds of class libraries, which
				// silently disables parts of Debugger::Startup's EnC
				// sync and causes ICorDebug::DebugActiveProcess to
				// return E_INVALIDARG from external attach attempts.
				// Setting it as a runtime property here overrides the
				// runtimeconfig value, so this works even when paired
				// with a stock Coral.Managed-Static.csproj (no
				// RuntimeHostConfigurationOption needed).
				s_CoreCLRFunctions.SetRuntimePropertyValue(
					m_HostFXRContext,
					CORAL_STR("System.Reflection.Metadata.MetadataUpdater.IsSupported"),
					CORAL_STR("true"));
			}

			status = s_CoreCLRFunctions.GetRuntimeDelegate(m_HostFXRContext, hdt_load_assembly_and_get_function_pointer, (void**) &s_CoreCLRFunctions.GetManagedFunctionPtr);
			CORAL_VERIFY(status == StatusCode::Success);
		}

		using InitializeFn = void(*)(void(*)(String, MessageLevel), void(*)(String));
		InitializeFn coralManagedEntryPoint = nullptr;
		coralManagedEntryPoint = LoadCoralManagedFunctionPtr<InitializeFn>(CORAL_STR("Coral.Managed.ManagedHost, Coral.Managed"), CORAL_STR("Initialize"));

		LoadCoralFunctions();

		coralManagedEntryPoint([](String InMessage, MessageLevel InLevel)
		{
			if (MessageFilter & InLevel)
			{
				std::string message = InMessage;
				MessageCallback(message, InLevel);
			}
		},
		[](String InMessage)
		{
			std::string message = InMessage;
			if (!ExceptionCallback)
			{
				MessageCallback(message, MessageLevel::Error);
				return;
			}
			
			ExceptionCallback(message);
		});

		ExceptionCallback = m_Settings.ExceptionCallback;

		return true;
	}

	void HostInstance::LoadCoralFunctions()
	{
		s_ManagedFunctions.CreateAssemblyLoadContextFptr = LoadCoralManagedFunctionPtr<CreateAssemblyLoadContextFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("CreateAssemblyLoadContext"));
		s_ManagedFunctions.UnloadAssemblyLoadContextFptr = LoadCoralManagedFunctionPtr<UnloadAssemblyLoadContextFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("UnloadAssemblyLoadContext"));
		s_ManagedFunctions.LoadAssemblyFptr = LoadCoralManagedFunctionPtr<LoadAssemblyFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("LoadAssembly"));
		s_ManagedFunctions.LoadAssemblyFromMemoryFptr = LoadCoralManagedFunctionPtr<LoadAssemblyFromMemoryFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("LoadAssemblyFromMemory"));
		s_ManagedFunctions.UnloadAssemblyLoadContextFptr = LoadCoralManagedFunctionPtr<UnloadAssemblyLoadContextFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("UnloadAssemblyLoadContext"));
		s_ManagedFunctions.GetLastLoadStatusFptr = LoadCoralManagedFunctionPtr<GetLastLoadStatusFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("GetLastLoadStatus"));
		s_ManagedFunctions.GetAssemblyNameFptr = LoadCoralManagedFunctionPtr<GetAssemblyNameFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("GetAssemblyName"));

		s_ManagedFunctions.RunMSBuildFptr = LoadCoralManagedFunctionPtr<RunMSBuildFn>(CORAL_STR("Coral.Managed.MSBuildRunner, Coral.Managed"), CORAL_STR("Run"));

		s_ManagedFunctions.GetAssemblyTypesFptr = LoadCoralManagedFunctionPtr<GetAssemblyTypesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetAssemblyTypes"));
		s_ManagedFunctions.GetFullTypeNameFptr = LoadCoralManagedFunctionPtr<GetFullTypeNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFullTypeName"));
		s_ManagedFunctions.GetAssemblyQualifiedNameFptr = LoadCoralManagedFunctionPtr<GetAssemblyQualifiedNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetAssemblyQualifiedName"));
		s_ManagedFunctions.GetBaseTypeFptr = LoadCoralManagedFunctionPtr<GetBaseTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetBaseType"));
		s_ManagedFunctions.GetInterfaceTypeCountFptr = LoadCoralManagedFunctionPtr<GetInterfaceTypeCountFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetInterfaceTypeCount"));
		s_ManagedFunctions.GetInterfaceTypesFptr = LoadCoralManagedFunctionPtr<GetInterfaceTypesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetInterfaceTypes"));
		s_ManagedFunctions.GetTypeSizeFptr = LoadCoralManagedFunctionPtr<GetTypeSizeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeSize"));
		s_ManagedFunctions.IsTypeSubclassOfFptr = LoadCoralManagedFunctionPtr<IsTypeSubclassOfFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("IsTypeSubclassOf"));
		s_ManagedFunctions.IsTypeAssignableToFptr = LoadCoralManagedFunctionPtr<IsTypeAssignableToFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("IsTypeAssignableTo"));
		s_ManagedFunctions.IsTypeAssignableFromFptr = LoadCoralManagedFunctionPtr<IsTypeAssignableFromFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("IsTypeAssignableFrom"));
		s_ManagedFunctions.IsTypeSZArrayFptr = LoadCoralManagedFunctionPtr<IsTypeSZArrayFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("IsTypeSZArray"));
		s_ManagedFunctions.GetElementTypeFptr = LoadCoralManagedFunctionPtr<GetElementTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetElementType"));
		s_ManagedFunctions.GetTypeMethodsFptr = LoadCoralManagedFunctionPtr<GetTypeMethodsFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeMethods"));
		s_ManagedFunctions.GetTypeFieldsFptr = LoadCoralManagedFunctionPtr<GetTypeFieldsFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeFields"));
		s_ManagedFunctions.GetTypePropertiesFptr = LoadCoralManagedFunctionPtr<GetTypePropertiesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeProperties"));
		s_ManagedFunctions.HasTypeAttributeFptr = LoadCoralManagedFunctionPtr<HasTypeAttributeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("HasTypeAttribute"));
		s_ManagedFunctions.GetTypeAttributesFptr = LoadCoralManagedFunctionPtr<GetTypeAttributesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeAttributes"));
		s_ManagedFunctions.GetTypeManagedTypeFptr = LoadCoralManagedFunctionPtr<GetTypeManagedTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeManagedType"));
		s_ManagedFunctions.InvokeStaticMethodFptr = LoadCoralManagedFunctionPtr<InvokeStaticMethodFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("InvokeStaticMethod"));
		s_ManagedFunctions.InvokeStaticMethodRetFptr = LoadCoralManagedFunctionPtr<InvokeStaticMethodRetFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("InvokeStaticMethodRet"));

		s_ManagedFunctions.GetMethodInfoNameFptr = LoadCoralManagedFunctionPtr<GetMethodInfoNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoName"));
		s_ManagedFunctions.GetMethodInfoReturnTypeFptr = LoadCoralManagedFunctionPtr<GetMethodInfoReturnTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoReturnType"));
		s_ManagedFunctions.GetMethodInfoParameterTypesFptr = LoadCoralManagedFunctionPtr<GetMethodInfoParameterTypesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoParameterTypes"));
		s_ManagedFunctions.GetMethodInfoAccessibilityFptr = LoadCoralManagedFunctionPtr<GetMethodInfoAccessibilityFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoAccessibility"));
		s_ManagedFunctions.GetMethodInfoAttributesFptr = LoadCoralManagedFunctionPtr<GetMethodInfoAttributesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoAttributes"));

		s_ManagedFunctions.GetFieldInfoNameFptr = LoadCoralManagedFunctionPtr<GetFieldInfoNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFieldInfoName"));
		s_ManagedFunctions.GetFieldInfoTypeFptr = LoadCoralManagedFunctionPtr<GetFieldInfoTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFieldInfoType"));
		s_ManagedFunctions.GetFieldInfoAccessibilityFptr = LoadCoralManagedFunctionPtr<GetFieldInfoAccessibilityFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFieldInfoAccessibility"));
		s_ManagedFunctions.GetFieldInfoAttributesFptr = LoadCoralManagedFunctionPtr<GetFieldInfoAttributesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFieldInfoAttributes"));

		s_ManagedFunctions.GetPropertyInfoNameFptr = LoadCoralManagedFunctionPtr<GetPropertyInfoNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetPropertyInfoName"));
		s_ManagedFunctions.GetPropertyInfoTypeFptr = LoadCoralManagedFunctionPtr<GetPropertyInfoTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetPropertyInfoType"));
		s_ManagedFunctions.GetPropertyInfoAttributesFptr = LoadCoralManagedFunctionPtr<GetPropertyInfoAttributesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetPropertyInfoAttributes"));

		s_ManagedFunctions.GetAttributeFieldValueFptr = LoadCoralManagedFunctionPtr<GetAttributeFieldValueFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetAttributeFieldValue"));
		s_ManagedFunctions.GetAttributeTypeFptr = LoadCoralManagedFunctionPtr<GetAttributeTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetAttributeType"));

		s_ManagedFunctions.SetInternalCallsFptr = LoadCoralManagedFunctionPtr<SetInternalCallsFn>(CORAL_STR("Coral.Managed.Interop.InternalCallsManager, Coral.Managed"), CORAL_STR("SetInternalCalls"));
		s_ManagedFunctions.CreateObjectFptr = LoadCoralManagedFunctionPtr<CreateObjectFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("CreateObject"));
		s_ManagedFunctions.CopyObjectFptr = LoadCoralManagedFunctionPtr<CopyObjectFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("CopyObject"));
		s_ManagedFunctions.InvokeMethodFptr = LoadCoralManagedFunctionPtr<InvokeMethodFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("InvokeMethod"));
		s_ManagedFunctions.InvokeMethodRetFptr = LoadCoralManagedFunctionPtr<InvokeMethodRetFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("InvokeMethodRet"));
		s_ManagedFunctions.SetFieldValueFptr = LoadCoralManagedFunctionPtr<SetFieldValueFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("SetFieldValue"));
		s_ManagedFunctions.GetFieldValueFptr = LoadCoralManagedFunctionPtr<GetFieldValueFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("GetFieldValue"));
		s_ManagedFunctions.SetPropertyValueFptr = LoadCoralManagedFunctionPtr<SetFieldValueFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("SetPropertyValue"));
		s_ManagedFunctions.GetPropertyValueFptr = LoadCoralManagedFunctionPtr<GetFieldValueFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("GetPropertyValue"));
		s_ManagedFunctions.DestroyObjectFptr = LoadCoralManagedFunctionPtr<DestroyObjectFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("DestroyObject"));
		s_ManagedFunctions.GetObjectTypeIdFptr = LoadCoralManagedFunctionPtr<GetObjectTypeIdFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("GetObjectTypeId"));

		s_ManagedFunctions.CollectGarbageFptr = LoadCoralManagedFunctionPtr<CollectGarbageFn>(CORAL_STR("Coral.Managed.GarbageCollector, Coral.Managed"), CORAL_STR("CollectGarbage"));
		s_ManagedFunctions.WaitForPendingFinalizersFptr = LoadCoralManagedFunctionPtr<WaitForPendingFinalizersFn>(CORAL_STR("Coral.Managed.GarbageCollector, Coral.Managed"), CORAL_STR("WaitForPendingFinalizers"));
	}

	void* HostInstance::LoadCoralManagedFunctionPtr(const std::filesystem::path& InAssemblyPath, const UCChar* InTypeName, const UCChar* InMethodName, const UCChar* InDelegateType) const
	{
		void* funcPtr = nullptr;

		int status = s_CoreCLRFunctions.GetManagedFunctionPtr(InAssemblyPath.c_str(), InTypeName, InMethodName, InDelegateType, nullptr, &funcPtr);
		if(status != StatusCode::Success || !funcPtr) {
			std::cerr << "Failed to retrieve managed function pointer `" << InTypeName << "`::`" << InMethodName << "` from `" << InAssemblyPath << "`" << std::endl;
			CORAL_VERIFY(false);
		}

		return funcPtr;
	}
}
