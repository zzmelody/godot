// SPDX-License-Identifier: MIT
#include "virtual_geometry_rasterizer.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {
namespace {
void dispatch_count(RD::ComputeListID p_list, uint32_t p_count, uint32_t p_threads) {
	const uint32_t groups = MAX(1u, (p_count + p_threads - 1u) / p_threads);
	RD::get_singleton()->compute_list_dispatch(p_list, MIN(65535u, groups), (groups + 65534u) / 65535u, 1);
}
} // namespace

VirtualGeometryRasterizer::~VirtualGeometryRasterizer() {
	RD *rd = RD::get_singleton();
	for (auto &pipeline : depth_pipelines) {
		pipeline.clear();
	}
	if (depth_version.is_valid()) {
		depth_shader.version_free(depth_version);
	}
	for (RID buffer : { candidates, classified, arguments, pending, occlusion_parameters }) {
		memory.free(buffer);
	}
	memory.free(visibility);
	for (RID rid : { hardware_pipeline, framebuffer }) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	reactivity_pipeline.free();
	if (reactivity_version.is_valid()) {
		reactivity_shader.version_free(reactivity_version);
	}
	bin_pipeline.free();
	dispatch_pipeline.free();
	material_bin_pipeline.free();
	scan_pipeline.free();
	software_pipeline.free();
	if (bin_version.is_valid()) {
		bin_shader.version_free(bin_version);
	}
	if (dispatch_version.is_valid()) {
		dispatch_shader.version_free(dispatch_version);
	}
	if (material_bin_version.is_valid()) {
		material_bin_shader.version_free(material_bin_version);
	}
	if (scan_version.is_valid()) {
		scan_shader.version_free(scan_version);
	}
	if (software_version.is_valid()) {
		software_shader.version_free(software_version);
	}
	if (hardware_version.is_valid()) {
		hardware_shader.version_free(hardware_version);
	}
}

Error VirtualGeometryRasterizer::_prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, bool p_shadow) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_COND_V(!rd->has_feature(RD::SUPPORTS_BUFFER_ATOMIC_UINT64_MIN_MAX), ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0 || uint64_t(p_size.x) * p_size.y > UINT32_MAX / 8, ERR_INVALID_PARAMETER);
	uint64_t total = 0;
	for (const auto &source : p_sources) {
		total += source.selected_capacity;
	}
	// Seven payload bits identify the source triangle. Zero is the clear value.
	ERR_FAIL_COND_V(total >= (1u << 25), ERR_OUT_OF_MEMORY);
	visible_capacity = total;
	if (size != p_size || visibility.is_null() || framebuffer.is_null()) {
		if (visibility.is_valid()) {
			memory.free(visibility);
		}
		if (framebuffer.is_valid()) {
			rd->free_rid(framebuffer);
		}
		size = p_size;
		visibility = memory.storage(uint64_t(size.x) * size.y * 8);
		framebuffer = rd->framebuffer_create_empty(size);
	}
	ERR_FAIL_COND_V(visibility.is_null() || framebuffer.is_null(), ERR_CANT_CREATE);
	const Error shade_error = shading.prepare(p_size, p_sources, p_default_material, visibility, p_sources.is_empty() ? RID() : p_sources[0].batch.geometry.geometry, p_shadow);
	if (shade_error != OK) {
		return shade_error;
	}
	if (bin_version.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		modes.write[0] = "#define VG_RASTER_MATERIALS\n#define VG_GLOBAL_RASTER\n";
		bin_shader.initialize(modes);
		modes.write[0] = "";
		dispatch_shader.initialize(modes);
		material_bin_shader.initialize(modes);
		scan_shader.initialize(modes);
		modes.write[0] = "#define VG_RASTER_MATERIALS\n#define VG_GLOBAL_RASTER\n";
		software_shader.initialize(modes);
		hardware_shader.initialize(modes);
		bin_version = bin_shader.version_create();
		dispatch_version = dispatch_shader.version_create();
		material_bin_version = material_bin_shader.version_create();
		scan_version = scan_shader.version_create();
		software_version = software_shader.version_create();
		hardware_version = hardware_shader.version_create();
		bin_pipeline.create_compute_pipeline(bin_shader.version_get_shader(bin_version, 0));
		dispatch_pipeline.create_compute_pipeline(dispatch_shader.version_get_shader(dispatch_version, 0));
		material_bin_pipeline.create_compute_pipeline(material_bin_shader.version_get_shader(material_bin_version, 0));
		scan_pipeline.create_compute_pipeline(scan_shader.version_get_shader(scan_version, 0));
		software_pipeline.create_compute_pipeline(software_shader.version_get_shader(software_version, 0));
		RD::PipelineRasterizationState raster;
		raster.cull_mode = RD::POLYGON_CULL_DISABLED;
		hardware_pipeline = rd->render_pipeline_create(hardware_shader.version_get_shader(hardware_version, 0), rd->framebuffer_get_format(framebuffer),
				RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, raster, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState());
	}
	ERR_FAIL_COND_V(bin_pipeline.get_rid().is_null() || dispatch_pipeline.get_rid().is_null() ||
					material_bin_pipeline.get_rid().is_null() || scan_pipeline.get_rid().is_null() ||
					software_pipeline.get_rid().is_null() || hardware_pipeline.is_null(),
			ERR_CANT_CREATE);

	return OK;
}

Error VirtualGeometryRasterizer::_prepare_bins(Span<VirtualGeometrySelection::Result> p_sources) {
	ERR_FAIL_COND_V(p_sources.is_empty(), ERR_INVALID_PARAMETER);
	bucket_count = shading.get_materials().size() + 1;
	const uint64_t capacity = shading.get_raster_candidate_capacity(p_sources);
	ERR_FAIL_COND_V(capacity == 0 || capacity > UINT32_MAX, ERR_OUT_OF_MEMORY);
	candidate_capacity = capacity;
	scan_levels.clear();
	uint64_t scratch = 2 + uint64_t(bucket_count) * 3;
	ScanLevel level{ bucket_count, 2, 2 + bucket_count };
	while (true) {
		if (level.count <= 128) {
			scan_levels.push_back(level);
			break;
		}
		const uint32_t blocks = (level.count + 127) / 128;
		level.sums = scratch;
		scratch += blocks;
		scan_levels.push_back(level);
		level = { blocks, level.sums, uint32_t(scratch) };
		scratch += blocks;
	}
	ERR_FAIL_COND_V(scratch > UINT32_MAX, ERR_OUT_OF_MEMORY);
	entries_word = scratch;
	Error error = memory.ensure(candidates, candidate_words, scratch + capacity);
	if (error != OK) {
		return error;
	}
	error = memory.ensure(classified, classified_words, uint64_t(bucket_count) * 4 + capacity * 2);
	if (error != OK) {
		return error;
	}
	error = memory.ensure(arguments, argument_words, uint64_t(bucket_count) * 12, true);
	if (error != OK) {
		return error;
	}
	error = memory.ensure(pending, pending_words, visible_capacity);
	if (error != OK) {
		return error;
	}
	if (occlusion_parameters.is_null()) {
		occlusion_parameters = memory.uniform(80);
	}
	ERR_FAIL_COND_V(occlusion_parameters.is_null(), ERR_OUT_OF_MEMORY);
	const RID geometry = p_sources[0].batch.geometry.geometry;
	geometry_buffer = geometry;
	for (const auto &source : p_sources) {
		ERR_FAIL_COND_V(source.batch.geometry.geometry != geometry, ERR_INVALID_DATA);
	}
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	scan_uniforms = cache->get_cache(scan_shader.version_get_shader(scan_version, 0), 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { candidates }));
	dispatch_uniforms = cache->get_cache(dispatch_shader.version_get_shader(dispatch_version, 0), 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { candidates }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, { classified }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, { arguments }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, { shading.get_status() }));
	ERR_FAIL_COND_V(scan_uniforms.is_null() || dispatch_uniforms.is_null(), ERR_CANT_CREATE);
	passes.clear();
	Pass fixed;
	fixed.work.material = UINT32_MAX;
	fixed.work.programs[0] = bin_shader.version_get_shader(bin_version, 0);
	fixed.work.programs[1] = software_shader.version_get_shader(software_version, 0);
	fixed.work.programs[2] = hardware_shader.version_get_shader(hardware_version, 0);
	fixed.work.pipelines[0] = bin_pipeline.get_rid();
	fixed.work.pipelines[1] = software_pipeline.get_rid();
	fixed.work.pipelines[2] = hardware_pipeline;
	passes.push_back(fixed);
	for (const auto &work : shading.get_raster_work()) {
		Pass pass;
		pass.work = work;
		passes.push_back(pass);
	}
	for (Pass &pass : passes) {
		const bool programmable = pass.work.material != UINT32_MAX;
		for (uint32_t mode = 0; mode < 3; ++mode) {
			LocalVector<RD::Uniform> uniforms;
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { geometry }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { p_sources[0].parameters }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { candidates }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, { classified }));
			if (mode != 0) {
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { visibility }));
			}
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { shading.get_scene() }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, { shading.get_raster_materials() }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, { shading.get_status() }));
			if (programmable) {
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 12, { shading.get_material_view() }));
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, { MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer() }));
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 28, { shading.get_frame_history() }));
				MaterialStorage::get_singleton()->samplers_rd_get_default().append_uniforms(uniforms, 32);
			}
			pass.uniforms[mode] = cache->get_cache_vec(pass.work.programs[mode], 0, uniforms);
			ERR_FAIL_COND_V(pass.uniforms[mode].is_null(), ERR_CANT_CREATE);
		}
	}
	return OK;
}

void VirtualGeometryRasterizer::_record_material_bins(uint32_t p_phase) {
	RD *rd = RD::get_singleton();
	rd->buffer_clear(candidates, 0, (2 + bucket_count * 3) * 4);
	rd->buffer_clear(classified, 0, bucket_count * 16);
	const RD::ComputeListID list = rd->compute_list_begin();
	auto bin_pass = [&](uint32_t p_scatter) {
		const uint32_t push[6] = { visible_capacity, bucket_count, entries_word, candidate_capacity, p_scatter, p_phase };
		rd->compute_list_bind_compute_pipeline(list, material_bin_pipeline.get_rid());
		rd->compute_list_bind_uniform_set(list, candidate_uniforms, 0);
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_count(list, visible_capacity, 1);
		rd->compute_list_add_barrier(list);
	};
	bin_pass(0);
	rd->compute_list_bind_compute_pipeline(list, scan_pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, scan_uniforms, 0);
	for (const ScanLevel &level : scan_levels) {
		const uint32_t push[5] = { 0, level.count, level.input, level.output, level.sums };
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_count(list, level.count, 128);
		rd->compute_list_add_barrier(list);
	}
	for (int index = int(scan_levels.size()) - 2; index >= 0; --index) {
		const ScanLevel &level = scan_levels[index];
		const uint32_t push[5] = { 1, level.count, scan_levels[index + 1].output, level.output, 0 };
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		dispatch_count(list, level.count, 128);
		rd->compute_list_add_barrier(list);
	}
	bin_pass(1);
	const uint32_t dispatch[4] = { 0, bucket_count, entries_word, candidate_capacity };
	rd->compute_list_bind_compute_pipeline(list, dispatch_pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, dispatch_uniforms, 0);
	rd->compute_list_set_push_constant(list, dispatch, sizeof(dispatch));
	dispatch_count(list, bucket_count, 64);
	rd->compute_list_end();
}

Error VirtualGeometryRasterizer::record(const Ref<RenderSceneBuffersRD> &p_buffers, const VirtualGeometryShading::ViewParameters &p_view, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material) {
	return _record(p_buffers, p_buffers->get_internal_size(), p_view, p_sources, p_default_material, false);
}

Error VirtualGeometryRasterizer::record_shadow(const Size2i &p_size, const VirtualGeometryShading::ViewParameters &p_view, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material) {
	return _record(Ref<RenderSceneBuffersRD>(), p_size, p_view, p_sources, p_default_material, true);
}

Error VirtualGeometryRasterizer::_record(const Ref<RenderSceneBuffersRD> &p_buffers, const Size2i &p_size, const VirtualGeometryShading::ViewParameters &p_view, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material, bool p_shadow) {
	ready = false;
	if (p_sources.is_empty()) {
		return OK;
	}
	Error error = _prepare(p_size, p_sources, p_default_material, p_shadow);
	if (error != OK) {
		return error;
	}
	error = shading.prepare_materials(p_buffers, p_sources.is_empty() ? RID() : p_sources[0].batch.geometry.geometry, p_view, RD::get_singleton()->framebuffer_get_format(framebuffer));
	if (error != OK) {
		return error;
	}
	error = _prepare_bins(p_sources);
	if (error != OK) {
		return error;
	}
	RD *rd = RD::get_singleton();
	rd->buffer_clear(visibility, 0, uint64_t(size.x) * size.y * 8);
	rd->buffer_clear(shading.get_bins(), 0, 8);
	shading.record_sources(p_sources);
	error = _record_geometry(p_view, 0);
	if (error != OK) {
		return error;
	}
	if (!p_view.prediction) {
		shading.record_bins();
		shading.record_materials();
	}
	ready = true;
	return OK;
}

Error VirtualGeometryRasterizer::recover(const VirtualGeometryShading::ViewParameters &p_view) {
	if (!ready) {
		return OK;
	}
	const Error error = _record_geometry(p_view, 1);
	if (error != OK) {
		ready = false;
		return error;
	}
	shading.record_bins();
	shading.record_materials();
	return OK;
}

Error VirtualGeometryRasterizer::_record_geometry(const VirtualGeometryShading::ViewParameters &p_view, uint32_t p_phase) {
	RD *rd = RD::get_singleton();
	struct Occlusion {
		float projection[16];
		uint32_t flags[4];
	} occlusion = {};
	static_assert(sizeof(Occlusion) == 80);
	MaterialStorage::store_camera(p_view.occlusion_projection, occlusion.projection);
	occlusion.flags[0] = p_view.occlusion.is_valid();
	rd->buffer_update(occlusion_parameters, 0, sizeof(occlusion), &occlusion);
	RID hierarchy = p_view.occlusion.is_valid() ? p_view.occlusion : TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	candidate_uniforms = UniformSetCacheRD::get_singleton()->get_cache(material_bin_shader.version_get_shader(material_bin_version, 0), 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { geometry_buffer }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { shading.get_scene() }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { candidates }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, { pending }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, { shading.get_raster_materials() }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, { shading.get_status() }),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 30, { sampler, hierarchy }),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 31, { occlusion_parameters }));
	ERR_FAIL_COND_V(candidate_uniforms.is_null(), ERR_CANT_CREATE);
	_record_material_bins(p_phase);
	for (const Pass &pass : passes) {
		const uint32_t bucket = pass.work.material == UINT32_MAX ? 0 : pass.work.material + 1;
		const uint32_t dispatch_parameters[4] = { 1, bucket_count, bucket, candidate_capacity };
		struct RasterParameters {
			uint32_t raster[4];
			float jitter[2];
			uint32_t header_words;
			uint32_t material;
		} raster = { { uint32_t(size.x), uint32_t(size.y), entries_word, candidate_capacity },
			{ float(p_view.jitter.x), float(p_view.jitter.y) }, bucket_count * 4, pass.work.material };
		static_assert(sizeof(RasterParameters) == 32);
		rd->draw_command_begin_label("Virtual Geometry Material Raster");
		RENDER_TIMESTAMP("Virtual Geometry Raster Bins Begin");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pass.work.pipelines[0]);
		rd->compute_list_bind_uniform_set(list, pass.uniforms[0], 0);
		if (pass.work.parameters[0].is_valid()) {
			rd->compute_list_bind_uniform_set(list, pass.work.parameters[0], 3);
		}
		rd->compute_list_set_push_constant(list, &raster, sizeof(raster));
		rd->compute_list_dispatch_indirect(list, arguments, bucket * 48);
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, dispatch_pipeline.get_rid());
		rd->compute_list_bind_uniform_set(list, dispatch_uniforms, 0);
		rd->compute_list_set_push_constant(list, dispatch_parameters, sizeof(dispatch_parameters));
		dispatch_count(list, 1, 64);
		rd->compute_list_end();
		RENDER_TIMESTAMP("Virtual Geometry Raster Bins End");
		RENDER_TIMESTAMP("Virtual Geometry Software Raster Begin");
		list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pass.work.pipelines[1]);
		rd->compute_list_bind_uniform_set(list, pass.uniforms[1], 0);
		if (pass.work.parameters[1].is_valid()) {
			rd->compute_list_bind_uniform_set(list, pass.work.parameters[1], 3);
		}
		rd->compute_list_set_push_constant(list, &raster, sizeof(raster));
		rd->compute_list_dispatch_indirect(list, arguments, bucket * 48 + 16);
		rd->compute_list_end();
		RENDER_TIMESTAMP("Virtual Geometry Software Raster End");
		RENDER_TIMESTAMP("Virtual Geometry Hardware Raster Begin");
		const RD::DrawListID draw = rd->draw_list_begin(framebuffer);
		rd->draw_list_bind_render_pipeline(draw, pass.work.pipelines[2]);
		rd->draw_list_bind_uniform_set(draw, pass.uniforms[2], 0);
		if (pass.work.parameters[2].is_valid()) {
			rd->draw_list_bind_uniform_set(draw, pass.work.parameters[2], 3);
		}
		rd->draw_list_set_push_constant(draw, &raster, sizeof(raster));
		rd->draw_list_draw_indirect(draw, false, arguments, bucket * 48 + 32);
		rd->draw_list_end();
		RENDER_TIMESTAMP("Virtual Geometry Hardware Raster End");
		rd->draw_command_end_label();
	}
	return OK;
}

Error VirtualGeometryRasterizer::record_reactivity(RID p_depth, RID p_scene_reactivity, bool p_combine_scene) {
	if (!ready || !shading.are_materials_ready()) {
		return OK;
	}
	RD *rd = RD::get_singleton();
	if (reactivity_version.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		reactivity_shader.initialize(modes);
		reactivity_version = reactivity_shader.version_create();
		reactivity_pipeline.create_compute_pipeline(reactivity_shader.version_get_shader(reactivity_version, 0));
	}
	const RID pipeline = reactivity_pipeline.get_rid();
	ERR_FAIL_COND_V(pipeline.is_null(), ERR_CANT_CREATE);
	const RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID set = UniformSetCacheRD::get_singleton()->get_cache(reactivity_shader.version_get_shader(reactivity_version, 0), 0,
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, { shading.get_reactivity() }),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { sampler, p_depth }),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { sampler, p_scene_reactivity }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, { visibility }));
	ERR_FAIL_COND_V(set.is_null(), ERR_CANT_CREATE);
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	const uint32_t combine = p_combine_scene;
	rd->compute_list_set_push_constant(list, &combine, sizeof(combine));
	rd->compute_list_dispatch_threads(list, size.x, size.y, 1);
	rd->compute_list_end();
	return OK;
}

uint64_t VirtualGeometryRasterizer::get_buffer_bytes() const {
	uint64_t bytes = visibility.is_valid() ? uint64_t(size.x) * size.y * 8 : 0;
	bytes += uint64_t(candidate_words + classified_words + argument_words + pending_words) * 4;
	return bytes + shading.get_buffer_bytes() + (occlusion_parameters.is_valid() ? 80 : 0);
}

Error VirtualGeometryRasterizer::record_depth(RID p_framebuffer, DepthMode p_mode, RID p_fitting_table, const Rect2i &p_region) {
	ERR_FAIL_COND_V(p_mode >= DEPTH_MODE_MAX || p_framebuffer.is_null(), ERR_INVALID_PARAMETER);
	if (!ready || shading.get_materials().is_empty() || (p_mode != DEPTH_SHADOW && shading.get_surface(6).is_null())) {
		return OK;
	}
	RD *rd = RD::get_singleton();
	if (depth_version.is_null()) {
		Vector<String> modes = { "", "#define VG_NORMAL_ROUGHNESS\n", "#define VG_NORMAL_ROUGHNESS\n#define VG_VOXEL_GI\n", "#define VG_SHADOW_DEPTH\n", "#define VG_MOTION\n" };
		depth_shader.initialize(modes);
		depth_version = depth_shader.version_create();
		RD::PipelineRasterizationState raster;
		raster.cull_mode = RD::POLYGON_CULL_DISABLED;
		RD::PipelineDepthStencilState depth;
		depth.enable_depth_test = true;
		depth.enable_depth_write = true;
		depth.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
		for (uint32_t mode = 0; mode < DEPTH_MODE_MAX; ++mode) {
			depth.enable_depth_write = mode != DEPTH_MOTION;
			const uint32_t attachments = mode == DEPTH_SHADOW ? 0 : mode == DEPTH_MOTION ? 1
																						 : mode;
			depth_pipelines[mode].setup(depth_shader.version_get_shader(depth_version, mode), RD::RENDER_PRIMITIVE_TRIANGLES,
					raster, RD::PipelineMultisampleState(), depth, RD::PipelineColorBlendState::create_disabled(attachments));
		}
	}
	RID program = depth_shader.version_get_shader(depth_version, p_mode);
	RID pipeline = depth_pipelines[p_mode].get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(p_framebuffer));
	ERR_FAIL_COND_V(pipeline.is_null(), ERR_CANT_CREATE);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { visibility }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { shading.get_bins() }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { shading.get_status() }));
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	if (p_mode != DEPTH_SHADOW) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { sampler, shading.get_surface(6) }));
	}
	if (p_mode == DEPTH_MOTION) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, { sampler, shading.get_motion() }));
	} else if (p_mode != DEPTH_ONLY && p_mode != DEPTH_SHADOW) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { sampler, shading.get_surface(1) }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, { p_fitting_table }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 5, { sampler }));
	}
	if (p_mode == DEPTH_NORMAL_ROUGHNESS || p_mode == DEPTH_NORMAL_ROUGHNESS_VOXEL_GI) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { shading.get_scene() }));
	}
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(program, 0, uniforms);
	ERR_FAIL_COND_V(set.is_null(), ERR_CANT_CREATE);
	RENDER_TIMESTAMP("Virtual Geometry Depth Resolve");
	const RD::DrawListID list = rd->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, {}, 0.0f, 0, p_region);
	rd->draw_list_bind_render_pipeline(list, pipeline);
	rd->draw_list_bind_uniform_set(list, set, 0);
	if (p_mode == DEPTH_SHADOW) {
		const Rect2i region = p_region.size == Size2i() ? Rect2i(Point2i(), size) : p_region;
		const int32_t shadow[4] = { size.x, size.y, region.position.x, region.position.y };
		rd->draw_list_set_viewport(list, region);
		rd->draw_list_enable_scissor(list, region);
		rd->draw_list_set_push_constant(list, shadow, sizeof(shadow));
	}
	rd->draw_list_draw(list, false, 1, 3);
	rd->draw_list_end();
	return OK;
}

} // namespace RendererRD
