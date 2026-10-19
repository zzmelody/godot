// SPDX-License-Identifier: MIT
#pragma once

#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_depth.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_raster_bin.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_raster_material_bin.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_raster_material_dispatch.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_scan.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_reactivity.glsl.gen.h"
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
		DEPTH_SHADOW,
		DEPTH_MOTION,
		DEPTH_MODE_MAX,
	};
	Error record(const Ref<RenderSceneBuffersRD> &p_buffers, const VirtualGeometryShading::ViewParameters &p_view, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material);
	Error record_shadow(const Size2i &p_size, const VirtualGeometryShading::ViewParameters &p_view, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material);
	Error record_sdfgi(const Size2i &p_size, const VirtualGeometryShading::ViewParameters &p_view, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material,
			const AABB &p_bounds, const Vector3i &p_from, const Vector3i &p_grid_size, RID p_albedo, RID p_emission, RID p_emission_aniso, RID p_facing);
	Error recover(const VirtualGeometryShading::ViewParameters &p_view);
	Error record_reactivity(RID p_depth, RID p_scene_reactivity, bool p_combine_scene);
	Error record_depth(RID p_framebuffer, DepthMode p_mode, RID p_fitting_table, const Rect2i &p_region);
	RID get_visibility() const { return ready ? visibility : RID(); }
	void clear_view() { ready = false; shading.clear_view(); }
	uint64_t get_buffer_bytes() const;
	const VirtualGeometryShading &get_shading() const { return shading; }
	VirtualGeometryShading &get_shading() { return shading; }
	~VirtualGeometryRasterizer();

private:
	VirtualGeometryWorkMemory memory;
	struct Pass {
		VirtualGeometryShading::RasterMaterialWork work;
		RID uniforms[3];
	};
	struct ScanLevel {
		uint32_t count;
		uint32_t input;
		uint32_t output;
		uint32_t sums = UINT32_MAX;
	};
	VirtualGeometryRasterBinShaderRD bin_shader;
	VirtualGeometryRasterMaterialBinShaderRD material_bin_shader;
	VirtualGeometryRasterMaterialDispatchShaderRD dispatch_shader;
	VirtualGeometryShadeScanShaderRD scan_shader;
	VirtualGeometryRasterSwShaderRD software_shader;
	VirtualGeometryRasterShaderRD hardware_shader;
	VirtualGeometryDepthShaderRD depth_shader;
	VirtualGeometryReactivityShaderRD reactivity_shader;
	RID reactivity_version;
	PipelineDeferredRD reactivity_pipeline;
	RID depth_version;
	PipelineCacheRD depth_pipelines[DEPTH_MODE_MAX];
	RID bin_version, material_bin_version, dispatch_version, scan_version, software_version, hardware_version;
	PipelineDeferredRD bin_pipeline, material_bin_pipeline, dispatch_pipeline, scan_pipeline, software_pipeline;
	RID hardware_pipeline;
	RID framebuffer;
	RID visibility;
	LocalVector<Pass> passes;
	LocalVector<ScanLevel> scan_levels;
	RID candidates, classified, arguments;
	RID pending, occlusion_parameters;
	RID voxel_parameters;
	RID geometry_buffer; // Borrowed for the current prepared view only.
	uint32_t pending_words = 0;
	RID candidate_uniforms, scan_uniforms, dispatch_uniforms;
	uint32_t candidate_words = 0, classified_words = 0, argument_words = 0;
	uint32_t bucket_count = 0, entries_word = 0, candidate_capacity = 0, visible_capacity = 0;
	Size2i size;
	bool ready = false;
	VirtualGeometryShading shading;

	Error _prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, bool p_shadow, bool p_motion);
	Error _prepare_bins(Span<VirtualGeometrySelection::Result> p_sources, bool p_voxel = false);
	Error _prepare_candidates(const VirtualGeometryShading::ViewParameters &p_view);
	void _record_material_bins(uint32_t p_phase);
	Error _record_geometry(const VirtualGeometryShading::ViewParameters &p_view, uint32_t p_phase);
	Error _record(const Ref<RenderSceneBuffersRD> &p_buffers, const Size2i &p_size, const VirtualGeometryShading::ViewParameters &p_view, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, bool p_shadow);
};

} // namespace RendererRD
