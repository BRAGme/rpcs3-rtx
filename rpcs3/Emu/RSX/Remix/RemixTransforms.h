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

		// Diagnostics.
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

	// RPCS3_REMIX_SKIPVP=<16 hex digits>: drop every draw of one vertex program. The one-run
	// bisector for "which program draws that". 0 when unset.
	u64 skip_vp_hash();

	// Debug light knobs so a derived camera can be judged visually at all.
	f32 debug_light_radius();
	f32 debug_light_radiance();
}

#endif
