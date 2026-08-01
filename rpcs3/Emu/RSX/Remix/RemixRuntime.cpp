#include "stdafx.h"
#include "RemixRuntime.h"

#ifdef _WIN32

#include <cstdlib>

namespace remix_rsx
{
	namespace
	{
		// Widen a narrow path for the Remix loader, which is wchar_t only.
		std::wstring widen(const std::string& src)
		{
			if (src.empty())
			{
				return {};
			}

			const int needed = MultiByteToWideChar(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), nullptr, 0);
			if (needed <= 0)
			{
				return {};
			}

			std::wstring result(static_cast<usz>(needed), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), result.data(), needed);
			return result;
		}

		// Read an environment variable as UTF-16. Empty if unset or empty.
		std::wstring read_env(const wchar_t* name)
		{
			const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
			if (needed <= 1)
			{
				return {};
			}

			std::wstring result(static_cast<usz>(needed) - 1, L'\0');
			const DWORD written = GetEnvironmentVariableW(name, result.data(), needed);
			result.resize(std::min<usz>(written, result.size()));
			return result;
		}

		// <exe dir>\remix\d3d9.dll, unless RPCS3_REMIX_DLL points elsewhere.
		// The runtime is deliberately NOT placed next to rpcs3.exe: a file called d3d9.dll
		// there would be picked up by anything else in the process that resolves that name.
		std::wstring resolve_runtime_path()
		{
			if (std::wstring env = read_env(L"RPCS3_REMIX_DLL"); !env.empty())
			{
				return env;
			}

			return widen(fs::get_executable_dir() + "remix/d3d9.dll");
		}
	}

	runtime::~runtime()
	{
		shutdown();
	}

	bool runtime::initialize(HWND hwnd)
	{
		if (m_ok)
		{
			return true;
		}

		const std::wstring path = resolve_runtime_path();
		if (path.empty())
		{
			rsx_log.error("Remix: could not resolve a runtime path");
			return false;
		}

		const remixapi_ErrorCode load_status = remixapi_lib_loadRemixDllAndInitialize(path.c_str(), &m_api, &m_dll);
		if (load_status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.error("Remix: remixapi_lib_loadRemixDllAndInitialize failed (%s)", error_name(load_status));
			m_api = {};
			m_dll = nullptr;
			return false;
		}

		remixapi_StartupInfo startup_info{};
		startup_info.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
		startup_info.pNext = nullptr;
		startup_info.hwnd = hwnd;
		startup_info.disableSrgbConversionForOutput = 0;
		startup_info.forceNoVkSwapchain = 0;
		startup_info.editorModeEnabled = 0;

		if (!m_api.Startup)
		{
			rsx_log.error("Remix: runtime exposes no Startup entry point");
			remixapi_lib_shutdownAndUnloadRemixDll(&m_api, m_dll);
			m_api = {};
			m_dll = nullptr;
			return false;
		}

		const u32 startup_status = guarded_startup(m_api.Startup, &startup_info);
		if (startup_status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.error("Remix: Startup failed (%s)", error_name(startup_status));
			remixapi_lib_shutdownAndUnloadRemixDll(&m_api, m_dll);
			m_api = {};
			m_dll = nullptr;
			return false;
		}

		m_ok = true;
		rsx_log.success("Remix: runtime started on HWND 0x%x", reinterpret_cast<u64>(hwnd));
		return true;
	}

	void runtime::shutdown()
	{
		if (!m_dll)
		{
			m_api = {};
			m_ok = false;
			return;
		}

		remixapi_lib_shutdownAndUnloadRemixDll(&m_api, m_dll);
		m_api = {};
		m_dll = nullptr;
		m_ok = false;
	}

	u32 guarded_startup(PFN_remixapi_Startup fn, const remixapi_StartupInfo* info)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_create_mesh(PFN_remixapi_CreateMesh fn, const remixapi_MeshInfo* info, remixapi_MeshHandle* out_handle)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(info, out_handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_destroy_mesh(PFN_remixapi_DestroyMesh fn, remixapi_MeshHandle handle)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_draw_instance(PFN_remixapi_DrawInstance fn, const remixapi_InstanceInfo* info)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_setup_camera(PFN_remixapi_SetupCamera fn, const remixapi_CameraInfo* info)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_create_light(PFN_remixapi_CreateLight fn, const remixapi_LightInfo* info, remixapi_LightHandle* out_handle)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(info, out_handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_destroy_light(PFN_remixapi_DestroyLight fn, remixapi_LightHandle handle)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_draw_light_instance(PFN_remixapi_DrawLightInstance fn, remixapi_LightHandle handle)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_present(PFN_remixapi_Present fn, const remixapi_PresentInfo* info)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	const char* error_name(u32 code)
	{
		switch (code)
		{
		case REMIXAPI_ERROR_CODE_SUCCESS: return "SUCCESS";
		case REMIXAPI_ERROR_CODE_GENERAL_FAILURE: return "GENERAL_FAILURE";
		case REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE: return "LOAD_LIBRARY_FAILURE";
		case REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS: return "INVALID_ARGUMENTS";
		case REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE: return "GET_PROC_ADDRESS_FAILURE";
		case REMIXAPI_ERROR_CODE_ALREADY_EXISTS: return "ALREADY_EXISTS";
		case REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D9_DEVICE: return "REGISTERING_NON_REMIX_D3D9_DEVICE";
		case REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED: return "REMIX_DEVICE_WAS_NOT_REGISTERED";
		case REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION: return "INCOMPATIBLE_VERSION";
		case REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE: return "SET_DLL_DIRECTORY_FAILURE";
		case REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE: return "GET_FULL_PATH_NAME_FAILURE";
		case REMIXAPI_ERROR_CODE_NOT_INITIALIZED: return "NOT_INITIALIZED";
		case REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES: return "HRESULT_NO_REQUIRED_GPU_FEATURES";
		case REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM: return "HRESULT_DRIVER_VERSION_BELOW_MINIMUM";
		case REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL: return "HRESULT_DXVK_INSTANCE_EXTENSION_FAIL";
		case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL: return "HRESULT_VK_CREATE_INSTANCE_FAIL";
		case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL: return "HRESULT_VK_CREATE_DEVICE_FAIL";
		case REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING: return "HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING";
		case error_code_faulted: return "FAULTED (structured exception inside the runtime)";
		default: return "UNKNOWN";
		}
	}

	f32 hardcoded_far_plane()
	{
		static const f32 value = []() -> f32
		{
			if (const std::wstring env = read_env(L"RPCS3_REMIX_FARPLANE"); !env.empty())
			{
				const f32 parsed = static_cast<f32>(::_wtof(env.c_str()));
				if (parsed > 1.f)
				{
					return parsed;
				}
			}

			return 1000.f;
		}();

		return value;
	}
}

#endif
