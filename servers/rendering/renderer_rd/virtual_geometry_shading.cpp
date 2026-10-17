// SPDX-License-Identifier: MIT
#include "virtual_geometry_shading.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {
namespace {

Error ensure_words(RID &r_buffer, uint32_t &r_capacity, uint64_t p_words, bool p_indirect = false) {
	ERR_FAIL_COND_V(p_words > UINT32_MAX / 4, ERR_OUT_OF_MEMORY);
	if (r_capacity >= p_words && r_buffer.is_valid()) {
		return OK;
	}
	RD *rd = RD::get_singleton();
	if (r_buffer.is_valid()) {
		rd->free_rid(r_buffer);
	}
	uint32_t capacity = Math::next_power_of_2(uint32_t(MAX(uint64_t(1), p_words)));
	if (uint64_t(capacity) * 4 > UINT32_MAX) {
		capacity = p_words;
	}
	r_buffer = rd->storage_buffer_create(capacity * 4, {}, p_indirect ? RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT : 0);
	r_capacity = capacity;
	return r_buffer.is_valid() ? OK : ERR_CANT_CREATE;
}

void dispatch_items(RD::ComputeListID p_list, uint32_t p_items, uint32_t p_threads) {
	const uint32_t groups = MAX(1u, (p_items + p_threads - 1) / p_threads);
	RD::get_singleton()->compute_list_dispatch(p_list, MIN(65535u, groups), (groups + 65534) / 65535, 1);
}

} // namespace

VirtualGeometryShading::~VirtualGeometryShading() {
	for (RID rid : { scene_buffer, bins_buffer, arguments, dummy_transforms }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
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

Error VirtualGeometryShading::prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, RID p_visibility, RID p_geometry) {
	materials.clear();
	material_ids.clear();
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
		ERR_FAIL_COND_V(instances > UINT32_MAX || material_map.size() > UINT32_MAX, ERR_OUT_OF_MEMORY);
		sources.push_back({ uint32_t(instances), uint32_t(material_map.size()) });
		instances += batch.count;
		visible += result.selected_capacity;
		for (uint32_t slot = 0; slot < slots.rids.size(); ++slot) {
			RID material = slots.rids[slot];
			const uint32_t identity = slots.identities[slot];
			if (identity < batch.surface_materials.size() && batch.surface_materials[identity].is_valid()) {
				material = batch.surface_materials[identity];
			}
			if (batch.material_override.is_valid()) {
				material = batch.material_override;
			}
			if (material.is_null()) {
				material = p_default_material;
			}
			ERR_FAIL_COND_V(!storage->owns_material(material), ERR_INVALID_DATA);
			const uint32_t *found = material_ids.getptr(material);
			uint32_t index;
			if (found) {
				index = *found;
			} else {
				index = materials.size();
				material_ids.insert(material, index);
				materials.push_back(material);
			}
			material_map.push_back(index);
		}
	}
	const uint64_t references_word = 4 + instances * 48;
	const uint64_t materials_word = references_word + visible * 2;
	const uint64_t total_words = materials_word + material_map.size();
	Error error = ensure_words(scene_buffer, scene_words, total_words);
	if (error != OK) {
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
	error = ensure_words(bins_buffer, bins_words, scratch + uint64_t(size.x) * size.y * 3);
	if (error != OK) {
		return error;
	}
	error = ensure_words(arguments, argument_words, MAX(uint64_t(3), uint64_t(materials.size()) * 3), true);
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
		dummy_transforms = rd->storage_buffer_create(48);
	}
	ERR_FAIL_COND_V(init_pipeline.get_rid().is_null() || bin_pipeline.get_rid().is_null() || scan_pipeline.get_rid().is_null() || dispatch_pipeline.get_rid().is_null() || dummy_transforms.is_null(), ERR_CANT_CREATE);
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	for (uint32_t index = 0; index < p_sources.size(); ++index) {
		const auto &source = p_sources[index];
		sources[index].uniforms = cache->get_cache(init_shader.version_get_shader(init_version, 0), 0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { source.batch.stride != 0 ? source.batch.transforms : dummy_transforms }),
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { source.parameters }), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { source.work }),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { scene_buffer }));
		ERR_FAIL_COND_V(sources[index].uniforms.is_null(), ERR_CANT_CREATE);
	}
	if (!materials.is_empty()) {
		bin_uniforms = cache->get_cache(bin_shader.version_get_shader(bin_version, 0), 0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { p_geometry }), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { p_visibility }),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { scene_buffer }), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { bins_buffer }));
		scan_uniforms = cache->get_cache(scan_shader.version_get_shader(scan_version, 0), 0, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { bins_buffer }));
		dispatch_uniforms = cache->get_cache(dispatch_shader.version_get_shader(dispatch_version, 0), 0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { bins_buffer }), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, { arguments }));
		ERR_FAIL_COND_V(bin_uniforms.is_null() || scan_uniforms.is_null() || dispatch_uniforms.is_null(), ERR_CANT_CREATE);
	}
	return OK;
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
		const uint32_t push[6] = { sources[index].first, sources[index].map, uint32_t(source.batch.has_color), uint32_t(source.batch.has_custom), uint32_t(source.batch.shader_uniforms_offset), source.batch.layer_mask };
		rd->compute_list_bind_uniform_set(list, sources[index].uniforms, 0);
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_items(list, source.batch.count, 64);
	}
	rd->compute_list_end();
	RENDER_TIMESTAMP("Virtual Geometry Material Sources End");
}

void VirtualGeometryShading::record_bins() {
	RD *rd = RD::get_singleton();
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

uint64_t VirtualGeometryShading::get_buffer_bytes() const {
	return uint64_t(scene_words + bins_words + argument_words) * 4 + (dummy_transforms.is_valid() ? 48 : 0);
}

} // namespace RendererRD
