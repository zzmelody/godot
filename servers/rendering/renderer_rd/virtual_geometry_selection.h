// SPDX-License-Identifier: MIT
#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_select.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_select_dispatch.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_geometry_storage.h"
#include "servers/rendering/renderer_rd/virtual_geometry_work_memory.h"

class RenderGeometryInstance;

namespace RendererRD {

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
		RID material_override;
		Vector<RID> surface_materials;
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
	struct Parameters {
		float model[16];
		uint32_t source[4];
		uint32_t limits[4];
		uint32_t offsets[4];
		uint32_t addresses[4];
		uint32_t prefix[4];
	};
	struct BufferCopy {
		RID source;
		uint32_t source_byte;
		uint32_t destination_byte;
		uint32_t bytes;
	};
	// Fine traversal may fall back to complete roots. This queue target never
	// limits admitted instances or the complete root coverage allocation.
	static constexpr uint32_t FINE_WORK_TARGET = 1048576;
	VirtualGeometrySelectShaderRD shader;
	VirtualGeometrySelectDispatchShaderRD dispatch_shader;
	RID version;
	RID dispatch_version;
	PipelineDeferredRD pipeline;
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
