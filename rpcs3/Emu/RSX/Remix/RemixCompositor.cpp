#include "stdafx.h"
#include "RemixCompositor.h"

#ifdef _WIN32

#include "Emu/RSX/Remix/RemixRuntime.h"
#include "Emu/RSX/Remix/RemixTextures.h"
#include "Emu/RSX/Remix/RemixTransforms.h"
#include "Emu/system_config.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace remix_rsx
{
	namespace
	{
		// Sanity ceiling on the overlay target. 4K RGBA8 is 32 MiB.
		constexpr u32 s_max_dimension = 4096;

		bool demons_flare_bilinear(const texture_entry& tex)
		{
			if (!demons_world_enabled())
			{
				return false;
			}

			switch (tex.content_hash)
			{
			case 0xEF05AD7EB0FFA8D5ull: // 64x64 white disk
			case 0x998989659002A614ull: // 64x64 Gaussian core
			case 0x817ABB299B3EEB42ull: // 64x64 chromatic ring
			case 0x61A94501E60650AEull: // 128x128 starburst
				return true;
			default:
				return false;
			}
		}

		// RPCS3_REMIX_UIRECTSHRINK, parsed once. See RemixCompositor.h for what it is for; the
		// parse is byte-for-byte the shape clamp_albedos() uses (hex, comma/semicolon/space
		// separated, zero entries ignored). Worst case is 8 x 16 hex + 7 separators + null = 136
		// wchar against 192, so a saturated list can neither overflow nor trip the length test.
		// The length test refuses an over-long LIST, not an over-long TOKEN: _wcstoui64 saturates
		// a >16-digit value to ULLONG_MAX and a leading '-' is negated, neither of which is
		// checked. Both yield a hash that simply never matches, which is the harmless failure.
		struct ui_rect_shrink_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const ui_rect_shrink_list& ui_rect_shrink_albedos()
		{
			static const ui_rect_shrink_list list = []()
			{
				ui_rect_shrink_list result{};
				wchar_t buffer[192]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIRECTSHRINK", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}

		// One triangle's (or quad's) sampled UV, pulled toward its own authored rectangle centre.
		// Returns 100 (no-op) unless the texture is listed AND the authored span is small enough to
		// be an atlas cell rather than a sheet blit: a listed full-sheet quad must not collapse onto
		// its middle quarter. The unit is normalised UV, so 0.5 means half the sheet; the largest
		// AUTHORED single-glyph span measured on Haze's atlas is 70/512 = 0.137 in u and 46/512 =
		// 0.09 in v, so the guard sits 2.7x above the population it is meant to admit.
		u32 ui_rect_shrink_for(const texture_entry& tex, f32 min_u, f32 max_u, f32 min_v, f32 max_v)
		{
			if (!ui_rect_shrink_matches(tex.content_hash))
			{
				return 100;
			}

			if (!std::isfinite(min_u) || !std::isfinite(max_u) || !std::isfinite(min_v) || !std::isfinite(max_v))
			{
				return 100;
			}

			if ((max_u - min_u) > 0.5f || (max_v - min_v) > 0.5f)
			{
				return 100;
			}

			return ui_rect_shrink_percent();
		}

		// c + (coordinate - c) * k, with c the authored rectangle's own centre. A no-op at k == 1.
		void apply_rect_shrink(f32& su, f32& sv, u32 percent,
			f32 min_u, f32 max_u, f32 min_v, f32 max_v)
		{
			if (percent == 100)
			{
				return;
			}

			const f32 k = static_cast<f32>(percent) / 100.f;
			const f32 cu = (min_u + max_u) * 0.5f;
			const f32 cv = (min_v + max_v) * 0.5f;

			su = cu + ((su - cu) * k);
			sv = cv + ((sv - cv) * k);
		}

		// ROUND 26. The SCREEN half of the same correction, and it is the half round 25 was missing.
		// Same k, same "about the primitive's own centre" rule, so the primitive's px-per-texel ratio
		// is left EXACTLY as authored - only the padding around the glyph is removed. Shrinking the UV
		// alone kept the full-size screen quad and therefore magnified the glyph 1/k times, which is
		// precisely what the user reported ("more legible but bigger and squished").
		void apply_screen_shrink(f32 (&x)[3], f32 (&y)[3], u32 percent)
		{
			if (percent == 100)
			{
				return;
			}

			const f32 k = static_cast<f32>(percent) / 100.f;
			const f32 cx = (std::min({ x[0], x[1], x[2] }) + std::max({ x[0], x[1], x[2] })) * 0.5f;
			const f32 cy = (std::min({ y[0], y[1], y[2] }) + std::max({ y[0], y[1], y[2] })) * 0.5f;

			for (u32 i = 0; i < 3; ++i)
			{
				x[i] = cx + ((x[i] - cx) * k);
				y[i] = cy + ((y[i] - cy) * k);
			}
		}

		// Is this triangle one half of an AXIS-ALIGNED rectangle in the given pair of coordinates?
		// True exactly when the three first-coordinate values take two distinct values and the three
		// second-coordinate values take two distinct values, which is the shape of (TL,TR,BR) and
		// (TL,BR,BL) and of nothing else.
		//
		// This is the guard that makes the screen shrink safe, and it is why the rule can be applied
		// per triangle at all: both halves of an axis-aligned quad have the SAME bounding box, so they
		// shrink about the same centre and still tile the shrunk quad exactly. A general triangle - a
		// strip's connecting triangle, a rotated UI element - has a bounding box of its own, would
		// shrink about a different centre than its neighbour, and would TEAR. Those are refused here
		// and keep round 24's sampling untouched, on both the UV and the screen axis.
		//
		// IT IS CALLED ON BOTH PAIRS, and a review defect is why. Screen-rectangularity alone is not
		// enough: apply_rect_shrink pulls toward the triangle's own UV box, and the 0.5 span guard is
		// evaluated per triangle, so a listed quad with a sheared or rotated UV mapping over an
		// axis-aligned screen quad would give its two halves different UV centres - and possibly
		// different shrink verdicts - which tears along the quad's diagonal. Both boxes have to be
		// rectangles for the per-triangle rule to be coherent.
		//
		// The tolerance is relative so that a coordinate arriving through the ortho matrix and the
		// viewport conversion is not rejected over the last mantissa bit; the absolute floor keeps a
		// degenerate primitive from making the test vacuous, and is passed in because a screen pixel
		// and a normalised texel are four orders of magnitude apart.
		bool is_axis_aligned_half(const f32 (&a)[3], const f32 (&b)[3], f32 floor_eps)
		{
			const f32 alo = std::min({ a[0], a[1], a[2] });
			const f32 ahi = std::max({ a[0], a[1], a[2] });
			const f32 blo = std::min({ b[0], b[1], b[2] });
			const f32 bhi = std::max({ b[0], b[1], b[2] });

			if (!std::isfinite(alo) || !std::isfinite(ahi) || !std::isfinite(blo) || !std::isfinite(bhi))
			{
				return false;
			}

			const f32 ea = std::max(floor_eps, (ahi - alo) * 1e-3f);
			const f32 eb = std::max(floor_eps, (bhi - blo) * 1e-3f);

			if ((ahi - alo) <= ea || (bhi - blo) <= eb)
			{
				return false;
			}

			for (u32 i = 0; i < 3; ++i)
			{
				// A NaN component would otherwise slip through: std::min/max over an initializer
				// list are min_element/max_element, every comparison against NaN is false, so the
				// bounds come back finite from the other two vertices and every |NaN - bound| > e
				// test below is false as well. Rejected explicitly instead.
				if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
				{
					return false;
				}

				if (std::abs(a[i] - alo) > ea && std::abs(a[i] - ahi) > ea)
				{
					return false;
				}

				if (std::abs(b[i] - blo) > eb && std::abs(b[i] - bhi) > eb)
				{
					return false;
				}
			}

			return true;
		}

		// --- round 7: the atlas sub-rect seam rule ---------------------------------------------
		// The defect this exists for, in one line of arithmetic. Under REPEAT a coordinate even
		// marginally outside [0,1] does not clamp to the glyph's own edge - 'coordinate -=
		// floor(coordinate)' sends it to the OPPOSITE EDGE OF THE WHOLE SHEET - and the fetch below
		// is NEAREST, so it then returns a different glyph's ink at 100% weight. Real RSX bilinear
		// blends such a seam over at most about one texel; this sampler has no such bound. The
		// project's own census caught it: vp=6f76ab0ad8d926b1 draws a 13-quad glyph batch with
		// u=[-0.018555..0.99902] on a repeat-bound atlas, and 0.0186 UV units is ~9.5 texels on a
		// 512-wide sheet - authored slop, an order of magnitude past float rounding.
		//
		// The discriminator is authored SPAN, not magnitude. An atlas sub-rect draw whose UV span
		// on an axis is at most one texture cannot be intending to tile, so a coordinate outside
		// [0,1] is padding and the behaviour matching what the guest's own raster produced is to
		// clamp to the sheet edge (usually empty gutter). A draw that genuinely spans more than one
		// texture keeps true repeat. Magnitude cannot separate these two populations - any epsilon
		// big enough to cover the measured 0.0186 excursion carves a visible dead band around every
		// seam of a genuinely tiling draw - which is the tell that magnitude is the wrong axis.
		//
		// The discriminator is PER-TRIANGLE by design, and that is load-bearing rather than
		// incidental: the census line above is a 13-quad batch whose DRAW-level u span is
		// 0.99902 - (-0.018555) = 1.0176, which is already past the 1 + 1e-3 slack. Each glyph
		// quad inside it spans a fraction of the sheet, so the rule sees a sub-rect at the only
		// granularity where the question "is this primitive tiling?" has an answer. Testing it
		// per draw would refuse the exact population it was written for.
		//
		// Round 8 (review defect A): span alone is not sufficient. A REPEAT-bound draw authored
		// u in [1.2, 1.8] - an offset or scrolling tile - has span 0.6 and would have been called
		// a sub-rect, at which point EVERY one of its coordinates is out of range and the clamp
		// below collapses the whole primitive onto texel width-1. Negatives collapse onto texel 0
		// the same way, and the damage is silent because the bad samples increment counters.seam,
		// so the live line reads "the fix is firing" exactly when it is destroying the draw. The
		// missing term is an INTERSECTION test - the authored window has to actually straddle the
		// unit square (max > 0 && min < 1), which is the rule's own stated rationale
		// ("marginally outside"). A window living entirely outside [0,1] is a tile offset, not
		// slop, and keeps true repeat.
		//
		// Per-axis and per-triangle, so the two regressions of the old combined force_clamp flag
		// (clamping both axes when either clamped; mirror collapsed to repeat) are impossible by
		// construction. Coordinates inside [0,1], mirror, clip, clamp and the force_clamp
		// native-overlay path are all bit-identical to before.
		// One primitive's authored UV window on one axis, classified. Both rasterizers call this so
		// the triangle and quad paths cannot drift apart, which is how the intersection term came
		// to be missing from one of them for a round.
		bool is_seam_subrect(f32 min_c, f32 max_c)
		{
			if (!std::isfinite(min_c) || !std::isfinite(max_c))
			{
				return false;
			}

			// Narrow enough not to be tiling, AND actually overlapping the sheet. Half-open on the
			// low side and closed on the high side matches address_coordinate's own bounds: a
			// window touching exactly max_c == 0 sits on the wrap image of texel 0 rather than
			// straddling, and min_c == 1 is the same case at the other end.
			return (max_c - min_c) <= 1.f + 1e-3f && max_c > 0.f && min_c < 1.f;
		}

		bool address_coordinate(f32& coordinate, u8 mode, bool force_clamp, bool subrect,
			bool seam_rule, uv_address_counters& counters)
		{
			const bool in_range = coordinate >= 0.f && coordinate <= 1.f;

			if (force_clamp || mode == 0)
			{
				if (in_range)
				{
					++counters.in;
				}
				else
				{
					++counters.clamp;
				}

				coordinate = std::clamp(coordinate, 0.f, 1.f);
				return true;
			}

			if (mode == 1)
			{
				// HALF-OPEN on purpose, and this is the one place the bound matters. Repeat's
				// 'coordinate -= floor(coordinate)' sends exactly 1.0 to 0.0, i.e. to texel 0, not
				// to texel dim-1. Taking the closed [0,1] early-out here would leave 1.0 alone and
				// sample the far edge instead - a behaviour change that would survive
				// UICLAMPSUBRECT=0 and quietly break the "0 restores today's sampling bit-exactly"
				// guarantee the whole A/B rests on. Below 1.0 the wrap is the identity, so the
				// early-out is exact.
				if (coordinate >= 0.f && coordinate < 1.f)
				{
					++counters.in;
					return true;
				}

				// THE FIX. Everything else in this function is round 6's behaviour verbatim.
				// Note this also captures coordinate == 1.0 exactly, which reaches here by the
				// half-open test above: on a sub-rect draw that is the sheet's right/bottom edge
				// and clamping keeps it there, where wrapping would have jumped it to texel 0.
				// That is the seam defect in miniature, and it only changes with the knob on.
				if (subrect && seam_rule)
				{
					++counters.seam;
					coordinate = std::clamp(coordinate, 0.f, 1.f);
					return true;
				}

				++counters.wrap;
				coordinate -= std::floor(coordinate);
				return true;
			}

			if (mode == 2)
			{
				if (in_range)
				{
					++counters.in;
				}
				else
				{
					++counters.mirror;
				}

				coordinate = std::fmod(coordinate, 2.f);
				if (coordinate < 0.f)
				{
					coordinate += 2.f;
				}
				coordinate = (coordinate <= 1.f) ? coordinate : (2.f - coordinate);
				return true;
			}

			// Clip is the closest available representation of RSX clamp-to-border.
			if (in_range)
			{
				++counters.in;
				return true;
			}

			++counters.clip;
			return false;
		}

		u32 sample_bgra(const texture_entry& tex, f32 u, f32 v, bool force_clamp,
			bool subrect_u, bool subrect_v, bool seam_rule, uv_address_counters& counters)
		{
			if (tex.pixels.empty() || tex.width == 0 || tex.height == 0)
			{
				return 0xFFFFFFFFu;
			}

			f32 su = u;
			f32 sv = v;

			// Round 8 (review defect C): both axes are evaluated before the verdict is combined.
			// '||' short-circuits, so under CLIP mode an out-of-range u returned false and the V
			// axis was never addressed at all - a clamp-to-border texture dropped its entire V
			// population out of the ui_uv census, and the partition silently stopped summing to
			// two increments per sampled texel.
			const bool keep_u = address_coordinate(su, tex.wrap_u, force_clamp, subrect_u, seam_rule, counters);
			const bool keep_v = address_coordinate(sv, tex.wrap_v, force_clamp, subrect_v, seam_rule, counters);

			if (!keep_u || !keep_v)
			{
				return 0;
			}

			const u32 x = std::min(tex.width - 1, static_cast<u32>(su * static_cast<f32>(tex.width)));
			const u32 y = std::min(tex.height - 1, static_cast<u32>(sv * static_cast<f32>(tex.height)));

			u32 texel;
			std::memcpy(&texel, tex.pixels.data() + ((usz{y} * tex.width + x) * 4), sizeof(u32));

			if (tex.b8_coverage)
			{
				// B8 UI sheets are coverage masks. Treating the expanded grayscale byte as
				// opaque colour makes every glyph's black padded quad erase the overlapping
				// glyph before it; Haze's 512x512 font sheet exposes that as sliced letters.
				return 0x00FFFFFFu | ((texel & 0xFFu) << 24);
			}

			if (demons_flare_bilinear(tex))
			{
				const f32 fx = (su * static_cast<f32>(tex.width)) - 0.5f;
				const f32 fy = (sv * static_cast<f32>(tex.height)) - 0.5f;
				const s32 x0 = static_cast<s32>(std::floor(fx));
				const s32 y0 = static_cast<s32>(std::floor(fy));
				const u32 x1 = std::min(tex.width - 1, static_cast<u32>(std::max(0, x0 + 1)));
				const u32 y1 = std::min(tex.height - 1, static_cast<u32>(std::max(0, y0 + 1)));
				const u32 bx = static_cast<u32>(std::max(0, x0));
				const u32 by = static_cast<u32>(std::max(0, y0));
				const f32 tx = std::clamp(fx - static_cast<f32>(x0), 0.f, 1.f);
				const f32 ty = std::clamp(fy - static_cast<f32>(y0), 0.f, 1.f);

				u32 samples[4]{};
				std::memcpy(&samples[0], tex.pixels.data() + ((usz{by} * tex.width + bx) * 4), sizeof(u32));
				std::memcpy(&samples[1], tex.pixels.data() + ((usz{by} * tex.width + x1) * 4), sizeof(u32));
				std::memcpy(&samples[2], tex.pixels.data() + ((usz{y1} * tex.width + bx) * 4), sizeof(u32));
				std::memcpy(&samples[3], tex.pixels.data() + ((usz{y1} * tex.width + x1) * 4), sizeof(u32));

				u32 filtered = 0;
				for (u32 channel = 0; channel < 4; ++channel)
				{
					const u32 shift = channel * 8;
					const f32 top = static_cast<f32>((samples[0] >> shift) & 0xFF) * (1.f - tx)
						+ static_cast<f32>((samples[1] >> shift) & 0xFF) * tx;
					const f32 bottom = static_cast<f32>((samples[2] >> shift) & 0xFF) * (1.f - tx)
						+ static_cast<f32>((samples[3] >> shift) & 0xFF) * tx;
					filtered |= static_cast<u32>(std::clamp(top * (1.f - ty) + bottom * ty, 0.f, 255.f) + 0.5f) << shift;
				}

				return filtered;
			}

			return texel;
		}

		u32 modulate(u32 a_bgra, u32 b_bgra)
		{
			u32 out = 0;

			for (u32 c = 0; c < 4; ++c)
			{
				const u32 shift = c * 8;
				const u32 lhs = (a_bgra >> shift) & 0xFF;
				const u32 rhs = (b_bgra >> shift) & 0xFF;
				out |= (((lhs * rhs) + 127) / 255) << shift;
			}

			return out;
		}

		u32 interpolate_bgra(const u32 (&bgra)[3], f32 w0, f32 w1, f32 w2)
		{
			u32 out = 0;

			for (u32 c = 0; c < 4; ++c)
			{
				const u32 shift = c * 8;
				const f32 value =
					static_cast<f32>((bgra[0] >> shift) & 0xFF) * w0 +
					static_cast<f32>((bgra[1] >> shift) & 0xFF) * w1 +
					static_cast<f32>((bgra[2] >> shift) & 0xFF) * w2;

				out |= static_cast<u32>(std::clamp(value, 0.f, 255.f) + 0.5f) << shift;
			}

			return out;
		}

		// --- round 61: blend()'s divisor ---------------------------------------------------------
		// ((src * alpha) + dst_term + out_alpha / 2) / out_alpha divides by a value that changes per
		// pixel, which no compiler can strength-reduce. ceil(2^32 / d) as a 64-bit multiplier with
		// a 32-bit shift is exact for every numerator below 2^32 / 255; blend()'s largest is
		// 255 * 254 + 64770 + 127 = 129667. PROVEN, not argued: every divisor 1..255 against every
		// numerator in [0, 2^18) - 66,846,720 cases - matched the division with zero mismatches
		// (docs/remix/uifastraster-reciptest.cpp, run 2026-09-10).
		struct alpha_reciprocals
		{
			u64 values[256]{};

			constexpr alpha_reciprocals()
			{
				for (u32 d = 1; d < 256; ++d)
				{
					values[d] = (0xFFFFFFFFull / d) + 1;
				}
			}
		};

		constexpr alpha_reciprocals s_alpha_recip{};

		inline u32 div_by_alpha(u32 n, u32 d)
		{
			return static_cast<u32>((u64{n} * s_alpha_recip.values[d]) >> 32);
		}

		// --- round 61: the span rasterizer's scanline bound --------------------------------------
		// The reference loop visits every bounding-box pixel and rejects with
		// `w0 < 0 || w1 < 0 || w2 < 0`. Each w is affine in fx in exact arithmetic, so the accepted
		// pixels of a scanline form one interval - but the reference evaluates them in binary32,
		// and the point of this path is to visit fewer pixels WITHOUT changing which ones survive.
		// So the interval solved here is the exact one widened by a bound on the reference's own
		// rounding error, and the reference test then runs verbatim on every pixel inside it. A
		// pixel outside has some exact w_j at least e_j below zero, where e_j bounds
		// |binary32 w_j - exact w_j| over the scanline, so the reference rejects it as well. That
		// is the identity argument; it needs finite vertices, which draw_triangle already
		// guarantees through its area test, and anything non-finite here falls back to the box.
		//
		// The bound, with u = 2^-24 the binary32 unit roundoff, in the reference's operand order:
		// raw_j = fl(fl(fl(xa - fx) * A) - fl(fl(xb - fx) * B)) with A, B the float-rounded
		// scanline constants, then w_j = fl(raw_j * inv_area). With M_j the scanline maximum of
		// |(xa - fx) A| and |(xb - fx) B|: |raw_j - exact| <= 6.3u M_j (8u used), and after the
		// inv_area multiply, whose own error is u|w| plus u|w| for inv_area = fl(1 / area),
		// |w_j - exact| <= 12.2u M_j / |area| (14u used). w2 = fl(fl(1 - w0) - w1) adds at most
		// 3u (1 + |w0| + |w1|). A contracted multiply-subtract only lowers the raw error, so the
		// bound holds under either code generation.
		bool fast_span(const f32 (&x)[3], const f32 (&y)[3], f32 fy, f32 area, s32 min_x, s32 max_x,
			s32& px_lo, s32& px_hi)
		{
			constexpr f64 u = 5.9604644775390625e-8; // 2^-24
			const f64 fx_lo = static_cast<f64>(min_x) + 0.5;
			const f64 fx_hi = static_cast<f64>(max_x) + 0.5;
			const f64 abs_area = std::abs(static_cast<f64>(area));

			// a + b * fx >= -e is "not provably rejected by edge j", in units of w.
			f64 a[3]{}, b[3]{}, e[3]{}, bound[2]{};

			for (u32 j = 0; j < 2; ++j)
			{
				// j = 0 is the reference's w0: xa = x[1], A = y[2] - fy, xb = x[2], B = y[1] - fy.
				// j = 1 is its w1: xa = x[2], A = y[0] - fy, xb = x[0], B = y[2] - fy. A and B are
				// rounded to binary32 exactly as the reference rounds them.
				const f32 xa = (j == 0) ? x[1] : x[2];
				const f32 xb = (j == 0) ? x[2] : x[0];
				const f32 fa = (j == 0) ? (y[2] - fy) : (y[0] - fy);
				const f32 fb = (j == 0) ? (y[1] - fy) : (y[2] - fy);
				const f64 A = fa;
				const f64 B = fb;
				const f64 dxa = std::max(std::abs(f64{xa} - fx_lo), std::abs(f64{xa} - fx_hi));
				const f64 dxb = std::max(std::abs(f64{xb} - fx_lo), std::abs(f64{xb} - fx_hi));
				const f64 M = std::max(std::abs(A) * dxa, std::abs(B) * dxb);
				// Both products are exact: 24-bit operands into a 53-bit mantissa.
				const f64 c = (f64{xa} * A) - (f64{xb} * B);
				const f64 s = B - A;
				a[j] = c / static_cast<f64>(area);
				b[j] = s / static_cast<f64>(area);
				e[j] = 14.0 * u * M / abs_area;
				bound[j] = 2.1 * M / abs_area;
			}

			a[2] = 1.0 - a[0] - a[1];
			b[2] = -(b[0] + b[1]);
			e[2] = (3.0 * u * (1.0 + bound[0] + bound[1])) + e[0] + e[1];

			f64 lo = fx_lo;
			f64 hi = fx_hi;

			for (u32 j = 0; j < 3; ++j)
			{
				if (!std::isfinite(a[j]) || !std::isfinite(b[j]) || !std::isfinite(e[j]))
				{
					px_lo = min_x;
					px_hi = max_x;
					return true;
				}

				if (b[j] > 0.0)
				{
					lo = std::max(lo, (-e[j] - a[j]) / b[j]);
				}
				else if (b[j] < 0.0)
				{
					hi = std::min(hi, (-e[j] - a[j]) / b[j]);
				}
				else if (a[j] < -e[j])
				{
					return false;
				}
			}

			// fx = px + 0.5. The 1e-6 is double-rounding slack for a boundary that lands within it
			// of a pixel centre: that pixel stays in, which is the safe side.
			if (lo - 1e-6 > hi + 1e-6)
			{
				return false;
			}

			px_lo = std::max(min_x, static_cast<s32>(std::ceil(lo - 0.5 - 1e-6)));
			px_hi = std::min(max_x, static_cast<s32>(std::floor(hi - 0.5 + 1e-6)));
			return px_lo <= px_hi;
		}
	}

	bool compositor_disabled()
	{
		static const bool env = []
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_NOUI", buffer, static_cast<DWORD>(std::size(buffer)));
			return written > 0 && written < std::size(buffer) && ::wcstol(buffer, nullptr, 10) != 0;
		}();

		return env || g_cfg.video.remix.no_ui;
	}

	bool clear_background_enabled()
	{
		static const bool value = []
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_CLEARBG", buffer, static_cast<DWORD>(std::size(buffer)));

			// Unset (or unreadable) falls through to the config; an explicit 0 really does turn it
			// off rather than being ORed away behind a config default of true.
			if (written == 0 || written >= std::size(buffer))
			{
				return g_cfg.video.remix.clear_background.get();
			}

			return ::wcstol(buffer, nullptr, 10) != 0;
		}();

		return value;
	}

	bool keep_render_target_blits()
	{
		static const bool value = []
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_KEEPRT", buffer, static_cast<DWORD>(std::size(buffer)));

			// Unset falls through to the config; KEEPRT=0 still overrides a config that says true.
			if (written == 0 || written >= std::size(buffer))
			{
				return g_cfg.video.remix.keep_render_targets.get();
			}

			return ::wcstol(buffer, nullptr, 10) != 0;
		}();

		return value;
	}

	usz mesh_cap()
	{
		static const usz env = []() -> usz
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_MESHCAP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);
			return parsed > 0 ? static_cast<usz>(parsed) : 0;
		}();

		return env ? env : static_cast<usz>(g_cfg.video.remix.mesh_cap);
	}

	bool ui_probe_enabled()
	{
		static const bool value = []
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIPROBE", buffer, static_cast<DWORD>(std::size(buffer)));
			return written > 0 && written < std::size(buffer) && ::wcstol(buffer, nullptr, 10) != 0;
		}();

		return value;
	}

	u32 compositor_max_width()
	{
		// 0xFFFFFFFF means "unset": fall through to the config.
		static const u32 env = []() -> u32
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIWIDTH", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				// 1080p class. Everything this rasterizer draws is authored at the guest's own
				// surface resolution (720p on both test titles) or, for rpcs3's overlays, a
				// virtual 1280x720, so more pixels than this buy no detail at all.
				return 0xFFFFFFFFu;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);
			return (parsed >= 0) ? static_cast<u32>(parsed) : 0xFFFFFFFFu;
		}();

		return env != 0xFFFFFFFFu ? env : g_cfg.video.remix.ui_width;
	}

	u32 ui_clamp_subrect_mode()
	{
		static const u32 value = []() -> u32
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UICLAMPSUBRECT", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 1;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);
			return (parsed >= 0) ? static_cast<u32>(parsed) : 1u;
		}();

		return value;
	}

	// See RemixCompositor.h for the measurement this exists for. Same latched-comma-list shape as
	// clamp_albedos() in RemixTransforms.cpp, kept local because both call sites are in this file.
	bool ui_rect_shrink_matches(u64 content_hash)
	{
		const ui_rect_shrink_list& list = ui_rect_shrink_albedos();
		const auto end = list.values.begin() + list.count;
		return content_hash != 0 && std::find(list.values.begin(), end, content_hash) != end;
	}

	u32 ui_rect_shrink_count()
	{
		return ui_rect_shrink_albedos().count;
	}

	u32 ui_rect_shrink_percent()
	{
		static const u32 value = []() -> u32
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIRECTSHRINKPCT", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 50;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);

			// Review defect, round 25: the doc said "clamped 10..100" and the code REJECTED out of
			// range back to 50 instead. That inverts the safe reading of the dangerous direction -
			// someone typing 200 expecting "even more of a no-op than 100" would have got the full
			// 50% shrink. Now it does what it says. wcstol yields 0 on garbage, which is the one
			// case that must NOT clamp (0 -> 10 would be a 90% shrink on unparseable input), so
			// nonsense still falls back to the default.
			if (parsed <= 0)
			{
				return 50;
			}

			return static_cast<u32>(std::clamp<long>(parsed, 10, 100));
		}();

		return value;
	}

	u32 ui_fast_raster_mode()
	{
		static const u32 value = []() -> u32
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIFASTRASTER", buffer, static_cast<DWORD>(std::size(buffer)));

			// Unset falls through to the default; an explicit 0 is a real 0. Not `env || default`,
			// the shape that made SMOOTHNORMALS=0 and TEXBUDGET=0 silent no-ops.
			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);
			return static_cast<u32>(std::clamp<long>(parsed, 0, 2));
		}();

		return value;
	}

	bool ui_sum_enabled()
	{
		static const bool value = []
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UISUM", buffer, static_cast<DWORD>(std::size(buffer)));
			return written > 0 && written < std::size(buffer) && ::wcstol(buffer, nullptr, 10) != 0;
		}();

		return value;
	}

	void compositor::begin_frame(u32 width, u32 height)
	{
		if (width == 0 || height == 0 || width > s_max_dimension || height > s_max_dimension)
		{
			m_dirty = false;
			return;
		}

		const usz needed = usz{width} * height * 4;

		if (m_width != width || m_height != height || m_buffer.size() != needed)
		{
			m_width = width;
			m_height = height;
			m_buffer.assign(needed, 0);
			m_dirty = false;
			return;
		}

		if (m_dirty)
		{
			std::fill(m_buffer.begin(), m_buffer.end(), u8{0});
			m_dirty = false;
		}

		clear_clip();
	}

	void compositor::set_clip(f32 x0, f32 y0, f32 x1, f32 y1)
	{
		m_clip[0] = x0;
		m_clip[1] = y0;
		m_clip[2] = x1;
		m_clip[3] = y1;
		m_clip_enabled = true;
	}

	void compositor::clear_clip()
	{
		m_clip_enabled = false;
	}

	void compositor::blend(u32 x, u32 y, u32 src_bgra)
	{
		++m_pixels;

		const u32 alpha = (src_bgra >> 24) & 0xFF;

		if (alpha == 0)
		{
			return;
		}

		if (m_clip_enabled &&
			(static_cast<f32>(x) < m_clip[0] || static_cast<f32>(x) > m_clip[2] ||
			 static_cast<f32>(y) < m_clip[1] || static_cast<f32>(y) > m_clip[3]))
		{
			return;
		}

		u8* dst = m_buffer.data() + ((usz{y} * m_width + x) * 4);

		if (alpha == 0xFF)
		{
			std::memcpy(dst, &src_bgra, sizeof(u32));
			return;
		}

		// Keep the compositor buffer in straight-alpha form, which is what
		// DrawScreenOverlay's compute pass expects. The old expression stored premultiplied
		// RGB while retaining alpha, so the runtime multiplied coverage a second time. Thin
		// native-overlay glyphs (including cellMsgDialog's Press-X prompt) became effectively
		// invisible, and overlapping B8 font glyphs accumulated dark fringes.
		const u32 inv = 255 - alpha;
		const u32 dst_alpha = dst[3];
		const u32 out_alpha = alpha + ((dst_alpha * inv + 127) / 255);

		for (u32 c = 0; c < 3; ++c)
		{
			const u32 src = (src_bgra >> (c * 8)) & 0xFF;
			const u32 dst_term = (u32{dst[c]} * dst_alpha * inv + 127) / 255;
			dst[c] = static_cast<u8>(((src * alpha) + dst_term + (out_alpha / 2)) / out_alpha);
		}

		dst[3] = static_cast<u8>(out_alpha);
	}

	void compositor::draw_triangle(const f32 (&x_in)[3], const f32 (&y_in)[3], const f32 (&u)[3], const f32 (&v)[3],
		const texture_entry* tex, const u32 (&tint_bgra)[3], bool clamp_uv, bool force_opaque)
	{
		if (m_buffer.empty())
		{
			return;
		}

		// Round 7: the triangle's own authored UV window, per axis. <= 1 texture wide means "this
		// is a sub-rect of an atlas", which cannot be an intent to tile. The 1e-3 slack admits a
		// full-sheet blit authored exactly [0,1] whose interpolation lands a hair over.
		//
		// Round 8: AND the window has to overlap the unit square. See the header comment - span
		// alone calls u in [1.2, 1.8] a sub-rect and then clamps the entire primitive onto one
		// edge texel.
		//
		// Round 26: hoisted above the area/bbox computation, because the screen shrink below now
		// rewrites the vertices this triangle is rasterized from.
		const f32 min_u = std::min({ u[0], u[1], u[2] });
		const f32 max_u = std::max({ u[0], u[1], u[2] });
		const f32 min_v = std::min({ v[0], v[1], v[2] });
		const f32 max_v = std::max({ v[0], v[1], v[2] });
		const bool subrect_u = is_seam_subrect(min_u, max_u);
		const bool subrect_v = is_seam_subrect(min_v, max_v);
		const bool seam_rule = ui_clamp_subrect_mode() != 0;

		// Round 25. 100 unless this texture is listed by RPCS3_REMIX_UIRECTSHRINK; hoisted out of
		// the per-pixel loop because it depends only on the triangle.
		//
		// Round 26. AND the triangle has to be one half of an axis-aligned rectangle in BOTH screen
		// and UV, because the rule now moves the SCREEN vertices too and that is only coherent when
		// both halves of a quad share both bounding boxes. A primitive that is listed and small but
		// not rectangular keeps round 24's sampling on BOTH axes rather than getting the UV half of
		// a correction whose screen half was refused.
		//
		// The empty-list test is FIRST and it is a performance gate, not a tidy-up: with no hash
		// listed this whole block costs one relaxed load of a function-local static and nothing
		// else per triangle. This rasterizer is what once held Haze at 1.9 FPS, so a default-off
		// feature must not put four min/max and a loop on every textured UI triangle.
		u32 shrink = 100;

		if (tex && ui_rect_shrink_count() != 0)
		{
			const u32 want = ui_rect_shrink_for(*tex, min_u, max_u, min_v, max_v);

			if (want != 100)
			{
				// 1e-3 px on screen; 1e-6 in normalised UV, which is 5e-4 of a texel on a 512 sheet.
				if (is_axis_aligned_half(x_in, y_in, 1e-3f) && is_axis_aligned_half(u, v, 1e-6f))
				{
					shrink = want;
				}
				else
				{
					++m_uv.rect_declined;
				}
			}
		}

		f32 x[3] = { x_in[0], x_in[1], x_in[2] };
		f32 y[3] = { y_in[0], y_in[1], y_in[2] };

		if (shrink != 100)
		{
			apply_screen_shrink(x, y, shrink);
		}

		const f32 area = ((x[1] - x[0]) * (y[2] - y[0])) - ((x[2] - x[0]) * (y[1] - y[0]));

		if (!std::isfinite(area) || std::abs(area) < 1e-6f)
		{
			return;
		}

		const f32 inv_area = 1.f / area;
		const bool flat_tint = tint_bgra[0] == tint_bgra[1] && tint_bgra[0] == tint_bgra[2];

		const s32 min_x = std::max<s32>(0, static_cast<s32>(std::floor(std::min({ x[0], x[1], x[2] }))));
		const s32 max_x = std::min<s32>(static_cast<s32>(m_width) - 1, static_cast<s32>(std::ceil(std::max({ x[0], x[1], x[2] }))));
		const s32 min_y = std::max<s32>(0, static_cast<s32>(std::floor(std::min({ y[0], y[1], y[2] }))));
		const s32 max_y = std::min<s32>(static_cast<s32>(m_height) - 1, static_cast<s32>(std::ceil(std::max({ y[0], y[1], y[2] }))));

		if (min_x > max_x || min_y > max_y)
		{
			return;
		}

		// Round 61. The reference loop, verbatim: every bounding-box pixel, the inside test, the
		// sampler, blend(). It is what RPCS3_REMIX_UIFASTRASTER=0 runs for every triangle, what
		// every primitive the span path does not take still runs, and the oracle the span path is
		// checked against under UIFASTRASTER=2.
		const auto reference_loop = [&]()
		{
			for (s32 py = min_y; py <= max_y; ++py)
			{
				const f32 fy = static_cast<f32>(py) + 0.5f;

				for (s32 px = min_x; px <= max_x; ++px)
				{
					const f32 fx = static_cast<f32>(px) + 0.5f;

					const f32 w0 = (((x[1] - fx) * (y[2] - fy)) - ((x[2] - fx) * (y[1] - fy))) * inv_area;
					const f32 w1 = (((x[2] - fx) * (y[0] - fy)) - ((x[0] - fx) * (y[2] - fy))) * inv_area;
					const f32 w2 = 1.f - w0 - w1;

					if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
					{
						continue;
					}

					// Most Haze UI, including its full-screen loading image, uses one tint for the
					// whole primitive. Avoid four channels of barycentric interpolation per pixel in
					// that overwhelmingly common case; varying-colour HUD triangles retain the exact
					// interpolation path below.
					const u32 tint = flat_tint ? tint_bgra[0] : interpolate_bgra(tint_bgra, w0, w1, w2);
					u32 colour = tint;

					if (tex)
					{
						f32 su = (u[0] * w0) + (u[1] * w1) + (u[2] * w2);
						f32 sv = (v[0] * w0) + (v[1] * w1) + (v[2] * w2);
						apply_rect_shrink(su, sv, shrink, min_u, max_u, min_v, max_v);
						colour = modulate(
							sample_bgra(*tex, su, sv, clamp_uv, subrect_u, subrect_v, seam_rule, m_uv),
							tint);

						if (force_opaque)
						{
							colour |= 0xFF000000u;
						}
					}

					blend(static_cast<u32>(px), static_cast<u32>(py), colour);
				}
			}
		};

		// Round 61. The span path for the dominant primitive - textured, one tint, no rect shrink,
		// a plain colour sheet (not B8 coverage, not a flare) - which is every full-screen menu
		// quad this backend has measured. Four changes against the reference, none of which moves
		// a pixel:
		//   1. per scanline only the interval fast_span() proves can hold accepted pixels is
		//      visited, and the reference's inside test still runs on each of them, verbatim;
		//   2. the row base, the texture's fields, the tint and the clip rectangle are read once
		//      per triangle instead of reloaded per pixel behind every byte store;
		//   3. blend()'s three variable-divisor divisions go through div_by_alpha();
		//   4. the tint / texture / shrink / force_opaque branches are resolved once.
		// The barycentrics and the UVs are recomputed per pixel exactly as the reference does -
		// no incremental stepping, whose accumulated error could flip an edge test. m_pixels and
		// the addressing partition are summed in from locals, so both read the same as the
		// reference would have produced.
		const auto fast_loop = [&]()
		{
			const texture_entry& t = *tex;
			const u8* const texels = t.pixels.data();
			const u32 tw = t.width;
			const u32 th = t.height;
			const f32 ftw = static_cast<f32>(tw);
			const f32 fth = static_cast<f32>(th);
			const u8 wrap_u = t.wrap_u;
			const u8 wrap_v = t.wrap_v;
			const u32 tint = tint_bgra[0];
			const u32 opaque_mask = force_opaque ? 0xFF000000u : 0u;
			u8* const buffer = m_buffer.data();
			const usz stride = usz{m_width} * 4;
			const bool clip_enabled = m_clip_enabled;
			const f32 clip_x0 = m_clip[0];
			const f32 clip_y0 = m_clip[1];
			const f32 clip_x1 = m_clip[2];
			const f32 clip_y1 = m_clip[3];

			uv_address_counters counters{};
			u64 pixels = 0;
			u64 visited = 0;
			u64 opaque = 0;
			u64 translucent = 0;
			u64 transparent = 0;

			for (s32 py = min_y; py <= max_y; ++py)
			{
				const f32 fy = static_cast<f32>(py) + 0.5f;
				s32 px_lo = 0;
				s32 px_hi = -1;

				if (!fast_span(x, y, fy, area, min_x, max_x, px_lo, px_hi))
				{
					continue;
				}

				u8* const row = buffer + (usz{static_cast<u32>(py)} * stride);
				const f32 fpy = static_cast<f32>(py);

				for (s32 px = px_lo; px <= px_hi; ++px)
				{
					++visited;
					const f32 fx = static_cast<f32>(px) + 0.5f;

					const f32 w0 = (((x[1] - fx) * (y[2] - fy)) - ((x[2] - fx) * (y[1] - fy))) * inv_area;
					const f32 w1 = (((x[2] - fx) * (y[0] - fy)) - ((x[0] - fx) * (y[2] - fy))) * inv_area;
					const f32 w2 = 1.f - w0 - w1;

					if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
					{
						continue;
					}

					f32 su = (u[0] * w0) + (u[1] * w1) + (u[2] * w2);
					f32 sv = (v[0] * w0) + (v[1] * w1) + (v[2] * w2);

					// sample_bgra() for this primitive class, 0 for a clipped coordinate included.
					const bool keep_u = address_coordinate(su, wrap_u, clamp_uv, subrect_u, seam_rule, counters);
					const bool keep_v = address_coordinate(sv, wrap_v, clamp_uv, subrect_v, seam_rule, counters);
					u32 sampled = 0;

					if (keep_u && keep_v)
					{
						const u32 tx = std::min(tw - 1, static_cast<u32>(su * ftw));
						const u32 ty = std::min(th - 1, static_cast<u32>(sv * fth));
						std::memcpy(&sampled, texels + ((usz{ty} * tw + tx) * 4), sizeof(u32));
					}

					const u32 colour = modulate(sampled, tint) | opaque_mask;

					// blend(), with the destination walked along the row instead of recomputed.
					++pixels;
					const u32 alpha = (colour >> 24) & 0xFF;

					if (alpha == 0)
					{
						++transparent;
						continue;
					}

					if (clip_enabled &&
						(static_cast<f32>(px) < clip_x0 || static_cast<f32>(px) > clip_x1 ||
						 fpy < clip_y0 || fpy > clip_y1))
					{
						continue;
					}

					u8* const dst = row + (usz{static_cast<u32>(px)} * 4);

					if (alpha == 0xFF)
					{
						++opaque;
						std::memcpy(dst, &colour, sizeof(u32));
						continue;
					}

					++translucent;
					const u32 inv = 255 - alpha;
					const u32 dst_alpha = dst[3];
					const u32 out_alpha = alpha + ((dst_alpha * inv + 127) / 255);

					for (u32 c = 0; c < 3; ++c)
					{
						const u32 src = (colour >> (c * 8)) & 0xFF;
						const u32 dst_term = (u32{dst[c]} * dst_alpha * inv + 127) / 255;
						dst[c] = static_cast<u8>(div_by_alpha((src * alpha) + dst_term + (out_alpha / 2), out_alpha));
					}

					dst[3] = static_cast<u8>(out_alpha);
				}
			}

			m_pixels += pixels;
			m_uv.in += counters.in;
			m_uv.wrap += counters.wrap;
			m_uv.seam += counters.seam;
			m_uv.mirror += counters.mirror;
			m_uv.clip += counters.clip;
			m_uv.clamp += counters.clamp;
			m_raster.visited += visited;
			m_raster.opaque += opaque;
			m_raster.translucent += translucent;
			m_raster.transparent += transparent;
		};

		const u32 fast_mode = ui_fast_raster_mode();
		const bool fast = fast_mode != 0 && tex && flat_tint && shrink == 100
			&& !tex->pixels.empty() && tex->width != 0 && tex->height != 0
			&& !tex->b8_coverage && !demons_flare_bilinear(*tex);

		m_raster.bbox_px += u64{static_cast<u32>(max_x - min_x + 1)} * static_cast<u32>(max_y - min_y + 1);

		if (!fast)
		{
			++m_raster.ref_tris;
			reference_loop();
		}
		else
		{
			const bool verify = fast_mode >= 2;

			if (verify)
			{
				// The reference draws this triangle into a copy of the buffer, the span path draws
				// it into the real one, and the two are compared whole. The counters the reference
				// bumps are put back so the window's totals describe the span path alone.
				const u64 pixels_before = m_pixels;
				const uv_address_counters uv_before = m_uv;
				m_verify.assign(m_buffer.begin(), m_buffer.end());
				std::swap(m_buffer, m_verify);
				reference_loop();
				std::swap(m_buffer, m_verify);
				m_pixels = pixels_before;
				m_uv = uv_before;
			}

			++m_raster.fast_tris;
			fast_loop();

			if (verify)
			{
				++m_raster.verify_tris;

				if (std::memcmp(m_buffer.data(), m_verify.data(), m_buffer.size()) != 0)
				{
					const usz texels = usz{m_width} * m_height;
					u64 bad = 0;

					for (usz i = 0; i < texels; ++i)
					{
						u32 got = 0;
						u32 ref = 0;
						std::memcpy(&got, m_buffer.data() + (i * 4), sizeof(u32));
						std::memcpy(&ref, m_verify.data() + (i * 4), sizeof(u32));

						if (got != ref)
						{
							if (bad == 0)
							{
								m_verify_first = { static_cast<u32>(i % m_width), static_cast<u32>(i / m_width), ref, got };
							}

							++bad;
						}
					}

					m_raster.verify_bad_px += bad;
				}
			}
		}

		// Round 26: counted HERE, beside m_dirty/m_draws, and not at the shrink itself. A review
		// defect: the degenerate-area return and the off-screen-bbox return both sit between the
		// two, so incrementing early would count primitives that were never rasterized and the
		// counter would disagree with m_draws. It says "primitives actually corrected", so it has
		// to be on the same path as the draw.
		if (shrink != 100)
		{
			++m_uv.rect_shrunk;
		}

		m_dirty = true;
		++m_draws;
	}

	void compositor::draw_triangle(const f32 (&x)[3], const f32 (&y)[3], const f32 (&u)[3], const f32 (&v)[3],
		const texture_entry* tex, u32 tint_bgra, bool clamp_uv, bool force_opaque)
	{
		const u32 tints[3] = { tint_bgra, tint_bgra, tint_bgra };
		draw_triangle(x, y, u, v, tex, tints, clamp_uv, force_opaque);
	}

	void compositor::draw_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1,
		const texture_entry* tex, u32 tint_bgra, bool clamp_uv)
	{
		if (m_buffer.empty())
		{
			return;
		}

		if (x1 < x0) std::swap(x0, x1), std::swap(u0, u1);
		if (y1 < y0) std::swap(y0, y1), std::swap(v0, v1);

		// Round 7: same sub-rect test as the triangle rasterizer, from this quad's own two UV
		// endpoints. rpcs3's own overlay quads reach here with force_clamp already true and so
		// never enter the new rule; the title's axis-aligned UI quads do.
		// Round 8: same intersection term - a quad authored v in [-1.8, -1.2] is a tile offset,
		// not seam slop, and must keep true repeat instead of collapsing onto row 0.
		const f32 min_u = std::min(u0, u1);
		const f32 max_u = std::max(u0, u1);
		const f32 min_v = std::min(v0, v1);
		const f32 max_v = std::max(v0, v1);
		const bool subrect_u = is_seam_subrect(min_u, max_u);
		const bool subrect_v = is_seam_subrect(min_v, max_v);
		const bool seam_rule = ui_clamp_subrect_mode() != 0;

		// Round 25: the same rule as the triangle path, from this quad's own endpoints. Both
		// rasterizers read it so they cannot drift apart, which is how the round-8 intersection
		// term came to be missing from one of them for a round.
		//
		// Round 26: hoisted above the pixel bounds, because the screen half of the rule shrinks
		// x0/y0/x1/y1 themselves. This entry point is axis-aligned in both spaces by construction,
		// so it needs no equivalent of the triangle path's shape test.
		//
		// HONESTY, from the review: this arm is currently UNREACHABLE. All four draw_quad call
		// sites pass tex = nullptr (rpcs3's own overlay bars and the UI probe), so `shrink` here is
		// always 100 and the round-25 apply_rect_shrink below is equally dead. It is kept, and kept
		// identical to the triangle path, because "both rasterizers read the same rule" is how the
		// round-8 intersection term came to be missing from one of them for a whole round. It means
		// rect_shrunk is only ever incremented from draw_triangle - i.e. in TRIANGLES, two per
		// glyph quad.
		const u32 shrink = (tex && ui_rect_shrink_count() != 0)
			? ui_rect_shrink_for(*tex, min_u, max_u, min_v, max_v)
			: 100u;

		if (shrink != 100)
		{
			const f32 k = static_cast<f32>(shrink) / 100.f;
			const f32 cx = (x0 + x1) * 0.5f;
			const f32 cy = (y0 + y1) * 0.5f;

			x0 = cx + ((x0 - cx) * k);
			x1 = cx + ((x1 - cx) * k);
			y0 = cy + ((y0 - cy) * k);
			y1 = cy + ((y1 - cy) * k);
		}

		const s32 min_x = std::max<s32>(0, static_cast<s32>(std::floor(x0)));
		const s32 max_x = std::min<s32>(static_cast<s32>(m_width) - 1, static_cast<s32>(std::ceil(x1)) - 1);
		const s32 min_y = std::max<s32>(0, static_cast<s32>(std::floor(y0)));
		const s32 max_y = std::min<s32>(static_cast<s32>(m_height) - 1, static_cast<s32>(std::ceil(y1)) - 1);

		if (min_x > max_x || min_y > max_y)
		{
			return;
		}

		const f32 span_x = std::max(1e-6f, x1 - x0);
		const f32 span_y = std::max(1e-6f, y1 - y0);

		for (s32 py = min_y; py <= max_y; ++py)
		{
			const f32 ty = ((static_cast<f32>(py) + 0.5f) - y0) / span_y;
			const f32 base_v = v0 + ((v1 - v0) * ty);

			for (s32 px = min_x; px <= max_x; ++px)
			{
				const f32 tx = ((static_cast<f32>(px) + 0.5f) - x0) / span_x;
				f32 su = u0 + ((u1 - u0) * tx);
				f32 sv = base_v;
				apply_rect_shrink(su, sv, shrink, min_u, max_u, min_v, max_v);

				const u32 colour = tex
					? modulate(sample_bgra(*tex, su, sv, clamp_uv, subrect_u, subrect_v, seam_rule, m_uv),
						tint_bgra)
					: tint_bgra;
				blend(static_cast<u32>(px), static_cast<u32>(py), colour);
			}
		}

		// Same reasoning as the triangle path: counted beside the draw, not at the shrink, so the
		// off-screen-bbox return above cannot inflate it.
		if (shrink != 100)
		{
			++m_uv.rect_shrunk;
		}

		m_dirty = true;
		++m_draws;
	}

	void compositor::draw_glyph_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1,
		const u8* coverage, u32 cov_width, u32 cov_height, u32 tint_bgra)
	{
		if (m_buffer.empty() || !coverage || cov_width == 0 || cov_height == 0)
		{
			return;
		}

		if (x1 < x0) std::swap(x0, x1), std::swap(u0, u1);
		if (y1 < y0) std::swap(y0, y1), std::swap(v0, v1);

		const s32 min_x = std::max<s32>(0, static_cast<s32>(std::floor(x0)));
		const s32 max_x = std::min<s32>(static_cast<s32>(m_width) - 1, static_cast<s32>(std::ceil(x1)) - 1);
		const s32 min_y = std::max<s32>(0, static_cast<s32>(std::floor(y0)));
		const s32 max_y = std::min<s32>(static_cast<s32>(m_height) - 1, static_cast<s32>(std::ceil(y1)) - 1);

		if (min_x > max_x || min_y > max_y)
		{
			return;
		}

		const f32 span_x = std::max(1e-6f, x1 - x0);
		const f32 span_y = std::max(1e-6f, y1 - y0);
		const u32 tint_rgb = tint_bgra & 0x00FFFFFFu;
		const u32 tint_a = (tint_bgra >> 24) & 0xFF;

		for (s32 py = min_y; py <= max_y; ++py)
		{
			const f32 ty = ((static_cast<f32>(py) + 0.5f) - y0) / span_y;
			const f32 sv = std::clamp(v0 + ((v1 - v0) * ty), 0.f, 1.f);
			const u32 cy = std::min(cov_height - 1, static_cast<u32>(sv * static_cast<f32>(cov_height)));

			for (s32 px = min_x; px <= max_x; ++px)
			{
				const f32 tx = ((static_cast<f32>(px) + 0.5f) - x0) / span_x;
				const f32 su = std::clamp(u0 + ((u1 - u0) * tx), 0.f, 1.f);
				const u32 cx = std::min(cov_width - 1, static_cast<u32>(su * static_cast<f32>(cov_width)));

				const u32 cov = coverage[(usz{cy} * cov_width) + cx];

				if (cov == 0)
				{
					continue;
				}

				const u32 alpha = ((cov * tint_a) + 127) / 255;
				blend(static_cast<u32>(px), static_cast<u32>(py), tint_rgb | (alpha << 24));
			}
		}

		m_dirty = true;
		++m_draws;
	}

	void compositor::underlay(u32 bgra)
	{
		const usz texels = usz{m_width} * m_height;

		if (texels == 0 || m_buffer.size() < texels * 4)
		{
			return;
		}

		const u32 bg_b = bgra & 0xFF;
		const u32 bg_g = (bgra >> 8) & 0xFF;
		const u32 bg_r = (bgra >> 16) & 0xFF;

		u8* p = m_buffer.data();

		for (usz i = 0; i < texels; ++i, p += 4)
		{
			const u32 a = p[3];

			if (a == 0xFF)
			{
				continue;
			}

			if (a == 0)
			{
				p[0] = static_cast<u8>(bg_b);
				p[1] = static_cast<u8>(bg_g);
				p[2] = static_cast<u8>(bg_r);
				p[3] = 0xFF;
				continue;
			}

			// The buffer is straight alpha (see blend()), so this is plain 'src over opaque dst':
			// out = src*a + bg*(1-a), and the result is opaque by construction. Doing it with
			// premultiplied maths here would darken every partially covered glyph edge.
			const u32 inv = 0xFFu - a;
			p[0] = static_cast<u8>((u32{p[0]} * a + bg_b * inv + 127) / 255);
			p[1] = static_cast<u8>((u32{p[1]} * a + bg_g * inv + 127) / 255);
			p[2] = static_cast<u8>((u32{p[2]} * a + bg_r * inv + 127) / 255);
			p[3] = 0xFF;
		}

		// Counted like any other shaded pixel: a full-screen pass is the compositor's largest
		// single item and the timing line must not hide it.
		m_pixels += texels;
		m_dirty = true;
	}

	u32 compositor::submit(const remixapi_Interface& api)
	{
		if (!m_dirty || m_buffer.empty())
		{
			return REMIXAPI_ERROR_CODE_SUCCESS;
		}

		++m_frames;

		return guarded_draw_screen_overlay(api.DrawScreenOverlay,
			m_buffer.data(), m_width, m_height, REMIXAPI_FORMAT_B8G8R8A8_UNORM, 1.f);
	}

	u64 compositor::checksum() const
	{
		// FNV-1a's constants over 64-bit words rather than bytes: eight times fewer dependent
		// multiplies across a 7.7 MB buffer, still order-sensitive, and only read behind UISUM.
		u64 hash = 0xcbf29ce484222325ull;
		const u8* p = m_buffer.data();
		const usz words = m_buffer.size() / sizeof(u64);

		for (usz i = 0; i < words; ++i, p += sizeof(u64))
		{
			u64 word = 0;
			std::memcpy(&word, p, sizeof(u64));
			hash = (hash ^ word) * 0x100000001b3ull;
		}

		for (usz i = words * sizeof(u64); i < m_buffer.size(); ++i)
		{
			hash = (hash ^ m_buffer[i]) * 0x100000001b3ull;
		}

		return hash;
	}
}

#endif
