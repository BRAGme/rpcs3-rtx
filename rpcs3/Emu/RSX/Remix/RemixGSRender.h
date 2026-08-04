#pragma once
#include "Emu/RSX/GSRender.h"

#ifdef _WIN32
#include "Emu/RSX/Overlays/overlay_controls.h"
#include "Emu/RSX/Remix/RemixCompositor.h"
#include "Emu/RSX/Remix/RemixRuntime.h"
#include "Emu/RSX/Remix/RemixTextures.h"
#include "Emu/RSX/Remix/RemixTransforms.h"
#include "Emu/RSX/Remix/RemixVertexDecode.h"

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
		u64 world_applied = 0;
		u64 world_fallback = 0;
		// Of the world_fallback population, how many were refused rather than drawn at the
		// identity. Counted separately so world_fallback stays comparable across every run
		// taken before the refusal existed. RPCS3_REMIX_DRAWNOWORLD=1 drives this back to 0.
		u64 world_refused = 0;
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
		u64 tex_bound = 0;
		u64 tex_none = 0;
		// Of the tex_bound population, how many left with real texcoords rather than the
		// (0,0) every vertex used to carry. uv_applied far below tex_bound means the albedo is
		// there and the coordinates are not, which renders as one flat colour per draw.
		u64 uv_applied = 0;
		u64 uv_none = 0;
		// Resolved from an attribute other than 8+unit, i.e. the convention did not hold.
		u64 uv_fallback = 0;
		u64 ui_draws = 0;
		u64 ui_skipped = 0;
		u64 ui_no_colour = 0;
		u64 ui_render_target = 0;
		u64 skin_submitted = 0;
		u64 skin_skipped = 0;
		u64 skin_bones_max = 0;
		// A program whose ucode carries skinning the recogniser cannot prove it understands
		// (a blend rig partially matched as single-bone). Refused, never mis-skinned: a missing
		// character is an acceptable result, an exploded one is not.
		u64 skin_unrecognised = 0;
		u64 skip_vp = 0;
		u64 cat_sky = 0;
		u64 cat_hidden = 0;
		u64 cat_particle = 0;
		u64 cat_decal = 0;
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
	};

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

	// Lowest referenced, enabled, 2D fragment texture unit for this draw, or -1.
	int albedo_texture_unit() const;

	// True when any referenced, enabled 2D fragment texture unit samples an address the RSX has
	// bound as a colour or depth surface: the title reading back its own framebuffer.
	bool samples_bound_surface() const;

	// Fills m_scratch_vertices' texcoords from the vertex attribute that feeds the albedo unit.
	// Must run before the mesh content hash is taken: the texcoords are part of the vertex data
	// the hash covers, and two draws that share positions but not UVs are different meshes.
	void apply_texcoords(u32 unit, const remix_rsx::texture_entry& entry,
		const rsx::fragment_texture& tex, u32 first_vertex, u32 vertex_count);

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

	// RPCS3_REMIX_UIPROBE=1: a fixed known pattern through the same path, so the fork call can
	// be judged on its own before any real UI data is wired through it.
	void draw_ui_probe();

	// Per-draw object-to-world transform. False means "no transform available, use identity".
	bool per_draw_transform(remixapi_Transform& out) const;

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

	// Vertex programs already reported by the indexed-constant refusal census.
	std::unordered_set<u64> m_indexed_census_seen;

	// Vertex programs already reported by the screen-space refusal census.
	std::unordered_set<u64> m_screen_census_seen;

	remix_rsx::texture_cache m_textures;
	remix_rsx::compositor m_compositor;

	// True once begin_frame() has run for the frame currently being built.
	bool m_compositor_open = false;

	// Diagnostic accumulator, reset every time it is logged.
	ui_biggest_draw m_ui_biggest{};

	// How many RPCS3_REMIX_UIDUMP lines have been emitted so far.
	u32 m_ui_dumped = 0;

	std::unordered_map<u64, remix_rsx::vp_fingerprint> m_vp_fingerprints;
	std::unordered_set<u64> m_vp_dumped;
	std::unordered_set<u64> m_dumped_textures;

	// Identification of the draw clause currently being submitted. Set once in end(),
	// because the vertex program cannot change between subdraws of one clause.
	u64 m_current_vp_hash = 0;
	const remix_rsx::vp_fingerprint* m_current_fingerprint = nullptr;

	camera_candidate m_frame_candidate{};
	camera_candidate m_active_camera{};
	u32 m_split_attempts = 0;

	// Reused across subdraws to keep the hot path allocation free.
	std::vector<remixapi_HardcodedVertex> m_scratch_vertices;
	std::vector<u32> m_scratch_indices;
	std::vector<u32> m_scratch_indices_alt;
	std::vector<u8> m_scratch_index_bytes;

	// Skinning scratch. 'slots' is the distinct set of palette offsets this draw touched;
	// 'indices' is the per-vertex index into it, which is what Remix's blendIndices means.
	std::vector<u32> m_scratch_bone_indices;
	std::vector<f32> m_scratch_bone_weights;
	std::vector<u32> m_scratch_bone_slots;
	std::vector<f32> m_scratch_bone_raw;
	std::vector<remixapi_Transform> m_scratch_bone_transforms;

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
