#pragma once

#ifdef _WIN32

#include "util/types.hpp"
#include "Emu/RSX/gcm_enums.h"

#include <vector>

namespace remix_rsx
{
	// Primitives this backend can turn into a triangle list. Everything else is counted and skipped.
	bool is_supported_primitive(rsx::primitive_type prim);

	// True when BufferUtils' index expansion machinery understands this primitive.
	// Triangle strips are deliberately excluded: they are native to GL/VK so BufferUtils
	// leaves them alone, and we expand them ourselves.
	bool is_buffer_utils_expandable(rsx::primitive_type prim);

	// Guest-side byte width of one attribute of this type/size. Unlike
	// rsx::get_vertex_type_size_on_host this does not pad 3 components up to 4.
	// Returns 0 for a format we do not decode.
	u32 attribute_byte_size(rsx::vertex_base_type type, u32 size);

	// CPU port of fetch_attribute() from RSXProg/RSXVertexFetch.glsl, position only.
	// 'src' points at the attribute inside one vertex. RSX sources are always big-endian,
	// so the shader's swap flag is unconditionally on here.
	// Returns false for a format we do not decode.
	bool decode_position(const u8* src, rsx::vertex_base_type type, u32 size, f32 (&out)[4]);

	// Sibling of decode_position that returns the components as the guest stored them: no
	// s_scale divide and no SNORM16 half-unit bias. A 'ub' bone index therefore reads as
	// 0..255 rather than idx/255, and 'ub' colours read as bytes. Absent components come back
	// as 0, except w which defaults to 1.
	// Returns false for a format we do not decode.
	bool decode_attribute_raw(const u8* src, rsx::vertex_base_type type, u32 size, f32 (&out)[4]);

	// [first, first + count) read as a triangle strip, appended to 'out' as a triangle list.
	void strip_to_list(u32 first, u32 count, std::vector<u32>& out);

	// An existing triangle-strip index list, appended to 'out' as a triangle list.
	void strip_to_list(const u32* indices, u32 count, std::vector<u32>& out);
}

#endif
