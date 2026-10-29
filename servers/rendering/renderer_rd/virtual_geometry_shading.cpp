// SPDX-License-Identifier: MIT
#include "virtual_geometry_shading.h"

#include "core/profiling/profiling.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
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
	_prune_material_history(0, false);
	for (RID snapshot : global_history) {
		if (snapshot.is_valid()) {
			memory.free(snapshot);
		}
	}
	for (RID rid : { scene_buffer, bins_buffer, arguments, draw_arguments, material_view, frame_history, previous_sources, previous_transforms, source_tasks, source_arguments, batch_parameters, view_status, raster_material_buffer, extensions, source_addresses }) {
		if (rid.is_valid()) {
			memory.free(rid);
		}
	}
	for (auto &pipeline : init_pipelines) {
		pipeline.free();
	}
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
	GodotProfileZone("Render/VirtualGeometry/source_parameters");
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
	source_levels.clear();
	source_history_copies.clear();
	source_reads.clear();
	source_address_data.clear();
	direct_sources = !p_sources.is_empty() && p_sources[0].direct_source_reads;
	size = p_size;
	uint64_t instances = 0, visible = 0;
	MaterialStorage *storage = MaterialStorage::get_singleton();
	for (const auto &result : p_sources) {
		const auto &batch = result.batch;
		ERR_FAIL_COND_V(batch.materials.is_empty(), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(instances > 0xffffffu || material_map.size() > UINT32_MAX, ERR_OUT_OF_MEMORY);
		sources.push_back({ uint32_t(instances), uint32_t(material_map.size()), uint32_t(visible), uint32_t(batch.materials.size()) });
		instances += batch.count;
		visible += result.selected_capacity;
		ERR_FAIL_COND_V(instances > 0xffffffu, ERR_OUT_OF_MEMORY);
		for (const RID &requested : batch.materials) {
			const RID material = requested.is_valid() ? requested : p_default_material;
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
	// Four existing words per material, followed by one direct-software owner
	// per material. This view's preparation resets every owner before use.
	for (uint32_t index = 0; index < materials.size(); ++index) {
		raster_material_data.push_back(index);
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
		if (previous_transform_words > 16) {
			memory.free(previous_transforms);
			previous_transforms = RID();
			previous_transform_words = 0;
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
		bin_shader.initialize(modes);
		scan_shader.initialize(modes);
		dispatch_shader.initialize(modes);
		modes.push_back("#define VG_DIRECT_SOURCE_READS\n");
		init_shader.initialize(modes);
		init_shader.set_variant_enabled(1, rd->has_feature(RD::SUPPORTS_BUFFER_DEVICE_ADDRESS));
		init_version = init_shader.version_create();
		bin_version = bin_shader.version_create();
		scan_version = scan_shader.version_create();
		dispatch_version = dispatch_shader.version_create();
		bin_pipeline.create_compute_pipeline(bin_shader.version_get_shader(bin_version, 0));
		scan_pipeline.create_compute_pipeline(scan_shader.version_get_shader(scan_version, 0));
		dispatch_pipeline.create_compute_pipeline(dispatch_shader.version_get_shader(dispatch_version, 0));
	}
	const uint32_t init_mode = direct_sources ? 1 : 0;
	if (init_pipelines[init_mode].get_rid().is_null()) {
		init_pipelines[init_mode].create_compute_pipeline(init_shader.version_get_shader(init_version, init_mode));
	}
	ERR_FAIL_COND_V(init_pipelines[init_mode].get_rid().is_null() || bin_pipeline.get_rid().is_null() || scan_pipeline.get_rid().is_null() || dispatch_pipeline.get_rid().is_null(), ERR_CANT_CREATE);
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	static_assert(sizeof(BatchParameters) == 264);
	error = ensure_words(memory, batch_parameters, batch_parameter_words, MAX(uint64_t(1), uint64_t(p_sources.size()) * 66));
	if (error != OK) {
		return error;
	}
	batch_parameter_data.resize(p_sources.size());
	if (direct_sources) {
		source_address_data.resize(p_sources.size());
	}
	uint64_t history_words = 0;
	for (uint32_t index = 0; index < p_sources.size(); ++index) {
		const auto &source = p_sources[index];
		const auto &batch = source.batch;
		ERR_FAIL_COND_V(source.work != p_sources[0].work || source.direct_source_reads != direct_sources ||
						(!direct_sources && source.transforms != p_sources[0].transforms),
				ERR_INVALID_PARAMETER);
		BatchParameters &input = batch_parameter_data[index];
		input = {};
		MaterialStorage::store_transform(batch.model, input.model);
		input.source[0] = batch.count;
		input.source[1] = batch.stride;
		input.source[2] = source.transform_offset;
		input.source[3] = batch.geometry.root_pages;
		memcpy(&input.displacement[0], &batch.max_vertex_displacement, sizeof(float));
		if (direct_sources) {
			SourceAddresses &addresses = source_address_data[index];
			addresses = {};
			if (batch.stride != 0) {
				const uint64_t stride_bytes = uint64_t(batch.stride) * 16;
				ERR_FAIL_COND_V(batch.count > UINT32_MAX / stride_bytes || batch.transform_offset > UINT32_MAX / stride_bytes, ERR_INVALID_PARAMETER);
				// Selection validated this allocation before packing the same view.
				// Keep the source RID in this list's read edges, but borrow its value
				// record rather than repeat the owner and address queries per batch.
				ERR_FAIL_COND_V(source.transform_source.buffer != batch.transforms, ERR_INVALID_PARAMETER);
				ERR_FAIL_COND_V(!source_reads.resolve(source.transform_source, uint64_t(batch.transform_offset) * stride_bytes, uint64_t(batch.count) * stride_bytes, addresses.current), ERR_INVALID_DATA);
			}
		}
		if (record_history) {
			MaterialStorage::store_transform(batch.previous_model, input.previous_model);
			input.previous_info[0] = uint32_t(history_words / 4);
			input.previous_info[1] = 1;
			if (batch.stride != 0) {
				if (batch.previous_transform_offset == batch.transform_offset) {
					// Static MultiMeshes share the current and previous span. Keep
					// the previous model, but reuse the same current local values.
					input.previous_info[2] = 1;
				} else {
					const uint64_t bytes = uint64_t(batch.count) * batch.stride * 16;
					const uint64_t source_byte = uint64_t(batch.previous_transform_offset) * batch.stride * 16;
					ERR_FAIL_COND_V(bytes > UINT32_MAX || source_byte > UINT32_MAX || history_words + bytes / 4 > UINT32_MAX / 4, ERR_OUT_OF_MEMORY);
					if (direct_sources) {
						ERR_FAIL_COND_V(!source_reads.resolve(rd, batch.transforms, source_byte, bytes, source_address_data[index].previous), ERR_INVALID_DATA);
					} else {
						source_history_copies.push_back({ batch.transforms, uint32_t(source_byte), uint32_t(history_words * 4), uint32_t(bytes) });
						history_words += bytes / 4;
					}
				}
			}
		}
		sources[index].batch_word = uint32_t(batches_word + uint64_t(index) * 20);
		uint32_t *info = input.info;
		info[0] = sources[index].first;
		info[1] = sources[index].map;
		info[2] = batch.has_color;
		info[3] = batch.has_custom;
		info[4] = uint32_t(batch.shader_uniforms_offset);
		info[5] = batch.layer_mask;
		info[6] = sources[index].visible_base;
		info[7] = source.selected_capacity;
		info[8] = sources[index].material_slots;
		info[9] = batch.render_flags;
		info[10] = batch.gi_offset;
		info[11] = source.work_word;
		const Rect2 &uv = batch.lightmap_uv_scale;
		const float lightmap_uv[4] = { uv.position.x, uv.position.y, uv.size.x, uv.size.y };
		memcpy(info + 12, lightmap_uv, sizeof(lightmap_uv));
		info[16] = uint32_t(batch.history_instance_id);
		info[17] = uint32_t(batch.history_instance_id >> 32);
		const uint64_t mesh_tag = hash64_murmur3_64(batch.geometry.generation, hash64_murmur3_64(batch.mesh.get_id(), 0));
		info[18] = uint32_t(mesh_tag);
		info[19] = uint32_t(mesh_tag >> 32);
		info[20] = sources[index].batch_word;
		info[21] = index * 66;
	}
	error = ensure_words(memory, previous_transforms, previous_transform_words, MAX(uint64_t(1), history_words));
	if (error != OK) {
		return error;
	}
	if (!batch_parameter_data.is_empty()) {
		rd->buffer_update(batch_parameters, 0, uint32_t(uint64_t(batch_parameter_data.size()) * sizeof(BatchParameters)), batch_parameter_data.ptr());
	}
	if (direct_sources) {
		error = ensure_words(memory, source_addresses, source_address_words, MAX(uint64_t(1), uint64_t(source_address_data.size()) * 4));
		if (error != OK) {
			return error;
		}
		rd->buffer_update(source_addresses, 0, uint32_t(uint64_t(source_address_data.size()) * sizeof(SourceAddresses)), source_address_data.ptr());
	}
	source_instances = uint32_t(instances);
	uint64_t task_scratch = 16 + uint64_t(p_sources.size()) * 2;
	if (!p_sources.is_empty()) {
		ScanLevel level{ uint32_t(p_sources.size()), 16, 16 + uint32_t(p_sources.size()) };
		while (true) {
			if (level.count <= 128) {
				source_levels.push_back(level);
				break;
			}
			const uint32_t blocks = (level.count + 127) / 128;
			level.sums = uint32_t(task_scratch);
			task_scratch += blocks;
			source_levels.push_back(level);
			level = { blocks, level.sums, uint32_t(task_scratch) };
			task_scratch += blocks;
		}
	}
	error = ensure_words(memory, source_tasks, source_task_words, task_scratch);
	if (error != OK) {
		return error;
	}
	const uint32_t tasks_header[16] = { 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	rd->buffer_update(source_tasks, 0, sizeof(tasks_header), tasks_header);
	error = ensure_words(memory, source_arguments, source_argument_words, 4, true);
	if (error != OK) {
		return error;
	}
	const uint32_t source_dispatch[4] = { 0, 1, 1, 0 };
	rd->buffer_update(source_arguments, 0, sizeof(source_dispatch), source_dispatch);
	if (!p_sources.is_empty()) {
		LocalVector<RD::Uniform> uniforms;
		if (direct_sources) {
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 30, { source_addresses }));
		} else {
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { p_sources[0].transforms }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, { previous_transforms }));
		}
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { p_sources[0].work }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { scene_buffer }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, { previous_sources }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, { batch_parameters }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, { source_tasks }));
		source_uniforms = cache->get_cache_vec(init_shader.version_get_shader(init_version, init_mode), 0, uniforms);
		source_scan_uniforms = cache->get_cache(scan_shader.version_get_shader(scan_version, 0), 0, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { source_tasks }));
		ERR_FAIL_COND_V(source_uniforms.is_null() || source_scan_uniforms.is_null(), ERR_CANT_CREATE);
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
	_prune_material_history(0, false);
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
	if (!direct_sources) {
		const Error snapshot = snapshot_copies.record(previous_transforms, Span<const HistoryCopy>(source_history_copies.ptr(), source_history_copies.size()));
		ERR_FAIL_COND(snapshot != OK);
	}
	RD::ComputeListID list = rd->compute_list_begin();
	auto init_pass = [&](uint32_t p_mode, uint32_t p_items, bool p_indirect = false) {
		const uint32_t push[4] = { uint32_t(p_sources.size()), p_mode, p_items, 0 };
		rd->compute_list_bind_compute_pipeline(list, init_pipelines[direct_sources ? 1 : 0].get_rid());
		rd->compute_list_bind_uniform_set(list, source_uniforms, 0);
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		if (p_indirect) {
			rd->compute_list_dispatch_indirect(list, source_arguments, 0);
		} else {
			dispatch_items(list, p_items, 64);
		}
		rd->compute_list_add_barrier(list);
	};
	init_pass(0, p_sources.size());
	rd->compute_list_bind_compute_pipeline(list, scan_pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, source_scan_uniforms, 0);
	for (const ScanLevel &level : source_levels) {
		const uint32_t push[5] = { 0, level.count, level.input, level.output, level.sums };
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_items(list, level.count, 128);
		rd->compute_list_add_barrier(list);
	}
	for (int index = int(source_levels.size()) - 2; index >= 0; --index) {
		const ScanLevel &level = source_levels[index];
		const uint32_t push[5] = { 1, level.count, source_levels[index + 1].output, level.output, 0 };
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_items(list, level.count, 128);
		rd->compute_list_add_barrier(list);
	}
	init_pass(3, 1);
	// Keep indirect parameters outside the storage buffer that the source
	// pipeline writes. The transfer also records an explicit producer edge.
	rd->compute_list_end();
	const Error copy_error = rd->buffer_copy(source_tasks, source_arguments, 16, 0, 12);
	ERR_FAIL_COND(copy_error != OK);
	list = rd->compute_list_begin();
	if (direct_sources) {
		source_reads.record(rd, list);
	}
	init_pass(1, source_instances);
	init_pass(2, 0, true);
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

void VirtualGeometryShading::_prune_material_history(uint64_t p_frame, bool p_enabled) {
	LocalVector<RID> retired;
	for (const KeyValue<RID, MaterialHistory> &entry : material_history) {
		const MaterialHistory &history = entry.value;
		if (!p_enabled || history.frame > p_frame || p_frame - history.frame > 1) {
			for (RID buffer : history.uniforms) {
				if (buffer.is_valid()) {
					memory.free(buffer);
				}
			}
			retired.push_back(entry.key);
		}
	}
	for (RID material : retired) {
		material_history.erase(material);
	}
}

Error VirtualGeometryShading::_capture_material_history(RID p_material, const MaterialHistoryInput &p_input, uint64_t p_frame, RID &r_previous, bool &r_valid) {
	ERR_FAIL_COND_V(p_input.bytes != 0 && p_input.uniform_data == nullptr, ERR_INVALID_DATA);
	MaterialHistory &history = material_history[p_material];
	if (history.bytes != p_input.bytes) {
		RID candidate[2];
		if (p_input.bytes != 0) {
			candidate[0] = memory.uniform(p_input.bytes);
			if (candidate[0].is_null()) {
				return ERR_OUT_OF_MEMORY;
			}
			candidate[1] = memory.uniform(p_input.bytes);
			if (candidate[1].is_null()) {
				memory.free(candidate[0]);
				return ERR_OUT_OF_MEMORY;
			}
		}
		for (uint32_t index = 0; index < 2; index++) {
			if (history.uniforms[index].is_valid()) {
				memory.free(history.uniforms[index]);
			}
			history.uniforms[index] = candidate[index];
		}
		history.bytes = p_input.bytes;
		history.frame = UINT64_MAX;
	}
	const bool compatible = history.shader == p_input.shader && history.shader_generation == p_input.shader_generation &&
			history.texture_revision == p_input.texture_revision && history.maximum_displacement == p_input.maximum_displacement && !p_input.dynamic_textures;
	if (history.frame != p_frame) {
		history.valid = history.frame != UINT64_MAX && history.frame + 1 == p_frame && compatible;
		std::swap(history.uniforms[0], history.uniforms[1]);
	} else {
		// A second draw in one frame can replace current values, but it must not
		// promote that draw into the previous frame or restore invalid history.
		history.valid &= compatible;
	}
	history.shader = p_input.shader;
	history.shader_generation = p_input.shader_generation;
	history.texture_revision = p_input.texture_revision;
	history.maximum_displacement = p_input.maximum_displacement;
	history.frame = p_frame;
	if (p_input.bytes != 0) {
		RD *rd = RD::get_singleton();
		// Ordinary material UBOs do not have transfer-source usage. Upload the
		// existing CPU packing instead of copying from an unsupported GPU source.
		Error error = rd->buffer_update(history.uniforms[0], 0, p_input.bytes, p_input.uniform_data);
		if (error == OK && !history.valid) {
			error = rd->buffer_update(history.uniforms[1], 0, p_input.bytes, p_input.uniform_data);
		}
		if (error != OK) {
			history.valid = false;
			history.frame = UINT64_MAX;
			return error;
		}
	}
	r_previous = history.uniforms[1];
	r_valid = history.valid;
	return OK;
}

Error VirtualGeometryShading::prepare_materials(const Ref<RenderSceneBuffersRD> &p_buffers, RID p_geometry, const ViewParameters &p_view, RD::FramebufferFormatID p_raster_format, bool p_voxel) {
	materials_ready = false;
	RD *rd = RD::get_singleton();
	MaterialStorage *storage = MaterialStorage::get_singleton();
	const uint64_t current_frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : global_history_frame + 1;
	if (!depth_only) {
		_prune_material_history(current_frame, p_view.motion);
	}
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
		const bool compatible = !replaced && layout == global_history_layout;
		if (current_frame != global_history_frame) {
			global_history_valid = compatible && global_history_frame != UINT64_MAX && current_frame == global_history_frame + 1;
			std::swap(global_history[0], global_history[1]);
			std::swap(global_history_words[0], global_history_words[1]);
		} else {
			global_history_valid &= compatible;
		}
		// Capture the final current values even when this view is prepared again
		// in one frame. Only a new frame can advance the previous snapshot.
		Error error = rd->buffer_copy(previous_global_binding, global_history[0], 0, 0, bytes);
		if (error == OK && !global_history_valid) {
			error = rd->buffer_copy(previous_global_binding, global_history[1], 0, 0, bytes);
		}
		if (error != OK) {
			global_history_valid = false;
			global_history_frame = UINT64_MAX;
			return error;
		}
		global_history_frame = current_frame;
		global_history_layout = layout;
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
			if (index == 1 || index == 7 || index == 8) {
				// Independent authored axes feed anisotropic specular lighting.
				// Half-float octahedral binormals amplify error in that BRDF.
				format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
			}
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
	extension_stride = 0;
	if (!depth_only) {
		for (RID material : materials) {
			auto *data = static_cast<SceneShader::MaterialData *>(storage->material_get_data(material, MaterialStorage::SHADER_TYPE_3D));
			ERR_FAIL_COND_V(data == nullptr || data->shader_data == nullptr, ERR_INVALID_DATA);
			String message;
			ERR_FAIL_COND_V_MSG(data->shader_data->get_virtual_geometry_shader(message).is_null(), ERR_UNAVAILABLE, message);
			extension_stride = MAX(extension_stride, data->shader_data->virtual_geometry_extension_words);
		}
		const Error error = ensure_words(memory, extensions, extension_words, MAX(uint64_t(1), uint64_t(size.x) * size.y * 4 * extension_stride));
		if (error != OK) {
			return error;
		}
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
	view.flags[3] = (p_view.motion && !global_history_valid ? 1u : 0u) | (p_view.reverse_cull ? 2u : 0u) | (p_view.multiview ? 4u : 0u);
	view.eye_offset[0] = p_view.eye_offset.x;
	view.eye_offset[1] = p_view.eye_offset.y;
	view.eye_offset[2] = p_view.eye_offset.z;
	view.eye_offset[3] = p_view.view_index;
	rd->buffer_update(material_view, 0, sizeof(view), &view);
	direct_material_heads.clear();
	direct_material_next.resize(materials.size());
	for (uint32_t material_index = 0; material_index < materials.size(); ++material_index) {
		raster_material_data[materials.size() * 4 + material_index] = material_index;
		direct_material_next[material_index] = UINT32_MAX;
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
		if (p_voxel || data->shader_data->uses_vertex || data->shader_data->uses_alpha_clip || data->shader_data->uses_discard || data->shader_data->writes_fragment_depth) {
			ERR_FAIL_COND_V_MSG(data->shader_data->uses_vertex && storage->material_get_max_vertex_displacement(material) <= 0.0f, ERR_UNAVAILABLE, "Virtual geometry vertex deformation requires a positive max_vertex_displacement.");
			// Bit 0 selects programmable raster; bit 1 prevents geometric Hi-Z
			// from rejecting a cluster whose authored depth can move forward.
			raster_material_data[material_index * 4 + 2] = 1u | (data->shader_data->writes_fragment_depth ? 2u : 0u);
			RasterMaterialWork raster;
			raster.material = material_index;
			raster.direct_material = material_index;
			raster.fused_software = !p_voxel && !data->shader_data->uses_alpha_clip && !data->shader_data->uses_discard && !data->shader_data->writes_fragment_depth;
			if (raster.fused_software) {
				raster_material_data[material_index * 4 + 2] |= 4u;
				raster.small_cluster_pipeline = data->shader_data->virtual_geometry_raster_small.get_rid();
				raster.direct_software_pipeline = data->shader_data->virtual_geometry_raster_direct.get_rid();
				ERR_FAIL_COND_V(raster.small_cluster_pipeline.is_null() || raster.direct_software_pipeline.is_null(), ERR_CANT_CREATE);
				if (!data->uses_dynamic_textures) {
					uint32_t hash = hash_murmur3_one_64(program.get_id(), data->get_parameters_hash(true));
					hash = hash_fmix32(hash_murmur3_one_32(raster_material_data[material_index * 4 + 1], hash));
					const uint32_t *head = direct_material_heads.getptr(hash);
					for (uint32_t previous = head != nullptr ? *head : UINT32_MAX; previous != UINT32_MAX; previous = direct_material_next[previous]) {
						auto *other = static_cast<SceneShader::MaterialData *>(storage->material_get_data(materials[previous], MaterialStorage::SHADER_TYPE_3D));
						// Shader identity includes its live generation and all compile
						// modes. Exact bytes/RIDs resolve every hash collision.
						if (other != nullptr && other->shader_data == data->shader_data &&
								raster_material_data[previous * 4 + 1] == raster_material_data[material_index * 4 + 1] &&
								data->has_same_parameters(*other, true)) {
							raster.direct_material = previous;
							break;
						}
					}
					if (raster.direct_material == material_index) {
						direct_material_next[material_index] = head != nullptr ? *head : UINT32_MAX;
						direct_material_heads.insert(hash, material_index);
					}
				}
				raster_material_data[materials.size() * 4 + material_index] = raster.direct_material;
			}
			for (uint32_t mode = p_voxel ? 2 : 0; mode < 3; ++mode) {
				// Fused opaque clusters finish software work in mode0. Only
				// their original hardware fallback still needs mode2 parameters.
				if (raster.fused_software && mode == 1) {
					continue;
				}
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
		RID previous_material = data->get_uniform_buffer(true);
		if (p_view.motion) {
			MaterialHistoryInput input;
			input.shader = data->shader_data->version;
			input.shader_generation = data->shader_data->virtual_geometry_generation;
			const Vector<uint8_t> &uniform_data = data->get_uniform_data(true);
			input.uniform_data = uniform_data.ptr();
			input.bytes = uniform_data.size();
			input.texture_revision = data->get_texture_revision();
			input.maximum_displacement = storage->material_get_max_vertex_displacement(material);
			input.dynamic_textures = data->uses_dynamic_textures;
			bool valid = false;
			const Error error = _capture_material_history(material, input, current_frame, previous_material, valid);
			if (error != OK) {
				return error;
			}
			if (!valid) {
				raster_material_data[material_index * 4 + 3] |= 2u;
			}
		}
		LocalVector<RD::Uniform> uniforms;
		if (data->shader_data->ubo_size != 0) {
			ERR_FAIL_COND_V(previous_material.is_null(), ERR_INVALID_DATA);
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 65, { previous_material }));
		}
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
		if (data->shader_data->virtual_geometry_extension_words != 0) {
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 64, { extensions }));
		}
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
#ifdef GODOT_USE_TRACY
	int64_t direct_passes = 0;
	int64_t fused_materials = 0;
	for (const auto &work : raster_work) {
		fused_materials += work.fused_software;
		direct_passes += work.fused_software && work.direct_material == work.material;
	}
	TracyPlot("Render/Geometry/fused_materials_per_view", fused_materials);
	TracyPlot("Render/Geometry/direct_passes_per_view", direct_passes);
#endif
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
		const uint32_t push[4] = { index, uint32_t(materials.size()), entries_word, extension_stride };
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
	uint64_t material_bytes = 0;
	for (const KeyValue<RID, MaterialHistory> &entry : material_history) {
		material_bytes += uint64_t(entry.value.bytes) * 2;
	}
	return material_bytes + snapshot_copies.get_buffer_bytes() + (uint64_t(scene_words) + bins_words + argument_words + draw_argument_words + raster_material_words + previous_source_words + previous_transform_words + source_task_words + source_argument_words + batch_parameter_words + extension_words + source_address_words) * 4 +
			uint64_t(global_history_words[0] + global_history_words[1]) * 4 +
			(material_view.is_valid() ? 320 : 0) +
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
