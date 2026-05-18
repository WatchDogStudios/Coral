#pragma once

#include "Core.hpp"
#include "MessageLevel.hpp"
#include "Assembly.hpp"
#include "ManagedObject.hpp"

#include <chrono>
#include <functional>

namespace Coral {

	using ExceptionCallbackFn = std::function<void(std::string_view)>;

	struct HostSettings
	{
		/// <summary>
		/// The file path to Coral.runtimeconfig.json (e.g C:\Dev\MyProject\ThirdParty\Coral)
		/// </summary>
		std::string CoralDirectory;

		MessageCallbackFn MessageCallback = nullptr;
		MessageLevel MessageFilter = MessageLevel::All;

		ExceptionCallbackFn ExceptionCallback = nullptr;

		// ---------------------------------------------------------------
		// External managed-debugger attach support
		// ---------------------------------------------------------------
		// Coral is a delegate host (hostfxr_initialize_for_runtime_config +
		// hdt_load_assembly_and_get_function_pointer). The CLR in that mode
		// is lazy: it's loaded into the host process but Debugger::Startup
		// and the diagnostic IPC server thread don't fully publish their
		// state until the first managed call AND a brief settling window
		// after that. An external managed debugger (vsdbg, JetBrains'
		// debugger, Visual Studio's managed engine) that calls
		// ICorDebug::DebugActiveProcess during that window gets back
		// E_INVALIDARG instead of attaching.
		//
		// The settings below close that window:

		/// <summary>
		/// When true, sets DOTNET_EnableDiagnostics* env vars (IPC,
		/// Debugger, Profiler) before loading hostfxr, and sets the
		/// MetadataUpdater.IsSupported runtime property to true. Required
		/// for vsdbg / Rider / Visual Studio to attach via ICorDebug.
		///
		/// Default true; only turn off in shipped builds where you want
		/// to deny external attach entirely (and you trust no DOTNET_*
		/// env vars are leaking in from the parent process).
		/// </summary>
		bool EnableManagedDebugger = true;

		/// <summary>
		/// When true, HostInstance::Initialize blocks until the .NET
		/// diagnostic IPC pipe (\\.\pipe\dotnet-diagnostic-&lt;pid&gt; on
		/// Windows; /tmp/dotnet-diagnostic-&lt;pid&gt;-... on POSIX) has
		/// been created by the runtime, OR DiagnosticPipeWaitTimeout
		/// elapses. Without this wait, there's a race where Initialize
		/// returns before the IPC server thread publishes its pipe, and
		/// any debugger attach probed during that window gets
		/// E_INVALIDARG from the runtime.
		///
		/// Times out silently rather than failing - the runtime works
		/// fine without the pipe in most cases, and we don't want to
		/// deadlock startup if the pipe never comes up (e.g. on a
		/// hardened CI runner where named-pipe creation is denied by
		/// policy).
		///
		/// Only meaningful when EnableManagedDebugger is also true.
		/// </summary>
		bool WaitForDiagnosticPipe = true;

		/// <summary>
		/// Wall-clock cap for WaitForDiagnosticPipe. Default 2 seconds,
		/// which comfortably covers the worst observed publish latency
		/// (~500ms on a slow VM) without making cold starts feel slow.
		/// </summary>
		std::chrono::milliseconds DiagnosticPipeWaitTimeout{ 2000 };
	};

	enum class CoralInitStatus
	{
		Success,
		CoralManagedNotFound,
		CoralManagedInitError,
		DotNetNotFound,
	};

	class HostInstance
	{
	public:
		CoralInitStatus Initialize(HostSettings InSettings);
		void Shutdown();

		AssemblyLoadContext CreateAssemblyLoadContext(std::string_view InName);
		void UnloadAssemblyLoadContext(AssemblyLoadContext& InLoadContext);

		// `InDllPath` is a colon-separated list of paths from which AssemblyLoader will try and resolve load paths at runtime.
		// This does not affect the behaviour of LoadAssembly from native code.
		AssemblyLoadContext CreateAssemblyLoadContext(std::string_view InName, std::string_view InDllPath);

	private:
		bool LoadHostFXR() const;
		bool InitializeCoralManaged();
		void LoadCoralFunctions();

		void* LoadCoralManagedFunctionPtr(const std::filesystem::path& InAssemblyPath, const UCChar* InTypeName, const UCChar* InMethodName, const UCChar* InDelegateType = CORAL_UNMANAGED_CALLERS_ONLY) const;

		template<typename TFunc>
		TFunc LoadCoralManagedFunctionPtr(const UCChar* InTypeName, const UCChar* InMethodName, const UCChar* InDelegateType = CORAL_UNMANAGED_CALLERS_ONLY) const
		{
			return (TFunc) LoadCoralManagedFunctionPtr(m_CoralManagedAssemblyPath, InTypeName, InMethodName, InDelegateType);
		}

	private:
		HostSettings m_Settings;
		std::filesystem::path m_CoralManagedAssemblyPath;
		void* m_HostFXRContext = nullptr;
		bool m_Initialized = false;

		friend class AssemblyLoadContext;
	};

}
