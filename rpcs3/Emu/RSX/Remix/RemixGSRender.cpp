#include "stdafx.h"
#include "RemixGSRender.h"

#ifdef _WIN32

#include "Emu/Memory/vm.h"
#include "Emu/RSX/Common/BufferUtils.h"
#include "Emu/RSX/Program/ProgramStateCache.h"
#include "Emu/RSX/Remix/RemixVertexDecode.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlays.h"
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

		if (remix_rsx::ui_probe_enabled() && m_remix.fork_features() && !remix_rsx::compositor_disabled())
		{
			draw_ui_probe();
		}

		// rpcs3's own overlay goes on top of the title's 2D draws, then everything 2D for this
		// frame reaches the runtime in one call, before the present.
		if (m_remix.fork_features() && !remix_rsx::compositor_disabled())
		{
			composite_native_overlay();
		}

		submit_compositor();

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

void RemixGSRender::do_local_task(rsx::FIFO::state state)
{
	rsx::thread::do_local_task(state);

	if (state == rsx::FIFO::state::lock_wait)
	{
		// Critical check finished.
		return;
	}

	// A native-UI flip request has to present while the guest thread is stalled waiting on the
	// dialog it just opened - otherwise the dialog is never drawn and the guest never advances.
	// Mirrors GLGSRender::do_local_task.
	if (m_overlay_manager)
	{
		const auto should_ignore = in_begin_end && state != rsx::FIFO::state::empty;

		if ((async_flip_requested & flip_request::native_ui) && !should_ignore && !is_stopped())
		{
			rsx::display_flip_info_t info{};
			info.buffer = current_display_buffer;
			flip(info);
		}
	}
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

const rsx::interleaved_range_info* RemixGSRender::find_attribute_block(u32 index) const
{
	// Every block is searched, not just the first: a bone index or a texcoord routinely lives
	// in a different interleaved block than the position.
	for (const auto* candidate : m_vertex_layout.interleaved_blocks)
	{
		for (const auto& location : candidate->locations)
		{
			if (location.index == index)
			{
				return candidate;
			}
		}
	}

	return nullptr;
}

RemixGSRender::attribute_status RemixGSRender::map_attribute(u32 index, u32 first_vertex, u32 vertex_count, attribute_view& out) const
{
	out = attribute_view{};

	if (index >= 16 || vertex_count == 0)
	{
		return attribute_status::absent;
	}

	if (m_vertex_layout.attribute_placement[index] != rsx::attribute_buffer_placement::persistent)
	{
		// Register-sourced, inline, or not fed at all: there is no strided stream to read.
		return attribute_status::absent;
	}

	const rsx::interleaved_range_info* block = find_attribute_block(index);

	if (!block || block->attribute_stride == 0)
	{
		return attribute_status::absent;
	}

	const auto& info = rsx::method_registers.vertex_arrays_info[index];
	const u32 attr_base = info.offset() & 0x7fffffff;

	if (attr_base < block->base_offset)
	{
		return attribute_status::layout;
	}

	const u32 attr_offset = attr_base - block->base_offset;
	const u32 attr_bytes = remix_rsx::attribute_byte_size(info.type(), info.size());

	if (attr_bytes == 0)
	{
		return attribute_status::layout;
	}

	const u32 stride = block->attribute_stride;

	// The span is this attribute's own: it starts at the block base and ends at the last
	// vertex's copy of this attribute, which is not where ATTR0's span ends.
	const u64 span_base = u64{block->real_offset_address} + (u64{first_vertex} * stride);
	const u64 span_bytes = (u64{vertex_count - 1} * stride) + attr_offset + attr_bytes;

	if ((span_base + span_bytes) > 0xFFFFFFFFull ||
		!vm::check_addr(span_base, vm::page_readable, static_cast<u32>(span_bytes)))
	{
		return attribute_status::memory;
	}

	out.base = vm::_ptr<const u8>(static_cast<u32>(span_base));
	out.stride = stride;
	out.offset = attr_offset;
	out.type = info.type();
	out.size = info.size();

	return attribute_status::ok;
}

bool RemixGSRender::compositor_target(u32& width, u32& height) const
{
	// Measured, not assumed: DrawScreenOverlay composites the buffer 1:1 in output pixels and
	// does not stretch it, so the buffer has to be the window's client size or the UI lands in
	// the top-left corner at the wrong scale. The guest's own surface is only the fallback.
	u32 w = m_frame ? static_cast<u32>(std::max(0, m_frame->client_width())) : 0;
	u32 h = m_frame ? static_cast<u32>(std::max(0, m_frame->client_height())) : 0;

	if (w == 0 || h == 0)
	{
		w = rsx::method_registers.surface_clip_width();
		h = rsx::method_registers.surface_clip_height();
	}

	width = w;
	height = h;

	return w != 0 && h != 0;
}

void RemixGSRender::draw_ui_probe()
{
	u32 width = 0;
	u32 height = 0;

	if (!compositor_target(width, height))
	{
		return;
	}

	m_compositor.begin_frame(width, height);
	m_compositor_open = true;

	const f32 w = static_cast<f32>(width);
	const f32 h = static_cast<f32>(height);

	// A pattern chosen to be unmistakable and to answer three questions at once: is anything
	// composited, is the buffer stretched to the output, and is the channel order BGRA.
	// Opaque red bar across the top (B=0x00 G=0x00 R=0xFF), opaque blue bar down the left.
	m_compositor.draw_quad(0.f, 0.f, w, h * 0.06f, 0.f, 0.f, 1.f, 1.f, nullptr, 0xFF0000FFu, true);
	m_compositor.draw_quad(0.f, 0.f, w * 0.04f, h, 0.f, 0.f, 1.f, 1.f, nullptr, 0xFFFF0000u, true);

	// Half-alpha green block in the middle: proves the alpha path, and shows the scene through it.
	m_compositor.draw_quad(w * 0.35f, h * 0.4f, w * 0.65f, h * 0.6f, 0.f, 0.f, 1.f, 1.f, nullptr, 0x8000FF00u, true);

	// One triangle, so the barycentric rasterizer is exercised too.
	const f32 tx[3] = { w * 0.75f, w * 0.95f, w * 0.85f };
	const f32 ty[3] = { h * 0.80f, h * 0.80f, h * 0.55f };
	const f32 tu[3] = { 0.f, 0.f, 0.f };
	const f32 tv[3] = { 0.f, 0.f, 0.f };
	m_compositor.draw_triangle(tx, ty, tu, tv, nullptr, 0xFFFFFFFFu, true);
}

void RemixGSRender::composite_ui_draw(u32 first_vertex, u32 vertex_count)
{
	u32 width = 0;
	u32 height = 0;

	if (!compositor_target(width, height) || m_scratch_indices.size() < 3)
	{
		++m_stats.ui_skipped;
		return;
	}

	if (!m_compositor_open)
	{
		m_compositor.begin_frame(width, height);
		m_compositor_open = true;
	}

	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	// The decoded positions are the *raw* attribute values, so the program's own matrix chain
	// has to be applied before they mean anything on screen. A program with no chain (the
	// pure screen_space archetype) already hands over post-projection coordinates.
	remix_rsx::mat4 clip = remix_rsx::mat4_identity();

	for (u32 i = 0; i < fp.group_count; ++i)
	{
		remix_rsx::slot_block slots{};

		if (!remix_rsx::read_slot_block(fp.group_base[i], slots))
		{
			++m_stats.ui_skipped;
			return;
		}

		clip = remix_rsx::mat4_multiply(clip, remix_rsx::slots_to_matrix(slots, fp.group_shape[i]));
	}

	if (remix_rsx::mat4 prescale{}; fp.group_count && remix_rsx::build_prescale(fp, prescale))
	{
		clip = remix_rsx::mat4_multiply(prescale, clip);
	}

	if (!remix_rsx::mat4_is_finite(clip))
	{
		++m_stats.ui_skipped;
		return;
	}

	m_scratch_ui_x.clear();
	m_scratch_ui_y.clear();
	m_scratch_ui_x.resize(vertex_count);
	m_scratch_ui_y.resize(vertex_count);

	f32 lo[2] = { +3.4e38f, +3.4e38f };
	f32 hi[2] = { -3.4e38f, -3.4e38f };

	for (u32 i = 0; i < vertex_count; ++i)
	{
		const remixapi_HardcodedVertex& v = m_scratch_vertices[i];
		const f32 p[4] = { v.position[0], v.position[1], v.position[2], 1.f };
		f32 out[4]{};

		for (u32 j = 0; j < 4; ++j)
		{
			out[j] = (p[0] * clip.m[0][j]) + (p[1] * clip.m[1][j]) + (p[2] * clip.m[2][j]) + (p[3] * clip.m[3][j]);
		}

		if (std::isfinite(out[3]) && std::abs(out[3]) > 1e-6f && std::abs(out[3] - 1.f) > 1e-6f)
		{
			out[0] /= out[3];
			out[1] /= out[3];
		}

		if (!std::isfinite(out[0]) || !std::isfinite(out[1]))
		{
			++m_stats.ui_skipped;
			return;
		}

		m_scratch_ui_x[i] = out[0];
		m_scratch_ui_y[i] = out[1];

		lo[0] = std::min(lo[0], out[0]);
		hi[0] = std::max(hi[0], out[0]);
		lo[1] = std::min(lo[1], out[1]);
		hi[1] = std::max(hi[1], out[1]);
	}

	const f32 fw = static_cast<f32>(width);
	const f32 fh = static_cast<f32>(height);

	// The guest authors its 2D against the render surface; the compositor buffer is the
	// window. Pixel-space draws therefore need the ratio between them.
	const f32 clip_w = static_cast<f32>(std::max<u32>(1, rsx::method_registers.surface_clip_width()));
	const f32 clip_h = static_cast<f32>(std::max<u32>(1, rsx::method_registers.surface_clip_height()));

	// Two shapes were observed in M2's dumps and both survive here: normalised device
	// coordinates, and coordinates already in clip pixels.
	const f32 extent = std::max(std::max(std::abs(lo[0]), std::abs(hi[0])), std::max(std::abs(lo[1]), std::abs(hi[1])));

	if (extent <= 1.5f)
	{
		for (u32 i = 0; i < vertex_count; ++i)
		{
			m_scratch_ui_x[i] = (m_scratch_ui_x[i] + 1.f) * 0.5f * fw;
			m_scratch_ui_y[i] = (1.f - m_scratch_ui_y[i]) * 0.5f * fh;
		}
	}
	else if (extent <= (std::max(clip_w, clip_h) * 2.f))
	{
		const f32 sx = fw / clip_w;
		const f32 sy = fh / clip_h;

		for (u32 i = 0; i < vertex_count; ++i)
		{
			m_scratch_ui_x[i] *= sx;
			m_scratch_ui_y[i] *= sy;
		}
	}
	else
	{
		// Neither space. Guessing would put the UI somewhere arbitrary; counting is honest.
		++m_stats.ui_skipped;
		return;
	}

	// --- texture and texcoords ----------------------------------------------------------
	const remix_rsx::texture_entry* entry = nullptr;
	attribute_view uvs{};
	bool have_uv = false;
	f32 uv_scale[2] = { 1.f, 1.f };

	if (const int unit = albedo_texture_unit(); unit >= 0)
	{
		const auto& tex = rsx::method_registers.fragment_textures[unit];
		m_textures.bind(m_remix.api(), tex, m_frame_counter, &entry);

		if (entry && entry->pixels.empty())
		{
			entry = nullptr;
		}

		if (entry)
		{
			// The texcoord set follows the texture unit: ATTR8 is in_tc0.
			have_uv = map_attribute(8 + static_cast<u32>(unit), first_vertex, vertex_count, uvs) == attribute_status::ok;

			if (tex.format() & CELL_GCM_TEXTURE_UN)
			{
				// Unnormalised coordinates are in texels.
				uv_scale[0] = (entry->width != 0) ? (1.f / static_cast<f32>(entry->width)) : 1.f;
				uv_scale[1] = (entry->height != 0) ? (1.f / static_cast<f32>(entry->height)) : 1.f;
			}
		}
	}

	// --- flat tint ----------------------------------------------------------------------
	// Per-vertex interpolation is out of scope; vertex 0's colour is the whole draw's tint.
	u32 tint = 0xFFFFFFFFu;

	if (attribute_view colours{}; map_attribute(3, first_vertex, vertex_count, colours) == attribute_status::ok)
	{
		f32 rgba[4] = { 1.f, 1.f, 1.f, 1.f };

		if (remix_rsx::decode_position(colours.at(0), colours.type, colours.size, rgba))
		{
			const auto channel = [](f32 v)
			{
				return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
			};

			// The compositor works in BGRA; RSX ATTR3 is R,G,B,A in component order.
			tint = channel(rgba[2]) | (channel(rgba[1]) << 8) | (channel(rgba[0]) << 16) | (channel(rgba[3]) << 24);
		}
	}

	const bool clamp_uv = entry && (entry->wrap_u == 0 || entry->wrap_v == 0);

	for (usz t = 0; (t + 2) < m_scratch_indices.size(); t += 3)
	{
		const u32 i0 = m_scratch_indices[t + 0];
		const u32 i1 = m_scratch_indices[t + 1];
		const u32 i2 = m_scratch_indices[t + 2];

		const f32 x[3] = { m_scratch_ui_x[i0], m_scratch_ui_x[i1], m_scratch_ui_x[i2] };
		const f32 y[3] = { m_scratch_ui_y[i0], m_scratch_ui_y[i1], m_scratch_ui_y[i2] };

		f32 u[3] = { 0.f, 0.f, 0.f };
		f32 v[3] = { 0.f, 0.f, 0.f };

		if (have_uv)
		{
			const u32 tri[3] = { i0, i1, i2 };

			for (u32 c = 0; c < 3; ++c)
			{
				f32 uv[4]{};

				if (!remix_rsx::decode_position(uvs.at(tri[c]), uvs.type, uvs.size, uv))
				{
					have_uv = false;
					break;
				}

				u[c] = uv[0] * uv_scale[0];
				v[c] = uv[1] * uv_scale[1];
			}
		}

		m_compositor.draw_triangle(x, y, u, v, have_uv ? entry : nullptr, tint, clamp_uv);
	}

	++m_stats.ui_draws;
}

const remix_rsx::texture_entry* RemixGSRender::overlay_image(const void* key, const u8* rgba, u32 width, u32 height)
{
	if (!key || !rgba || width == 0 || height == 0)
	{
		return nullptr;
	}

	auto it = m_overlay_images.find(key);

	if (it != m_overlay_images.end() && it->second.width == width && it->second.height == height)
	{
		return &it->second;
	}

	remix_rsx::texture_entry entry{};
	entry.width = width;
	entry.height = height;
	entry.wrap_u = 0;
	entry.wrap_v = 0;
	entry.pixels.resize(static_cast<usz>(width) * height * 4);

	// stb hands these over as RGBA8; the compositor is BGRA8. GL papers over the difference
	// with a sampler swizzle (GLOverlays.cpp:229), which a CPU rasterizer cannot do.
	for (usz i = 0; i < (static_cast<usz>(width) * height); ++i)
	{
		entry.pixels[(i * 4) + 0] = rgba[(i * 4) + 2];
		entry.pixels[(i * 4) + 1] = rgba[(i * 4) + 1];
		entry.pixels[(i * 4) + 2] = rgba[(i * 4) + 0];
		entry.pixels[(i * 4) + 3] = rgba[(i * 4) + 3];
	}

	it = m_overlay_images.insert_or_assign(key, std::move(entry)).first;
	return &it->second;
}

void RemixGSRender::composite_overlay_command(const rsx::overlays::compiled_resource::command& cmd, f32 scale_x, f32 scale_y)
{
	const auto& config = cmd.config;
	const auto& verts = cmd.verts;

	if (verts.empty())
	{
		return;
	}

	const auto channel = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
	const u32 tint = channel(config.color.b)
		| (channel(config.color.g) << 8)
		| (channel(config.color.r) << 16)
		| (channel(config.color.a) << 24);

	if (config.clip_region)
	{
		m_compositor.set_clip(config.clip_rect.x1 * scale_x, config.clip_rect.y1 * scale_y,
			config.clip_rect.x2 * scale_x, config.clip_rect.y2 * scale_y);
	}
	else
	{
		m_compositor.clear_clip();
	}

	const rsx::overlays::font* font_ref = (config.texture_ref == rsx::overlays::image_resource_id::font_file) ? config.font_ref : nullptr;
	const remix_rsx::texture_entry* image = nullptr;

	if (config.texture_ref == rsx::overlays::image_resource_id::raw_image)
	{
		if (const auto* info = static_cast<const rsx::overlays::image_info_base*>(config.external_data_ref))
		{
			image = overlay_image(config.external_data_ref, info->get_data(),
				static_cast<u32>(info->w), static_cast<u32>(info->h));
		}
	}
	else if (config.texture_ref != rsx::overlays::image_resource_id::none && !font_ref)
	{
		// game_icon (254) and backbuffer (255) are TODO in GL and VK too; everything from 1 to
		// the end of the standard set comes from resource_config.
		const u32 index = u32{config.texture_ref};

		if (index >= 1 && index < 252 && m_ui_resources.texture_raw_data.size() >= index)
		{
			const auto& res = m_ui_resources.texture_raw_data[index - 1];

			if (res)
			{
				image = overlay_image(res.get(), res->get_data(), static_cast<u32>(res->w), static_cast<u32>(res->h));
			}
		}
	}

	u32 glyph_w = 0;
	u32 glyph_h = 0;
	u32 glyph_pages = 1;
	const u8* glyph_data = nullptr;

	if (font_ref)
	{
		const auto dims = font_ref->get_glyph_data_dimensions();
		glyph_w = dims.width;
		glyph_h = dims.height;
		glyph_pages = std::max(1u, dims.depth);
		glyph_data = font_ref->get_glyph_data().data();
	}

	// Emits one triangle from three overlay vertices, in compositor pixels.
	const auto emit = [&](usz a, usz b, usz c)
	{
		const f32 x[3] = { verts[a].values[0] * scale_x, verts[b].values[0] * scale_x, verts[c].values[0] * scale_x };
		const f32 y[3] = { verts[a].values[1] * scale_y, verts[b].values[1] * scale_y, verts[c].values[1] * scale_y };
		f32 u[3] = { verts[a].values[2], verts[b].values[2], verts[c].values[2] };
		f32 v[3] = { verts[a].values[3], verts[b].values[3], verts[c].values[3] };

		if (glyph_data)
		{
			// OverlayRenderFS.glsl:212 - the integer part of V picks the atlas page.
			const u32 page = std::min(glyph_pages - 1, static_cast<u32>(std::max(0.f, std::trunc(v[0]))));
			const u8* coverage = glyph_data + (static_cast<usz>(page) * glyph_w * glyph_h);

			for (u32 i = 0; i < 3; ++i)
			{
				v[i] -= std::trunc(v[i]);
			}

			// draw_glyph_quad is axis aligned, so text is emitted as the quad it always is.
			const f32 x0 = std::min({ x[0], x[1], x[2] });
			const f32 x1 = std::max({ x[0], x[1], x[2] });
			const f32 y0 = std::min({ y[0], y[1], y[2] });
			const f32 y1 = std::max({ y[0], y[1], y[2] });
			const f32 u0 = std::min({ u[0], u[1], u[2] });
			const f32 u1 = std::max({ u[0], u[1], u[2] });
			const f32 v0 = std::min({ v[0], v[1], v[2] });
			const f32 v1 = std::max({ v[0], v[1], v[2] });

			m_compositor.draw_glyph_quad(x0, y0, x1, y1, u0, v0, u1, v1, coverage, glyph_w, glyph_h, tint);
			return;
		}

		m_compositor.draw_triangle(x, y, u, v, image, tint, true);
	};

	switch (config.primitives)
	{
	case rsx::overlays::primitive_type::quad_list:
	{
		// Disjoint 4-vertex triangle strips (GLOverlays.cpp:371-391).
		for (usz i = 0; (i + 3) < verts.size(); i += 4)
		{
			emit(i + 0, i + 1, i + 2);
			emit(i + 1, i + 3, i + 2);
		}
		break;
	}
	case rsx::overlays::primitive_type::triangle_strip:
	{
		for (usz i = 0; (i + 2) < verts.size(); ++i)
		{
			if (i & 1)
			{
				emit(i + 1, i + 0, i + 2);
			}
			else
			{
				emit(i + 0, i + 1, i + 2);
			}
		}
		break;
	}
	case rsx::overlays::primitive_type::triangle_fan:
	{
		for (usz i = 1; (i + 1) < verts.size(); ++i)
		{
			emit(0, i, i + 1);
		}
		break;
	}
	case rsx::overlays::primitive_type::line_list:
	case rsx::overlays::primitive_type::line_strip:
	{
		// The perf graph is line geometry; a 1px quad per segment is the cheapest honest
		// stand-in for a hardware line.
		const usz step = (config.primitives == rsx::overlays::primitive_type::line_list) ? 2 : 1;

		for (usz i = 0; (i + 1) < verts.size(); i += step)
		{
			const f32 x0 = verts[i].values[0] * scale_x;
			const f32 y0 = verts[i].values[1] * scale_y;
			const f32 x1 = verts[i + 1].values[0] * scale_x;
			const f32 y1 = verts[i + 1].values[1] * scale_y;

			m_compositor.draw_quad(std::min(x0, x1), std::min(y0, y1),
				std::max(x0, x1) + 1.f, std::max(y0, y1) + 1.f, 0.f, 0.f, 1.f, 1.f, nullptr, tint, true);
		}
		break;
	}
	default:
		break;
	}

	m_compositor.clear_clip();
}

void RemixGSRender::composite_native_overlay()
{
	if (!m_overlay_manager)
	{
		return;
	}

	// Reclaim first, exactly as GLPresent.cpp:283-298 does: a disposed view's cached image
	// data must not outlive it.
	if (m_overlay_manager->has_dirty())
	{
		m_overlay_manager->lock_shared();

		std::vector<u32> uids_to_dispose;
		uids_to_dispose.reserve(m_overlay_manager->get_dirty().size());

		for (const auto& view : m_overlay_manager->get_dirty())
		{
			uids_to_dispose.push_back(view->uid);
		}

		m_overlay_manager->unlock_shared();
		m_overlay_manager->dispose(uids_to_dispose);
		m_overlay_images.clear();
	}

	if (!m_overlay_manager->has_visible())
	{
		return;
	}

	u32 width = 0;
	u32 height = 0;

	if (!compositor_target(width, height))
	{
		return;
	}

	if (!m_compositor_open)
	{
		m_compositor.begin_frame(width, height);
		m_compositor_open = true;
	}

	if (!m_ui_resources_loaded)
	{
		m_ui_resources.load_files();
		m_ui_resources_loaded = true;
	}

	std::lock_guard lock(*m_overlay_manager);

	for (const auto& view : m_overlay_manager->get_views())
	{
		if (!view || !view->visible)
		{
			continue;
		}

		// Views are authored against a virtual 1280x720 unless they asked for window space.
		const f32 vw = view->use_window_space ? static_cast<f32>(width) : static_cast<f32>(view->get_virtual_width());
		const f32 vh = view->use_window_space ? static_cast<f32>(height) : static_cast<f32>(view->get_virtual_height());

		if (vw <= 0.f || vh <= 0.f)
		{
			continue;
		}

		const f32 scale_x = static_cast<f32>(width) / vw;
		const f32 scale_y = static_cast<f32>(height) / vh;

		for (const auto& cmd : view->get_compiled().draw_commands)
		{
			composite_overlay_command(cmd, scale_x, scale_y);
		}

		view->update(get_system_time());
	}
}

void RemixGSRender::submit_compositor()
{
	if (!m_remix.fork_features() || remix_rsx::compositor_disabled())
	{
		m_compositor_open = false;
		return;
	}

	if (!m_compositor.dirty())
	{
		m_compositor_open = false;
		return;
	}

	const u32 status = m_compositor.submit(m_remix.api());

	if (status != REMIXAPI_ERROR_CODE_SUCCESS)
	{
		++m_stats.ui_skipped;

		// Loud once: a failing DrawScreenOverlay means the whole 2D path is dead, and a
		// per-frame log would drown the file.
		if (m_stats.ui_skipped == 1)
		{
			rsx_log.error("Remix: DrawScreenOverlay failed (%s), UI compositing is not reaching the runtime",
				remix_rsx::error_name(status));
		}
	}

	m_compositor_open = false;
}

bool RemixGSRender::build_skinning(u32 first_vertex, u32 vertex_count)
{
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;

	attribute_view bones{};

	if (map_attribute(fp.bone_attribute, first_vertex, vertex_count, bones) != attribute_status::ok)
	{
		return false;
	}

	m_scratch_bone_indices.clear();
	m_scratch_bone_indices.resize(vertex_count);
	m_scratch_bone_raw.clear();
	m_scratch_bone_raw.resize(vertex_count);
	m_scratch_bone_slots.clear();
	m_scratch_bone_transforms.clear();

	for (u32 i = 0; i < vertex_count; ++i)
	{
		// The ucode's constants are authored against what the vertex fetch hands the shader,
		// i.e. the *scaled* value, so the index chain has to start from the same number.
		// decode_attribute_raw is used separately below, for the hash, where the stored bytes
		// are exactly what identifies the rigging.
		f32 scaled[4] = {};
		f32 raw[4] = {};

		if (!remix_rsx::decode_position(bones.at(i), bones.type, bones.size, scaled) ||
			!remix_rsx::decode_attribute_raw(bones.at(i), bones.type, bones.size, raw))
		{
			return false;
		}

		u32 offset = 0;

		if (!remix_rsx::evaluate_bone_offset(fp, scaled[fp.bone_component], offset))
		{
			return false;
		}

		m_scratch_bone_raw[i] = raw[fp.bone_component];

		// Remix indexes boneTransforms[] directly, so the palette offsets have to be packed
		// down to 0..count-1. Counts are small (one skeleton), so a linear scan is cheapest.
		u32 dense = umax;

		for (u32 s = 0; s < ::size32(m_scratch_bone_slots); ++s)
		{
			if (m_scratch_bone_slots[s] == offset)
			{
				dense = s;
				break;
			}
		}

		if (dense == umax)
		{
			if (m_scratch_bone_slots.size() >= REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT)
			{
				return false;
			}

			remix_rsx::slot_block slots{};

			if (!remix_rsx::read_slot_block(fp.palette_base + offset, slots))
			{
				return false;
			}

			const remix_rsx::mat4 bone = remix_rsx::slots_to_matrix(slots, fp.palette_shape);

			// A bone that is not a plain affine transform is not a bone; refusing it here is
			// what keeps a mis-read palette from smearing geometry across the world.
			if (!remix_rsx::mat4_is_finite(bone) || !remix_rsx::is_affine(bone, s_world_affine_tolerance))
			{
				return false;
			}

			dense = ::size32(m_scratch_bone_slots);
			m_scratch_bone_slots.push_back(offset);
			m_scratch_bone_transforms.push_back(remix_rsx::to_remix_transform(bone));
		}

		m_scratch_bone_indices[i] = dense;
	}

	if (m_scratch_bone_transforms.empty())
	{
		return false;
	}

	// One bone per vertex at weight 1: that is the shape the observed ucode presents (a single
	// MUL + 3 MAD against one address register, no in_weight attribute). Anything else is not
	// recognised and therefore never reaches here.
	m_scratch_bone_weights.assign(vertex_count, 1.f);

	m_stats.skin_bones_max = std::max(m_stats.skin_bones_max, static_cast<u64>(m_scratch_bone_transforms.size()));
	return true;
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

	if (fp.is_layered())
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

	if (m_active_camera.archetype == remix_rsx::vp_archetype::layered
		|| m_active_camera.archetype == remix_rsx::vp_archetype::skinned_layered)
	{
		if (fp.archetype == remix_rsx::vp_archetype::skinned_layered)
		{
			// The bone matrices already carry the vertex into the space the outer groups
			// expect, so the instance transform is only what sits *below* the view: the
			// groups the layered case would call the world. Two groups means the palette is
			// already world-space and the instance transform is the identity.
			const u32 world_groups = (fp.group_count >= 3) ? (fp.group_count - 2) : 0;
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

			if (remix_rsx::mat4 prescale{}; remix_rsx::build_prescale(fp, prescale))
			{
				world = remix_rsx::mat4_multiply(prescale, world);
			}

			if (!remix_rsx::mat4_is_finite(world) || !remix_rsx::is_affine(world, s_world_affine_tolerance))
			{
				return false;
			}

			out = remix_rsx::to_remix_transform(world);
			return true;
		}

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

	// One-run bisector: drop everything one program draws and see what disappears.
	if (const u64 skip_hash = remix_rsx::skip_vp_hash(); skip_hash != 0 && skip_hash == m_current_vp_hash)
	{
		++m_stats.skip_vp;
		return;
	}

	// --- screen-space classification ----------------------------------------------------
	// When dumping, the skip is deferred until after the decode so that the 2D programs
	// still get a dump line: their ATTR0 bounding box is the corroborating evidence.
	const bool screen_space = !remix_rsx::keep_ui_enabled() && is_screen_space_draw();

	// The title's 2D draws are no longer thrown away: they are rasterized into the overlay
	// buffer, which needs their positions, so the pre-decode early-out only applies when the
	// compositor is unavailable.
	const bool compositing = m_remix.fork_features() && !remix_rsx::compositor_disabled();

	if (screen_space && !remix_rsx::dump_enabled() && !compositing)
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
	// Cheap gate only; the real mapping (and its memory validation) runs once the vertex
	// range is known, through map_attribute().
	if (const rsx::interleaved_range_info* block = find_attribute_block(0); !block || block->attribute_stride == 0)
	{
		++m_stats.skip_layout;
		return;
	}

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
	const u32 first_vertex = index_base ? rsx::get_index_from_base(min_index, index_base) : min_index;

	attribute_view positions{};

	switch (map_attribute(0, first_vertex, vertex_count, positions))
	{
	case attribute_status::ok:
		break;
	case attribute_status::memory:
		++m_stats.skip_memory;
		return;
	default:
		++m_stats.skip_layout;
		return;
	}

	m_scratch_vertices.clear();
	m_scratch_vertices.resize(vertex_count);

	for (u32 i = 0; i < vertex_count; ++i)
	{
		f32 position[4] = {};

		if (!remix_rsx::decode_position(positions.at(i), positions.type, positions.size, position))
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

		if (compositing)
		{
			composite_ui_draw(first_vertex, vertex_count);
		}

		return;
	}

	// --- skinning -----------------------------------------------------------------------
	const remix_rsx::vp_fingerprint& fp = *m_current_fingerprint;
	bool skinned = false;

	if (fp.archetype == remix_rsx::vp_archetype::skinned_layered)
	{
		if (remix_rsx::noskin_enabled() || !m_remix.fork_features() || !build_skinning(first_vertex, vertex_count))
		{
			++m_stats.skin_skipped;
			return;
		}

		skinned = true;
	}
	else if (fp.indexed_const && !fp.has_outer())
	{
		// An indexed-constant program the recogniser did not resolve. Submitting it means
		// drawing it at identity, which is exactly what parked skinned meshes at the world
		// origin in M2/M3. Skipped, and counted, instead.
		++m_stats.skin_skipped;
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

		if (skinned)
		{
			// The rigging is part of the mesh's identity: the same geometry bound to a
			// different skeleton must not share a handle. The bone *matrices* are deliberately
			// not folded in - they change every animation frame, and folding them would churn
			// the mesh cache exactly the way CPU baking would.
			for (const f32 raw : m_scratch_bone_raw)
			{
				hash = rpcs3::hash64(hash, std::bit_cast<u32>(raw));
			}

			for (const u32 index : m_scratch_bone_indices)
			{
				hash = rpcs3::hash64(hash, index);
			}
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
		surface.skinning_hasvalue = skinned ? 1u : 0u;

		if (skinned)
		{
			surface.skinning_value.bonesPerVertex = 1;
			surface.skinning_value.blendWeights_values = m_scratch_bone_weights.data();
			surface.skinning_value.blendWeights_count = ::size32(m_scratch_bone_weights);
			surface.skinning_value.blendIndices_values = m_scratch_bone_indices.data();
			surface.skinning_value.blendIndices_count = ::size32(m_scratch_bone_indices);
		}

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

	remixapi_InstanceInfoBoneTransformsEXT bone_transforms{};

	remixapi_InstanceInfo instance{};
	instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.pNext = nullptr;
	instance.categoryFlags = 0;
	instance.mesh = it->second.handle;
	instance.transform = transform;
	instance.doubleSided = 1;

	if (skinned)
	{
		// Read per draw, not per clause: a title that uploads a new palette between draw calls
		// of one clause gets the pose it asked for.
		bone_transforms.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
		bone_transforms.pNext = nullptr;
		bone_transforms.boneTransforms_values = m_scratch_bone_transforms.data();
		bone_transforms.boneTransforms_count = ::size32(m_scratch_bone_transforms);
		instance.pNext = &bone_transforms;
	}

	const u32 status = remix_rsx::guarded_draw_instance(api.DrawInstance, &instance);
	if (status != REMIXAPI_ERROR_CODE_SUCCESS)
	{
		m_poisoned.insert(hash);
		++m_stats.skip_poisoned;
		return;
	}

	++m_stats.draws_submitted;

	if (skinned)
	{
		++m_stats.skin_submitted;
	}
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

	if (fp.skinned)
	{
		// The line that settles the open question: which attribute carries the bone index,
		// which component of it, and what is applied on the way to the address register.
		fmt::append(groups, " | skin palette=c[%u+a] shape=%s attr=ATTR%u.%c resolved=%d ops=%u",
			fp.palette_base,
			remix_rsx::shape_name(fp.palette_shape),
			fp.bone_attribute,
			"xyzw"[fp.bone_component & 3],
			fp.bone_resolved ? 1 : 0,
			fp.bone_op_count);

		for (u32 i = 0; i < fp.bone_op_count; ++i)
		{
			const remix_rsx::bone_index_op& op = fp.bone_ops[i];

			switch (op.op)
			{
			case remix_rsx::bone_index_op::kind::floor:
				groups += " floor";
				break;
			case remix_rsx::bone_index_op::kind::scale:
				fmt::append(groups, " *c%u.%c", op.mul_slot, "xyzw"[op.mul_component & 3]);
				break;
			case remix_rsx::bone_index_op::kind::affine:
				if (op.mul_slot == umax)
				{
					fmt::append(groups, " +c%u.%c", op.add_slot, "xyzw"[op.add_component & 3]);
				}
				else
				{
					fmt::append(groups, " *c%u.%c+c%u.%c", op.mul_slot, "xyzw"[op.mul_component & 3],
						op.add_slot, "xyzw"[op.add_component & 3]);
				}
				break;
			}
		}

		if (fp.skin_note[0])
		{
			fmt::append(groups, " (%s)", fp.skin_note);
		}
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
		"skip screen=%llu immediate=%llu inline=%llu volatile=%llu reg_attr0=%llu prim=%llu restart=%llu instanced=%llu layout=%llu mem=%llu decode=%llu poison=%llu vp=%llu | "
		"skin_submitted=%llu skin_skipped=%llu skin_bones_max=%llu | "
		"tex_bound=%llu tex_none=%llu tex_live=%llu tex_created=%llu tex_destroyed=%llu tex_hits=%llu tex_deferred=%llu tex_unreadable=%llu tex_unsupported=%llu tex_rehashed=%llu mat_created=%llu | "
		"ui_draws=%llu ui_skipped=%llu ui_prims=%llu ui_frames=%llu",
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
		m_stats.skip_vp,
		m_stats.skin_submitted,
		m_stats.skin_skipped,
		m_stats.skin_bones_max,
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
		tex.materials,
		m_stats.ui_draws,
		m_stats.ui_skipped,
		m_compositor.draws(),
		m_compositor.frames());
}

#endif
