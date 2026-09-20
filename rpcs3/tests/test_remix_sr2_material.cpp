#include <gtest/gtest.h>
#include "Emu/RSX/Remix/RemixSR2Material.h"

#include <cstring>

#ifdef _WIN32

namespace remix_rsx
{
	namespace
	{
		sr2_world_alpha_inputs world()
		{
			sr2_world_alpha_inputs input{};
			input.title_id = "BLUS30201";
			input.fp_hash = 0xF35A4F1A52105A61ull;
			input.rigid_world = true;
			input.depth_test = input.depth_write = true;
			input.normalized_target = true;
			input.sampled_mask = 0x800f;
			return input;
		}

		remixapi_InstanceInfoBlendEXT premultiplied()
		{
			remixapi_InstanceInfoBlendEXT state{};
			state.alphaBlendEnabled = 1;
			state.srcColorBlendFactor = 1;
			state.dstColorBlendFactor = 7;
			state.srcAlphaBlendFactor = 0;
			state.dstAlphaBlendFactor = 1;
			state.alphaTestCompareOp = 7;
			state.textureAlphaArg1Source = 1;
			state.textureAlphaArg2Source = 2;
			state.textureAlphaOperation = 3;
			state.tFactor = 0x40203040;
			return state;
		}

		void expect_refused(const sr2_world_alpha_inputs& input, remixapi_InstanceInfoBlendEXT state = premultiplied())
		{
			const auto before = state;
			EXPECT_FALSE(apply_sr2_opaque_world_alpha(input, state));
			EXPECT_EQ(0, std::memcmp(&before, &state, sizeof(state)));
		}
	}

	TEST(RemixSR2Material, EligibleFloorBecomesOpaqueWithoutChangingColorOrChain)
	{
		auto state = premultiplied();
		state.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
		state.pNext = &state;
		state.textureColorArg1Source = 3;
		state.textureColorArg2Source = 1;
		state.textureColorOperation = 3;
		state.writeMask = 7;
		state.isVertexColorBakedLighting = 1;
		EXPECT_TRUE(apply_sr2_opaque_world_alpha(world(), state));
		EXPECT_EQ(0u, state.alphaBlendEnabled);
		EXPECT_EQ(1u, state.srcColorBlendFactor);
		EXPECT_EQ(0u, state.dstColorBlendFactor);
		EXPECT_EQ(0u, state.colorBlendOp);
		EXPECT_EQ(1u, state.srcAlphaBlendFactor);
		EXPECT_EQ(0u, state.dstAlphaBlendFactor);
		EXPECT_EQ(0u, state.alphaBlendOp);
		EXPECT_EQ(0u, state.alphaTestEnabled);
		EXPECT_EQ(7u, state.alphaTestCompareOp);
		EXPECT_EQ(0u, state.alphaTestReferenceValue);
		EXPECT_EQ(3u, state.textureAlphaArg1Source);
		EXPECT_EQ(0u, state.textureAlphaArg2Source);
		EXPECT_EQ(1u, state.textureAlphaOperation);
		EXPECT_EQ(0xFF203040u, state.tFactor);
		EXPECT_EQ(3u, state.textureColorArg1Source);
		EXPECT_EQ(1u, state.textureColorArg2Source);
		EXPECT_EQ(3u, state.textureColorOperation);
		EXPECT_EQ(7u, state.writeMask);
		EXPECT_EQ(1u, state.isVertexColorBakedLighting);
		EXPECT_EQ(&state, state.pNext);
		EXPECT_EQ(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT, state.sType);
	}

	TEST(RemixSR2Material, OnlyObservedUnambiguousWorldFamilies)
	{
		constexpr u64 accepted[] = {0x0D483FC2DD6769F4ull, 0x392ADDB4D214CC0Aull, 0x4DC97FBBA1F43362ull,
			0x90CBA346BDBC7F3Dull, 0xE7C5397F668A828Aull, 0x15133FF917D853B8ull, 0x491E9D01B27BEB3Dull,
			0x6F53997F0EE4EE9Cull, 0xE2999A5123FD7053ull, 0x10144622113F00D2ull, 0xC7467D992DEE04A0ull,
			0xF35A4F1A52105A61ull, 0x25F7DB3826A5D0D6ull, 0x459D3DF17DA93EA8ull, 0x6FAE722E2A64C771ull,
			0x73DE527F66BA3F09ull, 0xD1E4FC0413063AD3ull};
		for (const u64 hash : accepted)
		{
			auto input = world(); input.fp_hash = hash;
			auto state = premultiplied();
			EXPECT_TRUE(apply_sr2_opaque_world_alpha(input, state)) << hash;
		}
		constexpr u64 excluded[] = {0ull, 0x7A2C165EF3879A90ull, 0x828B76E1862253A3ull,
			0x3522827750B82191ull, 0xC0FF4E18F07CAA05ull, 0xC2FF4E18F07CAA25ull,
			0x9C9C02E400818182ull, 0xC93340D51B04C452ull, 0x39243D77419C0EFAull, 0x7CB878193103E4C9ull};
		for (const u64 hash : excluded)
		{
			auto input = world(); input.fp_hash = hash;
			expect_refused(input);
		}
	}

	TEST(RemixSR2Material, WrongTitleAndNonWorldRoutesRemainUnchanged)
	{
		auto input = world(); input.title_id = "BLUS30443"; expect_refused(input);
		input = world(); input.title_id = ""; expect_refused(input);
		input = world(); input.rigid_world = false; expect_refused(input);
		input = world(); input.depth_test = false; expect_refused(input);
		input = world(); input.depth_write = false; expect_refused(input);
		input = world(); input.fp32_outputs = true; expect_refused(input);
	}

	TEST(RemixSR2Material, DiscardsAndActualCoverageRemainUnchanged)
	{
		auto input = world(); input.kil = true; expect_refused(input);
		input = world(); input.alpha_to_coverage = true; expect_refused(input);
		for (const u16 mask : {u16{1}, u16{2}, u16{8}, u16{0x8000}})
		{
			input = world(); input.alpha_kill_mask = mask; expect_refused(input);
		}
		input = world(); input.alpha_kill_mask = 0x10; // unsampled texture does not discard
		auto state = premultiplied();
		EXPECT_TRUE(apply_sr2_opaque_world_alpha(input, state));
	}

	TEST(RemixSR2Material, OnlyTrivialAlphaTestsCanBeRemoved)
	{
		for (u32 op = 0; op < 7; ++op)
		{
			auto state = premultiplied(); state.alphaTestEnabled = 1; state.alphaTestCompareOp = op;
			if (op == 6) EXPECT_TRUE(apply_sr2_opaque_world_alpha(world(), state));
			else expect_refused(world(), state);
			state = premultiplied(); state.alphaTestEnabled = 1; state.alphaTestCompareOp = op;
			state.alphaTestReferenceValue = 1;
			expect_refused(world(), state);
		}
		auto state = premultiplied(); state.alphaTestEnabled = 1; state.alphaTestReferenceValue = 128;
		EXPECT_TRUE(apply_sr2_opaque_world_alpha(world(), state)); // ALWAYS ignores reference
	}

	TEST(RemixSR2Material, FloatingTargetsAndUnquantizedReferencesRetainCutouts)
	{
		auto state = premultiplied(); state.alphaTestEnabled = 1; state.alphaTestCompareOp = 6;
		auto input = world(); input.normalized_target = false; input.guest_alpha_ref = 1.f / 1024.f;
		expect_refused(input, state); // FP16 positive reference was rounded to byte zero
		input.guest_alpha_ref = 0.f;
		expect_refused(input, state); // FP16 GEQUAL(0) can still reject negative alpha
		input.normalized_target = true; input.guest_alpha_ref = 1.f / 1024.f;
		expect_refused(input, state); // quantized zero alone never proves a trivial test
		input.guest_alpha_ref = 0.f;
		EXPECT_TRUE(apply_sr2_opaque_world_alpha(input, state));
	}

	TEST(RemixSR2Material, NonmatchingBlendModesRemainUnchanged)
	{
		auto state = premultiplied(); state.alphaBlendEnabled = 0; expect_refused(world(), state);
		state = premultiplied(); state.dstColorBlendFactor = 1; expect_refused(world(), state);
		state = premultiplied(); state.srcColorBlendFactor = 6; expect_refused(world(), state);
		state = premultiplied(); state.colorBlendOp = 1; expect_refused(world(), state);
	}
}

#endif
