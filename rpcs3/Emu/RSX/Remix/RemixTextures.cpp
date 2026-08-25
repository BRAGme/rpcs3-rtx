#include "stdafx.h"
#include "RemixTextures.h"

#ifdef _WIN32

#include "Emu/Memory/vm.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Common/io_buffer.h"
#include "Emu/RSX/RSXTexture.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Remix/RemixTransforms.h"
#include "Emu/RSX/gcm_enums.h"
#include "Emu/RSX/Remix/RemixRuntime.h"
#include "Emu/system_config.h"
#include "util/fnv_hash.hpp"

#include "3rdparty/bcdec/bcdec.hpp"

#include <algorithm>
#include <cstdio>

namespace remix_rsx
{
	namespace
	{
		// Frames a texture may go unreferenced before its handles are released: no longer a
		// constant here. The live value is texture_idle_frames() (RPCS3_REMIX_TEXIDLE /
		// "Texture Idle Frames", default 300 in system_config.h). Deliberately still the same
		// shape as the mesh LRU in RemixGSRender - and now the same knob shape too - so both age
		// out together and a title that wants a longer residency raises both.

		// Upper bound on a single decoded mip 0. 4096x4096 BGRA8 = 64 MiB.
		constexpr usz s_max_decoded_bytes = 64ull * 1024 * 1024;

		// Bytes read for the per-draw staleness fingerprint.
		constexpr u32 s_fingerprint_budget = 4096;

		u32 read_env_u32(const wchar_t* name, u32 fallback)
		{
			wchar_t buffer[64]{};
			const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return fallback;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);
			return (parsed >= 0) ? static_cast<u32>(parsed) : fallback;
		}

		// Remix/MDL address modes: Clamp=0, Repeat=1, Mirrored Repeat=2, Clip=3.
		// Mirror-once has no exact API representation; clamping is safer than turning one
		// reflected edge into the infinite tiling seen on Haze's alpha masks.
		u8 to_remix_wrap(rsx::texture_wrap_mode mode)
		{
			switch (mode)
			{
			case rsx::texture_wrap_mode::wrap:
				return 1;
			case rsx::texture_wrap_mode::mirror:
				return 2;
			case rsx::texture_wrap_mode::border:
				return 3;
			case rsx::texture_wrap_mode::mirror_once_clamp_to_edge:
			case rsx::texture_wrap_mode::mirror_once_border:
			case rsx::texture_wrap_mode::mirror_once_clamp:
				return 0;
			default:
				return 0;
			}
		}

		// FNV-1a over a byte range, the same shape as buffered_section::fast_hash_internal
		// (Emu\RSX\Common\texture_cache.cpp:157-197). Copied rather than widening its access.
		u64 fnv_bytes(const u8* data, usz length, u64 seed)
		{
			u64 hash = seed;
			usz i = 0;

			for (; (i + sizeof(u64)) <= length; i += sizeof(u64))
			{
				u64 word;
				std::memcpy(&word, data + i, sizeof(u64));
				hash = rpcs3::hash64(hash, word);
			}

			for (; i < length; ++i)
			{
				hash = rpcs3::hash64(hash, u64{data[i]});
			}

			return hash;
		}

		// Cheap strided sample used to notice in-place updates without rehashing everything.
		u64 sample_fingerprint(const u8* data, u32 length)
		{
			if (!data || length == 0)
			{
				return 0;
			}

			const u32 step = std::max<u32>(8, (length / s_fingerprint_budget) & ~7u);

			u64 hash = rpcs3::fnv_seed;
			hash = rpcs3::hash64(hash, u64{length});

			for (u32 i = 0; (i + sizeof(u64)) <= length; i += step)
			{
				u64 word;
				std::memcpy(&word, data + i, sizeof(u64));
				hash = rpcs3::hash64(hash, word);
			}

			return hash;
		}

		// 16-bit -> BGRA8 expanders. The source word is already native-endian by the time
		// upload_texture_subresource is done with it.
		using expand_fn = u32 (*)(u16);

		u32 expand_r5g6b5(u16 v)
		{
			const u32 r = ((v >> 11) & 0x1F) * 255 / 31;
			const u32 g = ((v >> 5) & 0x3F) * 255 / 63;
			const u32 b = (v & 0x1F) * 255 / 31;
			return b | (g << 8) | (r << 16) | 0xFF000000u;
		}

		u32 expand_a1r5g5b5(u16 v)
		{
			const u32 a = (v & 0x8000) ? 0xFFu : 0u;
			const u32 r = ((v >> 10) & 0x1F) * 255 / 31;
			const u32 g = ((v >> 5) & 0x1F) * 255 / 31;
			const u32 b = (v & 0x1F) * 255 / 31;
			return b | (g << 8) | (r << 16) | (a << 24);
		}

		u32 expand_d1r5g5b5(u16 v)
		{
			return expand_a1r5g5b5(v) | 0xFF000000u;
		}

		u32 expand_a4r4g4b4(u16 v)
		{
			const u32 a = ((v >> 12) & 0xF) * 17;
			const u32 r = ((v >> 8) & 0xF) * 17;
			const u32 g = ((v >> 4) & 0xF) * 17;
			const u32 b = (v & 0xF) * 17;
			return b | (g << 8) | (r << 16) | (a << 24);
		}

		u32 expand_r5g5b5a1(u16 v)
		{
			const u32 r = ((v >> 11) & 0x1F) * 255 / 31;
			const u32 g = ((v >> 6) & 0x1F) * 255 / 31;
			const u32 b = ((v >> 1) & 0x1F) * 255 / 31;
			const u32 a = (v & 1) ? 0xFFu : 0u;
			return b | (g << 8) | (r << 16) | (a << 24);
		}

		expand_fn expander_for(u32 gcm_format)
		{
			switch (gcm_format)
			{
			case CELL_GCM_TEXTURE_R5G6B5: return &expand_r5g6b5;
			case CELL_GCM_TEXTURE_A1R5G5B5: return &expand_a1r5g5b5;
			case CELL_GCM_TEXTURE_D1R5G5B5: return &expand_d1r5g5b5;
			case CELL_GCM_TEXTURE_A4R4G4B4: return &expand_a4r4g4b4;
			case CELL_GCM_TEXTURE_R5G5B5A1: return &expand_r5g5b5a1;
			default: return nullptr;
			}
		}

		// upload_texture_subresource writes a BGRA8 image directly for these.
		bool is_direct_bgra8(u32 gcm_format)
		{
			switch (gcm_format)
			{
			case CELL_GCM_TEXTURE_A8R8G8B8:
			case CELL_GCM_TEXTURE_D8R8G8B8:
			case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
			case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
				return true;
			default:
				return false;
			}
		}

		bool is_bc_format(u32 gcm_format)
		{
			return gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 ||
				gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 ||
				gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT45;
		}
	}

	bool textures_disabled()
	{
		static const bool env = read_env_u32(L"RPCS3_REMIX_NOTEX", 0) != 0;
		return env || g_cfg.video.remix.no_textures;
	}

	bool blend_state_enabled()
	{
		static const bool env = read_env_u32(L"RPCS3_REMIX_BLENDSTATE", 1) != 0;
		return env;
	}

	u32 texture_budget()
	{
		// Read live so it can be tuned without a restart. A camera turn exposes many new
		// materials at once, and anything over budget submits with a null material for that
		// frame, which renders untextured until a later frame lets it through. 0 = unlimited.
		static const u32 env = read_env_u32(L"RPCS3_REMIX_TEXBUDGET", 0);
		const u32 value = env ? env : g_cfg.video.remix.texture_budget;
		return value ? value : umax;
	}

	u32 texture_idle_frames()
	{
		// How long a decoded texture and its Remix handles survive after the last draw that bound
		// them. Read live like texture_budget, so it can be tuned without a restart. The default
		// 300 (~5 s at 60 fps) makes a title that revisits a room after a corridor pay the decode
		// and the CreateTexture again; raising it trades VRAM for that. The config floor is 30, but
		// the environment variable can express 0 - reap the frame a texture stops being bound -
		// which is a useful bisect, so umax is the unset sentinel rather than 0, exactly as
		// camera_hold_frames argues.
		static const u32 env = read_env_u32(L"RPCS3_REMIX_TEXIDLE", umax);
		return env != umax ? env : g_cfg.video.remix.texture_idle;
	}

	bool dump_texture_images()
	{
		// Alongside the 'Remix tex=' census line, write each unique decoded texture out as a BMP
		// under remix_tex\. On by default, because the only caller is already inside
		// dump_enabled() - i.e. the user has deliberately turned Log Draw Diagnostics on, which
		// is a debug mode that already costs ~590 ms frame stalls, and ~700 small files is not
		// what makes it expensive. It defaulted off first and produced nothing, because an
		// environment variable set in a shell never reaches an rpcs3 launched from anywhere else.
		// RPCS3_REMIX_TEXBMP=0 turns it off without turning the rest of the dump off.
		static const bool env = read_env_u32(L"RPCS3_REMIX_TEXBMP", 1) != 0;
		return env;
	}

	bool textures_linear()
	{
		static const bool env = read_env_u32(L"RPCS3_REMIX_TEXLINEAR", 0) != 0;
		return env || g_cfg.video.remix.texture_linear;
	}

	u32 texture_rehash_mode()
	{
		// Default 0 (descriptor only), measured rather than assumed: on Minecraft NPUB31419
		// mode 1 turned 682 CreateTexture calls into 604 rehashes over 9,120 frames, and
		// because the albedo hash is folded into the mesh key that dragged mesh churn from
		// 5,881 creates / 887 live (mode 0) to 188,427 creates / 31,793 live (mode 1).
		// Animated textures go stale instead; RPCS3_REMIX_TEXREHASH=1 or 2 buys them back.
		static const u32 env = std::min<u32>(2, read_env_u32(L"RPCS3_REMIX_TEXREHASH", 0));
		return env ? env : std::min<u32>(2, g_cfg.video.remix.texture_rehash);
	}

	bool texture_reap_safe()
	{
		// Round 9. When set (the default), reap() is handed the set of material handles that live
		// meshes have baked in and refuses to destroy those. 0 restores the pre-round-9 reap
		// bit-exactly, which is the control for the 30-second idle-degradation repro: if the
		// degradation comes back with 0 and not with 1, the reaper and the baked handle were the
		// mechanism. See texture_stats::reap_kept / reap_freed.
		static const u32 env = read_env_u32(L"RPCS3_REMIX_TEXREAPSAFE", 1);
		return env != 0;
	}

	u32 texture_verify_frames()
	{
		// Default 120: at 30 fps that is one full hash per live entry every four seconds, which is
		// fast enough to catch a streaming pool recycling an address between two areas and slow
		// enough that the cost is invisible against the per-draw work. 0 disables the check
		// entirely and is the bisect step if frame time regresses.
		static const u32 env = read_env_u32(L"RPCS3_REMIX_TEXVERIFY", 120);
		return env;
	}

	bool texture_stale_evict()
	{
		// Round 17: act on the round-8 detector. Default 0, which reproduces build 4b7bdb4 exactly -
		// this touches the hottest path in the backend and the A/B has to be free.
		//
		// The round-8 note above texture_verify_frames() left this unfixed because it assumed the
		// fix had to REBUILD IN PLACE with content_hash pinned, and pinning the hash is precisely
		// what makes destroying the Remix material unsafe (a reused mesh keeps the baked handle).
		// That assumption is what stalled it for nine rounds. Erasing the entry outright does not
		// need the pin: the next bind decodes the bytes that are there now, derives the CORRECT
		// content hash, gets its own material, and re-keys its own meshes. The old mesh keeps
		// referencing the old material, so the old material must simply not be destroyed - see the
		// orphan branch at the erase site. Bounded, measured leak instead of a dangling handle.
		//
		// This is not the RPCS3_REMIX_TEXREHASH trap either. That one hangs off a STRIDED
		// sample_fingerprint that false-positives (682 textures, 604 rehashes, 32x mesh churn); this
		// hangs off the full-hash cadence verify, which fired 134 times in 18,269 flips on the
		// round-16 Haze run.
		static const u32 env = read_env_u32(L"RPCS3_REMIX_TEXSTALEEVICT", 0);
		return env != 0;
	}

	u64 texture_descriptor::key() const
	{
		u64 hash = rpcs3::fnv_seed;
		hash = rpcs3::hash64(hash, u64{offset});
		hash = rpcs3::hash64(hash, u64{location} | (u64{format} << 8) | (u64{pitch} << 16));
		hash = rpcs3::hash64(hash, u64{width} | (u64{height} << 16) | (u64{depth} << 32) | (u64{mipmaps} << 48));
		hash = rpcs3::hash64(hash, u64{border} | (u64{wrap_s} << 8) | (u64{wrap_t} << 16) | (u64{cubemap} << 24) | (u64{dimension} << 32));
		hash = rpcs3::hash64(hash, u64{alpha_func} | (u64{alpha_ref} << 8));
		return hash ? hash : 1;
	}

	void texture_cache::begin_frame()
	{
		m_budget_left = texture_budget();
	}

	void texture_cache::note_refusal(const char* reason, u32 gcm_format, u32 width, u32 height)
	{
		u64 key = rpcs3::fnv_seed;
		key = rpcs3::hash64(key, u64{gcm_format});
		key = rpcs3::hash64(key, u64{width} | (u64{height} << 32));
		key = rpcs3::hash64(key, reinterpret_cast<u64>(reason));

		if (!m_refusals_seen.insert(key).second)
		{
			return;
		}

		rsx_log.notice("Remix texrefuse: %s fmt=%02x %ux%u", reason, gcm_format, width, height);
	}

	void texture_cache::note_stale(u64 key, const texture_entry& entry, u32 format, u64 frame)
	{
		if (m_texstale_lines >= s_max_texstale_lines || !m_texstale_seen.insert(key).second)
		{
			return;
		}

		++m_texstale_lines;

		const std::string line = fmt::format(
			"Remix texstale: key=%016llx albedo=%016llX fmt=%02x dims=%ux%u wrap=%u/%u "
			"age=%llu frame=%llu line=%u/%u",
			key,
			entry.content_hash,
			format,
			entry.width,
			entry.height,
			u32{entry.wrap_u},
			u32{entry.wrap_v},
			frame - entry.last_verified_frame,
			frame,
			m_texstale_lines,
			s_max_texstale_lines);

		rsx_log.notice("%s", line);

		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			out.write(line + '\n');
		}
	}

	void texture_cache::note_content_dup(u64 content_hash, u64 key_a, u64 key_b, u32 format, u32 width, u32 height)
	{
		if (m_texdup_lines >= s_max_texdup_lines || !m_texdup_seen.insert(content_hash).second)
		{
			return;
		}

		++m_texdup_lines;

		// albedo= is deliberately spelled the same way the pick line spells it, so a Ctrl+Click on
		// a wrong-textured surface joins against these lines by plain string match.
		const std::string line = fmt::format(
			"Remix texdup: albedo=%016llX key_a=%016llx key_b=%016llx fmt=%02x dims=%ux%u line=%u/%u",
			content_hash, key_a, key_b, format, width, height, m_texdup_lines, s_max_texdup_lines);

		rsx_log.notice("%s", line);

		if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
		{
			out.write(line + '\n');
		}
	}

	remixapi_MaterialHandle texture_cache::bind(const remixapi_Interface& api,
		const rsx::fragment_texture& tex,
		u64 frame,
		const texture_entry** out_entry,
		bool refresh_pixels)
	{
		if (out_entry)
		{
			*out_entry = nullptr;
		}

		if (textures_disabled())
		{
			return nullptr;
		}

		texture_descriptor desc{};
		desc.offset = tex.offset();
		desc.location = tex.location();
		desc.format = tex.format();
		desc.pitch = tex.pitch();
		desc.width = tex.width();
		desc.height = tex.height();
		desc.depth = tex.depth();
		desc.mipmaps = static_cast<u8>(std::min<u32>(0xFF, tex.get_exact_mipmap_count()));
		desc.border = tex.border_type();
		desc.wrap_s = static_cast<u8>(tex.wrap_s());
		desc.wrap_t = static_cast<u8>(tex.wrap_t());
		desc.cubemap = tex.cubemap() ? 1 : 0;
		desc.dimension = static_cast<u8>(tex.dimension());

		// Alpha-cutout foliage was rendering as a solid quad because M3 shipped every material
		// with alphaTestType 7 (ALWAYS, i.e. no test) and useDrawCallAlphaState 1 - and the
		// latter tells the runtime to read the alpha state from a remixapi_InstanceInfoBlendEXT
		// this backend never chains, so the state was neither the title's nor the material's.
		// Take it from the RSX registers, where it actually lives.
		// Recorded here rather than derived from desc.alpha_func at creation time, because ALWAYS
		// is both the "no test" default and a value a title can legitimately set.
		const bool alpha_tested = !alpha_state_disabled() && rsx::method_registers.alpha_test_enabled();

		if (alpha_tested)
		{
			// RSX comparison_function is CELL_GCM_NEVER..CELL_GCM_ALWAYS = 0x200..0x207, and
			// VkCompareOp (which is what the runtime's alphaTestType is) is 0..7 in the same
			// order, so the mapping is a subtraction. Anything outside the range falls back to
			// ALWAYS rather than guessing.
			const u32 func = static_cast<u32>(rsx::method_registers.alpha_func());

			if (func >= 0x200 && func <= 0x207)
			{
				desc.alpha_func = static_cast<u8>(func - 0x200);
			}

			// alpha_ref() is normalised 0..1 regardless of the surface's colour depth;
			// alphaReferenceValue is a uint8_t.
			desc.alpha_ref = static_cast<u8>(std::clamp(rsx::method_registers.alpha_ref(), 0.f, 1.f) * 255.f + 0.5f);
		}

		if (desc.width == 0 || desc.height == 0)
		{
			++m_stats.unsupported;
			note_refusal("zero-dims", desc.format, desc.width, desc.height);
			return nullptr;
		}

		const u64 key = desc.key();
		auto it = m_entries.find(key);

		if (it != m_entries.end())
		{
			texture_entry& entry = it->second;
			entry.last_used_frame = frame;

			if (entry.unsupported)
			{
				++m_stats.unsupported;
				++m_stats.tombstone_hits;

				// Of those, the ones tombstoned for a condition that was never permanent. The guest
				// range being unmapped, or the mip chain not written yet, is a statement about the
				// moment the decode ran - and this cache turns it into a verdict that outlives the
				// level. Counted, not acted on: retrying tombstones is a real behaviour change with
				// a mesh-churn failure mode, and this number is what decides whether it is worth
				// the risk.
				if (entry.refusal_reason == std::string_view("unreadable")
					|| entry.refusal_reason == std::string_view("no-mip0"))
				{
					++m_stats.tombstone_transient;
				}

				// Published even though the bind failed, so a caller walking texture units can
				// tell a *permanent format* refusal from every other null. Haze binds a
				// 2048x2048 DEPTH16 shadow map (fmt=92) on the lowest referenced unit of every
				// shadow-receiving draw: 55,360 refusals in one capture, and the albedo it
				// wanted is on a higher unit. A caller that cannot see the reason has to treat
				// this like any other miss. Safe for the refresh_pixels caller, which nulls any
				// entry with empty pixels - which an unsupported entry always has.
				if (out_entry)
				{
					*out_entry = &entry;
				}

				return nullptr;
			}

			bool stale = false;

			if (texture_rehash_mode() > 0)
			{
				// The descriptor is unchanged, so the guest range is too: sample it directly
				// instead of walking the subresource layout again.
				const u32 address = rsx::get_address(desc.offset, desc.location);
				const u32 length = static_cast<u32>(std::min<usz>(rsx::get_texture_size(tex), 0x4000000));

				if (length && vm::check_addr(address, vm::page_readable, length))
				{
					const u64 fingerprint = (texture_rehash_mode() >= 2)
						? fnv_bytes(vm::_ptr<const u8>(address), length, rpcs3::fnv_seed)
						: sample_fingerprint(vm::_ptr<const u8>(address), length);

					stale = (fingerprint != entry.fingerprint);
				}
			}

			if (!stale)
			{
				++m_stats.hits;

				// The CPU copy is what the compositor rasterizes from, and a title that
				// rewrites a font atlas in place under a stable descriptor leaves it holding
				// the page that happened to be resident on first sight. Measured on Haze
				// BLUS30094: the 512x512 B8 glyph sheet the menu samples is rewritten, and
				// without this the menu renders as overlapping glyph rows because the UVs
				// address a page the cached bytes no longer contain.
				//
				// Full hash rather than the strided sample: a font page swap changes a small
				// fraction of the bytes and the sample misses it. 512x512 B8 is 256 KiB, which
				// is far below what the scalar rasterizer downstream already costs.
				if (refresh_pixels && texture_rehash_mode() == 0 && !entry.pixels.empty())
				{
					const u32 address = rsx::get_address(desc.offset, desc.location);
					const u32 length = static_cast<u32>(std::min<usz>(rsx::get_texture_size(tex), 0x4000000));

					if (length && vm::check_addr(address, vm::page_readable, length))
					{
						if (const u64 full = fnv_bytes(vm::_ptr<const u8>(address), length, rpcs3::fnv_seed);
							full != entry.content_refresh)
						{
							// decode() rewrites content_hash and fingerprint as well as the
							// pixels. content_hash is folded into the mesh key
							// (RemixGSRender albedo_hash), so letting it move here would
							// re-key every mesh that shares this texture - the very churn
							// that made the global rehash policy unusable. Restore both; the
							// Remix texture/material handles are untouched either way, so the
							// GPU side stays exactly as it was.
							const u64 keep_hash = entry.content_hash;
							const u64 keep_fingerprint = entry.fingerprint;

							if (decode(tex, entry))
							{
								++m_stats.refreshed;
							}

							entry.content_hash = keep_hash;
							entry.fingerprint = keep_fingerprint;
							entry.content_refresh = full;
						}
					}
				}

				// --- round 8: the cadence staleness verify (measurement only) -------------------
				// The descriptor-only key plus zero write-tracking means a streaming pool that
				// recycles an address (bark -> soldier) keeps serving the old pixels AND the old
				// material forever. TEXREHASH would catch it, but its stale path re-keys
				// content_hash and that is the measured 32x mesh-churn trap. This says how often
				// it actually happens, by key, without touching a handle. See
				// texture_verify_frames() for why the in-place rebuild is not safe in this tree.
				//
				// Full hash, not the strided sample: the same reason the refresh_pixels block
				// above gives - a partial page swap changes a small fraction of the bytes.
				if (const u32 cadence = texture_verify_frames();
					cadence != 0 && texture_rehash_mode() == 0
					&& frame >= entry.last_verified_frame + cadence)
				{
					const u32 address = rsx::get_address(desc.offset, desc.location);
					const u32 length = static_cast<u32>(std::min<usz>(rsx::get_texture_size(tex), 0x4000000));

					if (length && vm::check_addr(address, vm::page_readable, length))
					{
						const u64 full = fnv_bytes(vm::_ptr<const u8>(address), length, rpcs3::fnv_seed);

						if (entry.verify_hash != 0 && full != entry.verify_hash)
						{
							++m_stats.stale_detected;
							note_stale(key, entry, desc.format, frame);

							// Round 17. Reuses the same 'stale' local the TEXREHASH arm sets, so
							// both staleness policies land on the one rebuild path below rather
							// than growing a second one. ++m_stats.hits has already fired for this
							// bind; stale_evicted is the correction, and the difference between the
							// two counters is the whole A/B.
							if (texture_stale_evict())
							{
								stale = true;
								++m_stats.stale_evicted;
							}
						}

						entry.verify_hash = full;
					}

					// Stamped even when the range was unreadable: retrying an unmapped range on
					// every single bind is exactly the cost this cadence exists to bound.
					entry.last_verified_frame = frame;
				}

				// Round 17: re-tested, because the cadence verify above may have just turned this
				// hit into a miss. With TEXSTALEEVICT=0 'stale' cannot have changed since the
				// enclosing test and this is the same unconditional return it was.
				if (!stale)
				{
					if (out_entry)
					{
						*out_entry = &entry;
					}

					return entry.material;
				}
			}

			// Content changed under a stable descriptor. Drop the old pair and fall through
			// to a rebuild; the budget applies to that rebuild like any other miss.
			//
			// Round 17: the TEXSTALEEVICT arm ORPHANS instead of destroying. The mesh cache bakes
			// the material handle at CreateMesh while keying the mesh on the CONTENT hash, and on
			// this title ~45 live entries share one content hash (tex_key_dup 11,212 of
			// tex_created 11,467), so meshes carrying this material are still being looked up by
			// OTHER entries every frame. Destroying it here is the dangling-handle failure the
			// round-9 reap split was added to prevent. The TEXREHASH arm keeps destroying because
			// that is its pre-existing behaviour and TEXSTALEEVICT=0 must stay bit-exact.
			// Cost of orphaning: one texture+material pair per stale event, 134 over the whole
			// round-16 run, reclaimed by the runtime at device teardown.
			const bool orphan = texture_stale_evict() && texture_rehash_mode() == 0;

			if (!orphan && entry.material)
			{
				guarded_destroy_material(api.DestroyMaterial, entry.material);
			}

			if (!orphan && entry.texture)
			{
				guarded_destroy_texture(api.DestroyTexture, entry.texture);
			}

			if (orphan)
			{
				++m_stats.stale_orphaned;
			}
			else
			{
				++m_stats.destroyed;
			}

			++m_stats.rehashed;
			m_entries.erase(it);
		}

		if (m_budget_left == 0)
		{
			++m_stats.deferred;
			return nullptr;
		}

		texture_entry entry{};
		entry.last_used_frame = frame;
		entry.wrap_u = to_remix_wrap(tex.wrap_s());
		entry.wrap_v = to_remix_wrap(tex.wrap_t());
		entry.alpha_func = desc.alpha_func;
		entry.alpha_ref = desc.alpha_ref;

		if (!decode(tex, entry))
		{
			// Remember the failure so the same descriptor is not retried every draw. The entry
			// is published for the same reason as the tombstone path above: this is a permanent
			// format refusal, and the unit walk has to be able to see that.
			entry.unsupported = true;
			auto inserted = m_entries.emplace(key, std::move(entry));

			if (out_entry)
			{
				*out_entry = &inserted.first->second;
			}

			return nullptr;
		}

		--m_budget_left;

		if (!upload(api, entry))
		{
			entry.unsupported = true;
			entry.pixels.clear();
			entry.pixels.shrink_to_fit();

			auto inserted = m_entries.emplace(key, std::move(entry));

			if (out_entry)
			{
				*out_entry = &inserted.first->second;
			}

			return nullptr;
		}

		// Did this image already exist under another descriptor key? The key is descriptor-only
		// (RemixTextures.cpp:265-274), so the same bytes bound with a different wrap mode or a
		// different alpha state legitimately produce a second entry - but the *content hash* is what
		// the mesh key, the pick line and every rtx.conf texture list are written against, and two
		// live entries sharing one is exactly the ambiguity a wrong-texture sighting would need.
		// Recorded before the emplace's entry is moved from, and never used to change a decision.
		{
			auto [it_content, fresh] = m_content_keys.try_emplace(entry.content_hash, key);

			if (!fresh && it_content->second != key)
			{
				++m_stats.key_duplicate_hash;
				// Round 8: named, not just counted. tex_key_dup has read 11,383 for several rounds
				// with no way to tell whether the duplicates are benign (one atlas bound clamp on
				// the UI and repeat in the world) or the wrong-texture bug itself. This is the line
				// a Ctrl+Click's albedo= joins against.
				note_content_dup(entry.content_hash, it_content->second, key, desc.format,
					entry.width, entry.height);
			}
		}

		// Round 8: seed the cadence verify from the bytes this entry was actually built from, so
		// the FIRST verify can already detect a swap. Seeding lazily instead would cost two full
		// cadence windows before any stale entry could be named.
		{
			const u32 address = rsx::get_address(desc.offset, desc.location);
			const u32 length = static_cast<u32>(std::min<usz>(rsx::get_texture_size(tex), 0x4000000));

			if (texture_verify_frames() != 0 && length && vm::check_addr(address, vm::page_readable, length))
			{
				entry.verify_hash = fnv_bytes(vm::_ptr<const u8>(address), length, rpcs3::fnv_seed);
			}

			entry.last_verified_frame = frame;
		}

		auto inserted = m_entries.emplace(key, std::move(entry));

		if (!alpha_tested)
		{
			++m_stats.materials_untested;
		}

		if (out_entry)
		{
			*out_entry = &inserted.first->second;
		}

		return inserted.first->second.material;
	}

	bool texture_cache::decode(const rsx::fragment_texture& tex, texture_entry& out)
	{
		const u32 raw_format = tex.format();
		const u32 gcm_format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
		const bool is_swizzled = !(raw_format & CELL_GCM_TEXTURE_LN);

		const u32 address = rsx::get_address(tex.offset(), tex.location());
		const usz total_size = rsx::get_texture_size(tex);

		if (total_size == 0 || total_size > 0x4000000)
		{
			++m_stats.unsupported;
			out.refusal_reason = "size";
			note_refusal("size", gcm_format, tex.width(), tex.height());
			return false;
		}

		if (!vm::check_addr(address, vm::page_readable, static_cast<u32>(total_size)))
		{
			// Render-target-sourced or unmapped: guest RAM is not authoritative here.
			++m_stats.unreadable;
			out.refusal_reason = "unreadable";
			note_refusal("unreadable", gcm_format, tex.width(), tex.height());
			return false;
		}

		const bool direct = is_direct_bgra8(gcm_format);
		const expand_fn expand = expander_for(gcm_format);
		const bool is_bc = is_bc_format(gcm_format);
		const bool is_b8 = (gcm_format == CELL_GCM_TEXTURE_B8);
		out.b8_coverage = is_b8;

		if (!direct && !expand && !is_bc && !is_b8)
		{
			++m_stats.unsupported;
			out.refusal_reason = "format";
			note_refusal("format", gcm_format, tex.width(), tex.height());
			return false;
		}

		const std::vector<rsx::subresource_layout> layouts = rsx::get_subresources_layout(tex);

		const rsx::subresource_layout* mip0 = nullptr;

		for (const auto& layout : layouts)
		{
			if (layout.level == 0 && layout.layer == 0)
			{
				mip0 = &layout;
				break;
			}
		}

		if (!mip0 || mip0->data.empty())
		{
			++m_stats.unreadable;
			out.refusal_reason = "no-mip0";
			note_refusal("no-mip0", gcm_format, tex.width(), tex.height());
			return false;
		}

		const u32 width = mip0->width_in_texel;
		const u32 height = mip0->height_in_texel;

		if (width == 0 || height == 0 || (usz{width} * height * 4) > s_max_decoded_bytes)
		{
			++m_stats.unsupported;
			out.refusal_reason = "decoded-dims";
			note_refusal("decoded-dims", gcm_format, width, height);
			return false;
		}

		// The modder-facing identity: the raw guest bytes of mip 0, exactly as the title
		// wrote them, mixed with the interpretation that makes them an image.
		u64 hash = fnv_bytes(mip0->data.data<u8>(), mip0->data.size(), rpcs3::fnv_seed);
		hash = rpcs3::hash64(hash, u64{gcm_format});
		hash = rpcs3::hash64(hash, u64{width} | (u64{height} << 32));

		out.content_hash = hash ? hash : 1;
		out.fingerprint = (texture_rehash_mode() >= 2)
			? fnv_bytes(vm::_ptr<const u8>(address), static_cast<u32>(total_size), rpcs3::fnv_seed)
			: sample_fingerprint(vm::_ptr<const u8>(address), static_cast<u32>(total_size));
		out.width = width;
		out.height = height;
		out.pixels.assign(usz{width} * height * 4, 0);

		if (is_bc)
		{
			const u32 block_bytes = (gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT1) ? 8u : 16u;
			const u32 blocks_x = mip0->width_in_block;
			const u32 blocks_y = mip0->height_in_block;
			const u32 src_pitch = mip0->pitch_in_block;

			const u8* src = mip0->data.data<u8>();
			const usz src_size = mip0->data.size();

			for (u32 by = 0; by < blocks_y; ++by)
			{
				for (u32 bx = 0; bx < blocks_x; ++bx)
				{
					const usz src_offset = (usz{by} * src_pitch + bx) * block_bytes;

					if ((src_offset + block_bytes) > src_size)
					{
						continue;
					}

					// rpcs3's bcdec fork writes 0xAARRGGBB words (3rdparty/bcdec/bcdec.hpp
					// bcdec__color_block: refColors[n] = 0xFF000000 | (r << 16) | (g << 8) | b),
					// which on a little-endian host is already B,G,R,A in memory - the same
					// order this cache hands to the compositor and to CreateTexture. The stale
					// "0xAABBGGRR" comment is upstream bcdec's; rpcs3 swapped r and b in its
					// copy so that its own BGRA8 backends need no fixup. Re-swapping here was
					// the R/B inversion that made yellow read as cyan.
					u8 block[4 * 4 * 4];

					switch (gcm_format)
					{
					case CELL_GCM_TEXTURE_COMPRESSED_DXT1: bcdec_bc1(src + src_offset, block, 16); break;
					case CELL_GCM_TEXTURE_COMPRESSED_DXT23: bcdec_bc2(src + src_offset, block, 16); break;
					default: bcdec_bc3(src + src_offset, block, 16); break;
					}

					for (u32 y = 0; y < 4; ++y)
					{
						const u32 dst_y = (by * 4) + y;

						if (dst_y >= height)
						{
							break;
						}

						for (u32 x = 0; x < 4; ++x)
						{
							const u32 dst_x = (bx * 4) + x;

							if (dst_x >= width)
							{
								break;
							}

							const u8* texel = block + (y * 16) + (x * 4);
							u8* dst = out.pixels.data() + ((usz{dst_y} * width + dst_x) * 4);

							// Already B,G,R,A - straight copy.
							std::memcpy(dst, texel, 4);
						}
					}
				}
			}

			return true;
		}

		// Non-BC: let the shared decoder do the swizzle/byteswap work. alignment = 1 makes
		// every destination row tightly packed regardless of word size, which is what the
		// BGRA8 expansion below and the compositor both assume.
		rsx::texture_uploader_capabilities caps
		{
			.supports_byteswap = false,
			.supports_vtc_decoding = false,
			.supports_hw_deswizzle = false,
			.supports_zero_copy = false,
			.supports_dxt = false,
			.alignment = 1
		};

		if (direct)
		{
			rsx::io_buffer dst{ out.pixels.data(), out.pixels.size() };
			rsx::upload_texture_subresource(dst, *mip0, static_cast<int>(gcm_format), is_swizzled, caps);
			return true;
		}

		if (is_b8)
		{
			m_scratch.assign(usz{width} * height, 0);
			rsx::io_buffer dst{ m_scratch.data(), m_scratch.size() };
			rsx::upload_texture_subresource(dst, *mip0, static_cast<int>(gcm_format), is_swizzled, caps);

			for (usz i = 0, count = usz{width} * height; i < count; ++i)
			{
				const u8 v = m_scratch[i];
				u8* pixel = out.pixels.data() + (i * 4);
				pixel[0] = v;
				pixel[1] = v;
				pixel[2] = v;
				pixel[3] = 0xFF;
			}

			return true;
		}

		// 16-bit packed colour.
		m_scratch.assign(usz{width} * height * 2, 0);
		rsx::io_buffer dst{ m_scratch.data(), m_scratch.size() };
		rsx::upload_texture_subresource(dst, *mip0, static_cast<int>(gcm_format), is_swizzled, caps);

		for (usz i = 0, count = usz{width} * height; i < count; ++i)
		{
			u16 word;
			std::memcpy(&word, m_scratch.data() + (i * 2), sizeof(u16));

			const u32 bgra = expand(word);
			std::memcpy(out.pixels.data() + (i * 4), &bgra, sizeof(u32));
		}

		return true;
	}

	// ROUND 39. Moved out of upload() verbatim so promote_sky_emissive() can re-run it on a
	// texture that was already uploaded before its dome-ness was known. Body unchanged; the only
	// edit is the enclosing signature and the loss of one level of indentation is deliberately NOT
	// taken, so a diff of this block against round 38 is empty.
	void texture_cache::measure_peak_uv(texture_entry& entry)
	{
		// --- round 23: where the sun sits INSIDE the sky dome's texture ------------------------
		// Only for the dome textures RPCS3_REMIX_SKYEMISSIVE names. Two extra walks of the buffer
		// for one or two textures per level, once per upload; every other texture skips the block
		// on a <=8-entry hash compare. See texture_entry::peak_uv for what is measured and why the
		// centroid is used instead of the single brightest texel.
		if (entry.width != 0 && entry.height != 0
			&& entry.pixels.size() >= usz{entry.width} * entry.height * 4
			&& sky_emissive_albedo_matches(entry.content_hash))
		{
			const usz count = usz{entry.width} * entry.height;

			f32 peak = -1.f;

			for (usz t = 0; t < count; ++t)
			{
				// BGRA8: index 0 is blue. Rec.709, the same weights the guest-light path uses.
				const f32 luma = (0.2126f * static_cast<f32>(entry.pixels[(t * 4) + 2]))
					+ (0.7152f * static_cast<f32>(entry.pixels[(t * 4) + 1]))
					+ (0.0722f * static_cast<f32>(entry.pixels[t * 4]));

				peak = std::max(peak, luma);
			}

			if (peak > 0.f)
			{
				// Round 24: RPCS3_REMIX_SUNSKYPEAKFRAC, default 98 = the round-23 constant exactly.
				// On the Selva dome the 98 window catches 112 texels on a SINGLE row at the top edge
				// of the panorama band, which drags the centroid to v=0.50146; widening the window
				// is the lever for that without moving the dome that already solves correctly.
				// DIVIDE, not multiply by 0.01f. Review defect, MEASURED in IEEE-754 single:
				// 0.98f is 0x3F7AE148 but 98.f * 0.01f is 0x3F7AE147, one ULP LOW - because 0.01f is
				// itself 0.00999999977648..., so the product rounds down past 0.98f. That would
				// break this knob's whole contract ("98 reproduces the round-23 constant exactly")
				// on the one feature whose justification is that it must not move the dome that
				// already solves correctly. IEEE division is correctly rounded, so p/100 rounds to
				// the nearest representable value by definition. Checked across the clamp range:
				// the multiply form is wrong at 85 AND 98; the divide form is exact at 50, 85, 90,
				// 98 and 100.
				const f32 threshold = peak
					* (static_cast<f32>(remix_rsx::sun_sky_peak_percent()) / 100.f);

				f64 weight = 0.0;
				f64 sum_x = 0.0;
				f64 sum_y = 0.0;
				u32 hits = 0;

				for (usz t = 0; t < count; ++t)
				{
					const f32 luma = (0.2126f * static_cast<f32>(entry.pixels[(t * 4) + 2]))
						+ (0.7152f * static_cast<f32>(entry.pixels[(t * 4) + 1]))
						+ (0.0722f * static_cast<f32>(entry.pixels[t * 4]));

					if (luma < threshold)
					{
						continue;
					}

					const f64 w = static_cast<f64>(luma);
					sum_x += w * (static_cast<f64>(t % entry.width) + 0.5);
					sum_y += w * (static_cast<f64>(t / entry.width) + 0.5);
					weight += w;
					++hits;
				}

				if (weight > 0.0)
				{
					entry.peak_uv[0] = static_cast<f32>((sum_x / weight) / static_cast<f64>(entry.width));
					entry.peak_uv[1] = static_cast<f32>((sum_y / weight) / static_cast<f64>(entry.height));
					entry.peak_luma = peak;
					entry.peak_texels = hits;
				}
			}
		}

	}

	// ROUND 39. Moved out of upload() so it has two callers: upload() as before, and
	// promote_sky_emissive(), which rebuilds ONLY the material for a texture that has just been
	// classified as a sky dome. Body unchanged except the CreateMaterial failure arm, which no
	// longer destroys the texture - see the comment there.
	bool texture_cache::build_material(const remixapi_Interface& api, texture_entry& entry)
	{
		// The fork resolves this pseudo-path against the texture manager's hash table, which
		// remixapi_CreateTexture just populated with entry.content_hash
		// (rtx_fork_api_entry.cpp textureHashPathLookup). That is what puts an API texture in
		// the same hash namespace as a native D3D9 one.
		wchar_t albedo_path[32]{};
		::swprintf_s(albedo_path, L"0x%016llX", entry.content_hash);

		remixapi_MaterialInfoOpaqueEXT opaque{};
		opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
		opaque.pNext = nullptr;
		opaque.roughnessTexture = nullptr;
		opaque.metallicTexture = nullptr;
		opaque.anisotropy = 0.f;
		opaque.albedoConstant = { 1.f, 1.f, 1.f };
		opaque.opacityConstant = 1.f;
		opaque.roughnessConstant = 0.7f;
		opaque.metallicConstant = 0.f;
		opaque.thinFilmThickness_hasvalue = 0;
		opaque.thinFilmThickness_value = 0.f;
		opaque.alphaIsThinFilmThickness = 0;
		opaque.heightTexture = nullptr;
		opaque.displaceIn = 0.f;
		// 1 means "read the alpha state from remixapi_InstanceInfoBlendEXT", which as of this
		// change the instance path does chain (RemixGSRender::submit_subdraw). It has to be 1
		// for blending to exist at all: calculateAlphaState() only looks at
		// DrawCallState::materialData.blendMode - the ext's landing site - when
		// UseLegacyAlphaState is set (rtx_instance_manager.cpp:705-709), and
		// remixapi_MaterialInfoOpaqueEXT has no blend-factor fields of its own to use instead.
		//
		// It also moves the alpha *test* to the instance (same option gates :687-693), which is
		// strictly the better home for it: this material is cached per texture descriptor while
		// alpha test is per draw. The two fields below stay populated because they are what the
		// runtime falls back to with the knob off, and because entry.alpha_func/alpha_ref are
		// still part of the cache key - a texture drawn with two different alpha funcs still
		// makes two materials, which is now redundant but harmless.
		//
		// Translucency deliberately does NOT become remixapi_MaterialInfoTranslucentEXT: that
		// type is refractive glass, and calculateAlphaState() returns early for it
		// (rtx_instance_manager.cpp:659-667) without ever reading the draw's blend state. An
		// opaque material plus a per-instance blend ext is what the runtime's own D3D9 path
		// produces for an alpha-blended draw, so the per-texture material can stay per-texture
		// and nothing about the blend state needs to enter the material key.
		opaque.useDrawCallAlphaState = blend_state_enabled() ? 1u : 0u;
		opaque.blendType_hasvalue = 0;
		opaque.blendType_value = 0;
		opaque.invertedBlend = 0;
		opaque.alphaTestType = static_cast<int>(entry.alpha_func);
		opaque.alphaReferenceValue = entry.alpha_ref;
		opaque.displaceOut = 0.f;

		remixapi_MaterialInfo material{};
		material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
		material.pNext = &opaque;
		// Keep the long-standing content hash for the default sampler state, but give every
		// non-default sampler/alpha variant its own material identity. Texture bytes alone are
		// not sufficient here: the same atlas is legitimately used as clamp, repeat, and mirror,
		// and aliasing those CreateMaterial definitions made the winning wrap state draw-order
		// dependent.
		u64 material_hash = entry.content_hash;
		if (entry.wrap_u != 1 || entry.wrap_v != 1 || entry.alpha_func != 7 || entry.alpha_ref != 0)
		{
			const u8 material_state[] = { entry.wrap_u, entry.wrap_v, entry.alpha_func, entry.alpha_ref };
			material_hash = fnv_bytes(material_state, sizeof(material_state), entry.content_hash);
			if (material_hash == 0)
			{
				material_hash = 1;
			}
		}

		// --- ROUND 39, AND THIS IS WHAT MAKES THE PROMOTION VISIBLE AT ALL --------------------
		// promote_sky_emissive() calls this function a SECOND time for a texture that already has
		// a material, to give it the emissive definition. Every input to material_hash above is
		// unchanged by a promotion - content hash, wrap modes, alpha func and ref are all
		// properties of the texture, not of the classification - so without this fold the second
		// CreateMaterial would declare a DIFFERENT definition under the SAME hash.
		//
		// The comment twelve lines above records what happens then, as measured fact rather than
		// as a worry: aliasing CreateMaterial definitions made the winning wrap state DRAW-ORDER
		// DEPENDENT. If the first (non-emissive) definition won, the dome would stay black while
		// skyclassify_promoted, skyclassify_entries and mat_skyemissive all reported success -
		// a counter reporting a change that never reached the screen, which is this project's
		// most expensive recurring failure. None of round 39's new counters can see it.
		//
		// Applied ON TOP of the wrap/alpha fold and ONLY for a promoted hash, deliberately. A
		// fifth byte added to material_state unconditionally would move the identity of every
		// non-default-wrap material in the title, and material_hash is the MODDING SURFACE - it
		// is the 'mat_<HASH>' a replacement in mod.usda targets. Unpromoted materials keep the
		// hash they have always had, bit for bit.
		//
		// CONSEQUENCE, stated because it is a real one: a promoted dome's material hash is no
		// longer equal to its albedo content hash. promote_sky_emissive() logs both values per
		// rebuilt entry so the new identity is discoverable rather than merely different.
		if (sky_emissive_promoted(entry.content_hash))
		{
			static constexpr u8 sky_tag[] = { 'S', 'K', 'Y', 'E' };
			material_hash = fnv_bytes(sky_tag, sizeof(sky_tag), material_hash);
			if (material_hash == 0)
			{
				material_hash = 1;
			}
		}
		material.hash = material_hash;
		// ROUND 39. Recorded before CreateMaterial so a caller can compare it across a rebuild.
		entry.material_hash = material_hash;
		material.albedoTexture = albedo_path;
		material.normalTexture = nullptr;
		material.tangentTexture = nullptr;
		material.emissiveTexture = nullptr;
		material.emissiveIntensity = 0.f;
		material.emissiveColorConstant = { 0.f, 0.f, 0.f };

		// RPCS3_REMIX_EMISSIVE. The list is latched once and materials are cache-keyed by content +
		// sampler/alpha state, so listing a hash cannot change what any *other* material is, and
		// cache coherence across the run is unaffected: a listed hash is emissive from the first
		// CreateMaterial to the last.
		//
		// This is the fixture's own surface glowing, which is the half of the lighting work with no
		// embedding problem - it emits from exactly the geometry the game drew, so a lamp shade
		// cannot occlude it. The sphere light injected on the render side carries the photometric
		// load; this carries the look.
		// Round 13: RPCS3_REMIX_SKYEMISSIVE is the same mechanism on a second list with a second
		// intensity - see the sky_emissive_albedo_matches() doc block. The fixture list wins a tie
		// because it is the older statement and because a hash on both lists is a configuration
		// mistake that should behave predictably rather than pick by list order.
		const bool fixture_emissive = emissive_albedo_matches(entry.content_hash);
		const bool sky_emissive = !fixture_emissive && sky_emissive_albedo_matches(entry.content_hash);

		// Round 14, ITEM 2: the sun card renders as a hard opaque yellow rectangle with visible
		// edges. This is the SAME mechanism as the sky dome above, on the SUNCARDALBEDO list and with
		// its own intensity - deliberately a third membership test feeding one code path rather than
		// a second implementation. For a glare card BlendType::kEmissive is the whole fix and not an
		// approximation of one: calcOpaqueSurfaceMaterialOpacity's kEmissive arm drives opacity to 0
		// with emissive influence 1, so the card's dark texels stop being drawn (black added is
		// nothing), its rectangle stops occluding what is behind it, and only the bright core emits.
		//
		// Why this and not a per-vertex alpha fold in the shape of apply_haze_fade, which is what
		// round 14's brief asked for: a fold replays a SPECIFIC fragment program's arithmetic, and
		// this round could not identify the sun card's fragment program at all. The six captured
		// bin\remix_ucode\*.fp files belong to three ordinary world-geometry vertex programs
		// (0214281b9a7a412d, ad7ce9d672a0bf6b, d0b6a471bb2d463b), not to a sun sprite. Inventing the
		// arithmetic of a program nobody has read is exactly the failure round 11 documented.
		const bool sun_card_emissive = !fixture_emissive && !sky_emissive
			&& sun_card_emissive_enabled() && sun_card_albedo_matches(entry.content_hash);

		// The two share every line below: per-texel emission plus an emissive blend type. Only the
		// intensity knob and the counter differ.
		const bool per_texel_emissive = sky_emissive || sun_card_emissive;

		if (fixture_emissive || per_texel_emissive)
		{
			// ROUND 30: emissive_intensity_for(), not emissive_intensity(). The fixture list now
			// accepts EMISSIVE=<hash>:<intensity> per entry and falls back to the global for any entry
			// that did not give one, so this line is bit-identical for a colon-free launcher and is the
			// only change needed to make one bulb hash brighter without touching the others. The
			// material cache is keyed on content + sampler/alpha state (see :1203) and the intensity is
			// a pure function of the content hash, so no cache-key change is needed: the same texture
			// always resolves to the same intensity for the whole run.
			material.emissiveIntensity = sun_card_emissive
				? sun_card_emissive_intensity()
				: (sky_emissive ? sky_emissive_intensity() : emissive_intensity_for(entry.content_hash));
			// The fixture's own mean colour, normalised so the largest component is 1: intensity is
			// the knob, hue is the texture's. mean_rgb defaults to white, so a texture that never
			// reached the measurement pass glows white rather than black.
			const f32 peak = std::max({ entry.mean_rgb[0], entry.mean_rgb[1], entry.mean_rgb[2] });
			if (peak > 1e-4f)
			{
				material.emissiveColorConstant = {
					entry.mean_rgb[0] / peak, entry.mean_rgb[1] / peak, entry.mean_rgb[2] / peak };
			}
			else
			{
				material.emissiveColorConstant = { 1.f, 1.f, 1.f };
			}

			++m_stats.materials_emissive;

			if (per_texel_emissive)
			{
				// The sky's emissive colour must be PER-TEXEL, not the flat mean the fixture path
				// uses. A lamp is one colour and a sky is a gradient with a bright side; a flat
				// dome is the one result that would look worse than today's.
				//
				// The runtime's own WorldUI arm does exactly this -
				// rtx_instance_manager.cpp:1106 sets the emissive colour texture to the albedo
				// texture - and it is reachable from the API because both are resolved through the
				// same synthetic "0x<hash>" path (textureHashPathLookup). Without it the shader
				// takes emissiveColorConstant instead: opaque_surface_material_interaction.slangh
				// :623-632 reads the constant and only overwrites it when an emissive texture
				// loaded, and it does NOT fall back to the albedo or to the vertex-colour arg
				// source. So this line is the whole difference between a sky and a coloured shell.
				material.emissiveTexture = albedo_path;
				material.emissiveColorConstant = { 1.f, 1.f, 1.f };

				// Counted apart so 'the dome attached' and 'the sun card attached' are two numbers on
				// the live line. A round that cannot tell those apart cannot attribute either.
				if (sky_emissive)
				{
					++m_stats.materials_sky_emissive;
				}
				else
				{
					++m_stats.materials_sun_card;
				}

				// --- the half that gives the sun back ------------------------------------------
				// An emissive dome that is still an OPAQUE shell around the camera is worse than
				// useless: rtx_instance_manager.cpp:1283 gives a non-blended instance
				// OBJECT_MASK_OPAQUE, and integrator_direct.slangh:101 traces the direct shadow ray
				// against exactly that mask - so the dome blocks 100% of the fallback distant sun
				// and the only light left in the scene is the dome's own. That is the user's
				// complaint ("the sky lights up the environment") stated as a mechanism.
				//
				// The escape is an EMISSIVE blend type. rtx_instance_manager.cpp:1216 sets
				// m_isUnordered on alphaState.emissiveBlend, :1270 then gives the instance
				// OBJECT_MASK_UNORDERED_ALL_EMISSIVE, and those bits are deliberately absent from
				// OBJECT_MASK_ALL_STANDARD (instance_definitions.h:93-95) - so the shadow ray
				// misses the dome entirely while primary rays still see it. It is also what
				// calcOpaqueSurfaceMaterialOpacity's kEmissive arm already documented elsewhere in
				// this backend: opacity -> 0, emissive influence 1, occludes nothing, contributes
				// only its emissive radiance. Which is what a sky IS.
				//
				// Declared on the MATERIAL rather than pushed through the instance's blend ext, and
				// that is the safer of the two routes the runtime offers. With
				// useDrawCallAlphaState = 0 the runtime takes calculateAlphaState's
				// '!useLegacyAlphaState' arm (rtx_instance_manager.cpp:704-707) and reads
				// getBlendEnabled()/getBlendType() straight off this material - no dependence on
				// blend-factor pattern matching, and no dependence on
				// rtx.enableEmissiveBlendModeTranslation being left on. blendType_hasvalue IS the
				// blend enable on this path (rtx_remix_api.cpp:511-512), which is why it is set
				// alongside the value rather than instead of it.
				//
				// 6 == BlendType::kEmissive (surface_shared.h:24-39, verified against
				// isBlendTypeEmissive in rtx_materials.h:49-60). Hard-coded as an integer because
				// remix_c.h carries the field as an int and does not export the enum.
				// For the DOME this is severable (SKYEMISSIVEBLEND), because emissive-and-occluding is
				// a state somebody may want. For the SUN CARD it is not severable and deliberately
				// has no second knob: the blend type IS the fix, and SUNCARDEMISSIVE=0 already
				// restores the card bit-for-bit.
				if (!sky_emissive || sky_emissive_blend_enabled())
				{
					opaque.useDrawCallAlphaState = 0u;
					opaque.blendType_hasvalue = 1;
					opaque.blendType_value = 6;
					++m_stats.materials_sky_unordered;
				}
			}
		}
		material.spriteSheetRow = 1;
		material.spriteSheetCol = 1;
		material.spriteSheetFps = 0;
		material.filterMode = 1; // Linear
		material.wrapModeU = entry.wrap_u;
		material.wrapModeV = entry.wrap_v;

		const u32 mat_status = guarded_create_material(api.CreateMaterial, &material, &entry.material);

		if (mat_status != REMIXAPI_ERROR_CODE_SUCCESS || !entry.material)
		{
			// ROUND 39. The texture is NOT destroyed here any more, because this body now has a
			// second caller (promote_sky_emissive) for which the texture is already live and
			// shared. upload() does the destroy on a false return, which is where it always
			// belonged: this function creates a material and nothing else.
			rsx_log.error("Remix: CreateMaterial failed for hash %016llx (%s)",
				entry.content_hash, error_name(mat_status));
			entry.material = nullptr;
			return false;
		}

		++m_stats.materials;
		return true;
	}

	bool texture_cache::upload(const remixapi_Interface& api, texture_entry& entry)
	{
		// --- round 7: RPCS3_REMIX_CLAMPALBEDO, the UI seam lever ----------------------------------
		// Applied HERE and not at the entry.wrap_* assignment: the list is keyed on the CONTENT
		// hash, which only exists once the guest bytes have been decoded and hashed. Everything
		// downstream reads entry.wrap_u/wrap_v and therefore picks this up for free - the material
		// identity below folds non-default wrap into material_hash (so the clamped variant cannot
		// alias the repeat one), material.wrapModeU/V carry it to the GPU sampler for the
		// world-space-UI route, and the CPU compositor's address_coordinate reads the same two
		// fields for the 2D route. One list entry closes both routes for that texture.
		//
		// Overriding real guest state is normally the bug class this backend exists to avoid, which
		// is why this is a manual, empty-by-default list rather than a heuristic: the guest's wrap
		// mode is correctly decoded and faithfully replayed, and the only thing wrong with it is
		// that a nearest-sampled atlas has no bilinear seam bound the way real RSX does.
		//
		// Round 8: the counter says LISTED-AND-CREATED, not "the wrap actually moved". Two ways it
		// used to lie, both of which make tex_wrap_forced=0 unreadable against its own header
		// comment ("separates a typo'd hash from a list that is simply empty"): it skipped any
		// texture the guest had already bound CLAMP (a correct list entry, zero count), and it
		// counted before CreateTexture, so an upload that failed still counted. Incremented below,
		// beside m_stats.created.
		const bool clamp_listed = clamp_albedo_matches(entry.content_hash);

		if (clamp_listed)
		{
			entry.wrap_u = 0;
			entry.wrap_v = 0;
		}

		const remixapi_Format format = textures_linear()
			? REMIXAPI_FORMAT_B8G8R8A8_UNORM
			: REMIXAPI_FORMAT_B8G8R8A8_SRGB;

		// Measured here rather than in the decoders: every format path converges on this buffer,
		// so one pass covers BC, direct, B8 and the 16-bit expansions without touching any of
		// them. Once per upload, not per draw.
		{
			u8 lo = 255;
			u8 hi = 0;

			// Mean colour accumulated in the same walk. u64 sums: a 2048x2048 texture is 4.2M
			// texels, and 4.2M * 255 overflows u32 on the first channel.
			u64 sum_b = 0;
			u64 sum_g = 0;
			u64 sum_r = 0;
			u64 texels = 0;

			for (usz i = 3; i < entry.pixels.size(); i += 4)
			{
				const u8 a = entry.pixels[i];
				lo = std::min(lo, a);
				hi = std::max(hi, a);

				// BGRA8 - the upload format below is B8G8R8A8, so index 0 is blue.
				sum_b += entry.pixels[i - 3];
				sum_g += entry.pixels[i - 2];
				sum_r += entry.pixels[i - 1];
				++texels;
			}

			entry.alpha_min = lo;
			entry.alpha_max = hi;

			if (texels != 0)
			{
				const f64 scale = 1.0 / (255.0 * static_cast<f64>(texels));
				entry.mean_rgb[0] = static_cast<f32>(static_cast<f64>(sum_r) * scale);
				entry.mean_rgb[1] = static_cast<f32>(static_cast<f64>(sum_g) * scale);
				entry.mean_rgb[2] = static_cast<f32>(static_cast<f64>(sum_b) * scale);
			}
		}

		measure_peak_uv(entry);

		remixapi_TextureInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
		info.pNext = nullptr;
		info.hash = entry.content_hash;
		info.width = entry.width;
		info.height = entry.height;
		info.depth = 1;
		info.mipLevels = 1;
		info.format = format;
		info.data = entry.pixels.data();
		info.dataSize = entry.pixels.size();

		const u32 tex_status = guarded_create_texture(api.CreateTexture, &info, &entry.texture);

		if (tex_status != REMIXAPI_ERROR_CODE_SUCCESS || !entry.texture)
		{
			rsx_log.error("Remix: CreateTexture failed for %ux%u hash %016llx (%s)",
				entry.width, entry.height, entry.content_hash, error_name(tex_status));
			entry.texture = nullptr;
			return false;
		}

		++m_stats.created;

		if (clamp_listed)
		{
			++m_stats.wrap_forced;
		}

		if (!build_material(api, entry))
		{
			guarded_destroy_texture(api.DestroyTexture, entry.texture);
			entry.texture = nullptr;
			entry.material = nullptr;
			++m_stats.destroyed;
			return false;
		}

		return true;
	}

	// --- ROUND 39: give an already-uploaded texture the sky dome's material -----------------------
	//
	// The ordering this exists to fix. A dome's texture is uploaded - and its material created - on
	// the FIRST draw that binds it, which is strictly before submit_subdraw() has the world-space
	// AABB it needs to decide the draw is a dome. So by the time the classifier says "this is a
	// sky", the material already exists and is not emissive, and every later bind() is a cache hit
	// that returns that same handle. Without this the promotion would be a no-op for the whole run.
	//
	// The old material handle is ORPHANED, never destroyed. RemixGSRender::mesh_entry::material
	// records why in full: "The Remix mesh object bakes its surface's material at CreateMesh and
	// there is no API to re-point it afterwards", so meshes created before this call are still
	// submitting the old handle and destroying it is the dangling-handle failure the round-9 reap
	// split exists to prevent. Round 17 already established orphaning as this cache's answer to
	// exactly that (the TEXSTALEEVICT arm), and priced it: one material per event. Here the event
	// count is bounded by the number of distinct domes the title has, which haze_domes.csv puts at
	// 16 - so the whole lifetime cost is at most a few dozen orphaned materials.
	//
	// The mesh side is closed separately and MUST be, or this function's work is invisible: the
	// mesh key folds sky_emissive_promoted() so a promoted albedo re-keys once and the next draw
	// creates a mesh carrying the NEW material. See the fold in RemixGSRender's mesh key build.
	u32 texture_cache::promote_sky_emissive(const remixapi_Interface& api, u64 content_hash)
	{
		if (content_hash == 0)
		{
			return 0;
		}

		u32 rebuilt = 0;

		// A linear walk of the whole cache, once per newly-classified dome. m_entries is keyed on
		// the texture DESCRIPTOR, not the content hash, and this title puts ~45 live entries on one
		// content hash (tex_key_dup 11,212 of tex_created 11,467), so a keyed lookup is not
		// available and every one of those aliases needs the new material or the dome flickers
		// between them.
		for (auto& [key, entry] : m_entries)
		{
			if (entry.content_hash != content_hash || entry.unsupported || !entry.texture)
			{
				continue;
			}

			// peak_uv is what gates derive_sky_sun(), and its walk in upload() was skipped for this
			// texture because the hash was not on the list yet. Re-take it now; the CPU pixels are
			// still resident (the cache keeps them for the compositor).
			measure_peak_uv(entry);

			remixapi_MaterialHandle previous = entry.material;

			const u64 previous_hash = entry.material_hash;

			if (!build_material(api, entry))
			{
				// build_material has already nulled entry.material. Put the old one back rather
				// than leaving the entry material-less: a stale emissive-less material is a dome
				// that stays black, an absent one is a draw that renders untextured grey.
				entry.material = previous;
				entry.material_hash = previous_hash;
				++m_stats.sky_promote_failed;
				continue;
			}

			// ROUND 39. The two hashes MUST differ, and this line is the proof rather than the
			// assumption. build_material folds a promoted hash's identity so the second
			// CreateMaterial cannot alias the first - without that fold the runtime is handed two
			// different definitions under one hash and which one wins is draw-order dependent,
			// i.e. the dome could stay black while every counter reported success. Printed once
			// per rebuilt entry, and it is also how a modder finds the promoted dome's new
			// mat_<HASH>, which is no longer equal to its albedo content hash.
			rsx_log.notice("Remix skypromote: content=%016llX mat %016llX -> %016llX %s (%ux%u)",
				entry.content_hash, previous_hash, entry.material_hash,
				(previous_hash == entry.material_hash) ? "IDENTICAL - THE FOLD DID NOT FIRE" : "ok",
				entry.width, entry.height);

			if (previous && previous != entry.material)
			{
				++m_stats.sky_promote_orphans;
			}

			++m_stats.sky_promote_entries;
			++rebuilt;
		}

		return rebuilt;
	}

	bool texture_cache::has_idle(u64 frame) const
	{
		const u64 idle_frames = texture_idle_frames();

		if (m_entries.empty() || frame < idle_frames)
		{
			return false;
		}

		const u64 cutoff = frame - idle_frames;

		for (const auto& [key, entry] : m_entries)
		{
			if (entry.last_used_frame <= cutoff)
			{
				return true;
			}
		}

		return false;
	}

	void texture_cache::reap(const remixapi_Interface& api, u64 frame,
		const std::unordered_set<const void*>* live_materials)
	{
		const u64 idle_frames = texture_idle_frames();

		if (m_entries.empty() || frame < idle_frames)
		{
			return;
		}

		const u64 cutoff = frame - idle_frames;

		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			if (it->second.last_used_frame > cutoff)
			{
				++it;
				continue;
			}

			// Round 9: the material-lifetime hole.
			//
			// bind() stamps last_used_frame on every descriptor HIT, so an entry only reaches this
			// point when nothing has bound it for TEXIDLE frames. That is NOT the same as "nothing
			// is drawing it": the mesh cache bakes the material handle into the Remix mesh at
			// CreateMesh and keys the mesh on its CONTENT, so a mesh whose geometry has not changed
			// is reused rather than rebuilt - and it keeps whatever material handle it was created
			// with. Destroying that material leaves the mesh holding a dead handle that no later
			// bind can ever replace, because the mesh key never changes. A permanently white
			// surface; and if the runtime recycles the freed handle value, a wrongly-textured one.
			//
			// Keeping the entry re-ages it (last_used_frame bumped to now) so it reaps normally
			// once its mesh is itself reaped by MESHIDLE. The retained set is bounded by the
			// materials of live meshes - i.e. what is on screen - which has to stay resident
			// anyway.
			if (live_materials && it->second.material
				&& live_materials->contains(static_cast<const void*>(it->second.material)))
			{
				it->second.last_used_frame = frame;
				++m_stats.reap_kept;
				++it;
				continue;
			}

			if (it->second.material)
			{
				guarded_destroy_material(api.DestroyMaterial, it->second.material);
			}

			if (it->second.texture)
			{
				guarded_destroy_texture(api.DestroyTexture, it->second.texture);
				++m_stats.destroyed;
			}

			++m_stats.reap_freed;
			it = m_entries.erase(it);
		}
	}

	void texture_cache::destroy_all(const remixapi_Interface& api)
	{
		for (auto& [key, entry] : m_entries)
		{
			if (entry.material)
			{
				guarded_destroy_material(api.DestroyMaterial, entry.material);
			}

			if (entry.texture)
			{
				guarded_destroy_texture(api.DestroyTexture, entry.texture);
			}
		}

		m_entries.clear();
	}
}

#endif
