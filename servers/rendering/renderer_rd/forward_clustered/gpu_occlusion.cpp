// SPDX-License-Identifier: MIT
#include "gpu_occlusion.h"

#include "core/io/image.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace RendererRD {

GpuOcclusion::GpuOcclusion() {
	Vector<String> modes;
	modes.push_back("");
	cull_shader.initialize(modes);
	cull_version = cull_shader.version_create();
	cull_pipeline = RD::get_singleton()->compute_pipeline_create(cull_shader.version_get_shader(cull_version, 0));
	Vector<uint64_t> dynamic_buffers;
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(0, 1));
	cull_dynamic_shader.initialize(modes, "", {}, dynamic_buffers);
	cull_dynamic_version = cull_dynamic_shader.version_create();
	cull_dynamic_pipeline = RD::get_singleton()->compute_pipeline_create(cull_dynamic_shader.version_get_shader(cull_dynamic_version, 0));
	modes.push_back("\n#define SOURCE_MSAA\n");
	reduce_shader.initialize(modes);
	reduce_version = reduce_shader.version_create();
	reduce_pipeline = RD::get_singleton()->compute_pipeline_create(reduce_shader.version_get_shader(reduce_version, 0));
	reduce_msaa_pipeline = RD::get_singleton()->compute_pipeline_create(reduce_shader.version_get_shader(reduce_version, 1));
	RD::SamplerState sampler_state;
	sampler = RD::get_singleton()->sampler_create(sampler_state);
	dummy_transform = RD::get_singleton()->storage_buffer_create(48);
}

void GpuOcclusion::free_hierarchy() {
	for (uint32_t i = 0; i < 2; ++i) {
		// Shared views are dependents of the parent texture and are freed with it.
		slices[i].clear();
		if (hierarchy[i].is_valid()) {
			RD::get_singleton()->free_rid(hierarchy[i]);
			hierarchy[i] = RID();
		}
	}
	history_valid = false;
}

GpuOcclusion::~GpuOcclusion() {
	free_hierarchy();
	for (RID rid : { indices, pending, commands, dummy_transform, sampler, cull_pipeline, cull_dynamic_pipeline, reduce_pipeline, reduce_msaa_pipeline }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	cull_shader.version_free(cull_version);
	cull_dynamic_shader.version_free(cull_dynamic_version);
	reduce_shader.version_free(reduce_version);
}

void GpuOcclusion::begin(uint32_t p_elements) {
	batches.clear();
	surface_batches.clear();
	element_batches.resize(p_elements);
	for (uint32_t i = 0; i < p_elements; ++i) {
		element_batches[i] = -1;
	}
	candidate_count = 0;
}

void GpuOcclusion::add(uint32_t p_element, Batch p_batch) {
	if (p_batch.count == 0 || p_batch.count > MAX_CANDIDATES - candidate_count) {
		return; // The caller retains the original draw for every omitted batch.
	}
	p_batch.candidate_offset = candidate_count;
	candidate_count += p_batch.count;
	element_batches[p_element] = batches.size();
	batches.push_back(p_batch);
}

bool GpuOcclusion::prepare(const Size2i &p_size, uint64_t p_epoch, const Projection &p_projection, const Projection &p_lens) {
	if (!cull_pipeline.is_valid() || !cull_dynamic_pipeline.is_valid() || !reduce_pipeline.is_valid() || !reduce_msaa_pipeline.is_valid()) {
		history_valid = false;
		return false;
	}
	RD *rd = RD::get_singleton();
	if (capacity < MAX(1u, candidate_count)) {
		if (indices.is_valid()) {
			rd->free_rid(indices);
			rd->free_rid(pending);
		}
		capacity = Math::next_power_of_2(MAX(1u, candidate_count));
		indices = rd->storage_buffer_create(capacity * 3 * sizeof(uint32_t));
		pending = rd->storage_buffer_create(capacity * sizeof(uint32_t));
	}
	if (command_capacity < MAX(1u, batches.size())) {
		if (commands.is_valid()) {
			rd->free_rid(commands);
		}
		command_capacity = Math::next_power_of_2(MAX(1u, batches.size()));
		commands = rd->storage_buffer_create(command_capacity * 15 * sizeof(uint32_t), {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		initial_commands.resize(command_capacity * 15);
	}
	if (indices.is_null() || pending.is_null() || commands.is_null()) {
		return false;
	}
	memset(initial_commands.ptrw(), 0, batches.size() * 15 * sizeof(uint32_t));
	for (uint32_t i = 0; i < batches.size(); ++i) {
		initial_commands.write[i * 15] = batches[i].depth_vertices;
		initial_commands.write[i * 15 + 5] = batches[i].depth_vertices;
		initial_commands.write[i * 15 + 10] = batches[i].color_vertices;
	}
	if (!batches.is_empty()) {
		rd->buffer_update(commands, 0, batches.size() * 15 * sizeof(uint32_t), initial_commands.ptr());
	}
	Size2i new_size(MAX(1, (p_size.x + 1) / 2), MAX(1, (p_size.y + 1) / 2));
	if (size != new_size || !hierarchy[0].is_valid()) {
		free_hierarchy();
		size = new_size;
		RD::TextureFormat format;
		format.width = size.x;
		format.height = size.y;
		format.format = RD::DATA_FORMAT_R32_SFLOAT;
		format.mipmaps = Image::get_image_required_mipmaps(size.x, size.y, Image::FORMAT_RF) + 1;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		for (uint32_t i = 0; i < 2; ++i) {
			hierarchy[i] = rd->texture_create(format, RD::TextureView());
			if (hierarchy[i].is_null()) {
				free_hierarchy();
				return false;
			}
			for (uint32_t level = 0; level < format.mipmaps; ++level) {
				slices[i].push_back(rd->texture_create_shared_from_slice(RD::TextureView(), hierarchy[i], 0, level));
			}
		}
	}
	if (epoch != p_epoch || previous_lens != p_lens) {
		history_valid = false;
		epoch = p_epoch;
	}
	previous_lens = p_lens;
	projection = p_projection;
	return indices.is_valid() && pending.is_valid() && commands.is_valid() && hierarchy[0].is_valid() && hierarchy[1].is_valid();
}

void GpuOcclusion::cull(uint32_t p_phase) {
	struct PushConstant {
		float view_projection[16];
		float minimum_margin[4];
		float maximum_bias[4];
		uint32_t source_info[4];
		uint32_t draw_info[4];
	};
	static_assert(sizeof(PushConstant) == 128);
	RD *rd = RD::get_singleton();
	RID depth = hierarchy[p_phase == 0 ? 1 - write_index : write_index];
	RD::ComputeListID list = rd->compute_list_begin();
	for (uint32_t i = 0; i < batches.size(); ++i) {
		const Batch &batch = batches[i];
		RID shader = batch.dynamic_transforms ? cull_dynamic_shader.version_get_shader(cull_dynamic_version, 0) : cull_shader.version_get_shader(cull_version, 0);
		rd->compute_list_bind_compute_pipeline(list, batch.dynamic_transforms ? cull_dynamic_pipeline : cull_pipeline);
		PushConstant push = {};
		Projection matrix = (p_phase == 0 && history_valid ? previous_projection : projection) * Projection(batch.model);
		MaterialStorage::store_camera(matrix, push.view_projection);
		for (uint32_t axis = 0; axis < 3; ++axis) {
			push.minimum_margin[axis] = batch.bounds.position[axis];
			push.maximum_bias[axis] = batch.bounds.get_end()[axis];
		}
		push.minimum_margin[3] = batch.margin;
		push.maximum_bias[3] = 0.00001f;
		push.source_info[0] = batch.count;
		push.source_info[1] = batch.stride;
		push.source_info[2] = batch.transform_offset;
		push.source_info[3] = batch.candidate_offset;
		push.draw_info[0] = i * 15;
		push.draw_info[1] = p_phase;
		push.draw_info[2] = p_phase != 0 || history_valid;
		push.draw_info[3] = batch.multimesh;
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { sampler, depth });
		RD::Uniform u_source(batch.dynamic_transforms ? RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC : RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { batch.multimesh ? batch.transforms : dummy_transform });
		RD::Uniform u_visible(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { indices });
		RD::Uniform u_pending(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, { pending });
		RD::Uniform u_commands(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { commands });
		RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_depth, u_source, u_visible, u_pending, u_commands);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch(list, (batch.count + 63) / 64, 1, 1);
	}
	rd->compute_list_end();
}

void GpuOcclusion::build_hierarchy(RID p_depth, bool p_msaa) {
	RD *rd = RD::get_singleton();
	RD::ComputeListID list = rd->compute_list_begin();
	for (uint32_t level = 0; level < slices[write_index].size(); ++level) {
		bool msaa = level == 0 && p_msaa;
		RID shader = reduce_shader.version_get_shader(reduce_version, msaa ? 1 : 0);
		RID source = level == 0 ? p_depth : slices[write_index][level - 1];
		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { sampler, source });
		RD::Uniform u_destination(RD::UNIFORM_TYPE_IMAGE, 1, { slices[write_index][level] });
		rd->compute_list_bind_compute_pipeline(list, msaa ? reduce_msaa_pipeline : reduce_pipeline);
		rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_source, u_destination), 0);
		rd->compute_list_dispatch(list, (MAX(1, size.x >> level) + 7) / 8, (MAX(1, size.y >> level) + 7) / 8, 1);
		rd->compute_list_add_barrier(list);
	}
	rd->compute_list_end();
}

void GpuOcclusion::finish() {
	previous_projection = projection;
	history_valid = true;
	write_index = 1 - write_index;
}

uint32_t GpuOcclusion::get_index_offset(uint32_t p_batch, uint32_t p_phase) const {
	const Batch &batch = batches[p_batch];
	return batch.candidate_offset * 3 + p_phase * batch.count;
}

uint64_t GpuOcclusion::get_hierarchy_bytes() const {
	uint64_t bytes = 0;
	for (uint32_t level = 0; level < slices[0].size(); ++level) {
		bytes += uint64_t(MAX(1, size.x >> level)) * MAX(1, size.y >> level) * sizeof(float) * 2;
	}
	return bytes;
}

} // namespace RendererRD
