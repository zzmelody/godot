// SPDX-License-Identifier: MIT
#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/fixed_vector.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_select.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_select_dispatch.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_buffer_source_reads.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_geometry_storage.h"
#include "servers/rendering/renderer_rd/virtual_geometry_snapshot_copy.h"
#include "servers/rendering/renderer_rd/virtual_geometry_work_memory.h"

class RenderGeometryInstance;

namespace RendererRD {

// Small resolved slot lists are values, not per-instance heap allocations.
// Larger authored material sets retain the existing shared-vector lifetime.
class VirtualGeometryMaterialSlots {
	FixedVector<RID, 2> inline_slots;
	Vector<RID> overflow_slots;

public:
	void clear() {
		inline_slots.clear();
		overflow_slots.clear();
	}
	Error resize(uint32_t p_count) {
		if (p_count <= 2) {
			overflow_slots.clear();
			return inline_slots.resize_initialized(p_count);
		} else {
			inline_slots.clear();
			return overflow_slots.resize(p_count);
		}
	}
	uint32_t size() const { return overflow_slots.is_empty() ? inline_slots.size() : uint32_t(overflow_slots.size()); }
	bool is_empty() const { return size() == 0; }
	RID *ptrw() { return overflow_slots.is_empty() ? inline_slots.ptr() : overflow_slots.ptrw(); }
	const RID *ptr() const { return overflow_slots.is_empty() ? inline_slots.ptr() : overflow_slots.ptr(); }
	const RID *begin() const { return ptr(); }
	const RID *end() const { return ptr() + size(); }
};

// One view owns the reusable work buffers. MeshStorage lends published model
// buffers and live MultiMesh transforms for the current render boundary only.
// No readback, source-array expansion or per-cluster CPU work occurs here.
class VirtualGeometrySelection {
public:
	struct Batch {
		VirtualGeometryStorage::Bindings geometry;
		RID mesh;
		RID source;
		// Borrowed renderer object, valid only between begin() boundaries.
		RenderGeometryInstance *source_instance = nullptr;
		uint64_t history_instance_id = 0;
		RID transforms;
		Transform3D model;
		Transform3D previous_model;
		uint32_t count = 1;
		uint32_t stride = 0;
		uint32_t transform_offset = 0;
		uint32_t previous_transform_offset = 0;
		// Resolved at the owner's dirty boundary. Empty RID uses the view default.
		VirtualGeometryMaterialSlots materials;
		bool uses_sss = false;
		uint32_t layer_mask = 1;
		uint32_t render_flags = 0;
		uint32_t gi_offset = UINT32_MAX;
		Rect2 lightmap_uv_scale;
		float max_vertex_displacement = 0.0f;
		int32_t shader_uniforms_offset = -1;
		bool has_color = false;
		bool has_custom = false;
		bool shadow_double_sided = false;
	};
	struct Result {
		Batch batch;
		// Borrowed view arena; this result's header begins at work_word.
		RID work;
		RID parameters;
		// Fallback borrows one current-transform snapshot. Direct reads use
		// batch.transforms instead; the logical vec4 offset still owns a span.
		RID transforms;
		bool direct_source_reads = false;
		StorageBufferSourceReads::ResolvedBuffer transform_source;
		uint32_t transform_offset = 0;
		uint32_t work_word = 0;
		uint32_t selected_capacity = 0;
		uint32_t root_capacity = 0;
	};

	void begin(const Transform3D &p_camera, const Projection &p_projection, bool p_flip_y, const Size2i &p_size, uint32_t p_shadow_flags = 0, float p_shadow_far = 0);
	void add_batch(const Batch &p_batch) { batches.push_back(p_batch); }
	// SDFGI captures the complete resident root proxy without requesting fine
	// pages; its dirty regions do not track ordinary page-residency changes.
	Error record(bool p_root_only = false);
	Error use_root_coverage();
	Span<Result> get_results() const { return Span<Result>(results.ptr(), used); }
	uint64_t get_buffer_bytes() const;
	~VirtualGeometrySelection();

private:
	VirtualGeometryWorkMemory memory;
	VirtualGeometrySnapshotCopy snapshot_copies;
	struct Parameters {
		float model[16];
		uint32_t source[4];
		uint32_t limits[4];
		uint32_t offsets[4];
		uint32_t addresses[4];
		uint32_t prefix[4];
	};
	using BufferCopy = VirtualGeometrySnapshotCopy::Copy;
	// Fine traversal may fall back to complete roots. This queue target never
	// limits admitted instances or the complete root coverage allocation.
	static constexpr uint32_t FINE_WORK_TARGET = 1048576;
	VirtualGeometrySelectShaderRD shader;
	VirtualGeometrySelectDispatchShaderRD dispatch_shader;
	RID version;
	RID dispatch_version;
	PipelineDeferredRD pipelines[2];
	PipelineDeferredRD dispatch_pipeline;
	RID scratch;
	uint32_t scratch_words = 0;
	RID result_buffer;
	uint32_t result_words = 0;
	RID metadata_buffer;
	uint32_t metadata_words = 0;
	RID transforms_buffer;
	uint32_t transforms_words = 0;
	RID feedback_buffer;
	uint32_t feedback_words = 0;
	RID dispatch_buffer;
	uint32_t dispatch_words = 0;
	RID dispatch_inputs;
	uint32_t dispatch_input_words = 0;
	RID view_parameters;
	RID source_addresses;
	uint32_t source_address_words = 0;
	struct SourceAddresses {
		uint64_t metadata = 0;
		uint64_t transforms = 0;
	};
	static_assert(sizeof(SourceAddresses) == 16);
	LocalVector<SourceAddresses> source_address_data;
	StorageBufferSourceReads source_reads;
	LocalVector<Result> results;
	LocalVector<Batch> batches;
	LocalVector<Parameters> parameter_data;
	HashMap<RID, uint32_t> metadata_offsets;
	LocalVector<BufferCopy> metadata_copies;
	LocalVector<BufferCopy> transform_copies;
	uint32_t used = 0;
	Transform3D camera_inverse;
	Projection projection;
	float near_plane = 0;
	float lod_scale = 0;
	float frustum_margin = 0;
	bool orthographic = false;
	uint32_t shadow_flags = 0;
};

} // namespace RendererRD
