#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace remix_rsx::preview_raster
{
	struct vertex
	{
		float clip[4] = { 0.f, 0.f, 0.f, 1.f };
		float uv[2] = {};
		std::uint32_t color = 0xffffffffu;
	};

	struct viewport
	{
		float scale_x = 1.f, scale_y = 1.f, offset_x = 0.f, offset_y = 0.f;
		float depth_scale = 1.f, depth_offset = 0.f;
		float clip_left = 0.f, clip_top = 0.f;
		float clip_right = std::numeric_limits<float>::infinity();
		float clip_bottom = std::numeric_limits<float>::infinity();
	};

	struct texture_view
	{
		const void* pixels = nullptr;
		std::uint32_t width = 0, height = 0;
		bool wrap_u = false, wrap_v = false;
		bool force_opaque = false;
	};

	// Small offscreen menu surfaces only. The ordinary screen compositor is unaffected.
	class surface
	{
		struct clipped_vertex
		{
			float clip[4]{}, uv[2]{}, color[4]{};
		};

		static clipped_vertex mix(const clipped_vertex& a, const clipped_vertex& b, float t)
		{
			clipped_vertex out{};
			for (unsigned c = 0; c < 4; ++c)
			{
				out.clip[c] = a.clip[c] + (b.clip[c] - a.clip[c]) * t;
				out.color[c] = a.color[c] + (b.color[c] - a.color[c]) * t;
			}
			for (unsigned c = 0; c < 2; ++c)
				out.uv[c] = a.uv[c] + (b.uv[c] - a.uv[c]) * t;
			return out;
		}

		static float plane_distance(const clipped_vertex& v, unsigned plane)
		{
			switch (plane)
			{
			case 0: return v.clip[3] - 1e-5f;
			case 1: return v.clip[0] + v.clip[3];
			case 2: return v.clip[3] - v.clip[0];
			case 3: return v.clip[1] + v.clip[3];
			case 4: return v.clip[3] - v.clip[1];
			case 5: return v.clip[2];
			default: return v.clip[3] - v.clip[2];
			}
		}

		static std::uint32_t sample(const texture_view& tex, float u, float v)
		{
			if (!tex.pixels || !tex.width || !tex.height) return 0xffffffffu;
			const auto coordinate = [](float value, std::uint32_t size, bool wrap)
			{
				value = wrap ? value - std::floor(value) : std::clamp(value, 0.f, 1.f);
				return std::min(size - 1, static_cast<std::uint32_t>(value * size));
			};
			const auto offset = std::size_t(coordinate(v, tex.height, tex.wrap_v)) * tex.width + coordinate(u, tex.width, tex.wrap_u);
			std::uint32_t pixel = 0;
			std::memcpy(&pixel, static_cast<const std::uint8_t*>(tex.pixels) + offset * sizeof(pixel), sizeof(pixel));
			return tex.force_opaque ? pixel | 0xff000000u : pixel;
		}

		static std::uint32_t over(std::uint32_t source, std::uint32_t dest)
		{
			const unsigned alpha = source >> 24;
			const unsigned inv = 255 - alpha;
			const unsigned da = dest >> 24;
			const unsigned oa = alpha + (da * inv + 127) / 255;
			if (!oa) return 0;
			std::uint32_t out = oa << 24;
			for (unsigned c = 0; c < 3; ++c)
			{
				const unsigned sc = (source >> (c * 8)) & 255;
				const unsigned dc = (dest >> (c * 8)) & 255;
				out |= ((sc * alpha + (dc * da * inv + 127) / 255 + oa / 2) / oa) << (c * 8);
			}
			return out;
		}

		void rasterize(std::array<clipped_vertex, 3> triangle, const viewport& vp, const texture_view& tex,
			bool depth_write, bool blend, std::uint32_t alpha_ref, bool depth_test)
		{
			float x[3]{}, y[3]{}, inv_w[3]{}, z[3]{};
			for (unsigned k = 0; k < 3; ++k)
			{
				inv_w[k] = 1.f / triangle[k].clip[3];
				x[k] = triangle[k].clip[0] * inv_w[k] * vp.scale_x + vp.offset_x;
				y[k] = triangle[k].clip[1] * inv_w[k] * vp.scale_y + vp.offset_y;
				z[k] = triangle[k].clip[2] * inv_w[k] * vp.depth_scale + vp.depth_offset;
				if (!std::isfinite(x[k]) || !std::isfinite(y[k]) || !std::isfinite(z[k])) return;
			}
			const auto edge = [&](unsigned a, unsigned b, float px, float py)
			{
				return (x[b] - x[a]) * (py - y[a]) - (y[b] - y[a]) * (px - x[a]);
			};
			const float area = edge(0, 1, x[2], y[2]);
			if (std::abs(area) < 1e-8f || !std::isfinite(area)) return;
			const float low_x = std::max({ 0.f, vp.clip_left, std::min({ x[0], x[1], x[2] }) });
			const float low_y = std::max({ 0.f, vp.clip_top, std::min({ y[0], y[1], y[2] }) });
			const float high_x = std::min({ float(width), vp.clip_right, std::max({ x[0], x[1], x[2] }) });
			const float high_y = std::min({ float(height), vp.clip_bottom, std::max({ y[0], y[1], y[2] }) });
			if (!(low_x < high_x && low_y < high_y)) return;
			const int min_x = std::max(0, static_cast<int>(std::ceil(low_x - .5f)));
			const int min_y = std::max(0, static_cast<int>(std::ceil(low_y - .5f)));
			const int max_x = std::min(int(width) - 1, static_cast<int>(std::ceil(high_x - .5f)));
			const int max_y = std::min(int(height) - 1, static_cast<int>(std::ceil(high_y - .5f)));
			for (int py = min_y; py <= max_y; ++py)
				for (int px = min_x; px <= max_x; ++px)
				{
					const float fx = px + .5f, fy = py + .5f;
					if (fx >= vp.clip_right || fy >= vp.clip_bottom) continue;
					const float b[3] = { edge(1, 2, fx, fy) / area, edge(2, 0, fx, fy) / area, edge(0, 1, fx, fy) / area };
					const auto includes_edge = [&](unsigned a, unsigned next)
					{
						if (area < 0.f) std::swap(a, next);
						return y[next] < y[a] || (y[next] == y[a] && x[next] > x[a]);
					};
					if (b[0] < 0.f || b[1] < 0.f || b[2] < 0.f
						|| (b[0] == 0.f && !includes_edge(1, 2))
						|| (b[1] == 0.f && !includes_edge(2, 0))
						|| (b[2] == 0.f && !includes_edge(0, 1))) continue;
					const float dz = b[0] * z[0] + b[1] * z[1] + b[2] * z[2];
					const std::size_t offset = std::size_t(py) * width + px;
					if (depth_test && dz > depth[offset]) continue;
					const float iw = b[0] * inv_w[0] + b[1] * inv_w[1] + b[2] * inv_w[2];
					if (!(iw > 0.f) || !std::isfinite(iw)) continue;
					float uv[2]{}, color[4]{};
					for (unsigned k = 0; k < 3; ++k)
					{
						const float weight = b[k] * inv_w[k] / iw;
						for (unsigned c = 0; c < 2; ++c) uv[c] += weight * triangle[k].uv[c];
						for (unsigned c = 0; c < 4; ++c) color[c] += weight * triangle[k].color[c];
					}
					if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) continue;
					const auto texel = sample(tex, uv[0], uv[1]);
					std::uint32_t shaded = 0;
					for (unsigned c = 0; c < 4; ++c)
					{
						const auto channel = static_cast<std::uint32_t>(std::clamp(float((texel >> (8 * c)) & 255) * color[c], 0.f, 255.f) + .5f);
						shaded |= channel << (8 * c);
					}
					if ((shaded >> 24) <= alpha_ref) continue;
					pixels[offset] = blend ? over(shaded, pixels[offset]) : shaded;
					if (depth_write) depth[offset] = dz;
				}
		}

	public:
		std::uint32_t width = 0, height = 0;
		std::vector<std::uint32_t> pixels;
		std::vector<float> depth;

		void reset(std::uint32_t w, std::uint32_t h, std::uint32_t color = 0)
		{
			width = w; height = h;
			pixels.assign(std::size_t(w) * h, color);
			depth.assign(pixels.size(), std::numeric_limits<float>::infinity());
		}

		void draw_triangle(const std::array<vertex, 3>& input, const viewport& vp, const texture_view& tex = {},
			bool depth_write = true, bool blend = false, std::uint32_t alpha_ref = 0, bool depth_test = true)
		{
			if (!width || !height || pixels.empty()) return;
			std::array<clipped_vertex, 16> polygon{}, output{};
			unsigned count = 3;
			for (unsigned k = 0; k < 3; ++k)
			{
				for (unsigned c = 0; c < 4; ++c)
				{
					if (!std::isfinite(input[k].clip[c])) return;
					polygon[k].clip[c] = input[k].clip[c];
					polygon[k].color[c] = float((input[k].color >> (8 * c)) & 255) / 255.f;
				}
				for (unsigned c = 0; c < 2; ++c)
				{
					if (!std::isfinite(input[k].uv[c])) return;
					polygon[k].uv[c] = input[k].uv[c];
				}
			}
			for (unsigned plane = 0; plane < 7 && count; ++plane)
			{
				unsigned next_count = 0;
				auto previous = polygon[count - 1];
				float previous_distance = plane_distance(previous, plane);
				for (unsigned i = 0; i < count; ++i)
				{
					const auto current = polygon[i];
					const float distance = plane_distance(current, plane);
					if ((distance >= 0.f) != (previous_distance >= 0.f))
						output[next_count++] = mix(previous, current, previous_distance / (previous_distance - distance));
					if (distance >= 0.f) output[next_count++] = current;
					previous = current; previous_distance = distance;
				}
				polygon = output; count = next_count;
			}
			for (unsigned i = 1; i + 1 < count; ++i)
				rasterize({ polygon[0], polygon[i], polygon[i + 1] }, vp, tex, depth_write, blend, alpha_ref, depth_test);
		}
	};
}
