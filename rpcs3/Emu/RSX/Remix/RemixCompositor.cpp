#include "stdafx.h"
#include "RemixCompositor.h"

#ifdef _WIN32

#include "Emu/RSX/Remix/RemixRuntime.h"
#include "Emu/RSX/Remix/RemixTextures.h"

#include <algorithm>
#include <cmath>

namespace remix_rsx
{
	namespace
	{
		// Sanity ceiling on the overlay target. 4K RGBA8 is 32 MiB.
		constexpr u32 s_max_dimension = 4096;

		u32 sample_bgra(const texture_entry& tex, f32 u, f32 v, bool clamp_uv)
		{
			if (tex.pixels.empty() || tex.width == 0 || tex.height == 0)
			{
				return 0xFFFFFFFFu;
			}

			f32 su = u;
			f32 sv = v;

			if (clamp_uv)
			{
				su = std::clamp(su, 0.f, 1.f);
				sv = std::clamp(sv, 0.f, 1.f);
			}
			else
			{
				su -= std::floor(su);
				sv -= std::floor(sv);
			}

			const u32 x = std::min(tex.width - 1, static_cast<u32>(su * static_cast<f32>(tex.width)));
			const u32 y = std::min(tex.height - 1, static_cast<u32>(sv * static_cast<f32>(tex.height)));

			u32 texel;
			std::memcpy(&texel, tex.pixels.data() + ((usz{y} * tex.width + x) * 4), sizeof(u32));
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
	}

	bool compositor_disabled()
	{
		static const bool value = []
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_NOUI", buffer, static_cast<DWORD>(std::size(buffer)));
			return written > 0 && written < std::size(buffer) && ::wcstol(buffer, nullptr, 10) != 0;
		}();

		return value;
	}

	bool keep_render_target_blits()
	{
		static const bool value = []
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_KEEPRT", buffer, static_cast<DWORD>(std::size(buffer)));
			return written > 0 && written < std::size(buffer) && ::wcstol(buffer, nullptr, 10) != 0;
		}();

		return value;
	}

	usz mesh_cap()
	{
		static const usz value = []() -> usz
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

		return value;
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
		static const u32 value = []
		{
			wchar_t buffer[16]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIWIDTH", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				// 1080p class. Everything this rasterizer draws is authored at the guest's own
				// surface resolution (720p on both test titles) or, for rpcs3's overlays, a
				// virtual 1280x720, so more pixels than this buy no detail at all.
				return 1920u;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);
			return (parsed >= 0) ? static_cast<u32>(parsed) : 1920u;
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

		// Straight alpha over, which is what DrawScreenOverlay's compute pass expects.
		const u32 inv = 255 - alpha;

		for (u32 c = 0; c < 3; ++c)
		{
			const u32 src = (src_bgra >> (c * 8)) & 0xFF;
			dst[c] = static_cast<u8>(((src * alpha) + (dst[c] * inv) + 127) / 255);
		}

		dst[3] = static_cast<u8>(std::min<u32>(255, alpha + ((dst[3] * inv) + 127) / 255));
	}

	void compositor::draw_triangle(const f32 (&x)[3], const f32 (&y)[3], const f32 (&u)[3], const f32 (&v)[3],
		const texture_entry* tex, u32 tint_bgra, bool clamp_uv)
	{
		if (m_buffer.empty())
		{
			return;
		}

		const f32 area = ((x[1] - x[0]) * (y[2] - y[0])) - ((x[2] - x[0]) * (y[1] - y[0]));

		if (!std::isfinite(area) || std::abs(area) < 1e-6f)
		{
			return;
		}

		const f32 inv_area = 1.f / area;

		const s32 min_x = std::max<s32>(0, static_cast<s32>(std::floor(std::min({ x[0], x[1], x[2] }))));
		const s32 max_x = std::min<s32>(static_cast<s32>(m_width) - 1, static_cast<s32>(std::ceil(std::max({ x[0], x[1], x[2] }))));
		const s32 min_y = std::max<s32>(0, static_cast<s32>(std::floor(std::min({ y[0], y[1], y[2] }))));
		const s32 max_y = std::min<s32>(static_cast<s32>(m_height) - 1, static_cast<s32>(std::ceil(std::max({ y[0], y[1], y[2] }))));

		if (min_x > max_x || min_y > max_y)
		{
			return;
		}

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

				u32 colour = tint_bgra;

				if (tex)
				{
					const f32 su = (u[0] * w0) + (u[1] * w1) + (u[2] * w2);
					const f32 sv = (v[0] * w0) + (v[1] * w1) + (v[2] * w2);
					colour = modulate(sample_bgra(*tex, su, sv, clamp_uv), tint_bgra);
				}

				blend(static_cast<u32>(px), static_cast<u32>(py), colour);
			}
		}

		m_dirty = true;
		++m_draws;
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
			const f32 sv = v0 + ((v1 - v0) * ty);

			for (s32 px = min_x; px <= max_x; ++px)
			{
				const f32 tx = ((static_cast<f32>(px) + 0.5f) - x0) / span_x;
				const f32 su = u0 + ((u1 - u0) * tx);

				const u32 colour = tex ? modulate(sample_bgra(*tex, su, sv, clamp_uv), tint_bgra) : tint_bgra;
				blend(static_cast<u32>(px), static_cast<u32>(py), colour);
			}
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
}

#endif
