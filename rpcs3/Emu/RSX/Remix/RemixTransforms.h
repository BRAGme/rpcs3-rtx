#pragma once

#ifdef _WIN32

#include "util/types.hpp"

#include "remix_c.h"

#include <string>

struct RSXVertexProgram;

namespace remix_rsx
{
	// ---------------------------------------------------------------------------------------
	// 4x4 matrix, ROW-VECTOR convention throughout this file: p' = p * M, translation in row 3.
	// That is what remixapi_CameraInfo.view / .projection expect (verified against the runtime).
	// remixapi_Transform is the opposite (column-vector); the two converters below are the only
	// places that difference is allowed to exist.
	// ---------------------------------------------------------------------------------------
	struct mat4
	{
		f32 m[4][4];
	};

	// The double-precision twin of mat4, used for exactly one thing: the gauge division. A fused
	// view x projection has a condition number in the 1e5..1e8 range, and a cofactor inversion of
	// such a matrix in f32 leaves up to 3e-3 of basis error and a whole unit of translation error -
	// which is the wobble. See gauge_f64_enabled() for the measured numbers. Nothing else in the
	// backend needs f64 and nothing else should acquire it.
	struct mat4d
	{
		f64 m[4][4];
	};

	mat4 mat4_identity();
	mat4 mat4_zero();
	mat4 mat4_multiply(const mat4& a, const mat4& b);
	mat4 mat4_transpose(const mat4& a);
	bool mat4_invert(const mat4& a, mat4& out);

	mat4d mat4d_from(const mat4& a);
	mat4 mat4d_to_f32(const mat4d& a);
	mat4d mat4d_multiply(const mat4d& a, const mat4d& b);
	// Same cofactor form as mat4_invert, in double. At cond2 <= 1e8 it leaves ~1e-8, three orders
	// below f32's own representation limit, so the f32 result of the division is exact to the last
	// bit that matters. Deliberately the same algorithm rather than a better one: the point is to
	// isolate precision as the variable, and an LU-with-pivoting rewrite would confound the A/B.
	bool mat4d_invert(const mat4d& a, mat4d& out);
	bool mat4d_is_finite(const mat4d& a);
	// max |(A x B) - I| over all sixteen entries, split into the 3x3 basis block and the rest. This
	// is the number the gauge self-check prints in both precisions.
	f64 mat4d_identity_residual(const mat4d& a, const mat4d& b);
	f32 mat4_l1_error(const mat4& a, const mat4& b);
	bool mat4_is_finite(const mat4& a);
	bool mat4_is_identity(const mat4& a, f32 tol = 1e-5f);

	// The four transform-constant slots as they sit in memory: slots[i] = c[N+i].
	struct slot_block
	{
		f32 v[4][4];
	};

	// ---------------------------------------------------------------------------------------
	// Vertex-program fingerprint
	// ---------------------------------------------------------------------------------------

	enum class chain_shape
	{
		none,
		// 4 x DP4/DPH, one per HPOS component, c[N+i] feeding component i.
		// clip_i = dot(c[N+i], pos) => c[N+i] is row i of a column-vector matrix.
		dp4,
		// MUL + 3 x MAD accumulating through a temp, c[N+k] scaled by pos_k.
		// clip = sum_k c[N+k]*pos_k => c[N+k] is column k, i.e. the slots hold the
		// row-vector matrix directly.
		mad,
	};

	// Up to this many chained 4-slot groups are followed back from HPOS.
	inline constexpr u32 max_transform_groups = 4;

	// vp_fingerprint::texcoord_input entry for "the ucode does not say".
	inline constexpr u8 s_no_texcoord_input = 0xff;

	// vp_fingerprint::texcoord_scale_slot entry for "the ucode names no scale constant".
	//
	// ROUND 52 (step 2a): u16, not u8. D1::const_src is 10 bits and Eat Lead (BLUS30267) keeps its
	// UV dequant in c463/c464, so the old u8 field could not represent the answer the matcher had
	// already found. The store site did not refuse it either - it wrote
	// static_cast<u8>(std::min<u32>(slot, 0xfeu)), i.e. it CLAMPED c464 to c254, a slot the title
	// never writes, and the apply site then read [0 0 0 0] and silently fell back to UVINTSCALE.
	// That is the census row, verbatim from bin\remix_dump.log of the frame-8137 run:
	//   Remix uvscale-fixed: vp=8d67dbfcc9db7a96 unit=0 attr=4 refusal=resolved slot=c254
	//                        inputs=0x0010 value=[0 0 0 0] read=1 ... fixed=4096
	// against that program's own ucode (bin\remix_ucode\8D67DBFCC9DB7A96.vp, instruction 5):
	//   5: VEC MUL r0.xy <- v4.xyxx, c464.xxxx
	// The matcher found c464; the u8 store turned it into c254. The uv_affine_form family below
	// was widened to u16 for exactly this reason and the scalar path was never brought along.
	inline constexpr u16 s_no_texcoord_scale = 0xffff;

	// Why resolve_texcoord_scale_slot refused a TEXn slice. These values are deliberately
	// exhaustive so every S32K draw that falls back to UVINTSCALE names the missing ucode form.
	enum class texcoord_scale_refusal : u8
	{
		no_multiply,
		two_constants,
		third_operand,
		two_attributes,
		no_attribute,
		// ROUND 52 (step 2a). The matcher resolved a slot the constant file cannot hold
		// (>= s_legal_constant_slots). Its predecessor was the silent clamp above, which produced
		// a slot that reads zero rather than a refusal that names itself; "refuse and count, never
		// guess" makes this its own exit so the population is visible instead of hiding inside
		// no_multiply. Counted as uv_scale_slot_refused at the apply site.
		slot_out_of_range,
		resolved = 0xff,
	};

	inline const char* texcoord_scale_refusal_name(texcoord_scale_refusal reason)
	{
		switch (reason)
		{
		case texcoord_scale_refusal::no_multiply:   return "no-mul";
		case texcoord_scale_refusal::two_constants: return "two-const";
		case texcoord_scale_refusal::third_operand:  return "third-operand";
		case texcoord_scale_refusal::two_attributes: return "two-attr";
		case texcoord_scale_refusal::no_attribute:   return "no-attr";
		case texcoord_scale_refusal::slot_out_of_range: return "slot-range";
		case texcoord_scale_refusal::resolved:       return "resolved";
		}

		return "unknown";
	}

	// The affine texcoord form, resolved per program and per texture unit.
	//
	// resolve_texcoord_scale_slot above recovers a single scalar divisor and nothing else, which
	// is why the visor program had to be replayed by hash (RPCS3_REMIX_UVAFFINEVP): its TEXn is
	// not attr*c, it is
	//     o[7+n].xy = c[scale][k] * (attr.x * c[row0].xy + attr.y * c[row1].xy) + c[bias].xy
	//                 + c[bias2][k2]
	// i.e. a 2x2 matrix and two biases around the scale. The uvscale evidence dump shows the same
	// shape across many of Haze's world programs with *different* slots and, decisively, different
	// components of the same slot per unit - c151.x for the attr8/i8 unit, c151.y for attr9/i9,
	// c151.z for attr10/i10 - so a per-hash replay with hardcoded slots can never cover them.
	// Every field here is a slot/component index; the values themselves are read from live
	// constants at draw time, exactly as the position decode is.
	//
	// rows/bias/bias2 are each optional: a program that only scales resolves with has_rows=false
	// and both biases unset, and the replay then degenerates to attr * c[scale][k].
	// Slot fields are u16, not u8: D1::const_src is 10 bits and this family reaches c467 on Haze,
	// which the existing texcoord_scale_slot (u8, clamped at 0xfe) cannot even represent.
	inline constexpr u16 s_no_uv_slot = 0xffff;

	struct uv_affine_form
	{
		bool resolved = false;
		bool has_rows = false;

		// ROUND 52 (step 2b). The form was reached through the MOV hop, i.e. the MUL multiplied a
		// TEMP rather than the attribute itself and the walk followed that temp's two lanes back to
		// an attribute. Recorded so the apply site can count the population separately
		// (uv_affine_hop): every one of these draws was on the fixed UVINTSCALE divisor before,
		// and RPCS3_REMIX_UVSCALEHOP=0 puts them back there in one relaunch.
		bool hopped = false;

		// ROUND 52 (step 2c). The walk refused with "affine:mul-two-scales": the program multiplies
		// its UV pair by two constants in series and this form carries exactly one scale slot, so
		// taking either would be a guess. A flag rather than a strcmp of 'reason' at the apply site,
		// because the string literal lives in another translation unit and pointer equality across
		// TUs is not guaranteed. Counted as uv_affine_two_scales.
		bool two_scales = false;

		u8 attribute = 0xff;

		u16 scale_slot = s_no_uv_slot;

		// One divisor component per lane (without rows) or per row (with rows) - NOT one scalar.
		// Haze's c151 is a *vector* of per-attribute-set divisors, proven by its own ucode: program
		// f7f12d5d15bb9c37 multiplies ATTR8 by c151.x, ATTR9 by c151.y and ATTR10 by c151.z in the
		// same TEX0 slice, and 24d1ba819f701e47's live c151 reads [0.00024426 3.05176e-05
		// 0.00024426 3.05176e-05] - two different divisors inside one uploaded vector. A program is
		// therefore free to divide u and v by different components, and bab9af462da74331 does
		// exactly that in one instruction: 'MUL o7.xy = ATTR8.xyxx * c68.xyxx'. A single scalar
		// cannot express it, so that form used to be refused ("affine:mul-two-components") and the
		// draw fell back to the fixed 1/4096 on *both* lanes.
		// RPCS3_REMIX_UVSCALELANES=0 restores the single-scalar reading and the refusal.
		u8 scale_component[2] = { 0, 0 };

		// ROUND 58 (step 5). The SECOND scale of the two-scale family, resolved rather than
		// refused, under RPCS3_REMIX_UVSCALE2. s_no_uv_slot = there is no second scale and the
		// replay multiplies by 'scale' alone, exactly as before this field existed. Applied AFTER
		// the first, per lane, in the order the ucode multiplies them (19E83AA14ADE8DF5.vp:
		// '2: MUL r1.xy <- v5, c464.x' then '11: MUL r1.xyzw <- r1.xyxy, c463.zzxy').
		u16 scale2_slot = s_no_uv_slot;
		u8 scale2_component[2] = { 0, 0 };

		// row_slot[0] multiplies the scaled attribute component attr_component[0], row_slot[1]
		// the one at attr_component[1]. row_component[r][l] is the component of that slot feeding
		// output lane l (0 = u, 1 = v), so one instruction supplies both lanes of one row.
		u16 row_slot[2] = { s_no_uv_slot, s_no_uv_slot };
		u8 row_component[2][2] = {};

		// Which attribute component feeds each row (with rows) or each output lane (without).
		// Recorded rather than assumed because the swizzles in this family are not the identity.
		u8 attr_component[2] = { 0, 1 };

		u16 bias_slot = s_no_uv_slot;
		u8 bias_component[2] = {};
		bool bias_negate = false;

		u16 bias2_slot = s_no_uv_slot;
		u8 bias2_component[2] = {};
		bool bias2_negate = false;

		// Why the walk stopped, for the uvscale-fixed census. Static string, never owned.
		const char* reason = "not-scanned";
	};

	// One step of the "vertex attribute -> address register" computation, stored innermost
	// first. Every factor is a transform constant, so the whole chain is re-evaluated per draw
	// from live register state rather than baked at scan time.
	struct bone_index_op
	{
		enum class kind : u8
		{
			scale, // v = v * c[mul_slot][mul_component]
			affine, // v = v * c[mul_slot][mul_component] + c[add_slot][add_component]
			floor, // v = floor(v)
			immediate_scale // v = v * immediate
		};

		kind op = kind::floor;
		u32 mul_slot = 0;
		u32 add_slot = 0;
		u8 mul_component = 0;
		u8 add_component = 0;

		// v = v * immediate. Unlike 'scale' the factor is an integer the *ucode* built out of
		// repeated self-addition rather than a constant slot, so there is nothing to read back
		// per draw. Every one of Resistance 2's (NPEA00431) 16 four-bone blend programs opens
		// with the same two instructions - 'r1 = attr + attr' then 'r1 = attr + r1' - which is
		// x3, and 3 is exactly the row stride of the c32/c33/c34 palette those programs read.
		// Without this the ADD walk below refuses them ("bone index ADD has no constant bias")
		// and the index lands on bone/3 instead of bone.
		f32 immediate = 1.f;
	};

	// Longest attribute -> address-register chain that is followed. The one observed program
	// needs three steps (MAD, FLR, MUL); the rest is headroom, not speculation.
	inline constexpr u32 max_bone_index_ops = 6;

	// One resolved "vertex attribute component -> address register component" path. The
	// single-matrix rigs need exactly one; a weighted blend needs one per bone, because each
	// bone's palette row is selected by a different component of the same address register.
	struct bone_index_chain
	{
		bool resolved = false;
		u32 attribute = 0;
		u32 component = 0;
		u32 op_count = 0;
		bone_index_op ops[max_bone_index_ops] = {};

		// The ARL's own source modifiers, applied after 'ops' and before the truncate, in the
		// hardware's order: absolute value first, then negate (VertexProgramDecompiler.cpp:151-163).
		bool index_abs = false;
		bool index_negate = false;
	};

	// Bones one vertex may be weighted to. Four is not a choice: it is what
	// remixapi_MeshInfoSkinning's consumer asserts (rtx_types.cpp:333,
	// 'assert(skinningData.numBonesPerVertex <= 4)') and what every observed RSX blend program
	// writes (one MUL plus three MADs per palette row, weighted by .x/.y/.z/.w of one attribute).
	inline constexpr u32 max_blend_bones = 4;

	// --- round 11: what the VERTEX program routes into COL0 -------------------------------------
	//
	// Rounds 9/10's vertex-colour replay rests on one unstated assumption: that the fragment
	// program's COL0 interpolant carries the mesh's ATTR3. COL0 is not an attribute - it is the
	// vertex program's output register o1, and the ucode is free to pass ATTR3 through, scale it,
	// compute it from something else entirely, or never write it at all. Five programs decoded from
	// Haze's own cached ucode span all of it (offline sweep, 82 cached .vp, round 11):
	//
	//   2F64C2F8FFD6ADD1  HUD gauges     1:MOV>o1.xyzw(I3.xyzw)                     passthrough
	//   FC0FAC8AFCCEC49A  sky dome       0:MOV>o1.xyzw(I3.xyzw)                     passthrough
	//   EDC10321BF7CEB8F  backdrop haze  0:MUL>o1.xyzw(I3.xyzw,c[18].xyzw)          scaled, K=18
	//   F39F504649B6F442  effects sheets 0:MUL>o1.xyzw(I3.xyzw,c[18].xyzw)          scaled, K=18
	//   C97CD1531AC480D8  giant flower   rgb: I3.xyz*c[62] -> *c[464].y -> *c[66].x scaled_chain
	//                                    alpha: o1.w's chain reaches a c[59]/c[466] distance ramp,
	//                                    NOT I3.w  =>  vcol_alpha_from_attr = false
	//
	// The flower row is the byte-level cause of round 9/10's invisible foliage: its mesh ATTR3
	// alpha is 00, the backend replayed that 00 into the submitted colour, the fragment program
	// modulates alpha by COL0.a and the draw dies - while on hardware the mesh alpha never reaches
	// the fragment stage at all. No consumption gate can fix a wrong SOURCE, which is why the route
	// supersedes round 10's FPVCOLALPHAGATE as the load-bearing protection (that gate stays; it is
	// still correct for its own blend-off/atest-off corner).
	//
	// The classification is deliberately strict: anything not matched exactly is 'computed', which
	// means NO replay, which means the vertices stay at the decode loop's 0xFFFFFFFF white. The
	// failure direction is white, never a new wrong colour.
	enum class vcol_route : u8
	{
		// No write to o1 at all: COL0 is undefined and replaying anything into it is fabrication.
		none = 0,
		// 'MOV o1.xyzw, I3.xyzw' - full mask, identity swizzle, no negate.
		passthrough,
		// 'MUL o1.xyzw, I3.xyzw, c[K]' - one constant slot, folded per draw.
		scaled,
		// o1's rgb lanes reach I3's rgb lanes through <= 3 MOV/MUL hops whose every other operand
		// is a transform constant. The alpha lane is classified SEPARATELY (vcol_alpha_from_attr).
		scaled_chain,
		// Written from anything else - a computed ramp, a lighting term, a partial mask, an
		// indexed constant, the scalar half.
		computed,
		// Round 12. 'MOV o1.xyzw, c[K]' - the mesh's ATTR3 is never read at all and COL0 is one
		// flat colour for the whole draw. Distinct from every route above because there is no
		// attribute to replay: these meshes frequently carry no ATTR3, so the ATTR3 decode returns
		// early and the draw keeps the decode loop's white. Round 11 classified this shape
		// 'computed' and refused it, which is why the HUD gauge twin renders white rather than the
		// amber the constant holds. See vcol_constant_route_enabled().
		constant,
	};

	// Static string for the census/pick lines. Never null.
	const char* vcol_route_name(vcol_route route);

	// How many constant factors one lane of the route may accumulate before the walk gives up.
	inline constexpr u32 max_vcol_scale_factors = 4;

	enum class vp_archetype
	{
		// Nothing usable found: unresolved indexed constants, SCA position write,
		// unresolved temp chase.
		unknown,
		// 0 or 1 distinct constants feed the position: a pre-transformed / 2D program.
		screen_space,
		// One group multiplies the position straight into HPOS: an MVP or VP, needs splitting.
		fused,
		// Two or more chained groups, so the split is already done by the title itself:
		//   clip = pos * G0 * G1 * ... * G(n-1)
		//   world = G0 * ... * G(n-3),  view = G(n-2) (identity when n == 2),  proj = G(n-1)
		layered,
		// As 'layered', plus an innermost bone-palette group read through the address register:
		//   clip = pos * c[palette_base + a ..] * G0 * ... * G(n-1)
		// The palette group is NOT one of the G's: it is submitted to Remix as per-instance
		// bone transforms, so the G's are exactly the world/view/projection stack again.
		skinned_layered,
	};

	struct vp_fingerprint
	{
		vp_archetype archetype = vp_archetype::unknown;

		// group[0] is the innermost (applied first), group[count - 1] writes HPOS.
		u32 group_count = 0;
		u32 group_base[max_transform_groups] = {};
		chain_shape group_shape[max_transform_groups] = {};

		// Rows the group actually supplies: 4, or 3 with the homogeneous w written separately as
		// 'MOV r.w, c[K].c'. The same "three rows plus a proven w, or all four" rule the palette
		// (match_indexed_affine) and the blend path already apply - the plain object matrix was the
		// one shape that still demanded four DP4s, which is why 716b0c260da02533 walks back from
		// HPOS, matches c8..c11, then fails to see the c32..c34 object matrix behind it and draws
		// object space straight into the world.
		// Read through read_group_matrix, never read_slot_block directly: a 3-row group's fourth
		// slot is an unrelated constant, and the row it stands in for is (0,0,0,1).
		u32 group_rows[max_transform_groups] = {};
		u32 group_w_slot[max_transform_groups] = {};
		u32 group_w_component[max_transform_groups] = {};

		// The innermost group's operand resolved to a vertex attribute. When false the
		// vertices we submit are not in the space the innermost group expects.
		bool inner_is_input = false;

		// Captured DS particle shader supplies XY/W from c8..c11 but synthesizes Z.
		// Replay the physical billboard depth; its per-vertex soft-depth bias is not geometry.
		bool demons_particle_depth = false;

		// Round 9. At least one of this program's fused groups only matched because the
		// mixed-lane chase resolved its differing row scalars back to one base temp. The replay
		// is then deliberately the un-swayed geometry - the additive hops the chase stepped over
		// are the animation. RPCS3_REMIX_MADCHAINMIX=0 refuses these programs again.
		bool mad_mixed_lanes = false;

		// Round 17. A mixed-lane group that ALSO needed the divide's lane permutation: its
		// per-vertex divide parked the position components in lanes other than x, y, z, and the
		// divide instruction's own writemask and source swizzle were read to prove which lane
		// carries which component. mad_lane_of_component[k] is the lane holding attribute
		// component k, and mad_lane_attribute is the attribute that divide read (required to be 0,
		// the position, before it can drive the replay). Separate from mad_mixed_lanes so the
		// census can tell the two populations apart. RPCS3_REMIX_MADLANEMAP=0 refuses these again.
		bool mad_lane_permuted = false;
		u8 mad_lane_attribute = 0;
		u8 mad_lane_of_component[3] = { 0, 1, 2 };

		// Round 11. What this program's ucode routes into output o1 (= COL0), and the constant
		// factors the replay has to fold to reproduce it. See the vcol_route doc block above.
		// vcol_scale_slot[lane][n] / vcol_scale_comp[lane][n] name the (slot, component) pairs the
		// walk collected for that lane, innermost first; the fold is their product. Empty for
		// 'passthrough' (nothing to fold) and for 'computed'/'none' (nothing is replayed).
		//
		// ROUND 12 CAVEAT - these arrays are OVERLOADED. For 'constant' they hold ONE pair per lane
		// whose meaning is the COLOUR ITSELF, not a multiplicative factor, so the fold loop in
		// apply_vertex_colour must never see a constant-route draw. It cannot: the constant branch
		// returns before it. Anything new that reads these arrays has to test vcol_route_kind
		// first - "has scale slots" no longer implies "is a scaled route".
		vcol_route vcol_route_kind = vcol_route::none;

		// o1.w's chain reaches I3.w. False on the flower, whose alpha is a computed distance ramp -
		// and that is exactly the draw whose alpha must never be replayed from the mesh attribute.
		bool vcol_alpha_from_attr = false;

		u8 vcol_scale_count[4] = {};
		u16 vcol_scale_slot[4][max_vcol_scale_factors] = {};
		u8 vcol_scale_comp[4][max_vcol_scale_factors] = {};

		// Round 10. This program's position decode was only reachable because match_const_affine
		// stepped over an accumulate-in-place decoration, merged a per-lane assembly, or forwarded
		// its scale operand through a MOV of a constant. Like mad_mixed_lanes, the replay is then
		// deliberately the UN-decorated geometry - the billboard offset and the sway the walk
		// stepped over are the animation. RPCS3_REMIX_MADACCUM=0 refuses these programs again.
		bool affine_accum_walk = false;

		// Some programs decompress the attribute with 'pos * s + b' before the first matrix
		// (quantised chunk geometry). Both operands are transform constants, so the affine
		// step can be rebuilt per draw and prepended to the world transform.
		bool has_prescale = false;
		u32 prescale_scale_slot = 0;
		u32 prescale_scale_component = 0;
		u32 prescale_bias_slot = 0;

		// The same decompression written as separate instructions instead of one MAD, and with a
		// scale that may differ per axis. Resistance 2 (NPEA00431) writes it both ways:
		//   ca526d308f1650bb  0:MUL>r0.xyz(I0.xyzx,C0.xyzx,...)c17i0        pos * c17.xyz
		//   c87769e09c995db9  0:ADD>r0.xyz(C0.xyzx,I0.xyzw,I0.xyzx)c18i0    pos + c18.xyz
		// match_prescale only recognises the fused 'MAD attr, scalar_temp, c[K]' form with a
		// broadcast scalar, so both of these came back has_prescale = 0 and the raw quantised
		// attribute was submitted against a matrix that expects the decoded one - the vertex
		// explosion. Same rebuild rule as has_prescale: every operand is a transform constant, so
		// the affine step is evaluated per draw and prepended to the world transform.
		bool has_const_affine = false;
		bool affine_has_scale = false;
		bool affine_has_bias = false;
		u32 affine_scale_slot = 0;
		u8 affine_scale_component[3] = { 0, 1, 2 };
		u32 affine_bias_slot = 0;
		// The scale came from RCP(c[K].<c>) in a scalar slot, so the factor is 1/value.
		bool affine_scale_reciprocal = false;
		// '(attr + b) * s' rather than 'attr * s + b'. build_prescale composes scale then bias, so
		// the translation row carries b*s when this is set.
		bool affine_bias_before_scale = false;
		// Which match_const_affine test refused, kept whether or not the match succeeded. A dozen
		// separate exits all read as 'affine=0' from the census, and "this decode is written in a
		// shape the grammar does not cover" and "this program has no decode" want opposite fixes.
		// String literal, so this is a borrowed pointer with static lifetime, never freed.
		const char* affine_reason = "untried";

		// Round 50. The vertex attribute this program's position was actually read from, as named
		// by match_const_affine's MUL arm. 0 means ATTR0, which is both the default and the value
		// every program carried before the round, so the single decode site can index by this
		// field unconditionally without moving any title that already rendered.
		//
		// Eat Lead (BLUS30267) is the title that forced it: its world programs feed HPOS from
		//     0: VEC MUL r4.xyz <- v2.xyzx, c[465].xxxx
		// and its sprite program from v1 * c[467].x, while ATTR0 carries a byte-quantised normal.
		// Submitting ATTR0 as the position turned the whole game into a 0..254-unit cloud.
		// RPCS3_REMIX_POSINPUT=0 makes the arm refuse again and this field stays 0 everywhere.
		u8 position_input = 0;

		// The object placement R2's indexed-palette character programs apply between the bone
		// palette and the outer group, which nothing expressed before:
		//     pos = (dot(p,cA), dot(p,cB), dot(p,cross(cA,cB))) * cS.w + cS.xyz
		// row_slot[0]=cA, row_slot[1]=cB, scale_slot=bias_slot=cS. See match_basis_affine.
		//
		// NOT YET APPLIED. per_draw_transform does not read these - the fields and the census line
		// exist so the composed matrix can be checked against a drawn frame before it is allowed to
		// move 82190 draws. Wiring it in is the next step, not this one.
		bool has_basis_affine = false;
		u32 basis_row_slot[2] = { 0, 0 };
		u32 basis_scale_slot = 0;
		u32 basis_bias_slot = 0;
		const char* basis_reason = "untried";

		// A 2D transform into HPOS.xy only. match_dp4_chain needs four writers, one per HPOS
		// component, over four consecutive slots - which a 3D 4x4 always has. A 2D ortho does
		// not: it needs rows for x and y, and z/w are a constant depth written by a single
		// instruction. R2's menu backdrop fc5915fd48d91a98 is exactly that shape -
		//   4:DP4>o0.x(...)c32   3:DP4>o0.y(...)c33   1:DP4>o0.zw(...)c35
		// - three distinct constants, so it fell into the 'no matrix chain, <4 constants'
		// screen_space case and composite_ui_draw used the *raw* attribute values as if they
		// were already projected. The dropped row is the one carrying the y flip, which is why
		// the menu drew upside down while the 2D programs that do match (their y row is
		// [0 -2 0 0]) drew correctly. Same class as the Haze bone palette: a slot-count
		// assumption rejecting a legitimate transform.
		//
		// Applied by the 2D compositor only. The 3D path still requires a full 4x4 - a program
		// that never writes HPOS.z is not describing a depth and has no business there.
		bool has_ortho2d = false;
		u32 ortho2d_slot_x = 0;
		u32 ortho2d_slot_y = 0;

		// The matrix chain into HPOS was only found by following a writer through the register it
		// was parked in. match_dp4_chain wants four writers of HPOS, each a DP4 writing one
		// component; Resistance 2 (NPEA00431) writes the w row into a temp and moves it out
		// afterwards, so one of the four is a MOV and the whole 4x4 was discarded:
		//   da1d428df5ba05b6  16:DP4>o0.x(T2,c32)  15:DP4>o0.y(T2,c33)  14:DP4>o0.z(T2,c34)
		//                     17:DP4>r1.w(T2,c35)  24:MOV>o0.w(T1.wwww)
		// c32..c35 over one source is an ordinary 4x4 that only the detour hid. Of R2's 129
		// programs, 46 came back arch=unknown "no matrix chain into HPOS", and 29 of those are
		// exactly this shape (11 more are MOV>o0.xyzw, 5 are ADD>o0.xyzw, 1 is mixed). Replaying
		// the rule over bin\remix_dump.log resolves 33 of the 46 - the 29 plus 4 of the
		// MOV>o0.xyzw - into c32 (18), c8 (11) and c0 (4). Only 34.1% of submitted draws got a
		// world matrix at 9c73eb0 (world_applied=437321 / submitted=1283161); the rest drew at the
		// identity and piled up on the origin, which is the reported vertex explosion.
		//
		// Same class as has_ortho2d and the ADD src2 operand scan: a structural assumption about
		// how a transform is written rejecting a legitimate one.
		bool hpos_indirect = false;

		// HPOS.z was written as a w-buffer premultiply, '(c[k].pos) * (c[w].pos)', and the matrix
		// was only recovered by taking z from the c[k] row (repair_wbuffer_z). Diagnostic; the
		// group it produces is an ordinary one.
		bool hpos_wbuffer_z = false;

		// At least one row reached its HPOS lane through a MOV or a MAD rather than landing there
		// as a DP4, and was only recovered by following that hop back (repair_split_rows). Same
		// standing as hpos_wbuffer_z: diagnostic, and the group it produces is an ordinary one.
		// 'hpos_split_shear_const' names the constant slot whose x supplies the MAD's factor - the
		// term the repair drops - or umax when no lane carried one. On Ratchet & Clank (RC1) that
		// is c[17], and the dropped term is exactly zero whenever c[17].x is.
		bool hpos_split_rows = false;
		u32 hpos_split_shear_const = umax;

		// A MOV was reached whose definition could not be pinned down - defined by the SCA half of
		// a co-issued word, written under a condition, or negated/saturated on the way out. Refused
		// rather than guessed, and counted so the size of that population is visible.
		bool hpos_indirect_refused = false;

		// The program's position slice reads an indexed constant, so the indirection was not
		// offered to it at all (see scan_vertex_program). These are the skinned rigs; resolving
		// them is a change to the skinning path, not to this one.
		bool hpos_indirect_indexed = false;

		// Which input attribute the ucode moves into each texcoord output's xy, read from the
		// program instead of inferred from the attribute's size and type. Index n is TEX<n>, i.e.
		// output register o[7+n] (rpcs3's own output table, VKVertexProgram.cpp:292-299).
		// s_no_texcoord_input when the write could not be reduced to one attribute read straight.
		u8 texcoord_input[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

		// The constant slot this program multiplies its texcoord attributes by before writing TEXn,
		// and a bit for every attribute multiplied by that same slot. A program may combine two UV
		// sets that share one scale; that is still an unambiguous divisor. A zero mask means the slice
		// holds no usable multiply. This is the divisor UVINTSCALE hardcodes:
		// Haze (BLUS30094) keeps it in c151 and it is not one value across programs - measured
		// 1/32768 on most, ~1/4094 on some and 1 on others - so no fixed constant can be right for
		// all of them. Read the slot instead; s_no_texcoord_scale falls back to UVINTSCALE.
		// ROUND 52 (step 2a): u16. See s_no_texcoord_scale for why the u8 was a silent wrong read
		// rather than a narrower one, and for the Eat Lead census row that proves it.
		u16 texcoord_scale_slot[8] = {
			s_no_texcoord_scale, s_no_texcoord_scale, s_no_texcoord_scale, s_no_texcoord_scale,
			s_no_texcoord_scale, s_no_texcoord_scale, s_no_texcoord_scale, s_no_texcoord_scale };
		u16 texcoord_scale_inputs[8] = {};
		// ROUND 53. WHICH COMPONENT of texcoord_scale_slot holds the divisor. Every reader of the
		// scalar slot used to take value[0] unconditionally, which is right only when the ucode
		// says c[N].x - and the MOV walk that now resolves Demon's Souls' world texcoords can
		// forward any component. Defaults to 0, so every program resolved before this existed
		// reads exactly as it did.
		u8 texcoord_scale_component[8] = {};
		texcoord_scale_refusal texcoord_scale_refused[8] = {};

		// The full affine form of the same TEXn write, when the slice matches the family
		// uv_affine_form documents. Strictly richer than texcoord_scale_slot above: where both
		// resolve they agree on the scale, and where only this one does the scalar path was
		// falling back to the fixed UVINTSCALE divisor. Replayed under RPCS3_REMIX_UVAFFINEALL.
		uv_affine_form texcoord_affine[8] = {};

		// --- ROUND 58 (step 4a): the SECOND lane pair of the same TEXn output --------------------
		// texcoord_affine above walks o[7+n].xy. A vertex program is free to put a second,
		// differently scaled copy of the same UV in .zw, and Eat Lead's interior programs do:
		//   7565B4CBD93682D9.vp   1: MUL r0.xy <- v4.xyxx, c[464].xxxx     (1/2048 dequant)
		//                        10: MUL r0.zw <- r0.xxxy, c[463].xxxx     (times live 0.1)
		//                        11: MOV o[7](TEX0).xyzw <- r0.xyzw
		//   8D67DBFCC9DB7A96.vp   the mirror image - .xy carries the two scales, .zw the single one
		// The fragment program then samples the macro/grime map through the .zw pair
		// (fp_fingerprint::coord_lanes), and applying the .xy form's divisor to it is the
		// ten-times-too-dense tiling. Resolved by the same walker with lane_pair = 1.
		uv_affine_form texcoord_affine_zw[8] = {};

		// Bit 0: the .xy form of this output is a two-scale (or otherwise different SCALE form)
		// while .zw resolved with one; bit 1: the reverse. Exactly one bit set = that pair is the
		// MACRO pair. Keyed on the scale form ONLY, so two pairs that differ merely in which
		// attribute they read - 1B20D02263AA8EE4.vp 'MOV r2.xy <- v4 / MOV r2.zw <- v5.xxxy /
		// MUL o7.xyzw <- r2, c464.x', and 70931FA705186608.vp - do NOT qualify, and neither does
		// 498CEDEB9B687381.vp, whose '0: MUL o[7].xyzw <- v4.xyxy, c[464].xxxx' gives both pairs
		// the same scale. Per TEXn output: a unit sampled through TEX4 is judged by TEX4's forms.
		u8 texcoord_macro_lanes[8] = {};

		// The program divides the position by the attribute's own w before the first matrix
		// ('pos.xyz * RCP(pos.w)'). The divisor is per vertex, so unlike has_prescale it cannot
		// be folded into the world transform - the submitted vertex has to be divided at decode
		// time or the mesh is blown apart from the inside. Always ATTR0; the matcher refuses any
		// other input because ATTR0 is the only attribute submitted as a position.
		bool has_wdivide = false;

		// Round 8. How many w-only writes match_wdivide's walk stepped past to find the divide MUL.
		// 0 = this program matched under the old single-last-writer selection too; >0 = it decodes
		// ONLY because of RPCS3_REMIX_WDIVWALK, i.e. it is one of the rescued giant-geometry
		// programs. Kept beside has_wdivide rather than folded into it because "does it divide" and
		// "why did we find the divide" are separate questions, and only the second one attributes a
		// visible change to this round.
		u8 wdivide_shadowed = 0;

		// Skinning. 'a' as computed below is already the bone's *slot* offset (bone * stride),
		// because the ucode reads c[palette_base + a] .. c[palette_base + a + 3] with a single
		// shared address register - so there is no separate stride to recover.
		bool skinned = false;
		u32 palette_base = 0;
		chain_shape palette_shape = chain_shape::none;

		// The palette's actual layout. Up to and including ae94587 every matcher produced a
		// 4-row, stride-1, all-columns group, because both of them refused anything else - and
		// that is why build_skinning had never run on any title and skin_submitted was 0 against
		// skin_unrecognised = 1481510 on Resistance 2 (NPEA00431), 8965 frames, 7588805 draws.
		//
		// Replaying the matchers on paper over every dumped program that reads an indexed constant
		// in its position slice (36 unique, fp.log / d8.log / r2_dump3.log) says what the real
		// shapes are. R2 uses 3 rows of DP4 at stride 1 from c31, with the homogeneous 1 moved into
		// the operand's w by a separate MOV and a constant translation added *after* the matrix -
		// 16 of the 36 programs, every one of them identical. Haze is on record as 3 rows of .xyz
		// at stride 2 (c27, c29). Neither could be expressed before.
		//
		// palette_w_slot is the constant component the ucode moves into the operand's w for a
		// 3-row group. It is read back per draw and required to be 1: three rows only transform a
		// *point* when the fourth coordinate is one. s_no_palette_w when the group supplies all four.
		u32 palette_rows = 4;
		u32 palette_stride = 1;
		bool palette_xyz_only = false;
		bool palette_has_bias = false;
		u32 palette_bias_slot = 0;
		u32 palette_w_slot = 0xffffffff;
		u32 palette_w_component = 0;
		// DPH supplies the point's homogeneous 1 without reading an attribute/constant w.
		bool palette_implicit_w = false;

		// The group's rows were written into one register and moved into another by the post-matrix
		// translation ('r1.xyz = c18.xyz + r0.xyz'), rather than translated in place. Same
		// instructions, same constants, different register allocation - Resistance 2 (NPEA00431)
		// writes both, and only the in-place form was expressible up to this change. Recorded so a
		// program that resolves only through the relaxed rule is identifiable in a capture.
		bool palette_bias_forwarded = false;

		// The attribute component that feeds the address register, and how.
		u32 bone_attribute = 0;
		u32 bone_component = 0;
		bool bone_resolved = false;
		u32 bone_op_count = 0;
		bone_index_op bone_ops[max_bone_index_ops] = {};

		// The source modifiers on the ARL itself, applied after bone_ops and before the truncate,
		// in the hardware's order: absolute value first, then negate
		// (VertexProgramDecompiler.cpp:151-163). Ignoring them read the wrong palette entry on
		// every Resistance 2 rig - see resolve_bone_index for the measurement. An index attribute
		// that packs a flag into its sign bit is read by the ucode as |w|, and 31 + (-1) = c30 is
		// not a bone: its three basis rows all point along Z, which draws a character as a line.
		bool bone_index_abs = false;
		bool bone_index_negate = false;

		// Per-vertex weighted skinning: the palette group is read once per bone per row and the
		// reads are summed, 'sum_k weight.k * c[palette_base + row + a.<swz_k>]'. Resistance 2
		// (NPEA00431) has 16 such programs and they are byte-for-byte the same shape - three
		// accumulators over c32/c33/c34, each built by one MUL and three MADs weighted by
		// .y/.x/.z/.w of one attribute (attr2 in 14 of them, attr3 in dc56c444e5cc83e7 and
		// 2f310e8120a344b2), and one ARL writing all four components of a0 from a temp holding
		// 3 x the index attribute.
		// They are the character rigs: refused whole at ae94587, where audit_indexing counted
		// their three extra address-register components as foreign_indexed_reads and the gate
		// took 541960 draws into skin_unrecognised.
		//
		// This is submitted to Remix as real per-vertex blend weights and indices
		// (remixapi_MeshInfoSkinning), *not* evaluated on the CPU: the API carries the channels,
		// so the mesh content stays the rigging (static per model) and only the bone matrices
		// move per frame. Baking the blend into positions instead would make the mesh content
		// change every animation frame, and a content-keyed mesh cache under that load is the
		// 32x mesh-creation blowup this backend has already been bitten by once.
		//
		// blend_weight_component[k] is the attribute component holding bone k's weight;
		// blend_bone[k] is the address-register path that selects bone k's palette entry. The
		// two are paired by the instruction that reads them, never by position: the ARL of every
		// R2 blend program carries a swizzle (a0.xyzw <- r1.wzxy in 12 of the 16), so assuming
		// "weight .x goes with a0.x" would read a different bone than the hardware does.
		bool skin_blended = false;

		// Round 52. The blend was matched by match_vertex_blend rather than match_blend_palette:
		// the ucode transforms the POINT by each bone and sums the four transformed points, instead
		// of summing the matrix rows and applying the sum once. Everything downstream of the match
		// is identical - same palette, same bones, same weights, same submission - so this exists to
		// scope the indexed-read audit (see scan_vertex_program) and to let the census and the
		// counters tell the two populations apart.
		bool skin_vertex_blend = false;

		u32 blend_bones = 0;
		u32 blend_weight_attribute = 0;
		u8 blend_weight_component[max_blend_bones] = {};
		u8 blend_addr_swz[max_blend_bones] = {};
		bone_index_chain blend_bone[max_blend_bones] = {};

		// The ucode carries skinning this recogniser cannot prove it understands: a second ARL,
		// or an indexed constant read through an address register/component other than the one
		// matched palette's. That is a blend rig being seen as single-bone, which is what
		// explodes a character. The draw is refused and counted, never drawn with one of its
		// bones - a missing character is an acceptable result, an exploded one is not.
		bool skin_unrecognised = false;

		// Diagnostics.
		u32 arl_count = 0;
		u32 indexed_reads = 0;
		u32 foreign_indexed_reads = 0;

		// Round 52, vertex-blend chains only: indexed reads of the matched palette that the matched
		// group does not itself explain. These are the program's OTHER uses of the same bone
		// matrices - Eat Lead rotates the normal, tangent and binormal by the palette's 256..258
		// columns, 36 reads that never reach HPOS - and they are why the exact 'rows x bones' count
		// cannot be applied to this family. 0 for every other shape. Printed as 'extra=' on the
		// indexed-world census; measurement only, nothing is refused on it.
		u32 indexed_reads_outside_chain = 0;
		u32 distinct_consts = 0;
		u32 chain_instructions = 0;
		bool indexed_const = false;

		// Every constant this program reads through the address register resolves to the *same*
		// constant for every vertex of the draw, because the one ARL in the ucode loads the address
		// from a constant slot rather than from an attribute. 'a' is then a draw-uniform value that
		// can be read out of the transform constants at submit time, and c[K + a] is an ordinary
		// constant read wearing an indexed encoding - there is no palette, so the indexed-const
		// refusal's stated hazard (the palette silently not applied) cannot arise.
		//
		// Resistance 2 (NPEA00431) f7576a48e6289f83 and ba93cfebcefde22f are that shape:
		//   3:ARL>r0.x(C0.wwww,...)c47i0     7:MUL>r1.xyz(C0.xyzx,T0.wwww,...)c52[a]i0
		// The indexed read is a fixed direction the ucode scales by attr1.z, not a transform.
		//
		// addr_const_* is where the address comes from and how the ARL reads it (abs first, then
		// negate, then truncate toward zero); indexed_base_* is the span of base slots the program
		// indexes, so the resolved offset can be range-checked against every read rather than one.
		bool indexed_addr_uniform = false;
		u32 addr_const_slot = 0;
		u32 addr_const_component = 0;
		bool addr_index_abs = false;
		bool addr_index_negate = false;
		u32 indexed_base_min = 0;
		u32 indexed_base_max = 0;

		const char* note = "";
		const char* skin_note = "";

		bool is_layered() const
		{
			return archetype == vp_archetype::layered || archetype == vp_archetype::skinned_layered;
		}

		bool has_outer() const
		{
			return archetype == vp_archetype::fused || is_layered();
		}

		u32 outer_base() const { return group_count ? group_base[group_count - 1] : 0; }
		chain_shape outer_shape() const { return group_count ? group_shape[group_count - 1] : chain_shape::none; }
	};

	// Fingerprints the rebased ucode of an already-analysed vertex program. Never throws;
	// anything it cannot follow comes back as vp_archetype::unknown with a reason in 'note'.
	vp_fingerprint scan_vertex_program(const RSXVertexProgram& vp);

	const char* archetype_name(vp_archetype a);
	const char* shape_name(chain_shape s);

	// ---------------------------------------------------------------------------------------
	// Fragment-program fingerprint
	// ---------------------------------------------------------------------------------------

	// Round 9. How a fragment program produces its final colour, restricted to the two shapes
	// this backend can honestly replay through the instance blend extension's fixed-function
	// stage. Both were decoded from Haze's own cached ucode:
	//
	//   vcol_pass      DBB822C008101A2.fp (fp=938cfb957fcb7db7, the yellow nectar danger pulse)
	//                  is literally ONE instruction: 'MOV r0.xyzw, COL0.xyzw' with the END bit.
	//                  16 bytes, no texture, no constant. The fragment colour IS the interpolated
	//                  vertex colour - the hue, the ring gradient and the fade are all ATTR3.
	//   vcol_modulate  D12A627700B7818A.fp (fp=4f1d1bce7ffdfd9f, the giant flower) is two:
	//                  'TEX r0.xyzw, TEX0, tex0' then 'MUL r0.xyzw, r0.xyzw, COL0.xyzw'.
	//                  Textbook texture-times-vertex-colour modulate.
	//
	// Anything deeper - extra instructions between the sample and the output, swizzled or negated
	// reads, a conditional write - is 'other' and changes nothing at all. The classification is
	// deliberately narrower than the population it could plausibly cover: a wrong answer here
	// re-colours world geometry, and the census names every program it does classify.
	// ROUND 52 adds the two TEXCOORD-routed shapes below. They are the same two shapes one level
	// across: a 2D program that carries its colour on a texcoord varying instead of on COL0.
	// Decoded from Eat Lead's (BLUS30267) own UI fragment programs this round, both on disk under
	// bin\remix_ucode\ and both fed by the vertex program F2B6988E84056628.vp, whose slice 5 is
	//     5: VEC MOV o[8].xyzw <- v0.xyzw        ; TEX1 = ATTR0, the ub4 vertex colour
	// so the fragment side reads its tint from an INPUT in the TEX0..TEX7 range and never touches
	// COL0 at all:
	//
	//   texcoord_pass      CDFE447F7B7DED21.fp (the 512x512 B8 glyph atlas 8BD777992BB589A1)
	//                        0: MOV R1.xyzw <- TEX1.xyzw
	//                        1: MUL R0.x    <- R1.wwww, C{1,0,0,0}.xxxx
	//                        2: TEX R0.w    <- TEX0.xyzw tex0
	//                        3: SLT         <- R0.wwww, C{1/255,0,0,0}.xxxx, R0.xyzw ; 6: KIL
	//                        4: MUL R0.w    <- R0.xyzw, R0.xxxx
	//                        5: MOV R0.xyz  <- R1.xyzw
	//                        8: MUL R0.xyzw <- R0.xyzw, C{1,1,1,1}.xyzw  END
	//                      rgb = TEX1.rgb, alpha = coverage * TEX1.w.
	//   texcoord_modulate  B8E4FC5C9DDDD4E4.fp (the 640x480 DXT45 draws)
	//                        0: MOV R1.xyzw <- TEX1.xyzw
	//                        1: MUL R1.xyz  <- R1.xyzw, C{1,1,1,1}.xyzw
	//                        2: TEX R0.xyzw <- TEX0.xyzw tex0
	//                        3: MUL R0.xyz  <- R0.xyzw, R1.xyzw
	//                        4: MUL R1.w    <- R1.xyzw, C{1,0,0,0}.xxxx
	//                        5: MUL R1.w    <- R0.xyzw, R1.xyzw
	//                        8: MUL R0.w    <- R1.xyzw, C{1,1,1,1}.xyzw  END
	//                      rgb = tex0.rgb * TEX1.rgb, alpha = tex0.a * TEX1.a.
	//
	// NEITHER shape reaches vcol_replayable() - that predicate still requires out_vcol_attr == 1,
	// which these leave at 0 - so the 3D path's blend-extension replay is byte-identical. The only
	// consumer is composite_ui_draw's tint, behind RPCS3_REMIX_UITINTTEXCOORD.
	enum class fp_out_source : u8
	{
		other = 0,
		vcol_pass,
		vcol_modulate,
		texcoord_pass,
		texcoord_modulate,
	};

	// Static string for the census/pick lines. Never null.
	const char* fp_out_source_name(fp_out_source source);

	// ROUND 58 (step 2c). Values of fp_fingerprint::coord_lanes[]. Deliberately NOT an enum class:
	// the field is a u8 array printed straight onto two census lines and compared against a plain
	// integer at one apply site, and a scoped enum would only add casts at both.
	inline constexpr u8 s_fp_lanes_xy = 0;
	inline constexpr u8 s_fp_lanes_zw = 1;
	inline constexpr u8 s_fp_lanes_mixed = 2;
	inline constexpr u8 s_fp_lanes_none = 3;

	// "xy" / "zw" / "mix" / "-". Static string, never null.
	const char* fp_lanes_name(u8 lanes);

	// Which fragment texture units the ucode uses as a *colour* source, as opposed to a
	// perturbation the backend cannot evaluate. Cached per fragment program like vp_fingerprint,
	// so the walk below costs nothing per draw.
	struct fp_fingerprint
	{
		// Bit N: unit N is sampled by a TEX-family instruction. Same population as
		// fragment_program_metadata::referenced_textures_mask, recomputed here because the walk
		// needs the instruction anyway and the two must agree by construction.
		u16 sampled_mask = 0;

		// Bit N: unit N's sample reaches the COL0 register through colour-preserving operations
		// only. See scan_fragment_program for what "colour-preserving" means and why a dot
		// product disqualifies a path.
		u16 colour_mask = 0;

		// ROUND 43b. Bit N: unit N is sampled, and EVERY sample of it writes fewer than three
		// destination channels. Such a unit cannot be carrying RGB - it is a height, gloss, mask
		// or lookup map. This is structural, not a heuristic: the channels are not there.
		//
		// WHY IT EXISTS. colour_mask above is documented as an over-approximation that "can only
		// ever add units". On Haze it SATURATES: over the 323 sampled programs in
		// bin\remix_ucode\, 202 come back with colour_mask == sampled_mask, which
		// albedo_unit_mask() reads as "the ucode declines" and answers with the LOWEST referenced
		// unit. The run's counters agreed exactly - tex_albedo_ucode = 0 against
		// tex_albedo_guess = 7,383,877, i.e. the discriminator resolved nothing all session.
		//
		// From fp=65a91390aaf6bef3, the program on the picked white wall 9AAA430414B3D49D:
		//
		//    4: TEX R1     TEX2.zwzz, tex2    4 channels
		//    9: TEX R2.x   TEX2,      tex0    ONE channel  <- a parallax height map
		//   18: ADD R4.xy  TEX2, R2                        <- ...used as a UV perturbation
		//   21: TEX R2.yw  R4,        tex3    2 channels
		//   24: TEX R2     R4,        tex1    4 channels   <- the real diffuse
		//
		// tex0 is the lowest referenced unit, so it wins the decline, and a near-white greyscale
		// height map bound as the albedo IS a white wall.
		//
		// WHY NOT A LIVENESS KILL. Round 43 first shipped exactly that - the same backward walk
		// with an ordinary kill on unpredicated writes. It reached the right answer here (mask
		// 0x1f -> 0x16, unit 0 -> 1, identical to this rule) but re-elected the albedo of 36
		// programs, 16 of which moved to a unit >= 8, and the play-test came back as full-screen
		// coloured static. This rule re-elects THREE, all of them unit 0 -> unit 1, none above
		// unit 1. Same answer where it was verified, one twelfth of the blast radius.
		u16 narrow_sample_mask = 0;

		// --- ROUND 58 (step 2c): which LANE PAIR of its TEXn varying each unit is sampled with ---
		// A fragment TEX reads its coordinate pair out of one input register, and the swizzle says
		// which two lanes. Eat Lead's interior vertex programs write the SAME uv into .xy at 1x and
		// into .zw at 0.1x (7565B4CBD93682D9.vp: '1: MUL r0.xy <- v4, c[464].x' then
		// '10: MUL r0.zw <- r0.xxxy, c[463].x' with live c463=[0.1 0 0 0]), and the fragment program
		// samples the macro/grime map through the .zw pair - one repeat per ten world units. So
		// "which lanes" is the vertex side of the same fact the blend weight states.
		//   s_fp_lanes_xy     the TEX reads .xy of its input  (identity swizzle in the pair)
		//   s_fp_lanes_zw     the TEX reads .zw
		//   s_fp_lanes_mixed  two samples of one unit disagree, or the pair is neither
		//   s_fp_lanes_none   no direct varying read for this unit (the coord came from a temp)
		// Filled in the same block as coord_inputs, from the TEX's own src0 swizzle.
		u8 coord_lanes[16] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };

		// --- ROUND 58 (step 3): the program's own colour arithmetic, when it is a two-texture lerp
		// The shape, in program order, from bin\remix_ucode\B3EFF9C2B4AE9F66.fp (fp 2dd8807bcbe4e373,
		// the wall the user picked at extent 31.92):
		//     12: TEX R2.xyz <- tc0.zwzz [tex0]
		//     10: TEX R0.xyz <- tc0      [tex1]
		//     13: ADD R0.xyz <- R0, -R2
		//     16: MAD R0.xyz <- R0, c[].x, R2      c=[0.85 0 0 0]
		// i.e. colour = tex0 + 0.85*(tex1 - tex0). lerp_a is the unit that is the MINOR term at
		// weight > 0.5 (here tex0), lerp_b the major one (tex1), lerp_weight the constant. Nothing
		// is elected from these fields unless RPCS3_REMIX_FPLERPUNIT is on; with the knob off they
		// are printed by the fpdump/albedo-elect census only, which is what sizes the change.
		bool lerp_valid = false;
		u8 lerp_a = 0xff;
		u8 lerp_b = 0xff;
		f32 lerp_weight = 0.f;
		// Why the detector refused, for the census. Static string, never owned, never null.
		const char* lerp_note = "not-scanned";

		// Four-bit TEX input index per texture unit (0 = TEX0, 1 = TEX1, ...; 0xf =
		// unresolved). Texture unit and coordinate input are independent in RSX fragment
		// programs. Keeping them separate prevents a unit-0 sample sourced from TEX1 from
		// being rasterized with the vertex program's TEX0 output.
		u64 coord_inputs = ~0ull;
		u16 coord_ambiguous_mask = 0;

		// colour_mask needed COL0.w to be non-empty - no unit reached COL0.rgb. Diagnostic only;
		// the mask is used the same either way.
		bool alpha_only = false;

		// The ucode contains flow control (BRK/CAL/IFE/LOOP/REP/RET). The walk does not follow it
		// and never kills a live register, so the result stays an over-approximation - which only
		// ever moves the answer back towards the lowest-unit guess. Diagnostic only.
		bool has_flow = false;

		// The walk ran off the end of the ucode instead of stopping on an 'end' bit, or hit more
		// instructions than it will hold. colour_mask is then whatever it managed and is treated
		// as unresolved by the caller.
		bool truncated = false;

		// --- per-pixel discard (the cutout channel the backend never read) ----------------------
		// The ucode contains at least one KIL (RSX_FP_OPCODE_KIL = 0x12). rpcs3's own decompiler
		// translates KIL into a per-pixel 'discard' under the instruction's execution mask
		// (FragmentProgramDecompiler.cpp:1486-1489 -> AddFlowOp("_kill()")), and that conditional
		// discard IS how an RSX title cuts out foliage: the alpha test registers stay disabled, so
		// no material and no blend state can carry the mechanism. Note KIL is not in fp_op_is_flow -
		// it has no branch target - so has_flow says nothing about it.
		bool has_kil = false;

		// The KIL executes under a *partial* condition mask. AddFlowOp's own three cases:
		// all of exec_if_lt/eq/gr set is an unconditional kill (every pixel dies - not cutout, and a
		// program that did that would draw nothing at all), none set is a no-op the decompiler
		// comments out, and anything in between is the conditional form. Only the conditional form
		// is a cutout, so this is the flag the replay gates on, not has_kil.
		bool kil_conditional = false;

		// Best-effort threshold recovery, 0..255 to match remixapi's alphaReferenceValue, or -1 when
		// the shape was anything the matcher does not trust. The classic cutout shape is
		//   TEX rN, ...            ; sample the albedo
		//   ADDc rM, rN.w, -c[k]   ; set_cond, compare alpha against a literal
		//   KIL(lt)                ; discard where alpha < threshold
		// so the recovered constant is the alpha threshold. -1 is an acceptable answer: the knob
		// RPCS3_REMIX_FPKILREF covers it and the census prints what was found, which is what lets a
		// later round extend the matcher against real shapes instead of guessed ones.
		s16 kil_ref_estimate = -1;

		// What the recovery walk actually saw, for the census. Points at a static string.
		const char* kil_note = "";

		u32 instructions = 0;
		const char* note = "";

		// The first eight inline literal vec4s, in program order, already byteswapped the way the
		// shader sees them. Measurement input for the 'Remix lightpass:' census: if Haze's per-light
		// shadow/lighting composite carries light positions, colours or falloff constants at all,
		// they are either here or in the vertex constants - and nothing has ever dumped fragment
		// literals on this title, so "no light-shaped constant exists" has never actually been
		// checked. Costs 132 bytes per cached program and is filled by the same walk that already
		// decodes each literal for the KIL threshold recovery.
		f32 literals[8][4]{};
		u8 literal_count = 0;

		// --- round 9: the output colour source --------------------------------------------------
		// Classified from the LAST writer of the COL0 register, separately for rgb and alpha
		// because a program is allowed to write them from different instructions. Both decoded
		// Haze shapes write xyzw in one go, so on those two the pair agrees; the fields are kept
		// apart anyway so an asymmetric mask cannot be read as symmetric.
		fp_out_source out_rgb_source = fp_out_source::other;
		fp_out_source out_alpha_source = fp_out_source::other;

		// Which vertex colour register the classified shape reads: 1 = COL0, 2 = COL1, 0 = none.
		// The Remix blend extension only names VertexColor0, so a COL1 program is classified but
		// the replay treats it as COL0 - RSX titles put the diffuse colour in ATTR3/COL0 and this
		// backend only ever decodes ATTR3 into the submitted vertex, so a COL1 shape would be
		// replayed from the wrong data. Kept as a field so the census can say so rather than the
		// classifier silently widening.
		u8 out_vcol_attr = 0;

		// True when the classified shape is one the blend-ext replay acts on. COL1 shapes and
		// 'other' are false.
		//
		// ROUND 52: deliberately NOT widened to the two texcoord_* classes. Those name a colour on
		// a TEXCOORD varying, which this backend does not decode into the submitted vertex at all,
		// so the blend extension has nothing to replay from and the 3D path must keep behaving
		// exactly as it did. out_vcol_attr stays 0 on both new classes, so this is false for them
		// by construction as well as by intent.
		bool vcol_replayable() const
		{
			return out_vcol_attr == 1 &&
				(out_rgb_source == fp_out_source::vcol_pass ||
				 out_rgb_source == fp_out_source::vcol_modulate);
		}

		// --- ROUND 52: which TEXn varying carries the colour, on the two texcoord_* classes -------
		// 0..7 = the fragment program's TEXn input (attr_reg 4..11 minus 4), 0xff = "no texcoord
		// tint", which is every program on every other title measured so far. The VERTEX program's
		// texcoord_input[n] then names the attribute that feeds it, and composite_ui_draw reads its
		// per-vertex tint from that attribute instead of the hardcoded ATTR3.
		//
		// Set from the rgb terminal when that classified, otherwise from the alpha terminal - the
		// same precedence out_vcol_attr uses, for the same reason (they can only disagree when two
		// different instructions wrote the two halves).
		u8 out_tint_texcoord = 0xff;

		// --- ROUND 41: why the classifier said 'other', and how far it had to walk ---------------
		// The classifier reaches two shapes in the whole of Haze - MEASURED fpclass=1/1/0 on a
		// 64,139-flip run, i.e. ONE vcol_pass program and ONE vcol_modulate program, with
		// fpvcol_applied=698 of 5,783,237 submitted draws (0.012%). Everything else is 'other', and
		// 'other' is what makes every textured surface ship the parity fill tcolor=1/0/3 (colour =
		// texture, vertex colour discarded) - MEASURED on 192 of 192 'Remix alphastate:' rows.
		// Meanwhile 22 of 47 vertex programs, the wall program ad7ce9d672a0bf6b among them, carry a
		// fully replayable route=scaled slots=[c[18].x..w]. The vertex side is ready and the
		// fragment side is the gate.
		//
		// These fields exist so the NEXT widening is designed on the terminal instruction's actual
		// shape instead of on a guess. Populated only when the rgb walk ended in 'other'.
		u8 out_rgb_hops = 0;          // identity-MOV copies the walk stepped through (FPVCOLHOP)
		u8 out_rgb_opcode = 0xFF;     // terminal instruction's opcode, 0xFF = no writer found
		u8 out_rgb_srccount = 0;
		bool out_rgb_predicated = false;
		u8 out_rgb_srctype[3] = { 0xFF, 0xFF, 0xFF };
		// Bit per source slot: 0x1 clean COL0/COL1 read, 0x2 temp whose writer sampled a texture,
		// 0x4 temp (any), 0x8 non-identity swizzle, 0x10 negated, 0x20 abs.
		u8 out_rgb_srckind[3] = {};

		// --- ROUND 42: the modulate is not the terminal instruction on this title -----------------
		// True when out_rgb_source was decided by the DEEP search rather than by the terminal
		// instruction. The terminal fields above stay populated in that case, so a deep-classified
		// program still reports the shape that defeated the round-41 walk.
		bool out_rgb_deep = false;
		// The product of the fragment-side constant scales sitting between COL0 and the modulate.
		// MEASURED as exactly 2.0 on all four Haze programs whose ucode is on disk - the classic
		// "vertex colour is a 0..2 lighting multiplier packed into 0..1" convention. 1.0 means the
		// chain was a plain copy. Only ever applied to rgb: the ucode scales .xyz and copies .w.
		f32 out_vcol_scale = 1.f;

		// --- ROUND 62: COL0.rgb is an inline literal ---------------------------------------------
		// True when the terminal writer of COL0.rgb (the same index the classifier above reads) is
		// an unconditional 'MOV col0.xyz, c[]' writing all three lanes, with no negate and no abs.
		// out_rgb_const_rgb is the literal read THROUGH the operand's swizzle - 'c[].xyyx' of
		// [0.263 0.231 0 0] is the colour (0.263, 0.231, 0.231), and 'c[].xxxx' of 0.1725 is a grey.
		// The swizzle is part of the value, not a discriminator between colours and scalars.
		//
		// A flag beside out_rgb_source rather than a fifth fp_out_source value, so that field stays
		// 'other' for these programs and its consumers - the ucode-store gate, the fpother census,
		// fpclass= - are byte-identical with RPCS3_REMIX_FPCONSTALBEDO off. Clamped to 0..1.
		bool out_rgb_const = false;
		f32 out_rgb_const_rgb[3] = { 1.f, 1.f, 1.f };
	};

	// Reads the fragment ucode and reports which sampled units feed the final colour. 'ucode' is
	// the rebased program start (RSXFragmentProgram::get_data()), 'ucode_length' its byte length,
	// and 'fp32_outputs' the CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS bit, which is what decides
	// whether COL0 is R0 or H0 (FragmentProgramDecompiler.cpp:64-82). Never throws; anything it
	// cannot follow comes back with colour_mask 0 and a reason in 'note'.
	// RPCS3_REMIX_FPVCOLHOP=<max hops> (default 0 = OFF, round-40 behaviour bit-exactly).
	// Lets scan_fragment_program's output walk step BACKWARDS through identity temp copies before
	// it classifies. An identity copy is an unconditional MOV of a TEMP with identity swizzle and
	// no neg/abs - the identity function - so hopping one cannot admit a shape the classifier did
	// not already recognise; it only admits programs that write the recognised shape into a temp
	// and then copy it out. That makes this a strictly safe widening, and also a LIMITED one: a
	// program that does real arithmetic (fog, specular, a lerp) between the modulate and the export
	// will not be reached and must stay 'other'. Clamped to 8.
	u32 fp_vcol_hop_budget();

	// RPCS3_REMIX_FPVCOLDEEP=1 (default 0 = OFF, round-41 behaviour bit-exactly).
	//
	// ROUND 42. The round-41 hop was the right idea aimed one instruction too late. MEASURED from
	// the four Haze fragment programs whose ucode is on disk in bin\remix_ucode - two of them
	// (34389B9B085589D5 = fp aa0fe222771ff5c0, FB9E6A29D5BCC2E6 = fp 65a91390aaf6bef3) are
	// programs the user's own Ctrl+Clicks landed on walls - every one of them has this shape:
	//
	//     14: MOV  H1,      ATTR1                  ; COL0
	//     15: MUL  R1.xyz,  H1, {2,0,0,0}.xxxx     ; the 0..2 lighting expansion
	//     17: MOV  R1.w,    H1                     ; alpha copied UNSCALED
	//     26: TEX  R0,      ATTR5, tex0            ; the albedo
	//     27: MUL  R1,      R0, R1                 ; *** albedo x (COL0 x 2) ***
	//     ... 30 more instructions of normal map, specular and lighting composite ...
	//     57: MUL  R0.xyz,  R0, {0.999001,...}.xxxx  END
	//
	// The modulate is real, it is unambiguous, and it is THIRTY INSTRUCTIONS upstream of the
	// export. A classifier that only ever looks at the terminal instruction cannot see it, which is
	// why 39 of the 64 'Remix fpother:' rows in the round-41 run read the same terminal shape
	// (op=MUL t0=TEMP t1=CONSTANT) - the near-identity output scale, not the modulate.
	// Independently corroborated: round 11 transcribed the haze card's fragment program into a
	// comment in RemixGSRender.cpp and wrote 'rgb = texRGB * (2 * COL0.rgb) * 0.944243'.
	//
	// So this searches the whole program for the modulate instead of only its last line, and the
	// search is deliberately NARROW - four terms, all of which must hold:
	//   1. an unconditional MUL writing rgb,
	//   2. one operand a temp whose writer sampled a texture (the same is_sampled_temp predicate
	//      the terminal classifier already uses),
	//   3. the other operand a clean COL0 read, or a temp reached from one through nothing but
	//      BROADCAST constant scales - never COL1, never a per-channel constant, never a swizzle,
	//      negate or abs,
	//   4. the product provably live into COL0.rgb, by the same backward walk colour_mask uses.
	// COL1 is excluded by measurement, not by caution: these same programs carry the packed
	// tangent-space NORMAL in ATTR2 ('MAD H7.xyz, H7, 2, -1' then NRM), so a rule that accepted
	// ATTR2 would replay a normal map as a colour.
	//
	// PRE-REGISTERED REFUTATION: if surfaces go visibly wrong-coloured rather than merely darker,
	// this is round 8's RETRYUNSUP repeating and the knob defaults off - one line.
	bool fp_vcol_deep_enabled();

	// RPCS3_REMIX_FPVCOLDEEPSCALE=1 (default 1). Folds fp_fingerprint::out_vcol_scale - the
	// measured fragment-side constant, 2.0 on every Haze program read - into the replayed vertex
	// colour's rgb. Remix's Modulate factor is an 8-bit unorm, so a factor above 1 cannot be
	// represented and the fold saturates above 0.5; that is still the faithful reading, because the
	// x2 convention means 0.5 is the unlit-neutral value.
	//
	// MIND THE DIRECTION. The submitted factor is min(1, COL0 x 2) at 1 and plain COL0 at 0, and
	// COL0 <= 1, so 1 is the BRIGHTER setting and 0 is the DARKER one - and BOTH can only darken
	// relative to the no-modulate factor of 1.0 a program gets today. Set to 0 for surfaces that
	// come out still white / washed out, or that show flat white patches where the x2 clips. There
	// is no brighter setting: at 1 the ceiling is the 8-bit unorm Modulate factor itself.
	bool fp_vcol_deep_scale_enabled();

	// RPCS3_REMIX_FPCONSTALBEDO=1 (default 0 = OFF, byte-identical to today). ROUND 62.
	//
	// For a draw whose fragment program's terminal writer of COL0.rgb is an unconditional
	// 'MOV col0.xyz, c[]' (fp_fingerprint::out_rgb_const), state that literal as the surface colour
	// through the instance blend extension: textureColorArg1Source = TFactor, SelectArg1, tFactor =
	// the literal, linearised to match what the runtime does with the material's texture format.
	// Applies whether the draw carries a guest material - the texture then only ever fed alpha, it
	// is a mask, and its alpha channel keeps doing exactly that - or round 6's grey (an untextured
	// draw of a flat-painted program). No new material, no mesh-key change.
	// Counter: fpconst_applied (draws) on 'Remix live:'. Census: one 'Remix fpconst:' per program.
	//
	// Tri-state parse copied from ui_fast_raster_mode(): unset = default, an explicit 0 is a real 0.
	u32 fp_const_albedo_mode();

	fp_fingerprint scan_fragment_program(const void* ucode, u32 ucode_length, bool fp32_outputs);

	// Compact disassembly of the instructions that feed HPOS. This is the diagnostic that
	// says why a program came back 'unknown'.
	std::string describe_position_slice(const RSXVertexProgram& vp, u32 max_instructions = 32);

	// The same disassembly for any vertex-program output register. TEX0..TEX7 are o7..o14 (the
	// mapping rpcs3's own backends use, VKVertexProgram.cpp:292-299).
	//
	// This is the instrument for the class of defect match_wdivide belongs to. The backend submits
	// the raw vertex attribute and never runs the ucode, so every operation the program applies on
	// the way to an output is silently dropped: for the position that was the w dequantisation
	// divide (e9a7956), and for a texcoord it would be any scale/bias - including a negated V,
	// which is what R2's vertically mirrored textures look like. Reading the slice is how that
	// stops being a guess.
	std::string describe_output_slice(const RSXVertexProgram& vp, u32 output_index, u32 max_instructions = 32);

	// ---------------------------------------------------------------------------------------
	// Slot -> matrix conversion
	// ---------------------------------------------------------------------------------------

	// Reads c[base .. base+3] out of rsx::method_registers.transform_constants.
	// Returns false when the block would run past the 468 legal slots.
	bool read_slot_block(u32 base, slot_block& out);

	// A single transform constant. Needed near the top of the range, where a 4-slot read
	// would run past the 468 legal slots.
	bool read_slot(u32 index, f32 (&out)[4]);

	// Slots to the row-vector matrix the rest of this file works in.
	mat4 slots_to_matrix(const slot_block& slots, chain_shape shape);

	// Group i of the fingerprint as a row-vector matrix: read_slot_block + slots_to_matrix, plus
	// the (0,0,0,1) row a 3-row group leaves implicit. Every reader of group_base goes through
	// this - reading the block directly takes the slot above the group as its fourth row.
	// False when the block is out of range, or when a 3-row group's homogeneous w is not 1: the
	// substituted row is only the right one if the ucode's constant w really is the homogeneous 1
	// (the check build_palette_matrix already makes for the same reason).
	bool read_group_matrix(const vp_fingerprint& fp, u32 group, mat4& out);

	// The 'pos * s + b' step described by the fingerprint, as a row-vector affine matrix.
	bool build_prescale(const vp_fingerprint& fp, mat4& out);

	// The HPOS.xy-only transform described by has_ortho2d, as a row-vector matrix with an
	// identity z/w. Reads the two slots live, so it is per draw like build_prescale.
	bool build_ortho2d(const vp_fingerprint& fp, mat4& out);

	// Runs the recorded bone-index op chain over one vertex's attribute component and
	// truncates like ARL does, yielding the palette slot offset 'a'. False when a constant is
	// out of range or the result is not a usable non-negative offset.
	bool evaluate_bone_offset(const vp_fingerprint& fp, f32 value, u32& out);

	// vp_fingerprint::palette_w_slot for "the group supplies all four rows itself".
	inline constexpr u32 s_no_palette_w = 0xffffffff;

	// The same walk, resolved all the way to the absolute constant slot the ucode reads,
	// palette_base + a. Split from evaluate_bone_offset because 'a' is only non-negative by
	// convention: Resistance 2's index attribute is a signed 16-bit integer (type 5, s32k) and its
	// dumps carry ranges like w=[-19..19], so the *slot* rather than the offset is the quantity
	// that has to land inside the 468 legal constants. evaluate_bone_offset is left exactly as it
	// was so the pre-existing path's refusals do not move.
	bool evaluate_palette_slot(const vp_fingerprint& fp, f32 value, u32& out_slot);

	// The same walk for one bone of a weighted blend. The palette geometry (base, rows) is shared
	// by every bone - they are entries of one palette - so only the attribute -> address-register
	// path differs, and that is what 'chain' carries.
	bool evaluate_palette_slot(const vp_fingerprint& fp, const bone_index_chain& chain, f32 value, u32& out_slot);

	// The object-to-world matrix the palette holds at 'slot', as a row-vector matrix: the rows the
	// group actually supplies, an implicit (0,0,0,1) for any it does not, and the constant
	// translation the ucode adds after it folded into row 3. Reads the slots live, so it is per
	// draw like build_prescale. False when a slot is out of range, a value is not finite, or - for
	// a 3-row group - the homogeneous w the ucode moves into the operand is not 1, which would mean
	// the rows are not being applied to a point at all.
	bool build_palette_matrix(const vp_fingerprint& fp, u32 slot, mat4& out);

	// Row-vector affine matrix to remixapi_Transform (column-vector, translation in column 3).
	remixapi_Transform to_remix_transform(const mat4& m);

	// Row-vector matrix into remixapi_CameraInfo.view / .projection (also row-vector).
	void to_camera_matrix(const mat4& m, f32 (&out)[4][4]);

	// depth = (z/w)*scale_z + offset_z, folded into the projection's z output column so the
	// matrix handed to Remix produces D3D-style [0,1] depth whatever convention the title used.
	mat4 fold_viewport_z(const mat4& p, f32 scale_z, f32 offset_z);

	// ---------------------------------------------------------------------------------------
	// Classifiers (ports of the local dxvk-remix-DX11 / camera-proxy gates)
	// ---------------------------------------------------------------------------------------

	// 0 = not perspective, 1 = already row-vector, 2 = looks like the transpose.
	int classify_perspective(const mat4& m);

	// m[3][3] ~= 1 with no perspective column.
	bool is_orthographic(const mat4& m);

	struct projection_params
	{
		f32 fov_y_degrees = 0.f;
		f32 aspect = 0.f;
		f32 near_plane = 0.f;
	};

	bool describe_projection(const mat4& m, projection_params& out);

	// Aspect-vs-viewport main camera pick. Higher is better; <= 0 means reject.
	f32 score_perspective(const mat4& m, f32 reference_aspect);

	// Perspective row (column 3) is 0,0,0,1 - i.e. the matrix is a plain affine transform.
	bool is_affine(const mat4& m, f32 tol = 1e-3f);

	// The 3x3 basis spans three dimensions: no axis is degenerate and the three are not coplanar.
	//
	// is_affine is not this test and never was - it only looks at the perspective column, so it
	// happily accepts a matrix with no rank at all. Resistance 2 handed one straight through: bone
	// c30, basis rows [0 0 0.97707], [0 0 -0.040698], [0 0 0.20897], all three parallel to Z,
	// determinant zero, affine=1. Every vertex through it lands on a line, which is the "long thin
	// diagonal box" a character rendered as. That is the exploded-rather-than-missing case the
	// skinning gate exists to prevent, so it is refused and counted instead.
	//
	// Scale-invariant by construction: the determinant is compared against the product of the three
	// axis lengths, so a legitimately tiny uniform scale passes and a flattened basis does not.
	bool has_usable_basis(const mat4& m, f32 tol = 1e-3f);

	// Longest of the three basis axes, and the length of the translation. Magnitude rather than
	// shape: has_usable_basis is scale-invariant by design (it answers "does this span three
	// dimensions"), so neither it nor is_affine can tell a pose from a transform that inflates the
	// mesh. These are what the per-draw sibling comparison in build_blend_skinning measures.
	// Non-finite components come back as infinity so the caller's own finite test refuses them.
	f32 basis_extent(const mat4& m);
	f32 translation_extent(const mat4& m);

	// ---------------------------------------------------------------------------------------
	// Archetype B: split a fused view-projection into a view and a projection by unprojecting
	// NDC points. Port of dxvk-remix-mirrorsedge's d3d9_rtx.cpp:3910.
	// ---------------------------------------------------------------------------------------
	struct vp_split
	{
		mat4 view = mat4_identity();
		mat4 projection = mat4_identity();
		f32 l1_error = 0.f;
		bool used_transpose = false;
	};

	bool split_view_projection(const mat4& fused, vp_split& out);

	// Debug helper: "[a b c d | e f g h | ...]"
	std::string format_matrix(const mat4& m);
	std::string format_slots(const slot_block& s);

	// Env toggles, GetEnvironmentVariableW based like hardcoded_far_plane().
	// RPCS3_REMIX_DUMP=1     one log line per unique vertex program.
	// RPCS3_REMIX_KEEP_UI=1  disable the screen-space skip (bisection).
	// RPCS3_REMIX_NOCAM=1    force the milestone-1 hardcoded camera + debug triangle.
	// RPCS3_REMIX_NOSKIN=1   skip every skinned draw instead of submitting bone transforms.
	bool dump_enabled();
	bool keep_ui_enabled();
	bool nocam_enabled();

	// RPCS3_REMIX_POSINPUT (round 50), DEFAULT 1.
	//
	// Lets match_const_affine's MUL arm accept a position decoded from an attribute other than
	// ATTR0, record which one on vp_fingerprint::position_input, and have the single decode site
	// that fills m_scratch_vertices read that attribute instead of ATTR0.
	//
	// Set to 0 to restore the pre-round-50 'mul-not-attr0' refusal exactly. That is the A/B: on a
	// title whose position really is ATTR0 the knob changes nothing (the field is 0 either way),
	// and on Eat Lead (BLUS30267) it is the difference between the world and a 0..254-unit cloud
	// of byte-quantised normals.
	//
	// env_u32(...,1) != 0, not env_flag: a default-on knob has to be able to express "=0".
	bool position_input_enabled();

	// RPCS3_REMIX_SKINSPAN: how far either side of palette_base a bone index may land before the
	// draw is refused. 0 = measure only, which is the default.
	u32 skin_index_span();
	bool noskin_enabled();

	// RPCS3_REMIX_CAMHOLD=<frames>: how many consecutive flips the last resolved camera is kept
	// when a frame produces no candidate of its own. 0 restores the unconditional per-frame latch
	// that shipped up to and including 4d62620, where a single candidate-less frame dropped the
	// camera and the backend submitted its origin fallback instead - the "camera gets lost when I
	// look at the sun" symptom, measured at 33.9% of R2's frames. Default 300 (~5 s at 60 fps).
	u32 camera_hold_frames();

	// RPCS3_REMIX_MESHIDLE=<frames>: how many frames a mesh handle survives after the last draw
	// that referenced it, before reap_idle_meshes destroys it. Default 300 (~5 s at 60 fps), which
	// on a title whose level geometry leaves and re-enters view over a corridor's length pays the
	// BLAS build again every round trip; raising it trades residency for that churn. Config
	// "Mesh Idle Frames"; the ceiling side is the separate Live Mesh Cap LRU, which this does not
	// touch. 0 reaps a mesh the first frame it goes unreferenced, so umax is the unset sentinel.
	u32 mesh_idle_frames();

	// RPCS3_REMIX_DRAWNOWORLD=1: submit a draw whose world transform could not be resolved at
	// the identity anyway - the behaviour up to and including 41d9adc. Off by default because
	// identity means "raw model-space vertices at the world origin", i.e. every unresolved draw
	// piled on top of every other one: the vertex explosion. Same principle as the skinning
	// gate - the worst case is a missing object, never an exploding one. Kept as a knob so the
	// change is bisectable against every capture taken before it.
	bool draw_without_world();

	// RPCS3_REMIX_NOWDIV=1: submit the stored ATTR0.xyz even for programs whose ucode divides the
	// position by ATTR0.w - the behaviour up to and including 4b8e925. The bisect knob for the
	// per-vertex divide: with it set, the affected meshes go back to being blown apart.
	bool nowdivide_enabled();

	// RPCS3_REMIX_RTVERTS=<n>: vertex ceiling for the 3D render-target-feedback gate. A draw that
	// samples a bound colour/depth surface is only treated as a post-process pass when it is also
	// small enough to be a full-screen quad - otherwise shadow-mapped and probe-lit world
	// geometry, which legitimately samples render targets, would be deleted. 0 disables the shape
	// test and refuses on the address alone. Default 32.
	u32 rt_feedback_max_vertices();

	// RPCS3_REMIX_STRICTINPUT=1: refuse any draw whose position chain never reached the vertex
	// attribute (the 'fused(innermost operand is not an attribute)' population). Those groups are
	// given a world transform even though the space their innermost operand lives in was never
	// proven, so if geometry is still exploding after the no-world refusal, this knob is the one
	// run that says whether they are the cause. Diagnostic only - off by default.
	bool strict_input_enabled();

	// RPCS3_REMIX_NOUV=1: submit world geometry with texcoord (0,0) on every vertex - the
	// behaviour up to and including e9a7956, where a bound albedo material produced one flat
	// colour per draw because every pixel sampled the same texel. The bisect knob for the UV
	// fetch.
	bool texcoords_disabled();

	// RPCS3_REMIX_UVATTR=<n>: force the vertex attribute the albedo texcoords are read from,
	// instead of the 8+unit convention with its fallback scan. 0 (default) leaves the automatic
	// choice in place. Accepts 1..15 when forced, not just the 8..15 the automatic scan walks:
	// a title whose texcoords sit on a low attribute could not be tested at all before, because
	// apply_texcoords rejected the forced index on the same bound the scan uses. 0 stays refused -
	// that is the position attribute.
	u32 texcoord_attribute();

	// RPCS3_REMIX_UVUCODE=0: ignore vp_fingerprint::texcoord_input and pick the texcoord attribute
	// with the size/type heuristic alone - the behaviour that shipped up to and including fdada2e,
	// where the scan walked {8..15} then {1,2,4,5,6,7}, accepted the first 2-component stream and
	// took the lowest index. That heuristic guesses at something the ucode states outright, and it
	// guesses wrong. Replaying resolve_output_input over Resistance 2's (NPEA00431) 70 TEX0-writing
	// programs resolves 67 of them, picking attribute 1 for 54, attribute 3 for 11 (among them
	// a3af6e3d5f0ac8e6, 92e7472e1c0f58cd, ccfc2dd606adf833, 731962646a64e3a4, 1438eb79c0843fea,
	// 7a4a57869f9c4a1f, c0aeb056121c7061 and 8496b26338eb1e01, all 'MOV>o7.xy(I0.xyxx,...)c0i3')
	// and attribute 2 for 2. The widened scan excludes attribute 3 outright as RSX's diffuse colour
	// register, so those 13 programs got attribute 1 - a different UV set - or nothing at all. On by
	// default; the bisect knob for reading the attribute out of the program.
	bool texcoord_from_ucode();

	// RPCS3_REMIX_ORTHO2D=0: ignore vp_fingerprint::has_ortho2d, so a 2D program whose transform
	// writes only HPOS.xy has that transform dropped and its raw attribute values composited as if
	// already projected - the behaviour up to and including 9c73eb0. Measured on Resistance 2
	// (NPEA00431): its menu backdrop fc5915fd48d91a98 reports arch=screen_space with consts=3 and a
	// raw attribute bbox of [-1.064 -0.02847]..[1.064 0.5107], which passes the extent <= 1.5 NDC
	// test and so composited straight, mirrored, at half height. On by default; the bisect knob for
	// applying a 2-row transform in the compositor.
	// RPCS3_REMIX_PASSSKIP=0: submit every pass of a multi-pass forward renderer, the behaviour up
	// to and including 9c73eb0. Resistance 2 (NPEA00431) draws each surface once to establish it
	// and again to light it; the lighting pass writes no depth and samples only the normal map on
	// unit 1. Blended on console, stacked on a path tracer - so the world rendered as its normal
	// maps. Measured over 665 dumped draws: 244 base (depth_write=1 blend=0) against 346 re-draws
	// (depth_test=1 depth_write=0). Set 0 to bisect a title where the skip removes real content.
	bool pass_skip_enabled();

	bool ortho2d_enabled();

	// RPCS3_REMIX_FULLCHAIN=0: under a *fused* active camera, fold only the outermost group of a
	// layered program's matrix chain and divide that by the reference inverse - the behaviour up to
	// and including 9c73eb0. For a layered program the outermost group is the projection alone, so
	// that dropped every inner group carrying the model and view rows and submitted
	// attr * G(n-1) * ref^-1 in place of attr * G0 * ... * G(n-1) * ref^-1. The result is geometry
	// placed by a transform that tracks the camera only partially, which is the shape of a HUD that
	// slides across the visor as the camera pitches instead of staying pinned to it. On by default;
	// the bisect knob for the full-chain fold, and the A/B that has to bring the drift back for that
	// hypothesis to hold. world_layered_ref in the stats line counts how wide the path fires.
	bool full_chain_enabled();

	// RPCS3_REMIX_HPOSINDIRECT=0: collect the writers of HPOS literally and require each of the
	// four to be a DP4/DPH writing one component itself - the behaviour up to and including
	// 9c73eb0, which discards any 4x4 whose rows did not all land on HPOS directly. Resistance 2
	// (NPEA00431) dumped 129 unique vertex programs, 46 of them arch=unknown "no matrix chain into
	// HPOS"; splitting those 46 by how they write o0 gives 29 x (DP4 x,y,z + MOV w), 11 x
	// MOV>o0.xyzw, 5 x ADD>o0.xyzw and 1 mixed. The 29 are a plain 4x4 over four consecutive slots
	// whose w row was computed into a temp first, and 4 of the 11 forward a whole MAD chain out of
	// a temp that an unrelated co-issued RCP had already disqualified. On by default; the bisect
	// knob for reaching a writer through the register it was parked in. vp_hpos_indirect and
	// vp_hpos_refused in the stats line count programs, not draws.
	bool hpos_indirect_enabled();

	// RPCS3_REMIX_FPALBEDO=0: ignore fp_fingerprint::colour_mask and pick the albedo unit the way
	// 9c73eb0 did - the lowest referenced, enabled 2D unit - with the retry loop free to walk to any
	// other referenced unit when the cache refuses that one. That walk is what puts a normal map in
	// the albedo slot. Measured on Resistance 2 (NPEA00431), 1477 texture binds in bin\remix_dump.log:
	// unit 0 takes 666 DXT1 (fmt=86), 240 DXT45 (fmt=88) and 5 A4R4G4B4, unit 1 takes 566 DXT45 and
	// *zero* DXT1. Matched pairs pin the layout - C813D51BD4E562B5 DXT1 128x128 on unit 0 against
	// 9C6C098778ACE6FA DXT45 128x128 on unit 1, same dimensions, adjacent guest offsets - so unit 0 is
	// the diffuse map (DXT1 opaque, DXT45 where the cutout needs alpha, which is the foliage that
	// already renders correctly) and unit 1 is the normal map. Tangent-space normals are ~(128,128,255),
	// which is exactly the flat blue the dev-menu texture grid shows bound as albedo today. On by
	// default; the bisect knob for reading the colour source out of the fragment program.
	bool fp_albedo_enabled();

	// RPCS3_REMIX_FPALBEDONARROW=0: never consult fp_fingerprint::narrow_sample_mask, restoring
	// the round-42 albedo election bit-exactly. Only read where colour_mask already saturated,
	// and only where dropping the narrow units changes which unit is elected.
	bool fp_albedo_narrow_enabled();

	// RPCS3_REMIX_SKYLEARN: admit the narrower latitude bands of a dome whose vertex program has
	// already produced a sky-tagged draw. A tessellated dome is a stack of bands and only the
	// widest clears sky_min_extent(); Haze's radius-5000 dome measures 10000 / 1558 / 797 across
	// three of them, so lowering the floor only moves the cut instead of closing it. Guarded to
	// draws that come from an armed program, write no depth, resolve no material, and failed on
	// extent alone. On by default; `0` restores extent-only. Counter: sky_learned_ring.
	// RPCS3_REMIX_UVSCALEUCODE: take the S32K texcoord divisor from the constant slot the vertex
	// program actually multiplies by (vp_fingerprint::texcoord_scale_slot) instead of the fixed
	// UVINTSCALE. Haze keeps it in c151 and it is not one value across programs, so no fixed
	// divisor can be right for all of them. Falls back to UVINTSCALE wherever the ucode names no
	// scale. On by default; `0` restores the fixed divisor. Counter: uv_scale_ucode.
	bool texcoord_scale_from_ucode();

	// RPCS3_REMIX_UVSCALETEMP: also accept the scale when the program multiplies a *temp* by the
	// constant rather than the attribute itself, i.e. after a MOV of the attribute into a register.
	// The strict form only matched `MUL rN.xy(I0.xyxx, C0.xxxx)` and every program that staged the
	// attribute first fell through to the fixed divisor - the population uv_scale_fixed counts.
	// Only consulted when the strict form finds nothing, and still refuses unless the slice names
	// exactly one constant slot and exactly one attribute. On by default; `0` restores strict-only,
	// which is the A/B against uv_scale_ucode / uv_scale_fixed.
	bool texcoord_scale_temp_form();

	// RPCS3_REMIX_UVSCALEMAD: also accept the scale when it is stated as a MAD - scale and bias in
	// one instruction - rather than a bare MUL. Only src0*src1 is examined; src2 is the bias and is
	// usually a constant too, so scanning it would make every MAD ambiguous. The bias is not
	// applied, only the scale: a bias shifts the coordinate, a wrong scale tiles the texture.
	// On by default; `0` restores MUL-only. Read the effect on uv_scale_fixed / uv_scale_ucode.
	bool texcoord_scale_mad_form();

	// RPCS3_REMIX_UVSCALEHOP: in resolve_texcoord_affine, follow a MOV hop out of the MUL whose
	// scaled operand is a TEMP instead of the attribute - the exit that used to refuse with
	// "affine:mul-temp-pair". This is the DOMINANT world shape on Eat Lead (BLUS30267): 595,023 of
	// its 807,360 fixed-divisor draws leave through the scalar matcher's two_attributes exit, and
	// they leave through it because the scalar slice of o7 is lane-blind and sees both v4 and v5,
	// while the pair that actually carries (u, v) is fed by v4 alone. Replayed from the stored
	// ucode (bin\remix_ucode\, decoded with rpcs3's own D0..D3/SRC bitfields):
	//
	//   70931FA705186608.vp (the 9,964-vertex world mesh)   1B20D02263AA8EE4.vp (prop family)
	//     4: VEC MOV r3.zw <- v5.xxxy                         4: VEC MOV r2.zw <- v5.xxxy
	//     5: VEC MOV r3.xy <- v4.xyxx                         5: VEC MOV r2.xy <- v4.xyxx
	//     6: VEC MUL o7.xyzw <- r3.xyzw, c463.xxxx            6: VEC MUL o7.xyzw <- r2.xyzw, c464.xxxx
	//
	// with both constants live at [0.00048828125 0.00048828125 1 1] = 1/2048 in every one of the 28
	// 'Remix uvrange:' rows that print them, against the 4096 UVINTSCALE hardcodes. The hop is
	// MOV-only and stops at the attribute; a hop that lands on a second MUL(attribute, constant) is
	// the two-scale family and is REFUSED by name ("affine:mul-two-scales"), never silently reduced
	// to one of its two scales. On by default; `0` restores the "affine:mul-temp-pair" refusal and
	// the fixed divisor in one relaunch. Counters: uv_affine_hop, uv_affine_two_scales.
	bool uv_scale_hop_enabled();

	// RPCS3_REMIX_FPLERPUNIT=1 (DEFAULT 0 = OFF, round-57 election bit-exactly): let
	// albedo_unit_mask() drop the MINOR unit of a two-texture lerp the fragment program states
	// outright. See fp_fingerprint::lerp_valid for the four decoded programs and
	// scan_fragment_program for the detector. The rule fires only when
	//   * the ucode resolved a lerp (lerp_valid) with weight > 0.5,
	//   * BOTH ends are in the candidate mask, and
	//   * dropping the minor end actually changes which unit would be elected
	// - the same containment test the narrow rule uses, for the same reason: 175 of the 178
	// programs that rule narrows do not change their election, and confining the retry walk on
	// those would have been a regression with no upside. MEASURED offline over all 726 .fp files
	// in bin\remix_ucode\ (docs\remix\fpelect.py): 31 programs match the shape, 23 change their
	// election, all 23 move unit 0 -> unit 1 and none moves above unit 1. Weight distribution is
	// bimodal - 0.20..0.49 stay, 0.60..1.20 move - so no program sits on the 0.5 boundary.
	// Leaves unit_from_ucode false, so the retry policy is untouched. Counter: tex_albedo_lerp.
	bool fp_lerp_unit_enabled();

	// RPCS3_REMIX_FORCEALBEDOVP / FORCEALBEDOFP / FORCEALBEDOUNIT: one title-profiled
	// albedo election for a measured program pair whose fragment arithmetic is not yet covered by
	// the generic colour walker. Both hashes must be non-zero and the requested unit must be in the
	// draw's eligible mask; otherwise this is inert. FORCEALBEDOUNIT defaults to 16 (disabled).
	u64 force_albedo_vp_hash();
	u64 force_albedo_fp_hash();
	u32 force_albedo_unit();

	// RPCS3_REMIX_UVLANES=1 (DEFAULT 0 = OFF): the same fact stated from the VERTEX side. Eat
	// Lead's interior programs write one UV pair into o[7].xy at the 1/2048 dequant and the SAME
	// pair into o[7].zw multiplied again by a second constant (live c463 = 0.1), i.e. one repeat
	// per ten world units - a MACRO map's coordinates. Two things follow, both behind this knob:
	//   * apply_texcoords() replays the .zw form (vp_fingerprint::texcoord_affine_zw) for a unit
	//     whose TEX samples the .zw lanes, instead of applying the .xy form's divisor to it -
	//     which is the ten-times-too-dense tiling. Counters: uv_lane_zw, uv_lane_refused.
	//   * albedo_unit_mask() drops units sampled through the MACRO lane pair when the mask still
	//     holds a wide colour unit sampled through the other pair, never demoting the last wide
	//     unit and containment-tested exactly like the lerp rule. Counter: tex_albedo_lane.
	// The macro pair is decided per TEXn output by vp_fingerprint::texcoord_macro_lanes, which
	// keys on the SCALE form only: two pairs that differ merely in which attribute they read
	// (1B20D02263AA8EE4.vp, 70931FA705186608.vp) do NOT qualify.
	bool uv_lanes_enabled();

	// RPCS3_REMIX_UVSCALE2=1 (DEFAULT 0 = OFF): resolve the two-scale family instead of refusing
	// it. reduce_moved_attribute()'s MUL arm currently returns "affine:mul-two-scales" because
	// uv_affine_form carried ONE scale slot; with this on it hands back the second one in
	// scale2_slot/scale2_component and apply_texcoords() multiplies by both, in the order the
	// ucode does. Replayed from bin\remix_ucode\19E83AA14ADE8DF5.vp:
	//    2: MUL r1.xy   <- v5.xyxx, c[464].xxxx
	//   11: MUL r1.xyzw <- r1.xyxy, c[463].zzxy
	//   13: MOV o[8](TEX1).xy <- r1.xyxx        with live c463 = [0.265 0.25 0.5 0]
	// i.e. TEX1.xy = v5 * c464.x * c463.z. That unit is read today from the heuristic attribute 1
	// with the fixed 1/4096 - the right magnitude by coincidence (0.5/2048 = 1/4096) from the
	// wrong attribute. Both constants are read live per draw; nothing is baked. Counter:
	// uv_affine_scale2. two_scales stays set on the form so the existing census keeps its meaning.
	bool uv_scale2_enabled();

	// RPCS3_REMIX_PICK: number every submitted instance and answer Ctrl+Click in the game window
	// with the vertex-program hash, albedo hash and sky/viewmodel verdict of whatever is under the
	// cursor, logged at 'Remix: picked'. Exists because Remix's own dev-menu picker is blank for
	// external draws by construction - see the pick_record comment in RemixGSRender.h. On by
	// default; the cost is one pNext struct and one vector push per instance.
	bool pick_enabled();

	// RPCS3_REMIX_SKYCAM: register a REMIXAPI_CAMERA_TYPE_SKY camera each frame, carrying the same
	// matrices as the world camera. Nothing registered one before, so every draw tagged SKY was
	// resolved by submitExternalDraw against an unregistered camera. On by default; `0` restores
	// the world-camera-only behaviour, which is the A/B for the dome placement and the lighting.
	bool sky_camera_enabled();

	// RPCS3_REMIX_AFFINETOL: how much perspective residue a resolved world matrix may carry before
	// per_draw_transform refuses the draw. Default 0.02, which is the value that has always been
	// hardcoded - and which refuses 97.6% of all world refusals, about a third of a Haze scene.
	// Raising it does not make a wrong matrix right: to_remix_transform drops the perspective row
	// outright, so a draw admitted here is flattened, and that is only correct when the residue is
	// small enough to be numerical rather than a second projection. Read against the
	// "Remix affine-residue:" histogram before changing it.
	f32 world_affine_tolerance();

	bool sky_learn_dome_enabled();

	// RPCS3_REMIX_RETRYUNSUP: let the albedo unit walk step past a unit that bind() refused for a
	// permanent format reason, instead of stopping on fp_albedo_enabled's guard. That guard exists
	// to stop a normal map being substituted for a diffuse map, which cannot be what is happening
	// when the unit in hand holds nothing bindable at all. Haze (BLUS30094) puts a 2048x2048
	// DEPTH16 shadow map on the lowest referenced unit of every shadow-receiving draw and its
	// ucode names no albedo unit (tex_albedo_ucode=0), so the guard fired unconditionally and
	// 55,360 draws in one capture reached Remix untextured. On by default; `0` restores the
	// unconditional guard. Counter: tex_retry_unsupported.
	bool retry_unsupported_enabled();

	// RPCS3_REMIX_POSAFFINE=0: ignore vp_fingerprint::has_const_affine and stop refusing draws whose
	// recognised position decode could not be rebuilt - the behaviour up to and including fdada2e,
	// where a program that decodes 'pos = attr * c[S] (+ c[B])' outside a single MAD had the decode
	// silently dropped and submitted its raw quantised attribute. Resistance 2 measured
	// prescale=0(c0.0,c0) and wdiv=0 on every one of its 70 dumped programs while replaying this
	// matcher over their position slices resolves eight, four of which are archetype 'fused' with
	// group_count 1 - i.e. draws that really do reach Remix: ca526d308f1650bb, 2fbe115e8fbc3371,
	// ab9725268da2ac85 and ab9725260da32c85, all 'MUL>r0.xyz(I0.xyzx,C0.xyzx,...)c17i0', a per-axis
	// scale by c17.xyz. Their attr0 is type=5 size=4 (S32K x4) with w=[-16511..-16384], so the raw
	// attribute they were submitted with is quantised integers: geometry at the wrong scale and
	// offset, which is the reported vertex explosion. On by default.
	bool position_affine_enabled();

	// RPCS3_REMIX_UVWIDE=0: restrict the automatic texcoord scan to attributes 8..15 - the
	// behaviour that shipped up to and including fdada2e, where a title that does not follow the
	// 8+unit convention had no automatic path at all. On by default: Resistance 2 (NPEA00431)
	// references no attribute above 4 in any of its 33 census programs and measured
	// uv_absent == uv_none == tex_bound == 722623, i.e. every textured draw in the run failed with
	// "the attribute was never placed" - a total failure the 8..15 scan can never fix. The widened
	// pass adds referenced attributes 1, 2, 4, 5, 6, 7 (never 0/position, never 3/colour, never the
	// skinning attribute) and takes only 2-component streams whose values decode finite; the full
	// eligibility argument is in resolve_texcoord_attribute. The bisect knob for that scan.
	bool texcoord_wide_scan();

	// RPCS3_REMIX_UVFLIPV=1: submit 1-v instead of v for every texcoord, on both the 3D and the
	// 2D path. Off by default, and it must stay off by default: v = 0 means row 0 of the decoded
	// texture on RSX exactly as it does in D3D9, so a global flip would invert every title whose
	// coordinates are already right.
	//
	// This is a per-title knob for a per-title defect. Resistance 2 (NPEA00431) is the first title
	// whose UVs this backend ever applied (they arrived with the widened ATTR1 scan that shipped
	// after fdada2e) and it samples every texture vertically mirrored - its on-screen text reads
	// left to right with every glyph upside down, which is a V inversion and not a mirrored quad.
	// The upload and the V convention are both excluded as the cause by Haze (BLUS30094): its menu
	// text renders correctly through composite_ui_draw, which reads the title's own texcoord
	// attribute, decodes it with the same decode_position, and samples the same texture_entry
	// pixel buffer that reaches Remix's CreateTexture, with v = 0 bound to the top row
	// (RemixCompositor.cpp:41-45). See commit 2414498, "Menu text now reads correctly".
	//
	// What is left is the title's own ucode: we submit the raw vertex attribute and never run the
	// vertex program, so any scale/bias it applies between reading the texcoord attribute and
	// writing it to o[7+n] is lost - the same class of trap as the position w dequantisation that
	// match_wdivide replicates (e9a7956). Which instruction R2 uses has not been read yet; the
	// texcoord slice added to the dump alongside this knob is what will name it, and a knob is
	// what makes the picture usable in the meantime.
	bool texcoord_flip_v();

	// The one place the flip is spelled out, so the 3D and 2D paths cannot disagree about it.
	// Applied after the unnormalise / S32K scaling, i.e. in normalised texture space: mirroring
	// about v = 0.5 there is the correct inverse under both REPEAT and CLAMP, and it keeps a
	// tiled coordinate tiling (v in 0..8 becomes -7..1, the same rows in the opposite order).
	//
	// 'flip' is passed in rather than read here because both callers run this per vertex and
	// texcoord_flip_v() reads a config atomic; the callers hoist it out of their loops.
	inline f32 apply_v_flip(f32 v, bool flip)
	{
		return flip ? (1.f - v) : v;
	}

	// RPCS3_REMIX_UVINTSCALE=<n>: divisor for S32K (raw 16-bit integer) texcoords, whose real
	// divisor is a vertex-program constant this backend does not read. Default 4096, inferred
	// from Haze's own UV ranges - see apply_texcoords. 0 submits them raw.
	u32 texcoord_int_scale();

	// RPCS3_REMIX_LOOSESLICE=1: let the backward slice from HPOS collect writers that sit *after*
	// the instruction whose operand is being traced - the behaviour up to and including e9a7956.
	// RSX vertex programs are straight-line code, so a write at a later instruction cannot reach
	// an earlier read; collecting them anyway made three of Haze's programs report indexed
	// addressing in the position chain when their indexed reads land after the HPOS write. The
	// bisect knob for the ordering constraint.
	bool loose_slice_enabled();

	// RPCS3_REMIX_DRAWINDEXED=1: submit draws whose position chain really does read a constant
	// through an address register instead of refusing them. Diagnostic - the refusal exists
	// because such a draw renders in its bind pose or torn across the map.
	bool draw_indexed_const();

	// Primitive restart decoding is enabled by default; zero disables it for diagnosis.
	bool primitive_restart_enabled();
	// RPCS3_REMIX_VTXNORMAL=0: submit the constant object-space normal (0,0,1) this backend wrote
	// into every vertex before the normal decode existed. 1 (default) decodes the guest's own.
	bool vertex_normals_enabled();

	// RPCS3_REMIX_NORMALATTR=<0..15>: which attribute carries it. Default 2, the RSX convention.
	u32 normal_attribute_index();

	// RPCS3_REMIX_NORMALCENSUS=0: silence 'Remix vtxattr:'.
	bool normal_census_enabled();

	// RPCS3_REMIX_UIUVONCE=0: let the general 2D ucode scale apply on top of the title-specific
	// one instead of standing down. See the definition for why that is a double multiply.
	bool ui_uv_apply_once();

	// RPCS3_REMIX_UVSCALEMOV=0: refuse a texcoord scale that reaches the multiply through a
	// MOV of a constant, which is the round-10 position relaxation applied to texcoords.
	bool texcoord_scale_mov_walk();

	// RPCS3_REMIX_DEMONSLIGHTS=0: do not submit the game's authored LIGHT_BANK lights and leave
	// the sun on the scattering direction. See the definition for why those differ.
	// RPCS3_REMIX_DEMONSSUNSRC: 1 = LIGHT_BANK dir0 (shading), 0 = the scattering sun (sky).
	u32 demons_sun_source();
	// RPCS3_REMIX_DEMONSHEMI=0: submit the authored directional lights but not the hemisphere fill.
	bool demons_hemisphere_enabled();
	// RPCS3_REMIX_VCOLALPHAONLY=0: drop a blended draw's vertex ALPHA whenever its RGB route is
	// unproven, which is what made every such draw arrive opaque.
	bool vcol_alpha_only_enabled();
	bool demons_water_any_target();
	// RPCS3_REMIX_WORLDTINT=0: do not let a WORLD draw take its vertex colour from the texcoord
	// the fragment program modulates by. See the definition for the two-hop resolution.
	bool world_tint_texcoord_enabled();
	bool demons_lights_enabled();
	f32 demons_light_scale();
	f32 demons_light_tolerance();

	bool demons_menu_enabled();
	bool demons_world_enabled();

	// RPCS3_REMIX_INDEXEDWORLD=0: refuse every program that reads a constant palette through the
	// address register, which is what the backend did up to and including ae94587. Measured there
	// on Resistance 2 (NPEA00431), 8965 frames: draws=7588805 submitted=1412430, of which
	// world_applied=1412430 world_fallback=2477524 world_refused=2477524, and the palette gate took
	// skin_unrecognised=1481510 draws with skin_submitted=0 and skin_bones_max=0 - i.e. the gate
	// had never once let a rig through, on any title. vp_hpos_indexed=30 counted the programs whose
	// HPOS chain the register-indirection resolver could follow but was told not to.
	//
	// With it on, three things change together and none of them can be had separately: the
	// indirection is offered to an indexed program, match_indexed_affine can express a 3-row
	// indexed group with a post-matrix translation, and match_mad_chain will accept a partial
	// writemask or a non-unit stride for an indexed group. Off restores all three at once.
	bool indexed_world_enabled();

	// RPCS3_REMIX_INDEXEDBIASREG=1: let match_indexed_affine express an indexed group whose rows are
	// written into one register and moved into another by the post-matrix translation, instead of
	// requiring the translation to be in place. Default off, so the shape stays refused until a run
	// says what it recovers.
	//
	// The counter that justified it is skin_unrecognised, 119357 draws on Resistance 2 (NPEA00431)
	// in a 2m41s capture, every one of them through the indexed-const gate. f56d765aa4ea4cb8 is in
	// that population and is instruction-for-instruction the program 6ee02187fb587944 - same mesh
	// (vtx=174 idx=936, same bbox), same c18.w dequantisation, same c31/c32/c33 rows, same ATTR0.w
	// index, same c18.xyz translation, same c8..c11 outer group - differing only in that its rows
	// land in r0 and the translation moves them to r1:
	//   6ee02187fb587944   8..10:DP4>r1.{zyx} c33/c32/c31[a]   17:ADD>r1.xyz(c18.xyz, r1.xyz)
	//   f56d765aa4ea4cb8   7..9:DP4>r0.{zyx}  c33/c32/c31[a]   16:ADD>r1.xyz(c18.xyz, r0.xyz)
	// The first resolves and draws; the second reports arch=fused "innermost operand is not an
	// attribute" and every draw is refused. Same class as has_ortho2d, hpos_indirect and the ADD
	// src2 operand scan: a structural assumption about how a transform is written rejecting a
	// legitimate one.
	//
	// Nothing downstream is loosened. A program this matches still has to pass the indexing audit,
	// resolve its bone index, and produce a palette entry that is affine with a usable basis before
	// anything is drawn.
	bool indexed_bias_reg_enabled();

	// RPCS3_REMIX_INDEXEDUNIFORM: submit a draw whose indexed constant reads are provably the same
	// constant for every vertex (vp_fingerprint::indexed_addr_uniform) instead of refusing it.
	//   0  off - the behaviour this replaces (default).
	//   1  release only programs whose matrix chain also reaches the vertex attribute.
	//   2  release every program with a uniform address register.
	//
	// The counter that justified it is skin_unrecognised, 119357 draws on Resistance 2 (NPEA00431).
	// Default off because proving the *index* uniform does not prove the *transform*: R2's two
	// programs of this shape (f7576a48e6289f83, ba93cfebcefde22f) build their object-to-world step
	// out of a constant basis - rows c47.xyz, c48.xyz and the cross product of the two, held in a
	// temp - which no matcher here can express, so they come back arch=fused with the chain stopping
	// at the camera group. Mode 1 refuses exactly those; mode 2 draws them at whatever the fused
	// derivation gives, which is the wrong place. See indexed-const refusal in RemixGSRender.cpp.
	u32 indexed_uniform_mode();

	// RPCS3_REMIX_BASISAFFINE=0: stop applying the object placement match_basis_affine recognises,
	// restoring the behaviour where R2's indexed-palette characters reached the outer group with no
	// placement and landed at the camera group's origin - visible as the stalker and the tank
	// floating in the air inside one another. On by default. This is the A/B for that symptom.
	bool basis_affine_enabled();

	// The concrete value of 'a' for this draw. Reads the address constant back out of the transform
	// constants and applies the modifiers in the hardware's order - absolute value, then negate,
	// then truncate toward zero (VertexProgramDecompiler.cpp:151-163 for the modifiers,
	// RSX_VEC_OPCODE_ARL -> ivec4() for the truncate). False when the program's address is not
	// uniform, the constant does not read back, or the offset moves one of the program's indexed
	// reads outside the legal constants - which is what a mis-read ARL looks like from here.
	bool evaluate_uniform_address(const vp_fingerprint& fp, s32& out_offset);

	// RPCS3_REMIX_BONEBLEND=0: refuse every program that blends more than one palette entry per
	// vertex, which is what the backend did up to and including ae94587. Measured there on
	// Resistance 2 (NPEA00431): 16 vertex programs of this exact shape
	//   1438eb79c0843fea 2f310e8120a344b2 333a616a4093f353 487c71da8d277fb0 6090af134d67ea65
	//   6090af134e07aa65 731962646a64e3a4 7a4a57869f9c4a1f 8496b26338eb1e01 88fe4699c66df009
	//   92e7472e1c0f58cd a3af6e3d5f0ac8e6 c0aeb056121c7061 ccfc2dd606adf833 dc56c444e5cc83e7
	//   edb0911a4c3181d6
	// took 541960 draws into skin_unrecognised, because audit_indexing sees a palette read through
	// four components of a0 and (correctly, for a single-bone path) calls three of them
	// foreign_indexed_reads. They are the character rigs; that is why the characters are absent.
	//
	// With it on, three things change together: match_blend_palette can express the three summed
	// accumulators, resolve_bone_index can follow the x3 index scaling the blend programs build by
	// repeated self-addition, and the indexing audit accepts the four matched address components
	// (and only those - a read through a fifth still refuses the program whole). Off restores all
	// three at once, so a run can attribute a regression to this work rather than to the milestone.
	bool bone_blend_enabled();

	// RPCS3_REMIX_VERTEXBLEND=0: refuse the TRANSPOSE of the shape above, restoring round 51's
	// behaviour exactly.
	//
	// match_blend_palette expresses a rig that blends the palette's matrix ROWS and applies the sum
	// once. Eat Lead (BLUS30267) writes the same blend the other way round - it transforms the
	// POINT by each bone (four indexed MAD column chains) and sums the four transformed points -
	// which is mathematically identical and structurally disjoint, and had no matcher at all. Eight
	// programs, every draw of every character:
	//   001e9e3d5495ee54 001e6e3d1495ee54 031ce5a041c06209 031cb5a001c06209
	//   a97036b5f4b84f25 a97006b5b4b84f25 349bacaa433e003c 645fc1a8edc0d28d
	// took 117183 draws into skin_unrec_indexed with skin_submitted = 0 and nothing on screen.
	//
	// With it on, match_vertex_blend runs LAST in find_chain - only on a group every existing arm
	// already refused - and the whole-program read audit accepts, for this family only, any indexed
	// read that addresses a row of the matched palette through a matched address component (these
	// programs rotate the normal, tangent and binormal by the same palette, 36 reads that never
	// touch HPOS). See the header on match_vertex_blend for the replayed ucode.
	bool vertex_blend_enabled();

	// RPCS3_REMIX_BONESCALE=0: submit a blended draw even when one of its bones is orders of
	// magnitude away from the median of the others in the same draw. The gate exists because a
	// 3-row palette reaches Remix with no magnitude check at all - build_palette_matrix writes the
	// perspective column itself, so is_affine inspects this code's own output, and has_usable_basis
	// is scale-invariant by construction. Resistance 2's stalker turret exploded through both with
	// skinblend_bone=0 and bone_degenerate=0. Off is the A/B that puts the explosion back.
	bool bone_scale_gate_enabled();

	// RPCS3_REMIX_BONEUNIFORM=0: refuse a blended draw whose palette entries are all the same
	// matrix, instead of submitting it. Default on (submit), because such a palette blends to
	// exactly that one matrix and the draw is a correct rigid draw - refusing deletes working
	// geometry. Off is the one-run A/B for whether that population is implicated in an artifact:
	// six of Resistance 2's eight blend rigs are in it, reporting eight identical bones at
	// c32..c53 while the two that look like real skeletons report 24 with a genuine spread.
	bool bone_uniform_allowed();

	// RPCS3_REMIX_SKINREACH=<ratio>: how many times the median a skinned draw's furthest
	// vertex-to-its-own-bone distance may be before audit_skin_extent reports it. Default 32, which
	// is far above anything a skin produces (extremities run a few times the median) and far below
	// an explosion. 0 disables the pass. Diagnostic only: nothing is ever refused on it.
	f32 skin_reach_ratio();

	// RPCS3_REMIX_VTXSPREAD=<ratio>: how many times the median a draw's furthest decoded vertex may
	// sit from the draw's own median position before audit_vertex_extent reports it. Default 64 -
	// a coherent object's furthest vertex is a small multiple of its typical one, and a subset that
	// collapsed elsewhere is orders of magnitude out. 0 disables the pass. Diagnostic only.
	f32 vertex_spread_ratio();

	// RPCS3_REMIX_VTXREFUSE=1: drop a draw audit_vertex_extent called geometrically incoherent
	// instead of submitting it. Off by default - it is an A/B, not a fix, until vtx_spread_submitted
	// says the flagged draws reach the scene at all.
	bool vertex_spread_refuse();

	// RPCS3_REMIX_DRAWAUDIT=0: skip audit_vertex_extent AND audit_world_extent entirely.
	//
	// Default 1 (current behaviour, bit-for-bit). This is a PERF knob, and it is the only one on the
	// backend that trades a diagnostic for frames rather than trading correctness. Read 'audit=' on
	// the 'Remix timing:' line first: it is a new round-32 child of 'draw' and it exists to say how
	// big this trade is before anyone takes it.
	//
	// What 0 costs: the vtx_spread census, the streak/wext census, and the wext refusal gate. On Haze
	// the gate is free to lose - wext_refused MEASURED at 0 across the whole round-31 session - but
	// that is a per-title fact, so check wext_refused on the 'Remix live:' line before setting 0
	// anywhere else. VTXREFUSE=1 and DRAWAUDIT=0 together are contradictory; the audit wins nothing
	// because it never runs.
	bool draw_audit_enabled();

	// RPCS3_REMIX_STREAKGATE=<ratio>: how many times the frame's own median world extent a draw's
	// post-transform bounding box may span before audit_world_extent refuses it. 0 keeps measurement
	// and census active but submits the flagged population instead of refusing it.
	//
	// Every audit that existed at 81af315 is scale-free by construction and therefore blind to
	// exactly this: audit_vertex_extent reports max/median over the *decoded* positions, before any
	// matrix; audit_skin_extent reports max/median over 'bones x position', measured against each
	// vertex's own bone origin and before the instance transform. A mesh that is coherent but a
	// thousand times too big passes both with ratio ~1, and at 81af315 both said so - vtx_spread
	// flagged 2036 draws of 1303434 and vtx_spread_submitted was 0 for every one of them, and
	// skin_reach_flagged was 0 outright, while the streaks were on screen.
	//
	// The one number nobody had was the absolute size of 'instance x bones x position'. The
	// sky-census 'wext' comes closest and is not it: it transforms the raw ATTR0 box by the instance
	// matrix only, which for a blended rig omits the bone matrices that carry that draw's whole
	// position decode - so it over-reports every skinned draw by the decode factor and cannot be read
	// as a world size for them.
	//
	// Default 128 against the frame's own median, so the test needs to know nothing about the title's
	// units and moves with the scene. Provisional: the wext_drawn histogram in the stats line is what
	// re-tunes it from one run, and it is deliberately far looser than the 64 audit_vertex_extent
	// uses in model space, because a terrain chunk really is tens of times a character.
	f32 streak_extent_ratio();

	// RPCS3_REMIX_STREAKALLOWVP / STREAKALLOWFP: exempt one measured program pair from the
	// world-extent refusal. The render path additionally requires the previous frame's main clip,
	// so a same-pair auxiliary pass is still audited and refused normally. Empty by default.
	u64 streak_allow_vp_hash();
	u64 streak_allow_fp_hash();

	// RPCS3_REMIX_DRAWNEXT=<ratio>: report, once per vertex program, a draw whose world extent
	// exceeds this many times the frame's median *and which was actually drawn*. 0 disables it.
	//
	// This exists because nothing else measures that. The streak census only fires for draws it
	// refuses (ratio > STREAKGATE, default 128); wext_drawn is an anonymous histogram; and
	// audit_vertex_extent runs before the world transform and, measured over one R2 session,
	// reported vtx_spread_submitted = 0 - every draw it flagged was dropped by a later gate. So a
	// mesh that comes apart in world space while staying under 128x the median is invisible to all
	// three, which is exactly the band a character-sized artifact lands in: a 2-unit character
	// spread to 500 units is glaring on screen and clears the gate with room to spare.
	f32 drawn_extent_ratio();

	// RPCS3_REMIX_WBUFFERZ=0: refuse a program that writes HPOS.z as its clip z premultiplied by
	// its clip w, instead of recovering the matrix row that feeds the premultiply. The behaviour up
	// to and including ae94587, where Resistance 2's 3152b710c603e12d - a 34679 x 0 x 32770 plane,
	// so a ground or water surface and nothing else - reported 'no matrix chain into HPOS' and was
	// never drawn, despite all four rows of its 4x4 being present and consecutive at c32..c35.
	bool wbuffer_z_enabled();

	// RPCS3_REMIX_SPLITROWS. Recover a matrix row that reached its HPOS lane through a MOV or a
	// MAD instead of a DP4 (repair_split_rows). Hooked last in find_chain, so 0 restores the
	// previous classification of every program bit for bit.
	bool split_rows_enabled();

	// RPCS3_REMIX_NOALPHA=1: create materials with the alpha state M3 shipped (alphaTestType 7 /
	// always-pass, useDrawCallAlphaState 1) instead of the title's own alpha test. The bisect
	// knob for cutout foliage.
	bool alpha_state_disabled();

	// Skinning bisect knobs. Each one isolates one link of the chain
	// attribute -> index -> palette slot -> matrix -> submitted pose, so a single run answers
	// one question instead of the whole thing being guessed at.
	// RPCS3_REMIX_SKINID=1      submit identity bone transforms, mesh/indices/weights unchanged.
	//                           Geometry rigid and correctly placed => the matrices are the fault.
	// RPCS3_REMIX_SKINBONE=<n>  clamp every dense bone index to n. Mesh rigid => the palette read
	//                           is fine and the index decode is the fault. umax when unset, so 0
	//                           stays a usable value.
	// RPCS3_REMIX_SKINRAW=1     feed the raw (un-scaled) attribute value to evaluate_bone_offset
	//                           instead of the scaled one - M4 deviation D1 under test.
	// RPCS3_REMIX_VTXALPHA=0    keep asserting that surface alpha is the albedo texture's alpha
	//                           channel even when that channel is a constant.
	bool vertex_alpha_enabled();

	bool skinid_enabled();
	u32 skinbone_index();
	bool skinraw_enabled();

	// RPCS3_REMIX_SKIPVP=<16 hex digits>: drop every draw of one vertex program. The one-run
	// bisector for "which program draws that". 0 when unset.
	u64 skip_vp_hash();

	// RPCS3_REMIX_SKIPALBEDO=<16 hex digits>: drop only draws that bind one identified albedo.
	// Unlike SKIPVP this does not remove every other material that shares the same vertex program.
	u64 skip_albedo_hash();

	// RPCS3_REMIX_SKIPPAIRVP + RPCS3_REMIX_SKIPPAIRALBEDO: drop only the draw whose
	// vertex program and albedo both match. This is the safe bisector for a material reused by
	// valid geometry or a program shared by several objects.
	u64 skip_pair_vp_hash();
	u64 skip_pair_albedo_hash();

	// RPCS3_REMIX_SKIPUNTEXTUREDVP=<16 hex digits>: drop only material-less draws from one
	// vertex program, leaving textured draws from the same camera/world family intact.
	u64 skip_untextured_vp_hash();
	u64 skip_untextured_fp_pair_vp_hash();
	u64 skip_untextured_fp_pair_fp_hash();

	// RPCS3_REMIX_SKIPUNBOUNDBLENDVP=<16 hex digits>: drop only blended, non-depth-writing
	// draws that named an albedo unit but failed to bind its material.
	u64 skip_unbound_blend_vp_hash();

	// RPCS3_REMIX_SKIPRTVP=<hex>[,<hex>...]: refuse render-target feedback only for the
	// named programs, including feedback meshes above the normal vertex-count ceiling.
	bool skip_rt_feedback_vp_matches(u64 hash);

	// RPCS3_REMIX_UVAFFINEVP=<16 hex digits>: enable the proven ATTR8-to-TEX3 affine UV
	// slice used by Haze's visor alpha program. Per-hash and unit-0 only, with every constant
	// slot hardcoded, so it can only ever cover the one program it was written against. Kept
	// untouched as a forced override; UVAFFINEALL below is the general form.
	u64 uv_affine_vp_hash();

	// RPCS3_REMIX_UVAFFINEALL=1 (default): replay vp_fingerprint::texcoord_affine - the same
	// 2x2-plus-biases form as UVAFFINEVP, but with the attribute, the scale slot *and component*,
	// the row slots and both bias slots resolved out of each program's own ucode - for every
	// program and unit where the walk resolves it. 0 restores per-hash-only, which is the bisect
	// for a material that changes for the worse; the uvscale-fixed census names every program the
	// walk still refuses, with its reason.
	//
	// Default-on because the population it targets is the one the scalar divisor path measurably
	// gets wrong: 267,355 draws took the fixed 4096 divisor in the last Haze run, and the constant
	// the ucode actually names is 1/32768 on most of those programs - the 8x tiling.
	bool uv_affine_all_enabled();

	// RPCS3_REMIX_UVSCALELANES=1 (default): let the resolved affine form carry one divisor
	// *component* per lane (or per 2x2 row) instead of one scalar for both.
	//
	// Read out of Haze's own ucode, not inferred from a symptom. c151 is a vector of per-attribute
	// divisors: f7f12d5d15bb9c37's TEX0 slice multiplies ATTR8 by c151.x, ATTR9 by c151.y and
	// ATTR10 by c151.z, and 24d1ba819f701e47's live c151 reads [0.00024426 3.05176e-05 0.00024426
	// 3.05176e-05] - two divisors, eight times apart, inside one uploaded vector. A program may
	// therefore give u and v different divisors in a single instruction, and bab9af462da74331 does:
	// 'MUL o7.xy = ATTR8.xyxx * c68.xyxx'. The single-scalar form could not express that, refused
	// with "affine:mul-two-components", and the draw fell back to the fixed 1/4096 on both lanes.
	//
	// Provably inert on every program that already resolves: those name the same component for both
	// lanes, so both readings compute the identical number. It can only ever change draws that are
	// refused today - uv_affine_lanes counts the ones where the two components actually differ, and
	// 0 restores the refusal in one relaunch.
	bool uv_scale_lanes_enabled();

	// RPCS3_REMIX_UVRANGECENSUS=1 (default): name every (program, texcoord output, albedo) whose
	// *submitted* UVs leave [0,1], with the resolved form and the live value of every constant slot
	// it reads. Pure logging. The pick line answers this for the handful of draws the user clicks;
	// this answers it for the whole scene, which is what is needed when the tiling surface is one
	// the user cannot conveniently put a cursor on. 0 removes the lines and the uv_tiled counter.
	bool uv_range_census_enabled();

	// RPCS3_REMIX_NOTEXCENSUS=1 (default): name every program submitted with no albedo material at
	// all - the tex_none population, which on Haze is larger than the textured one. Pure logging.
	// These draws never reach apply_texcoords, so they report uv_attr=-1 on the pick line; that is
	// "never asked", not "UV resolution failed", and the two were indistinguishable until this.
	bool notex_census_enabled();

	// RPCS3_REMIX_SKIPEXTENTVP=<hash> + RPCS3_REMIX_SKIPEXTENTMIN=<world units>:
	// drop only oversized draws from one program after their final submitted extent is measured.
	u64 skip_extent_vp_hash();
	f32 skip_extent_min();

	// RPCS3_REMIX_WORLDVP=<hex>[,<hex>...]: print the world transform for each named vertex program,
	// once per stats window, to RPCS3.log and remix_dump.log. The companion to SKIPVP:
	// SKIPVP names the program by making its draws disappear, WORLDVP then says what transform that
	// program was placed by, which branch of per_draw_transform built it, and - the load-bearing
	// number - the L1 residue of the perspective row that to_remix_transform is about to truncate
	// away. A residue near s_world_affine_tolerance means the draw is being flattened from a genuine
	// projective transform, which drifts with camera pitch instead of snapping. 0 when unset.
	u64 world_vp_hash();
	bool world_vp_matches(u64 hash);

	// RPCS3_REMIX_WORLDIDENTITYVP=<hex>[,<hex>...]: submit named programs' decoded positions
	// directly in world space. Diagnostic for titles that bake static terrain and props into ATTR0.
	u64 world_identity_vp_hash();
	bool world_identity_vp_matches(u64 hash);

	// RPCS3_REMIX_STATICINDEXVP=<hex>[,<hex>...]: accumulate changing triangle subsets for
	// named static-world programs instead of creating a new Remix mesh for every subset.
	// RPCS3_REMIX_STATICINDEXBUDGET limits union-mesh creations to N per presented frame.
	//
	// --- round 31: the budget is not a performance knob, it is a VISIBILITY knob ------------------
	//
	// What a deferral costs is stated at RemixGSRender.cpp:19258-19295 and :19398-19409, and it is
	// not frame time. There are THREE deferral exits with TWO outcomes (an earlier draft of this
	// comment said two exits; the third is on the budgeted-fallback path):
	//   * a previously built union handle is still live -> the draw renders THAT subset, i.e. a
	//     PREVIOUS FRAME'S accumulated TRIANGLE SET. A second, older copy of the surface trailing
	//     the fresh one.
	//     GEOMETRY ONLY. An earlier draft claimed the stale mesh also carried an OLDER MATERIAL.
	//     That is structurally impossible: static_key AND the union hash both fold in vp, albedo
	//     and the material pointer, so a material change lands on a DIFFERENT entry whose mesh_hash
	//     is 0, which makes active_valid false and takes the drop exit instead. A stale union always
	//     shades identically to the draw falling back to it.
	//     WHAT MAKES IT CONVERGE is the GEOMETRY stopping while flips CONTINUE - then nothing sets
	//     static_entry->dirty and the last-known-good union IS the current one. NOT 'when frames
	//     stop': ++m_frame_counter lives inside flip() and the budget resets only when that counter
	//     changes, so if flips stop the budget stays spent and the stale copy FREEZES rather than
	//     converging. An in-game pause menu is the first state, not the second, which is why it
	//     reads as 'aligned' while paused. DO NOT build a round-32 test on 'looks aligned while
	//     paused' without holding that distinction.
	//   * no live handle exists (never built, or reaped by MESHIDLE), or the budgeted fallback finds
	//     its subset mesh absent from the cache -> the draw RETURNS and is never submitted.
	//     Invisible, and invisible WITHOUT A TRACE: both of these exits precede the refusal
	//     accounting, so the surface can never be found in 'Remix world-refused:'.
	//
	// MEASURED, round-30 play-test, 60,211 flips: 'Remix static-index:' read peak=4 budget=4 on ALL
	// 811 lines with deferred=915,419 = 15.2 deferrals/frame against a budget of 4. The ceiling in
	// static_index_rebuild_budget() was ALSO 4, so the knob could only ever be turned down and the
	// saturated case could not be tested; round 31 raised the clamp to 64. The default is unchanged.
	//
	// Cost of relieving it, measured rather than guessed: 'Remix timing:' reports mesh_create at
	// 0.11 ms/frame over 20.02 creates/frame, i.e. 110 us / 20.02 = 5.49 us per create. (A
	// frame-weighted pass over all 840 windows gives 5.64 us; the difference is that the per-window
	// ms field is printed rounded to 0.01. Quote 5.49 with this numerator, or 5.64 with the
	// frame-weighted one - the two do not mix, and an earlier draft divided 0.11 by 20.02 and wrote
	// 5.64, which does not divide.) So ~4 extra creations/frame is ~22 us = 0.08% of the measured
	// 28.14 ms frame. 'stale' and 'dropped' on the static-index line split the deferral population
	// so the trade can be read directly instead of inferred.
	bool static_index_vp_matches(u64 hash);
	u32 static_index_rebuild_budget();

	// RPCS3_REMIX_WORLDIDENTITYPAIRVP=<hash> + RPCS3_REMIX_WORLDIDENTITYPAIRALBEDO=<hash>:
	// submit one exact program/material pair in world space when the program is also shared by
	// genuinely local geometry (for example Haze's quarry building and sky dome).
	u64 world_identity_pair_vp_hash();
	u64 world_identity_pair_albedo_hash();
	u64 world_identity_pair2_vp_hash();
	u64 world_identity_pair2_albedo_hash();
	u64 world_identity_pair3_vp_hash();
	u64 world_identity_pair3_albedo_hash();
	u64 world_identity_pair4_vp_hash();
	u64 world_identity_pair4_albedo_hash();
	u64 world_identity_pair5_vp_hash();
	u64 world_identity_pair5_fp_hash();
	u64 world_identity_pair5_albedo_hash();
	u64 world_identity_fp_pair_vp_hash();
	u64 world_identity_fp_pair_fp_hash();
	u64 world_identity_opaque_fp_pair_vp_hash();
	u64 world_identity_opaque_fp_pair_fp_hash();
	u64 world_identity_opaque_fp_pair2_vp_hash();
	u64 world_identity_opaque_fp_pair2_fp_hash();
	u64 world_identity_opaque_fp_pair3_vp_hash();
	u64 world_identity_opaque_fp_pair3_fp_hash();
	u64 world_identity_opaque_fp_pair4_vp_hash();
	u64 world_identity_opaque_fp_pair4_fp_hash();
	u64 guest_light_pair_vp_hash();
	u64 guest_light_pair_fp_hash();
	u64 guest_light_pair_albedo_hash();

	// RPCS3_REMIX_GUESTLIGHTALBEDO=<hex>[,<hex>...]: every lamp fixture picked in a session
	// becomes a light source on the next launch by editing the launcher, with no rebuild.
	// Bounded at 16 (was 8), same parser as world_identity_vp_matches.
	// guest_light_pair_albedo_hash() above stays as the first entry of the same variable, so a
	// single-hash setting behaves exactly as it did.
	//
	// Round 5: the albedo list is now the *only required* key. GUESTLIGHTVP / GUESTLIGHTFP became
	// optional narrowing - unset (0) means "any program". The configured pair on Haze
	// (830d7d1b9681c475) turned out to be a shared world program that also draws non-fixture
	// geometry, so requiring it excluded every fixture drawn by any other program while adding no
	// selectivity of its own. The albedo content hash IS the fixture identity, the same reasoning
	// the CAT_* lists rest on.
	bool guest_light_albedo_matches(u64 hash);
	bool guest_light_albedo_any();
	u32 guest_light_albedo_count();
	u64 guest_light_pair2_albedo_hash();
	f32 guest_light_radius();
	f32 guest_light_radiance();

	// RPCS3_REMIX_GUESTLIGHTRADIUSSCALE=<float> (default 0.35): sphere radius becomes
	// max(GUESTLIGHTRADIUS, draw extent * this). The fixed 0.2 sphere sat at the bbox centre of a
	// 2.6-unit lamp housing - occluded by its own shade. 0 restores the fixed radius.
	f32 guest_light_radius_scale();

	// RPCS3_REMIX_GUESTLIGHTCOLOR=1 (default): tint the injected light by the mean RGB of the
	// fixture's own decoded albedo, normalised so the largest component is 1 (so the knob changes
	// hue, never total output). 0 restores the fixed warm 1 / 0.86 / 0.68 constant.
	bool guest_light_color_enabled();

	// RPCS3_REMIX_GUESTLIGHTIDLE=<frames> (default 0 = never): destroy a guest light that has not
	// been re-matched by any draw for this many frames and free its slot.
	u32 guest_light_idle_frames();

	// RPCS3_REMIX_GUESTLIGHTMAX=<count> (default 64): live guest-light cap, was hardcoded.
	u32 guest_light_max();

	// RPCS3_REMIX_EMISSIVE=<hex>[:<intensity>][,<hex>[:<intensity>]...] (bound 16)
	// + RPCS3_REMIX_EMISSIVEINTENSITY=<float> (default 1): make the listed albedo content hashes
	// emissive at material creation, so the fixture's own visible surface glows from exactly where
	// the game says the light comes from. This is the *look*; the sphere light above carries the
	// photometric load. Empty list = today's behaviour (emissive fields zeroed).
	//
	// ROUND 30: THE PER-ALBEDO INTENSITY. The user's request was "increase the brightness by like 30
	// for this texture hash, it's the light bulbs in the plant 71D189E9B559A7F9", and until now
	// EMISSIVEINTENSITY was a single global applied to every hash on the list - so raising it for the
	// bulbs would also have blasted the smelting-plant windows and the two other listed surfaces. A
	// bare <hex> keeps using the global and is parsed EXACTLY as before, so a launcher that does not
	// use a colon is bit-identical to round 29. The syntax follows the SUNMAP=<hash>:<x>,<y>,<z>
	// precedent already in this file rather than inventing a second convention.
	//
	// Bounds and failure modes, stated because a silent parse failure here reads as "the knob did
	// nothing": an intensity is accepted only if it parses, is finite and is > 0, and it is clamped
	// to 1e6; a colon with nothing usable after it leaves that hash on the global and skips to the
	// next separator, so one typo cannot swallow the rest of the list. The environment buffer is 512
	// wchars (was 400, raised because 16 entries with intensities can reach 415 characters and the
	// over-long guard refuses the WHOLE list rather than truncating it).
	bool emissive_albedo_matches(u64 hash);
	u32 emissive_albedo_count();
	f32 emissive_intensity();
	// The intensity to create the material for this albedo with: the per-hash value if the list gave
	// one, otherwise emissive_intensity(). Callers do not need to test membership first - a hash that
	// is not listed also gets the global, which is what the one caller in RemixTextures.cpp wants
	// because it has already tested membership.
	f32 emissive_intensity_for(u64 hash);
	// How many list entries carried an explicit ':<intensity>'. On the banner and the live line so a
	// run that was launched with the old single-global form says so in its first ten lines.
	u32 emissive_albedo_intensity_count();

	// --- round 13: the sky dome's own emissive list ------------------------------------------
	// RPCS3_REMIX_SKYEMISSIVE=<hex>[,<hex>...] (bound 8) + RPCS3_REMIX_SKYEMISSIVEINT=<float>
	// (default 2.0). Separate list and separate intensity from RPCS3_REMIX_EMISSIVE above, which
	// is the ceiling-fixture list: a fixture and a sky dome want wildly different numbers and the
	// launcher already ships EMISSIVEINTENSITY=1 for the fixtures. Empty list (the default) =
	// today's behaviour, bit for bit.
	//
	// WHAT THIS IS FOR, measured rather than assumed. Haze's dome is
	//   vp=af06f6d32ec048ee albedo=D1A6D1B27ADE6232 vtx=304 rawext=26352, origin == the eye.
	// It is visible today for one reason only: 0xD1A6D1B27ADE6232 sits in rtx.worldSpaceUiTextures,
	// and the runtime's WorldUI arm force-feeds emission onto any such instance -
	// rtx_instance_manager.cpp:1103-1107 sets enableEmission(true), emissiveIntensity(2.0f) and
	// points the emissive texture at the albedo texture. So the dome is ALREADY emissive; the conf
	// list is what makes it so, and 2.0 is the number the runtime picked.
	//
	// This knob is the same effect expressed per-material, so the dome can keep its glow WITHOUT
	// the conf entry - and, unlike WorldUI's hardcoded 2.0, with an intensity the user can tune.
	// The default is 2.0 for exactly that reason: it reproduces rtx_instance_manager.cpp:1105, so
	// moving the dome off the conf list and onto this knob is a no-op in brightness and any change
	// the user then sees is a change they asked for.
	//
	// NOTE the ordering constraint that follows from the above: while the hash is still in
	// rtx.worldSpaceUiTextures, WorldUI's override runs AFTER material creation and wins, so this
	// knob is inert. It only takes effect once the conf entry is removed. See round14-inbox.md.
	bool sky_emissive_albedo_matches(u64 hash);
	// ROUND 39. The env list ONLY, with the runtime-promoted set deliberately excluded. Any census
	// field labelled 'listed' must use THIS - see its definition.
	bool sky_emissive_listed(u64 hash);
	u32 sky_emissive_albedo_count();
	f32 sky_emissive_intensity();

	// RPCS3_REMIX_SKYEMISSIVEBLEND=1 (default): the listed dome's material also declares
	// BlendType::kEmissive, which is the half that gives the sun back. Without it the dome is an
	// opaque shell and OBJECT_MASK_OPAQUE puts it in the path of every direct shadow ray, so the
	// fallback distant light is blocked by the sky and the only illumination left in the scene is
	// the dome's own emission - exactly the symptom this round exists to remove. 0 keeps the
	// emissive look and the occlusion, which is the A/B that attributes any lighting change to
	// this mechanism rather than to the intensity. Inert while the list is empty.
	bool sky_emissive_blend_enabled();

	// --- ROUND 53: the two knobs that make the GAME'S dome the sky on ANY runtime -------------
	//
	// RPCS3_REMIX_SKYEMISSIVEWEXT=1 (default): exempt a sky-emissive albedo from the world-extent
	// refusal in audit_world_extent(). THE GATE IS WHY THE DOME IS NOT ON SCREEN, measured on Eat
	// Lead (BLUS30267) in bin\remix_dump.log rather than inferred:
	//   Remix skip-census: gate=wext vp=bdc0776911119029 fp=9747c2d02c8abf97
	//     albedo=20703908CDA055F1 extent=1095.06 dist=4.76837e-07 clip=1280x720
	// against a frame median of ~0.149, i.e. ratio ~7330 into a STREAKGATE of 128, with
	// wext_refused reaching 7896 on a single 'Remix live:' line of that dump. The dome passes only
	// in the few frames before a median exists. So what the user has been looking at is the
	// runtime's own sky (rtx.skyMode=1), not the game's.
	//
	// The gate exists to refuse a mesh that arrived UNDECODED - its discriminator is the ratio to
	// the frame's own median, which is exactly the wrong question to ask of a sky: the sky is the
	// one draw in a scene whose extent is SUPPOSED to dwarf the median. A texture the user typed
	// into RPCS3_REMIX_SKYEMISSIVE is by declaration not an undecoded mesh, and audit_world_extent
	// already has the concept - is_sky and R2's visible backdrop take the same early-out, and an
	// exempt draw is left out of the median sample too, so the dome cannot raise the bar for
	// everything else.
	//
	// Keyed on sky_emissive_albedo_matches() (list UNION the classifier's promoted set) rather
	// than sky_emissive_listed(), deliberately: the exemption must follow exactly the predicate
	// that BUILDS the sky material, so a hash that gets an emissive material can never then be
	// refused on extent. The cost of that is stated rather than hidden - if SKYCLASSIFY is ever
	// armed at mode 2, a promoted hash is exempt from the gate too. Tell: skyclassify_promoted>0
	// alongside wext_exempt_sky climbing on a hash that is not in the conf.
	// Counter: wext_exempt_sky - the MARGINAL count, i.e. draws this rule alone exempted, not
	// those is_sky or the R2 backdrop would have exempted anyway. 0 = the rule never fired.
	bool sky_emissive_wext_exempt_enabled();

	// RPCS3_REMIX_SKYTAG=1 (default): may the anchored sky rules actually apply the SKY category,
	// or do they only MEASURE. 0 = measure-only.
	//
	// WHY A TITLE WOULD WANT 0, and it is a portability argument rather than a preference. What a
	// SKY-tagged API draw does is fork-dependent by construction: on an aerial-descended runtime
	// (remix-main+0ff0db1b, deployed here) CameraType::Sky reaches DrawCallState and
	// rtx_instance_manager sets m_isHidden, so the draw is DELETED; on numos3 (6476faea)
	// toRtDrawState clamps Sky->Main and the draw is merely re-cameraed. A title whose sky is made
	// visible by SKYEMISSIVE needs no category at all, so on that title the category is pure
	// fork-dependence with no upside - and on Eat Lead the anchored rule does fire, on the
	// untextured pass of the second dome program (vp=487557313fb93949, albedo=0, wext 3516-4176
	// past SKYEXTENT 2000).
	//
	// ONE gate, applied where is_sky is CONSUMED rather than at each of the four sites that set it
	// (the anchored chain, the learned ring, SKYBACKDROP=2 and SKYHASH=2). That is what keeps it
	// measure-only in the strict sense: every sub-rule's own counter still records what it would
	// have done, m_sky_dome_programs is still keyed on the would-have result so the learned-ring
	// rule stays armed and sky_learned_ring stays meaningful, and 'Remix sky-census:' still prints
	// TAGGED. A reader of that census must know that with skytag=0 on the banner, TAGGED means
	// "would have been tagged". The two things that DO change are the only two consumers: the SKY
	// category bit at submission, and the world-extent exemption is_sky grants.
	// Counter: sky_tag_suppressed - one increment per draw whose tag was withheld, all four sites
	// folded into the one counter. cat_sky must be 0 whenever skytag=0; if it is not, the switch
	// is wired wrong.
	bool sky_tag_enabled();

	// --- ROUND 39: the sky by CLASSIFICATION instead of a hand-written hash whitelist ----------
	//
	// THE DEFECT, stated as a mechanism. A dome renders as a lit sky only if its texture CONTENT
	// hash is on RPCS3_REMIX_SKYEMISSIVE, because that list is what makes its material emissive
	// (RemixTextures.cpp, the `sky_emissive` local in texture_cache::upload). The launcher carries
	// six hashes. haze_domes.csv - mined from the title's own archives - names SIXTEEN distinct
	// dome resources game-wide, so roughly ten of them have no emissive material and render black,
	// and the only way to extend the list is for the user to visit the level and Ctrl+click the
	// sky. MEASURED against bin\remix_dump.log: `mat_skyemissive=0 mat_skyunordered=0` on 14,726
	// stats lines - whole sessions in which the emissive material was never created once - against
	// non-zero values up to 43 on the sessions whose level does have a listed dome.
	//
	// THE ARCHIVES CANNOT CLOSE IT. The mining round recovered the dome NAMES but not their texture
	// hashes: assets are keyed by an unrecovered name hash, and a scan of all 28,278 cached.pak
	// members for every dome name returned zero hits. So the identification must happen at runtime.
	//
	// WHY THIS IS NOT THE EXISTING sky_hash_mode() RULE, AND THE PRE-REGISTERED TEST THAT SAYS SO.
	// sky_hash_mode() already learns "albedo hashes that only ever appear on dome-shaped draws",
	// has shipped at mode 1 (measure) for several rounds, and would have been the obvious thing to
	// promote. IT REJECTS THE TWO DOMES WE HAVE GROUND TRUTH FOR. MEASURED over every
	// 'Remix sky-hash-census:' line in bin\remix_dump.log:
	//
	//     albedo=D1A6D1B27ADE6232  reject:mixed  vp=af06f6d32ec048ee vtx=304 wext=26351.6 upv=86.68
	//     albedo=CDFE11B12552EA2D  reject:mixed  vp=af06f6d32ec048ee vtx=372 wext=27194.1 upv=73.10
	//
	// Both are on the user's own SKYEMISSIVE list, i.e. both are domes the user identified by
	// clicking the sky in game. The rule disqualified them because its units-per-vertex floor is
	// s_sky_backdrop_min_units_per_vertex = 100 and their own latitude bands measure 86.68 and
	// 73.10 - and one non-dome draw disqualifies a hash for the life of the process. A tessellated
	// dome is a stack of bands of DIFFERING density, so the strictest rule is guaranteed to be
	// disqualified by its own coarser bands. That is a structural incompatibility, not a tuning
	// miss, and it is why this is a second rule rather than a mode on the first.
	//
	// THE PREDICATE, and where each threshold's number comes from. Population: every
	// 'Remix sky-census:' line in the whole 969 MB log, parsed to unique rows, filtered to
	// depth_write=0 AND inside=1 AND wext >= sky_min_extent() - 187 unique rows, 23 vertex
	// programs, 46 albedo hashes.
	//
	//   depth_write == 0   A dome writes no depth. Already the sky_candidate gate; kept because it
	//                      is the cheapest of the four and the only one that is a statement about
	//                      the guest's own render state rather than about geometry.
	//   camera INSIDE the transformed AABB, not |translation - eye|. The legacy `anchor` quantity
	//                      collapses to |eye| for an absolute-world draw - round 36 measured
	//                      anchor=2137.85 against limit=4 - so it is dead for exactly the levels
	//                      this round exists to fix. `inside` is scale-free and translation-free.
	//   wext in [sky_min_extent(), sky_classify_max_extent()]
	//                      The CEILING is new and it is load-bearing. MEASURED: the candidate set
	//                      contains a family at wext 1.22e9 .. 1.12e18 (vp 8eae853c96f5a07e,
	//                      f352d7dafa72d0e0, 595b8e2fa69b566b, 5c9cfb394074a479) which cannot be
	//                      geometry - the mined scene_descriptor_template.vms sets farPlane 14000,
	//                      so the entire world fits in ~1.4e4 units. The largest row that carries
	//                      a hash the user hand-listed as a dome is 2.14e6 (35C2353F6B3CE2A8).
	//                      Default 4.0e6 sits in a gap that is 570x wide (2.14e6 -> 1.22e9), which
	//                      is why it is a round number and not a tuned one.
	//                      NOTE the ceiling deliberately does NOT exclude the million-unit family:
	//                      three hashes in it (35C2353F6B3CE2A8, 174F4F689CF2A3D8,
	//                      3213E0CC136ED294) are on the user's hand-written list, so they are
	//                      domes drawn at ~100x the scale of Haze's af06f6d32ec048ee dome. An
	//                      extent ceiling tight enough to be "sensible" would delete them.
	//   vertex_count in [sky_classify_min_vertices(), sky_classify_max_vertices()]
	//                      MEASURED terrain in the candidate set: 2714..12875 vertices (vp
	//                      2c62e34057e58c21, 487c71da8d277fb0, aae8e0d5ae292dd4, 788113cb1bad5321).
	//                      MEASURED domes: 33..747. The CEILING of 1024 sits between, 2.65x below
	//                      the lowest terrain row. The FLOOR of 16 exists because the census named
	//                      one concrete false positive: albedo AC936E2F25F147B0 on
	//                      vp=3c9186d8e026cec5 is a FOUR-VERTEX quad spanning 32,331 units with the
	//                      camera inside it - a full-screen backdrop card, not a dome.
	//   upv >= sky_classify_units_per_vertex()
	//                      MEASURED lowest units-per-vertex on any row carrying a hand-listed dome
	//                      hash, after the extent and vertex gates: 73.09 (CDFE11B12552EA2D).
	//                      MEASURED highest on any row the vertex ceiling excludes: 23.57. Default
	//                      60 is between them, a 3.1x gap.
	//                      BUT STATE THE LIMIT HONESTLY: on this data upv is very nearly REDUNDANT
	//                      with the vertex ceiling - every row the ceiling excludes also has
	//                      upv < 60 - so its independent contribution is small and the separating
	//                      power is really coming from the extent bounds, the vertex bounds and
	//                      `inside`. An earlier draft of this block claimed a 1.34x gap between
	//                      "terrain at 49.28" and "domes at 66.12"; both rows were mis-assigned
	//                      (the 66.12 row carries albedo=0, the 49.28 row is inside the vertex
	//                      gate) and that claim is withdrawn.
	//
	//
	// REPLAYED BEFORE SHIPPING, which is the closest thing to a dry run this rule can have. Applying
	// every gate above to every 'Remix sky-census:' row in the 969 MB log admits 19 distinct albedo
	// hashes with the vertex floor off and 18 with it on. ALL SIX hand-listed dome hashes are among
	// them, which is the ground-truth check passing on historical data before the play-test sees it.
	// The other 12 are candidates, not confirmations - the census is deduplicated per (program,
	// outcome) and carries no draw counts, so it cannot evaluate the 8-draw, no-disqualification,
	// settle-window rule that actually arms a hash. Expect the live number to be LOWER than 18.
	//
	// PER-HASH, NOT PER-DRAW, and that is forced rather than chosen: emissiveness is a property of
	// the MATERIAL, which is per texture, so a texture admitted on one draw glows on all of them.
	// A hash arms when it has been dome-shaped on sky_classify_min_draws() draws, has never been
	// seen on a disqualifying draw, and has been known for at least sky_classify_settle_frames()
	// frames. The settle window is the guard against the one failure this rule can produce that
	// cannot be undone - a material rebuilt emissive cannot be rebuilt back - and it is sized from
	// the sky_hash census's own observation that a shared texture's disqualifying draw arrives
	// within the first second.
	//
	// PRE-REGISTERED, AND THE REFUTATION IS NAMED. haze_domes.csv says 16 distinct dome resources
	// exist, of which only 12 appear outside multiplayer (mp_caves_sky, mp_mcv_sky, mp_pow_sky and
	// mp_shanty_sky are MP-only). A dome RESOURCE may bind more than one texture, so the expected
	// count of armed hashes over a full single-player pass is 8..20 and MUST NOT exceed ~28; above that the rule is
	// over-matching and sky_classify_units_per_vertex() is the knob. Two sharper checks:
	//   1. D1A6D1B27ADE6232 and CDFE11B12552EA2D must ARM. They are ground truth and the existing
	//      sky_hash rule rejects both. If they do not arm, the upv floor is still wrong and this
	//      round has not fixed the thing it claims to.
	//   2. RAVINE HAS NO DOME AUTHORED (haze_domes.csv: six jungle_ravine backgrounds, all
	//      "(no skyModel authored)"). A black sky there is CORRECT. Any hash arming in Ravine is a
	//      false positive, and it is the cheapest place in the game to see one.
	//
	//   RPCS3_REMIX_SKYCLASSIFY=0|1|2|3   default 1
	//     0 off entirely. 1 census only - names what mode 2 would admit and promotes nothing.
	//     QUALIFIED, because the unqualified claim is false: mode 1 mutates no material, no mesh
	//     key and no category, but it is not bit-identical in every configuration. This rule's
	//     gate widens the condition under which submit_subdraw walks the vertex bounding box, so
	//     with SKYHASH=0, SKYBACKDROP=0 and a textured draw that sky_allows_textured() excludes -
	//     a combination the shipped launcher does not use - mode 1 costs a walk that round 38 did
	//     not pay. No pixel changes either way; the cost does.
	//     2 promote. 3 promote and ignore the disqualification (arm on the dome count alone).
	//   RPCS3_REMIX_SKYCLASSIFYMAXEXT=<world units>   default 4.0e6
	//   RPCS3_REMIX_SKYCLASSIFYMINVTX=<n>             default 16
	//   RPCS3_REMIX_SKYCLASSIFYMAXVTX=<n>             default 1024
	//   RPCS3_REMIX_SKYCLASSIFYUPV=<world units>      default 60
	//   RPCS3_REMIX_SKYCLASSIFYMIN=<draws>            default 8
	//   RPCS3_REMIX_SKYCLASSIFYSETTLE=<frames>        default 60
	u32 sky_classify_mode();
	f32 sky_classify_max_extent();
	u32 sky_classify_max_vertices();
	u32 sky_classify_min_vertices();
	f32 sky_classify_units_per_vertex();
	u32 sky_classify_min_draws();
	u32 sky_classify_settle_frames();

	// The learned set itself. sky_emissive_albedo_matches() returns true for anything in here, so
	// promoting a hash is exactly equivalent to having typed it into RPCS3_REMIX_SKYEMISSIVE at
	// launch - same material, same intensity, same blend type, same peak_uv walk, same per-level
	// sun. Monotone: nothing is ever un-promoted, which is what lets the mesh key fold membership
	// in as a stable bit (see RemixGSRender's mesh key build).
	bool sky_emissive_promote(u64 hash);
	bool sky_emissive_promoted(u64 hash);
	bool sky_emissive_unpromote(u64 hash);
	u32 sky_emissive_promoted_count();
	u32 sky_emissive_promote_overflow();

	// RPCS3_REMIX_FPCENSUSVP=<hex>[,<hex>...] (bound 4, default empty = off): census only. Emits
	// one 'Remix fpcandidate:' line per (vp, albedo) the listed program draws, carrying the
	// geometry-to-eye distance that is the only discriminator between the PLAYER's weapon and an
	// NPC's on vp=830d7d1b9681c475 - a program that draws both, and whose instances all sit at the
	// world origin so the instance translation says nothing. Tags nothing, refuses nothing.
	bool fp_census_vp_matches(u64 hash);
	u32 fp_census_vp_count();

	// RPCS3_REMIX_FPCENSUSMAXDIST=<world units>, default 0 = off (round 13's census exactly).
	//
	// Round 17. The fpcandidate census was supposed to name the player's arms and weapon and after
	// four rounds it has not, because it cannot: all 242 of its lines in the log carry
	// vp=830d7d1b9681c475, the single hash on FPCENSUSVP, and the gate returns before any
	// measurement. The same first-person mesh is also drawn by af06f6d32ec048ee and
	// f39f504649b6f442; the latter also draws a 952-vertex mesh whose origin sits 1.82 units below
	// the eye in three runs at three world positions and which the census has never seen.
	//
	// Non-zero: an UNLISTED program may also emit, provided the draw's geometry centre is measured
	// and within this many units of the eye, and the census re-arms every stats window so a
	// (vp, albedo) pair reports repeatedly instead of once per process. FPCENSUSVP-listed programs
	// are exempt from the ceiling, so the tagged visor rows that anchor every comparison always
	// print. Census only: tags nothing, refuses nothing, moves no counter. New line fields:
	// 'listedvp=' and 'maxdist='.
	f32 fp_census_max_distance();

	// RPCS3_REMIX_VMCAMFOVX / RPCS3_REMIX_VMCAMFOVY, both in DEGREES, both default 0 = off.
	//
	// When both are > 0 the VIEW_MODEL camera is submitted with its own horizontal and vertical
	// field of view instead of being an exact copy of the world camera. That distinction is the
	// whole of whether the viewmodel pass can move anything: the runtime's correction matrix is
	//   mainViewToWorld * (mainProjectionToView * vmProjection * scale) * vmWorldToView
	// and with two identical cameras every factor cancels to the identity. Only the projection's
	// XY terms survive into it - the runtime overwrites the depth row from the main camera - so
	// these two knobs are the complete payload and the measured near plane is irrelevant.
	//
	// Measured on Haze's tagged draws by recovering G0: viewmodel fovx 60.001, fovy 36.132,
	// against the world's fovx 72.000, fovy 44.634. Those are the values to start from. Both must
	// be set; one alone is ignored, because a projection with one axis from the guest and one from
	// a knob is a shear nobody asked for.
	f32 viewmodel_camera_fov_x();
	f32 viewmodel_camera_fov_y();

	// RPCS3_REMIX_DEFERPREANCHOR=1 (default): when a draw's render source has no *current-frame*
	// gauge anchor, buffer the finished instance instead of dividing by last frame's anchor (or by
	// the cross-pass camera) and submit it the moment this frame's anchor for that source arrives.
	//
	// Round 4 attributed the residual wobble to exactly that population: the ceiling fixture
	// vp=c2003391127734f6 picks ref=anchor_prev 9/9 with its origin drifting ~0.5 units between
	// frames while its basis stays 1.00000+-3e-5 - the arithmetic is clean (round 4's f64 divide
	// fixed that), the *pose* is one frame of camera motion stale. gauge_prev was 25% of all
	// divides and gauge_absent another 20%.
	//
	// Skinned draws keep the immediate path (their bone arrays make the payload non-POD).
	// Anything still buffered at flip is flushed through today's prev/camera fallback, so no draw
	// is ever lost. 0 restores the immediate submit bit-exactly.
	bool defer_pre_anchor_enabled();
	u32 defer_pre_anchor_max();

	// RPCS3_REMIX_DEFERPREVONLY=1 (default): defer only the anchor_prev branch, never the
	// gauge_anchor_absent branch. An absent-source draw has had no anchor for two consecutive frames,
	// so holding it bets on an anchor appearing for a source that is not producing them; an
	// anchor_prev draw's source demonstrably produced one last frame. MEASURED: absent is 35.8% of
	// the deferral population in both round-31 sessions, so this removes a third of the buffering
	// cost before any judgement about whether the remainder pays. Read defer_absent_declined against
	// defer_buffered. 0 reproduces round 31 exactly. INERT while DEFERPREANCHOR=0 (its current value).
	bool defer_prev_only_enabled();

	// --- ROUND 46 -------------------------------------------------------------------------------
	// RPCS3_REMIX_DEFERVIEWMODEL=1: let a TAG-ONLY viewmodel draw into the pre-anchor deferral, i.e.
	// gate the exclusion at RemixGSRender.cpp on viewmodel_PLACE instead of viewmodel_DRAW.
	//
	// The exclusion's own comment argues from PLACEMENT - "a viewmodel draw is placed relative to
	// the weapon camera, so no world anchor for its render source will ever make it more correct".
	// RPCS3_REMIX_VMTAGONLY=1 (this title's shipped value) removes exactly that premise: with the
	// tag decoupled from the placement the draw IS a world draw, divided by the world gauge. Round
	// 34 already corrected the twin gate on the tail-rescue ladder to viewmodel_place with this same
	// argument (see ':17939'); this is that correction's missing sibling.
	//
	// MEASURED, round-45 play-test, 44,375 flips:
	//   * every viewmodel pick reads ref=anchor_prev anchor_vp=ad7ce9d672a0bf6b - the WORLD program
	//     one frame stale - 4/4, while all 15 world/prop picks read anchor or identity-bypass, 0/15
	//     anchor_prev. That split is the user's split: props stable, arms and helmet wobbling.
	//   * 'Remix albedo-trace:', 9,544 continuous samples with the camera free: ref=anchor gives a
	//     translation residue of EXACTLY 0.000 on 5,622 of 5,622 draws, and ref=anchor_prev/camera
	//     is displaced on 3,922 of 3,922. A fresh anchor is not merely better, it is exact.
	//   * defer_fresh 867,086 of defer_buffered 874,746 = 99.1% of held draws DO get a fresh anchor
	//     later in the same frame, so a held viewmodel draw is overwhelmingly likely to be flushed
	//     against one rather than time out.
	//   * the tail rescue is already armed for these draws (its gate is viewmodel_place) and never
	//     succeeds: tail_rescued_cur + tail_rescued_aged = 0 of 131,774 attempts, 99,428 of them
	//     failing 'same_ref'. No ALTERNATIVE EXISTING anchor helps. Only waiting for this frame's.
	//
	// The exclusion's second reason is real and is handled rather than relaxed: the flush recomputes
	// the transform and does not re-run apply_viewmodel_basis / apply_viewmodel_rotation, so a
	// deferred viewmodel draw would silently lose them. MEASURED this run: dbasis=0 (VMBASIS=0, the
	// operator is inert) but drot = 1.46..1.99 units on every one of 3,293 census lines. The submit
	// site therefore captures the composite world-space operator it applied, Op = post * pre^-1, and
	// the flush replays it onto the re-divided transform. At the shipped VMROTPIVOT=0 the pivot is
	// the eye, so Op does not depend on the placement it was measured at and the replay is exact.
	//
	// Default 0: bit-exact round-45 behaviour, so this is a one-line A/B.
	bool defer_viewmodel_enabled();

	// RPCS3_REMIX_FPA2C=1 (default): read the two alpha-to-coverage bits the RSX carries - the
	// fragment shader control word's RSX_SHADER_CONTROL_ALPHA_TO_COVERAGE (gcm_enums.h:468) and
	// NV4097_SET_ANTI_ALIASING_CONTROL's msaa_alpha_to_coverage - and replay them as an instance
	// alpha test (GREATER, FPKILREF) on material-bound draws that have no alpha test of their own.
	//
	// Same unread-state bug class as round 4's KIL, one layer up: rpcs3's own GL backend consumes
	// both bits (GLFragmentProgram.cpp:240, GLDraw.cpp:218-223) and this backend never read either.
	// It is the only remaining hardware mechanism that makes the Remix kil: census population
	// (alpha_range=0..255, alpha_test=0, blend=0, depth_write=1) cut out on real hardware.
	// 0 restores the always-pass alpha state.
	//
	// ROUND 6 - the semantics changed, and the round-5 shape is now a documented mistake.
	// Mode 1 (default) requires BOTH bits: ctrl && reg. Mode 2 is round 5's ctrl-only behaviour,
	// kept only so the regression can be reproduced. Mode 0 is off.
	//
	// Why: the hardware gates alpha-to-coverage on NV4097_SET_ANTI_ALIASING_CONTROL, and so does
	// rpcs3 core - GLDraw.cpp enables GL_SAMPLE_ALPHA_TO_COVERAGE from msaa_alpha_to_coverage_enabled(),
	// and the *shader* emulation path is no different: RSXThread.cpp:2170 masks the guest's shader
	// control word down to (32_BITS_EXPORTS | DEPTH_EXPORT | USES_KIL) and then SYNTHESISES
	// RSX_SHADER_CONTROL_ALPHA_TO_COVERAGE itself at :2191-2198, again from the register, only when
	// the backend cannot do hardware A2C. So core never reads an A2C bit out of the guest's word.
	//
	// And it cannot: RSX_SHADER_CONTROL_ALPHA_TO_COVERAGE is 0x01000000, which lies inside
	// RSX_SHADER_CONTROL_USED_TEMP_REGS_MASK (0xff000000). Read straight off the guest's shader
	// control word - which is what round 5 did - that bit is the low bit of the fragment program's
	// used-temp-register COUNT. That is the whole of a2c_ctrl=866,637: it is "this program uses an
	// odd number of temporaries", i.e. about half of every draw, and 658,012 of them were given a
	// spurious alpha test at reference 128. Haze's a2c_reg is 0 for the entire run, so with the AND
	// the replay is inert and the see-through surfaces go away - and the honest verdict on the
	// title is negative: Haze does not use alpha to coverage.
	//
	// There is no "correct threshold" to look for. A2C has no reference value - coverage is
	// proportional to alpha - so if a future title ever does set the register, the faithful replay
	// is the blend-to-cutout category, not a scalar test, and the ctrl term should be dropped
	// entirely rather than ANDed (it means temp-register parity, not coverage).
	bool fp_a2c_enabled();
	u32 fp_a2c_mode();

	// RPCS3_REMIX_NECTARMODE=<0|1> (default 1): 1 publishes haze.nectar_disruption="1" on every
	// frame the (A41A18E14C782613, E5D8F51451B96165) pass is submitted - round-4 behaviour. 0 never
	// publishes, which kills the fork-side greyscale. That pair is one of the generic half-res
	// blended shadow-sampling passes, drawn whenever its volume is on screen, so mode 1 fires
	// whenever the player merely *looks toward* the room rather than when the effect is active.
	// Mode 2 (publish on a discriminator) waits on the lightpass census naming one.
	u32 nectar_mode();

	// RPCS3_REMIX_LIGHTPASSCENSUS=1 (default): one 'Remix lightpass:' line per (vp, fp) per stats
	// window for draws on a smaller-than-main clip with blend on and depth-write off - the
	// candidate population for Haze's per-light shadow/lighting composite. Dumps the live blend
	// state and the fragment program's inline literal vec4s, which is where per-light positions or
	// colours would be if they exist at all. Measurement only; zero behaviour change.
	bool light_pass_census_enabled();

	// --- round 6 ---------------------------------------------------------------------------------

	// RPCS3_REMIX_NOTEXMAT=1 (default): attach a shared, lazily-created mid-grey material
	// (albedo 0.5/0.5/0.5, one 2x2 CPU texture through the synthetic "0x<hash>" path) to every draw
	// that reaches submission with no albedo material at all, instead of submitting material-less.
	//
	// This is a MITIGATION, not a fix. 41% of Haze's submitted draws reach Remix with no material
	// (tex_none=2,008,830 against tex_bound=2,879,211); a material-less surface renders white, which
	// the warm guest lights turn beige and an unlit neighbour turns black, and that is the worst
	// thing on screen. Neutral grey is the honest constant for "we do not know this surface's
	// colour": it neither invents a texture nor claims the surface is a perfect reflector.
	// Counter: notex_mat_applied. 0 restores today's material-less submit bit-exactly.
	bool notex_material_enabled();

	// RPCS3_REMIX_SKIPSHADOWONLY=1 (default): refuse, as world geometry, any draw that resolved no
	// material AND whose only eligible texture unit carries a DEPTH format (CELL_GCM_TEXTURE_DEPTH*)
	// AND whose render clip is strictly smaller than the main pass's.
	//
	// SKIPAUXUNTEX's successor, with the term that actually discriminates. Its predecessor keyed on
	// "no unit at all", fired 99,695 times on a population it was not written for, and was retired.
	// The measured shape here is different and specific: 'Remix notex:' on disk shows
	// class=retry-refused reason=format unit=0 fmt=0xb2 dims=2048x2048 - a 2048x2048 DEPTH16 shadow
	// map as the ONLY eligible unit, on the half-res lighting/shadow-projection passes. Those are
	// composite passes, not surfaces, and submitting them as white world geometry is the flat-white
	// wash. Main-clip shadow-only draws are deliberately exempt (censused, never skipped).
	// Counter: skip_shadowonly; census 'Remix shadowonly:'. 0 restores submission.
	bool skip_shadow_only_enabled();

	// RPCS3_REMIX_SKIPCMASK=1 (default): refuse, before decode, any draw whose colour mask has ALL
	// THREE of R, G and B off (NV4097_SET_COLOR_MASK). Alpha is deliberately not part of the test -
	// an alpha-only write feeding a later composite still writes nothing the guest sees in RGB.
	//
	// A draw the guest itself masked out of its colour buffer cannot be in the guest's picture, so
	// submitting it as world geometry can only add surfaces the guest never showed. Eat Lead
	// (BLUS30267) is the measured case: vp=dc76f9c50a96ccba fp=daf4787c7e54c2f7 is 218 draws/frame
	// of a 3-instruction fragment program that outputs a constant 1.0 and samples nothing, and the
	// runtime cannot rescue it - the API instance path reads only the ALPHA bit of writeMask
	// (rtx_instance_manager.cpp), so a masked draw arrives as a fully visible grey surface once
	// neutral_material is attached.
	// Counter: skip_cmask; census gate 'cmask'. 0 restores submission.
	bool skip_colour_mask_off_enabled();

	// RPCS3_REMIX_SKIPNOTARGET=1 (default): refuse, before decode, any draw whose surface colour
	// target is surface_target::none - a depth/stencil-only pass with no colour attachment at all.
	//
	// Same argument as SKIPCMASK and the same measured population: the Eat Lead pair above is also
	// drawn into 512x512 and 128x128 target=0 passes with depth_write=1 blend=0. Those draws are
	// generically invisible (there is no colour buffer to write), and refusing them before
	// update_camera_candidate() also keeps them out of camera election, which they currently
	// contest. Demon's Souls' title-gated skip_camera_surface refuses the same shape; with this on,
	// that title's count moves from camsurf to skip_notarget - a relabel, not a behaviour change.
	// Counter: skip_notarget; census gate 'notarget'. 0 restores submission.
	bool skip_no_colour_target_enabled();

	// RPCS3_REMIX_WALKSAMPLED=1 (default): when the albedo walk is about to refuse to substitute a
	// higher texture unit (the tex_retry_refused exit), step past the refused unit anyway - but ONLY
	// to a unit named in the fragment program's own sampled_mask whose bound texture carries a
	// COLOUR format.
	//
	// The sampled-mask restriction is the exact term the old RETRYUNSUP widening lacked: that one
	// stepped to units the fragment program never samples, which is how character faces ended up on
	// tree trunks. A unit the program provably samples and which is not a depth buffer is the only
	// widening the evidence sanctions. Counter: tex_walk_sampled. 0 restores the refusal exactly.
	bool walk_sampled_enabled();

	// --- round 7 -----------------------------------------------------------------------------

	// RPCS3_REMIX_MAINCLIPMAX=1 (default): compare "smaller than the main pass" against the
	// LARGEST textured clip seen this session rather than the largest one seen in the PREVIOUS
	// FRAME.
	//
	// This is the reason SKIPSHADOWONLY reached only 4.7% of the population it classifies
	// (437,810 classified against 20,656 skipped over a 13,400-frame session). The reference it
	// compared against, m_main_clip_area, is rebuilt every frame from that frame's textured draws,
	// and on Haze it reads 1024x576 on frames where the main world pass drew something textured
	// and 512x288 on the many frames where it did not. The half-res shadow/lighting passes are
	// themselves 512x288, so on those frames the test was 147456 < 147456 - false - and the rule
	// silently did nothing. A session maximum is a statement about the title; a per-frame maximum
	// is a statement about the previous frame. The reference can only ever grow, so this can only
	// widen such a rule, never narrow it. 0 restores the per-frame value exactly.
	bool main_clip_max_enabled();

	// RPCS3_REMIX_CLAMPALBEDO=<hash>[,<hash>...] (bound 16, default empty): force wrap mode CLAMP
	// on the listed albedo CONTENT hashes, overriding the guest's own RSX wrap state for those
	// textures only.
	//
	// The lever for the UI seam on the route no per-sample rule of ours can reach. Haze draws UI
	// both through the CPU compositor (where the round-7 sub-rect seam rule runs) and as
	// camera-anchored world geometry sampled on the GPU through a Remix material - the same
	// program appears in both 'Remix uvrange:' (3D) and the compositor's own census. A UI atlas
	// bound REPEAT with authored UVs a hair outside [0,1] wraps to the far edge of the sheet and
	// returns a foreign glyph; clamping the texture fixes both routes at once, because the
	// compositor and the material both read entry.wrap_u/wrap_v. Non-default wrap already folds
	// into material_hash, so the clamped variant gets its own material and cannot alias the repeat
	// one. Ships empty: zero behaviour change until a hash is listed. Counter: tex_wrap_forced.
	bool clamp_albedo_matches(u64 hash);
	u32 clamp_albedo_count();

	// RPCS3_REMIX_SKIPCCCONST=1 (default): when choosing the terminal instruction of the HPOS
	// chain, walk past a full-mask writer that is executed under a condition code AND whose every
	// consumed source is a constant.
	//
	// The measured blocker behind fail=lay_other on the Selva canopy programs. Round 6 read seven
	// cached .vp leaders offline and found the identical idiom in all of them: the last full-mask
	// writer to o0 is a MOV from a constant under cond != always - the particle/geometry cull
	// idiom, "if culled, park HPOS at a constant off-screen point". Starting the backward walk
	// there finds a MOV whose only operand is a constant, the source test fails two steps later,
	// and the entire real chain sitting directly behind it is discarded as 'no matrix chain into
	// HPOS'. Both terms are required: an UNconditional constant write really is all such a program
	// does with HPOS (refusing it is correct), and a predicated write reading a temp is a real
	// value that must not be stepped over. 0 restores the old terminal choice exactly.
	//
	// Honest expectation: the affected programs should move OFF lay_other onto an inner-chain
	// refusal reason. Getting further with a better-named error is the deliverable; a placed draw
	// is not promised.
	bool skip_cc_const_writer_enabled();

	// RPCS3_REMIX_WDIVWALK=1 (default): inside match_wdivide, choose the position temp's writer by
	// walking the writer list backwards and SKIPPING writes whose xyz mask is empty, instead of
	// trusting the single most recent VEC writer. 0 restores prog.last_temp_writer bit-exactly.
	//
	// This is the giant-geometry fix. Haze's character/prop compiler emits
	// 'MUL rN.xyz = ATTR0.xyz * RCP(ATTR0.w)' immediately followed by the w-only
	// 'MOV rN.w = c[467].z' that completes the homogeneous vector, and the matrix group after it.
	// last_temp_writer therefore returned the MOV, the MUL/mask test failed, and the matcher gave
	// up before its RCP back-scan ever ran - so has_wdivide stayed false and the backend submitted
	// the raw signed-16-bit quantised attribute. That is the ship at extent 9211, the "incorrect
	// bones" character at 669.7 and the giant NPC weapons: a clean instance basis around a mesh
	// that was never dequantised. The recorded areason 'sca-xyz' was match_const_affine's own
	// (correct) refusal for the RCP, i.e. the last matcher's exit, not the reason.
	//
	// A w-only write cannot alter the submitted xyz nor hide a position term - which is the rule
	// match_const_affine's hop loop already states in as many words and match_prescale already
	// implements. Sweep over every cached .vp on this title: 29 carry the wdivide MUL shape, 25
	// select the identical instruction under both policies, exactly 4 flip refuse->match
	// (57A12323F22F4988, 4D5A87BFFBCE0717, 96EDAAED0C27FD05, 1D9A973AF5CD1514), zero regress.
	// Nothing downstream of the selection moves: the replay is the existing per-vertex divide, and
	// the RCP back-scan still refuses any intervening VEC write to the scalar lane.
	// Counter: wdiv_shadowed_draws (subset of wdiv_draws). Census: 'Remix wdiv-shadowed:'.
	bool wdivide_walk_enabled();

	// --- round 9 -----------------------------------------------------------------------------
	//
	// RPCS3_REMIX_FPVCOL=1 (default): state the colour pipeline the FRAGMENT PROGRAM'S OWN BYTES
	// prove in the per-draw instance blend extension, instead of the parity default that says
	// "colour comes from the Texture argument" for every draw.
	//
	// This is the flat-grey-effects fix. Haze's yellow nectar danger pulse binds no texture, so
	// round 6's neutral grey material attaches - and its fragment program is ONE instruction,
	// 'MOV r0, COL0'. Its colour IS the vertex colour, apply_vertex_colour already decodes ATTR3
	// into the submitted mesh, the runtime's API path already binds color0Buffer from
	// remixapi_HardcodedVertex::color - and the blend extension then tells the fixed-function
	// stage to take colour from the Texture argument only, which is the grey 2x2. The vertex
	// colour is not missing; it is being dropped by explicit configuration.
	//
	// With the knob on, a program classified vcol_pass gets textureColorArg1Source=VertexColor0
	// with operation SelectArg1, and one classified vcol_modulate gets Arg1=Texture,
	// Arg2=VertexColor0, operation Modulate. Both also clear isVertexColorBakedLighting, because
	// under that flag the runtime normalises the vertex colour by its max channel and mixes it
	// toward white by rtx.vertexColorStrength (0.6 in this fork) - the baked-lighting reading,
	// which for an effect whose colour IS the vertex colour destroys the ramp. Everything else in
	// the fill - alphaTest*, the blend factors, tFactor - is untouched.
	//
	// 0 restores today's parity fill for every draw bit-exactly. Counter: fpvcol_applied (draws).
	// Census: 'Remix fpvcol:' (once per program) and 'Remix effect:' (once per vp/fp per window).
	bool fp_vertex_colour_replay_enabled();

	// RPCS3_REMIX_VCOLMOD=1 (default): let apply_vertex_colour run on a TEXTURED draw when its
	// fragment program proves the modulate. Today the call is gated '!material', with the comment
	// "a textured draw's ATTR3 modulates its albedo in the title's own fragment program with
	// per-title semantics this backend does not read, so tinting one would be a guess". The FP
	// bytes discharge exactly that objection: 'MUL r0, <TEX>, COL0' names the semantics.
	//
	// Severed separately from FPVCOL because this is the half that moves mesh content hashes -
	// the vertex colour is hashed into the mesh key, so an ANIMATED vertex colour makes a new mesh
	// per animation step. Watch 'mesh_created' on the live line. 0 restores the '!material' gate.
	// Counter: vcol_mod_applied.
	// RPCS3_REMIX_VCOLCONSTBLACK=0/1 (default 1). Refuse a constant-route vertex colour that
	// resolves to (near) black on a TEXTURED draw - a Modulate by zero deletes the surface. This is
	// the guard that makes RPCS3_REMIX_VCOLMOD=1 safe again after the 2026-08-15 black-HUD note.
	bool vcol_constant_black_guard_enabled();
	bool vertex_colour_modulate_enabled();

	// RPCS3_REMIX_UCODESTORE=1 (default): write the raw vertex ucode of every program whose
	// position decode this backend refuses (archetype unknown, or the innermost operand of the
	// position chain is not an attribute) to bin\remix_ucode\%016llX.vp, once per program per run.
	//
	// The reason this exists: bin\cache\...\shaders_cache\raw is written ONLY by pipeline
	// compilation (rsx_cache.h::store), which the Remix backend never performs - so its newest
	// files predate every Remix session and any program first met under this backend is
	// unreadable offline. That is why the 'mad-src0-not-input' foliage family (4 members) and the
	// Selva canopy pair f7f12d5d15bb9c37 / c1f88035801f88de cannot be decoded from disk today.
	// Same write rsx_cache.h::store performs (fs::write_file(name, fs::rewrite, vp.data)), from
	// the backend that actually runs, into a directory the real cache never touches.
	// No behaviour change. Counter: ucode_stored. Census: 'Remix ucode-store:'.
	bool ucode_store_enabled();

	// RPCS3_REMIX_MADCHAINMIX=1 (default): accept a fused matrix group whose per-row scalars are
	// broadcast reads of DIFFERENT temps/lanes, when each of them chases back through additive
	// accumulator hops to the SAME base temp with lanes exactly {x, y, z}.
	//
	// Named from DF46F03B1B7AB8A4.vp (752 bytes, 47 slots, decoded end to end): the group is at
	// c0..c3 - MUL r0 = r4.yyyy*c[1] @10, MAD r0 = r1.wwww*c[0]+r0 @32, MAD r0 = r1.wwww*c[2]+r0
	// @42, ADD o0 = r0+c[3] @44 - so match_mad_chain_pass refuses at its source-consistency test
	// (the scalars are r4.y, r1.w, r1.w) even though slot 9 is a clean wdivide
	// 'r4.xyz = I0.xyz * RCP@2(I0.w)' and each odd scalar chases back through ADDITIVE hops to a
	// distinct lane of r4: r1.w@27 = r2.x*c467.z + r4.x (x + SIN sway), r4.y direct (no lateral
	// sway on a trunk), r1.w@40 = r1.w*c467.z + r4.z (z + COS sway). The additive terms ARE the
	// wind; dropping them replays the canopy STATIC, which is the honest trade and the contract
	// round 6 anticipated. Tried LAST, after the strict and relaxed passes, so no program that
	// matches today changes matcher.
	//
	// 0 restores the single-source rule bit-exactly. Counters: madmix_resolved (programs),
	// madmix_draws. Census: 'Remix madmix:'.
	bool mad_chain_mixed_lanes_enabled();

	// RPCS3_REMIX_MADLANEMAP=1 (default): round 17, the Selva tree tops. Round 9's mixed-lane chase
	// requires each matrix row to resolve to the lane that matches its own constant-derived
	// component, i.e. it assumes the per-vertex divide wrote x, y, z into lanes x, y, z. The two
	// canopy programs captured by UCODESTORE this session do not:
	//   f7f12d5d15bb9c37  18: MUL r4.xyw = v0.xyxz * r4.wwww   (z parked in lane w)
	//   c1f88035801f88de  14: MUL r0.xzw = v0.xxyz * r1.yyyy   (y in lane z, z in lane w)
	// so both are refused with 'areason=no-chain / note=no matrix chain into HPOS' and their
	// geometry never reaches the scene. This arm reads the permutation out of the divide
	// instruction itself and requires the rows to agree with it, then lets the same permuted divide
	// satisfy the wdivide step so the replay still divides.
	//
	// Offline sweep of all 111 cached .vp programs (bin\cache\...\shaders_cache\raw plus
	// bin\remix_ucode): 5 distinct programs move, all of them currently refused with groups=0, so
	// the change is purely additive - f7f12d5d15bb9c37, c1f88035801f88de, 61ed1d272c0653aa,
	// 1d22398b18a0e97d, 5888b152531b2d91. Every named actor (gauges 2f64c2f8ffd6add1, sky dome
	// af06f6d32ec048ee, haze card edc10321bf7ceb8f, effects f39f504649b6f442, flower
	// c97cd1531ac480d8, first-person 830d7d1b9681c475, round 9's canopy df46f03b1b7ab8a4) is
	// untouched and still matches through the branch it matched through before.
	//
	// 0 restores round 9's identity-lane rule bit-exactly. Counters: madlanemap_resolved
	// (programs), madlanemap_draws. Census: 'Remix madlanemap:'.
	bool mad_lane_map_enabled();

	// RPCS3_REMIX_TAILRESCUE=1 (default): when per_draw_transform's affinity gate (fail=tail) is
	// about to refuse a draw that was NOT divided by its own render source's current-frame anchor,
	// retry the division against a usable same-pass-shape anchor and re-run the same gate.
	//
	// This is the missing-geometry fix. fail=tail is 57% of all world refusals (168,782 of 294,676
	// in the newest full run) and the shipped residue histogram says the tolerance cannot be the
	// answer: ZERO refusals sit near the tolerance and 83% sit 50-500x over it. What they are is
	// draws divided by the cross-pass half-res camera because no anchor covered their own source at
	// that moment - three different main-clip world programs refuse with the bit-identical residue
	// 3.40683, and a residue shared across programs is a property of the REFERENCE, not of the
	// programs. Usable main-pass anchors exist in essentially every window; refused draws were
	// simply never retried against them.
	//
	// Two retries, in order: this frame's anchor matching the draw's pass shape (color target +
	// clip size) at ANY surface offset, then the freshest same-shape anchor aged <= TAILRESCUEAGE
	// frames. Each recomputes world = f64(fused x anchor^-1), re-normalises w and re-runs the SAME
	// affinity gate - a rescued draw is never exempt from it, so a wrong gauge keeps its perspective
	// residue and still refuses. Worst case is today's behaviour (missing), never smeared geometry.
	// Synchronous: no buffering, no submit-order change, zero interaction with DEFERPREANCHOR.
	// Counters: tail_rescued_cur / tail_rescued_aged / tail_rescue_failed; census
	// 'Remix tail-rescue:'. 0 restores drop-at-refusal bit-exactly.
	bool tail_rescue_enabled();

	// RPCS3_REMIX_TAILRESCUEAGE=<frames> (default 30): how stale the second retry's same-shape
	// anchor may be. 30 is the GAUGECAMHOLD precedent - long enough to cover a cut or a burst frame
	// in which both anchor slots went stale, short enough that the pose it places a rescued draw
	// with is still the same shot.
	u32 tail_rescue_age();

	// RPCS3_REMIX_GUESTLIGHTLUM=<0..1> (default 0.7) and RPCS3_REMIX_GUESTLIGHTMAXEXT=<units>
	// (default 6): the census thresholds for the guest-light discriminator. A submitted draw whose
	// albedo's mean-RGB luminance is at least LUM, whose world extent is at most MAXEXT, and whose
	// render state is either opaque-fixture (depth_write=1, blend=0) or the glow-card signature
	// (blend=1, depth_write=0) is printed on a 'Remix light-candidate:' line.
	//
	// Measurement first, on purpose. The last generalisation of fixture identity (albedo alone) put
	// lights on doors and sheet-metal covers, because Haze shares fixture textures with other props.
	// The census de-risks the list in one ordinary session and costs nothing.
	f32 guest_light_lum();
	f32 guest_light_max_extent();

	// RPCS3_REMIX_GUESTLIGHTLISTEXT=0/1 (default 1). Apply guest_light_max_extent() to an
	// explicitly listed albedo as well as to the census/AUTO population. See the derivation on the
	// definition: one Haze bulb texture spans 0.5385 .. 83.18 units of world extent.
	bool guest_light_list_extent_enabled();

	// RPCS3_REMIX_GUESTLIGHTFIXTUREMAT=1: replace the visible material of an explicitly enrolled,
	// pair-qualified small fixture with a synthetic warm masked material. The guest albedo hash
	// is preserved for analytical-light triggering; default 0 changes no title.
	bool guest_light_fixture_material_enabled();

	// RPCS3_REMIX_GUESTLIGHTAUTO=0 (default): 1 treats every census-qualifying draw above as a
	// fixture trigger, through the existing dedup / cap / idle machinery, without needing its albedo
	// on GUESTLIGHTALBEDO. Default OFF because the census has to name the population before it is
	// trusted; flipping it on is a launcher edit inside the same sitting.
	// RPCS3_REMIX_GUESTLIGHTAUTO: 0 = off (default), 1 = whole census population (fixtures AND glow
	// cards, the round-6 meaning), 2 = GLOW CARDS ONLY. Clamped to 2. See the definition for the
	// measured two-population split behind mode 2.
	u32 guest_light_auto_mode();
	bool guest_light_auto_enabled();

	u64 nectar_disruption_vp_hash();
	u64 nectar_disruption_fp_hash();

	// RPCS3_REMIX_TRACEALBEDO=<16 hex digits>: trace one material once per frame through
	// mesh submission. Used to connect a Remix texture-picker hash to the RSX vertex program
	// and the active camera that placed it. 0 when unset.
	u64 trace_albedo_hash();
	u64 trace_albedo_vp_hash();

	// RPCS3_REMIX_CAMLOCKVP=<16 hex digits>: accept camera candidates only from one verified
	// vertex program. Diagnostic/bisector for titles that emit auxiliary perspective passes.
	// 0 when unset.
	u64 camera_lock_vp_hash();

	// RPCS3_REMIX_CAMFALLBACKVP=<16 hex digits>: accept this camera source only in frames where
	// CAMLOCKVP emitted no candidate. The normal continuity/switch gate still applies.
	u64 camera_fallback_vp_hash();

	// RPCS3_REMIX_CAMFALLBACKVP2=<16 hex digits>: a second verified fallback for scenes where
	// both the primary and first fallback program temporarily stop drawing.
	u64 camera_fallback_vp2_hash();

	// RPCS3_REMIX_CAMRELATCH=1 (default): update the active camera's matrices and reference
	// inverse *during* the frame, as soon as a candidate from the same source and within the
	// discontinuity tolerance is seen, instead of only at flip.
	//
	// The flip-only latch is deliberate and its comment argues it is image-exact: with
	// world = fused_now * reference_inverse_old, the submitted clip transform is
	// world * V_old * P_old = fused_now, so the *rasterised* image is exactly what the title
	// asked for. That argument holds for a rasteriser and fails for a path tracer, which reads
	// the world pose: every static instance's transform absorbs the camera's frame-to-frame
	// delta, so the whole scene breathes by the per-frame camera motion. Measured on repeated
	// picks of the same prop: basis lengths fluctuating 0.9995..1.0043 frame to frame.
	//
	// 0 restores the flip-only latch and is the A/B for the whole fix. Neither mode touches the
	// camera *age*, the pending-switch confirmation or the per-frame candidate vote - a relatch
	// can only refresh a camera that is already active and already continuous, never switch it.
	// world_ref_fresh / world_ref_stale on the live line say how much of the scene the relatch
	// actually reached, which is the number that decides whether deferred submission is needed.
	bool camera_relatch_enabled();

	// RPCS3_REMIX_CAMRELATCHTOL=<view delta> (default 0.8): how far the mid-frame relatch may
	// reach for "this frame's version of the active camera". 0.8 is
	// s_camera_discontinuity_tolerance, i.e. exactly today's test, so the default is BIT-EXACT
	// and the knob can only narrow.
	//
	// The menu symptom on Eat Lead is this relatch, not the vote. It refreshes m_active_camera
	// from the FIRST candidate of the frame merely within tolerance of the active view, and in
	// every sampled menu frame that was a ONE-vote animated part (view_delta 0 to the active
	// camera because it is what the active camera was last refreshed from) drifting frame to
	// frame, while the 9-vote static winner sat 0.40 away. submit_camera() then submits the
	// refreshed camera, so the Remix camera, the lighting anchoring and the temporal history all
	// follow a prop. The picture stays exact (world = fused x ref^-1 is exact whichever cluster
	// is the reference); everything a path tracer reads does not.
	//
	// 0.1 on this title: 4x under the props' 0.40 and >= 3x over the true camera's own per-frame
	// motion (p99 0.33 units against a matrix norm of ~8-35). A frame whose own cluster exceeds
	// the tolerance simply does not relatch - today's flip-only latch for that frame.
	// Counter: cam_relatch_refused (candidates that pass the 0.8 test and fail this one), which
	// is structurally 0 at the default.
	f32 camera_relatch_tolerance();

	// RPCS3_REMIX_GAUGEANCHOR=1 (default): divide every world draw by the *main pass's own*
	// view x projection, taken from that same frame's first WORLDIDENTITYVP draw on the same
	// surface key, instead of by the elected camera's reference inverse.
	//
	// Round 1 made the reference same-frame (88% fresh) and the geometry still shook, which
	// refutes staleness as the cause and leaves the *gauge*: on Haze the elected camera
	// 7f3d3abcefc8b057 is a half-resolution auxiliary pass (camsurf=01360000, camclip=512x288)
	// while the world draws sit on surf=014D0000 clip=1024x576, and camvp alternates between
	// that pass and ad7ce9d672a0bf6b frame to frame (cam_fallback 8212 vs cam_resolved 3298).
	// Consecutive frames therefore express world poses in two different gauges that disagree by
	// ~0.4% of basis scale - exactly 512/510 == 256/255, one pixel of viewport on a 512-wide
	// pass - plus units of translation once multiplied through a ~1790-unit camera distance.
	// That alternation is the shake.
	//
	// A WORLDIDENTITYVP program draws geometry whose world transform is the identity, so its
	// fused matrix *is* the pass's view x projection: same frame, same pass, bit-exact, handed
	// over by the title for free. Dividing by its inverse is closed-form correct and makes the
	// identity draws themselves divide to exact identity, which is the built-in self-check -
	// the 'Remix worldid-draw:' trace's translation must collapse to ~0.
	//
	// 0 restores the elected camera's reference inverse and is the whole attribution in one
	// relaunch. Counters: gauge_used / gauge_prev / gauge_absent partition every draw that
	// reaches the reference branch, gauge_contested is the tripwire for a listed program that
	// also draws another pass on the same surface key.
	bool gauge_anchor_enabled();

	// RPCS3_REMIX_GAUGETRACE=<frames> (default 600): how many frames of 'Remix gauge:' lines to
	// write, measuring the disagreement between the elected camera's reference and the anchor -
	// the number that confirms or refutes the alternation story numerically in the same run the
	// fix ships in. Re-armed by any successful pick. 0 disables the trace; the fix is unaffected.
	u32 gauge_trace_frames();

	// RPCS3_REMIX_GAUGESLOTS=<1..16> (default 16, 4 restores round-2 sizing): how many render
	// sources may hold an anchor at once. Round 2 shipped four slots, and a slot is only reusable
	// once its anchor is two frames old - so on Haze the shadow pass (2048x2048), the auxiliary
	// pass (512x288), the main pass (1024x576) and anything else drawing that frame compete for
	// four entries, and the loser's draws silently fall back to the elected camera's gauge. That is
	// one of the two silent exits behind gauge_absent=804650 (16% of draws) in the round-2 run;
	// gauge_slot_exhausted counts it directly, and 'Remix gauge-keys:' lists the live slots once
	// per stats window so the real per-frame key count is known rather than guessed.
	u32 gauge_slot_count();

	// RPCS3_REMIX_GAUGEPREVDIMS=1 (default): when the previous frame's anchor lookup misses on the
	// exact (surface offset, target, clip) key, retry matching only (target, clip width, clip
	// height). Surface offsets move between frames whenever the title double-buffers or
	// reallocates; the pass *shape* does not. 20% of round-2's draws took the prev-anchor path, and
	// every exact-key miss there sent the draw to the cross-pass camera instead. Counters:
	// gauge_prev_exact / gauge_prev_dims split the existing gauge_prev population; 0 restores
	// exact-key-only lookup.
	bool gauge_prev_dims_enabled();

	// RPCS3_REMIX_GAUGECURDIMS=1 (default): when the CURRENT frame's exact-key anchor lookup misses,
	// retry on (target, clip width, clip height) alone against THIS frame's anchors, before falling
	// back to the previous frame's. This is the same relaxation GAUGEPREVDIMS already ships for
	// frame t-1, applied one frame fresher, and it exists because of what round 38 measured.
	//
	// WHY (MEASURED, round 38, and this is the round's root cause). A draw that reaches the
	// gauge_prev branch is divided by fused_donor(t-1) while Remix renders it with camera(t). The
	// residual is exactly D = V(t) * V(t-1)^-1, which for a point p is p -> eye + (p - eye) * dR:
	// a rotation about the eye by one frame of camera turn. The displacement is therefore
	// (distance from the eye) x (turn per frame), so the SAME defect is a millimetre wobble on the
	// weapon, a hand-width jiggle on a dumpster, and metres on a light fixture across the room.
	//   * 4000 consecutive 'Remix vmbasis:' frames: the recovered viewmodel 3x3 is best aligned to
	//     the camera basis of frame t-1. Expressing it in the basis at t+s and measuring the
	//     frame-to-frame change gives s=-1 a STRICT minimum in median (0.2287 deg), mean (0.3750)
	//     and p95 (1.2348) over s in {-3,-2,-1,0,+1,+2,+3}; s=0 reads 0.2984/0.4655/1.5531.
	//     s=-2 and s=+1 are both worse than s=-1, so this is one frame and not "any decorrelation".
	//   * All 48 'Remix picked:'/'pick-deep:' lines for the family the user reports as jiggling
	//     (vp=830d7d1b9681c475 fp=c61b0b9586dd67fb) read ref=anchor_prev with anchor_frame ==
	//     frame-1 and cam_age=0 - the divisor is a frame old while the camera is current.
	//   * Magnitude check: the gun sits 0.73 m from the eye and the camera turns up to 8.86 deg in
	//     one frame; 0.73 * 8.86 deg = 0.113 m against a measured max |d cpost| of 0.1248 m.
	//
	// The in-tree justification for the prev branch (RemixGSRender.cpp, above the dispatch) is
	// "never worse than the old path: the elected camera's reference was always a frame old". That
	// premise has since been falsified by CAMRELATCH: world_ref_fresh=3598650 against
	// world_ref_stale=97515 is 97.4% of draws carrying a current-frame elected camera.
	//
	// COUNTERS, and read these before believing anything: gauge_cur_dims counts draws this route
	// actually rescued; gauge_cur_avail counts draws where a this-frame same-shape anchor EXISTED
	// at that moment, and is incremented whether or not the knob is armed. gauge_prev_camfresh
	// counts prev-branch draws whose elected camera was current, i.e. the size of the alternative
	// route nobody has tried. If gauge_cur_avail stays at 0 the route cannot fire at all for this
	// title - the props are drawn before ANY identity donor of that shape - and the answer is the
	// deferral that already exists (DEFERPREANCHOR) rather than a fresher lookup.
	//
	// BLAST RADIUS: this hands a different render source's this-frame anchor to a draw whose own
	// source has not anchored yet. That is the same guess find_gauge_anchor_shape already makes for
	// the tail rescue and GAUGEPREVDIMS already makes for frame t-1; the affinity gate at the end
	// of per_draw_transform still re-runs on the result. If static scenery lands in the wrong
	// place, this knob is the first suspect. 0 restores round 37 byte for byte.
	bool gauge_cur_dims_enabled();

	// RPCS3_REMIX_GAUGECAMHOLD=<frames> (default 30, 0 disables): when a flip has neither a
	// current-frame nor a previous-frame anchor to rebuild the submitted camera from, keep the last
	// anchor-derived split for up to this many frames instead of falling back to the elected
	// (half-resolution, alternating) gauge. Round 2 rebuilt the camera on only 43% of flips, so on
	// the other 57% the scene divided by anchors while the camera came from the old election - the
	// two disagreeing at frame rate is a whole-scene shake. Counters: gauge_cam (current anchor),
	// gauge_cam_prev (previous frame's), gauge_cam_held (this hold). Risk: a real scene cut with no
	// identity draws holds a stale gauge for up to this many frames; the tell is gauge_cam_held
	// spiking at cuts.
	u32 gauge_cam_hold_frames();

	// RPCS3_REMIX_ANCHORGAUGECENSUS=<lines> (default 32, 0 = off): PURE MEASUREMENT, no behaviour.
	// Bounded 'Remix anchor-gauge:' lines naming, per flip, the disagreement between the two places
	// m_active_camera.position is written from - the camera election (the guest's world-space eye)
	// and apply_gauge_anchor_camera's split of the gauge anchor (the anchor donor's own frame).
	//
	// ROUND 29's WHOLE POINT. These are not two estimates of one quantity, they are one quantity in
	// two frames of reference, and nothing in the backend labels which. Measured on the round-27 run
	// (bin\remix_dump.log, session at line 1906093, 62,315 flips): the anchor-derived value tracks
	// the guest's eye plus the translation the WORLDIDENTITYVP override is discarding on that
	// frame's anchor donor, to within +-1.01 units on 39 of 41 disagreeing frames against a signal
	// of 421 units. On Haze the main-clip anchor is held 90% of the time by AD7CE9D672A0BF6B, whose
	// vertices are LAND-CARRIER-LOCAL (round 22 measured mean |t| 197, max 4377), so the "camera
	// position" drifts at the carrier's speed - 0.54 units/frame - and jumps ~784 when the guest
	// re-bases the carrier's local origin. Counters: anchor_cam_offset (flips over 1 unit),
	// anchor_cam_offmax (the largest L-infinity disagreement seen).
	u32 anchor_gauge_census_lines();

	// RPCS3_REMIX_CAMANCHOREYE=1 (default): ROUND 30, AND IT IS THE FIX ROUND 29 SPECIFIED AND DID
	// NOT SHIP. Serve every consumer that compares the camera against a SUBMITTED transform an eye
	// taken from the same gauge anchor that produced that transform, instead of whichever of the two
	// frames of reference happened to write m_active_camera.position last. Six gates plus four
	// evidence-gating diagnostics, and the pick record - ten call sites, not six; "six" was the count
	// of load-bearing gates in round 29's audit and the drift is corrected here.
	//
	// THE PROBLEM, and it is measured, not argued. m_active_camera.position has three write sites and
	// two frames of reference:
	//   flip()'s election latch (:3187)              - the elected pass's own split
	//   consider_camera_candidate's mid-frame relatch - the same, refreshed (cam_relatch=52505 of
	//                                                  66,508 flips, so this is what consumers hold)
	//   apply_gauge_anchor_camera (:2618)            - the split of the GAUGE ANCHOR
	// and the geometry those consumers measure against is submitted as `fused * anchor->inverse`,
	// which is in the anchor's frame BY CONSTRUCTION. So the correct eye for them is the anchor's,
	// whatever frame the anchor happens to be in - and that is why this fix does not depend on
	// deciding which of the two frames is "the world".
	//
	// MEASURED, round 30, from the round-29 build's own census over the newest session in
	// bin\remix_dump.log (starts at line 1998822, flips=66508):
	//     anchor_cam_offset=2134   anchor_cam_offmax=429.96 units   gauge_cam=59008 (88.7% of flips)
	// i.e. on 2,134 flips (3.2%) the two writers disagreed by more than a unit, and the worst
	// disagreement was 429.96 units on X. Those are exactly the flips on which the sun's travel, the
	// sky tag's anchor test and the viewmodel distance gate were measuring a distance between two
	// points in different frames.
	//
	// WHERE ROUND 29 WAS WRONG, and it matters because its §4(a) would have made things worse. It
	// attributed the disagreement to donor AD7CE9D672A0BF6B, the land carrier's local-space program,
	// on 90% of flips. In the live census EVERY ONE of the 30 census lines with a magnitude bucket
	// over 1 unit carries anchor_vp=BD1C10DF5703E559 - the program round 24 documented as submitting
	// ABSOLUTE WORLD vertices (raw box x [-1397.69 .. +1146.32]) - while AD7CE9D672A0BF6B anchored
	// 10,377 census lines with the two eyes agreeing to ~1e-6. Refusing "displaced" donors, as §4(a)
	// proposed, would therefore have refused the genuine world-space map as a gauge donor. Not shipped.
	//
	// AND THE REFUSAL THAT MAKES IT SAFE, which the round-30 review caught as a blocker: the eye is
	// served ONLY to a draw that was actually divided by that same anchor. per_draw_transform starts
	// from the elected camera's reference and swaps in an anchor only when find_gauge_anchor hits;
	// gauge_anchor_absent counted 1,287,878 of 17,869,132 divides (7.21%) in the traced session, and
	// for every one of those the submitted transform IS in the elected frame, so substituting the
	// anchor eye would break gates that were already right. The gates in question are armed at
	// single digits (SKYANCHOR=4, VIEWMODELANCHOR=4, VMPAIRMAXDIST=2, SUNCARDMINDIST=4,
	// FPCENSUSMAXDIST=4) against a 429.96-unit frame gap, so this is a regression, not a rounding
	// concern. m_ref_pick_source / m_ref_pick_vp are the provenance test.
	//
	// 0 restores round 29 exactly: every consumer falls back to m_active_camera.position, which is the
	// value it read before this knob existed. Counters: camframe_served (consumer reads answered from
	// the anchor eye), camframe_corrected (those that differed from .position by over a unit) and
	// camframe_max on the live line. corrected and max accumulate with the knob OFF as well, so the
	// off arm of the A/B still reports the size of the correction it declined to make.
	bool camera_anchor_eye_enabled();

	// RPCS3_REMIX_SKIPAUXUNTEX=1 (default): refuse as world geometry any draw that resolved no
	// material and named no albedo unit whose render-source clip is *smaller* than the main pass's.
	// Haze draws a second, untextured pass over the same meshes into a half-resolution auxiliary
	// surface: 6004dce66b8b7a11 has material=0 albedo_unit=-1 clip=512x288 while the textured
	// soldier draw beside it - same vertex count, same origin, same frame - sits at 1024x576.
	// Submitted as world geometry that is a cloud of flat grey meshes filling the room, which is
	// the greyscale wash. The existing exact-pair skips (SKIPUNTEXTUREDVP, the nectar pair, the
	// 56CC character-depth pass) are subsets and are kept ahead of this rule so their counters keep
	// their lineage. Counter: skip_aux_untextured; census: 'Remix aux-untex:'. 0 restores
	// submission.
	bool skip_aux_untextured_enabled();

	// RPCS3_REMIX_WATCHALBEDO=<hex>[,<hex>...] (default empty): log every submit/skip decision for
	// draws binding one of the named albedo hashes, one line per verdict class per frame per
	// albedo ('Remix watch:'). Zero behaviour change. The instrument for a symptom that only
	// appears at a distance the cursor cannot reach: take albedo= from a close-up pick, arm it
	// here, walk backwards, and the log names the gate at the moment the object disappears - or
	// shows the albedo simply ceasing to be drawn, which is the title's own LOD swap and moves the
	// question to whatever the far draw is refused for.
	bool watch_albedo_matches(u64 hash);
	u32 watch_albedo_count();

	// RPCS3_REMIX_FPKIL=1 (default): replay the fragment program's per-pixel discard as a per-draw
	// alpha test. A draw whose ucode carries a *conditional* KIL (fp_fingerprint::kil_conditional),
	// or whose bound albedo unit has NV4097_SET_TEXTURE_CONTROL0 bit 2 set
	// (fragment_texture::alpha_kill_enabled), is submitted with alphaTestType = GREATER and a
	// reference of either the constant recovered from the ucode or RPCS3_REMIX_FPKILREF.
	//
	// Why this and not the material: 3559 of 3572 materials on this title are created while the RSX
	// alpha test is disabled (RemixTextures.cpp:334-352 reads NV4097_SET_ALPHA_TEST_* and nothing
	// else), so every material-side audit correctly reported "no cutout state anywhere" - the state
	// genuinely is not in the registers. It is in the ucode, and the backend disassembles vertex
	// programs exhaustively while reading fragment programs only for sampler masks. A grep of
	// Remix/ for KIL, ROP_discard or alpha_kill found nothing before this: the recurring bug shape
	// is an instruction in the ucode the backend never replays.
	//
	// Materials are content-hash-keyed and shared, so the same albedo can be drawn by a KIL program
	// and a non-KIL program in one frame; the per-draw blend extension already carries alpha state
	// and is already folded into the static-submit signature, so this needs no cache change.
	// Counters: kil_ucode / kil_ctrl / kil_disagree (detection), texkill_seen (the texture-control
	// channel), kil_alpha_applied / kil_alpha_texkill (the replay). Census: 'Remix kil:'.
	// 0 restores today's always-pass exactly.
	bool fp_kil_cutout_enabled();

	// RPCS3_REMIX_FPKILREF=<0..255> (default 128): the alpha reference used when the ucode walk
	// could not recover the program's own threshold constant. A recovered constant always wins, so
	// this only moves the programs whose 'Remix kil:' line reads ref=-1.
	u32 fp_kil_reference();

	// RPCS3_REMIX_GAUGEF64=1 (default): invert the gauge anchor's fused matrix in double precision
	// and carry out the world = fused x reference divide in double, casting only the finished world
	// matrix back to f32.
	//
	// The wobble is floating-point noise, not reference selection. A fused view x projection is
	// ill-conditioned by construction - its z and w columns are nearly parallel and its translation
	// row grows with camera distance from the world origin - and mat4_invert is a single-precision
	// cofactor expansion, the numerically worst algorithm for that input. Replaying the shipped
	// algorithm on the run's own %a-hex anchor matrices: cond2 8.9e7 on a cargo-plane anchor gives
	// ||A.inv32(A) - I|| of 3.2e-3 in the basis and 1.07 units in the translation row, against
	// 1.5e-9 in f64. That is the measured ref=anchor wobble (median d_basis 1.4e-3), why the
	// large-coordinate cargo plane shakes 2-9 units while small-coordinate Selva looks almost
	// still, and why ref=identity - which never multiplies by an inverse - is bit-exact.
	//
	// Both halves must be f64: the same experiment shows an f64 inverse cast back to f32 still
	// leaves ~1.0 of translation error, because the cast inverse's large entries cancel in the f32
	// multiply. Counter: world_div_f64. Self-check: 'Remix gauge-selfcheck:' prints the residual in
	// both precisions with the anchor's translation magnitude, which is the region tag the error
	// needs - it is multiplicative, so a measurement taken in a small-coordinate room proves
	// nothing. 0 restores the f32 path bit-exactly.
	bool gauge_f64_enabled();

	// RPCS3_REMIX_CULL=1: submit doubleSided=0 for draws whose RSX cull state is enabled instead
	// of forcing every instance double-sided. 23 of the 67 programs in the Haze census cull, and
	// a double-sided triangle costs the path tracer intersection work on both faces. Off by
	// default because a wrong winding anywhere in the strip/fan/quad expansion turns a
	// single-sided surface invisible, and a missing wall is worse than a slow one - turn it on
	// with a capture to hand.
	bool cull_from_rsx();

	// RPCS3_REMIX_NOVCOL=1: leave every submitted vertex colour at 0xFFFFFFFF instead of reading
	// ATTR3 for draws that resolved no material. The bisect knob for vertex-coloured geometry.
	bool vertex_colour_disabled();

	// RPCS3_REMIX_SMOOTHNORMALS=1: tag every submitted world instance
	// REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS, which makes Remix recompute the normal buffer
	// on the GPU as an area-weighted average over each mesh's own triangles
	// (RtxGeometryUtils::dispatchSmoothNormals, dispatched from RtxSceneManager on BVH build and
	// update only - static geometry pays once).
	//
	// This is not a refinement of the game's normals: this backend never recovers them. Every
	// vertex is submitted with a constant (0,0,1) normal, so with this off the whole scene is lit
	// off one direction that happens to be right for nothing. That is also what makes it cheap
	// here - the normal buffer already exists, so RtxSceneManager's forceNormals stays false, the
	// interleaved fast path survives, and the compute pass overwrites the constants in place.
	//
	// A global toggle rather than the comma-separated hash list the other categories use: those
	// four (sky/hidden/particle/decal) are per-material decisions, and a hash list here would mean
	// hunting hashes before any lighting improved at all. Dynamic - Remix promotes an instance to
	// kUpdateBVH when the category is added or removed, so toggling mid-frame is handled.
	bool smooth_normals_enabled();

	// RPCS3_REMIX_GUESTLIGHTCELLS=<count>: a guest-light SOURCE -- the (albedo, vp, fp) triple that
	// triggered it -- is permanently disqualified once it has been seen in this many DISTINCT
	// quantised cells. 0 = off. A bolted-down fixture occupies exactly ONE cell for a whole scene;
	// anything that arrived on legs has a history of others. Derivation on the definition in
	// RemixTransforms.cpp.
	u32 guest_light_max_cells();

	// RPCS3_REMIX_GUESTLIGHTSTABLE=<frames>: a GUESTLIGHTAUTO candidate must re-appear in the same
	// quantised cell on this many DISTINCT frames before it may create a light. 0 = off (round-41
	// behaviour). Clamped to 600. Full derivation on the definition in RemixTransforms.cpp.
	u32 guest_light_stable_frames();

	// RPCS3_REMIX_GUESTLIGHTMAINCLIP=0/1 (default 0): a guest light may only be minted or re-placed
	// from a draw whose surface clip is same / double / half of the session's main clip. Refuses the
	// aux-pass copies of a fixture, whose extent measures 2.2-4.1x the main-pass value under the
	// main camera's inverse. Counter guest_light_auxclip. Derivation on the definition.
	bool guest_light_main_clip_enabled();

	// RPCS3_REMIX_GUESTLIGHTTRACK=0/1 (default 0): identify a fixture by (albedo, vp, quantised
	// LOCAL AABB centre) and RE-PLACE its light at that frame's recovered position every frame it is
	// drawn, with isDynamic so the runtime does not sleep the update. The fix for lights that sit in
	// the wrong place and move as the camera turns on a title with no gauge anchor. With it OFF the
	// drift is still measured. Derivation on the definition.
	bool guest_light_track_enabled();

	// --- ROUND 52: AUTHORED LEVEL LIGHTS -----------------------------------------------------------
	// Eat Lead (BLUS30267) ships a full analytic light list inside its own region containers, and
	// tools/eatlead/extract_lights.py has extracted it: bin/eatlead_lights/<Level>.lights, 2,422
	// records over 10 levels, 2,285 of them with a world placement. These knobs drive the loader and
	// the injector that put that list into Remix as analytical lights.
	//
	// EVERY ONE OF THESE DEFAULTS TO OFF-OR-NEUTRAL. AUTHOREDLIGHTLEVEL empty means the loader never
	// even looks for a file, and AUTHOREDLIGHTS=0 means nothing is submitted even when one loaded --
	// so a build carrying this feature is byte-identical to one without it until two lines of
	// bin/BLUS30267.conf are uncommented. That split is deliberate: step 1 of the plan is "prove the
	// file loads" with LEVEL set and MODE still 0, and step 2 is the first submission.
	//
	// Each is a first-call function static, so a .conf change needs an RPCS3 RESTART, not a reload.

	// RPCS3_REMIX_AUTHOREDLIGHTS: 0 = off (nothing submitted, byte-identical to a build without this),
	// 1 = authored only (maybe_inject_guest_light early-returns, counter guest_light_suppressed),
	// 2 = both, for the A/B during transform bring-up. Expect double lighting on 2; that is the point.
	u32 authored_light_mode();

	// RPCS3_REMIX_AUTHOREDLIGHTLEVEL=<name>, e.g. `01_JapaneseRestaurant1`. EMPTY BY DEFAULT, and
	// empty means the loader does nothing at all. The backend has no notion of which level is loaded
	// (nothing under Emu/RSX/Remix/ reads cellFs or /dev_bdvd/), and deriving one from the guest's
	// own `USRDIR/Maps/<16HEX>.map` opens needs a hook outside this directory -- that is step 6 of
	// the plan. For the whole transform bring-up, which is a single-level exercise, a knob is
	// adequate and carries no risk.
	std::string authored_light_level();

	// RPCS3_REMIX_AUTHOREDLIGHTDIR=<dir> under the executable dir. Default `eatlead_lights`, matching
	// where the extractor writes and how every other backend file is located.
	std::string authored_light_dir();

	// RPCS3_REMIX_AUTHOREDLIGHTRADIANCE: global radiance scale, applied as
	// (R,G,B)/255 * f0 * this. Default 30, which is the derived guest-light path's fixed radiance,
	// so the first run is directly comparable to it. Calibration is step 5 and is not this round.
	f32 authored_light_radiance();

	// RPCS3_REMIX_AUTHOREDLIGHTMAX: cap on live authored handles, mirroring GUESTLIGHTMAX. Default
	// 64. Mission 1 has 398 placed lights and the largest level 416; creating 400 lights every frame
	// is probably fine for the runtime and is not free on the CPU side.
	u32 authored_light_max();

	// RPCS3_REMIX_AUTHOREDLIGHTDIST: cull radius in RENDER units from the eye. Default 40. Culling in
	// render space is safe even before the placement transform is settled, because the eye and the
	// transformed light positions are in the same space by construction.
	f32 authored_light_distance();

	// RPCS3_REMIX_AUTHOREDLIGHTMINRADIUS: floor on sphere radius. Default 0.2, matching
	// GUESTLIGHTRADIUS. The file's f6 is the source extent and goes to 0.5 and below on OmniLight.
	f32 authored_light_min_radius();

	// RPCS3_REMIX_AUTHOREDLIGHTCONESOFT: shaping cone softness for the spot classes. Default 0.1.
	// Signed/zero-tolerant parse, unlike env_float -- 0 is a legal softness, not "unset".
	f32 authored_light_cone_softness();

	// RPCS3_REMIX_AUTHOREDLIGHTIGNOREVM: remixapi_LightInfo::ignoreViewModel. Default 1, matching the
	// derived path. The viewmodel is submitted under its own camera; letting world lights into it is
	// a separate decision and this is the knob that makes it one.
	bool authored_light_ignore_viewmodel();

	// --- THE TWO BRING-UP DIALS. NEITHER IS A CONVENTION AND NEITHER MAY BECOME ONE. ---------------
	//
	// RPCS3_REMIX_AUTHOREDLIGHTAXIS: 0..47, the SIGNED AXIS PERMUTATION applied to the file's
	// position and to each row of its basis, BEFORE the world->render map. Encoding:
	// axis = permutation * 8 + signs, permutation 0..5 in the order
	// (x,y,z) (x,z,y) (y,x,z) (y,z,x) (z,x,y) (z,y,x), and signs bit 0/1/2 negating output
	// component 0/1/2. 0 IS THE EXACT IDENTITY -- no permutation, no negation, no rounding.
	//
	// This exists because ONE bit of the transform is genuinely unknown: whether the file's authoring
	// frame (Z-up, right-handed, metres) is the guest's runtime world frame. It is NOT a search space
	// and it is NOT a place to bake an answer. The earlier "Z is negated" claim was RETRACTED this
	// round: a 1-D vote over the Z offset scored 77/89 for +Z and 74/89 for -Z (no discriminating
	// power), and the per-frame rigid alignment against the WRONG LEVEL scored a perfect 4/4 for
	// 04_BelAirMansion on a mission-1 frame while mission 1 never beat the field. With four
	// simultaneous lights at a 0.6 m tolerance the test has no power at all. Until the section-6.3
	// measurement is taken WITH its wrong-level control, no axis mapping is entitled to be hardcoded,
	// which is why this is a conf knob and not a constant.
	u32 authored_light_axis();

	// RPCS3_REMIX_AUTHOREDLIGHTOFFSET="x,y,z": a constant offset added to the file position AFTER the
	// axis permutation and BEFORE the world->render map. Signed floats, comma / space / semicolon
	// separated; anything unparseable leaves that component 0. Default 0,0,0.
	//
	// A STATIC WORLD->RENDER OFFSET IS THE WRONG SHAPE OF ANSWER ON THIS TITLE and this knob is not
	// one: measured over 11 of 11 frame pairs, the render space differs BETWEEN FRAMES by a pure
	// translation (best case 0.002 units of residual across three independent difference vectors,
	// nine of eleven with t.z exactly 0), so any constant fitted in render space is wrong one frame
	// later. This offset is applied in the FILE's frame, upstream of the per-frame map, which is
	// where a fixture-to-glow-card offset (expected 0.3-0.5 m along the aim) would actually live.
	void authored_light_offset(f32 (&out)[3]);

	// RPCS3_REMIX_AUTHOREDLIGHTONLY: if >= 0, submit ONLY this one light index, at 10x radiance, and
	// exempt it from the distance cull and the handle cap so it cannot be silently dropped. Default
	// -1 = off. This is the step-2 probe: one light, one question, "is it in the right room?".
	s32 authored_light_only();

	// RPCS3_REMIX_AUTHOREDLIGHTDUMP: emit at most this many `Remix authored-light:` rows per frame,
	// each carrying the file position and the render position OF THE SAME LIGHT IN THE SAME FRAME.
	// That pairing is the entire input to the section-6.3 measurement and the only thing that makes
	// the residual solvable offline. 0 = off. Clamped to 128, like s_max_guest_light_lines.
	u32 authored_light_dump();

	// RPCS3_REMIX_CAMSANITY: 0 = census only (shipped), 1 = refuse an insane camera before
	// SetupCamera. RPCS3_REMIX_CAMSANITYTOL is the fractional FOV deviation from the title's own
	// latched reference that counts as insane. Full derivation on the definitions.
	u32 camera_sanity_mode();
	f32 camera_sanity_tolerance();

	// RPCS3_REMIX_SKYEXTENT=<units>: a draw that writes no depth, is anchored on the camera
	// (sky_max_anchor) and spans at least this much in its widest axis *in world units* is the
	// title's sky dome, and is tagged SKY. Haze draws its sky as an 82-vertex, 10,000-unit
	// vertex-coloured dome with depth writes off (vp=fc0fac8afccec49a); with no material it reached
	// Remix as an opaque white shell enclosing the camera. 0 disables the detection. Default 2000.
	//
	// "World units" is the correction made after ae94587. The extent used to be measured over the
	// submitted vertex positions, which on Resistance 2 (NPEA00431) are raw quantised integers
	// (attr0 type=5, w spanning -16511..16511) that the program decodes with a constant scale
	// before its matrix - so every R2 mesh "spanned tens of thousands of units" and the test stopped
	// filtering. Replaying the 157 dumped draws that carry a fused matrix: measured raw, 63 of them
	// (40.1%) clear 2000; measured after the instance transform, the world extents of the decoded
	// population collapse to 0.13..711 and only 27 clear it. Haze's dome only ever passed because
	// its positions are already in world units.
	f32 sky_min_extent();

	// RPCS3_REMIX_SKYANCHOR=<units>: how far the draw's own origin may sit from the camera and
	// still be a sky candidate. This is the signal that actually separates a dome from the rest of
	// the scene, and extent alone is not - 27 of the 157 fused draws dumped for ae94587 clear
	// sky_min_extent() with depth writes off, because R2's backdrops and water planes genuinely are
	// thousands of units across. A sky dome is the only thing in a scene whose model origin *is*
	// the camera:
	//   Haze     fc0fac8afccec49a  82 vtx, extent 10000, anchor 0.000  (origin exactly at the eye)
	//   R2       41c59a3a2bfc71bf  56 vtx, extent  3516, anchor 2.404  (origin 2.4 below the eye,
	//   R2       c1781a2e32aba35d  88 vtx, extent  3037, anchor 2.404   i.e. on the ground under it)
	// and the nearest draw that clears depth-write and extent but is not one of those sits at
	// anchor 96.91 (9cd34d4f4003439e, extent 5109) - a 24x margin over the 4.0 default and 40x over
	// the value R2 actually uses. Both gates are load-bearing: extent alone leaves 27 candidates,
	// the anchor alone leaves 5 (it admits Haze's view-anchored 9f591b6a6b825612 and
	// 15ad612980aca110, which are 4.4 and 0.11 units across). Together they leave 3, all domes.
	//
	// 0 disables the anchor requirement and restores the extent-only rule of ae94587 - the one that
	// tagged cat_sky=825915 of 2038738 R2 draws and turned the scene blue. The distance is measured
	// against the resolved camera, so a draw with no world transform is refused, not guessed.
	f32 sky_max_anchor();

	// --- round 36: the anchor test was asking the wrong question, and the black skies are it ------
	//
	// RPCS3_REMIX_SKYANCHORMODE=<0..3>, default 0 = today's behaviour bit for bit.
	//
	// THE DEFECT. sky_max_anchor() above is compared against |transform.translation - eye|. That is
	// only "is this dome centred on the eye" for a dome whose transform CARRIES its position. An
	// absolute-world draw - correct identity-ish transform, world-space vertices - has its
	// translation at the world origin, so the quantity it actually measures is |eye|, and the
	// bigger the level, the further from the origin the player walks, the more certainly it fails.
	// The user's newly-reachable levels read anchor=2137.85 against limit=4 with |camera| ~ 2137,
	// which is that identity to five figures. Round 31 established the same thing for
	// VIEWMODELANCHOR (the guard read ~2100 to a point no geometry occupied while the picks read
	// 1.5), and round 32 already listed SKYANCHOR as "dead for absolute-world geometry" - this is
	// that finding producing a visible symptom for the first time: the dome is never categorised
	// SKY, is submitted as ordinary unlit geometry, and renders black.
	//
	// THE REPLACEMENT, and why it is not "raise the limit". Raising SKYANCHOR cannot work: the
	// number it is compared against is a property of where the PLAYER is standing, so any limit
	// that admits the dome at 2137 also admits every other absolute-world draw in the level, and
	// SKY hides the instance from the world pass - over-tagging deletes terrain. What the test is
	// trying to ask is "does this thing surround the camera", and there is already a scale-free,
	// translation-free answer computed three lines above it: `inside`, whether the eye lies within
	// the draw's transformed AABB. A dome contains the eye whether it is authored on the eye or at
	// the world origin. Terrain does not - the eye stands ABOVE the ground surface.
	//
	//   0 = |translation - eye| <= sky_max_anchor(). Round 13..35. Default, unchanged.
	//   1 = |AABB centre - eye| <= sky_max_anchor(). The same test with the defect removed but the
	//       absolute limit kept - correct for an eye-following dome, still wrong for a static one
	//       (a dome authored at the world origin has its centre at the world origin too). Provided
	//       to separate "the translation was wrong" from "the distance test was wrong", which are
	//       different claims and only one run apart.
	//   2 = the eye is INSIDE the transformed AABB. Ignores sky_max_anchor() entirely. This is the
	//       one the launcher arms.
	//   3 = mode 2 OR mode 0, i.e. admit a dome that either surrounds the eye or sits on it. The
	//       most permissive, kept for the case where a dome's AABB genuinely excludes the eye
	//       (a dome clipped to the horizon band, say) - try this only if mode 2 leaves a level black.
	//
	// The extent floor (sky_min_extent(), 2000 by default) and the depth-write test both still run
	// AHEAD of this, unchanged, and they are what keeps mode 2 from being a blanket admission: a
	// draw has to be enormous AND depth-write-free AND contain the eye.
	//
	// AUDITABLE BY CONSTRUCTION: the census prints canchor= (the new quantity) and inside= beside
	// the existing anchor= on every row, at every mode, so the two can be compared on the SAME rows
	// before and after the switch rather than across two runs.
	//
	// REVERT, one launcher line, no rebuild: set "RPCS3_REMIX_SKYANCHORMODE=0".
	// PRE-REGISTERED REFUTATION: if a level's dome rows read inside=0, mode 2 will refuse them too
	// and the sky stays black - the AABB premise is then wrong and mode 3 is the next test, not a
	// bigger limit. If terrain starts vanishing, mode 2 is over-admitting and the revert is above.
	// env_u32, not env_float - env_float rejects 0 and 0 is this knob's OFF value (round 32's
	// SKYANCHOR=0 defect, which is exactly this trap).
	u32 sky_anchor_mode();

	// RPCS3_REMIX_SKYBACKDROP=<0|1|2>: the *backdrop* rule, which is a different object from the
	// sky-dome rule above and is deliberately not on by default.
	//   0  off (default). Nothing measured, nothing tagged.
	//   1  measure only. Counts sky_backdrop_hit / sky_backdrop_dw and changes nothing on screen.
	//   2  measure and tag SKY.
	//
	// It exists because the sky-dome rule cannot see Resistance 2's actual backdrop. The live
	// census (4759 frames) found it: c87769e09c995db9, a *4-vertex* quad 15,895 world units across
	// with a 27-vertex sibling at 6,310, camera-filling, world-authored (raw 15,935 -> world
	// 15,895, i.e. its decode is a translation not a scale) - and it writes depth, and its origin
	// sits 82 units from the eye. Both of the dome rule's gates reject it, and neither can be
	// relaxed on its own.
	//
	// The backdrop rule replaces both with three properties that draw actually has:
	//   - world extent >= sky_min_extent(), as before;
	//   - the camera is *inside* the draw's transformed bounding box. Not "the origin is on the
	//     eye" - a backdrop card is not centred on the camera, it encloses it. In the census
	//     c87769e09c995db9 sits at anchor/extent 0.005..0.015 while ca526d308f1650bb's 715-vertex
	//     terrain is at 2.64 and its 502-vertex draw at 1.00, i.e. outside;
	//   - at least s_sky_backdrop_min_units_per_vertex of extent per vertex. The census separates
	//     cleanly on this: the backdrop runs 234..3974 units per vertex, and the largest thing that
	//     is not it runs 27.9, with real terrain at 1.2..7.3.
	//
	// Why it ships off. Replaying the same rule over the 157 dumped draws from *earlier* scenes
	// admits 27 of them, against 3 for the dome rule - and those 27 are records, not draws, so the
	// number does not say what share of a frame they are. That is precisely the quantity the
	// 40.5%-of-the-scene regression was made of, and mode 1 is how to get it before tagging
	// anything: run once, read sky_backdrop_hit against draws_submitted, then decide.
	u32 sky_backdrop_mode();

	// RPCS3_REMIX_SKYHASH=<0|1|2>: identify the sky by *material* rather than by geometry.
	//   0  off. Exactly the 81af315 behaviour: nothing measured, nothing tagged.
	//   1  measure only (default). Learns the hashes, counts and censuses them, tags nothing.
	//   2  measure and tag SKY.
	//
	// Every geometric sky rule in this file asks a question about size or placement, and on this
	// title the answers are fuzzy: sky_backdrop_mode()'s rule matched 1.11% of submitted draws in
	// the capture it was measured against, which is far too broad to arm, because 1.11% of R2's
	// draws is world geometry. A texture hash is exact where an extent threshold is fuzzy - two
	// draws either sample the same image or they do not - so this keys on the albedo content hash
	// and uses the geometry only to *learn* which hashes belong to a dome.
	//
	// A hash is armed when every draw carrying it has been dome-shaped over at least
	// s_sky_hash_min_draws draws, and is disqualified permanently by a single draw that is not.
	// That inverts the usual failure mode: a threshold rule mis-tags whatever sits near the
	// threshold, while this one can only mis-tag a texture the title uses on nothing but domes.
	//
	// What tagging SKY actually does on this backend, read out of the runtime it targets rather
	// than assumed: the API path routes categoryFlags through toRtCategories() (dxvk-remix-numos3
	// src/dxvk/rtx_render/rtx_remix_api.cpp:741) and categoryToCameraType() (same file, :731), and
	// an instance whose cameraType is Sky is force-hidden - rtx_instance_manager.cpp:1005-1008,
	// "Hide the sky instance since it is not raytraced", which clears its ray mask to 0 at
	// rtx_instance_manager.cpp:1288-1289. It is *not* rasterised into the sky cubemap: that
	// happens in tryHandleSky(), which is called only from RtxContext (rtx_context.cpp:981 and
	// :2240, both D3D9 raster paths) and never from SceneManager::submitExternalDraw
	// (rtx_scene_manager.cpp:2388). So on this path SKY means "remove from the BVH", and the sky
	// itself has to come from somewhere else - set rtx.skyMode = 1 (Numos, Hillaire atmospheric
	// scattering, rtx_options.h:1266) so the hole is filled procedurally.
	//
	// Note also that the fork already matches rtx.skyBoxTextures against API-submitted draws by
	// albedo hash - fork_hooks::externalDrawTextureCategories, rtx_fork_submit.cpp:65-100, called
	// from rtx_scene_manager.cpp:2460 - so once the census names the hashes they can equally be
	// put in rtx.conf. This knob exists because it finds them.
	u32 sky_hash_mode();

	// RPCS3_REMIX_SKYTEXTURED=0: require a sky candidate to be untextured, the behaviour up to and
	// including 9c73eb0. That requirement was written against Haze's vertex-coloured dome and does
	// not generalise - a sky dome may perfectly well carry a texture. Resistance 2's (NPEA00431)
	// does: it is drawn with cloud and water detail, so albedo_texture_unit() resolves a material,
	// the '!material' arm was never entered, and the dome was submitted as world geometry - a solid
	// sphere standing inside the level that occluded the scene and turned with the camera without
	// enclosing the player. The remaining conditions carry the meaning: depth writes off, an origin
	// on the camera (sky_max_anchor) and a world-unit extent past sky_min_extent(). On by default;
	// set 0 to bisect a title where a large depth-write-off textured draw (a fog card, a full-screen
	// effect) is being mis-tagged and so hidden from the world pass.
	bool sky_allows_textured();

	// RPCS3_REMIX_VIEWMODEL. The first-person arms/weapon rule, added after 81af315, where the
	// backend had no notion of a viewmodel at all: REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL and
	// REMIXAPI_CAMERA_TYPE_VIEW_MODEL were both declared in remix_c.h and neither was ever used,
	// so the player's arms and weapon were submitted as ordinary world geometry.
	//   0  off. Exactly the 81af315 behaviour: nothing measured, nothing tagged.
	//   1  measure only. Counts and censuses, and leaves categoryFlags alone.
	//   2  measure and tag VIEW_MODEL (default).
	//
	// The rule is the viewport depth range, which is a mechanism rather than a correlation. The
	// G0 recovery says Resistance 2's projection maps to z_ndc = 1 - near/w, i.e. z_ndc in [0,1]
	// with the far plane at infinity, so viewport z maps that to [offset_z, offset_z + scale_z].
	// R2 draws the whole world at [0, 1] and one thing at [0, 0.2] - the front fifth - which
	// forces every one of its pixels in front of any world pixel past w = near/0.8. That is the
	// standard "the weapon never clips into a wall" trick, and it is *why* the bucket exists.
	//
	// Measured over every dump log in the scratchpad, 7041 R2 3D draws (clip 1280x704). Only two
	// depth ranges occur in the whole 3D pass, with nothing between them:
	//   [0, 1]     6873 draws over 77 vertex programs - the world.
	//   [0, 0.2]    168 draws (2.39%) over exactly two - 1438eb79c0843fea (depth_write=1) and
	//               7a4a57869f9c4a1f (depth_write=0) - always the same mesh (vtx=1687 idx=6948),
	//               always as that pair, always skinned, in every gameplay capture.
	// Those same two programs also draw a 5954-vertex character at [0, 1] in 78 dumped draws, so
	// the program hash does *not* separate the viewmodel from world geometry and a per-program
	// rule would tag that character. The depth range does separate them, per draw.
	//
	// Two independent measurements corroborate, neither of them used as a gate:
	//   projection  recovering the fused G0 gives the [0, 0.2] draws fovx 60.001 / fovy 36.132,
	//               near 0.090, against fovx 72.000 / fovy 44.634 for the world in the same
	//               captures. 44.634 is what the Remix dev menu reports for R2 (44.6), which is
	//               what says the recovery is right rather than merely self-consistent.
	//   anchor      on frames where a camera was sampled in the same frame, the [0, 0.2] draw's
	//               instance origin sits 0.446 units from the eye at eye height ([-4.048 15.241
	//               -6.450] against cam [-3.664 15.205 -6.226], dY 0.036) while the nearest
	//               same-frame world draw is 8.930 away. A 20x gap - the mirror image of the sky
	//               dome, pinned a fixed short distance in front of the eye instead of enclosing it.
	//
	// Ships at 2 because tagging is provably image-identical against the runtime this backend is
	// built for: bit 26 does not exist in dxvk-remix-numos3's own remix_c.h (its enum stops at
	// SMOOTH_NORMALS = 1 << 24) and toRtCategories() maps by name over bits 0..24, so the bit is
	// dropped. See the comment on the tagging site in submit_subdraw for what the fork needs
	// before it means anything. Mode 1 is there for the day that changes and the ratio needs
	// reading before the image does.
	u32 viewmodel_mode();

	// RPCS3_REMIX_VIEWMODELCAM. Which reference per_draw_transform divides a viewmodel draw by.
	//   0  off. Exactly 148b467: the world camera's reference, for every draw.
	//   1  refuse. Viewmodel draws are counted and dropped, nothing else changes.
	//   2  transform (default). A reference latched from the viewmodel population itself, with
	//      refusal when none has been latched yet.
	//
	// viewmodel_mode() names the population; this decides what to do with it. They are separate
	// knobs because the detection was measured at 148b467 and this was not.
	//
	// The fault, replayed on the G0 matrices dumped in bin\log\RPCS3.log (frame 4077, the capture
	// that recorded vm_tagged=7986 of vm_considered=1851953). R2's active camera is arch=fused with
	// has_reference, and the tagged programs dump as skinned_layered groups=1, so every one of them
	// takes per_draw_transform's ref/outer branch:
	//     world = fold_viewport_z(G0_draw, 0.2, 0) * inverse(fold_viewport_z(G0_camera, 1, 0))
	// The two matrices do not carry the same projection. Recovered from those same dumps:
	//     viewmodel  fovx 60.001  fovy 36.132  near 0.0900
	//     world      fovx 72.000  fovy 44.634  near ~0.08
	// so the composition never cancels. Evaluating it on the real numbers gives m[3][3] = -1077,
	// and after :5135 divides that out the basis is (0.491, 0.002, 1.150) - Y crushed ~500x, i.e.
	// the mesh flattened to a sheet - with a perspective residue of 0.029 and -0.068 sitting on
	// the 0.02 affine tolerance. The translation survives: the replay puts it at
	// [-4.03 15.24 -6.47] against the census line's origin=[-4.048 15.241 -6.4505], which is what
	// proves the code really took this path with this reference. A correct origin and a collapsed
	// basis is exactly a streak, and it swings with the melee animation because the bones move.
	//
	// Dividing the same draw by a *viewmodel* reference instead returns the identity to 1e-16
	// (basis 1.000, 1.000, 1.000; translation 0), because the viewmodel programs all share one G0
	// and the bones already carry the mesh into world space. That is the whole of the fix.
	u32 viewmodel_camera_mode();

	// RPCS3_REMIX_VIEWMODELVP=<hex>[,<hex>...]: treat the named vertex programs as viewmodel
	// geometry regardless of their viewport depth range, which is what feeds the (otherwise
	// dormant on this title) viewmodel-camera machinery: the viewmodel reference latch, the
	// own-reference division under VIEWMODELCAM=2, and the VIEW_MODEL category at submit.
	//
	// Haze reports scale_z=0.49875 offset_z=0.50125 on every draw in the scene including the sky
	// dome, so the depth rule selects nothing at all (vm_tagged=0 of vm_considered=353598) and
	// there is no threshold to retune. A hash list is the only discriminator left.
	//
	// A program hash provably does not separate viewmodel from world geometry on its own - R2's
	// two [0,0.2] programs also draw a 5954-vertex character at [0,1] - so every hash-tagged draw
	// is additionally required to sit within VIEWMODELANCHOR of the eye, measured with the closed
	// form the sky and viewmodel censuses already use. Bounded at 8.
	bool viewmodel_vp_matches(u64 hash);

	// How many hashes the list actually parsed, echoed on the live line so a typo'd or truncated
	// VIEWMODELVP is visible in the log instead of reading as "the fix did nothing".
	u32 viewmodel_vp_count();

	// RPCS3_REMIX_VIEWMODELANCHOR=<world units>, default 4 (s_viewmodel_max_anchor, the limit the
	// viewmodel census already prints). How far a hash-tagged draw's instance translation may sit
	// from the eye and still be treated as viewmodel geometry. Only the hash tagger consults it;
	// the depth-based path and its counters are untouched.
	f32 viewmodel_anchor_limit();

	// Debug light knobs so a derived camera can be judged visually at all.
	f32 debug_light_radius();
	f32 debug_light_radiance();

	// ---------------------------------------------------------------------------------------
	// Instance categories
	// ---------------------------------------------------------------------------------------
	// Comma-separated 16-hex albedo *content* hashes - the same values the 'Remix tex=' dump
	// line and the Remix dev menu display.
	//
	// CORRECTION (round 4): the claim that used to sit here - that the rtx.*Textures conf lists are
	// matched on the D3D9 path only, so setting categoryFlags at submit is the *only* mechanism
	// that reaches an API draw - is wrong, and it was wrong in the direction that costs debugging
	// time. The fork applies conf texture lists to API-submitted draws by albedo hash:
	// fork_hooks::externalDrawTextureCategories (dxvk-remix-ppsspp src/dxvk/rtx_render/
	// rtx_fork_submit.cpp:65-100), called from SceneManager::submitExternalDraw
	// (rtx_scene_manager.cpp:2432). The note at sky_hash_mode() above always said so; these two
	// comments contradicted each other and this is the one that was out of date.
	//
	// The practical consequence is that a hash can be categorised from *either* side, and that a
	// hash listed in two conf lists at once gets both behaviours - which is how 0xD1A6D1B27ADE6232
	// came to be in rtx.worldSpaceUiTextures and rtx.skyBoxTextures simultaneously.
	//
	// CORRECTION (round 13), because the second half of that sentence used to read "force-hidden on
	// this fork: the sky sometimes disappears", and that is not what the deployed runtime does. The
	// Sky category NEVER hides an API-submitted instance. The hide is
	//     if (drawCall.cameraType == CameraType::Sky) { currentInstance.m_isHidden = true; }
	// (rtx_instance_manager.cpp:1006) - keyed on the draw's CAMERA TYPE, not on the category - and
	// the deployed numos3 runtime assigns 'prototype.cameraType = (cameraType == CameraType::Sky)
	// ? CameraType::Main : cameraType' (rtx_remix_api.cpp:916), which maps Sky back to Main on
	// purpose. InstanceCategories::Sky is then read by nothing on the external-draw path: its only
	// consumers are in rtx_camera_manager.cpp's processCameraData, which is the D3D9 path.
	// MEASURED, same run: the dome censuses as 'sky-census: vp=af06f6d32ec048ee TAGGED' and is
	// visible on screen at the same time. So both RPCS3_REMIX_CAT_SKY and rtx.skyBoxTextures are
	// no-ops here, and what actually makes the dome self-lit is rtx.worldSpaceUiTextures alone
	// (rtx_instance_manager.cpp:1103-1107 forces emission at intensity 2.0 with the albedo as the
	// emissive texture). See sky_emissive_albedo_matches() for the round-13 replacement.
	//
	// These knobs remain the mechanism that does not require editing a conf, not the only
	// mechanism that works.
	//   RPCS3_REMIX_CAT_SKY      -> SKY          (selects the sky camera; does NOT hide - see above)
	//   RPCS3_REMIX_CAT_HIDE     -> HIDDEN       (IGNORE is a no-op for API draws; do not use it)
	//   RPCS3_REMIX_CAT_PARTICLE -> PARTICLE
	//   RPCS3_REMIX_CAT_DECAL    -> DECAL_STATIC
	enum class draw_category
	{
		sky,
		hide,
		particle,
		decal
	};

	bool hash_in_category(draw_category which, u64 hash);

	// True when any list has at least one entry, so the per-draw lookup can be skipped whole.
	bool any_category_listed();

	// ---------------------------------------------------------------------------------------
	// Default lighting
	// ---------------------------------------------------------------------------------------
	// The camera-parked debug sphere blows out everything near it and crushes everything far,
	// so the readable default is a distant sun; the sphere stays as an optional fill, off by
	// default. Direction is in the recovered world space and is normalised in code.
	//   RPCS3_REMIX_SUNDIR="x,y,z"  RPCS3_REMIX_SUNRADIANCE=<f>  RPCS3_REMIX_SUNANGLE=<degrees>
	//   RPCS3_REMIX_CAMLIGHT=<radiance>  (0 = off)
	//   RPCS3_REMIX_NOSUN=1  no default sun at all
	void sun_direction(f32 (&out)[3]);
	f32 sun_radiance();
	f32 sun_angular_diameter();
	f32 camera_light_radiance();
	bool nosun_enabled();

	// --- round 23: a PER-LEVEL sun -----------------------------------------------------------
	// sun_direction() is ONE vector for the whole process. Haze's sky is per-area, so one sun for
	// every level is wrong by construction. The key is the sky dome's ALBEDO HASH, which the
	// backend already watches change (RPCS3_REMIX_SKYEMISSIVE lists two of them) and which needs no
	// level-change signal from the guest.
	//
	// Precedence, highest first: SUNMAP entry -> sky-texture derivation (SUNSKY) -> sun card
	// (SUNTRACK) -> SUNDIR. SUNDIR is always the fallback and is never removed.
	//
	//   RPCS3_REMIX_SUNMAP=<skyalbedo>:<x>,<y>,<z>;<skyalbedo>:<x>,<y>,<z>   (bound 8)
	//     Hand-tuned per level, keyed on the sky dome albedo. Normalised here, so the vector need
	//     not be unit length. Same TRAVEL convention as SUNDIR (sun -> scene).
	//
	//   RPCS3_REMIX_SUNSKY=0|1   derive the direction from the dome texture's brightest region.
	//   RPCS3_REMIX_SUNSKYMINVTX=64   vertex floor for the dome draw the derivation may use.
	//
	// Why the texture works as the source: MEASURED offline on the dumped Selva dome
	// (bin\remix_tex\unit0_D1A6D1B27ADE6232_2048x1024.bmp) the sun IS painted into it. Peak Rec.709
	// luma 249.2 (rgb 255,251,214) at texel (1340,513); 112 texels within 2 % of peak, centroid uv
	// (0.6769, 0.5015); the top half of the image is black, i.e. the panorama is v in [0.5, 1].
	u32 sun_map_count();
	bool sun_map_lookup(u64 albedo_hash, f32 (&out)[3]);
	bool sun_sky_enabled();
	u32 sun_sky_min_vertices();

	// RPCS3_REMIX_SUNSKYPEAKFRAC=<percent, 50..100>, default 98. The fraction of the dome texture's
	// PEAK luma a texel must reach to join the luminance-weighted centroid that becomes peak_uv.
	// 98 reproduces the round-23 constant exactly.
	//
	// Round 24, MEASURED offline on bin\remix_tex\unit0_D1A6D1B27ADE6232_2048x1024.bmp: at 98 only
	// 112 texels qualify and they all lie on ONE row (y=513, x 1304..1463) - a 160x1 streak, not a
	// disc, because that row is a one-row spike (row 512 max 162.75, row 513 max 249.18, row 514
	// max 230.25) sitting on the top edge of the used band. The centroid therefore lands at
	// v=0.50146, the very first row of the panorama. Sensitivity on that texture:
	//   98 -> 112 texels, 1 row,   peak_uv=[0.67693 0.50146]
	//   90 -> 972 texels, 28 rows, peak_uv=[0.66404 0.50961]
	//   85 -> 19418 texels, 229 rows, peak_uv=[0.63368 0.56012]
	// A knob rather than a changed constant because the threshold is shared by every dome, and the
	// dome that currently solves correctly (CDFE11B12552EA2D) must not be moved to fix another.
	u32 sun_sky_peak_percent();

	// RPCS3_REMIX_SUNSKYDOWN=1 (default): refuse a DERIVED sky sun whose travel vector does not
	// point downwards, i.e. travel[1] >= 0. Travel is "the direction the light travels", so a
	// non-negative y is a sun at or below the horizon lighting the level from underneath.
	//
	// Round 24. This is a latch guard, not a tuning knob. A solution is taken once per dome albedo
	// and never revised, so a single bad derivation aims that level's sun wrongly for the entire
	// session - the exact failure mode four of round 23's six defects shared. MEASURED: the Selva
	// dome D1A6D1B27ADE6232 derived travel=[0.58892 0.11312 -0.80024] on this build, a POSITIVE y,
	// against the working dome's [-0.19933 -0.38269 0.90212]. With this guard the bad solve is
	// refused and SUNDIR keeps the level, which is strictly better than a permanent wrong aim.
	// 0 accepts any direction (the round-23 behaviour). SUNMAP is NOT subject to this: a
	// hand-written vector is the user's explicit instruction and is never second-guessed.
	bool sun_sky_downward_required();

	// --- round 23: force a listed vertex program onto the 2D compositor -----------------------
	// RPCS3_REMIX_UIFORCEVP=<vp>[,<vp>...] (bound 8). is_screen_space_draw() reads only the live
	// outer constant block and depth_write_enabled(), so a HUD program that is not statically
	// fingerprinted screen_space flips between the compositor and world geometry per draw. Listing
	// it here pins it to the compositor whenever depth writes are off.
	//
	// MEASURED (round-22 run): the nectar/health gauge program 2f64c2f8ffd6add1 takes BOTH paths in
	// one session - 277 'Remix uiwrap: route=2d' lines AND 'Remix sky-census:' rows placing it as
	// world geometry 1.5 units in front of the eye with raw vertices already inside the NDC cube.
	// The world placement is what makes it clip through geometry, rescale with the camera, and come
	// out white (VCOLMOD=0 means the world path never replays its vertex colour).
	bool ui_force_vp_matches(u64 vp_hash);
	u32 ui_force_vp_count();

	// --- ROUND 52: let UIFORCEVP admit a DEPTH-WRITING draw of a listed program ------------------
	// RPCS3_REMIX_UIFORCEVPDW, default 0 (off). When 1, the override above drops its
	// !depth_write_enabled() term for programs already on the UIFORCEVP list, and nothing else.
	//
	// WHY IT IS NEEDED, MEASURED ON Eat Lead (BLUS30267). Its menu text is drawn by
	// vp=f2b6988e84056628 with depth_write=1 ('Remix kil: vp=f2b6988e84056628 fp=53c93dc604379134
	// ... depth_write=1 vtx=76'), so is_screen_space_draw()'s `is_orthographic(outer) &&
	// !depth_write_enabled()` clause refuses it and the text travels the WORLD path - which is the
	// "the UI looks world space" half of the report, and it persists with the compositor on. The
	// existing UIFORCEVP override cannot rescue it either: that override carries the same
	// !depth_write term.
	//
	// WHY IT IS A KNOB PLUS A PER-TITLE LIST AND NOT A CLASSIFIER CHANGE. The !depth_write guard
	// exists so an orthographic DEPTH-WRITING world pass on another title is not mistaken for UI. It
	// stays. This only relaxes it for programs a conf file has already named, so the blast radius is
	// exactly the listed programs on the listed title and 0 everywhere else.
	//
	// Counter ui_forced_dw - draws admitted ONLY because of this, i.e. listed AND depth-writing - so
	// it is structurally 0 at the default and any non-zero value is proof the knob is armed and
	// doing work. Banner/live field uiforcevpdw=.
	bool ui_force_vp_depth_write_enabled();

	// --- ROUND 59: UIFORCEVPDW as a MODE, and the pre-world term -----------------------------------
	// RPCS3_REMIX_UIFORCEVPDW is read as a value, not a flag, by this accessor only. 0 and 1 are
	// unchanged - ui_force_vp_depth_write_enabled() above is still `value != 0`, so every reader
	// that existed before round 59 sees exactly what it saw. The new value is:
	//
	//   2 = the relaxation admits a depth-writing LISTED draw only after the frame's first world
	//       draw (m_frame_world_draws > 0). A listed program that draws BEFORE the scene keeps the
	//       routing it has today - it stays on the world path, it is not dropped.
	//
	// MEASURED, Eat Lead (BLUS30267). Every 'Remix ui-route:' row of the DXT1 backdrop
	// fp=0bde04241aed1c6a reads world_before=0 - 13/13 across the round-52 menu and gameplay
	// censuses, and 8/8 again on the 2026-09-10 menu run - while the menu text and HUD read 12..473.
	// The pre-world term is what keeps that backdrop on the world path when its vertex program is
	// force-listed. Its fragment program (bin\remix_ucode\95E97D9D65A7607F.fp) is the
	// texcoord_modulate shape, i.e. a full-frame OPAQUE background:
	//
	//    0: MOV R0.xyzw <- f[5](tc1)      ; R0 = TEX1 = v0, the vertex colour
	//   10: TEX R1.xyz  <- f[4](tc0) tex0
	//   11: MUL R0.xyz  <- R0, R1         ; rgb   = TEX1.rgb * tex0.rgb
	//   12: MUL R0.w    <- R2.x, {1,1,1,1} END   ; alpha = TEX1.w
	//
	// Composited, that draw is an opaque screen-filling wall painted OVER the Remix image, which is
	// what mode 2 exists to prevent without dropping anything.
	//
	// The term is applied to the RELAXATION only, never to the plain orthographic classifier: a
	// pre-world term in the classifier would push every 2D-only frame's draws (splash screens,
	// loading screens) onto the world path on every title.
	//
	// Counter ui_forced_dw_preworld - listed, depth-writing, would have been admitted at mode 1,
	// refused because the frame has no world yet. Structurally 0 unless the mode is 2 AND
	// RPCS3_REMIX_UIFORCEVP names a program, so a non-zero value is proof the term is doing work.
	u32 ui_force_vp_depth_write_mode();

	// RPCS3_REMIX_UIFORCEONLY=1: allow only explicitly forced UI programs/pairs into the guest
	// compositor. Automatically classified screen-space draws retain the normal skip path.
	bool ui_force_only_enabled();

	// --- ROUND 52: the tint a 2D program routes through a TEXCOORD varying -----------------------
	// RPCS3_REMIX_UITINTTEXCOORD, DEFAULT 1. Gates BOTH halves of the round-52 tint fix, so 0
	// restores the previous behaviour bit-exactly on the classifier as well as on the compositor:
	//
	//   * scan_fragment_program's two texcoord_* arms (fp_out_source above). With this at 0 those
	//     arms never run, out_tint_texcoord stays 0xff, and every affected program falls back into
	//     'other' with its round-41 'Remix fpother:' srckind census populated exactly as before.
	//   * composite_ui_draw's colour attribute. With this at 1 and a classified program, the tint
	//     comes from vp_fingerprint::texcoord_input[out_tint_texcoord] instead of the hardcoded
	//     ATTR3; with it at 0 the compositor reads ATTR3 and nothing else, as it always has.
	//
	// Eat Lead's menu title is authored LIGHT GREY - 'Remix vtxattr: vp=f2b6988e84056628 ...
	// a0=ub4[len=1.221/1.221/1.221 ...]', i.e. v0 ~ (0.705, 0.705, 0.705, a) - and the compositor
	// drew it WHITE ('Remix ui-biggest: ... tint=FFFFFFFF') because ATTR3 is not where that
	// program's colour lives. Every full-screen element inherited alpha 1 from the same default,
	// which is the opaque wall.
	//
	// env_u32(..., 1) != 0 rather than env_flag: a default-on knob has to be able to express "=0".
	bool ui_tint_texcoord_enabled();

	// --- ROUND 52: replay the program's own texcoord scale on the 2D path ------------------------
	// RPCS3_REMIX_UIUVUCODE, DEFAULT 1. composite_ui_draw computes u = attribute * uv_scale, where
	// uv_scale is 1 unless the texture is CELL_GCM_TEXTURE_UN - it has never applied the
	// texcoord_scale_slot / texcoord_affine forms the 3D path replays (uv_ucode=678129 in the
	// stats). Eat Lead's 2D program states one outright:
	//     0: VEC MUL o[7].xy <- v2.xyxx, c[466].xxxx      (F2B6988E84056628.vp)
	// so its TEX0 is the raw ATTR2 scaled by c[466].x, and this backend was sampling the atlas with
	// the unscaled attribute.
	//
	// Applied BEFORE the UN texel normalisation, which is the order the hardware uses: the program's
	// multiply happens in the vertex shader and CELL_GCM_TEXTURE_UN is a sampler property.
	//
	// Counters ui_uv_ucode (draws that folded a stated scale) and ui_uv_ucode_refused (the program
	// named a slot but it could not be read, or the coefficients were not finite - the raw attribute
	// is kept and the draw is counted rather than guessed at). 0 restores today's arithmetic
	// bit-exactly and is the one-relaunch bisect for any UI that changes size.
	bool ui_uv_ucode_enabled();

	// --- ROUND 52: store the ucode of a NAMED fragment program -----------------------------------
	// RPCS3_REMIX_UCODESTOREFPHASH=<fp>[,<fp>...] (bound 8, base 16, same parser as UIFORCEVP).
	//
	// store_refused_fp_ucode() only ever keeps UNTEXTURED programs the classifier could not name
	// (`!material` at the call site, `out_rgb_source == other` inside), which is the right scope for
	// the population it was built for and useless for a TEXTURED program whose shape has to be read
	// offline. A listed hash bypasses both gates and is additionally stored BEFORE the 2D/world
	// split, because a composited draw returns long before the untextured store site is reached.
	//
	// The file name is unchanged - the fp32 export name, i.e. the log's hash xor
	// 0x9e3779b97f4a7c15 - so a stored program lands beside every other one in bin\remix_ucode\.
	// Counter: the existing ucode_fp_stored. Diagnostic only; refuses nothing and moves no pixel.
	bool ucode_store_fp_hash_matches(u64 fp_hash);
	u32 ucode_store_fp_hash_count();

	// --- ROUND 59: fragment programs that are never UI ---------------------------------------------
	// RPCS3_REMIX_UIREFUSEFP=<fp>[,<fp>...] (bound 8, base 16, the same parser as UIFORCEVP and
	// UCODESTOREFPHASH). Default EMPTY, so the knob is structurally inert until a conf names a
	// program.
	//
	// WHAT IT IS FOR. A full-frame draw the compositor can only place ON TOP of the finished Remix
	// image - a background, a "code wall", a copy of the scene produced by a blit the render-target
	// gate cannot see. Compositing one of those hides the ray-traced frame behind it. Refusing it in
	// composite_ui_draw is exactly what RPCS3_REMIX_NOUI=1 already does to a depth-off 2D draw
	// (skip_screen_space), so a listed draw keeps today's picture rather than gaining a new one.
	//
	// KEYED ON THE FRAGMENT PROGRAM, not the albedo: the code wall's content hash rotates through
	// five values on a ~121-frame cycle (round-51 'Remix texstale:' rows) and a scene copy's changes
	// every frame, so an albedo list could never hold it. The FP is stable.
	//
	// The refusal happens AFTER the 'Remix ui-route:' census row and after the UIDUMP BMP, so a
	// refused draw is still fully described in the log - and BEFORE the triangle loop and before
	// ++ui_draws, so ui_draws keeps meaning "rasterised". Census reason=refusedfp; counter
	// ui_refused_fp.
	bool ui_refuse_fp_matches(u64 fp_hash);
	u32 ui_refuse_fp_count();

	// --- round 36: the same route keyed on a (vp, fp) PAIR, for the helmet -----------------------
	//
	// RPCS3_REMIX_UIFORCEPAIRVP + RPCS3_REMIX_UIFORCEPAIRFP, both hex, both default empty, and an
	// empty EITHER half disarms the route - the same rule hide_pair_matches() uses, and the same
	// one-line revert.
	//
	// WHY A PAIR AND NOT THE EXISTING LIST. The user has asked four times for the helmet to be
	// treated as UI so it stops casting a big shadow and clipping through geometry, and the pick
	// that finally isolates it is
	//   vp=830d7d1b9681c475 fp=479890ff55f1d96e albedo=C61753D31FB96507 vtx=59 extent=6.465
	//   depth_test=0 depth_write=0 blend=1
	// MEASURED over the last 400 MB of bin\remix_dump.log (1,038,135 lines):
	//   - vp=830d7d1b9681c475 alone appears on 47,399 lines across 31 distinct fragment programs,
	//     and its two largest fps carry 230 and 193 distinct vertex counts. It is a general-purpose
	//     program covering most of the scene. Adding it to RPCS3_REMIX_UIFORCEVP is a ~26x
	//     over-match and is round 20's failure mode exactly.
	//   - the PAIR (830d7d1b9681c475, 479890ff55f1d96e) appears on 1,833 lines, 1,767 of them
	//     (96.4%) at albedo C61753D31FB96507 / vtx=59 - the helmet. The 3.6% residue is 35 lines of
	//     a vtx=152 family at extent ~2.2 and 9 sub-0.13-extent quads at vtx 6/14/44/66.
	//
	// WHY THE UI ROUTE AND NOT A CATEGORY FLAG, decided on evidence rather than preference. Round 35
	// read the deployed runtime's source and established that there is NO per-instance castShadow
	// flag anywhere in it; Hidden sets mask = 0 and removes the instance from PRIMARY rays too (so
	// the helmet would vanish, failing "visible"); and THIRD_PERSON_PLAYER_MODEL needs two
	// rtx.conf lines, one of which (rtx.playerModel.enableInPrimarySpace = True) masks every
	// VIEW_MODEL candidate to zero and would take the arms with it - which collides head-on with
	// the round-36 viewmodel work. The UI-force route needs none of that: a forced draw returns
	// from the screen-space block BEFORE per_draw_transform and submit_subdraw, so no mesh, no
	// instance, no material and no BLAS are ever created. No shadow and no world clipping by
	// construction, not by flag.
	//
	// The existing guard still applies and is doing real work here: a listed draw is forced only
	// when depth_write_enabled() is false, and the helmet pick reads depth_write=0. Any member of
	// the pair that writes depth is left on the world path untouched.
	//
	// THE RISK, stated so it is not a surprise: the compositor can still refuse a forced draw
	// (ui_skipped / ui_space_none / ui_render_target) and a refusal DELETES the draw rather than
	// falling back to world geometry. If the helmet DISAPPEARS instead of flattening, that is what
	// happened - read those three counters, not this one. ui_forced_pair counts only the forcing.
	bool ui_force_pair_matches(u64 vp_hash, u64 fp_hash);
	u64 ui_force_pair_vp_hash();
	u64 ui_force_pair_fp_hash();

	// --- round 37 ---------------------------------------------------------------------------------
	// RPCS3_REMIX_UIFORCEPAIRVP2 / UIFORCEPAIRFP2: comma-separated lists of ADDITIONAL (vp, fp)
	// pairs for the same route, matched POSITIONALLY - slot i against slot i, never crossed. Up to
	// eight pairs. Empty (the default) means this is exactly round 36's single-pair behaviour.
	//
	// Why it was needed, MEASURED on the round-36 play-test log and not inferred: the route WORKS -
	// ui_forced_pair=14664 at exactly one per frame over four consecutive stats intervals, 138
	// 'Remix uiwrap: ... route=2d' lines for the helmet's albedo, zero compositor refusals since
	// frame 9678 - but the helmet albedo C61753D31FB96507 is submitted by THREE (vp, fp) pairs and a
	// one-slot key held one of them. The other two, f39f504649b6f442/4afa02b3dbbe9b7e (33 lines, all
	// of them that albedo) and 830d7d1b9681c475/609a4216b89e296a (2 lines), reached DrawInstance as
	// world geometry. That is the clipping.
	//
	// PRE-REGISTERED, and each of these can read otherwise:
	//   ui_forced_pair on 'Remix stats:' must rise from ~1/frame to ~2/frame. It is per sub-draw, so
	//   the arithmetic is checkable against the frame delta on two consecutive stats lines - if it
	//   stays at 1/frame the extra list did not parse, and uiforcepairs= on the knobs line says so.
	//   The helmet must stop clipping AND must stay visible. If it VANISHES, the compositor refused
	//   the new pairs - read ui_skipped, which has been frozen at 5077 since frame 9678 and would
	//   have to move for that to be the explanation.
	//   The caveat worth stating before it is seen: the existing forced pair's uiwrap lines read
	//   u=[2676..30089] v=[3417..28455] on a 512x512 texture, three orders of magnitude outside
	//   [0,1]. Whatever the compositor paints for the helmet today is not a sane atlas region, so
	//   expect the newly-routed copies to look like the composited one, not like the world one.
	bool ui_force_pair_any_matches(u64 vp_hash, u64 fp_hash);
	u32 ui_force_pair_extra_count();

	// ---------------------------------------------------------------------------------------
	// round 10
	// ---------------------------------------------------------------------------------------
	//
	// RPCS3_REMIX_CAMCLIPGATE=1 (default): with a camera lock configured, refuse any candidate
	// whose surface clip is not the same as, exactly double, or exactly half the session's
	// main-clip reference.
	//
	// This is the "whole scene very far away, like looking down into another portal" fix, and the
	// mechanism is named rather than guessed. The LOCK PROGRAM ITSELF draws Haze's 2048x2048
	// shadow pass, so the shadow variant is a legitimate *primary* candidate:
	//   Remix cam-elect: vp=7f3d3abcefc8b057 surf=0x0 clip=2048x2048 src=resolved candidates=17
	//                    cam=[-35.4 31.95 -32.05] frame=23022
	// It won at frame 23022 with the camera 30 units above a player whose gameplay frames read
	// y=1.6-2.5, and held until 24426. A cutscene-run tally of elected identities reads 10x
	// 7f3d @512x288, 8x ad7c @1024x576, 2x 7f3d @surf=0x0 2048x2048 - it wins repeatedly whenever
	// the gameplay variants pause and the 12-frame confirm is satisfied.
	//
	// The rule is not new: it is the same same/double/half compatible_surface test the world-draw
	// path already trusts at the skip_camera_surface site, moved one level up so it decides which
	// candidate may be ELECTED rather than only which draws may follow an elected one. Against a
	// 1024x576 reference, 2048x2048 fails (2048 = 2x1024 but 2048 != 2x576), while 512x288 (half)
	// and 1024x576 (same) both pass. The reference is the SESSION main clip, never a hardcode; if
	// it is unset the active camera's clip is used, and if both are unset every candidate is
	// admitted, because a boot frame must not be camera-less.
	//
	// The gate sits at the head of consider_camera_candidate, above the lock/fallback filter and
	// therefore above everything downstream of it - including the split-failure recovery path,
	// which runs inside candidate consideration and so inherits the gate.
	// 0 restores today's admission bit-exactly. Counter: cam_clipgate_refused.
	// Census: 'Remix cam-clipgate:'.
	//
	// ROUND 52 adds MODE 2: run the same ladder with NO lock configured. Eat Lead cannot use a
	// lock at all (no VP exceeds 60.6% frame presence; pinning one was measured live as
	// cam_resolved=0, 100% fallback, a black screen), and its 128x128 fov=90 aspect=1.0 near=0.01
	// cube-face pass wins the vote outright - 14 frames of run 13 including the FIRST camera of
	// the run (frame 931, candidates=1, the camera 30 units off). score_perspective's
	// square-aspect refusal cannot catch it: that test is fed the candidate's OWN clip aspect,
	// which is 1.0 for a square pass, so it is structurally disabled for exactly the pass it was
	// written for. Refusal on clip is. 155 of the 157 128x128 rows are on target 31, the same
	// target as the main pass, so a target rule would not separate them either.
	u32 camera_clip_gate_mode();
	bool camera_clip_gate_enabled();

	// RPCS3_REMIX_CAMFBRELATCH=1 (default): when the active camera is the LOCK program's and the
	// frame's winning candidate is a FALLBACK-program candidate that is pose-continuous with it,
	// refresh the active camera's matrices from that candidate instead of entering the pending-
	// switch machinery.
	//
	// This is the dev-menu camera-type flicker. Haze's lock program 7F3D3ABCEFC8B057 stops
	// producing camera candidates for stretches (its matrices become numerically unsplittable near
	// the vertical view, and the split-failure recovery additionally requires the surface to match
	// the ACTIVE camera's - so once the election has flipped away, the recovery is locked out).
	// AD7CE9D672A0BF6B then wins frames, and because the two express the same pose on different
	// surfaces (512x288 aux vs 1024x576 main, positions within 0.2 units), camera_source_changed
	// treats every handover as a full identity change needing 12 confirm frames. Measured: 11
	// cam-elect lines in one run alternating exactly those two identities, flips >=40 frames apart,
	// candidates=1 throughout (which the lock filter guarantees in either outcome and which
	// therefore does NOT mean only one pass drew).
	//
	// The relatch is the existing mid-frame relatch contract applied to the one case it excludes:
	// matrices only (view / projection / reference_inverse / position / latch_frame), never the
	// identity, never the age, never the vote. A fallback candidate that is NOT pose-continuous -
	// a real cut while the lock is absent - falls through to today's confirm-and-switch unchanged.
	// 0 restores the flicker. Counter: cam_fallback_relatch.
	bool camera_fallback_relatch_enabled();

	// RPCS3_REMIX_CAMSTICKY=1 (default 0 = off): identity hysteresis in the NO-LOCK vote. A
	// cluster that is the active camera's own cluster - the mid-frame relatch's predicate,
	// evaluated at CAMRELATCHTOL - outranks any cluster that is not, regardless of votes. Among
	// equals the observation count then the score decide exactly as today, and a frame with no own
	// cluster elects by vote exactly as today, so a real cut still goes through the existing
	// 12-frame confirm. The lock path is untouched: this is that path's own
	// continuous_with_active_primary rank, generalised.
	//
	// Measured on Eat Lead run 13: 72 identity changes in 4,806 gameplay frames (15 per 1,000,
	// winners 10-25 units apart), 54 of them with a cluster continuous with the previous winner
	// present and simply out-voted by a mean margin of 2.3 votes, 42 with one within 0.1.
	//
	// Degradation on a lens change: the aim-down-sights sweep 35.983 -> 28.120 moves the
	// projection about 0.3% per frame, under the 1e-2 projection tolerance, so the incumbent
	// follows it; only the sweep's first step (35.983 -> 35.307, delta ~0.0125) exceeds it, for
	// exactly one frame, in which sticky falls back to the vote - today's behaviour, and today's
	// relatch already skips that frame.
	//
	// ARM ONLY WITH CAMRELATCHTOL BELOW 0.8. With the relatch still taking props, "own cluster"
	// would be measured against a prop-relatched view and the hysteresis would lock onto the prop.
	// Counters: cam_sticky_kept (an own cluster elected over a cluster with strictly more votes),
	// cam_sticky_lost (the frame had clusters but no own cluster, so the vote moved the identity).
	bool camera_sticky_enabled();

	// --- round 53 ---------------------------------------------------------------------------------

	// RPCS3_REMIX_CAMTRACK=1 (default 0 = off): CONTINUITY OF IDENTITY OVER TIME. A bounded set of
	// camera tracks is maintained at flip from the frame's cluster census; the incumbent track keeps
	// the elected identity for as long as its object is drawn, and when it dies the matched track
	// with the greatest tenure takes over.
	//
	// This exists because no per-frame RANK can fix Eat Lead. Read with a position-continuity
	// tracker, run 13's gameplay window (frames 2259-7026, 4,761 contested frames, 83,257 cluster
	// rows) says the winner track changed 84 times and IN 80 OF THE 84 THE PREVIOUS WINNER'S TRACK
	// WAS ABSENT FROM THE FRAME - only 4 were an incumbent present and out-voted, which is the case
	// CAMSTICKY already addresses. Every cluster is split(M_obj x V x P), i.e. the true view with
	// some placed object's model matrix folded in, and no cluster is the bare V x P: the most
	// persistent track is present in 1,466 of 4,761 frames (30.8%). There is no "true camera"
	// cluster to prefer, only a choice of WITNESS OBJECT, and the identity moves whenever the
	// current witness is culled.
	//
	// Simulated on run 13's logged clusters: 16 handovers in 4,761 frames (3.4 per 1,000) against
	// 84, incumbent duration p50 252 / p90 827 / max 1,021 frames. The vote disagreed with the
	// tenure incumbent on 55.7% of frames, which is why this is a knob with counters and not a
	// tweak of the rank. 0 restores the vote bit-exactly.
	//
	// Scoped OFF whenever a camera lock is configured: a title with a lock keeps its lock path.
	// Counters: cam_track_kept / cam_track_override / cam_track_missed / cam_track_handover /
	// cam_track_reset / cam_track_expired / cam_track_overflow. Census: 'Remix cam-track:'.
	u32 camera_track_mode();

	// RPCS3_REMIX_CAMTRACKPOS (default 1.0): how far a cluster's implied eye may move in one frame
	// and still be the same witness object. 1.0 world unit is 1.35x the largest per-frame step of
	// any long track in run 13 (p99 0.12-0.28, max 0.74) and at least 10x under the 10-30-unit
	// separation of 47 of the 84 winner jumps, so it is loose enough for a flick turn and tight
	// enough that a handover can never be mistaken for continuity.
	f32 camera_track_position_tolerance();

	// RPCS3_REMIX_CAMTRACKMISS (default 3): how many consecutive frames the incumbent's track may go
	// unmatched before it dies. 3 bridged 14 of the 16 simulated handovers; 8 bridged 11 of 13, i.e.
	// the extra five frames buy nothing and lengthen every hold.
	u32 camera_track_miss_frames();

	// RPCS3_REMIX_CAMREBASE=1 (default 0 = off, 2 reserved): CARRY THE BASIS ACROSS A HANDOVER. A
	// persistent rigid transform R (submitted view = R x raw view) is composed at each bridged
	// handover from the relation the outgoing and incoming witness expressed while they were
	// co-present: for two static witnesses C = V_new x V_old^-1 = M_new x M_old^-1, in which the
	// true camera cancels, so it is measurable in any frame where both are drawn.
	//
	// reference_inverse and view_proj_inverse are re-expressed with R^-1 at the same time, so
	// world' = fused x ref_inv x R^-1 and world' x (R x V) x P = fused exactly - the composite the
	// whole backend rests on is preserved to the bit. Continuity predicates compare RAW views, so a
	// handover reads as continuous at the flip discontinuity test and the 12-frame confirm stops
	// firing on it, which removes ~12 stale frames per handover as a side effect.
	//
	// A bridge whose self-check residual exceeds the pending-match tolerance is refused and counted
	// (cam_rebase_refused); a handover with no co-present predecessor resets R to the identity
	// (cam_rebase_reset) and falls through to today's confirm. Mode 2 - acting on a witness that is
	// itself moving - is DOCUMENTED, NOT IMPLEMENTED: this round only counts it
	// (cam_track_witness_moving / cam_track_incumbent_moving).
	//
	// Requires CAMTRACK. Preconditions: gauge_absent must be 100% on the title (the two gauge-path
	// writes of m_active_camera.view are a tripwire, cam_rebase_bypassed, not a supported route),
	// and absolute-world consumers such as AUTHOREDLIGHTS would need R^-1 folded into their
	// placement - they are off and are not adjusted here.
	u32 camera_rebase_mode();

	// RPCS3_REMIX_GLDRIFTMAT=1 (default 0 = off): print the RAW MATRICES behind every guest-light
	// drift row (round 54). MEASUREMENT ONLY - nothing here changes a matrix or a submission.
	//
	// A drift row states that the light moved and, as of round 54, whether the divisor
	// (m_active_camera.reference_inverse) is bit-identical to the one the light was last placed
	// under (ref_same). When it IS identical and the light still moved, the remaining two causes are
	// "the guest moved the fixture" and "the decode wobbled", and those are told apart by the shape
	// of the change: a guest move is a translation row only, a decode wobble is spread through the
	// entries. This knob emits 'Remix guest-light-drift-mat: id= frame= fused=[16 x %a] ref=[16 x %a]
	// fused_valid= world_branch=' immediately after each drift row, sharing the drift census's
	// 64-per-window cap. %a because narrowing to %g destroys exactly the residual being looked for.
	//
	// fused comes from m_ref_pick_fused, the current draw's own folded matrix, which is written on
	// the has_reference branch of per_draw_transform and reset per call; a lamp drawn through another
	// branch prints fused_valid=0 and increments gl_mat_unavailable, and the ref half is still
	// printed. 0 restores today's output bit-exactly.
	u32 guest_light_drift_matrices();

	// RPCS3_REMIX_CAMRELATCHTRACE=1 (default 0 = off): census of the MID-FRAME RELATCH (round 54).
	// MEASUREMENT ONLY - the relatch rule, its tolerance and its effect are untouched.
	//
	// The relatch is the only writer that refreshes m_active_camera between the flip latch and a
	// draw, so it decides which draws of a frame divide by a fresh reference and which by last
	// frame's - and, because it takes the first candidate of the frame inside CAMRELATCHTOL rather
	// than the tracker's incumbent, it can also refresh from a DIFFERENT OBJECT than the one the
	// witness tracker is following. Nothing logged either fact. At 1 this emits:
	//
	//   'Remix cam-relatch:'          one line per relatch - the raw eye it took, the step it applied
	//                                 to the submitted eye, inc_ok (was that draw within CAMTRACKPOS
	//                                 of the incumbent track), and ord (world draws already placed on
	//                                 the stale reference this frame).
	//   'Remix cam-relatch-disagree:' one line per frame in which the relatch's draw and the tracker's
	//                                 own match for the incumbent are further apart than 0.05 units -
	//                                 a READING THRESHOLD (the magnitude of s_camera_witness_tolerance
	//                                 and ~3x the p50 per-frame eye step), not a gate.
	//
	// Both capped 64 per stats window, the report_camera_track idiom. Counters cam_relatch_incumbent /
	// cam_relatch_foreign / cam_relatch_noinc partition cam_relatch exactly and accumulate whether or
	// not this is armed; cam_relatch_agree / cam_relatch_disagree partition the frames that had both a
	// relatch and a matched incumbent. 0 restores today's output bit-exactly.
	u32 camera_relatch_trace_mode();

	// RPCS3_REMIX_CAMREFSYNTH=0 (default) / 1 / 2: refresh the un-projection REFERENCE on a tracker
	// miss WITHOUT touching camera identity (round 55). Requires CAMTRACK; inert without it.
	//
	// The hold that update_camera_tracks takes when the incumbent witness is alive but not drawn this
	// frame freezes m_active_camera whole - the identity AND the matrices. Freezing the identity is
	// right; freezing reference_inverse is what makes the world and the injected lights slide by the
	// camera's motion for the length of the hold and then snap. The two are separable because two
	// static objects express a rigid relation that the true camera cancels out of:
	//
	//     rel = view_J x view_I^-1 = M_J x M_I^-1        (already stored as track.rel_first/rel_last)
	//     view_I            = rel^-1 x view_J            (this frame's camera, in I's basis)
	//     reference_inverse_I = reference_inverse_J x rel
	//
	// so a frame in which I is missing but J is drawn can still be placed in I's frame:
	// world = fused x (ref_inv_J x rel) = M_obj x M_J^-1 x M_J x M_I^-1 = M_obj x M_I^-1.
	//
	//   mode 1  synthesise the incumbent's candidate from the best qualifying co-present witness on a
	//           cam_track_missed flip and install it through the ordinary latch; AND make the mid-frame
	//           relatch refuse a candidate outside CAMTRACKPOS of a live incumbent
	//           (cam_relatch_foreign_refused) - on a synthesised frame every near candidate is a
	//           different object, which is the mechanism that hijacked the basis at run-54 frame 1568.
	//   mode 2  mode 1, plus: a flip whose candidate was elected by a tracker handover/reset skips the
	//           12-frame discontinuity confirm (cam_ref_synth_install). Under CAMTRACK that confirm can
	//           never restore the outgoing witness - it is dead, which is why the handover happened -
	//           so it is pure delay, at the price of one switch per handover instead of one per
	//           confirmed window.
	//
	// A synthesis that cannot be made holds EXACTLY as today and increments a named reason counter;
	// never a guessed matrix. cam_ref_synth + cam_ref_synth_nowitness + cam_ref_synth_short +
	// cam_ref_synth_rigid + cam_ref_synth_basis + cam_ref_synth_badrel == cam_track_missed while this
	// is non-zero (all six are 0 while it is 0). The refusals mean, in the order they are tested:
	// basis  = the active camera is not in the incumbent's basis (a confirm is pending on a new
	//          witness, or the incumbent's own basis was never installed);
	// nowitness = no other matched track shares the incumbent's identity keys and carries a relation
	//          anchored against THIS incumbent; short = one co-present frame only (no rigidity test is
	//          possible); rigid = rel_last vs rel_first moved more than s_camera_witness_tolerance (the
	//          "witness" was moving); badrel = the relation or the resulting view would not invert, or
	//          the bridge's own self-check failed.
	//
	// Identity is not written by any of this: m_camera_track_incumbent, track.position/view/missed,
	// track.moving, expiry and the tenure election are untouched, and the 'moving' verdict stays gated
	// on CAMREBASE so the election is bit-exact on the refsynth-only arm. NOTE cam_held changes meaning
	// here - a synthesised flip is not a hold, so cam_track_missed and cam_held stop counting the same
	// frame.
	//
	// Rows: 'Remix cam-synth:' one line per synthesis under CAMRELATCHTRACE, sharing the cam-relatch
	// 64-per-window cap. Fields refsrc= (the per-flip m_camera_ref_state) and switch= appear on every
	// 'Remix guest-light-drift:' row, refused= on every 'Remix cam-relatch:' row, and cam_basis_switch
	// / cam_discontinuity_held / cam_switch_confirmed on 'Remix live:' - on BOTH arms, because the
	// measurement has to read on the control. 0 restores today's output bit-exactly.
	u32 camera_ref_synth_mode();

	// RPCS3_REMIX_CAMXFLIP=1: un-mirror the SUBMITTED camera's right vector. Default 0.
	//
	// try_split_once does not recover the view, it CONSTRUCTS it. forward and up_hint come out of the
	// fused matrix by unprojection and carry no convention, but the basis is then closed with
	//
	//     right = up_hint x forward        (cross3(up_hint, forward, right))
	//     up    = forward x right
	//
	// and that cross-product ORDER is an assumption that the guest's world is left-handed. For a
	// right-handed world up x forward is screen-LEFT. The recovered projection P = viewToWorld x fused
	// then absorbs the error as a negative x-scale, so the product V x P - and therefore the rendered
	// image - is correct while NEITHER FACTOR IS. This is the "mirrored view + mirrored projection"
	// case: the mirror lives in the sign of P[0][0], never in the view's determinant.
	//
	// A determinant test on the view clears it and is wrong. right = up x forward makes
	// (right, up, forward) an orthonormal triple with det = +1 by construction on EVERY frame,
	// whatever the guest's handedness, which is why vdet= below reads 1 on both arms and p00= is the
	// field that names the bug.
	//
	// MEASURED without a rebuild, off score_perspective's -0.5 dock for m[0][0] < 0 (which makes an
	// integer score impossible once it fires): 'Remix camera trace:' rows read 8167 x score=5.500 +
	// 672 x score=4.500 on run pid27232 and 8475 x score=5.500 on run pid30572 - zero integer scores
	// in 17,314 resolved frames. P[0][0] < 0 on 100% of them; P[1][1] > 0 on all of them too, since a
	// -1.0 dock would have restored an integer.
	//
	// The symptom this explains: the runtime's free camera strafes along viewToWorld row 0
	// (RtCamera::getRight() is viewToWorld[0].xyz() verbatim) and ties yaw to the same row, while
	// pitch reads row 1 and forward/back row 2 - so a reversed row 0 reverses left/right for movement
	// AND turning and nothing else, which is exactly what was reported.
	//
	// Mode 1 inserts S = diag(-1,1,1,1) between the submitted V and P in submit_camera and nowhere
	// else: V' = V x S negates the view's COLUMN 0 (all four rows, translation included), P' = S x P
	// negates the projection's ROW 0. V'P' = V S S P = VP EXACTLY, so the image cannot change -
	// remixapi_SetupCamera applies the matrices verbatim, primary rays come from the inverse of the
	// product, and determineInstanceFlags' winding test reads viewToProjection x worldToView, the
	// product. What does change: viewToWorld' row 0 is the true screen-right (det -1, which is what
	// Remix's convention for a right-handed world under a left-handed projection is), and
	// P'[0][0] > 0. P[2][2], P[3][3], near, far and fov are untouched, so DecomposeProjection's
	// bLeftHanded = a22 > 0 still reads Left-handed and only the dev menu's 'Overall Handedness'
	// (isLHS ^ isMirrorTransform(viewToWorld)) flips - Left-handed today, Right-handed on the arm.
	//
	// Boundary-only by construction: it is applied after both to_camera_matrix copies, so
	// m_active_camera is never written and the split, the tracker, the relatch, the rebase, the
	// reference synthesis, the drift census and every light placement never see it. Every internal
	// quantity is S-invariant anyway (view x projection = fused, reference_inverse = inverse(folded),
	// rel = view_J x view_I^-1, position = row 3 of inverse(view) - S negates only column 0 of V, so
	// inverse(V') = S x inverse(V) keeps row 3), which is why CAMTRACK / CAMREFSYNTH / CAMREBASE
	// cannot regress from it. The sky and viewmodel twins copy camera_info below the fix and inherit
	// it, and the twin's copysign then copies a POSITIVE sign.
	//
	// Counters: cam_xmirror counts every frame whose submitted world projection had [0][0] < 0 and
	// is printed on BOTH arms (the measurement has to read on the control); cam_xflip counts the
	// frames the flip was actually applied to, so cam_xmirror == cam_xflip == cam_resolved is the
	// pre-registered reading on this title with the knob armed and cam_xflip == 0 without it. Rows:
	// vdet= (the f64 determinant of the frame candidate's view 3x3) and p00= appear at the END of
	// every 'Remix camera trace:' row on both arms.
	//
	// NOT a fix of the root cause. try_split_once's cross-product order is where the mirror is born,
	// but every continuity predicate, relation table and rebase in three shipped rounds compares
	// views produced by that function, so changing it there moves internal state on every frame and
	// cannot be A/B'd at the boundary. This is exact and complete for the API; the split is a later
	// round with its own invariance proof. 0 restores today's output bit-exactly.
	bool camera_xflip_enabled();

	// RPCS3_REMIX_PICKFRESH=1: measure the POSE ERROR of a followed object, not just its per-frame
	// delta. Default 0.
	//
	// The arithmetic, in three sentences. A world draw is placed as
	// world = OS x (fused x reference_inverse), and the frame's own reference_inverse only exists
	// once the frame's first camera-carrying draw reaches consider_camera_candidate and the
	// mid-frame relatch installs it, so every world draw before that ordinal is divided by the
	// FLIP LATCH OF FRAME N-1 and lands at OS x M x Q_N with Q_N = V_N x V_{N-1}^-1, the camera's
	// own frame-to-frame rigid motion. For a turn of dtheta about the eye that displaces a prop at
	// eye distance r by about r x dtheta, and for a translation t of the eye by -t; a draw that
	// arrives AFTER the relatch cancels exactly (fused x (V_N P_N)^-1 = M) and cannot move at all.
	// MEASURED on the frozen pid-28088 slice before any of this was built: the two props that
	// visibly swim are age=1 (drawn pre-relatch) on 298 of 298 rows and print d_origin=0 EXACTLY on
	// the frames whose cam-relatch step is 0, while the three quiet objects are level chunks at the
	// world origin and age=0 on 93% of theirs.
	//
	// What it emits. At the followed draw, per_draw_transform stashes the three operands the live
	// path actually used (fused, the object-space composite obtained by running the very same
	// prepend_object_space lambda on an identity, and *reference - the divisor, not the member).
	// At flip, after flush_deferred_at_flip and BEFORE apply_gauge_anchor_camera overwrites
	// m_active_camera, that same fused is re-divided by the frame's OWN relatched reference and one
	// 'Remix pick-fresh:' row is written beside the frame's 'Remix pick-follow:' row, carrying
	// origin (as submitted), origin_fresh, err and its vector, d_fresh, r, cam_step, cam_rot,
	// recheck, fresh, and the draw ordinal against the relatch ordinal. err is the quantity the
	// user SEES; d_origin on the follow row is only its derivative, which is why the visible band
	// sits several times the camera's translation step on the same frames.
	//
	// The recheck guarantee. recheck re-divides the SAME stashed fused by the SAME stashed (stale)
	// reference with the same operands in the same order, and must reproduce the submitted origin
	// EXACTLY - it is the built-in proof that the recomputation is the live arithmetic and not a
	// second, subtly different copy of it. pf_recheck_fail counts any row over 1e-5; if it is
	// non-zero, err is not readable and the missing operator has to be stashed too. The second
	// built-in control is age: on an age=0 draw the stale and the fresh reference are the same
	// matrix, so err must be exactly 0.
	//
	// Counters on 'Remix live:': pf_rows / pf_first / pf_stale / pf_fresh / pf_step0..4 /
	// pf_age_unknown / pf_rot_degenerate for the follow row, and pf_fresh_rows / pf_norelatch /
	// pf_recheck_fail / pf_place_refused / pf_err0..4 for the pick-fresh row. Two partitions hold
	// by construction and are the acceptance check, not a hope: pf_stale + pf_fresh == pf_rows,
	// pf_first + pf_step0..4 == pf_rows, and pf_norelatch + pf_place_refused + pf_err0..4 ==
	// pf_fresh_rows.
	//
	// The five fields appended to the pick-follow row (age, ord, relatched, d_rot, cpos) print on
	// BOTH arms - a reader should not have to infer the reference age from two frame numbers - but
	// the stash, the re-division and the pick-fresh row are all behind this knob.
	// 0 restores today's output bit-exactly.
	bool pick_fresh_enabled();

	// RPCS3_REMIX_DEFERPRERELATCH=1: hold a world draw that would be divided by a STALE reference
	// and flush it at the frame's mid-frame relatch, re-divided by the fresh one. Default 0.
	//
	// This is the exact fix for the mechanism PICKFRESH measures, and it is a fix rather than a
	// prediction: the held draw is re-placed with the SAME arithmetic a draw that arrived after the
	// relatch would have used (OS x (fused x reference_inverse), normalise, to_remix_transform), so
	// there is nothing to extrapolate and no residual by construction. Extrapolating the reference
	// instead (Q_pred = V_{N-1} V_{N-2}^-1) is a guess where this is exact, and is deliberately not
	// what this does.
	//
	// The machinery already exists and is already proven: deferred_instance / submit_deferred /
	// flush_deferred_for_anchor / flush_deferred_at_flip were root-caused, shipped and CONFIRMED by
	// the user to stop props sliding on Haze, then switched off for cost (MEASURED ~14 fps at
	// ~50 held instances per frame, 72% of which did no work). On this title DEFERPREANCHOR is armed
	// but structurally inert - defer_absent_declined=3087767, defer_buffered=0 - because it keys on
	// gauge anchors this title never produces. This knob keys on the RELATCH instead: the population
	// is world_ref_stale, MEASURED at 207,124 of 3,087,767 world draws (6.7%, about 12.5 per frame,
	// against a relatch-ordinal median of 12), and every held draw does useful work.
	//
	// What it changes, and why it therefore ships OFF. Submit ORDER: those ~12 instances per frame
	// leave their draw ordinal and go out at the relatch ordinal instead. Nothing else about them
	// changes - the same InstanceInfo, the same pNext chain, the same picking value - but a title
	// that depends on submission order for decals or blended layers could show it, and that is not
	// decidable from a log. So it is an A/B: run PICKFRESH=1 alone first, then the same protocol
	// with this armed, and compare within each run.
	//
	// Counters defer_relatch_armed / defer_relatch_flushed / defer_relatch_vmop, with two partitions:
	// defer_relatch_armed + defer_absent_declined == gauge_anchor_absent, and
	// defer_buffered == defer_flushed_fresh + defer_relatch_flushed + defer_flushed_flip. The
	// within-run PROOF that the runtime received the fresh transform is on the pick-fresh row:
	// deferred=1 with d_deferred=0 EXACTLY, where d_deferred is the L1 between the transform the
	// flush rebuilt and the origin_fresh this instrument computed independently at flip;
	// pf_deferred_mismatch counts any row where those two disagree. Leftovers on a frame that never
	// relatched go out through the existing flush_deferred_at_flip with the transform they carry,
	// which is today's behaviour one flush later, so no draw is ever lost.
	// 0 restores today's output bit-exactly.
	bool defer_pre_relatch_enabled();

	// RPCS3_REMIX_ANCHORSTICKY=1 (default): elect the per-key gauge anchor by CONTINUITY with its
	// own previous frame instead of by draw order.
	//
	// This is Selva's "geometry follows the camera / the land carrier and its cargo move in
	// different frames". capture_gauge_anchor is first-draw-wins per (surface, target, clip);
	// gauge_anchor_contested read 462,080 when the lead was queued and 550,394 at close-out (vs ~0
	// in prior runs), and all 61 'Remix gauge-contested:' lines in the Selva run are ONE pair on
	// the main key: holder BD1C10DF5703E559 (a genuine identity-world donor - absolute world-span
	// strips, x from -1373 to +1113, and a worldvp probe showing its divided world = identity to
	// 1e-15) against contender AD7CE9D672A0BF6B, which is ALSO on WORLDIDENTITYVP but demonstrably
	// submits non-identity draws here (deltas 1.7-7.4 drifting over time - a moving object). The
	// gauge-keys census proves the main-key holder DOES change identity across a run.
	//
	// While BD1C draws first the tripwire is correctly refusing AD7C's donations. On frames where
	// BD1C is culled, AD7C's first draw takes the slot; when that draw is a non-identity one the
	// whole frame's divide gauge becomes a prop-relative frame - world = fused x wrong_ref^-1
	// leaves a camera-correlated residue on every draw, so geometry tracks the viewpoint while the
	// sun (a Remix light, world-anchored by the runtime) stays put - and draws issued before the
	// frame's first identity draw divide by the PREVIOUS frame's anchor, putting two references
	// inside one frame, which is the carrier/cargo split.
	//
	// With the knob on: a first same-key candidate that disagrees with the slot's previous-frame
	// gauge beyond the discontinuity tolerance is PARKED rather than installed; a later same-key
	// candidate that IS continuous installs normally; and at flip, any key that received no
	// continuous donor promotes its parked candidate, so a real cut converges in one frame. In the
	// gap the divide's existing previous-frame anchor path and GAUGECAMHOLD cover lookups, which is
	// exactly today's behaviour for anchor-less frames. The contested tripwire keeps counting as
	// now - it is measuring AD7C's bad donations, which this leaves refused.
	// 0 restores first-draw-wins bit-exactly. Counters: gauge_anchor_parked / _recaptured /
	// _promoted. Census: 'Remix anchor-elect:'.
	bool gauge_anchor_sticky_enabled();

	// RPCS3_REMIX_GAUGEDONORBEST (whole world units, 0 = OFF = round 43 byte for byte).
	//
	// ROUND 44b, and it replaces RPCS3_REMIX_GAUGEDONORMAXT, which is REMOVED FROM THE BUILD.
	//
	// THE DEFECT IS REAL AND IS NOT IN DOUBT. capture_gauge_anchor() installs a WORLDIDENTITYVP
	// draw as the whole frame's world gauge qualified on the vp-hash list alone - it never looks
	// at the draw's own placement, while the submit site's keep_resolved does exactly that against
	// WORLDIDMAXT. MEASURED: of 759 "Remix worldid-draw:" frames carrying two or more rows, 559
	// have EVERY row on one bit-identical pre-translation across unrelated meshes; "Remix gauge:
	// translation=" has mean 41.99 and max 1718.67 over 17,386 frames; and E40BF80AF519848A's raw
	// vertex box never moves while its transform swings 0 -> 21.75 -> 966.5 units.
	//
	// WHAT ROUND 44 GOT WRONG, and the rule that falls out of it. GAUGEDONORMAXT REFUSED an
	// off-origin candidate and handed it to the ANCHORSTICKY park, expecting promote_parked_anchors()
	// to recover. It does recover - AT FLIP, one frame late. MEASURED per flip, round 43 -> round 44:
	//   anchor_parked    0.0769 -> 0.1542    anchor_recap  0.0766 -> 0.0181
	//   anchor_promoted  0.00032 -> 0.1360   (+42,512%)   park outcomes 99.6%/0.4% -> 11.8%/88.2%
	//   share of placements on last frame's anchor: 19.22% -> 37.09%
	// The refused donor was installed anyway, one frame later, and the population placed by a stale
	// gauge nearly doubled. The user saw "the whole scene warps aggressively when turning camera",
	// which is a one-frame-old gauge under rotation: distance-from-eye x turn-per-frame, everywhere.
	// A GAUGE REMEDY MUST NEVER MAKE THE GAUGE LATER OR ABSENT. That constrains every future attempt.
	//
	// (Two further errors in that round's guard, recorded because they are the reusable part: the
	// threshold was set on an ABSOLUTE cumulative counter and compared across sessions of different
	// length - gauge_absent 57,424 vs 295,391 is 3.27/flip vs 5.24/flip - and even normalised it was
	// the WRONG COUNTER, because the failure route was park -> promote, so the gauge was never
	// absent, only late. anchor_promoted moved 425x and had no threshold on it.)
	//
	// WHAT THIS DOES INSTEAD. The frame's first donor installs IMMEDIATELY and unconditionally, as
	// today, so the gauge is never late and never absent. A LATER donor of the same frame may then
	// REPLACE it, but only when its own placement - measured against the FRAME REFERENCE, the gauge
	// held when the first donor arrived - is at least twice as close to the origin, and only when
	// the installed donor is itself beyond this threshold. First-draw-wins becomes best-draw-wins.
	// Nothing is parked, promoted or refused, so anchor_promoted structurally cannot move.
	// 75.0% of AD7CE9D672A0BF6B's 3,316,291 draws sit at |t| <= 1, so a good donor is usually
	// present in the frame; only the ORDER was wrong.
	//
	// Counters: gauge_donor_upgrade_avail (counted WHETHER OR NOT armed, so an unarmed run sizes the
	// route) and gauge_donor_upgraded. avail > 0 with upgraded == 0 means the knob is off.
	// PRE-REGISTERED REFUTATION: the gauge changes mid-frame, so draws submitted before the upgrade
	// used the old one. If the scene TEARS within a frame - part of the world offset from the rest,
	// props separating from the floor they stand on - that is this. 0 reverts it exactly.
	u32 gauge_donor_best();

	// RPCS3_REMIX_VCOLBGRA=1 (default): pack the replayed vertex colour in D3DCOLOR byte order
	// (B, G, R, A) to match the format the runtime actually binds it with.
	//
	// A byte-order bug, found by reading both sides. apply_vertex_colour packs
	// 'R | G<<8 | B<<16 | A<<24' - bytes R,G,B,A - and the runtime binds that field as
	// VK_FORMAT_B8G8R8A8_UNORM (fork dxvk-remix-ppsspp, rtx_remix_api.cpp: both color0Buffer
	// bindings use offsetof(remixapi_HardcodedVertex, color) with B8G8R8A8), i.e. bytes B,G,R,A.
	// Red and blue are therefore swapped in every replayed vertex colour, and Haze's amber HUD
	// gauges rendering "blue-ish" is that bug verbatim: amber has R >> B, which reads back as
	// B >> R = cyan. Greys are swap-invariant, which is why the nectar pulse's flat grey is a
	// different fault (see FPVCOLEMISSIVE).
	//
	// apply_vertex_alpha's 0x00FFFFFF white constant is swap-invariant and is untouched.
	// Note: mesh CONTENT hashes cover the vertex bytes, so the first run after this change
	// re-creates every coloured mesh once - expected, bounded, and visible as a one-time
	// mesh_created bump that then settles. 0 restores the swapped pack bit-exactly, which is the
	// A/B that attributes the gauges turning amber.
	bool vertex_colour_bgra_enabled();

	// RPCS3_REMIX_FPVCOLALPHAGATE=1 (default): apply the ALPHA halves of the round-9 FPVCOL replay
	// only when the guest actually consumes fragment alpha (blend enabled or alpha test enabled).
	//
	// Round 9's alpha half is unfaithful to the hardware on a draw where blend and alpha test are
	// both off: the guest ROP reads no fragment alpha at all there. It is also actively harmful.
	// The Selva effect census shows the giant-flower program submitting TEXTURED draws
	//   vp=c97cd1531ac480d8 fp=4f1d1bce7ffdfd9f class=vcol_modulate material=1 blend=0 dw=1
	//   vtx=649..1960 vcol=[rgb-varies=1 alpha=00..00]
	// and with textureAlphaArg2Source=VertexColor0 + Modulate the runtime opacity becomes
	// texA x 0 = 0. In the fork's calcOpaqueSurfaceMaterialOpacity
	// (opaque_surface_material_blending.slangh) a blending-DISABLED surface then takes
	// 'opacity = newAlpha > 0 ? 1 : 0' - zero - so the draw renders FULLY INVISIBLE. Opaque
	// vegetation carrying 0-alpha vertex colours has been invisible since the round-9 build.
	//
	// The RGB halves are untouched, and the KIL / A2C arms set their own alpha test downstream and
	// are untouched. 0 restores round 9's unconditional alpha replay. Counter: fpvcol_alpha_skipped.
	bool fp_vcol_alpha_gate_enabled();

	// RPCS3_REMIX_FPVCOLEMISSIVE=<float> (default 1.0, 0 = off): attach a self-illuminated twin of
	// the neutral material to a draw whose fragment program proves it is self-lit.
	//
	// The nectar pulse is not mis-coloured any more (round 9 fixed that) - it is UNLIT. The runtime
	// honours SelectArg1(VertexColor0) for albedo, so the pulse's albedo IS its vertex colour, but
	// albedo is reflectance and in a dark room reflectance reads grey. The decisive reading is from
	// the fork's own shader: the SAME fixed-function stage is applied a second time to
	// emissiveColor (opaque_surface_material_interaction.slangh drives
	// chooseTextureOperationColor(emissiveColor, ...) from the SAME textureColorArg sources), and
	// emissiveRadiance = emissiveBlendOverrideInfluence(1.0 default) x emissiveColor x
	// opaqueSurfaceMaterial.emissiveIntensity. With round 9's arg-source replay already shipped,
	// the ONLY missing ingredient is a material whose emissiveIntensity > 0 - the per-vertex
	// gradient then reaches emissive radiance intact, with no fork edit and no per-draw materials.
	//
	// Scoped to exactly the decoded shape: no guest material, fp classified vcol_pass, sampled_mask
	// == 0 (a program that samples nothing and writes 'MOV out, COL0' is by definition
	// self-illuminated), and the submitted vertex colours actually vary. The rgb-varies gate keeps
	// the flat fc0f sky-dome variant (vtx=32 rgb-varies=0) on today's path - a named exclusion.
	// The HUD gauges cannot use this lever (they are TEXTURED, with shared per-texture materials);
	// theirs is round 5's RPCS3_REMIX_EMISSIVE albedo list, applied at material creation.
	// 0 restores the plain neutral-grey attach. Counter: fpvcol_emissive. Census flag: selflit=.
	f32 fp_vcol_emissive();

	// RPCS3_REMIX_FPVCOLADDITIVE=1 (default): for exactly the FPVCOLEMISSIVE signature PLUS
	// depth_write disabled and guest blend disabled, state ONE/ONE ADD on the instance blend ext
	// instead of the opaque alias.
	//
	// The pulse arrives blend=0 dw=0 alpha=ff..ff because on PS3 the pass is composited offscreen;
	// replayed as opaque world geometry it would be a solid glowing wall. ONE/ONE is classified
	// BlendType::kEmissive by the runtime (calcOpaqueSurfaceMaterialOpacity's kEmissive arm:
	// opacity -> 0, influence 1), so the surface occludes nothing and contributes only its emissive
	// radiance - and the black portions of the gradient vanish for free.
	//
	// Interpretive rather than proven, hence its own knob: the guest composites this pass offscreen
	// and we approximate it in world space. Draws whose guest blend is ENABLED keep their own
	// translated blend exactly as today. Severable from FPVCOLEMISSIVE in both directions.
	// 0 restores the opaque-alias fill. Counter: fpvcol_additive. Census flag: additive=.
	bool fp_vcol_additive_enabled();

	// RPCS3_REMIX_VIEWMODELALBEDO=<hex>[,<hex>...] (bounded 16): tag a draw as viewmodel geometry
	// by its ALBEDO content hash, beside the existing program list.
	//
	// The visor is vp=830d7d1b9681c475 in two pieces ~1.2 units in front of the eye (albedo
	// 1BF8325ADEF3C986 opaque shell vtx=1446 ext=3.23, albedo C61753D31FB96507 blended layer vtx=59
	// ext=7.46), and it CANNOT be tagged program-wide: 830d7d1b9681c475 is also GUESTLIGHTVP, so
	// tagging the program drags the ceiling-light fixtures into the viewmodel camera and undoes
	// round 5's lighting. The user's design goal is not to shrink the visor - in the raster original
	// it is deliberately ~10 feet in front of the player and does not clip world geometry, because
	// it is a HUD element shaped like a visor - it is to classify it as a viewmodel so Remix gives
	// it its own near plane and excludes it from world clipping.
	//
	// Tag rule: albedo matches AND (VIEWMODELVP is empty OR the program matches it too), so an
	// armed albedo list works on its own while a user who has set both keeps the intersection.
	// Nothing is pre-filled in code; the launcher carries a commented paste-ready line.
	// Counter: vm_tagged_albedo (its own counter - vm_tagged_hash is NOT overloaded).
	bool viewmodel_albedo_matches(u64 hash);
	u32 viewmodel_albedo_count();

	// RPCS3_REMIX_VMANCHORGEO=1 (default): measure the viewmodel anchor guard against the draw's
	// GEOMETRY, not against its instance origin.
	//
	// The proven defect, and the reason the viewmodel machinery has never worked for anything since
	// round 1. With VIEWMODELANCHOR=4 the hash matched 54,389 draws and the guard refused ALL of
	// them (vm_hash_anchor_refused=54389, vm_tagged_hash=0); raising the limit to 100000 flipped it
	// to vm_tagged_hash=67901, vm_hash_anchor_refused=0. The picks report the draws ~1.5 units from
	// the eye, but they carry identity-ish transforms (origin ~ [0,0,0]) with WORLD-SPACE vertices
	// while the camera sits at [1761,-23,1145] - so the guard was measuring ~2100 units to a point
	// no geometry occupies.
	//
	// The fix measures the local AABB centre transformed by the draw's own matrix - the exact
	// computation maybe_inject_guest_light already performs - against the eye. With geometry
	// measured, the user's pinned VIEWMODELANCHOR=4 becomes CORRECT for the visor (~1.2 < 4) while
	// still excluding world geometry, which is what the guard's name always claimed.
	// If a tagged draw ever reaches the guard before its vertices are decoded, that draw falls back
	// to the origin measurement and is counted vm_anchor_unmeasured rather than guessed at.
	// 0 restores the origin measurement bit-exactly.
	bool viewmodel_anchor_geometry_enabled();

	// RPCS3_REMIX_VMALBEDOCAM=1 (default): when the albedo list is armed, viewmodel_mode >= 2 and
	// no dedicated viewmodel reference has been latched, submit the VIEW_MODEL camera as the world
	// camera's twin.
	//
	// createViewModelInstances returns early on '!cameraManager.isCameraValid(CameraType::ViewModel)'
	// and then builds its correction matrix from BOTH cameras, taking XY from the viewmodel
	// projection and Z/W from the main one - so a VIEW_MODEL camera must be submitted every frame or
	// the tag is inert. The albedo route has no program to latch a dedicated one from (VIEWMODELVP
	// is empty by design on this title), and a twin is the correct answer rather than a convenient
	// one: identical cameras yield an identity correction, which is the intended no-op, and the
	// tagged geometry simply gains the viewmodel pass's own near plane and its exclusion from world
	// clipping - the stated design goal. rtx.viewModel.enable = True is already in conf.
	// A SetupCamera rejection is censused once and falls back silently. Counter: vmcam_twin.
	bool viewmodel_albedo_camera_enabled();

	// RPCS3_REMIX_MADACCUM=1 (default): three coupled relaxations of match_const_affine's backward
	// walk, shipped as ONE knob because the offline sweep proves each of them is individually inert.
	// This is the giant-flower fix (C97CD1531AC480D8, extent 2956-6383, refusal
	// 'mad-src0-not-input').
	//
	// The plan for this round specified only the first of the three and predicted it would rescue
	// the flower. The sweep REFUTED that: alone it rescues nothing, and the flower's real decode is
	// two relaxations further down. What ships is what the bytes proved, not what was predicted.
	//
	//   M - the accumulate-in-place skip. 'MAD dst.xyz = a*b + dst.xyz' with neither factor an
	//       attribute is a decoration, not a term of the position. The flower carries one at slot 52
	//       ('MAD r0.xyz = r1.xyzx, r1.wwww, r0.xyzx', a distance-scaled billboard offset) and it is
	//       the sole reason the walk never reaches the base writer. Tested only where the arm has
	//       ALREADY decided to refuse (guarded ordering): tested before the opcode switch instead it
	//       would also swallow 'ADD dst.xyz = dst.xyz + c[K]', a real constant bias the ADD arm
	//       folds correctly - 24 such sites exist in this title's programs and the unguarded
	//       ordering fires on 12 of them.
	//   A - the MOV-forwarded constant scale. The flower writes 'MOV r0.xz = c[467].yyzy' and then
	//       reads 'MAD r4.xyz = I0.xyzx, r0.xxxx, c[61].xyzx', so its factor IS c[467].y and
	//       'mad-scale-not-const' was the last thing standing between it and a correct decode. Same
	//       chase match_prescale already performs for its own scalar operand, tightened to
	//       last_component_writer (the definition live at that point, not merely a writer of the
	//       register) plus a from_sca refusal (a SCA writer is RCP/RSQ - a computed scalar, not a
	//       constant copy). Broadcast reads only.
	//   B - the per-lane merge. Beneath the decoration the flower assembles r0 one lane at a time
	//       ('r0.x@21', 'r0.y@10', 'r0.z@31') and all three carry the SAME-LANE addend from r4,
	//       whose xyz is one instruction at slot 7. Fires only where the walk would otherwise refuse
	//       'partial-xyz', at most once per walk, and only when all three lanes reach the SAME
	//       full-xyz writer each on its OWN lane - a permutation is refused, never read as the
	//       identity. It only repositions the cursor; the ordinary arms still judge what it landed
	//       on, which is why B alone correctly yields 'mad-scale-not-const' rather than inventing a
	//       match.
	//
	// SWEEP (64 cached .vp + 17 captured, run before and after; the mandatory blast radius):
	//   variant   unchanged  refuse->match  REGRESSIONS  still refused
	//   M               0          0             0            81
	//   A               0          0             0            81
	//   B               0          0             0            81
	//   M+A             0          0             0            81
	//   M+A+B           0          1             0            80   <- the flower, and only it
	// Universal probe (match_const_affine from every temp x every 'before', 29,049 entry points):
	// M+A+B gives 78 gains, 0 regressions, and every gain resolves to that one program.
	// The rebuilt decode is 'pos = ATTR0.xyz * c[467].y + c[61].xyz' - scale slot 467 component y on
	// all three axes, bias slot 61, bias_before_scale FALSE (the walk meets a MAD, which is
	// 'attr * s + b': scale first). build_prescale returns true on it, so the draw keeps drawing
	// with the decode applied rather than being refused.
	//
	// NOT SHIPPED, and the reason is worth keeping: a fourth relaxation (generalising M's skip to
	// 'MAD dst.xyz = a*b + X' terminating on X == ATTR0.xyz) flips 5 more programs refuse->match
	// with 0 matcher regressions - and would DELETE their geometry. Those decodes carry neither a
	// scale nor a bias, build_prescale's const-affine branch ends 'return has_scale || has_bias',
	// and per_draw_transform turns that false into '++pos_decode_refused; return false'. A matcher
	// sweep that stops at the matcher would have shipped it. It needs an explicit identity flag on
	// the fingerprint first.
	//
	// Dropping the decoration replays the geometry STATIC and un-billboarded - the same
	// static-replay contract round 9 shipped for the DF46 canopy, stated rather than hidden.
	// 0 restores all three refusals bit-exactly. Counters: madaccum_resolved (programs),
	// madaccum_draws. Census: 'Remix madaccum:', which prints the rebuilt scale/bias slots because a
	// wrong scale and a refused decode look identical from outside.
	bool mad_accumulate_walk_enabled();

	// --- round 11 --------------------------------------------------------------------------------

	// RPCS3_REMIX_FPVCOLROUTE=1 (default): gate every ATTR3 replay on what the VERTEX program's own
	// ucode routes into COL0 (vp_fingerprint::vcol_route_kind - see its doc block above).
	//
	// Rounds 9/10 replayed ATTR3 into the submitted vertex colour and pointed the blend extension's
	// arg sources at VertexColor0 whenever the FRAGMENT program named COL0. That is only faithful
	// when the vertex program passes ATTR3 through. Half the cached population does not: 22 of 82
	// scale it by c[18], and the giant flower computes its alpha from a distance ramp that never
	// touches I3.w at all. Replaying the mesh attribute there is fabrication, and on the flower it
	// fabricated a zero alpha and deleted the foliage.
	//
	// The rule at all four consumption sites is one sentence: replay what the hardware would see,
	// else leave white. 'passthrough' replays as today; 'scaled'/'scaled_chain' replay with the
	// constants folded (RPCS3_REMIX_VCOLFOLD); 'computed'/'none' do not replay at all. Every alpha
	// half additionally requires vcol_alpha_from_attr.
	//
	// 0 restores round 10's route-blind replay bit-exactly. Counter: vcol_route_blocked (draws
	// refused a replay by route). Census: 'Remix vcolroute:' (once per program).
	bool fp_vcol_route_enabled();

	// RPCS3_REMIX_VCOLFOLD=1 (default): for the 'scaled'/'scaled_chain' routes, multiply the decoded
	// ATTR3 by the transform constants the ucode multiplies it by, read live per draw out of
	// rsx::method_registers.transform_constants (the same access the UV scale machinery performs -
	// see the uvrange census's c151 fill, and 'Remix pick-deep:' slotval=[...] which proves the
	// idiom reads the value the draw actually used).
	//
	// 0 makes the scaled routes replay UNFOLDED - i.e. the raw attribute, round 10's behaviour - so
	// "is the fold right?" and "is the route right?" are separately answerable. Counter:
	// vcol_fold_applied.
	bool vcol_fold_enabled();

	// RPCS3_REMIX_FPVCOLSKYGATE=1 (default): additionally require a self-lit candidate's raw extent
	// to be below sky_min_extent() before it may take the emissive/additive material.
	//
	// Closing a latent regression round 10 shipped without knowing. Its rgb-varies exclusion was
	// calibrated on the WRONG dome variant: the 32-vertex one is flat, but the 82-vertex one carries
	// the horizon gradient and satisfies every term of the self-lit verdict (its census rows print
	// selflit=1 additive=1). It only escaped becoming an emissive ONE/ONE additive shell because it
	// draws at the menu, where there is no camera and the world-transform gate refuses it before the
	// attach. With rtx.skyMode=0 the rasterized sky is the visible sky, and the first open-sky visit
	// with a camera present would have turned it into a level-sized light.
	//
	// Extent, not a hash list: the dome spans 10000 units, sky_min_extent() is 2000, and no
	// room-scale effect approaches it. 0 restores round 10's verdict bit-exactly. Counter:
	// fpvcol_skygate (draws excluded).
	bool fp_vcol_sky_gate_enabled();

	// RPCS3_REMIX_UCODESTOREFP=1 (default): the fragment-program twin of UCODESTORE. Writes the raw
	// fp ucode of every program classified 'other' on an untextured draw into
	// bin\remix_ucode\<rawhash>.fp, so the families the classifier cannot yet name can be decoded
	// offline instead of guessed at through one Ctrl+Click at a time.
	//
	// <rawhash> is the UNMODIFIED ucode hash, matching both cache filename conventions - the logged
	// fp= carries an extra 0x9e3779b97f4a7c15 term when the program exports 32-bit registers.
	// First expected customer: the sun card's sibling pair (vp=fc10c996fd0ec49a
	// fp=938cfb957fcb7e17, raw 0DBB822C00810202, class=other blend=1 material=0), which is NOT in
	// the raw shader cache. Counter: ucode_fp_stored.
	bool ucode_store_fp_enabled();

	// RPCS3_REMIX_HAZEFADE=1 (default): replay the per-vertex fade of Haze's atmospheric haze /
	// god-ray card, which the backend has been submitting as an opaque black wall.
	//
	// The card is NOT misplaced - that reading was an artefact of reading the pick's origin=, which
	// is the instance-transform translation (identity => zero), not the geometry's location. Its
	// vertices are world-space at z~35 with the camera at z~-42, its fused matrix equals the active
	// camera's V x P to ~1e-5, and its w-divide is a no-op (attr0 is 3-component float, w=1). What
	// is missing is the fade, and the fade lives entirely in fragment-program maths the backend
	// never replayed. Decoded from the cached ucode (fp raw 3C3D0320F8611D4F, 23 instructions;
	// vp EDC10321BF7CEB8F, 30 slots):
	//
	//   vp   0:MUL>o1.xyzw(I3.xyzw, c[18].xyzw)              COL0 = ATTR3 * c[18]
	//        9,10,13,14,20: o7 = [c8 c9 c10]*pos + c11       TEX0 = view-space position p
	//        2,3,5,8,16,18,21..29: o8 = quaternion(I9)       TEX1 = view-space card direction d
	//   fp   0,1:   r1.xyz = TEX0.xyz ; r1.w = |p|^2
	//        6,7:   r1.z = dot(p,d)/|p|                      = cos(theta)
	//        8:     r1.xy = (K.x-K.y, K.z-K.w)               the two ramp WIDTHS
	//        9,10:  rgb   = texRGB * (2*COL0.rgb) * 0.944243
	//        12..16 dist  = |K.x-K.y|>0.1 ? sat((|p|-K.y)/(K.x-K.y)) : (|p| >= K.x)
	//        17,18,21 ang = |K.z-K.w|>0.1 ? sat((|cos|-K.w)/(K.z-K.w)) : (|cos| >= K.z)
	//        20,22: alpha = texA * COL0.a * dist * ang
	//   with the embedded literal K = [0, 0, 0.9, 0.4].
	//
	// Two things follow that the plan for this round did not know. First, K.x == K.y == 0 makes the
	// DISTANCE ramp identically 1 for this program - the whole translucency is the ANGLE ramp, so
	// shipping "the distance ramp alone" would have shipped no fade at all. Second, the picked
	// albedo EC31300DCF914C64 has alpha_range 255..255 on the 'Remix kil:' census - the texture's
	// alpha is fully opaque - so on hardware ALL of this card's translucency comes from COL0.a and
	// those two per-pixel fades. The backend replays none of them (the fp is correctly classified
	// 'other'), so the card arrives at alpha 255 under SRC_ALPHA/ONE_MINUS_SRC_ALPHA: an effectively
	// opaque plane, path-lit in a dark jungle. That is the black backdrop plane.
	//
	// Per-vertex rather than per-pixel is the contract, and a 10-vertex card makes it faithful in
	// practice. The card is NOT hidden - the user asked for it rendered where it is, fading as
	// authored. 0 restores today's opaque card bit-exactly. Counter: hazefade_applied.
	// Census: 'Remix hazefade:'.
	bool haze_fade_enabled();

	// RPCS3_REMIX_FXREFPROBE=1 (default): a read-only instrument, no behaviour change.
	//
	// The Selva smoke and explosions are NOT the emissive family - the effect census never grows a
	// new untextured vcol_pass pair. What the round-10 sessions show all session long is a refused
	// TEXTURED effects family on the main surface: vp=f39f504649b6f442 (plus siblings
	// af06f6d32ec048ee, 15ad612980aca110, 57a12323f22f4988), 952..3649-vertex batches re-created
	// every frame, refused with fail=tail areason=wdivide persp_residue~3.71 against a 0.02
	// tolerance, ref=camera / ref=anchor / ref=anchor_prev all rescue=failed. A residue of 3.71 is
	// not noise: the reference that divided it is simply not the projection those draws went
	// through (the rows show the AUX camera active at 512x288 while the draw is on the 1024x576
	// main surface).
	//
	// The probe divides such a draw's recovered fused matrix against EVERY live gauge slot and both
	// camera poses of the same frame and prints the best three. Pre-registered verdicts: a best
	// residue within tolerance against some OTHER live reference means reference SELECTION is the
	// defect (round 12 ships a retry-against-alternates divide); a best residue far outside against
	// everything means these draws carry their own projection (the banked second-projection-
	// reference design's entry ticket is met). No fix ships on this thread this round - guessing
	// before the measurement is the rounds-1-3 failure mode. 0 silences it. Census: 'Remix fxref:'.
	bool fx_ref_probe_enabled();

	// --- round 18: the fxref census was saturated, and its verdict has now been read ------------
	//
	// MEASURED over the three most recent BLUS30094 runs: the census emits EXACTLY its 8-line cap in
	// every stats window it speaks in (744 lines / 93 windows in the round-17 run - 8.00), so the
	// "320 fxref lines" round 18's brief read as a sample of the effects family are the first eight
	// refusals of one frame per window, repeated. 114 of runA's 320 and 85 of runB's 744 are
	// vp=641d6432efd6add4, which is NOT an effect: its ucode is six instructions long and reads
	//
	//     o7(TEX0).xy = v8.xy - c67.xy ;  o7(TEX0).zw = v8.xy + c67.xy
	//     o0(HPOS)    = v0.x*c0 + v0.y*c1 + v0.z*c2 + v0.w*c3
	//
	// - a symmetric two-tap sample offset on a 4-vertex quad covering a 512x288 half-res buffer.
	// That is a separable blur/downsample, and its refused_residue is bit-identical (0.999988) in
	// every frame of every run because it equals 1/Q of the title's projection and depends on
	// nothing in the scene. It is correctly refused and must stay refused.
	//
	// RPCS3_REMIX_FXREFVP=<up to 8 comma-separated hex vp hashes>: restrict the census to named
	// programs. EMPTY (the default) is exactly the pre-round-18 behaviour - every fused refusal.
	bool fx_ref_vp_listed();
	bool fx_ref_vp_matches(u64 hash);

	// RPCS3_REMIX_FXREFMAX=<lines per stats window>, default 8 = the pre-round-18 constant. Clamped
	// to 64 so a mis-set knob cannot make this census the dominant writer in the dump log.
	u32 fx_ref_max_lines();

	// --- round 18: the second-projection reference, built from the split that already succeeds ---
	//
	// RPCS3_REMIX_PROJSPLIT=1: at the tail-rescue FAILED exit only - the exit where the draw is
	// dropped today - rebuild the reference by crossing the anchor's VIEW with the draw's OWN
	// PROJECTION, and re-run the affinity gate.
	//
	// Why this is the indicated fix and not a guess. The refusal residue
	// |m03|+|m13|+|m23|+|m33-1| of world = fused * reference_inverse is structurally BLIND to any
	// difference in the view: for reference = V_a*P and fused = W*V_d*P the product is
	// W*V_d*P*P^-1*V_a^-1 = W*V_d*V_a^-1, which is affine for any two rigid views, so its residue is
	// 0. A non-zero residue therefore means one thing only - the draw's PROJECTION is not the
	// reference's. MEASURED for the effects family vp=f39f504649b6f442: residue 3.21949 as the first
	// sampled refusal of all three runs and a median of 3.409 (p10 3.386, p90 3.763) over 226
	// samples spanning three sessions at unrelated camera positions. A camera-lag or wrong-anchor
	// defect cannot hold a residue that still while the player walks and turns; a second projection
	// can, and does.
	//
	// The construction is exact rather than heuristic. split_view_projection already reports
	// split=1 for this family on every refusal it prints, and its contract is M = view * projection
	// with the view forced orthonormal. So for anchor_fused = V*P_a (the anchor is an identity draw,
	// W=I) and fused = W*V*P_d:
	//
	//     cross   = split(anchor_fused).view * split(fused).projection = V * P_d
	//     world   = fused * cross^-1 = W*V*P_d * P_d^-1*V^-1 = W
	//
	// which is affine by construction when the premise holds, and fails the same is_affine gate
	// everything else passes when it does not. A split that won on the TRANSPOSE is refused
	// outright: view*projection reconstructs fused^T there, and mixing the two conventions is how a
	// matrix bug becomes a geometry bug.
	//
	// 0 (the code default) restores the pre-round-18 drop bit-exactly. Acceptance counters:
	// proj_split_applied / proj_split_refused / proj_split_nosplit on 'Remix live:'.
	bool proj_split_enabled();

	// RPCS3_REMIX_PROJSPLITERR=<L1 x 1000>, default 50 = 0.05, scaled exactly as AFFINETOL is. The
	// largest split reconstruction error either split may carry before the cross is refused. The
	// splitter reports l1_error per split; a large one means the "view" it synthesised is not the
	// draw's actual view and the cross is meaningless.
	f32 proj_split_max_error();

	// --- round 19 --------------------------------------------------------------------------------

	// RPCS3_REMIX_PROJSPLITVDELTA=<L1 x 1000>, default 0 = NO GATE = round 18 behaviour bit for bit.
	//
	// Round 18's is_affine gate on the proj_split cross is VACUOUS - see the long derivation beside
	// the definition. Both rigid views come out of try_split_once's orthonormal basis, so the
	// recovered world is V_draw * V_anchor^-1, rigid and therefore affine no matter how wrong the
	// premise is. proj_split_refused is pinned at 0 for that reason and not because the fix is
	// perfect.
	//
	// This is the number that actually tests the premise: ||V_draw - V_anchor||_1. It is printed as
	// vdelta= on every 'Remix tail-rescue: ... outcome=projsplit' line whether or not the gate is
	// armed, so the distribution can be read before anything is refused on it. Arm it only once the
	// log says where the population sits.
	f32 proj_split_view_delta_max();

	// RPCS3_REMIX_VMBASIS=<0..7>, default 0 = OFF = today's behaviour bit for bit.
	//
	// Sign flips applied to the VIEW_MODEL-tagged instance transform in the camera's own frame,
	// about the eye: bit0 (1) = right axis, bit1 (2) = up axis, bit2 (4) = forward axis. The axes
	// come from the columns of the row-vector worldToView and are orthonormal for any rigid view,
	// so the operator is an exact reflection/rotation and cannot rescale the viewmodel.
	//
	// 6 = up+forward = a 180 degree rotation about the camera's right axis, the round-19 launcher's
	// explicit first guess; 3 = right+up if the report is a left/right mirror instead. Wrong guess
	// costs one launcher edit, not a rebuild.
	u32 viewmodel_basis_flip();

	// RPCS3_REMIX_VMBASISMAX=<lines>, default 24, clamped to 96 in code. Cap for the
	// 'Remix vmbasis:' census, which prints the recovered world matrix and the camera basis for the
	// VIEW_MODEL-tagged draws.
	//
	// ROUND 34 CORRECTION - "the measurement no census has ever emitted" is now FALSE, and this doc
	// block said it for fourteen rounds. It emitted SIX lines in bin\log\RPCS3.log at 0:00:57 and
	// 0:02:30 of the RPCS3_REMIX_VMDEPTHOFFSET=2 run that ended 2026-08-17 09:37:23, on
	// vp=830d7d1b9681c475 / af06f6d32ec048ee / 57a12323f22f4988, with vm_tagged=7960 of
	// vm_considered=632100. The reason it had never fired before is not the cap and not the dedup: it
	// is that nothing had ever been tagged, and the depth knob is what tagged it. Read those six
	// lines before adding any instrument here.
	u32 viewmodel_basis_census_max();

	// RPCS3_REMIX_VMBASISPIVOT=<0|1|2>, default 0 = the eye = today's behaviour bit for bit.
	//
	// ROUND 34. viewmodel_basis_flip() above reflects about the EYE, on the premise that first-person
	// geometry sits at the eye so a reflection there only reorients it. MEASURED from the six census
	// lines named in viewmodel_basis_census_max(): that premise is false for every tagged draw on this
	// title. The arms' recovered instance transform has its translation at the WORLD ORIGIN
	// (pre translation [0.0555 -0.2443 0.00686] on the vtx=3649 arms draw) while the eye is at
	// [1774.14 -31.2479 1177.04], i.e. 2129 units away - so reflecting about the eye moved the mesh to
	// [158.622 -59.112 2563.8]. All six lines land 2537..2564 units from where they started. That, and
	// not a 0.4-unit nudge behind the camera, is why the arms and the weapon vanished outright.
	//
	//   0 = the anchor-frame eye (round 19..33).
	//   1 = the draw's own geometry centroid under the pre-operator transform. Keeps the mesh exactly
	//       where it is and rotates it in place, which is what an orientation correction is supposed to
	//       be. Falls back to the eye when the centroid is unmeasurable, reported as pivotsrc=3.
	//   2 = the transform's own translation column, i.e. the object origin. Cheaper than 1 and correct
	//       whenever the mesh is modelled about its origin; on this title's viewmodel that origin is
	//       the world origin, so 2 is the wrong choice HERE and is provided only to separate "modelled
	//       about the origin" from "modelled about the centroid" in one run.
	//
	// The 'Remix vmbasis:' census proves which of the two halves moved: dbasis= must be non-zero (the
	// operator did something) and dcentre= must be ~0 (it did not displace the mesh). At pivot 0 this
	// title reads dcentre ~2550; a correct pivot reads ~1e-4.
	//
	// READ THIS BEFORE JUDGING THE PIVOT: this knob is INERT while RPCS3_REMIX_VMBASIS=0, because
	// apply_viewmodel_basis returns on 'flip == 0' before the pivot is selected. The round-34 launcher
	// arms VMBASIS=0 deliberately, so its census will read 'pivotsrc=0 pivot=[0 0 0] dbasis=0' and that
	// is NOT a failure of the pivot - it is the operator not running. The dbasis/dcentre criterion above
	// belongs to STEP 2 (VMBASIS=6 with this at 1). Step 1 judges PLACEMENT only, from cdist_pre=.
	// 'pivotsrc=0' is shared by "chose the eye" and "returned early"; flip= and cam= on the same line
	// disambiguate.
	u32 viewmodel_basis_pivot();

	// --- round 36: a REAL rotation, because the sign mask above cannot express the fix -----------
	//
	// RPCS3_REMIX_VMROTAXIS=<0..6>, default 0 = OFF = today's behaviour bit for bit.
	// RPCS3_REMIX_VMROTDEG=<0..359>, default 0 (taken mod 360, so 360 means 0 - say 180, not 360).
	// RPCS3_REMIX_VMROTPIVOT=<0..3>, default 0 = the RAW eye.
	//
	// WHY THIS EXISTS. viewmodel_basis_flip() is a 3-bit sign mask about a pivot, and round 36
	// measured that no setting of it can be right, because the defect is not a sign pattern - it is
	// a 180 degree rotation about the camera's RIGHT axis THROUGH THE EYE, which moves the mesh as
	// well as turning it. MEASURED over 287 'Remix vmbasis:' lines in bin\remix_dump.log
	// (the last 400 MB, the VMBASIS=5 session):
	//
	//   - the camera basis is left-handed on all 287 lines (right x up . fwd = +1.00), so 'fwd'
	//     genuinely points into the scene and the sign of the forward coordinate is readable;
	//   - EVERY near-eye VIEW_MODEL-tagged draw lands with its submitted centroid BEHIND the eye.
	//     26 distinct (vp, vtx) groups inside 5 units, fwd ranging -0.06 .. -0.81, up +0.05 .. +0.94.
	//     The one near-eye group that is NOT tagged (vp=aae8e0d5ae292dd4, vtx=91) reads fwd
	//     +0.083 .. +0.484 - in FRONT - which is the control that says the sign is not a convention
	//     error in the instrument;
	//   - the object basis relative to the camera basis is a rotation about the camera's RIGHT axis
	//     of 118 .. 179 degrees (axis component on right >= 0.996 on every near-eye line). Not a
	//     yaw, not a mirror: det(D) = +1.00000 and the column norms are 1.0000.
	//
	// One operator produces all three at once: rotate 180 degrees about the camera's right axis,
	// pivoted at the EYE. It maps up -> -up, fwd -> -fwd and leaves right alone, which is exactly
	// "upside down and backwards" - the user's own words for VMBASIS=0.
	//
	// WHY VMBASIS=6 DID NOT FIX IT, and this is the part the sign mask cannot reach: flip 6 is the
	// SAME 3x3 (at 180 degrees the Rodrigues form collapses to -I + 2 a a^T, which is what
	// 'sum_k s_k a_k a_k^T' with s = (+1,-1,-1) already is), but the launcher pivots it at the
	// CENTROID (VMBASISPIVOT=1), so it turns the mesh in place and leaves it behind the eye. The
	// user then sees correctly-oriented arms hanging above and behind them, which reads as "still
	// upside down". Pivot 0 was not the alternative because it is the ANCHOR-frame eye
	// (anchor_frame_eye(), and RPCS3_REMIX_CAMANCHOREYE=1 is armed), which round 34 measured
	// throwing the mesh 2537..2564 units.
	//
	// So the new pivot 0 here is the RAW m_active_camera.position with NO anchor conversion, and
	// that is justified by measurement, not by preference: cdist_pre on the current build reads
	// 0.43 .. 1.04 on all 287 lines, and cdist_pre IS |centroid - m_active_camera.position|. A
	// centroid half a unit from that point cannot be in a different frame from it.
	//
	//   axis 0       = off.
	//   axis 1, 2, 3 = the camera's right / up / forward axis, in world space, taken from the
	//                  columns of the row-vector worldToView. LEFT-multiplied about the pivot, so
	//                  it turns the geometry in the camera's frame.
	//   axis 4, 5, 6 = the MODEL's own X / Y / Z axis. RIGHT-multiplied (M' = M * R), which is a
	//                  rotation in the mesh's own space about its own origin - this is the knob for
	//                  the "authored Z-up in a Y-up renderer" hypothesis (that would be axis 4 with
	//                  270 degrees). The pivot knob is INERT for 4..6 and the census says so with
	//                  rotpivsrc=5.
	//
	//   pivot 0 = the raw eye (m_active_camera.position). NEW, and the one the launcher arms.
	//   pivot 1 = the draw's own geometry centroid, i.e. turn in place (what VMBASISPIVOT=1 does).
	//   pivot 2 = the transform's translation column.
	//   pivot 3 = the anchor-frame eye, i.e. round 19..35's pivot 0. Kept so the old behaviour is
	//             still reachable for an A/B, not because it is believed correct.
	//
	// Composition: viewmodel_basis_flip() still runs FIRST and this runs after it, so the two are
	// independent and the launcher arms VMBASIS=0 to isolate this one. Setting both is legal and
	// the census reports each half separately (dbasis= is the flip, drot= is this).
	//
	// PRE-REGISTERED READING for VMROTAXIS=1 VMROTDEG=180 VMROTPIVOT=0, all on 'Remix vmbasis:':
	//   cpost=[right up fwd] must have fwd > 0 where cpre had fwd < 0, and up < 0 where cpre had
	//   up > 0, with right unchanged to ~1e-3. relpost must be (180 - relpre) +- 1 degree, i.e.
	//   1 .. 62 rather than 118 .. 179. cdist_post must equal cdist_pre to within 1% - a rotation
	//   about the eye cannot change the distance from the eye, so a cdist_post that MOVES is proof
	//   the pivot is not the eye. dcentre must be 0.3 .. 1.6, NOT ~0 (that would be a centroid
	//   pivot leaking back in) and NOT ~2550 (that would be the anchor-frame eye leaking back in).
	u32 viewmodel_rotate_axis();
	u32 viewmodel_rotate_degrees();
	u32 viewmodel_rotate_pivot();

	// --- round 37 ---------------------------------------------------------------------------------
	// RPCS3_REMIX_VMROTPIVOTRIGHT / VMROTPIVOTUP / VMROTPIVOTFWD, signed world units, default 0.
	//
	// WHY A PIVOT OFFSET AND NOT A TRANSLATION, and why the value is derived rather than eyeballed.
	//
	// Write the pivot in camera coordinates as (p_r, p_u, p_f) measured from the eye. A 180 degree
	// rotation about the camera's RIGHT axis through that point sends a centroid at camera-space
	// (r, u, f) to (r, 2*p_u - u, 2*p_f - f). At p = 0 that is the round-36 operator exactly, so
	// arming these at 0 is bit-for-bit today's behaviour and the default is genuinely inert.
	//
	// The consequence that makes this the right lever: the operator PRESERVES |centroid - eye| when
	// and only when the pivot is the eye. Round 36 read cdist_post == cdist_pre as a success
	// criterion, and it is - for the pivot - but it also means the round-36 operator cannot change
	// the weapon's distance from the eye at all, whatever the true distance should be. The user
	// reports the weapon is too close and reports having to free-cam BACKWARDS to read the gun's own
	// screen. If the real defect is a flip about a point ahead of the eye rather than about the eye,
	// then the corrected forward distance is short by exactly 2*p_f and no other knob in the backend
	// can reach it.
	//
	// So this is a falsifiable prediction, not a taste setting: cpost's forward component must move
	// by EXACTLY 2 * VMROTPIVOTFWD and cdist_post must stop tracking cdist_pre. If cpost moves by
	// 1x, the offset is being applied as a translation somewhere and the composition is wrong.
	//
	// env_float_signed and never env_float: 0 is the OFF value for all three, env_float rejects 0
	// outright (round 32's SKYANCHOR=0 defect), and negative is a real value here - it pulls the
	// model back towards the eye - so a reader that clamps at zero would delete half the range.
	// No clamp: a first-person offset of order one unit is the intended range, and a wrong value is
	// visible immediately and revertible from the launcher without a rebuild.
	f32 viewmodel_rotate_pivot_right();
	f32 viewmodel_rotate_pivot_up();
	f32 viewmodel_rotate_pivot_fwd();

	// RPCS3_REMIX_VMROTLOCK=<0|1|2>, default 0 = OFF.
	//
	// ROUND 37. The residual after the round-36 operator is NOT a pivot error and NOT a stale
	// camera - both were refuted by measurement, see the round-37 block in docs/remix/KNOBS.md. It
	// is an algebraic identity of the operator: for a rotation about the camera's RIGHT axis,
	//
	//     cos(relpre) + cos(relpost) = dotpre[0] - 1
	//
	// which was verified to better than 4e-6 on 8 of the 9 census lines the play-test produced. A
	// rotation about `right` cannot change any component along `right`, so whatever misalignment the
	// object's own X axis has from the camera's right axis survives the operator untouched, and
	// relpost is (to within 1.5 degrees on all 9 lines) exactly acos(dotpre[0]).
	//
	// That residual is either the mesh's GENUINE pose - an aim pitch, a hand, a swinging arm - or it
	// is a second defect. Nothing already in the log can tell those apart, so this knob is the test
	// that separates them in one play-test, and it is designed to be able to fail visibly:
	//
	//   1 = replace the object's 3x3 with the camera's basis, column lengths preserved so scale
	//       survives. relpost then reads ~0 BY CONSTRUCTION - that reading proves only that the
	//       operator composed, it is not evidence about the world. The evidence is what the user
	//       sees: if the arms and weapon look right and stop swimming, the residual was a defect;
	//       if they freeze rigid, lose their aim pitch or the hands detach, the residual was the
	//       genuine pose and this knob must go back to 0.
	//   2 = align only the object's X column to the camera's right axis, by the minimal rotation
	//       that does it, leaving the remaining roll about right alone. This is the half of mode 1
	//       that the identity above says is responsible for the whole residual.
	//
	// Applied after the rotation and about the SAME pivot, so it cannot move the model - only turn
	// it. A position change here would mean the pivot did not survive, which is itself a bug.
	u32 viewmodel_rotate_lock();

	// RPCS3_REMIX_VMBASISEVERY=<N>, default 0 = OFF = the round-19..36 dedup.
	//
	// ROUND 37, and this is the round's most important change even though it fixes no rendering.
	//
	// The 'Remix vmbasis:' census deduplicates on (vp ^ albedo * golden) and inserts into a set that
	// lives for the whole session (RemixGSRender.cpp, the m_viewmodel_basis_census_seen guard). So
	// every (vp, albedo) pair emits exactly ONE line per session, ever. The round-36 play-test
	// produced NINE lines across six frames from a session of 19,200 frames.
	//
	// The user's complaint is that the weapon "doesn't follow the camera turning smoothly". That is
	// a statement about time. A census that emits one sample per object per session structurally
	// CANNOT observe it - not "did not", cannot - and three rounds of asking the log about jitter
	// were therefore unanswerable before they were asked. That is the failure this knob fixes.
	//
	// N >= 1 drops the dedup entirely and emits on every frame where (frame % N) == 0, so N=1 is a
	// contiguous per-frame time series. Bound it with VMBASISMAX (whose ceiling this round raises
	// from 96 to 65536) and narrow it with VMBASISVTX so the volume stays readable.
	u32 viewmodel_basis_census_every();

	// RPCS3_REMIX_VMBASISVTX=<N>, default 0 = any. Restricts the census to draws of exactly N
	// vertices, which is what makes VMBASISEVERY=1 affordable: the round-36 census found ~10 near-eye
	// viewmodel groups live at once, so an unfiltered per-frame census burns its budget in ~400
	// frames, while one pinned mesh gives ~4000 consecutive frames from the same budget.
	u32 viewmodel_basis_census_vtx();

	// RPCS3_REMIX_VMTAGONLY=<0|1>, default 0 = OFF = today's behaviour bit for bit.
	//
	// ROUND 34, and it is the actual viewmodel fix. Tagging a draw VIEW_MODEL currently does four
	// things at once in per_draw_transform, three of which are about PLACEMENT and one of which is the
	// tag. 1 severs the three and keeps the tag:
	//
	//   - it stops the draw being divided by m_active_viewmodel.reference_inverse,
	//   - it stops 'defer_candidate = false' taking it out of the deferral population,
	//   - it stops the REFUSED:noref / REFUSED:mode1 arm dropping the draw outright,
	//   - and it stops '&& !viewmodel_draw' disarming the tail-rescue ladder, which is where PROJSPLIT
	//     lives - round 28 found that same gate deleting the arms through the VMPAIRVP route.
	//
	// WHY THE VIEWMODEL REFERENCE IS THE WRONG DIVISOR, MEASURED. It is latched as
	// inverse(folded) from the FIRST viewmodel-depth draw of the frame (RemixGSRender.cpp,
	// 'm_frame_viewmodel_candidate.reference_inverse = viewmodel_inverse'), and the comment beside it
	// states the population is expected to share one view-projection. So world = fused x
	// reference_inverse is a matrix divided by (near enough) its own inverse and collapses to the
	// identity BY CONSTRUCTION - the same self-referential trap round 32 caught in the worldid-census
	// tmax field. Every viewmodel draw therefore lands at the world origin. The six census lines show
	// exactly that: two of the six have a bit-near-exact identity 'pre' and all six have a translation
	// within 0.4 of [0 0 0].
	//
	// AND THE WORLD DIVISOR IS MEASURABLY CORRECT FOR THESE DRAWS. 'Remix picked:' lines for the same
	// program in the same level, untagged and therefore on the world path, read
	// origin=[1780.78 -31.2362 1186.65] against cam=[1780.42 -31.3929 1186.57] (albedo
	// 0721D150DF278E7D vtx=2140) and origin=[1780.75 -31.1536 1186.66] against
	// cam=[1780.42 -31.3948 1186.57] (albedo 86885A0E60751491 vtx=4456) - 0.40 and 0.44 units from the
	// eye, with basis=[0.998796 0.99919 1.00065] and [1.00293 1.00239 1.0015].
	//
	// THAT RETIRES ROUND 5's JUSTIFICATION. The doc block on the latch says dividing the viewmodel by
	// the world camera "collapses the basis to (0.491, 0.002, 1.150)". Against current bytes the same
	// quantity reads unity to three decimals, because the f64 gauge (round 4/14), the anchor election
	// and PROJSPLIT all landed after that measurement. Do not re-derive the old number from the old
	// comment - re-measure it.
	//
	// Pre-registered refutation: with 1 set, vm_tagged stays non-zero while vmcam_considered goes to
	// ZERO (the whole block is skipped), and the census's own centre_pre / cdist_pre must read ~0.4
	// from the eye instead of ~2129. If cdist_pre stays at ~2129 with tagonly=1 then the relocation is
	// NOT the viewmodel divide and this knob is the wrong lever.
	bool viewmodel_tag_only();

	// RPCS3_REMIX_VMDEPTHOFFSET=<threshold x 1000>, default 0 = use the built-in 1e-3 constant =
	// today's behaviour bit for bit.
	//
	// The viewmodel depth rule's near-end threshold. RemixGSRender.cpp:5313's claim that Haze uses
	// one depth range for every draw is REFUTED by the round-18 run: three programs - the arms
	// 830d7d1b9681c475, 57a12323f22f4988 and the visor 9f591b6a6b825612 - render with
	// scale_z=0.00125 offset_z=0.00125 while the other 35 censused programs use 0.49875/0.50125.
	// The rule misses them by 25%. Setting this to 2 (=0.002) lets it select them.
	f32 viewmodel_depth_offset_max();

	// --- round 35: the (vertex program, FRAGMENT program) hide gate ------------------------------
	//
	// RPCS3_REMIX_HIDEPAIRVP=<hex> + RPCS3_REMIX_HIDEPAIRFP=<hex>, both EMPTY by default, and an
	// empty EITHER half disarms the route - today's behaviour bit for bit.
	// RPCS3_REMIX_HIDEPAIRMODE=<1|2>, default 1.
	//
	// For "stop the player's body casting a full-body shadow". The fragment program is the
	// discriminator the (vp, albedo) pair could not be: on Haze, (f39f504649b6f442, 0721D150DF278E7D)
	// selects the legs AND the arms, while (f39f504649b6f442, b64dc06f79b8b42b) is 1381 of 1381
	// census rows at vtx=952. Derivation and the mode semantics are in RemixTransforms.cpp beside
	// hide_pair_vp_hash().
	//
	// The fp hash compared here is m_current_fp_hash, i.e. the same value 'Remix picked:' and
	// 'Remix fpcandidate:' print as fp= - so a hash read off a Ctrl+Click can be pasted straight in.
	u64 hide_pair_vp_hash();
	u64 hide_pair_fp_hash();
	u32 hide_pair_mode();
	bool hide_pair_matches(u64 vp_hash, u64 fp_hash);

	// --- round 20 --------------------------------------------------------------------------------

	// RPCS3_REMIX_VMPAIRVP=<hex>[,...] (bound 4) + RPCS3_REMIX_VMPAIRALBEDO=<hex>[,...] (bound 8).
	// Both default EMPTY, and an empty EITHER half disarms the whole route - today's behaviour bit
	// for bit.
	//
	// A (vertex program, albedo) PAIR gate for the viewmodel tag, the same shape SUNCARDVP +
	// SUNCARDALBEDO uses and SKIPPAIRVP + SKIPPAIRALBEDO used before it: the draw is tagged only if
	// its vp is on the first list AND its albedo is on the second. It is a THIRD, independent route
	// that is ORed into the tag decision alongside the depth verdict and the albedo route - which is
	// safe precisely because it cannot fire on anything but an exact pair.
	//
	// WHY A NEW ROUTE AND NOT VIEWMODELVP. Round 20's brief asked for VIEWMODELVP to be "ANDed
	// instead of ORed". Measured against the source, that is half right and the half that is wrong
	// matters:
	//
	//   * viewmodel_albedo_matches() is ALREADY ANDed with the program list - RemixGSRender.cpp
	//     :13647 and :17499 both read
	//         by_albedo = viewmodel_albedo_matches(a) && (viewmodel_vp_count() == 0 || by_hash)
	//     so with VIEWMODELVP non-empty the albedo route is already a pair gate.
	//   * What is ORed is the DEPTH verdict, and classify_viewmodel_depth returns 'tagged'
	//     IMMEDIATELY for anything on VIEWMODELVP (RemixGSRender.cpp:5336, ahead of both depth
	//     tests). So listing a program there tags EVERY draw of it whatever its albedo, and the OR
	//     at :13683 / :17555 then carries that straight through.
	//
	// The documented consequence is real and is why 830d7d1b9681c475 must never go on VIEWMODELVP:
	// it is also GUESTLIGHTVP, and tagging it program-wide drags the ceiling light fixtures into the
	// viewmodel camera and undoes round 5's lighting (RemixGSRender.cpp:13641). This route reaches
	// the same draws without touching classify_viewmodel_depth at all.
	//
	// Line numbers above are as of round 20 and are cited because they are load-bearing, not
	// decorative; if they have drifted, grep for the expressions themselves.
	//
	// The anchor guard (viewmodel_anchor_rejects, limit RPCS3_REMIX_VIEWMODELANCHOR) still applies
	// on top, so a pair that matches something 40 units away is still refused.
	bool viewmodel_pair_matches(u64 vp_hash, u64 albedo_hash);

	// Counts for the banner. Either being 0 means the route is disarmed.
	u32 viewmodel_pair_vp_count();
	u32 viewmodel_pair_albedo_count();

	// --- round 21 --------------------------------------------------------------------------------

	// RPCS3_REMIX_ALPHACENSUS=<lines>, default 0 = OFF, clamped to 256. Cap for the
	// 'Remix alphastate:' census, deduped per albedo hash.
	//
	// This exists because round 21 could not answer "why does a card with a soft alpha render as a
	// hard-edged square" or "why is the effects family placed but invisible" from any instrument that
	// existed. What WAS measurable (from the deployed round-20 build's own 'Remix stats:' line in
	// bin\log\RPCS3.log, which is where log_stats() writes - NOT remix_dump.log) is that the blend
	// translation is clean: blend_chained=1183539 blend_translucent=405800 blend_unmapped=0
	// blend_rtopaque=0, pairs {6/7/0=322419 1/7/0=71486 6/1/0=7951 1/1/0=3944}. So no draw is losing
	// its blend on the way out of this backend, and both "the blend state translation" and "the
	// runtime dropped the pair to opaque" are refuted for the whole run.
	//
	// What is still unmeasured is the state a NAMED albedo ships with, and the runtime verdict that
	// state produces. calculateAlphaState (rtx_instance_manager.cpp:654-849, deployed numos3) derives
	//   isFullyOpaque      = !blendEnabled && alphaTestType == kAlways      (:845)
	//   isBlendingDisabled = !blendEnabled                                  (:846)
	// and the shader forces opacity to 1.0 on the first (opaque_surface_material_blending.slangh:39)
	// and binarises it to 0-or-1 on the second (:90). Either one turns a soft radial falloff into a
	// hard edge. This census prints the inputs and the derived verdict side by side, so the next
	// round reads the mechanism instead of inferring it.
	u32 alpha_state_census_max();

	// --- round 22: the WORLDIDENTITYVP override, measured -----------------------------------------
	//
	// MEASURED (round-21 build, pid 22856, frames 9390-30480, from 'Remix worldid-draw:' which prints
	// the transform this override is ABOUT to discard). The override is free for two of the six
	// programs on the list and destructive for the rest:
	//
	//   vp                 rows   mean|t|   max|t|   rows|t|>32   max basis_delta
	//   bd1c10df5703e559    383     0.000    0.000            0          0.00000
	//   7f02e76d7369d09e    289     0.000    0.000            0          0.00000
	//   ad7ce9d672a0bf6b   4654   197.305 2123.000         3875          0.51289
	//   0214281b9a7a412d    931   334.508 3909.850           96          1.98014
	//   c1d482dcd1b03ed0     38   161.688  355.394           32          0.00022
	//   d0b6a471bb2d463b     26   162.833  290.523           26          0.00022
	//
	// The first two submit absolute world vertices, so forcing the identity is a no-op and the raw
	// bounding boxes are world-sized (bd1c10df: x in [-1397.7, 1146.3]). AD7CE9D672A0BF6B's raw boxes
	// are a fixed 220x54x81 volume centred on its own origin (x in [-122.8, 101.0], y in [-19.1,
	// 35.0], z in [-45.6, 35.5]) - LOCAL coordinates for the Mantel land carrier, whose world
	// placement is exactly the translation being discarded. Pinning it to the world origin is why the
	// vehicle "went away from the camera" while the player kept riding it.
	//
	// The comment at the override site predicted this in round-N words: "a VP-wide transform rule
	// cannot distinguish the gameplay draw from a camera-relative copy". These knobs measure the two
	// populations and, only when armed, let the displaced one keep its resolved transform.

	// RPCS3_REMIX_WORLDIDCENSUS=<n>: print n per-program slots on 'Remix worldid-census:' each stats
	// window. Run-cumulative, so the last line of a run is the run total. 0 (default) = OFF. The
	// backend keeps at most 12 slots; higher values are clamped to that.
	u32 world_identity_census_max();

	// RPCS3_REMIX_WORLDIDMAXT=<whole world units>: when non-zero, a draw whose discarded translation
	// exceeds this keeps the transform per_draw_transform resolved for it instead of being pinned to
	// the world origin. RPCS3_REMIX_WORLDIDMAXB is the same test on the 3x3 basis deviation, in
	// thousandths (same idiom as RPCS3_REMIX_AFFINETOL). BOTH DEFAULT TO 0 = OFF = the round-21
	// behaviour, byte for byte. Neither changes world_identity_match, so the refusal gate at the end
	// of per_draw_transform still treats these draws as identity-covered and cannot drop them, and
	// neither applies to a draw whose world never resolved.
	u32 world_identity_keep_translation();
	u32 world_identity_keep_basis_milli();

	// RPCS3_REMIX_WORLDIDMAXTEXEMPTVP=<comma-separated vp hashes, bound 8>: programs for which the
	// WORLDIDMAXT / WORLDIDMAXB escape hatch NEVER applies - they are always pinned to the identity,
	// i.e. the round-21 behaviour for those programs only. Empty (default) = the hatch applies to
	// every listed program exactly as before, byte for byte.
	//
	// Round 24, MEASURED. The hatch exists for programs that submit LOCAL vertices, where the
	// discarded translation is the model's placement. It is actively wrong for a program that
	// submits ABSOLUTE world vertices, because there the resolved translation is not a placement at
	// all and applying it moves already-correct geometry. This title has two such programs, and the
	// current run measures the hatch displacing both:
	//
	//   vp                  draws    kept   tmax     raw vertex box (from 'Remix worldid-draw:')
	//   BD1C10DF5703E559    24760    5584   847.5    x [-1397.69 .. +1146.32]  - 2544 units across
	//   7F02E76D7369D09E   131397      38   293.3    world-sized, same shape
	//
	// A bd1c draw carrying translation=417.553 has raw=[-1397.69 ...]..[+1146.32 ...]: the vertices
	// already span the map, so the 417 units are added on top of a correct position. 92 of 140
	// traced bd1c rows sit above the WORLDIDMAXT=32 threshold and are therefore kept and displaced.
	// Both programs were measured at 0.0% displaced in round 22 ("absolute coords, identity is
	// free"), which is exactly why they are the two that must be exempt rather than the two that
	// benefit.
	bool world_identity_keep_exempt_matches(u64 hash);
	u32 world_identity_keep_exempt_count();

	// --- round 12 --------------------------------------------------------------------------------

	// RPCS3_REMIX_DIAGLINES=1 (default): emit the bounded round-11/12 diagnostic census lines.
	//
	// ROUND 12'S HEADLINE DEFECT, and it made round 11 unverifiable. All four of round 11's new
	// census emitters - 'Remix vcolroute:', 'Remix hazefade:', 'Remix selflit-miss:', 'Remix fxref:'
	// - opened their guard with dump_enabled(), which is RPCS3_REMIX_DUMP (set to 0 in this title's
	// launcher) OR the "Log Draw Diagnostics" config flag (off). So all four were structurally
	// silent for the whole session while their counters climbed: measured hazefade_applied=22365 and
	// vcol_route_blocked=288153 with ZERO lines of either. The partition is exact - in the round-11
	// run every one of the seven dump_enabled()-gated censuses emitted 0 lines and every one of the
	// eleven ungated censuses emitted, so the gate is the whole mechanism.
	//
	// dump_enabled() is the right gate for the UNBOUNDED dumps it also guards ('Remix vp= slice:',
	// the per-texture and per-fragment-program dumps) - those cost frame time and are opened for one
	// window at a time. It is the wrong gate for a census capped at 64 lines per RUN, which is what
	// these four are: they cost a hash-set probe per draw, exactly like 'Remix effect:' and
	// 'Remix sky-census:' next to them, both of which are unconditional and both of which printed.
	//
	// 0 restores round 11's behaviour exactly (the four lines fall silent again). It does NOT change
	// any decision - all four functions are pure reporters.
	bool diag_lines_enabled();

	// RPCS3_REMIX_VCOLCONST=1 (default): classify and replay the 'constant' vertex-colour route.
	//
	// Round 11's offline sweep found programs whose COL0 is 'MOV o1.xyzw, c[K]' - a flat colour from
	// the constant file, with the mesh's ATTR3 never read. Round 11 lumped that shape in with
	// 'computed' and refused it, so those draws render WHITE. That is the safe failure direction but
	// it is not the colour: the HUD gauges' ground truth (the user's Vulkan-renderer screenshot) is
	// amber, and the amber lives in c[K].
	//
	// The replay is deliberately narrower than every other route in two ways:
	//   * RGB only. The submitted ALPHA stays at the decode loop's opaque 255, because a constant
	//     alpha of 0 would delete the draw and rounds 9/10 already proved what replaying an alpha
	//     this backend has not measured costs. vcol_alpha_from_attr is false for this route by
	//     construction, so every alpha consumer refuses it without a special case.
	//   * No ATTR3 map. This route runs BEFORE map_attribute(3), because the whole point is that
	//     these meshes need not have an ATTR3 at all - going through the attribute path is how the
	//     colour was silently dropped even once the route was known.
	//
	// The knob gates ONLY the replay, in vcol_route_replayable(); scan_vcol_route names the route
	// unconditionally. So 0 restores round 11's RENDERING bit-exactly - the route is not replayable,
	// the draw takes the same refusal 'computed' took, and it goes back to white - while the census
	// still prints route=constant and the live cval=[...]. The measurement never depends on having
	// already adopted the change it exists to justify. Counter: vcol_const_applied.
	//
	// Round 13, two corrections to the above:
	//   * "the knob gates ONLY the replay, in vcol_route_replayable()" was one site short. That
	//     function returns an unconditional 'true' when RPCS3_REMIX_FPVCOLROUTE=0, so with the route
	//     scanner off this knob was bypassed entirely and the flat replay ran anyway. The flat
	//     branch in apply_vertex_colour now re-tests it. Both knobs mean what they say again.
	//   * The classifier additionally requires the 'MOV o1.xyzw, c[K]' to be UNCONDITIONAL. A
	//     conditional write replayed on every vertex is a WRONG colour, not a refusal, and this
	//     route's entire safety argument is that its failure direction is white.
	bool vcol_constant_route_enabled();

	// --- round 14: the sun, aimed at the game's own sun ------------------------------------------
	//
	// FIRST, THE CORRECTION THAT EVERYTHING HERE RESTS ON. The light this scene is lit by is the
	// backend's OWN distant light, created in ensure_sun_light() as a remixapi_LightInfoDistantEXT
	// with light_info.hash = 0x4 and aimed by sun_direction() above. It is NOT
	// rtx.fallbackLightDirection. Round 14's brief was written against the fallback light and that
	// premise was wrong.
	//
	// SECOND, AND UNCOMFORTABLE: the user's rtx.conf sets rtx.fallbackLightMode = 2 (Always), and at
	// Always the runtime creates its fallback distant light REGARDLESS of how many external lights
	// the client has supplied - rtx_light_manager.cpp:247-254 short-circuits on the mode and never
	// consults noLightsPresent. So this scene currently has TWO distant suns: ours at
	// {-0.35,-0.9,-0.25} and the runtime's at its default {-0.2,-1.0,0.4}. Aiming ours does not move
	// the other one. The conf recommendation that goes with this code is rtx.fallbackLightMode = 0.
	//
	// THIRD, THE SIGN. remixapi_LightInfoDistantEXT::direction is the direction the light TRAVELS
	// (sun -> scene), verified end to end in the deployed runtime this round: rtx_remix_api.cpp
	// :714-721 -> RtDistantLight::tryCreate, rtx_lights.cpp:808-825 maps +Z onto it, and
	// distant_light.slangh:90 samples the light at position + (-direction) * 100000, so the shadow
	// ray travels along -direction. An overhead sun is (0,-1,0). The published vector is therefore
	// the NEGATION of "camera -> sun card".
	//
	// RPCS3_REMIX_SUNCARDALBEDO=<hex>[,...] (bound 4). The sun card's albedo content hash. Nothing
	// in this repository identifies it - see the round-14 report - so it is empty by default and the
	// census below is how it gets named.
	bool sun_card_albedo_matches(u64 hash);
	u32 sun_card_albedo_count();

	// RPCS3_REMIX_SUNCARDVP=<hex>[,...] (bound 4), round 16. The albedo list ALONE cannot express
	// "the sun" on Haze: C61753D31FB96507 is both the warmest card in the census and the visor's
	// blended layer, and it is drawn by three different programs. So the pin is a PAIR, the same
	// shape SKIPPAIRVP+SKIPPAIRALBEDO and VIEWMODELVP+VIEWMODELALBEDO already use: albedo matches
	// AND (this list is empty OR the vertex program matches it too). Empty = albedo-only, i.e.
	// round 14's behaviour bit-exactly.
	//
	// This gates the RENDER-side pin only. The material-side pin (SUNCARDEMISSIVE) runs inside
	// CreateMaterial, which is keyed on texture content and has no vertex program in scope, so it
	// stays albedo-only and CANNOT be narrowed this way.
	bool sun_card_vp_matches(u64 hash);
	u32 sun_card_vp_count();

	// RPCS3_REMIX_SUNTRACK. 0 (default) = today's behaviour bit-exactly: sun_direction()'s latched
	// value is used once at creation and never revisited. 1 = retarget the distant light from a
	// PINNED SUNCARDALBEDO draw. 2 = 1, plus allow the census's own best-scoring candidate to drive
	// it when no pin matched. 2 can be wrong on its own evidence, which is why it is not the default.
	u32 sun_track_mode();

	// RPCS3_REMIX_SUNCARDCENSUS=1 (default). 'Remix suncard:' - one line per (vp, albedo) per
	// window, DIAGLINES-gated. Reports the card's world centre, the camera, the derived direction
	// (both senses), its elevation and azimuth, its mean RGB and its alpha range. This census aims
	// nothing and refuses nothing; it exists because the sun card has never been identified.
	bool sun_card_census_enabled();

	// Shape bounds for both the census and the election. MAXVTX 64: a sun sprite is a quad, allowing
	// a small flare chain. MINELEV 2 degrees: a candidate below the horizon is not the sun, and
	// without that term every blended ground decal qualifies. Negative MINELEV disables the test.
	u32 sun_card_max_vertices();
	f32 sun_card_min_elevation();

	// RPCS3_REMIX_SUNCARDMINDIST=0 (default = today's behaviour), round 16. An eye-distance floor
	// on the census LINE BUDGET only - it never suppresses a pinned or elected row, it changes no
	// counter, and it is not part of the shape gate. Measured on the round-15 run: 3394 of 6796
	// 'Remix suncard:' lines (49.9%) sit closer than 4 units, 2919 of them 4-vertex HUD quads from
	// vp=2f64c2f8ffd6add1, and 210 of the 236 distinct (albedo, vp) pairs survive a floor of 4.
	//
	// Distance rather than the extent or vertex floor round 16's brief proposed, because both of
	// those delete real candidates: 1360 lines (20%) are at dist >= 5 with ext < 1, and the far
	// cards drawn by f352d7dafa72d0e0 carry 3 to 8 vertices - which is the shape a sun sprite has.
	f32 sun_card_min_distance();

	// RPCS3_REMIX_VMPAIRMAXDIST=0 (default = round 22 exactly), round 23. An eye-distance CEILING
	// on the VMPAIRVP+VMPAIRALBEDO route ONLY. It exists so a first-person albedo that is SHARED
	// with distant character meshes can be admitted: distance is the discriminator the pair lists
	// do not have. See the accessor in RemixTransforms.cpp for the measured 20x separation on
	// 830d7d1b9681c475 (first-person 0.43..1.09 vs character meshes 20.7..38.8).
	f32 viewmodel_pair_max_distance();

	// RPCS3_REMIX_SUNTRACKDEG=1.0 (default). Degrees of movement before the light is re-created.
	// CORRECTED round 26, and left here as a warning: this comment used to read "the Remix C API has
	// no update-light entry point ... so a retarget is a destroy+create". That was FALSE, it stood for
	// twelve rounds, and the destroy+create it justified was DELETING THE SUN - remixapi_DestroyLight
	// only queues, remixapi_Present applies destroys first and then tombstones any create for the same
	// handle in the same frame. CreateLight with an existing hash IS the update (LightManager::
	// addExternalLight -> updateLightStaticSleep). The hysteresis below is now only about cost.
	f32 sun_track_hysteresis();

	// RPCS3_REMIX_SUNCARDEMISSIVE=0 (default) + RPCS3_REMIX_SUNCARDINT=2.0. ITEM 2: the sun card
	// renders as a hard opaque rectangle with visible edges. This gives a listed albedo the SAME
	// treatment round 13 gave the sky dome - emissive material, albedo as the per-texel emissive
	// texture, and BlendType::kEmissive so the instance becomes unordered - reusing that machinery
	// rather than adding a second one. For a glare card kEmissive is the whole fix: opacity -> 0, so
	// the dark texels stop being drawn and the card's edges stop occluding, while the bright core
	// still emits. Inert until SUNCARDALBEDO names a hash. Counter: mat_suncard.
	bool sun_card_emissive_enabled();
	f32 sun_card_emissive_intensity();

	// --- round 27: the particle billboard replay --------------------------------------------------
	//
	// Six vertex programs on this title expand a billboard INSIDE the vertex program, so the 4x4 into
	// HPOS takes a computed temp rather than an attribute and the matcher refuses every one of them
	// with arch=unknown / "no matrix chain into HPOS". MEASURED round 26/27: lay_other=209,904 draws
	// dropped in a single run, and that population is this title's smoke, missile trails and
	// explosions. Both lists are EMPTY by default, which is byte-for-byte today's behaviour.
	//
	// THE MEASUREMENT THAT SHAPED THE IMPLEMENTATION, and it corrects round 26's framing: these are
	// NOT one-vertex point sprites. The guest submits FOUR REAL VERTICES per quad and the shader only
	// DISPLACES them. Census over the whole round-26 run: 6,148 'Remix world-refused:' lines across
	// all six programs, every single vtx= divisible by 4, zero exceptions. So the replay rewrites
	// positions in place - it never changes the vertex count or the index buffer, which is what makes
	// it safe to run this late in submit_subdraw().
	//
	// Two families, two knobs, because they are different geometry and one may be right while the
	// other is wrong:
	//
	// RPCS3_REMIX_PARTICLEBILLBOARDVP - family A, the camera-facing rotating sprite. Decoded from
	//   2D5186A8011589B8 / 2FC998873A9F54B4 / 4ECE0A28F80FFC72 / 946A6296D06C4AF8:
	//       corner = v8.xy * c467.z - c467.w                 (the [0,1] -> [-1,+1] remap)
	//       RIGHT  = (c8.x, c9.x, c10.x)                     (view basis rows = world axes)
	//       UP     = (c8.y, c9.y, c10.y)
	//       A      = RIGHT*cos(ATTR0.w) + UP*sin(ATTR0.w)    (per-particle roll)
	//       F      = normalize(ATTR0.xyz - c26.xyz)          (c26 is the eye)
	//       B      = normalize(cross(A, F))
	//       k      = saturate((|ATTR0.xyz - c26| / (saturate(min(v8.z,v8.w)) + c467.y) - c85.z)*c85.w)
	//       world  = ATTR0.xyz + A*(v8.z*k*corner.x) + B*(v8.w*k*corner.y)
	//   and o[TEX0].xy = v8.xy * (v10.zy - v10.xw) + v10.xw, which is a PER-VERTEX scale AND bias from
	//   a second attribute - a form UVAFFINEALL/UVSCALELANES can never resolve, which is why the UV is
	//   replayed here too rather than left on the 1/4096 fallback.
	//
	// RPCS3_REMIX_PARTICLERIBBONVP - family B, the trail ribbon. Decoded from 315E21388632FE3F /
	//   391B10C33305C812. Two endpoints, no roll, no size fade:
	//       base  = ATTR0.xyz + (v12.xyz - ATTR0.xyz) * v8.x
	//       T     = normalize(v12.xyz - ATTR0.xyz)
	//       V     = normalize(ATTR0.xyz - c26.xyz)
	//       world = base + normalize(cross(T, V)) * ((v8.y*c467.y - c467.z) * v8.z)
	//   and o[TEX0].xy = (v10.zy - v10.xw) * (v8.w*(c467.z - v8.x), v8.y) + v10.xw.
	//
	// Both replays produce WORLD-space positions, because in both programs the matrix that follows is
	// the fused view*projection in c0..c3. The draw is therefore submitted at transform = identity.
	// The census prints c26 beside the backend's own camera position so that "is the particle pass's
	// world the same world as the gauge anchor's" is answered by the log rather than assumed.
	bool particle_billboard_vp_matches(u64 hash);
	u32 particle_billboard_vp_count();
	bool particle_ribbon_vp_matches(u64 hash);
	u32 particle_ribbon_vp_count();

	// RPCS3_REMIX_PARTICLECENSUS=<lines>, default 0 = OFF, clamped to 64. One 'Remix particle:' line
	// per (program, outcome) per window. This is the acceptance instrument: it prints the decoded
	// centre, size, corner remap, the derived basis and the eye, so a wrong replay can be corrected
	// from the log instead of from another blind round.
	u32 particle_census_max();

	// RPCS3_REMIX_PARTICLEUV=1 (default). Replay the atlas sub-rect UV as well as the position. 0
	// leaves the UV on whatever apply_texcoords() decided, which on these programs is the fixed
	// 1/4096 fallback - useful only to isolate whether a bad-looking particle is geometry or texture.
	bool particle_uv_enabled();

	// RPCS3_REMIX_PARTICLEFLIPBOOKVP=<hex>[,...], default EMPTY. Two of the six programs
	// (946A6296D06C4AF8 in family A, 391B10C33305C812 in family B) index an ANIMATION GRID inside the
	// atlas rect instead of using the rect directly:
	//     gx=v9.x  gy=v9.y  phase=v9.z ; f = (gx*gy - 1)*phase ; f0 = floor(f) ; row = floor(f0/gx)
	//     uv = ( base.u/gx + (f0/gx - row), base.v/gy + row/gy )
	// It needs its own list rather than being detected, because the other four programs also feed
	// ATTR9 (they read only v9.w, as an opaque passthrough) - so "ATTR9 exists" is not the test, and
	// applying the grid to them would move every particle's UV onto the wrong cell. Empty = every
	// listed program uses the plain sub-rect, which is correct for four of the six.
	bool particle_flipbook_vp_matches(u64 hash);
	u32 particle_flipbook_vp_count();

	// RPCS3_REMIX_PARTICLEFLIPPHASE=1 (default), 0 = round 27's behaviour.
	//
	// Of the three values the grid comment above names, gx and gy are per-effect constants but PHASE
	// IS PER PARTICLE - it is that particle's age through its own animation. Round 27 read all three
	// once, from vertex 0 of the draw, and applied that one cell to the entire batch. These draws
	// batch heavily (censused vtx counts up to 1,620, i.e. 405 quads in one draw), so that pins every
	// particle of an explosion to the first particle's frame; if that frame is a spent cell the whole
	// batch is invisible - which is the exact shape of "gun smoke and impacts render, explosions do
	// not". 1 re-reads v9.z per quad.
	//
	// It cannot regress a genuinely per-effect phase, but the reason is by VALUE and not by pointer:
	// map_attribute() cannot hand back a stride-0 view at all (it refuses a non-persistent attribute
	// and a zero stride separately), so a register-sourced ATTR9 turns the flipbook path OFF rather
	// than aliasing vertex 0. If the stream genuinely carries one phase per effect, every vertex holds
	// the same value and the per-quad fetch returns it.
	bool particle_flipbook_phase_per_quad();

	// --- round 27: aim the sun from the sun SPRITE -------------------------------------------------
	//
	// RPCS3_REMIX_SUNSPRITE=<albedo>[,<albedo>...], default EMPTY = OFF = today's behaviour exactly.
	//
	// Haze draws its sun as a 64x64 SCREEN-SPACE sprite (albedo A61A3CBECA257FE0, route=2d, drawn by
	// 2f64c2f8ffd6add1 and 2f650a38ffe6add1). That is why it is not clickable in the dev menu and why
	// round 16's "head-locked, dist pinned at ~2.0" measurement was right about the geometry and wrong
	// about the conclusion. Because the sprite is drawn where the sun APPEARS, its screen position
	// encodes the sun's direction: unproject the quad's NDC centre through the live camera and the
	// resulting ray is eye->sun, per level and per frame, with no texture analysis and no hand table.
	//
	// PRECEDENCE WHEN ARMED. **CORRECTED IN ROUND 28 - THIS BLOCK DOCUMENTED THE OPPOSITE OF THE CODE**
	// for a whole round. It read "SPRITE > SUNMAP ... SUNMAP is demoted below the sprite", which was
	// round 27's FIRST DRAFT; that draft was reversed before shipping (the reasoning is at the head of
	// update_sun_light(): every level pak authors its own k_scene_sun block, so SUNMAP is file-derived
	// ground truth wherever it has an entry and ground truth outranks a derivation). The shipped chain,
	// then and now, is:
	//     SUNMAP  >  SPRITE  >  SUNSKY  >  SUNTRACK card  >  SUNDIR
	// SUNMAP is already on top, so blanking SUNSPRITE does not "put it back" - it removes the automatic
	// per-level rung that covers every level SUNMAP has no entry for. No rebuild either way.
	//
	// GUARDED, because a sprite clipped to a screen edge no longer sits where the sun is: the quad
	// must be FULLY inside the NDC cube, and its two NDC spans must both be under SUNSPRITEMAXSPAN and
	// within 2x of each other (it is a square sprite). Rejections are counted and censused, not
	// silently dropped.
	bool sun_sprite_albedo_matches(u64 hash);
	u32 sun_sprite_albedo_count();

	// RPCS3_REMIX_SUNSPRITEMAXSPAN=0.5 (default), NDC units. Ceiling on either span of the accepted
	// quad. A full-screen draw sharing the albedo is not a sun.
	f32 sun_sprite_max_span();

	// RPCS3_REMIX_SUNSPRITECENSUS=<lines>, default 0 = OFF, clamped to 64. 'Remix sunsprite:' lines
	// carrying the NDC box, the derived direction and the reject reason.
	u32 sun_sprite_census_max();

	// RPCS3_REMIX_SUNSPRITEHOLD=<frames>, default 0 = round 27's one-frame rule exactly. Clamped to
	// 3600 (~60 s at 60 fps, and far shorter than any Haze level).
	//
	// The sprite can only be solved while it is FULLY on screen, so it necessarily stops solving the
	// moment the player tilts far enough for the sun to leave the view - and one frame later the
	// precedence chain drops to the sky-texture centroid, which on the land-carrier level points ~100
	// degrees away. MEASURED in the round-27 run at frames 4337 -> 4339: src=sprite
	// travel=[0.3695 -0.8084 -0.4582] then src=sky travel=[-0.1993 -0.3827 0.9021], moved=100.2 deg.
	// A sun that swings with head pitch is worse than a sun that is slightly wrong and stays put.
	//
	// This holds the last SOLVED DIRECTION and recomputes nothing while held; 'Remix sun-retarget:'
	// prints src=spritehold rather than src=sprite so the two states are never confused. The bound
	// exists so a level change still releases it.
	//
	// **IT APPLIES TO THE SUNMAP RUNG TOO, AND THAT IS NOT OPTIONAL.** m_sky_sun_frame is republished
	// only while that area's dome is being drawn, so holding only the sprite would INVERT the
	// precedence above: any two frames without a dome would drop SUNMAP out of the chain and let the
	// held sprite re-aim the light away from file-derived ground truth. Both rungs take this same
	// value, so they can never trade places because of it, and `src=sunmaphold` names the SUNMAP half.
	// Known residue, bounded and deliberate: neither latch is cleared on a level change, so for up to
	// this many frames after a transition the previous level's direction can still be held.
	u32 sun_sprite_hold_frames();

	// --- round 27: the viewmodel (vp, fp, albedo) TRIPLE -------------------------------------------
	//
	// RPCS3_REMIX_VMTRIPLEVP / VMTRIPLEFP / VMTRIPLEALBEDO, all EMPTY by default = OFF. Any one of the
	// three being empty disarms the route, the same rule VMPAIR uses.
	//
	// Why a triple and not the (vp, fp) pair the round-27 brief proposed: MEASURED this round, the
	// fragment program does NOT partition the rig. The user's picks of BOTH the weapon (vtx=2140) and
	// the arms (vtx=4456) read fp=0ccd70030837ee85 on vp=830d7d1b9681c475, and that same fp also draws
	// four WORLD textures (1CDD5249E6504F13 has 164 fpcandidate rows and 79 world-refused rows on the
	// same program). So (vp, fp) alone would tag world geometry. The albedo term has to stay; the fp
	// is what excludes the NPC meshes that share the albedo.
	//
	// READ THIS BEFORE ARMING IT. The route is shipped disarmed on purpose. MEASURED round-26 run:
	// vm_tagged=1 out of vm_considered=13,631,618. The viewmodel tag currently fires ONCE in a
	// 72,000-frame session, so VMBASIS is acting on nothing and the arms the user is looking at are
	// going through the ORDINARY WORLD PATH. Before tagging is worth tuning, the arms' own refusal has
	// to be dealt with: they refuse with fail=tail at residue 3.2-6.2 against tol=0.02 and the tail
	// rescue does not recover them.
	bool viewmodel_triple_matches(u64 vp_hash, u64 fp_hash, u64 albedo_hash);
	u32 viewmodel_triple_vp_count();
	u32 viewmodel_triple_fp_count();
	u32 viewmodel_triple_albedo_count();
}

#endif
