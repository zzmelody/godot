// SPDX-License-Identifier: MIT
#pragma once

#include "core/templates/a_hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/span.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_snapshot_copy.glsl.gen.h"
#include "servers/rendering/renderer_rd/virtual_geometry_snapshot_copy_plan.h"
#include "servers/rendering/renderer_rd/virtual_geometry_work_memory.h"

namespace RendererRD {

// View-owned tiled copies for the selection and material source snapshots.
// Source RIDs are borrowed only while recording; RD tracks every device-address
// read and the descriptor-backed writes. The buffers share the VG work budget.
class VirtualGeometrySnapshotCopy {
public:
	struct Copy {
		RID source;
		uint32_t source_byte = 0;
		uint32_t destination_byte = 0;
		uint32_t bytes = 0;
	};

	Error record(RID p_first_destination, Span<const Copy> p_first, RID p_second_destination = RID(), Span<const Copy> p_second = {});
	uint64_t get_buffer_bytes() const { return uint64_t(job_capacity) * VirtualGeometrySnapshotCopyPlan::JOB_BYTES; }
	~VirtualGeometrySnapshotCopy();

private:
	struct SourceInfo {
		uint64_t address = 0;
		uint32_t bytes = 0;
	};
	struct Job {
		uint32_t source[2];
		uint32_t destination_word;
		uint32_t words;
		uint32_t output;
		uint32_t first_tile;
	};
	struct Selection {
		uint32_t output;
		uint32_t index;
	};
	static_assert(sizeof(Job) == VirtualGeometrySnapshotCopyPlan::JOB_BYTES);
	VirtualGeometryWorkMemory memory;
	VirtualGeometrySnapshotCopyShaderRD shader;
	RID version;
	PipelineDeferredRD pipeline;
	RID job_buffer;
	uint32_t job_capacity = 0;
	LocalVector<Job> jobs;
	LocalVector<Selection> selected;
	AHashMap<RID, SourceInfo> sources;
	Error _record_direct(RID p_first_destination, Span<const Copy> p_first, RID p_second_destination, Span<const Copy> p_second);
};

} // namespace RendererRD
