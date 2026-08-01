#pragma once

#ifdef _WIN32

#include "util/types.hpp"

#include "Emu/RSX/Remix/remix_c.h"

#include <unordered_map>
#include <vector>

namespace rsx
{
	class fragment_texture;
}

namespace remix_rsx
{
	// Everything the RSX texture registers say about one bound unit. Two draws with an
	// identical descriptor read the same guest bytes with the same interpretation, so this is
	// the cache key; the content hash below is what a modder actually sees.
	struct texture_descriptor
	{
		u32 offset = 0;
		u32 location = 0;
		u32 format = 0;
		u32 pitch = 0;
		u16 width = 0;
		u16 height = 0;
		u16 depth = 0;
		u8 mipmaps = 0;
		u8 border = 0;
		u8 wrap_s = 0;
		u8 wrap_t = 0;
		u8 cubemap = 0;
		u8 dimension = 0;

		bool operator==(const texture_descriptor&) const = default;
		u64 key() const;
	};

	// One decoded + uploaded texture and the material that names it.
	struct texture_entry
	{
		remixapi_TextureHandle texture = nullptr;
		remixapi_MaterialHandle material = nullptr;

		// FNV-1a over the raw guest mip-0 bytes, mixed with format and dimensions. This is
		// the modder-facing identity: same content => same value across runs.
		u64 content_hash = 0;

		// Cheap strided sample of the same bytes, re-taken every draw so a texture updated
		// in place (animated water/fire) is noticed without rehashing megabytes.
		u64 fingerprint = 0;

		u32 width = 0;
		u32 height = 0;

		// Remix wrap-mode enumerants derived from the RSX sampler state.
		u8 wrap_u = 1;
		u8 wrap_v = 1;

		// Decoded BGRA8, kept so the UI compositor can sample it CPU-side.
		std::vector<u8> pixels;

		u64 last_used_frame = 0;
		bool unsupported = false;
	};

	struct texture_stats
	{
		u64 created = 0;
		u64 destroyed = 0;
		u64 hits = 0;
		u64 deferred = 0;
		u64 unreadable = 0;
		u64 unsupported = 0;
		u64 rehashed = 0;
		u64 materials = 0;
	};

	// Per-draw albedo texture cache. Owns every remixapi texture and material it creates.
	class texture_cache
	{
	public:
		texture_cache() = default;
		texture_cache(const texture_cache&) = delete;
		texture_cache& operator=(const texture_cache&) = delete;

		// Called once per flip: resets the per-frame CreateTexture budget.
		void begin_frame();

		// Resolves the bound unit to a material. Returns nullptr when the draw should be
		// submitted without one (unreadable, unsupported, over budget or disabled).
		// 'out_entry' is set on success and stays valid until the next reap.
		remixapi_MaterialHandle bind(const remixapi_Interface& api,
			const rsx::fragment_texture& tex,
			u64 frame,
			const texture_entry** out_entry);

		void reap(const remixapi_Interface& api, u64 frame);
		void destroy_all(const remixapi_Interface& api);

		usz live() const { return m_entries.size(); }
		const texture_stats& stats() const { return m_stats; }

	private:
		bool decode(const rsx::fragment_texture& tex, texture_entry& out);
		bool upload(const remixapi_Interface& api, texture_entry& entry);

		std::unordered_map<u64, texture_entry> m_entries;
		texture_stats m_stats{};
		u32 m_budget_left = 0;

		// Reused across draws to keep the hot path allocation free.
		std::vector<u8> m_scratch;
	};

	// Env knobs, read once. RPCS3_REMIX_NOTEX=1 disables the whole workstream,
	// RPCS3_REMIX_TEXBUDGET caps CreateTexture calls per frame, RPCS3_REMIX_TEXLINEAR=1
	// uploads UNORM instead of SRGB, RPCS3_REMIX_TEXREHASH picks the staleness policy
	// (0 = descriptor only, 1 = sampled fingerprint (default), 2 = full hash every draw).
	bool textures_disabled();
	u32 texture_budget();
	bool textures_linear();
	u32 texture_rehash_mode();
}

#endif
