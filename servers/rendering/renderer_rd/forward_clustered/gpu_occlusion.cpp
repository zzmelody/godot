// SPDX-License-Identifier: MIT
#include "gpu_occlusion.h"

#include "core/io/image.h"
#include "core/profiling/profiling.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace RendererRD {

GpuOcclusion::GpuOcclusion() {
	Vector<String> modes;
	modes.push_back("");
	device_addresses = RD::get_singleton()->has_feature(RD::SUPPORTS_BUFFER_DEVICE_ADDRESS);
	Vector<String> static_modes = modes;
	if (device_addresses) {
		static_modes.push_back("\n#define USE_DEVICE_ADDRESSES\n");
	}
	cull_shader.initialize(static_modes);
	cull_version = cull_shader.version_create();
	cull_pipeline = RD::get_singleton()->compute_pipeline_create(cull_shader.version_get_shader(cull_version, 0));
	if (device_addresses) {
		cull_addressed_pipeline = RD::get_singleton()->compute_pipeline_create(cull_shader.version_get_shader(cull_version, 1));
	}
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
	for (RID rid : { indices, pending, commands, dummy_transform, sampler, cull_pipeline, cull_dynamic_pipeline, cull_addressed_pipeline, reduce_pipeline, reduce_msaa_pipeline, parameter_buffers[0], parameter_buffers[1], task_buffer, address_buffer }) {
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
	total_tasks = 0;
	groups.clear();
	sources.clear();
	group_ranges.clear();
	tasks.clear();
	addresses.clear();
	source_buffers.clear();
	for (uint32_t phase = 0; phase < 2; ++phase) {
		group_maps[phase].clear();
		parameters[phase].clear();
	}
}

void GpuOcclusion::add(uint32_t p_element, Batch p_batch) {
	if (p_batch.count == 0 || p_batch.count > MAX_CANDIDATES - candidate_count) {
		return; // The caller retains the original draw for every omitted batch.
	}
	uint32_t batch_tasks = GpuOcclusionDispatch::task_count(p_batch.count);
	if (!GpuOcclusionDispatch::fits_budget(batches.size() + 1, total_tasks + batch_tasks, parameter_capacity, task_capacity, device_addresses)) {
		return; // Keep the original draw when retained metadata would exceed 8 MiB.
	}
	total_tasks += batch_tasks;
	p_batch.candidate_offset = candidate_count;
	candidate_count += p_batch.count;
	element_batches[p_element] = batches.size();
	batches.push_back(p_batch);
}

bool GpuOcclusion::prepare(const Size2i &p_size, uint64_t p_epoch, const Projection &p_projection, const Projection &p_lens) {
	if (!cull_pipeline.is_valid() || !cull_dynamic_pipeline.is_valid() || !reduce_pipeline.is_valid() || !reduce_msaa_pipeline.is_valid() || (device_addresses && !cull_addressed_pipeline.is_valid())) {
		history_valid = false;
		return false;
	}
	RD *rd = RD::get_singleton();
	if (capacity < MAX(1u, candidate_count) || indices.is_null() || pending.is_null()) {
		if (indices.is_valid()) {
			rd->free_rid(indices);
		}
		if (pending.is_valid()) {
			rd->free_rid(pending);
		}
		capacity = Math::next_power_of_2(MAX(1u, candidate_count));
		indices = rd->storage_buffer_create(capacity * 3 * sizeof(uint32_t));
		pending = rd->storage_buffer_create(capacity * sizeof(uint32_t));
	}
	if (command_capacity < MAX(1u, batches.size()) || commands.is_null()) {
		if (commands.is_valid()) {
			rd->free_rid(commands);
		}
		command_capacity = Math::next_power_of_2(MAX(1u, batches.size()));
		commands = rd->storage_buffer_create(command_capacity * 15 * sizeof(uint32_t), {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		initial_commands.resize(command_capacity * 15);
	}
	if (indices.is_null() || pending.is_null() || commands.is_null()) {
		history_valid = false;
		return false;
	}
	memset(initial_commands.ptrw(), 0, batches.size() * 15 * sizeof(uint32_t));
	for (uint32_t i = 0; i < batches.size(); ++i) {
		initial_commands.write[i * 15] = batches[i].depth_vertices;
		initial_commands.write[i * 15 + 5] = batches[i].depth_vertices;
		initial_commands.write[i * 15 + 10] = batches[i].color_vertices;
	}
	if (!batches.is_empty()) {
		if (rd->buffer_update(commands, 0, batches.size() * 15 * sizeof(uint32_t), initial_commands.ptr()) != OK) {
			history_valid = false;
			return false;
		}
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
				RID slice = rd->texture_create_shared_from_slice(RD::TextureView(), hierarchy[i], 0, level);
				if (slice.is_null()) {
					free_hierarchy();
					return false;
				}
				slices[i].push_back(slice);
			}
		}
	}
	if (epoch != p_epoch || previous_lens != p_lens) {
		history_valid = false;
		epoch = p_epoch;
	}
	previous_lens = p_lens;
	projection = p_projection;
	if (!prepare_dispatches()) {
		history_valid = false;
		return false;
	}
	return true;
}

bool GpuOcclusion::prepare_dispatches() {
	RD *rd = RD::get_singleton();
	dispatch_limit = uint32_t(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X));
	if (dispatch_limit == 0) {
		return false;
	}
	if (parameter_capacity < MAX(1u, batches.size()) || parameter_buffers[0].is_null() || parameter_buffers[1].is_null()) {
		parameter_capacity = GpuOcclusionDispatch::capacity_for(batches.size());
		for (uint32_t phase = 0; phase < 2; ++phase) {
			if (parameter_buffers[phase].is_valid()) {
				rd->free_rid(parameter_buffers[phase]);
			}
			parameter_buffers[phase] = rd->storage_buffer_create(parameter_capacity * sizeof(BatchParameters));
		}
	}
	if (task_capacity < MAX(1u, total_tasks) || task_buffer.is_null()) {
		if (task_buffer.is_valid()) {
			rd->free_rid(task_buffer);
		}
		task_capacity = GpuOcclusionDispatch::capacity_for(total_tasks);
		task_buffer = rd->storage_buffer_create(task_capacity * sizeof(GpuOcclusionDispatch::Task));
	}
	if (device_addresses && (address_capacity < parameter_capacity || address_buffer.is_null())) {
		if (address_buffer.is_valid()) {
			rd->free_rid(address_buffer);
		}
		address_capacity = parameter_capacity;
		address_buffer = rd->storage_buffer_create(address_capacity * sizeof(GpuOcclusionDispatch::Address));
	}
	if (parameter_buffers[0].is_null() || parameter_buffers[1].is_null() || task_buffer.is_null() || (device_addresses && address_buffer.is_null())) {
		return false;
	}
	sources.resize(batches.size());
	addresses.resize(batches.size());
	for (uint32_t i = 0; i < batches.size(); ++i) {
		const Batch &batch = batches[i];
		// Dynamic buffers resolve their current frame slice while encoding. Keep
		// those under the descriptor path rather than retaining a stale address.
		const bool addressed = device_addresses && !batch.dynamic_transforms;
		addresses[i] = {};
		if (addressed && batch.multimesh) {
			RD::BufferInfo info;
			const RD::BufferInfo *cached = source_buffers.getptr(batch.transforms);
			if (cached) {
				info = *cached;
			} else {
				if (!rd->buffer_get_info(batch.transforms, info) || !info.storage || info.device_address == 0) {
					return false;
				}
				source_buffers.insert(batch.transforms, info);
			}
			addresses[i] = { uint32_t(info.device_address), uint32_t(info.device_address >> 32) };
		}
		RID transforms = batch.multimesh && !addressed ? batch.transforms : dummy_transform;
		AHashMap<RID, uint32_t> &map = group_maps[batch.dynamic_transforms ? 1 : 0];
		const uint32_t *existing = map.getptr(transforms);
		uint32_t group = existing ? *existing : groups.size();
		if (!existing) {
			DispatchGroup entry;
			entry.transforms = transforms;
			entry.dynamic = batch.dynamic_transforms;
			entry.addressed = addressed;
			groups.push_back(entry);
			map.insert(transforms, group);
		}
		sources[i] = { group, batch.count };
	}
	group_ranges.resize(groups.size());
	tasks.resize(total_tasks);
	GpuOcclusionDispatch::build_tasks(sources.ptr(), sources.size(), group_ranges.ptr(), group_ranges.size(), tasks.ptr());
	if (!tasks.is_empty() && rd->buffer_update(task_buffer, 0, tasks.size() * sizeof(GpuOcclusionDispatch::Task), tasks.ptr()) != OK) {
		return false;
	}
	if (device_addresses && !addresses.is_empty() && rd->buffer_update(address_buffer, 0, addresses.size() * sizeof(GpuOcclusionDispatch::Address), addresses.ptr()) != OK) {
		return false;
	}
	// Prepare both phases before any prediction/depth draw. Recovery must not
	// fail after part of this view has already switched to indirect commands.
	for (uint32_t phase = 0; phase < 2; ++phase) {
		parameters[phase].resize(batches.size());
		for (uint32_t i = 0; i < batches.size(); ++i) {
			const Batch &batch = batches[i];
			BatchParameters &params = parameters[phase][i];
			params = {};
			Projection matrix = (phase == 0 && history_valid ? previous_projection : projection) * Projection(batch.model);
			MaterialStorage::store_camera(matrix, params.view_projection);
			for (uint32_t axis = 0; axis < 3; ++axis) {
				params.minimum_margin[axis] = batch.bounds.position[axis];
				params.maximum_bias[axis] = batch.bounds.get_end()[axis];
			}
			params.minimum_margin[3] = batch.margin;
			params.maximum_bias[3] = 0.00001f;
			params.source_info[0] = batch.count;
			params.source_info[1] = batch.stride;
			params.source_info[2] = batch.transform_offset;
			params.source_info[3] = batch.candidate_offset;
			params.draw_info[0] = i * 15;
			params.draw_info[1] = phase;
			params.draw_info[2] = phase != 0 || history_valid;
			params.draw_info[3] = batch.multimesh;
		}
		if (!batches.is_empty() && rd->buffer_update(parameter_buffers[phase], 0, batches.size() * sizeof(BatchParameters), parameters[phase].ptr()) != OK) {
			return false;
		}
		for (DispatchGroup &group : groups) {
			RID shader = group.dynamic ? cull_dynamic_shader.version_get_shader(cull_dynamic_version, 0) : cull_shader.version_get_shader(cull_version, group.addressed ? 1 : 0);
			RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { sampler, hierarchy[phase == 0 ? 1 - write_index : write_index] });
			RD::Uniform u_source(group.dynamic ? RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC : RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { group.transforms });
			RD::Uniform u_visible(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { indices });
			RD::Uniform u_pending(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, { pending });
			RD::Uniform u_commands(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { commands });
			RD::Uniform u_parameters(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, { parameter_buffers[phase] });
			RD::Uniform u_tasks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { task_buffer });
			if (group.addressed) {
				RD::Uniform u_addresses(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, { address_buffer });
				group.uniform_sets[phase] = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_depth, u_source, u_visible, u_pending, u_commands, u_parameters, u_tasks, u_addresses);
			} else {
				group.uniform_sets[phase] = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_depth, u_source, u_visible, u_pending, u_commands, u_parameters, u_tasks);
			}
			if (!rd->uniform_set_is_valid(group.uniform_sets[phase])) {
				return false;
			}
		}
	}
	return true;
}

void GpuOcclusion::cull(uint32_t p_phase) {
	RD *rd = RD::get_singleton();
	RD::ComputeListID list = rd->compute_list_begin();
	// Physical-address reads are not visible in uniform-set reflection. Include
	// every borrowed source in RD's transfer and render-graph dependency tracking.
	{
		GodotProfileZone("Render/Occlusion/source_read_edges");
		for (const KeyValue<RID, RD::BufferInfo> &source : source_buffers) {
			rd->compute_list_add_buffer_read(list, source.key, source.value);
		}
	}
	for (uint32_t i = 0; i < groups.size(); ++i) {
		const DispatchGroup &group = groups[i];
		const GpuOcclusionDispatch::Range &range = group_ranges[i];
		rd->compute_list_bind_compute_pipeline(list, group.dynamic ? cull_dynamic_pipeline : (group.addressed ? cull_addressed_pipeline : cull_pipeline));
		rd->compute_list_bind_uniform_set(list, group.uniform_sets[p_phase], 0);
		for (uint32_t offset = 0; offset < range.count;) {
			uint32_t count = GpuOcclusionDispatch::dispatch_count(range.count - offset, dispatch_limit);
			uint32_t push[4] = { range.offset + offset, count, 0, 0 };
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch(list, count, 1, 1);
			offset += count;
		}
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
