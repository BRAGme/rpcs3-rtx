#pragma once

#ifdef _WIN32

#include "util/types.hpp"

#include "remix_c.h"

namespace remix_rsx
{
	// Returned by the guarded wrappers when the runtime raised a structured exception.
	// Chosen outside the remixapi_ErrorCode range so it can never collide with a real code.
	inline constexpr u32 error_code_faulted = 0x7FFF0001u;

	// Owns the dxvk-remix module and its interface table.
	// Nothing here throws: a missing or broken runtime degrades the renderer to a no-op.
	class runtime
	{
	public:
		runtime() = default;
		~runtime();

		runtime(const runtime&) = delete;
		runtime& operator=(const runtime&) = delete;

		// Loads the runtime DLL and calls Startup() against 'hwnd'.
		bool initialize(HWND hwnd);
		void shutdown();

		bool ok() const { return m_ok; }
		const remixapi_Interface& api() const { return m_api; }

	private:
		remixapi_Interface m_api{};
		HMODULE m_dll = nullptr;
		bool m_ok = false;
	};

	// SEH-guarded leaf calls. POD parameters only: __try/__except cannot coexist with
	// objects that require unwinding in the same function (C2712).
	u32 guarded_startup(PFN_remixapi_Startup fn, const remixapi_StartupInfo* info);
	u32 guarded_create_mesh(PFN_remixapi_CreateMesh fn, const remixapi_MeshInfo* info, remixapi_MeshHandle* out_handle);
	u32 guarded_destroy_mesh(PFN_remixapi_DestroyMesh fn, remixapi_MeshHandle handle);
	u32 guarded_draw_instance(PFN_remixapi_DrawInstance fn, const remixapi_InstanceInfo* info);
	u32 guarded_setup_camera(PFN_remixapi_SetupCamera fn, const remixapi_CameraInfo* info);
	u32 guarded_create_light(PFN_remixapi_CreateLight fn, const remixapi_LightInfo* info, remixapi_LightHandle* out_handle);
	u32 guarded_destroy_light(PFN_remixapi_DestroyLight fn, remixapi_LightHandle handle);
	u32 guarded_draw_light_instance(PFN_remixapi_DrawLightInstance fn, remixapi_LightHandle handle);
	u32 guarded_present(PFN_remixapi_Present fn, const remixapi_PresentInfo* info);

	// Readable name for a remixapi_ErrorCode or for error_code_faulted.
	const char* error_name(u32 code);

	// Far plane used by the hardcoded camera. Overridable at runtime through
	// RPCS3_REMIX_FARPLANE so the unit-scale sweep in the bring-up plan needs no rebuild.
	f32 hardcoded_far_plane();
}

#endif
