#pragma once
#include "Emu/RSX/GSRender.h"

#ifdef _WIN32
#include "Emu/RSX/Remix/RemixRuntime.h"
#include "Emu/RSX/Remix/RemixTransforms.h"

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

	// True when this draw is 2D / pre-projected and must not reach Remix.
	bool is_screen_space_draw() const;

	// Per-draw object-to-world transform. False means "no transform available, use identity".
	bool per_draw_transform(remixapi_Transform& out) const;

	// Stage B: one subdraw of the current draw clause.
	void submit_subdraw();

	// Vertex-program identification, cached per ucode hash so the ucode walk runs once
	// per program rather than once per draw.
	const remix_rsx::vp_fingerprint& fingerprint_for(u64 vp_hash);

	// RPCS3_REMIX_DUMP=1: one line per unique vertex program, the permanent diagnostic.
	void dump_vertex_program(u32 vertex_count, u32 index_count);

	void reap_idle_meshes();
	void log_stats();

	remix_rsx::runtime m_remix;
	bool m_remix_ok = false;

	remixapi_MeshHandle m_debug_mesh = nullptr;
	remixapi_LightHandle m_debug_light = nullptr;

	rsx::vertex_input_layout m_vertex_layout{};

	std::unordered_map<u64, mesh_entry> m_meshes;
	std::unordered_set<u64> m_poisoned;

	std::unordered_map<u64, remix_rsx::vp_fingerprint> m_vp_fingerprints;
	std::unordered_set<u64> m_vp_dumped;

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

	stat_counters m_stats{};
	u64 m_frame_counter = 0;
#endif
};
