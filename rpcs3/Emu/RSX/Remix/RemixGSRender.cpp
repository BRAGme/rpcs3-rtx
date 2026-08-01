#include "stdafx.h"
#include "RemixGSRender.h"

#ifdef _WIN32

#include "Emu/Memory/vm.h"
#include "Emu/RSX/Common/BufferUtils.h"
#include "Emu/RSX/Program/ProgramStateCache.h"
#include "Emu/RSX/Remix/RemixVertexDecode.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/rsx_utils.h"
#include "util/fnv_hash.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <variant>

namespace
{
	// Frames a mesh may go unreferenced before its handle is released.
	constexpr u64 s_mesh_idle_frames = 300;

	// How often the stats line is emitted, in flips.
	constexpr u64 s_stats_interval_flips = 120;

	// Sanity ceiling on a single submitted mesh. Guards against a malformed draw clause
	// turning into a multi-gigabyte allocation.
	constexpr u32 s_max_vertices_per_mesh = 0x40000;
	constexpr u32 s_max_indices_per_mesh = 0x100000;

	// Archetype B has to invert and split a 4x4 to score a candidate. Every draw of a fused
	// program yields the same projection, so a handful of attempts per frame is plenty.
	constexpr u32 s_max_split_attempts_per_frame = 32;

	// Tolerance on the perspective row of a derived world transform.
	constexpr f32 s_world_affine_tolerance = 0.02f;

	constexpr remixapi_Transform s_identity_transform =
	{ {
		{ 1.f, 0.f, 0.f, 0.f },
		{ 0.f, 1.f, 0.f, 0.f },
		{ 0.f, 0.f, 1.f, 0.f },
	} };
}

#endif

u64 RemixGSRender::get_cycles()
{
	return thread_ctrl::get_cycles(static_cast<named_thread<RemixGSRender>&>(*this));
}

RemixGSRender::RemixGSRender(utils::serial* ar) noexcept : GSRender(ar)
{
}

void RemixGSRender::on_init_thread()
{
	GSRender::on_init_thread();

#ifdef _WIN32
	// Init, submit and present all have to happen on this thread.
	if (!m_frame)
	{
		rsx_log.error("Remix: no game window available, rendering is disabled");
		return;
	}

	if (!m_remix.initialize(m_frame->handle()))
	{
		rsx_log.error("Remix: runtime unavailable, degrading to a no-op renderer");
		return;
	}

	if (!create_debug_scene())
	{
		rsx_log.error("Remix: debug scene setup failed, degrading to a no-op renderer");
		m_remix.shutdown();
		return;
	}

	m_remix_ok = true;
	rsx_log.success("Remix: renderer is live (far plane %.1f)", static_cast<f64>(remix_rsx::hardcoded_far_plane()));
#endif
}

void RemixGSRender::on_exit()
{
#ifdef _WIN32
	if (m_remix.ok())
	{
		const auto& api = m_remix.api();

		for (const auto& [hash, entry] : m_meshes)
		{
			if (entry.handle)
			{
				remix_rsx::guarded_destroy_mesh(api.DestroyMesh, entry.handle);
			}
		}

		if (m_debug_mesh)
		{
			remix_rsx::guarded_destroy_mesh(api.DestroyMesh, m_debug_mesh);
		}

		if (m_debug_light)
		{
			remix_rsx::guarded_destroy_light(api.DestroyLight, m_debug_light);
		}

		m_textures.destroy_all(api);
	}

	m_debug_mesh = nullptr;
	m_debug_light = nullptr;
	m_meshes.clear();
	m_poisoned.clear();
	m_remix_ok = false;

	// Tear the runtime down before the base class lets go of the window it presents to.
	m_remix.shutdown();
#endif

	GSRender::on_exit();
}

void RemixGSRender::flip(const rsx::display_flip_info_t& info)
{
#ifdef _WIN32
	if (m_remix_ok)
	{
		submit_camera();

		const u32 status = remix_rsx::guarded_present(m_remix.api().Present, nullptr);
		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.error("Remix: Present failed (%s)", remix_rsx::error_name(status));
		}

		// Latch this frame's winner for the next frame. The camera submitted above and the
		// world transforms used during the frame therefore always share one reference.
		m_active_camera = m_frame_candidate;
		m_frame_candidate = camera_candidate{};
		m_split_attempts = 0;

		++m_frame_counter;
		m_textures.begin_frame();
		reap_idle_meshes();
		m_textures.reap(m_remix.api(), m_frame_counter);
		log_stats();
	}
#endif

	// Always run the base flip: gs_frame::flip is what shows the window on the first frame
	// and what drives the FPS readout in the title bar.
	GSRender::flip(info);

	// GSRender::flip only presents the window. rsx::thread::flip is what clears
	// async_flip_requested (without it every later guest flip request is dropped), stamps
	// last_host_flip_timestamp (the overlay refresh rate limit reads it) and clears the
	// display interrupt so do_local_task runs every 64th cycle instead of every cycle.
	// GL and VK both call it explicitly (GLPresent.cpp:518, VKPresent.cpp:966); GSRender::flip
	// does not.
	rsx::thread::flip(info);
}

void RemixGSRender::end()
{
#ifdef _WIN32
	if (!m_remix_ok || skip_current_frame || !m_graphics_state.test(rsx::rtt_config_valid) || cond_render_ctrl.disable_rendering())
	{
		execute_nop_draw();
		rsx::thread::end();
		return;
	}

	// Fills current_vp_metadata (referenced input mask) and the vertex program ucode.
	analyse_current_rsx_pipeline();

	// The vertex program cannot change between subdraws of one clause, so identify it once.
	m_current_vp_hash = current_vertex_program.data.empty()
		? 0
		: u64{program_hash_util::vertex_program_utils::get_vertex_program_ucode_hash(current_vertex_program)};
	m_current_fingerprint = &fingerprint_for(m_current_vp_hash);

	auto& draw_call = rsx::method_registers.current_draw_clause;

	draw_call.begin();
	u32 sub_index = 0;

	do
	{
		const rsx::flags32_t vertex_state_mask = rsx::vertex_base_changed | rsx::vertex_arrays_changed;
		const rsx::flags32_t vertex_state = (sub_index == 0)
			? rsx::vertex_arrays_changed
			: draw_call.execute_pipeline_dependencies(m_ctx) & vertex_state_mask;

		if (vertex_state & rsx::vertex_arrays_changed)
		{
			m_draw_processor.analyse_inputs_interleaved(m_vertex_layout, current_vp_metadata);
		}
		else if (vertex_state & rsx::vertex_base_changed)
		{
			const u32 vertex_base_offset = rsx::method_registers.vertex_data_base_offset();
			for (auto& block : m_vertex_layout.interleaved_blocks)
			{
				block->vertex_range.second = 0;
				block->real_offset_address = rsx::get_address(rsx::get_vertex_offset_from_base(vertex_base_offset, block->base_offset), block->memory_location);
			}
		}

		++m_stats.draws_seen;

		if (m_vertex_layout.validate())
		{
			submit_subdraw();
		}
		else
		{
			++m_stats.skip_layout;
		}

		++sub_index;

		if (draw_call.is_trivial_instanced_draw && !m_current_fingerprint->has_outer())
		{
			// Instance-only repeats differ solely by transform constants. Without a known
			// slot group they would all land on the same identity transform, so drop them.
			m_stats.skip_instanced += draw_call.pass_count() - 1;
			draw_call.end();
		}
	}
	while (draw_call.next());

	rsx::thread::end();
#else
	execute_nop_draw();
	rsx::thread::end();
#endif
}

#ifdef _WIN32

bool RemixGSRender::create_debug_scene()
{
	const auto& api = m_remix.api();

	// Sphere light, straight from the official remixapi_example_c.c.
	const f32 origin_light[3] = { 0.f, -1.f, 0.f };
	place_debug_light(origin_light);

	if (!m_debug_light)
	{
		return false;
	}

	// Debug triangle at z = 10, in front of the hardcoded camera at the origin.
	const auto make_vertex = [](f32 x, f32 y, f32 z)
	{
		remixapi_HardcodedVertex v{};
		v.position[0] = x;
		v.position[1] = y;
		v.position[2] = z;
		v.normal[0] = 0.f;
		v.normal[1] = 0.f;
		v.normal[2] = -1.f;
		v.texcoord[0] = 0.f;
		v.texcoord[1] = 0.f;
		v.color = 0xFFFFFFFF;
		return v;
	};

	const remixapi_HardcodedVertex verts[3] =
	{
		make_vertex(5.f, -5.f, 10.f),
		make_vertex(0.f, 5.f, 10.f),
		make_vertex(-5.f, -5.f, 10.f),
	};

	remixapi_MeshInfoSurfaceTriangles triangles{};
	triangles.vertices_values = verts;
	triangles.vertices_count = 3;
	triangles.indices_values = nullptr;
	triangles.indices_count = 0;
	triangles.skinning_hasvalue = 0;
	triangles.material = nullptr;

	remixapi_MeshInfo mesh_info{};
	mesh_info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	mesh_info.pNext = nullptr;
	mesh_info.hash = 0x1;
	mesh_info.surfaces_values = &triangles;
	mesh_info.surfaces_count = 1;

	const u32 status = remix_rsx::guarded_create_mesh(api.CreateMesh, &mesh_info, &m_debug_mesh);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS || !m_debug_mesh)
	{
		rsx_log.error("Remix: CreateMesh failed for the debug triangle (%s)", remix_rsx::error_name(status));
		return false;
	}

	return true;
}

void RemixGSRender::submit_debug_scene()
{
	const auto& api = m_remix.api();

	const int width = m_frame ? m_frame->client_width() : 0;
	const int height = m_frame ? m_frame->client_height() : 0;

	// Hardcoded camera. Deriving one from RSX state is explicitly out of scope here.
	remixapi_CameraInfoParameterizedEXT camera_ext{};
	camera_ext.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
	camera_ext.pNext = nullptr;
	camera_ext.position = { 0.f, 0.f, 0.f };
	camera_ext.forward = { 0.f, 0.f, 1.f };
	camera_ext.up = { 0.f, 1.f, 0.f };
	camera_ext.right = { 1.f, 0.f, 0.f };
	camera_ext.fovYInDegrees = 70.f;
	camera_ext.aspect = (width > 0 && height > 0) ? (static_cast<f32>(width) / static_cast<f32>(height)) : (16.f / 9.f);
	camera_ext.nearPlane = 0.1f;
	camera_ext.farPlane = remix_rsx::hardcoded_far_plane();

	remixapi_CameraInfo camera_info{};
	camera_info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
	camera_info.pNext = &camera_ext;

	remix_rsx::guarded_setup_camera(api.SetupCamera, &camera_info);

	if (m_debug_mesh)
	{
		remixapi_InstanceInfo instance{};
		instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		instance.pNext = nullptr;
		instance.categoryFlags = 0;
		instance.mesh = m_debug_mesh;
		instance.transform = s_identity_transform;
		instance.doubleSided = 1;

		remix_rsx::guarded_draw_instance(api.DrawInstance, &instance);
	}

	if (m_debug_light)
	{
		remix_rsx::guarded_draw_light_instance(api.DrawLightInstance, m_debug_light);
	}
}

void RemixGSRender::place_debug_light(const f32 (&position)[3])
{
	const auto& api = m_remix.api();

	if (m_debug_light)
	{
		remix_rsx::guarded_destroy_light(api.DestroyLight, m_debug_light);
		m_debug_light = nullptr;
	}

	remixapi_LightInfoSphereEXT sphere_light{};
	sphere_light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
	sphere_light.pNext = nullptr;
	sphere_light.position = { position[0], position[1], position[2] };
	sphere_light.radius = remix_rsx::debug_light_radius();
	sphere_light.shaping_hasvalue = 0;
	// Zero-init leaves this at 0, but the runtime's own default is 1.0
	// (rtx_lights.h kVolumetricRadianceScaleDefaultValue). At 0 the light contributes
	// nothing volumetrically.
	sphere_light.volumetricRadianceScale = 1.f;

	const f32 radiance = remix_rsx::debug_light_radiance();

	remixapi_LightInfo light_info{};
	light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
	light_info.pNext = &sphere_light;
	light_info.hash = 0x3;
	// Neutral white. The 2x green came from NVIDIA's sample app ({100, 200, 100}); the runtime
	// gives radiance no per-channel semantics, so it only tinted every judgement green.
	// The default is deliberately not raised to compensate for the ~1.7x luma drop:
	// RPCS3_REMIX_LIGHTRADIANCE covers it, and the right value is scene-scale dependent.
	light_info.radiance = { radiance, radiance, radiance };

	const u32 status = remix_rsx::guarded_create_light(api.CreateLight, &light_info, &m_debug_light);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS || !m_debug_light)
	{
		rsx_log.error("Remix: CreateLight failed (%s)", remix_rsx::error_name(status));
		m_debug_light = nullptr;
	}
}

void RemixGSRender::submit_camera()
{
	if (!m_active_camera.valid || remix_rsx::nocam_enabled())
	{
		++m_stats.cam_fallback;
		submit_debug_scene();
		return;
	}

	++m_stats.cam_resolved;

	const auto& api = m_remix.api();

	remixapi_CameraInfo camera_info{};
	camera_info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
	camera_info.pNext = nullptr;
	camera_info.type = REMIXAPI_CAMERA_TYPE_WORLD;
	remix_rsx::to_camera_matrix(m_active_camera.view, camera_info.view);
	remix_rsx::to_camera_matrix(m_active_camera.projection, camera_info.projection);

	const u32 status = remix_rsx::guarded_setup_camera(api.SetupCamera, &camera_info);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS)
	{
		rsx_log.error("Remix: SetupCamera failed (%s)", remix_rsx::error_name(status));
	}

	// Park the debug light on the camera so the derived pose can be judged visually.
	// The title's own lights are out of scope for this milestone.
	place_debug_light(m_active_camera.position);

	if (m_debug_light)
	{
		remix_rsx::guarded_draw_light_instance(api.DrawLightInstance, m_debug_light);
	}
}

bool RemixGSRender::is_screen_space_draw() const
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	if (fp.archetype == remix_rsx::vp_archetype::screen_space)
	{
		return true;
	}

	if (!fp.has_outer())
	{
		// Unknown: keep submitting it rather than guessing it away.
		return false;
	}

	remix_rsx::slot_block slots{};
	if (!remix_rsx::read_slot_block(fp.outer_base(), slots))
	{
		return false;
	}

	const remix_rsx::mat4 outer = remix_rsx::slots_to_matrix(slots, fp.outer_shape());

	// An identity projection block is the PS3 2D convention (Tiny3D) and is also Remix's own
	// primary "this draw has no camera" test.
	if (remix_rsx::mat4_is_identity(outer, 1e-5f))
	{
		return true;
	}

	return remix_rsx::is_orthographic(outer) && !rsx::method_registers.depth_write_enabled();
}

int RemixGSRender::albedo_texture_unit() const
{
	// referenced_textures_mask comes from the fragment ucode disassembly that
	// analyse_current_rsx_pipeline() already ran, so it costs nothing here. GL and VK iterate
	// the same mask (GLDraw.cpp:301, VKDraw.cpp:286) rather than trusting enabled() alone.
	u32 mask = current_fp_metadata.referenced_textures_mask;

	for (u32 unit = 0; mask; mask >>= 1, ++unit)
	{
		if (!(mask & 1))
		{
			continue;
		}

		const auto& tex = rsx::method_registers.fragment_textures[unit];

		if (!tex.enabled())
		{
			continue;
		}

		if (tex.get_extended_texture_dimension() != rsx::texture_dimension_extended::texture_dimension_2d)
		{
			// Cube / 3D albedo is out of scope for this milestone.
			continue;
		}

		return static_cast<int>(unit);
	}

	return -1;
}

void RemixGSRender::update_camera_candidate()
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	if (!fp.has_outer())
	{
		return;
	}

	remix_rsx::slot_block slots{};
	if (!remix_rsx::read_slot_block(fp.outer_base(), slots))
	{
		return;
	}

	const f32 clip_w = static_cast<f32>(rsx::method_registers.surface_clip_width());
	const f32 clip_h = static_cast<f32>(rsx::method_registers.surface_clip_height());
	const f32 reference_aspect = (clip_h > 0.f) ? (clip_w / clip_h) : (16.f / 9.f);

	const remix_rsx::mat4 raw = remix_rsx::slots_to_matrix(slots, fp.outer_shape());
	const remix_rsx::mat4 folded = remix_rsx::fold_viewport_z(raw,
		rsx::method_registers.viewport_scale_z(),
		rsx::method_registers.viewport_offset_z());

	if (fp.archetype == remix_rsx::vp_archetype::layered)
	{
		// The title already split its own transform: the outermost group is the projection,
		// the one below it is the view (identity when there are only two groups).
		const f32 score = remix_rsx::score_perspective(folded, reference_aspect);

		if (score <= m_frame_candidate.score)
		{
			return;
		}

		remix_rsx::mat4 view = remix_rsx::mat4_identity();

		if (fp.group_count >= 3)
		{
			remix_rsx::slot_block view_slots{};
			if (!remix_rsx::read_slot_block(fp.group_base[fp.group_count - 2], view_slots))
			{
				return;
			}

			view = remix_rsx::slots_to_matrix(view_slots, fp.group_shape[fp.group_count - 2]);

			if (!remix_rsx::is_affine(view, 0.02f))
			{
				// Not a view: fall back to anchoring the world in view space.
				view = remix_rsx::mat4_identity();
			}
		}

		m_frame_candidate.valid = true;
		m_frame_candidate.score = score;
		m_frame_candidate.archetype = fp.archetype;
		m_frame_candidate.view = view;
		m_frame_candidate.projection = folded;
		m_frame_candidate.has_reference = false;
		m_frame_candidate.group_count = fp.group_count;
		m_frame_candidate.position[0] = 0.f;
		m_frame_candidate.position[1] = 0.f;
		m_frame_candidate.position[2] = 0.f;

		remix_rsx::mat4 view_to_world{};
		if (remix_rsx::mat4_invert(view, view_to_world))
		{
			m_frame_candidate.position[0] = view_to_world.m[3][0];
			m_frame_candidate.position[1] = view_to_world.m[3][1];
			m_frame_candidate.position[2] = view_to_world.m[3][2];
		}

		m_frame_candidate.has_view_proj_inverse = remix_rsx::mat4_invert(
			remix_rsx::mat4_multiply(view, folded), m_frame_candidate.view_proj_inverse);

		return;
	}

	// Fused group: split it into a view and a projection.
	if (remix_rsx::is_affine(folded, 1e-4f) || m_split_attempts >= s_max_split_attempts_per_frame)
	{
		return;
	}

	++m_split_attempts;

	remix_rsx::vp_split split{};
	if (!remix_rsx::split_view_projection(folded, split))
	{
		return;
	}

	const f32 score = remix_rsx::score_perspective(split.projection, reference_aspect);

	if (score <= m_frame_candidate.score)
	{
		return;
	}

	remix_rsx::mat4 reference_inverse{};
	if (!remix_rsx::mat4_invert(folded, reference_inverse))
	{
		return;
	}

	m_frame_candidate.valid = true;
	m_frame_candidate.score = score;
	m_frame_candidate.archetype = fp.archetype;
	m_frame_candidate.view = split.view;
	m_frame_candidate.projection = split.projection;
	m_frame_candidate.has_reference = true;
	m_frame_candidate.reference_inverse = reference_inverse;

	// Row 3 of the inverse view is the camera position in world space.
	remix_rsx::mat4 view_to_world{};
	if (remix_rsx::mat4_invert(split.view, view_to_world))
	{
		m_frame_candidate.position[0] = view_to_world.m[3][0];
		m_frame_candidate.position[1] = view_to_world.m[3][1];
		m_frame_candidate.position[2] = view_to_world.m[3][2];
	}
}

bool RemixGSRender::per_draw_transform(remixapi_Transform& out) const
{
	out = s_identity_transform;

	if (!m_active_camera.valid || remix_rsx::nocam_enabled())
	{
		return false;
	}

	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;
	remix_rsx::mat4 world{};

	if (m_active_camera.archetype == remix_rsx::vp_archetype::layered)
	{
		if (fp.archetype == remix_rsx::vp_archetype::layered
			&& fp.group_count == m_active_camera.group_count
			&& fp.inner_is_input)
		{
			// world = G0 * ... * G(n-3); the last two groups are the view and the projection.
			const u32 world_groups = (fp.group_count >= 3) ? (fp.group_count - 2) : 1;
			world = remix_rsx::mat4_identity();

			for (u32 i = 0; i < world_groups; ++i)
			{
				remix_rsx::slot_block slots{};
				if (!remix_rsx::read_slot_block(fp.group_base[i], slots))
				{
					return false;
				}

				world = remix_rsx::mat4_multiply(world, remix_rsx::slots_to_matrix(slots, fp.group_shape[i]));
			}
		}
		else if (fp.archetype == remix_rsx::vp_archetype::fused && m_active_camera.has_view_proj_inverse)
		{
			// The program folded everything above the vertex attribute into one group, so
			// dividing out the resolved view-projection leaves exactly the world transform -
			// including any vertex decompression the program applies before the matrix.
			remix_rsx::slot_block slots{};
			if (!remix_rsx::read_slot_block(fp.outer_base(), slots))
			{
				return false;
			}

			const remix_rsx::mat4 fused = remix_rsx::fold_viewport_z(
				remix_rsx::slots_to_matrix(slots, fp.outer_shape()),
				rsx::method_registers.viewport_scale_z(),
				rsx::method_registers.viewport_offset_z());

			world = remix_rsx::mat4_multiply(fused, m_active_camera.view_proj_inverse);
		}
		else
		{
			return false;
		}

		// The decompression the program applies before its first matrix has to come first here
		// too, because the vertices submitted to Remix are the raw attribute values.
		if (remix_rsx::mat4 prescale{}; remix_rsx::build_prescale(fp, prescale))
		{
			world = remix_rsx::mat4_multiply(prescale, world);
		}
	}
	else if (m_active_camera.has_reference)
	{
		if (!fp.has_outer())
		{
			return false;
		}

		remix_rsx::slot_block slots{};
		if (!remix_rsx::read_slot_block(fp.outer_base(), slots))
		{
			return false;
		}

		const remix_rsx::mat4 fused = remix_rsx::fold_viewport_z(
			remix_rsx::slots_to_matrix(slots, fp.outer_shape()),
			rsx::method_registers.viewport_scale_z(),
			rsx::method_registers.viewport_offset_z());

		world = remix_rsx::mat4_multiply(fused, m_active_camera.reference_inverse);
	}
	else
	{
		return false;
	}

	if (!remix_rsx::mat4_is_finite(world))
	{
		return false;
	}

	// Normalise out any projective scale before testing for affinity.
	if (const f32 w = world.m[3][3]; std::abs(w) > 1e-6f && std::abs(w - 1.f) > 1e-6f)
	{
		const f32 inv = 1.f / w;
		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				world.m[i][j] *= inv;
			}
		}
	}

	if (!remix_rsx::is_affine(world, s_world_affine_tolerance))
	{
		return false;
	}

	out = remix_rsx::to_remix_transform(world);
	return true;
}

void RemixGSRender::submit_subdraw()
{
	auto& draw_call = rsx::method_registers.current_draw_clause;

	// --- classify and skip -------------------------------------------------------------
	if (draw_call.is_immediate_draw)
	{
		++m_stats.skip_immediate;
		return;
	}

	if (draw_call.command == rsx::draw_command::inlined_array)
	{
		++m_stats.skip_inline_array;
		return;
	}

	if (!m_vertex_layout.volatile_blocks.empty())
	{
		++m_stats.skip_volatile;
		return;
	}

	if (m_vertex_layout.attribute_placement[0] != rsx::attribute_buffer_placement::persistent)
	{
		// ATTR0 comes from a register or is not fed at all: no positions to read.
		++m_stats.skip_register_attr0;
		return;
	}

	const rsx::primitive_type prim = draw_call.primitive;
	if (!remix_rsx::is_supported_primitive(prim))
	{
		++m_stats.skip_primitive;
		return;
	}

	if (rsx::method_registers.restart_index_enabled())
	{
		// Restart sentinels would have to be split into separate meshes.
		++m_stats.skip_restart_index;
		return;
	}

	// --- screen-space classification ----------------------------------------------------
	// When dumping, the skip is deferred until after the decode so that the 2D programs
	// still get a dump line: their ATTR0 bounding box is the corroborating evidence.
	const bool screen_space = !remix_rsx::keep_ui_enabled() && is_screen_space_draw();

	if (screen_space && !remix_rsx::dump_enabled())
	{
		++m_stats.skip_screen_space;
		return;
	}

	if (!screen_space)
	{
		// Every 3D draw is a camera candidate, whether or not it survives the gates below.
		update_camera_candidate();
	}

	// --- locate the interleaved block that feeds ATTR0 ---------------------------------
	const rsx::interleaved_range_info* block = nullptr;
	for (const auto* candidate : m_vertex_layout.interleaved_blocks)
	{
		for (const auto& location : candidate->locations)
		{
			if (location.index == 0)
			{
				block = candidate;
				break;
			}
		}

		if (block)
		{
			break;
		}
	}

	if (!block || block->attribute_stride == 0)
	{
		++m_stats.skip_layout;
		return;
	}

	const auto& attr0 = rsx::method_registers.vertex_arrays_info[0];
	const u32 attr0_base = attr0.offset() & 0x7fffffff;

	if (attr0_base < block->base_offset)
	{
		++m_stats.skip_layout;
		return;
	}

	const u32 attr0_offset = attr0_base - block->base_offset;
	const u32 stride = block->attribute_stride;

	// --- build a u32 triangle list ------------------------------------------------------
	m_scratch_indices.clear();

	u32 min_index = 0;
	u32 max_index = 0;
	u32 index_base = 0;

	const auto command = m_draw_processor.get_draw_command(rsx::method_registers);

	if (const auto* indexed = std::get_if<rsx::draw_indexed_array_command>(&command))
	{
		const rsx::index_array_type type = rsx::method_registers.index_type();
		const u32 type_size = get_index_type_size(type);
		const u32 element_count = draw_call.get_elements_count();
		const bool expandable = remix_rsx::is_buffer_utils_expandable(prim);
		const u32 index_capacity = expandable ? get_index_count(prim, element_count) : element_count;

		if (index_capacity == 0 || index_capacity > s_max_indices_per_mesh)
		{
			++m_stats.skip_layout;
			return;
		}

		m_scratch_index_bytes.resize(static_cast<usz>(index_capacity) * type_size);

		u32 written = 0;
		std::tie(min_index, max_index, written) = write_index_array_data_to_buffer(
			{ reinterpret_cast<std::byte*>(m_scratch_index_bytes.data()), m_scratch_index_bytes.size() },
			indexed->raw_index_buffer,
			type,
			prim,
			false,
			0,
			[](rsx::primitive_type p) { return remix_rsx::is_buffer_utils_expandable(p); });

		if (written == 0 || max_index < min_index)
		{
			++m_stats.skip_layout;
			return;
		}

		// Widen to u32 and rebase onto the decoded vertex block.
		m_scratch_indices.reserve(written);

		if (type == rsx::index_array_type::u16)
		{
			const u16* src = reinterpret_cast<const u16*>(m_scratch_index_bytes.data());
			for (u32 i = 0; i < written; ++i)
			{
				m_scratch_indices.push_back(u32{src[i]} - min_index);
			}
		}
		else
		{
			const u32* src = reinterpret_cast<const u32*>(m_scratch_index_bytes.data());
			for (u32 i = 0; i < written; ++i)
			{
				m_scratch_indices.push_back(src[i] - min_index);
			}
		}

		if (prim == rsx::primitive_type::triangle_strip)
		{
			// Native to GL/VK, so BufferUtils left it as a strip.
			m_scratch_indices_alt.clear();
			remix_rsx::strip_to_list(m_scratch_indices.data(), ::size32(m_scratch_indices), m_scratch_indices_alt);
			m_scratch_indices.swap(m_scratch_indices_alt);
		}

		index_base = rsx::method_registers.vertex_data_base_index();
	}
	else if (std::holds_alternative<rsx::draw_array_command>(command))
	{
		const u32 vertex_count = draw_call.get_elements_count();

		if (vertex_count < 3 || vertex_count > s_max_vertices_per_mesh)
		{
			++m_stats.skip_layout;
			return;
		}

		min_index = draw_call.min_index();
		max_index = (min_index + vertex_count) - 1;

		if (prim == rsx::primitive_type::triangles)
		{
			m_scratch_indices.resize(vertex_count);
			std::iota(m_scratch_indices.begin(), m_scratch_indices.end(), 0u);
		}
		else if (prim == rsx::primitive_type::triangle_strip)
		{
			remix_rsx::strip_to_list(0u, vertex_count, m_scratch_indices);
		}
		else
		{
			// Fan / polygon / quads. The BufferUtils helper only emits u16 indices.
			if (vertex_count > 0xFFFF)
			{
				++m_stats.skip_layout;
				return;
			}

			const u32 index_count = get_index_count(prim, vertex_count);
			if (index_count == 0 || index_count > s_max_indices_per_mesh)
			{
				++m_stats.skip_layout;
				return;
			}

			m_scratch_index_bytes.resize(static_cast<usz>(index_count) * sizeof(u16));
			write_index_array_for_non_indexed_non_native_primitive_to_buffer(
				reinterpret_cast<char*>(m_scratch_index_bytes.data()), prim, vertex_count);

			const u16* src = reinterpret_cast<const u16*>(m_scratch_index_bytes.data());
			m_scratch_indices.reserve(index_count);
			for (u32 i = 0; i < index_count; ++i)
			{
				m_scratch_indices.push_back(u32{src[i]});
			}
		}
	}
	else
	{
		++m_stats.skip_inline_array;
		return;
	}

	if (m_scratch_indices.size() < 3 || (m_scratch_indices.size() % 3) != 0)
	{
		++m_stats.skip_layout;
		return;
	}

	const u32 vertex_count = (max_index - min_index) + 1;
	if (vertex_count == 0 || vertex_count > s_max_vertices_per_mesh)
	{
		++m_stats.skip_layout;
		return;
	}

	// --- decode ATTR0 positions ---------------------------------------------------------
	const rsx::vertex_base_type attr0_type = attr0.type();
	const u32 attr0_size = attr0.size();
	const u32 attr0_bytes = remix_rsx::attribute_byte_size(attr0_type, attr0_size);

	if (attr0_bytes == 0)
	{
		++m_stats.skip_decode;
		return;
	}

	const u32 first_vertex = index_base ? rsx::get_index_from_base(min_index, index_base) : min_index;
	const u64 span_base = u64{block->real_offset_address} + (u64{first_vertex} * stride);
	const u64 span_bytes = (u64{vertex_count - 1} * stride) + attr0_offset + attr0_bytes;

	if ((span_base + span_bytes) > 0xFFFFFFFFull ||
		!vm::check_addr(span_base, vm::page_readable, static_cast<u32>(span_bytes)))
	{
		++m_stats.skip_memory;
		return;
	}

	const u8* block_base = vm::_ptr<const u8>(static_cast<u32>(span_base));

	m_scratch_vertices.clear();
	m_scratch_vertices.resize(vertex_count);

	for (u32 i = 0; i < vertex_count; ++i)
	{
		f32 position[4] = {};

		if (!remix_rsx::decode_position(block_base + (static_cast<usz>(i) * stride) + attr0_offset, attr0_type, attr0_size, position))
		{
			++m_stats.skip_decode;
			return;
		}

		remixapi_HardcodedVertex& v = m_scratch_vertices[i];
		v.position[0] = position[0];
		v.position[1] = position[1];
		v.position[2] = position[2];
		v.normal[0] = 0.f;
		v.normal[1] = 0.f;
		v.normal[2] = 1.f;
		v.texcoord[0] = 0.f;
		v.texcoord[1] = 0.f;
		v.color = 0xFFFFFFFF;
	}

	// Bounds check the index list against what we actually decoded.
	for (const u32 index : m_scratch_indices)
	{
		if (index >= vertex_count)
		{
			++m_stats.skip_layout;
			return;
		}
	}

	if (remix_rsx::dump_enabled())
	{
		dump_vertex_program(vertex_count, ::size32(m_scratch_indices));
	}

	if (screen_space)
	{
		++m_stats.skip_screen_space;
		return;
	}

	// --- albedo material ----------------------------------------------------------------
	// Resolved before the mesh hash because the material binds at CreateMesh time, so two
	// draws that share geometry but not their texture must not share a mesh handle.
	const auto& api = m_remix.api();

	remixapi_MaterialHandle material = nullptr;
	u64 albedo_hash = 0;

	if (m_remix.fork_features())
	{
		if (const int unit = albedo_texture_unit(); unit >= 0)
		{
			const remix_rsx::texture_entry* entry = nullptr;
			material = m_textures.bind(api, rsx::method_registers.fragment_textures[unit], m_frame_counter, &entry);

			if (material && entry)
			{
				albedo_hash = entry->content_hash;
				++m_stats.tex_bound;

				if (remix_rsx::dump_enabled() && m_dumped_textures.insert(albedo_hash).second)
				{
					dump_texture(*entry, rsx::method_registers.fragment_textures[unit], static_cast<u32>(unit));
				}
			}
			else
			{
				++m_stats.tex_none;
			}
		}
		else
		{
			++m_stats.tex_none;
		}
	}

	// --- content hash -> mesh handle ----------------------------------------------------
	usz hash = rpcs3::fnv_seed;

	{
		const u64* words = reinterpret_cast<const u64*>(m_scratch_vertices.data());
		const usz word_count = (m_scratch_vertices.size() * sizeof(remixapi_HardcodedVertex)) / sizeof(u64);
		for (usz i = 0; i < word_count; ++i)
		{
			hash = rpcs3::hash64(hash, words[i]);
		}

		for (const u32 index : m_scratch_indices)
		{
			hash = rpcs3::hash64(hash, index);
		}

		if (m_current_vp_hash)
		{
			hash = rpcs3::hash64(hash, m_current_vp_hash);
		}

		if (albedo_hash)
		{
			hash = rpcs3::hash64(hash, albedo_hash);
		}
	}

	if (hash == 0)
	{
		// Remix treats a zero hash as unset.
		hash = 1;
	}

	if (m_poisoned.contains(hash))
	{
		++m_stats.skip_poisoned;
		return;
	}

	auto it = m_meshes.find(hash);

	if (it == m_meshes.end())
	{
		remixapi_MeshInfoSurfaceTriangles surface{};
		surface.vertices_values = m_scratch_vertices.data();
		surface.vertices_count = m_scratch_vertices.size();
		surface.indices_values = m_scratch_indices.data();
		surface.indices_count = m_scratch_indices.size();
		surface.skinning_hasvalue = 0;
		surface.material = material;

		remixapi_MeshInfo mesh_info{};
		mesh_info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
		mesh_info.pNext = nullptr;
		mesh_info.hash = hash;
		mesh_info.surfaces_values = &surface;
		mesh_info.surfaces_count = 1;

		remixapi_MeshHandle handle = nullptr;
		const u32 status = remix_rsx::guarded_create_mesh(api.CreateMesh, &mesh_info, &handle);

		if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle)
		{
			m_poisoned.insert(hash);
			++m_stats.skip_poisoned;
			return;
		}

		++m_stats.meshes_created;
		it = m_meshes.emplace(hash, mesh_entry{ handle, m_frame_counter }).first;
	}

	it->second.last_used_frame = m_frame_counter;

	remixapi_Transform transform = s_identity_transform;

	if (per_draw_transform(transform))
	{
		++m_stats.world_applied;
	}
	else
	{
		++m_stats.world_fallback;
	}

	remixapi_InstanceInfo instance{};
	instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.pNext = nullptr;
	instance.categoryFlags = 0;
	instance.mesh = it->second.handle;
	instance.transform = transform;
	instance.doubleSided = 1;

	const u32 status = remix_rsx::guarded_draw_instance(api.DrawInstance, &instance);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS)
	{
		m_poisoned.insert(hash);
		++m_stats.skip_poisoned;
		return;
	}

	++m_stats.draws_submitted;
}

const remix_rsx::vp_fingerprint& RemixGSRender::fingerprint_for(u64 vp_hash)
{
	auto it = m_vp_fingerprints.find(vp_hash);

	if (it == m_vp_fingerprints.end())
	{
		it = m_vp_fingerprints.emplace(vp_hash, remix_rsx::scan_vertex_program(current_vertex_program)).first;
	}

	return it->second;
}

void RemixGSRender::dump_vertex_program(u32 vertex_count, u32 index_count)
{
	const u64 vp_hash = m_current_vp_hash;
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	if (!m_vp_dumped.insert(vp_hash).second)
	{
		return;
	}

	// ATTR0 bounding box, straight off the positions we just decoded.
	f32 lo[3] = { +3.4e38f, +3.4e38f, +3.4e38f };
	f32 hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };

	for (const auto& v : m_scratch_vertices)
	{
		for (u32 c = 0; c < 3; ++c)
		{
			lo[c] = std::min(lo[c], v.position[c]);
			hi[c] = std::max(hi[c], v.position[c]);
		}
	}

	std::string groups;

	for (u32 i = 0; i < fp.group_count; ++i)
	{
		remix_rsx::slot_block slots{};
		if (!remix_rsx::read_slot_block(fp.group_base[i], slots))
		{
			continue;
		}

		const remix_rsx::mat4 g = remix_rsx::slots_to_matrix(slots, fp.group_shape[i]);
		fmt::append(groups, " G%u c[%u..%u] %s %s persp=%d",
			i, fp.group_base[i], fp.group_base[i] + 3,
			remix_rsx::shape_name(fp.group_shape[i]),
			remix_rsx::format_matrix(g),
			remix_rsx::classify_perspective(g));
	}

	if (fp.archetype == remix_rsx::vp_archetype::unknown)
	{
		// The ucode is the only thing that can say why identification failed.
		fmt::append(groups, " slice:%s", remix_rsx::describe_position_slice(current_vertex_program));
	}

	const std::string line = fmt::format(
		"Remix dump vp=%016llx arch=%s(%s) groups=%u input=%d prescale=%d(c%u.%u,c%u) consts=%u slice=%u ucode=%u inputs=0x%x | "
		"vtx=%u idx=%u prim=%u bbox=[%.4g %.4g %.4g]..[%.4g %.4g %.4g] | "
		"vp_scale_z=%.6g vp_offset_z=%.6g clip=%ux%u depth_test=%d depth_write=%d blend=%d cull=%d |%s",
		vp_hash,
		remix_rsx::archetype_name(fp.archetype),
		fp.note,
		fp.group_count,
		fp.inner_is_input ? 1 : 0,
		fp.has_prescale ? 1 : 0,
		fp.prescale_scale_slot,
		fp.prescale_scale_component,
		fp.prescale_bias_slot,
		fp.distinct_consts,
		fp.chain_instructions,
		current_vp_metadata.ucode_length,
		u32{current_vp_metadata.referenced_inputs_mask},
		vertex_count,
		index_count,
		static_cast<u32>(rsx::method_registers.current_draw_clause.primitive),
		static_cast<f64>(lo[0]), static_cast<f64>(lo[1]), static_cast<f64>(lo[2]),
		static_cast<f64>(hi[0]), static_cast<f64>(hi[1]), static_cast<f64>(hi[2]),
		static_cast<f64>(rsx::method_registers.viewport_scale_z()),
		static_cast<f64>(rsx::method_registers.viewport_offset_z()),
		u32{rsx::method_registers.surface_clip_width()},
		u32{rsx::method_registers.surface_clip_height()},
		rsx::method_registers.depth_test_enabled() ? 1 : 0,
		rsx::method_registers.depth_write_enabled() ? 1 : 0,
		rsx::method_registers.blend_enabled() ? 1 : 0,
		rsx::method_registers.cull_face_enabled() ? 1 : 0,
		groups);

	rsx_log.notice("%s", line);

	// RPCS3.log is held with an exclusive lock while the emulator runs, so mirror the dump
	// into a file that can be read live.
	if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
	{
		out.write(line + '\n');
	}
}

void RemixGSRender::dump_texture(const remix_rsx::texture_entry& entry, const rsx::fragment_texture& tex, u32 unit)
{
	const std::string line = fmt::format(
		"Remix tex=%016llX fmt=%02x %ux%u unit=%u mips=%u swizzled=%d pitch=%u loc=%u offset=0x%x wrap=%u,%u",
		entry.content_hash,
		u32{tex.format()} & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
		entry.width,
		entry.height,
		unit,
		u32{tex.get_exact_mipmap_count()},
		(tex.format() & CELL_GCM_TEXTURE_LN) ? 0 : 1,
		tex.pitch(),
		u32{tex.location()},
		tex.offset(),
		u32{entry.wrap_u},
		u32{entry.wrap_v});

	rsx_log.notice("%s", line);

	if (fs::file out{ fs::get_executable_dir() + "remix_dump.log", fs::write + fs::create + fs::append })
	{
		out.write(line + '\n');
	}
}

void RemixGSRender::reap_idle_meshes()
{
	if (m_meshes.empty() || m_frame_counter < s_mesh_idle_frames)
	{
		return;
	}

	const u64 cutoff = m_frame_counter - s_mesh_idle_frames;
	const auto& api = m_remix.api();

	for (auto it = m_meshes.begin(); it != m_meshes.end();)
	{
		if (it->second.last_used_frame > cutoff)
		{
			++it;
			continue;
		}

		if (it->second.handle)
		{
			remix_rsx::guarded_destroy_mesh(api.DestroyMesh, it->second.handle);
			++m_stats.meshes_destroyed;
		}

		it = m_meshes.erase(it);
	}
}

void RemixGSRender::log_stats()
{
	if ((m_frame_counter % s_stats_interval_flips) != 0)
	{
		return;
	}

	const remix_rsx::texture_stats& tex = m_textures.stats();

	rsx_log.notice(
		"Remix stats: frame=%llu draws=%llu submitted=%llu meshes_live=%llu created=%llu destroyed=%llu poisoned=%llu | "
		"cam_resolved=%llu cam_fallback=%llu arch=%s world_applied=%llu world_fallback=%llu | "
		"skip screen=%llu immediate=%llu inline=%llu volatile=%llu reg_attr0=%llu prim=%llu restart=%llu instanced=%llu layout=%llu mem=%llu decode=%llu poison=%llu | "
		"tex_bound=%llu tex_none=%llu tex_live=%llu tex_created=%llu tex_destroyed=%llu tex_hits=%llu tex_deferred=%llu tex_unreadable=%llu tex_unsupported=%llu tex_rehashed=%llu mat_created=%llu",
		m_frame_counter,
		m_stats.draws_seen,
		m_stats.draws_submitted,
		static_cast<u64>(m_meshes.size()),
		m_stats.meshes_created,
		m_stats.meshes_destroyed,
		static_cast<u64>(m_poisoned.size()),
		m_stats.cam_resolved,
		m_stats.cam_fallback,
		remix_rsx::archetype_name(m_active_camera.archetype),
		m_stats.world_applied,
		m_stats.world_fallback,
		m_stats.skip_screen_space,
		m_stats.skip_immediate,
		m_stats.skip_inline_array,
		m_stats.skip_volatile,
		m_stats.skip_register_attr0,
		m_stats.skip_primitive,
		m_stats.skip_restart_index,
		m_stats.skip_instanced,
		m_stats.skip_layout,
		m_stats.skip_memory,
		m_stats.skip_decode,
		m_stats.skip_poisoned,
		m_stats.tex_bound,
		m_stats.tex_none,
		static_cast<u64>(m_textures.live()),
		tex.created,
		tex.destroyed,
		tex.hits,
		tex.deferred,
		tex.unreadable,
		tex.unsupported,
		tex.rehashed,
		tex.materials);
}

#endif
