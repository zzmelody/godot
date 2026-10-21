// SPDX-License-Identifier: MIT
#include "virtual_geometry_shading.h"

#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

#include <utility>

namespace RendererRD {
namespace {

Error ensure_words(VirtualGeometryWorkMemory &p_memory, RID &r_buffer, uint32_t &r_capacity, uint64_t p_words, bool p_indirect = false) {
	return p_memory.ensure(r_buffer, r_capacity, p_words, p_indirect);
}

void dispatch_items(RD::ComputeListID p_list, uint32_t p_items, uint32_t p_threads) {
	const uint32_t groups = MAX(1u, (p_items + p_threads - 1) / p_threads);
	RD::get_singleton()->compute_list_dispatch(p_list, MIN(65535u, groups), (groups + 65534) / 65535, 1);
}

} // namespace

VirtualGeometryShading::~VirtualGeometryShading() {
	for (RID snapshot : global_history) {
		if (snapshot.is_valid()) {
			memory.free(snapshot);
		}
	}
	for (RID rid : { scene_buffer, bins_buffer, arguments, draw_arguments, dummy_transforms, material_view, frame_history, previous_sources, batch_parameters, view_status, raster_material_buffer }) {
		if (rid.is_valid()) {
			memory.free(rid);
		}
	}
	init_pipeline.free();
	bin_pipeline.free();
	scan_pipeline.free();
	dispatch_pipeline.free();
	if (init_version.is_valid()) {
		init_shader.version_free(init_version);
	}
	if (bin_version.is_valid()) {
		bin_shader.version_free(bin_version);
	}
	if (scan_version.is_valid()) {
		scan_shader.version_free(scan_version);
	}
	if (dispatch_version.is_valid()) {
		dispatch_shader.version_free(dispatch_version);
	}
}

Error VirtualGeometryShading::prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, RID p_visibility, RID p_geometry, bool p_depth_only, bool p_motion) {
	auto binding_frame = source_bindings.begin();
	depth_only = p_depth_only;
	const bool record_history = !depth_only && p_motion;
	materials.clear();
	raster_material_data.clear();
	material_ids[0].clear();
	material_ids[1].clear();
	material_two_sided.clear();
	material_map.clear();
	sources.clear();
	levels.clear();
	size = p_size;
	uint64_t instances = 0, visible = 0;
	MaterialStorage *storage = MaterialStorage::get_singleton();
	for (const auto &result : p_sources) {
		const auto &batch = result.batch;
		const auto slots = MeshStorage::get_singleton()->mesh_get_virtual_geometry_materials(batch.mesh);
		ERR_FAIL_COND_V(slots.rids.size() != slots.identities.size() || slots.rids.is_empty(), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(instances > 0xffffffu || material_map.size() > UINT32_MAX, ERR_OUT_OF_MEMORY);
		sources.push_back({ uint32_t(instances), uint32_t(material_map.size()), uint32_t(visible), uint32_t(slots.rids.size()) });
		instances += batch.count;
		visible += result.selected_capacity;
		ERR_FAIL_COND_V(instances > 0xffffffu, ERR_OUT_OF_MEMORY);
		for (uint32_t slot = 0; slot < slots.rids.size(); ++slot) {
			const RID material = slots.resolve(slot, batch.surface_materials, batch.material_override, p_default_material);
			ERR_FAIL_COND_V(!storage->owns_material(material), ERR_INVALID_DATA);
			const uint32_t two_sided = depth_only && batch.shadow_double_sided ? 1 : 0;
			const uint32_t *found = material_ids[two_sided].getptr(material);
			uint32_t index;
			if (found) {
				index = *found;
			} else {
				index = materials.size();
				material_ids[two_sided].insert(material, index);
				materials.push_back(material);
				material_two_sided.push_back(two_sided);
			}
			material_map.push_back(index);
		}
	}
	const uint64_t references_word = 4 + instances * 48;
	for (uint32_t index = 0; index < materials.size(); ++index) {
		const RID material = materials[index];
		const auto mode = storage->material_get_cull_mode(material);
		uint32_t cull = 0;
		if (mode == RSE::CULL_MODE_BACK) {
			cull = 1;
		} else if (mode == RSE::CULL_MODE_FRONT) {
			cull = 2;
		}
		if (material_two_sided[index]) {
			cull = 0;
		}
		raster_material_data.push_back(cull);
		const float maximum = storage->material_get_max_vertex_displacement(material);
		uint32_t bits;
		memcpy(&bits, &maximum, sizeof(bits));
		raster_material_data.push_back(bits);
		raster_material_data.push_back(0);
		raster_material_data.push_back(0);
	}
	Error cull_error = ensure_words(memory, raster_material_buffer, raster_material_words, raster_material_data.size());
	if (cull_error != OK) {
		return cull_error;
	}
	if (!raster_material_data.is_empty()) {
		RD::get_singleton()->buffer_update(raster_material_buffer, 0, raster_material_data.size() * 4, raster_material_data.ptr());
	}
	const uint64_t materials_word = references_word + visible * 2;
	const uint64_t batches_word = materials_word + material_map.size();
	const uint64_t total_words = batches_word + uint64_t(p_sources.size()) * 20;
	Error error = ensure_words(memory, scene_buffer, scene_words, total_words);
	if (error != OK) {
		WARN_PRINT_ONCE(vformat("Virtual geometry scene table refused: batches=%s instances=%s references=%s words=%s depth_only=%s.", p_sources.size(), instances, visible, total_words, depth_only));
		return error;
	}
	if (!record_history) {
		if (previous_source_words > 16) {
			memory.free(previous_sources);
			previous_sources = RID();
			previous_source_words = 0;
		}
	}
	error = ensure_words(memory, previous_sources, previous_source_words, record_history ? MAX(uint64_t(1), instances) * 24 : 1);
	if (error != OK) {
		WARN_PRINT_ONCE(vformat("Virtual geometry previous-source table refused: batches=%s instances=%s words=%s depth_only=%s.", p_sources.size(), instances, MAX(uint64_t(1), instances) * 24, depth_only));
		return error;
	}
	const uint32_t header[4] = { 4, uint32_t(references_word), uint32_t(materials_word), uint32_t(materials.size()) };
	RD *rd = RD::get_singleton();
	rd->buffer_update(scene_buffer, 0, sizeof(header), header);
	if (!material_map.is_empty()) {
		rd->buffer_update(scene_buffer, materials_word * 4, material_map.size() * 4, material_map.ptr());
	}
	uint64_t scratch = 2 + uint64_t(materials.size()) * 3;
	if (!materials.is_empty()) {
		ScanLevel level{ uint32_t(materials.size()), 2, 2 + uint32_t(materials.size()) };
		while (true) {
			if (level.count <= 128) {
				levels.push_back(level);
				break;
			}
			const uint32_t blocks = (level.count + 127) / 128;
			level.sums = scratch;
			scratch += blocks;
			levels.push_back(level);
			level = { blocks, level.sums, uint32_t(scratch) };
			scratch += blocks;
		}
	}
	ERR_FAIL_COND_V(scratch > UINT32_MAX, ERR_OUT_OF_MEMORY);
	entries_word = scratch;
	error = ensure_words(memory, bins_buffer, bins_words, depth_only ? 2 : scratch + uint64_t(size.x) * size.y * 3);
	if (error != OK) {
		return error;
	}
	error = ensure_words(memory, arguments, argument_words, MAX(uint64_t(3), uint64_t(materials.size()) * 3), true);
	if (error != OK) {
		return error;
	}
	error = ensure_words(memory, draw_arguments, draw_argument_words, MAX(uint64_t(4), uint64_t(materials.size()) * 4), true);
	if (error != OK) {
		return error;
	}
	if (init_version.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		init_shader.initialize(modes);
		bin_shader.initialize(modes);
		scan_shader.initialize(modes);
		dispatch_shader.initialize(modes);
		init_version = init_shader.version_create();
		bin_version = bin_shader.version_create();
		scan_version = scan_shader.version_create();
		dispatch_version = dispatch_shader.version_create();
		init_pipeline.create_compute_pipeline(init_shader.version_get_shader(init_version, 0));
		bin_pipeline.create_compute_pipeline(bin_shader.version_get_shader(bin_version, 0));
		scan_pipeline.create_compute_pipeline(scan_shader.version_get_shader(scan_version, 0));
		dispatch_pipeline.create_compute_pipeline(dispatch_shader.version_get_shader(dispatch_version, 0));
	}
	if (dummy_transforms.is_null()) {
		dummy_transforms = memory.storage(48);
	}
	ERR_FAIL_COND_V(init_pipeline.get_rid().is_null() || bin_pipeline.get_rid().is_null() || scan_pipeline.get_rid().is_null() || dispatch_pipeline.get_rid().is_null() || dummy_transforms.is_null(), ERR_CANT_CREATE);
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	static_assert(sizeof(BatchParameters) == 176);
	error = ensure_words(memory, batch_parameters, batch_parameter_words, MAX(uint64_t(1), uint64_t(p_sources.size()) * 44));
	if (error != OK) {
		return error;
	}
	batch_parameter_data.resize(p_sources.size());
	for (uint32_t index = 0; index < p_sources.size(); ++index) {
		const auto &batch = p_sources[index].batch;
		BatchParameters &input = batch_parameter_data[index];
		input = {};
		MaterialStorage::store_transform(batch.model, input.model);
		input.source[0] = batch.count;
		input.source[1] = batch.stride;
		input.source[2] = batch.transform_offset;
		input.source[3] = batch.geometry.root_pages;
		memcpy(&input.displacement[0], &batch.max_vertex_displacement, sizeof(float));
		if (record_history) {
			MaterialStorage::store_transform(batch.previous_model, input.previous_model);
			input.previous_info[0] = batch.previous_transform_offset;
			input.previous_info[1] = 1;
		}
	}
	if (!batch_parameter_data.is_empty()) {
		rd->buffer_update(batch_parameters, 0, uint32_t(uint64_t(batch_parameter_data.size()) * sizeof(BatchParameters)), batch_parameter_data.ptr());
	}
	for (uint32_t index = 0; index < p_sources.size(); ++index) {
		const auto &source = p_sources[index];
		sources[index].batch_word = uint32_t(batches_word + uint64_t(index) * 20);
		const RID transforms = source.batch.stride != 0 ? source.batch.transforms : dummy_transforms;
		sources[index].uniforms = source_bindings.get(init_shader.version_get_shader(init_version, 0), { transforms, source.work, scene_buffer, previous_sources, batch_parameters });
		ERR_FAIL_COND_V(sources[index].uniforms.is_null(), ERR_CANT_CREATE);
	}
	if (!materials.is_empty() && !depth_only) {
		bin_uniforms = cache->get_cache(bin_shader.version_get_shader(bin_version, 0), 0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { p_geometry }), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { p_visibility }),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { scene_buffer }), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { bins_buffer }));
		scan_uniforms = cache->get_cache(scan_shader.version_get_shader(scan_version, 0), 0, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { bins_buffer }));
		dispatch_uniforms = cache->get_cache(dispatch_shader.version_get_shader(dispatch_version, 0), 0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { bins_buffer }), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, { arguments }),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, { draw_arguments }));
		ERR_FAIL_COND_V(bin_uniforms.is_null() || scan_uniforms.is_null() || dispatch_uniforms.is_null(), ERR_CANT_CREATE);
	}
	return OK;
}

void VirtualGeometryShading::clear_view() {
	materials_ready = false;
	// Ordinary-only frames must not expose a previous virtual surface through
	// a compositor or a shared lighting input. The mask invalidates all fields.
	if (RD::get_singleton()->texture_is_valid(surfaces[6])) {
		RD::get_singleton()->texture_clear(surfaces[6], Color(), 0, 1, 0, 1);
	}
}

void VirtualGeometryShading::record_sources(Span<VirtualGeometrySelection::Result> p_sources) {
	if (p_sources.is_empty()) {
		return;
	}
	RD *rd = RD::get_singleton();
	RENDER_TIMESTAMP("Virtual Geometry Material Sources Begin");
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, init_pipeline.get_rid());
	for (uint32_t index = 0; index < p_sources.size(); ++index) {
		const auto &source = p_sources[index];
		uint32_t push[22] = { sources[index].first, sources[index].map, uint32_t(source.batch.has_color), uint32_t(source.batch.has_custom), uint32_t(source.batch.shader_uniforms_offset), source.batch.layer_mask,
			sources[index].visible_base, source.selected_capacity, sources[index].material_slots, source.batch.render_flags, source.batch.gi_offset, source.work_word };
		const Rect2 &uv = source.batch.lightmap_uv_scale;
		const float lightmap_uv[4] = { uv.position.x, uv.position.y, uv.size.x, uv.size.y };
		memcpy(push + 12, lightmap_uv, sizeof(lightmap_uv));
		push[16] = uint32_t(source.batch.source.get_id());
		push[17] = uint32_t(source.batch.source.get_id() >> 32);
		push[18] = uint32_t(source.batch.mesh.get_id());
		push[19] = uint32_t(source.batch.mesh.get_id() >> 32);
		push[20] = sources[index].batch_word;
		push[21] = index * 44;
		rd->compute_list_bind_uniform_set(list, sources[index].uniforms, 0);
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_items(list, MAX(source.batch.count, source.selected_capacity), 64);
	}
	rd->compute_list_end();
	RENDER_TIMESTAMP("Virtual Geometry Material Sources End");
}

void VirtualGeometryShading::record_bins() {
	RD *rd = RD::get_singleton();
	if (depth_only) {
		rd->buffer_clear(bins_buffer, 0, 8);
		return;
	}
	rd->buffer_clear(bins_buffer, 0, (2 + materials.size() * 3) * 4);
	if (materials.is_empty()) {
		return;
	}
	RENDER_TIMESTAMP("Virtual Geometry Shading Bins Begin");
	const RD::ComputeListID list = rd->compute_list_begin();
	auto bin_pass = [&](uint32_t p_scatter) {
		const uint32_t push[5] = { uint32_t(size.x), uint32_t(size.y), uint32_t(materials.size()), entries_word, p_scatter };
		rd->compute_list_bind_compute_pipeline(list, bin_pipeline.get_rid());
		rd->compute_list_bind_uniform_set(list, bin_uniforms, 0);
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_items(list, ((size.x + 1) / 2) * ((size.y + 1) / 2), 64);
		rd->compute_list_add_barrier(list);
	};
	bin_pass(0);
	rd->compute_list_bind_compute_pipeline(list, scan_pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, scan_uniforms, 0);
	for (const ScanLevel &level : levels) {
		const uint32_t push[5] = { 0, level.count, level.input, level.output, level.sums };
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_items(list, level.count, 128);
		rd->compute_list_add_barrier(list);
	}
	for (int index = int(levels.size()) - 2; index >= 0; --index) {
		const ScanLevel &level = levels[index];
		const uint32_t push[5] = { 1, level.count, levels[index + 1].output, level.output, 0 };
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_items(list, level.count, 128);
		rd->compute_list_add_barrier(list);
	}
	bin_pass(1);
	const uint32_t count = materials.size();
	rd->compute_list_bind_compute_pipeline(list, dispatch_pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, dispatch_uniforms, 0);
	rd->compute_list_set_push_constant(list, &count, sizeof(count));
	dispatch_items(list, count, 64);
	rd->compute_list_end();
	RENDER_TIMESTAMP("Virtual Geometry Shading Bins End");
}

Error VirtualGeometryShading::prepare_materials(const Ref<RenderSceneBuffersRD> &p_buffers, RID p_geometry, const ViewParameters &p_view, RD::FramebufferFormatID p_raster_format, bool p_voxel) {
	materials_ready = false;
	RD *rd = RD::get_singleton();
	MaterialStorage *storage = MaterialStorage::get_singleton();
	const uint64_t current_frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : global_history_frame + 1;
	previous_global_binding = storage->global_shader_uniforms_get_storage_buffer();
	if (p_view.motion && !depth_only) {
		const uint32_t bytes = storage->global_shader_uniforms_get_storage_buffer_bytes();
		const uint64_t layout = storage->global_shader_uniforms_get_storage_layout_version();
		ERR_FAIL_COND_V(bytes == 0 || bytes % 4 != 0, ERR_INVALID_DATA);
		bool replaced = false;
		for (uint32_t index = 0; index < 2; ++index) {
			replaced |= global_history[index].is_null() || global_history_words[index] < bytes / 4;
			const Error error = memory.ensure(global_history[index], global_history_words[index], bytes / 4);
			if (error != OK) {
				return error;
			}
		}
		if (current_frame != global_history_frame || replaced || layout != global_history_layout) {
			global_history_valid = !replaced && layout == global_history_layout && global_history_frame != UINT64_MAX && current_frame == global_history_frame + 1;
			std::swap(global_history[0], global_history[1]);
			std::swap(global_history_words[0], global_history_words[1]);
			rd->buffer_copy(previous_global_binding, global_history[0], 0, 0, bytes);
			if (!global_history_valid) {
				rd->buffer_copy(previous_global_binding, global_history[1], 0, 0, bytes);
			}
			global_history_frame = current_frame;
			global_history_layout = layout;
		}
		previous_global_binding = global_history[1];
	}
	using SceneShader = RendererSceneRenderImplementation::SceneShaderForwardClustered;
	material_work.clear();
	raster_work.clear();
	surface_context = "virtual_geometry_" + itos(p_view.view_index);
	motion_context = "virtual_geometry_motion_" + itos(p_view.view_index);
	if (!depth_only && (surface_size != size || surfaces[0].is_null())) {
		p_buffers->clear_context(surface_context);
		const char *names[10] = { "albedo_metallic", "normal_roughness", "emission_ao", "specular_rim_clearcoat", "backlight_ao", "anisotropy", "surface_flags", "tangent", "geometric_normal", "lightmap_uv" };
		for (uint32_t index = 0; index < 10; ++index) {
			RD::TextureFormat format;
			format.width = size.x;
			format.height = size.y;
			format.format = index == 6 ? RD::DATA_FORMAT_R32_UINT : (index == 9 ? RD::DATA_FORMAT_R16G16_SFLOAT : RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
			format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			surfaces[index] = p_buffers->create_texture_from_format(surface_context, names[index], format);
			ERR_FAIL_COND_V(surfaces[index].is_null(), ERR_CANT_CREATE);
		}
		surface_size = size;
	}
	if (!depth_only) {
		rd->texture_clear(surfaces[6], Color(), 0, 1, 0, 1);
		const Size2i expected_motion = p_view.motion ? size : Size2i(1, 1);
		if (motion_size != expected_motion || motion_surface.is_null()) {
			p_buffers->clear_context(motion_context);
			RD::TextureFormat format;
			format.width = expected_motion.x;
			format.height = expected_motion.y;
			format.format = RD::DATA_FORMAT_R16G16_SFLOAT;
			format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			motion_surface = p_buffers->create_texture_from_format(motion_context, "motion", format);
			format.format = RD::DATA_FORMAT_R32_SFLOAT;
			reactive_surface = p_buffers->create_texture_from_format(motion_context, "reactivity", format);
			ERR_FAIL_COND_V(motion_surface.is_null() || reactive_surface.is_null(), ERR_CANT_CREATE);
			motion_size = expected_motion;
		}
	}
	if (!depth_only) {
		if (current_frame != temporal_frame) {
			std::swap(temporal_keys[0], temporal_keys[1]);
			std::swap(temporal_key_words[0], temporal_key_words[1]);
			temporal_frame = current_frame;
		}
		const uint64_t words = p_view.motion ? uint64_t(size.x) * size.y * 4 : 4;
		for (uint32_t index = 0; index < 2; ++index) {
			const bool replaced = temporal_key_words[index] < words || temporal_keys[index].is_null();
			const Error error = memory.ensure(temporal_keys[index], temporal_key_words[index], words);
			if (error != OK) {
				return error;
			}
			if (replaced || index == 0 || !p_view.history_valid) {
				rd->buffer_clear(temporal_keys[index], 0, temporal_key_words[index] * 4);
			}
		}
		rd->texture_clear(reactive_surface, Color(), 0, 1, 0, 1);
	}
	if (materials.is_empty()) {
		return OK;
	}
	if (view_status.is_null()) {
		view_status = memory.storage(4);
	}
	ERR_FAIL_COND_V(view_status.is_null(), ERR_CANT_CREATE);
	rd->buffer_clear(view_status, 0, 4);
	if (material_view.is_null()) {
		material_view = memory.uniform(320);
	}
	ERR_FAIL_COND_V(material_view.is_null(), ERR_CANT_CREATE);
	if (frame_history.is_null()) {
		frame_history = memory.uniform(272);
	}
	ERR_FAIL_COND_V(frame_history.is_null(), ERR_CANT_CREATE);
	struct FrameHistory {
		float projection[16];
		float view[16];
		float inverse_view[16];
		float main_camera[16];
		float state[4];
	} history = {};
	static_assert(sizeof(FrameHistory) == 272);
	MaterialStorage::store_camera(p_view.previous_projection, history.projection);
	MaterialStorage::store_transform(p_view.previous_camera.affine_inverse(), history.view);
	MaterialStorage::store_transform(p_view.previous_camera, history.inverse_view);
	MaterialStorage::store_transform(p_view.main_camera, history.main_camera);
	history.state[0] = p_view.previous_time;
	history.state[1] = p_view.motion && !depth_only ? 1.0f : 0.0f;
	history.state[2] = p_view.previous_jitter.x;
	history.state[3] = p_view.previous_jitter.y;
	rd->buffer_update(frame_history, 0, sizeof(history), &history);
	struct ViewUniforms {
		float projection[16];
		float inverse_projection[16];
		float view[16];
		float inverse_view[16];
		float dimensions[4];
		float state[4];
		uint32_t flags[4];
		float eye_offset[4];
	} view = {};
	static_assert(sizeof(ViewUniforms) == 320);
	MaterialStorage::store_camera(p_view.projection, view.projection);
	MaterialStorage::store_camera(p_view.projection.inverse(), view.inverse_projection);
	MaterialStorage::store_transform(p_view.camera.affine_inverse(), view.view);
	MaterialStorage::store_transform(p_view.camera, view.inverse_view);
	view.dimensions[0] = size.x;
	view.dimensions[1] = size.y;
	view.dimensions[2] = p_view.jitter.x;
	view.dimensions[3] = p_view.jitter.y;
	view.state[0] = p_view.time;
	view.state[1] = p_view.exposure;
	view.state[2] = p_view.shadow_far;
	view.flags[0] = p_view.layers;
	view.flags[1] = p_view.shadow_flags;
	view.flags[2] = p_view.history_valid && (!p_view.motion || global_history_valid);
	view.flags[3] = (p_view.motion && !global_history_valid ? 1u : 0u) | (p_view.reverse_cull ? 2u : 0u);
	view.eye_offset[0] = p_view.eye_offset.x;
	view.eye_offset[1] = p_view.eye_offset.y;
	view.eye_offset[2] = p_view.eye_offset.z;
	view.eye_offset[3] = p_view.view_index;
	rd->buffer_update(material_view, 0, sizeof(view), &view);
	for (uint32_t material_index = 0; material_index < materials.size(); ++material_index) {
		raster_material_data[material_index * 4 + 3] = p_view.reverse_cull ? 1u : 0u;
		RID material = materials[material_index];
		auto *data = static_cast<SceneShader::MaterialData *>(storage->material_get_data(material, MaterialStorage::SHADER_TYPE_3D));
		ERR_FAIL_COND_V(data == nullptr || data->shader_data == nullptr, ERR_INVALID_DATA);
		String message;
		RID program = data->shader_data->get_virtual_geometry_shader(message);
		ERR_FAIL_COND_V_MSG(program.is_null(), ERR_UNAVAILABLE, message);
		ERR_FAIL_COND_V_MSG(data->shader_data->get_virtual_geometry_composite_shader(false).is_null() ||
						data->shader_data->get_virtual_geometry_composite_shader(true).is_null(),
				ERR_UNAVAILABLE,
				"Virtual geometry scene lighting shader compilation failed.");
		if (p_voxel || data->shader_data->uses_vertex || data->shader_data->uses_alpha_clip || data->shader_data->uses_discard) {
			ERR_FAIL_COND_V_MSG(data->shader_data->uses_vertex && storage->material_get_max_vertex_displacement(material) <= 0.0f, ERR_UNAVAILABLE, "Virtual geometry vertex deformation requires a positive max_vertex_displacement.");
			raster_material_data[material_index * 4 + 2] = 1;
			RasterMaterialWork raster;
			raster.material = material_index;
			for (uint32_t mode = p_voxel ? 2 : 0; mode < 3; ++mode) {
				raster.programs[mode] = p_voxel ? data->shader_data->get_virtual_geometry_voxel_shader() : data->shader_data->get_virtual_geometry_raster_shader(mode);
				ERR_FAIL_COND_V(raster.programs[mode].is_null(), ERR_CANT_CREATE);
				raster.pipelines[mode] = p_voxel ? data->shader_data->get_virtual_geometry_voxel_pipeline(p_raster_format) : data->shader_data->get_virtual_geometry_raster_pipeline(mode, p_raster_format);
				ERR_FAIL_COND_V(raster.pipelines[mode].is_null(), ERR_CANT_CREATE);
				raster.parameters[mode] = data->get_parameters_uniform_set(raster.programs[mode], 3, true, data->shader_data->texture_uniforms);
				ERR_FAIL_COND_V(raster.parameters[mode].is_null() && (data->shader_data->ubo_size != 0 || !data->shader_data->texture_uniforms.is_empty()), ERR_CANT_CREATE);
			}
			raster_work.push_back(raster);
		}
		if (depth_only) {
			data->set_as_used();
			continue;
		}
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { p_geometry }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, { previous_global_binding }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { scene_buffer }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { bins_buffer }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, { storage->global_shader_uniforms_get_storage_buffer() }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 12, { material_view }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, { raster_material_buffer }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, { view_status }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 25, { motion_surface }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 27, { reactive_surface }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 30, { temporal_keys[1] }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 31, { temporal_keys[0] }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, { previous_sources }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 28, { frame_history }));
		for (uint32_t index = 0; index < 10; ++index) {
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, index == 9 ? 29 : 16 + index, { surfaces[index] }));
		}
		storage->samplers_rd_get_default().append_uniforms(uniforms, 32);
		RID scene = UniformSetCacheRD::get_singleton()->get_cache_vec(program, 0, uniforms);
		ERR_FAIL_COND_V(scene.is_null(), ERR_CANT_CREATE);
		RID parameters = data->get_parameters_uniform_set(program, 3, true, data->shader_data->texture_uniforms);
		ERR_FAIL_COND_V(parameters.is_null() && (data->shader_data->ubo_size != 0 || !data->shader_data->texture_uniforms.is_empty()), ERR_CANT_CREATE);
		data->set_as_used();
		material_work.push_back({ data->shader_data->get_virtual_geometry_pipeline(), scene, parameters });
	}
	rd->buffer_update(raster_material_buffer, 0, raster_material_data.size() * 4, raster_material_data.ptr());
	return OK;
}

void VirtualGeometryShading::record_materials() {
	if (depth_only) {
		materials_ready = true;
		return;
	}
	if (material_work.is_empty()) {
		return;
	}
	RD *rd = RD::get_singleton();
	RENDER_TIMESTAMP("Virtual Geometry Materials Begin");
	const RD::ComputeListID list = rd->compute_list_begin();
	for (uint32_t index = 0; index < material_work.size(); ++index) {
		const auto &work = material_work[index];
		const uint32_t push[3] = { index, uint32_t(materials.size()), entries_word };
		rd->compute_list_bind_compute_pipeline(list, work.pipeline);
		rd->compute_list_bind_uniform_set(list, work.scene, 0);
		if (work.material.is_valid()) {
			rd->compute_list_bind_uniform_set(list, work.material, 3);
		}
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		rd->compute_list_dispatch_indirect(list, arguments, index * 12);
	}
	rd->compute_list_end();
	RENDER_TIMESTAMP("Virtual Geometry Materials End");
	materials_ready = true;
}

uint64_t VirtualGeometryShading::get_buffer_bytes() const {
	return uint64_t(scene_words + bins_words + argument_words + draw_argument_words + raster_material_words + previous_source_words + batch_parameter_words) * 4 +
			uint64_t(global_history_words[0] + global_history_words[1]) * 4 +
			(dummy_transforms.is_valid() ? 48 : 0) + (material_view.is_valid() ? 320 : 0) +
			(frame_history.is_valid() ? 272 : 0) + (view_status.is_valid() ? 4 : 0) + uint64_t(surface_size.x) * surface_size.y * 72 + uint64_t(motion_size.x) * motion_size.y * 8 + uint64_t(temporal_key_words[0] + temporal_key_words[1]) * 4;
}

uint64_t VirtualGeometryShading::get_raster_candidate_capacity(Span<VirtualGeometrySelection::Result> p_sources) {
	const uint32_t buckets = materials.size() + 1;
	if (raster_bucket_marks.size() < buckets) {
		raster_bucket_marks.resize(buckets);
		for (uint32_t &mark : raster_bucket_marks) {
			mark = 0;
		}
	}
	uint64_t capacity = 0;
	for (uint32_t index = 0; index < p_sources.size(); ++index) {
		if (++raster_bucket_epoch == 0) {
			for (uint32_t &mark : raster_bucket_marks) {
				mark = 0;
			}
			++raster_bucket_epoch;
		}
		uint32_t pipelines = 0;
		const Source &source = sources[index];
		for (uint32_t slot = 0; slot < source.material_slots; ++slot) {
			const uint32_t material = material_map[source.map + slot];
			const uint32_t bucket = raster_material_data[material * 4u + 2u] == 0 ? 0 : material + 1;
			if (raster_bucket_marks[bucket] != raster_bucket_epoch) {
				raster_bucket_marks[bucket] = raster_bucket_epoch;
				++pipelines;
			}
		}
		capacity += uint64_t(p_sources[index].selected_capacity) * MIN(64u, pipelines);
	}
	return capacity;
}

} // namespace RendererRD
