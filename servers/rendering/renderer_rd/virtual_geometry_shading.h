// SPDX-License-Identifier: MIT
#pragma once

#include "core/templates/a_hash_map.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_scene_init.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_bin.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_dispatch.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_scan.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/virtual_geometry_selection.h"

namespace RendererRD {

// View-owned source snapshots, primitive-preserving material quad bins and
// compute material dispatch. Material/geometry RIDs remain borrowed. Surface
// textures belong to RenderSceneBuffersRD; lighting is a separate consumer.
class VirtualGeometryShading {
public:
	Error prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, RID p_visibility, RID p_geometry);
	void record_sources(Span<VirtualGeometrySelection::Result> p_sources);
	void record_bins();
	Error record_materials(const Ref<RenderSceneBuffersRD> &p_buffers, RID p_geometry, const Projection &p_projection, const Transform3D &p_camera, const Vector2 &p_jitter, float p_time, float p_exposure, uint32_t p_layers);
	RID get_surface(uint32_t p_index) const { return surfaces[p_index]; }
	bool are_materials_ready() const { return materials_ready; }
	RID get_scene() const { return scene_buffer; }
	RID get_bins() const { return bins_buffer; }
	RID get_arguments() const { return arguments; }
	RID get_draw_arguments() const { return draw_arguments; }
	RID get_raster_materials() const { return raster_material_buffer; }
	uint32_t get_entries_word() const { return entries_word; }
	uint32_t get_source_base(uint32_t p_batch) const { return sources[p_batch].first; }
	Span<RID> get_materials() const { return Span<RID>(materials.ptr(), materials.size()); }
	uint64_t get_buffer_bytes() const;
	~VirtualGeometryShading();

private:
	struct Source {
		uint32_t first = 0;
		uint32_t map = 0;
		RID uniforms;
	};
	struct ScanLevel {
		uint32_t count = 0;
		uint32_t input = 0;
		uint32_t output = 0;
		uint32_t sums = UINT32_MAX;
	};
	VirtualGeometrySceneInitShaderRD init_shader;
	VirtualGeometryShadeBinShaderRD bin_shader;
	VirtualGeometryShadeScanShaderRD scan_shader;
	VirtualGeometryShadeDispatchShaderRD dispatch_shader;
	RID init_version, bin_version, scan_version, dispatch_version;
	PipelineDeferredRD init_pipeline, bin_pipeline, scan_pipeline, dispatch_pipeline;
	RID scene_buffer, bins_buffer, arguments, dummy_transforms;
	RID draw_arguments;
	uint32_t draw_argument_words = 0;
	RID material_view;
	RID raster_material_buffer;
	uint32_t raster_material_words = 0;
	LocalVector<uint32_t> material_cull_modes;
	RID surfaces[9];
	bool materials_ready = false;
	Size2i surface_size;
	struct MaterialWork {
		RID pipeline;
		RID scene;
		RID material;
	};
	LocalVector<MaterialWork> material_work;
	RID bin_uniforms, scan_uniforms, dispatch_uniforms;
	uint32_t scene_words = 0, bins_words = 0, argument_words = 0;
	uint32_t entries_word = 0;
	Size2i size;
	LocalVector<Source> sources;
	LocalVector<ScanLevel> levels;
	LocalVector<RID> materials;
	AHashMap<RID, uint32_t> material_ids;
	LocalVector<uint32_t> material_map;
};

} // namespace RendererRD
