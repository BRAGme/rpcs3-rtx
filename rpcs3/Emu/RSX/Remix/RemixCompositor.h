#pragma once

#ifdef _WIN32

#include "util/types.hpp"

#include "Emu/RSX/Remix/remix_c.h"

#include <array>
#include <vector>

namespace remix_rsx
{
	struct texture_entry;

	// Round 7. Which addressing path every sampled UI texel took, counted where the outcome is
	// actually decided (per axis, inside address_coordinate) rather than inferred from draw state.
	// This is the partition that says whether the seam fix below is firing and on how much: 'in'
	// dominant with 'seam' non-zero is the fix working; 'wrap' non-zero names a genuinely tiling
	// UI draw, which is the population the rule must NOT touch; 'clamp' is the native rpcs3
	// overlay and any clamp-bound texture; 'mirror' and 'clip' must not move at all.
	struct uv_address_counters
	{
		u64 in = 0;
		u64 wrap = 0;
		u64 seam = 0;
		u64 mirror = 0;
		u64 clip = 0;
		u64 clamp = 0;

		// Round 26. The first six are per-SAMPLE; these two are per-PRIMITIVE and share the struct
		// only because it is the one channel that already carries compositor state out to a log
		// line - specifically `Remix live:` (in remix_dump.log), NOT `Remix stats:`, which carries
		// no uv_counters field at all.
		//
		// rect_shrunk   = primitives the UIRECTSHRINK rule actually corrected, counted beside
		//                 m_draws so a primitive dropped for degenerate area or an off-screen
		//                 bounding box is not counted. In practice these are TRIANGLES, two per
		//                 glyph quad, because the only reachable call site is draw_triangle.
		// rect_declined = primitives that matched the hash and the span guard but were NOT one half
		//                 of an axis-aligned rectangle in BOTH screen and UV, so the rule refused
		//                 them rather than tearing them.
		//
		// rect_shrunk == 0 while HUD text is on screen means the rule is not reaching the glyphs at
		// all; rect_declined climbing means the glyph batches are not the quads this assumes.
		u64 rect_shrunk = 0;
		u64 rect_declined = 0;
	};

	// CPU rasterizer for everything that must appear as flat 2D on top of the path-traced
	// image: the title's own screen-space draws and rpcs3's native overlay. The whole buffer
	// is handed to the fork's DrawScreenOverlay once per flip, which composites it with a
	// compute pass after tone mapping.
	class compositor
	{
	public:
		// Every 'bgra' colour word below is 0xAARRGGBB: bits 0..7 are BLUE, because that is
		// byte 0 of the buffer and submit() declares the buffer REMIXAPI_FORMAT_B8G8R8A8_UNORM.
		// Stated here because the UI probe was once authored in the opposite order and rendered
		// its "red" bar blue (measured B=255, R=23) against a runtime that was behaving.

		// Sizes and clears the target if anything was drawn last frame.
		void begin_frame(u32 width, u32 height);

		// One textured/tinted triangle in pixel space, top-left origin.
		// 'tex' may be null, in which case only 'tint' is used.
		void draw_triangle(const f32 (&x)[3], const f32 (&y)[3], const f32 (&u)[3], const f32 (&v)[3],
			const texture_entry* tex, const u32 (&tint_bgra)[3], bool clamp_uv, bool force_opaque = false);
		void draw_triangle(const f32 (&x)[3], const f32 (&y)[3], const f32 (&u)[3], const f32 (&v)[3],
			const texture_entry* tex, u32 tint_bgra, bool clamp_uv, bool force_opaque = false);

		// Axis-aligned textured quad helper for rpcs3's overlay quads.
		void draw_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1,
			const texture_entry* tex, u32 tint_bgra, bool clamp_uv);

		// Composites everything drawn so far OVER an opaque background of 'bgra', leaving the
		// whole buffer opaque. Used for frames the title painted with a framebuffer CLEAR and
		// nothing else -- see RemixGSRender::submit_compositor(). Straight-alpha in, straight
		// (fully opaque) alpha out, so it must run after the frame's draws, never before.
		void underlay(u32 bgra);

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

		// Pixels the scalar rasterizer actually shaded. This is the compositor's real bill:
		// draw counts say nothing when one full-screen blit outweighs a thousand glyphs.
		u64 pixels() const { return m_pixels; }
		void reset_pixels() { m_pixels = 0; }

		// Round 7: the per-sample addressing partition. Cumulative for the life of the renderer,
		// like m_draws/m_frames and unlike m_pixels (which the timing line resets per window).
		const uv_address_counters& uv_counters() const { return m_uv; }

		// Round 61. The span rasterizer's bill and its proof counters, reset per timing window
		// with m_pixels. bbox_px is counted per triangle on BOTH paths (it is what the reference
		// loop visits); every other field is counted only inside the span path, so an
		// UIFASTRASTER=0 run pays nothing for them and stays a clean baseline.
		struct raster_counters
		{
			u64 bbox_px = 0;       // bounding-box pixels of every rasterized triangle
			u64 fast_tris = 0;     // triangles the span path drew
			u64 ref_tris = 0;      // triangles the reference loop drew (knob off, or ineligible)
			u64 visited = 0;       // span-path pixels that reached the inside test
			u64 opaque = 0;        // span-path alpha == 255 stores
			u64 translucent = 0;   // span-path 0 < alpha < 255 blends: the reciprocal-divide population
			u64 transparent = 0;   // span-path alpha == 0 early-outs
			u64 verify_tris = 0;   // UIFASTRASTER=2: triangles drawn by both paths and compared
			u64 verify_bad_px = 0; // ... and how many 32-bit pixels differed, summed
		};

		const raster_counters& raster() const { return m_raster; }
		void reset_raster() { m_raster = {}; }

		// UIFASTRASTER=2: x, y, reference pixel, span pixel of the first difference in the latest
		// triangle that differed. All zero until one does.
		const std::array<u32, 4>& verify_first() const { return m_verify_first; }

		// Order-sensitive 64-bit digest of the whole buffer, for the per-frame identity check.
		u64 checksum() const;

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
		u64 m_pixels = 0;
		uv_address_counters m_uv{};

		raster_counters m_raster{};
		std::vector<u8> m_verify;
		std::array<u32, 4> m_verify_first{};
	};

	// RPCS3_REMIX_NOUI=1 disables the compositor entirely.
	bool compositor_disabled();

	// RPCS3_REMIX_CLEARBG. Whether a frame that submitted no world geometry gets the title's own
	// framebuffer clear colour painted under the 2D overlay. Defaults to ON, so the accessor is
	// deliberately tri-state: an `env != 0` test against a true fallback would make CLEARBG=0 a
	// silent no-op, which is the shape several knobs in this backend were already caught by.
	bool clear_background_enabled();

	// RPCS3_REMIX_KEEPRT=1 restores the old behaviour of compositing the title's own
	// render-target blits (its post-process chain) through the CPU rasterizer. Off by default:
	// those draws are not UI, they cost a full-screen fill each, and they are what held Haze
	// at 1.9 FPS. Provided so the change can be A/B'd in one run.
	bool keep_render_target_blits();

	// RPCS3_REMIX_MESHCAP=N caps the live mesh-handle cache at N with LRU eviction. 0 (default)
	// leaves it unbounded, which is what the idle-frame rule alone gives.
	usz mesh_cap();

	// RPCS3_REMIX_UIPROBE=1 draws a fixed known pattern through DrawScreenOverlay instead of
	// judging the call for the first time with real UI data flowing through it.
	bool ui_probe_enabled();

	// Ceiling on the compositor buffer's width in pixels; the height follows the window's
	// aspect. RPCS3_REMIX_UIWIDTH overrides it, 0 removes the cap. Default 1920.
	u32 compositor_max_width();

	// RPCS3_REMIX_UICLAMPSUBRECT=1 (default): under REPEAT addressing, a coordinate outside [0,1]
	// on an axis whose authored UV span is at most one texture clamps to the sheet edge instead of
	// wrapping to the opposite edge of the atlas. This is the guest-UI seam fix - see the long
	// block above address_coordinate in RemixCompositor.cpp for the mechanism and the measured
	// draw. 0 restores round 6's sampling bit-exactly and is the A/B that attributes any change.
	// u32 rather than a flag because the useful setting is the explicit off one.
	u32 ui_clamp_subrect_mode();

	// RPCS3_REMIX_UIRECTSHRINK=<albedo>[,<albedo>...] (bound 8, default empty) and
	// RPCS3_REMIX_UIRECTSHRINKPCT=<percent> (default 50; a value that parses is CLAMPED into
	// 10..100, so 200 becomes 100 and is a true no-op; anything unparseable falls back to 50).
	//
	// ROUND 26 CORRECTS ROUND 25: the rule now shrinks the primitive's SCREEN rectangle by the same
	// percentage about the same per-primitive centre, not just its UV rectangle. Round 25 shipped the
	// UV half alone and the user's verdict was "more legible but bigger and squished" - which is
	// exactly what the UV half alone predicts, because magnifying the middle quarter of the UV onto
	// an unchanged screen quad draws the right glyph at 1/k times its authored size.
	//
	// PROVEN, not argued, by replaying the RPCS3_REMIX_UIDUMP capture against the dumped atlas:
	//   * The per-vertex dump (`Remix ui-quads[..]`, RPCS3.log) gives 4 vertices per glyph, and
	//     92 vertices for the string "11:17 Hours, 18th June 2048" - exactly 23 non-space glyphs.
	//     So these ARE quads, vertices 4n..4n+3, which settles round 25's open strip risk.
	//   * Sampling the atlas through the AUTHORED rectangles reproduces the round-24 screenshot
	//     (three bands: bottom of the row above, the wanted glyph, top of the row below).
	//   * Shrinking only the UV reproduces the round-25 screenshot (right glyphs, double size,
	//     44% overlap between neighbours).
	//   * Shrinking BOTH renders clean, correctly spaced text.
	// The px-per-texel ratio is IDENTICAL between "authored" and "both shrunk" - k cancels - so this
	// leaves every glyph exactly the size it already was on screen and removes only the padding. That
	// is why it is the correct correction and the UV-only one was not.
	//
	// WHY IT CANNOT BE FIXED FURTHER UPSTREAM. Stated carefully, because a review found the first
	// draft of this argument overreached. Two hypotheses have to die, and they die differently.
	//
	// (i) A global scale ANCHORED AT THE ORIGIN (a wrong uv_scale, a wrong viewport term, a dropped
	//     0.5 anywhere in the recovered shader chain) is excluded EMPIRICALLY, not geometrically:
	//     halving each rectangle about its own centre yields a cleanly framed glyph on every
	//     rectangle tested, while halving the same rectangle about the texture origin - or either
	//     corner - does not. An origin-anchored error cannot be undone by a centre-anchored fix.
	//     Note this is a measurement, and it is the load-bearing one; the ratio argument below does
	//     NOT cover it on the v axis, where a single line of text gives adjacent glyph centres the
	//     same v and the extent/spacing ratio is 0/0.
	//
	// (ii) A scale about the DRAW's OWN CENTRE is affine, is applied identically to every vertex,
	//     and scales extents and centre spacings together - so it survives the ratio argument and
	//     is the dangerous counter-hypothesis. It is excluded by REPLAY: shrinking each quad about
	//     its OWN centre renders "11:17 Hours, 18th June 2048" correctly across all 23 quads, both
	//     ends included. Under a draw-centred doubling only the quads near the middle of the string
	//     would come out right and the ends would sample the wrong glyphs. They do not.
	//
	// What survives: the expansion is per quad, about that quad's own centre, in both spaces. Every
	// step AFTER the vertex decode - the recovered vertex-program matrix, build_prescale, the
	// NDC/pixel conversion, the viewport terms, uv_scale - is affine and draw-wide, so none of them
	// can produce it. (The decode itself is not affine, and neither are the wrap/trunc steps further
	// down this file, which is why the claim is scoped to "after decode".) Whatever doubles the
	// rectangle is per-vertex, in the decoded attribute data, and a per-primitive correction is the
	// only shape of fix available at this layer. RPCS3_REMIX_UIDUMPVP now also stores that
	// program's raw ucode for offline decode, which is the instrument that should finish the job.
	//
	// ROUND 25, and it is a BRIDGE, not the root cause. MEASURED: Haze's HUD draws its glyphs from
	// the 512x512 B8 atlas 6575ACE3A42A78E6 with a per-quad UV rectangle that is EXACTLY TWICE the
	// glyph cell on both axes, expanded about the rectangle's own centre. The rendered result is
	// therefore the atlas content of a 3x3 block of cells - the bottom of the row above, the wanted
	// glyph, and the top of the row below - which is the doubled/overlapping text the user
	// photographed. The compositor is innocent: cropping the dumped atlas at the exact rectangle the
	// existing 'Remix uiwrap:' census logs reproduces the photographed pixels, so the sampler,
	// the wrap mode and the seam rule are all doing what they are told and the fault is in the
	// authored/decoded rectangle upstream.
	//
	// The 0.5 factor is not fitted, it is measured, and it is exact. For each of four independent
	// single-glyph rectangles taken from the census, scaling the rectangle about its own centre and
	// scoring the mean ink on the resulting one-texel border gives 0.0 at 0.50 and non-zero at every
	// other factor tested from 0.35 to 1.00 and at every other anchor (low corner, high corner,
	// texture origin). Four of four exactly zero on a quantity with no reason to be zero.
	//
	// Applied PER TRIANGLE, which is the only granularity where this is meaningful: a string draw's
	// draw-level UV box is the union of many glyph cells, but each glyph quad's two triangles carry
	// that quad's box and nothing else. Guarded to spans of at most half the sheet on both axes so a
	// full-sheet blit of a listed texture cannot be collapsed onto its middle quarter.
	//
	// RISK (1) IS CLOSED IN ROUND 26, twice over. It was: "each glyph quad's two triangles carry that
	// quad's box" holds for quads / triangles / indexed lists but NOT for a triangle STRIP, whose
	// connecting triangle straddles two cells at a span under the 0.5 guard and would be MOVED rather
	// than resized. Closed (i) by measurement - the UIDUMP capture gives 4 vertices per glyph and 92
	// vertices for a 23-glyph string, so these are not strips; and (ii) by construction -
	// is_axis_aligned_half now refuses any primitive that is not one half of an axis-aligned quad,
	// which is exactly the shape a strip's connecting triangle is not. Refusals are counted as
	// uv_address_counters::rect_declined, so the assumption is now instrumented rather than assumed.
	//
	// RISK (2) STANDS: the list is keyed on ALBEDO alone. Three vertex programs share this atlas
	// (5bb8451bcd6f1feb, the HUD counters; 6f76ab0ad8d926b1 and 3f73fa83fa68911f, the multi-glyph
	// strings) and if one of them authored its rectangle CORRECTLY its text would now render at half
	// size. Measured against that: the smallest v extent any of the three ever authors is 46 texels
	// against an atlas row pitch of about 23.5, so all three are doubled and all three want the fix.
	// Signature if that measurement is wrong: some UI text goes half-size while the HUD is right.
	//
	// 100 restores today's sampling bit-exactly even with a hash listed, and an empty list is a
	// no-op for every texture. Both rasterizers read it so the triangle and quad paths cannot drift.
	bool ui_rect_shrink_matches(u64 content_hash);
	u32 ui_rect_shrink_count();
	u32 ui_rect_shrink_percent();

	// RPCS3_REMIX_UIFASTRASTER: 0 = the reference rasterizer for every triangle; 1 = the span
	// rasterizer for the dominant textured/flat-tint case (pixel-identical by construction, see
	// fast_span in RemixCompositor.cpp); 2 = the span rasterizer with every eligible triangle also
	// drawn by the reference into a shadow buffer and compared, mismatches counted on the timing
	// line. Tri-state on purpose: an explicit 0 is a real 0, not "unset".
	u32 ui_fast_raster_mode();

	// RPCS3_REMIX_UISUM=1 logs 'Remix ui-sum:' with the buffer digest once per submitted frame.
	bool ui_sum_enabled();
}

#endif
