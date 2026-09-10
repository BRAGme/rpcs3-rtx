#include "stdafx.h"
#include "RemixRuntime.h"

#ifdef _WIN32

#include "util/fnv_hash.hpp"
#include "Emu/system_config.h"

#include <cstdlib>

// ---------------------------------------------------------------------------------------------
// Runtime provenance. The vendored header and the deployed runtime MUST come from the same
// source commit, always in the same commit here -- the API's version gate only compares the
// minor number, so two builds of the same minor with a different interface layout would pass
// the check and then misroute every slot after the first divergence.
//
//   Fork          : RemixProjGroup/dxvk-remix ("Remix Plus", maintainer Kim2091)
//   Source commit : 6476faeaf148b0314c4e03480e48445e796afe5c, branch numos3 (BRAGme/dxvk-remix)
//                   Past remix-plus-1.5.1, whose copy is still blob 3f4acf5f.../0.1000.0.
//   API version   : 0.1000.1
//   remix_c.h     : blob ae61b5db53fa40e1563a47b07822cbbcd4122e9f, 55351 bytes
//                   SHA-256 693334BE266D380BFE324FCBF2675849E98757CCC5BEB4224A2ED9FB20C6C4F5
//                   (byte-identical to public/include/remix/remix_c.h at that commit)
//   Runtime asset : local build of that commit. Deployed to <exe dir>\remix\; d3d9.dll is
//                   240657408 bytes,
//                   SHA-256 16A0B512F33EBB66A89AC703E75289D9E008558A13D2C9A6A5455B0BE7C40858.
//                   ROUND 34 RE-BASELINE: this block used to name
//                   36A5641AF4FA848E... / fnv1a 63656dfe3da8f069, and no file on this machine
//                   hashes to that -- it was an earlier incremental link of the same tree, since
//                   overwritten, which is why the load-time warning fired on every run from
//                   2026-08-16 onward and was read as noise for many rounds.
//                   For this exact binary log_dll_identity prints
//                   "size=240657408 fnv1a=09653f484ec94dc0", so a run's log line can be
//                   compared against this block character for character, with no rehashing
//                   and no access to the build tree. Both values are mirrored below in
//                   vendored_runtime_size / vendored_runtime_fnv1a, which warn at load if the
//                   DLL is a different one -- change them here and there in the same commit.
//                   FNV-1a is not collision-resistant and is only meant to answer "is this the
//                   binary the comment describes"; the SHA-256 above stays the identity for
//                   anything stronger. Built from a dirty
//                   tree (17 files modified as of round 34, none of them remix_c.h or the API
//                   implementation -- verified by `git status --porcelain` on
//                   src/dxvk/rtx_render/rtx_remix_api.cpp and public/include/remix/remix_c.h,
//                   both clean -- so the surface still matches the header above), which means the
//                   commit alone does not reproduce it -- identify the binary by hash.
//                   RETRACTED CLAIM: this block used to say the Remix_Plus_v1.5.1 release zip
//                   "predates the VIEW_MODEL category bit this backend relies on". Not supported.
//                   The July CI build kept at bin\remix\d3d9.dll.bak-0729 (242589696 bytes,
//                   SHA-256 7EA6282B5A242DD9..., API 0.1000.0) ALREADY carries a bit-26 arm
//                   (bt eax,0x1a at RVA 0x001FAEB6), and 6476faea's own message says
//                   "Both values match the remix-plus-1.5.1 tag, which already carried this work."
//                   What 6476faea adds on top is the Sky->Main clamp and the 0.1000.1 bump.
//
// Never update bin\remix\ without re-vendoring remix_c.h in the same commit.
// ---------------------------------------------------------------------------------------------

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

		// Narrow a UTF-16 path for logging only.
		std::string narrow(const std::wstring& src)
		{
			if (src.empty())
			{
				return {};
			}

			const int needed = WideCharToMultiByte(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), nullptr, 0, nullptr, nullptr);
			if (needed <= 0)
			{
				return {};
			}

			std::string result(static_cast<usz>(needed), '\0');
			WideCharToMultiByte(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), result.data(), needed, nullptr, nullptr);
			return result;
		}

		// The runtime described by the provenance block at the top of this file. Kept next to the
		// check that uses them so the two cannot drift apart; both must be updated whenever
		// bin\remix\ is redeployed, in the same commit as the re-vendored remix_c.h.
		//
		// ROUND 34: RE-BASELINED ON THE FILE THAT IS ACTUALLY DEPLOYED, and the old value is recorded
		// here rather than deleted because the warning it produced was a TRUE positive that nobody
		// read for many rounds. Old: 0x63656dfe3da8f069, described as sha256 36A5641AF4FA848E...
		// No file anywhere on this machine hashes to that value - it was an earlier incremental link
		// of the same tree (same PDB GUID 228D2E7A-E41C-451F-8C80-D8B7ADD7E065, a lower Age) that has
		// since been overwritten. New value is the deployed bin\remix\d3d9.dll, sha256
		// 16a0b512f33ebb66a89ac703e75289d9e008558a13d2c9a6a5455b0be7c40858, PDB Age 48, PE
		// TimeDateStamp 0x6A81B564 = 2026-08-16 13:04:36Z.
		//
		// The size was ALREADY correct at 240657408 and needed no change - which is why the warning
		// fired on the hash alone and read as noise. Reconciled so a future mismatch means something.
		//
		// FOR THE RECORD, MEASURED: the API surface of the deployed file is identical to
		// dxvk-remix-numos3\_output\d3d9.dll (fnv1a 5c5478cd184f4b0a). toRtDrawState is byte-identical
		// across all 15,328 bytes; a whole-file diff is 2,282 bytes over 9 regions, eight of which are
		// build stamps (PE TimeDateStamp, OptionalHeader CheckSum, debug-directory timestamps, the
		// RSDS Age byte 0x30 -> 0x31, .pdata/xdata fixups) and the ninth is
		// ImGui_ImplWin32_WndProcHandler at RVA 0x003823C0. So the "header and runtime may have come
		// apart" warning was true of the BYTES and false of the INTERFACE.
		//
		// PDB TRAP, worth more than this constant: bin\remix\d3d9.pdb has GUID
		// 04C3AFFD-472B-4565-9AA0-08EBE452E439 Age 24, which is d3d9.dll.bak-0729's PDB, NOT the
		// deployed DLL's. Symbolizing a crash in the deployed runtime with it gives WRONG function
		// names. The matching lineage is dxvk-remix-numos3\_output\d3d9.pdb (GUID 228D2E7A..., Age 49).
		constexpr u64 vendored_runtime_size  = 240657408;
		constexpr u64 vendored_runtime_fnv1a = 0x09653f484ec94dc0;

		// Identity of the DLL actually loaded, so a report can say which binary produced a run.
		void log_dll_identity(const std::wstring& path)
		{
			const std::string narrow_path = narrow(path);

			fs::file dll{narrow_path};
			if (!dll)
			{
				rsx_log.warning("Remix: runtime DLL '%s' could not be opened for fingerprinting", narrow_path);
				return;
			}

			const u64 size = dll.size();
			const std::vector<u8> bytes = dll.to_vector<u8>();

			usz hash = rpcs3::fnv_seed;
			for (const u8 b : bytes)
			{
				hash = rpcs3::hash64(hash, b);
			}

			rsx_log.notice("Remix: runtime DLL '%s' size=%llu fnv1a=%016llx", narrow_path, size, static_cast<u64>(hash));

			if (size != vendored_runtime_size || static_cast<u64>(hash) != vendored_runtime_fnv1a)
			{
				// Deliberately not fatal. An incompatible ABI is already rejected by the version
				// gate in initialize(), and pointing RPCS3_REMIX_DLL at another build is a thing
				// worth doing. This covers only what the gate cannot see: a runtime on the same
				// API minor whose interface layout has moved out from under the vendored header.
				rsx_log.warning("Remix: runtime DLL is not the build this source was vendored against "
					"(expected size=%llu fnv1a=%016llx) -- header and runtime may have come apart",
					vendored_runtime_size, vendored_runtime_fnv1a);
			}
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

		log_dll_identity(path);

		rsx_log.notice("Remix: header API version %u.%u.%u, sizeof(remixapi_Interface)=%llu (+%llu bytes slack)",
			u32{REMIXAPI_VERSION_MAJOR}, u32{REMIXAPI_VERSION_MINOR}, u32{REMIXAPI_VERSION_PATCH},
			static_cast<u64>(sizeof(remixapi_Interface)), static_cast<u64>(interface_slack_bytes));

		const remixapi_ErrorCode load_status = remixapi_lib_loadRemixDllAndInitialize(path.c_str(), &m_storage.api, &m_dll);
		if (load_status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			// INCOMPATIBLE_VERSION here is the header/runtime lockstep check doing its job:
			// isVersionCompatible matches the minor exactly for 0.y.z, so both mismatch
			// directions fail loudly instead of silently misrouting interface slots.
			rsx_log.error("Remix: remixapi_lib_loadRemixDllAndInitialize failed (%s)", error_name(load_status));
			m_storage = {};
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

		if (!m_storage.api.Startup)
		{
			rsx_log.error("Remix: runtime exposes no Startup entry point");
			remixapi_lib_shutdownAndUnloadRemixDll(&m_storage.api, m_dll);
			m_storage = {};
			m_dll = nullptr;
			return false;
		}

		// Snapshot the window's proc across Startup(). The runtime subclasses the HWND it is
		// given, and that hook has to come back off at shutdown or the next boot crashes in it --
		// see unhook_window_proc(). Read through the charset-matched entry point, the same way
		// dxvk-remix's HookWindowProc does, because Windows keeps A and W procs in different
		// representations and mixing the two hands back a thunk rather than the real pointer.
		const bool hwnd_unicode = hwnd && IsWindowUnicode(hwnd);
		const WNDPROC proc_before = hwnd
			? reinterpret_cast<WNDPROC>(hwnd_unicode
				? GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
				: GetWindowLongPtrA(hwnd, GWLP_WNDPROC))
			: nullptr;

		const u32 startup_status = guarded_startup(m_storage.api.Startup, &startup_info);

		// Recorded before the status is even looked at: a Startup that hooked the window and then
		// failed leaves exactly the same dangling proc behind as a successful one.
		if (hwnd)
		{
			const WNDPROC proc_after = reinterpret_cast<WNDPROC>(hwnd_unicode
				? GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
				: GetWindowLongPtrA(hwnd, GWLP_WNDPROC));

			if (proc_after != proc_before)
			{
				m_hwnd = hwnd;
				m_original_wndproc = proc_before;
				m_remix_wndproc = proc_after;

				rsx_log.notice("Remix: runtime subclassed HWND 0x%x (proc 0x%x -> 0x%x); it will be restored at shutdown",
					reinterpret_cast<u64>(hwnd), reinterpret_cast<u64>(proc_before), reinterpret_cast<u64>(proc_after));
			}
		}

		if (startup_status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.error("Remix: Startup failed (%s)", error_name(startup_status));
			unhook_window_proc();
			remixapi_lib_shutdownAndUnloadRemixDll(&m_storage.api, m_dll);
			m_storage = {};
			m_dll = nullptr;
			return false;
		}

		m_ok = true;

		// A stock (non-fork) runtime that somehow passed the version gate would leave these
		// slots at the zeroed value the storage was constructed with, so a null check is a
		// real detector and not just defensive noise.
		m_fork_features = check_fork_slots() && probe_create_texture();

		if (m_fork_features)
		{
			rsx_log.success("Remix: fork features available (CreateTexture/CreateMaterial/DrawScreenOverlay)");
		}
		else
		{
			rsx_log.error("Remix: fork features disabled -- running geometry-only (no textures, no UI compositor)");
		}

		rsx_log.success("Remix: runtime started on HWND 0x%x", reinterpret_cast<u64>(hwnd));
		return true;
	}

	bool runtime::check_fork_slots()
	{
		struct slot
		{
			const char* name;
			const void* fn;
		};

		const slot required[] =
		{
			{ "CreateTexture", reinterpret_cast<const void*>(m_storage.api.CreateTexture) },
			{ "DestroyTexture", reinterpret_cast<const void*>(m_storage.api.DestroyTexture) },
			{ "CreateMaterial", reinterpret_cast<const void*>(m_storage.api.CreateMaterial) },
			{ "DestroyMaterial", reinterpret_cast<const void*>(m_storage.api.DestroyMaterial) },
			{ "DrawScreenOverlay", reinterpret_cast<const void*>(m_storage.api.DrawScreenOverlay) },
			{ "SetGameValue", reinterpret_cast<const void*>(m_storage.api.SetGameValue) },
		};

		bool all_present = true;

		for (const slot& s : required)
		{
			if (!s.fn)
			{
				rsx_log.error("Remix: runtime does not expose %s -- not a Remix Plus runtime?", s.name);
				all_present = false;
			}
		}

		return all_present;
	}

	bool runtime::probe_create_texture()
	{
		// 2x2 RGBA8, distinctive content. If a slot were misrouted the call would land in an
		// unrelated function; the SEH wrapper turns that into a loud degrade instead of a crash.
		static constexpr u8 probe_pixels[16] =
		{
			0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
			0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		};

		remixapi_TextureInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
		info.pNext = nullptr;
		info.hash = 0x5250435333303031ull; // "RPCS3001"
		info.width = 2;
		info.height = 2;
		info.depth = 1;
		info.mipLevels = 1;
		info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
		info.data = probe_pixels;
		info.dataSize = sizeof(probe_pixels);

		remixapi_TextureHandle handle = nullptr;
		const u32 status = guarded_create_texture(m_storage.api.CreateTexture, &info, &handle);

		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.error("Remix: CreateTexture probe failed (%s)", error_name(status));
			return false;
		}

		rsx_log.notice("Remix: CreateTexture probe SUCCESS (handle %p)", handle);

		if (handle)
		{
			guarded_destroy_texture(m_storage.api.DestroyTexture, handle);
		}

		probe_set_config_variable();

		return true;
	}

	// --- ROUND 39: is rtx.* reachable from THIS client, at runtime? --------------------------------
	//
	// Why this probe exists rather than a design built on the answer. Haze authors per-zone fog
	// (fogCol / fogNear / fogFar / fogIntensity) and the question was whether a remixapi client can
	// set fog at all. Reading the deployed runtime's source (dxvk-remix-numos3, branch numos3, HEAD
	// 6476faea - the tree RemixRuntime.cpp:143 records as matching the deployed d3d9.dll) gives a
	// split answer, and the useful half of it depends entirely on one function pointer:
	//
	//   NOT REACHABLE - D3D9 fixed-function fog. FogState has exactly one producer in the whole
	//     tree, setFogState() in src/d3d9/d3d9_rtx_utils.cpp:245, called only from
	//     src/d3d9/d3d9_rtx.cpp:686. rtx_remix_api.cpp:900 default-constructs DrawCallState, so
	//     fogState.mode stays D3DFOG_NONE for every external draw, forever. The conf options that
	//     look like the answer - rtx.enableFog / rtx.fogColorScale / rtx.maxFogDistance,
	//     rtx_composite.h:90-92 - only SCALE that state, and composite.slangh:33-36 returns
	//     vec4(0.0) when fogMode == D3DFOG_NONE. They multiply a hard zero. This is the same shape
	//     as sky rasterization, ignoreTextures and terrain baking: the option parses and the
	//     feature never runs. THREE ROUNDS OF THIS PROJECT HAVE BEEN LOST TO THAT PATTERN.
	//
	//   REACHABLE - global volumetrics. RtxGlobalVolumetrics::getVolumeArgs
	//     (rtx_global_volumetrics.cpp:467, called per frame from rtx_context.cpp:1365) reads
	//     transmittanceColor(), transmittanceMeasurementDistanceMeters(), singleScatteringAlbedo()
	//     and anisotropy() DIRECTLY from RtxOption values. The D3D9 fog only ever overrides them,
	//     behind a gate (`fogState.mode != D3DFOG_NONE`, :483-486) an external draw cannot open -
	//     and volumetrics stay ON in that case rather than off, because shouldConvertToPhysicalFog
	//     returns true immediately for D3DFOG_NONE (:445).
	//
	// So per-zone fog COLOUR and DENSITY are reachable and per-zone fogNear is not - there is no
	// start distance anywhere in VolumeArgs. But every word of that is INFERRED from source, and
	// the one thing it all hangs on is whether remixapi_Interface::SetConfigVariable is actually
	// populated in the DEPLOYED build. Nothing in this project has ever called it.
	//
	// The probe is deliberately a NEGATIVE one and changes no option. rtx_remix_api.cpp:1653 looks
	// the key up with RtxOptionImpl::getOptionByFullName and returns GENERAL_FAILURE when it is
	// unknown, so a deliberately invalid key exercises the whole dispatch - pointer, marshalling,
	// return path - and provably writes nothing. A SUCCESS here would be the alarming result: it
	// would mean the slot is misrouted into some other function that accepted our arguments.
	void runtime::probe_set_config_variable()
	{
		if (!m_storage.api.SetConfigVariable)
		{
			rsx_log.notice("Remix: SetConfigVariable slot is NULL -- runtime rtx.* options are not "
				"settable from this client, so per-zone volumetric fog is unreachable");
			return;
		}

		const u32 status = guarded_set_config_variable(m_storage.api.SetConfigVariable,
			"rtx.rpcs3.probeKeyThatCannotExist", "0");

		// GENERAL_FAILURE is the PASS. Reported at notice level either way, with the reading
		// spelled out, because a bare error code on this line will be read months from now by
		// somebody deciding whether fog is worth attempting.
		rsx_log.notice("Remix: SetConfigVariable probe returned %s (%u) -- %s",
			error_name(status), status,
			(status == REMIXAPI_ERROR_CODE_SUCCESS)
				? "UNEXPECTED SUCCESS on a key that cannot exist: treat the slot as misrouted and "
				  "do NOT drive rtx.* options through it"
				: "expected: the slot is live and rejected an unknown key, so rtx.volumetrics.* "
				  "IS settable at runtime from this client");
	}

	void runtime::unhook_window_proc()
	{
		const HWND hwnd = m_hwnd;
		const WNDPROC original = m_original_wndproc;
		const WNDPROC hooked = m_remix_wndproc;

		// Cleared unconditionally: whatever happens below, this runtime instance has no further
		// claim on that window, and a stale snapshot must never be written back on a later call.
		m_hwnd = nullptr;
		m_original_wndproc = nullptr;
		m_remix_wndproc = nullptr;

		if (!hwnd || !original || !hooked || !IsWindow(hwnd))
		{
			return;
		}

		const bool hwnd_unicode = IsWindowUnicode(hwnd);
		const WNDPROC current = reinterpret_cast<WNDPROC>(hwnd_unicode
			? GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
			: GetWindowLongPtrA(hwnd, GWLP_WNDPROC));

		if (current != hooked)
		{
			// Either the runtime took its own hook off (dxvk-remix's ResetWindowProc does this
			// on paths that run), or something subclassed on top of it. Writing the snapshot
			// back in either case would drop somebody else's proc out of the chain.
			rsx_log.notice("Remix: HWND 0x%x no longer carries the runtime's window proc (0x%x), leaving it alone",
				reinterpret_cast<u64>(hwnd), reinterpret_cast<u64>(current));
			return;
		}

		if (hwnd_unicode)
		{
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
		}
		else
		{
			SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
		}

		rsx_log.notice("Remix: restored the original window proc on HWND 0x%x", reinterpret_cast<u64>(hwnd));
	}

	void runtime::shutdown()
	{
		// Ahead of Shutdown(), not after it. The window lives on the main thread and this runs on
		// the RSX thread, so any message dispatched while the runtime is tearing itself down would
		// otherwise still be routed into a swapchain that is in the middle of being destroyed.
		// dxvk-remix's own ResetWindowProc tolerates finding a foreign proc in place -- it only
		// restores when the proc is still its own, and erases its map entry either way.
		unhook_window_proc();

		if (!m_dll)
		{
			m_storage = {};
			m_ok = false;
			m_fork_features = false;
			return;
		}

		remixapi_lib_shutdownAndUnloadRemixDll(&m_storage.api, m_dll);
		m_storage = {};
		m_dll = nullptr;
		m_ok = false;
		m_fork_features = false;
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

	u32 guarded_create_material(PFN_remixapi_CreateMaterial fn, const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out_handle)
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

	u32 guarded_destroy_material(PFN_remixapi_DestroyMaterial fn, remixapi_MaterialHandle handle)
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

	u32 guarded_create_texture(PFN_remixapi_CreateTexture fn, const remixapi_TextureInfo* info, remixapi_TextureHandle* out_handle)
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

	u32 guarded_destroy_texture(PFN_remixapi_DestroyTexture fn, remixapi_TextureHandle handle)
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

	u32 guarded_draw_screen_overlay(PFN_remixapi_DrawScreenOverlay fn, const void* pixels, u32 width, u32 height, remixapi_Format format, f32 opacity)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(pixels, width, height, format, opacity);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_set_config_variable(PFN_remixapi_SetConfigVariable fn, const char* key, const char* value)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(key, value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_set_game_value(PFN_remixapi_SetGameValue fn, const char* key, const char* value)
	{
		if (!fn)
		{
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		}

		__try
		{
			return fn(key, value);
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
		static const f32 env = []() -> f32
		{
			if (const std::wstring text = read_env(L"RPCS3_REMIX_FARPLANE"); !text.empty())
			{
				const f32 parsed = static_cast<f32>(::_wtof(text.c_str()));
				if (parsed > 1.f)
				{
					return parsed;
				}
			}

			return 0.f;
		}();

		return env > 1.f ? env : static_cast<f32>(g_cfg.video.remix.far_plane);
	}
}

#endif
