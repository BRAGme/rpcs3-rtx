#pragma once
#include "Emu/RSX/GSRender.h"

#ifdef _WIN32
#include "Emu/RSX/Overlays/overlay_controls.h"
#include "Emu/RSX/Remix/RemixCompositor.h"
#include "Emu/RSX/Remix/RemixRuntime.h"
#include "Emu/RSX/Remix/RemixTextures.h"
#include "Emu/RSX/Remix/RemixTransforms.h"
#include "Emu/RSX/Remix/RemixVertexDecode.h"

#include <atomic>
#include <memory>
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

		// Round 9. Which material this mesh was CREATED with. The Remix mesh object bakes its
		// surface's material at CreateMesh and there is no API to re-point it afterwards, while
		// the mesh key is content-derived and therefore stable across texture lifetime events -
		// so this handle is what the mesh will keep submitting for as long as its geometry is
		// unchanged, whatever happens to the texture cache entry that produced it.
		//
		// Recorded for one reason: texture_cache::reap can then be told which materials are still
		// referenced and refuse to destroy them. Without this field reap has no way to ask, which
		// is the lifetime hole behind permanently-white surfaces (and, on handle reuse, wrongly
		// textured ones). Not part of the key and never compared - purely a back-reference.
		remixapi_MaterialHandle material = nullptr;
	};

	struct static_triangle
	{
		u32 a = 0;
		u32 b = 0;
		u32 c = 0;

		bool operator==(const static_triangle&) const = default;
	};

	struct static_triangle_hash
	{
		usz operator()(const static_triangle& triangle) const noexcept
		{
			usz value = triangle.a;
			value ^= static_cast<usz>(triangle.b) + 0x9e3779b9u + (value << 6) + (value >> 2);
			value ^= static_cast<usz>(triangle.c) + 0x9e3779b9u + (value << 6) + (value >> 2);
			return value;
		}
	};

	struct static_index_entry
	{
		std::vector<remixapi_HardcodedVertex> vertices;
		std::vector<u32> indices;
		std::unordered_map<u32, u32> vertex_lookup;
		std::unordered_set<static_triangle, static_triangle_hash> triangles;
		std::unordered_set<usz> submitted_signatures;
		// ROUND 44, diagnostic. The mesh hash the LAST submission under this entry actually
		// carried. submitted_signatures keys on the instance transform, the category flags,
		// doubleSided and the blend state and on NOTHING about which geometry the draw covers -
		// so on a title that bakes world geometry in world space, where every tile shares the
		// identity transform, all of a frame's tiles collide on one signature and only the first
		// submits. That is correct if and only if the union mesh the first one carried already
		// held every tile. This field is what lets the suppression be split into "same mesh,
		// genuinely redundant" and "DIFFERENT mesh, geometry silently discarded".
		u64 last_submitted_mesh_hash = 0;
		u64 mesh_hash = 0;
		u64 submit_frame = umax;
		bool dirty = false;
		bool overflow = false;
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
		// Vertices whose bone index resolved further from palette_base than RPCS3_REMIX_SKINSPAN
		// allows. Stays 0 at the default span - see skin_index_span.
		u64 skin_index_out_of_range = 0;

		u64 skip_screen_space = 0;
		// Draws that found their mesh already in the cache. Read against meshes_created: the ratio
		// is how stable the geometry key is, and per-flip creations are the number that says whether
		// Remix is seeing one object move or a new object every frame.
		u64 meshes_reused = 0;

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
		// Camera selection is a vote over distinct view/projection pairs observed during a frame.
		// These counters expose whether a title actually has competing cameras and whether the
		// bounded census is large enough for it: clusters is the total distinct population,
		// contested counts frames with more than one, and winner_votes is the winning support.
		u64 cam_clusters = 0;
		u64 cam_contested = 0;
		u64 cam_winner_votes = 0;
		u64 cam_cluster_overflow = 0;
		// Extreme camera changes held for confirmation, and genuine switches that survived the
		// confirmation window. Separating them makes a rejected auxiliary camera measurable.
		u64 cam_discontinuity_held = 0;
		u64 cam_switch_confirmed = 0;
		// The fused-split path in update_camera_candidate: how often it was entered, and how often
		// split_view_projection could not factor the matrix. The pair says whether a frame with no
		// candidate had no 3D draws at all (attempted == 0) or had them and could not split them.
		u64 split_attempted = 0;
		u64 split_failed = 0;
		u64 split_reserved = 0;
		u64 world_applied = 0;
		u64 world_fallback = 0;

		// The reference-divided population split by whether the reference was latched in this
		// same frame. They partition every draw that reaches the reference branch of
		// per_draw_transform, so fresh + stale is that branch's whole population and the ratio
		// says how much of the scene the mid-frame relatch actually reached. Stale dominant means
		// Haze's camera program draws late in its frame and the fix has to become deferred
		// submission - which is itself the answer the round is looking for, stated as a number
		// rather than argued.
		u64 world_ref_fresh = 0;
		u64 world_ref_stale = 0;
		// How many times the active camera's matrices were refreshed mid-frame instead of at
		// flip. Expected to climb roughly once per flip: the first same-source, continuous
		// candidate of a frame refreshes, the rest find latch_frame already current. Spiking far
		// above one per flip means several distinct matrices are squeaking under the continuity
		// tolerance and RPCS3_REMIX_CAMRELATCH=0 is the bisect.
		u64 cam_relatch_midframe = 0;

		// The gauge-anchor split. These three partition every draw that reaches the reference
		// branch of per_draw_transform while RPCS3_REMIX_GAUGEANCHOR is on: divided by an anchor
		// captured in this same frame, by the previous frame's anchor, or by the elected camera
		// because no anchor exists for that draw's surface key at all. used >> absent is the
		// success reading; prev dominant means the title's identity-listed programs draw late in
		// the frame and deferred submission is the next round's architecture answer.
		u64 gauge_anchor_used = 0;
		u64 gauge_anchor_prev = 0;
		u64 gauge_anchor_absent = 0;
		// A later identity-listed draw on an already-anchored surface key whose own fused matrix
		// disagrees with the stored anchor by more than the camera discontinuity tolerance. The
		// tripwire for "a listed VP also draws a non-main pass on this surface": non-zero means
		// read the gauge trace before trusting the anchor. The anchor is never replaced.
		u64 gauge_anchor_contested = 0;
		// Frames whose submitted camera was rebuilt from the anchor's own split, and frames where
		// that split (or its perspective score) failed and the elected camera was kept instead.
		// The second is expected to be small outside Haze's near-vertical views, where
		// split_view_projection is known to stop factoring.
		u64 gauge_cam_from_anchor = 0;
		u64 gauge_cam_split_failed = 0;
		// The two silent exits in capture_gauge_anchor, which until round 3 left no trace at all:
		// every slot already held a source whose anchor was too young to reuse, and the fused
		// matrix would not invert. Frames 70200/70320 of the round-2 run prove whole frames went
		// anchor-less while identity draws were present, and these are the only paths that can do
		// that. slot_exhausted non-zero at GAUGESLOTS=4 and ~0 at 16 is the whole of step 1's
		// evidence; invert_failed non-zero says a listed program's fused matrix is singular and the
		// list, not the slot count, is the problem.
		u64 gauge_slot_exhausted = 0;
		u64 gauge_invert_failed = 0;
		// The gauge_anchor_prev population split by how the previous frame's anchor was found: on
		// the exact (surface offset, target, clip) key, or on the (target, clip) shape alone after
		// the exact key missed. These two sum to gauge_anchor_prev exactly, so runs taken before
		// GAUGEPREVDIMS existed stay comparable. dims large is the measurement that says surface
		// offsets really do move between frames on this title.
		u64 gauge_prev_exact = 0;
		u64 gauge_prev_dims = 0;
		// --- ROUND 38: the three numbers that decide whether the stale-divisor fix can fire -------
		// gauge_cur_dims: draws rescued by RPCS3_REMIX_GAUGECURDIMS - the exact-key CURRENT-frame
		//   lookup missed, but a this-frame anchor of the same (target, clip) shape existed and was
		//   used instead of last frame's. These draws never reach gauge_anchor_prev, so
		//   gauge_used + gauge_cur_dims + gauge_prev + gauge_absent is the whole population.
		// gauge_cur_avail: how often that this-frame same-shape anchor EXISTED at that moment,
		//   counted whether or not the knob is armed. This is the availability measurement, and it
		//   is the one to read first: gauge_cur_avail == 0 means the route is structurally dead on
		//   this title (every such draw is issued before ANY donor of its shape) and no value of
		//   GAUGECURDIMS can change anything. gauge_cur_avail >= gauge_cur_dims always.
		// gauge_prev_camfresh: prev-branch draws whose ELECTED camera was latched this frame. The
		//   size of the untried alternative - divide by the fresh elected camera rather than by a
		//   stale anchor. Measurement only; nothing consumes it yet.
		u64 gauge_cur_dims = 0;
		u64 gauge_cur_avail = 0;
		u64 gauge_prev_camfresh = 0;
		// Flips whose submitted camera came from the *previous* frame's anchor, and flips that had
		// neither and kept the last anchor-derived split rather than falling back to the elected
		// gauge. gauge_cam + gauge_cam_prev + gauge_cam_held approaching one per flip is the
		// success reading for step 3; held spiking at scene cuts is the risk note's tell.
		u64 gauge_cam_prev = 0;
		u64 gauge_cam_held = 0;
		// ROUND 29, MEASUREMENT ONLY. Flips on which apply_gauge_anchor_camera's anchor-derived
		// camera position disagreed with the frame's own elected candidate by more than one unit on
		// any axis - i.e. flips where m_active_camera.position stopped being the guest's world-space
		// eye and became the eye in the anchor donor's frame. Not an error metric: both values are
		// individually correct, in different frames, and no consumer of .position knows which it has.
		// Non-zero with anchor_cam_offmax in the hundreds is the land-carrier reading (RemixTransforms.h
		// at anchor_gauge_census_lines has the whole measurement).
		u64 anchor_cam_offset = 0;
		// ROUND 30, the fix round 29 specified. camframe_served counts consumer reads answered from
		// the anchor-frame eye instead of m_active_camera.position; camframe_corrected counts the
		// subset where the two differed by more than a unit on some axis - i.e. reads that WOULD have
		// measured a distance between two points in different frames of reference. camframe_absent
		// counts reads that asked and were refused (no anchor eye this frame, or the knob is off), and
		// those fall back to exactly the round-29 value. served + absent is every read that asked.
		// Read camframe_max (live line, an f32 kept outside this struct) against anchor_cam_offmax:
		// they measure the same disagreement at different sites and should be the same order.
		u64 camframe_served = 0;
		u64 camframe_corrected = 0;
		u64 camframe_absent = 0;
		// Draws whose live viewport z scale/offset differ from the ones the anchor's donor draw
		// folded into its fused matrix. Measurement only - nothing is refolded this round. Non-zero
		// on a wobbling pick means the anchor and the draw disagree about the depth range and
		// refolding with the anchor's own registers is the next one-liner.
		u64 gauge_zfold_mismatch = 0;
		// Draws refused by RPCS3_REMIX_SKIPAUXUNTEX: no material, no albedo unit, and a render
		// source strictly smaller than the main pass. The greyscale-wash population.
		u64 skip_aux_untextured = 0;
		// Draws divided in double precision instead of single. Partitions nothing on its own - read
		// it against gauge_used + gauge_prev, which is the population it should equal at
		// GAUGEF64=1. Zero with gauge_used large means the knob is off or every anchor's f64 inverse
		// failed, and the two are told apart by 'Remix gauge-selfcheck:' existing at all.
		u64 world_div_f64 = 0;
		// --- per-pixel discard detection (the foliage cutout channels) --------------------------
		// Draws whose fragment ucode carries a KIL, and draws whose guest-set shader_control claims
		// one (RSX_SHADER_CONTROL_USES_KIL). The ucode is ground truth: the control word is set by
		// the guest and this backend never calls get_current_fragment_program(), so
		// current_fragment_program.ctrl is not filled on its path at all. kil_disagree counts the
		// two channels differing, which is the tell for a scanner bug in either direction.
		u64 kil_ucode = 0;
		u64 kil_ctrl = 0;
		u64 kil_disagree = 0;
		// Draws whose *bound albedo unit* has NV4097_SET_TEXTURE_CONTROL0 bit 2 set - the second,
		// entirely independent cutout channel, consumed by core at RSXThread.cpp:2267-2271 and never
		// once by this backend.
		u64 texkill_seen = 0;
		// Draws that actually had an alpha test written into their blend extension because of the
		// above, and the subset of those that were driven by the texture-control bit rather than by
		// the ucode. applied climbing with foliage still solid means the runtime is resolving the
		// test some other way; applied at 0 with kil_ucode large means the replay's own
		// preconditions (bound material, RSX alpha test disabled, BLENDSTATE on) are not met.
		u64 kil_alpha_applied = 0;
		u64 kil_alpha_texkill = 0;
		// Ctrl+clicks the RSX thread actually saw. Read against the pick_resolved atomic on the
		// live line: 0/0 means the user never clicked, n/0 means every click was seen and none
		// resolved. Round 1 could not tell those two apart at all.
		u64 pick_clicks = 0;
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
		// With an explicit camera-program lock, draws aimed at a different render surface are
		// auxiliary views (reflection/passenger/shadow cameras), not gameplay-world geometry.
		u64 skip_camera_surface = 0;
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
		// Round 8. Strict subset of wdiv_draws: the draws whose program only matched match_wdivide
		// because RPCS3_REMIX_WDIVWALK stepped past a w-only write to reach the divide MUL. >0 is
		// the proof the giant-geometry fix is live on real draws; WDIVWALK=0 drives it to 0 and
		// takes the same draws out of wdiv_draws with it.
		u64 wdiv_shadowed_draws = 0;
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
		// The five refusal exits in resolve_texcoord_scale_slot, counted per fixed-divisor draw.
		u64 uv_scale_no_multiply = 0;
		u64 uv_scale_two_constants = 0;
		u64 uv_scale_third_operand = 0;
		u64 uv_scale_two_attributes = 0;
		u64 uv_scale_no_attribute = 0;
		// Draws whose UVs were replayed from the program's own resolved affine form
		// (vp_fingerprint::texcoord_affine) instead of the scalar divisor path. Reads against
		// uv_scale_fixed from the run before it: what this gains, that population loses.
		// RPCS3_REMIX_UVAFFINEALL=0 drives it to 0 and restores the per-hash replay only.
		u64 uv_affine_general = 0;
		// Of those, the draws whose resolved form names a *different* divisor component for its two
		// lanes (or its two 2x2 rows) - the population RPCS3_REMIX_UVSCALELANES exists for. 0 means
		// no program in this scene divides u and v differently and the knob changed nothing; the
		// forms it newly resolves are the ones the uvscale-fixed census used to name with
		// "affine:mul-two-components" or "affine:row-mismatch".
		u64 uv_affine_lanes = 0;
		// Draws whose *submitted* UVs left the unit square by more than a texel's slack, i.e. the
		// material repeats at least once. Not a fault on its own - content tiles on purpose - but
		// it is the size of the population the uvrange census names, and the number that has to
		// move if a UV fix is ever the right one. RPCS3_REMIX_UVRANGECENSUS=0 drives it to 0.
		u64 uv_tiled = 0;
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
		// ROUND 43. Sign of the submitted instance transform's 3x3 determinant, over every
		// instance that reached DrawInstance. xform_mirrored counts REFLECTIONS - the geometry is
		// handed to the tracer inside-out, so its texture reads mirrored and its shading comes
		// from the wrong side. xform_degenerate counts det == 0 or NaN, a collapsed frame, which
		// is a different fault. READING THEM: xform_mirrored == 0 kills the reflection hypothesis
		// for the black voids outright and the next round looks elsewhere; anything else, and the
		// det= field on 'Remix picked:' names the surfaces.
		u64 xform_measured = 0;
		u64 xform_mirrored = 0;
		u64 xform_degenerate = 0;
		u64 tex_albedo_ucode = 0;
		u64 tex_albedo_guess = 0;
		// ROUND 43b. Draws whose elected albedo unit changed because units that cannot carry RGB
		// were dropped (fp_fingerprint::narrow_sample_mask). A subset of tex_albedo_GUESS, not of
		// tex_albedo_ucode: this rule leaves unit_from_ucode false on purpose, so the retry policy
		// is unchanged and these draws are still counted as guesses.
		//
		// It counts only draws the rule actually MOVED. Every draw where dropping the narrow units
		// would have elected the same unit returns 'referenced' untouched and is not counted, so
		// this number is the exact size of the behavioural delta - expect it small and bounded:
		// offline, only 3 of 323 programs re-elect, all of them unit 0 -> unit 1.
		//
		// READING IT: 0 means the rule never moved a draw. That is a real possibility and NOT the
		// same as "it is broken" - check fpalbedonarrow=1 on the banner first, then whether the
		// containment tests in albedo_unit_mask (no bindable unit, or the same unit either way)
		// are what declined.
		u64 tex_albedo_narrow = 0;
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
		u64 skip_albedo = 0;

		// ROUND 45. skip_albedo is the SUM of six independent gates, and for eleven rounds it was
		// the only number any of them produced. It is printed on 'Remix stats:', which goes only to
		// bin\log\RPCS3.log - so "how much does gate X eat" was answerable from neither log without
		// blanking a knob and relaunching. These six partition it exactly
		// (skip_gate_pair + skip_gate_albedo + skip_gate_untexvp + skip_gate_untexfppair
		//  + skip_gate_chardepth + skip_gate_unbound == skip_albedo, an invariant a run can check),
		// and they are printed on 'Remix live:', which DOES reach remix_dump.log.
		//
		// Note what this replaces rather than adds: 'Remix skip-census:' already names each gate the
		// first time it eats a given (gate, vp, fp, albedo), and its 256-line cap has never been
		// reached on this title (6 of 256 in the round-44b play-test), so a gate with no census line
		// fired ZERO times run-wide. What the census cannot say is HOW MUCH, because it dedups. That
		// is the hole these close.
		u64 skip_gate_pair = 0;
		u64 skip_gate_albedo = 0;
		u64 skip_gate_untexvp = 0;
		u64 skip_gate_untexfppair = 0;
		u64 skip_gate_chardepth = 0;
		u64 skip_gate_unbound = 0;

		// skip_vp has the same defect and one of its two gates is armed on main-pass geometry:
		// SKIPEXTENTVP=57A12323F22F4988 with SKIPEXTENTMIN=128. skip_gate_vp + skip_gate_extent
		// == skip_vp.
		u64 skip_gate_vp = 0;
		u64 skip_gate_extent = 0;
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

		// --- ROUND 39: the dome classifier -----------------------------------------------------
		// Read in this order; each pair is a different question and reading them out of order is
		// how the last four rounds mis-attributed a sky result.
		//
		//   skyclassify_seen      draws that reached the rule at all (albedo != 0, mode != 0).
		//   skyclassify_dome      of those, the ones whose own geometry passed every gate.
		//   skyclassify_tracked   unique hashes in the table.
		//   skyclassify_armed     hashes that met the draw count, the settle window and the
		//                         no-disqualification rule. THIS is the number to check against
		//                         haze_domes.csv: 16 dome resources exist but only 12 outside
		//                         multiplayer, so 8..20 is expected over a full single-player
		//                         pass and anything above ~28 is over-matching.
		//   skyclassify_promoted  of those, the ones actually inserted into the emissive set. It is
		//                         BELOW armed by exactly the number of hashes that were already on
		//                         RPCS3_REMIX_SKYEMISSIVE - so armed-minus-promoted is the count of
		//                         ground-truth domes the classifier agreed with, and if it is 0 the
		//                         rule found nothing the user had already found by hand, which is a
		//                         reason to distrust it rather than to celebrate it.
		//   skyclassify_rejected  DRAWS carrying a hash that a non-dome draw disqualified - not
		//                         hashes. It is incremented on every later draw of a disqualified
		//                         hash, so it climbs with traffic and says nothing about how many
		//                         textures were refused. The count of DISTINCT refused hashes is
		//                         the number of 'reject:mixed' lines in 'Remix skyclassify:'.
		//                         (Same per-draw convention as sky_hash_rejected above.)
		//   skyclassify_settling  arm attempts refused only because the settle window had not
		//                         elapsed. Non-zero and then falling is the window working; stuck
		//                         high at the end of a session means SKYCLASSIFYSETTLE is too long
		//                         for how briefly the level is visited.
		//   skyclassify_entries   texture-cache entries whose material the promotion rebuilt. This
		//                         is the number that says the promotion REACHED the material; 0
		//                         with skyclassify_promoted > 0 means the texture was not resident
		//                         and the rebuild found nothing to do.
		u64 skyclassify_seen = 0;
		u64 skyclassify_dome = 0;
		u64 skyclassify_tracked = 0;
		u64 skyclassify_armed = 0;
		u64 skyclassify_promoted = 0;
		u64 skyclassify_rejected = 0;
		u64 skyclassify_settling = 0;
		u64 skyclassify_entries = 0;

		// ROUND 39, both added when arming was moved to latch on SUCCESS rather than on intent.
		//   skyclassify_overflow  arm attempts refused because the 64-entry promoted array is
		//                         full. Nothing was promoted and nothing latched, so the hash is
		//                         retried. Non-zero means the title has more dome textures than
		//                         the array holds - raise the array, not a threshold.
		//   skyclassify_failed    arm attempts where the hash entered the set but EVERY resident
		//                         texture entry refused the material rebuild. The promotion is
		//                         withdrawn and retried. Non-zero is the case that would
		//                         otherwise be a permanently black dome reported as a success.
		u64 skyclassify_overflow = 0;
		u64 skyclassify_failed = 0;

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

		// The RPCS3_REMIX_VIEWMODELVP subset of viewmodel_tagged: draws tagged because their
		// vertex program is on the hash list rather than because their viewport depth range said
		// so. An exact subset - counted at the one site that increments viewmodel_tagged - so
		// tagged minus this is still the depth rule's own population, which on Haze is 0.
		u64 viewmodel_tagged_hash = 0;
		// Hash-tagged draws the anchor guard put back: the program is on the list but this
		// particular draw sits further than VIEWMODELANCHOR from the eye, i.e. it is the world
		// geometry the program also draws. The R2 lesson (a program hash does not separate
		// viewmodel from world) made measurable. Large relative to tagged_hash means the listed
		// program is mostly world geometry and does not belong on the list.
		u64 viewmodel_hash_anchor_refused = 0;

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
		// Round 35. Kept SEPARATE from cat_hidden on purpose: cat_hidden was already 2617 in the
		// round-34 run from the albedo list, so a shared counter could not answer "did the new
		// (vp, fp) route fire at all". Counts matches, whichever mode they took.
		u64 cat_hide_pair = 0;
		u64 cat_particle = 0;
		u64 cat_decal = 0;
		// Not a hash-list category: this is the global "Generate Smooth Normals" toggle, so in a
		// normal run it equals the number of world draws submitted rather than a subset of them.
		u64 cat_smooth_normals = 0;

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
		// Alpha-blended draws whose albedo texture has a constant alpha channel, so the blend the
		// game asked for resolves to a no-op. 'rescued' took its alpha from ATTR3 instead;
		// 'stranded' had no varying ATTR3 alpha to take and stays opaque. stranded > 0 means
		// there is a second alpha source in the title's fragment programs that this does not
		// reach, and only fragment-program analysis will find it.
		u64 blend_alpha_rescued = 0;
		u64 blend_alpha_stranded = 0;

		u64 blend_chained = 0;
		u64 blend_translucent = 0;
		u64 blend_unmapped = 0;
		// The lower bound the comment above asks for, measured instead of inferred: draws that
		// translated cleanly (so blend_unmapped did not see them), reached the runtime with
		// alphaBlendEnabled = 1, and will still be raytraced opaque because their factor pair is
		// not in calculateAlphaState()'s table.
		//
		// Excludes the ONE/ZERO opaque alias, which is the runtime agreeing with the draw rather
		// than overruling it. The first version of this counter did not, and reported 5921 on a
		// Resistance 2 run that had no genuine rejections at all - the game uses exactly four
		// blend setups (ONE/ZERO, SRC_ALPHA/ONE_MINUS_SRC_ALPHA, ONE/ONE, SRC_ALPHA/ONE) and the
		// table accepts every one of them. A non-zero value here now means a real rejection.
		u64 blend_runtime_opaque = 0;

		// --- round 5: guest light injection ------------------------------------------------------
		// Draws whose albedo matched RPCS3_REMIX_GUESTLIGHTALBEDO (or the GUESTLIGHTALBEDO2 glow
		// card) and passed the optional vp/fp narrowing - i.e. how often the *trigger* fired,
		// independent of whether a light was created. match large with guest_lights_created at 0
		// says the dedup radius, the per-frame attempt limit or the cap is the binding constraint,
		// not the matcher; both at 0 says the list is wrong (or empty).
		u64 guest_light_match = 0;
		u64 guest_lights_created = 0;
		// Lights destroyed because no draw re-matched them for GUESTLIGHTIDLE frames. 0 at the
		// default (idle = 0, keep forever).
		u64 guest_lights_reaped = 0;
		// Matches refused because m_guest_lights was already at GUESTLIGHTMAX. Non-zero is the
		// "light spam from a shared fixture texture" risk firing - trim the list.
		u64 guest_light_capped = 0;
		// --- round 41 -----------------------------------------------------------------------
		// Matches refused because the draw's WORLD EXTENT exceeded GUESTLIGHTMAXEXT. The round-40
		// run made this necessary: RPCS3_REMIX_GUESTLIGHTALBEDO=71D189E9B559A7F9 is a bulb texture,
		// and 'Remix guest-light:' recorded 31 lights on it whose extent ranged 0.5385 .. 83.18 -
		// a 154x spread on ONE albedo. The 0.54 .. 1.73 rows are the bulbs; the 7.89 .. 83.18 rows
		// are a large mesh sharing the texture, and the biggest of them produced a 29.1-unit sphere
		// the user saw as "too big and not aligned with the bulbs". small_enough already existed
		// and gated the census and the AUTO trigger; it did NOT gate an explicitly listed albedo,
		// which was an oversight rather than a decision - no comment ever argued for the exemption.
		// guest_light_match still counts the match, so match - toobig is the accepted population
		// and the two cannot be confused. RPCS3_REMIX_GUESTLIGHTLISTEXT=0 restores round 40.
		u64 guest_light_toobig = 0;
		// ROUND 50: sources rejected by the MOTION gate -- seen in more than GUESTLIGHTCELLS distinct
		// quantised cells, i.e. it walked. Counted rather than only logged, so a run can be judged
		// without grepping: this climbing while guest_light lines stay flat is the gate working.
		u64 guest_light_moving = 0;

		// --- round 48 -------------------------------------------------------------------------
		// AUTO candidates refused because the draw was the player's own viewmodel. Round 41's
		// GUESTLIGHTAUTO=2 put lights on the arms/weapon and on soldiers; this counter sizes the
		// first half. It counts REFUSALS, not draws, so it can exceed the number of fixtures.
		u64 guest_light_vm_refused = 0;
		// AUTO candidates that had not yet re-appeared in the same quantised cell on
		// GUESTLIGHTSTABLE distinct frames. A walking NPC's glow card lands here every frame and
		// never graduates; a bolted-down lamp graduates after N. 0 with GUESTLIGHTSTABLE non-zero
		// and guest_lights also 0 means the population never REACHED the gate - look upstream at
		// guest_light_match, not here.
		u64 guest_light_unstable = 0;

		// --- round 5: alpha-to-coverage (the cutout mechanism this backend never read) -----------
		// Draws whose fragment shader control word carries RSX_SHADER_CONTROL_ALPHA_TO_COVERAGE,
		// and draws with NV4097_SET_ANTI_ALIASING_CONTROL's alpha-to-coverage bit set. Both are
		// counted whether or not RPCS3_REMIX_FPA2C replays anything: detection is the deliverable,
		// and a run with both at 0 is the negative verdict on the whole alpha-test theory for the
		// Remix kil: population.
		u64 a2c_ctrl = 0;
		u64 a2c_reg = 0;
		// Draws that actually got an alpha test written into their blend extension because of the
		// above. Bounded by the same preconditions as kil_alpha_applied (bound material, RSX alpha
		// test disabled, BLENDSTATE on).
		u64 a2c_applied = 0;

		// --- round 5: pre-anchor deferral (the anchor_prev wobble population) --------------------
		// Instances buffered because their render source had no current-frame anchor yet;
		// flushed_fresh is the subset submitted when that anchor arrived (the success partition),
		// flushed_flip is the remainder pushed out at flip through the old prev/camera fallback.
		// fresh >> flip is the reading that says the fix landed. flip dominant means Haze's anchor
		// arrives too late in its frame and full flip-deferral is the round-6 case.
		u64 defer_buffered = 0;
		u64 defer_flushed_fresh = 0;
		u64 defer_flushed_flip = 0;
		// Deferral candidates that were submitted immediately anyway: skinned draws (bone arrays
		// make the payload non-POD) and frames that hit DEFERPREANCHORMAX.
		u64 defer_skipped_skinned = 0;
		u64 defer_spilled = 0;
		// --- round 32: the deferral candidates NOT taken, because the bet is unlikely to pay ---------
		// Draws on a render source with no anchor in this frame OR the previous one, which
		// RPCS3_REMIX_DEFERPREVONLY (default 1) declines to buffer. MEASURED at 35.8% of the deferral
		// population in both of the round-31 sessions (gauge_absent / (gauge_prev + gauge_absent)).
		// Non-zero here with DEFERPREANCHOR=0 is expected and harmless - the count is taken at the
		// election, which runs regardless; it only changes behaviour when deferral is enabled.
		u64 defer_absent_declined = 0;

		// --- ROUND 46: the tag-only viewmodel population, and its operator replay -----------------
		// A FUNNEL, and each stage is counted where it actually happens rather than where it is
		// decided. Round 46's own review caught the first draft counting defer_viewmodel ~150 lines
		// upstream of the arming gate, which made it non-zero at DEFERPREANCHOR=0 (where nothing is
		// deferred at all) and left it over-counting against three cancellation sites and three
		// submit-site rejections - inverting the reading its comment prescribed.
		//
		// vm_op_captured   : submit-site captures of Op = post * pre^-1, the composite of
		//                    apply_viewmodel_basis and apply_viewmodel_rotation. Counted whether or
		//                    not the draw is then deferred, and whether or not any knob is armed, so
		//                    it sizes the viewmodel population per frame on ANY run.
		// vm_op_declined   : viewmodel draws whose operator is NOT a placement-independent
		//                    world-space left-multiply at the current knob values, so Op cannot be
		//                    replayed onto a re-divided transform. VMROTAXIS >= 4 is a MODEL-space
		//                    right-multiply (M' = M*R); VMROTPIVOT != 0 and VMBASISPIVOT != 0 derive
		//                    the pivot from the placement itself. These draws are refused entry to
		//                    the deferral and take the immediate path, i.e. exactly round-45
		//                    behaviour. Non-zero at the shipped VMROTAXIS=1 VMROTPIVOT=0 VMBASIS=0
		//                    would mean a knob moved.
		// vm_op_singular   : captures abandoned because the pre-operator 3x3 would not invert, or the
		//                    result did not survive the narrowing to f32. Same consequence as
		//                    declined: no deferral, immediate path, nothing lost.
		// defer_viewmodel  : viewmodel draws ACTUALLY BUFFERED, counted at the buffering site beside
		//                    defer_buffered. Structurally 0 unless BOTH DEFERVIEWMODEL=1 and
		//                    DEFERPREANCHOR=1, and it cannot count a draw that a later gate rejected.
		//                    0 with both knobs armed means the route has no candidates and the
		//                    diagnosis, not the plumbing, is wrong.
		// vm_op_replayed   : flushes that re-applied a captured Op. Bounded above by defer_viewmodel;
		//                    the gap is exactly the draws that timed out to flip, which keep the
		//                    operator-corrected transform they already carried. A gap ~equal to
		//                    defer_viewmodel means every held viewmodel draw reached flip and the
		//                    knob changed nothing visible.
		u64 defer_viewmodel = 0;
		u64 defer_vm_op_captured = 0;
		u64 defer_vm_op_declined = 0;
		u64 defer_vm_op_singular = 0;
		u64 defer_vm_op_replayed = 0;

		// --- ROUND 47: the proj_split half of the deferral -----------------------------------------
		// defer_projsplit      : draws BUFFERED that the proj_split rescue placed with a stale anchor.
		//                        Counted at the buffering site beside defer_buffered, so like
		//                        defer_viewmodel it cannot count a draw a later gate rejected.
		//                        Structurally 0 unless DEFERVIEWMODEL=1 and DEFERPREANCHOR=1 both.
		// defer_projsplit_fresh: flushes where the cross was REBUILT against the anchor that just
		//                        landed and passed the same is_affine gate the ladder accepts on.
		//                        This is the population whose placement actually improved.
		// defer_projsplit_kept : flushes where the fresh cross was not constructible or failed that
		//                        gate, so the draw KEPT the stale placement the ladder gave it. This
		//                        arm is bit-exact round-46 behaviour for that draw - the change can
		//                        never be worse than not deferring, only inert.
		//                        fresh + kept == defer_projsplit is NOT an invariant: a draw whose
		//                        anchor never lands goes out through flush_deferred_at_flip, which
		//                        touches neither counter. The residual is exactly that population.
		u64 defer_projsplit = 0;
		u64 defer_projsplit_fresh = 0;
		u64 defer_projsplit_kept = 0;

		// --- round 5: nectar publish gate --------------------------------------------------------
		// Flips on which haze.nectar_disruption was published as "1". 0 at NECTARMODE=0.
		u64 nectar_published = 0;

		// --- round 6: the untextured 41%, split so the two possible fixes can be told apart ------
		// NEITHER is a subset of tex_retry_refused, as written (corrected round 7 - the older
		// comment here claimed both were). They are counted in the '!material' block that runs
		// after the walk has finished, so they cover every walk exit: no-unit, budget, tombstone
		// and retry-refused alike. colorunit happens to be armed on the retry-refused edge, but
		// it is sticky across the whole walk and survives a later exit; shadowonly is decided by
		// re-reading the WHOLE eligible unit mask and has no relationship to the exit at all.
		// shadowonly: the ONLY eligible unit the walk could see carries a DEPTH format, so no
		// colour texture exists for this draw anywhere in its own sampler set - it is a
		// shadow-projection / lighting composite pass, and the answer is not to submit it as a
		// white surface. colorunit: the fragment program's sampled_mask names some OTHER eligible
		// unit whose bound texture has a colour format - i.e. reachable albedo the walk refused,
		// and the answer is the walk. The pair is the whole (a)-vs-(b) verdict, readable
		// mid-session on 'Remix live:'. They are not a partition of tex_retry_refused: a draw can
		// be neither (a colour unit exists but is not sampled).
		u64 tex_none_shadowonly = 0;
		u64 tex_none_colorunit = 0;
		// Draws that reached submission with no albedo material and were given the shared mid-grey
		// fallback instead of being submitted material-less (i.e. white). Reads against tex_none:
		// the difference is the draws refused before submission by a skip rule.
		// RPCS3_REMIX_NOTEXMAT=0 drives it to 0.
		u64 notex_mat_applied = 0;
		// Draws refused as world geometry by SKIPSHADOWONLY. Its named census is 'Remix shadowonly:'.
		// Reads against tex_none_shadowonly: the difference is the main-clip shadow-only draws,
		// which are deliberately exempt this round.
		u64 skip_shadowonly = 0;
		// Draws whose albedo walk stepped past a refused unit to a unit the fragment program
		// actually samples, carrying a colour format - the WALKSAMPLED widening. Reads against
		// tex_unit_substituted: this is the subset that only exists because of the new term.
		u64 tex_walk_sampled = 0;

		// --- round 6: the tail-refusal rescue ladder (the missing world geometry) ---------------
		// Draws the affinity gate was about to refuse (fail=tail) that a re-division against a
		// same-pass-shape anchor rescued: _cur from this frame's anchor, _aged from one up to
		// TAILRESCUEAGE frames old. tail_rescue_failed counts the draws that took the ladder and
		// still refused - that number is what says whether a consensus/voting gauge is needed,
		// because it is exactly the population no existing anchor can place.
		u64 tail_rescued_cur = 0;
		u64 tail_rescued_aged = 0;
		u64 tail_rescue_failed = 0;

		// --- round 7: the denominators round 6's counters were read against, and were wrong -----
		// notex_mat_applied was compared against tex_none and read as "the grey fallback covers
		// 0.03% of the untextured population". That comparison has no meaning: the fallback site
		// is the LAST thing in submit_subdraw, after nine earlier returns, so the population it
		// can ever reach is not tex_none but "material-less draws that survive to submission".
		// These two name the draws that did not survive, so the ratio can be computed instead of
		// assumed. notex_refused_world is the affinity/camera gate (the dominant one on Haze,
		// fail=lay_other); notex_refused_skip is every named skip gate, counted at the one place
		// they all pass through, EXCLUDING the two that fire before any albedo has been resolved
		// and pass a literal 0 ('skipvp', 'viewmodelrefused' - see report_skip_census).
		// tex_none - (world + skip + notex_mat_applied) is the residue that reached submission
		// with the knob off or the material creation broken.
		u64 notex_refused_world = 0;
		u64 notex_refused_skip = 0;

		// Why a shadow-only-classified draw did NOT reach the skip. Round 6 shipped
		// skip_shadowonly with no partition, so 'classified 437810, skipped 20656' could not be
		// told apart from a wiring fault. nomain: no textured draw had established a main clip
		// yet. notsmaller: the draw's own clip was not strictly smaller than the reference - the
		// measured cause, because m_main_clip_* is the PREVIOUS FRAME's largest textured clip and
		// on Haze that is 512x288 on most frames, exactly the clip the shadow-only passes use.
		// The remainder (classified - skipped - nomain - notsmaller) is the population an earlier
		// exact skip rule refused first, which is not a defect: it is already not submitted.
		u64 shadowonly_nomain = 0;
		u64 shadowonly_notsmaller = 0;

		// Rescue retries whose 'different' anchor was bit-identical to the reference the draw had
		// already been divided by. Round 6's whole premise was that fail=tail draws were divided
		// by the WRONG reference; if this counter tracks tail_rescue_failed the premise is dead,
		// because the ladder is re-running the same division and must reproduce the same residue.
		u64 tail_rescue_same_ref = 0;

		// Round 8, measurement only. Partition of tail_rescue_failed by whether the draw's OWN
		// fused matrix splits into a plausible view x projection. They sum to tail_rescue_failed
		// by construction. ok >> fail says these draws carry a real, *different* projection and
		// the round-9 fix is "a second projection family gets its own reference"; fail dominant
		// says no anchor of any kind could ever place them and the ladder is the wrong tool.
		u64 tail_split_ok = 0;
		u64 tail_split_fail = 0;

		// --- round 18: the second-projection reference (RPCS3_REMIX_PROJSPLIT) -----------------
		// The three of these partition the tail_split_ok population exactly when PROJSPLIT is on,
		// and are all 0 when it is off. proj_split_applied is THE acceptance counter for the
		// Selva effects item: it counts draws that were dropped before this round and are now
		// placed by a reference crossing the anchor's view with the draw's own projection.
		// proj_split_refused is the honest other half - the cross was constructible and the
		// result still failed the same is_affine gate, which would refute the premise rather
		// than the implementation. proj_split_nosplit counts the draws whose own fused matrix or
		// whose anchor's would not split at all, or split only on the transpose.
		u64 proj_split_applied = 0;
		u64 proj_split_refused = 0;
		u64 proj_split_nosplit = 0;

		// --- round 19 -------------------------------------------------------------------------
		// The subset of proj_split_refused that was rejected on the VIEW-DELTA premise rather than
		// on affinity. It is a strict subset, not a fourth partition class: the vdelta arm
		// increments both, so proj_split_applied + refused + nosplit still partitions tail_split_ok
		// exactly.
		//
		// Why it exists at all: round 18's is_affine gate on this construction is VACUOUS. Both
		// splits come out of try_split_once's orthonormal basis, so the recovered world is
		// V_draw * V_anchor^-1 - rigid, hence affine, hence accepted no matter how wrong the
		// premise. That is why proj_split_refused read 0/74552 and 0/170115 in the two measured
		// runs, and it is NOT evidence the fix is correct. With RPCS3_REMIX_PROJSPLITVDELTA=0 (the
		// default) this stays 0 and nothing is refused; the vdelta= field on the census reports the
		// distribution regardless, so the threshold can be chosen from data rather than guessed.
		u64 proj_split_viewdelta_refused = 0;

		// --- round 9 --------------------------------------------------------------------------
		// Draws whose instance blend extension stated the colour pipeline the fragment ucode
		// proves instead of the parity "colour = Texture" default. This is the effects fix
		// counted where it lands - a draw, not a program. RPCS3_REMIX_FPVCOL=0 drives it to 0 and
		// the nectar pulse must go back to flat grey with it.
		u64 fpvcol_applied = 0;

		// Strict subset of vcol_applied: TEXTURED draws that got their ATTR3 decoded into the
		// submitted mesh because their fragment program proved the modulate. This is the half
		// that moves mesh content hashes, so it is counted apart from the material-less
		// population. RPCS3_REMIX_VCOLMOD=0 drives it to 0.
		u64 vcol_mod_applied = 0;

		// Programs (not draws) classified by scan_fragment_program's output-source walk. Counted
		// once per unique fragment program, at the scan. fp_vcol_pass is the 'MOV out, COL0'
		// shape, fp_vcol_mod the 'MUL out, <TEX>, COL0' shape.
		u64 fp_vcol_pass = 0;
		u64 fp_vcol_mod = 0;
		// Classified into a vcol shape but reading COL1, which this backend does not decode into
		// the submitted vertex - named rather than silently replayed from the wrong data.
		u64 fp_vcol_col1 = 0;

		// --- ROUND 42 ---------------------------------------------------------------------------
		// fp_vcol_deep: PROGRAMS the deep modulate search classified that the terminal instruction
		// could not - a strict subset of fp_vcol_mod. vcol_deep_applied: the DRAWS those programs
		// account for, counted where vcol_mod_applied is. The pair is what says whether the
		// widening reached, and how much of the screen it reached: fp_vcol_deep counts shapes,
		// vcol_deep_applied counts pixels' worth of them. Both 0 with fpvcoldeep=1 on the banner
		// means the search ran and matched nothing, which is a different failure from not running.
		// RPCS3_REMIX_FPVCOLDEEP=0 drives both to 0.
		u64 fp_vcol_deep = 0;
		u64 vcol_deep_applied = 0;

		// Vertex programs whose raw ucode was written to bin\remix_ucode\ because their position
		// decode was refused. Once per program per run. RPCS3_REMIX_UCODESTORE=0 drives it to 0.
		u64 ucode_stored = 0;
		// The write itself failed (directory not creatable, disk full). Non-zero means the
		// capture is not on disk however healthy ucode_stored looks.
		u64 ucode_store_failed = 0;

		// The mixed-lane fused-group arm. madmix_resolved counts PROGRAMS whose group only
		// matched because the row scalars were chased back to one base temp; madmix_draws counts
		// the draws that then used them. RPCS3_REMIX_MADCHAINMIX=0 drives both to 0.
		u64 madmix_resolved = 0;
		u64 madmix_draws = 0;

		// Round 17. The subset of the above that ALSO needed the divide's lane permutation read
		// out of the ucode - the Selva tree tops. madlanemap_resolved counts PROGRAMS, and the
		// offline sweep says it should reach 5 on this title once all five are drawn;
		// madlanemap_draws counts the draws that used them. RPCS3_REMIX_MADLANEMAP=0 drives both
		// to 0 and puts the canopy back where round 16 had it.
		u64 madlanemap_resolved = 0;
		u64 madlanemap_draws = 0;

		// --- round 10 -------------------------------------------------------------------------
		// Camera candidates refused before the vote because their surface clip is neither the
		// same as, nor double, nor half the session main clip. Counts CANDIDATE OBSERVATIONS,
		// not frames: Haze's 2048x2048 shadow pass offered 17 in the frame it won the election.
		// RPCS3_REMIX_CAMCLIPGATE=0 drives it to 0 and the portal frames come back with it.
		u64 cam_clipgate_refused = 0;

		// Frames in which a pose-continuous FALLBACK-program candidate refreshed the held lock
		// camera's matrices instead of starting a 12-frame identity switch. This is the flicker
		// that stops being reported: cam-elect lines go quiet while this climbs.
		u64 cam_fallback_relatch = 0;

		// The continuity-elected gauge anchor, three counters that partition the new behaviour.
		// parked: a frame's FIRST same-key candidate disagreed with the slot's previous-frame
		//         gauge and was held aside instead of seizing the slot (the Selva fix firing).
		// recaptured: a later, continuous same-key candidate installed and displaced a parked one
		//         (the good donor arrived after the bad one - the exact Selva ordering).
		// promoted: no continuous donor arrived all frame, so the parked candidate was installed
		//         at flip. A real cut converges in one frame here; steady play should read ~0.
		u64 gauge_anchor_parked = 0;
		u64 gauge_anchor_recaptured = 0;
		u64 gauge_anchor_promoted = 0;

		// ROUND 44b. offside survives round 44 as PURE MEASUREMENT: candidates whose own placement
		// against the held gauge exceeds RPCS3_REMIX_WORLDIDMAXT. Nothing reads it. It measured
		// 160,740 over 17,583 flips (9.14/flip), which is why the defect is not in doubt even though
		// round 44's remedy for it was wrong. gauge_donor_refused is GONE with that remedy.
		u64 gauge_donor_offside = 0;

		// ROUND 44b, RPCS3_REMIX_GAUGEDONORBEST. upgrade_avail counts frames where a later donor was
		// at least twice as close to the origin as the installed one, WHETHER OR NOT the knob is
		// armed - so an unarmed run sizes the route before anyone plays it. upgraded counts the
		// replacements that actually happened. avail > 0 with upgraded == 0 means the knob is off.
		u64 gauge_donor_upgrade_avail = 0;
		u64 gauge_donor_upgraded = 0;

		// ROUND 44, diagnostic only, no knob and no pixel effect.
		// dedup:    draws suppressed by the static-index submit-signature gate. It has never had
		//           a counter, and it sits between ++xform_measured and ++draws_submitted as the
		//           only exit besides the poisoned-mesh one - so on the round-43 build its size
		//           was already implied: xform_measured 8,569,414 - draws_submitted 7,944,541
		//           - poisoned 0 = 624,873 draws, 7.29% of everything that resolved a mesh.
		// meshdiff: the subset where the mesh this draw had selected is NOT the mesh already
		//           submitted under that signature. Those are the ones where a strictly larger
		//           union was built and thrown away. THIS is the number round 45 needs: near 0
		//           means the collapse is benign and the checkerboard is elsewhere; large means
		//           the floor is being drawn from a partial union every frame.
		u64 static_submit_dedup = 0;
		u64 static_submit_meshdiff = 0;

		// Draws whose ALPHA halves of the FPVCOL replay were withheld because the guest consumes
		// no fragment alpha (blend and alpha test both off). Round 9 replayed alpha there
		// unconditionally, which turned 0-vertex-alpha opaque geometry fully invisible.
		// RPCS3_REMIX_FPVCOLALPHAGATE=0 drives it to 0.
		u64 fpvcol_alpha_skipped = 0;

		// Draws that took the self-illuminated twin of the neutral material instead of the plain
		// grey, and the subset of those that additionally stated ONE/ONE ADD. Both are draws.
		u64 fpvcol_emissive = 0;
		u64 fpvcol_additive = 0;

		// --- round 11 -------------------------------------------------------------------------
		// Draws that satisfied every other term of the self-lit verdict and were excluded by the
		// extent gate alone. On Haze this is the 82-vertex sky dome and nothing else; a non-zero
		// value with rtx.skyMode=0 is the gate DOING ITS JOB, not a refusal to investigate.
		// RPCS3_REMIX_FPVCOLSKYGATE=0 drives it to 0 and re-opens the regression.
		u64 fpvcol_skygate = 0;

		// Fragment programs whose raw ucode was captured to bin\remix_ucode\*.fp, and the failures.
		u64 ucode_fp_stored = 0;
		u64 ucode_fp_store_failed = 0;

		// Draws refused an ATTR3 replay because the VERTEX program does not route ATTR3 into COL0
		// (vcol_route computed/none), or refused only the ALPHA half because o1.w's chain never
		// reaches I3.w (the flower). This is the partition term against round 10's fpvcol_applied:
		// a drop there of exactly this size is the route gate working as designed.
		u64 vcol_route_blocked = 0;

		// Draws whose replay multiplied the decoded ATTR3 by the transform constants the ucode
		// multiplies it by. RPCS3_REMIX_VCOLFOLD=0 drives it to 0 without disabling the replay.
		u64 vcol_fold_applied = 0;

		// Round 12. Draws whose COL0 came from the ucode's own constant ('MOV o1, c[K]') rather
		// than from a mesh attribute. Round 11 refused this shape as 'computed' and the draws
		// rendered white. RPCS3_REMIX_VCOLCONST=0 drives it to 0 and restores that white.
		u64 vcol_const_applied = 0;
		// --- round 41 ---------------------------------------------------------------------
		// Constant-route replays REFUSED on a textured draw because the resolved RGB is (near)
		// black. MEASURED motivation: `Remix vcolroute:` names two constant-route programs on
		// this title and one of them resolves to `cval=[0 0 0 1]` - pure black
		// (vp=b01bfce3fc580e3b). RPCS3_REMIX_VCOLMOD was turned off on 2026-08-15 because "the
		// HUD gauges render BLACK with this on", and a Modulate by [0 0 0] is exactly that.
		// Refusing leaves the decode loop's white, i.e. the texture unmodified, which is the
		// same "a missing tint is better than a wrong one" identity this file applies to a
		// missing texture and to an unproven alpha.
		u64 vcol_const_black = 0;

		// Draws whose per-vertex atmospheric fade was replayed (the backdrop haze card).
		u64 hazefade_applied = 0;

		// --- round 27: the particle billboard replay ---------------------------------------------
		// THE ACCEPTANCE COUNTER for item 1. particle_replayed is draws whose four-corner expansion
		// this backend performed on the CPU and then submitted; particle_quads is the number of
		// particles inside them. With PARTICLEBILLBOARDVP / PARTICLERIBBONVP armed these must climb
		// while smoke or a missile trail is on screen, and lay_other on 'Remix world-fail:' must fall
		// by the same draw count - that pairing is the whole attribution.
		u64 particle_replayed = 0;
		u64 particle_quads = 0;

		// The refusal partition, so a replay that does nothing says WHY rather than going quiet.
		//   _attr    : ATTR0 / TC0 (/ TC4 for the ribbon) could not be mapped or decoded
		//   _consts  : a transform constant slot the family needs could not be read
		//   _group   : vertex count not a multiple of 4, or a quad's four corners disagreed on ATTR0
		//               - the layout assumption failing loudly instead of writing garbage
		//   _basis   : SIX distinct sites - ateye / basis / degenerate / zeroseg / nonfinite - and
		//              NOT "the camera basis was degenerate", which is what this comment said for a
		//              round. ROUND 28 MEASURED: 100% of them are `zeroseg` and 100% of those are
		//              ribbons, so read particle_zeroseg below, not this.
		//
		// UNITS, and they are NOT uniform: _attr / _consts / _group each fire both before the quad
		// loop (per DRAW) and inside it (per QUAD); _basis fires only inside it, so it is purely per
		// QUAD. They are a diagnosis, never a partition, and a draw can appear in one of them AND in
		// particle_replayed. For the partition use particle_replayed + particle_declined.
		u64 particle_refused_attr = 0;
		u64 particle_refused_consts = 0;
		u64 particle_refused_group = 0;
		u64 particle_refused_basis = 0;

		// --- round 28 -----------------------------------------------------------------------------
		// THE PARTITION round 27 lacked. A draw on a PARTICLEBILLBOARDVP / PARTICLERIBBONVP program
		// either has its four-corner expansion replayed (particle_replayed) or is declined and falls
		// through to the ordinary world refusal (particle_declined). Both count DRAWS, and together
		// they are every matched draw THAT REACHED THE REPLAY SITE - not every matched draw. That
		// qualifier is load-bearing: submit_subdraw returns early in ~40 places before the replay, and
		// skip_instanced in particular discards pass_count()-1 passes of a trivially instanced draw on
		// the test !fp->has_outer(), which is exactly the property these six programs have.
		//
		// This exists because round 27's stated acceptance test - "lay_other must fall by the same
		// draw count" - is structurally impossible: lay_other is raised inside per_draw_transform()
		// before the replay's verdict is ever consulted, so it counts the population rather than the
		// drop and stays flat however well the replay works.
		u64 particle_declined = 0;

		// Ribbon quads whose two endpoints coincided, so no tangent and no side vector exist. The
		// guest's own RSQ(0) makes the same quad vanish, so this is FAITHFUL, not lost geometry.
		// Counts QUADS. Round-27 run: this was the entire content of particle_refused_basis.
		u64 particle_zeroseg = 0;

		// Quads whose replayed distance-alpha fade reached zero, i.e. exactly the ones the guest's own
		// cull-park hides. NOT a failure: this climbing while particles are on screen is the fade
		// working, and it is what stops faded smoke rendering at full opacity. Counts QUADS.
		u64 particle_alpha_culled = 0;

		// Draws whose atlas sub-rect UV was replayed too (RPCS3_REMIX_PARTICLEUV=0 drives this to 0
		// without disabling the position replay).
		u64 particle_uv_applied = 0;

		// --- round 27: the sun sprite ------------------------------------------------------------
		// THE ACCEPTANCE COUNTER for item 2. sun_sprite_seen is draws carrying a SUNSPRITE albedo;
		// sun_sprite_solved is the subset that passed the on-screen and span guards and published a
		// direction. seen climbing with solved at 0 means every sighting was rejected - read the
		// 'Remix sunsprite:' census for the reason rather than guessing.
		u64 sun_sprite_seen = 0;
		u64 sun_sprite_solved = 0;
		u64 sun_sprite_offscreen = 0;  // the quad touched or crossed the NDC edge
		u64 sun_sprite_span = 0;       // too large, or too far from square
		// No camera, OR a camera with no view*projection inverse, OR an unprojection that produced a
		// degenerate ray. ROUND 28 MEASURED that in the round-27 run this was 610/610 the middle case
		// (`camvalid=1 vpinv=0`) and never the first, and the cause - the archetype-B camera path
		// never filling has_view_proj_inverse - is fixed this round. Read the census verdict, not the
		// counter name.
		u64 sun_sprite_nocam = 0;
		// Split out of sun_sprite_nocam in round 28. The ray came out pointing AWAY from the camera's
		// own forward axis, which is what a reverse-Z guest would do and which would otherwise aim the
		// sun exactly 180 degrees backwards. Measured at zero on this title; any non-zero reading is
		// a real finding and must not be averaged into the bucket above.
		u64 sun_sprite_backwards = 0;

		// --- round 14 ---------------------------------------------------------------------------
		// Draws that passed the sun-card SHAPE test (blended, no depth write, few vertices, textured,
		// above the horizon). This is the census population, not a decision: it climbs with
		// SUNTRACK=0 and names nothing on its own.
		u64 suncard_seen = 0;

		// The strict subset whose albedo is on RPCS3_REMIX_SUNCARDALBEDO. With a non-empty list this
		// must climb, or the pin is wrong and everything downstream of it is inert.
		u64 suncard_pinned = 0;

		// Frames in which a candidate won the election and published a direction. suncard_elected
		// with sun_retargeted at 0 means the direction never moved past SUNTRACKDEG - which is the
		// correct reading for a static sun, not a failure.
		u64 suncard_elected = 0;

		// Distant-light re-aims actually performed. This is the cost counter: if it tracks the
		// frame count, raise RPCS3_REMIX_SUNTRACKDEG.
		//
		// Round 26: these are re-aims, not destroy+create pairs. The destroy is gone - see the note
		// at the top of update_sun_light() for why it was deleting the sun outright.
		u64 sun_retargeted = 0;

		// Round 26. DestroyLight calls made against the sun handle, anywhere. This must read 0 on
		// every stats line of a normal run: the deployed runtime defers DestroyLight to Present and
		// applies it AFTER an immediate CreateLight for the same handle, so any non-zero value here
		// during play means the sun is being deleted again and the direction will appear frozen.
		// Only the shutdown teardown is allowed to raise it.
		u64 sun_destroys = 0;

		// Draws tagged viewmodel by ALBEDO rather than by program hash. Deliberately its own
		// counter: vm_tagged_hash must stay the program-list population so the two can be read
		// apart when only one of the two routes is armed.
		u64 vm_tagged_albedo = 0;

		// Round 20. Draws tagged viewmodel by the (RPCS3_REMIX_VMPAIRVP, RPCS3_REMIX_VMPAIRALBEDO)
		// PAIR route. Its own counter for the same reason vm_tagged_albedo is: the three routes must
		// be readable apart, and this one is the only one that can be armed without either widening
		// the albedo list or tagging a program wholesale. Structurally 0 unless both lists are set.
		u64 vm_tagged_pair = 0;

		// Round 23. Draws the PAIR route matched and then refused because their geometry centre sat
		// further from the eye than RPCS3_REMIX_VMPAIRMAXDIST. Structurally 0 while that knob is 0,
		// which is the default, so a non-zero value is proof the new bound is doing work.
		u64 vm_pair_far = 0;

		// Round 23. Draws routed to the 2D compositor by RPCS3_REMIX_UIFORCEVP that
		// is_screen_space_draw() would have submitted as world geometry. Structurally 0 while that
		// list is empty. skip_screen_space counts them too - this is the forced SUBSET of it.
		u64 ui_forced = 0;

		// Round 36. The (RPCS3_REMIX_UIFORCEPAIRVP, RPCS3_REMIX_UIFORCEPAIRFP) subset of the same
		// override - the helmet route. Structurally 0 while either half is blank, so a 0 here means
		// "not armed or the hashes are wrong", never "the mechanism is broken". Kept separate from
		// ui_forced above for the reason round 35 had to split cat_hidepair out of cat_hidden: a
		// shared counter cannot say WHICH key matched.
		u64 ui_forced_pair = 0;

		// Round 23. Sky-dome draws examined by the per-level sun derivation, and the ways it ends:
		// a solution written, or the draw rejected for having too few vertices / no usable UV match
		// / no valid camera, and since round 24 also for deriving a sun at or below the horizon.
		// BOTH sun_sky_solved and sun_sky_refused are bounded by the number of distinct dome
		// albedos, because round 24 made the refusal STICKY - a refused albedo claims its slot with
		// source=0, so find_sky_sun answers for it and derive_sky_sun is never re-entered for it.
		u64 sun_sky_examined = 0;
		u64 sun_sky_solved = 0;
		u64 sun_sky_refused = 0;

		// Round 24. The strict subset of sun_sky_refused attributable to the SUNSKYDOWN horizon
		// guard. Separate because the other three refusal paths are structurally expected traffic
		// (every 4-vertex card sharing a dome's albedo trips the vertex floor), so a shared counter
		// could never be used as evidence that this guard fired - which is exactly what the
		// launcher's play-test card needs it for.
		u64 sun_sky_refused_up = 0;

		// Frames in which the VIEW_MODEL camera was submitted as the world camera's twin because
		// the albedo route had no dedicated viewmodel reference to latch from.
		u64 vmcam_twin = 0;

		// Round 13. Frames on which the VIEW_MODEL camera carried its own FOV rather than being an
		// exact copy of the world camera. A twin makes the runtime's correction matrix the identity
		// (every factor cancels - see the submit site), so vmcam_twin climbing while this stays 0 is
		// the statement "a viewmodel camera exists and it cannot move anything".
		u64 vmcam_real = 0;

		// A tagged draw reached the anchor guard before its vertices were decoded, so the guard
		// fell back to the instance-origin measurement for it. Non-zero means VMANCHORGEO is
		// partially inert and the submit order needs re-reading - it is not a guess, it is a
		// named gap.
		u64 vm_anchor_unmeasured = 0;

		// The accumulate-in-place walk. madaccum_resolved counts PROGRAMS whose base writer was
		// only reachable by skipping an accumulate-in-place decoration; madaccum_draws counts the
		// draws that then used them. RPCS3_REMIX_MADACCUM=0 drives both to 0.
		u64 madaccum_resolved = 0;
		u64 madaccum_draws = 0;
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
		u64 mesh_create = 0; // guarded_create_mesh, summed over the frame
		// --- round 31: what the rest of 'draw' is -----------------------------------------------
		// MEASURED in the round-30 play-test: draw=20.15 ms/frame of a 28.14 ms frame, of which the
		// two existing children account for ui=3.20 and mesh_create=0.11 - leaving 16.84 ms/frame
		// (59.8% of the whole frame) attributed to nothing. draw is also 98.8% of the 30.98 ms
		// quarry-vs-interior gap, so this is the only measurement that matters for frame rate and it
		// was the one the line could not make. These three are the plausible occupants of that gap,
		// each a single well-defined call site inside submit_subdraw. All three are siblings of ui /
		// mesh_create - i.e. they are INSIDE draw - so draw - (ui + mesh_create + tex_bind + uv +
		// draw_instance) is the new residual.
		u64 tex_bind = 0;      // texture_cache::bind, summed over the frame (loops over texture units)
		// apply_texcoords, summed over the frame. Runs AT MOST ONCE per sub-draw despite sitting
		// inside the texture-unit walk: its call site is guarded by (material && entry) and the
		// walk breaks unconditionally right after. Only tex_bind above accumulates per unit, so
		// dividing uv by units-walked would be wrong by that factor. (Corrected by review.)
		u64 uv = 0;
		u64 draw_instance = 0; // guarded_draw_instance at the per-draw submit, summed over the frame
		// --- round 32: split 'rest', which is where the frame actually goes -----------------------
		// MEASURED in the round-31 log's worst window that submitted geometry (bin\log\RPCS3.log,
		// frames=38): draw=50.88 ms/frame of a 53.03 ms frame, and the five children above account
		// for ui=2.53 mesh_create=0.14 tex_bind=1.11 uv=5.67 draw_instance=0.48 - leaving
		// rest=40.96 ms/frame, i.e. 80.5% of 'draw' and 77% of the whole frame, attributed to
		// nothing. That is the entire remaining Haze perf story and round 31 could not name it.
		//
		// These four are siblings of the five above (all inside submit_subdraw, none overlapping),
		// so 'rest' stays a valid saturating residual and shrinks by exactly what they capture.
		// Chosen because each is a single contiguous span of O(vertex_count) work that runs on
		// EVERY sub-draw before any gate - which is the shape a 40 ms residual has to have.
		//
		// 'audit' is the one to watch: audit_vertex_extent makes four separate passes over the
		// decoded positions and audit_world_extent walks the whole index list transforming each
		// vertex into world space, and BOTH are pure diagnostics. If audit dominates, the fix is a
		// knob that skips them (RPCS3_REMIX_DRAWAUDIT) rather than an optimisation - which is why
		// the timer and that knob ship together. Every other child here is load-bearing work.
		u64 decode = 0;   // index build + strip expansion + ATTR0 map + per-vertex position decode
		u64 audit = 0;    // audit_vertex_extent + audit_world_extent, both diagnostics
		u64 hash = 0;     // static-index union accumulation + mesh-identity/union re-hash
		u64 xform = 0;    // per_draw_transform: matcher replay, gauge divide, mat4_invert
		// submit_deferred's OWN DrawInstance, which is NOT a child of 'draw' and must never be added
		// to draw_instance above. Its two callers sit in two different parents - one on the draw path
		// (write_gauge_anchor -> flush_deferred_for_anchor) and one inside flip
		// (flush_deferred_at_flip) - so a shared counter could exceed 'draw' and saturate 'rest' to 0.
		// Printed outside the draw=(..) group for that reason. 0 while DEFERPREANCHOR=0.
		u64 deferred_instance = 0;
		u64 mesh_creates = 0;
		u64 mesh_creates_peak = 0;
		u64 mesh_create_buckets[5] = {}; // 0, 1-15, 16-63, 64-127, 128+
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
		u32 observations = 0;
		u64 vp_hash = 0;
		u32 surface_offset = 0;
		u32 color_target = 0;
		u32 clip_width = 0;
		u32 clip_height = 0;
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

		// The frame counter value when these matrices were last written. Flip latches for the
		// *next* frame, so a camera that only ever latches at flip reads latch_frame ==
		// m_frame_counter - 1 for every draw of the frame it serves, and every world transform
		// computed as fused_now * reference_inverse then carries one frame of camera motion in
		// its world-space pose. That is invisible to a rasteriser (the composite is exact) and is
		// exactly the residual prop wobble under a path tracer. world_ref_fresh / world_ref_stale
		// count the two populations; camera_relatch_enabled() is what moves draws into the first.
		u64 latch_frame = umax;

		// The viewport registers in force when this candidate's draw was seen. Diagnostic only:
		// the 512/510 == 256/255 basis ratio measured between Haze's two camera passes is one
		// pixel of viewport on a 512-wide pass, and this is the only place that hypothesis can be
		// tested directly rather than inferred from a residual.
		f32 viewport_scale[2] = { 0.f, 0.f };
		f32 viewport_offset[2] = { 0.f, 0.f };
	};

	// The gauge anchor: one render pass's own view x projection, taken from a draw the title
	// itself states is world-space (a RPCS3_REMIX_WORLDIDENTITYVP program, whose world transform
	// is the identity, so its fused matrix is V*P exactly). Keyed by the render source rather
	// than matched against the elected camera's source on purpose - on Haze the elected camera
	// draws on a half-resolution auxiliary surface, so requiring a match would capture nothing
	// and the whole point is to divide each pass's draws by *that pass's* gauge.
	struct gauge_anchor
	{
		bool valid = false;
		u64 frame = umax;
		u64 vp_hash = 0;
		u32 surface_offset = 0;
		u32 color_target = 0;
		u32 clip_width = 0;
		u32 clip_height = 0;
		remix_rsx::mat4 fused{};
		remix_rsx::mat4 inverse{};
		// The same inverse computed and kept in double. The f32 copy above stays because every
		// consumer outside the divide (the camera reference, the viewmodel probe, the trace) is f32
		// and narrowing once at the source keeps them all reading one gauge; the divide itself takes
		// this one. RPCS3_REMIX_GAUGEF64=0 leaves the f32 inverse in charge of everything, which is
		// the bit-exact A/B.
		remix_rsx::mat4d inverse_f64{};
		bool inverse_f64_valid = false;
		// The self-check has run for this anchor in this stats window. Bounded so a per-frame anchor
		// capture cannot turn 'Remix gauge-selfcheck:' into the dominant writer in the dump log.
		u64 selfcheck_window = umax;
		// The viewport depth-range registers the donor draw folded into 'fused'. A draw divided by
		// this anchor whose own registers differ is being placed by a gauge built for a different
		// depth range; gauge_zfold_mismatch counts that population and the pick line names it per
		// object. Measurement only this round.
		f32 fold_scale_z = 0.f;
		f32 fold_offset_z = 0.f;

		// --- round 10: the parked candidate (RPCS3_REMIX_ANCHORSTICKY) -------------------------
		// A frame's first same-key donor that disagreed with the gauge above beyond the
		// discontinuity tolerance is held here instead of seizing the slot, and is installed at
		// flip only if no continuous donor turned up all frame. That is what makes a real cut
		// converge in one frame while a single stray non-identity draw cannot redefine the whole
		// frame's divide gauge. Cleared the moment it is promoted or displaced.
		// --- ROUND 44b: the frame reference (RPCS3_REMIX_GAUGEDONORBEST) ---------------------
		// The gauge this slot carried when the frame's FIRST donor arrived - last frame's - kept
		// for the duration of the frame so that two candidates of one frame are comparable.
		// slot.inverse cannot serve: it is overwritten by whichever donor installed first, so
		// measuring a later candidate against it says only that the two disagree, never which is
		// closer to the world origin. installed_translation is the installed donor's own |t|
		// against this reference, so "is the candidate better" is one comparison.
		// Negative installed_translation = no usable reference this frame; no upgrade can fire.
		remix_rsx::mat4 frame_ref_inverse{};
		bool frame_ref_valid = false;
		u64 frame_ref_frame = umax;
		f32 installed_translation = -1.f;

		bool parked_valid = false;
		u64 parked_frame = umax;
		u64 parked_vp_hash = 0;
		remix_rsx::mat4 parked_fused{};
		f32 parked_fold_scale_z = 0.f;
		f32 parked_fold_offset_z = 0.f;
	};

	// Which reference divided a draw. Carried on the pick record so any surviving wobble arrives
	// pre-attributed: round 2's fix fired and the symptom stayed, and nothing in that run said
	// which of the two gauges had placed the object the user was looking at.
	enum class ref_source : u8
	{
		camera = 0,      // the elected camera's reference inverse - the cross-pass gauge
		anchor,          // this frame's anchor for the draw's own render source
		anchor_prev,     // the previous frame's anchor for that source
		viewmodel,       // the viewmodel camera's reference inverse
		identity_bypass, // WORLDIDENTITYVP: the transform was replaced by the identity outright
	};

	static const char* ref_source_name(ref_source source)
	{
		switch (source)
		{
		case ref_source::camera:          return "camera";
		case ref_source::anchor:          return "anchor";
		case ref_source::anchor_prev:     return "anchor_prev";
		case ref_source::viewmodel:       return "viewmodel";
		case ref_source::identity_bypass: return "identity-bypass";
		}

		return "?";
	}

	// Stage A: a hardcoded lit triangle that proves init / camera / present independently
	// of anything the RSX produces. Still the fallback whenever no camera resolves.
	bool create_debug_scene();
	// with_triangle=false submits the hardcoded camera and no geometry, i.e. an empty frame.
	// That is what a camera-less frame needs once the title has ever resolved one: skipping
	// SetupCamera entirely leaves the runtime presenting its last frame, which reads as a freeze.
	void submit_debug_scene(bool with_triangle = true);

	// Camera derived from the title's own transform constants, or the stage A fallback.
	void submit_camera();
	void update_camera_candidate();
	void consider_camera_candidate(const camera_candidate& candidate, bool reserved_attempt = false);

	// Recreates the debug sphere light at 'position' so a derived camera can be judged
	// visually at all. Extracting the title's own lights is out of scope for this milestone.
	void place_debug_light(const f32 (&position)[3]);

	// The scene's default readable light: one distant sun, created once and drawn every frame.
	// RSX has no fixed-function light state to read - PS3 titles light in fragment-program
	// constants with per-title semantics - so there is nothing engine-agnostic to extract.
	// False when the light could not be created; the caller then just has no sun.
	bool ensure_sun_light();
	void reap_idle_guest_lights();
	// ROUND 48: is_viewmodel is the caller's own per-draw viewmodel verdict (the local at the
	// submit site, not m_scratch_defer_is_viewmodel - that one is gated on has_vm_op and reads
	// false at VIEWMODELCAM 0/1, which would make this gate silently inert at two reachable knob
	// values). It refuses the AUTO trigger only; an explicitly listed GUESTLIGHTALBEDO is the
	// user's own instruction and is left alone.
	void maybe_inject_guest_light(u64 vp_hash, u64 fp_hash, u64 albedo_hash,
		const remixapi_Transform& transform, bool is_viewmodel);
	void publish_nectar_disruption();

	// Remix instance categories for one draw, from the albedo hash lists. Replaces the
	// hardcoded categoryFlags = 0: the rtx.*Textures conf lists never reach an API draw.
	u32 classify_draw(u64 albedo_hash);
	// Called from both skinning paths - 'path' records which one, since a rig appearing on one and
	// not the other is itself the finding.
	void dump_bone_palette(u32 vertex_count, const char* path);

	// True when this draw is 2D / pre-projected and must not reach Remix.
	bool is_screen_space_draw() const;

	// Which fragment texture units may be used as albedo for this draw, and whether the answer
	// came from the fragment ucode. Non-null 'from_ucode' reports true only when the program named
	// a colour source *and* excluded at least one other referenced unit - i.e. it discriminated.
	// A program that reaches COL0 from every unit it samples has told the caller nothing, so it is
	// reported as a guess and the retry loop stays shut.
	//
	// ROUND 43b: 'from_narrow' reports that colour_mask saturated and the answer came from
	// dropping units that cannot carry RGB (fp_fingerprint::narrow_sample_mask). It does NOT
	// imply from_ucode and must not be made to - see the note at the return site for why the
	// retry policy is deliberately left alone.
	u32 albedo_unit_mask(bool* from_ucode = nullptr, bool* from_narrow = nullptr) const;

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
	//
	// ROUND 36. centre_anchor is |AABB centre - eye|, printed as canchor= BESIDE the existing
	// anchor= (which is |translation - eye|), so the quantity the new SKYANCHORMODE compares and
	// the quantity the old test compared are visible on the SAME row at every mode. That is what
	// makes the switch auditable from one run instead of two.
	void report_sky_census(sky_outcome outcome, u32 vertex_count, bool depth_write,
		const f32 (&lo)[3], const f32 (&hi)[3], const remixapi_Transform& transform,
		f32 world_extent, f32 anchor, f32 centre_anchor, bool measured, bool camera_inside,
		f32 units_per_vertex, u64 albedo_hash);

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

	// --- ROUND 39: the dome CLASSIFIER (remix_rsx::sky_classify_mode) -------------------------
	// Full derivation, every measured threshold and the pre-registered refutation are on
	// sky_classify_mode() in RemixTransforms.h. This is the per-hash bookkeeping only.
	//
	// Deliberately a SECOND table beside m_sky_hash_seen rather than a mode on it. The two rules
	// disagree on the two hashes we have ground truth for - sky_hash rejects D1A6D1B27ADE6232 and
	// CDFE11B12552EA2D because its units-per-vertex floor is 100 and their own bands measure 86.68
	// and 73.10 - so sharing a table would make the disagreement invisible and would let one rule's
	// disqualification silence the other's. Two tables, two censuses, two sets of counters, and the
	// A/B between them is readable in one run.
	struct sky_classify_entry
	{
		u32 dome = 0;
		u32 other = 0;

		// The frame this hash was first seen at all. The settle window is measured from here, not
		// from the first dome-shaped draw: the risk being guarded against is a texture that is
		// shared with world geometry, and the shared draw can arrive before or after the dome one.
		u64 first_frame = 0;

		// The program that first drew it dome-shaped, and the shape of that draw, so the census can
		// be checked against what is on screen without a second lookup.
		u64 vp = 0;
		f32 extent = 0.f;
		f32 units_per_vertex = 0.f;
		u32 vertices = 0;

		// Latched once. 'promoted' is the state that matters; 'reported' only bounds the log.
		bool promoted = false;
		bool reported_armed = false;
		bool reported_mixed = false;
	};

	// One line per hash per transition. Prints whether the hash was ALREADY on
	// RPCS3_REMIX_SKYEMISSIVE, which is the ground-truth check: D1A6D1B27ADE6232 and
	// CDFE11B12552EA2D must both appear with listed=1 armed=1 or the upv floor is still wrong.
	// 'listed' MUST be sampled by the caller BEFORE the promotion runs. Re-querying
	// sky_emissive_albedo_matches() inside this function would read 1 for every armed hash at
	// mode 2, because promotion has by then inserted it - the field would say "the rule agreed
	// with the user" about hashes the rule found on its own, which is the exact opposite of what
	// it is for. Caught before this shipped; the parameter exists so it cannot come back.
	void report_sky_classify_census(u64 albedo_hash, const sky_classify_entry& entry,
		const char* verdict, f32 world_extent, f32 units_per_vertex, u32 vertex_count,
		bool camera_inside, bool depth_write, bool listed, bool armed);

	// Same 4096 headroom as m_sky_hash_seen and for the same reason - only hashes that have been
	// dome-shaped at least once are ever inserted.
	static constexpr usz s_max_sky_classify_tracked = 4096;

	std::unordered_map<u64, sky_classify_entry> m_sky_classify_seen;

	// ROUND 39. TWO budgets, not one. 'reject:mixed' fires on the first disqualifying draw of any
	// tracked hash while 'ARMED' needs eight dome draws plus a settle window, so a single shared
	// ceiling is exhausted by rejects before one ARMED line exists - and the ARMED line is the
	// deliverable, because it is the only place the camera position is printed and therefore the
	// only way to attribute a dome hash to a level. m_sky_classify_census_lines is kept as the
	// total for the stats field; the two below are what actually gate.
	u32 m_sky_classify_census_lines = 0;
	u32 m_sky_classify_armed_lines = 0;
	u32 m_sky_classify_reject_lines = 0;
	static constexpr u32 s_max_sky_classify_armed_lines = 48;
	static constexpr u32 s_max_sky_classify_reject_lines = 32;

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
	// RPCS3_REMIX_VIEWMODELVP short-circuits it: a listed program is viewmodel geometry whatever
	// its depth range says, which is the only discriminator left on a title that reports the same
	// scale_z/offset_z for the sky dome and the player's hands. The depth path below is untouched,
	// so a title where the rule works keeps working with the list unset.
	viewmodel_outcome classify_viewmodel_depth(f32& scale_z, f32& offset_z) const;

	// Whether the current draw's vertex program is on the VIEWMODELVP list. Separate from the
	// classifier so the two sites that also own an instance transform can apply the anchor guard
	// to hash-tagged draws only, and leave the depth-tagged population exactly as it was.
	bool viewmodel_hash_tagged() const;

	// True when a hash-tagged draw's world placement puts it further than VIEWMODELANCHOR from
	// the eye, i.e. this is the world geometry the listed program also draws. Counts
	// viewmodel_hash_anchor_refused when it refuses.
	//
	// ROUND 30: 'eye' is a PARAMETER, and it has to be. The doc line this replaces said 'position' is
	// "the draw's translation under the *world* camera reference, which is the only space the eye
	// position is expressed in" - and that was the bug, stated as an assumption. There are two callers
	// and they measure 'position' in two different frames: per_draw_transform probes
	// `fused * m_active_camera.reference_inverse` (the elected camera's frame, self-consistent with
	// m_active_camera.position because the two are written together), while submit_subdraw measures the
	// SUBMITTED transform, `fused * anchor->inverse`, which is in the gauge anchor's frame. Each caller
	// now names the eye it means. See RemixTransforms.h at camera_anchor_eye_enabled().
	bool viewmodel_anchor_rejects(const f32 (&position)[3], const f32 (&eye)[3]);

	// Round 23. The PAIR route's own eye-distance ceiling (RPCS3_REMIX_VMPAIRMAXDIST). Separate
	// from viewmodel_anchor_rejects so the two bounds stay readable apart and so the pair route can
	// be tightened without moving VIEWMODELANCHOR, which every other route shares. Increments
	// vm_pair_far itself, which is why it is not const. 'eye' for the reason above.
	bool viewmodel_pair_rejects(const f32 (&position)[3], const f32 (&eye)[3]);

	// One line per (vertex program, outcome) for the reference decision, once, under its own
	// ceiling. Separate from report_viewmodel_census because that one names what the depth rule
	// selects and this one names what the selection was then transformed by; a run where those two
	// disagree is the thing worth reading.
	void report_viewmodel_camera_census(const char* outcome_name);

	std::unordered_set<u64> m_viewmodel_camera_census_seen;
	u32 m_viewmodel_camera_census_lines = 0;
	static constexpr u32 s_max_viewmodel_camera_census_lines = 16;

	// --- round 19: the viewmodel basis correction and its census ----------------------------
	//
	// apply_viewmodel_basis rewrites the instance transform of a VIEW_MODEL-tagged draw with a
	// sign flip on a chosen pair of the camera's own axes, about the eye. It is a pure
	// reflection/rotation built from the orthonormal columns of the row-vector worldToView, so a
	// wrong RPCS3_REMIX_VMBASIS cannot rescale or shear the viewmodel - only reorient it.
	//
	// report_viewmodel_basis_census prints the recovered matrix before and after, the camera's
	// three world-space axes, and the object origin resolved onto those axes as
	// right/up/forward metres. That last triple is the measurement nineteen rounds have been
	// missing: its sign pattern names the wrong axes directly instead of by eye.
	//
	// Deduped per (vp, albedo) since round 20 and bounded by RPCS3_REMIX_VMBASISMAX, like every
	// census here.
	//
	// ROUND 34. The pivot is now selectable (RPCS3_REMIX_VMBASISPIVOT) because the eye pivot was
	// measured displacing the viewmodel 2537..2564 units on all six lines this census emitted on
	// 2026-08-17 - the sentence above about "only reorient it" is true of the 3x3 and false of the
	// whole affine map whenever the transform's translation is not the eye, which on this title it
	// never is. apply_viewmodel_basis reports the pivot it chose back to the caller rather than
	// storing it, because it is const and the one caller passes it straight into the census.
	//   pivot_source: 0 = eye, 1 = geometry centroid, 2 = transform translation,
	//                 3 = centroid requested but unmeasurable, fell back to the eye.
	void apply_viewmodel_basis(remixapi_Transform& transform, f32 (&pivot_out)[3],
		u32& pivot_source) const;

	// ROUND 36. The sign mask above cannot express the fix: MEASURED over 287 'Remix vmbasis:'
	// lines, every near-eye VIEW_MODEL-tagged draw is submitted BEHIND the eye (fwd -0.06..-0.81 in
	// the camera's own left-handed frame, against fwd +0.08..+0.48 for the one near-eye group that
	// is not tagged), with its basis a 118..179 degree rotation about the camera's RIGHT axis. One
	// operator explains both - a 180 degree rotation about that axis THROUGH THE EYE - and a mask
	// pivoted at the centroid can only undo the turning half, which is why VMBASIS=6 came out
	// "facing the right way, just upside down". Axis+angle, camera space or model space, own pivot.
	// Runs after apply_viewmodel_basis and is independent of it. Derivation, clamps and the
	// pre-registered readings: viewmodel_rotate_axis() in RemixTransforms.h.
	//   pivot_source: 0 = raw eye, 1 = centroid, 2 = translation, 3 = anchor-frame eye,
	//                 4 = centroid unmeasurable and fell back to the raw eye, 5 = model space,
	//                 6 = camera invalid (did not run), 7 = knob off (did not run).
	void apply_viewmodel_rotation(remixapi_Transform& transform, f32 (&pivot_out)[3],
		u32& pivot_source) const;

	// ROUND 37. The second half of the operator, called from the tail of apply_viewmodel_rotation
	// when RPCS3_REMIX_VMROTLOCK is armed, about the SAME pivot so it can only turn the instance and
	// never move it. cam_axis is the camera's three world-space axes, already normalised, passed in
	// rather than re-extracted so the two halves cannot drift apart under a later edit.
	//   mode 1 = replace the object 3x3 with the camera's basis (column lengths preserved)
	//   mode 2 = align only the object's X column to the camera's right axis, minimal rotation
	// Derivation and why relpost=0 afterwards is NOT the evidence: viewmodel_rotate_lock() in
	// RemixTransforms.h.
	void apply_viewmodel_lock(remixapi_Transform& transform, const f32 (&cam_axis)[3][3],
		const f32 (&pivot)[3], u32 mode) const;

	// ROUND 36. 'mid' is the transform BETWEEN the two operators - after apply_viewmodel_basis and
	// before apply_viewmodel_rotation - so dbasis= keeps its round-34 meaning (what the flip did)
	// and drot= is the new operator's own delta. Passing only (pre, post) would have silently
	// re-pointed dbasis at the sum of both.
	void report_viewmodel_basis_census(const remixapi_Transform& pre,
		const remixapi_Transform& mid, const remixapi_Transform& post, u32 vertex_count,
		const f32 (&pivot)[3], u32 pivot_source, const f32 (&rot_pivot)[3], u32 rot_pivot_source);

	std::unordered_set<u64> m_viewmodel_basis_census_seen;
	u32 m_viewmodel_basis_census_lines = 0;

	// round 21: 'Remix alphastate:' - one line per distinct albedo, printing exactly what this
	// backend shipped in remixapi_InstanceInfoBlendEXT for that albedo alongside the verdict
	// calculateAlphaState will derive from it. See the doc block on alpha_state_census_max() in
	// RemixTransforms.h for why the existing instruments could not answer this. Off unless
	// RPCS3_REMIX_ALPHACENSUS is armed.
	void report_alpha_state_census(u64 albedo_hash, const remixapi_InstanceInfoBlendEXT& blend,
		u32 category_flags, u32 vertex_count);
	std::unordered_set<u64> m_alpha_state_census_seen;
	u32 m_alpha_state_census_lines = 0;

	// --- round 13: the sky dome census ------------------------------------------------------
	// One line per (vp, albedo) that clears the sky test, naming the dome and whether the
	// RPCS3_REMIX_SKYEMISSIVE material was actually created for it. Bounded and deduped like
	// every census above; gated on RPCS3_REMIX_DIAGLINES, NOT on dump_enabled() - the launcher
	// ships DUMP=0 and that gate is what silenced all four of round 11's diagnostics.
	void report_sky_emissive_census(u64 albedo_hash, u32 vertex_count, f32 extent, bool tagged_sky);
	std::unordered_set<u64> m_sky_emissive_census_seen;
	u32 m_sky_emissive_census_lines = 0;
	static constexpr u32 s_max_sky_emissive_census_lines = 16;

	// --- round 13: the first-person weapon census -------------------------------------------
	// RPCS3_REMIX_FPCENSUSVP names the program; this reports one line per (vp, albedo) it draws,
	// with the discriminator the next round needs - the distance from the EYE to the draw's
	// GEOMETRY centre, not to its instance origin. vp=830d7d1b9681c475 draws both the player's
	// weapon and whole NPC soldiers, so extent alone cannot separate them and a hash tagged by
	// guess would stick an NPC's rifle to the player's face. Census only: nothing is tagged.
	void report_fp_candidate_census(u64 albedo_hash, u32 vertex_count, f32 extent,
		const remixapi_Transform& transform);
	std::unordered_set<u64> m_fp_candidate_census_seen;
	u32 m_fp_candidate_census_lines = 0;
	// Round 17. Which stats window the seen-set and the line budget were last armed for, under
	// RPCS3_REMIX_FPCENSUSMAXDIST. umax rather than 0 because frame 0 is a real window and must not
	// compare equal to "never armed". Unused while the knob is 0, which is its default.
	u64 m_fp_candidate_census_window = umax;
	static constexpr u32 s_max_fp_candidate_census_lines = 48;

	// Fills m_scratch_vertices' texcoords from the vertex attribute that feeds the albedo unit.
	// Must run before the mesh content hash is taken: the texcoords are part of the vertex data
	// the hash covers, and two draws that share positions but not UVs are different meshes.
	void apply_texcoords(u32 unit, const remix_rsx::texture_entry& entry,
		const rsx::fragment_texture& tex, u32 first_vertex, u32 vertex_count);

	// Fills m_scratch_vertices' colours from ATTR3 for draws that resolved no material, so
	// vertex-coloured geometry stops reaching Remix as flat white. Like apply_texcoords it must
	// run before the mesh content hash, which covers the colour.
	// ROUND 41: `textured` is the caller's `material != nullptr`. Needed by the constant-route
	// black guard - modulating an albedo by a constant black deletes it, while flooding an
	// UNTEXTURED draw with the same constant is the round-12 behaviour that has always been right.
	void apply_vertex_colour(u32 first_vertex, u32 vertex_count, bool textured);

	// The textured counterpart, for alpha only. Writes ATTR3's alpha into m_scratch_vertices'
	// colour alpha and leaves RGB white, so no tint is guessed at - only the channel the draw
	// provably cannot get from its texture. True means the alpha varies and Remix should be
	// told to read VertexColor0; false leaves the draw exactly as it was.
	bool apply_vertex_alpha(u32 first_vertex, u32 vertex_count);

	// Alpha range of the albedo texture bound for the current draw, and whether
	// apply_vertex_alpha found a usable ATTR3 alpha to substitute for it.
	u8 m_scratch_albedo_alpha_min = 255;
	u8 m_scratch_albedo_alpha_max = 0;
	bool m_scratch_vertex_alpha = false;

	// Mean RGB of that same texture, in 0..1. Carried on the draw for the same reason the alpha
	// range is: the guest-light injection site runs after DrawInstance, by which time the texture
	// entry pointer is out of scope. White until a bind fills it, so an untextured draw tints
	// nothing.
	f32 m_scratch_albedo_mean_rgb[3] = { 1.f, 1.f, 1.f };

	// Round 23. The bound texture's brightest-region UV, carried on the draw for exactly the reason
	// the mean RGB above is: the sky-sun derivation runs where the decoded vertices live, by which
	// time the texture entry pointer is gone. Negative until a bind fills it, and only ever filled
	// for a texture on RPCS3_REMIX_SKYEMISSIVE - so "non-negative" IS the "this draw is a listed sky
	// dome" test, and costs no hash scan per draw.
	f32 m_scratch_albedo_peak_uv[2] = { -1.f, -1.f };

	// Round 23 defect fix. Did apply_texcoords actually WRITE this draw's texcoords? It has six
	// early returns (uv_none / uv_absent / uv_layout / uv_memory), and m_scratch_vertices.resize()
	// value-initialises texcoord to {0,0}, so a draw that took one of them still presents a
	// perfectly finite (0,0) texcoord to any reader. The sky-sun derivation solves ONCE per dome
	// albedo and never revises it, so consuming those zeros would latch a wrong sun for the whole
	// session. Set beside each of the three ++m_stats.uv_applied sites, which are exactly the
	// points at which real texcoords have just been written.
	bool m_scratch_uv_applied = false;

	// The two alpha-to-coverage bits, resolved once per draw beside the KIL channels above and read
	// again at the blend-extension fill, the kil census and the pick record. a2c_ctrl is the
	// fragment shader control word's RSX_SHADER_CONTROL_ALPHA_TO_COVERAGE; a2c_reg is
	// NV4097_SET_ANTI_ALIASING_CONTROL's own bit. Either alone is enough for the guest to get a
	// cutout on real hardware, which is why they are ORed at the replay and reported apart here.
	bool m_scratch_a2c_ctrl = false;
	bool m_scratch_a2c_reg = false;
	bool m_scratch_a2c_applied = false;

	// Whether the draw the lightpass census is about bound a material. Carried rather than
	// re-derived because report_light_pass is called from the middle of the albedo walk's caller,
	// where the local is in scope but the method is not.
	bool m_scratch_light_pass_material = false;

	// 'Remix lightpass:' state. One line per (vp, fp) per stats window, bounded, so a pass that only
	// appears in one room still reports and a pass that draws every frame does not flood.
	void report_light_pass();
	static constexpr u32 s_max_light_pass_lines = 64;
	u64 m_light_pass_window = umax;
	u32 m_light_pass_lines = 0;
	std::unordered_set<u64> m_light_pass_seen;

	// The per-pixel discard state for the draw under consideration, resolved once at the albedo
	// bind and read again at the blend-extension fill and at the pick record. The texture-control
	// bit is a property of the *winning* unit, so it cannot be answered before that walk finishes,
	// and re-reading the register later would answer for whichever unit happened to be last.
	bool m_scratch_texkill = false;
	bool m_scratch_kil_applied = false;
	s16 m_scratch_kil_ref = -1;

	// --- round 10: the effects verdicts, decided once per draw --------------------------------
	// The submitted vertex colour, measured once and reused. Three consumers need it in the same
	// draw (the self-lit material attach, the additive blend arm and the effect census) and the
	// measurement is an O(vertices) walk, so it is computed lazily on first ask and reset per
	// draw rather than recomputed at each site.
	bool m_scratch_vcol_measured = false;
	bool m_scratch_vcol_varies = false;
	u8 m_scratch_vcol_alpha_min = 255;
	u8 m_scratch_vcol_alpha_max = 0;
	// Round 11: the first submitted vertex's packed rgb, retained by the same walk. The gauge
	// question - is the data really (0,0,0,255), or is map_attribute/decode_position misreading
	// this layout - cannot be answered from 'rgb-varies' alone, and one number per census row
	// answers it passively instead of costing a click.
	u32 m_scratch_vcol_first = 0;
	void measure_submitted_vertex_colour();

	// Round 11: the submitted geometry's largest axis extent, measured lazily by the same rule and
	// reused by the self-lit extent gate and the effect census - ONE site of truth, so a census row
	// can never print an extent the verdict did not use. (submit_draw's own sky_lo/sky_hi walk runs
	// LATER in the same function and answers a different question - the bbox in the sky rule's
	// coordinates - so it is deliberately not shared.)
	bool m_scratch_extent_measured = false;
	f32 m_scratch_extent = 0.f;
	f32 measure_submitted_extent();

	// Round 13. The sky rule's own anchor - the distance from the eye to the draw's instance origin
	// - carried out of the sky block so the 'Remix skyemissive:' line can print the number the sky
	// verdict was actually made on rather than recomputing it and risking a different answer. -1
	// means the sky block never measured this draw (it was not a candidate, or world/camera was
	// unresolved), which must not read as "the draw is on the eye".
	f32 m_scratch_sky_anchor = -1.f;

	// The self-illuminated verdict for this draw (RPCS3_REMIX_FPVCOLEMISSIVE) and its additive
	// refinement (RPCS3_REMIX_FPVCOLADDITIVE). Decided once, immediately after the vertex colour
	// is applied, so the material attach, the blend fill and the census cannot disagree about
	// what the draw is.
	bool m_scratch_fp_selflit = false;
	bool m_scratch_fp_additive = false;

	// --- round 10: the viewmodel verdict, decided once per draw -------------------------------
	// Today the same draw can be measured by two guards at two different positions - the divide
	// site probes fused x reference_inverse (whose translation row is the LOCAL ORIGIN) and the
	// submit site probes the instance transform's translation - and they disagreed 100% of the
	// time (vmcam_applied = 54389 = vm_hash_anchor_refused, vm_tagged_hash = 0). Classified once
	// at the divide site, both consumers read this, and a refused verdict now ALSO keeps the draw
	// off the viewmodel-camera divide instead of dividing by a reference it is not tagged for.
	enum class vm_verdict : u8
	{
		none = 0,    // not selected by the depth rule, or by neither list
		tagged,      // listed and within the anchor limit
		refused,     // listed but the anchor guard turned it away
	};

	vm_verdict m_scratch_vm_verdict = vm_verdict::none;
	// The latch that makes "once" literal. The divide site classifies first when it reaches its
	// reference branch; a draw that takes another branch is classified at the submit site instead.
	// Exactly one of the two runs per draw, which is what makes the counters partition.
	bool m_scratch_vm_classified = false;
	// Which route selected it, so vm_tagged_hash and vm_tagged_albedo can be counted apart at the
	// submit site from a verdict that was reached at the divide site.
	bool m_scratch_vm_by_hash = false;
	bool m_scratch_vm_by_albedo = false;
	// Round 20's (vp, albedo) pair route. Third and last in the attribution chain, so it only ever
	// claims draws neither older route did.
	bool m_scratch_vm_by_pair = false;
	// The albedo hash resolved for this draw, published before per_draw_transform runs so the
	// verdict site can consult the albedo list. 0 when the draw resolved no material.
	u64 m_scratch_vm_albedo = 0;

	// The draw's local AABB centre, transformed by a caller-supplied matrix, for the anchor
	// guard. The same computation maybe_inject_guest_light performs; 'row_vector' selects the
	// convention (per_draw_transform's mat4 carries its translation in row 3, remixapi_Transform
	// in column 3). Returns false when no vertices are decoded yet, which is the vm_anchor_
	// unmeasured case.
	bool geometry_centre_in(const remix_rsx::mat4& matrix, f32 (&out)[3]) const;
	bool geometry_centre_in(const remixapi_Transform& transform, f32 (&out)[3]) const;

	// Why the albedo walk produced no material, and what the unit it gave up on looked like.
	// Carried on the draw because report_no_material's signature (one bool) cannot express five
	// different failures, and because the walk's local state is gone by the time it is called.
	const char* m_scratch_notex_class = "";
	const char* m_scratch_notex_reason = "";
	s8 m_scratch_notex_unit = -1;
	u32 m_scratch_notex_format = 0;
	u32 m_scratch_notex_width = 0;
	u32 m_scratch_notex_height = 0;
	// Round 6. Which of the two untextured sub-populations this draw belongs to, decided at the
	// walk where the formats are already in hand rather than re-read later: shadowonly = every
	// eligible unit the walk could see carries a DEPTH format; colorunit = the fragment program's
	// sampled_mask names some other eligible unit whose bound texture has a colour format. Both can
	// be false (a colour unit exists but is not sampled); they cannot both be true.
	bool m_scratch_notex_shadowonly = false;
	bool m_scratch_notex_colorunit = false;

	// Round 6. What the tail-rescue ladder did for the draw under consideration, printed on the
	// world-refused census's rescue= field: "off" (knob off or the draw never took the ladder),
	// "n/a" (refused before the affinity gate), "cur"/"aged" (rescued) or "failed".
	const char* m_scratch_rescue = "n/a";

	// Round 19. Extra fields appended to a single 'Remix tail-rescue:' line, set immediately before
	// the report call on the proj_split accepting path and cleared immediately after. Empty on every
	// other outcome, so the three pre-existing call sites keep byte-identical output and every
	// existing grep over this census still matches.
	//
	// It carries world= (the matrix the draw was actually placed with, which round 18's fxref line
	// could no longer reach once proj_split started accepting) and vdelta= (how far the anchor's view
	// is from the draw's own - the premise test the vacuous is_affine gate never performed).
	std::string m_scratch_projsplit_note;

	// Decoded ATTR3 alpha for the draw under consideration. Held across the two passes
	// apply_vertex_alpha makes - measure the range, then commit only if it varies - so the
	// attribute is walked once and nothing is written until the draw is known to qualify.
	std::vector<u8> m_scratch_alpha;

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
	bool track_bone_offset(const remix_rsx::vp_fingerprint& fp, u32 slot);
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

	struct guest_light_entry
	{
		remixapi_LightHandle handle = nullptr;
		f32 position[3]{};

		// Stamped on every dedup hit, i.e. every frame a draw of the same fixture lands near this
		// light. RPCS3_REMIX_GUESTLIGHTIDLE frames without one and the light is destroyed and its
		// slot freed. Default 0 = never, so the light survives the player looking away, which is
		// the behaviour a room lit by its own ceiling fixtures needs.
		u64 last_matched_frame = 0;
	};

	std::vector<guest_light_entry> m_guest_lights;

	// --- ROUND 48: the static-fixture staging table --------------------------------------------
	// Keyed by the SAME quantised-position hash the light itself uses, so a cell and its light are
	// the same identity by construction and cannot drift apart.
	//
	// 'confirmed' is deliberately sticky and is the flicker half of the feature. Once a cell has
	// proved itself static, a bulb that flickers off - loses its card, loses its light to
	// GUESTLIGHTIDLE - and flickers back on re-lights on the first frame its card returns. Without
	// stickiness any stability window longer than the flicker period would hold the bulb dark
	// forever, which is precisely the symptom the feature exists to fix.
	struct guest_light_cell
	{
		// Distinct frames this cell has been seen on. Saturates at the knob value; it is a
		// graduation counter, not a census.
		u32 frames_seen = 0;
		// Guards the once-per-frame increment. A fixture drawn three times in one frame is one
		// frame of evidence, not three - otherwise a single multi-pass draw confirms instantly and
		// the gate measures draw count instead of time.
		u64 last_frame = umax;
		bool confirmed = false;
	};

	std::unordered_map<u64, guest_light_cell> m_guest_light_cells;

	// ROUND 50: the MOTION test, keyed on the SOURCE rather than on a position.
	//
	// guest_light_cell above answers "has this spot been occupied a while", which an idle NPC
	// satisfies as readily as a lamp. This answers "has this emitter ever been anywhere else",
	// which only something with legs can fail. Measured on Haze: every real fixture occupies
	// exactly ONE quantised cell for a whole scene, the suit glow card occupied 26.
	//
	// Keyed on the (albedo, vp, fp) triple, NOT on albedo alone: this title binds one albedo to
	// eight distinct (vp, fp) pairs, one of them the smoke program, so an albedo-only key would
	// disqualify a fixture because something unrelated sharing its texture moved.
	struct guest_light_source
	{
		// Distinct cells this source has occupied. Dropped once disqualified -- at that point the
		// set has served its purpose and only the verdict matters, so a source that wanders the
		// whole level costs one bool rather than one entry per 0.25 units it travelled.
		std::unordered_set<u64> cells;
		bool disqualified = false;
		// Once-per-frame guard, for the same reason guest_light_cell has one: a fixture drawn three
		// times in one frame is one observation, not three.
		u64 last_frame = umax;
	};

	std::unordered_map<u64, guest_light_source> m_guest_light_sources;
	u64 m_guest_light_attempt_frame = umax;
	u32 m_guest_light_attempts = 0;

	// 'Remix guest-light:' is mirrored to remix_dump.log, bounded like every other census line
	// here. Round 4 created four bulbs in the exact room the user called too dark and wrote them
	// to rsx_log only, where nobody could read them until the emulator exited.
	static constexpr u32 s_max_guest_light_lines = 128;

	// --- ROUND 48: the staging table's ceiling and its quiet window ------------------------------
	// The table is keyed on a quantised world position and the population it exists to REJECT is
	// the one that mints a new key every frame, so it must be bounded or a walking NPC grows it for
	// the whole session. 4096 cells is ~40x the GUESTLIGHTMAX of 128 and comfortably covers a level
	// of real fixtures plus the transient trails in flight at any moment.
	//
	// The quiet window is what the prune retires: an UNCONFIRMED cell not seen for this many frames
	// is a trail, not a fixture. It is deliberately much longer than any plausible
	// GUESTLIGHTSTABLE so a fixture that is drawn intermittently - a bulb seen through a doorway,
	// or one that flickers - accumulates its evidence across the gaps instead of being reset by
	// them. Confirmed cells are never pruned at all.
	static constexpr u32 s_max_guest_light_cells = 4096;
	static constexpr u64 s_guest_light_cell_quiet = 900;
	u32 m_guest_light_lines = 0;

	bool m_nectar_disruption_seen = false;

	// --- pre-anchor deferral (RPCS3_REMIX_DEFERPREANCHOR) ---------------------------------------
	//
	// A draw whose render source has no *current-frame* gauge anchor is placed by last frame's
	// anchor (gauge_prev, 25% of divides) or by the cross-pass camera (gauge_absent, 20%). Both put
	// the object one frame of camera motion out of date, which is exactly the residual wobble round
	// 4 measured: ~0.5 units of origin drift on a ceiling fixture whose basis was clean to 3e-5.
	//
	// Instead of guessing, the finished instance is held until this frame's anchor for that source
	// arrives, then re-divided by it and submitted. Everything needed to redo the divide travels in
	// the record: the fused matrix, the object-space prepend (captured by running the same lambda on
	// an identity, so it cannot drift from the live one), and the source key. The payload is a
	// by-value copy of the instance and its extension structs, so no pointer into a dead stack frame
	// survives - which is why skinned draws are excluded, their bone array being a pointer to
	// per-draw scratch.
	struct deferred_instance
	{
		remixapi_InstanceInfo info{};
		remixapi_InstanceInfoBlendEXT blend{};
		remixapi_InstanceInfoObjectPickingEXT picking{};
		bool has_blend = false;
		bool has_picking = false;

		// Divide inputs, so the flush recomputes rather than corrects.
		remix_rsx::mat4 fused{};
		remix_rsx::mat4 object_space{};

		// The render-source key this draw is waiting on an anchor for.
		u32 surface_offset = 0;
		u32 color_target = 0;
		u32 clip_width = 0;
		u32 clip_height = 0;

		// For poisoning on a failed submit, exactly as the immediate path does.
		u64 mesh_hash = 0;

		// ROUND 46. The composite world-space operator the submit site applied to a VIEW_MODEL-tagged
		// draw - apply_viewmodel_basis followed by apply_viewmodel_rotation - captured as
		// Op = post * pre^-1 and stored in the SAME 3x4 row layout remixapi_Transform uses, so the
		// replay below is the identical arithmetic those two operators perform and no matrix
		// convention has to be re-derived. The flush recomputes the transform from fused/anchor and
		// would otherwise drop the correction entirely: MEASURED drot = 1.46..1.99 units on all 3,293
		// census lines of the round-45 play-test, so the loss would be a teleport, not a rounding
		// difference. has_vm_op == false means "no operator ran", which is every non-viewmodel draw
		// and any draw whose pre-operator basis would not invert.
		f32 vm_op[3][4]{};
		bool has_vm_op = false;

		// ROUND 47. This draw was placed by the proj_split rescue (V_anchor x P_draw)^-1, not by the
		// plain division, so the flush must rebuild it the same way against the anchor that just
		// landed. Nothing else is stored: the cross is a pure function of 'fused' and the anchor's own
		// 'fused', both of which the flush already holds, so there is no captured matrix here to drift
		// from the construction in the ladder.
		bool used_projsplit = false;
	};

	std::vector<deferred_instance> m_deferred_instances;

	// Set by per_draw_transform when the draw it just placed is a deferral candidate, latched by
	// the submit path immediately after that call. The fused/object-space matrices and the source
	// key travel beside it for the same reason.
	bool m_scratch_defer_pending = false;

	// ROUND 47. Set by the proj_split acceptance branch of the tail-rescue ladder when it placed the
	// draw with a STALE anchor (cross_age != 0) and therefore left the deferral armed. It selects the
	// flush's re-cross path: a proj_split draw failed the plain division by construction - the ladder
	// only runs at that exit - so flushing it through 'fused * anchor.inverse' would hand it back the
	// very residue the rescue removed. False on every other deferred draw.
	bool m_scratch_defer_projsplit = false;

	remix_rsx::mat4 m_scratch_defer_fused{};
	remix_rsx::mat4 m_scratch_defer_object_space{};
	u32 m_scratch_defer_surface = 0;
	u32 m_scratch_defer_target = 0;
	u32 m_scratch_defer_clip_w = 0;
	u32 m_scratch_defer_clip_h = 0;

	// ROUND 46. Same lifetime and the same reason as the fields above, but written LATER in the
	// draw - per_draw_transform cannot know it, because the two viewmodel operators run at the
	// submit site after the tag is decided. Cleared at the top of per_draw_transform so a refused or
	// non-viewmodel draw can never inherit the previous draw's operator.
	f32 m_scratch_defer_vm_op[3][4]{};
	bool m_scratch_defer_vm_op_valid = false;

	// True for any draw that ran the viewmodel operator block, whether or not the operator capture
	// succeeded. The buffering site needs BOTH: '_is_viewmodel && !_vm_op_valid' is the one
	// combination that must never be deferred, because the flush would rebuild the transform and
	// submit the mesh without a correction it cannot reconstruct.
	bool m_scratch_defer_is_viewmodel = false;

	// Submits every buffered instance whose source key matches, re-divided by 'anchor'. Called from
	// capture_gauge_anchor the moment a frame's anchor for that source lands.
	void flush_deferred_for_anchor(const gauge_anchor& anchor);

	// Submits everything still buffered at flip, with the transform each draw already carried from
	// the old prev/camera fallback. Nothing is ever lost, whatever the anchor did.
	void flush_deferred_at_flip();

	// Shared tail: rebuilds the pNext chain from the copied ext structs and calls DrawInstance.
	void submit_deferred(deferred_instance& entry);

	rsx::vertex_input_layout m_vertex_layout{};

	std::unordered_map<u64, mesh_entry> m_meshes;

	// Round 9. Scratch for the reap-safety pass: the material handles live meshes have baked in.
	// A member rather than a local so the allocation survives between reaps - it is rebuilt at
	// most once per TEXIDLE window in steady state, but rebuilding it should not also allocate.
	std::unordered_set<const void*> m_live_materials;
	std::unordered_set<u64> m_poisoned;
	std::unordered_map<u64, static_index_entry> m_static_indices;
	u64 m_static_index_budget_frame = umax;
	u32 m_static_index_rebuilds = 0;
	u64 m_static_index_triangles = 0;
	u64 m_static_index_rebuild_total = 0;
	u64 m_static_index_deferred = 0;
	// --- round 31: the two halves of m_static_index_deferred, which are NOT the same defect -------
	// When the per-frame union-rebuild budget is spent, a draw on a STATICINDEXVP program takes one
	// of THREE exits with TWO outcomes, and until now all three incremented one counter, so a run
	// could not say which:
	//
	//   _stale   - a previously built union mesh handle is still live, so the draw renders THAT
	//              subset. That is a PREVIOUS FRAME'S TRIANGLE SET - geometry only. It converges the
	//              instant the GEOMETRY stops changing while flips CONTINUE - nothing sets
	//              static_entry->dirty, so the rebuild branch is never entered and the
	//              last-known-good union IS the current one. NOT when flips stop: the budget
	//              resets only when the frame counter changes, so if flips stop it stays spent
	//              and the stale copy FREEZES instead. (Corrected by review.)
	//              NOT a material effect: static_key and the union hash both fold in vp, albedo and
	//              the material pointer, so a material change lands on a different entry (mesh_hash
	//              0 -> active_valid false) and drops instead. A stale union always shades the same.
	//   _dropped - no live handle exists (never built, or reaped by MESHIDLE), or the budgeted
	//              fallback found its subset mesh absent from the cache - TWO sites - so the draw RETURNS
	//              and is never submitted. It is invisible, and because it exits before the refusal
	//              accounting it leaves no line in 'Remix world-refused:' and cannot be found there.
	//
	// The sum is m_static_index_deferred exactly, so runs taken before this split stay readable.
	// Measurement only: neither counter changes a decision.
	u64 m_static_index_deferred_stale = 0;
	u64 m_static_index_deferred_dropped = 0;
	u32 m_static_index_rebuild_peak = 0;

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
	struct camera_surface_reject
	{
		u64 count = 0;
		u32 clip_width = 0;
		u32 clip_height = 0;
	};
	std::unordered_map<u64, camera_surface_reject> m_camera_surface_rejects_by_vp;
	u64 m_meshes_created_at_last_flip = 0;

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
	std::unordered_set<u64> m_ui_dumped_contents;

	// Stats window (m_frame_counter / s_stats_interval_flips) each RPCS3_REMIX_WORLDVP probe last
	// printed in. The probe fires per draw, so without this it would write one line per draw call.
	std::unordered_map<u64, u64> m_worldvp_windows;
	std::unordered_set<u64> m_world_identity_trace_keys;
	u64 m_world_identity_trace_window = umax;
	u32 m_world_identity_trace_lines = 0;
	u64 m_meshtrace_window = umax;
	u32 m_meshtrace_lines = 0;
	bool m_world_identity_probe_logged = false;
	bool m_world_identity_logged = false;

	// --- round 22: the identity-override census ---------------------------------------------------
	//
	// 'Remix worldid-draw:' already prints the transform the WORLDIDENTITYVP override is about to
	// discard, but its dedup key hashes the translation and basis BUCKETS along with vp/albedo/surf/
	// clip/vtx, so a window's lines cannot be summed into "how many draws were displaced". These
	// counters can be, and they are run-cumulative rather than per-window so the last emitted line is
	// the run total. Bounded by construction: at most s_world_identity_census_slots programs, one
	// linear scan of that array per identity-forced draw, no allocation.
	struct world_identity_census_slot
	{
		u64 vp_hash = 0;
		u64 draws = 0;
		u64 kept = 0;
		// Discarded |translation|, bucketed <=1 / <=32 / <=128 / >128.
		u64 t_buckets[4] = {};
		// Discarded 3x3 deviation, bucketed <=0.02 / <=0.1 / >0.1.
		u64 b_buckets[3] = {};
		f32 t_max = 0.f;
		f32 b_max = 0.f;
	};

	static constexpr u32 s_world_identity_census_slots = 12;
	world_identity_census_slot m_world_identity_census[s_world_identity_census_slots]{};
	u32 m_world_identity_census_used = 0;
	u64 m_world_identity_census_overflow = 0;

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
	// Projection score alone cannot distinguish a gameplay camera from a passenger, reflection,
	// or cinematic pass with the same lens. Keep a bounded census and let the camera supporting
	// the most 3D draws win; score remains the tie-breaker for one-off candidates.
	static constexpr u32 s_max_camera_candidates_per_frame = 32;
	std::array<camera_candidate, s_max_camera_candidates_per_frame> m_frame_camera_candidates{};
	u32 m_frame_camera_candidates_used = 0;
	// With a camera VP lock active, keep a separate bounded census of candidates rejected by the
	// lock. It is emitted only under CAMTRACE on a held-camera frame, so a missing gameplay source
	// can be identified without allowing every passenger/reflection camera back into selection.
	std::array<camera_candidate, s_max_camera_candidates_per_frame> m_frame_rejected_camera_candidates{};
	u32 m_frame_rejected_camera_candidates_used = 0;
	u32 m_frame_rejected_camera_candidates_overflow = 0;
	bool m_frame_camera_lock_seen = false;
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
	// How apply_texcoords arrived at the divisor it used, reported on the pick line. 'none' is
	// the honest answer for every format that needs no divisor at all, which is most of them.
	enum class uv_scale_source : u8
	{
		none,
		unnormalised,  // CELL_GCM_TEXTURE_UN: 1/width, 1/height
		ucode,         // the constant slot the program itself multiplies by
		fixed,         // RPCS3_REMIX_UVINTSCALE, i.e. the guess
		affine_hash,   // RPCS3_REMIX_UVAFFINEVP, the per-hash replay
		affine_general,// vp_fingerprint::texcoord_affine, the resolved general form
		no_material    // no albedo bound, so apply_texcoords never ran: 'never asked', not 'failed'
	};

	static const char* uv_scale_source_name(uv_scale_source source)
	{
		switch (source)
		{
		case uv_scale_source::none:           return "none";
		case uv_scale_source::unnormalised:   return "unnorm";
		case uv_scale_source::ucode:          return "ucode";
		case uv_scale_source::fixed:          return "fixed";
		case uv_scale_source::affine_hash:    return "affinevp";
		case uv_scale_source::affine_general: return "affineall";
		case uv_scale_source::no_material:    return "notex";
		}

		return "?";
	}

	struct pick_record
	{
		u64 vp_hash = 0;
		u64 fp_hash = 0;
		u64 albedo_hash = 0;
		u64 camera_vp_hash = 0;
		u64 frame = 0;
		u32 vertex_count = 0;
		f32 extent = 0.f;
		f32 origin[3]{};
		f32 basis[3]{};
		// ROUND 43. The 3x3 determinant of the same transform. basis[] above is three row NORMS
		// and therefore cannot distinguish an identity from a reflection; this can. Negative means
		// the instance was submitted mirrored.
		f32 det = 0.f;
		f32 camera_position[3]{};
		// ROUND 30. The eye in origin='s OWN frame - the gauge anchor's - captured at the same submit
		// as camera_position so one pick line carries both and 'Remix picked:' stops being the field
		// that quietly mixed them. Equal to camera_position on the flips where the two frames agree,
		// which is 96.8% of them (anchor_cam_offset=2134 of flips=66508, MEASURED round 30).
		f32 camera_anchor_position[3]{};
		u32 camera_age = 0;
		u32 sampled_mask = 0;
		u16 clip_width = 0;
		u16 clip_height = 0;
		u8 alpha_min = 255;
		u8 alpha_max = 0;
		bool sky = false;
		bool viewmodel = false;
		bool material = false;
		s8 albedo_unit = -1;
		bool depth_test = false;
		bool depth_write = false;
		bool blend = false;

		// --- round 11: what the guest's own ROP state was, per object -------------------------
		// The effect census can only print blend=1/0 for a whole program; which FACTORS a draw
		// asked for is what decides whether a black quad is a darkening layer doing its job or a
		// broken one, and blend_pairs= on the stats line proves those factors reach the
		// translation. alpha_test is the term the flower's invisibility was inferred through by
		// elimination in round 10 and is now read directly.
		bool alpha_test = false;
		u16 blend_src = 0;
		u16 blend_dst = 0;
		u16 blend_op = 0;

		// ATTR3's stored shape and its FIRST decoded value, before any route fold. The two
		// pre-registered readings for the black HUD gauges: 'first=[0 0 0 1]' with a sane
		// place/type/size is real (0,0,0,255) data - a dark depletion layer rendering faithfully,
		// and the amber comes from a layer the dedupe never censused - while a nonsense
		// place/type/stride is a decode misread and the printed shape IS the repro.
		u8 attr3_status = 0;   // attribute_status, cast
		u8 attr3_type = 0;
		u8 attr3_size = 0;
		u16 attr3_stride = 0;
		f32 attr3_first[4] = { 0.f, 0.f, 0.f, 0.f };

		// What the vertex program routes into COL0, carried per object so 'the fragment program
		// reads COL0' and 'the vertex program puts ATTR3 there' can be read apart on one line.
		remix_rsx::vcol_route vcol_route_kind = remix_rsx::vcol_route::none;
		bool vcol_alpha_from_attr = false;

		// The decode verdict for the program that drew it. Carried here rather than looked up
		// afterwards because these are the fields that separate "this mesh is genuinely large"
		// from "this mesh arrived undecoded", and a click on a streak is asking exactly that.
		// affine_reason points into static strings owned by the fingerprint cache.
		u8 archetype = 0;
		bool has_prescale = false;
		bool has_const_affine = false;
		bool skinned = false;
		const char* affine_reason = "";

		// Where this draw's texture coordinates came from, so a click on a tiling surface IS the
		// diagnosis instead of the start of one. 'u=[0..8] scale=fixed4096' confirms the divisor
		// miss outright; 'u=[0..1] scale=ucode(3.05e-05)' exonerates UV scale and moves the
		// question to the material or the blend. Filled from what apply_texcoords actually did,
		// not from the fingerprint, so a program that resolved a scale and then took another
		// path cannot read as if it had used it.
		u8 uv_attribute = 0xff;
		bool uv_from_ucode = false;
		uv_scale_source uv_scale = uv_scale_source::none;
		f32 uv_scale_value = 0.f;
		f32 uv_min[2] = { 0.f, 0.f };
		f32 uv_max[2] = { 0.f, 0.f };

		// Which reference divided this draw, and the identity of that reference. Round 2 shipped a
		// fix that fired and a symptom that stayed, and no line in that run said whether the object
		// the user was watching had been divided by its pass's own anchor or by the cross-pass
		// camera - so a persisting wobble could not be attributed to a population. It can now.
		ref_source reference = ref_source::camera;
		u64 reference_frame = umax;
		u64 reference_vp = 0;
		// The anchor's donor folded a different viewport depth range than this draw uses.
		bool reference_zfold = false;

		// The cutout channels, per object. A click on the leaf wall is meant to BE the diagnosis:
		// kil=1 src=ucode says the ucode discards per pixel; src=texctl says the texture unit does;
		// src=both says both; kil=0 says neither, and then the blend fields on the same line name
		// the population the fix has to move to instead.
		bool kil = false;
		bool kil_conditional = false;
		bool kil_texture = false;
		bool kil_applied = false;
		s16 kil_ref = -1;

		// The third channel: alpha to coverage, per object. a2c=0/0 on the foliage barrier is the
		// negative verdict for that surface, read directly off one click.
		bool a2c_ctrl = false;
		bool a2c_reg = false;
		bool a2c_applied = false;

		// Round 9: the fourth channel, the colour pipeline. Stored as the raw enum value so the
		// pick struct stays POD-ish and the header does not have to order itself against
		// RemixTransforms.h's enum definition. 0 = other, 1 = vcol_pass, 2 = vcol_modulate.
		u8 fp_out_rgb = 0;
		u8 fp_out_alpha = 0;
		bool fp_vcol_replayed = false;

		// --- pick-deep -------------------------------------------------------------------------
		// The vertex-data and ucode reading the brief demands, attached to the user's own clicks so
		// it is per-object and cannot be skipped. attr0's stored shape and decoded w range come
		// from the DUMP-gated census (dump_vertex_program), computed once per program rather than
		// per draw; everything else is live state at the moment of this draw.
		bool deep_valid = false;
		u8 attr0_type = 0xff;
		u8 attr0_size = 0;
		u16 attr0_stride = 0;
		f32 attr0_w_min = 0.f;
		f32 attr0_w_max = 0.f;
		bool has_wdivide = false;
		u16 outer_base = 0;
		u8 group_count = 0;
		// The UV affine form the draw actually replayed, plus the four live components of the slot
		// it divides by. This is the number that decides a divisor question on the picked object.
		s8 uv_unit = -1;
		u16 uv_scale_slot = 0xffff;
		u8 uv_scale_component[2] = { 0, 0 };
		bool uv_has_rows = false;
		f32 uv_scale_slot_value[4] = {};
		// The fused (chain x viewport-z) matrix this draw was divided from, in %a hex on the line:
		// exact, and the only form in which a narrowing artefact could ever be read back off a log.
		f32 fused[16] = {};
	};

	// Census of the distinct (srcColor, dstColor, colorBlendOp) triples blended draws submit,
	// packed src<<16 | dst<<8 | op. A title uses a handful of blend setups, so a small fixed
	// table holds all of them; anything past the end is counted in the overflow rather than
	// silently dropped. Kept for the whole run rather than per stats window - the interesting
	// pairs are the rare ones, and a window boundary would split them.
	static constexpr u32 s_max_blend_census = 24;
	std::array<u32, s_max_blend_census> m_blend_census_key{};
	std::array<u64, s_max_blend_census> m_blend_census_count{};
	u32 m_blend_census_used = 0;
	u64 m_blend_census_overflow = 0;

	void census_blend(u32 src, u32 dst, u32 op);

	// A frame that submits more draws than this stops numbering them rather than growing
	// without bound. Haze peaks around 1500 instances a frame, so this is slack, not a cut.
	static constexpr usz s_max_pick_records = 16384;

	// index + 1 == objectPickingValue, so 0 stays "nothing was drawn here".
	std::vector<pick_record> m_pick_table;

	// Each asynchronous request owns the exact table and click generation it was submitted
	// against. A later frame or click therefore cannot change how its returned IDs are decoded.
	struct pick_callback_state
	{
		// High 32 bits are the click generation. In the low half, bit 31 means a response already
		// claimed the click and the remaining bits are retries. Publishing them together prevents
		// an old callback from clearing a newly-started click.
		std::atomic<u64> click{0};
		// Same generation in the high half, lowest successful radius attempt in the low half.
		std::atomic<u64> winner{0xffffffffu};
		std::atomic<u32> pending{0};
		std::atomic<bool> alive{true};

		// The pick-follow arm. The callback runs on a runtime thread and has no renderer to
		// write to, so the winning record's identity is published here and the RSX thread picks
		// it up at the next submit. 'generation' is bumped last and is what the reader tests, so
		// a half-written pair can never be followed.
		std::atomic<u64> follow_vp{0};
		std::atomic<u64> follow_albedo{0};
		// ROUND 44. The clicked record's vertex count, published alongside the pair because
		// (vp, albedo) DOES NOT IDENTIFY AN OBJECT. Haze draws a tiled prefab as many separate
		// instances of one mesh sharing one texture, so the old two-term match followed
		// "whichever tile was submitted first this frame" and its d_origin was the distance
		// between two DIFFERENT tiles, not one tile's motion. Measured: E40BF80AF519848A alone
		// carries 42 distinct raw vertex boxes across 41 distinct vertex counts on one program,
		// and the "jumps" it reported were lattice steps between neighbouring tiles.
		// Stored before generation, like the pair, so a half-written set can never be read.
		std::atomic<u32> follow_vertex_count{0};
		std::atomic<u64> follow_generation{0};

		// Clicks that produced at least one identified object. Lives here rather than in the stat
		// block because the callback runs on a runtime thread and has no renderer to write to.
		// Printed beside pick_clicks so "the instrument is dead" and "the user never clicked" are
		// two different readings of the live line instead of the same absence of output.
		std::atomic<u64> resolved{0};
	};

	struct pick_request_context
	{
		std::shared_ptr<pick_callback_state> state;
		std::vector<pick_record> snapshot;
		u32 generation = 0;
		u32 attempt = 0;
	};

	// Frames left to keep re-requesting. The runtime only allocates the object-picking
	// target once a request has been seen (rtx_context.cpp:2052), so the first request
	// after a click reliably reads back nothing; retrying for a few frames is what makes a
	// single click work.
	std::shared_ptr<pick_callback_state> m_pick_state = std::make_shared<pick_callback_state>();
	u32 m_pick_attempt = 0;
	s32 m_pick_x = 0;
	s32 m_pick_y = 0;
	bool m_pick_button_down = false;
	// 'Remix pick: armed' once, the first time the runtime slot is found non-null. Without it a
	// run with no pick lines at all is ambiguous between a runtime too old to answer, a knob left
	// off, and a user who simply never Ctrl+clicked - which is exactly the ambiguity that cost
	// round 1 its pick evidence.
	bool m_pick_armed_logged = false;
	// The click generation whose 'exhausted' line has already been written. A click that burns
	// all eight radii without claiming used to end in silence, and the poll runs every flip
	// forever after, so the line has to be reported once per click and not once per flip.
	u32 m_pick_exhausted_generation = 0;

	// --- pick-follow --------------------------------------------------------------------
	// A pick answers "what is that", once. The wobble question is "does that object's transform
	// change between frames when nothing about it should", which no single line can answer. So a
	// successful pick arms a trace on the object it named: one line per frame carrying the
	// instance origin, the basis lengths, and the L1 delta of both against the previous frame.
	// A stable prop reads deltas at 1e-4 or below; the measured wobble is +/-3e-3 on the basis
	// alone. Toggling RPCS3_REMIX_CAMRELATCH between two runs and re-picking the same locker is
	// what closes the attribution, and it is a number either way.
	static constexpr u64 s_pick_follow_frames = 180;
	static constexpr u32 s_max_pick_follow_lines = 400;
	u64 m_pick_follow_generation = 0;
	u64 m_pick_follow_vp = 0;
	u64 m_pick_follow_albedo = 0;
	u32 m_pick_follow_vertex_count = 0;
	u64 m_pick_follow_until_frame = 0;
	u64 m_pick_follow_last_frame = umax;
	u32 m_pick_follow_lines = 0;
	bool m_pick_follow_have_previous = false;
	f32 m_pick_follow_previous_origin[3]{};
	f32 m_pick_follow_previous_basis[3]{};

	// One line per (vp, unit) that ends up on the fixed UVINTSCALE divisor, naming the refusal
	// exit and the slot the strict matcher would have used if it named one. Bounded like every
	// other census here; the population is 267,355 draws and a line per draw would be the log.
	static constexpr u32 s_max_uvscale_fixed_lines = 128;
	std::unordered_set<u64> m_uvscale_fixed_seen;
	u32 m_uvscale_fixed_lines = 0;

	// One line per (program, texcoord output, albedo) whose submitted UVs leave the unit square.
	// Keyed on the albedo too, because one program draws many meshes and it is the texture that
	// says which surface the user is looking at.
	static constexpr u32 s_max_uvrange_lines = 256;
	std::unordered_set<u64> m_uvrange_seen;
	u32 m_uvrange_lines = 0;

	// One line per (vertex program, fragment program, refusal class) submitted with no albedo
	// material at all.
	//
	// Round 6 re-keyed this per stats window instead of per run. The lifetime cap was 128 lines for
	// a whole session and every line on disk came from frames 349-2760, so a census meant to answer
	// "which of the untextured populations dominates" could only ever describe the loading screens.
	// Same window-reset shape as world_refused_census_slot(), for the same reason.
	static constexpr u32 s_max_notex_lines = 128;
	std::unordered_set<u64> m_notex_seen;
	u32 m_notex_lines = 0;
	u64 m_notex_window = umax;

	// --- round 6 censuses --------------------------------------------------------------------
	// One line per (vp, fp) SKIPSHADOWONLY refuses, per window. The rule is a generalisation, so
	// the one thing it must not be allowed to do quietly is widen - this names every program it
	// touches with the sampled mask and the two clips it compared.
	static constexpr u32 s_max_shadowonly_lines = 64;
	std::unordered_set<u64> m_shadowonly_seen;
	u32 m_shadowonly_lines = 0;
	u64 m_shadowonly_window = umax;

	// Round 8: one line per vertex program rescued by the WDIVWALK writer walk. Once per RUN, not
	// per stats window - a program is scanned once and this names a permanent property of its
	// ucode, so repeating it every window would only bury the censuses that describe live state.
	// The expected population on Haze is exactly four (57A12323F22F4988, 4D5A87BFFBCE0717,
	// 96EDAAED0C27FD05, 1D9A973AF5CD1514); a fifth line is an uncached program rescued for free
	// and worth reporting.
	static constexpr u32 s_max_wdiv_shadowed_lines = 32;
	std::unordered_set<u64> m_wdiv_shadowed_seen;
	u32 m_wdiv_shadowed_lines = 0;

	// Round 9: one line per (vertex program, fragment program) per stats window for every
	// submitted draw that has no guest material OR whose fragment program names a vertex-colour
	// output shape. This is the instrument that names the rest of the effects family - the smoke
	// trails, the fire explosions, the death dissolve - the moment the user plays through them,
	// without anyone having to pick each one by hand. The vcol variance is measured from the
	// scratch vertices AFTER apply_vertex_colour, so 'rgb-varies=1' means the mesh really carries
	// a gradient and not just one flat colour.
	static constexpr u32 s_max_effect_lines = 64;
	std::unordered_set<u64> m_effect_seen;
	u32 m_effect_lines = 0;
	u64 m_effect_window = umax;

	// Round 9: one line per vertex program whose raw ucode was captured to bin\remix_ucode\.
	// Once per RUN - it names a permanent property of the program, like the wdiv-shadowed census.
	static constexpr u32 s_max_ucode_store_lines = 64;
	std::unordered_set<u64> m_ucode_stored_seen;
	u32 m_ucode_store_lines = 0;

	// --- round 11 census state -----------------------------------------------------------------
	// One line per FRAGMENT program captured to bin\remix_ucode\*.fp (RPCS3_REMIX_UCODESTOREFP).
	std::unordered_set<u64> m_ucode_fp_stored_seen;
	u32 m_ucode_fp_store_lines = 0;

	// One line per vertex program naming what its ucode routes into COL0, once per run. A
	// permanent property of the program, like the ucode-store census.
	static constexpr u32 s_max_vcol_route_lines = 64;
	std::unordered_set<u64> m_vcol_route_seen;
	u32 m_vcol_route_lines = 0;

	// One line per (vp, albedo) per window for the haze card whose fade is being replayed. Prints
	// both ramps' ranges so a wrong term is visible numerically before it is visible aesthetically.
	static constexpr u32 s_max_hazefade_lines = 16;
	std::unordered_set<u64> m_hazefade_seen;
	u32 m_hazefade_lines = 0;
	u64 m_hazefade_window = umax;

	// Bounded, once per (vp, fail) per window: the verdict said self-lit and the draw was refused
	// before the attach anyway. This is the instrument that would have named round 10's
	// fpvcol_emissive = 0 in one session instead of costing a planning round.
	static constexpr u32 s_max_selflit_miss_lines = 16;
	std::unordered_set<u64> m_selflit_miss_seen;
	u32 m_selflit_miss_lines = 0;
	u64 m_selflit_miss_window = umax;

	// The multi-reference residue probe for the refused effects family (RPCS3_REMIX_FXREFPROBE).
	// Read-only: it changes no decision, it only names which reference WOULD have divided the draw.
	static constexpr u32 s_max_fxref_lines = 8;
	u32 m_fxref_lines = 0;
	u64 m_fxref_window = umax;

	// The haze card's per-vertex fade. Returns the number of vertices whose alpha it wrote, and
	// publishes the two ramps' ranges for the census. Runs from the submit flow after the vertex
	// decode and BEFORE the mesh content hash - the same placement rule apply_vertex_colour
	// documents, because the hash must cover the result.
	u32 apply_haze_fade(u32 first_vertex, u32 vertex_count);
	bool m_scratch_haze_applied = false;
	f32 m_scratch_haze_dist_lo = 0.f;
	f32 m_scratch_haze_dist_hi = 0.f;
	f32 m_scratch_haze_angle_lo = 0.f;
	f32 m_scratch_haze_angle_hi = 0.f;
	u8 m_scratch_haze_alpha_lo = 255;
	u8 m_scratch_haze_alpha_hi = 0;
	// Where ATTR9's facing quaternion came from: a persistent stream, or the vertex REGISTER (one
	// constant for the whole draw, which is what the hardware feeds I9 for a non-persistent
	// attribute). Censused because a register source makes the angle ramp per-draw rather than
	// per-vertex, and that is a fact about the layout worth reading rather than assuming.
	bool m_scratch_haze_quat_stream = false;
	void report_haze_fade(u64 albedo_hash);

	// --- round 27: the particle billboard replay ------------------------------------------------
	// Rewrites m_scratch_vertices IN PLACE for a program on PARTICLEBILLBOARDVP (family A, the
	// camera-facing rotating sprite) or PARTICLERIBBONVP (family B, the trail ribbon), replaying the
	// expansion the vertex program performs on the GPU. Returns the number of QUADS rewritten, 0 if
	// the draw was not a match or was refused.
	//
	// Placement rule, same as apply_haze_fade above and for the same reason: it runs after the vertex
	// decode and BEFORE the mesh content hash, because the hash must cover the rewritten positions or
	// two frames of a moving particle would share one cached BLAS.
	//
	// It NEVER resizes m_scratch_vertices and NEVER touches m_scratch_indices. MEASURED: all six
	// programs submit a vertex count divisible by 4 on every one of 6,148 censused draws, so the guest
	// already supplies the four corners and the shader only displaces them. The 4-vertex grouping is
	// re-verified per draw (all four corners must carry an identical ATTR0) and a draw that fails that
	// test is refused rather than rewritten - that check is what makes this safe on an unseen layout.
	enum class particle_kind
	{
		none,
		billboard, // family A
		ribbon     // family B
	};

	u32 apply_particle_billboards(u32 first_vertex, u32 vertex_count, particle_kind kind);

	// Set for the draw whose positions were rewritten. Read after per_draw_transform() to force the
	// instance transform to identity: the replay outputs WORLD-space positions because the matrix that
	// follows in both families is the fused view*projection in c0..c3.
	bool m_scratch_particle_applied = false;

	// Bounded, deduped on (program, outcome). 'Remix particle:' lines.
	//
	// `vertex_count` is passed in rather than read back from m_scratch_vertices.size(): thirteen of
	// the eighteen call sites are refusals that report BEFORE the decode, where the scratch buffer
	// still holds the PREVIOUS draw's contents. On a brand-new route, vtx= is one of the few numbers
	// that separates a 4-vertex sprite from a mis-sized batch, and a stale value would be read exactly
	// when someone is trying to work out why nothing happened.
	//
	// `qn` is a COUNT on the applied path and the FAILING QUAD INDEX on every refusal path, which is
	// why the field is not called `quads`.
	void report_particle(particle_kind kind, u32 outcome, const char* reason, u32 qn,
		u32 vertex_count, const f32 (&centre)[3], const f32 (&size)[2], const f32 (&eye)[3]);
	static constexpr u32 s_max_particle_lines = 64;
	std::unordered_set<u64> m_particle_seen;
	u32 m_particle_lines = 0;
	u64 m_particle_window = umax;

	// Round 11 instruments, all bounded and all dump-mirrored.
	void report_vcol_route();
	void report_selflit_miss();
	void store_refused_fp_ucode();

	// The multi-reference residue probe (RPCS3_REMIX_FXREFPROBE). per_draw_transform publishes the
	// draw's recovered fused matrix on the rescue=failed exit only - the probe runs at the refused
	// return in submit_draw and cannot see that local.
	void report_fx_ref_probe();

	// --- round 14: the sun card ------------------------------------------------------------------
	// One line per (vp, albedo) per window, DIAGLINES-gated, naming every draw shaped like a sun
	// sprite. It aims nothing; it exists because no artefact in this tree identifies the sun card and
	// the previously-named candidate (fc10c996fd0ec49a) is UNTEXTURED and has not appeared in the
	// last four runs. Bound 32: this has to be readable, and one window's worth is the whole answer.
	static constexpr u32 s_max_suncard_lines = 32;
	std::unordered_set<u64> m_suncard_seen;
	u32 m_suncard_lines = 0;
	u64 m_suncard_window = umax;

	// Called from maybe_inject_guest_light, which has already paid for the bounding box and has the
	// draw's world centre, world extent and the albedo's mean RGB in hand. Reusing that host is why
	// the sun census costs no second bounding-box walk.
	// pinned is (pin_albedo && pin_vp); the two halves ride along unmerged so the census line can
	// attribute a miss to the albedo or to the program (round 16).
	void note_sun_card(u64 albedo_hash, const f32 (&centre)[3], f32 world_extent, bool pinned,
		bool pin_albedo, bool pin_vp);
	void report_sun_card(u64 albedo_hash, const f32 (&centre)[3], f32 world_extent,
		const f32 (&travel)[3], f32 elevation_deg, f32 azimuth_deg, bool pinned, bool elected,
		bool pin_albedo, bool pin_vp);

	// The direction this frame's elected sun card asks the distant light to TRAVEL (sun -> scene,
	// i.e. already negated from camera -> card; see the sign derivation in RemixTransforms.h).
	// Latched per frame and consumed by update_sun_light at the end of the frame, so a card drawn
	// after the light would otherwise have been a frame late.
	bool m_sun_track_have = false;
	f32 m_sun_track_travel[3] = { 0.f, 0.f, 0.f };
	f32 m_sun_track_score = 0.f;
	u64 m_sun_track_frame = umax;
	u64 m_sun_track_albedo = 0;

	// What the LIVE remixapi light was actually created with, so the hysteresis compares against the
	// published value rather than against last frame's candidate.
	bool m_sun_light_aimed = false;
	f32 m_sun_light_travel[3] = { 0.f, 0.f, 0.f };

	// --- round 23: the PER-LEVEL sun ---------------------------------------------------------
	// One solved direction per sky-dome albedo, so walking from one area to another re-aims the
	// distant light without any level-change signal from the guest. A slot is written ONCE, the
	// first time that dome albedo is seen with usable geometry, and is never revised: the sun does
	// not move within a level, and re-deriving it every frame would make the light flicker on a
	// tessellation change.
	//
	// source: 0 = unsolved, 1 = derived from the dome texture (RPCS3_REMIX_SUNSKY),
	//         2 = RPCS3_REMIX_SUNMAP override. SUNDIR remains the fallback when nothing matches.
	struct sky_sun_solution
	{
		u64 albedo = 0;
		f32 travel[3] = { 0.f, 0.f, 0.f };
		f32 peak_uv[2] = { -1.f, -1.f };
		f32 uv_error = -1.f;
		f32 centroid_travel[3] = { 0.f, 0.f, 0.f };
		u32 vertices = 0;
		u32 source = 0;
	};

	std::array<sky_sun_solution, 8> m_sky_sun_solutions{};
	u32 m_sky_sun_count = 0;

	// The solution the CURRENT area's dome selected, republished to the light by update_sun_light.
	bool m_sky_sun_have = false;
	// Round 23 defect fix: the frame the active solution was last republished by a live dome draw.
	// Without it, m_sky_sun_have latches on the first level that solves and never clears, so a
	// stale direction would outrank a live SUNTRACK card forever - including in levels whose own
	// dome never solves, which is the exact opposite of the per-level behaviour this exists for.
	// Going stale does NOT revert to SUNDIR: update_sun_light() simply finds no source and returns,
	// holding the last aim, so an indoor stretch cannot make the sun thrash.
	u64 m_sky_sun_frame = umax;
	u64 m_sky_sun_albedo = 0;
	u32 m_sky_sun_source = 0;
	f32 m_sky_sun_travel[3] = { 0.f, 0.f, 0.f };
	u32 m_sky_sun_census_lines = 0;
	static constexpr u32 s_max_sky_sun_census_lines = 32;

	// --- round 27: the sun derived from the SUN SPRITE -------------------------------------------
	// Published by observe_sun_sprite() from the 2D route, consumed by update_sun_light() ahead of
	// every other source. Same one-frame freshness rule as m_sky_sun_frame above and for exactly the
	// same reason: the sprite is per-level by construction, so a latched value from the level the
	// player just left must not keep aiming the light. Going stale holds the last aim rather than
	// snapping to SUNDIR, again matching the sky path.
	bool m_sun_sprite_have = false;
	u64 m_sun_sprite_frame = umax;
	u64 m_sun_sprite_albedo = 0;
	f32 m_sun_sprite_travel[3] = { 0.f, 0.f, 0.f };
	f32 m_sun_sprite_ndc[2] = { 0.f, 0.f };

	// Reads the quad's NDC bounding box on the 2D route, rejects anything that is not a small, roughly
	// square, fully on-screen sprite, and unprojects the centre through the live camera. Direction
	// convention on the way out is TRAVEL (sun -> scene), i.e. the negation of eye -> sun, matching
	// m_sun_light_travel and every other source.
	void observe_sun_sprite(u64 albedo_hash, const f32 (&lo)[2], const f32 (&hi)[2]);
	void report_sun_sprite(u64 albedo_hash, const char* verdict, const f32 (&lo)[2],
		const f32 (&hi)[2], const f32 (&travel)[3]);
	u32 m_sun_sprite_census_lines = 0;
	u64 m_sun_sprite_census_window = umax;

	// Runs at the point the decoded vertices and their texcoords exist, for a draw whose bound
	// texture is a listed sky dome. Returns true when it wrote a solution.
	bool derive_sky_sun(u64 albedo_hash, const remixapi_Transform& transform, u32 vertex_count);
	const sky_sun_solution* find_sky_sun(u64 albedo_hash) const;

	// Destroy + re-create m_sun_light when the tracked direction has moved more than
	// RPCS3_REMIX_SUNTRACKDEG. There is no update-light entry point in the Remix C API.
	void update_sun_light();
	remix_rsx::mat4 m_scratch_fxref_fused{};
	bool m_scratch_fxref_valid = false;

	// Round 11: does this draw's vertex program route ATTR3 into COL0 at all, and may its ALPHA
	// lane be replayed? Both answer 'yes' unchanged when RPCS3_REMIX_FPVCOLROUTE=0.
	bool vcol_route_replayable() const;
	bool vcol_route_alpha_replayable() const;
	// Round 12. c[K] for the 'constant' vertex-colour route, read live. False = unreadable, in
	// which case nothing is replayed and the draw keeps white.
	bool resolve_constant_vertex_colour(f32 (&out)[4]) const;

	// Round 9: one line per vertex program resolved by the mixed-lane fused-group arm, once per
	// run. Expected population on Haze from the offline sweep: exactly one (df46f03b1b7ab8a4),
	// plus whatever the ucode capture adds.
	std::unordered_set<u64> m_madmix_seen;

	// Round 17: the same, for the programs that additionally needed the divide's lane permutation.
	// Expected population on Haze from the offline sweep: five - f7f12d5d15bb9c37,
	// c1f88035801f88de, 61ed1d272c0653aa, 1d22398b18a0e97d and 5888b152531b2d91.
	std::unordered_set<u64> m_madlanemap_seen;

	// Round 9 (new user report, instrument only): a bounded once-per-window census of which
	// camera the election settled on and why it changed. The user watches the dev menu's camera
	// list flicker and occasionally gets a frame with everything far away; cam_resolved /
	// cam_fallback / cam_held / cam_contested say HOW OFTEN but never WHICH, so a flicker cannot
	// be attributed to a program, a surface or a clip. Changes no decision.
	static constexpr u32 s_max_cam_elect_lines = 48;
	u32 m_cam_elect_lines = 0;
	u64 m_cam_elect_window = umax;
	u64 m_cam_elect_last_key = umax;

	// --- round 10 -----------------------------------------------------------------------------
	// One line per (vp, clip) the candidate clip gate refuses, per window. Bounded small on
	// purpose: the population is meant to be one shadow pass, and if it is ever anything else -
	// a cutscene camera at a novel clip - that has to be visible in the first session rather
	// than argued about later. This is the line that says "the portal frames are gone because
	// THIS pass stopped being electable".
	static constexpr u32 s_max_clipgate_lines = 16;
	std::unordered_set<u64> m_clipgate_seen;
	u32 m_clipgate_lines = 0;
	u64 m_clipgate_window = umax;

	// One line per gauge-anchor key whose HOLDER IDENTITY changes between frames - the
	// anchor-level twin of cam-elect. The contested census says "someone disagreed"; this says
	// "the slot changed hands", which is the event that actually moves geometry. Steady play
	// should print none of these; a real cut prints one per key.
	static constexpr u32 s_max_anchor_elect_lines = 32;
	u32 m_anchor_elect_lines = 0;
	u64 m_anchor_elect_window = umax;

	// ROUND 29. 'Remix anchor-gauge:' - the disagreement between the two writers of
	// m_active_camera.position, measured at the one site that can see both: inside
	// apply_gauge_anchor_camera, before the flip's election latch has replaced the override.
	// Deduped on (anchor identity, elected identity, magnitude bucket, dominant axis) so a steady
	// carrier ride prints one line and a change of donor prints another. Budget from
	// RPCS3_REMIX_ANCHORGAUGECENSUS, capped by the constant so the knob cannot flood the log.
	static constexpr u32 s_max_anchor_gauge_lines = 64;
	u32 m_anchor_gauge_lines = 0;
	u64 m_anchor_gauge_window = umax;
	u64 m_anchor_gauge_last_key = umax;
	// The largest L-infinity disagreement seen this run, printed on the live line beside
	// anchor_cam_offset. Kept out of the stats struct so that struct stays all-integer apart from
	// the one float it already carries (wext_drawn_max).
	f32 m_anchor_cam_offset_max = 0.f;

	// --- ROUND 30: the anchor-frame eye ------------------------------------------------------
	// The camera position expressed in the frame the SUBMITTED geometry is in, published once per
	// flip by apply_gauge_anchor_camera - which already computes exactly this value and then writes
	// it into m_active_camera.position, where the flip-tail election latch and the mid-frame relatch
	// overwrite it with the elected pass's eye on 79% of frames (cam_relatch=52505 of 66508).
	//
	// Kept as a separate named field rather than by changing what .position means, for two reasons.
	// (1) .position and .reference_inverse are always written TOGETHER from one source, and the
	// viewmodel probe at per_draw_transform divides by .reference_inverse - so decoupling them would
	// break a gate that is currently self-consistent. (2) Sixteen diagnostic lines print .position as
	// `cam=`; changing it silently would make every round's logs incomparable.
	//
	// STALENESS, stated as a number rather than left to be discovered: this is one flip old for a
	// mid-frame consumer, so on the land carrier it lags by that donor's own travel, 0.54 units per
	// frame (round 29, least-squares over 480 frames). That is 0.13% of the 429.96-unit error it
	// removes. The freshness test in anchor_frame_eye() refuses anything older than one frame.
	f32 m_anchor_frame_eye[3]{};
	bool m_anchor_frame_eye_valid = false;
	u64 m_anchor_frame_eye_frame = umax;
	// WHICH anchor the eye came from. Load-bearing, not a diagnostic: the getter refuses to serve the
	// eye unless the draw asking for it was divided by THIS anchor (m_ref_pick_vp), because
	// find_gauge_anchor keys on the exact (surface, target, clip) while main_gauge_anchor - which
	// publishes this eye - returns the largest-area slot. Without the test, a draw divided by a
	// secondary anchor would be measured against the main anchor's eye.
	u64 m_anchor_frame_eye_vp = 0;
	// Whether the published eye came from the GAUGECAMHOLD branch rather than from a fresh split. A
	// held value can be up to GAUGECAMHOLD (30) frames old while still being re-stamped as fresh, and
	// the one consumer that must never accept that is derive_sky_sun, whose verdict is latched per
	// albedo for the whole run. Found by review; see anchor_frame_eye_counted's allow_held.
	bool m_anchor_frame_eye_held = false;
	// The largest correction this run: max over consumer reads of |anchor eye - .position|_inf.
	// Outside the stats struct for the same reason m_anchor_cam_offset_max is.
	f32 m_camframe_max = 0.f;

	// One line per vertex program resolved by the accumulate-in-place walk, once per run. Same
	// shape and lifetime as m_madmix_seen: it names a permanent property of the program.
	std::unordered_set<u64> m_madaccum_seen;

	// Round 7: one line per (vertex program, albedo content hash) per stats window for every 2D UI
	// draw the compositor rasterizes - the atlas, its real bound wrap modes, the authored UV range
	// and how far outside [0,1] that range goes IN TEXELS. This is the measurement the seam fix is
	// asserted against rather than assumed from: a line with wrap=1/1 and a non-zero exc is a draw
	// that WAS sampling the far edge of its own sheet, and its albedo is paste-ready for
	// RPCS3_REMIX_CLAMPALBEDO if the same texture also garbles through the 3D route.
	// Round 7: one line per (vertex program, albedo) per stats window for every SUBMITTED draw
	// whose affine analysis refused with a 'sca*' reason - the giant-ship / giant-weapon /
	// wrong-bones family. Names the population and its extent spread from an ordinary session
	// instead of one pick at a time.
	static constexpr u32 s_max_scaxyz_lines = 64;
	std::unordered_set<u64> m_scaxyz_seen;
	u32 m_scaxyz_lines = 0;
	u64 m_scaxyz_window = umax;

	static constexpr u32 s_max_uiwrap_lines = 64;
	std::unordered_set<u64> m_uiwrap_seen;
	u32 m_uiwrap_lines = 0;
	u64 m_uiwrap_window = umax;
	void report_ui_wrap(const remix_rsx::texture_entry& entry, f32 u_lo, f32 u_hi, f32 v_lo, f32 v_hi,
		bool subrect_u, bool subrect_v);

	// One line per (vp, albedo) the tail rescue touched, per window: the residue before and after,
	// which retry answered, and how old the anchor was. This is what turns "world_refused dropped"
	// into "these programs were rescued by an anchor this many frames old".
	static constexpr u32 s_max_tailrescue_lines = 96;
	std::unordered_set<u64> m_tailrescue_seen;
	u32 m_tailrescue_lines = 0;
	u64 m_tailrescue_window = umax;

	// One line per vertex program refused at the !fp.has_outer() exit (fail=lay_other), per window,
	// naming the chain length and constant count the fingerprint did resolve. The population owns
	// 35% of world refusals and has never been named per program in a form the offline ucode read
	// can be matched against.
	static constexpr u32 s_max_layother_lines = 64;
	std::unordered_set<u64> m_layother_seen;
	u32 m_layother_lines = 0;
	u64 m_layother_window = umax;

	// One line per (albedo, vp, fp) that looks like a light fixture by the GUESTLIGHTLUM /
	// GUESTLIGHTMAXEXT / render-state test, per window. Measurement input for the guest-light
	// discriminator and for the EMISSIVE list; nothing acts on it unless GUESTLIGHTAUTO is on.
	static constexpr u32 s_max_lightcand_lines = 96;
	std::unordered_set<u64> m_lightcand_seen;
	u32 m_lightcand_lines = 0;
	u64 m_lightcand_window = umax;

	// --- ROUND 41: 'Remix fpother:' ------------------------------------------------------------
	// One line per fragment program whose output-colour walk ended in 'other'. No dedup set is
	// needed: the block that emits it runs inside the m_fp_fingerprints cache MISS, i.e. exactly
	// once per unique fragment program for the life of the process. Not per window either - the
	// program set is fixed, so a second window would print nothing new. 64 covers the 47 programs
	// this title's own vcolroute census names with room to spare.
	static constexpr u32 s_max_fpother_lines = 64;
	u32 m_fpother_lines = 0;

	// One line per contesting vertex program per window for gauge_anchor_contested, which went from
	// 0 for four rounds to 5,467 in the newest run. First-draw-wins is measurably fragile and the
	// counter alone cannot say which program disagreed. Measurement only; the consensus redesign is
	// its own round.
	static constexpr u32 s_max_contested_lines = 32;
	std::unordered_set<u64> m_contested_seen;
	u32 m_contested_lines = 0;
	u64 m_contested_window = umax;

	// The shared mid-grey fallback material (RPCS3_REMIX_NOTEXMAT). Created on the first
	// material-less draw and kept for the life of the renderer: it has no guest bytes behind it, so
	// it can never go stale and must not enter the texture cache's reap cycle. 'failed' latches a
	// creation error so a broken runtime does not retry the create once per draw forever.
	remixapi_MaterialHandle m_neutral_material = nullptr;
	remixapi_TextureHandle m_neutral_texture = nullptr;
	bool m_neutral_material_failed = false;
	// Lazily creates (or returns) the material above. Null when NOTEXMAT is off or creation failed.
	remixapi_MaterialHandle neutral_material(const remixapi_Interface& api);

	// --- round 10: the self-illuminated twin (RPCS3_REMIX_FPVCOLEMISSIVE) ----------------------
	// Byte-for-byte the neutral material above, except emissiveIntensity = fp_vcol_emissive() and
	// emissiveColorConstant = white. It needs its own constant hash because the runtime resolves
	// a material's albedo through the synthetic "0x<hash>" path and two materials sharing one
	// hash are one material. Same three outcomes, same dump reporting, same lifetime: no guest
	// bytes behind it, so it can never go stale and must never enter the reap cycle.
	remixapi_MaterialHandle m_self_lit_material = nullptr;
	remixapi_TextureHandle m_self_lit_texture = nullptr;
	bool m_self_lit_material_failed = false;
	remixapi_MaterialHandle self_lit_material(const remixapi_Interface& api);

	// The candidate clip gate's census: one line per (vp, clip) per window.
	void report_clip_gate(u64 vp_hash, u32 surface_offset, u32 clip_width, u32 clip_height,
		u32 main_width, u32 main_height);

	// The anchor-level election census: fired when a key's holder identity changes between frames.
	void report_anchor_elect(const gauge_anchor& slot, u64 old_vp, bool continuous,
		bool parked, bool promoted);

	// Installs any parked gauge candidate whose key saw no continuous donor this frame. Runs at
	// flip, beside the per-frame camera resets, so a real cut converges in exactly one frame.
	void promote_parked_anchors();

	// One gauge written into one slot, shared by the capture path and the flip-time promotion so
	// the two cannot drift apart. False only on a singular matrix (gauge_invert_failed).
	bool write_gauge_anchor(gauge_anchor& slot, const remix_rsx::mat4& fused, u64 vp_hash,
		u32 surface_offset, u32 color_target, u32 clip_width, u32 clip_height,
		f32 fold_scale_z, f32 fold_offset_z);

	// What apply_texcoords did for the draw currently being submitted, read at the pick-record
	// fill site. Reset per subdraw, so a draw that never reached apply_texcoords reports 'none'
	// rather than the previous draw's provenance.
	u8 m_uv_pick_attribute = 0xff;
	bool m_uv_pick_from_ucode = false;
	uv_scale_source m_uv_pick_scale = uv_scale_source::none;
	f32 m_uv_pick_scale_value = 0.f;
	f32 m_uv_pick_min[2]{};
	f32 m_uv_pick_max[2]{};
	bool m_uv_pick_valid = false;
	// The rest of the resolved form, for 'Remix pick-deep:'. The uvrange census already prints this
	// for the whole scene; the pick line is the same reading taken on the object the user named.
	s8 m_uv_pick_unit = -1;
	u16 m_uv_pick_scale_slot = 0xffff;
	u8 m_uv_pick_scale_component[2] = { 0, 0 };
	bool m_uv_pick_has_rows = false;
	f32 m_uv_pick_slot_value[4]{};

	// --- reference provenance for the draw currently being submitted -------------------------
	// Written by per_draw_transform, read at the pick-record fill site. Reset per subdraw so a
	// refused or bypassed draw cannot inherit the previous draw's reference.
	ref_source m_ref_pick_source = ref_source::camera;
	u64 m_ref_pick_frame = umax;
	u64 m_ref_pick_vp = 0;
	bool m_ref_pick_zfold = false;
	bool m_ref_pick_fused_valid = false;
	remix_rsx::mat4 m_ref_pick_fused{};

	// attr0's stored shape and decoded w range, computed once per vertex program rather than per
	// draw - a per-draw pass over every vertex is exactly the cost the DUMP gate exists to avoid,
	// and the shape is a property of the program's vertex layout, not of one draw.
	struct attr0_shape
	{
		bool valid = false;
		u8 type = 0xff;
		u8 size = 0;
		u16 stride = 0;
		f32 w_min = 0.f;
		f32 w_max = 0.f;
	};

	static constexpr usz s_max_attr0_shapes = 512;
	std::unordered_map<u64, attr0_shape> m_attr0_shapes;

	// One 'Remix pick-deep:' line per resolved pick. Static because it is emitted from
	// pick_callback, which runs on a runtime thread and has no renderer to reach: everything it
	// prints was captured into the record at draw time, which is the only place it existed.
	static void trace_pick_deep(const pick_record& record);

	// One line per (vertex program, fragment program) refused by SKIPAUXUNTEX.
	static constexpr u32 s_max_aux_untex_lines = 128;
	std::unordered_set<u64> m_aux_untex_seen;
	u32 m_aux_untex_lines = 0;
	void report_aux_untextured(u64 albedo_hash);

	// One 'Remix kil:' line per (fragment program, albedo) that carries a per-pixel discard on
	// either channel - the ucode's KIL or the texture unit's alpha-kill bit. This is the artefact
	// the foliage verdict rests on: it cross-tabulates the two detection channels against the
	// draw's blend state and the bound texture's real alpha range, so one Ctrl+Click on a leaf wall
	// says which mechanism the title uses, or says that it uses neither - which is a shippable
	// answer with bytes behind it rather than a fourth refuted guess.
	static constexpr u32 s_max_kil_lines = 128;
	std::unordered_set<u64> m_kil_seen;
	u32 m_kil_lines = 0;
	void report_kil(u64 albedo_hash, bool texkill, bool applied, u32 applied_ref);

	// The main pass's clip size: the largest clip a *textured* draw used in the previous frame.
	// Textured on purpose - a material-less depth or shadow pass can be larger than the world pass
	// (Haze's shadow map is 2048x2048) and must not be allowed to define "main", or every
	// material-less draw on the real world pass would be refused with it.
	u32 m_main_clip_width = 0;
	u32 m_main_clip_height = 0;
	u64 m_main_clip_area = 0;
	u32 m_frame_main_clip_width = 0;
	u32 m_frame_main_clip_height = 0;
	u64 m_frame_main_clip_area = 0;

	// Round 7: the same measurement, taken as a session-wide maximum instead of a per-frame one.
	// m_main_clip_* above is the PREVIOUS frame's largest textured clip, and on Haze that value
	// oscillates: 1024x576 on frames where the main world pass drew something textured, 512x288 on
	// the many frames where it did not. Every rule phrased as "strictly smaller than the main
	// pass" therefore fires or does not fire depending on what the previous frame happened to
	// contain - which is exactly why SKIPSHADOWONLY reached only 4.7% of the population it
	// classifies (the half-res passes are 512x288, i.e. NOT strictly smaller than 512x288). The
	// largest textured clip a title has ever used is a stable statement about the title; the
	// previous frame's is a statement about the previous frame. RPCS3_REMIX_MAINCLIPMAX=0 restores
	// the per-frame value everywhere.
	u32 m_max_clip_width = 0;
	u32 m_max_clip_height = 0;
	u64 m_max_clip_area = 0;

	// The clip area a "smaller than the main pass" test should compare against, honouring
	// MAINCLIPMAX. Never smaller than m_main_clip_area, so turning the knob on can only ever widen
	// such a rule, never narrow it.
	u64 main_clip_reference() const;

	// One line per (gate, vp, fp, albedo) for every gate that drops a draw after submission began.
	// The instrument for "what ate the soldier's outfit" - which is a question about *which* rule
	// fired, and every one of them used to increment a shared counter and return in silence.
	static constexpr u32 s_max_skip_census_lines = 256;
	std::unordered_set<u64> m_skip_census_seen;
	u32 m_skip_census_lines = 0;
	// extent/dist are -1 at the gates that fire before per_draw_transform has run: they are
	// printed where they exist rather than faked where they do not.
	void report_skip_census(const char* gate, u64 albedo_hash, f32 extent, f32 dist);
	// Handed to audit_world_extent, whose signature carries neither, so the wext gate's census line
	// has the same shape as every other one. Set immediately before that call.
	f32 m_skip_census_dist = -1.f;
	u64 m_skip_census_albedo = 0;
	// RPCS3_REMIX_WATCHALBEDO. One line per verdict class (submitted / skipped) per frame per
	// watched albedo, so a skip can never be hidden behind a submit of the same texture.
	std::unordered_map<u64, u64> m_watch_submit_frame;
	std::unordered_map<u64, u64> m_watch_skip_frame;
	// 'fail' names which per_draw_transform exit refused the draw, for the world_refused verdict;
	// nullptr everywhere else and printed as '-'.
	void note_watch(const char* verdict, u64 albedo_hash, f32 extent, f32 dist, bool submitted = false,
		const char* fail = nullptr);
	// The material trace is intentionally one line per frame: a material may be reused by every
	// aircraft submesh, while the camera association only changes frame-to-frame.
	u64 m_trace_albedo_frame = umax;
	// One line per program that resolves no world transform, bounded. The population is half
	// the scene and has never been named.
	static constexpr u32 s_max_world_refused_lines = 128;
	std::unordered_set<u64> m_world_refused_seen;
	u32 m_world_refused_lines = 0;
	// Re-keyed per (vertex program, fail code) and reset per stats window. The first cut deduped on
	// the program alone with a *lifetime* 128-line cap, and the round-3 run exhausted that cap by
	// frame 3301 on 'fail=nocam' lines from the pre-camera frames - so 883,441 refusals in the part
	// of the run the user was actually playing produced no lines at all, and a program refused for
	// two different reasons could only ever report the first. Both changes are pure logging.
	u64 m_world_refused_window = umax;
	// Claims a census slot for the current draw, doing the window reset and the re-key. False means
	// this (program, reason) has already been printed in this window or the window's cap is spent.
	bool world_refused_census_slot();

	// Which exit per_draw_transform took when it refused. Fourteen returns share one counter,
	// and the last round proved how expensive guessing between them is: the camera-less-frame
	// theory predicted ~92% of world_refused and measured 0.11%. Set at each return, printed by
	// the world-refused census, so the population is named rather than modelled.
	static constexpr const char* s_world_fail_names[] = {
		"nocam", "idxworld", "sl_group", "sl_bone", "lay_group", "lay_ref", "fused_vpi",
		"lay_other", "ref_group", "ref_bone", "ref_vm", "ref_none", "no_reference", "tail" };

	// How far past the affinity tolerance the refused draws actually sit. Buckets are
	// <0.05, <0.2, <1, <10, >=10 against a tolerance of 0.02.
	static constexpr const char* s_bone_fail_names[] = {
		"nonfinite", "affine", "basis", "palette_affine", "palette_basis" };

	u64 m_bone_fail_counts[std::size(s_bone_fail_names)] = {};
	f32 m_bone_residue_max = 0.f;
	u64 m_bone_residue_buckets[5] = {};

	// True when this bone must not enter the blend, counting which of the three faults it was.
	bool bone_rejected(const remix_rsx::mat4& bone);

	f32 m_affine_residue_max = 0.f;
	u64 m_affine_residue_buckets[5] = {};

	const char* m_world_fail = "";

	// The perspective residue the affinity gate measured on the last refused draw, carried to the
	// world-refused census so each named program reads as "needs a reference of its own" (residue
	// orders of magnitude over the tolerance, e.g. af06f6d32ec048ee at 3.39) or "gauge casualty"
	// (residue near the tolerance). Zeroed at every other refusal exit so a program refused before
	// the gate cannot inherit the previous draw's number.
	f32 m_world_refused_residue = 0.f;

	// Per *draw*, not per program. The first cut of this printed one census line the first time
	// a program was refused, which biased the whole distribution: early frames have no camera,
	// so 17 programs were tagged "nocam" on their first refusal and carried that label forever
	// even though world_refused_nocam is 0.11% of the population. Counting every refusal is the
	// only form of this that answers the question.
	u64 m_world_fail_counts[std::size(s_world_fail_names)] = {};

	// The index behind m_world_fail, kept so the census can key on the reason without hashing a
	// string pointer whose value is not stable across builds.
	u32 m_world_fail_index = 0;

	void note_world_fail(u32 reason)
	{
		m_world_fail = s_world_fail_names[reason];
		m_world_fail_index = reason;
		++m_world_fail_counts[reason];
		// Only the affinity gate has a residue to report; it sets the member back after calling
		// this, so every other exit reports 0 rather than the last gated draw's number.
		m_world_refused_residue = 0.f;
	}


	// True once any frame has resolved a camera of the title's own. Gates the stage-A debug
	// triangle so it stays a pre-first-camera fallback and never replaces a running scene.
	bool m_camera_ever_valid = false;

	bool m_sky_camera_warned = false;
	// Round 10: the VIEW_MODEL twin's one-shot rejection census, on the same terms as the sky
	// camera's - a runtime that refuses the camera must not write one error line per frame.
	bool m_viewmodel_twin_warned = false;
	bool m_pick_slot_warned = false;
	bool m_pick_request_warned = false;

	// Ctrl+Click in the game window, polled once per flip. Deliberately not routed through
	// rpcs3's input system: that would mean touching a pad handler, and the mouse is not
	// bound to anything here anyway.
	void poll_pick_request();
	static void pick_callback(const u32* values, u32 count, void* user);

	// One 'Remix pick-follow:' line per frame for the object the last pick named. Called from the
	// pick-record fill site, where the instance transform for this draw already exists.
	void trace_pick_follow(const pick_record& record);

	// What apply_texcoords did, recorded for the pick line. Called from every path that writes a
	// texcoord, including the two affine replays, so the pick line cannot report a provenance the
	// draw did not take. Measures the submitted u/v range as it does so.
	// The trailing arguments are the census's, not the pick line's: the resolved form and the live
	// constant vectors the replay just read. Only the general affine path has them, so they default
	// to null and the census prints what it was given.
	void record_uv_provenance(u32 attribute, bool from_ucode, uv_scale_source source,
		f32 scale_value, u32 vertex_count, u32 unit = 0, u32 coord_output = 0, u64 albedo = 0,
		const remix_rsx::uv_affine_form* form = nullptr, const f32* scale_slot_value = nullptr,
		const f32* row0_value = nullptr, const f32* row1_value = nullptr,
		const f32* bias_value = nullptr, const f32* bias2_value = nullptr,
		const f32* lane_scale = nullptr);

	// One census line per (program, texcoord output, albedo) whose submitted UVs leave the unit
	// square, with the resolved form and every constant it reads. RPCS3_REMIX_UVRANGECENSUS=0 off.
	void report_uv_range(u32 unit, u32 coord_output, u32 attribute, u64 albedo,
		const f32 (&lo)[2], const f32 (&hi)[2], u32 vertex_count, uv_scale_source source,
		const remix_rsx::uv_affine_form* form, const f32* scale_slot_value,
		const f32* row0_value, const f32* row1_value, const f32* bias_value,
		const f32* bias2_value, const f32* lane_scale);

	// One census line per (vertex program, fragment program) submitted with no albedo material.
	// RPCS3_REMIX_NOTEXCENSUS=0 off.
	void report_no_material(bool had_unit);

	// --- round 6 -----------------------------------------------------------------------------
	// True when the raw RSX texture format byte (LN/UN bits included) names one of the four
	// CELL_GCM_TEXTURE_DEPTH* formats. That, and not "no unit at all", is the term that separates
	// a shadow-projection pass from a vertex-coloured surface: Haze's untextured lighting passes
	// name a 2048x2048 DEPTH16 (raw 0xb2) as their only eligible unit.
	static bool is_depth_texture_format(u32 raw_format);

	// The next eligible unit at or above 'from' that the fragment program's own sampled_mask names
	// AND whose bound texture carries a colour format. -1 when there is none. The sampled-mask term
	// is what stops this from repeating the RETRYUNSUP fault of stepping onto units the program
	// never reads.
	int next_sampled_colour_unit(u32 unit_mask, u32 from) const;

	// One line per (vp, fp) SKIPSHADOWONLY refuses, per stats window.
	void report_shadow_only();

	// One line per vertex program whose position decode was rescued by the round-8 w-only writer
	// walk, once per run. Names the program and how many w-only writes shadowed its divide MUL.
	void report_wdiv_shadowed();

	// Round 9. One line per (vp, fp) per stats window for every submitted draw that is either
	// material-less or FP-classified as vertex-coloured. Called from the submit path AFTER
	// apply_vertex_colour so the measured colour variance is the one actually submitted.
	void report_effect_draw(u64 albedo_hash, bool has_material);

	// Round 9. Write one refused vertex program's raw ucode to bin\remix_ucode\%016llX.vp.
	// Once per program per run; no behaviour change.
	void store_refused_ucode(u64 vp_hash, const remix_rsx::vp_fingerprint& fp);

	// Round 9, instrument only. Names the camera the election settled on and what changed about
	// it, once per (elected vp, surface, clip, source) per stats window.
	void report_camera_election(const char* source);

	// One line per (vp, albedo) the tail rescue ladder touched, per stats window.
	// 'split' is -1 where it was not measured (the rescued outcomes) and 0/1 on the failed exit:
	// whether the draw's own fused matrix splits into a plausible view x projection.
	void report_tail_rescue(const char* outcome, f32 residue_before, f32 residue_after,
		u64 anchor_vp, u64 anchor_age, u32 clip_w, u32 clip_h, u32 surface, u32 target,
		bool same_ref, s32 split = -1);

	// One line per vertex program refused at the !fp.has_outer() exit, per stats window.
	void report_lay_other();

	// One line per (albedo, vp, fp) that qualifies as a light-fixture candidate, per stats window.
	void report_light_candidate(u64 albedo_hash, f32 extent, const f32 (&mean_rgb)[3]);

	// One census line per (program, texcoord output) that fell back to the fixed UVINTSCALE
	// divisor, naming the refusal exit, the slot the strict matcher would have used, and what the
	// general affine walk made of the same slice.
	void report_uv_scale_fixed(u32 output, u32 attribute, remix_rsx::texcoord_scale_refusal refusal);

	camera_candidate m_active_camera{};
	// A radically different camera must persist before it can replace the established gameplay
	// view. This filters Haze's short auxiliary-camera passes without delaying normal movement.
	camera_candidate m_pending_camera{};
	u32 m_pending_camera_frames = 0;

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

	// --- gauge anchors ------------------------------------------------------------------
	// One slot per render source, each carrying the frame it was captured in. No rotation at
	// flip: a lookup asks for this frame's anchor or the previous frame's by comparing that
	// stamp, so a source whose identity draws stop appearing ages out on its own instead of
	// serving a two-frame-old gauge. Draws that arrive before the frame's first identity draw
	// use the previous frame's anchor, which is never worse than the elected camera's reference -
	// that was always a frame old.
	//
	// Sixteen slots, of which RPCS3_REMIX_GAUGESLOTS chooses how many are live (default 16; 4
	// reproduces round 2 exactly). Round 2's four were not enough: a slot is only reusable once its
	// anchor is two frames old, so the shadow pass, the auxiliary pass, the main pass and anything
	// else drawing that frame competed for four entries and the loser's draws fell back to the
	// elected camera - one of the two silent exits behind gauge_absent. gauge_slot_exhausted counts
	// the loss and 'Remix gauge-keys:' lists the live slots, so "is 16 enough" is answered by the
	// run instead of by this comment.
	static constexpr u32 s_max_gauge_anchors = 16;
	std::array<gauge_anchor, s_max_gauge_anchors> m_gauge_anchors{};

	// Store this frame's anchor for the current draw's render source, first identity draw wins.
	void capture_gauge_anchor(const remix_rsx::mat4& fused);
	// The anchor for one render source, from this frame ('current') or the previous one. For the
	// previous-frame lookup only, an exact-key miss retries on (target, clip) alone when
	// RPCS3_REMIX_GAUGEPREVDIMS is on; 'dims_fallback' reports which pass answered.
	const gauge_anchor* find_gauge_anchor(bool current, u32 surface_offset, u32 color_target,
		u32 clip_width, u32 clip_height, bool* dims_fallback = nullptr) const;
	// The tail rescue's lookup (RPCS3_REMIX_TAILRESCUE): the freshest anchor for one PASS SHAPE -
	// colour target plus clip rectangle - at ANY surface offset, whose age in frames lies in
	// [min_age, max_age]. 'out_age' reports how stale the winner was. The two retries are
	// [0,0] (this frame) and [1, TAILRESCUEAGE], so the second can never re-test the first's pick.
	//
	// Deliberately a separate function rather than a mode of find_gauge_anchor: that one's
	// current-frame lookup keeps the exact surface key by design, because within a frame the offset
	// IS the pass's identity and aliasing two same-shaped sources there would be a real fault. The
	// reasoning holds for a draw that is going to be accepted. It does not hold for a draw that is
	// otherwise going to be dropped outright, which is the only caller here.
	const gauge_anchor* find_gauge_anchor_shape(u32 color_target, u32 clip_width, u32 clip_height,
		u64 min_age, u64 max_age, u64* out_age) const;
	// The main-pass anchor: the one covering the largest clip area, which on any title that
	// renders its world at full resolution and its auxiliary passes at half is the world pass.
	// What the submitted camera is rebuilt from, so poses and camera share one gauge.
	const gauge_anchor* main_gauge_anchor(bool current) const;
	// One 'Remix gauge:' line per frame measuring anchor vs elected-camera disagreement, plus a
	// per-window viewport census for each elected camera program. Called at flip, before the
	// camera is overridden, so it reports the disagreement rather than zero.
	void trace_gauge_anchor();
	// Rebuild m_active_camera's matrices from the anchor's own split, at flip, before submission.
	void apply_gauge_anchor_camera();
	// ROUND 30. The eye in the frame the submitted geometry is in - see m_anchor_frame_eye.
	//
	// FOUR refusals, and each one leaves the caller on m_active_camera.position, which is the round-29
	// behaviour exactly:
	//   * nothing published yet (no anchor this run, or GAUGEANCHOR=0);
	//   * the published eye is more than one flip old (the scene-cut guard);
	//   * **THIS DRAW WAS NOT DIVIDED BY AN ANCHOR AT ALL.** per_draw_transform starts with
	//     `reference = &m_active_camera.reference_inverse` and only replaces it when
	//     find_gauge_anchor hits; on a miss (gauge_anchor_absent, MEASURED at 1,287,878 of 17,869,132
	//     divides = 7.21% in the traced session) the draw is divided by the ELECTED camera and its
	//     submitted transform is in the elected frame - so for that draw the correct eye is
	//     m_active_camera.position and substituting the anchor's would BREAK a gate that was right.
	//     This was the blocker the round-30 review found; the test is m_ref_pick_source, which
	//     per_draw_transform already resets per draw and sets only on the anchor branches;
	//   * the draw was divided by a DIFFERENT anchor than the one that published the eye
	//     (m_ref_pick_vp != m_anchor_frame_eye_vp) - find_gauge_anchor keys on the exact
	//     (surface, target, clip), main_gauge_anchor returns the largest-area slot, so these can differ.
	//     Residual, stated: two slots held by the SAME program pass this test, and by measurement that
	//     program's own eye disagreement is at most 0.124146 units, so the residual is sub-unit.
	// const and side-effect free, because one caller (apply_viewmodel_basis) is itself const.
	bool anchor_frame_eye(f32 (&out)[3]) const;
	// The same tests WITHOUT the knob, so the CAMANCHOREYE=0 arm of the A/B still measures the size of
	// the correction it is declining to make. Round 30's first draft gated everything on the knob and
	// the off arm therefore reported nothing at all.
	bool anchor_frame_eye_available(f32 (&out)[3]) const;
	// The counting wrapper: camframe_served / camframe_corrected / camframe_absent and m_camframe_max.
	// corrected and max accumulate in BOTH arms of the knob; served only when it is armed.
	// allow_held=false additionally refuses an eye that came from the GAUGECAMHOLD branch - passed by
	// derive_sky_sun alone, because its verdict is latched per albedo for the whole run and a held eye
	// can be up to 30 frames old.
	// Non-const, so apply_viewmodel_basis uses the bare getter and is the one gate whose reads are not
	// counted; it is also inert until RPCS3_REMIX_VMPAIRVP is non-blank (vm_tagged=0 all session).
	bool anchor_frame_eye_counted(f32 (&out)[3], bool allow_held = true);
	// One 'Remix gauge-keys:' line per stats window listing every live anchor slot. The direct
	// answer to "how many render sources actually compete for a slot", which round 2 guessed at.
	void report_gauge_keys();
	// One 'Remix gauge-selfcheck:' line per anchor per stats window: ||A.inv(A) - I||max computed
	// in f32 and again in f64, beside the anchor's own translation magnitude.
	//
	// tmag is not decoration - it is the region tag, and without it the line is unreadable. The
	// error is multiplicative and the condition number of a fused view x projection grows with the
	// camera's distance from the world origin, so the same code reads err32 ~3e-5 in a room whose
	// coordinates are single digits and ~3e-3 where they are in the thousands. Three rounds of
	// wobble reports were taken without stating the region; this line makes that impossible.
	void report_gauge_selfcheck(gauge_anchor& anchor);

	// Bounded like every other trace here, and re-armed by a pick so the measurement is running
	// exactly when the user is looking at the object they just named.
	u64 m_gauge_trace_generation = 0;
	u32 m_gauge_trace_lines = 0;
	u64 m_gauge_trace_last_frame = umax;
	// One viewport census line per elected camera program per stats window.
	std::unordered_map<u64, u64> m_camvp_viewport_windows;
	// One gauge-keys line per stats window.
	u64 m_gauge_keys_window = umax;

	// The last anchor-derived camera split, kept so a flip with no anchor at all can hold it
	// instead of dropping back to the elected (alternating, half-resolution) gauge. Only matrices
	// are held: the age, the pending-switch confirmation and the per-frame vote are never touched,
	// exactly as apply_gauge_anchor_camera's own comment requires.
	bool m_gauge_cam_hold_valid = false;
	u64 m_gauge_cam_hold_frame = umax;
	remix_rsx::mat4 m_gauge_cam_hold_view{};
	remix_rsx::mat4 m_gauge_cam_hold_projection{};
	remix_rsx::mat4 m_gauge_cam_hold_reference_inverse{};
	remix_rsx::mat4 m_gauge_cam_hold_view_proj_inverse{};
	bool m_gauge_cam_hold_has_vpinv = false;
	f32 m_gauge_cam_hold_position[3] = { 0.f, 0.f, 0.f };

	u32 m_split_attempts = 0;
	std::array<u8, 3> m_reserved_camera_split_attempts{};
	u8 m_frame_camera_roles_admitted = 0;

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

	// Widest bone-index offset either side of palette_base seen in the draw being built. The
	// palette dump reports it so a rig that indexes outside its own palette is visible without
	// having to refuse anything first.
	// palette_base of the rig being built, so the dump can print discovered slots as offsets.
	u32 m_scratch_palette_base = 0;
	u32 m_scratch_palette_rows = 0;
	u32 m_scratch_palette_stride = 0;
	s32 m_scratch_bone_off_min = 0;
	s32 m_scratch_bone_off_max = 0;
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

	// Vertex programs whose bound albedo has been reported once. Joins a 'Remix dump vp=' line
	// to the 'Remix tex=' line for the texture it draws with, which no existing line does.
	std::unordered_set<u64> m_dumped_vp_albedo;

	// Vertex programs already reported by audit_skin_extent. The flag describes a shape, and a
	// shape repeats every frame the rig is on screen; skin_reach_flagged carries the per-draw count.
	std::unordered_set<u64> m_skin_reach_seen;

	// Vertex programs already reported by audit_vertex_extent, same once-per-shape rule.
	std::unordered_set<u64> m_vertex_spread_seen;
	// One drawn-extent census line per vertex program. Separate from m_vertex_spread_seen because
	// that set is keyed on a pre-transform measurement and a program can trip one without the other.
	std::unordered_set<u64> m_drawn_extent_seen;
	// One built-palette dump per vertex program. Separate from the dump-line diagnostics because
	// those run before the skinning gate, where m_scratch_bone_transforms is not yet populated -
	// which is why describe_skinning reads bone0 out of the constants instead of the palette.
	std::unordered_set<u64> m_bone_palette_seen;

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
	// The same draw's extent measured before bones and instance transform, so the drawn-extent
	// census can report the scale the draw actually received rather than only its final size.
	f32 m_streak_raw_extent = 0.f;
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
