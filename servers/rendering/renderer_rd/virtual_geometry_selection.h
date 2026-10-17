// SPDX-License-Identifier: MIT
#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_select.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_select_dispatch.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_geometry_storage.h"

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
		RID transforms;
		Transform3D model;
		uint32_t count = 1;
		uint32_t stride = 0;
		uint32_t transform_offset = 0;
		RID material_override;
		Vector<RID> surface_materials;
		uint32_t layer_mask = 1;
		int32_t shader_uniforms_offset = -1;
		bool has_color = false;
		bool has_custom = false;
	};
	struct Result {
		Batch batch;
		RID work;
		RID parameters;
		RID dispatch;
		uint32_t words = 0;
		uint32_t selected_capacity = 0;
	};

	void begin(const Transform3D &p_camera, const Projection &p_projection, bool p_flip_y, const Size2i &p_size);
	Error record(const Batch &p_batch);
	Span<Result> get_results() const { return Span<Result>(results.ptr(), used); }
	uint64_t get_buffer_bytes() const;
	~VirtualGeometrySelection();

private:
	static constexpr uint32_t MAX_WORK_ITEMS = 1048576;
	VirtualGeometrySelectShaderRD shader;
	VirtualGeometrySelectDispatchShaderRD dispatch_shader;
	RID version;
	RID dispatch_version;
	PipelineDeferredRD pipeline;
	PipelineDeferredRD dispatch_pipeline;
	RID dummy_transforms;
	LocalVector<Result> results;
	uint32_t used = 0;
	Transform3D camera_inverse;
	Projection projection;
	float near_plane = 0;
	float lod_scale = 0;
	float frustum_margin = 0;
	bool orthographic = false;
};

} // namespace RendererRD
