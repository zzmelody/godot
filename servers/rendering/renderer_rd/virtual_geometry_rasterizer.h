// SPDX-License-Identifier: MIT
#pragma once

#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_depth.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_raster_bin.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_raster_dispatch.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_raster_sw.glsl.gen.h"
#include "servers/rendering/renderer_rd/virtual_geometry_selection.h"
#include "servers/rendering/renderer_rd/virtual_geometry_shading.h"

namespace RendererRD {

// View-owned mixed raster work and one atomic visibility surface. Selection,
// source transforms and MeshStorage buffers are borrowed for this frame only.
// The depth resolve borrows the view's framebuffer and preserves ordinary depth.
class VirtualGeometryRasterizer {
public:
	enum DepthMode {
		DEPTH_ONLY,
		DEPTH_NORMAL_ROUGHNESS,
		DEPTH_NORMAL_ROUGHNESS_VOXEL_GI,
		DEPTH_MODE_MAX,
	};
	Error record(const Ref<RenderSceneBuffersRD> &p_buffers, const VirtualGeometryShading::ViewParameters &p_view, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material);
	Error record_depth(RID p_framebuffer, DepthMode p_mode, RID p_fitting_table, const Rect2i &p_region);
	RID get_visibility() const { return ready ? visibility : RID(); }
	uint64_t get_buffer_bytes() const;
	const VirtualGeometryShading &get_shading() const { return shading; }
	VirtualGeometryShading &get_shading() { return shading; }
	~VirtualGeometryRasterizer();

private:
	struct Pass {
		VirtualGeometryShading::RasterMaterialWork work;
		RID uniforms[3];
	};
	struct Slot {
		LocalVector<Pass> passes;
		RID bins;
		RID arguments;
		RID build_uniforms;
		RID dispatch_uniforms;
		RID software_uniforms;
		RID hardware_uniforms;
		uint32_t capacity = 0;
	};
	VirtualGeometryRasterBinShaderRD bin_shader;
	VirtualGeometryRasterDispatchShaderRD dispatch_shader;
	VirtualGeometryRasterSwShaderRD software_shader;
	VirtualGeometryRasterShaderRD hardware_shader;
	VirtualGeometryDepthShaderRD depth_shader;
	RID depth_version;
	PipelineCacheRD depth_pipelines[DEPTH_MODE_MAX];
	RID bin_version, dispatch_version, software_version, hardware_version;
	PipelineDeferredRD bin_pipeline, dispatch_pipeline, software_pipeline;
	RID hardware_pipeline;
	RID framebuffer;
	RID visibility;
	LocalVector<Slot> slots;
	Size2i size;
	bool ready = false;
	VirtualGeometryShading shading;

	Error _prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material);
};

} // namespace RendererRD
