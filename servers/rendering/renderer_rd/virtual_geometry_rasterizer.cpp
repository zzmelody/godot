// SPDX-License-Identifier: MIT
#include "virtual_geometry_rasterizer.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {

VirtualGeometryRasterizer::~VirtualGeometryRasterizer() {
	RD *rd = RD::get_singleton();
	for (auto &pipeline : depth_pipelines) {
		pipeline.clear();
	}
	if (depth_version.is_valid()) {
		depth_shader.version_free(depth_version);
	}
	for (const Slot &slot : slots) {
		if (slot.bins.is_valid()) {
			rd->free_rid(slot.bins);
		}
		if (slot.arguments.is_valid()) {
			rd->free_rid(slot.arguments);
		}
	}
	for (RID rid : { hardware_pipeline, framebuffer, visibility }) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	bin_pipeline.free();
	dispatch_pipeline.free();
	software_pipeline.free();
	if (bin_version.is_valid()) {
		bin_shader.version_free(bin_version);
	}
	if (dispatch_version.is_valid()) {
		dispatch_shader.version_free(dispatch_version);
	}
	if (software_version.is_valid()) {
		software_shader.version_free(software_version);
	}
	if (hardware_version.is_valid()) {
		hardware_shader.version_free(hardware_version);
	}
}

Error VirtualGeometryRasterizer::_prepare(const Size2i &p_size, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_COND_V(!rd->has_feature(RD::SUPPORTS_BUFFER_ATOMIC_UINT64_MIN_MAX), ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0 || uint64_t(p_size.x) * p_size.y > UINT32_MAX / 8, ERR_INVALID_PARAMETER);
	uint64_t total = 0;
	for (const auto &source : p_sources) {
		total += source.selected_capacity;
	}
	// Seven payload bits identify the source triangle. Zero is the clear value.
	ERR_FAIL_COND_V(total >= (1u << 25), ERR_OUT_OF_MEMORY);
	if (size != p_size || visibility.is_null() || framebuffer.is_null()) {
		if (visibility.is_valid()) {
			rd->free_rid(visibility);
		}
		if (framebuffer.is_valid()) {
			rd->free_rid(framebuffer);
		}
		size = p_size;
		visibility = rd->storage_buffer_create(uint64_t(size.x) * size.y * 8);
		framebuffer = rd->framebuffer_create_empty(size);
	}
	ERR_FAIL_COND_V(visibility.is_null() || framebuffer.is_null(), ERR_CANT_CREATE);
	const Error shade_error = shading.prepare(p_size, p_sources, p_default_material, visibility, p_sources.is_empty() ? RID() : p_sources[0].batch.geometry.geometry);
	if (shade_error != OK) {
		return shade_error;
	}
	if (bin_version.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		bin_shader.initialize(modes);
		dispatch_shader.initialize(modes);
		modes.write[0] = "#define VG_RASTER_MATERIALS\n";
		software_shader.initialize(modes);
		hardware_shader.initialize(modes);
		bin_version = bin_shader.version_create();
		dispatch_version = dispatch_shader.version_create();
		software_version = software_shader.version_create();
		hardware_version = hardware_shader.version_create();
		bin_pipeline.create_compute_pipeline(bin_shader.version_get_shader(bin_version, 0));
		dispatch_pipeline.create_compute_pipeline(dispatch_shader.version_get_shader(dispatch_version, 0));
		software_pipeline.create_compute_pipeline(software_shader.version_get_shader(software_version, 0));
		RD::PipelineRasterizationState raster;
		raster.cull_mode = RD::POLYGON_CULL_DISABLED;
		hardware_pipeline = rd->render_pipeline_create(hardware_shader.version_get_shader(hardware_version, 0), rd->framebuffer_get_format(framebuffer),
				RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, raster, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState());
	}
	ERR_FAIL_COND_V(bin_pipeline.get_rid().is_null() || dispatch_pipeline.get_rid().is_null() ||
					software_pipeline.get_rid().is_null() || hardware_pipeline.is_null(),
			ERR_CANT_CREATE);
	while (slots.size() < p_sources.size()) {
		slots.push_back(Slot());
	}
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	for (uint32_t index = 0; index < p_sources.size(); ++index) {
		Slot &slot = slots[index];
		const auto &source = p_sources[index];
		if (slot.capacity < source.selected_capacity) {
			if (slot.bins.is_valid()) {
				rd->free_rid(slot.bins);
			}
			slot.capacity = Math::next_power_of_2(source.selected_capacity);
			slot.bins = rd->storage_buffer_create((4 + slot.capacity * 2) * 4);
		}
		if (slot.arguments.is_null()) {
			slot.arguments = rd->storage_buffer_create(48, {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		}
		ERR_FAIL_COND_V(slot.bins.is_null() || slot.arguments.is_null(), ERR_CANT_CREATE);
		const RD::Uniform geometry(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { source.batch.geometry.geometry });
		const RD::Uniform parameters(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { source.parameters });
		const RD::Uniform work(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { source.work });
		const RD::Uniform bins(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, { slot.bins });
		const RD::Uniform pixels(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { visibility });
		const RD::Uniform arguments(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, { slot.arguments });
		const RD::Uniform scene(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { shading.get_scene() });
		const RD::Uniform materials(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, { shading.get_raster_materials() });
		slot.build_uniforms = cache->get_cache(bin_shader.version_get_shader(bin_version, 0), 0, geometry, parameters, work, bins, scene);
		slot.dispatch_uniforms = cache->get_cache(dispatch_shader.version_get_shader(dispatch_version, 0), 0, work, bins, arguments);
		slot.software_uniforms = cache->get_cache(software_shader.version_get_shader(software_version, 0), 0, geometry, parameters, work, bins, pixels, scene, materials);
		slot.hardware_uniforms = cache->get_cache(hardware_shader.version_get_shader(hardware_version, 0), 0, geometry, parameters, work, bins, pixels, scene, materials);
		ERR_FAIL_COND_V(slot.build_uniforms.is_null() || slot.dispatch_uniforms.is_null() || slot.software_uniforms.is_null() || slot.hardware_uniforms.is_null(), ERR_CANT_CREATE);
	}
	return OK;
}

Error VirtualGeometryRasterizer::record(const Size2i &p_size, const Vector2 &p_jitter, Span<VirtualGeometrySelection::Result> p_sources, RID p_default_material) {
	ready = false;
	const Error error = _prepare(p_size, p_sources, p_default_material);
	if (error != OK) {
		return error;
	}
	RD *rd = RD::get_singleton();
	rd->buffer_clear(visibility, 0, uint64_t(size.x) * size.y * 8);
	shading.record_sources(p_sources);
	uint32_t base = 0;
	for (uint32_t index = 0; index < p_sources.size(); ++index) {
		const Slot &slot = slots[index];
		const auto &source = p_sources[index];
		rd->buffer_clear(slot.bins, 0, 16);
		struct RasterParameters {
			uint32_t raster[4];
			float jitter[2];
			uint32_t source_base;
		} raster = { { uint32_t(size.x), uint32_t(size.y), base, source.selected_capacity }, { float(p_jitter.x), float(p_jitter.y) }, shading.get_source_base(index) };
		static_assert(sizeof(RasterParameters) == 28);
		rd->draw_command_begin_label("Virtual Geometry Mixed Raster");
		RENDER_TIMESTAMP("Virtual Geometry Raster Bins Begin");
		RD::ComputeListID list = rd->compute_list_begin();
		auto prepare = [&](uint32_t p_mode) {
			rd->compute_list_bind_compute_pipeline(list, dispatch_pipeline.get_rid());
			rd->compute_list_bind_uniform_set(list, slot.dispatch_uniforms, 0);
			rd->compute_list_set_push_constant(list, &p_mode, sizeof(p_mode));
			rd->compute_list_dispatch(list, 1, 1, 1);
			rd->compute_list_add_barrier(list);
		};
		prepare(0);
		rd->compute_list_bind_compute_pipeline(list, bin_pipeline.get_rid());
		rd->compute_list_bind_uniform_set(list, slot.build_uniforms, 0);
		rd->compute_list_set_push_constant(list, &raster, sizeof(raster));
		rd->compute_list_dispatch_indirect(list, slot.arguments, 0);
		rd->compute_list_add_barrier(list);
		prepare(1);
		rd->compute_list_end();
		RENDER_TIMESTAMP("Virtual Geometry Raster Bins End");
		RENDER_TIMESTAMP("Virtual Geometry Software Raster Begin");
		list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, software_pipeline.get_rid());
		rd->compute_list_bind_uniform_set(list, slot.software_uniforms, 0);
		rd->compute_list_set_push_constant(list, &raster, sizeof(raster));
		rd->compute_list_dispatch_indirect(list, slot.arguments, 16);
		rd->compute_list_end();
		RENDER_TIMESTAMP("Virtual Geometry Software Raster End");
		RENDER_TIMESTAMP("Virtual Geometry Hardware Raster Begin");
		const RD::DrawListID draw = rd->draw_list_begin(framebuffer);
		rd->draw_list_bind_render_pipeline(draw, hardware_pipeline);
		rd->draw_list_bind_uniform_set(draw, slot.hardware_uniforms, 0);
		rd->draw_list_set_push_constant(draw, &raster, sizeof(raster));
		rd->draw_list_draw_indirect(draw, false, slot.arguments, 32);
		rd->draw_list_end();
		RENDER_TIMESTAMP("Virtual Geometry Hardware Raster End");
		rd->draw_command_end_label();
		base += source.selected_capacity;
	}
	shading.record_bins();
	ready = true;
	return OK;
}

uint64_t VirtualGeometryRasterizer::get_buffer_bytes() const {
	uint64_t bytes = visibility.is_valid() ? uint64_t(size.x) * size.y * 8 : 0;
	for (const Slot &slot : slots) {
		bytes += uint64_t(4 + slot.capacity * 2) * 4 + 48;
	}
	return bytes + shading.get_buffer_bytes();
}

Error VirtualGeometryRasterizer::record_depth(RID p_framebuffer, DepthMode p_mode, RID p_fitting_table, const Rect2i &p_region) {
	ERR_FAIL_COND_V(p_mode >= DEPTH_MODE_MAX || p_framebuffer.is_null(), ERR_INVALID_PARAMETER);
	if (!ready || shading.get_materials().is_empty() || shading.get_surface(6).is_null()) {
		return OK;
	}
	RD *rd = RD::get_singleton();
	if (depth_version.is_null()) {
		Vector<String> modes = { "", "#define VG_NORMAL_ROUGHNESS\n", "#define VG_NORMAL_ROUGHNESS\n#define VG_VOXEL_GI\n" };
		depth_shader.initialize(modes);
		depth_version = depth_shader.version_create();
		RD::PipelineRasterizationState raster;
		raster.cull_mode = RD::POLYGON_CULL_DISABLED;
		RD::PipelineDepthStencilState depth;
		depth.enable_depth_test = true;
		depth.enable_depth_write = true;
		depth.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
		for (uint32_t mode = 0; mode < DEPTH_MODE_MAX; ++mode) {
			depth_pipelines[mode].setup(depth_shader.version_get_shader(depth_version, mode), RD::RENDER_PRIMITIVE_TRIANGLES,
					raster, RD::PipelineMultisampleState(), depth, RD::PipelineColorBlendState::create_disabled(mode));
		}
	}
	RID program = depth_shader.version_get_shader(depth_version, p_mode);
	RID pipeline = depth_pipelines[p_mode].get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(p_framebuffer));
	ERR_FAIL_COND_V(pipeline.is_null(), ERR_CANT_CREATE);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { visibility }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { shading.get_bins() }));
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { sampler, shading.get_surface(6) }));
	if (p_mode != DEPTH_ONLY) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { sampler, shading.get_surface(1) }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, { p_fitting_table }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 5, { sampler }));
	}
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(program, 0, uniforms);
	ERR_FAIL_COND_V(set.is_null(), ERR_CANT_CREATE);
	RENDER_TIMESTAMP("Virtual Geometry Depth Resolve");
	const RD::DrawListID list = rd->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, {}, 0.0f, 0, p_region);
	rd->draw_list_bind_render_pipeline(list, pipeline);
	rd->draw_list_bind_uniform_set(list, set, 0);
	rd->draw_list_draw(list, false, 1, 3);
	rd->draw_list_end();
	return OK;
}

} // namespace RendererRD
