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

	mat4 mat4_identity();
	mat4 mat4_zero();
	mat4 mat4_multiply(const mat4& a, const mat4& b);
	mat4 mat4_transpose(const mat4& a);
	bool mat4_invert(const mat4& a, mat4& out);
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
	inline constexpr u8 s_no_texcoord_scale = 0xff;

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

		// The constant slot this program multiplies its texcoord attribute by before writing TEXn,
		// and the attribute that multiply reads. s_no_texcoord_input when the slice holds no such
		// MUL, or holds more than one and they disagree. This is the divisor UVINTSCALE hardcodes:
		// Haze (BLUS30094) keeps it in c151 and it is not one value across programs - measured
		// 1/32768 on most, ~1/4094 on some and 1 on others - so no fixed constant can be right for
		// all of them. Read the slot instead; s_no_texcoord_scale falls back to UVINTSCALE.
		u8 texcoord_scale_slot[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		u8 texcoord_scale_input[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

		// The program divides the position by the attribute's own w before the first matrix
		// ('pos.xyz * RCP(pos.w)'). The divisor is per vertex, so unlike has_prescale it cannot
		// be folded into the world transform - the submitted vertex has to be divided at decode
		// time or the mesh is blown apart from the inside. Always ATTR0; the matcher refuses any
		// other input because ATTR0 is the only attribute submitted as a position.
		bool has_wdivide = false;

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

		u32 instructions = 0;
		const char* note = "";
	};

	// Reads the fragment ucode and reports which sampled units feed the final colour. 'ucode' is
	// the rebased program start (RSXFragmentProgram::get_data()), 'ucode_length' its byte length,
	// and 'fp32_outputs' the CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS bit, which is what decides
	// whether COL0 is R0 or H0 (FragmentProgramDecompiler.cpp:64-82). Never throws; anything it
	// cannot follow comes back with colour_mask 0 and a reason in 'note'.
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

	// RPCS3_REMIX_STREAKGATE=<ratio>: how many times the frame's own median world extent a draw's
	// post-transform bounding box may span before audit_world_extent refuses it. 0 restores the
	// behaviour at 81af315, where nothing measured the geometry Remix actually receives.
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
	bool skinid_enabled();
	u32 skinbone_index();
	bool skinraw_enabled();

	// RPCS3_REMIX_SKIPVP=<16 hex digits>: drop every draw of one vertex program. The one-run
	// bisector for "which program draws that". 0 when unset.
	u64 skip_vp_hash();

	// RPCS3_REMIX_WORLDVP=<16 hex digits>: print the world transform one named vertex program is
	// being given, once per stats window, to RPCS3.log and remix_dump.log. The companion to SKIPVP:
	// SKIPVP names the program by making its draws disappear, WORLDVP then says what transform that
	// program was placed by, which branch of per_draw_transform built it, and - the load-bearing
	// number - the L1 residue of the perspective row that to_remix_transform is about to truncate
	// away. A residue near s_world_affine_tolerance means the draw is being flattened from a genuine
	// projective transform, which drifts with camera pitch instead of snapping. 0 when unset.
	u64 world_vp_hash();

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

	// Debug light knobs so a derived camera can be judged visually at all.
	f32 debug_light_radius();
	f32 debug_light_radiance();

	// ---------------------------------------------------------------------------------------
	// Instance categories
	// ---------------------------------------------------------------------------------------
	// Comma-separated 16-hex albedo *content* hashes - the same values the 'Remix tex=' dump
	// line and the Remix dev menu display. Setting categoryFlags at submit time is the only
	// mechanism that reaches a submitExternalDraw-path draw: the rtx.*Textures conf lists are
	// matched on the D3D9 path only, which is why tagging a texture in the dev menu did
	// nothing for this backend.
	//   RPCS3_REMIX_CAT_SKY      -> SKY          (selects the sky camera AND hides the instance)
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
}

#endif
