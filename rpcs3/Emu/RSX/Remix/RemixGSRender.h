#pragma once
#include "Emu/RSX/GSRender.h"

#ifdef _WIN32
#include "Emu/RSX/Overlays/overlay_controls.h"
#include "Emu/RSX/Remix/RemixCompositor.h"
#include "Emu/RSX/Remix/RemixRuntime.h"
#include "Emu/RSX/Remix/RemixTextures.h"
#include "Emu/RSX/Remix/RemixTransforms.h"
#include "Emu/RSX/Remix/RemixVertexDecode.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#endif

class RemixGSRender : public GSRender
{
public:
	u64 get_cycles() final;

	RemixGSRender(utils::serial* ar) noexcept;
	RemixGSRender() noexcept : RemixGSRender(nullptr) {}

	void on_init_thread() override;
	void on_exit() override;
	void flip(const rsx::display_flip_info_t& info) override;
	void do_local_task(rsx::FIFO::state state) override;

	// Guest-thread entry point (rsx::g_access_violation_handler, called from the host fault
	// handler in Utilities/Thread.cpp). MUST be implemented: rsx::reports::ZCULL_control marks the
	// occlusion-report page PROT_NONE while a query is in flight, and the only thing that ever
	// unmarks it is this callback. Without it the guest re-faults on that page forever, and because
	// the fault path sets cpu_flag::temp the spinning thread can never acknowledge cpu_flag::suspend,
	// which wedges lv2's g_pending and freezes the whole emulated machine.
	bool on_access_violation(u32 address, bool is_writing) override;

private:
	void end() override;

#ifdef _WIN32
	struct mesh_entry
	{
		remixapi_MeshHandle handle = nullptr;
		u64 last_used_frame = 0;
	};

	// Every skip has its own counter so a "submitted == 0" result says which gate to relax.
	struct stat_counters
	{
		u64 draws_seen = 0;
		u64 draws_submitted = 0;
		u64 skip_immediate = 0;
		u64 skip_inline_array = 0;
		u64 skip_volatile = 0;
		u64 skip_register_attr0 = 0;
		u64 skip_primitive = 0;
		u64 skip_restart_index = 0;
		u64 skip_instanced = 0;
		u64 skip_layout = 0;
		u64 skip_memory = 0;
		u64 skip_decode = 0;
		u64 skip_poisoned = 0;
		u64 skip_screen_space = 0;
		u64 meshes_created = 0;
		u64 meshes_destroyed = 0;
		u64 cam_resolved = 0;
		u64 cam_fallback = 0;
		// Frames that produced no candidate of their own and reused the previous frame's camera
		// instead of falling back to the origin one. cam_fallback keeps its old meaning - a frame
		// with no usable camera at all - so both counters stay comparable with captures taken
		// before the hold existed. R2 (NPEA00431) measured cam_resolved=10438 cam_fallback=5352,
		// i.e. 33.9% of frames dropped a perfectly good camera; those frames land here now.
		u64 cam_held = 0;
		// The fused-split path in update_camera_candidate: how often it was entered, and how often
		// split_view_projection could not factor the matrix. The pair says whether a frame with no
		// candidate had no 3D draws at all (attempted == 0) or had them and could not split them.
		u64 split_attempted = 0;
		u64 split_failed = 0;
		u64 world_applied = 0;
		u64 world_fallback = 0;
		// Of the world_fallback population, how many were refused rather than drawn at the
		// identity. Counted separately so world_fallback stays comparable across every run
		// taken before the refusal existed. RPCS3_REMIX_DRAWNOWORLD=1 drives this back to 0.
		u64 world_refused = 0;

		// The world_refused subset that failed only because the frame had no camera at all -
		// per_draw_transform refuses on !m_active_camera.valid before it looks at the program.
		// Split out because the two have completely different fixes: this one is a camera that
		// did not resolve for the whole frame, the remainder is a vertex program whose matrix
		// chain could not be read. Inferring the ratio from flip counts put it near 92%, which
		// is far too load-bearing a number to leave as arithmetic on two other counters.
		u64 world_refused_nocam = 0;
		// Draws whose world came from folding the *whole* group chain of a layered program under a
		// fused reference camera, instead of only its outermost group. Appended after world_refused
		// rather than folded into it so captures taken before the full-chain fold existed stay
		// comparable. Zero means the fix never fired: either no layered program reaches that branch
		// on this title, or RPCS3_REMIX_FULLCHAIN=0. It counts draws, not programs, so it scales
		// with how wide the changed placement path is - which is the tell the risk note asks for.
		u64 world_layered_ref = 0;
		// A 3D draw whose albedo samples a surface the RSX itself rendered into: a post-process
		// pass classified as world geometry. The 3D twin of ui_render_target.
		u64 skip_render_target = 0;
		// Sampled a bound surface but was too big to be a post-process quad - a shadow-mapped or
		// probe-lit world draw. Kept. The pair (skip_render_target, rt_feedback_kept) is what
		// says whether the shape test is doing anything.
		u64 rt_feedback_kept = 0;
		// RPCS3_REMIX_STRICTINPUT only: draws refused because their matrix chain never reached
		// the vertex attribute.
		u64 skip_not_input = 0;
		// Draws whose positions were divided by ATTR0.w at decode time, undoing the packing the
		// ucode undoes. RPCS3_REMIX_NOWDIV=1 drives this to 0.
		u64 wdiv_draws = 0;
		// Draws refused because the program's recognised 'pos = attr * s + b' decode could not be
		// rebuilt from live constants (slot out of range, zero or non-finite scale). Drawing them
		// anyway means raw quantised positions against a matrix expecting decoded ones - the vertex
		// explosion. Non-zero with world_applied unchanged means the matcher is recognising a decode
		// whose constants are not where it thinks; RPCS3_REMIX_POSAFFINE=0 drives it to 0.
		u64 pos_decode_refused = 0;
		// Draws dropped as a re-draw of already-established geometry: no depth write, and the
		// fragment program samples no texture on unit 0. On R2 (NPEA00431) these are the passes
		// that light the base pass using only a normal map, and submitting them to a path tracer
		// covers the diffuse surface with the normal map instead of blending into it. Zero on a
		// title that renders in a single pass. RPCS3_REMIX_PASSSKIP=0 drives it to 0; if the scene
		// loses real content, that knob is the bisect and this counter says how much went.
		u64 skip_lighting_pass = 0;
		u64 tex_bound = 0;
		u64 tex_none = 0;
		// Of the tex_none population, how many had no referenced, enabled 2D fragment texture
		// unit at all - i.e. the draw is not textured by the title, so no budget, format or
		// decode change can ever give it a material. Separating this from "the cache refused"
		// is what says whether a white surface is our bug or the title's own vertex-coloured
		// geometry (Haze's sky dome, vp=fc0fac8afccec49a, is the latter).
		u64 tex_no_unit = 0;
		// Of the tex_bound population, how many left with real texcoords rather than the
		// (0,0) every vertex used to carry. uv_applied far below tex_bound means the albedo is
		// there and the coordinates are not, which renders as one flat colour per draw.
		u64 uv_applied = 0;
		u64 uv_none = 0;
		// The uv_none population split by the best attribute_status the scan ever saw, so a total
		// failure says *which* gate refused. R2 measured uv_none == tex_bound == 2450329 exactly,
		// i.e. every textured draw in the session; the aggregate alone cannot tell "the title feeds
		// no such attribute" from "it does and we mis-read the block". uv_none is kept as the
		// aggregate so every capture taken before this split stays comparable, the same rule the
		// world_refused comment above states. The residual
		// uv_none - (uv_absent + uv_layout + uv_memory) is the decode-time rejection below, where
		// an attribute mapped fine but its type/size pair is one decode_position cannot read.
		u64 uv_absent = 0;
		u64 uv_layout = 0;
		u64 uv_memory = 0;
		// Resolved from an attribute other than 8+unit, i.e. the convention did not hold.
		u64 uv_fallback = 0;
		// The uv_applied population split by how the attribute was chosen: read out of the program's
		// own TEX<unit> write, or inferred by the size/type heuristic that is now the fallback. The
		// two always sum to uv_applied. R2 (NPEA00431) is the case that forced the split: replaying
		// the resolver over its 70 TEX0-writing programs resolves 67 of them and picks attribute 1
		// for 54, attribute 3 for 11 and attribute 2 for 2 - i.e. 13 programs whose UVs the
		// size/type heuristic could never have got right, since it excludes attribute 3 outright as
		// RSX's colour register. A high uv_applied alone cannot say whether those draws are correct.
		u64 uv_ucode = 0;
		u64 uv_heuristic = 0;
		// Times the widened low-attribute scan mapped a 2-component stream and then threw it away
		// because a sampled vertex did not decode to a finite pair. Counted separately from the
		// uv_* statuses above because this refusal deliberately leaves 'best' at 'absent' (see
		// resolve_texcoord_attribute), so without it a draw refused here is indistinguishable in
		// the stats from a draw whose title feeds no texcoord attribute at all. Not a subset of
		// uv_none: the scan may refuse attribute 1 here and still resolve a later one.
		u64 uv_nonfinite = 0;
		// S32K texcoord draws whose divisor came from the constant slot the vertex program
		// multiplies by, rather than the fixed UVINTSCALE. Reads against uv_applied: on Haze this
		// should cover the population that was tiling 8x, and on a title whose ucode names no
		// scale it stays 0 and nothing changes.
		u64 uv_scale_ucode = 0;

		// The other half of the S32K population: draws whose program named no scale, so the fixed
		// UVINTSCALE divisor was used. uv_scale_ucode alone could not be read, because it was only
		// ever comparable against uv_applied - which counts every UV path including the normalised
		// formats that never divide at all - and 344234/738738 says nothing about how many S32K
		// draws were actually guessed at. These two sum to the S32K population exactly.
		u64 uv_scale_fixed = 0;
		// Untextured draws that left with the title's own ATTR3 colour instead of flat white.
		u64 vcol_applied = 0;
		// Times a referenced texture unit yielded no material and the next one was tried.
		u64 tex_unit_retry = 0;
		// How the albedo unit was chosen, split by evidence. tex_albedo_ucode counts draws whose
		// fragment program named at least one colour source and excluded at least one other
		// referenced unit; tex_albedo_guess counts the rest, which fall back to 9c73eb0's lowest
		// referenced enabled 2D unit. The two sum to the draws that had any unit at all. A high
		// guess count on a title with two-unit materials means the walk in scan_fragment_program
		// is not following that title's shaders, not that the units are unambiguous.
		u64 tex_albedo_ucode = 0;
		u64 tex_albedo_guess = 0;
		// Retries the ucode refused to sanction: the first unit yielded no material and the program
		// named no *other* colour source to walk to. Before this refusal existed the loop walked to
		// the next referenced unit regardless, which on Resistance 2 (NPEA00431) is unit 1 - 566
		// DXT45 binds, zero DXT1, the normal map - and bound it as albedo. That is the flat blue.
		// A missing texture is better than a wrong one, so the draw leaves untextured and says so.
		u64 tex_retry_refused = 0;
		// Times the walk stepped past a unit that bind() refused for a permanent format reason
		// (a tombstoned descriptor) rather than letting tex_retry_refused stop it. There is no
		// good albedo on such a unit to substitute away from, so the guard above does not apply.
		// Reads against tex_unsupported: on Haze the two should track, because its refused unit
		// is one 2048x2048 DEPTH16 shadow map bound by every shadow-receiving draw. If this is
		// high while tex_unit_substituted stays 0, the walk is running but finding nothing.
		u64 tex_retry_unsupported = 0;
		// Draws whose material came from a unit other than the first one chosen, i.e. a retry
		// actually substituted. Non-zero with tex_albedo_ucode high is the healthy case (Haze's
		// COMPRESSED_HILO8 normal map below its diffuse); non-zero with tex_albedo_guess high is
		// the population that can still be wrong.
		u64 tex_unit_substituted = 0;
		u64 ui_draws = 0;
		u64 ui_skipped = 0;
		u64 ui_no_colour = 0;
		u64 ui_render_target = 0;
		// Which coordinate space composite_ui_draw decided each 2D draw was already in. The
		// UI probe put the bar it draws at the top AT the top and the bar it draws at the left
		// AT the left, so nothing downstream of the compositor mirrors; the only place left for
		// the reported vertical flip is this classification and the row conversion that follows
		// it. The 'unit' counter is the load-bearing one: it counts draws whose post-matrix
		// bbox fits entirely inside [-0.05, 1.05] on both axes, which the extent<=1.5 test
		// cannot tell apart from real [-1,1] NDC even though the two spaces disagree about
		// where y=0.4 lands (row 0.4h vs row 0.3h) and about which way y grows.
		u64 ui_space_ndc = 0;
		u64 ui_space_unit = 0;   // subset of ui_space_ndc: indistinguishable from [0,1] top-left

		// 2D draws whose HPOS.xy-only transform was rebuilt and applied instead of being dropped
		// (vp_fingerprint::has_ortho2d). Zero on a title whose 2D programs all write a full 4x4.
		u64 ui_ortho2d = 0;
		u64 ui_space_pixel = 0;  // clip-pixel branch
		u64 ui_space_none = 0;   // neither shape; refused rather than guessed

		// The clip-pixel branch's row conversion, audited. As of ae94587 it multiplied the guest
		// coordinate by fh/surface_clip_height, which silently asserts that guest pixel y=0 is the
		// top row; no capture ever established that. It is now derived from the viewport registers
		// the guest programmed, which say so outright - RSX window space is y-down exactly when
		// viewport_scale_y is negative. R2 (NPEA00431) reads vp_scale_y=-352 vp_offset_y=352 at
		// clip=1280x704, where the derivation collapses back to the old expression, so ydown is
		// expected to carry every pixel-branch draw there and yup to stay 0. A non-zero yup on any
		// title is the population the old form drew mirrored. fallback counts draws whose viewport
		// was unusable (scale ~0 or non-finite) and kept the legacy form.
		// RPCS3_REMIX_UISPACE=0 restores it everywhere.
		u64 ui_space_vp_ydown = 0;
		u64 ui_space_vp_yup = 0;
		u64 ui_space_vp_fallback = 0;

		// The orientation audit, and the measurement that settles the remaining upside-down HUD
		// without a screenshot or a ucode read. A textured UI quad is upright exactly when the
		// atlas row it samples grows the same way the composited row does, so the sign of the
		// covariance between v and screen y over one triangle is the answer, per draw.
		//
		// Split by branch because the comparison is the point. Against
		// ui_draws=91008 ui_no_colour=18610 ui_ndc=22413 ui_unit=5064 ui_pixel=218395
		// ui_nospace=0 ui_ortho2d=66702, the ortho2d/NDC family is confirmed correct on screen, so
		// it calibrates 'ok' for this title's atlases and V convention. ndc_ok dominant while
		// pixel_bad dominant is the flip, proven; both families agreeing means the flip is not in
		// this classification and the fragment side is the next place to look. A draw with no UVs,
		// or whose first triangle spans no v or no y, votes for neither.
		u64 ui_vflip_ndc_ok = 0;
		u64 ui_vflip_ndc_bad = 0;
		u64 ui_vflip_pixel_ok = 0;
		u64 ui_vflip_pixel_bad = 0;
		// Textured draws whose first triangle carried no usable orientation - too little vertical
		// span, too little v span, or a v that tracks x rather than y (a rotated or skewed sprite).
		// Large relative to the votes means the audit is looking at geometry it cannot read, and
		// the ok/bad split above describes a minority of what is actually on screen.
		u64 ui_vflip_abstain = 0;
		u64 skin_submitted = 0;
		u64 skin_skipped = 0;
		u64 skin_bones_max = 0;
		// A program whose ucode carries skinning the recogniser cannot prove it understands
		// (a blend rig partially matched as single-bone). Refused, never mis-skinned: a missing
		// character is an acceptable result, an exploded one is not.
		u64 skin_unrecognised = 0;

		// Which of the three hardening conditions in scan_vertex_program refused the rig. The
		// aggregate above has been non-zero and unattributed for this whole title: 186648 draws
		// against skin_submitted=43698 in the 4:50 capture, i.e. 81% of the skinned population
		// dropped, with no record of why. The reason was already computed and stored in
		// vp_fingerprint::skin_note and simply never reported, so these cost nothing to produce.
		//   arl      more than one ARL: the address register is reloaded mid-program.
		//   foreign  the palette is read through an address component the match does not cover.
		//   reads    the program indexes the palette more times than rows x bones explains.
		u64 skin_unrec_arl = 0;
		u64 skin_unrec_foreign = 0;
		u64 skin_unrec_reads = 0;

		// The other site that feeds skin_unrecognised: the indexed-const gate, which refuses a
		// program that reads a constant palette the recogniser never turned into bone transforms.
		// The three counters above only split the scan_vertex_program hardening path, so the two
		// populations were indistinguishable in the aggregate - and on Resistance 2 (NPEA00431)
		// the hardening path contributed nothing at all: all 119357 refusals in a 2m41s capture
		// came through the gate. With this the four partition skin_unrecognised exactly.
		u64 skin_unrec_indexed = 0;

		// The two mechanisms that resolve an indexed-const refusal instead of taking it. Each is
		// its own knob and each partitions exactly: considered = resolved + refused.
		//
		// 'idxuniform' is the draw-uniform address register (RPCS3_REMIX_INDEXEDUNIFORM): the whole
		// program's indexing collapses to one constant row, so there is no palette to leave
		// unapplied. Considered counts draws whose program proved uniform, refused counts those
		// whose address did not read back inside the legal constants or whose matrix chain never
		// reached the vertex attribute (mode 1).
		//
		// 'idxbias' is the forwarded post-matrix translation (RPCS3_REMIX_INDEXEDBIASREG): a group
		// whose rows land in a different register than the translation. These draws reach the
		// skinned_layered path, so refused is the same proven-or-nothing refusal the rest of that
		// path uses - an index that does not evaluate, or a palette entry that is not a usable
		// affine transform.
		u64 idxuniform_considered = 0;
		u64 idxuniform_resolved = 0;
		u64 idxuniform_refused = 0;
		u64 idxbias_considered = 0;
		u64 idxbias_resolved = 0;
		u64 idxbias_refused = 0;

		u64 skip_vp = 0;
		// Unique vertex programs, not draws: how the HPOS writer collection went
		// (vp_fingerprint::hpos_indirect). 'recovered' is programs whose matrix chain was only
		// found by following a writer through a register, 'refused' is programs where a MOV was
		// reached whose definition could not be pinned down, and 'indexed' is the skinned rigs the
		// indirection is deliberately not offered to. Replaying the rule over Resistance 2's
		// (NPEA00431) 129 dumped programs gives 33 and 25; the dump does not print the condition,
		// negate or saturate bits, so 'refused' is the one of the three a run has to report rather
		// than confirm. RPCS3_REMIX_HPOSINDIRECT=0 drives the first two to 0.
		u64 vp_hpos_indirect = 0;
		u64 vp_hpos_refused = 0;
		u64 vp_hpos_indexed = 0;

		// What became of the draws that read a constant palette through the address register - the
		// population the gate refused whole at ae94587, where it was skin_unrecognised = 1481510 of
		// 7588805 draws with skin_submitted = 0.
		//
		// 'rigid' is a draw whose index came out the same for every vertex, so the palette read is
		// one fixed matrix and it folds into the instance transform with no bones at all: the
		// terrain-chunk and batched-prop case, and the one that does not need the Remix fork.
		// 'skinned' is a draw whose index genuinely varies, handed to build_skinning as bone
		// transforms. 'refused' is everything still proven-or-nothing: an index that does not
		// evaluate, a slot outside the 468 legal constants, or a palette matrix that is not affine.
		//
		// Programs, not draws, for the last two: 'unmatched' counts unique vertex programs whose
		// position reads an indexed constant that match_indexed_affine could not express - on
		// Resistance 2 that is the four-bone blend rigs, 16 of its 36 indexed programs, which stay
		// refused because submitting one bone of a four-bone blend is what tears a character apart.
		u64 indexed_world_rigid = 0;
		u64 indexed_world_skinned = 0;
		u64 indexed_world_refused = 0;
		u64 vp_indexed_matched = 0;
		u64 vp_indexed_unmatched = 0;

		// Draws refused because a palette entry's 3x3 basis does not span three dimensions
		// (has_usable_basis). Separate from the other refusals because it means the *index* is
		// landing outside the palette, not that the palette is unreadable - it is the counter that
		// says whether the ARL source modifiers are being replayed correctly. Should be 0 once the
		// index decode is right; a non-zero value with characters drawn is the thin-box artifact.
		u64 bone_degenerate = 0;

		// Weighted (multi-bone) skinning, the population ae94587 refused whole: 16 vertex programs
		// on Resistance 2 (NPEA00431) whose palette is read once per bone and summed, taking 541960
		// draws into skin_unrecognised with skin_submitted = 0. They are the character rigs.
		//
		// 'submitted' is draws handed to Remix with real per-vertex blend weights and indices
		// (remixapi_MeshInfoSkinning, bonesPerVertex 4). The four refusals are split because they
		// fail for different reasons and only one of them is a bug in this code:
		//   _index   a bone's index attribute did not decode, or evaluated to a slot outside the
		//            468 legal constants. The index decode is wrong, or the attribute is not the
		//            one the ucode reads.
		//   _weight  the weights are not a convex blend - non-finite, negative, or summing to zero.
		//            Remix derives the last weight as 1 - sum(the others), so a set that does not
		//            sum cannot be expressed and forcing it would move geometry.
		//   _bone    a palette entry is not a usable affine transform (is_affine /
		//            has_usable_basis). Same gate the single-bone path uses; a rank-deficient bone
		//            draws its vertices on a line, which is the reported collapsed box.
		//   _palette more than REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT distinct bones in one draw.
		// 'rescaled' is not a refusal: the draw was submitted, but its weights did not sum to 1
		// within 1/32 and were normalised. Non-zero means the weight attribute is not stored the
		// way this assumes, and the number says how much of the scene is affected.
		u64 skin_blend_submitted = 0;
		u64 skin_blend_refused_index = 0;
		u64 skin_blend_refused_weight = 0;
		u64 skin_blend_refused_bone = 0;
		u64 skin_blend_refused_palette = 0;
		u64 skin_blend_rescaled = 0;

		// Draws refused because one bone's basis or translation sits orders of magnitude away from
		// the median of the other bones in the same draw. This is the only magnitude gate on the
		// path: a 3-row palette passes is_affine (whose perspective column build_palette_matrix
		// wrote itself) and passes has_usable_basis (scale-invariant by construction), so before
		// this existed an inflating bone reached Remix with every refusal counter reading 0 -
		// which is what Resistance 2's stalker turret did while its legs, weighted to other bones
		// of the same palette, drew correctly. Non-zero here with the character still intact is the
		// gate working: the draw it refuses is the sub-mesh that used to explode.
		u64 skin_blend_refused_scale = 0;

		// Blended draws whose palette entries are all the same matrix - the shape no rig has. Six
		// of Resistance 2's eight blend rigs are in this population, reporting eight identical
		// bones at c32..c53 against the two that report 24 with a real spread. Submitted rather
		// than refused: an identical palette blends to exactly that one matrix, so the draw is a
		// correct rigid draw and refusing it would delete working geometry. It also cannot produce
		// radiating shards - it moves every vertex the same way, which is what rigid means.
		// RPCS3_REMIX_BONEUNIFORM=0 refuses them, which is the one-run A/B on that population.
		u64 skin_blend_uniform = 0;

		// Skinned draws whose blended result travels far further from its own bones than the mesh's
		// own spread explains (audit_skin_extent). Purely an instrument - nothing is refused on it -
		// and the number that matters is whether it is zero. Zero while the shards are on screen
		// exonerates the skinning path outright: an affine instance transform maps every vertex of
		// a mesh the same way and cannot tear one apart, so a torn mesh whose skinning measures
		// clean was not torn by this backend's per-vertex maths.
		u64 skin_reach_flagged = 0;

		// Draws whose decoded positions do not form one object (audit_vertex_extent), measured
		// before any transform is applied. 'spread' is the furthest vertex being a large multiple
		// of the typical one, or a non-finite position; 'zero_split' is the much more specific
		// finding that some - but not all - of the draw's vertices decoded to exactly (0,0,0),
		// which is a read that returned nothing rather than arithmetic that went wrong. Both are
		// instruments: nothing is refused on either. skin_reach_flagged staying 0 while these are
		// non-zero locates the tearing in the decode rather than the transform.
		u64 vtx_spread_flagged = 0;
		u64 vtx_zero_split = 0;

		// Of the flagged draws, how many survived every later gate and actually reached the scene.
		// The audit runs right after the decode, so 'flagged' counts draws that may still be
		// dropped for unrelated reasons - and the extreme case on Resistance 2 reports
		// arch=unknown, which has no matrix chain into HPOS and is already discarded by
		// world_refused. flagged >> submitted means the incoherent geometry is not being drawn and
		// the artifact is elsewhere; flagged == submitted means it is.
		u64 vtx_spread_submitted = 0;

		// Draws dropped by RPCS3_REMIX_VTXREFUSE=1. Zero unless that knob is set.
		u64 vtx_spread_refused = 0;

		// --- post-transform geometry census (audit_world_extent) ----------------------------
		//
		// The size of the geometry Remix is actually handed: 'instance transform x bone palette x
		// decoded position', in world units. Everything above this line is measured before at least
		// one of those three, which is why all of it read clean at 81af315 while the streaks were on
		// screen - see RemixTransforms.h, RPCS3_REMIX_STREAKGATE.
		//
		// The names say where each number is taken, because reading a flagged count as a drawn count
		// has already cost this session two captures:
		//   examined  reached the measurement. It sits after the world transform is resolved and
		//             after every refusal above it, but before the mesh, the material and
		//             DrawInstance, any of which can still drop the draw. NOT a count of drawn
		//             geometry.
		//   exempt    measured and never gated: sky domes are legitimately the size of the level, so
		//             gating them on the scene's median would delete the sky. Excluded from the
		//             median sample too, for the same reason.
		//   refused   the gate dropped it. These never reached the scene, by construction.
		//   flagged_drawn  over the ratio and drawn anyway, i.e. RPCS3_REMIX_STREAKGATE=0. This is
		//             the A/B number: non-zero here with streaks on screen and zero without is what
		//             ties the artifact to this population.
		//   drawn[]   the only histogram of geometry that reached the scene: filled from the stored
		//             extent at the point DrawInstance returned success, not at the gate.
		u64 wext_examined = 0;
		u64 wext_exempt = 0;
		u64 wext_nonfinite = 0;
		u64 wext_refused = 0;
		u64 wext_flagged_drawn = 0;

		// Decades of world extent over drawn geometry: <1, <10, <100, <1e3, <1e4, <1e5, <1e6, rest.
		u64 wext_drawn[8] = {};
		f32 wext_drawn_max = 0.f;

		// Unique vertex programs whose HPOS.z was written as a w-buffer premultiply and whose 4x4
		// was only recovered by taking z from the row feeding it (vp_fingerprint::hpos_wbuffer_z).
		u64 vp_wbuffer_z = 0;
		u64 cat_sky = 0;

		// Sky-dome detection, added after ae94587 - where it had no instrumentation at all, and an
		// over-tag was only visible as a uniformly blue scene. 'candidates' is every draw that
		// reached the test (depth writes off, and either untextured or sky_allows_textured()), and
		// it is exactly the sum of the three refusals plus the cat_sky increments the test itself
		// makes, so the shape of a mis-tag is readable in one line:
		//   candidates high, extent ~= candidates    the title's geometry is simply not sky-sized;
		//                                            the normal resting state.
		//   candidates high, cat_sky a large share   the anchor gate is not filtering - this is the
		//                                            ae94587 failure, where the raw-extent rule
		//                                            tagged cat_sky=825915 of 2038738 draws (40.5%)
		//                                            on Resistance 2 and lit the scene as sky.
		//   noworld dominating                       the camera or the world transform is not
		//                                            resolving for these draws, so the test never
		//                                            ran; look at world_refused, not at the sky.
		u64 sky_candidates = 0;
		u64 sky_refused_noworld = 0;
		u64 sky_refused_extent = 0;
		u64 sky_refused_anchor = 0;

		// Latitude bands of an already-recognised dome, admitted by the learned-dome rule after
		// failing on extent alone. Reads against sky_refused_extent: on a title whose dome is one
		// draw this stays 0, and on Haze it should absorb most of that population. High here with
		// no visible sky improvement means the rule is admitting something that is not a ring.
		u64 sky_learned_ring = 0;

		// The subset of sky_refused_anchor measured against a *held* camera - a frame that resolved
		// no candidate of its own and reused the previous frame's (m_camera_age != 0). Those are the
		// only frames on which the anchor can be measured against a camera that is not the one the
		// draw was placed by: a frame with no usable camera at all takes cam_fallback, and there
		// per_draw_transform refuses the draw outright, so it never reaches the sky test. The first
		// capture bears that out - 1242 of 4377 R2 frames were cam_fallback and sky_noworld was 0.
		u64 sky_refused_anchor_held = 0;

		// The backdrop rule (RPCS3_REMIX_SKYBACKDROP). 'hit' is every draw it selects - counted at
		// mode 1 without tagging anything, so the cost of turning it on is a number read from an
		// ordinary run rather than a blue screen. 'dw' is the subset that writes depth, i.e. the
		// part the sky-dome rule can never reach; on R2 the visible backdrop is entirely in there.
		//
		// The ratio to watch is sky_backdrop_hit / draws_submitted. The regression this is guarding
		// against measured 825915 / 2038738 = 40.5%; the backdrop the census names is three draws
		// of one program, so a healthy reading is a fraction of a percent. Anything approaching a
		// percent means the rule is selecting geometry, not backdrop, and mode 2 must not be used.
		u64 sky_backdrop_hit = 0;
		u64 sky_backdrop_dw = 0;

		// The albedo-hash sky rule (sky_hash_mode), added after 81af315. Read as ratios; the whole
		// point of the set is that an over-match is a number here rather than a missing world.
		//
		//   sky_hash_considered  every submitted draw that carried an albedo hash the rule is
		//                        tracking, i.e. one that has been dome-shaped at least once. It
		//                        partitions exactly:
		//                          considered == matched + rejected
		//   sky_hash_dome        the subset of considered whose *own* geometry was dome-shaped.
		//                        dome / considered is the rule's internal agreement: a hash that
		//                        really is the sky runs at 1.0, and anything well below it is a
		//                        texture the title also uses on something that is not a dome.
		//   sky_hash_matched     draws whose hash is currently armed (all-dome, enough samples).
		//   sky_hash_tagged      the subset actually given SKY, i.e. matched at mode 2. At mode 1
		//                        this is 0 by construction and matched is the preview of what
		//                        mode 2 would do, with the image untouched.
		//   sky_hash_rejected    considered-but-refused: the hash has been seen on a non-dome draw
		//                        and is disqualified for the life of the process. This is the
		//                        counter that says the rule is *working* rather than merely quiet -
		//                        a title whose sky texture is also a wall texture reads here.
		//   sky_hash_tracked     unique hashes in the table, capped at s_max_sky_hash_tracked.
		//
		// The ratio to watch before arming mode 2 is sky_hash_matched / draws_submitted, against
		// the two numbers this rule exists to beat: the raw-extent regression of ae94587 tagged
		// 825915 of 2038738 draws (40.5%), and the geometric backdrop rule measured 1.11%. A sky is
		// a handful of draws a frame, so a healthy reading is small fractions of a percent.
		u64 sky_hash_considered = 0;
		u64 sky_hash_dome = 0;
		u64 sky_hash_matched = 0;
		u64 sky_hash_tagged = 0;
		u64 sky_hash_rejected = 0;
		u64 sky_hash_tracked = 0;

		// Viewmodel detection (viewmodel_mode), added after 81af315. 'considered' is every draw
		// that reached the test, and the three buckets below partition it exactly:
		//
		//   viewmodel_considered == viewmodel_tagged
		//                         + viewmodel_refused_full_range
		//                         + viewmodel_refused_offset
		//
		// so an over-tag reads as one ratio. The expected resting value on Resistance 2 is
		// tagged / considered ~= 2.4% (the replay gives 168 of 7041 dumped 3D draws, 2.39%),
		// with refused_full_range carrying essentially all of the remainder. The failure shapes:
		//   tagged approaching a double-digit share   the depth-range test is selecting world
		//                                             geometry; read the census for which
		//                                             programs and set RPCS3_REMIX_VIEWMODEL=1.
		//   refused_offset large                      some title puts its whole scene in a
		//                                             sub-unit depth slice that is not pinned at
		//                                             the near end; the offset gate is doing the
		//                                             work and the rule needs re-measuring there.
		//   tagged = 0 on R2 gameplay                 the arms are not reaching submit_subdraw at
		//                                             all - look at skip_screen_space and
		//                                             world_refused, not here.
		u64 viewmodel_considered = 0;
		u64 viewmodel_tagged = 0;
		u64 viewmodel_refused_full_range = 0;
		u64 viewmodel_refused_offset = 0;

		// Tagged draws whose origin was further from the eye than s_viewmodel_max_anchor, and
		// tagged draws for which no anchor could be measured at all (no resolved world transform
		// or no valid camera). Neither refuses anything - they are the over-tag alarm described
		// on s_viewmodel_max_anchor. far > 0 with tagged small means the depth-range test found
		// something that is not in front of the player's face.
		//
		// Only meaningful at viewmodel_camera_mode 0. The anchor reads the instance transform's
		// translation, which was the draw's world origin only because the mismatched world
		// reference left one there ([-4.048 15.241 -6.4505], anchor 0.446). Under a viewmodel
		// reference the instance transform is the inter-frame camera delta and the bones carry the
		// placement, so the translation is ~0 and the anchor degenerates to the distance from the
		// world origin to the eye - about 17 on the measured frame, so vm_far tracks vm_tagged.
		// That is the alarm losing its subject, not the arms moving.
		u64 viewmodel_far = 0;
		u64 viewmodel_noanchor = 0;

		// Which reference each viewmodel draw was divided by (viewmodel_camera_mode). 'considered'
		// is every viewmodel-range draw that reached per_draw_transform's reference branch, and the
		// three below partition it exactly:
		//
		//   viewmodel_cam_considered == viewmodel_cam_applied
		//                             + viewmodel_cam_fallback
		//                             + viewmodel_cam_refused
		//
		// so an over-broad depth-range rule reads as a ratio here rather than as a missing weapon.
		// applied is the fix working; fallback is mode 0 only (the 148b467 world reference);
		// refused is mode 1, or mode 2 before any viewmodel reference has been latched.
		//
		// The tell: refused staying high through gameplay means viewmodel draws are being found but
		// no viewmodel draw is winning the candidate, so read viewmodel_cam_diverted next - if that
		// is also 0 the two populations disagree and the depth predicate is being applied to
		// different registers in the two places, which is the one bug this shared predicate exists
		// to make impossible.
		//
		// Under RPCS3_REMIX_DUMP the diagnostic path calls per_draw_transform a second time for
		// every skinned_layered program it describes, which is the viewmodel archetype, so
		// 'considered' runs a little ahead of vm_tagged on a dump run. The three-way split is
		// unaffected - each of those calls still lands in exactly one bucket.
		u64 viewmodel_cam_considered = 0;
		u64 viewmodel_cam_applied = 0;
		u64 viewmodel_cam_fallback = 0;
		u64 viewmodel_cam_refused = 0;

		// Draws routed to the viewmodel candidate instead of the world one, frames that latched a
		// viewmodel reference, and flips that reference survived without a candidate of its own.
		//
		// diverted is also the alarm at the camera end: at 148b467 update_camera_candidate had no
		// depth-range test at all, so a viewmodel draw could win the *world* camera. R2's viewmodel
		// programs are skinned_layered, which takes the layered path and publishes
		// has_reference = false, so a frame it won would move the whole world onto a different
		// branch of per_draw_transform. diverted counts what used to be able to do that.
		//
		// conflict is the assumption the reference rests on, made into a number: how many diverted
		// draws presented a view-projection differing from the one the frame already latched.
		// Expected 0 - the two viewmodel programs dumped with a G0 carry identical matrices - and a
		// non-zero reading says the population holds more than one camera and first-wins is
		// choosing between them arbitrarily.
		// unusable: diverted draws whose outer group is not the chain per_draw_transform would
		// divide by, so they cannot serve as the reference. diverted > 0 with latched == 0 and
		// unusable == diverted means every viewmodel program in the title is layered past one
		// group, and the reference has to be built from the full chain instead.
		u64 viewmodel_cam_diverted = 0;
		u64 viewmodel_cam_unusable = 0;
		u64 viewmodel_cam_conflict = 0;
		u64 viewmodel_cam_latched = 0;
		u64 viewmodel_cam_held = 0;

		u64 cat_hidden = 0;
		u64 cat_particle = 0;
		u64 cat_decal = 0;

		// Alpha blending, added after ae94587. blend_chained is every instance that carried a
		// remixapi_InstanceInfoBlendEXT (i.e. every submitted draw while RPCS3_REMIX_BLENDSTATE
		// is on), blend_translucent is the subset that reached the runtime with
		// alphaBlendEnabled = 1, and blend_unmapped is draws whose GCM factor or equation had no
		// Vulkan counterpart and were therefore left opaque.
		//
		// The tell: blend_translucent = 0 over a window in which the scene visibly has light
		// shafts or particles means the state is not reaching the instance at all, whereas
		// blend_translucent > 0 with the shafts still solid means it is reaching it and
		// calculateAlphaState() is rejecting the factor pair - it silently disables blending for
		// anything outside its table and for any colorBlendOp other than ADD
		// (rtx_instance_manager.cpp:802-807), which those two numbers cannot distinguish on
		// their own but which blend_unmapped bounds from below. One Resistance 2 capture of 665
		// dumped draws put the expected blend_translucent share at ~163/665, i.e. about a
		// quarter of submitted draws.
		u64 blend_chained = 0;
		u64 blend_translucent = 0;
		u64 blend_unmapped = 0;
	};

	// Wall-clock breakdown of the RSX thread's frame, in microseconds, accumulated over one
	// stats window. A frame-time collapse in which no CPU thread is busy is a *wait*, not
	// work, and only a per-call clock says which call is doing the waiting. 'window' is the
	// real elapsed time the frames covered, so 'window - flip' is everything the RSX thread
	// did outside flip() (FIFO decode, draw submission, guest stalls).
	struct frame_timing
	{
		u64 window_start = 0;
		u64 window = 0;
		u64 frames = 0;
		u64 flip = 0;
		u64 overlay = 0;  // composite_native_overlay: CPU rasterize of rpcs3's own UI
		u64 submit = 0;   // submit_compositor -> DrawScreenOverlay
		u64 present = 0;  // guarded_present
		u64 ui = 0;       // composite_ui_draw, summed over the frame
		u64 draw = 0;     // submit_subdraw, summed over the frame
	};

	// Diagnostic only: the widest-covering UI draw seen in the current stats window. The
	// white-slab symptom is "a large 2D draw resolved no albedo unit", so the unit, the pixel
	// count behind it and the attribute mask that fed it are exactly what names the cause.
	struct ui_biggest_draw
	{
		f32 area = 0.f;
		u64 vp_hash = 0;
		int unit = -1;
		usz pixels = 0;
		u32 verts = 0;
		u32 inputs = 0;
		u32 tint = 0;
		bool have_uv = false;
		// The draw's bbox *before* the space classification, i.e. straight out of the
		// program's own matrix chain. Printed so a run says outright which space R2's 2D
		// programs emit: [-1,1] on both axes is NDC, [0,1] with y growing downwards is a
		// top-left screen space that the NDC row conversion mirrors and halves.
		f32 clip_lo[2] = { 0.f, 0.f };
		f32 clip_hi[2] = { 0.f, 0.f };
		bool clip_unit = false;
	};

	// The orientation audit, broken out per vertex program. The aggregate counters cannot tell
	// "this branch's row conversion is inverted" from "one program with a high draw count is
	// inverted", and those two call for opposite fixes - the first is a mapping change, the second
	// is a classification change for one program. The first live run made that distinction the
	// whole question: ui_vflip_ndc came back 92 upright / 12300 upside down while ui_vflip_pixel
	// came back 54237 / 385, which reads as a wholesale NDC inversion, yet the ortho2d family that
	// dominates the NDC branch is confirmed upright on screen. Both cannot be true of the same
	// population, so the population has to be named before anything is changed.
	//
	// 'ortho' records whether that program's draws had their HPOS.xy-only transform rebuilt
	// (vp_fingerprint::has_ortho2d), which answers the specific question directly: an inverted
	// population with ortho=0 is not the visually-confirmed family and never was.
	struct ui_vote_row
	{
		u64 vp_hash = 0;
		u64 ndc_ok = 0;
		u64 ndc_bad = 0;
		u64 pixel_ok = 0;
		u64 pixel_bad = 0;
		bool ortho = false;
	};

	static constexpr u32 s_ui_vote_rows = 24;

	// One vertex attribute located inside its interleaved block, with the guest span it
	// spells out already validated. 'base' points at the block's first decoded vertex.
	struct attribute_view
	{
		const u8* base = nullptr;
		u32 stride = 0;
		u32 offset = 0;
		rsx::vertex_base_type type = rsx::vertex_base_type::f;
		u32 size = 0;

		const u8* at(u32 vertex) const { return base + (static_cast<usz>(vertex) * stride) + offset; }
	};

	// Why an attribute could not be mapped. Each reason maps onto its own skip counter so a
	// "nothing decoded" result says which gate refused.
	enum class attribute_status
	{
		ok,
		absent, // not fed from a persistent interleaved block at all
		layout, // block found but the offsets do not describe a readable stream
		memory  // the span is not readable guest memory
	};

	// The frame's best camera guess. Latched at flip and used for the whole next frame so
	// the camera and the per-draw worlds are always derived from the same reference.
	struct camera_candidate
	{
		bool valid = false;
		f32 score = 0.f;
		remix_rsx::vp_archetype archetype = remix_rsx::vp_archetype::unknown;

		// World -> view and view -> projection, both row-vector, projection viewport-z folded.
		remix_rsx::mat4 view{};
		remix_rsx::mat4 projection{};

		// Archetype B only: the fused matrix this was derived from, and its inverse, which
		// turns each draw's own fused matrix into a world transform.
		bool has_reference = false;
		remix_rsx::mat4 reference_inverse{};

		// Layered only: how many groups the winning program chained, so a draw with a
		// different layering is not transformed with the wrong split.
		u32 group_count = 0;

		// inverse(view * projection). Turns a fused per-draw matrix into a world transform,
		// which is what a program that folds its model matrix into the outer group needs.
		bool has_view_proj_inverse = false;
		remix_rsx::mat4 view_proj_inverse{};

		// Camera position in world space, used to park the debug light.
		f32 position[3] = { 0.f, 0.f, 0.f };
	};

	// Stage A: a hardcoded lit triangle that proves init / camera / present independently
	// of anything the RSX produces. Still the fallback whenever no camera resolves.
	bool create_debug_scene();
	void submit_debug_scene();

	// Camera derived from the title's own transform constants, or the stage A fallback.
	void submit_camera();
	void update_camera_candidate();

	// Recreates the debug sphere light at 'position' so a derived camera can be judged
	// visually at all. Extracting the title's own lights is out of scope for this milestone.
	void place_debug_light(const f32 (&position)[3]);

	// The scene's default readable light: one distant sun, created once and drawn every frame.
	// RSX has no fixed-function light state to read - PS3 titles light in fragment-program
	// constants with per-title semantics - so there is nothing engine-agnostic to extract.
	// False when the light could not be created; the caller then just has no sun.
	bool ensure_sun_light();

	// Remix instance categories for one draw, from the albedo hash lists. Replaces the
	// hardcoded categoryFlags = 0: the rtx.*Textures conf lists never reach an API draw.
	u32 classify_draw(u64 albedo_hash);

	// True when this draw is 2D / pre-projected and must not reach Remix.
	bool is_screen_space_draw() const;

	// Which fragment texture units may be used as albedo for this draw, and whether the answer
	// came from the fragment ucode. Non-null 'from_ucode' reports true only when the program named
	// a colour source *and* excluded at least one other referenced unit - i.e. it discriminated.
	// A program that reaches COL0 from every unit it samples has told the caller nothing, so it is
	// reported as a guess and the retry loop stays shut.
	u32 albedo_unit_mask(bool* from_ucode = nullptr) const;

	// Lowest enabled 2D fragment texture unit in 'mask' at or above 'skip_below', or -1.
	int albedo_texture_unit_in(u32 mask, u32 skip_below) const;

	// Lowest eligible, enabled, 2D fragment texture unit at or above 'skip_below', or -1, over
	// albedo_unit_mask()'s population.
	int albedo_texture_unit(u32 skip_below = 0) const;

	// Cached fragment-program fingerprint, keyed the same way m_vp_fingerprints keys the vertex
	// one: scanned once per program, never per draw.
	const remix_rsx::fp_fingerprint& fp_fingerprint_for(u64 fp_hash);

	// True when any referenced, enabled 2D fragment texture unit samples an address the RSX has
	// bound as a colour or depth surface: the title reading back its own framebuffer.
	bool samples_bound_surface() const;

	// "no attribute resolved", the return of resolve_texcoord_attribute on failure. Not 0: that is
	// a legal attribute index (the position), just never a texcoord one.
	static constexpr u32 no_attribute = 0xFFFFFFFFu;

	// Picks the vertex attribute carrying the texcoord set that feeds fragment texture 'unit', and
	// maps it. Shared by the 3D path (apply_texcoords) and the 2D one (composite_ui_draw) so both
	// see the same attribute: the compositor used to hardcode 8+unit with no fallback, which meant
	// a menu quad whose UVs sit elsewhere resolved a texture it could not coordinate and died in
	// the ui_no_colour refusal. Returns the chosen index or no_attribute, and reports through
	// 'best' the closest any scanned attribute got, for report_uv_failure's census.
	// Not const: the widened scan's finite-value gate is a refusal like any other and counts itself.
	// 'from_ucode', when given, reports whether the pick came from the program's own TEX<unit> write
	// or from the size/type heuristic that is now only the fallback.
	u32 resolve_texcoord_attribute(u32 unit, u32 first_vertex, u32 vertex_count,
		attribute_view& out, attribute_status& best, bool* from_ucode = nullptr);

	// One line per vertex program whose texcoord scan found nothing, once. Names the two masks
	// that decide what analyse_inputs_interleaved even placed, and the declared layout of every
	// attribute the program references - which is what says whether a title's UVs are somewhere
	// this code is not looking, or are not vertex attributes at all.
	void report_uv_failure(attribute_status best);

	// Which gate the sky test came out of, for the census below. 'depth_write' is not one of the
	// test's own outcomes - a depth-writing draw is never a candidate - but it is censused anyway,
	// because "is the dome one of the draws we refuse for writing depth?" is a question the
	// counters cannot answer and a guess at it would be a threshold change made blind.
	enum class sky_outcome
	{
		tagged,
		tagged_backdrop,
		tagged_hash,
		reject_extent,
		reject_anchor,
		reject_noworld,
		reject_depth_write,
		count
	};

	// Minimum world units of extent per vertex for the backdrop rule (sky_backdrop_mode). The live
	// census separates on this by a factor of 8: R2's backdrop c87769e09c995db9 runs 234..3974
	// units per vertex, the largest draw that is not it runs 27.9, and its 715- and 502-vertex
	// terrain draws run 2.3 and 1.2. A 15,895-unit surface made of four vertices is a card, not a
	// wall, and this is the number that says so.
	static constexpr f32 s_sky_backdrop_min_units_per_vertex = 100.f;

	// One line per (vertex program, outcome), and again only when that pair draws something at
	// least twice as wide as whatever was already reported for it - so a program that draws one
	// big thing and a thousand small ones is named by its big one. Bounded twice over: the 2x rule
	// makes re-emission monotone, and s_max_sky_census_lines stops the whole census, including the
	// bounding-box scan that feeds it, once the budget is spent.
	void report_sky_census(sky_outcome outcome, u32 vertex_count, bool depth_write,
		const f32 (&lo)[3], const f32 (&hi)[3], const remixapi_Transform& transform,
		f32 world_extent, f32 anchor, bool measured, bool camera_inside, f32 units_per_vertex,
		u64 albedo_hash);

	// --- albedo-hash sky rule (sky_hash_mode) -----------------------------------------------
	// What the rule knows about one albedo content hash. 'dome' and 'other' partition every
	// submitted draw that carried it since it was first seen dome-shaped, so dome/(dome+other) is
	// the hash's agreement with the geometry and 'other' is the disqualifier.
	struct sky_hash_entry
	{
		u32 dome = 0;
		u32 other = 0;

		// The first program seen drawing it as a dome, so the census can name a program as well as
		// a hash - a hash says which texture, a program says which mesh, and the two together are
		// what a reader can check against what is on screen.
		u64 vp = 0;

		// One census line when the hash arms and one when it is disqualified, never repeated.
		bool reported_armed = false;
		bool reported_mixed = false;
	};

	// One line per hash per transition, in the shape of 'Remix sky-census:'.
	void report_sky_hash_census(u64 albedo_hash, const sky_hash_entry& entry, bool armed,
		f32 world_extent, f32 units_per_vertex, u32 vertex_count);

	// How many all-dome draws a hash needs before it is armed. 8 rather than 1 because a single
	// dome-shaped draw is exactly what a large flat effect card looks like for one frame, and R2
	// draws its backdrop every frame - so a hash that is really the sky reaches 8 in under a
	// second at 60 fps while a one-off cannot reach it at all.
	static constexpr u32 s_sky_hash_min_draws = 8;

	// Hard cap on the table. Only hashes that have been dome-shaped at least once are ever
	// inserted, which on the dumped R2 captures is a few hundred textures out of 737 unique ones
	// in remix_tex\, so this is headroom rather than a limit that is expected to bind.
	static constexpr usz s_max_sky_hash_tracked = 4096;

	std::unordered_map<u64, sky_hash_entry> m_sky_hash_seen;

	u32 m_sky_hash_census_lines = 0;
	static constexpr u32 s_max_sky_hash_census_lines = 64;

	// --- viewmodel (viewmodel_mode) --------------------------------------------------------
	enum class viewmodel_outcome
	{
		tagged,
		reject_full_range,
		reject_offset,
		count
	};

	// Largest viewport depth-range span a draw may occupy and still be called a viewmodel. R2
	// runs the world at scale_z=1 and the arms/weapon at scale_z=0.2 across all 7041 dumped 3D
	// draws, with nothing at all in between; 0.5 is the round number in a 5x gap, not a tuned
	// one. A draw at or above it is ordinary world geometry.
	static constexpr f32 s_viewmodel_max_depth_scale = 0.5f;

	// How far from 0 the near end of that span may sit. A compressed slice is only a viewmodel if
	// it is pinned at the *front* of the range; a compressed slice anywhere else is some other
	// title-specific trick. Replaying the shipped rule over all 10153 dumped draws in the
	// scratchpad, this gate is the one that carries the 153 draws of the second title in those
	// captures - [0.50125, 1.0] and [0.00125, 0.0025], clip 1024x576 and 512x288 - which the span
	// test alone admits (their scale_z of 0.49875 and 0.00125 are both under s_viewmodel_max_
	// depth_scale). R2's own 2D UI at [0.5, 1.0] never reaches here: scale_z is exactly 0.5, so
	// the span test rejects it on the boundary. 1e-3 sits below the smallest non-zero offset_z on
	// record, 0.00125.
	static constexpr f32 s_viewmodel_max_depth_offset = 1e-3f;

	// Distance from the eye past which a tagged draw is counted as suspicious - reported, never
	// refused. The measured viewmodel anchor is 0.446 world units and the nearest same-frame
	// world draw is 8.930, so 4.0 (the sky rule's number, and for the same reason: it is the
	// round value inside a 20x gap) separates them with room to spare. It is not a gate because
	// the depth-range test was already exclusive over 8857 draws with no false positive, and an
	// anchor needs a resolved world transform and a live camera, either of which can be missing
	// on exactly the frames the arms still need drawing. Replaying the shipped rule over all
	// 10153 dumped draws returns 168, all of them those two programs, with no false positive to
	// gate away. Its job is to be the alarm that fires if that stops being true on some other
	// title, not a second gate.
	static constexpr f32 s_viewmodel_max_anchor = 4.f;

	// One line per (vertex program, outcome), once, under a hard ceiling. No re-emission rule
	// like the sky census': the population this names is two programs, so "once" is already
	// bounded, and a repeat would say nothing the counters do not.
	void report_viewmodel_census(viewmodel_outcome outcome, u32 vertex_count,
		f32 scale_z, f32 offset_z, const remixapi_Transform& transform, f32 anchor, bool measured);

	// The depth-range rule itself, reading the two viewport registers and returning which of the
	// three buckets above this draw falls in. Shared rather than inlined because three places now
	// have to agree on the same population - the tagging site in submit_subdraw, the candidate
	// router in update_camera_candidate, and the reference choice in per_draw_transform - and two
	// of those decide what a draw is *divided by*. A second copy that drifted would put the arms
	// and the arms' camera in different populations, which is the failure this fix removes.
	viewmodel_outcome classify_viewmodel_depth(f32& scale_z, f32& offset_z) const;

	// One line per (vertex program, outcome) for the reference decision, once, under its own
	// ceiling. Separate from report_viewmodel_census because that one names what the depth rule
	// selects and this one names what the selection was then transformed by; a run where those two
	// disagree is the thing worth reading.
	void report_viewmodel_camera_census(const char* outcome_name);

	std::unordered_set<u64> m_viewmodel_camera_census_seen;
	u32 m_viewmodel_camera_census_lines = 0;
	static constexpr u32 s_max_viewmodel_camera_census_lines = 16;

	// Fills m_scratch_vertices' texcoords from the vertex attribute that feeds the albedo unit.
	// Must run before the mesh content hash is taken: the texcoords are part of the vertex data
	// the hash covers, and two draws that share positions but not UVs are different meshes.
	void apply_texcoords(u32 unit, const remix_rsx::texture_entry& entry,
		const rsx::fragment_texture& tex, u32 first_vertex, u32 vertex_count);

	// Fills m_scratch_vertices' colours from ATTR3 for draws that resolved no material, so
	// vertex-coloured geometry stops reaching Remix as flat white. Like apply_texcoords it must
	// run before the mesh content hash, which covers the colour.
	void apply_vertex_colour(u32 first_vertex, u32 vertex_count);

	// Locates one vertex attribute in the interleaved blocks and validates the guest span it
	// would be read through. Unlike the old ATTR0-only code this searches *every* block: a
	// bone index or a texcoord routinely lives in a different block than the position.
	attribute_status map_attribute(u32 index, u32 first_vertex, u32 vertex_count, attribute_view& out) const;

	// The interleaved block that carries 'index', or null. Cheap: no memory validation.
	const rsx::interleaved_range_info* find_attribute_block(u32 index) const;

	// Size of the overlay buffer for this frame: the render surface the title's own 2D draws
	// are authored against. False when neither the surface nor the window has usable dims.
	bool compositor_target(u32& width, u32& height) const;

	// Hands the frame's overlay buffer to the fork's DrawScreenOverlay, once per flip.
	void submit_compositor();

	// Rasterizes one of the title's own screen-space draws into the overlay buffer. Called
	// after the positions have been decoded, in submission order.
	void composite_ui_draw(u32 first_vertex, u32 vertex_count);

	// Walks rpcs3's own overlay views into the same compositor at flip: message dialogs, the
	// home menu, the perf overlay. Mirrors GLPresent's dirty-drain + locked view walk.
	void composite_native_overlay();

	// Converts one overlay draw command's vertices into compositor primitives.
	void composite_overlay_command(const rsx::overlays::compiled_resource::command& cmd,
		f32 scale_x, f32 scale_y);

	// RGBA8 overlay images converted to the compositor's BGRA8 once, keyed by source pointer.
	const remix_rsx::texture_entry* overlay_image(const void* key, const u8* rgba, u32 width, u32 height);

	// Fills the bone scratch buffers for a skinned draw: per-vertex palette offsets, their
	// dense remap, and the matrices behind them. False means the draw must be skipped - never
	// drawn at identity, which is what parked skinned meshes at the world origin before.
	bool build_skinning(u32 first_vertex, u32 vertex_count);

	// The same, for a rig that blends several palette entries per vertex
	// (vp_fingerprint::skin_blended). Reached through build_skinning, never called directly, so
	// every caller keeps one entry point and one refusal contract.
	bool build_blend_skinning(u32 first_vertex, u32 vertex_count);

	// True when every bone this draw assembled is plausible next to the others in the same draw.
	// The only magnitude test on the skinning path; see the definition for why nothing else can be
	// one. Reads m_scratch_bone_axis / m_scratch_bone_offset, so it runs after the bones are built.
	bool bones_consistent();

	// Reproduces the blend Remix is about to perform and reports draws whose result travels far
	// further than the mesh's own spread can explain. Measures the artifact rather than any theory
	// about it; see the definition. Runs on both skinned paths, after the bones are final.
	void audit_skin_extent(u32 vertex_count);

	// Asks whether a draw's decoded positions form one object, before any transform touches them.
	// Runs on every draw, skinned or not; see the definition. Diagnostic only.
	void audit_vertex_extent(u32 first_vertex, u32 vertex_count, const attribute_view& positions, bool w_divide);

	// The only measurement of the geometry Remix is actually handed. Returns false when the streak
	// gate refuses the draw. See RemixTransforms.h, RPCS3_REMIX_STREAKGATE.
	bool audit_world_extent(const remixapi_Transform& transform, bool skinned, u32 vertex_count, bool exempt);

	// Decides what an indexed-constant draw actually is, from the draw's own vertex data rather
	// than from the ucode: evaluates the palette index for every vertex and, when they all agree,
	// reports the one matrix that index selects. That is the rigid/instanced case - a terrain
	// chunk or a batched prop, where the "palette" holds one object transform per object and a
	// given draw only ever reads one of them - and it needs no bone transforms and no Remix fork.
	// A draw whose indices differ is a real rig and goes to build_skinning instead.
	//
	// False means refuse: an index that does not evaluate, a slot outside the legal constants, or
	// a matrix that is not a finite affine transform. Never drawn at identity - the palette
	// unapplied is a mesh at the world origin, which is the vertex explosion by another route.
	bool resolve_indexed_world(u32 first_vertex, u32 vertex_count, bool& out_rigid);

	// RPCS3_REMIX_UIPROBE=1: a fixed known pattern through the same path, so the fork call can
	// be judged on its own before any real UI data is wired through it.
	void draw_ui_probe();

	// Per-draw object-to-world transform. False means "no transform available, use identity".
	// Not const: a program whose position decode is recognised but not rebuildable is refused here
	// rather than drawn raw, and that refusal counts itself (pos_decode_refused).
	bool per_draw_transform(remixapi_Transform& out);

	// Stage B: one subdraw of the current draw clause.
	void submit_subdraw();

	// Vertex-program identification, cached per ucode hash so the ucode walk runs once
	// per program rather than once per draw.
	const remix_rsx::vp_fingerprint& fingerprint_for(u64 vp_hash);

	// RPCS3_REMIX_DUMP=1: one line per unique vertex program, the permanent diagnostic.
	void dump_vertex_program(u32 first_vertex, u32 vertex_count, u32 index_count);

	// The '| skinval' half of that line: the decoded bone attribute, the offsets and dense
	// indices the first few vertices produce, the palette matrix behind dense bone 0 and the
	// world transform the draw would be submitted with. Read once per skinned program, so the
	// skinning diagnosis is read off a log line instead of guessed.
	std::string describe_skinning(u32 first_vertex, u32 vertex_count);

	// RPCS3_REMIX_DUMP=1: one line per unique texture content hash. This is the line a
	// modder reads to get the value they type into rtx.conf.
	void dump_texture(const remix_rsx::texture_entry& entry, const rsx::fragment_texture& tex, u32 unit);

	void reap_idle_meshes();
	void log_stats();

	remix_rsx::runtime m_remix;
	bool m_remix_ok = false;

	remixapi_MeshHandle m_debug_mesh = nullptr;
	remixapi_LightHandle m_debug_light = nullptr;

	// The default sun. Created once, not destroyed and recreated every frame the way the
	// camera sphere is: its parameters come from knobs that cannot change mid-run.
	remixapi_LightHandle m_sun_light = nullptr;
	bool m_sun_failed = false;

	rsx::vertex_input_layout m_vertex_layout{};

	std::unordered_map<u64, mesh_entry> m_meshes;
	std::unordered_set<u64> m_poisoned;

	// Guest addresses the RSX has bound as a colour or depth surface. A draw sampling one of
	// these is the title reading back its own framebuffer, which the Remix path already
	// produces; see composite_ui_draw (2D) and submit_subdraw (3D).
	std::unordered_set<u32> m_surface_addresses;

	// The set stops growing at s_max_tracked_surfaces. A title that binds more distinct
	// surfaces than that would leave the later ones untracked, and both feedback gates would
	// silently start missing. Logged once so a miss is diagnosable instead of invisible.
	bool m_surface_cap_logged = false;

	// Vertex programs already reported by the render-target feedback gate, so its census is one
	// line per program instead of one per draw.
	std::unordered_set<u64> m_rt_feedback_seen;

	// (vertex program, albedo unit, texcoord attribute) triples already reported by the UV
	// census, so a dump run gets one line per combination instead of one per draw.
	std::unordered_set<u64> m_uv_census_seen;

	// Vertex programs already reported by the UV *failure* census. Separate from the success
	// census above because the failing draws never reach it - they return before it runs.
	std::unordered_set<u64> m_uv_fail_census_seen;

	// Vertex programs already reported by the indexed-constant refusal census.
	std::unordered_set<u64> m_indexed_census_seen;

	// Vertex programs already reported by the screen-space refusal census.
	std::unordered_set<u64> m_screen_census_seen;

	// Sky-dome census state. Per vertex program, the largest world extent already reported under
	// each sky_outcome, or -1 for "not reported yet".
	struct sky_census_entry
	{
		// One per sky_outcome - keep the initialiser the same length as the enum.
		f32 printed_extent[static_cast<usz>(sky_outcome::count)] = { -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f };
	};

	std::unordered_map<u64, sky_census_entry> m_sky_census_seen;

	// Hard ceiling on census lines. Once spent, every sky-census branch short-circuits on this one
	// comparison and the diagnostic costs nothing - which matters because the census arm that
	// covers depth-writing draws has to scan a bounding box for draws the sky test itself skips.
	u32 m_sky_census_lines = 0;
	static constexpr u32 s_max_sky_census_lines = 256;

	// Viewmodel census state: (vertex program ^ outcome) already named. 64 lines is generous for
	// a population the dumps put at two programs - if it ever fills, the rule is selecting far
	// more than a viewmodel and viewmodel_tagged / viewmodel_considered will say so first.
	std::unordered_set<u64> m_viewmodel_census_seen;
	u32 m_viewmodel_census_lines = 0;
	static constexpr u32 s_max_viewmodel_census_lines = 64;

	// CreateMesh calls attributed to the vertex program that issued them, reset every stats
	// window. A title that skins on the SPU rewrites its vertex bytes every frame, so the
	// content-hashed mesh cache mints a fresh handle per frame per animated draw - and this map
	// is what says whether the churn is concentrated in a few animated programs (fixable by
	// bounding them) or spread across the whole scene (not).
	std::unordered_map<u64, u64> m_mesh_creates_by_vp;

	remix_rsx::texture_cache m_textures;
	remix_rsx::compositor m_compositor;

	// True once begin_frame() has run for the frame currently being built.
	bool m_compositor_open = false;

	// Diagnostic accumulator, reset every time it is logged.
	ui_biggest_draw m_ui_biggest{};

	// Per-program orientation votes. Cumulative like m_stats, not per-window like m_ui_biggest,
	// so the last line printed is the whole run. Programs past the table fold into m_ui_vote_spill
	// rather than evicting a row - a stable table beats a complete one here, because the question
	// is which of a handful of 2D programs is inverted.
	ui_vote_row m_ui_votes[s_ui_vote_rows]{};
	u64 m_ui_vote_spill = 0;

	// How many RPCS3_REMIX_UIDUMP lines have been emitted so far.
	u32 m_ui_dumped = 0;

	// Stats window (m_frame_counter / s_stats_interval_flips) the RPCS3_REMIX_WORLDVP probe last
	// printed in. The probe fires per draw, so without this it would write one line per draw call;
	// umax is the "never printed" sentinel because window 0 is a real window.
	u64 m_worldvp_window = umax;

	std::unordered_map<u64, remix_rsx::vp_fingerprint> m_vp_fingerprints;
	std::unordered_map<u64, remix_rsx::fp_fingerprint> m_fp_fingerprints;
	// Vertex programs that have produced a sky-tagged draw on their own merits. A tessellated
	// dome's narrower latitude bands are then admitted by sky_learn_dome_enabled() even though
	// their extent alone would refuse them. Armed only, never disarmed: a program that draws a
	// dome does not later draw a wall, and the per-draw guards (no depth write, no material,
	// refused on extent alone) carry the real weight.
	std::unordered_set<u64> m_sky_dome_programs;

	std::unordered_set<u64> m_vp_dumped;
	std::unordered_set<u64> m_fp_dumped;
	std::unordered_set<u64> m_dumped_textures;

	// Identification of the draw clause currently being submitted. Set once in end(),
	// because the vertex program cannot change between subdraws of one clause.
	u64 m_current_vp_hash = 0;
	const remix_rsx::vp_fingerprint* m_current_fingerprint = nullptr;

	// The same for the fragment program. Null when the clause had no valid fragment ucode, which
	// leaves every albedo choice on the lowest-unit fallback.
	u64 m_current_fp_hash = 0;
	const remix_rsx::fp_fingerprint* m_current_fp_fingerprint = nullptr;

	camera_candidate m_frame_candidate{};
	// --- click-to-identify ---------------------------------------------------------------
	//
	// Remix's own dev-menu picker ("hover over an object on screen") cannot resolve a draw
	// this backend submits, and no amount of binding fixes it. The runtime maps a picked
	// pixel to a texture through DrawCallMetaInfo::legacyTextureHash, which SceneManager
	// fills from drawCallState.getMaterialData().getColorTexture().getImageHash() - and
	// toRtDrawState() assigns 'materialData.colorTextures[0] = TextureRef {}' for every
	// external draw (rtx_remix_api.cpp:920). The lookup therefore reads an empty hash and
	// the hover readout stays blank by construction, which is exactly the "can't click on
	// any textures" symptom. The texture grid still populates because that is fed by
	// CreateTexture, on a different path.
	//
	// The picking *machinery* is unaffected: the runtime writes each instance's
	// remixapi_InstanceInfoObjectPickingEXT::objectPickingValue into a G-buffer and reads
	// it back for a screen rect. Only the value->texture mapping is missing, and that is
	// ours to supply. So every instance is numbered, the value under the cursor is asked
	// for, and the identity is looked up on this side - which also lets the answer carry
	// the vertex-program hash and the sky/viewmodel verdict, neither of which Remix could
	// have reported even on the D3D9 path.
	struct pick_record
	{
		u64 vp_hash = 0;
		u64 albedo_hash = 0;
		u32 vertex_count = 0;
		f32 extent = 0.f;
		bool sky = false;
		bool viewmodel = false;

		// The decode verdict for the program that drew it. Carried here rather than looked up
		// afterwards because these are the fields that separate "this mesh is genuinely large"
		// from "this mesh arrived undecoded", and a click on a streak is asking exactly that.
		// affine_reason points into static strings owned by the fingerprint cache.
		u8 archetype = 0;
		bool has_prescale = false;
		bool has_const_affine = false;
		bool skinned = false;
		const char* affine_reason = "";
	};

	// A frame that submits more draws than this stops numbering them rather than growing
	// without bound. Haze peaks around 1500 instances a frame, so this is slack, not a cut.
	static constexpr usz s_max_pick_records = 16384;

	// index + 1 == objectPickingValue, so 0 stays "nothing was drawn here".
	std::vector<pick_record> m_pick_table;

	// The table as it stood on the frame the request was made. Readback lands one or more
	// frames later, by which time m_pick_table has been rebuilt, so it cannot be consulted.
	std::vector<pick_record> m_pick_snapshot;
	std::mutex m_pick_mutex;

	// Frames left to keep re-requesting. The runtime only allocates the object-picking
	// target once a request has been seen (rtx_context.cpp:2052), so the first request
	// after a click reliably reads back nothing; retrying for a few frames is what makes a
	// single click work.
	u32 m_pick_frames_left = 0;
	s32 m_pick_x = 0;
	s32 m_pick_y = 0;
	bool m_pick_button_down = false;
	// One line per program that resolves no world transform, bounded. The population is half
	// the scene and has never been named.
	static constexpr u32 s_max_world_refused_lines = 128;
	std::unordered_set<u64> m_world_refused_seen;
	u32 m_world_refused_lines = 0;

	// Which exit per_draw_transform took when it refused. Fourteen returns share one counter,
	// and the last round proved how expensive guessing between them is: the camera-less-frame
	// theory predicted ~92% of world_refused and measured 0.11%. Set at each return, printed by
	// the world-refused census, so the population is named rather than modelled.
	static constexpr const char* s_world_fail_names[] = {
		"nocam", "idxworld", "sl_group", "sl_bone", "lay_group", "lay_ref", "fused_vpi",
		"lay_other", "ref_group", "ref_bone", "ref_vm", "ref_none", "no_reference", "tail" };

	// How far past the affinity tolerance the refused draws actually sit. Buckets are
	// <0.05, <0.2, <1, <10, >=10 against a tolerance of 0.02.
	f32 m_affine_residue_max = 0.f;
	u64 m_affine_residue_buckets[5] = {};

	const char* m_world_fail = "";

	// Per *draw*, not per program. The first cut of this printed one census line the first time
	// a program was refused, which biased the whole distribution: early frames have no camera,
	// so 17 programs were tagged "nocam" on their first refusal and carried that label forever
	// even though world_refused_nocam is 0.11% of the population. Counting every refusal is the
	// only form of this that answers the question.
	u64 m_world_fail_counts[std::size(s_world_fail_names)] = {};

	void note_world_fail(u32 reason)
	{
		m_world_fail = s_world_fail_names[reason];
		++m_world_fail_counts[reason];
	}


	bool m_sky_camera_warned = false;
	bool m_pick_slot_warned = false;
	bool m_pick_request_warned = false;

	// Ctrl+Click in the game window, polled once per flip. Deliberately not routed through
	// rpcs3's input system: that would mean touching a pad handler, and the mouse is not
	// bound to anything here anyway.
	void poll_pick_request();
	static void pick_callback(const u32* values, u32 count, void* user);

	camera_candidate m_active_camera{};

	// The same pair for the viewmodel population, latched independently at the same flip. Only
	// has_reference and reference_inverse are ever read off these: the anchor the census reports is
	// deliberately still measured against m_active_camera.position, because the claim being tested
	// is that the arms sit near the *world* eye.
	camera_candidate m_frame_viewmodel_candidate{};
	camera_candidate m_active_viewmodel{};
	u32 m_viewmodel_camera_age = 0;

	// Consecutive flips m_active_camera has survived without a candidate of its own. Zeroed every
	// time a frame resolves one; once it passes remix_rsx::camera_hold_frames() the camera is
	// dropped and submit_camera() goes back to the origin fallback.
	u32 m_camera_age = 0;

	u32 m_split_attempts = 0;

	// Reused across subdraws to keep the hot path allocation free.
	std::vector<remixapi_HardcodedVertex> m_scratch_vertices;
	std::vector<u32> m_scratch_indices;
	std::vector<u32> m_scratch_indices_alt;
	std::vector<u8> m_scratch_index_bytes;

	// Skinning scratch. 'slots' is the distinct set of palette offsets this draw touched;
	// 'indices' is the per-vertex index into it, which is what Remix's blendIndices means.
	//
	// 'indices', 'weights' and 'raw' are laid out as vertex-major tuples of
	// m_scratch_bones_per_vertex entries, which is the layout remixapi_MeshInfoSkinning documents
	// ("each tuple of 'bonesPerVertex' ... defines a vertex") and the stride the fork's unpacker
	// builds its buffers with (rtx_remix_api.cpp:1116-1117). A single-bone rig is the same layout
	// with a tuple of one, so the submit path does not branch on it.
	std::vector<u32> m_scratch_bone_indices;
	std::vector<f32> m_scratch_bone_weights;
	std::vector<u32> m_scratch_bone_slots;
	std::vector<f32> m_scratch_bone_raw;
	std::vector<remixapi_Transform> m_scratch_bone_transforms;

	// Per accepted bone, the longest basis axis and the translation length, parallel to
	// m_scratch_bone_transforms. Kept because the transform itself is already converted to Remix's
	// column-vector form by then, and because the per-draw median needs all of them before any can
	// be judged. 'sorted' is the scratch the median selection sorts, so the two source vectors stay
	// in bone order for the diagnostic.
	std::vector<f32> m_scratch_bone_axis;
	std::vector<f32> m_scratch_bone_offset;
	std::vector<f32> m_scratch_bone_sorted;
	u32 m_scratch_bones_per_vertex = 1;

	// The position decode is already composed into m_scratch_bone_transforms, so
	// per_draw_transform must not compose it into the instance transform as well. Set only by
	// build_blend_skinning, cleared per subdraw next to m_indexed_world_valid - the two answer the
	// same question ("what did this subdraw already fold in") and must never survive into another.
	bool m_scratch_bone_prescale_folded = false;

	// The one object matrix a rigid indexed draw resolved to, valid only for the subdraw that set
	// it. per_draw_transform composes it between the position decode and the outer group, because
	// that is exactly where the ucode reads it: clip = ((attr * decode) * palette[a]) * outer.
	remix_rsx::mat4 m_indexed_world{};
	bool m_indexed_world_valid = false;

	// One census line per indexed program, the same shape the render-target and indexed-const
	// gates already use: what the recogniser made of it and what its indices actually were.
	std::unordered_set<u64> m_indexed_world_seen;

	// Vertex programs already reported by audit_skin_extent. The flag describes a shape, and a
	// shape repeats every frame the rig is on screen; skin_reach_flagged carries the per-draw count.
	std::unordered_set<u64> m_skin_reach_seen;

	// Vertex programs already reported by audit_vertex_extent, same once-per-shape rule.
	std::unordered_set<u64> m_vertex_spread_seen;

	// Reused by audit_vertex_extent for its per-axis and per-distance median selection. A member so
	// a pass that runs on every draw does not allocate on every draw.
	std::vector<f32> m_scratch_vertex_spread;
	std::vector<u32> m_scratch_outlier_indices;

	// audit_vertex_extent flagged this subdraw's geometry as incoherent. Read at the submission
	// point, because the audit runs before every gate that could still drop the draw.
	bool m_vertex_flagged = false;

	// --- post-transform geometry census, per subdraw ------------------------------------
	// The world extent audit_world_extent measured for this subdraw, and whether it was over the
	// ratio. Both have the same per-subdraw lifetime as m_vertex_flagged and are read once more at
	// the point DrawInstance succeeded - which is what keeps 'examined' and 'drawn' separate
	// counters rather than one number that has to be interpreted.
	f32 m_streak_extent = 0.f;
	bool m_streak_flagged = false;
	bool m_streak_measured = false;

	// World extents of this frame's non-exempt drawn geometry, and the median of the frame before
	// it. The gate compares a draw against its own scene rather than against a constant, so it needs
	// to know nothing about the title's units; the previous frame's median is used because the
	// current frame's is not knowable while the frame is still being submitted. Reset per flip.
	std::vector<f32> m_scratch_world_extents;
	f32 m_world_extent_median = 0.f;
	u64 m_world_extent_samples = 0;

	// One census line per program, the same once-per-shape rule the indexed-world census uses, with
	// a hard line cap on top: a streak population that turns out to be a hundred programs must not
	// spend the whole log before the frame it appeared in is readable.
	std::unordered_set<u64> m_world_extent_seen;
	u32 m_world_extent_census_lines = 0;
	static constexpr u32 s_max_world_extent_census_lines = 64;

	// One basis-affine line per program. Unbounded by a line cap because it is keyed on the
	// program hash and R2 has a few dozen programs, not thousands.
	std::unordered_set<u64> m_basis_census_seen;

	// One line per unrecognised rig, once per program, bounded - the same shape. The dump cannot
	// answer this: it emitted 72 lines in the 4:50 capture and not one of them carried
	// unrecognised=1, so the programs being dropped were never sampled by it at all.
	std::unordered_set<u64> m_skin_unrec_seen;
	u32 m_skin_unrec_census_lines = 0;
	static constexpr u32 s_max_skin_unrec_census_lines = 32;

	// The gate stays disarmed until the previous frame has this many non-exempt samples. A median
	// over a handful of draws is not a scene scale, and arming on one would refuse whatever happened
	// to be biggest in a frame that drew almost nothing - a loading screen, or the first frame after
	// a camera change.
	static constexpr u64 s_streak_median_min_samples = 16;

	// The ratio audit_world_extent reports at when the gate is switched off, i.e. the same threshold
	// RPCS3_REMIX_STREAKGATE defaults to. Present so that the off run and the on run flag the same
	// draws and their censuses can be diffed line for line; if it drifted from the env default the
	// A/B would be comparing two different questions.
	static constexpr f32 s_streak_report_ratio = 128.f;

	// Screen-space positions in compositor pixels, one entry per decoded vertex.
	std::vector<f32> m_scratch_ui_x;
	std::vector<f32> m_scratch_ui_y;

	// BGRA8 copies of rpcs3's own overlay images, keyed by the source data pointer. Cleared
	// when the overlay manager reports the owning view dirty.
	std::unordered_map<const void*, remix_rsx::texture_entry> m_overlay_images;

	// rpcs3's own overlay icon set, loaded on the first native-overlay frame.
	rsx::overlays::resource_config m_ui_resources;
	bool m_ui_resources_loaded = false;

	stat_counters m_stats{};
	frame_timing m_timing{};
	u64 m_frame_counter = 0;

	// Flip heartbeat. When the picture stops moving, "blocked inside Present", "grinding
	// through draws without ever reaching flip" and "the guest stopped asking to flip" are
	// indistinguishable from outside the process; end() checks these to tell them apart.
	u64 m_last_flip_us = 0;
	u64 m_end_calls = 0;

	// How many guest-thread forensics dumps the stall path has already emitted.
	u32 m_stall_dumps = 0;
#endif
};
