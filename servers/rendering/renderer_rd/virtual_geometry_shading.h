// SPDX-License-Identifier: MIT
#pragma once

#include "core/templates/a_hash_map.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_scene_init.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_bin.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_dispatch.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_scan.glsl.gen.h"
#include "servers/rendering/renderer_rd/virtual_geometry_selection.h"

namespace RendererRD {

// View-owned frame source/visibility lookup and primitive-preserving material
// quad bins. Materials and source buffers remain borrowed renderer resources.
// Material evaluation and surface/lighting output are separate later stages.
class VirtualGeometryShading {
public:
	Error prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, RID p_visibility, RID p_geometry);
	void record_sources(Span<VirtualGeometrySelection::Result> p_sources);
	void record_bins();
	RID get_scene() const { return scene_buffer; }
	RID get_bins() const { return bins_buffer; }
	RID get_arguments() const { return arguments; }
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
