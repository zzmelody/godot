// SPDX-License-Identifier: MIT
#pragma once

#include "core/templates/a_hash_map.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_scene_init.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_bin.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_dispatch.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_shade_scan.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/virtual_geometry_selection.h"
#include "servers/rendering/renderer_rd/virtual_geometry_uniform_cache.h"

namespace RendererRD {

// View-owned source snapshots, primitive-preserving material quad bins and
// compute material dispatch. Material/geometry RIDs remain borrowed. Surface
// textures belong to RenderSceneBuffersRD; lighting is a separate consumer.
class VirtualGeometryShading {
public:
	struct ViewParameters {
		Projection projection;
		Transform3D camera;
		Vector2 jitter;
		float time = 0.0f;
		float exposure = 1.0f;
		uint32_t layers = 1;
		uint32_t shadow_flags = 0;
		float shadow_far = 0;
		bool reverse_cull = false;
		Transform3D main_camera;
		Transform3D previous_camera;
		Projection previous_projection;
		float previous_time = 0;
		bool motion = false;
		bool history_valid = false;
		Vector2 previous_jitter;
		uint32_t view_index = 0;
		Vector3 eye_offset;
		bool multiview = false;
		bool prediction = false;
		RID occlusion;
		Projection occlusion_projection;
	};
	Error prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, RID p_visibility, RID p_geometry, bool p_depth_only, bool p_motion);
	Error prepare_materials(const Ref<RenderSceneBuffersRD> &p_buffers, RID p_geometry, const ViewParameters &p_view, RD::FramebufferFormatID p_raster_format, bool p_voxel = false);
	void record_sources(Span<VirtualGeometrySelection::Result> p_sources);
	void record_bins();
	void record_materials();
	void clear_view();
	RID get_surface(uint32_t p_index) const { return surfaces[p_index]; }
	RID get_extensions() const { return extensions; }
	uint32_t get_extension_stride() const { return extension_stride; }
	RID get_motion() const { return motion_surface; }
	RID get_reactivity() const { return reactive_surface; }
	bool are_materials_ready() const { return materials_ready; }
	RID get_scene() const { return scene_buffer; }
	RID get_bins() const { return bins_buffer; }
	RID get_arguments() const { return arguments; }
	RID get_draw_arguments() const { return draw_arguments; }
	struct RasterMaterialWork {
		uint32_t material = 0;
		RID programs[3];
		RID pipelines[3];
		RID parameters[3];
	};
	Span<RasterMaterialWork> get_raster_work() const { return Span<RasterMaterialWork>(raster_work.ptr(), raster_work.size()); }
	RID get_material_view() const { return material_view; }
	RID get_frame_history() const { return frame_history; }
	RID get_previous_globals() const { return previous_global_binding; }
	RID get_status() const { return view_status; }
	RID get_raster_materials() const { return raster_material_buffer; }
	uint32_t get_entries_word() const { return entries_word; }
	uint32_t get_source_base(uint32_t p_batch) const { return sources[p_batch].first; }
	Span<RID> get_materials() const { return Span<RID>(materials.ptr(), materials.size()); }
	uint64_t get_buffer_bytes() const;
	uint64_t get_raster_candidate_capacity(Span<VirtualGeometrySelection::Result> p_sources);
	~VirtualGeometryShading();

private:
	VirtualGeometryWorkMemory memory;
	struct Source {
		uint32_t first = 0;
		uint32_t map = 0;
		uint32_t visible_base = 0;
		uint32_t material_slots = 0;
		uint32_t batch_word = 0;
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
	RID frame_history;
	RID global_history[2];
	uint32_t global_history_words[2] = {};
	RID previous_global_binding;
	uint64_t global_history_frame = UINT64_MAX;
	uint64_t global_history_layout = 0;
	uint64_t temporal_frame = UINT64_MAX;
	bool global_history_valid = false;
	struct MaterialHistoryInput {
		RID shader;
		// Borrowed only during capture on the render thread, never retained.
		const uint8_t *uniform_data = nullptr;
		uint64_t shader_generation = 0;
		uint64_t texture_revision = 0;
		uint32_t bytes = 0;
		float maximum_displacement = 0.0f;
		bool dynamic_textures = false;
	};
	struct MaterialHistory {
		RID uniforms[2];
		RID shader;
		uint64_t frame = UINT64_MAX;
		uint64_t shader_generation = 0;
		uint64_t texture_revision = 0;
		uint32_t bytes = 0;
		float maximum_displacement = 0.0f;
		bool valid = false;
	};
	HashMap<RID, MaterialHistory> material_history;
	void _prune_material_history(uint64_t p_frame, bool p_enabled);
	Error _capture_material_history(RID p_material, const MaterialHistoryInput &p_input, uint64_t p_frame, RID &r_previous, bool &r_valid);
	RID previous_sources;
	uint32_t previous_source_words = 0;
	RID batch_parameters;
	uint32_t batch_parameter_words = 0;
	struct BatchParameters {
		float model[16];
		uint32_t source[4];
		uint32_t displacement[4];
		float previous_model[16];
		uint32_t previous_info[4];
	};
	LocalVector<BatchParameters> batch_parameter_data;
	VirtualGeometryUniformCache source_bindings{ { 2, 4, 8, 26, 27 } };
	RID view_status;
	LocalVector<RasterMaterialWork> raster_work;
	RID raster_material_buffer;
	uint32_t raster_material_words = 0;
	LocalVector<uint32_t> raster_material_data;
	RID surfaces[10];
	RID extensions;
	uint32_t extension_words = 0;
	uint32_t extension_stride = 0;
	RID motion_surface;
	RID reactive_surface;
	RID temporal_keys[2];
	uint32_t temporal_key_words[2] = {};
	Size2i motion_size;
	StringName surface_context;
	StringName motion_context;
	bool materials_ready = false;
	bool depth_only = false;
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
	AHashMap<RID, uint32_t> material_ids[2];
	LocalVector<uint8_t> material_two_sided;
	LocalVector<uint32_t> material_map;
	LocalVector<uint32_t> raster_bucket_marks;
	uint32_t raster_bucket_epoch = 0;
};

} // namespace RendererRD
