#include "stdafx.h"
#include "RemixGSRender.h"
#include "Emu/RSX/Host/MM.h"

#ifdef _WIN32

#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/IdManager.h"
#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_locking.h"
#include "Emu/RSX/Common/BufferUtils.h"
#include "Emu/RSX/Program/ProgramStateCache.h"
#include "Emu/RSX/Remix/RemixVertexDecode.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlays.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/rsx_utils.h"
#include "Utilities/stack_trace.h"
#include "util/fnv_hash.hpp"
#include "util/sysinfo.hpp"
#include "util/tsc.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <variant>

namespace
{
	// Microsecond monotonic clock for the frame-time breakdown. Two reads per call site per
	// frame, so the measurement itself is far below the resolution of what it measures.
	inline u64 now_us()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// Ceiling on the tracked render-surface address set. A title rotates a handful of targets;
	// this only stops a pathological guest from growing the set without bound.
	constexpr usz s_max_tracked_surfaces = 256;

	// Sampling a render target does NOT by itself make a 3D draw a post-process pass: shadow
	// maps, reflection probes and environment maps are all render targets that real world
	// geometry legitimately samples, and refusing those would delete the level. A post-process
	// pass is additionally a *quad*, so the feedback gate also requires a small vertex count.
	// Tunable with RPCS3_REMIX_RTVERTS; 0 disables the shape test (refuse on the address alone).
	constexpr u32 s_rt_feedback_max_vertices = 32;

	// Frames a mesh may go unreferenced before its handle is released: no longer a constant here.
	// The live value is remix_rsx::mesh_idle_frames() (RPCS3_REMIX_MESHIDLE / "Mesh Idle Frames",
	// default 300 in system_config.h), because 300 (~5 s at 60 fps) is a compromise, not a fact - a
	// title that streams its level in large pieces wants it far higher, and nothing inside the
	// backend can tell which title it is looking at. Same shape as camera_hold_frames.

	// How often the stats line is emitted, in flips, and the wall-clock bound that also
	// forces one so a stalled or sub-1-FPS renderer still reports.
	constexpr u64 s_stats_interval_flips = 120;
	constexpr u64 s_stats_interval_us = 2'000'000;

	// How long flips may stop before end() says so.
	constexpr u64 s_flip_stall_us = 2'000'000;

	// Stall forensics. The stall's defining fact is that the *guest* stopped writing to the
	// command ring while the RSX thread kept running, so the only thing that can name the cause
	// is the guest side: which PPU/SPU threads exist, what address each is sitting on, and which
	// cpu_flags they carry. A thread frozen with cpu_flag::pause set means a global suspend is in
	// progress and stuck; a thread advancing its PC means the guest is spinning in its own code.
	// Two samples a fixed interval apart separate those two cases without another run.
	constexpr u32 s_stall_dumps_max = 3;
	constexpr u64 s_stall_dump_gap_ms = 250;

	// RSX-thread work log. The stall dump itself runs on the RSX thread, so by definition the RSX
	// thread is not inside a Remix call when it prints - what matters is whether a long call
	// happened at the moment the guest parked, which is ~2 s earlier. This keeps the last few
	// RSX-thread operations that took longer than a frame's worth of time, with wall-clock stamps,
	// so that moment can be read off the dump.
	constexpr usz s_rsx_op_log_size = 16;
	constexpr u64 s_rsx_op_log_threshold_us = 4000;

	struct rsx_op_event
	{
		const char* name;
		u64 start_us;
		u64 dur_us;
	};

	rsx_op_event s_rsx_op_log[s_rsx_op_log_size]{};
	usz s_rsx_op_log_pos = 0;

	// Scoped timer, RSX thread only (no synchronisation - single producer).
	struct rsx_op_scope
	{
		const char* name;
		u64 start;

		explicit rsx_op_scope(const char* n)
			: name(n), start(now_us())
		{
		}

		~rsx_op_scope()
		{
			const u64 dur = now_us() - start;

			if (dur >= s_rsx_op_log_threshold_us)
			{
				s_rsx_op_log[s_rsx_op_log_pos++ % s_rsx_op_log_size] = { name, start, dur };
			}
		}
	};

	// Host-side backtrace of one guest thread's OS thread. The guest-side dump can only say "this
	// thread's cia is not moving"; it cannot say whether the OS thread is spinning in recompiled
	// guest code, parked in an lv2 wait, or blocked on a host lock inside the emulator. That
	// distinction is the whole question, and only the native stack answers it.
	//
	// The target is suspended for exactly as long as it takes to copy its CONTEXT, then resumed.
	// Symbolisation (which allocates and takes DbgHelp's global lock) happens after the resume, so
	// a suspended thread can never be holding a lock this function then waits on.
	std::vector<std::string> native_backtrace(u64 native_id)
	{
		if (!native_id)
		{
			return {};
		}

		const HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
			FALSE, static_cast<DWORD>(native_id));

		if (!h)
		{
			return { fmt::format("<OpenThread failed, err=%u>", ::GetLastError()) };
		}

		CONTEXT ctx{};
		ctx.ContextFlags = CONTEXT_FULL;

		const bool suspended = ::SuspendThread(h) != static_cast<DWORD>(-1);
		const bool got = suspended && ::GetThreadContext(h, &ctx);

		if (suspended)
		{
			::ResumeThread(h);
		}

		::CloseHandle(h);

		if (!got)
		{
			return { "<GetThreadContext failed>" };
		}

		return utils::get_backtrace_symbols(utils::get_backtrace(24, &ctx));
	}

	void dump_guest_threads(const char* tag)
	{
		struct sample
		{
			u32 id;
			std::string name;
			u32 pc;
			std::string flags;
			const char* func;
			const char* last_func;
			bool ack;
			u64 checks;   // cpu_dbg::check_counter
			u64 cycles;   // host thread cycle counter
			u64 native;   // host thread id
		};

		auto collect = [](std::vector<sample>& ppus, std::vector<sample>& spus)
		{
			idm::select<named_thread<ppu_thread>>([&](u32 id, named_thread<ppu_thread>& ppu)
			{
				ppus.push_back({ id, ppu.get_name(), ppu.cia,
					fmt::format("%s%s", +ppu.state, ppu.ack_suspend ? " ACK_SUSPEND" : ""),
					ppu.current_function, ppu.last_function, ppu.ack_suspend,
					cpu_dbg::check_counter(id),
					thread_ctrl::get_cycles(ppu),
					thread_ctrl::get_native_id(ppu) });
			}, idm::unlocked);

			idm::select<named_thread<spu_thread>>([&](u32 id, named_thread<spu_thread>& spu)
			{
				spus.push_back({ id, spu.get_name(), spu.pc, fmt::format("%s", +spu.state), spu.current_func,
					nullptr, false, cpu_dbg::check_counter(id), thread_ctrl::get_cycles(spu),
					thread_ctrl::get_native_id(spu) });
			}, idm::unlocked);
		};

		std::vector<sample> ppu_a, spu_a, ppu_b, spu_b;
		const u64 sctr_a = cpu_thread::g_suspend_counter;
		collect(ppu_a, spu_a);
		std::this_thread::sleep_for(std::chrono::milliseconds(s_stall_dump_gap_ms));
		const u64 sctr_b = cpu_thread::g_suspend_counter;
		collect(ppu_b, spu_b);

		rsx_log.error("Remix stall/%s: suspend_counter %llu -> %llu (phase %llu) | ppu=%llu spu=%llu "
			"| vm_range_bits=%llx/%llx | ppu_slots=%u | lv2_pending=%u sched_ready=%d lv2_mutex_free=%d",
			tag, sctr_a, sctr_b, sctr_b & 3, static_cast<u64>(ppu_b.size()), static_cast<u64>(spu_b.size()),
			vm::g_range_lock_bits[0].load(), vm::g_range_lock_bits[1].load(),
			static_cast<u32>(g_cfg.core.ppu_threads),
			lv2_obj::get_pending_count(),
			lv2_obj::is_scheduler_ready() ? 1 : 0,
			[]
			{
				if (!lv2_obj::g_mutex.try_lock_shared())
				{
					return 0;
				}

				lv2_obj::g_mutex.unlock_shared();
				return 1;
			}());

		// lv2's own view of who is runnable. Every guest thread carrying cpu_flag::suspend while
		// this list is empty or all-suspended means the scheduler, not the title, is what stopped.
		for (u32 slot = 0; slot < static_cast<u32>(g_cfg.core.ppu_threads); slot++)
		{
			const ppu_thread* running = lv2_obj::get_running_ppu(slot);

			rsx_log.error("Remix stall/%s:   lv2_run[%u] = %s flags=%s", tag, slot,
				running ? running->get_name() : std::string("<none>"),
				running ? fmt::format("%s", +running->state) : std::string("-"));
		}

		auto report = [tag](const char* kind, const std::vector<sample>& a, const std::vector<sample>& b)
		{
			for (usz i = 0; i < b.size(); i++)
			{
				const bool paired = i < a.size() && a[i].id == b[i].id;
				const u32 before = paired ? a[i].pc : b[i].pc;

				// checks/cycles deltas are the liveness verdict: checks==0 with cycles>0 means the
				// OS thread is running but never re-entering check_state(); both 0 means it is
				// genuinely parked and the native stack below says on what.
				rsx_log.error("Remix stall/%s:   %s[0x%x] '%s' pc=0x%x->0x%x %s flags=%s checks=+%llu cycles=+%llu cur_fn=%s last_fn=%s",
					tag, kind, b[i].id, b[i].name, before, b[i].pc,
					before == b[i].pc ? "FROZEN" : "moving",
					b[i].flags,
					paired ? b[i].checks - a[i].checks : 0,
					paired ? b[i].cycles - a[i].cycles : 0,
					b[i].func ? b[i].func : "-",
					b[i].last_func ? b[i].last_func : "-");
			}
		};

		report("ppu", ppu_a, ppu_b);
		report("spu", spu_a, spu_b);

		// RSX-thread operations that overran, newest last, with how long ago they ended. If a
		// long Remix call is what the guest tripped over, one of these lands on the moment the
		// guest stopped feeding the ring.
		{
			const u64 now = now_us();
			const usz count = std::min(s_rsx_op_log_pos, s_rsx_op_log_size);

			for (usz i = 0; i < count; i++)
			{
				const auto& e = s_rsx_op_log[(s_rsx_op_log_pos - count + i) % s_rsx_op_log_size];

				rsx_log.error("Remix stall/%s:   rsx_op '%s' took %.1f ms, ended %.1f s ago",
					tag, e.name ? e.name : "-",
					static_cast<double>(e.dur_us) / 1000.0,
					static_cast<double>(now - (e.start_us + e.dur_us)) / 1e6);
			}
		}

		// Every g_pending transition still in the ring, newest last. The charge that was never
		// repaid is the last '+' with no matching '-' for the same thread id.
		{
			const auto events = lv2_obj::get_pending_log();
			const u64 tsc_now = utils::get_tsc();
			const u64 tsc_hz = std::max<u64>(1, utils::get_tsc_freq());

			for (const auto& e : events)
			{
				rsx_log.error("Remix stall/%s:   pending%c site=%c thr=0x%x flags=0x%x -> %u (%.3f ms ago)",
					tag, e.delta, e.site, e.id, e.flags, e.value,
					static_cast<double>(tsc_now - e.tsc) * 1000.0 / static_cast<double>(tsc_hz));
			}
		}

		// Native stack of every PPU thread that owes an acknowledgement, plus of the two threads
		// lv2 believes are runnable. Bounded so a wide dump cannot stall the RSX thread for long.
		u32 walked = 0;

		for (const auto& s : ppu_b)
		{
			if (!s.ack || walked >= 3)
			{
				continue;
			}

			walked++;

			const auto frames = native_backtrace(s.native);

			rsx_log.error("Remix stall/%s:   native stack of %s (tid=%llu), %llu frames:",
				tag, s.name, s.native, static_cast<u64>(frames.size()));

			for (usz i = 0; i < frames.size(); i++)
			{
				rsx_log.error("Remix stall/%s:     #%02u %s", tag, static_cast<u32>(i), frames[i]);
			}
		}
	}

	// Sanity ceiling on a single submitted mesh. Guards against a malformed draw clause
	// turning into a multi-gigabyte allocation.
	constexpr u32 s_max_vertices_per_mesh = 0x40000;
	constexpr u32 s_max_indices_per_mesh = 0x100000;

	// Archetype B has to invert and split a 4x4 to score a candidate. Every draw of a fused
	// program yields the same projection, so a handful of attempts per frame is plenty.
	constexpr u32 s_max_split_attempts_per_frame = 32;

	// Tolerance on the perspective row of a derived world transform.
	constexpr f32 s_world_affine_tolerance = 0.02f;

	// How far a bone's 3x3 basis may be from spanning three dimensions before it is refused, as the
	// ratio |det| / (|x||y||z|). 1.0 is any rotation at any uniform scale; 0 is coplanar. 1e-3
	// admits a heavily squashed but still invertible basis and rejects the collapse Resistance 2
	// produced at c30, whose determinant is exactly zero. Deliberately loose: the purpose is to
	// catch a mis-read palette, not to police a title's own modelling.
	constexpr f32 s_bone_basis_tolerance = 1e-3f;

	// How far one bone's basis or translation may sit from the median of the other bones in the
	// same draw before the draw is refused. The bones of one draw are one skeleton at one model
	// scale, so they are each other's control - and unlike s_bone_basis_tolerance these are
	// magnitude tests, which is the thing no other gate here performs: build_palette_matrix
	// synthesises the perspective column for a 3-row palette, so is_affine passes unconditionally,
	// and has_usable_basis is scale-invariant by construction.
	//
	// 8x and 64x are deliberately loose. A skeleton's bones differ by a few percent, not by an
	// order of magnitude; anything this catches is a palette read that landed outside the bones the
	// title uploaded, not a rig that squashes a limb. Both are ratios, so they hold whatever units
	// the title models in.
	constexpr f32 s_bone_scale_ratio = 8.f;
	constexpr f32 s_bone_offset_ratio = 64.f;

	// RPCS3_REMIX_UIDUMP=N logs the geometry of the first N textured UI draws: screen bbox and
	// the raw (x,y,u,v) of the first triangle. A glyph batch whose per-quad UVs span the whole
	// 0..1 atlas instead of one glyph cell is the "overlapping text" signature, and this is the
	// only way to tell that apart from a positioning fault.
	u32 ui_dump_limit()
	{
		static const u32 value = []() -> u32
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIDUMP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);
			return (parsed > 0) ? static_cast<u32>(parsed) : 0;
		}();

		return value;
	}

	// RPCS3_REMIX_UIDUMPVP=<hex vp hash> narrows the UI dump to one vertex program and makes it
	// log every quad rather than the first triangle, and writes that draw's albedo texture out
	// as a BMP once. Needed to tell "the glyph cell is right and the placement is wrong" from
	// "the placement is right and the cell is wrong".
	u64 ui_dump_vp()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIDUMPVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::wcstoull(buffer, nullptr, 16);
		}();

		return value;
	}

	// RPCS3_REMIX_UISPACE=0 restores ae94587's clip-pixel row conversion, which multiplied the
	// guest coordinate by fh/surface_clip_height and so hard-coded 'guest pixel y=0 is the top
	// row'. Nothing in any capture established that. The default now derives the same conversion
	// from the viewport registers the guest itself programmed, which state the convention
	// outright: RSX window space is y-down exactly when viewport_scale_y is negative.
	//
	// This is deliberately not a flip. On Resistance 2 (NPEA00431) the registers read
	// vp_scale_y=-352 vp_offset_y=352 at clip=1280x704, and substituting those reproduces the old
	// expression exactly - (p.y - 352)/(-352) unprojected through (1-ndc)*0.5*fh is p.y*fh/704 -
	// so R2's numbers do not move. What changes is that the convention is now read rather than
	// assumed, and a title whose viewport is y-up stops being drawn mirrored.
	bool ui_space_from_viewport()
	{
		static const bool value = []() -> bool
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UISPACE", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return true;
			}

			return ::wcstol(buffer, nullptr, 10) != 0;
		}();

		return value;
	}

	// 32-bit bottom-up BMP of a BGRA8 buffer. Debug only.
	void write_bgra_bmp(const std::string& path, const u8* pixels, u32 width, u32 height)
	{
		fs::file out{ path, fs::rewrite };

		if (!out || width == 0 || height == 0)
		{
			return;
		}

		const u32 image_bytes = width * height * 4;
		const u32 file_bytes = 14 + 40 + image_bytes;

		u8 header[54]{};
		header[0] = 'B';
		header[1] = 'M';
		std::memcpy(header + 2, &file_bytes, 4);
		const u32 offset = 54;
		std::memcpy(header + 10, &offset, 4);
		const u32 dib = 40;
		std::memcpy(header + 14, &dib, 4);
		std::memcpy(header + 18, &width, 4);
		std::memcpy(header + 22, &height, 4);
		const u16 planes = 1;
		std::memcpy(header + 26, &planes, 2);
		const u16 bpp = 32;
		std::memcpy(header + 28, &bpp, 2);
		std::memcpy(header + 34, &image_bytes, 4);
		out.write(header, sizeof(header));

		for (u32 y = height; y-- > 0;)
		{
			out.write(pixels + (usz{y} * width * 4), usz{width} * 4);
		}
	}

	constexpr remixapi_Transform s_identity_transform =
	{ {
		{ 1.f, 0.f, 0.f, 0.f },
		{ 0.f, 1.f, 0.f, 0.f },
		{ 0.f, 0.f, 1.f, 0.f },
	} };

	// remixapi_InstanceInfoBlendEXT carries *Vulkan* enum values, not D3D9 ones, even though the
	// fork is a D3D9 runtime. Verified in the fork this backend targets rather than assumed:
	// rtx_remix_api.cpp:930-936 casts srcColorBlendFactor/dstColorBlendFactor to VkBlendFactor,
	// colorBlendOp/alphaBlendOp to VkBlendOp and writeMask to VkColorComponentFlags, all into a
	// DxvkBlendMode whose members are declared with exactly those types
	// (dxvk_constant_state.h:155-164); alphaTestCompareOp becomes a VkCompareOp
	// (rtx_materials.h:1816), reinterpreted as AlphaTestType at rtx_instance_manager.cpp:691,
	// and surface_shared.h:46-58 says AlphaTestType is deliberately identical to VkCompareOp.
	// The fork's own C++ wrapper spells the encoding out in its defaults - remix.h:818-826 writes
	// "alphaTestCompareOp = 7 /* VK_COMPARE_OP_ALWAYS */" and "srcColorBlendFactor = 1
	// /* VK_BLEND_FACTOR_ONE */". D3DBLEND would have been off by one on every single value and
	// would have matched the wrong arm of calculateAlphaState()'s factor-pair table silently.
	//
	// The GCM -> Vulkan half is VKGSRender.cpp's get_blend_factor()/get_blend_op()
	// (Emu/RSX/VK/VKGSRender.cpp:133-176) transcribed, minus the fmt::throw_exception: aborting
	// the emulation over a blend factor nobody has seen yet is not a trade this path wants, so an
	// unrecognised value degrades to ONE/ZERO/ADD - the runtime's own "opaque alias", i.e. the
	// pre-ae94587 behaviour for that one draw - and is counted as blend_unmapped.
	u32 vk_blend_factor_from_gcm(rsx::blend_factor factor, bool& mapped)
	{
		switch (factor)
		{
		case rsx::blend_factor::zero: return 0;                     // VK_BLEND_FACTOR_ZERO
		case rsx::blend_factor::one: return 1;                      // VK_BLEND_FACTOR_ONE
		case rsx::blend_factor::src_color: return 2;                // VK_BLEND_FACTOR_SRC_COLOR
		case rsx::blend_factor::one_minus_src_color: return 3;      // VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
		case rsx::blend_factor::dst_color: return 4;                // VK_BLEND_FACTOR_DST_COLOR
		case rsx::blend_factor::one_minus_dst_color: return 5;      // VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
		case rsx::blend_factor::src_alpha: return 6;                // VK_BLEND_FACTOR_SRC_ALPHA
		case rsx::blend_factor::one_minus_src_alpha: return 7;      // VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
		case rsx::blend_factor::dst_alpha: return 8;                // VK_BLEND_FACTOR_DST_ALPHA
		case rsx::blend_factor::one_minus_dst_alpha: return 9;      // VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
		case rsx::blend_factor::constant_color: return 10;          // VK_BLEND_FACTOR_CONSTANT_COLOR
		case rsx::blend_factor::one_minus_constant_color: return 11;// VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR
		case rsx::blend_factor::constant_alpha: return 12;          // VK_BLEND_FACTOR_CONSTANT_ALPHA
		case rsx::blend_factor::one_minus_constant_alpha: return 13;// VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA
		case rsx::blend_factor::src_alpha_saturate: return 14;      // VK_BLEND_FACTOR_SRC_ALPHA_SATURATE
		default: break;
		}

		mapped = false;
		return 1;
	}

	u32 vk_blend_op_from_gcm(rsx::blend_equation op, bool& mapped)
	{
		switch (op)
		{
		// Same emulation VKGSRender makes: the signed variants bias the result by -0.5 on RSX and
		// there is no Vulkan (or Remix) op for that, so they run as their unsigned counterpart.
		case rsx::blend_equation::add_signed:
		case rsx::blend_equation::add: return 0;                    // VK_BLEND_OP_ADD
		case rsx::blend_equation::subtract: return 1;               // VK_BLEND_OP_SUBTRACT
		case rsx::blend_equation::reverse_subtract_signed:
		case rsx::blend_equation::reverse_subtract: return 2;       // VK_BLEND_OP_REVERSE_SUBTRACT
		case rsx::blend_equation::min: return 3;                    // VK_BLEND_OP_MIN
		case rsx::blend_equation::max: return 4;                    // VK_BLEND_OP_MAX
		default: break;                                             // incl. reverse_add_signed, which VKGSRender also refuses
		}

		mapped = false;
		return 0;
	}
}

#endif

u64 RemixGSRender::get_cycles()
{
	return thread_ctrl::get_cycles(static_cast<named_thread<RemixGSRender>&>(*this));
}

RemixGSRender::RemixGSRender(utils::serial* ar) noexcept : GSRender(ar)
{
}

void RemixGSRender::on_init_thread()
{
	GSRender::on_init_thread();

#ifdef _WIN32
	// Init, submit and present all have to happen on this thread.
	if (!m_frame)
	{
		rsx_log.error("Remix: no game window available, rendering is disabled");
		return;
	}

	if (!m_remix.initialize(m_frame->handle()))
	{
		rsx_log.error("Remix: runtime unavailable, degrading to a no-op renderer");
		return;
	}

	if (!create_debug_scene())
	{
		rsx_log.error("Remix: debug scene setup failed, degrading to a no-op renderer");
		m_remix.shutdown();
		return;
	}

	m_remix_ok = true;
	rsx_log.success("Remix: renderer is live (far plane %.1f)", static_cast<f64>(remix_rsx::hardcoded_far_plane()));
#endif
}

// Counted on the guest thread that faulted, so it cannot live in the RSX-thread-only stat block.
static atomic_t<u64> g_remix_av_seen{0};
static atomic_t<u64> g_remix_av_handled{0};

bool RemixGSRender::on_access_violation(u32 address, bool is_writing)
{
	// Mirrors VKGSRender::on_access_violation minus the texture cache, which this backend does not
	// have. The Remix backend never write-protects guest memory itself, so every fault that reaches
	// here belongs to the ZCULL report pages that rsx::reports::ZCULL_control locked down in
	// on_report_enqueued(). Returning false (the rsx::thread default this class used to inherit)
	// leaves the page PROT_NONE and the faulting guest thread spins on it forever.
	rsx::mm_flush(address);

	const u64 seen = g_remix_av_seen++;
	const bool handled = zcull_ctrl && zcull_ctrl->on_access_violation(address);

	if (handled)
	{
		g_remix_av_handled++;
	}

	if (seen < 8)
	{
		rsx_log.notice("Remix: guest access violation at 0x%x (writing=%d) handled=%d", address, is_writing ? 1 : 0, handled ? 1 : 0);
	}

	return handled;
}

void RemixGSRender::on_exit()
{
#ifdef _WIN32
	if (m_remix.ok())
	{
		const auto& api = m_remix.api();

		for (const auto& [hash, entry] : m_meshes)
		{
			if (entry.handle)
			{
				remix_rsx::guarded_destroy_mesh(api.DestroyMesh, entry.handle);
			}
		}

		if (m_debug_mesh)
		{
			remix_rsx::guarded_destroy_mesh(api.DestroyMesh, m_debug_mesh);
		}

		if (m_debug_light)
		{
			remix_rsx::guarded_destroy_light(api.DestroyLight, m_debug_light);
		}

		if (m_sun_light)
		{
			remix_rsx::guarded_destroy_light(api.DestroyLight, m_sun_light);
		}

		m_textures.destroy_all(api);
	}

	m_debug_mesh = nullptr;
	m_debug_light = nullptr;
	m_sun_light = nullptr;
	m_meshes.clear();
	m_poisoned.clear();
	m_remix_ok = false;

	// Tear the runtime down before the base class lets go of the window it presents to.
	m_remix.shutdown();
#endif

	GSRender::on_exit();
}

void RemixGSRender::pick_callback(const u32* values, u32 count, void* user)
{
	// Called from a runtime thread, not the RSX thread.
	auto* self = static_cast<RemixGSRender*>(user);

	if (!self || !values || count == 0)
	{
		return;
	}

	std::lock_guard lock(self->m_pick_mutex);

	// Logged before anything is filtered, because "the callback never fired" and "it fired and
	// every pixel read 0" and "it fired and the values are past the end of the table" are three
	// different faults with one symptom - no output at all - and guessing between them costs a
	// run each. nonzero=0 means the runtime is not writing objectPickingValue for our instances;
	// max past 'table' means the snapshot is the wrong frame's.
	u32 nonzero = 0;
	u32 highest = 0;

	for (u32 i = 0; i < count; ++i)
	{
		nonzero += (values[i] != 0) ? 1 : 0;
		highest = std::max(highest, values[i]);
	}

	{
		const std::string line = fmt::format(
			"Remix pick-readback: count=%u nonzero=%u max=%u table=%llu",
			count, nonzero, highest, static_cast<u64>(self->m_pick_snapshot.size()));

		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			out.write(line + '\n');
		}
	}

	// Distinct values only. A rect a few pixels across over one surface reads the same value
	// many times, and the interesting case - two surfaces stacked at the cursor - is exactly
	// what the duplicates would bury.
	std::unordered_set<u32> reported;

	for (u32 i = 0; i < count; ++i)
	{
		const u32 value = values[i];

		// 0 is the cleared value: nothing was drawn at that pixel.
		if (value == 0 || value > self->m_pick_snapshot.size() || !reported.insert(value).second)
		{
			continue;
		}

		const pick_record& r = self->m_pick_snapshot[value - 1];

		// albedo=0 is the answer as often as not - it is the tex_none population, the draws
		// that reach Remix with no texture at all - so it is printed rather than skipped.
		const std::string line = fmt::format(
			"Remix picked: vp=%016llx albedo=%016llX vtx=%u extent=%.4g sky=%d viewmodel=%d "
			"arch=%s skinned=%d prescale=%d affine=%d areason=%s",
			r.vp_hash, r.albedo_hash, r.vertex_count, static_cast<f64>(r.extent),
			r.sky ? 1 : 0, r.viewmodel ? 1 : 0,
			remix_rsx::archetype_name(static_cast<remix_rsx::vp_archetype>(r.archetype)),
			r.skinned ? 1 : 0, r.has_prescale ? 1 : 0, r.has_const_affine ? 1 : 0,
			r.affine_reason);

		rsx_log.success("%s", line);

		// Mirrored for the same reason every census here is: RPCS3.log is held open and
		// exclusively locked for the whole run, so a line that only lands there cannot be read
		// until the emulator is closed - and a picked draw is worth nothing after the fact,
		// because the whole point is to name what is on screen right now.
		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			out.write(line + '\n');
		}
	}

	if (!reported.empty())
	{
		// Stop retrying: the readback landed.
		self->m_pick_frames_left = 0;
	}
}

void RemixGSRender::poll_pick_request()
{
	if (!remix_rsx::pick_enabled())
	{
		return;
	}

	const auto& api = m_remix.api();

	if (!api.pick_RequestObjectPicking)
	{
		// Once: a runtime without the slot can never answer a click, and silently doing nothing
		// looks identical to a click that missed.
		if (!m_pick_slot_warned)
		{
			m_pick_slot_warned = true;
			rsx_log.error("Remix: pick_RequestObjectPicking is null - runtime too old for click-to-identify");

			if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
			{
				out.write(std::string("Remix pick: unavailable (pick_RequestObjectPicking is null)\n"));
			}
		}

		return;
	}

	// Ctrl+Click rather than a bare click, so ordinary interaction with the window - including
	// the Remix dev menu, which owns the same cursor - never fires a request.
	const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
		&& (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

	if (down && !m_pick_button_down)
	{
		POINT pt{};

		const HWND hwnd = m_frame->handle();
		RECT client{};

		if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && GetClientRect(hwnd, &client))
		{
			m_pick_x = pt.x;
			m_pick_y = pt.y;
			m_pick_frames_left = 8;

			usz table = 0;
			{
				std::lock_guard lock(m_pick_mutex);
				m_pick_snapshot = m_pick_table;
				table = m_pick_snapshot.size();
			}

			// The click itself is reported, separately from any readback. Without this a click
			// that was never seen - wrong window, cursor owned by the dev menu, Ctrl not held -
			// is indistinguishable from one that was seen and found nothing, and the pixelRegion
			// is documented against the *output* size while these are client pixels, so the
			// client extent is printed to make a scaling mismatch visible rather than inferred.
			const std::string line = fmt::format(
				"Remix pick: click at %d,%d client=%dx%d table=%llu",
				m_pick_x, m_pick_y,
				static_cast<s32>(client.right - client.left),
				static_cast<s32>(client.bottom - client.top),
				static_cast<u64>(table));

			rsx_log.success("%s", line);

			if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
			{
				out.write(line + '\n');
			}
		}
	}

	m_pick_button_down = down;

	if (m_pick_frames_left == 0)
	{
		return;
	}

	--m_pick_frames_left;

	// A few pixels either side of the cursor. One pixel is unusable in practice - it lands
	// between the thin geometry the interesting draws are made of - and a wide rect stops
	// answering the question that was asked.
	constexpr s32 radius = 3;

	const remixapi_Rect2D region{
		m_pick_x - radius,
		m_pick_y - radius,
		m_pick_x + radius,
		m_pick_y + radius };

	const u32 status = api.pick_RequestObjectPicking(&region, &RemixGSRender::pick_callback, this);

	if (status != REMIXAPI_ERROR_CODE_SUCCESS && !m_pick_request_warned)
	{
		m_pick_request_warned = true;

		const std::string line = fmt::format("Remix pick: request rejected (%s)", remix_rsx::error_name(status));
		rsx_log.error("%s", line);

		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			out.write(line + '\n');
		}
	}
}

void RemixGSRender::flip(const rsx::display_flip_info_t& info)
{
#ifdef _WIN32
	if (m_remix_ok)
	{
		const rsx_op_scope _flip_scope("flip");
		const u64 flip_enter = now_us();

		if (m_timing.window_start == 0)
		{
			m_timing.window_start = flip_enter;
		}

		submit_camera();

		// Before the table is cleared for the next frame: it still describes the geometry the
		// user was looking at when they clicked.
		poll_pick_request();
		m_pick_table.clear();

		if (remix_rsx::ui_probe_enabled() && m_remix.fork_features() && !remix_rsx::compositor_disabled())
		{
			draw_ui_probe();
		}

		// rpcs3's own overlay goes on top of the title's 2D draws, then everything 2D for this
		// frame reaches the runtime in one call, before the present.
		if (m_remix.fork_features() && !remix_rsx::compositor_disabled())
		{
			const u64 t0 = now_us();
			composite_native_overlay();
			m_timing.overlay += now_us() - t0;
		}

		const u64 t_submit = now_us();
		submit_compositor();
		m_timing.submit += now_us() - t_submit;

		const u64 t_present = now_us();
		u32 status = REMIXAPI_ERROR_CODE_SUCCESS;
		{
			const rsx_op_scope _present_scope("Present");
			status = remix_rsx::guarded_present(m_remix.api().Present, nullptr);
		}
		m_timing.present += now_us() - t_present;

		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.error("Remix: Present failed (%s)", remix_rsx::error_name(status));
		}

		// Latch this frame's winner for the next frame. The camera submitted above and the
		// world transforms used during the frame therefore always share one reference.
		//
		// A frame that produced no candidate keeps the previous one rather than clearing it. The
		// unconditional latch this replaces is what produced R2's "camera gets lost when I look at
		// the sun": a sky-dominant frame yields few or no perspective world draws, so
		// m_frame_candidate stays invalid, m_active_camera was wiped, and submit_camera() fell into
		// submit_debug_scene()'s origin camera - position 0,0,0, forward +Z, far = the configured
		// far plane. Measured over one R2 session: cam_resolved=10438 cam_fallback=5352, and
		// cam_fallback rose 5251 -> 5352 during two minutes of ordinary gameplay.
		//
		// Holding is image-exact for the archetype R2 actually resolves (arch=fused, has_reference).
		// per_draw_transform computes world = fused_now * reference_inverse on that path, so the
		// submitted clip transform is world * V_old * P_old = fused_now: the rasterised image is
		// exactly what the title asked for, and only Remix's world-space light anchoring is stale
		// for the held frames. That argument does NOT hold for a 'layered' active camera, where the
		// per-draw world is absolute - hence the age cap rather than an unbounded hold.
		if (m_frame_candidate.valid)
		{
			m_active_camera = m_frame_candidate;
			m_camera_age = 0;
		}
		else if (m_active_camera.valid && ++m_camera_age <= remix_rsx::camera_hold_frames())
		{
			++m_stats.cam_held;
		}
		else
		{
			m_active_camera = camera_candidate{};
			m_camera_age = 0;
		}

		// The viewmodel reference latches on the same flip and holds under the same cap, so the
		// arms and the world are never a frame apart. Held rather than cleared for the reason the
		// world camera is: the viewmodel population is a handful of draws and a frame that shows no
		// arms at all (a cutscene, a ladder, a reload that hides the mesh) would otherwise refuse
		// every viewmodel draw in the frame after it.
		if (m_frame_viewmodel_candidate.valid)
		{
			m_active_viewmodel = m_frame_viewmodel_candidate;
			m_viewmodel_camera_age = 0;
			++m_stats.viewmodel_cam_latched;
		}
		else if (m_active_viewmodel.valid && ++m_viewmodel_camera_age <= remix_rsx::camera_hold_frames())
		{
			++m_stats.viewmodel_cam_held;
		}
		else
		{
			m_active_viewmodel = camera_candidate{};
			m_viewmodel_camera_age = 0;
		}

		m_frame_candidate = camera_candidate{};
		m_frame_viewmodel_candidate = camera_candidate{};
		m_split_attempts = 0;

		// The scene scale the next frame's streak gate measures against. Taken at flip because the
		// current frame's median is not knowable while that frame is still being submitted, and
		// cleared every flip so a scene change carries the reference with it instead of averaging
		// across the cut. Exempt draws never entered the sample - see audit_world_extent.
		//
		// nth_element rather than sort: every drawn extent in the frame is a sample and only the
		// middle one is ever read.
		m_world_extent_samples = m_scratch_world_extents.size();

		if (m_world_extent_samples > 0)
		{
			const usz mid = m_scratch_world_extents.size() / 2;

			std::nth_element(m_scratch_world_extents.begin(),
				m_scratch_world_extents.begin() + mid,
				m_scratch_world_extents.end());

			m_world_extent_median = m_scratch_world_extents[mid];
		}
		else
		{
			m_world_extent_median = 0.f;
		}

		// clear() keeps the capacity, so the per-frame sample costs no allocation after the first
		// few frames. Without this the vector grew for the whole run.
		m_scratch_world_extents.clear();

		++m_frame_counter;
		m_textures.begin_frame();
		reap_idle_meshes();
		m_textures.reap(m_remix.api(), m_frame_counter);

		const u64 flip_exit = now_us();
		m_timing.flip += flip_exit - flip_enter;
		m_timing.window = flip_exit - m_timing.window_start;
		++m_timing.frames;
		m_last_flip_us = flip_exit;

		log_stats();
	}
#endif

	// Always run the base flip: gs_frame::flip is what shows the window on the first frame
	// and what drives the FPS readout in the title bar.
	GSRender::flip(info);

	// GSRender::flip only presents the window. rsx::thread::flip is what clears
	// async_flip_requested (without it every later guest flip request is dropped), stamps
	// last_host_flip_timestamp (the overlay refresh rate limit reads it) and clears the
	// display interrupt so do_local_task runs every 64th cycle instead of every cycle.
	// GL and VK both call it explicitly (GLPresent.cpp:518, VKPresent.cpp:966); GSRender::flip
	// does not.
	rsx::thread::flip(info);
}

void RemixGSRender::do_local_task(rsx::FIFO::state state)
{
	rsx::thread::do_local_task(state);

#ifdef _WIN32
	// Stall heartbeat. This runs on every FIFO tick whether or not draws are arriving, so it
	// reports even when the RSX thread is spinning on an empty ring - which end() cannot.
	// 'state' is the FIFO's own verdict: empty means the guest has stopped submitting and the
	// stall is upstream of this backend entirely.
	if (m_remix_ok && m_last_flip_us && ((++m_end_calls) & 0x3FF) == 0)
	{
		if (const u64 now = now_us(); now - m_last_flip_us > s_flip_stall_us)
		{
			// 'ctrl' is the guest-visible FIFO ring control, inherited from GCM_context.
			// get == put is the definition of "the guest has stopped submitting commands".
			const RsxDmaControl* dma = ctrl;

			rsx_log.error("Remix: no flip for %.1fs | fifo_state=%d in_begin_end=%d async_flip=0x%x "
				"| draws=%llu meshes_live=%llu created=%llu | get=0x%x put=0x%x",
				static_cast<f64>(now - m_last_flip_us) / 1e6,
				static_cast<int>(state),
				in_begin_end ? 1 : 0,
				static_cast<u32>(async_flip_requested),
				m_stats.draws_seen,
				static_cast<u64>(m_meshes.size()),
				m_stats.meshes_created,
				dma ? static_cast<u32>(dma->get.load()) : 0u,
				dma ? static_cast<u32>(dma->put.load()) : 0u);

			rsx_log.error("Remix stall/rsx: flip_status=%u vsync=%d vblank=%llu int_flip=%llu "
				"guest_flip_ts=%llu host_flip_ts=%llu ext_lock=%u sync_req=%d eng_mask=0x%x",
				flip_status,
				requested_vsync.load() ? 1 : 0,
				vblank_count.load(),
				int_flip_index,
				last_guest_flip_timestamp,
				last_host_flip_timestamp,
				external_interrupt_lock.load(),
				sync_point_request.load() ? 1 : 0,
				static_cast<u32>(m_eng_interrupt_mask.load()));

			// The guest side is the only thing that can name this. Bounded so a long stall does
			// not turn the log into a wall of dumps.
			if (m_stall_dumps < s_stall_dumps_max)
			{
				++m_stall_dumps;
				dump_guest_threads("t");
			}

			m_last_flip_us = now_us();
		}
	}
#endif

	if (state == rsx::FIFO::state::lock_wait)
	{
		// Critical check finished.
		return;
	}

	// A native-UI flip request has to present while the guest thread is stalled waiting on the
	// dialog it just opened - otherwise the dialog is never drawn and the guest never advances.
	// Mirrors GLGSRender::do_local_task.
	if (m_overlay_manager)
	{
		const auto should_ignore = in_begin_end && state != rsx::FIFO::state::empty;

		if ((async_flip_requested & flip_request::native_ui) && !should_ignore && !is_stopped())
		{
			rsx::display_flip_info_t info{};
			info.buffer = current_display_buffer;
			flip(info);
		}
	}
}

void RemixGSRender::end()
{
#ifdef _WIN32
	if (!m_remix_ok || skip_current_frame || !m_graphics_state.test(rsx::rtt_config_valid) || cond_render_ctrl.disable_rendering())
	{
		execute_nop_draw();
		rsx::thread::end();
		return;
	}

	// Heartbeat. If flips have stopped but draw clauses keep arriving, the RSX thread is alive
	// and the stall is downstream of end(); if this never fires while the picture is frozen,
	// the thread is stuck inside flip() or the guest stopped submitting altogether.
	if (((++m_end_calls) & 0xFF) == 0 && m_last_flip_us)
	{
		if (const u64 now = now_us(); now - m_last_flip_us > s_flip_stall_us)
		{
			rsx_log.error("Remix: no flip for %.1fs, still receiving draws (draws=%llu meshes_live=%llu created=%llu tex_live=%llu)",
				static_cast<f64>(now - m_last_flip_us) / 1e6,
				m_stats.draws_seen,
				static_cast<u64>(m_meshes.size()),
				m_stats.meshes_created,
				static_cast<u64>(m_textures.live()));

			m_last_flip_us = now;
		}
	}

	const rsx_op_scope _end_scope("end");

	// Fills current_vp_metadata (referenced input mask) and the vertex program ucode.
	analyse_current_rsx_pipeline();

	// Remember every address the RSX has rendered into. A later screen-space draw that samples
	// one of these is the title re-reading its own framebuffer - a post-process pass - not UI.
	// See composite_ui_draw for why that distinction is the difference between 13 ms and 150 ms
	// a frame. Recorded here because a blit samples a target that is no longer bound by then.
	if (m_surface_addresses.size() < s_max_tracked_surfaces)
	{
		for (const u32 addr : get_color_surface_addresses())
		{
			if (addr)
			{
				m_surface_addresses.insert(addr);
			}
		}

		if (const u32 z = get_zeta_surface_address())
		{
			m_surface_addresses.insert(z);
		}
	}
	else if (!m_surface_cap_logged)
	{
		// Past the cap the set stops learning, so a surface bound later is never recognised as
		// one and its feedback draws are submitted as geometry. Say so once rather than let the
		// gates fail silently.
		m_surface_cap_logged = true;
		rsx_log.warning("Remix: surface-address set hit its cap of %llu; later render targets will not be recognised",
			static_cast<u64>(s_max_tracked_surfaces));
	}

	// The vertex program cannot change between subdraws of one clause, so identify it once.
	m_current_vp_hash = current_vertex_program.data.empty()
		? 0
		: u64{program_hash_util::vertex_program_utils::get_vertex_program_ucode_hash(current_vertex_program)};
	m_current_fingerprint = &fingerprint_for(m_current_vp_hash);

	// The fragment program cannot change between subdraws either, so scan it once here too. The
	// hash is the same one the program caches key on, plus the export-width bit: that bit decides
	// whether COL0 is R0 or H0, so two programs with identical ucode and different shader_control
	// have different colour sources and must not share a fingerprint. Note it is read from
	// method_registers rather than current_fragment_program.ctrl - the latter is only filled by
	// get_current_fragment_program(), which this backend never calls (RSXThread.cpp:2170).
	const bool fp32_outputs = (rsx::method_registers.shader_control() & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) != 0;

	if (current_fragment_program.valid && current_fragment_program.ucode_length >= 16)
	{
		m_current_fp_hash = u64{program_hash_util::fragment_program_utils::get_fragment_program_ucode_hash(current_fragment_program)}
			^ (fp32_outputs ? 0x9e3779b97f4a7c15ull : 0ull);
		m_current_fp_fingerprint = &fp_fingerprint_for(m_current_fp_hash);
	}
	else
	{
		m_current_fp_hash = 0;
		m_current_fp_fingerprint = nullptr;
	}

	auto& draw_call = rsx::method_registers.current_draw_clause;

	draw_call.begin();
	u32 sub_index = 0;

	do
	{
		const rsx::flags32_t vertex_state_mask = rsx::vertex_base_changed | rsx::vertex_arrays_changed;
		const rsx::flags32_t vertex_state = (sub_index == 0)
			? rsx::vertex_arrays_changed
			: draw_call.execute_pipeline_dependencies(m_ctx) & vertex_state_mask;

		if (vertex_state & rsx::vertex_arrays_changed)
		{
			m_draw_processor.analyse_inputs_interleaved(m_vertex_layout, current_vp_metadata);
		}
		else if (vertex_state & rsx::vertex_base_changed)
		{
			const u32 vertex_base_offset = rsx::method_registers.vertex_data_base_offset();
			for (auto& block : m_vertex_layout.interleaved_blocks)
			{
				block->vertex_range.second = 0;
				block->real_offset_address = rsx::get_address(rsx::get_vertex_offset_from_base(vertex_base_offset, block->base_offset), block->memory_location);
			}
		}

		++m_stats.draws_seen;

		if (m_vertex_layout.validate())
		{
			const u64 t0 = now_us();
			submit_subdraw();
			m_timing.draw += now_us() - t0;
		}
		else
		{
			++m_stats.skip_layout;
		}

		++sub_index;

		if (draw_call.is_trivial_instanced_draw && !m_current_fingerprint->has_outer())
		{
			// Instance-only repeats differ solely by transform constants. Without a known
			// slot group they would all land on the same identity transform, so drop them.
			m_stats.skip_instanced += draw_call.pass_count() - 1;
			draw_call.end();
		}
	}
	while (draw_call.next());

	rsx::thread::end();
#else
	execute_nop_draw();
	rsx::thread::end();
#endif
}

#ifdef _WIN32

bool RemixGSRender::create_debug_scene()
{
	const auto& api = m_remix.api();

	// Sphere light, straight from the official remixapi_example_c.c.
	const f32 origin_light[3] = { 0.f, -1.f, 0.f };
	place_debug_light(origin_light);

	if (!m_debug_light)
	{
		return false;
	}

	// Debug triangle at z = 10, in front of the hardcoded camera at the origin.
	const auto make_vertex = [](f32 x, f32 y, f32 z)
	{
		remixapi_HardcodedVertex v{};
		v.position[0] = x;
		v.position[1] = y;
		v.position[2] = z;
		v.normal[0] = 0.f;
		v.normal[1] = 0.f;
		v.normal[2] = -1.f;
		v.texcoord[0] = 0.f;
		v.texcoord[1] = 0.f;
		v.color = 0xFFFFFFFF;
		return v;
	};

	const remixapi_HardcodedVertex verts[3] =
	{
		make_vertex(5.f, -5.f, 10.f),
		make_vertex(0.f, 5.f, 10.f),
		make_vertex(-5.f, -5.f, 10.f),
	};

	remixapi_MeshInfoSurfaceTriangles triangles{};
	triangles.vertices_values = verts;
	triangles.vertices_count = 3;
	triangles.indices_values = nullptr;
	triangles.indices_count = 0;
	triangles.skinning_hasvalue = 0;
	triangles.material = nullptr;

	remixapi_MeshInfo mesh_info{};
	mesh_info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	mesh_info.pNext = nullptr;
	mesh_info.hash = 0x1;
	mesh_info.surfaces_values = &triangles;
	mesh_info.surfaces_count = 1;

	const u32 status = remix_rsx::guarded_create_mesh(api.CreateMesh, &mesh_info, &m_debug_mesh);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS || !m_debug_mesh)
	{
		rsx_log.error("Remix: CreateMesh failed for the debug triangle (%s)", remix_rsx::error_name(status));
		return false;
	}

	return true;
}

void RemixGSRender::submit_debug_scene(bool with_triangle)
{
	const auto& api = m_remix.api();

	const int width = m_frame ? m_frame->client_width() : 0;
	const int height = m_frame ? m_frame->client_height() : 0;

	// Hardcoded camera. Deriving one from RSX state is explicitly out of scope here.
	remixapi_CameraInfoParameterizedEXT camera_ext{};
	camera_ext.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
	camera_ext.pNext = nullptr;
	camera_ext.position = { 0.f, 0.f, 0.f };
	camera_ext.forward = { 0.f, 0.f, 1.f };
	camera_ext.up = { 0.f, 1.f, 0.f };
	camera_ext.right = { 1.f, 0.f, 0.f };
	camera_ext.fovYInDegrees = 70.f;
	camera_ext.aspect = (width > 0 && height > 0) ? (static_cast<f32>(width) / static_cast<f32>(height)) : (16.f / 9.f);
	camera_ext.nearPlane = 0.1f;
	camera_ext.farPlane = remix_rsx::hardcoded_far_plane();

	remixapi_CameraInfo camera_info{};
	camera_info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
	camera_info.pNext = &camera_ext;

	remix_rsx::guarded_setup_camera(api.SetupCamera, &camera_info);

	if (m_debug_mesh && with_triangle)
	{
		remixapi_InstanceInfo instance{};
		instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		instance.pNext = nullptr;
		instance.categoryFlags = 0;
		instance.mesh = m_debug_mesh;
		instance.transform = s_identity_transform;
		instance.doubleSided = 1;

		remix_rsx::guarded_draw_instance(api.DrawInstance, &instance);
	}

	if (m_debug_light)
	{
		remix_rsx::guarded_draw_light_instance(api.DrawLightInstance, m_debug_light);
	}
}

void RemixGSRender::place_debug_light(const f32 (&position)[3])
{
	const auto& api = m_remix.api();

	if (m_debug_light)
	{
		remix_rsx::guarded_destroy_light(api.DestroyLight, m_debug_light);
		m_debug_light = nullptr;
	}

	remixapi_LightInfoSphereEXT sphere_light{};
	sphere_light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
	sphere_light.pNext = nullptr;
	sphere_light.position = { position[0], position[1], position[2] };
	sphere_light.radius = remix_rsx::debug_light_radius();
	sphere_light.shaping_hasvalue = 0;
	// Zero-init leaves this at 0, but the runtime's own default is 1.0
	// (rtx_lights.h kVolumetricRadianceScaleDefaultValue). At 0 the light contributes
	// nothing volumetrically.
	sphere_light.volumetricRadianceScale = 1.f;

	// RPCS3_REMIX_CAMLIGHT is both the on-switch and the radiance for the camera fill. Unset it
	// and this stays exactly the stage-A debug light create_debug_scene has always placed at the
	// origin for the no-camera fallback scene.
	const f32 camlight = remix_rsx::camera_light_radiance();
	const f32 radiance = (camlight > 0.f) ? camlight : remix_rsx::debug_light_radiance();

	remixapi_LightInfo light_info{};
	light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
	light_info.pNext = &sphere_light;
	light_info.hash = 0x3;
	// Neutral white. The 2x green came from NVIDIA's sample app ({100, 200, 100}); the runtime
	// gives radiance no per-channel semantics, so it only tinted every judgement green.
	// The default is deliberately not raised to compensate for the ~1.7x luma drop:
	// RPCS3_REMIX_LIGHTRADIANCE covers it, and the right value is scene-scale dependent.
	light_info.radiance = { radiance, radiance, radiance };

	const u32 status = remix_rsx::guarded_create_light(api.CreateLight, &light_info, &m_debug_light);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS || !m_debug_light)
	{
		rsx_log.error("Remix: CreateLight failed (%s)", remix_rsx::error_name(status));
		m_debug_light = nullptr;
	}
}

bool RemixGSRender::ensure_sun_light()
{
	if (m_sun_light)
	{
		return true;
	}

	if (m_sun_failed || remix_rsx::nosun_enabled())
	{
		return false;
	}

	const auto& api = m_remix.api();

	f32 direction[3]{};
	remix_rsx::sun_direction(direction);

	remixapi_LightInfoDistantEXT distant{};
	distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
	distant.pNext = nullptr;
	distant.direction = { direction[0], direction[1], direction[2] };
	distant.angularDiameterDegrees = remix_rsx::sun_angular_diameter();
	// Same reasoning as the sphere light: zero-init leaves this at 0, where the light
	// contributes nothing volumetrically, while the runtime's own default is 1.0.
	distant.volumetricRadianceScale = 1.f;

	const f32 radiance = remix_rsx::sun_radiance();

	remixapi_LightInfo light_info{};
	light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
	light_info.pNext = &distant;
	light_info.hash = 0x4;
	light_info.radiance = { radiance, radiance, radiance };

	const u32 status = remix_rsx::guarded_create_light(api.CreateLight, &light_info, &m_sun_light);

	if (status != REMIXAPI_ERROR_CODE_SUCCESS || !m_sun_light)
	{
		rsx_log.error("Remix: CreateLight(distant sun) failed (%s)", remix_rsx::error_name(status));
		m_sun_light = nullptr;
		m_sun_failed = true;
		return false;
	}

	rsx_log.notice("Remix: default sun dir=[%.4g %.4g %.4g] radiance=%.4g angle=%.4g deg",
		static_cast<f64>(direction[0]), static_cast<f64>(direction[1]), static_cast<f64>(direction[2]),
		static_cast<f64>(radiance), static_cast<f64>(remix_rsx::sun_angular_diameter()));

	return true;
}

u32 RemixGSRender::classify_draw(u64 albedo_hash)
{
	u32 flags = 0;

	if (remix_rsx::smooth_normals_enabled())
	{
		// Ahead of the hash-list gate below rather than inside it. This is a global toggle and has
		// nothing to do with whether any texture category list is populated - behind that early
		// return it would be silently dead for everyone who never filled one in, which is the
		// default state.
		flags |= REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS;
		++m_stats.cat_smooth_normals;
	}

	if (!albedo_hash || !remix_rsx::any_category_listed())
	{
		return flags;
	}

	if (remix_rsx::hash_in_category(remix_rsx::draw_category::sky, albedo_hash))
	{
		// SKY both selects the sky camera and hides the instance from the world pass, which is
		// exactly what a sun card drawing through buildings needs.
		flags |= REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
		++m_stats.cat_sky;
	}

	if (remix_rsx::hash_in_category(remix_rsx::draw_category::hide, albedo_hash))
	{
		// HIDDEN, not IGNORE: IGNORE is a no-op on the API draw path.
		flags |= REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN;
		++m_stats.cat_hidden;
	}

	if (remix_rsx::hash_in_category(remix_rsx::draw_category::particle, albedo_hash))
	{
		flags |= REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE;
		++m_stats.cat_particle;
	}

	if (remix_rsx::hash_in_category(remix_rsx::draw_category::decal, albedo_hash))
	{
		flags |= REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
		++m_stats.cat_decal;
	}

	return flags;
}

void RemixGSRender::submit_camera()
{
	if (!m_active_camera.valid || remix_rsx::nocam_enabled())
	{
		++m_stats.cam_fallback;

		// The stage-A debug triangle is the milestone-1 fallback: it proves init/camera/present
		// independently of anything the RSX produces, which is exactly what is wanted before a
		// title has ever resolved a camera. After one has, it is a regression - standing still in
		// the ship long enough for the hold to lapse replaces the entire scene with a hardcoded
		// triangle on a hardcoded camera, which is what "sometimes it just shows the render
		// triangle" is. Submitting nothing keeps the last presented frame instead of overwriting
		// the world with a test pattern.
		//
		// Deliberately not fixed by extending the hold. The hold is bounded on purpose (the
		// image-exactness argument only covers arch=fused with has_reference), and a title that
		// stops resolving a camera for minutes at a time is a separate fault that this must not
		// paper over - cam_fallback still counts every frame it happens.
		// The camera is submitted either way; only the triangle is conditional. Returning
		// without calling SetupCamera at all was the loading-screen freeze: the runtime keeps
		// presenting the last frame it was given, so a stretch with no camera reads as the image
		// locking up rather than as an empty scene. Neither the test pattern nor a stale world is
		// right - an empty frame is.
		submit_debug_scene(!m_camera_ever_valid || remix_rsx::nocam_enabled());
		return;
	}

	m_camera_ever_valid = true;
	++m_stats.cam_resolved;

	const auto& api = m_remix.api();

	remixapi_CameraInfo camera_info{};
	camera_info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
	camera_info.pNext = nullptr;
	camera_info.type = REMIXAPI_CAMERA_TYPE_WORLD;
	remix_rsx::to_camera_matrix(m_active_camera.view, camera_info.view);
	remix_rsx::to_camera_matrix(m_active_camera.projection, camera_info.projection);

	const u32 status = remix_rsx::guarded_setup_camera(api.SetupCamera, &camera_info);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS)
	{
		rsx_log.error("Remix: SetupCamera failed (%s)", remix_rsx::error_name(status));
	}

	// The sky camera, which nothing was registering. The dev menu's camera panel reads
	// "SKY: Position: -" on every frame of a Haze capture, and that is not cosmetic: tagging
	// an instance REMIXAPI_INSTANCE_CATEGORY_BIT_SKY makes convert::categoryToCameraType()
	// resolve ExternalDrawState::cameraType to CameraType::Sky, and that field's whole job is
	// to select which camera's matrices SceneManager::submitExternalDraw fetches. With no sky
	// camera ever set up, every sky-tagged draw was being resolved against an unregistered
	// camera - which is both why the dome has repeatedly landed a few feet in front of the
	// player instead of around them, and why it lights the scene from a handful of angles and
	// is black from the rest.
	//
	// Identical to the world camera, and that is the correct answer here rather than a
	// convenient one. On the D3D9 path a sky camera differs from the main one because sky is
	// rasterized separately into a cubemap by tryHandleSky(), which external draws never reach
	// (rtx_remix_api.cpp says so outright). This backend submits the dome as an ordinary
	// world-space mesh with a real world transform, so the matrices that place every other
	// draw are exactly the matrices that place it.
	if (remix_rsx::sky_camera_enabled())
	{
		camera_info.type = REMIXAPI_CAMERA_TYPE_SKY;

		const u32 sky_status = remix_rsx::guarded_setup_camera(api.SetupCamera, &camera_info);

		if (sky_status != REMIXAPI_ERROR_CODE_SUCCESS && !m_sky_camera_warned)
		{
			m_sky_camera_warned = true;
			rsx_log.error("Remix: SetupCamera(sky) failed (%s)", remix_rsx::error_name(sky_status));
		}
	}

	// The scene's readable light: one distant sun, created once, drawn every frame.
	if (ensure_sun_light())
	{
		remix_rsx::guarded_draw_light_instance(api.DrawLightInstance, m_sun_light);
	}

	// The camera sphere is now an optional fill, off by default. It is what blew out
	// everything near the player and crushed everything far from them; RPCS3_REMIX_CAMLIGHT
	// brings it back for anyone who wants it, and it still costs a destroy+create per frame
	// because it moves with the camera.
	if (remix_rsx::camera_light_radiance() > 0.f)
	{
		place_debug_light(m_active_camera.position);

		if (m_debug_light)
		{
			remix_rsx::guarded_draw_light_instance(api.DrawLightInstance, m_debug_light);
		}
	}
}

bool RemixGSRender::is_screen_space_draw() const
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	if (fp.archetype == remix_rsx::vp_archetype::screen_space)
	{
		return true;
	}

	if (!fp.has_outer())
	{
		// Unknown: keep submitting it rather than guessing it away.
		return false;
	}

	remix_rsx::slot_block slots{};
	if (!remix_rsx::read_slot_block(fp.outer_base(), slots))
	{
		return false;
	}

	const remix_rsx::mat4 outer = remix_rsx::slots_to_matrix(slots, fp.outer_shape());

	// An identity projection block is the PS3 2D convention (Tiny3D) and is also Remix's own
	// primary "this draw has no camera" test.
	if (remix_rsx::mat4_is_identity(outer, 1e-5f))
	{
		return true;
	}

	return remix_rsx::is_orthographic(outer) && !rsx::method_registers.depth_write_enabled();
}

u32 RemixGSRender::albedo_unit_mask(bool* from_ucode) const
{
	// referenced_textures_mask comes from the fragment ucode disassembly that
	// analyse_current_rsx_pipeline() already ran, so it costs nothing here. GL and VK iterate
	// the same mask (GLDraw.cpp:301, VKDraw.cpp:286) rather than trusting enabled() alone.
	const u32 referenced = current_fp_metadata.referenced_textures_mask;

	if (from_ucode)
	{
		*from_ucode = false;
	}

	if (!remix_rsx::fp_albedo_enabled() || !m_current_fp_fingerprint)
	{
		return referenced;
	}

	// Which of the referenced units the program actually reads as a colour. See
	// scan_fragment_program: a sample that only reaches COL0 through a dot product is a normal,
	// and this mask is what removes it.
	const u32 colour = u32{m_current_fp_fingerprint->colour_mask} & referenced;

	// 'colour == referenced' is not a resolution, it is the ucode declining to discriminate - the
	// answer is then identical to the lowest-unit guess and, more importantly, sanctions nothing
	// for the retry loop below. Reported as a guess so tex_albedo_ucode only ever counts draws
	// where reading the program changed what the caller may do.
	if (colour == 0 || colour == referenced)
	{
		return referenced;
	}

	if (from_ucode)
	{
		*from_ucode = true;
	}

	return colour;
}

int RemixGSRender::albedo_texture_unit(u32 skip_below) const
{
	return albedo_texture_unit_in(albedo_unit_mask(), skip_below);
}

int RemixGSRender::albedo_texture_unit_in(u32 mask, u32 skip_below) const
{
	// 'skip_below' lets the caller walk past a unit the cache could not turn into a material -
	// but only within 'mask'. Haze binds a 2048x2048 COMPRESSED_HILO8 normal map on a lower unit
	// than the diffuse map it pairs it with, and taking the lowest unit unconditionally meant
	// every one of those draws resolved a format this cache cannot decode and submitted flat
	// white with a perfectly good diffuse texture sitting one unit up. That single descriptor
	// accounted for 123,057 of the 123,059 'unsupported' draws in the p0 run - 40% of every
	// untextured draw in the frame. The walk is what recovers those; albedo_unit_mask is what
	// stops it from walking onto a normal map instead.
	for (u32 unit = 0; mask; mask >>= 1, ++unit)
	{
		if (!(mask & 1) || unit < skip_below)
		{
			continue;
		}

		const auto& tex = rsx::method_registers.fragment_textures[unit];

		if (!tex.enabled())
		{
			continue;
		}

		if (tex.get_extended_texture_dimension() != rsx::texture_dimension_extended::texture_dimension_2d)
		{
			// Cube / 3D albedo is out of scope for this milestone.
			continue;
		}

		return static_cast<int>(unit);
	}

	return -1;
}

void RemixGSRender::report_uv_failure(attribute_status best)
{
	// One line per program, once, at notice level - the same census shape the render-target and
	// indexed-const gates use. This is the line that names the root cause of a zero uv_applied
	// even when no widening of the scan hits: R2's whole session decoded 0 texcoords across
	// 2,450,329 textured draws, and the aggregate counter cannot say why.
	//
	// The two masks are the inputs to analyse_inputs_interleaved (RSXDrawCommands.cpp:19), which
	// only places attributes present in 'input_mask & referenced_inputs_mask'. An attribute the
	// vertex program never reads is therefore never placed, and map_attribute reports it absent -
	// so "absent on every attribute 8..15" and "ref=0x9" together mean the program reads position
	// and ATTR3 only, i.e. there is no texcoord attribute to find.
	if (!m_uv_fail_census_seen.insert(m_current_vp_hash).second)
	{
		return;
	}

	const auto status_name = [](attribute_status s) -> const char*
	{
		switch (s)
		{
		case attribute_status::ok:     return "ok";
		case attribute_status::absent: return "absent";
		case attribute_status::layout: return "layout";
		case attribute_status::memory: return "memory";
		}

		return "?";
	};

	const u32 referenced = u32{current_vp_metadata.referenced_inputs_mask};
	const u32 input_mask = u32{rsx::method_registers.vertex_attrib_input_mask()};

	std::string attrs;

	for (u32 index = 0; index < 16; ++index)
	{
		if (!(referenced & (1u << index)))
		{
			continue;
		}

		const auto& info = rsx::method_registers.vertex_arrays_info[index];
		const rsx::interleaved_range_info* block = find_attribute_block(index);

		fmt::append(attrs, " a%u=place%u/type%u/size%u/stride%u/blk%u",
			index,
			static_cast<u32>(m_vertex_layout.attribute_placement[index]),
			static_cast<u32>(info.type()),
			u32{info.size()},
			u32{info.stride()},
			block ? block->attribute_stride : 0u);
	}

	const std::string line = fmt::format(
		"Remix uv-fail: vp=%016llx best=%s ref=0x%x input=0x%x |%s",
		m_current_vp_hash, status_name(best), referenced, input_mask, attrs);

	rsx_log.notice("%s", line);

	// RPCS3.log is held with an exclusive lock while the emulator runs, so a dump run gets this
	// mirrored into the file that can be read live, exactly as dump_vertex_program does.
	if (remix_rsx::dump_enabled())
	{
		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			out.write(line + '\n');
		}
	}
}

void RemixGSRender::report_sky_census(sky_outcome outcome, u32 vertex_count, bool depth_write,
	const f32 (&lo)[3], const f32 (&hi)[3], const remixapi_Transform& transform,
	f32 world_extent, f32 anchor, bool measured, bool camera_inside, f32 units_per_vertex,
	u64 albedo_hash)
{
	// The aggregate counters say how many draws each gate refused, not *which*. On the first live
	// capture of the anchored-sky rule that was the whole problem: 739228 candidates, 707975
	// refused on extent, 28025 on anchor, 3228 tagged - and the dome the user could see was in one
	// of the refused piles with no way to say which. This names them.
	//
	// Bounded three ways so it cannot flood a session: one line per (program, outcome); a repeat
	// only when the pair draws something at least twice as wide as it has already reported, which
	// makes re-emission monotone in the extent; and a hard ceiling of s_max_sky_census_lines lines
	// after which the caller stops scanning bounding boxes for it at all.
	//
	// The line carries the extent *and* the raw box it came from, because the two together are
	// what distinguish "this draw is small" from "the decode in its instance transform is wrong":
	// a raw box of tens of thousands collapsing to a world extent of single digits is a decode
	// that is scaling the dome away, not a small dome.
	auto& entry = m_sky_census_seen[m_current_vp_hash];
	f32& printed = entry.printed_extent[static_cast<usz>(outcome)];

	if (printed >= 0.f && !(world_extent > printed * 2.f))
	{
		return;
	}

	printed = std::max(world_extent, 0.f);
	++m_sky_census_lines;

	const char* outcome_name = "?";

	switch (outcome)
	{
	case sky_outcome::tagged:             outcome_name = "TAGGED";             break;
	case sky_outcome::tagged_backdrop:    outcome_name = "TAGGED:backdrop";    break;
	case sky_outcome::tagged_hash:        outcome_name = "TAGGED:hash";        break;
	case sky_outcome::reject_extent:      outcome_name = "reject:extent";      break;
	case sky_outcome::reject_anchor:      outcome_name = "reject:anchor";      break;
	case sky_outcome::reject_noworld:     outcome_name = "reject:noworld";     break;
	case sky_outcome::reject_depth_write: outcome_name = "reject:depthwrite";  break;
	case sky_outcome::count:                                                   break;
	}

	const f32 raw_extent = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] });

	const std::string line = fmt::format(
		// albedo is on this line because until it was, no capture in this project could join a
		// draw's shape to its material: 'Remix tex=' is written once per unique texture at
		// creation and names no draw, and this line named no texture. That missing join is why
		// the hash rule had to be built before it could be measured.
		"Remix sky-census: vp=%016llx %s vtx=%u depth_write=%d prim=%u albedo=%016llX | "
		"raw=[%.5g %.5g %.5g]..[%.5g %.5g %.5g] rawext=%.6g wext=%.6g minext=%.6g | "
		"anchor=%.6g limit=%.6g measured=%d origin=[%.5g %.5g %.5g] cam=[%.5g %.5g %.5g] "
		"cam_age=%u cam_arch=%s | inside=%d upv=%.6g upvmin=%.6g backdrop=%d | frame=%llu line=%u/%u",
		m_current_vp_hash,
		outcome_name,
		vertex_count,
		depth_write ? 1 : 0,
		static_cast<u32>(rsx::method_registers.current_draw_clause.primitive),
		albedo_hash,
		static_cast<f64>(lo[0]), static_cast<f64>(lo[1]), static_cast<f64>(lo[2]),
		static_cast<f64>(hi[0]), static_cast<f64>(hi[1]), static_cast<f64>(hi[2]),
		static_cast<f64>(raw_extent),
		static_cast<f64>(world_extent),
		static_cast<f64>(remix_rsx::sky_min_extent()),
		static_cast<f64>(anchor),
		static_cast<f64>(remix_rsx::sky_max_anchor()),
		measured ? 1 : 0,
		static_cast<f64>(transform.matrix[0][3]),
		static_cast<f64>(transform.matrix[1][3]),
		static_cast<f64>(transform.matrix[2][3]),
		static_cast<f64>(m_active_camera.position[0]),
		static_cast<f64>(m_active_camera.position[1]),
		static_cast<f64>(m_active_camera.position[2]),
		m_camera_age,
		remix_rsx::archetype_name(m_active_camera.archetype),
		camera_inside ? 1 : 0,
		static_cast<f64>(units_per_vertex),
		static_cast<f64>(s_sky_backdrop_min_units_per_vertex),
		// What the backdrop rule makes of this draw, whether or not it is armed. A census run with
		// no env var set therefore already says which programs mode 2 would tag.
		(measured && camera_inside && world_extent >= remix_rsx::sky_min_extent()
			&& units_per_vertex >= s_sky_backdrop_min_units_per_vertex) ? 1 : 0,
		m_frame_counter,
		m_sky_census_lines,
		s_max_sky_census_lines);

	rsx_log.notice("%s", line);

	// RPCS3.log is held with an exclusive lock while the emulator runs, so this is mirrored into
	// the file that can be read live. Unconditionally, not behind dump_enabled(): the census exists
	// to be read against what is on screen during an ordinary run, and requiring a dump run to see
	// it would change the frame timing of the thing being looked at.
	if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
	{
		out.write(line + '\n');
	}
}

void RemixGSRender::report_sky_hash_census(u64 albedo_hash, const sky_hash_entry& entry, bool armed,
	f32 world_extent, f32 units_per_vertex, u32 vertex_count)
{
	// The counters say how many draws the hash rule matched and refused; this says *which textures*,
	// which is the only form of the answer that can be acted on - pasted into RPCS3_REMIX_CAT_SKY to
	// pin the rule, or into rtx.skyBoxTextures in rtx.conf, which the fork already matches against
	// API-submitted draws by albedo hash (fork_hooks::externalDrawTextureCategories,
	// dxvk-remix-numos3 src/dxvk/rtx_render/rtx_fork_submit.cpp:65).
	//
	// Bounded three ways, like the sky census: one line when a hash arms, one when it is
	// disqualified, and a hard ceiling of s_max_sky_hash_census_lines. A full budget is itself the
	// signal that the rule is wrong - a title has a handful of sky textures, not sixty-four.
	if (m_sky_hash_census_lines >= s_max_sky_hash_census_lines)
	{
		return;
	}

	++m_sky_hash_census_lines;

	const u32 total = entry.dome + entry.other;

	const std::string line = fmt::format(
		"Remix sky-hash-census: albedo=%016llX %s dome=%u other=%u total=%u agree=%.4g | "
		"vp=%016llx vtx=%u wext=%.6g minext=%.6g upv=%.6g upvmin=%.6g need=%u mode=%u | "
		"frame=%llu line=%u/%u",
		albedo_hash,
		armed ? "ARMED" : "reject:mixed",
		entry.dome,
		entry.other,
		total,
		// dome / total. 1.0 is a texture this title has never drawn as anything but a dome; the
		// distance below 1.0 is exactly the false-positive risk of arming it, per draw.
		total ? static_cast<f64>(entry.dome) / static_cast<f64>(total) : 0.0,
		entry.vp,
		vertex_count,
		static_cast<f64>(world_extent),
		static_cast<f64>(remix_rsx::sky_min_extent()),
		static_cast<f64>(units_per_vertex),
		static_cast<f64>(s_sky_backdrop_min_units_per_vertex),
		s_sky_hash_min_draws,
		remix_rsx::sky_hash_mode(),
		m_frame_counter,
		m_sky_hash_census_lines,
		s_max_sky_hash_census_lines);

	rsx_log.notice("%s", line);

	// Mirrored for the same reason the sky census is: RPCS3.log is locked while the emulator runs,
	// and this census exists to be read against what is on screen during an ordinary run.
	if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
	{
		out.write(line + '\n');
	}
}

void RemixGSRender::report_viewmodel_census(viewmodel_outcome outcome, u32 vertex_count,
	f32 scale_z, f32 offset_z, const remixapi_Transform& transform, f32 anchor, bool measured)
{
	// Names the programs the viewmodel rule selects, in the shape of 'Remix: indexed-world' and
	// 'Remix sky-census:'. The counters say how many; this says which - and on this title that is
	// the whole question, because the two programs it should name draw world characters as well
	// and a census line is the only thing that can distinguish "it tagged the arms" from "it
	// tagged the arms and a squadmate".
	//
	// Bounded twice: one line per (program, outcome) for the life of the process, and a hard
	// ceiling of s_max_viewmodel_census_lines. The dumps put the tagged population at two
	// programs, so a full budget is itself the signal that the rule is wrong.
	const u64 key = m_current_vp_hash ^ (u64{static_cast<u32>(outcome)} << 60);

	if (!m_viewmodel_census_seen.insert(key).second)
	{
		return;
	}

	++m_viewmodel_census_lines;

	const char* outcome_name = "?";

	switch (outcome)
	{
	case viewmodel_outcome::tagged:            outcome_name = "TAGGED";            break;
	case viewmodel_outcome::reject_full_range: outcome_name = "reject:fullrange";  break;
	case viewmodel_outcome::reject_offset:     outcome_name = "reject:offset";     break;
	case viewmodel_outcome::count:                                                 break;
	}

	// depth=[near..far] is the window depth span the draw was given, which is the whole rule in
	// one field: R2's world reads [0..1] and its arms read [0..0.2]. anchor and the origin/cam
	// pair are the corroboration, not the rule, and measured=0 says the anchor is meaningless on
	// this line rather than zero.
	const std::string line = fmt::format(
		"Remix viewmodel-census: vp=%016llx %s vtx=%u prim=%u depth_test=%d depth_write=%d | "
		"scale_z=%.6g offset_z=%.6g depth=[%.6g..%.6g] maxscale=%.6g maxoffset=%.6g | "
		"anchor=%.6g limit=%.6g measured=%d origin=[%.5g %.5g %.5g] cam=[%.5g %.5g %.5g] "
		"cam_age=%u | mode=%u frame=%llu line=%u/%u",
		m_current_vp_hash,
		outcome_name,
		vertex_count,
		static_cast<u32>(rsx::method_registers.current_draw_clause.primitive),
		rsx::method_registers.depth_test_enabled() ? 1 : 0,
		rsx::method_registers.depth_write_enabled() ? 1 : 0,
		static_cast<f64>(scale_z),
		static_cast<f64>(offset_z),
		static_cast<f64>(offset_z),
		static_cast<f64>(offset_z + scale_z),
		static_cast<f64>(s_viewmodel_max_depth_scale),
		static_cast<f64>(s_viewmodel_max_depth_offset),
		static_cast<f64>(anchor),
		static_cast<f64>(s_viewmodel_max_anchor),
		measured ? 1 : 0,
		static_cast<f64>(transform.matrix[0][3]),
		static_cast<f64>(transform.matrix[1][3]),
		static_cast<f64>(transform.matrix[2][3]),
		static_cast<f64>(m_active_camera.position[0]),
		static_cast<f64>(m_active_camera.position[1]),
		static_cast<f64>(m_active_camera.position[2]),
		m_camera_age,
		remix_rsx::viewmodel_mode(),
		m_frame_counter,
		m_viewmodel_census_lines,
		s_max_viewmodel_census_lines);

	rsx_log.notice("%s", line);

	// Mirrored into remix_dump.log for the same reason report_sky_census is: RPCS3.log is held
	// under an exclusive lock while the emulator runs, and this census has to be readable against
	// what is on screen during an ordinary run rather than only after it.
	if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
	{
		out.write(line + '\n');
	}
}

RemixGSRender::viewmodel_outcome RemixGSRender::classify_viewmodel_depth(f32& scale_z, f32& offset_z) const
{
	scale_z = rsx::method_registers.viewport_scale_z();
	offset_z = rsx::method_registers.viewport_offset_z();

	// Ordered so the counters partition 'considered' exactly: the span test first, because it is
	// the one that rejects the whole world, then the near-end test on what survives.
	// std::isfinite guards a register that has never been seen unset but is guest-writable.
	if (!std::isfinite(scale_z) || !(scale_z > 0.f) || scale_z >= s_viewmodel_max_depth_scale)
	{
		return viewmodel_outcome::reject_full_range;
	}

	if (!std::isfinite(offset_z) || std::abs(offset_z) > s_viewmodel_max_depth_offset)
	{
		return viewmodel_outcome::reject_offset;
	}

	return viewmodel_outcome::tagged;
}

void RemixGSRender::report_viewmodel_camera_census(const char* outcome_name)
{
	// Which reference each viewmodel program was actually divided by. The counters give the ratio;
	// this gives the names, and vm_ref/world_ref on the line say whether both references were even
	// available at the moment the draw was placed - which is what separates "the fix is off" from
	// "the fix had nothing latched to apply".
	//
	// Keyed on the outcome's first character rather than an enum because the outcomes here are
	// strings. The two REFUSED variants share a key, which is harmless: viewmodel_camera_mode()
	// caches its env read in a function-local static, so a process can only ever produce one of
	// them.
	if (m_viewmodel_camera_census_lines >= s_max_viewmodel_camera_census_lines)
	{
		return;
	}

	const u64 key = m_current_vp_hash ^ (static_cast<u64>(static_cast<u8>(outcome_name[0])) << 56);

	if (!m_viewmodel_camera_census_seen.insert(key).second)
	{
		return;
	}

	++m_viewmodel_camera_census_lines;

	const std::string line = fmt::format(
		"Remix viewmodel-camera: vp=%016llx %s vtx=%u | vm_ref=%d vm_age=%u world_ref=%d "
		"world_arch=%s cam_age=%u | mode=%u frame=%llu line=%u/%u",
		m_current_vp_hash,
		outcome_name,
		::size32(m_scratch_vertices),
		m_active_viewmodel.has_reference ? 1 : 0,
		m_viewmodel_camera_age,
		m_active_camera.has_reference ? 1 : 0,
		remix_rsx::archetype_name(m_active_camera.archetype),
		m_camera_age,
		remix_rsx::viewmodel_camera_mode(),
		m_frame_counter,
		m_viewmodel_camera_census_lines,
		s_max_viewmodel_camera_census_lines);

	rsx_log.notice("%s", line);

	if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
	{
		out.write(line + '\n');
	}
}

u32 RemixGSRender::resolve_texcoord_attribute(u32 unit, u32 first_vertex, u32 vertex_count,
	attribute_view& out, attribute_status& best, bool* from_ucode)
{
	out = attribute_view{};

	if (from_ucode)
	{
		*from_ucode = false;
	}

	// The best status any scanned attribute reached, ordered by how close it got to a usable
	// stream: absent (not fed from a persistent block at all) < layout (block found, offsets do
	// not describe a readable stream) < memory (stream described, span unreadable). Recorded
	// because R2 measured uv_none == tex_bound == 722623 - a total failure, where the aggregate
	// counter alone cannot say whether the title feeds no texcoord attribute or feeds one this
	// code mis-locates.
	best = attribute_status::absent;

	u32 chosen = no_attribute;

	const auto rank = [](attribute_status s) -> u32
	{
		switch (s)
		{
		case attribute_status::absent: return 0;
		case attribute_status::layout: return 1;
		case attribute_status::memory: return 2;
		case attribute_status::ok:     return 3;
		}

		return 0;
	};

	// "Finite" is the cheap proof that a widened candidate really is coordinates rather than some
	// packed field reinterpreted as two halves. Sampled, not exhaustive: this runs per draw, and
	// eight points spread across the range catch a garbage stream without paying a second full
	// decode pass on top of the one the caller is about to run.
	const auto decodes_finite = [&](const attribute_view& view) -> bool
	{
		const u32 step = std::max<u32>(1, vertex_count / 8);

		for (u32 i = 0; i < vertex_count; i += step)
		{
			f32 uv[4]{};

			if (!remix_rsx::decode_position(view.at(i), view.type, view.size, uv) ||
				!std::isfinite(uv[0]) || !std::isfinite(uv[1]))
			{
				return false;
			}
		}

		return true;
	};

	// 'forced' relaxes the lower bound to 1: RPCS3_REMIX_UVATTR exists to test a title that does
	// not follow the 8+unit convention, and until now it could not express "the UVs are on a low
	// attribute" at all because the bound was the same 8 the automatic scan uses. 0 stays refused -
	// that is the position attribute, never a texcoord. 'widened' additionally demands the stream
	// decode finite, which the 8..15 convention does not need: those attributes are texcoords by
	// declaration, a low attribute is only a texcoord by inference.
	const auto try_attribute = [&](u32 index, bool forced, bool widened)
	{
		if (index < (forced ? 1u : 8u) || index > 15 || chosen != no_attribute)
		{
			return;
		}

		attribute_view view{};
		const attribute_status status = map_attribute(index, first_vertex, vertex_count, view);

		if (status == attribute_status::ok && widened && !decodes_finite(view))
		{
			// Not ranked into 'best': a stream that does not decode as coordinates is not a
			// failed texcoord attribute, and reporting it as one would tell report_uv_failure's
			// census that a usable attribute was found when none was.
			//
			// Counted, because 'best' staying at 'absent' is exactly what makes this refusal
			// invisible, and it is the only one of the widened scan's gates that can refuse an
			// attribute the layout census prints as eligible. R2's post-widening capture left
			// three programs in the uv-fail census whose a1 reads place1/type3/size2/blk20 -
			// persistent, half x2, block found - which map_attribute cannot report as 'absent'
			// (RemixGSRender.cpp:1676-1687 returns absent only for a non-persistent placement or
			// a missing block, and reports 'layout'/'memory' for everything after that). This
			// gate is the sole remaining path to best=absent on those three, and this counter is
			// what says whether it fires once per program or on every draw of them.
			++m_stats.uv_nonfinite;
			return;
		}

		if (rank(status) > rank(best))
		{
			best = status;
		}

		if (status == attribute_status::ok)
		{
			out = view;
			chosen = index;
		}
	};

	if (const u32 forced = remix_rsx::texcoord_attribute(); forced != 0)
	{
		try_attribute(forced, true, false);
		return chosen;
	}

	// The ucode states which attribute feeds TEX<unit>. Everything below this point infers it from
	// the attribute's declared size and type, which is guessing at something the program spells out:
	// 'MOV>o7.xy(I0.xyxx,I0.xyzw,I0.xyzw)c0i1' is "TEX0.xy = attribute 1, no scale, no bias".
	//
	// The heuristic is not merely redundant, it is wrong on real programs. Of Resistance 2's 70
	// dumped TEX0 writers, eight read attribute 3 - a3af6e3d5f0ac8e6, 92e7472e1c0f58cd,
	// ccfc2dd606adf833, 731962646a64e3a4, 1438eb79c0843fea, 7a4a57869f9c4a1f, c0aeb056121c7061 and
	// 8496b26338eb1e01, all 'MOV>o7.xy(...)c0i3' - and the widened scan excludes attribute 3
	// outright because it is RSX's diffuse colour register on every other title. Those programs got
	// attribute 1 (a different UV set) or nothing at all.
	//
	// The scan below stays as the fallback, because the ucode does not always answer: a program that
	// assembles TEX0 from temps (ab9725268da2ac85: 'MUL>o7.xyzw(T0.xxxy,C0.xyzw,...)c20i0') or
	// texture-matrixes the position into it (ef8f10966ce1500b: 'DP4>o7.y(I0.xyzw,C0.xyzw,...)c2i0',
	// attribute 0) leaves texcoord_input at s_no_texcoord_input. RPCS3_REMIX_UVUCODE=0 skips this.
	if (const remix_rsx::vp_fingerprint* fp = m_current_fingerprint;
		fp && unit < 8 && remix_rsx::texcoord_from_ucode())
	{
		if (const u8 declared = fp->texcoord_input[unit]; declared != remix_rsx::s_no_texcoord_input)
		{
			// 'widened' so the same finite-value proof the low-attribute scan demands applies here:
			// the ucode names the attribute, it does not promise the stream behind it is readable.
			try_attribute(declared, true, true);

			if (chosen != no_attribute)
			{
				if (from_ucode)
				{
					*from_ucode = true;
				}

				return chosen;
			}
		}
	}

	try_attribute(8 + unit, false, false);

	// A program that samples unit 1 from in_tc0 (a lightmap or detail map sharing the
	// diffuse UVs) is common enough to be worth a second look before giving up.
	for (u32 index = 8; index <= 15; ++index)
	{
		try_attribute(index, false, false);
	}

	if (chosen != no_attribute || !remix_rsx::texcoord_wide_scan())
	{
		return chosen;
	}

	// ATTR8 == in_tc0 is a convention, not a rule, and Resistance 2 does not follow it. Its run
	// measured uv_absent == uv_none == tex_bound == 722623 with uv_layout = uv_memory = 0: every
	// textured draw failed with "the attribute was never placed", not with a decode fault. All 33
	// of its census programs reference only attributes 0..4 (ref= 0x3, 0x7, 0xf, 0x13, 0x17, 0x1b,
	// 0x1f - the highest bit set is bit 4), so the 8..15 pass above cannot hit even once. The UVs
	// are on ATTR1: type3/size2 (half x2), interleaved at the position's own stride, in 31 of
	// those 33 programs.
	//
	// Which attributes are eligible is the whole safety argument, because a wrong pick is worse
	// than no pick - it scrambles the texture *and* moves the mesh content hash:
	//   0                  never. The position, submitted as the vertex position.
	//   3                  never. RSX's diffuse colour register, which apply_vertex_colour reads.
	//   fp.bone_attribute  never. It feeds the address register, not a sampler.
	//   not referenced     never. analyse_inputs_interleaved only places attributes present in
	//                      'input_mask & referenced_inputs_mask' (RSXDrawCommands.cpp:19), so an
	//                      unreferenced index cannot be a real stream anyway.
	//   size != 2          never. A texcoord is two components. R2's own a2/a3 are type6/size1
	//                      packed normals and tangents and its a4 colour is type7/size4; demanding
	//                      exactly two components is what keeps those out of the UV slot.
	//
	// Known-unhandled, deliberately: two of the 33 programs (9baa914c45f319dc, c46bfca2424f1c41)
	// carry a1=type3/size4, which reads as two UV sets packed into one half4. Admitting size 4
	// here would also admit half4/float4 tangents, blend weights and secondary colours in every
	// other title, on the strength of two programs whose share of the draws was never measured.
	// RPCS3_REMIX_UVATTR=1 pins ATTR1 for the run that would measure it.
	//
	// The lowest qualifying index wins, so the pick is stable run to run: the submitted mesh is
	// content-hashed over its texcoords, and a choice that moved would mint a fresh mesh handle
	// every time it did.
	static constexpr u32 wide_scan[] = { 1, 2, 4, 5, 6, 7 };

	// Null-checked rather than dereferenced like its neighbours: this is the one texcoord path the
	// 2D compositor also calls, and it must not depend on the clause fingerprint being set.
	const remix_rsx::vp_fingerprint* fp = m_current_fingerprint;
	const u32 referenced = u32{current_vp_metadata.referenced_inputs_mask};

	for (const u32 index : wide_scan)
	{
		if (!(referenced & (1u << index)))
		{
			continue;
		}

		if (fp && fp->bone_resolved && index == fp->bone_attribute)
		{
			continue;
		}

		if (u32{rsx::method_registers.vertex_arrays_info[index].size()} != 2)
		{
			continue;
		}

		try_attribute(index, true, true);

		if (chosen != no_attribute)
		{
			break;
		}
	}

	return chosen;
}

void RemixGSRender::apply_texcoords(u32 unit, const remix_rsx::texture_entry& entry,
	const rsx::fragment_texture& tex, u32 first_vertex, u32 vertex_count)
{
	// Every world vertex used to leave here with texcoord (0,0). A mesh whose UVs are all the
	// same point samples one texel of its albedo across every pixel of every triangle, so the
	// material is applied but the picture is a flat colour - which is what Haze showed while
	// the counters said tex_bound=5.7M and mat_created=6715. The texture path was never the
	// defect; the coordinates were missing.
	//
	// RSX feeds texture unit n from vertex attribute 8+n (ATTR8 == in_tc0). That mapping is a
	// convention, not a guarantee - the fragment program's TEXn input is fed by whatever the vertex
	// program writes to o[7+n] (TEX0..TEX7 are o7..o14, per rpcs3's own output table in
	// VKVertexProgram.cpp:292-299; an earlier revision of this comment said o[9+n], which was
	// wrong) - so resolve_texcoord_attribute owns the choice: the convention, a
	// sweep of 8..15, a widened low-attribute scan, and an override knob. composite_ui_draw calls
	// the same helper, so the 2D and 3D paths cannot disagree about where a title's UVs live.
	if (remix_rsx::texcoords_disabled())
	{
		return;
	}

	attribute_view uvs{};
	attribute_status best_status = attribute_status::absent;

	bool uv_from_ucode = false;
	const u32 chosen = resolve_texcoord_attribute(unit, first_vertex, vertex_count, uvs, best_status, &uv_from_ucode);

	if (chosen == no_attribute)
	{
		++m_stats.uv_none;

		switch (best_status)
		{
		case attribute_status::layout: ++m_stats.uv_layout; break;
		case attribute_status::memory: ++m_stats.uv_memory; break;
		default:                       ++m_stats.uv_absent; break;
		}

		report_uv_failure(best_status);
		return;
	}

	f32 uv_scale[2] = { 1.f, 1.f };

	if (tex.format() & CELL_GCM_TEXTURE_UN)
	{
		// Unnormalised coordinates are in texels, exactly as on the UI path.
		uv_scale[0] = entry.width ? (1.f / static_cast<f32>(entry.width)) : 1.f;
		uv_scale[1] = entry.height ? (1.f / static_cast<f32>(entry.height)) : 1.f;
	}
	else if (uvs.type == rsx::vertex_base_type::s32k)
	{
		// S32K is the one texcoord format the vertex fetch does not normalise - rpcs3's
		// scaling_table gives it 1.0, so decode_position hands back the stored 16-bit integer
		// and the divisor lives in a vertex-program constant. Submitted raw, a 0..32767 UV
		// minifies the texture by four orders of magnitude, and every pixel of the surface
		// averages to one flat colour: the grey tree trunks and white slabs in tx1_u1/u2.
		//
		// The divisor is 4096 (12.4 fixed point). Read off the title's own data rather than
		// assumed: of the thirteen S32K programs in the tx1 census, four span a maximum of
		// exactly 4095 / 4109 / 4028 - i.e. one full tile of a 4096 unit, with the small
		// overshoot a wrap - and the rest run to ~30000-32600, which is that same unit tiled
		// seven or eight times across a wall. The alternative reading (unit 32768) would give
		// those four programs a sixteenth of their 128x128 and 512x512 textures, which no
		// content pipeline produces. RPCS3_REMIX_UVINTSCALE=<n> overrides it in one variable.
		// ...except when the program says otherwise. The paragraph above had to choose between
		// "unit 4096, tiled 7-8x" and "unit 32768" from the UV ranges alone, and picked 4096. The
		// constant settles it: on the programs that run to ~32600 Haze's c151 holds 3.05176e-05,
		// which is 1/32768 exactly, so those are one full tile of a 32768 unit and dividing them by
		// 4096 puts them 8x too large. Other programs in the same scene hold ~1/4094 and 1, so this
		// is per-program and no single fixed divisor can serve them all.
		f32 ucode_divisor = 0.f;

		if (remix_rsx::texcoord_scale_from_ucode() && m_current_fingerprint && unit < 8)
		{
			const u8 slot = m_current_fingerprint->texcoord_scale_slot[unit];
			const u8 scale_input = m_current_fingerprint->texcoord_scale_input[unit];

			// Only when the scale belongs to the attribute actually being read: a program can
			// scale one texcoord set and copy another straight through.
			if (slot != remix_rsx::s_no_texcoord_scale && scale_input == chosen)
			{
				if (f32 v[4]{}; remix_rsx::read_slot(slot, v))
				{
					// The multiply is by a reciprocal, so the divisor is 1/c. Guard against a slot
					// that is zero or not yet uploaded rather than producing an infinity.
					if (std::isfinite(v[0]) && v[0] > 0.f)
					{
						ucode_divisor = 1.f / v[0];
					}
				}
			}
		}

		const f32 divisor = ucode_divisor > 0.f
			? ucode_divisor
			: static_cast<f32>(remix_rsx::texcoord_int_scale());

		if (ucode_divisor > 0.f)
		{
			++m_stats.uv_scale_ucode;
		}
		else
		{
			++m_stats.uv_scale_fixed;
		}

		if (divisor > 0.f)
		{
			uv_scale[0] = 1.f / divisor;
			uv_scale[1] = 1.f / divisor;
		}
	}

	const bool flip_v = remix_rsx::texcoord_flip_v();

	for (u32 i = 0; i < vertex_count; ++i)
	{
		f32 uv[4]{};

		if (!remix_rsx::decode_position(uvs.at(i), uvs.type, uvs.size, uv))
		{
			// decode_position only rejects on the type/size pair, which is constant across the
			// view, so this fires on i == 0 or never - no half-written UV set escapes.
			++m_stats.uv_none;
			return;
		}

		m_scratch_vertices[i].texcoord[0] = uv[0] * uv_scale[0];
		m_scratch_vertices[i].texcoord[1] = remix_rsx::apply_v_flip(uv[1] * uv_scale[1], flip_v);
	}

	++m_stats.uv_applied;

	// Every pick the 8+unit convention did not make, whether it came from the 8..15 sweep or the
	// widened low-attribute scan - which is what says the convention is not what carried the run.
	// No separate counter for the widened path: on a title like R2 that references nothing above
	// attribute 4, uv_fallback == uv_applied already means the widened scan did all of the work,
	// and uv_applied == 0 with uv_absent == tex_bound means it did none.
	if (chosen != (8 + unit))
	{
		++m_stats.uv_fallback;
	}

	// Which of the two mechanisms actually carried the run. uv_ucode == uv_applied means the
	// programs answered for every textured draw and the heuristic below it is dead weight;
	// uv_heuristic staying large means a whole population still writes TEX0 in a shape
	// resolve_output_input refuses, and the tc0 slices in the dump name it.
	if (uv_from_ucode)
	{
		++m_stats.uv_ucode;
	}
	else
	{
		++m_stats.uv_heuristic;
	}

	// One line per (program, unit, attribute), once: if the picture comes back textured but
	// wrong, the UV range and the attribute that produced it are the first two things to read.
	if (remix_rsx::dump_enabled())
	{
		const u64 census_key = m_current_vp_hash ^ (u64{unit} << 56) ^ (u64{chosen} << 48);

		if (m_uv_census_seen.insert(census_key).second)
		{
			f32 u_lo = +3.4e38f, u_hi = -3.4e38f, v_lo = +3.4e38f, v_hi = -3.4e38f;

			for (u32 i = 0; i < vertex_count; ++i)
			{
				u_lo = std::min(u_lo, m_scratch_vertices[i].texcoord[0]);
				u_hi = std::max(u_hi, m_scratch_vertices[i].texcoord[0]);
				v_lo = std::min(v_lo, m_scratch_vertices[i].texcoord[1]);
				v_hi = std::max(v_hi, m_scratch_vertices[i].texcoord[1]);
			}

			rsx_log.notice("Remix uv: vp=%016llx unit=%u attr=%u type=%u size=%u u=[%.3f..%.3f] v=[%.3f..%.3f] tex=%ux%u",
				m_current_vp_hash, unit, chosen, static_cast<u32>(uvs.type), uvs.size,
				u_lo, u_hi, v_lo, v_hi, entry.width, entry.height);
		}
	}
}

void RemixGSRender::apply_vertex_colour(u32 first_vertex, u32 vertex_count)
{
	// Every vertex leaves the decode loop with color = 0xFFFFFFFF, so a draw that resolves no
	// albedo material reaches Remix as pure white. Some of those draws are not untextured by
	// accident - they are *vertex-coloured by design*, and Haze's sky dome (vp=fc0fac8afccec49a,
	// inputs=0x9 = position + ATTR3 only, no texcoord attribute at all) is the clearest case.
	//
	// Deliberately applied only when no material bound. A textured draw's ATTR3 modulates its
	// albedo in the title's own fragment program with per-title semantics this backend does not
	// read, so tinting one would be a guess; leaving white is the safe identity. An untextured
	// draw has nothing to lose - the alternative is the flat white it already shows.
	if (remix_rsx::vertex_colour_disabled())
	{
		return;
	}

	// ATTR3 is RSX's diffuse colour register.
	attribute_view colours{};

	if (map_attribute(3, first_vertex, vertex_count, colours) != attribute_status::ok)
	{
		return;
	}

	const auto channel = [](f32 v) -> u32
	{
		return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
	};

	for (u32 i = 0; i < vertex_count; ++i)
	{
		f32 rgba[4]{};

		// decode_position divides by the type's scale, so 'ub' (the usual colour storage)
		// arrives as 0..1 and float attributes arrive unchanged - which is the normalisation
		// a colour wants in both cases.
		if (!remix_rsx::decode_position(colours.at(i), colours.type, colours.size, rgba))
		{
			return;
		}

		m_scratch_vertices[i].color = channel(rgba[0])
			| (channel(rgba[1]) << 8)
			| (channel(rgba[2]) << 16)
			| (channel(colours.size >= 4 ? rgba[3] : 1.f) << 24);
	}

	++m_stats.vcol_applied;
}

bool RemixGSRender::samples_bound_surface() const
{
	// Every referenced 2D unit is tested, not just albedo_texture_unit()'s: that one returns the
	// *lowest* referenced unit, and a post-process pass routinely samples the framebuffer on a
	// higher unit with a gradient or a LUT on unit 0. Testing only the lowest would let exactly
	// those through.
	u32 mask = current_fp_metadata.referenced_textures_mask;

	for (u32 unit = 0; mask; mask >>= 1, ++unit)
	{
		if (!(mask & 1))
		{
			continue;
		}

		const auto& tex = rsx::method_registers.fragment_textures[unit];

		if (!tex.enabled() || tex.get_extended_texture_dimension() != rsx::texture_dimension_extended::texture_dimension_2d)
		{
			continue;
		}

		if (m_surface_addresses.contains(rsx::get_address(tex.offset(), tex.location())))
		{
			return true;
		}
	}

	return false;
}

const rsx::interleaved_range_info* RemixGSRender::find_attribute_block(u32 index) const
{
	// Every block is searched, not just the first: a bone index or a texcoord routinely lives
	// in a different interleaved block than the position.
	for (const auto* candidate : m_vertex_layout.interleaved_blocks)
	{
		for (const auto& location : candidate->locations)
		{
			if (location.index == index)
			{
				return candidate;
			}
		}
	}

	return nullptr;
}

RemixGSRender::attribute_status RemixGSRender::map_attribute(u32 index, u32 first_vertex, u32 vertex_count, attribute_view& out) const
{
	out = attribute_view{};

	if (index >= 16 || vertex_count == 0)
	{
		return attribute_status::absent;
	}

	if (m_vertex_layout.attribute_placement[index] != rsx::attribute_buffer_placement::persistent)
	{
		// Register-sourced, inline, or not fed at all: there is no strided stream to read.
		return attribute_status::absent;
	}

	const rsx::interleaved_range_info* block = find_attribute_block(index);

	if (!block || block->attribute_stride == 0)
	{
		return attribute_status::absent;
	}

	const auto& info = rsx::method_registers.vertex_arrays_info[index];
	const u32 attr_base = info.offset() & 0x7fffffff;

	if (attr_base < block->base_offset)
	{
		return attribute_status::layout;
	}

	const u32 attr_offset = attr_base - block->base_offset;
	const u32 attr_bytes = remix_rsx::attribute_byte_size(info.type(), info.size());

	if (attr_bytes == 0)
	{
		return attribute_status::layout;
	}

	const u32 stride = block->attribute_stride;

	// The span is this attribute's own: it starts at the block base and ends at the last
	// vertex's copy of this attribute, which is not where ATTR0's span ends.
	const u64 span_base = u64{block->real_offset_address} + (u64{first_vertex} * stride);
	const u64 span_bytes = (u64{vertex_count - 1} * stride) + attr_offset + attr_bytes;

	if ((span_base + span_bytes) > 0xFFFFFFFFull ||
		!vm::check_addr(span_base, vm::page_readable, static_cast<u32>(span_bytes)))
	{
		return attribute_status::memory;
	}

	out.base = vm::_ptr<const u8>(static_cast<u32>(span_base));
	out.stride = stride;
	out.offset = attr_offset;
	out.type = info.type();
	out.size = info.size();

	return attribute_status::ok;
}

bool RemixGSRender::compositor_target(u32& width, u32& height) const
{
	// DrawScreenOverlay does NOT require client-sized pixels. Read from the fork's own source:
	// dispatchScreenOverlay (rtx_fork_overlay.cpp) dispatches over the *final output* extent
	// and screen_overlay.comp.slang samples this buffer with normalised UVs -
	// uv = (threadId + 0.5) / imageSize - through a LINEAR / CLAMP_TO_EDGE sampler. So any
	// resolution is stretched to fill the output. The M4 note that it composites 1:1 and lands
	// in the top-left corner was wrong; the only thing that has to match is the aspect ratio,
	// because the stretch is independent per axis.
	//
	// The window is still what sets the aspect. The guest's own surface is only the fallback.
	u32 w = m_frame ? static_cast<u32>(std::max(0, m_frame->client_width())) : 0;
	u32 h = m_frame ? static_cast<u32>(std::max(0, m_frame->client_height())) : 0;

	if (w == 0 || h == 0)
	{
		w = rsx::method_registers.surface_clip_width();
		h = rsx::method_registers.surface_clip_height();
	}

	// A 1280x720 window at 192 DPI has a 2560x1440 client area, so the scalar CPU rasterizer
	// was paying for 3.7 M pixels per full-screen layer to display a 720p image. Cap the width
	// and let the height follow the aspect: 2560x1440 becomes 1920x1080, 1.8x fewer pixels,
	// and the compute pass stretches it back with no visible change.
	if (const u32 cap = remix_rsx::compositor_max_width(); cap != 0 && w > cap && h != 0)
	{
		h = std::max<u32>(1, static_cast<u32>((u64{h} * cap) / w));
		w = cap;
	}

	width = w;
	height = h;

	return w != 0 && h != 0;
}

void RemixGSRender::draw_ui_probe()
{
	u32 width = 0;
	u32 height = 0;

	if (!compositor_target(width, height))
	{
		return;
	}

	m_compositor.begin_frame(width, height);
	m_compositor_open = true;

	const f32 w = static_cast<f32>(width);
	const f32 h = static_cast<f32>(height);

	// A pattern chosen to be unmistakable and to answer three questions at once: is anything
	// composited, is the buffer stretched to the output, and is the channel order BGRA.
	//
	// The compositor's colour word is 0xAARRGGBB: blend() takes alpha from bits 24..31 and
	// writes bits 0..7 to byte 0 of the buffer, which submit() declares to the runtime as
	// REMIXAPI_FORMAT_B8G8R8A8_UNORM - so byte 0, i.e. bits 0..7, is BLUE. Every other
	// producer already packs that way: the title's vertex colours (composite_ui_draw),
	// rpcs3's own overlay (composite_overlay_command) and the texture decoder
	// (RemixTextures.cpp, which also declares B8G8R8A8 to CreateTexture).
	//
	// This probe was the one exception, and the run confirmed it: the bar below used to be
	// written 0xFF0000FF with a comment claiming "B=0x00 G=0x00 R=0xFF", and it rendered
	// blue (measured B=255, R=23), while the left bar written 0xFFFF0000 rendered red
	// (R=255, B=0). The literals were authored in 0xAABBGGRR order; the runtime and the
	// four real producers are right, only these two constants were wrong. Swapping the
	// pipeline instead would have broken all four.
	//
	// Opaque red bar across the top (R=0xFF in bits 16..23), opaque blue bar down the left.
	m_compositor.draw_quad(0.f, 0.f, w, h * 0.06f, 0.f, 0.f, 1.f, 1.f, nullptr, 0xFFFF0000u, true);
	m_compositor.draw_quad(0.f, 0.f, w * 0.04f, h, 0.f, 0.f, 1.f, 1.f, nullptr, 0xFF0000FFu, true);

	// Half-alpha green block in the middle: proves the alpha path, and shows the scene through it.
	m_compositor.draw_quad(w * 0.35f, h * 0.4f, w * 0.65f, h * 0.6f, 0.f, 0.f, 1.f, 1.f, nullptr, 0x8000FF00u, true);

	// One triangle, so the barycentric rasterizer is exercised too.
	const f32 tx[3] = { w * 0.75f, w * 0.95f, w * 0.85f };
	const f32 ty[3] = { h * 0.80f, h * 0.80f, h * 0.55f };
	const f32 tu[3] = { 0.f, 0.f, 0.f };
	const f32 tv[3] = { 0.f, 0.f, 0.f };
	m_compositor.draw_triangle(tx, ty, tu, tv, nullptr, 0xFFFFFFFFu, true);
}

void RemixGSRender::composite_ui_draw(u32 first_vertex, u32 vertex_count)
{
	u32 width = 0;
	u32 height = 0;

	if (!compositor_target(width, height) || m_scratch_indices.size() < 3)
	{
		++m_stats.ui_skipped;
		return;
	}

	if (!m_compositor_open)
	{
		m_compositor.begin_frame(width, height);
		m_compositor_open = true;
	}

	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	// The decoded positions are the *raw* attribute values, so the program's own matrix chain
	// has to be applied before they mean anything on screen. A program with no chain (the
	// pure screen_space archetype) already hands over post-projection coordinates.
	remix_rsx::mat4 clip = remix_rsx::mat4_identity();

	// ...except when the chain matcher rejected a real transform for having too few slots. A 2D
	// ortho writes HPOS.x and HPOS.y from one constant each and puts a fixed depth in z/w, so it
	// never has the four consecutive slots match_dp4_chain requires, and before 9c73eb0 its
	// transform was silently dropped here. Resistance 2's menu backdrop fc5915fd48d91a98 is that
	// shape (consts=3: x<-c32, y<-c33, zw<-c35), and compositing its raw attribute bbox of
	// [-1.064 -0.02847]..[1.064 0.5107] is what drew the menu mirrored and at half height - the
	// dropped row is the one carrying the y flip. RPCS3_REMIX_ORTHO2D=0 restores dropping it.
	//
	// Note this counter is incremented *before* the space classification and nothing between the
	// two can return for a group_count == 0 program - the group loop and build_prescale are both
	// gated on group_count, and a finite ortho over finite attributes passes the isfinite checks.
	// So every rebuilt draw reaches the classification, which makes ui_ortho2d directly comparable
	// against ui_ndc + ui_pixel. That comparison is load-bearing: the first audit run had
	// ui_ortho2d=66702 against ui_ndc=22413, so at least 44289 rebuilt draws were classified as
	// clip-pixel, not NDC. The ortho2d family is therefore not confined to the NDC branch, and
	// "the NDC branch votes inverted" does not contradict "the ortho2d text is upright on screen".
	//
	// used_ortho2d carries the per-draw fact down to the orientation audit so the inverted
	// population can be named instead of inferred.
	bool used_ortho2d = false;

	if (fp.group_count == 0)
	{
		if (remix_rsx::mat4 ortho{}; remix_rsx::build_ortho2d(fp, ortho))
		{
			clip = ortho;
			used_ortho2d = true;
			++m_stats.ui_ortho2d;
		}
	}

	for (u32 i = 0; i < fp.group_count; ++i)
	{
		remix_rsx::slot_block slots{};

		if (!remix_rsx::read_slot_block(fp.group_base[i], slots))
		{
			++m_stats.ui_skipped;
			return;
		}

		clip = remix_rsx::mat4_multiply(clip, remix_rsx::slots_to_matrix(slots, fp.group_shape[i]));
	}

	if (remix_rsx::mat4 prescale{}; fp.group_count && remix_rsx::build_prescale(fp, prescale))
	{
		clip = remix_rsx::mat4_multiply(prescale, clip);
	}

	if (!remix_rsx::mat4_is_finite(clip))
	{
		++m_stats.ui_skipped;
		return;
	}

	m_scratch_ui_x.clear();
	m_scratch_ui_y.clear();
	m_scratch_ui_x.resize(vertex_count);
	m_scratch_ui_y.resize(vertex_count);

	f32 lo[2] = { +3.4e38f, +3.4e38f };
	f32 hi[2] = { -3.4e38f, -3.4e38f };

	for (u32 i = 0; i < vertex_count; ++i)
	{
		const remixapi_HardcodedVertex& v = m_scratch_vertices[i];
		const f32 p[4] = { v.position[0], v.position[1], v.position[2], 1.f };
		f32 out[4]{};

		for (u32 j = 0; j < 4; ++j)
		{
			out[j] = (p[0] * clip.m[0][j]) + (p[1] * clip.m[1][j]) + (p[2] * clip.m[2][j]) + (p[3] * clip.m[3][j]);
		}

		if (std::isfinite(out[3]) && std::abs(out[3]) > 1e-6f && std::abs(out[3] - 1.f) > 1e-6f)
		{
			out[0] /= out[3];
			out[1] /= out[3];
		}

		if (!std::isfinite(out[0]) || !std::isfinite(out[1]))
		{
			++m_stats.ui_skipped;
			return;
		}

		m_scratch_ui_x[i] = out[0];
		m_scratch_ui_y[i] = out[1];

		lo[0] = std::min(lo[0], out[0]);
		hi[0] = std::max(hi[0], out[0]);
		lo[1] = std::min(lo[1], out[1]);
		hi[1] = std::max(hi[1], out[1]);
	}

	const f32 fw = static_cast<f32>(width);
	const f32 fh = static_cast<f32>(height);

	// The guest authors its 2D against the render surface; the compositor buffer is the
	// window. Pixel-space draws therefore need the ratio between them.
	const f32 clip_w = static_cast<f32>(std::max<u32>(1, rsx::method_registers.surface_clip_width()));
	const f32 clip_h = static_cast<f32>(std::max<u32>(1, rsx::method_registers.surface_clip_height()));

	// Two shapes were observed in M2's dumps and both survive here: normalised device
	// coordinates, and coordinates already in clip pixels.
	const f32 extent = std::max(std::max(std::abs(lo[0]), std::abs(hi[0])), std::max(std::abs(lo[1]), std::abs(hi[1])));

	// Diagnostic only, no behaviour change: does this draw's post-matrix bbox fit inside the
	// unit square? If it does, the NDC branch below and a [0,1] top-left screen space are
	// observationally identical here, and the branch is a coin toss. R2 emits both shapes -
	// most of its 2D programs carry [2 0 0 0 | 0 -2 0 0 | 0 0 1 0 | -1 1 0 1] over an attribute
	// bbox of [0 0]..[1 1], which is genuine NDC, but ef8f10966ce1500b lands its whole draw in
	// x 0.55..0.75 / y 0.43..0.78 and 2f7d2b0aefd94351 in x -0.04..0.10 / y 1.00..1.07, neither
	// of which reaches a single NDC edge. The slack is 0.05 so a quad authored flush to a [0,1]
	// edge still counts.
	const bool space_unit =
		lo[0] >= -0.05f && hi[0] <= 1.05f && lo[1] >= -0.05f && hi[1] <= 1.05f;

	// Which branch mapped this draw, so the orientation audit below can be attributed to it and
	// the known-good NDC family acts as the control for the pixel one.
	bool mapped_pixel = false;

	if (extent <= 1.5f)
	{
		++m_stats.ui_space_ndc;
		m_stats.ui_space_unit += space_unit ? 1 : 0;

		for (u32 i = 0; i < vertex_count; ++i)
		{
			m_scratch_ui_x[i] = (m_scratch_ui_x[i] + 1.f) * 0.5f * fw;
			m_scratch_ui_y[i] = (1.f - m_scratch_ui_y[i]) * 0.5f * fh;
		}
	}
	else if (extent <= (std::max(clip_w, clip_h) * 2.f))
	{
		++m_stats.ui_space_pixel;
		mapped_pixel = true;

		// The legacy conversion. It is only correct when the guest's window space is y-down, and
		// ae94587 asserted that rather than reading it.
		f32 sx = fw / clip_w;
		f32 sy = fh / clip_h;
		f32 bx = 0.f;
		f32 by = 0.f;
		bool from_viewport = false;

		// The guest states its own convention in the viewport registers, and this branch's input
		// is by construction in the space those registers produce: a 'clip pixel' draw is one
		// whose vertices are already window coordinates. Inverting the viewport back to NDC and
		// re-projecting through the same (1-y)*0.5*fh the NDC branch uses makes the two branches
		// agree by derivation instead of by coincidence:
		//     ndc = (p - offset) / scale
		//     col = (ndc.x + 1) * 0.5 * fw      row = (1 - ndc.y) * 0.5 * fh
		// which collapses to the affine form below. Resistance 2's registers (scale -352 / offset
		// 352 at clip height 704) put sy back at fh/704 and by at 0, i.e. bit-for-bit the legacy
		// expression - this is not a flip, it is the same number with its assumption removed.
		if (ui_space_from_viewport())
		{
			const f32 vsx = rsx::method_registers.viewport_scale_x();
			const f32 vsy = rsx::method_registers.viewport_scale_y();
			const f32 vox = rsx::method_registers.viewport_offset_x();
			const f32 voy = rsx::method_registers.viewport_offset_y();

			if (std::isfinite(vsx) && std::isfinite(vsy) && std::isfinite(vox) && std::isfinite(voy) &&
				std::abs(vsx) > 1e-3f && std::abs(vsy) > 1e-3f)
			{
				// col = ((p.x - vox)/vsx + 1) * 0.5 * fw
				sx = (0.5f * fw) / vsx;
				bx = ((-vox / vsx) + 1.f) * 0.5f * fw;

				// row = (1 - (p.y - voy)/vsy) * 0.5 * fh
				sy = -(0.5f * fh) / vsy;
				by = (1.f + (voy / vsy)) * 0.5f * fh;

				from_viewport = true;

				// vsy < 0 is the standard cellGcmSetViewport form and means window y grows
				// downward, which is what the legacy expression assumed. vsy > 0 is the case it
				// got wrong. Counted rather than asserted so the next capture says which one this
				// title is in one number.
				if (vsy < 0.f)
				{
					++m_stats.ui_space_vp_ydown;
				}
				else
				{
					++m_stats.ui_space_vp_yup;
				}
			}
		}

		if (!from_viewport)
		{
			++m_stats.ui_space_vp_fallback;
		}

		for (u32 i = 0; i < vertex_count; ++i)
		{
			m_scratch_ui_x[i] = (m_scratch_ui_x[i] * sx) + bx;
			m_scratch_ui_y[i] = (m_scratch_ui_y[i] * sy) + by;
		}
	}
	else
	{
		// Neither space. Guessing would put the UI somewhere arbitrary; counting is honest.
		++m_stats.ui_space_none;
		++m_stats.ui_skipped;
		return;
	}

	// --- texture and texcoords ----------------------------------------------------------
	const remix_rsx::texture_entry* entry = nullptr;
	attribute_view uvs{};
	bool have_uv = false;
	f32 uv_scale[2] = { 1.f, 1.f };

	if (const int unit = albedo_texture_unit(); unit >= 0)
	{
		const auto& tex = rsx::method_registers.fragment_textures[unit];

		// A full-screen quad sampling something the RSX itself rendered into is a post-process
		// pass (tone map, bloom, colour grade), not UI. Compositing it is wrong twice over:
		// Remix already owns the final image, and with 'Write Color Buffers' off the guest copy
		// of that surface holds stale bytes anyway. It is also what makes this path unusable -
		// each one is a full compositor-buffer fill through the scalar rasterizer, ~20 ms at
		// 1920x1083, and Haze issues three to seven of them per frame.
		if (!remix_rsx::keep_render_target_blits() &&
			m_surface_addresses.contains(rsx::get_address(tex.offset(), tex.location())))
		{
			++m_stats.ui_render_target;
			return;
		}

		// refresh_pixels: this path rasterizes from the CPU copy, and a title that rewrites a
		// font atlas under a stable descriptor otherwise keeps the page that was resident when
		// the entry was created. Only the CPU copy is rebuilt, so no mesh key moves.
		m_textures.bind(m_remix.api(), tex, m_frame_counter, &entry, true);

		if (entry && entry->pixels.empty())
		{
			entry = nullptr;
		}

		if (entry)
		{
			// The same resolution the 3D path runs, not the bare 8+unit lookup this used to do.
			// ATTR8 == in_tc0 is a convention, and a title that does not follow it lost every 2D
			// draw here twice over: no UVs meant the texture did not count towards the colour gate
			// below, so the quad was refused outright. R2 (NPEA00431) carries its texcoords on
			// ATTR1 in 31 of its 33 census programs and measured ui_draws=0 / ui_no_colour=122580
			// over 3968 frames - the whole live-rendered main menu, refused.
			attribute_status uv_status = attribute_status::absent;

			have_uv = resolve_texcoord_attribute(static_cast<u32>(unit), first_vertex, vertex_count,
				uvs, uv_status) != no_attribute;

			if (tex.format() & CELL_GCM_TEXTURE_UN)
			{
				// Unnormalised coordinates are in texels.
				uv_scale[0] = (entry->width != 0) ? (1.f / static_cast<f32>(entry->width)) : 1.f;
				uv_scale[1] = (entry->height != 0) ? (1.f / static_cast<f32>(entry->height)) : 1.f;
			}
		}
	}

	// --- flat tint ----------------------------------------------------------------------
	// Per-vertex interpolation is out of scope; vertex 0's colour is the whole draw's tint.
	u32 tint = 0xFFFFFFFFu;
	bool have_colour = false;

	if (attribute_view colours{}; map_attribute(3, first_vertex, vertex_count, colours) == attribute_status::ok)
	{
		f32 rgba[4] = { 1.f, 1.f, 1.f, 1.f };

		if (remix_rsx::decode_position(colours.at(0), colours.type, colours.size, rgba))
		{
			have_colour = true;
			const auto channel = [](f32 v)
			{
				return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
			};

			// The compositor works in BGRA; RSX ATTR3 is R,G,B,A in component order.
			tint = channel(rgba[2]) | (channel(rgba[1]) << 8) | (channel(rgba[0]) << 16) | (channel(rgba[3]) << 24);
		}
	}

	// Neither an albedo texture nor a vertex colour: the draw's colour lives somewhere this
	// compositor cannot read (a vertex/fragment program constant). Filling it with the default
	// white tint at alpha 1 paints an opaque slab over everything behind it, which is exactly
	// the symptom M4 shipped. Refusing and counting is the honest fallback.
	// Note the texture only counts when texcoords were resolved too: draw_triangle is handed
	// 'have_uv ? entry : nullptr', so a texture without UVs rasterizes as flat tint just the
	// same.
	if (!(entry && have_uv) && !have_colour)
	{
		++m_stats.ui_no_colour;
		return;
	}

	const bool clamp_uv = entry && (entry->wrap_u == 0 || entry->wrap_v == 0);
	const bool flip_v = remix_rsx::texcoord_flip_v();

	// Diagnostic: remember the draw covering the most screen area this stats window, with the
	// albedo unit and pixel count it resolved. A large draw with unit=-1 / pixels=0 is a draw
	// the rasterizer fills with flat tint, which is the white-slab signature.
	{
		f32 sx_lo = +3.4e38f, sx_hi = -3.4e38f, sy_lo = +3.4e38f, sy_hi = -3.4e38f;

		for (u32 i = 0; i < vertex_count; ++i)
		{
			sx_lo = std::min(sx_lo, m_scratch_ui_x[i]);
			sx_hi = std::max(sx_hi, m_scratch_ui_x[i]);
			sy_lo = std::min(sy_lo, m_scratch_ui_y[i]);
			sy_hi = std::max(sy_hi, m_scratch_ui_y[i]);
		}

		if (const f32 area = (sx_hi - sx_lo) * (sy_hi - sy_lo); area > m_ui_biggest.area)
		{
			m_ui_biggest.area = area;
			m_ui_biggest.vp_hash = m_current_vp_hash;
			m_ui_biggest.unit = albedo_texture_unit();
			m_ui_biggest.pixels = entry ? entry->pixels.size() : 0;
			m_ui_biggest.verts = vertex_count;
			m_ui_biggest.inputs = u32{current_vp_metadata.referenced_inputs_mask};
			m_ui_biggest.tint = tint;
			m_ui_biggest.have_uv = have_uv;
			m_ui_biggest.clip_lo[0] = lo[0];
			m_ui_biggest.clip_lo[1] = lo[1];
			m_ui_biggest.clip_hi[0] = hi[0];
			m_ui_biggest.clip_hi[1] = hi[1];
			m_ui_biggest.clip_unit = space_unit;
		}
	}

	for (usz t = 0; (t + 2) < m_scratch_indices.size(); t += 3)
	{
		const u32 i0 = m_scratch_indices[t + 0];
		const u32 i1 = m_scratch_indices[t + 1];
		const u32 i2 = m_scratch_indices[t + 2];

		const f32 x[3] = { m_scratch_ui_x[i0], m_scratch_ui_x[i1], m_scratch_ui_x[i2] };
		const f32 y[3] = { m_scratch_ui_y[i0], m_scratch_ui_y[i1], m_scratch_ui_y[i2] };

		f32 u[3] = { 0.f, 0.f, 0.f };
		f32 v[3] = { 0.f, 0.f, 0.f };

		if (have_uv)
		{
			const u32 tri[3] = { i0, i1, i2 };

			for (u32 c = 0; c < 3; ++c)
			{
				f32 uv[4]{};

				if (!remix_rsx::decode_position(uvs.at(tri[c]), uvs.type, uvs.size, uv))
				{
					have_uv = false;
					break;
				}

				u[c] = uv[0] * uv_scale[0];
				v[c] = remix_rsx::apply_v_flip(uv[1] * uv_scale[1], flip_v);
			}
		}

		// The orientation audit. A textured UI quad is upright exactly when the atlas row it
		// samples grows the same way the composited row does - v and y must move together, or the
		// glyph is drawn upside down inside a correctly placed cell. That is a property of the
		// three numbers already in hand, so it needs no ucode reading, no screenshot and no guess
		// about which coordinate space the draw was authored in.
		//
		// Split by branch because the point is the comparison, not the absolute value: the
		// ortho2d/NDC family is confirmed to render correctly today, so it calibrates what 'ok'
		// looks like for this title's atlases and V convention. ndc_ok high with pixel_bad high is
		// the remaining flip, proven, in two numbers. Both families agreeing means the flip is not
		// in this classification at all and the next place to look is the fragment side.
		//
		// Only the first triangle votes (one vote per draw, not per triangle), and only when the
		// triangle actually spans some v and some y - a degenerate or axis-parallel edge carries
		// no orientation and votes for neither.
		// The gates matter as much as the sign. v is only an atlas row when a texture was actually
		// resolved - without one the draw rasterizes as flat tint and 'v' is whatever attribute
		// resolve_texcoord_attribute happened to pick, which votes on nothing. And the sign of
		// dv/dy only describes an orientation when v genuinely varies along y: on a rotated or
		// skewed sprite v varies mostly along x and the covariance picks up a cross term whose
		// sign is meaningless. Requiring the correlation to be near-perfect keeps axis-aligned UI
		// (where a quad's triangle gives |r| == 1 exactly) and abstains on everything else. The
		// first run's audit had neither gate, which is one reason its aggregate cannot be trusted.
		if (t == 0 && have_uv && entry)
		{
			const f32 my = (y[0] + y[1] + y[2]) / 3.f;
			const f32 mv = (v[0] + v[1] + v[2]) / 3.f;
			const f32 mx = (x[0] + x[1] + x[2]) / 3.f;

			f32 cov = 0.f;
			f32 var_y = 0.f;
			f32 var_v = 0.f;
			f32 cov_x = 0.f;
			f32 var_x = 0.f;

			for (u32 c = 0; c < 3; ++c)
			{
				cov += (y[c] - my) * (v[c] - mv);
				var_y += (y[c] - my) * (y[c] - my);
				var_v += (v[c] - mv) * (v[c] - mv);
				cov_x += (x[c] - mx) * (v[c] - mv);
				var_x += (x[c] - mx) * (x[c] - mx);
			}

			// A quarter of a composited pixel of vertical span, and a v span wide enough to be a
			// real texel step rather than interpolation noise.
			const bool spans = var_y > 0.0625f && var_v > 1e-8f;
			const f32 denom = std::sqrt(var_y * var_v);
			const f32 r = (denom > 0.f) ? (cov / denom) : 0.f;

			// v must track y and not x. Both tests, because a triangle can satisfy one by accident.
			const bool axis_aligned = std::abs(r) > 0.9f &&
				(var_x <= 0.0625f || std::abs(cov_x) * std::abs(cov_x) * var_y <= std::abs(cov) * std::abs(cov) * var_x);

			if (std::isfinite(cov) && std::isfinite(cov_x) && spans && axis_aligned)
			{
				ui_vote_row* row = nullptr;

				for (u32 i = 0; i < s_ui_vote_rows; ++i)
				{
					if (m_ui_votes[i].vp_hash == m_current_vp_hash)
					{
						row = &m_ui_votes[i];
						break;
					}

					if (m_ui_votes[i].vp_hash == 0)
					{
						m_ui_votes[i].vp_hash = m_current_vp_hash;
						row = &m_ui_votes[i];
						break;
					}
				}

				if (row)
				{
					row->ortho = used_ortho2d;
				}
				else
				{
					++m_ui_vote_spill;
				}

				if (mapped_pixel)
				{
					if (cov > 0.f)
					{
						++m_stats.ui_vflip_pixel_ok;
						if (row) ++row->pixel_ok;
					}
					else
					{
						++m_stats.ui_vflip_pixel_bad;
						if (row) ++row->pixel_bad;
					}
				}
				else
				{
					if (cov > 0.f)
					{
						++m_stats.ui_vflip_ndc_ok;
						if (row) ++row->ndc_ok;
					}
					else
					{
						++m_stats.ui_vflip_ndc_bad;
						if (row) ++row->ndc_bad;
					}
				}
			}
			else
			{
				++m_stats.ui_vflip_abstain;
			}
		}

		if (t == 0 && entry && have_uv && ui_dump_vp() != 0 && m_current_vp_hash == ui_dump_vp() &&
			m_ui_dumped < ui_dump_limit())
		{
			++m_ui_dumped;

			if (m_ui_dumped == 1)
			{
				write_bgra_bmp(fs::get_executable_dir() + "remix_atlas.bmp",
					entry->pixels.data(), entry->width, entry->height);

				// Which unit the fragment program actually samples is an assumption
				// (albedo_texture_unit takes the lowest referenced+enabled 2D unit). List every
				// unit so a mismatch between the sampled atlas and the authored UVs is visible.
				std::string units = fmt::format("Remix ui-units: chosen=%d fp_mask=0x%x",
					albedo_texture_unit(), u32{current_fp_metadata.referenced_textures_mask});

				for (u32 unit = 0; unit < 16; ++unit)
				{
					const auto& t = rsx::method_registers.fragment_textures[unit];

					if (!t.enabled())
					{
						continue;
					}

					fmt::append(units, " | u%u %ux%u fmt=%02x mips=%u dim=%u ref=%d", unit,
						u32{t.width()}, u32{t.height()},
						u32{t.format()} & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
						u32{t.get_exact_mipmap_count()},
						static_cast<u32>(t.get_extended_texture_dimension()),
						(current_fp_metadata.referenced_textures_mask >> unit) & 1);
				}

				rsx_log.notice("%s", units);

				// The one value no capture has ever carried, and the one that closes the
				// screen_space(0-1 constants feed HPOS) case outright.
				//
				// That archetype is assigned when the position slice reads at most one constant,
				// so there is no matrix for build_ortho2d to rebuild and composite_ui_draw falls
				// through to classifying the raw attribute by its magnitude. Resistance 2's only
				// program of that shape, 73eb16ea7b1cbed6, is
				//     9:ADD>r0.zw(C0.xxxy,..)c6  10:RCP>r0.y(C0.wwww)c6  11:RCP>r0.x(C0.zzzz)c6
				//     12:MUL>o0.y(T0.yyyy,T0.wwww)  13:MUL>o0.x(T0.zzzz,T0.xxxx)
				// i.e. HPOS.xy = (attr.xy + c6.xy) / c6.zw over an attribute bbox of exactly
				// [0 0]..[1280 704] against clip=1280x704 - a viewport inverse. Solving it against
				// the viewport leaves precisely one bit undetermined by geometry: c6.w is -352 if
				// the guest authored y downward and +352 if upward, and c6.y is -352 either way.
				// Printing the slots next to the registers they should mirror makes that bit
				// readable directly instead of inferred, and confirms or refutes the reading that
				// c6 is just (-offset.xy, scale.xy).
				std::string consts = fmt::format(
					"Remix ui-consts: vp=%016llx arch=%s(%s) clip=%ux%u vp_scale=%.6g,%.6g vp_offset=%.6g,%.6g",
					m_current_vp_hash,
					remix_rsx::archetype_name(fp.archetype),
					fp.note,
					static_cast<u32>(rsx::method_registers.surface_clip_width()),
					static_cast<u32>(rsx::method_registers.surface_clip_height()),
					static_cast<f64>(rsx::method_registers.viewport_scale_x()),
					static_cast<f64>(rsx::method_registers.viewport_scale_y()),
					static_cast<f64>(rsx::method_registers.viewport_offset_x()),
					static_cast<f64>(rsx::method_registers.viewport_offset_y()));

				// The low slots are where a 2D program parks its screen constants: R2's ortho2d
				// family uses c32/c33/c35 and is already rebuilt, the one that is not uses c6.
				// 16 slots is enough to cover both without turning the dump into a constant file.
				for (u32 slot = 0; slot < 16; ++slot)
				{
					f32 k[4]{};

					if (!remix_rsx::read_slot(slot, k))
					{
						break;
					}

					if (k[0] == 0.f && k[1] == 0.f && k[2] == 0.f && k[3] == 0.f)
					{
						continue;
					}

					fmt::append(consts, " c%u=[%.4g %.4g %.4g %.4g]", slot,
						static_cast<f64>(k[0]), static_cast<f64>(k[1]),
						static_cast<f64>(k[2]), static_cast<f64>(k[3]));
				}

				rsx_log.notice("%s", consts);
			}

			// Every quad, in vertex order: two triangles per quad, so vertices 4n..4n+3.
			std::string line = fmt::format("Remix ui-quads[%u]: vp=%016llx tex=%ux%u verts=%u",
				m_ui_dumped, m_current_vp_hash, entry->width, entry->height, vertex_count);

			for (u32 i = 0; i < vertex_count; ++i)
			{
				f32 uvq[4]{};

				if (!remix_rsx::decode_position(uvs.at(i), uvs.type, uvs.size, uvq))
				{
					break;
				}

				fmt::append(line, " [%u](%.1f,%.1f,%.4f,%.4f)", i,
					m_scratch_ui_x[i], m_scratch_ui_y[i], uvq[0] * uv_scale[0], uvq[1] * uv_scale[1]);
			}

			rsx_log.notice("%s", line);
		}

		if (t == 0 && entry && have_uv && ui_dump_vp() == 0 && m_ui_dumped < ui_dump_limit())
		{
			++m_ui_dumped;

			f32 u_lo = +3.4e38f, u_hi = -3.4e38f, v_lo = +3.4e38f, v_hi = -3.4e38f;

			for (u32 i = 0; i < vertex_count; ++i)
			{
				f32 uv[4]{};

				if (!remix_rsx::decode_position(uvs.at(i), uvs.type, uvs.size, uv))
				{
					break;
				}

				u_lo = std::min(u_lo, uv[0] * uv_scale[0]);
				u_hi = std::max(u_hi, uv[0] * uv_scale[0]);
				v_lo = std::min(v_lo, uv[1] * uv_scale[1]);
				v_hi = std::max(v_hi, uv[1] * uv_scale[1]);
			}

			f32 x_lo = +3.4e38f, x_hi = -3.4e38f, y_lo = +3.4e38f, y_hi = -3.4e38f;

			for (u32 i = 0; i < vertex_count; ++i)
			{
				x_lo = std::min(x_lo, m_scratch_ui_x[i]);
				x_hi = std::max(x_hi, m_scratch_ui_x[i]);
				y_lo = std::min(y_lo, m_scratch_ui_y[i]);
				y_hi = std::max(y_hi, m_scratch_ui_y[i]);
			}

			rsx_log.notice(
				"Remix ui-dump[%u]: vp=%016llx verts=%u tris=%llu tex=%ux%u uvtype=%u/%u uvscale=%.5f,%.5f "
				"box=[%.1f,%.1f]..[%.1f,%.1f] uvbox=[%.4f,%.4f]..[%.4f,%.4f] "
				"t0=(%.1f,%.1f,%.4f,%.4f)(%.1f,%.1f,%.4f,%.4f)(%.1f,%.1f,%.4f,%.4f) tint=%08X",
				m_ui_dumped,
				m_current_vp_hash,
				vertex_count,
				static_cast<u64>(m_scratch_indices.size() / 3),
				entry->width, entry->height,
				static_cast<u32>(uvs.type), u32{uvs.size},
				uv_scale[0], uv_scale[1],
				x_lo, y_lo, x_hi, y_hi,
				u_lo, v_lo, u_hi, v_hi,
				x[0], y[0], u[0], v[0],
				x[1], y[1], u[1], v[1],
				x[2], y[2], u[2], v[2],
				tint);
		}

		m_compositor.draw_triangle(x, y, u, v, have_uv ? entry : nullptr, tint, clamp_uv);
	}

	++m_stats.ui_draws;
}

const remix_rsx::texture_entry* RemixGSRender::overlay_image(const void* key, const u8* rgba, u32 width, u32 height)
{
	if (!key || !rgba || width == 0 || height == 0)
	{
		return nullptr;
	}

	auto it = m_overlay_images.find(key);

	if (it != m_overlay_images.end() && it->second.width == width && it->second.height == height)
	{
		return &it->second;
	}

	remix_rsx::texture_entry entry{};
	entry.width = width;
	entry.height = height;
	entry.wrap_u = 0;
	entry.wrap_v = 0;
	entry.pixels.resize(static_cast<usz>(width) * height * 4);

	// stb hands these over as RGBA8; the compositor is BGRA8. GL papers over the difference
	// with a sampler swizzle (GLOverlays.cpp:229), which a CPU rasterizer cannot do.
	for (usz i = 0; i < (static_cast<usz>(width) * height); ++i)
	{
		entry.pixels[(i * 4) + 0] = rgba[(i * 4) + 2];
		entry.pixels[(i * 4) + 1] = rgba[(i * 4) + 1];
		entry.pixels[(i * 4) + 2] = rgba[(i * 4) + 0];
		entry.pixels[(i * 4) + 3] = rgba[(i * 4) + 3];
	}

	it = m_overlay_images.insert_or_assign(key, std::move(entry)).first;
	return &it->second;
}

void RemixGSRender::composite_overlay_command(const rsx::overlays::compiled_resource::command& cmd, f32 scale_x, f32 scale_y)
{
	const auto& config = cmd.config;
	const auto& verts = cmd.verts;

	if (verts.empty())
	{
		return;
	}

	const auto channel = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
	const u32 tint = channel(config.color.b)
		| (channel(config.color.g) << 8)
		| (channel(config.color.r) << 16)
		| (channel(config.color.a) << 24);

	if (config.clip_region)
	{
		m_compositor.set_clip(config.clip_rect.x1 * scale_x, config.clip_rect.y1 * scale_y,
			config.clip_rect.x2 * scale_x, config.clip_rect.y2 * scale_y);
	}
	else
	{
		m_compositor.clear_clip();
	}

	const rsx::overlays::font* font_ref = (config.texture_ref == rsx::overlays::image_resource_id::font_file) ? config.font_ref : nullptr;
	const remix_rsx::texture_entry* image = nullptr;

	if (config.texture_ref == rsx::overlays::image_resource_id::raw_image)
	{
		if (const auto* info = static_cast<const rsx::overlays::image_info_base*>(config.external_data_ref))
		{
			image = overlay_image(config.external_data_ref, info->get_data(),
				static_cast<u32>(info->w), static_cast<u32>(info->h));
		}
	}
	else if (config.texture_ref != rsx::overlays::image_resource_id::none && !font_ref)
	{
		// game_icon (254) and backbuffer (255) are TODO in GL and VK too; everything from 1 to
		// the end of the standard set comes from resource_config.
		const u32 index = u32{config.texture_ref};

		if (index >= 1 && index < 252 && m_ui_resources.texture_raw_data.size() >= index)
		{
			const auto& res = m_ui_resources.texture_raw_data[index - 1];

			if (res)
			{
				image = overlay_image(res.get(), res->get_data(), static_cast<u32>(res->w), static_cast<u32>(res->h));
			}
		}
	}

	u32 glyph_w = 0;
	u32 glyph_h = 0;
	u32 glyph_pages = 1;
	const u8* glyph_data = nullptr;

	if (font_ref)
	{
		const auto dims = font_ref->get_glyph_data_dimensions();
		glyph_w = dims.width;
		glyph_h = dims.height;
		glyph_pages = std::max(1u, dims.depth);
		glyph_data = font_ref->get_glyph_data().data();
	}

	// Emits one triangle from three overlay vertices, in compositor pixels.
	const auto emit = [&](usz a, usz b, usz c)
	{
		const f32 x[3] = { verts[a].values[0] * scale_x, verts[b].values[0] * scale_x, verts[c].values[0] * scale_x };
		const f32 y[3] = { verts[a].values[1] * scale_y, verts[b].values[1] * scale_y, verts[c].values[1] * scale_y };
		f32 u[3] = { verts[a].values[2], verts[b].values[2], verts[c].values[2] };
		f32 v[3] = { verts[a].values[3], verts[b].values[3], verts[c].values[3] };

		if (glyph_data)
		{
			// OverlayRenderFS.glsl:212 - the integer part of V picks the atlas page.
			const u32 page = std::min(glyph_pages - 1, static_cast<u32>(std::max(0.f, std::trunc(v[0]))));
			const u8* coverage = glyph_data + (static_cast<usz>(page) * glyph_w * glyph_h);

			for (u32 i = 0; i < 3; ++i)
			{
				v[i] -= std::trunc(v[i]);
			}

			// draw_glyph_quad is axis aligned, so text is emitted as the quad it always is.
			const f32 x0 = std::min({ x[0], x[1], x[2] });
			const f32 x1 = std::max({ x[0], x[1], x[2] });
			const f32 y0 = std::min({ y[0], y[1], y[2] });
			const f32 y1 = std::max({ y[0], y[1], y[2] });
			const f32 u0 = std::min({ u[0], u[1], u[2] });
			const f32 u1 = std::max({ u[0], u[1], u[2] });
			const f32 v0 = std::min({ v[0], v[1], v[2] });
			const f32 v1 = std::max({ v[0], v[1], v[2] });

			m_compositor.draw_glyph_quad(x0, y0, x1, y1, u0, v0, u1, v1, coverage, glyph_w, glyph_h, tint);
			return;
		}

		m_compositor.draw_triangle(x, y, u, v, image, tint, true);
	};

	switch (config.primitives)
	{
	case rsx::overlays::primitive_type::quad_list:
	{
		// Disjoint 4-vertex triangle strips (GLOverlays.cpp:371-391).
		for (usz i = 0; (i + 3) < verts.size(); i += 4)
		{
			emit(i + 0, i + 1, i + 2);
			emit(i + 1, i + 3, i + 2);
		}
		break;
	}
	case rsx::overlays::primitive_type::triangle_strip:
	{
		for (usz i = 0; (i + 2) < verts.size(); ++i)
		{
			if (i & 1)
			{
				emit(i + 1, i + 0, i + 2);
			}
			else
			{
				emit(i + 0, i + 1, i + 2);
			}
		}
		break;
	}
	case rsx::overlays::primitive_type::triangle_fan:
	{
		for (usz i = 1; (i + 1) < verts.size(); ++i)
		{
			emit(0, i, i + 1);
		}
		break;
	}
	case rsx::overlays::primitive_type::line_list:
	case rsx::overlays::primitive_type::line_strip:
	{
		// The perf graph is line geometry; a 1px quad per segment is the cheapest honest
		// stand-in for a hardware line.
		const usz step = (config.primitives == rsx::overlays::primitive_type::line_list) ? 2 : 1;

		for (usz i = 0; (i + 1) < verts.size(); i += step)
		{
			const f32 x0 = verts[i].values[0] * scale_x;
			const f32 y0 = verts[i].values[1] * scale_y;
			const f32 x1 = verts[i + 1].values[0] * scale_x;
			const f32 y1 = verts[i + 1].values[1] * scale_y;

			m_compositor.draw_quad(std::min(x0, x1), std::min(y0, y1),
				std::max(x0, x1) + 1.f, std::max(y0, y1) + 1.f, 0.f, 0.f, 1.f, 1.f, nullptr, tint, true);
		}
		break;
	}
	default:
		break;
	}

	m_compositor.clear_clip();
}

void RemixGSRender::composite_native_overlay()
{
	if (!m_overlay_manager)
	{
		return;
	}

	// Reclaim first, exactly as GLPresent.cpp:283-298 does: a disposed view's cached image
	// data must not outlive it.
	if (m_overlay_manager->has_dirty())
	{
		m_overlay_manager->lock_shared();

		std::vector<u32> uids_to_dispose;
		uids_to_dispose.reserve(m_overlay_manager->get_dirty().size());

		for (const auto& view : m_overlay_manager->get_dirty())
		{
			uids_to_dispose.push_back(view->uid);
		}

		m_overlay_manager->unlock_shared();
		m_overlay_manager->dispose(uids_to_dispose);
		m_overlay_images.clear();
	}

	if (!m_overlay_manager->has_visible())
	{
		return;
	}

	u32 width = 0;
	u32 height = 0;

	if (!compositor_target(width, height))
	{
		return;
	}

	if (!m_compositor_open)
	{
		m_compositor.begin_frame(width, height);
		m_compositor_open = true;
	}

	if (!m_ui_resources_loaded)
	{
		m_ui_resources.load_files();
		m_ui_resources_loaded = true;
	}

	std::lock_guard lock(*m_overlay_manager);

	for (const auto& view : m_overlay_manager->get_views())
	{
		if (!view || !view->visible)
		{
			continue;
		}

		// Views are authored against a virtual 1280x720 unless they asked for window space.
		const f32 vw = view->use_window_space ? static_cast<f32>(width) : static_cast<f32>(view->get_virtual_width());
		const f32 vh = view->use_window_space ? static_cast<f32>(height) : static_cast<f32>(view->get_virtual_height());

		if (vw <= 0.f || vh <= 0.f)
		{
			continue;
		}

		const f32 scale_x = static_cast<f32>(width) / vw;
		const f32 scale_y = static_cast<f32>(height) / vh;

		for (const auto& cmd : view->get_compiled().draw_commands)
		{
			composite_overlay_command(cmd, scale_x, scale_y);
		}

		view->update(get_system_time());
	}
}

void RemixGSRender::submit_compositor()
{
	if (!m_remix.fork_features() || remix_rsx::compositor_disabled())
	{
		m_compositor_open = false;
		return;
	}

	if (!m_compositor.dirty())
	{
		m_compositor_open = false;
		return;
	}

	const u32 status = m_compositor.submit(m_remix.api());

	if (status != REMIXAPI_ERROR_CODE_SUCCESS)
	{
		++m_stats.ui_skipped;

		// Loud once: a failing DrawScreenOverlay means the whole 2D path is dead, and a
		// per-frame log would drown the file.
		if (m_stats.ui_skipped == 1)
		{
			rsx_log.error("Remix: DrawScreenOverlay failed (%s), UI compositing is not reaching the runtime",
				remix_rsx::error_name(status));
		}
	}

	m_compositor_open = false;
}

bool RemixGSRender::resolve_indexed_world(u32 first_vertex, u32 vertex_count, bool& out_rigid)
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	out_rigid = false;
	m_indexed_world_valid = false;

	attribute_view indices{};

	if (map_attribute(fp.bone_attribute, first_vertex, vertex_count, indices) != attribute_status::ok)
	{
		return false;
	}

	// Whether the draw reads one palette entry or many is a property of the *data*, not of the
	// ucode, so it is answered here and never assumed. Resistance 2's dumps only print the min and
	// max of the index attribute (w=[-1..4], w=[-19..19], w=[-16511..-16384]) and those ranges
	// cannot say how many distinct values a draw actually uses - which is precisely why the
	// question is settled per draw against the real vertices.
	u32 first_slot = umax;
	bool uniform = true;

	for (u32 i = 0; i < vertex_count; ++i)
	{
		f32 scaled[4] = {};
		f32 raw[4] = {};

		if (!remix_rsx::decode_position(indices.at(i), indices.type, indices.size, scaled) ||
			!remix_rsx::decode_attribute_raw(indices.at(i), indices.type, indices.size, raw))
		{
			return false;
		}

		// Same choice build_skinning makes, and for the same reason: the ucode's constants are
		// authored against what the vertex fetch hands the shader.
		const f32 index_value = remix_rsx::skinraw_enabled()
			? raw[fp.bone_component]
			: scaled[fp.bone_component];

		u32 slot = 0;

		if (!remix_rsx::evaluate_palette_slot(fp, index_value, slot))
		{
			return false;
		}

		if (first_slot == umax)
		{
			first_slot = slot;
		}
		else if (slot != first_slot)
		{
			uniform = false;
			break;
		}
	}

	if (first_slot == umax || !uniform)
	{
		// Not one matrix. The caller hands it to the skinning path, which proves its own case.
		return false;
	}

	remix_rsx::mat4 palette{};

	if (!remix_rsx::build_palette_matrix(fp, first_slot, palette))
	{
		return false;
	}

	// The last proofs, and the ones that catch a mis-read palette whatever the index said: an
	// object-to-world transform is affine, and its basis spans three dimensions. The second is not
	// implied by the first - Resistance 2's c30 is affine and rank 1 - and a rank-deficient basis
	// draws the whole mesh as a line.
	if (!remix_rsx::is_affine(palette, s_world_affine_tolerance))
	{
		++m_bone_fail_counts[3];
		return false;
	}

	if (!remix_rsx::has_usable_basis(palette, s_bone_basis_tolerance))
	{
		++m_bone_fail_counts[4];
		++m_stats.bone_degenerate;
		return false;
	}

	m_indexed_world = palette;
	m_indexed_world_valid = true;
	out_rigid = true;
	return true;
}

// Weighted skinning: one palette entry per bone per vertex, blended by per-vertex weights.
//
// Submitted to Remix as blend weights and indices rather than evaluated here. The API carries the
// channels (remixapi_MeshInfoSkinning: bonesPerVertex, blendWeights_values, blendIndices_values)
// and the fork consumes them in a compute pass that does 'sum_k bone[idx_k] * pos * w_k'
// (dxvk-remix src/dxvk/shaders/rtx/pass/skinning.h), so the mesh this backend uploads stays the
// rigging - which is static per model - and only the bone matrices move per animation frame. The
// alternative, folding the blend into the vertex positions on the CPU, would make the mesh content
// change every frame, and this backend's mesh cache is keyed on content.
//
// Two properties of the consumer decide the layout and are not negotiable:
//   - numBonesPerVertex is asserted <= 4 (rtx_types.cpp:333), which is exactly the four the
//     observed ucode blends.
//   - the last weight of each tuple is *derived*, not read: the shader computes
//     lastWeight = 1 - sum(the first bonesPerVertex - 1). So the weights this writes have to sum
//     to 1, or the fourth bone silently gets a different weight than the ucode gave it. That is
//     why a set that does not sum is normalised (and counted) rather than passed through.
// The bones of one draw are one skeleton, so they are each other's control.
//
// Nothing else on this path bounds the *magnitude* of a bone, and for a 3-row palette nothing else
// can. build_palette_matrix synthesises the perspective column, so is_affine inspects a column this
// code wrote itself and passes unconditionally; has_usable_basis is a scale-invariant rank test
// (|det| / product of axis lengths) that a huge but full-rank basis passes cleanly. So an inflating
// bone reaches Remix with every refusal counter reading 0 - which is exactly what Resistance 2's
// stalker turret did while its legs, weighted to other bones of the same palette, drew correctly.
//
// The leading cause is an index landing past the end of the palette the title actually uploaded.
// evaluate_palette_slot only bounds the slot by the 468 legal constants; the real palette is
// however many bones the skeleton has (skin_bones_max peaked at 122), so an index past it reads
// whatever uniforms sit beyond - fog, colours, ranges - which form a full-rank 3x4 window of
// plausible-looking numbers at an arbitrary scale. That window cannot be recognised on its own; it
// can only be recognised next to the bones around it.
//
// Median rather than mean, because the thing being detected is precisely the outlier that would
// drag a mean. Ratios rather than absolutes, so the test holds whatever units the title models in.
// 8x and 64x are deliberately loose: a skeleton's bones differ by a few percent, not by an order of
// magnitude, so anything this catches is a bad palette read rather than a rig that squashes a limb.
// Fewer than three bones gives no median worth the name and the draw is left alone.
//
// Shared by both skinned paths - the blend rig and the single-bone rig have the same palette and
// the same hole - so the two can never disagree about what a plausible bone is.
// RPCS3_REMIX_BONESCALE=0 disables it and puts the explosion back.
bool RemixGSRender::bones_consistent()
{
	if (!remix_rsx::bone_scale_gate_enabled() || m_scratch_bone_axis.size() < 3)
	{
		return true;
	}

	m_scratch_bone_sorted = m_scratch_bone_axis;
	std::sort(m_scratch_bone_sorted.begin(), m_scratch_bone_sorted.end());
	const f32 axis_ref = m_scratch_bone_sorted[m_scratch_bone_sorted.size() / 2];

	if (!(axis_ref > 0.f) || !std::isfinite(axis_ref))
	{
		return false;
	}

	for (const f32 axis : m_scratch_bone_axis)
	{
		if (!(axis > 0.f) || !std::isfinite(axis)
			|| axis > (s_bone_scale_ratio * axis_ref)
			|| (axis * s_bone_scale_ratio) < axis_ref)
		{
			return false;
		}
	}

	m_scratch_bone_sorted = m_scratch_bone_offset;
	std::sort(m_scratch_bone_sorted.begin(), m_scratch_bone_sorted.end());
	const f32 offset_ref = m_scratch_bone_sorted[m_scratch_bone_sorted.size() / 2];

	// One-sided, and only when the skeleton has a translation spread to compare against: a bone
	// sitting at the rig's own origin is ordinary, a bone 64x further out than half its siblings is
	// not. A median of zero means there is nothing to measure against, so the test is skipped
	// rather than guessed at.
	if (!(offset_ref > 0.f) || !std::isfinite(offset_ref))
	{
		return true;
	}

	for (const f32 offset : m_scratch_bone_offset)
	{
		if (!std::isfinite(offset) || offset > (s_bone_offset_ratio * offset_ref))
		{
			return false;
		}
	}

	return true;
}

// Do the vertices this draw submits form one object?
//
// audit_skin_extent measured the transform and cleared it: across 26747 blended and 28558 skinned
// draws, with the shards on screen, not one vertex blended outside its own bone's neighbourhood.
// An affine instance transform maps every vertex of a mesh identically - it can move, scale or
// rotate a mesh but cannot tear one apart - so a torn mesh whose skinning measures clean was torn
// before any transform touched it. That leaves the positions themselves, which is what this reads.
//
// The measure is the same shape as skin-reach, one level earlier and with no transform involved:
// each vertex's distance from the draw's own median position, reported as max/median. Scale-free
// again, so it needs to know nothing about the title's units or how big the object is. A coherent
// object has a narrow spread - the furthest vertex is a small multiple of the typical one. A mesh
// with a subset collapsed somewhere else has two populations, and the ratio separates them
// immediately.
//
// That is exactly the artifact's shape: long thin shards radiating from one point. Every triangle
// spanning a correctly-decoded vertex and a collapsed one stretches into a spike between the two,
// which is what a partial decode failure draws.
//
// Median rather than centroid, and nth_element rather than a sort, so one bad population cannot
// drag the reference it is being measured against and the pass stays linear. Runs on every draw -
// Resistance 2 submits on the order of 130 per frame - because if the flagged draws turn out not to
// be skinned, that closes the whole line of investigation.
//
// Exact zeros are counted and reported separately on purpose: "N of M vertices decoded to exactly
// (0,0,0)" is a far more specific finding than "N are outliers", and it points straight at a read
// that returned nothing rather than at arithmetic that went wrong.
//
// RPCS3_REMIX_VTXSPREAD=<ratio> sets the threshold (0 disables). Diagnostic only - nothing is
// refused on it, which is what made the skin-reach result trustworthy.
void RemixGSRender::audit_vertex_extent(u32 first_vertex, u32 vertex_count, const attribute_view& positions, bool w_divide)
{
	const f32 ratio_limit = remix_rsx::vertex_spread_ratio();

	if (!(ratio_limit > 0.f) || vertex_count < 4 || m_scratch_vertices.size() < vertex_count)
	{
		return;
	}

	u32 zeros = 0;
	u32 nonfinite = 0;

	for (u32 i = 0; i < vertex_count; ++i)
	{
		const f32* p = m_scratch_vertices[i].position;

		if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
		{
			++nonfinite;
		}
		else if (p[0] == 0.f && p[1] == 0.f && p[2] == 0.f)
		{
			++zeros;
		}
	}

	// Per-axis median, so the reference point is one a majority of the vertices agree on.
	f32 centre[3] = {};

	for (u32 axis = 0; axis < 3; ++axis)
	{
		m_scratch_vertex_spread.clear();
		m_scratch_vertex_spread.reserve(vertex_count);

		for (u32 i = 0; i < vertex_count; ++i)
		{
			const f32 v = m_scratch_vertices[i].position[axis];

			if (std::isfinite(v))
			{
				m_scratch_vertex_spread.push_back(v);
			}
		}

		if (m_scratch_vertex_spread.empty())
		{
			return;
		}

		const usz mid = m_scratch_vertex_spread.size() / 2;
		std::nth_element(m_scratch_vertex_spread.begin(), m_scratch_vertex_spread.begin() + mid, m_scratch_vertex_spread.end());
		centre[axis] = m_scratch_vertex_spread[mid];
	}

	m_scratch_vertex_spread.clear();
	m_scratch_vertex_spread.reserve(vertex_count);

	f32 worst = -1.f;
	u32 worst_vertex = 0;

	for (u32 i = 0; i < vertex_count; ++i)
	{
		const f32* p = m_scratch_vertices[i].position;
		const f32 dx = p[0] - centre[0];
		const f32 dy = p[1] - centre[1];
		const f32 dz = p[2] - centre[2];
		const f32 d = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

		if (!std::isfinite(d))
		{
			continue;
		}

		m_scratch_vertex_spread.push_back(d);

		if (d > worst)
		{
			worst = d;
			worst_vertex = i;
		}
	}

	if (m_scratch_vertex_spread.size() < 4)
	{
		return;
	}

	const usz mid = m_scratch_vertex_spread.size() / 2;
	std::nth_element(m_scratch_vertex_spread.begin(), m_scratch_vertex_spread.begin() + mid, m_scratch_vertex_spread.end());
	const f32 median = m_scratch_vertex_spread[mid];

	const f32 ratio = (median > 1e-9f) ? (worst / median) : 0.f;

	// A subset at exactly the origin while the rest of the mesh is elsewhere is the specific shape
	// this is hunting, so it is counted whether or not the spread test also trips.
	const bool split_zero = (zeros > 0) && (zeros < vertex_count);

	if (split_zero)
	{
		++m_stats.vtx_zero_split;
	}

	const bool flagged = (nonfinite > 0) || (ratio > ratio_limit);

	if (flagged)
	{
		++m_stats.vtx_spread_flagged;

		// This pass runs immediately after the decode, long before the world transform is resolved,
		// so a flagged draw has not yet been through any of the gates that might drop it. That
		// matters here more than usual: the one program producing an extreme ratio reports
		// arch=unknown, and a program with no matrix chain into HPOS gets no world transform, which
		// world_refused already discards. Whether the incoherent geometry ever reaches the scene is
		// therefore an open question that this counter closes - and it has to be answered before a
		// refusal is worth writing, because refusing a draw that is already refused changes nothing
		// and would leave the real cause untouched.
		m_vertex_flagged = true;
	}

	if (!flagged && !split_zero)
	{
		return;
	}

	// One line per program: the flag describes a shape, and a shape repeats. The two counters carry
	// the per-draw populations.
	if (!m_vertex_spread_seen.insert(m_current_vp_hash).second)
	{
		return;
	}

	// How much of the mesh is in the far population, which vertices, and - the question that
	// separates a short data block from a stride error - how those indices are arranged. A tail
	// means the read ran off the end of real data; an even spacing means the walk is landing on the
	// wrong bytes at a fixed period; a scatter means neither and the layout is not the story.
	u32 outliers = 0;
	std::string offenders;

	m_scratch_outlier_indices.clear();

	for (u32 i = 0; i < vertex_count; ++i)
	{
		const f32* p = m_scratch_vertices[i].position;
		const f32 dx = p[0] - centre[0];
		const f32 dy = p[1] - centre[1];
		const f32 dz = p[2] - centre[2];
		const f32 d = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

		if (std::isfinite(d) && median > 1e-9f && d <= (ratio_limit * median))
		{
			continue;
		}

		++outliers;
		m_scratch_outlier_indices.push_back(i);

		if (outliers <= 4)
		{
			fmt::append(offenders, " v%u=[%.6g %.6g %.6g]", i,
				static_cast<f64>(p[0]), static_cast<f64>(p[1]), static_cast<f64>(p[2]));
		}
	}

	// Topology of the outlier set, computed rather than left to be eyeballed off a list.
	std::string topology;

	if (!m_scratch_outlier_indices.empty())
	{
		const u32 lo = m_scratch_outlier_indices.front();
		const u32 hi = m_scratch_outlier_indices.back();
		const bool contiguous = (hi - lo + 1) == ::size32(m_scratch_outlier_indices);
		const bool tail = contiguous && (hi + 1 == vertex_count);

		// The most common gap between consecutive offenders. A single dominant gap over most of the
		// set is a period; a spread of gaps is not.
		u32 best_gap = 0;
		u32 best_count = 0;

		for (u32 candidate = 1; candidate <= 8; ++candidate)
		{
			u32 hits = 0;

			for (usz k = 1; k < m_scratch_outlier_indices.size(); ++k)
			{
				if ((m_scratch_outlier_indices[k] - m_scratch_outlier_indices[k - 1]) == candidate)
				{
					++hits;
				}
			}

			if (hits > best_count)
			{
				best_count = hits;
				best_gap = candidate;
			}
		}

		fmt::append(topology, " span=[%u..%u] contiguous=%d tail=%d gap=%u/%llu", lo, hi,
			contiguous ? 1 : 0, tail ? 1 : 0, best_gap,
			static_cast<u64>(m_scratch_outlier_indices.size() > 1 ? m_scratch_outlier_indices.size() - 1 : 0));

		topology += " idx=";

		for (usz k = 0; k < m_scratch_outlier_indices.size() && k < 48; ++k)
		{
			fmt::append(topology, "%s%u", k ? "," : "", m_scratch_outlier_indices[k]);
		}

		if (m_scratch_outlier_indices.size() > 48)
		{
			topology += ",...";
		}
	}

	// A high quantile beside the median says whether the distribution is genuinely bimodal or
	// merely long-tailed: p90 close to the median with a huge max is two populations.
	const usz p90_index = (m_scratch_vertex_spread.size() * 9) / 10;
	std::nth_element(m_scratch_vertex_spread.begin(), m_scratch_vertex_spread.begin() + p90_index, m_scratch_vertex_spread.end());
	const f32 p90 = m_scratch_vertex_spread[p90_index];

	// The worst vertex's attribute exactly as it sits in guest memory, beside what the fetch made
	// of it. A stride or offset that walks off the real data shows up here as bytes that are not
	// the shape of the format; a decode fault shows up as sane bytes with an insane result.
	std::string raw_bytes;
	const u32 attr_bytes = remix_rsx::attribute_byte_size(positions.type, positions.size);

	if (attr_bytes != 0)
	{
		const u8* src = positions.at(worst_vertex);

		for (u32 b = 0; b < attr_bytes && b < 16; ++b)
		{
			fmt::append(raw_bytes, "%02x", src[b]);
		}
	}

	f32 decoded[4] = {};
	f32 raw_attr[4] = {};
	remix_rsx::decode_position(positions.at(worst_vertex), positions.type, positions.size, decoded);
	remix_rsx::decode_attribute_raw(positions.at(worst_vertex), positions.type, positions.size, raw_attr);

	// Which decode the ucode applies before its first matrix, and the matrix it rebuilds to. This
	// travels in the instance transform (or, for a blended rig, in the bone matrices), so it is not
	// applied to the numbers above - but a draw whose decode cannot be read back is a draw whose
	// submitted positions mean something different from what the transform expects.
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;
	std::string decode;

	if (fp.has_prescale)
	{
		fmt::append(decode, "prescale scale=c%u.%c bias=c%u",
			fp.prescale_scale_slot, "xyzw"[fp.prescale_scale_component & 3], fp.prescale_bias_slot);
	}
	else if (fp.has_const_affine)
	{
		fmt::append(decode, "const_affine scale=%d c%u.[%c%c%c] bias=%d c%u",
			fp.affine_has_scale ? 1 : 0, fp.affine_scale_slot,
			"xyzw"[fp.affine_scale_component[0] & 3],
			"xyzw"[fp.affine_scale_component[1] & 3],
			"xyzw"[fp.affine_scale_component[2] & 3],
			fp.affine_has_bias ? 1 : 0, fp.affine_bias_slot);
	}
	else
	{
		decode = "none";
	}

	if (remix_rsx::mat4 built{}; remix_rsx::build_prescale(fp, built))
	{
		fmt::append(decode, " -> %s", remix_rsx::format_matrix(built));
	}
	else if (fp.has_prescale || fp.has_const_affine)
	{
		decode += " -> UNREADABLE";
	}

	rsx_log.notice("Remix: vtx-spread frame=%llu vp=%016llx arch=%s vtx=%u ratio=%.4g worst=%.6g median=%.6g p90=%.6g "
		"| zeros=%u/%u nonfinite=%u outliers=%u%s | centre=[%.6g %.6g %.6g]%s "
		"| worst=v%u raw=%s decoded=[%.6g %.6g %.6g %.6g] stored=[%.6g %.6g %.6g %.6g] "
		"| attr0 type=%u size=%u stride=%u off=%u first_vertex=%u wdiv=%d | decode %s",
		m_frame_counter,
		m_current_vp_hash,
		remix_rsx::archetype_name(fp.archetype),
		vertex_count,
		static_cast<f64>(ratio),
		static_cast<f64>(worst),
		static_cast<f64>(median),
		static_cast<f64>(p90),
		zeros, vertex_count, nonfinite, outliers,
		topology,
		static_cast<f64>(centre[0]), static_cast<f64>(centre[1]), static_cast<f64>(centre[2]),
		offenders,
		worst_vertex,
		raw_bytes.empty() ? "?" : raw_bytes.c_str(),
		static_cast<f64>(decoded[0]), static_cast<f64>(decoded[1]), static_cast<f64>(decoded[2]), static_cast<f64>(decoded[3]),
		static_cast<f64>(raw_attr[0]), static_cast<f64>(raw_attr[1]), static_cast<f64>(raw_attr[2]), static_cast<f64>(raw_attr[3]),
		static_cast<u32>(positions.type), positions.size, positions.stride, positions.offset,
		first_vertex, w_divide ? 1 : 0,
		decode);
}

// How big is the geometry Remix actually receives?
//
// Nothing at 81af315 answered that, and the reason every audit there read clean is that each one is
// scale-free by construction and therefore cannot see a mesh that is simply too big:
//
//   audit_vertex_extent   max/median over the *decoded* positions. No matrix of any kind. 2036
//                         draws of 1303434 flagged, and vtx_spread_submitted = 0 for every one of
//                         them - i.e. the incoherent decodes were all dropped by a later gate and
//                         none of them reached the scene.
//   audit_skin_extent     max/median over 'bones x position', each vertex measured against its own
//                         dominant bone's origin. Applies the palette but not the instance
//                         transform, and reports a ratio, so a palette that is uniformly a thousand
//                         times too large reads exactly 1. skin_reach_flagged = 0.
//   sky-census 'wext'     the raw ATTR0 box transformed by the instance matrix. Absolute, but it
//                         omits the bones - and a blended rig folds its whole position decode into
//                         the bone matrices (build_blend_skinning, m_scratch_bone_prescale_folded),
//                         so for every skinned draw this over-reports by the decode factor. It is
//                         why 11 of the 12 largest 'wext' in the rK/rM/rN censuses are blend=4 rigs
//                         whose wext/rawext is ~1.0: not a measurement of those draws at all.
//
// This is 'instance x bones x position' per vertex, in world units, taken at the submission point.
// One pass, same shape and same cost as the pass audit_skin_extent already runs, and it is the
// number the three above each miss a different third of.
//
// What it found in the replay of the captured censuses, and what the gate is set from: R2's camera
// moves in tens of units and its ordinary draws measure under 1 to a few hundred, while a
// population of programs whose position decode the ucode matcher never recognised (dumps report
// prescale=0) reaches the world at raw quantised scale - c87769e09c995db9 at 15895 units across 4
// vertices, d736a5bdc6e2552a at 8621, a426abcdd77da419 at 5445. A flat 16000-unit sheet in a
// hundred-unit scene is a streak across the screen and a BLAS every primary ray intersects, which
// is the shape of both symptoms.
//
// Ratio against the frame's own median rather than an absolute limit, so this needs to know nothing
// about the title's units - the same reason bones_consistent measures a bone against its siblings
// rather than against a number. Sky is exempt: a dome legitimately spans the level, and gating it on
// the scene median would delete the sky.
//
// Returns false when the draw is refused. RPCS3_REMIX_STREAKGATE=0 restores 81af315.
bool RemixGSRender::audit_world_extent(const remixapi_Transform& transform, bool skinned, u32 vertex_count, bool exempt)
{
	m_streak_extent = 0.f;
	m_streak_flagged = false;
	m_streak_measured = false;

	if (vertex_count == 0 || m_scratch_vertices.size() < vertex_count)
	{
		return true;
	}

	// Exactly the blend dxvk-remix's skinning pass performs (src/dxvk/shaders/rtx/pass/skinning.h,
	// 'sum_k bone[idx_k] * pos * w_k'), then the instance transform Remix applies on top of it.
	// Reproduced rather than approximated, because the whole point of this pass is to measure the
	// value the runtime ends up with and not the one this backend thinks it sent.
	const u32 per_vertex = std::max<u32>(1u, m_scratch_bones_per_vertex);
	const bool blend = skinned && !m_scratch_bone_transforms.empty();

	f32 lo[3] = { +3.4e38f, +3.4e38f, +3.4e38f };
	f32 hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
	u32 nonfinite = 0;
	u32 counted = 0;

	// Over the vertices the index buffer actually references, not over the whole decoded buffer.
	// Remix builds its BLAS from the index list, so an unreferenced vertex contributes no geometry
	// and must not be able to refuse the draw. On this title the gap is not hypothetical: the first
	// armed run censused vp=d2468f79dc136cfd at vtx=695 idx=36 and refused it, i.e. a box drawn
	// around 695 decoded positions decided the fate of the 36 that render. Falls back to the whole
	// buffer only when the draw has no index list at all.
	const usz index_count = m_scratch_indices.size();
	const usz sample_count = index_count ? index_count : static_cast<usz>(vertex_count);

	for (usz n = 0; n < sample_count; ++n)
	{
		const u32 i = index_count ? static_cast<u32>(m_scratch_indices[n]) : static_cast<u32>(n);

		if (i >= vertex_count)
		{
			continue;
		}

		const f32* src = m_scratch_vertices[i].position;
		f32 p[3] = { src[0], src[1], src[2] };

		if (blend)
		{
			f32 blended[3] = {};

			for (u32 k = 0; k < per_vertex; ++k)
			{
				const usz t = (static_cast<usz>(i) * per_vertex) + k;

				if (t >= m_scratch_bone_weights.size() || t >= m_scratch_bone_indices.size())
				{
					break;
				}

				const f32 w = m_scratch_bone_weights[t];
				const u32 dense = m_scratch_bone_indices[t];

				if (!(w > 0.f) || dense >= m_scratch_bone_transforms.size())
				{
					continue;
				}

				const auto& m = m_scratch_bone_transforms[dense].matrix;

				for (u32 r = 0; r < 3; ++r)
				{
					blended[r] += w * ((m[r][0] * p[0]) + (m[r][1] * p[1]) + (m[r][2] * p[2]) + m[r][3]);
				}
			}

			p[0] = blended[0];
			p[1] = blended[1];
			p[2] = blended[2];
		}

		f32 world[3] = {};
		bool finite = true;

		for (u32 r = 0; r < 3; ++r)
		{
			world[r] = (transform.matrix[r][0] * p[0]) + (transform.matrix[r][1] * p[1])
				+ (transform.matrix[r][2] * p[2]) + transform.matrix[r][3];

			finite = finite && std::isfinite(world[r]);
		}

		if (!finite)
		{
			++nonfinite;
			continue;
		}

		for (u32 r = 0; r < 3; ++r)
		{
			lo[r] = std::min(lo[r], world[r]);
			hi[r] = std::max(hi[r], world[r]);
		}

		++counted;
	}

	++m_stats.wext_examined;

	if (nonfinite > 0)
	{
		++m_stats.wext_nonfinite;
	}

	if (counted == 0)
	{
		// Every vertex non-finite. There is no extent to measure and nothing this can be compared
		// against, so it is counted and left to the gates that already refuse on finiteness.
		return true;
	}

	f32 extent = 0.f;

	for (u32 r = 0; r < 3; ++r)
	{
		extent = std::max(extent, hi[r] - lo[r]);
	}

	m_streak_extent = extent;
	m_streak_measured = true;

	if (exempt)
	{
		++m_stats.wext_exempt;
		return true;
	}

	// The sample the *next* frame's median is taken from. Exempt draws are left out of it for the
	// same reason they are not gated: a dome the size of the level is not this scene's typical
	// object and letting it into the median would raise the bar for everything else.
	m_scratch_world_extents.push_back(extent);

	// The knob decides whether the draw is *refused*, never whether it is *measured*: with
	// RPCS3_REMIX_STREAKGATE=0 the same test still runs and still names the same programs, and the
	// draws land in wext_flagged_drawn instead of wext_refused. That is the whole A/B - one run with
	// the gate off and one with it on, against the same census - and it only exists because the
	// measurement is independent of the refusal.
	const f32 knob = remix_rsx::streak_extent_ratio();
	const bool gate_on = (knob > 0.f);
	const f32 ratio_limit = gate_on ? knob : s_streak_report_ratio;

	// No median means no scene to compare against, so nothing is judged. Both conditions are about
	// the reference rather than about this draw.
	if (!(m_world_extent_median > 0.f) || m_world_extent_samples < s_streak_median_min_samples)
	{
		return true;
	}

	const f32 ratio = extent / m_world_extent_median;

	if (!(ratio > ratio_limit))
	{
		return true;
	}

	m_streak_flagged = true;

	// One line per program, once, bounded - the shape the indexed-world census uses. The counters
	// say how many; this says which, with the vertex count and the two extents that separate "this
	// mesh is genuinely large" from "this mesh arrived undecoded".
	if (m_world_extent_seen.insert(m_current_vp_hash).second
		&& m_world_extent_census_lines < s_max_world_extent_census_lines)
	{
		++m_world_extent_census_lines;

		f32 model_lo[3] = { +3.4e38f, +3.4e38f, +3.4e38f };
		f32 model_hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };

		// Same referenced-only rule as the world pass, so world/model stays a ratio between two
		// boxes drawn around the same vertices and 'scale' remains readable as a decode factor.
		for (usz n = 0; n < sample_count; ++n)
		{
			const u32 i = index_count ? static_cast<u32>(m_scratch_indices[n]) : static_cast<u32>(n);

			if (i >= vertex_count)
			{
				continue;
			}

			for (u32 r = 0; r < 3; ++r)
			{
				model_lo[r] = std::min(model_lo[r], m_scratch_vertices[i].position[r]);
				model_hi[r] = std::max(model_hi[r], m_scratch_vertices[i].position[r]);
			}
		}

		f32 model_extent = 0.f;

		for (u32 r = 0; r < 3; ++r)
		{
			model_extent = std::max(model_extent, model_hi[r] - model_lo[r]);
		}

		// world/model is the whole diagnosis in one number. A recognised decode drives it to ~1e-4
		// on this title (quantised 16-bit positions); ~1 means the raw attribute values reached the
		// world untouched, which is the population the dumps report as prescale=0.
		const f32 decode_scale = (model_extent > 1e-9f) ? (extent / model_extent) : 0.f;

		// 'affine' is the other half of decodes_position (has_prescale || has_const_affine), and
		// without it this line cannot answer the only question it is asked. prescale=0 on its own
		// is ambiguous three ways: no decode was recognised and the raw quantised attribute was
		// submitted; the MUL/ADD form was recognised as const_affine and folded into the transform,
		// so a large 'model' is expected and correct; or it was recognised and applied wrongly.
		// The first reading is the vertex explosion, the second is healthy, and every refused draw
		// in the first R2 capture read prescale=0. s/b are affine_has_scale / affine_has_bias: a
		// bias-only affine cannot rescale a quantised range, so affine=1(s0,b1) with model ~1e4 is
		// still an undecoded draw despite the flag being set.
		rsx_log.notice("Remix: world-extent vp=%016llx arch=%s vtx=%u idx=%llu skinned=%d bones=%llu "
			"prescale=%d affine=%d(s%d,b%d) areason=%s "
			"wext=%.6g model=%.6g scale=%.4g median=%.6g ratio=%.4g nonfinite=%u refused=%d",
			m_current_vp_hash,
			remix_rsx::archetype_name(m_current_fingerprint->archetype),
			vertex_count,
			static_cast<u64>(m_scratch_indices.size()),
			blend ? 1 : 0,
			static_cast<u64>(m_scratch_bone_transforms.size()),
			m_current_fingerprint->has_prescale ? 1 : 0,
			m_current_fingerprint->has_const_affine ? 1 : 0,
			m_current_fingerprint->affine_has_scale ? 1 : 0,
			m_current_fingerprint->affine_has_bias ? 1 : 0,
			m_current_fingerprint->affine_reason,
			static_cast<f64>(extent),
			static_cast<f64>(model_extent),
			static_cast<f64>(decode_scale),
			static_cast<f64>(m_world_extent_median),
			static_cast<f64>(ratio),
			nonfinite,
			gate_on ? 1 : 0);
	}

	if (!gate_on)
	{
		// Flagged and left alone. The draw goes on to the mesh, the material and DrawInstance, any
		// of which can still drop it - so it is counted at the far side of DrawInstance and not
		// here, which is what makes wext_flagged_drawn a count of geometry that reached the scene.
		return true;
	}

	// Refuse-and-count. The draw never reaches CreateMesh, so wext_refused and wext_flagged_drawn
	// are disjoint by construction and neither can be read as a drawn count.
	++m_stats.wext_refused;
	return false;
}

// Measures the explosion instead of theorising about it.
//
// Every hypothesis about the shards so far has been a guess at a *cause* - an index past the end of
// the palette, a palette of identical matrices - and each was killed by data that showed the bones
// were healthy. This measures the *effect*: it reproduces exactly the blend Remix is about to
// perform and asks how far the result travels.
//
// The quantity is deliberately scale-free. For each vertex it computes the blended world position
// the way dxvk-remix's skinning pass computes it (sum_k w_k * bone_k * position, with the bone
// matrices as submitted, decode already composed in) and takes the distance from that position to
// the origin of the bone carrying most of its weight. On a sane skin every vertex sits within a
// limb's length of its own bone, so the spread of that distance across the mesh is narrow - the
// extremities are a small multiple of the median, never an order of magnitude. A vertex being
// thrown across the map is a huge multiple, whatever produced it.
//
// Reporting max/median rather than an absolute distance means the test needs to know nothing about
// the title's units, the quantisation scale, or how big a character is - which is what makes it an
// instrument rather than another assumption. It names the program, the vertex, the bone and the
// palette slot, so the next question is asked of a specific bone in a specific draw.
//
// Runs on every skinned draw: Resistance 2 submits on the order of twenty of them per frame, so the
// per-vertex pass costs nothing measurable. RPCS3_REMIX_SKINREACH=<ratio> sets the threshold, 0
// disables it entirely.
void RemixGSRender::audit_skin_extent(u32 vertex_count)
{
	const f32 ratio_limit = remix_rsx::skin_reach_ratio();

	if (!(ratio_limit > 0.f) || vertex_count == 0
		|| m_scratch_bone_transforms.empty()
		|| m_scratch_vertices.size() < vertex_count)
	{
		return;
	}

	const u32 per_vertex = std::max<u32>(1u, m_scratch_bones_per_vertex);

	m_scratch_bone_sorted.clear();
	m_scratch_bone_sorted.reserve(vertex_count);

	f32 worst = -1.f;
	u32 worst_vertex = 0;
	u32 worst_bone = 0;
	f32 worst_pos[3] = {};
	u32 nonfinite = 0;

	// The submitted geometry's own extent, and the source it was built from, both reported on the
	// flag line: they say whether the draw grew because the bones spread it or because the vertices
	// arrived wrong.
	f32 src_lo[3] = { +3.4e38f, +3.4e38f, +3.4e38f };
	f32 src_hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
	f32 out_lo[3] = { +3.4e38f, +3.4e38f, +3.4e38f };
	f32 out_hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };

	for (u32 i = 0; i < vertex_count; ++i)
	{
		const remixapi_HardcodedVertex& v = m_scratch_vertices[i];
		const f32 p[3] = { v.position[0], v.position[1], v.position[2] };

		for (u32 r = 0; r < 3; ++r)
		{
			src_lo[r] = std::min(src_lo[r], p[r]);
			src_hi[r] = std::max(src_hi[r], p[r]);
		}

		f32 blended[3] = {};
		f32 best_weight = -1.f;
		u32 dominant = umax;

		for (u32 k = 0; k < per_vertex; ++k)
		{
			const usz t = (static_cast<usz>(i) * per_vertex) + k;

			if (t >= m_scratch_bone_weights.size() || t >= m_scratch_bone_indices.size())
			{
				break;
			}

			const f32 w = m_scratch_bone_weights[t];
			const u32 dense = m_scratch_bone_indices[t];

			if (dense >= m_scratch_bone_transforms.size())
			{
				continue;
			}

			if (w > best_weight)
			{
				best_weight = w;
				dominant = dense;
			}

			if (!(w > 0.f))
			{
				continue;
			}

			// remixapi_Transform is column-vector: out_i = sum_j matrix[i][j] * in_j, in_3 = 1.
			const auto& m = m_scratch_bone_transforms[dense].matrix;

			for (u32 r = 0; r < 3; ++r)
			{
				blended[r] += w * ((m[r][0] * p[0]) + (m[r][1] * p[1]) + (m[r][2] * p[2]) + m[r][3]);
			}
		}

		if (dominant == umax)
		{
			continue;
		}

		for (u32 r = 0; r < 3; ++r)
		{
			out_lo[r] = std::min(out_lo[r], blended[r]);
			out_hi[r] = std::max(out_hi[r], blended[r]);
		}

		// Distance from the vertex's own bone's origin - the bone it is mostly weighted to. That is
		// the length the skin would have to stretch for this vertex to be where it ended up.
		const auto& d = m_scratch_bone_transforms[dominant].matrix;
		const f32 dx = blended[0] - d[0][3];
		const f32 dy = blended[1] - d[1][3];
		const f32 dz = blended[2] - d[2][3];
		const f32 reach = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

		if (!std::isfinite(reach))
		{
			++nonfinite;
			continue;
		}

		m_scratch_bone_sorted.push_back(reach);

		if (reach > worst)
		{
			worst = reach;
			worst_vertex = i;
			worst_bone = dominant;
			worst_pos[0] = blended[0];
			worst_pos[1] = blended[1];
			worst_pos[2] = blended[2];
		}
	}

	if (m_scratch_bone_sorted.empty())
	{
		return;
	}

	std::sort(m_scratch_bone_sorted.begin(), m_scratch_bone_sorted.end());
	const f32 median = m_scratch_bone_sorted[m_scratch_bone_sorted.size() / 2];

	// A median of zero means every vertex sits on its bone's origin, which is not a mesh this test
	// can say anything about.
	const f32 ratio = (median > 1e-6f) ? (worst / median) : 0.f;

	if (nonfinite == 0 && ratio <= ratio_limit)
	{
		return;
	}

	++m_stats.skin_reach_flagged;

	// One line per program: the flag is about a shape, and a shape repeats. The counter carries the
	// per-draw population.
	if (!m_skin_reach_seen.insert(m_current_vp_hash).second)
	{
		return;
	}

	const u32 slot = (worst_bone < m_scratch_bone_slots.size()) ? m_scratch_bone_slots[worst_bone] : umax;

	std::string weights;

	for (u32 k = 0; k < per_vertex; ++k)
	{
		const usz t = (static_cast<usz>(worst_vertex) * per_vertex) + k;

		if (t < m_scratch_bone_weights.size() && t < m_scratch_bone_indices.size())
		{
			const u32 dense = m_scratch_bone_indices[t];
			const u32 dense_slot = (dense < m_scratch_bone_slots.size()) ? m_scratch_bone_slots[dense] : umax;
			fmt::append(weights, " b%u[w=%.4g dense=%u c%u]", k,
				static_cast<f64>(m_scratch_bone_weights[t]), dense, dense_slot);
		}
	}

	// The offending bone's palette entry as it stands *at this instant*, straight out of the
	// constant file. The fingerprint is cached per program and cannot go stale; the constants are
	// read live per draw and can. If a rig that measures clean in one frame is flagged in the next
	// with garbage here while its fingerprint is unchanged, the fault is the palette not being
	// ready when this samples it - a timing problem, not a matcher problem. That is the one
	// hypothesis the cached-fingerprint diagnostics cannot reach, and it is the shape a flickering
	// defect on a static mesh with a static rig takes.
	std::string worst_raw;

	if (slot != umax)
	{
		if (remix_rsx::slot_block raw{}; remix_rsx::read_slot_block(slot, raw))
		{
			fmt::append(worst_raw, " raw c%u=[%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g]",
				slot,
				static_cast<f64>(raw.v[0][0]), static_cast<f64>(raw.v[0][1]), static_cast<f64>(raw.v[0][2]), static_cast<f64>(raw.v[0][3]),
				static_cast<f64>(raw.v[1][0]), static_cast<f64>(raw.v[1][1]), static_cast<f64>(raw.v[1][2]), static_cast<f64>(raw.v[1][3]),
				static_cast<f64>(raw.v[2][0]), static_cast<f64>(raw.v[2][1]), static_cast<f64>(raw.v[2][2]), static_cast<f64>(raw.v[2][3]));
		}
	}

	rsx_log.notice("Remix: skin-reach frame=%llu vp=%016llx blended=%d bones=%llu vtx=%u ratio=%.4g "
		"worst=%.6g median=%.6g nonfinite=%u | vertex=%u bone=%u c%u at=[%.6g %.6g %.6g]%s | "
		"src=[%.6g %.6g %.6g] out=[%.6g %.6g %.6g]%s",
		m_frame_counter,
		m_current_vp_hash,
		(m_scratch_bones_per_vertex > 1) ? 1 : 0,
		static_cast<u64>(m_scratch_bone_transforms.size()),
		vertex_count,
		static_cast<f64>(ratio),
		static_cast<f64>(worst),
		static_cast<f64>(median),
		nonfinite,
		worst_vertex,
		worst_bone,
		slot,
		static_cast<f64>(worst_pos[0]), static_cast<f64>(worst_pos[1]), static_cast<f64>(worst_pos[2]),
		weights,
		static_cast<f64>(src_hi[0] - src_lo[0]), static_cast<f64>(src_hi[1] - src_lo[1]), static_cast<f64>(src_hi[2] - src_lo[2]),
		static_cast<f64>(out_hi[0] - out_lo[0]), static_cast<f64>(out_hi[1] - out_lo[1]), static_cast<f64>(out_hi[2] - out_lo[2]),
		worst_raw);
}

bool RemixGSRender::build_blend_skinning(u32 first_vertex, u32 vertex_count)
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	const u32 bones_per_vertex = std::min<u32>(fp.blend_bones, remix_rsx::max_blend_bones);

	if (bones_per_vertex == 0)
	{
		return false;
	}

	attribute_view indices{};
	attribute_view weights{};

	if (map_attribute(fp.bone_attribute, first_vertex, vertex_count, indices) != attribute_status::ok
		|| map_attribute(fp.blend_weight_attribute, first_vertex, vertex_count, weights) != attribute_status::ok)
	{
		return false;
	}

	// The position decode belongs in *front* of the bones, and this is the only place it can go.
	//
	//   ucode:  clip = ((attr * decode) * M_blend + bias) * outer
	//   Remix:  world = objectToWorld * (sum_k w_k * bone_k * vertex)
	//
	// The vertices this backend submits are the raw attribute values, so per_draw_transform folds
	// the decode into the instance transform (prepend_object_space). For a rigid draw that is the
	// right place - one matrix, one order. For a skinned draw it is the wrong side of the bones:
	// Remix applies the palette first, so the instance transform lands as (attr * M) * decode
	// instead of (attr * decode) * M. With a bone that has a translation the two are not the same
	// transform - the decode scales the bone's translation as well as its rotation. Every one of
	// the sixteen R2 blend programs decodes with a uniform 'attr0.xyz * c18.w', so getting this
	// backwards shrinks every bone's translation by that factor and pulls the whole skeleton in
	// towards its own origin: a character drawn as a collapsed box.
	//
	// So the decode is composed into each bone matrix here, and m_scratch_bone_prescale_folded
	// tells per_draw_transform not to apply it a second time on the far side.
	remix_rsx::mat4 prescale = remix_rsx::mat4_identity();
	const bool have_prescale = remix_rsx::build_prescale(fp, prescale);

	if ((fp.has_prescale || fp.has_const_affine) && remix_rsx::position_affine_enabled() && !have_prescale)
	{
		// The decode was recognised but its constants cannot be read back. Refused, not drawn raw:
		// raw quantised positions against a matrix that expects decoded ones is the vertex
		// explosion, and the whole point of recognising the decode is knowing the raw values are
		// wrong. Same rule per_draw_transform applies for the unskinned case.
		++m_stats.pos_decode_refused;
		return false;
	}

	const usz tuples = static_cast<usz>(vertex_count) * bones_per_vertex;

	m_scratch_bone_indices.clear();
	m_scratch_bone_indices.resize(tuples);
	m_scratch_bone_weights.clear();
	m_scratch_bone_weights.resize(tuples);
	m_scratch_bone_raw.clear();
	m_scratch_bone_raw.resize(tuples);
	m_scratch_bone_slots.clear();
	m_scratch_bone_transforms.clear();
	m_scratch_bone_axis.clear();
	m_scratch_bone_offset.clear();

	// 8-bit and 16-bit normalised weights quantise, so an exact sum of 1 is not available: four
	// components of a ub256 attribute can be off by up to 4/255 = 0.0157 between them. 1/32 clears
	// that with room to spare while still catching the cases this is really guarding against - an
	// attribute that is not normalised at all (sums near 255), or a weight set that is not a blend.
	constexpr f32 s_weight_sum_tolerance = 1.f / 32.f;

	for (u32 i = 0; i < vertex_count; ++i)
	{
		f32 index_scaled[4] = {};
		f32 index_raw[4] = {};
		f32 weight_scaled[4] = {};

		if (!remix_rsx::decode_position(indices.at(i), indices.type, indices.size, index_scaled)
			|| !remix_rsx::decode_attribute_raw(indices.at(i), indices.type, indices.size, index_raw)
			|| !remix_rsx::decode_position(weights.at(i), weights.type, weights.size, weight_scaled))
		{
			++m_stats.skin_blend_refused_index;
			return false;
		}

		// The weights first: a bone whose weight is zero contributes nothing, and its index is
		// commonly padding the ucode never reads. Resolving those would refuse a whole draw over a
		// slot the hardware never touches.
		f32 w[remix_rsx::max_blend_bones] = {};
		f32 sum = 0.f;

		for (u32 k = 0; k < bones_per_vertex; ++k)
		{
			w[k] = weight_scaled[fp.blend_weight_component[k] & 3];

			if (!std::isfinite(w[k]) || w[k] < 0.f)
			{
				// Remix's blend skips any weight <= 0 outright, so a negative weight is not a
				// thing this can submit and pretend to have submitted.
				++m_stats.skin_blend_refused_weight;
				return false;
			}

			sum += w[k];
		}

		if (!(sum > 0.f) || !std::isfinite(sum))
		{
			++m_stats.skin_blend_refused_weight;
			return false;
		}

		if (std::abs(sum - 1.f) > s_weight_sum_tolerance)
		{
			++m_stats.skin_blend_rescaled;
		}

		const f32 inv_sum = 1.f / sum;

		for (u32 k = 0; k < bones_per_vertex; ++k)
		{
			w[k] *= inv_sum;
		}

		for (u32 k = 0; k < bones_per_vertex; ++k)
		{
			const usz out = (static_cast<usz>(i) * bones_per_vertex) + k;

			m_scratch_bone_weights[out] = w[k];

			if (w[k] <= 0.f)
			{
				// Weightless: any valid bone will do, and dense 0 always exists by the time the
				// mesh is submitted (the loop below refuses the draw if it never fills one).
				m_scratch_bone_indices[out] = 0;
				m_scratch_bone_raw[out] = 0.f;
				continue;
			}

			const remix_rsx::bone_index_chain& chain = fp.blend_bone[k];

			// Same choice the single-bone path makes and for the same reason: the ucode's constants
			// are authored against what the vertex fetch hands the shader, i.e. the scaled value.
			const f32 index_value = remix_rsx::skinraw_enabled()
				? index_raw[chain.component & 3]
				: index_scaled[chain.component & 3];

			u32 slot = 0;

			if (!remix_rsx::evaluate_palette_slot(fp, chain, index_value, slot))
			{
				++m_stats.skin_blend_refused_index;
				return false;
			}

			m_scratch_bone_raw[out] = index_raw[chain.component & 3];

			u32 dense = umax;

			for (u32 s = 0; s < ::size32(m_scratch_bone_slots); ++s)
			{
				if (m_scratch_bone_slots[s] == slot)
				{
					dense = s;
					break;
				}
			}

			if (dense == umax)
			{
				// The fork packs blendIndices into one byte each (rtx_remix_api.cpp:1088,
				// 'vertIndices |= blendIndicesStorage[j + k] << 8 * k'), so a dense index has to
				// stay inside 0..255 as well as inside the 256-entry transform array.
				if (m_scratch_bone_slots.size() >= REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT)
				{
					++m_stats.skin_blend_refused_palette;
					return false;
				}

				remix_rsx::mat4 bone{};

				if (!remix_rsx::build_palette_matrix(fp, slot, bone))
				{
					++m_stats.skin_blend_refused_bone;
					return false;
				}

				// 'attr * decode * bone', in the ucode's order. Checked below in the composed form
				// because that is the matrix Remix is handed.
				if (have_prescale)
				{
					bone = remix_rsx::mat4_multiply(prescale, bone);
				}

				if (bone_rejected(bone))
				{
					++m_stats.skin_blend_refused_bone;
					return false;
				}

				// ...and neither is one whose basis has collapsed. is_affine only inspects the
				// perspective column, so Resistance 2's c30 passes it with three basis rows all
				// parallel to Z - every vertex weighted to that bone lands on a line, which is the
				// character-as-a-collapsed-box artifact this whole gate exists to refuse.
				if (!remix_rsx::has_usable_basis(bone, s_bone_basis_tolerance))
				{
					++m_bone_fail_counts[2];
					++m_stats.bone_degenerate;
					++m_stats.skin_blend_refused_bone;
					return false;
				}

				dense = ::size32(m_scratch_bone_slots);
				m_scratch_bone_slots.push_back(slot);
				m_scratch_bone_transforms.push_back(remix_rsx::to_remix_transform(bone));
				m_scratch_bone_axis.push_back(remix_rsx::basis_extent(bone));
				m_scratch_bone_offset.push_back(remix_rsx::translation_extent(bone));
			}

			m_scratch_bone_indices[out] = dense;
		}
	}

	if (m_scratch_bone_transforms.empty())
	{
		// Every vertex weightless: nothing was proven about any bone, so nothing is drawn.
		++m_stats.skin_blend_refused_weight;
		return false;
	}

	if (!bones_consistent())
	{
		++m_stats.skin_blend_refused_scale;
		return false;
	}

	// --- every bone in this palette is the same matrix ------------------------------------
	//
	// Measured on Resistance 2: six of the eight blend rigs report eight bones at c32..c53 whose
	// assembled matrices are identical - unit basis, and a translation equal to the c18 bias to
	// every printed digit, which is what happens when the palette's own fourth components are zero.
	// The two that look like real skeletons (a3af6e3d5f0ac8e6, 6090af134e07aa65) report 24 bones
	// with a genuine spread. Eight consecutive palette entries holding one matrix is not a pose.
	//
	// Counted, not refused, and the distinction is not cosmetic: a palette whose entries are all
	// the same matrix blends - for weights that sum to 1, which skinblend_rescaled proves they do -
	// to exactly that one matrix. The draw is then a mathematically *correct* rigid draw, and
	// refusing it would delete geometry that is rendering properly. It also cannot be the source of
	// radiating shards: an identical palette moves every vertex of the mesh the same way, which is
	// the definition of rigid, whereas shards require neighbouring vertices to be pulled apart.
	//
	// RPCS3_REMIX_BONEUNIFORM=0 refuses them instead, which is the one-run A/B for whether this
	// population is implicated in an artifact at all.
	if (m_scratch_bone_transforms.size() >= 2)
	{
		bool uniform = true;

		for (usz b = 1; b < m_scratch_bone_transforms.size() && uniform; ++b)
		{
			for (u32 r = 0; r < 3 && uniform; ++r)
			{
				for (u32 c = 0; c < 4; ++c)
				{
					if (std::abs(m_scratch_bone_transforms[b].matrix[r][c] - m_scratch_bone_transforms[0].matrix[r][c]) > 1e-6f)
					{
						uniform = false;
						break;
					}
				}
			}
		}

		if (uniform)
		{
			++m_stats.skin_blend_uniform;

			if (!remix_rsx::bone_uniform_allowed())
			{
				return false;
			}
		}
	}

	// --- bisect knobs -------------------------------------------------------------------
	// Applied after the real decode, same as the single-bone path, so only the one link under
	// test changes.
	if (const u32 forced = remix_rsx::skinbone_index(); forced != umax)
	{
		const u32 clamped = std::min<u32>(forced, ::size32(m_scratch_bone_transforms) - 1);
		std::fill(m_scratch_bone_indices.begin(), m_scratch_bone_indices.end(), clamped);
	}

	if (remix_rsx::skinid_enabled())
	{
		std::fill(m_scratch_bone_transforms.begin(), m_scratch_bone_transforms.end(), s_identity_transform);
	}

	m_scratch_bones_per_vertex = bones_per_vertex;
	m_scratch_bone_prescale_folded = have_prescale;
	m_stats.skin_bones_max = std::max(m_stats.skin_bones_max, static_cast<u64>(m_scratch_bone_transforms.size()));
	++m_stats.skin_blend_submitted;
	audit_skin_extent(vertex_count);
	return true;
}

bool RemixGSRender::build_skinning(u32 first_vertex, u32 vertex_count)
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	if (fp.skin_blended)
	{
		return build_blend_skinning(first_vertex, vertex_count);
	}

	m_scratch_bones_per_vertex = 1;

	attribute_view bones{};

	if (map_attribute(fp.bone_attribute, first_vertex, vertex_count, bones) != attribute_status::ok)
	{
		return false;
	}

	m_scratch_bone_indices.clear();
	m_scratch_bone_indices.resize(vertex_count);
	m_scratch_bone_raw.clear();
	m_scratch_bone_raw.resize(vertex_count);
	m_scratch_bone_slots.clear();
	m_scratch_bone_transforms.clear();
	m_scratch_bone_axis.clear();
	m_scratch_bone_offset.clear();

	for (u32 i = 0; i < vertex_count; ++i)
	{
		// The ucode's constants are authored against what the vertex fetch hands the shader,
		// i.e. the *scaled* value, so the index chain has to start from the same number.
		// decode_attribute_raw is used separately below, for the hash, where the stored bytes
		// are exactly what identifies the rigging.
		f32 scaled[4] = {};
		f32 raw[4] = {};

		if (!remix_rsx::decode_position(bones.at(i), bones.type, bones.size, scaled) ||
			!remix_rsx::decode_attribute_raw(bones.at(i), bones.type, bones.size, raw))
		{
			return false;
		}

		u32 slot = 0;

		// RPCS3_REMIX_SKINRAW=1 feeds the stored value instead of the scaled one, which is the
		// M4 deviation D1 decision under test: a ub bone index that the ucode does not rescale
		// reads 0..255 raw but 0..1 scaled.
		const f32 index_value = remix_rsx::skinraw_enabled()
			? raw[fp.bone_component]
			: scaled[fp.bone_component];

		// The absolute slot rather than the offset, because a signed index attribute makes the
		// offset alone meaningless - see evaluate_palette_slot. For every rig that resolved before
		// this change the two are the same number plus palette_base, so the dense remap below is
		// keyed on exactly the same distinctions it was.
		if (!remix_rsx::evaluate_palette_slot(fp, index_value, slot))
		{
			return false;
		}

		m_scratch_bone_raw[i] = raw[fp.bone_component];

		// Remix indexes boneTransforms[] directly, so the palette slots have to be packed
		// down to 0..count-1. Counts are small (one skeleton), so a linear scan is cheapest.
		u32 dense = umax;

		for (u32 s = 0; s < ::size32(m_scratch_bone_slots); ++s)
		{
			if (m_scratch_bone_slots[s] == slot)
			{
				dense = s;
				break;
			}
		}

		if (dense == umax)
		{
			if (m_scratch_bone_slots.size() >= REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT)
			{
				return false;
			}

			// build_palette_matrix rather than a raw 4-slot read: the group may supply three rows
			// rather than four, may be strided, and may carry a constant translation the ucode
			// applies after it. Reading four consecutive slots and calling it a matrix is what made
			// this refuse every real palette it was ever shown.
			remix_rsx::mat4 bone{};

			if (!remix_rsx::build_palette_matrix(fp, slot, bone))
			{
				return false;
			}

			// A bone that is not a plain affine transform is not a bone; refusing it here is
			// what keeps a mis-read palette from smearing geometry across the world.
			if (bone_rejected(bone))
			{
				return false;
			}

			// ...and neither is one whose basis has collapsed. is_affine only inspects the
			// perspective column, so it passed Resistance 2's c30 - three basis rows all parallel
			// to Z, determinant zero - and every vertex weighted to that bone landed on a line.
			// That is the character-as-a-thin-box artifact, and it is the exploded case this gate
			// exists to refuse.
			if (!remix_rsx::has_usable_basis(bone, s_bone_basis_tolerance))
			{
				++m_stats.bone_degenerate;
				return false;
			}

			dense = ::size32(m_scratch_bone_slots);
			m_scratch_bone_slots.push_back(slot);
			m_scratch_bone_transforms.push_back(remix_rsx::to_remix_transform(bone));
			m_scratch_bone_axis.push_back(remix_rsx::basis_extent(bone));
			m_scratch_bone_offset.push_back(remix_rsx::translation_extent(bone));
		}

		m_scratch_bone_indices[i] = dense;
	}

	if (m_scratch_bone_transforms.empty())
	{
		return false;
	}

	// The same sibling test the blend path applies, and for the same reason: this palette is also
	// three rows, so is_affine above inspected a column build_palette_matrix wrote itself and
	// has_usable_basis cannot see an inflating bone either. Counted into the same statistic so one
	// number covers "a bone was implausible next to its siblings" whichever rig produced it.
	if (!bones_consistent())
	{
		++m_stats.skin_blend_refused_scale;
		return false;
	}

	// --- bisect knobs -------------------------------------------------------------------
	// Both are deliberately applied after the real decode, so the mesh, the weights and the
	// palette read are exactly what an unknobbed run produces and only the one link under
	// test changes.
	if (const u32 forced = remix_rsx::skinbone_index(); forced != umax)
	{
		const u32 clamped = std::min<u32>(forced, ::size32(m_scratch_bone_transforms) - 1);
		std::fill(m_scratch_bone_indices.begin(), m_scratch_bone_indices.end(), clamped);
	}

	if (remix_rsx::skinid_enabled())
	{
		std::fill(m_scratch_bone_transforms.begin(), m_scratch_bone_transforms.end(), s_identity_transform);
	}

	// One bone per vertex at weight 1: that is the shape the observed ucode presents (a single
	// MUL + 3 MAD against one address register, no in_weight attribute). Anything else is not
	// recognised and therefore never reaches here.
	m_scratch_bone_weights.assign(vertex_count, 1.f);

	m_stats.skin_bones_max = std::max(m_stats.skin_bones_max, static_cast<u64>(m_scratch_bone_transforms.size()));
	audit_skin_extent(vertex_count);
	return true;
}

void RemixGSRender::update_camera_candidate()
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	if (!fp.has_outer())
	{
		return;
	}

	remix_rsx::slot_block slots{};
	if (!remix_rsx::read_slot_block(fp.outer_base(), slots))
	{
		return;
	}

	const f32 clip_w = static_cast<f32>(rsx::method_registers.surface_clip_width());
	const f32 clip_h = static_cast<f32>(rsx::method_registers.surface_clip_height());
	const f32 reference_aspect = (clip_h > 0.f) ? (clip_w / clip_h) : (16.f / 9.f);

	const remix_rsx::mat4 raw = remix_rsx::slots_to_matrix(slots, fp.outer_shape());
	const remix_rsx::mat4 folded = remix_rsx::fold_viewport_z(raw,
		rsx::method_registers.viewport_scale_z(),
		rsx::method_registers.viewport_offset_z());

	// A draw in the viewmodel depth range is a candidate for the *viewmodel* reference and for
	// nothing else. Both halves matter. It has to feed its own reference because dividing it by the
	// world camera's is what collapses the basis to (0.491, 0.002, 1.150) - see
	// viewmodel_camera_mode() - and it has to stop feeding the world's because R2's viewmodel
	// programs are skinned_layered, so a frame one of them won would publish has_reference = false
	// and move every world draw in the next frame onto a different branch of per_draw_transform.
	//
	// Neither scored nor split. Both of those exist to pick the scene camera out of a frame's worth
	// of unrelated perspective draws - shadow cascades, cube faces - and the depth-range predicate
	// has already done that selection. score_perspective would in fact return 0 for every draw
	// here: it opens with classify_perspective, which rejects any matrix with |m[0][3]| > 0.02, and
	// a *fused* view-projection carries the view forward vector in that slot - -0.393 on the
	// viewmodel G0 dumped in bin\log\RPCS3.log. Scoring this population would latch nothing and
	// turn the whole fix into a silent suppression.
	//
	// What the reference does have to be is a usable divisor: finite, not affine (a model matrix is
	// not a projection), and invertible.
	if (remix_rsx::viewmodel_camera_mode() >= 2)
	{
		f32 depth_scale = 0.f;
		f32 depth_offset = 0.f;

		if (classify_viewmodel_depth(depth_scale, depth_offset) == viewmodel_outcome::tagged)
		{
			++m_stats.viewmodel_cam_diverted;

			if (!remix_rsx::mat4_is_finite(folded) || remix_rsx::is_affine(folded, 1e-4f))
			{
				return;
			}

			// 'folded' is the outer group alone, so it is only a valid divisor for draws whose
			// chain per_draw_transform builds is that same group. It builds a longer one - the full
			// G0..G(n-1) product - for a layered program with more than one group, and dividing a
			// groups=1 draw by the outermost group of such a program would leave the view standing
			// in the transform instead of cancelling it. Refused rather than reconstructed: the
			// chain loop lives in per_draw_transform and duplicating it here is how the two drift.
			// Dormant on R2, whose viewmodel programs are skinned_layered with group_count 1.
			if (fp.is_layered() && fp.group_count > 1 && remix_rsx::full_chain_enabled())
			{
				++m_stats.viewmodel_cam_unusable;
				return;
			}

			if (m_frame_viewmodel_candidate.valid)
			{
				// The viewmodel population is expected to share one view-projection, so the first
				// valid draw of the frame wins and the rest only have to agree. Measured rather
				// than assumed: the two programs dumped at [0,0.2] in that capture carry identical
				// G0s, but the census tags five, and the other three were only ever dumped drawing
				// world geometry at [0,1]. A non-zero conflict count means first-wins is picking
				// arbitrarily and the population needs splitting further.
				f32 delta = 0.f;
				f32 norm = 0.f;

				for (u32 i = 0; i < 4; ++i)
				{
					for (u32 j = 0; j < 4; ++j)
					{
						delta += std::abs(folded.m[i][j] - m_frame_viewmodel_candidate.projection.m[i][j]);
						norm += std::abs(m_frame_viewmodel_candidate.projection.m[i][j]);
					}
				}

				if (delta > 1e-3f * (norm + 1.f))
				{
					++m_stats.viewmodel_cam_conflict;
				}

				return;
			}

			remix_rsx::mat4 viewmodel_inverse{};
			if (!remix_rsx::mat4_invert(folded, viewmodel_inverse))
			{
				return;
			}

			m_frame_viewmodel_candidate.valid = true;
			m_frame_viewmodel_candidate.archetype = fp.archetype;
			m_frame_viewmodel_candidate.projection = folded;
			m_frame_viewmodel_candidate.has_reference = true;
			m_frame_viewmodel_candidate.reference_inverse = viewmodel_inverse;
			return;
		}
	}

	if (fp.is_layered())
	{
		// The title already split its own transform: the outermost group is the projection,
		// the one below it is the view (identity when there are only two groups).
		const f32 score = remix_rsx::score_perspective(folded, reference_aspect);

		if (score <= m_frame_candidate.score)
		{
			return;
		}

		remix_rsx::mat4 view = remix_rsx::mat4_identity();

		if (fp.group_count >= 3)
		{
			remix_rsx::slot_block view_slots{};
			if (!remix_rsx::read_slot_block(fp.group_base[fp.group_count - 2], view_slots))
			{
				return;
			}

			view = remix_rsx::slots_to_matrix(view_slots, fp.group_shape[fp.group_count - 2]);

			if (!remix_rsx::is_affine(view, 0.02f))
			{
				// Not a view: fall back to anchoring the world in view space.
				view = remix_rsx::mat4_identity();
			}
		}

		m_frame_candidate.valid = true;
		m_frame_candidate.score = score;
		m_frame_candidate.archetype = fp.archetype;
		m_frame_candidate.view = view;
		m_frame_candidate.projection = folded;
		m_frame_candidate.has_reference = false;
		m_frame_candidate.group_count = fp.group_count;
		m_frame_candidate.position[0] = 0.f;
		m_frame_candidate.position[1] = 0.f;
		m_frame_candidate.position[2] = 0.f;

		remix_rsx::mat4 view_to_world{};
		if (remix_rsx::mat4_invert(view, view_to_world))
		{
			m_frame_candidate.position[0] = view_to_world.m[3][0];
			m_frame_candidate.position[1] = view_to_world.m[3][1];
			m_frame_candidate.position[2] = view_to_world.m[3][2];
		}

		m_frame_candidate.has_view_proj_inverse = remix_rsx::mat4_invert(
			remix_rsx::mat4_multiply(view, folded), m_frame_candidate.view_proj_inverse);

		return;
	}

	// Fused group: split it into a view and a projection.
	if (remix_rsx::is_affine(folded, 1e-4f) || m_split_attempts >= s_max_split_attempts_per_frame)
	{
		return;
	}

	++m_split_attempts;

	// Counted so "this frame produced no candidate" separates into its three causes without a
	// dump run: split_attempted == 0 over a window means no qualifying 3D draw reached here at
	// all (or the 32-attempts-per-frame cap above swallowed them), while
	// split_failed ~ split_attempted means the draws were there and the 4x4 would not factor.
	++m_stats.split_attempted;

	remix_rsx::vp_split split{};
	if (!remix_rsx::split_view_projection(folded, split))
	{
		++m_stats.split_failed;
		return;
	}

	const f32 score = remix_rsx::score_perspective(split.projection, reference_aspect);

	if (score <= m_frame_candidate.score)
	{
		return;
	}

	remix_rsx::mat4 reference_inverse{};
	if (!remix_rsx::mat4_invert(folded, reference_inverse))
	{
		return;
	}

	m_frame_candidate.valid = true;
	m_frame_candidate.score = score;
	m_frame_candidate.archetype = fp.archetype;
	m_frame_candidate.view = split.view;
	m_frame_candidate.projection = split.projection;
	m_frame_candidate.has_reference = true;
	m_frame_candidate.reference_inverse = reference_inverse;

	// Row 3 of the inverse view is the camera position in world space.
	remix_rsx::mat4 view_to_world{};
	if (remix_rsx::mat4_invert(split.view, view_to_world))
	{
		m_frame_candidate.position[0] = view_to_world.m[3][0];
		m_frame_candidate.position[1] = view_to_world.m[3][1];
		m_frame_candidate.position[2] = view_to_world.m[3][2];
	}
}

// Splits the one condition that refused every bad bone into the three faults it actually
// covers. The world gate taught this: fourteen exits behind one counter cost three sessions of
// guessing, and the split answered it in one run. Here the three have unrelated causes - a
// non-finite bone is arithmetic that already went wrong upstream, a perspective residue means
// the bone was never an object-to-world matrix in the first place, and a collapsed basis is a
// matrix that is affine and still draws the mesh as a line.
//
// The residue is bucketed for the same reason the world one is. Barrels are stable and skinned
// meshes shake, so if the bone residues sit just over the tolerance this gate is refusing poses
// it should accept and the fix is the same shape as AFFINETOL; if they are large, these are not
// bone matrices and no tolerance will make them into any.
bool RemixGSRender::bone_rejected(const remix_rsx::mat4& bone)
{
	if (!remix_rsx::mat4_is_finite(bone))
	{
		++m_bone_fail_counts[0];
		return true;
	}

	if (!remix_rsx::is_affine(bone, s_world_affine_tolerance))
	{
		const f32 residue = std::abs(bone.m[0][3]) + std::abs(bone.m[1][3])
			+ std::abs(bone.m[2][3]) + std::abs(bone.m[3][3] - 1.f);

		m_bone_residue_max = std::max(m_bone_residue_max, residue);

		++m_bone_residue_buckets[
			residue < 0.05f ? 0 :
			residue < 0.2f  ? 1 :
			residue < 1.f   ? 2 :
			residue < 10.f  ? 3 : 4];

		++m_bone_fail_counts[1];
		return true;
	}

	return false;
}

bool RemixGSRender::per_draw_transform(remixapi_Transform& out)
{
	out = s_identity_transform;

	if (!m_active_camera.valid || remix_rsx::nocam_enabled())
	{
		note_world_fail(0);
		return false;
	}

	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;
	remix_rsx::mat4 world{};

	// Which of the four world-building branches below produced 'world'. Read only by the
	// RPCS3_REMIX_WORLDVP probe at the tail; costs one stack slot otherwise.
	const char* world_branch = "none";

	// The object placement match_basis_affine found, composed from this draw's live constants and
	// printed once per program - and deliberately NOT applied. It is the missing step between the
	// palette and the outer group for R2's indexed-palette characters, so wiring it in moves 82190
	// draws at once; this line is how the matrix gets read before that happens. A right-handed
	// basis has row lengths near equal and det>0. Order is the composition order, not the ucode's:
	// rows first, then the uniform scale, then the translation.
	if (fp.has_basis_affine && m_basis_census_seen.insert(m_current_vp_hash).second)
	{
		f32 a[4]{}, b[4]{}, s[4]{};

		if (remix_rsx::read_slot(fp.basis_row_slot[0], a)
			&& remix_rsx::read_slot(fp.basis_row_slot[1], b)
			&& remix_rsx::read_slot(fp.basis_scale_slot, s))
		{
			const f32 cross[3] = {
				(a[1] * b[2]) - (a[2] * b[1]),
				(a[2] * b[0]) - (a[0] * b[2]),
				(a[0] * b[1]) - (a[1] * b[0]),
			};

			const auto len3 = [](const f32 v[3])
			{
				return std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
			};

			const f32 av[3] = { a[0], a[1], a[2] };
			const f32 bv[3] = { b[0], b[1], b[2] };

			// det of the 3x3 whose rows are a, b, cross - i.e. |cross|^2 for a genuine cross
			// product, so a negative or near-zero value says the basis is not what was matched.
			const f32 det = (cross[0] * cross[0]) + (cross[1] * cross[1]) + (cross[2] * cross[2]);

			rsx_log.notice("Remix: basis-affine vp=%016llx arch=%s rows=c%u,c%u scale=c%u.w bias=c%u.xyz "
				"| a=[%.6g %.6g %.6g] |a|=%.6g | b=[%.6g %.6g %.6g] |b|=%.6g "
				"| axb=[%.6g %.6g %.6g] |axb|=%.6g det=%.6g | w=%.6g bias=[%.6g %.6g %.6g] | applied=%d",
				m_current_vp_hash,
				remix_rsx::archetype_name(fp.archetype),
				fp.basis_row_slot[0], fp.basis_row_slot[1], fp.basis_scale_slot, fp.basis_bias_slot,
				static_cast<f64>(a[0]), static_cast<f64>(a[1]), static_cast<f64>(a[2]), static_cast<f64>(len3(av)),
				static_cast<f64>(b[0]), static_cast<f64>(b[1]), static_cast<f64>(b[2]), static_cast<f64>(len3(bv)),
				static_cast<f64>(cross[0]), static_cast<f64>(cross[1]), static_cast<f64>(cross[2]),
				static_cast<f64>(len3(cross)), static_cast<f64>(det),
				static_cast<f64>(s[3]),
				static_cast<f64>(s[0]), static_cast<f64>(s[1]), static_cast<f64>(s[2]),
				remix_rsx::basis_affine_enabled() ? 1 : 0);
		}
	}

	// Everything the ucode applies between the vertex attribute and the outer group, in the order
	// it applies it:
	//     clip = ((attr * decode) * palette[a]) * outer
	// The decode half is why every branch below ends by prepending build_prescale - the vertices
	// submitted to Remix are the raw attribute values. The palette half is the one object matrix a
	// rigid indexed draw resolved to (resolve_indexed_world); it is composed here rather than in
	// each branch so the two can never end up in the wrong order relative to each other. A skinned
	// draw leaves m_indexed_world_valid clear, because its palette travels as bone transforms.
	const auto prepend_object_space = [&](remix_rsx::mat4& m)
	{
		// The object placement match_basis_affine found, applied first so it ends up between the
		// palette and the outer group - which is where the ucode applies it:
		//     clip = ((attr * decode) * palette[a]) * BASIS * outer
		// Prepending to 'm' while 'm' is still the outer group yields BASIS*outer; the palette and
		// decode prepends below then land in front of it, giving that order. Doing it after the
		// palette would place the character relative to its own bone rather than the world.
		//
		// Verified before it was allowed to run: on ba93cfeb and f7576a48 the composed basis reads
		// |a|=|b|=|axb|=1, a.b=0, det=1 - an orthonormal right-handed yaw about world up, with
		// w=1 and a plausible level-coordinate translation. See the 'Remix: basis-affine' census,
		// which still prints the same matrix and now reports applied=1.
		// RPCS3_REMIX_BASISAFFINE=0 restores the previous behaviour, where these draws reached the
		// outer group unplaced and landed at the camera group's origin - the floating, intersecting
		// stalker and tank.
		if (fp.has_basis_affine && remix_rsx::basis_affine_enabled())
		{
			f32 a[4]{}, b[4]{}, s[4]{};

			if (remix_rsx::read_slot(fp.basis_row_slot[0], a)
				&& remix_rsx::read_slot(fp.basis_row_slot[1], b)
				&& remix_rsx::read_slot(fp.basis_scale_slot, s))
			{
				const f32 w = s[3];
				const f32 cross[3] = {
					(a[1] * b[2]) - (a[2] * b[1]),
					(a[2] * b[0]) - (a[0] * b[2]),
					(a[0] * b[1]) - (a[1] * b[0]),
				};

				// Row-vector convention, matching the rest of this file (row 3 is the translation,
				// as the inverse-view camera position above relies on). out.x = dot(p, a) * w means
				// column 0 carries a, so the rows below are the transpose of the ucode's DP3 rows.
				remix_rsx::mat4 basis{};
				basis.m[0][0] = a[0] * w; basis.m[0][1] = b[0] * w; basis.m[0][2] = cross[0] * w; basis.m[0][3] = 0.f;
				basis.m[1][0] = a[1] * w; basis.m[1][1] = b[1] * w; basis.m[1][2] = cross[1] * w; basis.m[1][3] = 0.f;
				basis.m[2][0] = a[2] * w; basis.m[2][1] = b[2] * w; basis.m[2][2] = cross[2] * w; basis.m[2][3] = 0.f;
				basis.m[3][0] = s[0];     basis.m[3][1] = s[1];     basis.m[3][2] = s[2];         basis.m[3][3] = 1.f;

				m = remix_rsx::mat4_multiply(basis, m);
			}
		}

		if (m_indexed_world_valid)
		{
			m = remix_rsx::mat4_multiply(m_indexed_world, m);
		}

		// ...unless the bones already carry it. Remix applies the palette before the instance
		// transform, so for a blended draw the decode has to sit in front of each bone matrix
		// rather than out here - build_blend_skinning composes it there and sets this flag. Doing
		// both would apply the decode twice.
		if (m_scratch_bone_prescale_folded)
		{
			return;
		}

		if (remix_rsx::mat4 prescale{}; remix_rsx::build_prescale(fp, prescale))
		{
			m = remix_rsx::mat4_multiply(prescale, m);
		}
	};

	// The vertices submitted to Remix are the raw attribute values, so any decode the program
	// applies before its first matrix is part of this transform. A program whose decode was
	// recognised but whose constants cannot be read back is refused, not drawn raw: raw quantised
	// positions against a matrix that expects decoded ones is the vertex explosion, and the whole
	// point of recognising the decode is knowing that the raw values are wrong.
	// RPCS3_REMIX_POSAFFINE=0 restores drawing them.
	const bool decodes_position = fp.has_prescale || fp.has_const_affine;

	if (decodes_position && remix_rsx::position_affine_enabled())
	{
		if (remix_rsx::mat4 probe{}; !remix_rsx::build_prescale(fp, probe))
		{
			++m_stats.pos_decode_refused;
			note_world_fail(1);
			return false;
		}
	}

	if (m_active_camera.archetype == remix_rsx::vp_archetype::layered
		|| m_active_camera.archetype == remix_rsx::vp_archetype::skinned_layered)
	{
		if (fp.archetype == remix_rsx::vp_archetype::skinned_layered)
		{
			// The bone matrices already carry the vertex into the space the outer groups
			// expect, so the instance transform is only what sits *below* the view: the
			// groups the layered case would call the world. Two groups means the palette is
			// already world-space and the instance transform is the identity.
			const u32 world_groups = (fp.group_count >= 3) ? (fp.group_count - 2) : 0;
			world = remix_rsx::mat4_identity();

			for (u32 i = 0; i < world_groups; ++i)
			{
				remix_rsx::slot_block slots{};
				if (!remix_rsx::read_slot_block(fp.group_base[i], slots))
				{
					note_world_fail(2);
					return false;
				}

				world = remix_rsx::mat4_multiply(world, remix_rsx::slots_to_matrix(slots, fp.group_shape[i]));
			}

			prepend_object_space(world);

			if (!remix_rsx::mat4_is_finite(world) || !remix_rsx::is_affine(world, s_world_affine_tolerance))
			{
				note_world_fail(3);
				return false;
			}

			out = remix_rsx::to_remix_transform(world);
			return true;
		}

		if (fp.archetype == remix_rsx::vp_archetype::layered
			&& fp.group_count == m_active_camera.group_count
			&& fp.inner_is_input)
		{
			// world = G0 * ... * G(n-3); the last two groups are the view and the projection.
			world_branch = "layered";
			const u32 world_groups = (fp.group_count >= 3) ? (fp.group_count - 2) : 1;
			world = remix_rsx::mat4_identity();

			for (u32 i = 0; i < world_groups; ++i)
			{
				remix_rsx::slot_block slots{};
				if (!remix_rsx::read_slot_block(fp.group_base[i], slots))
				{
					note_world_fail(4);
					return false;
				}

				world = remix_rsx::mat4_multiply(world, remix_rsx::slots_to_matrix(slots, fp.group_shape[i]));
			}
		}
		else if (fp.archetype == remix_rsx::vp_archetype::fused && m_active_camera.has_view_proj_inverse)
		{
			// The program folded everything above the vertex attribute into one group, so
			// dividing out the resolved view-projection leaves exactly the world transform -
			// including any vertex decompression the program applies before the matrix.
			world_branch = "fused/vpinv";
			remix_rsx::slot_block slots{};
			if (!remix_rsx::read_slot_block(fp.outer_base(), slots))
			{
				note_world_fail(5);
				return false;
			}

			const remix_rsx::mat4 fused = remix_rsx::fold_viewport_z(
				remix_rsx::slots_to_matrix(slots, fp.outer_shape()),
				rsx::method_registers.viewport_scale_z(),
				rsx::method_registers.viewport_offset_z());

			world = remix_rsx::mat4_multiply(fused, m_active_camera.view_proj_inverse);
		}
		else
		{
			note_world_fail(6);
			return false;
		}

		// The decompression the program applies before its first matrix has to come first here
		// too, because the vertices submitted to Remix are the raw attribute values.
		prepend_object_space(world);
	}
	else if (m_active_camera.has_reference)
	{
		if (!fp.has_outer())
		{
			note_world_fail(7);
			return false;
		}

		// This branch accepts any has_outer() program - fused *and* layered - but it only ever read
		// fp.outer_base(). For a layered program (2+ groups) the outermost group is the projection
		// alone, so the submitted clip was attr * G(n-1) * ref^-1 instead of
		// attr * G0 * ... * G(n-1) * ref^-1: the inner groups carrying the model and view rows were
		// silently dropped. Geometry placed by a transform that only *partially* tracks the camera
		// is the helmet-HUD symptom verbatim.
		//
		// The full product is what the program itself computes - composite_ui_draw folds exactly the
		// same way for the 2D path (the loop above) and the layered-camera branch folds G0..G(n-3)
		// for its own case - so this is not a new model, it is the missing multiply.
		// RPCS3_REMIX_FULLCHAIN=0 restores the outer-only fold.
		const bool full_chain = fp.is_layered() && fp.group_count > 1 && remix_rsx::full_chain_enabled();

		world_branch = full_chain ? "ref/fullchain" : "ref/outer";

		remix_rsx::mat4 chain{};

		if (full_chain)
		{
			chain = remix_rsx::mat4_identity();

			for (u32 i = 0; i < fp.group_count; ++i)
			{
				remix_rsx::slot_block slots{};
				if (!remix_rsx::read_slot_block(fp.group_base[i], slots))
				{
					note_world_fail(8);
					return false;
				}

				chain = remix_rsx::mat4_multiply(chain, remix_rsx::slots_to_matrix(slots, fp.group_shape[i]));
			}

			++m_stats.world_layered_ref;
		}
		else
		{
			remix_rsx::slot_block slots{};
			if (!remix_rsx::read_slot_block(fp.outer_base(), slots))
			{
				note_world_fail(9);
				return false;
			}

			chain = remix_rsx::slots_to_matrix(slots, fp.outer_shape());
		}

		const remix_rsx::mat4 fused = remix_rsx::fold_viewport_z(
			chain,
			rsx::method_registers.viewport_scale_z(),
			rsx::method_registers.viewport_offset_z());

		// Which reference this draw is divided by. A viewmodel draw carries its own projection -
		// fovx 60.001 / fovy 36.132 / near 0.0900 against the world's 72.000 / 44.634 - so composing
		// it with the world camera's inverse leaves a matrix that is not a change of basis at all:
		// replayed on the dumped G0s it comes out with m[3][3] = -1077 and, once :5135 normalises
		// that away, a basis of (0.491, 0.002, 1.150). The translation survives intact, which is why
		// this passed every audit that looked at origins - the census reads anchor=0.446 - while the
		// mesh itself was being flattened 500x on Y and smeared across the screen.
		//
		// is_affine below cannot catch it: it tests column 3 only, so an anisotropic collapse is
		// invisible to it. The gate is the reference, not the test after it.
		// Counted in every mode, including 0, so the partition
		// considered == applied + fallback + refused holds whatever the knob is set to and mode 0
		// reads as all-fallback - which is the 148b467 behaviour stated as a number.
		const remix_rsx::mat4* reference = &m_active_camera.reference_inverse;

		f32 depth_scale = 0.f;
		f32 depth_offset = 0.f;

		if (classify_viewmodel_depth(depth_scale, depth_offset) == viewmodel_outcome::tagged)
		{
			const u32 vm_mode = remix_rsx::viewmodel_camera_mode();

			++m_stats.viewmodel_cam_considered;

			if (vm_mode == 0)
			{
				++m_stats.viewmodel_cam_fallback;
				report_viewmodel_camera_census("FALLBACK:world");
			}
			else if (vm_mode >= 2 && m_active_viewmodel.has_reference)
			{
				reference = &m_active_viewmodel.reference_inverse;
				++m_stats.viewmodel_cam_applied;
				report_viewmodel_camera_census("APPLIED");
			}
			else
			{
				// Mode 1, or mode 2 with nothing latched yet. Refused rather than drawn with the
				// world reference, on the rule the world gate two branches up already applies: a
				// missing object is shippable, a smeared one is not.
				++m_stats.viewmodel_cam_refused;
				report_viewmodel_camera_census(vm_mode >= 2 ? "REFUSED:noref" : "REFUSED:mode1");
				note_world_fail(10);
				return false;
			}
		}

		world = remix_rsx::mat4_multiply(fused, *reference);

		// The layered branches above have prepended the decode since e9a7956; this one never did,
		// so every title whose active camera is archetype 'fused' - which is R2's, on every frame
		// the log records arch=fused - had its quantised positions submitted undecoded even when the
		// matcher had already recognised the decode. clip = (attr*S + B) * M and we submit attr, so
		// the instance transform has to be S,B * M * reference_inverse, in that order.
		prepend_object_space(world);
	}
	else
	{
		note_world_fail(11);
		return false;
	}

	if (!remix_rsx::mat4_is_finite(world))
	{
		note_world_fail(12);
		return false;
	}

	// Normalise out any projective scale before testing for affinity.
	if (const f32 w = world.m[3][3]; std::abs(w) > 1e-6f && std::abs(w - 1.f) > 1e-6f)
	{
		const f32 inv = 1.f / w;
		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				world.m[i][j] *= inv;
			}
		}
	}

	// RPCS3_REMIX_WORLDVP=<16 hex> probe. Instruments the second HUD hypothesis: the gate below
	// accepts anything within s_world_affine_tolerance and to_remix_transform then *drops* the
	// perspective row outright (RemixTransforms.h), so a program whose own projection differs
	// slightly from the reference camera's passes the gate and is silently flattened - which drifts
	// with pitch rather than snapping. The residue printed here is exactly what that truncation
	// throws away; a residue near 0 exonerates the gate, a residue near the tolerance indicts it.
	// Rate-limited to one line per stats window, and mirrored to remix_dump.log because RPCS3.log is
	// exclusively locked while the emulator runs. Diagnostic only; nothing below reads it.
	// Note: the skinned_layered branch returns before this point, so it is not instrumented - Haze
	// skins on the SPU, so its HUD cannot be that archetype.
	if (const u64 probe_vp = remix_rsx::world_vp_hash(); probe_vp != 0 && probe_vp == m_current_vp_hash)
	{
		if (const u64 window = m_frame_counter / s_stats_interval_flips; window != m_worldvp_window)
		{
			m_worldvp_window = window;

			const f32 residue =
				std::abs(world.m[0][3]) + std::abs(world.m[1][3]) +
				std::abs(world.m[2][3]) + std::abs(world.m[3][3] - 1.f);

			const std::string line = fmt::format(
				"Remix worldvp: frame=%llu vp=%016llx arch=%s groups=%u branch=%s inner_input=%d "
				"prescale=%d affine=%d persp_residue=%.6g tol=%.4g world=%s",
				m_frame_counter,
				m_current_vp_hash,
				remix_rsx::archetype_name(fp.archetype),
				fp.group_count,
				world_branch,
				fp.inner_is_input ? 1 : 0,
				fp.has_prescale ? 1 : 0,
				fp.has_const_affine ? 1 : 0,
				static_cast<f64>(residue),
				static_cast<f64>(s_world_affine_tolerance),
				remix_rsx::format_matrix(world));

			rsx_log.notice("%s", line);

			if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
			{
				out.write(line + '\n');
			}
		}
	}

	if (!remix_rsx::is_affine(world, remix_rsx::world_affine_tolerance()))
	{
		// 97.6% of every world refusal lands here - 623183 of 638630 in one Haze run, roughly a
		// third of the scene. The matrix chain resolved: this draw has a world transform, it just
		// carries a perspective row that did not cancel.
		//
		// That is a specific claim about the reference. world = fused * reference_inverse cancels
		// the projection only when the draw's projection is the reference camera's; a title that
		// draws through a second projection - a different FOV for a scope, a HUD, a cutscene rig -
		// leaves a residue proportional to how far the two disagree. So the residue distribution
		// separates the two possible fixes and no argument can: bucketed just over the tolerance
		// means one reference is right and the gate is merely too tight, while a residue orders of
		// magnitude over means these draws need a reference of their own and raising the tolerance
		// would only flatten them into the wrong place quietly.
		const f32 residue = std::abs(world.m[0][3]) + std::abs(world.m[1][3])
			+ std::abs(world.m[2][3]) + std::abs(world.m[3][3] - 1.f);

		m_affine_residue_max = std::max(m_affine_residue_max, residue);

		const usz bucket =
			residue < 0.05f ? 0 :
			residue < 0.2f  ? 1 :
			residue < 1.f   ? 2 :
			residue < 10.f  ? 3 : 4;

		++m_affine_residue_buckets[bucket];

		note_world_fail(13);
		return false;
	}

	out = remix_rsx::to_remix_transform(world);
	return true;
}

void RemixGSRender::submit_subdraw()
{
	auto& draw_call = rsx::method_registers.current_draw_clause;

	// Cleared here rather than next to the code that sets it, because dump_vertex_program runs
	// further down but *before* the skinning gate and calls per_draw_transform through
	// describe_skinning: leaving a previous subdraw's matrix live would have the diagnostic print
	// a world transform belonging to some other draw.
	m_indexed_world_valid = false;

	// Same reason, same lifetime: a blended draw folds the position decode into its bone matrices
	// and this says so. Left set, the next draw's instance transform would silently lose its
	// decode - the raw-quantised-position explosion, arriving one draw late.
	m_scratch_bone_prescale_folded = false;

	// Same per-subdraw lifetime: set by audit_vertex_extent, read at the submission point.
	m_vertex_flagged = false;

	// --- classify and skip -------------------------------------------------------------
	if (draw_call.is_immediate_draw)
	{
		++m_stats.skip_immediate;
		return;
	}

	if (draw_call.command == rsx::draw_command::inlined_array)
	{
		++m_stats.skip_inline_array;
		return;
	}

	if (!m_vertex_layout.volatile_blocks.empty())
	{
		++m_stats.skip_volatile;
		return;
	}

	if (m_vertex_layout.attribute_placement[0] != rsx::attribute_buffer_placement::persistent)
	{
		// ATTR0 comes from a register or is not fed at all: no positions to read.
		++m_stats.skip_register_attr0;
		return;
	}

	const rsx::primitive_type prim = draw_call.primitive;
	if (!remix_rsx::is_supported_primitive(prim))
	{
		++m_stats.skip_primitive;
		return;
	}

	if (rsx::method_registers.restart_index_enabled())
	{
		// Restart sentinels would have to be split into separate meshes.
		++m_stats.skip_restart_index;
		return;
	}

	// One-run bisector: drop everything one program draws and see what disappears.
	if (const u64 skip_hash = remix_rsx::skip_vp_hash(); skip_hash != 0 && skip_hash == m_current_vp_hash)
	{
		++m_stats.skip_vp;
		return;
	}

	// --- screen-space classification ----------------------------------------------------
	// When dumping, the skip is deferred until after the decode so that the 2D programs
	// still get a dump line: their ATTR0 bounding box is the corroborating evidence.
	const bool screen_space = !remix_rsx::keep_ui_enabled() && is_screen_space_draw();

	// The title's 2D draws are no longer thrown away: they are rasterized into the overlay
	// buffer, which needs their positions, so the pre-decode early-out only applies when the
	// compositor is unavailable.
	const bool compositing = m_remix.fork_features() && !remix_rsx::compositor_disabled();

	if (screen_space && !remix_rsx::dump_enabled() && !compositing)
	{
		++m_stats.skip_screen_space;
		return;
	}

	if (!screen_space)
	{
		// Every 3D draw is a camera candidate, whether or not it survives the gates below.
		update_camera_candidate();
	}

	// --- locate the interleaved block that feeds ATTR0 ---------------------------------
	// Cheap gate only; the real mapping (and its memory validation) runs once the vertex
	// range is known, through map_attribute().
	if (const rsx::interleaved_range_info* block = find_attribute_block(0); !block || block->attribute_stride == 0)
	{
		++m_stats.skip_layout;
		return;
	}

	// --- build a u32 triangle list ------------------------------------------------------
	m_scratch_indices.clear();

	u32 min_index = 0;
	u32 max_index = 0;
	u32 index_base = 0;

	const auto command = m_draw_processor.get_draw_command(rsx::method_registers);

	if (const auto* indexed = std::get_if<rsx::draw_indexed_array_command>(&command))
	{
		const rsx::index_array_type type = rsx::method_registers.index_type();
		const u32 type_size = get_index_type_size(type);
		const u32 element_count = draw_call.get_elements_count();
		const bool expandable = remix_rsx::is_buffer_utils_expandable(prim);
		const u32 index_capacity = expandable ? get_index_count(prim, element_count) : element_count;

		if (index_capacity == 0 || index_capacity > s_max_indices_per_mesh)
		{
			++m_stats.skip_layout;
			return;
		}

		m_scratch_index_bytes.resize(static_cast<usz>(index_capacity) * type_size);

		u32 written = 0;
		std::tie(min_index, max_index, written) = write_index_array_data_to_buffer(
			{ reinterpret_cast<std::byte*>(m_scratch_index_bytes.data()), m_scratch_index_bytes.size() },
			indexed->raw_index_buffer,
			type,
			prim,
			false,
			0,
			[](rsx::primitive_type p) { return remix_rsx::is_buffer_utils_expandable(p); });

		if (written == 0 || max_index < min_index)
		{
			++m_stats.skip_layout;
			return;
		}

		// Widen to u32 and rebase onto the decoded vertex block.
		m_scratch_indices.reserve(written);

		if (type == rsx::index_array_type::u16)
		{
			const u16* src = reinterpret_cast<const u16*>(m_scratch_index_bytes.data());
			for (u32 i = 0; i < written; ++i)
			{
				m_scratch_indices.push_back(u32{src[i]} - min_index);
			}
		}
		else
		{
			const u32* src = reinterpret_cast<const u32*>(m_scratch_index_bytes.data());
			for (u32 i = 0; i < written; ++i)
			{
				m_scratch_indices.push_back(src[i] - min_index);
			}
		}

		if (prim == rsx::primitive_type::triangle_strip)
		{
			// Native to GL/VK, so BufferUtils left it as a strip.
			m_scratch_indices_alt.clear();
			remix_rsx::strip_to_list(m_scratch_indices.data(), ::size32(m_scratch_indices), m_scratch_indices_alt);
			m_scratch_indices.swap(m_scratch_indices_alt);
		}

		index_base = rsx::method_registers.vertex_data_base_index();
	}
	else if (std::holds_alternative<rsx::draw_array_command>(command))
	{
		const u32 vertex_count = draw_call.get_elements_count();

		if (vertex_count < 3 || vertex_count > s_max_vertices_per_mesh)
		{
			++m_stats.skip_layout;
			return;
		}

		min_index = draw_call.min_index();
		max_index = (min_index + vertex_count) - 1;

		if (prim == rsx::primitive_type::triangles)
		{
			m_scratch_indices.resize(vertex_count);
			std::iota(m_scratch_indices.begin(), m_scratch_indices.end(), 0u);
		}
		else if (prim == rsx::primitive_type::triangle_strip)
		{
			remix_rsx::strip_to_list(0u, vertex_count, m_scratch_indices);
		}
		else
		{
			// Fan / polygon / quads. The BufferUtils helper only emits u16 indices.
			if (vertex_count > 0xFFFF)
			{
				++m_stats.skip_layout;
				return;
			}

			const u32 index_count = get_index_count(prim, vertex_count);
			if (index_count == 0 || index_count > s_max_indices_per_mesh)
			{
				++m_stats.skip_layout;
				return;
			}

			m_scratch_index_bytes.resize(static_cast<usz>(index_count) * sizeof(u16));
			write_index_array_for_non_indexed_non_native_primitive_to_buffer(
				reinterpret_cast<char*>(m_scratch_index_bytes.data()), prim, vertex_count);

			const u16* src = reinterpret_cast<const u16*>(m_scratch_index_bytes.data());
			m_scratch_indices.reserve(index_count);
			for (u32 i = 0; i < index_count; ++i)
			{
				m_scratch_indices.push_back(u32{src[i]});
			}
		}
	}
	else
	{
		++m_stats.skip_inline_array;
		return;
	}

	if (m_scratch_indices.size() < 3 || (m_scratch_indices.size() % 3) != 0)
	{
		++m_stats.skip_layout;
		return;
	}

	const u32 vertex_count = (max_index - min_index) + 1;
	if (vertex_count == 0 || vertex_count > s_max_vertices_per_mesh)
	{
		++m_stats.skip_layout;
		return;
	}

	// --- decode ATTR0 positions ---------------------------------------------------------
	const u32 first_vertex = index_base ? rsx::get_index_from_base(min_index, index_base) : min_index;

	attribute_view positions{};

	switch (map_attribute(0, first_vertex, vertex_count, positions))
	{
	case attribute_status::ok:
		break;
	case attribute_status::memory:
		++m_stats.skip_memory;
		return;
	default:
		++m_stats.skip_layout;
		return;
	}

	m_scratch_vertices.clear();
	m_scratch_vertices.resize(vertex_count);

	// The ucode may undo a per-vertex packing before its first matrix: 'pos.xyz * RCP(pos.w)'.
	// The divisor differs per vertex, so it cannot be folded into the world transform - it has to
	// be applied here, to the same value the vertex fetch would have handed the shader. Without
	// it every vertex sits displaced along its own ray and the mesh is blown apart from inside.
	const bool w_divide = m_current_fingerprint->has_wdivide && !remix_rsx::nowdivide_enabled();

	if (w_divide)
	{
		++m_stats.wdiv_draws;
	}

	for (u32 i = 0; i < vertex_count; ++i)
	{
		f32 position[4] = {};

		if (!remix_rsx::decode_position(positions.at(i), positions.type, positions.size, position))
		{
			++m_stats.skip_decode;
			return;
		}

		if (w_divide)
		{
			// A zero divisor is what the RSX would turn into an infinity; leaving the vertex
			// alone keeps one bad value from poisoning the whole mesh's bounds.
			const f32 w = position[3];

			if (std::isfinite(w) && std::abs(w) > 1e-8f)
			{
				const f32 inv = 1.f / w;
				position[0] *= inv;
				position[1] *= inv;
				position[2] *= inv;
			}
		}

		remixapi_HardcodedVertex& v = m_scratch_vertices[i];
		v.position[0] = position[0];
		v.position[1] = position[1];
		v.position[2] = position[2];
		v.normal[0] = 0.f;
		v.normal[1] = 0.f;
		v.normal[2] = 1.f;
		v.texcoord[0] = 0.f;
		v.texcoord[1] = 0.f;
		v.color = 0xFFFFFFFF;
	}

	// Bounds check the index list against what we actually decoded.
	for (const u32 index : m_scratch_indices)
	{
		if (index >= vertex_count)
		{
			++m_stats.skip_layout;
			return;
		}
	}

	// Every draw, skinned or not: do these vertices form one object?
	audit_vertex_extent(first_vertex, vertex_count, positions, w_divide);

	// RPCS3_REMIX_VTXREFUSE=1 drops a draw the audit called incoherent instead of submitting it.
	// Deliberately off by default and deliberately not landed as a fix: whether these draws reach
	// the scene at all is what vtx_spread_submitted is measuring, and refusing before that number
	// is in would be the third hypothesis-led change in a row on a problem that has twice turned
	// out to be somewhere else. With it on, one run says whether removing this geometry removes the
	// artifact - which is the same one-run A/B the skinning knobs provide, and the reason the
	// skin-reach result could be trusted.
	if (m_vertex_flagged && remix_rsx::vertex_spread_refuse())
	{
		++m_stats.vtx_spread_refused;
		return;
	}

	if (remix_rsx::dump_enabled())
	{
		dump_vertex_program(first_vertex, vertex_count, ::size32(m_scratch_indices));
	}

	if (screen_space)
	{
		++m_stats.skip_screen_space;

		// One line per program, once. The first-person weapon/arms are absent from every
		// capture and this is the largest refusal bucket that could plausibly hold them: a
		// viewmodel is drawn with its own near-plane projection, which is exactly what the
		// orthographic test can mistake for UI. A viewmodel reads as a few hundred to a few
		// thousand vertices in a bounding box a metre or two across; real UI reads as a handful
		// of vertices on a unit quad. The bbox is what tells them apart.
		if (remix_rsx::dump_enabled() && m_screen_census_seen.insert(m_current_vp_hash).second)
		{
			f32 lo[3] = { +3.4e38f, +3.4e38f, +3.4e38f };
			f32 hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };

			for (const remixapi_HardcodedVertex& v : m_scratch_vertices)
			{
				for (u32 c = 0; c < 3; ++c)
				{
					lo[c] = std::min(lo[c], v.position[c]);
					hi[c] = std::max(hi[c], v.position[c]);
				}
			}

			rsx_log.notice("Remix: screen-space refusal vp=%016llx vtx=%u idx=%llu bbox=[%.4g %.4g %.4g]..[%.4g %.4g %.4g] depth_write=%d",
				m_current_vp_hash, vertex_count, static_cast<u64>(m_scratch_indices.size()),
				lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
				rsx::method_registers.depth_write_enabled() ? 1 : 0);
		}

		if (compositing)
		{
			const u64 t0 = now_us();
			composite_ui_draw(first_vertex, vertex_count);
			m_timing.ui += now_us() - t0;
		}

		return;
	}

	// --- render-target feedback ---------------------------------------------------------
	// A 3D draw whose albedo samples a surface the RSX itself rendered into is the title
	// re-reading its own framebuffer - a post-process pass (tone map, bloom, colour grade) -
	// not world geometry. composite_ui_draw refuses exactly this on the 2D path; the ones that
	// happen to carry a matrix chain are classified 3D and land here instead, where submitting
	// them puts a screen-covering slab into the scene. Wrong twice over, for the same two
	// reasons the UI path gives: Remix already owns the final image, and with 'Write Color
	// Buffers' off the guest copy of that surface holds stale bytes anyway.
	//
	// Refused rather than submitted with categoryFlags HIDDEN, matching the UI path: a hidden
	// instance still costs a mesh, a material, a hash and a BLAS update every frame to produce
	// nothing, and HIDDEN is a fork-only bit while this hazard exists on every runtime.
	//
	// Placed after the decode so that a dump run still gets a 'Remix vp=' line for these
	// programs - the ATTR0 bounding box is the corroborating evidence that it was full-screen.
	if (!remix_rsx::keep_render_target_blits() && samples_bound_surface())
	{
		const u32 vertex_ceiling = remix_rsx::rt_feedback_max_vertices();
		const bool is_quad = (vertex_ceiling == 0) || (vertex_count <= vertex_ceiling);

		// One line per program, once, so the census of what this gate touches is readable from a
		// normal run: without it a large 'rt=' count cannot be told apart from a gate that is
		// eating the level.
		if (m_rt_feedback_seen.insert(m_current_vp_hash).second)
		{
			rsx_log.notice("Remix: rt-feedback vp=%016llx vtx=%u idx=%llu refused=%d",
				m_current_vp_hash, vertex_count, static_cast<u64>(m_scratch_indices.size()), is_quad ? 1 : 0);
		}

		if (is_quad)
		{
			++m_stats.skip_render_target;
			return;
		}

		++m_stats.rt_feedback_kept;
	}

	// --- skinning -----------------------------------------------------------------------
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;
	bool skinned = false;

	if (remix_rsx::strict_input_enabled() && fp.has_outer() && !fp.inner_is_input)
	{
		// Diagnostic bisect: drop the population whose matrix chain never reached the vertex
		// attribute, so a capture says whether the residual exploded geometry comes from them.
		++m_stats.skip_not_input;
		return;
	}

	if (fp.skin_unrecognised)
	{
		// A rig the recogniser detected but cannot prove it understands. Nothing is drawn: an
		// absent character is a shippable result, a character torn across the map is not.
		++m_stats.skin_unrecognised;

		// Split by which hardening condition fired, in the same order scan_vertex_program tests
		// them, so the three counters partition skin_unrecognised exactly and the dominant reason
		// is a number rather than a guess. Reading this aggregate as one population is what has
		// kept 186648 dropped draws unexplained.
		if (fp.arl_count > 1)
		{
			++m_stats.skin_unrec_arl;
		}
		else if (fp.foreign_indexed_reads != 0)
		{
			++m_stats.skin_unrec_foreign;
		}
		else
		{
			++m_stats.skin_unrec_reads;
		}

		// Names the program and the counts the refusal was decided on. Without this the only record
		// of a dropped rig is an aggregate, and the missing characters cannot be tied to a program.
		if (m_skin_unrec_seen.insert(m_current_vp_hash).second
			&& m_skin_unrec_census_lines < s_max_skin_unrec_census_lines)
		{
			++m_skin_unrec_census_lines;

			rsx_log.notice("Remix: skin-unrecognised vp=%016llx arch=%s arl=%u idx_reads=%u foreign=%u "
				"blended=%d bones=%u vtx=%u line=%u/%u | %s",
				m_current_vp_hash,
				remix_rsx::archetype_name(fp.archetype),
				fp.arl_count,
				fp.indexed_reads,
				fp.foreign_indexed_reads,
				fp.skin_blended ? 1 : 0,
				fp.blend_bones,
				vertex_count,
				m_skin_unrec_census_lines,
				s_max_skin_unrec_census_lines,
				fp.skin_note);
		}

		return;
	}

	if (fp.archetype == remix_rsx::vp_archetype::skinned_layered)
	{
		// Rigid first. A draw whose palette index comes out the same for every vertex is reading
		// one fixed matrix, so it needs no bone transforms at all - which also means it does not
		// need the Remix fork, and on a stock runtime it is the only half of this that can be
		// submitted. On Resistance 2 that is the terrain-chunk and batched-prop shape: 16 of its
		// 36 indexed programs are a 3-row object matrix at c31 selected by attr0.w, and a draw
		// that only ever touches one object is one matrix by definition.
		bool rigid = false;

		// Draws that only reached this path because the post-matrix translation was followed into
		// another register. Counted here rather than in the matcher because what matters is how
		// many draws it recovers, and because 'refused' has to be the same proven-or-nothing
		// refusal every other rig on this path answers to.
		if (fp.palette_bias_forwarded)
		{
			++m_stats.idxbias_considered;
		}

		// A blended rig is never offered to the rigid path. resolve_indexed_world reads one
		// attribute component and asks whether it is uniform over the draw; on a blend program that
		// component is bone 0's index only, so a draw whose bone 0 happens to be constant would be
		// collapsed onto a single matrix and the other three bones dropped - which is a character
		// drawn as one rigid lump. Proving a blend rig is really rigid would mean evaluating all
		// four bones and all four weights, i.e. doing build_blend_skinning's work to decide whether
		// to call it, so the draw simply goes there.
		if (!fp.skin_blended
			&& remix_rsx::indexed_world_enabled() && resolve_indexed_world(first_vertex, vertex_count, rigid) && rigid)
		{
			++m_stats.indexed_world_rigid;

			if (fp.palette_bias_forwarded)
			{
				++m_stats.idxbias_resolved;
			}
		}
		else if (remix_rsx::noskin_enabled() || !m_remix.fork_features() || !build_skinning(first_vertex, vertex_count))
		{
			// Neither one matrix nor a palette this can prove. Refused and counted, never drawn
			// with the palette unapplied - that is a mesh in its bind pose at the world origin.
			++m_stats.skin_skipped;
			++m_stats.indexed_world_refused;

			if (fp.palette_bias_forwarded)
			{
				++m_stats.idxbias_refused;
			}

			return;
		}
		else
		{
			skinned = true;
			++m_stats.indexed_world_skinned;

			if (fp.palette_bias_forwarded)
			{
				++m_stats.idxbias_resolved;
			}
		}

		// One line per program, once, at notice level: what the recogniser made of this rig and
		// what its indices turned out to be. Without it 'rigid' and 'skinned' are two numbers over
		// however many programs happen to land in them, and the shape of the palette - which is the
		// thing every previous milestone here got wrong - is not readable from a run at all.
		if (m_indexed_world_seen.insert(m_current_vp_hash).second)
		{
			std::string blend;

			// The four resolved slots of a blend rig, straight from the fingerprint: which
			// attribute component carries each bone's weight, which address-register component
			// selects its palette entry, and which component of the index attribute that address
			// component came from. The pairing is the one thing here that cannot be guessed - the
			// ARL of twelve of the sixteen R2 programs permutes a0 - so it is printed rather than
			// assumed, the same way the abs/negate bits are.
			if (fp.skin_blended)
			{
				fmt::append(blend, " blend=%u wattr=ATTR%u bones=[", fp.blend_bones, fp.blend_weight_attribute);

				for (u32 k = 0; k < fp.blend_bones && k < remix_rsx::max_blend_bones; ++k)
				{
					const remix_rsx::bone_index_chain& chain = fp.blend_bone[k];

					fmt::append(blend, "%sw.%c->a0.%c=%s%sATTR%u.%c%s ops=%u",
						k ? " " : "",
						"xyzw"[fp.blend_weight_component[k] & 3],
						"xyzw"[fp.blend_addr_swz[k] & 3],
						chain.index_negate ? "-" : "",
						chain.index_abs ? "|" : "",
						chain.attribute,
						"xyzw"[chain.component & 3],
						chain.index_abs ? "|" : "",
						chain.op_count);

					for (u32 o = 0; o < chain.op_count && o < remix_rsx::max_bone_index_ops; ++o)
					{
						const remix_rsx::bone_index_op& op = chain.ops[o];

						switch (op.op)
						{
						case remix_rsx::bone_index_op::kind::floor:
							blend += ",floor";
							break;
						case remix_rsx::bone_index_op::kind::immediate_scale:
							fmt::append(blend, ",*%g", static_cast<f64>(op.immediate));
							break;
						case remix_rsx::bone_index_op::kind::scale:
							fmt::append(blend, ",*c%u.%c", op.mul_slot, "xyzw"[op.mul_component & 3]);
							break;
						case remix_rsx::bone_index_op::kind::affine:
							fmt::append(blend, ",*c%u.%c+c%u.%c", op.mul_slot, "xyzw"[op.mul_component & 3],
								op.add_slot, "xyzw"[op.add_component & 3]);
							break;
						}
					}
				}

				blend += "]";
			}

			rsx_log.notice("Remix: indexed-world vp=%016llx base=c%u rows=%u stride=%u bias=%d fwdbias=%d attr=%u.%c abs=%d neg=%d rigid=%d vtx=%u%s",
				m_current_vp_hash, fp.palette_base, fp.palette_rows, fp.palette_stride,
				fp.palette_has_bias ? 1 : 0, fp.palette_bias_forwarded ? 1 : 0,
				fp.bone_attribute, "xyzw"[fp.bone_component & 3],
				fp.bone_index_abs ? 1 : 0, fp.bone_index_negate ? 1 : 0,
				rigid ? 1 : 0, vertex_count, blend);
		}
	}
	else if (fp.indexed_const && !remix_rsx::draw_indexed_const())
	{
		// A program whose one ARL loads the address register from a *constant* indexes the same
		// row for every vertex of the draw, so there is no palette here to leave unapplied - the
		// reads are ordinary constant reads wearing an indexed encoding. Proving that is what
		// makes the refusal below inapplicable; it is not a loosening of it. The address is
		// resolved rather than assumed so that a mis-read ARL fails here instead of drawing.
		//
		// Off by default: the two Resistance 2 (NPEA00431) programs of this shape build their
		// object-to-world step out of a constant basis - three DP3 rows against c47.xyz, c48.xyz
		// and a third vector the ucode builds from those two in a temp, then a MAD by c46.w and
		// c46.xyz - which no matcher here expresses, so their chain stops at the camera group and
		// mode 2 draws them in the wrong place. Mode 1 refuses exactly that case. The indexing was
		// never what blocked them. See indexed_uniform_mode.
		const u32 uniform_mode = remix_rsx::indexed_uniform_mode();
		bool released = false;
		s32 uniform_offset = 0;
		bool uniform_resolved = false;

		if (uniform_mode != 0 && fp.indexed_addr_uniform)
		{
			++m_stats.idxuniform_considered;

			uniform_resolved = remix_rsx::evaluate_uniform_address(fp, uniform_offset);
			released = uniform_resolved && (uniform_mode > 1 || fp.inner_is_input);

			if (released)
			{
				++m_stats.idxuniform_resolved;
			}
			else
			{
				++m_stats.idxuniform_refused;
			}
		}

		// One line per program, once, at notice level - the same census shape the render-target
		// gate uses. Without it this counter is a single number covering however many programs
		// happen to land in it, and the first Haze run after the geometry fix had it at 2.6 M
		// draws against 5.6 M submitted, i.e. it was silently eating a third of the scene.
		//
		// The uniform-address facts are on the same line because they are what the decision turns
		// on, and because 'uniform=1 input=0' is the one combination that says the indexing was
		// never the reason this program could not be placed.
		if (m_indexed_census_seen.insert(m_current_vp_hash).second)
		{
			rsx_log.notice("Remix: indexed-const refusal vp=%016llx vtx=%u idx=%llu arch=%s "
				"uniform=%d addr=c%u.%c abs=%d neg=%d base=[c%u..c%u] a=%d input=%d released=%d",
				m_current_vp_hash, vertex_count, static_cast<u64>(m_scratch_indices.size()),
				remix_rsx::archetype_name(fp.archetype),
				fp.indexed_addr_uniform ? 1 : 0,
				fp.addr_const_slot, "xyzw"[fp.addr_const_component & 3],
				fp.addr_index_abs ? 1 : 0, fp.addr_index_negate ? 1 : 0,
				fp.indexed_base_min, fp.indexed_base_max,
				uniform_resolved ? uniform_offset : -1,
				fp.inner_is_input ? 1 : 0,
				released ? 1 : 0);
		}

		// The program reads a constant palette through an address register and the recogniser
		// did not turn it into bone transforms - whether or not it managed to match an outer
		// group. Submitting it draws the mesh with the palette simply not applied: the bind
		// pose, in the wrong place, or a character torn across the map. That is the symptom
		// this milestone exists to remove, so the draw is refused and counted.
		//
		// Note the '!fp.has_outer()' this replaces: once the outer group matches (which it now
		// does for Haze, see the ADD-operand fix in match_mad_chain), 'has_outer()' is true and
		// the old condition let exactly these draws through.
		if (!released)
		{
			++m_stats.skin_unrecognised;
			++m_stats.skin_unrec_indexed;
			return;
		}
	}

	// --- multi-pass forward: keep the pass that establishes the surface -------------------
	// Resistance 2 (NPEA00431) draws its world in several forward passes over the same geometry,
	// all into one colour target (every draw reports mrt=1 ctarget=1, so this is not a deferred
	// G-buffer). The base pass writes depth and samples the diffuse map on unit 0; later passes
	// re-draw the same surfaces with depth_write off, sampling only the normal map on unit 1 to
	// light them. On the console those blend into one image. Submitted to a path tracer as
	// independent instances they do not blend - the later pass simply sits on top, so the world
	// renders as the normal maps.
	//
	// Measured, one capture, 665 dumped draws: 244 with depth_write=1 blend=0 (the base pass) and
	// 346 with depth_test=1 depth_write=0 - re-draws of geometry whose depth is already laid down.
	// The unit-1 textures dumped to remix_tex\ are visibly normal maps, and they match the Remix
	// 'Diffuse Albedo' debug view pixel for pixel: same panel lines, same rivets, same rock relief.
	// This also accounts for the frame cost, ~2-3x the geometry the title actually shows.
	//
	// The rule keeps anything that establishes a surface and drops only a re-draw that cannot be
	// one: no depth write, and the fragment program never samples the base unit. A pass that does
	// sample unit 0 is left alone even without a depth write, because decals and blended effects
	// look exactly like that and are real content. RPCS3_REMIX_PASSSKIP=0 restores submitting
	// every pass.
	//
	// This is deliberately the cheap half of the fix. The right end state is to merge the passes -
	// albedo from the base pass, normal map from the lighting pass, one Remix material carrying
	// both - which would also give real normal mapping instead of discarding it.
	// The depth-write half of this test was wrong and cost a run: skip_lighting_pass came back 0
	// because these programs *do* write depth. What identifies them is only which units they
	// sample. Proven three ways in one capture: 'Disable Textures' renders the scene grey rather
	// than blue, so the blue is a texture and not lighting; the BMPs written from the bind path -
	// i.e. images that were *selected as albedo* - are 107 normal maps on unit 1 against 155
	// diffuse maps on unit 0, a clean split; and the fragment-program census reports 6 programs
	// whose sampled mask has no bit 0 at all. Six programs binding 107 different normal maps is a
	// large share of the world, and with no diffuse map anywhere in the program there is nothing
	// for albedo_texture_unit() to pick but the normal map.
	if (m_current_fp_fingerprint && remix_rsx::pass_skip_enabled()
		&& m_current_fp_fingerprint->sampled_mask != 0
		&& (m_current_fp_fingerprint->sampled_mask & 1u) == 0)
	{
		++m_stats.skip_lighting_pass;
		return;
	}

	// --- albedo material ----------------------------------------------------------------
	// Resolved before the mesh hash because the material binds at CreateMesh time, so two
	// draws that share geometry but not their texture must not share a mesh handle.
	const auto& api = m_remix.api();

	remixapi_MaterialHandle material = nullptr;
	u64 albedo_hash = 0;

	if (m_remix.fork_features())
	{
		// Walk the eligible 2D units in order and keep the first that yields a material.
		// A refusal on the lowest unit is not a refusal for the draw: see albedo_texture_unit_in.
		bool unit_from_ucode = false;
		const u32 unit_mask = albedo_unit_mask(&unit_from_ucode);

		int unit = albedo_texture_unit_in(unit_mask, 0);
		const int first_unit = unit;
		const bool had_unit = (unit >= 0);

		if (had_unit)
		{
			// The pair is what says whether reading the ucode is doing anything on this title.
			if (unit_from_ucode)
			{
				++m_stats.tex_albedo_ucode;
			}
			else
			{
				++m_stats.tex_albedo_guess;
			}
		}

		while (unit >= 0)
		{
			const remix_rsx::texture_entry* entry = nullptr;
			const rsx::fragment_texture& tex = rsx::method_registers.fragment_textures[unit];
			material = m_textures.bind(api, tex, m_frame_counter, &entry);

			if (material && entry)
			{
				albedo_hash = entry->content_hash;
				++m_stats.tex_bound;

				if (unit != first_unit)
				{
					++m_stats.tex_unit_substituted;
				}

				// Must happen before the mesh hash below: the texcoords live inside
				// m_scratch_vertices, which is what the hash is taken over.
				apply_texcoords(static_cast<u32>(unit), *entry, tex, first_vertex, vertex_count);

				if (remix_rsx::dump_enabled() && m_dumped_textures.insert(albedo_hash).second)
				{
					dump_texture(*entry, tex, static_cast<u32>(unit));
				}

				break;
			}

			if (m_textures.over_budget())
			{
				// Out of budget every unit returns null; walking on would bind a cached wrong
				// unit rather than this draw's diffuse map. Leave it untextured for this frame.
				break;
			}

			// A unit refused for a permanent *format* reason is not a texture this backend could
			// ever bind, so walking past it is not the substitution the guard below exists to
			// prevent - there is no good albedo on this unit to lose. Haze (BLUS30094) is the
			// case: every shadow-receiving draw names a 2048x2048 DEPTH16 shadow map (fmt=92) on
			// its lowest referenced unit, the only refused format in a whole capture, and the
			// diffuse map it wants sits on a higher one. Measured 2026-08-08 in-world:
			// tex_unsupported=55360, tex_tombstone=55359 and tex_retry_refused=55360 are the same
			// population, with tex_unit_retry=0 - i.e. the guard fired on every one of them and
			// the walk never once ran. That is the untextured white on the legs, the helmet and
			// the sky dome. Haze reports tex_albedo_ucode=0, so unit_from_ucode is *always* false
			// here and the guard can never not fire; the R2 reasoning below is sound but its
			// precondition does not hold for a title whose ucode names no albedo unit at all.
			// RPCS3_REMIX_RETRYUNSUP=0 restores the unconditional guard.
			const bool unit_unsupported = entry && entry->unsupported;

			if (unit_unsupported && remix_rsx::retry_unsupported_enabled())
			{
				++m_stats.tex_retry_unsupported;
			}
			else if (remix_rsx::fp_albedo_enabled() && !unit_from_ucode)
			{
				// The program named no *other* colour source, so there is nothing this walk can
				// legitimately substitute - only the next referenced unit, which is the hazard the
				// budget guard above already recognised and only guarded for its own case. On
				// Resistance 2 (NPEA00431) that next unit is unit 1: 566 DXT45 binds and zero DXT1
				// against unit 0's 666 DXT1, i.e. the normal map, and binding it as albedo is the
				// flat blue on every large surface and the blue thumbnails in Remix's texture grid.
				// A missing texture is better than a wrong one, so the draw leaves untextured.
				// RPCS3_REMIX_FPALBEDO=0 restores the unrestricted walk of 9c73eb0.
				++m_stats.tex_retry_refused;
				break;
			}

			++m_stats.tex_unit_retry;
			unit = albedo_texture_unit_in(unit_mask, static_cast<u32>(unit) + 1);
		}

		if (!material)
		{
			++m_stats.tex_none;

			if (!had_unit)
			{
				++m_stats.tex_no_unit;
			}
		}
	}

	// An untextured draw is the only one whose colour this backend can improve on: white is
	// what it shows today. Before the mesh hash, which covers the colour.
	if (!material)
	{
		apply_vertex_colour(first_vertex, vertex_count);
	}

	// --- sky dome -----------------------------------------------------------------------
	// A draw that binds no texture, writes no depth and spans thousands of units is the title's
	// sky. Haze's is vp=fc0fac8afccec49a: 82 vertices, bbox [-5000 0 -5000]..[5000 398 5000],
	// depth_write=0, inputs=0x9 (position + diffuse colour, no texcoord at all). Because it is
	// vertex-coloured rather than textured, albedo_texture_unit() returns -1 and it reached Remix
	// with a null material - i.e. as an opaque *white* shell, drawn with a translation-free
	// camera-locked matrix, which is exactly the "white untextured dome surrounding me" report.
	// Tagged SKY it renders as the sky instead of as world geometry enclosing the player.
	//
	// The '!material' half of that test was Haze-specific and wrong as a general rule. A sky dome
	// is allowed to be textured, and Resistance 2's (NPEA00431) is: it carries cloud and water
	// detail, so albedo_texture_unit() resolves, 'material' is non-null, this test never ran, and
	// the dome was submitted as ordinary world geometry - a solid sphere standing inside the level,
	// occluding the scene and turning with the camera without ever enclosing the player.
	//
	// The decision itself is made further down, once per_draw_transform() has resolved the
	// instance transform, because both of the conditions that mean "sky" are statements about
	// *world* space and nothing here is in world space yet. All that is measured here is the raw
	// bounding box of what was submitted, which is the input to that.
	f32 sky_lo[3] = { +3.4e38f, +3.4e38f, +3.4e38f };
	f32 sky_hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };

	const bool sky_depth_ok = !rsx::method_registers.depth_write_enabled();

	const bool sky_texture_ok =
		(!material || remix_rsx::sky_allows_textured()) &&
		remix_rsx::sky_min_extent() > 0.f;

	const bool sky_candidate = sky_texture_ok && sky_depth_ok;

	// The census deliberately covers one population the test itself refuses to look at: draws that
	// write depth. The first live capture tagged 3228 draws out of 739228 candidates and the dome
	// was not among them, and the two anchored programs left untagged at that build were exactly
	// the depth-writing ones (f7576a48e6289f83 / ba93cfebcefde22f, 87 vtx, 3608 units). Censusing
	// them is what turns "should the depth-write requirement be relaxed?" into a measurement.
	const bool sky_census = sky_texture_ok && m_sky_census_lines < s_max_sky_census_lines;

	// The backdrop rule needs the same bounding box, and needs it for draws the sky test skips.
	const bool sky_backdrop = sky_texture_ok && remix_rsx::sky_backdrop_mode() != 0;

	// The hash rule needs it for a third population again: every *textured* draw, whatever its
	// depth state and whatever sky_allows_textured() says, because it is not deciding anything from
	// the geometry - it is learning which albedo hashes only ever appear on dome-shaped draws, and
	// a hash cannot be disqualified by a draw nobody measured. Untextured draws are skipped because
	// there is no hash to key on; the anchored rule above is what covers those (Haze's dome).
	const bool sky_hash = albedo_hash != 0
		&& remix_rsx::sky_hash_mode() != 0
		&& remix_rsx::sky_min_extent() > 0.f;

	if (sky_candidate || sky_census || sky_backdrop || sky_hash)
	{
		for (const remixapi_HardcodedVertex& v : m_scratch_vertices)
		{
			for (u32 c = 0; c < 3; ++c)
			{
				sky_lo[c] = std::min(sky_lo[c], v.position[c]);
				sky_hi[c] = std::max(sky_hi[c], v.position[c]);
			}
		}
	}

	// --- content hash -> mesh handle ----------------------------------------------------
	usz hash = rpcs3::fnv_seed;

	{
		const u64* words = reinterpret_cast<const u64*>(m_scratch_vertices.data());
		const usz word_count = (m_scratch_vertices.size() * sizeof(remixapi_HardcodedVertex)) / sizeof(u64);
		for (usz i = 0; i < word_count; ++i)
		{
			hash = rpcs3::hash64(hash, words[i]);
		}

		for (const u32 index : m_scratch_indices)
		{
			hash = rpcs3::hash64(hash, index);
		}

		if (m_current_vp_hash)
		{
			hash = rpcs3::hash64(hash, m_current_vp_hash);
		}

		if (albedo_hash)
		{
			hash = rpcs3::hash64(hash, albedo_hash);
		}

		if (skinned)
		{
			// The rigging is part of the mesh's identity: the same geometry bound to a
			// different skeleton must not share a handle. The bone *matrices* are deliberately
			// not folded in - they change every animation frame, and folding them would churn
			// the mesh cache exactly the way CPU baking would.
			for (const f32 raw : m_scratch_bone_raw)
			{
				hash = rpcs3::hash64(hash, std::bit_cast<u32>(raw));
			}

			for (const u32 index : m_scratch_bone_indices)
			{
				hash = rpcs3::hash64(hash, index);
			}

			// ...and the weights, for the same reason: on a blend rig they are per-vertex mesh
			// content, so the same positions bound with a different weighting are a different
			// mesh. They are as static as the indices are - a skin's weighting does not animate,
			// only the matrices it selects do - so this does not churn the cache.
			for (const f32 weight : m_scratch_bone_weights)
			{
				hash = rpcs3::hash64(hash, std::bit_cast<u32>(weight));
			}

			hash = rpcs3::hash64(hash, m_scratch_bones_per_vertex);
		}
	}

	if (hash == 0)
	{
		// Remix treats a zero hash as unset.
		hash = 1;
	}

	if (m_poisoned.contains(hash))
	{
		++m_stats.skip_poisoned;
		return;
	}

	auto it = m_meshes.find(hash);

	if (it != m_meshes.end())
	{
		// The cache hit. Counted against meshes_created so the pair reads as a key-stability
		// measure and not just a memory statistic: a scene drawn from stable geometry hits this
		// on nearly every draw after the first frame, because the same object hashes to the same
		// key every time it is submitted. Creations that keep pace with the draw count instead
		// mean the key is churning - Remix is being handed brand new geometry every frame where
		// it should be handed the same geometry moving, which is what the instance-coloured debug
		// view showed as a ground plane that fragments and reassembles between frames.
		++m_stats.meshes_reused;
	}

	if (it == m_meshes.end())
	{
		remixapi_MeshInfoSurfaceTriangles surface{};
		surface.vertices_values = m_scratch_vertices.data();
		surface.vertices_count = m_scratch_vertices.size();
		surface.indices_values = m_scratch_indices.data();
		surface.indices_count = m_scratch_indices.size();
		surface.skinning_hasvalue = skinned ? 1u : 0u;

		if (skinned)
		{
			// One tuple entry per bone per vertex. The fork asserts this is <= 4
			// (rtx_types.cpp:333) and derives the last weight of each tuple as
			// 1 - sum(the rest), which build_blend_skinning normalises for.
			surface.skinning_value.bonesPerVertex = m_scratch_bones_per_vertex;
			surface.skinning_value.blendWeights_values = m_scratch_bone_weights.data();
			surface.skinning_value.blendWeights_count = ::size32(m_scratch_bone_weights);
			surface.skinning_value.blendIndices_values = m_scratch_bone_indices.data();
			surface.skinning_value.blendIndices_count = ::size32(m_scratch_bone_indices);
		}

		surface.material = material;

		remixapi_MeshInfo mesh_info{};
		mesh_info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
		mesh_info.pNext = nullptr;
		mesh_info.hash = hash;
		mesh_info.surfaces_values = &surface;
		mesh_info.surfaces_count = 1;

		remixapi_MeshHandle handle = nullptr;
		const u32 status = remix_rsx::guarded_create_mesh(api.CreateMesh, &mesh_info, &handle);

		if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle)
		{
			m_poisoned.insert(hash);
			++m_stats.skip_poisoned;
			return;
		}

		++m_stats.meshes_created;
		++m_mesh_creates_by_vp[m_current_vp_hash];
		it = m_meshes.emplace(hash, mesh_entry{ handle, m_frame_counter }).first;
	}

	it->second.last_used_frame = m_frame_counter;

	remixapi_Transform transform = s_identity_transform;

	const bool world_resolved = per_draw_transform(transform);

	if (world_resolved)
	{
		++m_stats.world_applied;
	}
	else
	{
		++m_stats.world_fallback;

		// No resolved world transform means the mesh is submitted in raw model space at the
		// world origin - and every unresolved draw lands on the same spot, which is precisely
		// the "vertex explosion" the ADD-src2 fix removed for the 71 % it could resolve. The
		// residue is the ~20 programs still classified unknown(no matrix chain into HPOS).
		// Same rule as the skinning gate: a missing object is shippable, an exploding one is
		// not. RPCS3_REMIX_DRAWNOWORLD=1 restores the old behaviour for bisection.
		//
		// NOCAM is exempt: per_draw_transform returns false unconditionally there because that
		// mode *is* the all-identity milestone-1 path, and refusing would empty the scene.
		// Which programs, and why. The counter has said "half the scene" for several runs without
		// naming a single program, and "~20 programs still classified unknown" is an estimate
		// carried in a comment rather than a measurement. One line per program, once, bounded -
		// the same shape as the world-extent census. 'note' is the fingerprint's own account of
		// what it could not resolve ("no HPOS write" and friends), which is the field that says
		// whether the chain is absent, indexed, or simply unrecognised.
		if (m_world_refused_seen.insert(m_current_vp_hash).second
			&& m_world_refused_lines < s_max_world_refused_lines)
		{
			++m_world_refused_lines;

			const std::string line = fmt::format(
				"Remix world-refused: vp=%016llx arch=%s vtx=%u skinned=%d prescale=%d affine=%d "
				"consts=%u chain=%u indexed=%d fail=%s note=%s areason=%s | frame=%llu line=%u/%u",
				m_current_vp_hash,
				m_current_fingerprint ? remix_rsx::archetype_name(m_current_fingerprint->archetype) : "none",
				vertex_count,
				skinned ? 1 : 0,
				(m_current_fingerprint && m_current_fingerprint->has_prescale) ? 1 : 0,
				(m_current_fingerprint && m_current_fingerprint->has_const_affine) ? 1 : 0,
				m_current_fingerprint ? m_current_fingerprint->distinct_consts : 0u,
				m_current_fingerprint ? m_current_fingerprint->chain_instructions : 0u,
				(m_current_fingerprint && m_current_fingerprint->indexed_const) ? 1 : 0,
				m_world_fail,
				m_current_fingerprint ? m_current_fingerprint->note : "no fingerprint",
				m_current_fingerprint ? m_current_fingerprint->affine_reason : "",
				m_frame_counter,
				m_world_refused_lines,
				s_max_world_refused_lines);

			rsx_log.notice("%s", line);

			if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
			{
				out.write(line + '\n');
			}
		}

		if (!remix_rsx::draw_without_world() && !remix_rsx::nocam_enabled())
		{
			++m_stats.world_refused;

			if (!m_active_camera.valid)
			{
				++m_stats.world_refused_nocam;
			}

			return;
		}
	}

	// --- sky dome, decided --------------------------------------------------------------
	// Both tests are in world space, which is why this could not run where the bounding box was
	// taken: the vertices submitted to Remix are the program's *raw* attribute values and the
	// decode that turns them into world units lives in 'transform' (per_draw_transform prepends
	// build_prescale). Measuring the raw box is what tagged cat_sky=825915 of 2038738 R2 draws -
	// 40.5% of the scene, lit as sky, uniformly blue - because R2's positions are quantised 16-bit
	// integers and every one of its meshes "spans tens of thousands of units" before the decode.
	//
	//   extent  the transformed AABB's widest axis, sum_j |M[i][j]| * raw_extent_j, which is exact
	//           for an affine transform. Replaying the 157 dumped draws that carry a fused matrix:
	//           63 clear 2000 measured raw, 27 measured in world units.
	//   anchor  the distance from the camera to the draw's own origin - the instance transform's
	//           translation. A sky dome is the only thing in a scene that is *placed on the eye*:
	//           Haze's fc0fac8afccec49a sits at 0.000, R2's 41c59a3a2bfc71bf and c1781a2e32aba35d
	//           at 2.404 (on the ground below the eye), and the nearest draw that clears the other
	//           two gates without being a dome is 96.91 away. That is what carries the filter -
	//           extent alone leaves 27 of 157, the anchor alone leaves 5, the pair leaves 3 and all
	//           3 are domes.
	//
	// No resolved world transform means no world space to measure in and no camera to measure
	// against, so those draws are refused rather than guessed - the same rule the world gate above
	// applies to the geometry itself. RPCS3_REMIX_SKYANCHOR=0 drops the anchor requirement.
	bool is_sky = false;

	if (sky_candidate || sky_census || sky_backdrop || sky_hash)
	{
		// Counted here rather than at the bounding box, so that candidates is exactly the sum of
		// the four refusals plus the cat_sky increments made below - the RPCS3_REMIX_CAT_SKY hash
		// list also raises cat_sky, and it is not this test. An over-tag is then one ratio to read
		// rather than a blue screen to look at.
		if (sky_candidate)
		{
			++m_stats.sky_candidates;
		}

		const bool measured = world_resolved && m_active_camera.valid;

		f32 extent = 0.f;
		f32 anchor = 0.f;
		f32 units_per_vertex = 0.f;

		// Whether the camera is inside the draw's transformed bounding box. Computed here rather
		// than inside the backdrop rule that consumes it, so that an ordinary census run - no env
		// var set - already reports it per program and the rule can be judged before it is armed.
		bool inside = false;

		if (measured)
		{
			inside = true;

			for (u32 i = 0; i < 3; ++i)
			{
				// Centre and half-extent of the transformed box on this world axis. The transform
				// is affine, so the image of the box is exactly the box of the transformed corners
				// and this closed form is that box without visiting all eight.
				f32 centre = transform.matrix[i][3];
				f32 half = 0.f;

				for (u32 j = 0; j < 3; ++j)
				{
					centre += transform.matrix[i][j] * (sky_hi[j] + sky_lo[j]) * 0.5f;
					half += std::abs(transform.matrix[i][j]) * (sky_hi[j] - sky_lo[j]) * 0.5f;
				}

				extent = std::max(extent, half * 2.f);

				if (!(std::abs(m_active_camera.position[i] - centre) <= half))
				{
					inside = false;
				}
			}

			const f32 dx = transform.matrix[0][3] - m_active_camera.position[0];
			const f32 dy = transform.matrix[1][3] - m_active_camera.position[1];
			const f32 dz = transform.matrix[2][3] - m_active_camera.position[2];
			anchor = std::sqrt(dx * dx + dy * dy + dz * dz);

			units_per_vertex = extent / static_cast<f32>(std::max(vertex_count, 1u));
		}

		const f32 anchor_limit = remix_rsx::sky_max_anchor();

		sky_outcome outcome = sky_outcome::tagged;

		if (!sky_depth_ok)
		{
			// Census-only: this draw was never a candidate, so nothing is counted for it.
			outcome = sky_outcome::reject_depth_write;
		}
		else if (!measured)
		{
			outcome = sky_outcome::reject_noworld;
			++m_stats.sky_refused_noworld;
		}
		else if (!std::isfinite(extent) || extent < remix_rsx::sky_min_extent())
		{
			outcome = sky_outcome::reject_extent;
			++m_stats.sky_refused_extent;
		}
		else if (anchor_limit > 0.f && (!std::isfinite(anchor) || anchor > anchor_limit))
		{
			outcome = sky_outcome::reject_anchor;
			++m_stats.sky_refused_anchor;

			// Split out the frames where the camera is a held one, i.e. the anchor was measured
			// against the previous frame's eye rather than this frame's. Those are the only
			// rejections that can be an artefact of the camera rather than of the draw.
			if (m_camera_age != 0)
			{
				++m_stats.sky_refused_anchor_held;
			}
		}
		else
		{
			is_sky = true;
		}

		// --- learned-dome rule ---------------------------------------------------------------
		// A tessellated dome is not one draw, it is a stack of latitude bands, and only the
		// widest of them clears sky_min_extent(). Haze's (vp=fc0fac8afccec49a, radius 5000) is
		// three or more: the horizon band spans [-5000 0 -5000]..[5000 398 5000] (extent 10000,
		// tagged), then [y 4939..4984] at extent 1558, then the zenith cap [y 4984..5000] at
		// extent 797. Lowering the floor only moves the cut - measured directly: at minext=2000
		// the refusal is the 1558 band, at minext=1200 it is the 797 cap. Every ring is the same
		// vertex program, camera-locked (origin=[0 0 0]), depth-write-off and materialless, so
		// the program is the discriminator the extent cannot be.
		//
		// Same shape as the SKYHASH rule - learn an identity from draws that already passed on
		// their own, then admit its siblings - but keyed on the vertex program instead of the
		// albedo hash, because a vertex-coloured dome has albedo=0 and SKYHASH structurally
		// cannot see it. The guards are what keep this from generalising into world geometry: a
		// ring must come from an armed program, write no depth, resolve no material, and still
		// have failed *only* on extent. Anything that writes depth or binds a texture is refused
		// by the rule above and never reaches here.
		if (is_sky && m_current_vp_hash)
		{
			m_sky_dome_programs.insert(m_current_vp_hash);
		}
		else if (outcome == sky_outcome::reject_extent
			&& remix_rsx::sky_learn_dome_enabled()
			&& !material
			&& m_current_vp_hash
			&& m_sky_dome_programs.count(m_current_vp_hash) != 0)
		{
			is_sky = true;
			++m_stats.sky_learned_ring;
		}

		// --- backdrop rule, evaluated independently of everything above ---------------------
		// R2's visible backdrop is refused twice over by the dome rule: it writes depth, and its
		// origin is 82 units from the eye. Relaxing either gate on its own does not reach it, and
		// relaxing both at once is exactly the change that produced the 40.5% over-tag. So this
		// asks three different questions of the same draw - is it bigger than sky_min_extent(), is
		// the camera *inside* it, and is it made of so few vertices per unit that it cannot be a
		// wall - and at mode 1 it only counts the answer. RPCS3_REMIX_SKYBACKDROP=2 tags.
		if (sky_backdrop && measured && std::isfinite(extent) && extent >= remix_rsx::sky_min_extent())
		{
			if (inside && units_per_vertex >= s_sky_backdrop_min_units_per_vertex)
			{
				++m_stats.sky_backdrop_hit;

				if (!sky_depth_ok)
				{
					++m_stats.sky_backdrop_dw;
				}

				if (remix_rsx::sky_backdrop_mode() >= 2)
				{
					is_sky = true;
					outcome = sky_outcome::tagged_backdrop;
				}
			}
		}

		// --- albedo-hash rule (sky_hash_mode) ----------------------------------------------
		// Material identity instead of geometry. Every rule above asks how big a draw is or where
		// it sits, and on this title those answers are fuzzy: the geometric backdrop rule matched
		// 1.11% of submitted draws in the capture it was measured against, which is far too broad
		// to arm because 1.11% of R2's draws is world geometry. A texture hash is exact - two draws
		// either sample the same image or they do not.
		//
		// The geometry is still used, but only to *learn*. 'dome_shaped' is the backdrop rule's own
		// predicate, evaluated here for its answer rather than for its verdict, and a hash is armed
		// only when every draw carrying it has come back dome-shaped over at least
		// s_sky_hash_min_draws draws. A single non-dome draw disqualifies it permanently. That
		// inverts the failure mode: a threshold rule mis-tags whatever sits near the threshold,
		// while this one can only mis-tag a texture the title uses on nothing but domes - and the
		// census names those before mode 2 tags anything.
		//
		// A hash is inserted only on a dome-shaped draw, which is what keeps the table small: R2
		// binds 737 unique textures across the dumped captures and only a handful of them are ever
		// drawn as a dome. The cost of that is an ordering asymmetry worth stating - a texture used
		// on world geometry *before* its first dome-shaped draw contributes nothing to 'other'
		// until the next world draw carrying it. In practice both orders occur within a frame,
		// because the draws that would disqualify it are submitted every frame, so the
		// disqualification arrives inside the first second and s_sky_hash_min_draws is not reached.
		if (sky_hash)
		{
			const bool dome_shaped = measured && inside
				&& std::isfinite(extent) && extent >= remix_rsx::sky_min_extent()
				&& units_per_vertex >= s_sky_backdrop_min_units_per_vertex;

			auto it = m_sky_hash_seen.find(albedo_hash);

			if (it == m_sky_hash_seen.end() && dome_shaped && m_sky_hash_seen.size() < s_max_sky_hash_tracked)
			{
				it = m_sky_hash_seen.emplace(albedo_hash, sky_hash_entry{}).first;
				++m_stats.sky_hash_tracked;
			}

			if (it != m_sky_hash_seen.end())
			{
				sky_hash_entry& hash_entry = it->second;

				++m_stats.sky_hash_considered;

				if (dome_shaped)
				{
					++hash_entry.dome;
					++m_stats.sky_hash_dome;

					if (!hash_entry.vp)
					{
						hash_entry.vp = m_current_vp_hash;
					}
				}
				else
				{
					++hash_entry.other;
				}

				// Refuse-and-count: a mixed hash is not merely skipped, it is counted, so that
				// "the rule tagged nothing" and "the rule found the sky and threw it away because
				// the title reuses the texture" are two different readings rather than one silence.
				if (hash_entry.other != 0)
				{
					++m_stats.sky_hash_rejected;

					if (!hash_entry.reported_mixed)
					{
						hash_entry.reported_mixed = true;
						report_sky_hash_census(albedo_hash, hash_entry, false, extent, units_per_vertex, vertex_count);
					}
				}
				else if (hash_entry.dome >= s_sky_hash_min_draws)
				{
					++m_stats.sky_hash_matched;

					if (!hash_entry.reported_armed)
					{
						hash_entry.reported_armed = true;
						report_sky_hash_census(albedo_hash, hash_entry, true, extent, units_per_vertex, vertex_count);
					}

					// Mode 1 stops here: matched is the preview of what mode 2 would tag, with the
					// image untouched, which is the measurement the geometric rules never had
					// before they were widened.
					if (remix_rsx::sky_hash_mode() >= 2)
					{
						is_sky = true;
						outcome = sky_outcome::tagged_hash;
						++m_stats.sky_hash_tagged;
					}
				}
			}
		}

		if (sky_census)
		{
			report_sky_census(outcome, vertex_count, !sky_depth_ok, sky_lo, sky_hi, transform,
				extent, anchor, measured, inside, units_per_vertex, albedo_hash);
		}
	}

	// --- viewmodel, decided by the viewport depth range ---------------------------------------
	// The first-person arms and weapon. Up to 81af315 nothing in this backend had any notion of
	// one: REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL and REMIXAPI_CAMERA_TYPE_VIEW_MODEL were both
	// declared in remix_c.h and neither appeared anywhere in code, so the arms were submitted as
	// ordinary world geometry - lit by world lights, occluded by world walls, placed by the world
	// camera - and the Remix dev menu read "VIEWMODEL Position: -" on every capture.
	//
	// This is the mirror image of the sky-dome test above and is decided in the same place and for
	// the same reason: the anchor it reports is a statement about world space, so it cannot be made
	// before per_draw_transform() has resolved the instance transform. The *rule* itself needs
	// neither world space nor a camera, which is deliberate - see s_viewmodel_max_anchor.
	//
	// The rule is the viewport depth range, and it is a mechanism rather than a correlation. The
	// G0 recovery over these dumps gives z_ndc = 1 - near/w, i.e. z_ndc in [0, 1] with the far
	// plane at infinity, so the viewport maps a draw into window depth [offset_z, offset_z+scale_z].
	// R2 runs its entire world at [0, 1] and one thing at [0, 0.2]: the front fifth of the buffer,
	// which forces every pixel of it in front of any world pixel past w = near/0.8 ~= 0.11 units.
	// That is the standard "the weapon never clips into a wall" trick and it is why the bucket is
	// there at all. Full measurement and the two corroborating readings are on viewmodel_mode().
	//
	// Replayed before it was written, over all 10153 draws in the scratchpad dumps: this rule
	// selects 168, every one of them [0, 0.2] at clip 1280x704, over exactly two vertex programs.
	// Nothing from any other program, title or resolution.
	//
	// The one thing worth restating here, because it is what a shortcut would get wrong: the two
	// programs in that bucket, 1438eb79c0843fea and 7a4a57869f9c4a1f, *also* draw a 5954-vertex
	// character at [0, 1] in 78 dumped draws. The program hash does not separate the viewmodel from
	// world geometry. Only the per-draw depth range does, which is why this test is here and not in
	// classify_draw() or a hash list.
	bool is_viewmodel = false;

	const u32 viewmodel_mode = remix_rsx::viewmodel_mode();

	if (viewmodel_mode != 0)
	{
		++m_stats.viewmodel_considered;

		f32 depth_scale = 0.f;
		f32 depth_offset = 0.f;

		const viewmodel_outcome outcome = classify_viewmodel_depth(depth_scale, depth_offset);

		switch (outcome)
		{
		case viewmodel_outcome::reject_full_range:
			++m_stats.viewmodel_refused_full_range;
			break;
		case viewmodel_outcome::reject_offset:
			++m_stats.viewmodel_refused_offset;
			break;
		case viewmodel_outcome::tagged:
			is_viewmodel = true;
			++m_stats.viewmodel_tagged;
			break;
		case viewmodel_outcome::count:
			break;
		}

		// Measured for every draw the rule selects, reported and counted, never used to refuse.
		// Same closed form the sky anchor uses: the instance transform's translation against the
		// eye. R2's viewmodel reads 0.446 against a nearest same-frame world draw of 8.930.
		f32 anchor = 0.f;

		// Measured for every *considered* draw, not only tagged ones. On a title the depth rule
		// selects, the two populations are the same and this changes nothing. On Haze (BLUS30094)
		// the rule selects nothing at all - vm_tagged=0 of vm_considered=353598, because it reports
		// scale_z=0.49875 offset_z=0.50125 on every draw in the scene including the sky dome, so
		// there is no depth signal to threshold - and gating the measurement on the tag meant the
		// census printed anchor=0 measured=0 on every line and could not be used to look for a
		// replacement discriminator. The anchor is the obvious candidate: first-person geometry is
		// camera-locked, so its instance translation should sit ~0 from the eye every frame, which
		// is the same closed form the sky anchor already uses from the other end of the scale.
		const bool measured = world_resolved && m_active_camera.valid;

		{
			if (measured)
			{
				const f32 dx = transform.matrix[0][3] - m_active_camera.position[0];
				const f32 dy = transform.matrix[1][3] - m_active_camera.position[1];
				const f32 dz = transform.matrix[2][3] - m_active_camera.position[2];
				anchor = std::sqrt(dx * dx + dy * dy + dz * dz);

				if (!std::isfinite(anchor) || anchor > s_viewmodel_max_anchor)
				{
					++m_stats.viewmodel_far;
				}
			}
			else
			{
				++m_stats.viewmodel_noanchor;
			}
		}

		// reject_full_range is deliberately not censused. It is every world program in the title -
		// 77 of them at clip 1280x704 in these dumps - and naming them would spend the whole
		// budget before the two that matter ever drew. The counter already bounds that
		// population; the census exists to name the draws the rule *selects*, and the near-miss
		// it rejects on the near-end test.
		if (outcome != viewmodel_outcome::reject_full_range &&
			m_viewmodel_census_lines < s_max_viewmodel_census_lines)
		{
			report_viewmodel_census(outcome, vertex_count, depth_scale, depth_offset,
				transform, anchor, measured);
		}
	}

	// --- post-transform geometry, measured ----------------------------------------------
	// The last point at which every input to the submitted geometry exists: the vertices, the bone
	// palette, and the instance transform. Placed after the sky decision because a dome is
	// legitimately the size of the level and has to be exempt, and before the instance is built
	// because a refusal here must cost nothing downstream.
	if (!audit_world_extent(transform, skinned, vertex_count, is_sky))
	{
		return;
	}

	remixapi_InstanceInfoBoneTransformsEXT bone_transforms{};
	remixapi_InstanceInfoBlendEXT blend_state{};

	remixapi_InstanceInfo instance{};
	instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.pNext = nullptr;
	instance.categoryFlags = classify_draw(albedo_hash);

	if (is_sky)
	{
		instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
		++m_stats.cat_sky;
	}

	if (is_viewmodel && viewmodel_mode >= 2)
	{
		// The runtime side of this now exists. The fork at dxvk-remix-numos3 declares
		// VIEW_MODEL = 1 << 26 (matching this header, which is synced wholesale from its
		// public/include), routes it in categoryToCameraType() (rtx_remix_api.cpp:730), and
		// assigns the result to 'prototype.cameraType' in toRtDrawState() (:916) rather than
		// hardcoding CameraType::Main. That assignment is the load-bearing half: the runtime
		// registers a viewmodel candidate on 'drawCall.cameraType == CameraType::ViewModel'
		// and nothing else (rtx_instance_manager.cpp:1374), and ExternalDrawState's own
		// cameraType field never reaches it - it only selects which camera's matrices
		// submitExternalDraw() fetches. Requires runtime >= 0.1000.1.
		//
		// Note the bit is deliberately *not* an InstanceCategories member on the runtime side:
		// internally "view model" is a CameraType, so toRtCategories() ignores this bit by
		// design. Do not expect it to show up in category-flag dumps.
		//
		// Tagging is necessary but still not sufficient. Three things outside this file gate
		// whether the arms actually render through the viewmodel pass:
		//   1. a REMIXAPI_CAMERA_TYPE_VIEW_MODEL camera submitted every frame - not optional
		//      and not synthesised. createViewModelInstances() returns early on
		//      '!cameraManager.isCameraValid(CameraType::ViewModel)' (rtx_instance_manager.cpp
		//      :1504) and then builds its correction matrix from *both* cameras, taking XY
		//      from the viewmodel projection and Z/W from the main one (:1521 onward). The
		//      measurement is in hand: these draws carry fovx 60.001 / fovy 36.132 with near
		//      0.090, against the world's 72.000 / 44.634. That is the camera to submit;
		//   2. rtx.viewModel.enable = True in rtx.conf. It defaults to false
		//      (rtx_options.h:435), and the pass bails at rtx_instance_manager.cpp:1499;
		//   3. rtx.playerModel.enableInPrimarySpace = False. When it is on, the pass masks
		//      every viewmodel candidate to zero and returns (:1510) - the tag is honoured
		//      and the geometry is then deliberately hidden.
		// Of these, only (1) is this backend's job; (2) and (3) are rtx.conf.
		instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
	}

	instance.mesh = it->second.handle;
	instance.transform = transform;
	instance.doubleSided = (remix_rsx::cull_from_rsx() && rsx::method_registers.cull_face_enabled()) ? 0u : 1u;

	if (skinned)
	{
		// Read per draw, not per clause: a title that uploads a new palette between draw calls
		// of one clause gets the pose it asked for.
		bone_transforms.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
		bone_transforms.pNext = nullptr;
		bone_transforms.boneTransforms_values = m_scratch_bone_transforms.data();
		bone_transforms.boneTransforms_count = ::size32(m_scratch_bone_transforms);
		instance.pNext = &bone_transforms;
	}

	if (remix_rsx::blend_state_enabled())
	{
		// Everything up to ae94587 submitted every draw fully opaque: instance.pNext only ever
		// carried bone transforms, so the runtime never saw NV4097_SET_BLEND_ENABLE at all. One
		// Resistance 2 capture of 665 dumped draws had ~163 with blend=1 and all 163 arrived as
		// solid geometry - light shafts as white walls, a sun card occluding the level behind it,
		// world-space distance markers as black boxes.
		//
		// Chained *in front of* whatever is already there rather than replacing it: a skinned
		// translucent draw has to carry both structs, and the runtime walks the chain by sType
		// (pnext::find, rtx_remix_api.cpp:916) so the order does not matter.
		blend_state.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;

		// These nine are copied out of the ext unconditionally (rtx_remix_api.cpp:921-929),
		// i.e. whether or not the material asked for the draw call's alpha state, so they must
		// reproduce LegacyMaterialData's own defaults (rtx_materials.h:1822-1831) or merely
		// chaining the struct would rewrite a fixed-function texture stage this backend has
		// never configured. They are byte-for-byte what remix.h:827-834 defaults them to.
		// isVertexColorBakedLighting is the one with a visible effect and true is what the
		// pre-ext path ran with, so parity means 1 here, not 0.
		blend_state.textureColorArg1Source = 1;  // RtTextureArgSource::Texture
		blend_state.textureColorArg2Source = 0;  // RtTextureArgSource::None
		blend_state.textureColorOperation = 3;   // DxvkRtTextureOperation::Modulate
		blend_state.textureAlphaArg1Source = 1;  // RtTextureArgSource::Texture
		blend_state.textureAlphaArg2Source = 0;  // RtTextureArgSource::None
		blend_state.textureAlphaOperation = 1;   // DxvkRtTextureOperation::SelectArg1
		blend_state.tFactor = 0xFFFFFFFFu;
		blend_state.isTextureFactorBlend = 0;
		blend_state.isVertexColorBakedLighting = 1;

		// ONE/ZERO with ADD is the runtime's "Opaque Alias" (rtx_instance_manager.cpp:717-719):
		// the pair that means "this draw is not really blended". Correct rest state for the
		// blend-disabled majority, and correct fallback for anything the mapping refuses.
		blend_state.alphaBlendEnabled = 0;
		blend_state.srcColorBlendFactor = 1;
		blend_state.dstColorBlendFactor = 0;
		blend_state.colorBlendOp = 0;
		blend_state.srcAlphaBlendFactor = 1;
		blend_state.dstAlphaBlendFactor = 0;
		blend_state.alphaBlendOp = 0;
		blend_state.writeMask = 0xFu;            // R|G|B|A, VkColorComponentFlags
		blend_state.alphaTestEnabled = 0;
		blend_state.alphaTestReferenceValue = 0;
		blend_state.alphaTestCompareOp = 7;      // VK_COMPARE_OP_ALWAYS, i.e. no test

		// The material now sets useDrawCallAlphaState = 1, and that one option gates alpha test
		// and alpha blend together (rtx_instance_manager.cpp:687-709), so the alpha test has to
		// be restated here or the cutout fix that put it on the material would be undone. Same
		// RSX-register source and same 0x200 subtraction the texture cache used, just read per
		// draw instead of once per texture descriptor - which is the more honest scope for it.
		// Note alphaTestEnabled itself is dead weight in the RT path (it lands on
		// LegacyMaterialData::alphaTestEnabled, which only the rasteriser reads); what actually
		// turns the test on is alphaTestCompareOp != 7 at rtx_instance_manager.cpp:680.
		if (!remix_rsx::alpha_state_disabled() && rsx::method_registers.alpha_test_enabled())
		{
			const u32 func = static_cast<u32>(rsx::method_registers.alpha_func());

			if (func >= 0x200 && func <= 0x207)
			{
				blend_state.alphaTestCompareOp = func - 0x200;
			}

			blend_state.alphaTestReferenceValue =
				static_cast<uint8_t>(std::clamp(rsx::method_registers.alpha_ref(), 0.f, 1.f) * 255.f + 0.5f);
			blend_state.alphaTestEnabled = 1;
		}

		if (!remix_rsx::alpha_state_disabled() && rsx::method_registers.blend_enabled())
		{
			bool mapped = true;

			blend_state.alphaBlendEnabled = 1;
			blend_state.srcColorBlendFactor = vk_blend_factor_from_gcm(rsx::method_registers.blend_func_sfactor_rgb(), mapped);
			blend_state.dstColorBlendFactor = vk_blend_factor_from_gcm(rsx::method_registers.blend_func_dfactor_rgb(), mapped);
			blend_state.colorBlendOp = vk_blend_op_from_gcm(rsx::method_registers.blend_equation_rgb(), mapped);
			blend_state.srcAlphaBlendFactor = vk_blend_factor_from_gcm(rsx::method_registers.blend_func_sfactor_a(), mapped);
			blend_state.dstAlphaBlendFactor = vk_blend_factor_from_gcm(rsx::method_registers.blend_func_dfactor_a(), mapped);
			blend_state.alphaBlendOp = vk_blend_op_from_gcm(rsx::method_registers.blend_equation_a(), mapped);

			// Surface 0 only: this backend submits one instance per draw, and the alpha bit is
			// the only one the runtime reads - it uses "alpha writes disabled" to tell
			// premultiplied alpha from inverted reverse-emissive when the colour pair is
			// ONE / ONE_MINUS_SRC_ALPHA (rtx_instance_manager.cpp:745-752).
			blend_state.writeMask =
				(rsx::method_registers.color_mask_r(0) ? 0x1u : 0u) |
				(rsx::method_registers.color_mask_g(0) ? 0x2u : 0u) |
				(rsx::method_registers.color_mask_b(0) ? 0x4u : 0u) |
				(rsx::method_registers.color_mask_a(0) ? 0x8u : 0u);

			if (mapped)
			{
				++m_stats.blend_translucent;
			}
			else
			{
				// Refuse the pair rather than ship half of it: a factor the mapping does not know
				// would otherwise land on whatever arm of calculateAlphaState()'s table the
				// fallback happened to hit, which is a worse failure than staying opaque.
				blend_state.srcColorBlendFactor = 1;
				blend_state.dstColorBlendFactor = 0;
				blend_state.colorBlendOp = 0;
				blend_state.srcAlphaBlendFactor = 1;
				blend_state.dstAlphaBlendFactor = 0;
				blend_state.alphaBlendOp = 0;
				blend_state.alphaBlendEnabled = 0;
				++m_stats.blend_unmapped;
			}
		}

		blend_state.pNext = instance.pNext;
		instance.pNext = &blend_state;
		++m_stats.blend_chained;
	}

	// Numbered last, so the value identifies a draw that actually reached DrawInstance and the
	// table cannot name geometry that was refused somewhere above. Declared at this scope
	// because the runtime reads the chain during the call below.
	remixapi_InstanceInfoObjectPickingEXT picking{};

	if (remix_rsx::pick_enabled() && m_pick_table.size() < s_max_pick_records)
	{
		pick_record record{};
		record.vp_hash = m_current_vp_hash;
		record.albedo_hash = albedo_hash;
		record.vertex_count = vertex_count;
		record.extent = m_streak_extent;
		record.sky = is_sky;
		record.viewmodel = is_viewmodel;
		record.skinned = skinned;

		if (m_current_fingerprint)
		{
			record.archetype = static_cast<u8>(m_current_fingerprint->archetype);
			record.has_prescale = m_current_fingerprint->has_prescale;
			record.has_const_affine = m_current_fingerprint->has_const_affine;
			record.affine_reason = m_current_fingerprint->affine_reason;
		}

		m_pick_table.push_back(record);

		picking.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT;

		// 1-based: the G-buffer clears to 0, so 0 has to stay "no draw here" and cannot be a
		// legitimate index.
		picking.objectPickingValue = static_cast<u32>(m_pick_table.size());
		picking.pNext = instance.pNext;
		instance.pNext = &picking;
	}

	const u32 status = remix_rsx::guarded_draw_instance(api.DrawInstance, &instance);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS)
	{
		m_poisoned.insert(hash);
		++m_stats.skip_poisoned;
		return;
	}

	++m_stats.draws_submitted;

	// The draw survived every gate and is now in the scene. If it was flagged as geometrically
	// incoherent back at the decode, this is the counter that says the artifact is actually being
	// rendered rather than being discarded later for an unrelated reason.
	if (m_vertex_flagged)
	{
		++m_stats.vtx_spread_submitted;
	}

	if (skinned)
	{
		++m_stats.skin_submitted;
	}

	// Drawn, not merely measured. Both of these are read at the far side of DrawInstance for the
	// reason the header gives: wext_examined sits before the mesh, the material and DrawInstance,
	// any of which can still drop the draw, so it cannot be read as a drawn count. These can.
	//
	// wext_drawn is the only histogram of the geometry that actually reached the scene; a streak is
	// a decade that has no business being occupied, and it is readable without arming anything.
	// wext_flagged_drawn is the A/B: over the ratio and rendered anyway because the gate was off.
	if (m_streak_measured)
	{
		u32 decade = 0;

		for (f32 limit = 1.f; decade < 7 && m_streak_extent >= limit; limit *= 10.f)
		{
			++decade;
		}

		++m_stats.wext_drawn[decade];
		m_stats.wext_drawn_max = std::max(m_stats.wext_drawn_max, m_streak_extent);
	}

	if (m_streak_flagged)
	{
		++m_stats.wext_flagged_drawn;
	}
}

const remix_rsx::vp_fingerprint& RemixGSRender::fingerprint_for(u64 vp_hash)
{
	auto it = m_vp_fingerprints.find(vp_hash);

	if (it == m_vp_fingerprints.end())
	{
		it = m_vp_fingerprints.emplace(vp_hash, remix_rsx::scan_vertex_program(current_vertex_program)).first;

		// Counted here rather than in the scan: the map is what makes it once per unique program.
		m_stats.vp_hpos_indirect += it->second.hpos_indirect ? 1 : 0;
		m_stats.vp_hpos_refused += it->second.hpos_indirect_refused ? 1 : 0;
		m_stats.vp_hpos_indexed += it->second.hpos_indirect_indexed ? 1 : 0;
		m_stats.vp_wbuffer_z += it->second.hpos_wbuffer_z ? 1 : 0;

		// The indexed population split by program rather than by draw: how many of the programs
		// that read a constant palette this file can now express, and how many it still cannot.
		// The second number is the honest size of what is left. It was 16 at ae94587 - the four-bone
		// blend rigs - and those are now expressed (match_blend_palette), so on Resistance 2 it
		// should be the 4 remaining oddities the paper replay listed: 2 that index a single
		// non-matrix row off a constant, 1 with three ARLs and branches, 1 reading two unrelated
		// indexed slots. Larger than 4 and there is another shape to find.
		if (it->second.indexed_const)
		{
			if (it->second.skinned && it->second.bone_resolved && !it->second.skin_unrecognised)
			{
				++m_stats.vp_indexed_matched;
			}
			else
			{
				++m_stats.vp_indexed_unmatched;
			}
		}
	}

	return it->second;
}

const remix_rsx::fp_fingerprint& RemixGSRender::fp_fingerprint_for(u64 fp_hash)
{
	auto it = m_fp_fingerprints.find(fp_hash);

	if (it == m_fp_fingerprints.end())
	{
		const bool fp32_outputs = (rsx::method_registers.shader_control() & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) != 0;

		it = m_fp_fingerprints.emplace(fp_hash, remix_rsx::scan_fragment_program(
			current_fragment_program.get_data(), current_fragment_program.ucode_length, fp32_outputs)).first;

		// One line per fragment program, once. This is the only instrument that can say whether the
		// colour-source walk is following a title's shaders: 'sampled=0x3 colour=0x1' is the
		// two-unit material this exists for, 'sampled=0x3 colour=0x3' is a program that told us
		// nothing, and 'colour=0x0' is one the walk could not follow at all.
		if (remix_rsx::dump_enabled() && m_fp_dumped.insert(fp_hash).second)
		{
			const remix_rsx::fp_fingerprint& fp = it->second;

			rsx_log.notice("Remix fpdump fp=%016llx sampled=0x%x colour=0x%x ref=0x%x instrs=%u fp32=%u alpha_only=%u flow=%u trunc=%u (%s)",
				fp_hash, u32{fp.sampled_mask}, u32{fp.colour_mask},
				u32{current_fp_metadata.referenced_textures_mask}, fp.instructions,
				fp32_outputs ? 1 : 0, fp.alpha_only ? 1 : 0, fp.has_flow ? 1 : 0, fp.truncated ? 1 : 0,
				fp.note);
		}
	}

	return it->second;
}

std::string RemixGSRender::describe_skinning(u32 first_vertex, u32 vertex_count)
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	std::string out = " | skinval";

	attribute_view bones{};
	const attribute_status status = map_attribute(fp.bone_attribute, first_vertex, vertex_count, bones);

	if (status != attribute_status::ok)
	{
		const char* reason = "absent";

		switch (status)
		{
		case attribute_status::layout: reason = "layout"; break;
		case attribute_status::memory: reason = "memory"; break;
		default: break;
		}

		fmt::append(out, " attr=ATTR%u UNMAPPED(%s)", fp.bone_attribute, reason);
		return out;
	}

	// 'idx=' is how the ARL reads the attribute, not just which component: the abs and negate bits
	// do not appear in the position slice (describe_output_slice prints neither), so without this
	// the one thing that decides which palette entry a draw reads is invisible in a capture.
	fmt::append(out, " attr=ATTR%u.%c idx=%s%s%c%s type=%u size=%u stride=%u off=%u first_vertex=%u verts=%u",
		fp.bone_attribute,
		"xyzw"[fp.bone_component & 3],
		fp.bone_index_negate ? "-" : "",
		fp.bone_index_abs ? "|" : "",
		"xyzw"[fp.bone_component & 3],
		fp.bone_index_abs ? "|" : "",
		static_cast<u32>(bones.type),
		bones.size,
		bones.stride,
		bones.offset,
		first_vertex,
		vertex_count);

	// The blend, decoded exactly the way build_blend_skinning does it: for the first few vertices,
	// every bone's weight and the palette slot it resolved to, side by side. That pairing is the
	// thing a wrong blend shows up in first - a swapped weight-to-address mapping gives plausible
	// slots with implausible weights on them - and it is not derivable from any of the aggregate
	// counters. The same shape of line is what caught the ARL modifier bug in one read.
	if (fp.skin_blended)
	{
		attribute_view blend_weights{};
		const attribute_status weight_status = map_attribute(fp.blend_weight_attribute, first_vertex, vertex_count, blend_weights);

		fmt::append(out, " | blend bones=%u wattr=ATTR%u(%s)",
			fp.blend_bones, fp.blend_weight_attribute,
			(weight_status == attribute_status::ok) ? "ok" : "UNMAPPED");

		if (weight_status == attribute_status::ok)
		{
			u32 blend_decode_failed = 0;
			u32 blend_slot_failed = 0;
			u32 weight_unsummed = 0;
			std::string blend_samples;

			for (u32 i = 0; i < vertex_count; ++i)
			{
				f32 index_scaled[4] = {};
				f32 index_raw[4] = {};
				f32 weight_scaled[4] = {};

				if (!remix_rsx::decode_position(bones.at(i), bones.type, bones.size, index_scaled)
					|| !remix_rsx::decode_attribute_raw(bones.at(i), bones.type, bones.size, index_raw)
					|| !remix_rsx::decode_position(blend_weights.at(i), blend_weights.type, blend_weights.size, weight_scaled))
				{
					++blend_decode_failed;
					continue;
				}

				f32 sum = 0.f;
				std::string per_bone;

				for (u32 k = 0; k < fp.blend_bones && k < remix_rsx::max_blend_bones; ++k)
				{
					const remix_rsx::bone_index_chain& chain = fp.blend_bone[k];
					const f32 weight = weight_scaled[fp.blend_weight_component[k] & 3];
					const f32 index_value = remix_rsx::skinraw_enabled()
						? index_raw[chain.component & 3]
						: index_scaled[chain.component & 3];

					sum += weight;

					u32 slot = 0;
					const bool slot_ok = remix_rsx::evaluate_palette_slot(fp, chain, index_value, slot);

					if (!slot_ok)
					{
						++blend_slot_failed;
					}

					if (i < 4)
					{
						fmt::append(per_bone, " b%u[w=%.4g idx=%.6g->%s%u]",
							k, static_cast<f64>(weight), static_cast<f64>(index_value),
							slot_ok ? "c" : "!c", slot);
					}
				}

				// The number Remix's shader actually depends on: it derives the last weight as
				// 1 - sum(the others), so a set that does not sum to 1 is submitted with a
				// different fourth bone than the ucode blended.
				if (std::abs(sum - 1.f) > (1.f / 32.f))
				{
					++weight_unsummed;
				}

				if (i < 4)
				{
					fmt::append(blend_samples, " v%u(sum=%.4g)%s", i, static_cast<f64>(sum), per_bone);
				}
			}

			fmt::append(out, " decode_fail=%u slot_fail=%u unsummed=%u/%u%s",
				blend_decode_failed, blend_slot_failed, weight_unsummed, vertex_count, blend_samples);

			// Every distinct bone this draw touches, assembled exactly as build_blend_skinning
			// assembles it, with the two numbers no other gate looks at: the longest basis axis and
			// the translation length.
			//
			// The whole draw is scanned, and the extremes are reported by slot. The first version of
			// this stopped collecting once it had 24 distinct bones, which is precisely the wrong
			// place to stop: a rig with more bones than that would hide the outlier the line exists
			// to find, and the two Resistance 2 rigs that look like real skeletons have exactly 24.
			// The per-bone list is still truncated - only the scan is not.
			{
				std::vector<u32> bone_slots;
				std::string extents;

				remix_rsx::mat4 first_bone{};
				bool have_first = false;
				bool uniform = true;

				f32 axis_min = 0.f, axis_max = 0.f, trans_min = 0.f, trans_max = 0.f;
				u32 axis_min_slot = 0, axis_max_slot = 0, trans_min_slot = 0, trans_max_slot = 0;

				for (u32 i = 0; i < vertex_count; ++i)
				{
					f32 index_scaled[4] = {};
					f32 index_raw[4] = {};
					f32 weight_scaled[4] = {};

					if (!remix_rsx::decode_position(bones.at(i), bones.type, bones.size, index_scaled)
						|| !remix_rsx::decode_attribute_raw(bones.at(i), bones.type, bones.size, index_raw)
						|| !remix_rsx::decode_position(blend_weights.at(i), blend_weights.type, blend_weights.size, weight_scaled))
					{
						continue;
					}

					for (u32 k = 0; k < fp.blend_bones && k < remix_rsx::max_blend_bones; ++k)
					{
						const remix_rsx::bone_index_chain& chain = fp.blend_bone[k];

						if (!(weight_scaled[fp.blend_weight_component[k] & 3] > 0.f))
						{
							continue;
						}

						const f32 index_value = remix_rsx::skinraw_enabled()
							? index_raw[chain.component & 3]
							: index_scaled[chain.component & 3];

						u32 slot = 0;

						if (!remix_rsx::evaluate_palette_slot(fp, chain, index_value, slot)
							|| std::find(bone_slots.begin(), bone_slots.end(), slot) != bone_slots.end())
						{
							continue;
						}

						bone_slots.push_back(slot);

						remix_rsx::mat4 bone{};

						if (!remix_rsx::build_palette_matrix(fp, slot, bone))
						{
							if (bone_slots.size() <= 12)
							{
								fmt::append(extents, " c%u=UNBUILDABLE", slot);
							}

							uniform = false;
							continue;
						}

						const f32 axis = remix_rsx::basis_extent(bone);
						const f32 trans = remix_rsx::translation_extent(bone);

						if (!have_first)
						{
							first_bone = bone;
							have_first = true;
							axis_min = axis_max = axis;
							trans_min = trans_max = trans;
							axis_min_slot = axis_max_slot = trans_min_slot = trans_max_slot = slot;
						}
						else
						{
							if (axis < axis_min) { axis_min = axis; axis_min_slot = slot; }
							if (axis > axis_max) { axis_max = axis; axis_max_slot = slot; }
							if (trans < trans_min) { trans_min = trans; trans_min_slot = slot; }
							if (trans > trans_max) { trans_max = trans; trans_max_slot = slot; }

							for (u32 r = 0; r < 4 && uniform; ++r)
							{
								for (u32 c = 0; c < 4; ++c)
								{
									if (std::abs(bone.m[r][c] - first_bone.m[r][c]) > 1e-6f)
									{
										uniform = false;
										break;
									}
								}
							}
						}

						if (bone_slots.size() <= 12)
						{
							fmt::append(extents, " c%u[axis=%.4g trans=%.4g%s]",
								slot,
								static_cast<f64>(axis),
								static_cast<f64>(trans),
								remix_rsx::has_usable_basis(bone, s_bone_basis_tolerance) ? "" : " RANK");
						}
					}
				}

				// 'uniform' is the shape no rig has: every bone of the palette the same matrix. It
				// is reported rather than refused - see build_blend_skinning for why an identical
				// palette is a mathematically *correct* rigid draw - but it is the signature of a
				// palette that was never written, so it is the first thing to look for.
				fmt::append(out, " | bones=%u uniform=%d axis[%.4g@c%u..%.4g@c%u] trans[%.4g@c%u..%.4g@c%u]%s",
					::size32(bone_slots), (have_first && uniform) ? 1 : 0,
					static_cast<f64>(axis_min), axis_min_slot, static_cast<f64>(axis_max), axis_max_slot,
					static_cast<f64>(trans_min), trans_min_slot, static_cast<f64>(trans_max), trans_max_slot,
					extents);

				// The palette as the ucode actually reads it, before build_palette_matrix folds the
				// bias in and before the decode is composed onto it. This is what separates "the
				// constants were never written" from "we are reading the wrong place": an identity
				// row set says the title has not posed this rig, a plausible rotation with a zero
				// fourth component says the translation lives somewhere this does not look, and
				// garbage says the base is wrong. Two bones is enough to tell those apart, and the
				// bias is printed beside them because for a 3-row group it is the entire translation
				// of every bone when the palette's own fourth components are zero.
				for (u32 b = 0; b < 2 && b < bone_slots.size(); ++b)
				{
					remix_rsx::slot_block raw{};

					if (!remix_rsx::read_slot_block(bone_slots[b], raw))
					{
						fmt::append(out, " | raw c%u UNREADABLE", bone_slots[b]);
						continue;
					}

					fmt::append(out, " | raw c%u=[%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g]",
						bone_slots[b],
						static_cast<f64>(raw.v[0][0]), static_cast<f64>(raw.v[0][1]), static_cast<f64>(raw.v[0][2]), static_cast<f64>(raw.v[0][3]),
						static_cast<f64>(raw.v[1][0]), static_cast<f64>(raw.v[1][1]), static_cast<f64>(raw.v[1][2]), static_cast<f64>(raw.v[1][3]),
						static_cast<f64>(raw.v[2][0]), static_cast<f64>(raw.v[2][1]), static_cast<f64>(raw.v[2][2]), static_cast<f64>(raw.v[2][3]));
				}

				if (fp.palette_has_bias)
				{
					if (remix_rsx::slot_block bias{}; remix_rsx::read_slot_block(fp.palette_bias_slot, bias))
					{
						fmt::append(out, " | bias c%u=[%.4g %.4g %.4g]", fp.palette_bias_slot,
							static_cast<f64>(bias.v[0][0]), static_cast<f64>(bias.v[0][1]), static_cast<f64>(bias.v[0][2]));
					}
				}
			}

			// This runs before the skinning gate, so m_scratch_bone_prescale_folded is still clear
			// and the 'world=' matrix printed at the end of this line still carries the position
			// decode. On the real submission the decode travels in front of the bone matrices
			// instead - see build_blend_skinning - so the two differ by exactly that factor.
			if (fp.has_prescale || fp.has_const_affine)
			{
				out += " (world below still carries the decode; the submitted bones carry it instead)";
			}
		}
	}

	// Decoded per vertex exactly the way build_skinning does it, with both candidate values
	// carried side by side so the raw-vs-scaled question (M4 D1) is answered by reading the
	// line rather than by another run.
	std::vector<u32> slots;
	u32 decode_failed = 0;
	u32 scaled_failed = 0;
	u32 raw_failed = 0;
	std::string samples;

	for (u32 i = 0; i < vertex_count; ++i)
	{
		f32 scaled[4] = {};
		f32 raw[4] = {};

		if (!remix_rsx::decode_position(bones.at(i), bones.type, bones.size, scaled) ||
			!remix_rsx::decode_attribute_raw(bones.at(i), bones.type, bones.size, raw))
		{
			++decode_failed;
			continue;
		}

		// The absolute palette slot, which is what build_skinning and resolve_indexed_world both
		// key on now - printing the offset would no longer describe the code being diagnosed.
		u32 scaled_offset = 0;
		u32 raw_offset = 0;
		const bool scaled_ok = remix_rsx::evaluate_palette_slot(fp, scaled[fp.bone_component], scaled_offset);
		const bool raw_ok = remix_rsx::evaluate_palette_slot(fp, raw[fp.bone_component], raw_offset);

		if (!scaled_ok) { ++scaled_failed; }
		if (!raw_ok) { ++raw_failed; }

		u32 dense = umax;

		if (scaled_ok)
		{
			for (u32 s = 0; s < ::size32(slots); ++s)
			{
				if (slots[s] == scaled_offset)
				{
					dense = s;
					break;
				}
			}

			if (dense == umax && slots.size() < REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT)
			{
				dense = ::size32(slots);
				slots.push_back(scaled_offset);
			}
		}

		if (i < 8)
		{
			fmt::append(samples, " v%u[raw=%.6g->%s%u scaled=%.6g->%s%u dense=%d]",
				i,
				static_cast<f64>(raw[fp.bone_component]),
				raw_ok ? "" : "!",
				raw_offset,
				static_cast<f64>(scaled[fp.bone_component]),
				scaled_ok ? "" : "!",
				scaled_offset,
				(dense == umax) ? -1 : static_cast<int>(dense));
		}
	}

	// 'distinct' is the whole rigid-vs-skinned question in one number: 1 means this draw reads one
	// object matrix and folds into the instance transform, more than 1 means it is a real rig.
	fmt::append(out, " base=c%u rows=%u stride=%u bias=%d distinct=%u decode_fail=%u scaled_fail=%u raw_fail=%u%s",
		fp.palette_base, fp.palette_rows, fp.palette_stride, fp.palette_has_bias ? 1 : 0,
		::size32(slots), decode_failed, scaled_failed, raw_failed, samples);

	// The palette itself. A bind pose is affine with plausible translations; a transpose puts
	// the translation in the perspective row, which is exactly what this prints. Built the way the
	// submit path builds it - rows the group actually supplies, the post-matrix translation folded
	// in - so a matrix that looks wrong here is wrong in the draw too.
	if (!slots.empty())
	{
		remix_rsx::mat4 bone{};

		if (remix_rsx::build_palette_matrix(fp, slots[0], bone))
		{
			fmt::append(out, " | bone0 c[%u] mat=%s affine=%d",
				slots[0],
				remix_rsx::format_matrix(bone),
				remix_rsx::is_affine(bone, s_world_affine_tolerance) ? 1 : 0);
		}
		else
		{
			fmt::append(out, " | bone0 c[%u] UNBUILDABLE", slots[0]);
		}
	}

	// The instance transform this draw would be submitted with: the suspect for a character
	// standing in the wrong place (per_draw_transform's skinned branch).
	remixapi_Transform world{};
	const bool have_world = per_draw_transform(world);

	fmt::append(out, " | world applied=%d groups=%u [%.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g]",
		have_world ? 1 : 0,
		fp.group_count,
		static_cast<f64>(world.matrix[0][0]), static_cast<f64>(world.matrix[0][1]), static_cast<f64>(world.matrix[0][2]), static_cast<f64>(world.matrix[0][3]),
		static_cast<f64>(world.matrix[1][0]), static_cast<f64>(world.matrix[1][1]), static_cast<f64>(world.matrix[1][2]), static_cast<f64>(world.matrix[1][3]),
		static_cast<f64>(world.matrix[2][0]), static_cast<f64>(world.matrix[2][1]), static_cast<f64>(world.matrix[2][2]), static_cast<f64>(world.matrix[2][3]));

	return out;
}

void RemixGSRender::dump_vertex_program(u32 first_vertex, u32 vertex_count, u32 index_count)
{
	const u64 vp_hash = m_current_vp_hash;
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	if (!m_vp_dumped.insert(vp_hash).second)
	{
		return;
	}

	// ATTR0 bounding box, straight off the positions we just decoded.
	f32 lo[3] = { +3.4e38f, +3.4e38f, +3.4e38f };
	f32 hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };

	for (const auto& v : m_scratch_vertices)
	{
		for (u32 c = 0; c < 3; ++c)
		{
			lo[c] = std::min(lo[c], v.position[c]);
			hi[c] = std::max(hi[c], v.position[c]);
		}
	}

	std::string groups;

	for (u32 i = 0; i < fp.group_count; ++i)
	{
		remix_rsx::slot_block slots{};
		if (!remix_rsx::read_slot_block(fp.group_base[i], slots))
		{
			continue;
		}

		const remix_rsx::mat4 g = remix_rsx::slots_to_matrix(slots, fp.group_shape[i]);
		fmt::append(groups, " G%u c[%u..%u] %s %s persp=%d",
			i, fp.group_base[i], fp.group_base[i] + 3,
			remix_rsx::shape_name(fp.group_shape[i]),
			remix_rsx::format_matrix(g),
			remix_rsx::classify_perspective(g));
	}

	if (fp.skinned)
	{
		// The line that settles the open question: which attribute carries the bone index,
		// which component of it, and what is applied on the way to the address register.
		fmt::append(groups, " | skin palette=c[%u+a] shape=%s attr=ATTR%u.%c resolved=%d ops=%u arl=%u idx=%u foreign=%u unrecognised=%d blend=%u",
			fp.palette_base,
			remix_rsx::shape_name(fp.palette_shape),
			fp.bone_attribute,
			"xyzw"[fp.bone_component & 3],
			fp.bone_resolved ? 1 : 0,
			fp.bone_op_count,
			fp.arl_count,
			fp.indexed_reads,
			fp.foreign_indexed_reads,
			fp.skin_unrecognised ? 1 : 0,
			// 0 for a single-matrix palette, 4 for the summed four-bone form. 'idx' should be
			// rows x blend for a blend rig - 12 for R2's three-row palettes - and 'foreign' 0:
			// that pair is the whole indexing audit in two numbers.
			fp.skin_blended ? fp.blend_bones : 0);

		for (u32 i = 0; i < fp.bone_op_count; ++i)
		{
			const remix_rsx::bone_index_op& op = fp.bone_ops[i];

			switch (op.op)
			{
			case remix_rsx::bone_index_op::kind::floor:
				groups += " floor";
				break;
			case remix_rsx::bone_index_op::kind::scale:
				fmt::append(groups, " *c%u.%c", op.mul_slot, "xyzw"[op.mul_component & 3]);
				break;
			case remix_rsx::bone_index_op::kind::affine:
				if (op.mul_slot == umax)
				{
					fmt::append(groups, " +c%u.%c", op.add_slot, "xyzw"[op.add_component & 3]);
				}
				else
				{
					fmt::append(groups, " *c%u.%c+c%u.%c", op.mul_slot, "xyzw"[op.mul_component & 3],
						op.add_slot, "xyzw"[op.add_component & 3]);
				}
				break;
			}
		}

		if (fp.skin_note[0])
		{
			fmt::append(groups, " (%s)", fp.skin_note);
		}

		if (fp.archetype == remix_rsx::vp_archetype::skinned_layered)
		{
			groups += describe_skinning(first_vertex, vertex_count);
		}
	}

	// ATTR0's stored shape. A four-component position is the precondition for the per-vertex
	// divide, so this is the line that says whether has_wdivide could ever have been right.
	if (attribute_view attr0{}; map_attribute(0, first_vertex, vertex_count, attr0) == attribute_status::ok)
	{
		fmt::append(groups, " | attr0 type=%u size=%u stride=%u wdiv=%d",
			static_cast<u32>(attr0.type), attr0.size, attr0.stride, fp.has_wdivide ? 1 : 0);

		f32 w_lo = +3.4e38f;
		f32 w_hi = -3.4e38f;

		for (u32 i = 0; i < vertex_count; ++i)
		{
			f32 decoded[4] = {};

			if (remix_rsx::decode_position(attr0.at(i), attr0.type, attr0.size, decoded))
			{
				w_lo = std::min(w_lo, decoded[3]);
				w_hi = std::max(w_hi, decoded[3]);
			}
		}

		fmt::append(groups, " w=[%.6g..%.6g]", static_cast<f64>(w_lo), static_cast<f64>(w_hi));
	}

	// The ucode is the only thing that can say why identification failed - and, for a program
	// that indexes constants, whether the rig blends several bones per vertex at all. Those get
	// a longer slice: the palette read sits below the outer matrix, past the default cut.
	if (fp.archetype == remix_rsx::vp_archetype::unknown || fp.skinned || fp.indexed_const || !fp.inner_is_input)
	{
		fmt::append(groups, " slice:%s",
			remix_rsx::describe_position_slice(current_vertex_program, (fp.indexed_const || !fp.inner_is_input) ? 96 : 32));
	}

	// The texcoord half of the same argument. We submit the raw texcoord attribute and never run
	// the ucode, so a program that scales, biases or negates it on the way to TEX0 has that
	// operation dropped - the position's w-divide trap (e9a7956) applied to UVs. R2 renders every
	// texture vertically mirrored on the first build that ever applied UVs at all, and the flip
	// is not in the upload or the V convention (both are exercised correctly by Haze's menu text
	// through composite_ui_draw, commit 2414498), which leaves the ucode. This slice is what names
	// the instruction; RPCS3_REMIX_UVFLIPV is the stopgap until it does.
	//
	// Only TEX0 and only for programs that write it: a per-unit sweep would multiply the dump for
	// a question about the diffuse coordinates.
	if (const std::string tc0 = remix_rsx::describe_output_slice(current_vertex_program, 7, 32);
		tc0 != " <not written>")
	{
		fmt::append(groups, " tc0:%s", tc0);

		// Live values of every constant slot the TEX0 slice reads. resolve_output_input refuses a
		// program that scales its texcoord before writing o7, and on Haze 17 of the 25 refusals
		// carry the same shape - MUL>rN.xy(I0.xyxx, C0.xxxx, ...)c151i8, i.e. attr8 times slot 151.
		// The heuristic then supplies attr8 raw and the scale is dropped, which is the tiling.
		// UVINTSCALE's own doc already concedes the point ("whose real divisor is a vertex-program
		// constant this backend does not read"); this prints the constant so the 4096 stand-in can
		// be checked against it rather than assumed. Parsed back out of the slice string because
		// that is where the slot numbers already are - a diagnostic, not a decode path.
		{
			std::string slot_values;
			std::unordered_set<u32> seen_slots;

			for (usz p = tc0.find(")c"); p != std::string::npos; p = tc0.find(")c", p + 1))
			{
				u32 slot = 0;
				usz d = p + 2;

				for (; d < tc0.size() && tc0[d] >= '0' && tc0[d] <= '9'; ++d)
				{
					slot = (slot * 10) + static_cast<u32>(tc0[d] - '0');
				}

				// Slot 0 is the "no constant" filler in the slice format, not a real read.
				if (d == p + 2 || slot == 0 || !seen_slots.insert(slot).second)
				{
					continue;
				}

				if (f32 v[4]{}; remix_rsx::read_slot(slot, v))
				{
					fmt::append(slot_values, " c%u=[%g %g %g %g]", slot, v[0], v[1], v[2], v[3]);
				}
			}

			if (!slot_values.empty())
			{
				fmt::append(groups, " tc0consts:%s", slot_values);
			}
		}
	}

	// What the two ucode readers made of this program: the attribute each texcoord output takes its
	// xy from (-1 = the write could not be reduced to one attribute read straight), and the constant
	// scale/bias the position decode resolved to. Printed next to the slices they were derived from
	// so a wrong pick is one line to diagnose rather than a rebuild.
	{
		std::string tc_inputs;

		for (u32 unit = 0; unit < 8; ++unit)
		{
			if (fp.texcoord_input[unit] != remix_rsx::s_no_texcoord_input)
			{
				fmt::append(tc_inputs, " tc%u<-a%u", unit, u32{fp.texcoord_input[unit]});
			}
		}

		if (!tc_inputs.empty())
		{
			fmt::append(groups, " uvsrc:%s", tc_inputs);
		}
	}

	if (fp.has_const_affine)
	{
		fmt::append(groups, " affine=");

		if (fp.affine_has_scale)
		{
			fmt::append(groups, "*c%u.%c%c%c", fp.affine_scale_slot,
				"xyzw"[fp.affine_scale_component[0] & 3],
				"xyzw"[fp.affine_scale_component[1] & 3],
				"xyzw"[fp.affine_scale_component[2] & 3]);
		}

		if (fp.affine_has_bias)
		{
			fmt::append(groups, "+c%u.xyz", fp.affine_bias_slot);
		}

		if (remix_rsx::mat4 built{}; remix_rsx::build_prescale(fp, built))
		{
			fmt::append(groups, "=%s", remix_rsx::format_matrix(built));
		}
		else
		{
			groups += "=<unbuildable>";
		}
	}

	// The Y half of the viewport transform, alongside the Z half fold_viewport_z already folds.
	// Nothing under Emu\RSX\Remix reads these two registers, and RSX viewport scale Y is commonly
	// negative for a top-left window origin - which is the last live candidate for the reported
	// whole-image vertical flip. Logged, not acted on: a global Y flip applied on a guess would
	// invert every title whose orientation is already right, exactly as the global V flip did.
	fmt::append(groups, " vp_scale_y=%.6g vp_offset_y=%.6g",
		static_cast<f64>(rsx::method_registers.viewport_scale_y()),
		static_cast<f64>(rsx::method_registers.viewport_offset_y()));

	// How many colour surfaces this draw writes. Nothing under Emu\RSX\Remix has ever read this
	// register, which means the backend cannot currently tell a deferred G-buffer fill from the
	// pass that produces the final image - it submits both. That is the leading explanation for
	// Resistance 2 (NPEA00431): its albedo debug view is vivid blue carrying correct surface
	// detail (i.e. normals written as colour), several of its fragment programs sample only
	// texture unit 1 and nothing else (fpdump sampled=0x2, which a lit surface never does but a
	// normal-only prepass does), geometry visibly overlaps itself, and draw time is 40.77 ms of a
	// 45.79 ms frame where it used to be present-bound. One mesh submitted once per pass explains
	// every one of those at once.
	//
	// Logged, not acted on. If the blue programs and the correct ones separate on mrt/ctarget then
	// skipping the fill pass is a small change; if they do not, this rules the theory out before
	// any code is written for it.
	{
		const auto target = rsx::method_registers.surface_color_target();

		u32 colour_surfaces = 0;
		switch (target)
		{
		case rsx::surface_target::none:             colour_surfaces = 0; break;
		case rsx::surface_target::surface_a:
		case rsx::surface_target::surface_b:        colour_surfaces = 1; break;
		case rsx::surface_target::surfaces_a_b:     colour_surfaces = 2; break;
		case rsx::surface_target::surfaces_a_b_c:   colour_surfaces = 3; break;
		case rsx::surface_target::surfaces_a_b_c_d: colour_surfaces = 4; break;
		}

		// The surface address as well: two passes over the same geometry writing different targets
		// is the signature, and the address is what says "different target" rather than just
		// "different count".
		fmt::append(groups, " mrt=%u ctarget=%u surf=0x%x depthfmt=%u",
			colour_surfaces,
			static_cast<u32>(target),
			rsx::method_registers.surface_offset(0),
			static_cast<u32>(rsx::method_registers.surface_depth_fmt()));
	}

	const std::string line = fmt::format(
		// 'basis' rides on the dump line rather than in per_draw_transform because the census that
		// composes the matrix only runs for draws that survive the indexed-const gate - and those
		// are exactly the draws a knob can switch off, which made the matcher's result unreadable
		// in the first run that tried it. This fires at fingerprint time, so it reports whether
		// match_basis_affine matched for every program regardless of what happens to the draw.
		// The affine reason is on this line as well as on 'world-refused', because that line is only
		// emitted for a draw the world-extent census actually refused. A program whose decode does
		// not match but which resolves a camera and passes the extent test never appears there, so
		// its 'prescale=0' had no reason attached anywhere - which is exactly the population this
		// instrument was added to explain. This line is per unique vertex program and unconditional.
		"Remix dump vp=%016llx arch=%s(%s) groups=%u input=%d prescale=%d(c%u.%u,c%u:%s) basis=%d(c%u,c%u,c%u:%s) consts=%u slice=%u ucode=%u inputs=0x%x | "
		"vtx=%u idx=%u prim=%u bbox=[%.4g %.4g %.4g]..[%.4g %.4g %.4g] | "
		"vp_scale_z=%.6g vp_offset_z=%.6g clip=%ux%u depth_test=%d depth_write=%d blend=%d cull=%d |%s",
		vp_hash,
		remix_rsx::archetype_name(fp.archetype),
		fp.note,
		fp.group_count,
		fp.inner_is_input ? 1 : 0,
		fp.has_prescale ? 1 : 0,
		fp.prescale_scale_slot,
		fp.prescale_scale_component,
		fp.prescale_bias_slot,
		fp.affine_reason,
		fp.has_basis_affine ? 1 : 0,
		fp.basis_row_slot[0],
		fp.basis_row_slot[1],
		fp.basis_scale_slot,
		fp.basis_reason,
		fp.distinct_consts,
		fp.chain_instructions,
		current_vp_metadata.ucode_length,
		u32{current_vp_metadata.referenced_inputs_mask},
		vertex_count,
		index_count,
		static_cast<u32>(rsx::method_registers.current_draw_clause.primitive),
		static_cast<f64>(lo[0]), static_cast<f64>(lo[1]), static_cast<f64>(lo[2]),
		static_cast<f64>(hi[0]), static_cast<f64>(hi[1]), static_cast<f64>(hi[2]),
		static_cast<f64>(rsx::method_registers.viewport_scale_z()),
		static_cast<f64>(rsx::method_registers.viewport_offset_z()),
		u32{rsx::method_registers.surface_clip_width()},
		u32{rsx::method_registers.surface_clip_height()},
		rsx::method_registers.depth_test_enabled() ? 1 : 0,
		rsx::method_registers.depth_write_enabled() ? 1 : 0,
		rsx::method_registers.blend_enabled() ? 1 : 0,
		rsx::method_registers.cull_face_enabled() ? 1 : 0,
		groups);

	rsx_log.notice("%s", line);

	// RPCS3.log is held with an exclusive lock while the emulator runs, so mirror the dump
	// into a file that can be read live.
	if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
	{
		out.write(line + '\n');
	}
}

void RemixGSRender::dump_texture(const remix_rsx::texture_entry& entry, const rsx::fragment_texture& tex, u32 unit)
{
	const std::string line = fmt::format(
		"Remix tex=%016llX fmt=%02x %ux%u unit=%u mips=%u swizzled=%d pitch=%u loc=%u offset=0x%x wrap=%u,%u alpha=%u/%u",
		entry.content_hash,
		u32{tex.format()} & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
		entry.width,
		entry.height,
		unit,
		u32{tex.get_exact_mipmap_count()},
		(tex.format() & CELL_GCM_TEXTURE_LN) ? 0 : 1,
		tex.pitch(),
		u32{tex.location()},
		tex.offset(),
		u32{entry.wrap_u},
		u32{entry.wrap_v},
		u32{entry.alpha_func},
		u32{entry.alpha_ref});

	rsx_log.notice("%s", line);

	if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
	{
		out.write(line + '\n');
	}

	// RPCS3_REMIX_TEXBMP=1: write the decoded pixels out as well, one BMP per unique texture, into
	// remix_tex\. Every colour theory tried on Resistance 2 (NPEA00431) has died on a counter -
	// the decode (a frame renders correctly at spawn), sky over-tagging (cat_sky=0 and still blue),
	// unit retry (tex_unit_retry=0), a stale cache (tex_rehashed=3 with full content hashing), and
	// a deferred G-buffer fill (every draw reports mrt=1 ctarget=1). What has never been done is
	// look at the image actually handed to Remix as albedo. If the texture bound for a blue surface
	// is a normal map, the fault is unit selection here; if it is the right diffuse map, the fault
	// is downstream and nothing in this file will fix it. Off by default because a level is ~700
	// unique textures.
	if (!remix_rsx::dump_texture_images())
	{
		return;
	}

	if (entry.pixels.size() < usz{entry.width} * entry.height * 4 || !entry.width || !entry.height)
	{
		return;
	}

	const std::string dir = fs::get_executable_dir() + "remix_tex/";
	fs::create_dir(dir);

	const u32 row_bytes = entry.width * 4;
	const u32 pixel_bytes = row_bytes * entry.height;

	// 32-bit BGRA, which is exactly how texture_cache::decode already stores it, so the bytes go
	// out untouched - a re-pack here could itself invert a channel and answer the wrong question.
	// Negative height makes it top-down, matching the decode's row order.
	u8 header[54]{};
	header[0] = 'B'; header[1] = 'M';
	*reinterpret_cast<u32*>(header + 2) = 54 + pixel_bytes;
	*reinterpret_cast<u32*>(header + 10) = 54;
	*reinterpret_cast<u32*>(header + 14) = 40;
	*reinterpret_cast<s32*>(header + 18) = static_cast<s32>(entry.width);
	*reinterpret_cast<s32*>(header + 22) = -static_cast<s32>(entry.height);
	*reinterpret_cast<u16*>(header + 26) = 1;
	*reinterpret_cast<u16*>(header + 28) = 32;
	*reinterpret_cast<u32*>(header + 34) = pixel_bytes;

	const std::string path = fmt::format("%sunit%u_%016llX_%ux%u.bmp",
		dir, unit, entry.content_hash, entry.width, entry.height);

	if (fs::file img{ path, fs::write + fs::create + fs::trunc })
	{
		img.write(header, sizeof(header));
		img.write(entry.pixels.data(), pixel_bytes);
	}
}

void RemixGSRender::reap_idle_meshes()
{
	if (m_meshes.empty())
	{
		return;
	}

	const auto& api = m_remix.api();

	const u64 idle_frames = remix_rsx::mesh_idle_frames();

	if (m_frame_counter >= idle_frames)
	{
		const u64 cutoff = m_frame_counter - idle_frames;

		for (auto it = m_meshes.begin(); it != m_meshes.end();)
		{
			if (it->second.last_used_frame > cutoff)
			{
				++it;
				continue;
			}

			if (it->second.handle)
			{
				remix_rsx::guarded_destroy_mesh(api.DestroyMesh, it->second.handle);
				++m_stats.meshes_destroyed;
			}

			it = m_meshes.erase(it);
		}
	}

	// RPCS3_REMIX_MESHCAP=N: hard ceiling with LRU eviction. The idle rule above is a *frame*
	// rule, so a title that mints a new mesh every frame grows the cache without bound for 300
	// frames - and grows it forever once flips stop, because the frame counter stops with them.
	// Off by default; this exists to A/B whether unbounded CreateMesh pressure is what stalls
	// the guest, and it is the fix if it is.
	const usz cap = remix_rsx::mesh_cap();

	if (!cap || m_meshes.size() <= cap)
	{
		return;
	}

	std::vector<std::pair<u64, u64>> by_age; // last_used_frame, hash
	by_age.reserve(m_meshes.size());

	for (const auto& [hash, entry] : m_meshes)
	{
		by_age.emplace_back(entry.last_used_frame, hash);
	}

	const usz excess = m_meshes.size() - cap;
	std::partial_sort(by_age.begin(), by_age.begin() + excess, by_age.end());

	for (usz i = 0; i < excess; i++)
	{
		auto it = m_meshes.find(by_age[i].second);

		if (it == m_meshes.end())
		{
			continue;
		}

		if (it->second.handle)
		{
			remix_rsx::guarded_destroy_mesh(api.DestroyMesh, it->second.handle);
			++m_stats.meshes_destroyed;
		}

		m_meshes.erase(it);
	}
}

void RemixGSRender::log_stats()
{
	// Flip count OR wall clock, whichever comes first. A fixed 120-flip interval reports once
	// a minute at 2 FPS and never at all if flips stop, which is exactly the regime that needs
	// watching, so the time bound is what makes this instrument usable during a stall.
	if (m_timing.frames < s_stats_interval_flips && m_timing.window < s_stats_interval_us)
	{
		return;
	}

	const remix_rsx::texture_stats& tex = m_textures.stats();

	rsx_log.notice(
		"Remix stats: frame=%llu draws=%llu submitted=%llu meshes_live=%llu created=%llu destroyed=%llu poisoned=%llu | "
		"cam_resolved=%llu cam_fallback=%llu cam_held=%llu split_attempted=%llu split_failed=%llu arch=%s world_applied=%llu world_fallback=%llu world_refused=%llu world_layered_ref=%llu | "
		"skip screen=%llu immediate=%llu inline=%llu volatile=%llu reg_attr0=%llu prim=%llu restart=%llu instanced=%llu layout=%llu mem=%llu decode=%llu poison=%llu vp=%llu rt=%llu rt_kept=%llu notinput=%llu wdiv=%llu posdecode_refused=%llu | "
		"vp_hpos_indirect=%llu vp_hpos_refused=%llu vp_hpos_indexed=%llu vp_wbuffer_z=%llu | "
		"idxworld_rigid=%llu idxworld_skinned=%llu idxworld_refused=%llu vp_indexed_matched=%llu vp_indexed_unmatched=%llu bone_degenerate=%llu | "
		"skin_submitted=%llu skin_skipped=%llu skin_unrecognised=%llu skin_bones_max=%llu "
		"skin_unrec_arl=%llu skin_unrec_foreign=%llu skin_unrec_reads=%llu skin_unrec_indexed=%llu skin_unrec_census=%u | "
		"idxuniform=%llu/%llu/%llu idxbias=%llu/%llu/%llu | "
		"skinblend_submitted=%llu skinblend_idx=%llu skinblend_weight=%llu skinblend_bone=%llu skinblend_palette=%llu skinblend_scale=%llu skinblend_uniform=%llu skinblend_rescaled=%llu skin_reach_flagged=%llu vtx_spread=%llu vtx_zero_split=%llu vtx_spread_submitted=%llu vtx_spread_refused=%llu | "
		"wext_examined=%llu wext_exempt=%llu wext_nonfinite=%llu wext_refused=%llu wext_flagged_drawn=%llu "
		"wext_median=%.6g wext_samples=%llu wext_max=%.6g wext_drawn=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu | "
		"skip_lighting_pass=%llu | "
		"cat_sky=%llu sky_cand=%llu sky_noworld=%llu sky_extent=%llu sky_anchor=%llu sky_anchor_held=%llu sky_census=%u "
		"sky_backdrop_hit=%llu sky_backdrop_dw=%llu sky_backdrop_mode=%u sky_learned_ring=%llu | "
		"skyhash_considered=%llu skyhash_dome=%llu skyhash_matched=%llu skyhash_tagged=%llu "
		"skyhash_rejected=%llu skyhash_tracked=%llu skyhash_census=%u skyhash_mode=%u | "
		"vm_tagged=%llu vm_considered=%llu vm_fullrange=%llu vm_offset=%llu vm_far=%llu vm_noanchor=%llu "
		"vm_census=%u vm_mode=%u | "
		"vmcam_considered=%llu vmcam_applied=%llu vmcam_fallback=%llu vmcam_refused=%llu "
		"vmcam_diverted=%llu vmcam_unusable=%llu vmcam_conflict=%llu vmcam_latched=%llu vmcam_held=%llu "
		"vmcam_census=%u vmcam_mode=%u | "
		"cat_hidden=%llu cat_particle=%llu cat_decal=%llu cat_smoothnormals=%llu | "
		"blend_chained=%llu blend_translucent=%llu blend_unmapped=%llu | "
		"tex_bound=%llu tex_none=%llu tex_no_unit=%llu tex_unit_retry=%llu tex_albedo_ucode=%llu tex_albedo_guess=%llu tex_retry_refused=%llu tex_unit_substituted=%llu uv_applied=%llu uv_none=%llu uv_absent=%llu uv_layout=%llu uv_memory=%llu uv_fallback=%llu uv_ucode=%llu uv_heuristic=%llu uv_nonfinite=%llu vcol_applied=%llu tex_live=%llu tex_created=%llu tex_destroyed=%llu tex_hits=%llu tex_deferred=%llu tex_unreadable=%llu tex_unsupported=%llu tex_tombstone=%llu tex_rehashed=%llu tex_refreshed=%llu mat_created=%llu tex_retry_unsupported=%llu mat_untested=%llu uv_scale_ucode=%llu uv_scale_fixed=%llu | "
		"ui_draws=%llu ui_skipped=%llu ui_no_colour=%llu ui_rt=%llu ui_prims=%llu ui_frames=%llu ui_ndc=%llu ui_unit=%llu ui_pixel=%llu ui_nospace=%llu ui_ortho2d=%llu "
		"ui_vpydown=%llu ui_vpyup=%llu ui_vpfallback=%llu ui_vflip_ndc=%llu/%llu ui_vflip_pixel=%llu/%llu ui_vflip_abstain=%llu | "
		"zcull_av=%llu zcull_av_handled=%llu",
		m_frame_counter,
		m_stats.draws_seen,
		m_stats.draws_submitted,
		static_cast<u64>(m_meshes.size()),
		m_stats.meshes_created,
		m_stats.meshes_destroyed,
		static_cast<u64>(m_poisoned.size()),
		m_stats.cam_resolved,
		m_stats.cam_fallback,
		m_stats.cam_held,
		m_stats.split_attempted,
		m_stats.split_failed,
		remix_rsx::archetype_name(m_active_camera.archetype),
		m_stats.world_applied,
		m_stats.world_fallback,
		m_stats.world_refused,
		m_stats.world_layered_ref,
		m_stats.skip_screen_space,
		m_stats.skip_immediate,
		m_stats.skip_inline_array,
		m_stats.skip_volatile,
		m_stats.skip_register_attr0,
		m_stats.skip_primitive,
		m_stats.skip_restart_index,
		m_stats.skip_instanced,
		m_stats.skip_layout,
		m_stats.skip_memory,
		m_stats.skip_decode,
		m_stats.skip_poisoned,
		m_stats.skip_vp,
		m_stats.skip_render_target,
		m_stats.rt_feedback_kept,
		m_stats.skip_not_input,
		m_stats.wdiv_draws,
		m_stats.pos_decode_refused,
		m_stats.vp_hpos_indirect,
		m_stats.vp_hpos_refused,
		m_stats.vp_hpos_indexed,
		m_stats.vp_wbuffer_z,
		m_stats.indexed_world_rigid,
		m_stats.indexed_world_skinned,
		m_stats.indexed_world_refused,
		m_stats.vp_indexed_matched,
		m_stats.vp_indexed_unmatched,
		m_stats.bone_degenerate,
		m_stats.skin_submitted,
		m_stats.skin_skipped,
		m_stats.skin_unrecognised,
		m_stats.skin_bones_max,
		m_stats.skin_unrec_arl,
		m_stats.skin_unrec_foreign,
		m_stats.skin_unrec_reads,
		m_stats.skin_unrec_indexed,
		m_skin_unrec_census_lines,
		m_stats.idxuniform_considered,
		m_stats.idxuniform_resolved,
		m_stats.idxuniform_refused,
		m_stats.idxbias_considered,
		m_stats.idxbias_resolved,
		m_stats.idxbias_refused,
		m_stats.skin_blend_submitted,
		m_stats.skin_blend_refused_index,
		m_stats.skin_blend_refused_weight,
		m_stats.skin_blend_refused_bone,
		m_stats.skin_blend_refused_palette,
		m_stats.skin_blend_refused_scale,
		m_stats.skin_blend_uniform,
		m_stats.skin_blend_rescaled,
		m_stats.skin_reach_flagged,
		m_stats.vtx_spread_flagged,
		m_stats.vtx_zero_split,
		m_stats.vtx_spread_submitted,
		m_stats.vtx_spread_refused,
		m_stats.wext_examined,
		m_stats.wext_exempt,
		m_stats.wext_nonfinite,
		m_stats.wext_refused,
		m_stats.wext_flagged_drawn,
		static_cast<f64>(m_world_extent_median),
		m_world_extent_samples,
		static_cast<f64>(m_stats.wext_drawn_max),
		m_stats.wext_drawn[0],
		m_stats.wext_drawn[1],
		m_stats.wext_drawn[2],
		m_stats.wext_drawn[3],
		m_stats.wext_drawn[4],
		m_stats.wext_drawn[5],
		m_stats.wext_drawn[6],
		m_stats.wext_drawn[7],
		m_stats.skip_lighting_pass,
		m_stats.cat_sky,
		m_stats.sky_candidates,
		m_stats.sky_refused_noworld,
		m_stats.sky_refused_extent,
		m_stats.sky_refused_anchor,
		m_stats.sky_refused_anchor_held,
		m_sky_census_lines,
		m_stats.sky_backdrop_hit,
		m_stats.sky_backdrop_dw,
		remix_rsx::sky_backdrop_mode(),
		m_stats.sky_learned_ring,
		m_stats.sky_hash_considered,
		m_stats.sky_hash_dome,
		m_stats.sky_hash_matched,
		m_stats.sky_hash_tagged,
		m_stats.sky_hash_rejected,
		m_stats.sky_hash_tracked,
		m_sky_hash_census_lines,
		remix_rsx::sky_hash_mode(),
		m_stats.viewmodel_tagged,
		m_stats.viewmodel_considered,
		m_stats.viewmodel_refused_full_range,
		m_stats.viewmodel_refused_offset,
		m_stats.viewmodel_far,
		m_stats.viewmodel_noanchor,
		m_viewmodel_census_lines,
		remix_rsx::viewmodel_mode(),
		m_stats.viewmodel_cam_considered,
		m_stats.viewmodel_cam_applied,
		m_stats.viewmodel_cam_fallback,
		m_stats.viewmodel_cam_refused,
		m_stats.viewmodel_cam_diverted,
		m_stats.viewmodel_cam_unusable,
		m_stats.viewmodel_cam_conflict,
		m_stats.viewmodel_cam_latched,
		m_stats.viewmodel_cam_held,
		m_viewmodel_camera_census_lines,
		remix_rsx::viewmodel_camera_mode(),
		m_stats.cat_hidden,
		m_stats.cat_particle,
		m_stats.cat_decal,
		m_stats.cat_smooth_normals,
		m_stats.blend_chained,
		m_stats.blend_translucent,
		m_stats.blend_unmapped,
		m_stats.tex_bound,
		m_stats.tex_none,
		m_stats.tex_no_unit,
		m_stats.tex_unit_retry,
		m_stats.tex_albedo_ucode,
		m_stats.tex_albedo_guess,
		m_stats.tex_retry_refused,
		m_stats.tex_unit_substituted,
		m_stats.uv_applied,
		m_stats.uv_none,
		m_stats.uv_absent,
		m_stats.uv_layout,
		m_stats.uv_memory,
		m_stats.uv_fallback,
		m_stats.uv_ucode,
		m_stats.uv_heuristic,
		m_stats.uv_nonfinite,
		m_stats.vcol_applied,
		static_cast<u64>(m_textures.live()),
		tex.created,
		tex.destroyed,
		tex.hits,
		tex.deferred,
		tex.unreadable,
		tex.unsupported,
		tex.tombstone_hits,
		tex.rehashed,
		tex.refreshed,
		tex.materials,
		m_stats.tex_retry_unsupported,
		tex.materials_untested,
		m_stats.uv_scale_ucode,
		m_stats.uv_scale_fixed,
		m_stats.ui_draws,
		m_stats.ui_skipped,
		m_stats.ui_no_colour,
		m_stats.ui_render_target,
		m_compositor.draws(),
		m_compositor.frames(),
		m_stats.ui_space_ndc,
		m_stats.ui_space_unit,
		m_stats.ui_space_pixel,
		m_stats.ui_space_none,
		m_stats.ui_ortho2d,
		// Which convention the guest's viewport states for the clip-pixel branch, and the ok/bad
		// orientation vote per branch. 'ui_vflip_ndc=A/B' reads A upright, B upside down, over the
		// family already confirmed correct on screen; ui_vflip_pixel is the same vote for the
		// branch whose row conversion had no evidence behind it. The two disagreeing is the
		// remaining flip, measured.
		m_stats.ui_space_vp_ydown,
		m_stats.ui_space_vp_yup,
		m_stats.ui_space_vp_fallback,
		m_stats.ui_vflip_ndc_ok,
		m_stats.ui_vflip_ndc_bad,
		m_stats.ui_vflip_pixel_ok,
		m_stats.ui_vflip_pixel_bad,
		m_stats.ui_vflip_abstain,
		// ZCULL occlusion-report page faults taken by guest threads. Non-zero means the title polls
		// cellGcmGetReport while a query is in flight; every one of these that is NOT handled wedges
		// the faulting PPU thread forever (see on_access_violation).
		g_remix_av_seen.load(),
		g_remix_av_handled.load());

	// Where the RSX thread's wall clock actually went, per frame, over this window. 'other' is
	// window - flip: everything outside flip(), which is FIFO decode plus any guest stall.
	// 'ui' is measured inside 'draw', not alongside it.
	{
		const f64 n = static_cast<f64>(std::max<u64>(1, m_timing.frames));
		const auto ms = [n](u64 us) { return static_cast<f64>(us) / 1000.0 / n; };

		rsx_log.notice(
			"Remix timing: frames=%llu frame_ms=%.2f | flip=%.2f (overlay=%.2f submit=%.2f present=%.2f) | draw=%.2f (ui=%.2f) | other=%.2f | ui_px/frame=%.0f",
			m_timing.frames,
			ms(m_timing.window),
			ms(m_timing.flip),
			ms(m_timing.overlay),
			ms(m_timing.submit),
			ms(m_timing.present),
			ms(m_timing.draw),
			ms(m_timing.ui),
			ms(m_timing.window > m_timing.flip ? m_timing.window - m_timing.flip : 0),
			static_cast<f64>(m_compositor.pixels()) / n);

		// Which programs minted the window's mesh handles. Printed per frame so the number is
		// directly comparable against the ~350 sub-draws a frame submits: a program at 30
		// creates/frame is producing a brand-new BLAS for every one of its draws, every frame.
		if (!m_mesh_creates_by_vp.empty())
		{
			std::vector<std::pair<u64, u64>> by_count; // creates, vp
			by_count.reserve(m_mesh_creates_by_vp.size());

			for (const auto& [vp, count] : m_mesh_creates_by_vp)
			{
				by_count.emplace_back(count, vp);
			}

			std::sort(by_count.begin(), by_count.end(),
				[](const auto& a, const auto& b) { return a.first > b.first; });

			std::string top;

			for (usz i = 0; i < by_count.size() && i < 8; ++i)
			{
				fmt::append(top, " %016llx=%.1f", by_count[i].second,
					static_cast<f64>(by_count[i].first) / n);
			}

			rsx_log.notice("Remix meshchurn: programs=%llu creates/frame:%s",
				static_cast<u64>(m_mesh_creates_by_vp.size()), top);

			m_mesh_creates_by_vp.clear();
		}

		m_timing = {};
		m_compositor.reset_pixels();
	}

	if (m_ui_biggest.area > 0.f)
	{
		rsx_log.notice(
			"Remix ui-biggest: vp=%016llx area=%.0fpx verts=%u inputs=0x%x unit=%d pixels=%llu uv=%d tint=%08X clip=[%.4f %.4f]..[%.4f %.4f] unit_square=%d",
			m_ui_biggest.vp_hash,
			m_ui_biggest.area,
			m_ui_biggest.verts,
			m_ui_biggest.inputs,
			m_ui_biggest.unit,
			static_cast<u64>(m_ui_biggest.pixels),
			m_ui_biggest.have_uv ? 1 : 0,
			m_ui_biggest.tint,
			m_ui_biggest.clip_lo[0],
			m_ui_biggest.clip_lo[1],
			m_ui_biggest.clip_hi[0],
			m_ui_biggest.clip_hi[1],
			m_ui_biggest.clip_unit ? 1 : 0);

		m_ui_biggest = {};
	}

	// The orientation audit, named. One row per vertex program that cast a vote, so a run says
	// which programs are inverted rather than which branch - the distinction the aggregate cannot
	// draw and the one that decides whether the fix is a mapping change or a classification
	// change for a single program. 'ortho=1' is the family already confirmed upright on screen, so
	// an inverted row with ortho=1 means the audit disagrees with the screen and the instrument is
	// wrong; an inverted row with ortho=0 names the program actually still flipped.
	{
		bool any = false;

		for (u32 i = 0; i < s_ui_vote_rows; ++i)
		{
			const ui_vote_row& row = m_ui_votes[i];

			if (row.vp_hash == 0)
			{
				break;
			}

			any = true;

			rsx_log.notice("Remix ui-vote: vp=%016llx ortho=%d ndc=%llu/%llu pixel=%llu/%llu",
				row.vp_hash, row.ortho ? 1 : 0,
				row.ndc_ok, row.ndc_bad, row.pixel_ok, row.pixel_bad);
		}

		if (any && m_ui_vote_spill != 0)
		{
			rsx_log.notice("Remix ui-vote: spill=%llu (programs past the table)", m_ui_vote_spill);
		}
	}

	// The counters that answer a "did that change anything?" question, mirrored where they can
	// be read during the run. Everything above this point goes only to RPCS3.log, which is held
	// exclusively locked until the emulator exits - so the whole stats block has been unreadable
	// while the thing it describes is on screen, and a question like "is the tiling still there"
	// had to be answered by eye. Deliberately a short line and not the full block: the full one
	// is 60 counters wide and would bury the census lines it sits between.
	{
		const std::string line = fmt::format(
			"Remix live: seen=%llu submitted=%llu | uv_applied=%llu uv_scale_ucode=%llu uv_scale_fixed=%llu | "
			"tex_bound=%llu tex_none=%llu | world_refused=%llu wext_refused=%llu | "
			"cam_resolved=%llu cam_fallback=%llu cam_held=%llu world_refused_nocam=%llu | "
			"mesh_created=%llu mesh_reused=%llu mesh_live=%llu mesh_destroyed=%llu flips=%llu",
			m_stats.draws_seen,
			m_stats.draws_submitted,
			m_stats.uv_applied,
			m_stats.uv_scale_ucode,
			m_stats.uv_scale_fixed,
			m_stats.tex_bound,
			m_stats.tex_none,
			m_stats.world_refused,
			m_stats.wext_refused,
			m_stats.cam_resolved,
			m_stats.cam_fallback,
			m_stats.cam_held,
			m_stats.world_refused_nocam,
			m_stats.meshes_created,
			m_stats.meshes_reused,
			static_cast<u64>(m_meshes.size()),
			m_stats.meshes_destroyed,
			m_frame_counter);

		std::string bones = fmt::format(
			"Remix bone-fail: max=%.6g <0.05=%llu <0.2=%llu <1=%llu <10=%llu >=10=%llu |",
			static_cast<f64>(m_bone_residue_max),
			m_bone_residue_buckets[0], m_bone_residue_buckets[1], m_bone_residue_buckets[2],
			m_bone_residue_buckets[3], m_bone_residue_buckets[4]);

		for (usz i = 0; i < std::size(s_bone_fail_names); ++i)
		{
			fmt::append(bones, " %s=%llu", s_bone_fail_names[i], m_bone_fail_counts[i]);
		}

		bones += '\n';

		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			out.write(bones);
		}

		std::string fails = fmt::format(
			"Remix affine-residue: tol=%.4g max=%.6g <0.05=%llu <0.2=%llu <1=%llu <10=%llu >=10=%llu\n",
			static_cast<f64>(remix_rsx::world_affine_tolerance()),
			static_cast<f64>(m_affine_residue_max),
			m_affine_residue_buckets[0], m_affine_residue_buckets[1], m_affine_residue_buckets[2],
			m_affine_residue_buckets[3], m_affine_residue_buckets[4]);

		fails += "Remix world-fail:";

		for (usz i = 0; i < std::size(s_world_fail_names); ++i)
		{
			if (m_world_fail_counts[i] != 0)
			{
				fmt::append(fails, " %s=%llu", s_world_fail_names[i], m_world_fail_counts[i]);
			}
		}

		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			fails += '\n';
			out.write(fails);
		}

		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			out.write(line + '\n');
		}
	}
}

#endif
