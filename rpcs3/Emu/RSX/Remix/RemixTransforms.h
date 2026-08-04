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

	// One step of the "vertex attribute -> address register" computation, stored innermost
	// first. Every factor is a transform constant, so the whole chain is re-evaluated per draw
	// from live register state rather than baked at scan time.
	struct bone_index_op
	{
		enum class kind : u8
		{
			scale, // v = v * c[mul_slot][mul_component]
			affine, // v = v * c[mul_slot][mul_component] + c[add_slot][add_component]
			floor  // v = floor(v)
		};

		kind op = kind::floor;
		u32 mul_slot = 0;
		u32 add_slot = 0;
		u8 mul_component = 0;
		u8 add_component = 0;
	};

	// Longest attribute -> address-register chain that is followed. The one observed program
	// needs three steps (MAD, FLR, MUL); the rest is headroom, not speculation.
	inline constexpr u32 max_bone_index_ops = 6;

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

		// The attribute component that feeds the address register, and how.
		u32 bone_attribute = 0;
		u32 bone_component = 0;
		bool bone_resolved = false;
		u32 bone_op_count = 0;
		bone_index_op bone_ops[max_bone_index_ops] = {};

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

	// Compact disassembly of the instructions that feed HPOS. This is the diagnostic that
	// says why a program came back 'unknown'.
	std::string describe_position_slice(const RSXVertexProgram& vp, u32 max_instructions = 32);

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

	// The 'pos * s + b' step described by the fingerprint, as a row-vector affine matrix.
	bool build_prescale(const vp_fingerprint& fp, mat4& out);

	// Runs the recorded bone-index op chain over one vertex's attribute component and
	// truncates like ARL does, yielding the palette slot offset 'a'. False when a constant is
	// out of range or the result is not a usable non-negative offset.
	bool evaluate_bone_offset(const vp_fingerprint& fp, f32 value, u32& out);

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
