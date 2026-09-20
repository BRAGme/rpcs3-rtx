#include <gtest/gtest.h>

#include "Emu/RSX/Remix/RemixTransforms.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32

namespace remix_rsx
{
	// Eat Lead (BLUS30267) fragment programs, byte-for-byte as RPCS3_REMIX_UCODESTOREFP wrote them
	// to bin\remix_ucode\<fpraw>.fp: little-endian u32 words, byteswapped by the scanner itself
	// (fp_decode_word). Listings quoted from docs\remix\fpdis.py. All four export 32-bit (fp32=1 on
	// their 'Remix ucode-store-fp:' lines), so COL0 is R0 and R4 is ocol3, the fourth MRT.
	namespace
	{
		// D0D82B380F6466F3, a decal. '6: MOV R0.xyz <- c[].xyyx   c=[0.262745 0.231373 0 0]': the
		// literal is read THROUGH the swizzle, so the colour is (0.262745, 0.231373, 0.231373).
		// tex0 feeds only alpha ('5: MUL R1.w <- R0.xxxx, tc0.w'), and R4 carries
		// [0 0.0627451 1 0], which is a material-parameter target and must not become the colour.
		constexpr u32 s_decal_brown[] = {
			0x001700a2, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x0001080e, 0x9d1c02c8, 0x010000c8, 0x010000c8,
			0x00000000, 0x803d8180, 0x803f0000, 0x00000000, 0x00020002, 0x9d1c00c8, 0x00000200, 0x010000c8,
			0x193f9a99, 0x00000000, 0x00000000, 0x00000000, 0x00020290, 0x9c1c0000, 0x010001c8, 0xe13f00c8,
			0x0001000e, 0x9c1c0228, 0x010000c8, 0x010000c8, 0x863e8386, 0x6c3e0bed, 0x00000000, 0x00000000,
			0x00090402, 0x9d1c04fe, 0x010004fe, 0x010000c8, 0x00010010, 0x9d1c04c8, 0x010000c8, 0x010000c8,
			0x00090410, 0x9d1c04c8, 0x00000800, 0x010000c8, 0x0001040e, 0x9c1c0200, 0x010000c8, 0x010000c8,
			0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x004a7e11, 0x9d1c08c8, 0x00000200, 0x010000c8,
			0x803b8180, 0x00000000, 0x00000000, 0x00000000, 0x0101020e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x00527e06, 0xf51f00c8, 0x010000c8, 0x010000c8, 0x00010410, 0x9d1c04c8, 0x010000c8, 0x010000c8,
			0x00010610, 0x9d1c04c8, 0x010000c8, 0x010000c8, 0x00010810, 0x9d1c04c8, 0x010000c8, 0x010000c8,
			0x0004070e, 0x9d1c04c8, 0x00000200, 0x00000200, 0x003f0000, 0x00000000, 0x00000000, 0x00000000,
		};

		// C91606BCC3890030, the same family through a broadcast: '4: MOV R0.xyz <- c[].xxxx
		// c=[0.172549 0 0 0]' - a grey. Also carries the conditional KIL the kil census reports.
		constexpr u32 s_decal_grey[] = {
			0x001700a2, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x0001080e, 0x9d1c02c8, 0x010000c8, 0x010000c8,
			0x00000000, 0x803d8180, 0x803f0000, 0x00000000, 0x00020290, 0x9c1c0000, 0x010001c8, 0xe13f00c8,
			0x0001000e, 0x9c1c0200, 0x010000c8, 0x010000c8, 0x303eafb0, 0x00000000, 0x00000000, 0x00000000,
			0x00090402, 0x9d1c04fe, 0x010004fe, 0x010000c8, 0x00010010, 0x9d1c04c8, 0x010000c8, 0x010000c8,
			0x00090410, 0x9d1c04c8, 0x00000800, 0x010000c8, 0x0001040e, 0x9c1c0200, 0x010000c8, 0x010000c8,
			0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x004a7e11, 0x9d1c08c8, 0x00000200, 0x010000c8,
			0x803b8180, 0x00000000, 0x00000000, 0x00000000, 0x0101020e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x00527e06, 0xf51f00c8, 0x010000c8, 0x010000c8, 0x00010410, 0x9d1c04c8, 0x010000c8, 0x010000c8,
			0x00010610, 0x9d1c04c8, 0x010000c8, 0x010000c8, 0x00010810, 0x9d1c04c8, 0x010000c8, 0x010000c8,
			0x0004070e, 0x9d1c04c8, 0x00000200, 0x00000200, 0x003f0000, 0x00000000, 0x00000000, 0x00000000,
		};

		// FF5CB727D10E58A5: '11: MOV R0.xyzw <- c[].xxxx   c=[1 0 0 0]   END' - white on COL0,
		// with the sampled tex0 written to R2 (MRT1) instead.
		constexpr u32 s_white[] = {
			0x02170096, 0x9d1c015c, 0x010000c8, 0xe13f00c8, 0x0001080e, 0x9d1c02c8, 0x010000c8, 0x010000c8,
			0x00000000, 0x803d8180, 0x803f0000, 0x00000000, 0x00010810, 0x9c1c00aa, 0x010000c8, 0x010000c8,
			0x0017048e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x00010410, 0x9c1c0000, 0x010000c8, 0x010000c8,
			0x00010610, 0x9d1c00c8, 0x010000c8, 0x010000c8, 0x000102ee, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x0004060e, 0x9d1c04c8, 0x00000200, 0x00000200, 0x003f0000, 0x00000000, 0x00000000, 0x00000000,
			0x007e7e1e, 0x9d1c00c8, 0x010000c8, 0x010000c8, 0x0001011e, 0x9c1c0200, 0x010000c8, 0x010000c8,
			0x803f0000, 0x00000000, 0x00000000, 0x00000000,
		};

		// 6FD147A3B666FC1E, textured: '17: TEX R0.xyz <- tc0 [tex0]' is the terminal COL0.rgb
		// writer, while '5: MOV R4.xyz <- c[].xyzw   c=[0 0.0627451 1 0]' writes MRT3. The control
		// for the whole rule: a literal on another output register is not the colour.
		constexpr u32 s_textured_mrt3_blue[] = {
			0x04170096, 0x9d1c015c, 0x010000c8, 0xe13f00c8, 0x00010610, 0x9d1c00c8, 0x010000c8, 0x010000c8,
			0x00010410, 0x9c1c0000, 0x010000c8, 0x010000c8, 0x0101046e, 0x9d1c01c8, 0x010000c8, 0xe1bf00c8,
			0x00010810, 0x9c1c00aa, 0x010000c8, 0x010000c8, 0x0001080e, 0x9d1c02c8, 0x010000c8, 0x010000c8,
			0x00000000, 0x803d8180, 0x803f0000, 0x00000000, 0x01050042, 0x9d1c01c8, 0x010001c8, 0xe1bf00c8,
			0x013b024e, 0x9d1c01c8, 0x010000c8, 0xe1bf00c8, 0x00050210, 0x9d1c04c8, 0x011008c8, 0x010000c8,
			0x000106ee, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x0004020e, 0x9f1c04c8, 0x010004fe, 0x010008c8,
			0x02170010, 0x9f1c04c8, 0x010000c8, 0x010000c8, 0x00020010, 0x9d1c00c8, 0x00000200, 0x010000c8,
			0x7f410000, 0x00000000, 0x00000000, 0x00000000, 0x0004060e, 0x9d1c0cc8, 0x000002aa, 0x000002aa,
			0x00000000, 0x003f0000, 0x00000000, 0x00000000, 0x0017008e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x00090010, 0x9d1c00c8, 0x00000200, 0x010000c8, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
			0x0002040e, 0x9d1c00c8, 0x000002aa, 0x010000c8, 0x00000000, 0x003f0000, 0x00000000, 0x00000000,
			0x00080010, 0x9d1c00c8, 0x00000200, 0x010000c8, 0x80400000, 0x00000000, 0x00000000, 0x00000000,
			0x022f020e, 0x9f1c04c8, 0x010000fe, 0x010000c8, 0x0004040e, 0x9d1c04c8, 0x00000200, 0x010008c8,
			0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x007d7e1e, 0x9d1c00c8, 0x010000c8, 0x010000c8,
			0x00010110, 0x9c1c0200, 0x010000c8, 0x010000c8, 0x803f0000, 0x00000000, 0x00000000, 0x00000000,
		};

		// --- ROUND 63: the hand-rolled lerp ------------------------------------------------------

		// CE3BE6B2685F446C (fp 500c9f0b17153879), the "code blood" texture 0755B93E0780C976. No MOV
		// states its colour: '9: ADD R1.xyz <- -R0.xyzw, c[].xyzw   c=[0.694118 0.807843 1 1]' then
		// '12: MAD R0.xyz <- R1.wwww, R1.xyzw, R0.xyzw   END' is lerp(R0, C, t) toward the pale blue
		// (177, 206, 255), with t = tc1.x * 1.0 ('7: MUL R1.w <- R1.xxxx, c[].xyzw', lane w = 1).
		constexpr u32 s_code_blood_blue[] = {
			0x001700ae, 0x9d1c01d2, 0x010000c8, 0xe13f00c8, 0x0002020e, 0x9d1c00c8, 0x010002c8, 0x010000c8,
			0x803f0000, 0x663f6666, 0x333f3333, 0x00000000, 0x0001009e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x0002000e, 0x9d1c04c8, 0x010000c8, 0x010000c8, 0x00010010, 0x9d1c00c8, 0x010000c8, 0x010000c8,
			0x000102a2, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x00020210, 0x9c1c0400, 0x010002c8, 0x010000c8,
			0x313fb2b1, 0x4e3fd0ce, 0x803f0000, 0x803f0000, 0x0003020e, 0x9f1c00c8, 0x010002c8, 0x010000c8,
			0x313fb2b1, 0x4e3fd0ce, 0x803f0000, 0x803f0000, 0x007e7e1e, 0x9d1c00c8, 0x010000c8, 0x010000c8,
			0x0004010e, 0x9d1c04fe, 0x010004c8, 0x010000c8,
		};

		// 9F30538CBDE6C249 (fp 01072a35c2acbe5c), the OTHER code-blood texture 921904970ADA8A23, and
		// the shape a matcher written for the one above is blind to: '7: ADD R1.yzw <- -R0.xxyz,
		// c[].xxyz   c=[1 1 0.858824 0.203922]' then '11: MAD R0.xyz <- R1.xxxx, R1.yzww, R0.xyzw' -
		// the endpoint sits in lanes y,z,w and is read back through .yzww, so it is (1, 1, 0.858824),
		// the level-wide fog colour. Its weight is '9: MUL R1.x <- R1.xyzw, c[].wwww' = tc1.x *
		// 0.203922, the same 36-program fog family as the main world programs, and the weight gate
		// must refuse it: a flat pale-yellow field is not what a 20 % fade looks like.
		constexpr u32 s_code_blood_fog[] = {
			0x001700be, 0x9d1c01d2, 0x010000c8, 0xe13f00c8, 0x0002000e, 0x9d1c00c8, 0x010002c8, 0x010000c8,
			0x003f0000, 0x4c3fcdcc, 0xc03f0000, 0x00000000, 0x0001029e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x0002000e, 0x9d1c00c8, 0x010004c8, 0x010000c8, 0x00020010, 0x9d1c00c8, 0x010004c8, 0x010000c8,
			0x000102a2, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x0003021c, 0x9f1c0020, 0x01000220, 0x010000c8,
			0x803f0000, 0x803f0000, 0x5b3fdddb, 0x503ed2d0, 0x00020202, 0x9d1c04c8, 0x010002fe, 0x010000c8,
			0x803f0000, 0x803f0000, 0x5b3fdddb, 0x503ed2d0, 0x0004010e, 0x9c1c0400, 0x010004f2, 0x010000c8,
		};

		// The program above with ONE word changed: the MUL's literal lane w (word 43, the weight)
		// raised from 0.203922 to 1.0 (0x803f0000 = 1.0f in the stored half-swapped byte order). No
		// real program on disk has the swizzle-routed shape at a passing weight, and this is the
		// only way the lane composition (m -> a == b, C[swz_c[m]]) gets executed by the C++ at all.
		// docs/remix/fplerp.py reads the same bytes as (1, 1, 0.858824) at k = 1.
		constexpr u32 s_code_blood_fog_weight_one[] = {
			0x001700be, 0x9d1c01d2, 0x010000c8, 0xe13f00c8, 0x0002000e, 0x9d1c00c8, 0x010002c8, 0x010000c8,
			0x003f0000, 0x4c3fcdcc, 0xc03f0000, 0x00000000, 0x0001029e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x0002000e, 0x9d1c00c8, 0x010004c8, 0x010000c8, 0x00020010, 0x9d1c00c8, 0x010004c8, 0x010000c8,
			0x000102a2, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x0003021c, 0x9f1c0020, 0x01000220, 0x010000c8,
			0x803f0000, 0x803f0000, 0x5b3fdddb, 0x503ed2d0, 0x00020202, 0x9d1c04c8, 0x010002fe, 0x010000c8,
			0x803f0000, 0x803f0000, 0x5b3fdddb, 0x803f0000, 0x0004010e, 0x9c1c0400, 0x010004f2, 0x010000c8,
		};

		// --- ROUND 67: the per-channel tint on a sampled texel -----------------------------------

		// 1B98CE5D069789E2, a deferred G-buffer program: '0: TEX R0 <- tc1 [tex0]' then '1: MUL R0.xyz
		// <- R0, c[].xyzw   c=[0.47451 0.509804 0.607843 0]' is the code blood's tint shape, terminal
		// on COL0.rgb and on the only colour unit (tex1 feeds a DP3 luminance into R4.x) - and it
		// writes R2, R3 and R4. It draws the concrete strip FDF6489A939E6563 x219 in one run; its
		// tint is the wall's material colour, and the MRT term is the ONLY reason it is refused.
		constexpr u32 s_world_concrete_tint[] = {
			0x001700be, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x0002000e, 0x9d1c00c8, 0x010002c8, 0x010000c8,
			0xf23efaf2, 0x023f8482, 0x1b3f999b, 0x00000000, 0x0001080c, 0x9c1c02a0, 0x010000c8, 0x010000c8,
			0x003d8180, 0x803f0000, 0x00000000, 0x00000000, 0x00020482, 0x9d1c00fe, 0x010001fe, 0xe13f00c8,
			0x021702ae, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x00050802, 0x9d1c04c8, 0x00000200, 0x010000c8,
			0xaa3efa7e, 0x00000000, 0x00000000, 0x00000000, 0x00090210, 0x9c1c0800, 0x00000800, 0x010000c8,
			0x00090010, 0x9c1c0800, 0x010004c8, 0x010000c8, 0x0101060e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x004a7e11, 0x9d1c00c8, 0x00000200, 0x010000c8, 0x803b8180, 0x00000000, 0x00000000, 0x00000000,
			0x0004060e, 0x9d1c0cc8, 0x000002aa, 0x000002aa, 0x00000000, 0x003f0000, 0x00000000, 0x00000000,
			0x00010010, 0x9c1c0800, 0x010000c8, 0x010000c8, 0x00527e06, 0xf51f00c8, 0x010000c8, 0x010000c8,
			0x00010610, 0x9c1c0800, 0x010000c8, 0x010000c8, 0x00010410, 0x9c1c0800, 0x010000c8, 0x010000c8,
			0x00010810, 0x9c1c0800, 0x010000c8, 0x010000c8, 0x0001050e, 0x9c1c0200, 0x010000c8, 0x010000c8,
			0x00000000, 0x00000000, 0x00000000, 0x00000000,
		};

		// A50DE5B1F5682CE2, the rejected discriminator's counter-example: ONE sampled unit, the same
		// tint terminal on COL0.rgb, and R2/R3/R4 written - a G-buffer program drawing the concrete
		// wall E6AE2AA616434545 x355. "Only one sampled unit" would have tinted this wall blue-grey
		// and left its twin above raw; the MRT term refuses both.
		constexpr u32 s_world_concrete_tint_single_unit[] = {
			0x001700be, 0x9d1c01c8, 0x010000c8, 0xe13f00c8, 0x0002000e, 0x9d1c00c8, 0x010002c8, 0x010000c8,
			0xf23efaf2, 0x023f8482, 0x1b3f999b, 0x00000000, 0x00020010, 0x9d1c00c8, 0x010002c8, 0x010000c8,
			0x00000000, 0x00000000, 0x00000000, 0x193f9a99, 0x0001080e, 0x9d1c02c8, 0x010000c8, 0x010000c8,
			0x00000000, 0x803d8180, 0x803f0000, 0x00000000, 0x00020282, 0x9d1c00fe, 0x010001fe, 0xe13f00c8,
			0x00090010, 0x9c1c0400, 0x00000400, 0x010000c8, 0x0101060e, 0x9d1c01c8, 0x010000c8, 0xe13f00c8,
			0x0004060e, 0x9d1c0cc8, 0x00000200, 0x00000200, 0x003f0000, 0x00000000, 0x00000000, 0x00000000,
			0x00090610, 0x9c1c0400, 0x010000c8, 0x010000c8, 0x004a7e03, 0x9d1c0cfe, 0x00000200, 0x010000c8,
			0x803b8180, 0x00000000, 0x00000000, 0x00000000, 0x00010010, 0x9c1c0400, 0x010000c8, 0x010000c8,
			0x00527e06, 0x150000c8, 0x010000c8, 0x010000c8, 0x00010410, 0x9c1c0400, 0x010000c8, 0x010000c8,
			0x00010610, 0x9c1c0400, 0x010000c8, 0x010000c8, 0x00010910, 0x9c1c0400, 0x010000c8, 0x010000c8,
		};

		template <usz N>
		fp_fingerprint scan(const u32 (&words)[N])
		{
			return scan_fragment_program(words, static_cast<u32>(N * sizeof(u32)), true);
		}
	}

	TEST(RemixFpLerp, EndpointIsReadFromTheAdd)
	{
		const fp_fingerprint fp = scan(s_code_blood_blue);

		EXPECT_FALSE(fp.out_rgb_const);
		ASSERT_TRUE(fp.out_rgb_lerp);
		EXPECT_NEAR(fp.out_rgb_lerp_rgb[0], 0.694118f, 1e-5f);
		EXPECT_NEAR(fp.out_rgb_lerp_rgb[1], 0.807843f, 1e-5f);
		EXPECT_FLOAT_EQ(fp.out_rgb_lerp_rgb[2], 1.f);
		EXPECT_FLOAT_EQ(fp.out_rgb_lerp_weight, 1.f);
	}

	TEST(RemixFpLerp, FogWeightIsRefused)
	{
		const fp_fingerprint fp = scan(s_code_blood_fog);

		EXPECT_FALSE(fp.out_rgb_const);
		EXPECT_FALSE(fp.out_rgb_lerp);
	}

	TEST(RemixFpLerp, SwizzleRoutedLanesAreComposed)
	{
		const fp_fingerprint fp = scan(s_code_blood_fog_weight_one);

		EXPECT_FALSE(fp.out_rgb_const);
		ASSERT_TRUE(fp.out_rgb_lerp);
		EXPECT_FLOAT_EQ(fp.out_rgb_lerp_rgb[0], 1.f);
		EXPECT_FLOAT_EQ(fp.out_rgb_lerp_rgb[1], 1.f);
		EXPECT_NEAR(fp.out_rgb_lerp_rgb[2], 0.858824f, 1e-5f);
		EXPECT_FLOAT_EQ(fp.out_rgb_lerp_weight, 1.f);
	}

	TEST(RemixFpLerp, ALiteralIsNotALerp)
	{
		EXPECT_FALSE(scan(s_decal_brown).out_rgb_lerp);
		EXPECT_FALSE(scan(s_white).out_rgb_lerp);
		EXPECT_FALSE(scan(s_textured_mrt3_blue).out_rgb_lerp);
	}

	// The C++ detector over the whole ucode corpus against docs/remix/fplerp.py's independent
	// reading of the same files: exactly two programs pass, both the pale blue. Point
	// RPCS3_REMIX_UCODE_DIR at bin\remix_ucode to run it; skipped otherwise.
	TEST(RemixFpLerp, CorpusAgreesWithTheOfflineScan)
	{
#pragma warning(push)
#pragma warning(disable: 4996)
		const char* dir = std::getenv("RPCS3_REMIX_UCODE_DIR");
#pragma warning(pop)

		// This gtest predates GTEST_SKIP; a skip prints as a pass, so it announces itself.
		if (!dir || !std::filesystem::is_directory(dir))
		{
			std::printf("[  SKIPPED ] RPCS3_REMIX_UCODE_DIR is not a directory - corpus check not run\n");
			return;
		}

		std::set<std::string> lerp_programs;
		u32 scanned = 0;

		for (const auto& entry : std::filesystem::directory_iterator(dir))
		{
			if (entry.path().extension() != ".fp")
			{
				continue;
			}

			std::ifstream file(entry.path(), std::ios::binary);
			std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

			const fp_fingerprint fp = scan_fragment_program(bytes.data(), static_cast<u32>(bytes.size()), true);
			++scanned;

			if (fp.out_rgb_lerp)
			{
				lerp_programs.insert(entry.path().stem().string());
			}
		}

		EXPECT_EQ(scanned, 793u);
		EXPECT_EQ(lerp_programs, (std::set<std::string>{ "436265406A3B303B", "CE3BE6B2685F446C" }));
	}

	TEST(RemixFpConst, SwizzledLiteralIsReadThroughTheSwizzle)
	{
		const fp_fingerprint fp = scan(s_decal_brown);

		EXPECT_EQ(fp.instructions, 16u);
		ASSERT_TRUE(fp.out_rgb_const);
		EXPECT_NEAR(fp.out_rgb_const_rgb[0], 0.262745f, 1e-5f);
		EXPECT_NEAR(fp.out_rgb_const_rgb[1], 0.231373f, 1e-5f);
		EXPECT_NEAR(fp.out_rgb_const_rgb[2], 0.231373f, 1e-5f);
	}

	TEST(RemixFpConst, BroadcastLiteralIsAGrey)
	{
		const fp_fingerprint fp = scan(s_decal_grey);

		EXPECT_EQ(fp.instructions, 15u);
		EXPECT_TRUE(fp.has_kil);
		ASSERT_TRUE(fp.out_rgb_const);
		EXPECT_NEAR(fp.out_rgb_const_rgb[0], 0.172549f, 1e-5f);
		EXPECT_NEAR(fp.out_rgb_const_rgb[1], 0.172549f, 1e-5f);
		EXPECT_NEAR(fp.out_rgb_const_rgb[2], 0.172549f, 1e-5f);
	}

	TEST(RemixFpConst, WhiteFromLaneX)
	{
		const fp_fingerprint fp = scan(s_white);

		EXPECT_EQ(fp.instructions, 10u);
		ASSERT_TRUE(fp.out_rgb_const);
		EXPECT_FLOAT_EQ(fp.out_rgb_const_rgb[0], 1.f);
		EXPECT_FLOAT_EQ(fp.out_rgb_const_rgb[1], 1.f);
		EXPECT_FLOAT_EQ(fp.out_rgb_const_rgb[2], 1.f);
	}

	TEST(RemixFpConst, LiteralOnAnotherOutputIsNotTheColour)
	{
		const fp_fingerprint fp = scan(s_textured_mrt3_blue);

		EXPECT_EQ(fp.instructions, 22u);
		EXPECT_EQ(fp.sampled_mask, 0x7u);
		EXPECT_FALSE(fp.out_rgb_const);
	}

	// --- ROUND 67 ------------------------------------------------------------------------------

	TEST(RemixFpTint, CodeBloodTintIsReadRawThroughTheSwizzle)
	{
		const fp_fingerprint fp = scan(s_code_blood_fog);

		EXPECT_FALSE(fp.out_rgb_const);
		EXPECT_FALSE(fp.out_rgb_lerp);
		ASSERT_TRUE(fp.out_rgb_tint);
		EXPECT_EQ(fp.out_rgb_tint_unit, 0u);
		EXPECT_FLOAT_EQ(fp.out_rgb_tint_rgb[0], 0.5f);
		EXPECT_FLOAT_EQ(fp.out_rgb_tint_rgb[1], 0.8f);
		// Stored RAW, above 1: the apply site clamps, the fingerprint does not.
		EXPECT_FLOAT_EQ(fp.out_rgb_tint_rgb[2], 1.5f);
	}

	TEST(RemixFpTint, TheExactReadingsComeFirst)
	{
		// CE3BE6B2685F446C carries the same shape ('1: MUL R0.xyz <- R0, c[1 0.9 0.7]') under its
		// round-63 lerp; the lerp is the exact reading and keeps the program.
		const fp_fingerprint blue = scan(s_code_blood_blue);
		ASSERT_TRUE(blue.out_rgb_lerp);
		EXPECT_FALSE(blue.out_rgb_tint);

		const fp_fingerprint fog_one = scan(s_code_blood_fog_weight_one);
		ASSERT_TRUE(fog_one.out_rgb_lerp);
		EXPECT_FALSE(fog_one.out_rgb_tint);
	}

	TEST(RemixFpTint, AGBufferMaterialColourIsNotATint)
	{
		const fp_fingerprint strip = scan(s_world_concrete_tint);
		EXPECT_EQ(strip.sampled_mask, 0x3u);
		EXPECT_FALSE(strip.out_rgb_const);
		EXPECT_FALSE(strip.out_rgb_lerp);
		EXPECT_FALSE(strip.out_rgb_tint);

		const fp_fingerprint wall = scan(s_world_concrete_tint_single_unit);
		EXPECT_EQ(wall.sampled_mask, 0x1u);
		EXPECT_FALSE(wall.out_rgb_tint);
	}

	TEST(RemixFpTint, ALiteralIsNotATint)
	{
		EXPECT_FALSE(scan(s_decal_brown).out_rgb_tint);
		EXPECT_FALSE(scan(s_decal_grey).out_rgb_tint);
		EXPECT_FALSE(scan(s_white).out_rgb_tint);
		EXPECT_FALSE(scan(s_textured_mrt3_blue).out_rgb_tint);
	}

	// Against docs/remix/fptint.py's independent reading of the same 793 files: exactly the two
	// constant-variants of the code-blood program. Same RPCS3_REMIX_UCODE_DIR switch as above.
	TEST(RemixFpTint, CorpusAgreesWithTheOfflineScan)
	{
#pragma warning(push)
#pragma warning(disable: 4996)
		const char* dir = std::getenv("RPCS3_REMIX_UCODE_DIR");
#pragma warning(pop)

		if (!dir || !std::filesystem::is_directory(dir))
		{
			std::printf("[  SKIPPED ] RPCS3_REMIX_UCODE_DIR is not a directory - corpus check not run\n");
			return;
		}

		std::set<std::string> tint_programs;
		u32 scanned = 0;

		for (const auto& entry : std::filesystem::directory_iterator(dir))
		{
			if (entry.path().extension() != ".fp")
			{
				continue;
			}

			std::ifstream file(entry.path(), std::ios::binary);
			std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

			const fp_fingerprint fp = scan_fragment_program(bytes.data(), static_cast<u32>(bytes.size()), true);
			++scanned;

			if (fp.out_rgb_tint)
			{
				tint_programs.insert(entry.path().stem().string());
			}
		}

		EXPECT_EQ(scanned, 793u);
		EXPECT_EQ(tint_programs, (std::set<std::string>{ "9F30538CBDC6C235", "9F30538CBDE6C249" }));
	}
}

#endif
