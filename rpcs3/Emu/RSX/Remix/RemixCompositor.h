#pragma once

#ifdef _WIN32

#include "util/types.hpp"

#include "Emu/RSX/Remix/remix_c.h"

#include <vector>

namespace remix_rsx
{
	struct texture_entry;

	// NOT WIRED YET. This translation unit compiles and is registered in all three build
	// systems, but nothing calls it: the two producers it exists for -- the title's own
	// screen-space draws (M3 step 5) and rpcs3's compiled_resource overlay (M3 step 6) --
	// were deferred to M4. Do not assume any of it has been exercised at runtime.
	//
	// CPU rasterizer for everything that must appear as flat 2D on top of the path-traced
	// image: the title's own screen-space draws and rpcs3's native overlay. The whole buffer
	// is handed to the fork's DrawScreenOverlay once per flip, which composites it with a
	// compute pass after tone mapping.
	class compositor
	{
	public:
		// Sizes and clears the target if anything was drawn last frame.
		void begin_frame(u32 width, u32 height);

		// One textured/tinted triangle in pixel space, top-left origin.
		// 'tex' may be null, in which case only 'tint' is used.
		void draw_triangle(const f32 (&x)[3], const f32 (&y)[3], const f32 (&u)[3], const f32 (&v)[3],
			const texture_entry* tex, u32 tint_bgra, bool clamp_uv);

		// Axis-aligned textured quad helper for rpcs3's overlay quads.
		void draw_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1,
			const texture_entry* tex, u32 tint_bgra, bool clamp_uv);

		// Single-channel coverage source (rpcs3's font atlas is R8).
		void draw_glyph_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1,
			const u8* coverage, u32 cov_width, u32 cov_height, u32 tint_bgra);

		void set_clip(f32 x0, f32 y0, f32 x1, f32 y1);
		void clear_clip();

		bool dirty() const { return m_dirty; }
		u32 width() const { return m_width; }
		u32 height() const { return m_height; }

		// Submits the buffer if anything was rasterized. Returns the API status, or SUCCESS
		// when there was nothing to do.
		u32 submit(const remixapi_Interface& api);

		u64 draws() const { return m_draws; }
		u64 frames() const { return m_frames; }

	private:
		void blend(u32 x, u32 y, u32 src_bgra);

		std::vector<u8> m_buffer;
		u32 m_width = 0;
		u32 m_height = 0;
		bool m_dirty = false;

		f32 m_clip[4] = { 0.f, 0.f, 0.f, 0.f };
		bool m_clip_enabled = false;

		u64 m_draws = 0;
		u64 m_frames = 0;
	};

	// RPCS3_REMIX_NOUI=1 disables the compositor entirely.
	bool compositor_disabled();
}

#endif
