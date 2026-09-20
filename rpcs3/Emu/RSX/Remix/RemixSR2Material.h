#pragma once

#ifdef _WIN32

#include "util/types.hpp"
#include "remix_c.h"

#include <string_view>

namespace remix_rsx
{
	inline bool sr2_opaque_world_fp(u64 hash)
	{
		// Observed raw hashes with only standard, flat-reflection, or two-tone diffuse aliases
		// in the shipped CGB pack. Shared decal/window/grass permutations are excluded.
		switch (hash)
		{
		case 0x0D483FC2DD6769F4ull:
		case 0x392ADDB4D214CC0Aull:
		case 0x4DC97FBBA1F43362ull:
		case 0x90CBA346BDBC7F3Dull:
		case 0xE7C5397F668A828Aull:
		case 0x15133FF917D853B8ull:
		case 0x491E9D01B27BEB3Dull:
		case 0x6F53997F0EE4EE9Cull:
		case 0xE2999A5123FD7053ull:
		case 0x10144622113F00D2ull:
		case 0xC7467D992DEE04A0ull:
		case 0xF35A4F1A52105A61ull:
		case 0x25F7DB3826A5D0D6ull:
		case 0x459D3DF17DA93EA8ull:
		case 0x6FAE722E2A64C771ull:
		case 0x73DE527F66BA3F09ull:
		case 0xD1E4FC0413063AD3ull:
			return true;
		default:
			return false;
		}
	}

	struct sr2_world_alpha_inputs
	{
		std::string_view title_id;
		u64 fp_hash = 0;
		bool rigid_world = false;
		bool depth_test = false;
		bool depth_write = false;
		bool fp32_outputs = false;
		bool kil = false;
		bool alpha_to_coverage = false;
		bool normalized_target = false;
		f32 guest_alpha_ref = 0.f;
		u16 sampled_mask = 0;
		u16 alpha_kill_mask = 0;
	};

	inline bool apply_sr2_opaque_world_alpha(const sr2_world_alpha_inputs& input,
		remixapi_InstanceInfoBlendEXT& state)
	{
		if (input.title_id != "BLUS30201" || !sr2_opaque_world_fp(input.fp_hash)
			|| !input.rigid_world || !input.depth_test || !input.depth_write
			|| input.fp32_outputs || input.kil || input.alpha_to_coverage
			|| (input.sampled_mask & input.alpha_kill_mask) != 0
			|| !state.alphaBlendEnabled || state.srcColorBlendFactor != 1
			|| state.dstColorBlendFactor != 7 || state.colorBlendOp != 0)
		{
			return false;
		}

		// GREATER(0) can still cut exact-zero holes. GEQUAL(0) is trivial only on
		// nonnegative integer-target output, with a genuinely zero guest reference.
		// A positive floating reference can round to zero in the Remix extension.
		if (state.alphaTestCompareOp != 7
			&& !(state.alphaTestCompareOp == 6 && state.alphaTestReferenceValue == 0
				&& input.normalized_target && input.guest_alpha_ref == 0.f))
		{
			return false;
		}

		// Interpret SR2's rigid depth-writing pass as opaque, as the PC proxy does. This
		// is not shader parity: a shader alpha fade is deliberately not world opacity here.
		state.alphaBlendEnabled = 0;
		state.srcColorBlendFactor = 1;
		state.dstColorBlendFactor = 0;
		state.colorBlendOp = 0;
		state.srcAlphaBlendFactor = 1;
		state.dstAlphaBlendFactor = 0;
		state.alphaBlendOp = 0;
		state.alphaTestEnabled = 0;
		state.alphaTestCompareOp = 7;
		state.alphaTestReferenceValue = 0;
		state.textureAlphaArg1Source = 3; // RtTextureArgSource::TFactor
		state.textureAlphaArg2Source = 0;
		state.textureAlphaOperation = 1; // DxvkRtTextureOperation::SelectArg1
		state.tFactor |= 0xFF000000u;
		return true;
	}
}

#endif
