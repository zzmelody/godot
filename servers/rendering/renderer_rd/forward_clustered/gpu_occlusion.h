// SPDX-License-Identifier: MIT
#pragma once

#include "gpu_occlusion_dispatch.h"

#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/gpu_occlusion.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/gpu_occlusion_reduce.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// One owner per RenderSceneBuffersRD. Source geometry and transform buffers
// remain borrowed and immutable; only view-local lists/commands are written.
class GpuOcclusion {
public:
	struct Batch {
		RID transforms;
		Transform3D model;
		AABB bounds;
		float margin = 0.0f;
		uint32_t count = 1;
		uint32_t stride = 0;
		uint32_t transform_offset = 0;
		uint32_t previous_transform_offset = 0;
		uint32_t candidate_offset = 0;
		uint32_t depth_vertices = 0;
		uint32_t color_vertices = 0;
		bool multimesh = false;
		bool dynamic_transforms = false;
	};
	LocalVector<Batch> batches;
	LocalVector<int32_t> element_batches;
	AHashMap<const void *, uint32_t> surface_batches;

	GpuOcclusion();
	~GpuOcclusion();
	void begin(uint32_t p_elements);
	void add(uint32_t p_element, Batch p_batch);
	bool prepare(const Size2i &p_size, uint64_t p_epoch, const Projection &p_projection, const Projection &p_lens);
	void cull(uint32_t p_phase);
	void build_hierarchy(RID p_depth, bool p_msaa);
	void finish();
	RID get_indices() const { return indices; }
	RID get_commands() const { return commands; }
	uint64_t get_multidraw_sources_address() const { return multidraw_sources_address; }
	uint64_t get_multidraw_source_address(uint32_t p_batch) const { return multidraw_sources_address + uint64_t(p_batch) * sizeof(MultiDrawSource); }
	void record_multidraw_source_reads(RD::DrawListID p_list) const;
	static constexpr uint32_t COMMAND_STRIDE = 15 * sizeof(uint32_t);
	uint32_t get_index_offset(uint32_t p_batch, uint32_t p_phase) const;
	uint32_t get_command_offset(uint32_t p_batch, uint32_t p_phase) const { return (p_batch * 15 + p_phase * 5) * sizeof(uint32_t); }
	uint32_t get_candidate_count() const { return candidate_count; }
	uint32_t get_workgroup_count() const { return total_tasks; }
	uint32_t get_dispatch_count() const {
		if (dispatch_limit == 0) {
			return 0;
		}
		uint32_t count = 0;
		for (const GpuOcclusionDispatch::Range &range : group_ranges) {
			count += range.count / dispatch_limit + (range.count % dispatch_limit != 0);
		}
		return count;
	}
	uint32_t get_dispatch_group_count() const { return groups.size(); }
	bool uses_device_addresses() const { return device_addresses; }
	uint32_t get_addressed_buffer_count() const { return source_buffers.size(); }
	uint64_t get_metadata_bytes() const { return GpuOcclusionDispatch::metadata_bytes(parameter_capacity, task_capacity, device_addresses); }
	uint64_t get_buffer_bytes() const { return uint64_t(capacity) * 16 + uint64_t(command_capacity) * 60 + get_metadata_bytes() + uint64_t(multidraw_source_capacity) * sizeof(MultiDrawSource); }
	uint64_t get_hierarchy_bytes() const;
	bool has_history() const { return history_valid; }
	RID get_hierarchy(bool p_previous) const { return hierarchy[p_previous ? 1 - write_index : write_index]; }
	const Projection &get_projection(bool p_previous) const { return p_previous ? previous_projection : projection; }
	static constexpr uint32_t MAX_CANDIDATES = 1048576;

private:
	struct BatchParameters {
		float view_projection[16];
		float minimum_margin[4];
		float maximum_bias[4];
		uint32_t source_info[4];
		uint32_t draw_info[4];
	};
	static_assert(sizeof(BatchParameters) == GpuOcclusionDispatch::PARAMETER_BYTES);
	struct DispatchGroup {
		RID transforms;
		RID uniform_sets[2];
		bool dynamic = false;
		bool addressed = false;
	};
	// Rebuilt at every prepare. Capacities remain view-owned and reusable.
	AHashMap<RID, uint32_t> group_maps[2];
	LocalVector<DispatchGroup> groups;
	LocalVector<GpuOcclusionDispatch::Source> sources;
	LocalVector<GpuOcclusionDispatch::Range> group_ranges;
	LocalVector<GpuOcclusionDispatch::Task> tasks;
	LocalVector<BatchParameters> parameters[2];
	LocalVector<GpuOcclusionDispatch::Address> addresses;
	AHashMap<RID, RD::BufferInfo> source_buffers;
	// One table per prepared view, shared by prediction, recovery and color.
	// Command order identifies the source; render-list indices remain draw-local.
	struct MultiDrawSource {
		uint64_t transforms = 0;
		uint32_t visibility_offset = 0;
		uint32_t visibility_stride = 0;
		uint32_t current_offset = 0;
		uint32_t previous_offset = 0;
	};
	static_assert(sizeof(MultiDrawSource) == 24);
	LocalVector<MultiDrawSource> multidraw_sources;
	RID multidraw_source_buffer;
	RD::BufferInfo multidraw_source_info;
	uint64_t multidraw_sources_address = 0;
	uint32_t multidraw_source_capacity = 0;
	bool multidraw_supported = false;
	RID parameter_buffers[2], task_buffer, address_buffer;
	uint32_t parameter_capacity = 0, task_capacity = 0, total_tasks = 0;
	uint32_t address_capacity = 0;
	bool device_addresses = false;
	uint32_t dispatch_limit = 0;
	bool prepare_dispatches();
	GpuOcclusionShaderRD cull_shader;
	GpuOcclusionShaderRD cull_dynamic_shader;
	GpuOcclusionReduceShaderRD reduce_shader;
	RID cull_version, reduce_version, cull_pipeline, reduce_pipeline, reduce_msaa_pipeline;
	RID cull_dynamic_version, cull_dynamic_pipeline;
	RID cull_addressed_pipeline;
	RID sampler, dummy_transform, indices, pending, commands;
	RID hierarchy[2];
	LocalVector<RID> slices[2];
	Size2i size;
	uint32_t capacity = 0, command_capacity = 0, candidate_count = 0;
	uint32_t write_index = 0;
	uint64_t epoch = 0;
	bool history_valid = false;
	Vector<uint32_t> initial_commands;
	Projection projection, previous_projection;
	Projection previous_lens;
	void free_hierarchy();
};

} // namespace RendererRD
