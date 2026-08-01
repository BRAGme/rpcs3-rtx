#include "stdafx.h"
#include "RemixVertexDecode.h"

#ifdef _WIN32

#include <bit>
#include <cmath>
#include <limits>

namespace remix_rsx
{
	namespace
	{
		// Component byte width per rsx::vertex_base_type value.
		// Mirrors elem_size_table in RSXVertexFetch.glsl; index 0 is the undefined type.
		constexpr u32 s_elem_size[8] = { 0, 2, 4, 2, 1, 2, 4, 1 };

		// Post-decode divisor per type. Mirrors scaling_table in the same shader.
		constexpr f32 s_scale[8] = { 1.f, 32767.5f, 1.f, 1.f, 255.f, 1.f, 32767.f, 1.f };

		// The shader's sext(): treat a 16-bit field as signed.
		f32 sext16(u32 bits)
		{
			return (bits < 0x8000u) ? static_cast<f32>(bits) : static_cast<f32>(static_cast<s32>(bits) - 65536);
		}

		f32 half_to_float(u16 bits)
		{
			const u32 sign = (bits >> 15) & 1u;
			const s32 exponent = (bits >> 10) & 0x1F;
			const u32 mantissa = bits & 0x3FFu;

			f32 value;

			if (exponent == 0)
			{
				value = std::ldexp(static_cast<f32>(mantissa), -24);
			}
			else if (exponent == 31)
			{
				value = mantissa ? std::numeric_limits<f32>::quiet_NaN() : std::numeric_limits<f32>::infinity();
			}
			else
			{
				value = std::ldexp(static_cast<f32>(mantissa | 0x400u), exponent - 25);
			}

			return sign ? -value : value;
		}

		u32 read_be(const u8* src, u32 width)
		{
			switch (width)
			{
			case 1: return u32{src[0]};
			case 2: return (u32{src[0]} << 8) | u32{src[1]};
			case 4: return (u32{src[0]} << 24) | (u32{src[1]} << 16) | (u32{src[2]} << 8) | u32{src[3]};
			default: return 0;
			}
		}
	}

	bool is_supported_primitive(rsx::primitive_type prim)
	{
		switch (prim)
		{
		case rsx::primitive_type::triangles:
		case rsx::primitive_type::triangle_strip:
		case rsx::primitive_type::triangle_fan:
		case rsx::primitive_type::quads:
		case rsx::primitive_type::polygon:
			return true;
		default:
			// points, lines, line_loop, line_strip: not a triangle family.
			// quad_strip: BufferUtils has no expansion for it either.
			return false;
		}
	}

	bool is_buffer_utils_expandable(rsx::primitive_type prim)
	{
		switch (prim)
		{
		case rsx::primitive_type::triangle_fan:
		case rsx::primitive_type::polygon:
		case rsx::primitive_type::quads:
			return true;
		default:
			return false;
		}
	}

	u32 attribute_byte_size(rsx::vertex_base_type type, u32 size)
	{
		const u32 type_index = static_cast<u32>(type);

		if (type_index == 0 || type_index > 7)
		{
			return 0;
		}

		const u32 components = (type == rsx::vertex_base_type::cmp) ? 1u : size;

		if (components == 0 || components > 4)
		{
			return 0;
		}

		return components * s_elem_size[type_index];
	}

	bool decode_position(const u8* src, rsx::vertex_base_type type, u32 size, f32 (&out)[4])
	{
		const u32 type_index = static_cast<u32>(type);

		if (type_index == 0 || type_index > 7)
		{
			return false;
		}

		const u32 elem_size = s_elem_size[type_index];
		const f32 scale = s_scale[type_index];

		// CMP32 packs all components into a single dword; the layout code forces size to 1.
		const u32 components = (type == rsx::vertex_base_type::cmp) ? 1u : size;

		if (components == 0 || components > 4)
		{
			return false;
		}

		u32 raw[4] = {};
		for (u32 i = 0; i < components; ++i)
		{
			raw[i] = read_be(src + (i * elem_size), elem_size);
		}

		f32 decoded[4] = { 0.f, 0.f, 0.f, 0.f };

		switch (type)
		{
		case rsx::vertex_base_type::s1:
		case rsx::vertex_base_type::s32k:
		{
			// SNORM16 is biased by half a unit before the divide; SINT16 is not.
			const f32 bias = (type == rsx::vertex_base_type::s1) ? 0.5f : 0.f;
			for (u32 i = 0; i < components; ++i)
			{
				decoded[i] = sext16(raw[i]) + bias;
			}
			break;
		}
		case rsx::vertex_base_type::f:
		{
			for (u32 i = 0; i < components; ++i)
			{
				decoded[i] = std::bit_cast<f32>(raw[i]);
			}
			break;
		}
		case rsx::vertex_base_type::sf:
		{
			for (u32 i = 0; i < components; ++i)
			{
				decoded[i] = half_to_float(static_cast<u16>(raw[i]));
			}
			break;
		}
		case rsx::vertex_base_type::ub:
		case rsx::vertex_base_type::ub256:
		{
			for (u32 i = 0; i < components; ++i)
			{
				decoded[i] = static_cast<f32>(raw[i]);
			}
			break;
		}
		case rsx::vertex_base_type::cmp:
		{
			// X11Y11Z10, each field left-aligned into a 16-bit signed field before extension.
			const u32 x = (raw[0] >> 0) & 0x7FFu;
			const u32 y = (raw[0] >> 11) & 0x7FFu;
			const u32 z = (raw[0] >> 22) & 0x3FFu;

			decoded[0] = sext16((x << 5) & 0xFFFFu);
			decoded[1] = sext16((y << 5) & 0xFFFFu);
			decoded[2] = sext16((z << 6) & 0xFFFFu);
			decoded[3] = scale;
			break;
		}
		default:
			return false;
		}

		// The shader forces w to the scale so that narrow attributes divide down to 1.0.
		if (components < 4)
		{
			decoded[3] = scale;
		}

		for (u32 i = 0; i < 4; ++i)
		{
			out[i] = decoded[i] / scale;
		}

		return true;
	}

	void strip_to_list(u32 first, u32 count, std::vector<u32>& out)
	{
		if (count < 3)
		{
			return;
		}

		out.reserve(out.size() + (static_cast<usz>(count) - 2) * 3);

		for (u32 i = 0; (i + 2) < count; ++i)
		{
			// Alternate the winding so the list keeps the strip's facing.
			if (i & 1)
			{
				out.push_back(first + i + 1);
				out.push_back(first + i);
			}
			else
			{
				out.push_back(first + i);
				out.push_back(first + i + 1);
			}

			out.push_back(first + i + 2);
		}
	}

	void strip_to_list(const u32* indices, u32 count, std::vector<u32>& out)
	{
		if (!indices || count < 3)
		{
			return;
		}

		out.reserve(out.size() + (static_cast<usz>(count) - 2) * 3);

		for (u32 i = 0; (i + 2) < count; ++i)
		{
			if (i & 1)
			{
				out.push_back(indices[i + 1]);
				out.push_back(indices[i]);
			}
			else
			{
				out.push_back(indices[i]);
				out.push_back(indices[i + 1]);
			}

			out.push_back(indices[i + 2]);
		}
	}
}

#endif
