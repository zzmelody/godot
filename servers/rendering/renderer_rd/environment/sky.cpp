/**************************************************************************/
/*  sky.cpp                                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "sky.h"

#include "core/config/project_settings.h"
#include "core/math/math_defs.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

#define RB_SCOPE_SKY SNAME("sky_buffers")
#define RB_HALF_TEXTURE SNAME("half_texture")
#define RB_QUARTER_TEXTURE SNAME("quarter_texture")

////////////////////////////////////////////////////////////////////////////////
// SKY SHADER

void SkyRD::SkyShaderData::set_code(const String &p_code) {
	//compile

	code = p_code;
	valid = false;
	ubo_size = 0;
	uniforms.clear();

	if (code.is_empty()) {
		return; //just invalid, but no error
	}

	ShaderCompiler::GeneratedCode gen_code;
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["sky"] = ShaderCompiler::STAGE_FRAGMENT;

	uses_time = false;
	uses_half_res = false;
	uses_quarter_res = false;
	uses_position = false;
	uses_light = false;

	actions.render_mode_flags["use_half_res_pass"] = &uses_half_res;
	actions.render_mode_flags["use_quarter_res_pass"] = &uses_quarter_res;

	actions.usage_flag_pointers["TIME"] = &uses_time;
	actions.usage_flag_pointers["POSITION"] = &uses_position;
	actions.usage_flag_pointers["LIGHT0_ENABLED"] = &uses_light;
	actions.usage_flag_pointers["LIGHT0_ENERGY"] = &uses_light;
	actions.usage_flag_pointers["LIGHT0_DIRECTION"] = &uses_light;
	actions.usage_flag_pointers["LIGHT0_COLOR"] = &uses_light;
	actions.usage_flag_pointers["LIGHT0_SIZE"] = &uses_light;
	actions.usage_flag_pointers["LIGHT1_ENABLED"] = &uses_light;
	actions.usage_flag_pointers["LIGHT1_ENERGY"] = &uses_light;
	actions.usage_flag_pointers["LIGHT1_DIRECTION"] = &uses_light;
	actions.usage_flag_pointers["LIGHT1_COLOR"] = &uses_light;
	actions.usage_flag_pointers["LIGHT1_SIZE"] = &uses_light;
	actions.usage_flag_pointers["LIGHT2_ENABLED"] = &uses_light;
	actions.usage_flag_pointers["LIGHT2_ENERGY"] = &uses_light;
	actions.usage_flag_pointers["LIGHT2_DIRECTION"] = &uses_light;
	actions.usage_flag_pointers["LIGHT2_COLOR"] = &uses_light;
	actions.usage_flag_pointers["LIGHT2_SIZE"] = &uses_light;
	actions.usage_flag_pointers["LIGHT3_ENABLED"] = &uses_light;
	actions.usage_flag_pointers["LIGHT3_ENERGY"] = &uses_light;
	actions.usage_flag_pointers["LIGHT3_DIRECTION"] = &uses_light;
	actions.usage_flag_pointers["LIGHT3_COLOR"] = &uses_light;
	actions.usage_flag_pointers["LIGHT3_SIZE"] = &uses_light;

	actions.uniforms = &uniforms;

	// !BAS! Contemplate making `SkyShader sky` accessible from this struct or even part of this struct.
	RendererSceneRenderRD *scene_singleton = static_cast<RendererSceneRenderRD *>(RendererSceneRenderRD::singleton);

	Error err = scene_singleton->sky.sky_shader.compiler.compile(RSE::SHADER_SKY, code, &actions, path, gen_code);
	ERR_FAIL_COND_MSG(err != OK, "Shader compilation failed.");

	if (version.is_null()) {
		version = scene_singleton->sky.sky_shader.shader.version_create();
	}

#if 0
	print_line("**compiling shader:");
	print_line("**defines:\n");
	for (int i = 0; i < gen_code.defines.size(); i++) {
		print_line(gen_code.defines[i]);
	}

	HashMap<String, String>::Iterator el = gen_code.code.begin();
	while (el) {
		print_line("\n**code " + el->key + ":\n" + el->value);
		++el;
	}

	print_line("\n**uniforms:\n" + gen_code.uniforms);
	print_line("\n**vertex_globals:\n" + gen_code.stage_globals[ShaderCompiler::STAGE_VERTEX]);
	print_line("\n**fragment_globals:\n" + gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT]);
#endif

	scene_singleton->sky.sky_shader.shader.version_set_code(version, gen_code.code, gen_code.uniforms, gen_code.stage_globals[ShaderCompiler::STAGE_VERTEX], gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT], gen_code.defines);
	ERR_FAIL_COND(!scene_singleton->sky.sky_shader.shader.version_is_valid(version));

	ubo_size = gen_code.uniform_total_size;
	ubo_offsets = gen_code.uniform_offsets;
	texture_uniforms = gen_code.texture_uniforms;

	//update pipelines

	for (int i = 0; i < SKY_VERSION_MAX; i++) {
		RD::PipelineDepthStencilState depth_stencil_state;
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;

		if (scene_singleton->sky.sky_shader.shader.is_variant_enabled(i)) {
			RID shader_variant = scene_singleton->sky.sky_shader.shader.version_get_shader(version, i);
			pipelines[i].setup(shader_variant, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), depth_stencil_state, RD::PipelineColorBlendState::create_disabled(), 0);
		} else {
			pipelines[i].clear();
		}
	}

	valid = true;
}

bool SkyRD::SkyShaderData::is_animated() const {
	return false;
}

bool SkyRD::SkyShaderData::casts_shadows() const {
	return false;
}

RenderingServerTypes::ShaderNativeSourceCode SkyRD::SkyShaderData::get_native_source_code() const {
	RendererSceneRenderRD *scene_singleton = static_cast<RendererSceneRenderRD *>(RendererSceneRenderRD::singleton);

	return scene_singleton->sky.sky_shader.shader.version_get_native_source_code(version);
}

Pair<ShaderRD *, RID> SkyRD::SkyShaderData::get_native_shader_and_version() const {
	RendererSceneRenderRD *scene_singleton = static_cast<RendererSceneRenderRD *>(RendererSceneRenderRD::singleton);
	return { &scene_singleton->sky.sky_shader.shader, version };
}

SkyRD::SkyShaderData::~SkyShaderData() {
	RendererSceneRenderRD *scene_singleton = static_cast<RendererSceneRenderRD *>(RendererSceneRenderRD::singleton);
	ERR_FAIL_NULL(scene_singleton);
	//pipeline variants will clear themselves if shader is gone
	if (version.is_valid()) {
		scene_singleton->sky.sky_shader.shader.version_free(version);
	}
}

////////////////////////////////////////////////////////////////////////////////
// Sky material

bool SkyRD::SkyMaterialData::update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) {
	RendererSceneRenderRD *scene_singleton = static_cast<RendererSceneRenderRD *>(RendererSceneRenderRD::singleton);

	uniform_set_updated = true;

	return update_parameters_uniform_set(p_parameters, p_uniform_dirty, p_textures_dirty, shader_data->uniforms, shader_data->ubo_offsets.ptr(), shader_data->texture_uniforms, shader_data->default_texture_params, shader_data->ubo_size, uniform_set, scene_singleton->sky.sky_shader.shader.version_get_shader(shader_data->version, 0), SKY_SET_MATERIAL, true, true);
}

SkyRD::SkyMaterialData::~SkyMaterialData() {
	free_parameters_uniform_set(uniform_set);
}

////////////////////////////////////////////////////////////////////////////////
// Render sky

static _FORCE_INLINE_ void store_transform_3x3(const Basis &p_basis, float *p_array) {
	p_array[0] = p_basis.rows[0][0];
	p_array[1] = p_basis.rows[1][0];
	p_array[2] = p_basis.rows[2][0];
	p_array[3] = 0;
	p_array[4] = p_basis.rows[0][1];
	p_array[5] = p_basis.rows[1][1];
	p_array[6] = p_basis.rows[2][1];
	p_array[7] = 0;
	p_array[8] = p_basis.rows[0][2];
	p_array[9] = p_basis.rows[1][2];
	p_array[10] = p_basis.rows[2][2];
	p_array[11] = 0;
}

void SkyRD::_render_sky(RD::DrawListID p_list, float p_time, RID p_fb, PipelineCacheRD *p_pipeline, RID p_uniform_set, RID p_texture_set, const Projection &p_projection, const Basis &p_orientation, const Vector3 &p_position, float p_luminance_multiplier, float p_brightness_multiplier, float p_border_size, RID p_scene_uniform_set) {
	SkyPushConstant sky_push_constant;

	memset(&sky_push_constant, 0, sizeof(SkyPushConstant));

	// We only need key components of our projection matrix
	sky_push_constant.projection[0] = p_projection.columns[2][0];
	sky_push_constant.projection[1] = p_projection.columns[0][0];
	sky_push_constant.projection[2] = p_projection.columns[2][1];
	sky_push_constant.projection[3] = p_projection.columns[1][1];

	sky_push_constant.position[0] = p_position.x;
	sky_push_constant.position[1] = p_position.y;
	sky_push_constant.position[2] = p_position.z;
	sky_push_constant.time = p_time;
	sky_push_constant.border_size[0] = p_border_size;
	sky_push_constant.border_size[1] = 1.0f - p_border_size * 2.0;
	sky_push_constant.luminance_multiplier = p_luminance_multiplier;
	sky_push_constant.brightness_multiplier = p_brightness_multiplier;
	store_transform_3x3(p_orientation, sky_push_constant.orientation);

	RenderingDevice::FramebufferFormatID fb_format = RD::get_singleton()->framebuffer_get_format(p_fb);

	RD::DrawListID draw_list = p_list;

	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, p_pipeline->get_render_pipeline(RD::INVALID_ID, fb_format, false, RD::get_singleton()->draw_list_get_current_pass()));

	// Update uniform sets.
	{
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, p_scene_uniform_set.is_valid() ? p_scene_uniform_set : sky_scene_state.uniform_set, SKY_SET_UNIFORMS);
		if (p_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_uniform_set)) { // Material may not have a uniform set.
			RD::get_singleton()->draw_list_bind_uniform_set(draw_list, p_uniform_set, SKY_SET_MATERIAL);
		}
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, p_texture_set, SKY_SET_TEXTURES);
		// Fog uniform set can be invalidated before drawing, so validate at draw time
		if (p_scene_uniform_set.is_null() && sky_scene_state.fog_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(sky_scene_state.fog_uniform_set)) {
			RD::get_singleton()->draw_list_bind_uniform_set(draw_list, sky_scene_state.fog_uniform_set, SKY_SET_FOG);
		} else {
			RD::get_singleton()->draw_list_bind_uniform_set(draw_list, sky_scene_state.default_fog_uniform_set, SKY_SET_FOG);
		}
	}

	RD::get_singleton()->draw_list_set_push_constant(draw_list, &sky_push_constant, sizeof(SkyPushConstant));

	RD::get_singleton()->draw_list_draw(draw_list, false, 1u, 3u);
}

////////////////////////////////////////////////////////////////////////////////
// ReflectionData

void SkyRD::ReflectionData::clear_reflection_data() {
	layers.clear();
	radiance_base_octmap = RID();
	if (downsampled_radiance_octmap.is_valid()) {
		RD::get_singleton()->free_rid(downsampled_radiance_octmap);
	}
	downsampled_radiance_octmap = RID();
	downsampled_layer.mipmaps.clear();
	coefficient_buffer = RID();
}

void SkyRD::ReflectionData::update_reflection_data(int p_size, int p_mipmaps, bool p_use_array, RID p_base_cube, int p_base_layer, bool p_low_quality, int p_roughness_layers, RD::DataFormat p_texture_format, float p_border_size) {
	//recreate radiance and all data

	int mipmaps = p_mipmaps;
	uint32_t w = p_size, h = p_size;

	bool use_raster_effect = RendererRD::CopyEffects::get_singleton()->get_raster_effects().has_flag(RendererRD::CopyEffects::RASTER_EFFECT_OCTMAP);
	uv_border_size = p_border_size;

	if (p_use_array) {
		int num_layers = p_low_quality ? Sky::REAL_TIME_ROUGHNESS_LAYERS : p_roughness_layers;
		for (int i = 0; i < num_layers; i++) {
			ReflectionData::Layer layer;
			uint32_t mmw = w;
			uint32_t mmh = h;
			layer.mipmaps.resize(mipmaps);
			for (int j = 0; j < mipmaps; j++) {
				ReflectionData::Layer::Mipmap &mm = layer.mipmaps[j];
				mm.size.width = mmw;
				mm.size.height = mmh;

				mm.view = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), p_base_cube, p_base_layer + i, j);

				Vector<RID> fbtex;
				fbtex.append(mm.view);
				mm.framebuffer = RD::get_singleton()->framebuffer_create(fbtex);

				mmw = MAX(1u, mmw >> 1);
				mmh = MAX(1u, mmh >> 1);
			}

			layers.push_back(layer);
		}
	} else {
		mipmaps = p_low_quality ? Sky::REAL_TIME_ROUGHNESS_LAYERS : mipmaps;

		ReflectionData::Layer layer;
		uint32_t mmw = w;
		uint32_t mmh = h;
		layer.mipmaps.resize(mipmaps);
		for (int j = 0; j < mipmaps; j++) {
			ReflectionData::Layer::Mipmap &mm = layer.mipmaps[j];
			mm.size.width = mmw;
			mm.size.height = mmh;
			mm.view = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), p_base_cube, p_base_layer, j);

			Vector<RID> fbtex;
			fbtex.push_back(mm.view);
			mm.framebuffer = RD::get_singleton()->framebuffer_create(fbtex);

			mmw = MAX(1u, mmw >> 1);
			mmh = MAX(1u, mmh >> 1);
		}

		layers.push_back(layer);
	}

	radiance_base_octmap = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), p_base_cube, p_base_layer, 0);
	RD::get_singleton()->set_resource_name(radiance_base_octmap, "Radiance Base Octmap");

	RD::TextureFormat tf;
	tf.format = p_texture_format;
	tf.width = p_low_quality ? 160 : p_size >> 1; // Always 160x160 when using REALTIME.
	tf.height = p_low_quality ? 160 : p_size >> 1;
	tf.mipmaps = p_low_quality ? 5 : mipmaps - 1;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
	if (!use_raster_effect) {
		tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
	}

	downsampled_radiance_octmap = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(downsampled_radiance_octmap, "Downsampled Radiance Octmap");
	{
		uint32_t mmw = tf.width;
		uint32_t mmh = tf.height;
		downsampled_layer.mipmaps.resize(tf.mipmaps);
		for (uint32_t j = 0; j < downsampled_layer.mipmaps.size(); j++) {
			ReflectionData::DownsampleLayer::Mipmap &mm = downsampled_layer.mipmaps[j];
			mm.size.width = mmw;
			mm.size.height = mmh;
			mm.view = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), downsampled_radiance_octmap, 0, j);
			RD::get_singleton()->set_resource_name(mm.view, "Downsampled Radiance Octmap Mip " + itos(j) + " ");
			if (use_raster_effect) {
				// We need a framebuffer for the octmap.
				Vector<RID> fbtex;
				fbtex.push_back(mm.view);
				mm.framebuffer = RD::get_singleton()->framebuffer_create(fbtex);
			}

			mmw = MAX(1u, mmw >> 1);
			mmh = MAX(1u, mmh >> 1);
		}
	}
}

void SkyRD::ReflectionData::create_reflection_fast_filter(bool p_use_arrays) {
	RendererRD::CopyEffects *copy_effects = RendererRD::CopyEffects::get_singleton();
	ERR_FAIL_NULL_MSG(copy_effects, "Effects haven't been initialized");
	bool use_raster_effect = copy_effects->get_raster_effects().has_flag(RendererRD::CopyEffects::RASTER_EFFECT_OCTMAP);

	if (use_raster_effect) {
		RD::get_singleton()->draw_command_begin_label("Downsample Radiance Map");
		copy_effects->octmap_downsample_raster(radiance_base_octmap, downsampled_layer.mipmaps[0].framebuffer, downsampled_layer.mipmaps[0].size, uv_border_size);

		for (uint32_t i = 1; i < downsampled_layer.mipmaps.size(); i++) {
			copy_effects->octmap_downsample_raster(downsampled_layer.mipmaps[i - 1].view, downsampled_layer.mipmaps[i].framebuffer, downsampled_layer.mipmaps[i].size, uv_border_size);
		}
		RD::get_singleton()->draw_command_end_label(); // Downsample Radiance

		if (p_use_arrays) {
			RD::get_singleton()->draw_command_begin_label("Filter Radiance Map into Array Heads");
			for (uint32_t i = 0; i < layers.size(); i++) {
				copy_effects->octmap_filter_raster(downsampled_radiance_octmap, layers[i].mipmaps[0].framebuffer, i, uv_border_size);
			}
		} else {
			RD::get_singleton()->draw_command_begin_label("Filter Radiance Map into Mipmaps Directly");
			for (uint32_t j = 0; j < layers[0].mipmaps.size(); j++) {
				copy_effects->octmap_filter_raster(downsampled_radiance_octmap, layers[0].mipmaps[j].framebuffer, j, uv_border_size);
			}
		}
		RD::get_singleton()->draw_command_end_label(); // Filter radiance
	} else {
		RD::get_singleton()->draw_command_begin_label("Downsample Radiance Map");
		copy_effects->octmap_downsample(radiance_base_octmap, downsampled_layer.mipmaps[0].view, downsampled_layer.mipmaps[0].size, uv_border_size);

		for (uint32_t i = 1; i < downsampled_layer.mipmaps.size(); i++) {
			copy_effects->octmap_downsample(downsampled_layer.mipmaps[i - 1].view, downsampled_layer.mipmaps[i].view, downsampled_layer.mipmaps[i].size, uv_border_size);
		}
		RD::get_singleton()->draw_command_end_label(); // Downsample Radiance
		Vector<RID> views;
		if (p_use_arrays) {
			for (uint32_t i = 1; i < layers.size(); i++) {
				views.push_back(layers[i].mipmaps[0].view);
			}
		} else {
			for (uint32_t i = 1; i < layers[0].mipmaps.size(); i++) {
				views.push_back(layers[0].mipmaps[i].view);
			}
		}
		RD::get_singleton()->draw_command_begin_label("Fast Filter Radiance");
		copy_effects->octmap_filter(downsampled_radiance_octmap, views, p_use_arrays, uv_border_size);
		RD::get_singleton()->draw_command_end_label(); // Filter radiance
	}
}

void SkyRD::ReflectionData::create_reflection_importance_sample(bool p_use_arrays, int p_base_layer, uint32_t p_sky_ggx_samples_quality) {
	RendererRD::CopyEffects *copy_effects = RendererRD::CopyEffects::get_singleton();
	ERR_FAIL_NULL_MSG(copy_effects, "Effects haven't been initialized");
	bool use_raster_effect = copy_effects->get_raster_effects().has_flag(RendererRD::CopyEffects::RASTER_EFFECT_OCTMAP);

	if (use_raster_effect) {
		if (p_base_layer == 1) {
			RD::get_singleton()->draw_command_begin_label("Downsample Radiance Map");
			copy_effects->octmap_downsample_raster(radiance_base_octmap, downsampled_layer.mipmaps[0].framebuffer, downsampled_layer.mipmaps[0].size, uv_border_size);

			for (uint32_t i = 1; i < downsampled_layer.mipmaps.size(); i++) {
				copy_effects->octmap_downsample_raster(downsampled_layer.mipmaps[i - 1].view, downsampled_layer.mipmaps[i].framebuffer, downsampled_layer.mipmaps[i].size, uv_border_size);
			}
			RD::get_singleton()->draw_command_end_label(); // Downsample Radiance
		}

		RD::get_singleton()->draw_command_begin_label("High Quality Filter Radiance");
		if (p_use_arrays) {
			copy_effects->octmap_roughness_raster(
					downsampled_radiance_octmap,
					layers[p_base_layer].mipmaps[0].framebuffer,
					p_sky_ggx_samples_quality,
					float(p_base_layer) / (layers.size() - 1.0),
					downsampled_layer.mipmaps[0].size.x,
					layers[p_base_layer].mipmaps[0].size.x,
					uv_border_size);
		} else {
			copy_effects->octmap_roughness_raster(
					downsampled_radiance_octmap,
					layers[0].mipmaps[p_base_layer].framebuffer,
					p_sky_ggx_samples_quality,
					float(p_base_layer) / (layers[0].mipmaps.size() - 1.0),
					downsampled_layer.mipmaps[0].size.x,
					layers[0].mipmaps[p_base_layer].size.x,
					uv_border_size);
		}
	} else {
		if (p_base_layer == 1) {
			RD::get_singleton()->draw_command_begin_label("Downsample Radiance Map");
			copy_effects->octmap_downsample(radiance_base_octmap, downsampled_layer.mipmaps[0].view, downsampled_layer.mipmaps[0].size, uv_border_size);

			for (uint32_t i = 1; i < downsampled_layer.mipmaps.size(); i++) {
				copy_effects->octmap_downsample(downsampled_layer.mipmaps[i - 1].view, downsampled_layer.mipmaps[i].view, downsampled_layer.mipmaps[i].size, uv_border_size);
			}
			RD::get_singleton()->draw_command_end_label(); // Downsample Radiance
		}

		RD::get_singleton()->draw_command_begin_label("High Quality Filter Radiance");
		if (p_use_arrays) {
			copy_effects->octmap_roughness(downsampled_radiance_octmap, layers[p_base_layer].mipmaps[0].view, p_sky_ggx_samples_quality, float(p_base_layer) / (layers.size() - 1.0), downsampled_layer.mipmaps[0].size.x, layers[p_base_layer].mipmaps[0].size.x, uv_border_size);
		} else {
			copy_effects->octmap_roughness(downsampled_radiance_octmap, layers[0].mipmaps[p_base_layer].view, p_sky_ggx_samples_quality, float(p_base_layer) / (layers[0].mipmaps.size() - 1.0), downsampled_layer.mipmaps[0].size.x, layers[0].mipmaps[p_base_layer].size.x, uv_border_size);
		}
	}
	RD::get_singleton()->draw_command_end_label(); // Filter radiance
}

void SkyRD::ReflectionData::update_reflection_mipmaps(int p_start, int p_end) {
	RendererRD::CopyEffects *copy_effects = RendererRD::CopyEffects::get_singleton();
	ERR_FAIL_NULL_MSG(copy_effects, "Effects haven't been initialized");
	bool use_raster_effect = copy_effects->get_raster_effects().has_flag(RendererRD::CopyEffects::RASTER_EFFECT_OCTMAP);

	RD::get_singleton()->draw_command_begin_label("Update Radiance Octmap Array Mipmaps");
	for (int i = p_start; i < p_end; i++) {
		for (uint32_t j = 0; j < layers[i].mipmaps.size() - 1; j++) {
			RID view = layers[i].mipmaps[j].view;
			Size2i size = layers[i].mipmaps[j + 1].size;
			if (use_raster_effect) {
				RID framebuffer = layers[i].mipmaps[j + 1].framebuffer;
				copy_effects->octmap_downsample_raster(view, framebuffer, size, uv_border_size);
			} else {
				RID texture = layers[i].mipmaps[j + 1].view;
				copy_effects->octmap_downsample(view, texture, size, uv_border_size);
			}
		}
	}
	RD::get_singleton()->draw_command_end_label();
}

////////////////////////////////////////////////////////////////////////////////
// SkyRD::Sky

void SkyRD::Sky::free_radiance() {
	if (capture_work) {
		capture_work->free();
		memdelete(capture_work);
		capture_work = nullptr;
	}
	for (RID rid : { capture_scene_buffer, capture_light_buffer }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	capture_scene_buffer = RID();
	capture_light_buffer = RID();
	capture_scene_set = RID();
	active_input = CaptureInput();
	pending_input = CaptureInput();
	published_complete = false;
	published_generation = 0;
	published_fog_orientation = Basis();
	published_has_height_fog = false;
	blend_started = -1.0;
	blend_weight = 0.0;
	if (radiance.is_valid()) {
		RD::get_singleton()->free_rid(radiance);
		radiance = RID();
	}
	if (radiance_first_layer_slice.is_valid()) {
		if (RD::get_singleton()->texture_is_valid(radiance_first_layer_slice)) {
			RD::get_singleton()->free_rid(radiance_first_layer_slice);
		}
		radiance_first_layer_slice = RID();
	}
}

void SkyRD::Sky::free() {
	free_radiance();
	reflection.clear_reflection_data();

	if (uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(uniform_buffer);
		uniform_buffer = RID();
	}

	if (material.is_valid()) {
		material = RID();
	}
}

RID SkyRD::SkyShader::get_default_shader_rd(bool p_is_multiview) {
	RID &shader_rd = p_is_multiview ? default_multiview_shader_rd : default_shader_rd;

	if (shader_rd.is_null()) {
		RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
		ERR_FAIL_NULL_V(material_storage, RID());
		ERR_FAIL_COND_V(!default_material.is_valid(), RID());

		SkyMaterialData *md = static_cast<SkyMaterialData *>(material_storage->material_get_data(default_material, RendererRD::MaterialStorage::SHADER_TYPE_SKY));
		shader_rd = shader.version_get_shader(md->shader_data->version, p_is_multiview ? SKY_VERSION_BACKGROUND_MULTIVIEW : SKY_VERSION_BACKGROUND);
	}

	return shader_rd;
}

RID SkyRD::Sky::get_textures(SkyTextureSetVersion p_version, RID p_default_shader_rd, bool p_is_multiview, Ref<RenderSceneBuffersRD> p_render_buffers) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 0;
		if (radiance.is_valid() && p_version <= SKY_TEXTURE_SET_QUARTER_RES) {
			u.append_id(radiance_first_layer_slice.is_valid() ? radiance_first_layer_slice : radiance);
		} else {
			u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 1; // half res
		if (p_version >= SKY_TEXTURE_SET_OCTMAP) {
			if (reflection.layers.size() && reflection.layers[0].mipmaps.size() >= 2 && reflection.layers[0].mipmaps[1].view.is_valid() && p_version != SKY_TEXTURE_SET_OCTMAP_HALF_RES) {
				u.append_id(reflection.layers[0].mipmaps[1].view);
			} else {
				u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
			}
		} else {
			RID half_texture = p_render_buffers->has_texture(RB_SCOPE_SKY, RB_HALF_TEXTURE) ? p_render_buffers->get_texture(RB_SCOPE_SKY, RB_HALF_TEXTURE) : RID();
			if (half_texture.is_valid() && p_version != SKY_TEXTURE_SET_HALF_RES) {
				u.append_id(half_texture);
			} else {
				u.append_id(texture_storage->texture_rd_get_default(p_is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE));
			}
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 2; // quarter res
		if (p_version >= SKY_TEXTURE_SET_OCTMAP) {
			if (reflection.layers.size() && reflection.layers[0].mipmaps.size() >= 3 && reflection.layers[0].mipmaps[2].view.is_valid() && p_version != SKY_TEXTURE_SET_OCTMAP_QUARTER_RES) {
				u.append_id(reflection.layers[0].mipmaps[2].view);
			} else {
				u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
			}
		} else {
			RID quarter_texture = p_render_buffers->has_texture(RB_SCOPE_SKY, RB_QUARTER_TEXTURE) ? p_render_buffers->get_texture(RB_SCOPE_SKY, RB_QUARTER_TEXTURE) : RID();
			if (quarter_texture.is_valid() && p_version != SKY_TEXTURE_SET_QUARTER_RES) {
				u.append_id(quarter_texture);
			} else {
				u.append_id(texture_storage->texture_rd_get_default(p_is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE));
			}
		}
		uniforms.push_back(u);
	}

	bool use_array = RendererSceneRenderRD::get_singleton()->get_sky()->sky_use_octmap_array;
	RID black = texture_storage->texture_rd_get_default(use_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	bool capture_pass = p_version >= SKY_TEXTURE_SET_OCTMAP;
	RID current = !capture_pass && managed_capture && published_complete ? radiance : black;
	RID next = !capture_pass && managed_capture && blend_started >= 0.0 && capture_work ? capture_work->radiance : current;
	for (int i = 0; i < 2; i++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 3 + i;
		u.append_id(i == 0 ? current : next);
		uniforms.push_back(u);
	}

	return UniformSetCacheRD::get_singleton()->get_cache_vec(p_default_shader_rd, SKY_SET_TEXTURES, uniforms);
}

bool SkyRD::Sky::set_radiance_size(int p_radiance_size) {
	ERR_FAIL_COND_V(p_radiance_size < 32 || p_radiance_size > 2048, false);
	if (radiance_size == p_radiance_size) {
		return false;
	}
	radiance_size = p_radiance_size;

	if (mode == RSE::SKY_MODE_REALTIME && radiance_size != REAL_TIME_SIZE) {
		WARN_PRINT(vformat("Realtime Skies can only use a radiance size of %d. Radiance size will be set to %d internally.", REAL_TIME_SIZE, REAL_TIME_SIZE));
		radiance_size = REAL_TIME_SIZE;
	}

	free_radiance();
	reflection.clear_reflection_data();

	return true;
}

int SkyRD::Sky::get_radiance_size() const {
	return radiance_size;
}

bool SkyRD::Sky::set_mode(RSE::SkyMode p_mode) {
	if (mode == p_mode) {
		return false;
	}

	mode = p_mode;

	if (mode == RSE::SKY_MODE_REALTIME && radiance_size != REAL_TIME_SIZE) {
		WARN_PRINT(vformat("Realtime Skies can only use a radiance size of %d. Radiance size will be set to %d internally.", REAL_TIME_SIZE, REAL_TIME_SIZE));
		set_radiance_size(REAL_TIME_SIZE);
	}

	free_radiance();
	reflection.clear_reflection_data();

	return true;
}

bool SkyRD::Sky::set_material(RID p_material) {
	if (material == p_material) {
		return false;
	}

	material = p_material;
	return true;
}

Ref<Image> SkyRD::Sky::bake_panorama(float p_energy, int p_roughness_layers, bool p_use_array, const Size2i &p_size) {
	if (radiance.is_valid()) {
		RendererRD::CopyEffects *copy_effects = RendererRD::CopyEffects::get_singleton();

		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT; // Could be RGBA16
		tf.width = p_size.width;
		tf.height = p_size.height;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

		RID rad_tex = RD::get_singleton()->texture_create(tf, RD::TextureView());
		copy_effects->copy_octmap_to_panorama(radiance, rad_tex, p_size, p_roughness_layers, p_use_array, Size2(uv_border_size, 1.0f - uv_border_size * 2.0), managed_capture ? 1.0f : -1.0f);
		Vector<uint8_t> data = RD::get_singleton()->texture_get_data(rad_tex, 0);
		RD::get_singleton()->free_rid(rad_tex);

		Ref<Image> img = Image::create_from_data(p_size.width, p_size.height, false, Image::FORMAT_RGBAF, data);
		for (int i = 0; i < p_size.width; i++) {
			for (int j = 0; j < p_size.height; j++) {
				Color c = img->get_pixel(i, j);
				c.r *= p_energy;
				c.g *= p_energy;
				c.b *= p_energy;
				img->set_pixel(i, j, c);
			}
		}
		return img;
	}

	return Ref<Image>();
}

////////////////////////////////////////////////////////////////////////////////
// SkyRD

RendererRD::MaterialStorage::ShaderData *SkyRD::_create_sky_shader_func() {
	SkyShaderData *shader_data = memnew(SkyShaderData);
	return shader_data;
}

RendererRD::MaterialStorage::ShaderData *SkyRD::_create_sky_shader_funcs() {
	// !BAS! Why isn't _create_sky_shader_func not just static too?
	return static_cast<RendererSceneRenderRD *>(RendererSceneRenderRD::singleton)->sky._create_sky_shader_func();
}

RendererRD::MaterialStorage::MaterialData *SkyRD::_create_sky_material_func(SkyShaderData *p_shader) {
	SkyMaterialData *material_data = memnew(SkyMaterialData);
	material_data->shader_data = p_shader;
	//update will happen later anyway so do nothing.
	return material_data;
}

RendererRD::MaterialStorage::MaterialData *SkyRD::_create_sky_material_funcs(RendererRD::MaterialStorage::ShaderData *p_shader) {
	// !BAS! same here, we could just make _create_sky_material_func static?
	return static_cast<RendererSceneRenderRD *>(RendererSceneRenderRD::singleton)->sky._create_sky_material_func(static_cast<SkyShaderData *>(p_shader));
}

SkyRD::SkyRD() {
	roughness_layers = GLOBAL_GET("rendering/reflections/sky_reflections/roughness_layers");
	sky_ggx_samples_quality = GLOBAL_GET("rendering/reflections/sky_reflections/ggx_samples");
	sky_use_octmap_array = GLOBAL_GET("rendering/reflections/sky_reflections/texture_array_reflections");
}

void SkyRD::init() {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	{
		// Start with the directional lights for the sky
		sky_scene_state.max_directional_lights = 4;
		uint32_t directional_light_buffer_size = sky_scene_state.max_directional_lights * sizeof(SkyDirectionalLightData);
		sky_scene_state.directional_lights = memnew_arr(SkyDirectionalLightData, sky_scene_state.max_directional_lights);
		sky_scene_state.last_frame_directional_lights = memnew_arr(SkyDirectionalLightData, sky_scene_state.max_directional_lights);
		sky_scene_state.last_frame_directional_light_count = sky_scene_state.max_directional_lights + 1;
		sky_scene_state.directional_light_buffer = RD::get_singleton()->uniform_buffer_create(directional_light_buffer_size);

		String defines = "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(sky_scene_state.max_directional_lights) + "\n";
		defines += "\n#define SAMPLERS_BINDING_FIRST_INDEX " + itos(SAMPLERS_BINDING_FIRST_INDEX) + "\n";

		if (sky_use_octmap_array) {
			defines += "\n#define HEIGHT_FOG_RADIANCE_ARRAY\n";
		}
		// Initialize sky
		Vector<String> sky_modes;
		sky_modes.push_back(""); // Full size
		sky_modes.push_back("\n#define USE_HALF_RES_PASS\n"); // Half Res
		sky_modes.push_back("\n#define USE_QUARTER_RES_PASS\n"); // Quarter res
		sky_modes.push_back("\n#define USE_CUBEMAP_PASS\n"); // Cubemap
		sky_modes.push_back("\n#define USE_CUBEMAP_PASS\n#define USE_HALF_RES_PASS\n"); // Half Res Cubemap
		sky_modes.push_back("\n#define USE_CUBEMAP_PASS\n#define USE_QUARTER_RES_PASS\n"); // Quarter res Cubemap

		sky_modes.push_back("\n#define USE_MULTIVIEW\n"); // Full size multiview
		sky_modes.push_back("\n#define USE_HALF_RES_PASS\n#define USE_MULTIVIEW\n"); // Half Res multiview
		sky_modes.push_back("\n#define USE_QUARTER_RES_PASS\n#define USE_MULTIVIEW\n"); // Quarter res multiview

		sky_shader.shader.initialize(sky_modes, defines);

		if (!RendererCompositorRD::get_singleton()->is_xr_enabled()) {
			sky_shader.shader.set_variant_enabled(SKY_VERSION_BACKGROUND_MULTIVIEW, false);
			sky_shader.shader.set_variant_enabled(SKY_VERSION_HALF_RES_MULTIVIEW, false);
			sky_shader.shader.set_variant_enabled(SKY_VERSION_QUARTER_RES_MULTIVIEW, false);
		}
	}

	// register our shader funds
	material_storage->shader_set_data_request_function(RendererRD::MaterialStorage::SHADER_TYPE_SKY, _create_sky_shader_funcs);
	material_storage->material_set_data_request_function(RendererRD::MaterialStorage::SHADER_TYPE_SKY, _create_sky_material_funcs);

	{
		ShaderCompiler::DefaultIdentifierActions actions;

		actions.renames["COLOR"] = "color";
		actions.renames["ALPHA"] = "alpha";
		actions.renames["EYEDIR"] = "cube_normal";
		actions.renames["POSITION"] = "params.position";
		actions.renames["SKY_COORDS"] = "panorama_coords";
		actions.renames["SCREEN_UV"] = "uv";
		actions.renames["FRAGCOORD"] = "gl_FragCoord";
		actions.renames["TIME"] = "params.time";
		actions.renames["PI"] = String::num(Math::PI);
		actions.renames["TAU"] = String::num(Math::TAU);
		actions.renames["E"] = String::num(Math::E);
		actions.renames["HALF_RES_COLOR"] = "half_res_color";
		actions.renames["QUARTER_RES_COLOR"] = "quarter_res_color";
		actions.renames["RADIANCE"] = "radiance";
		actions.renames["FOG"] = "custom_fog";
		actions.renames["LIGHT0_ENABLED"] = "directional_lights.data[0].enabled";
		actions.renames["LIGHT0_DIRECTION"] = "directional_lights.data[0].direction_energy.xyz";
		actions.renames["LIGHT0_ENERGY"] = "directional_lights.data[0].direction_energy.w";
		actions.renames["LIGHT0_COLOR"] = "directional_lights.data[0].color_size.xyz";
		actions.renames["LIGHT0_SIZE"] = "directional_lights.data[0].color_size.w";
		actions.renames["LIGHT1_ENABLED"] = "directional_lights.data[1].enabled";
		actions.renames["LIGHT1_DIRECTION"] = "directional_lights.data[1].direction_energy.xyz";
		actions.renames["LIGHT1_ENERGY"] = "directional_lights.data[1].direction_energy.w";
		actions.renames["LIGHT1_COLOR"] = "directional_lights.data[1].color_size.xyz";
		actions.renames["LIGHT1_SIZE"] = "directional_lights.data[1].color_size.w";
		actions.renames["LIGHT2_ENABLED"] = "directional_lights.data[2].enabled";
		actions.renames["LIGHT2_DIRECTION"] = "directional_lights.data[2].direction_energy.xyz";
		actions.renames["LIGHT2_ENERGY"] = "directional_lights.data[2].direction_energy.w";
		actions.renames["LIGHT2_COLOR"] = "directional_lights.data[2].color_size.xyz";
		actions.renames["LIGHT2_SIZE"] = "directional_lights.data[2].color_size.w";
		actions.renames["LIGHT3_ENABLED"] = "directional_lights.data[3].enabled";
		actions.renames["LIGHT3_DIRECTION"] = "directional_lights.data[3].direction_energy.xyz";
		actions.renames["LIGHT3_ENERGY"] = "directional_lights.data[3].direction_energy.w";
		actions.renames["LIGHT3_COLOR"] = "directional_lights.data[3].color_size.xyz";
		actions.renames["LIGHT3_SIZE"] = "directional_lights.data[3].color_size.w";
		actions.renames["AT_CUBEMAP_PASS"] = "AT_CUBEMAP_PASS";
		actions.renames["AT_HALF_RES_PASS"] = "AT_HALF_RES_PASS";
		actions.renames["AT_QUARTER_RES_PASS"] = "AT_QUARTER_RES_PASS";
		actions.custom_samplers["RADIANCE"] = "SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP";
		actions.usage_defines["HALF_RES_COLOR"] = "\n#define USES_HALF_RES_COLOR\n";
		actions.usage_defines["QUARTER_RES_COLOR"] = "\n#define USES_QUARTER_RES_COLOR\n";
		actions.render_mode_defines["disable_fog"] = "#define DISABLE_FOG\n";
		actions.render_mode_defines["use_debanding"] = "#define USE_DEBANDING\n";

		actions.base_texture_binding_index = 1;
		actions.texture_layout_set = 1;
		actions.base_uniform_string = "material.";
		actions.base_varying_index = 10;

		actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
		actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
		actions.global_buffer_array_variable = "global_shader_uniforms.data";

		sky_shader.compiler.initialize(actions);
	}

	{
		// default material and shader for sky shader
		sky_shader.default_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(sky_shader.default_shader);

		material_storage->shader_set_code(sky_shader.default_shader, R"(
// Default sky shader.

shader_type sky;

void sky() {
	COLOR = vec3(0.0);
}
)");

		sky_shader.default_material = material_storage->material_allocate();
		material_storage->material_initialize(sky_shader.default_material);

		material_storage->material_set_shader(sky_shader.default_material, sky_shader.default_shader);

		SkyMaterialData *md = static_cast<SkyMaterialData *>(material_storage->material_get_data(sky_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_SKY));
		sky_shader.default_shader_rd = sky_shader.shader.version_get_shader(md->shader_data->version, SKY_VERSION_BACKGROUND);

		sky_scene_state.uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(SkySceneState::UBO));

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 1;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 2;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.append_id(sky_scene_state.uniform_buffer);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 3;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.append_id(sky_scene_state.directional_light_buffer);
			uniforms.push_back(u);
		}

		material_storage->samplers_rd_get_default().append_uniforms(uniforms, SAMPLERS_BINDING_FIRST_INDEX);

		sky_scene_state.uniform_set = RD::get_singleton()->uniform_set_create(uniforms, sky_shader.default_shader_rd, SKY_SET_UNIFORMS);
	}

	{
		Vector<RD::Uniform> uniforms;
		{
			RD::Uniform u;
			u.binding = 0;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID vfog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
			u.append_id(vfog);
			uniforms.push_back(u);
		}

		sky_scene_state.default_fog_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, sky_shader.default_shader_rd, SKY_SET_FOG);
	}

	{
		// Need defaults for using fog with clear color
		sky_scene_state.fog_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(sky_scene_state.fog_shader);

		material_storage->shader_set_code(sky_scene_state.fog_shader, R"(
// Default clear color sky shader.

shader_type sky;

uniform vec4 clear_color;

void sky() {
	COLOR = clear_color.rgb;
}
)");
		sky_scene_state.fog_material = material_storage->material_allocate();
		material_storage->material_initialize(sky_scene_state.fog_material);

		material_storage->material_set_shader(sky_scene_state.fog_material, sky_scene_state.fog_shader);
	}
}

RID SkyRD::SkySceneState::get_fog_only_texture_uniform_set(RID p_default_shader_rd, bool p_is_multiview) {
	RID &uniform_set_rid = p_is_multiview ? fog_only_texture_multiview_uniform_set : fog_only_texture_uniform_set;

	if (uniform_set_rid.is_null()) {
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

		Vector<RD::Uniform> uniforms;
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.binding = 0;
			u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.binding = 1;
			u.append_id(texture_storage->texture_rd_get_default(p_is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE));
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.binding = 2;
			u.append_id(texture_storage->texture_rd_get_default(p_is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE));
			uniforms.push_back(u);
		}

		bool use_array = RendererSceneRenderRD::get_singleton()->get_sky()->sky_use_octmap_array;
		for (int i = 0; i < 2; i++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.binding = 3 + i;
			u.append_id(texture_storage->texture_rd_get_default(use_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
			uniforms.push_back(u);
		}
		uniform_set_rid = RD::get_singleton()->uniform_set_create(uniforms, p_default_shader_rd, SKY_SET_TEXTURES);
	}

	return uniform_set_rid;
}

void SkyRD::set_texture_format(RD::DataFormat p_texture_format) {
	texture_format = p_texture_format;
}

SkyRD::~SkyRD() {
	// cleanup anything created in init...
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	SkyMaterialData *md = static_cast<SkyMaterialData *>(material_storage->material_get_data(sky_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_SKY));
	sky_shader.shader.version_free(md->shader_data->version);
	RD::get_singleton()->free_rid(sky_scene_state.directional_light_buffer);
	RD::get_singleton()->free_rid(sky_scene_state.uniform_buffer);
	memdelete_arr(sky_scene_state.directional_lights);
	memdelete_arr(sky_scene_state.last_frame_directional_lights);
	material_storage->shader_free(sky_shader.default_shader);
	material_storage->material_free(sky_shader.default_material);
	material_storage->shader_free(sky_scene_state.fog_shader);
	material_storage->material_free(sky_scene_state.fog_material);

	if (RD::get_singleton()->uniform_set_is_valid(sky_scene_state.uniform_set)) {
		RD::get_singleton()->free_rid(sky_scene_state.uniform_set);
	}

	if (RD::get_singleton()->uniform_set_is_valid(sky_scene_state.default_fog_uniform_set)) {
		RD::get_singleton()->free_rid(sky_scene_state.default_fog_uniform_set);
	}

	if (sky_scene_state.fog_only_texture_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(sky_scene_state.fog_only_texture_uniform_set)) {
		RD::get_singleton()->free_rid(sky_scene_state.fog_only_texture_uniform_set);
	}

	if (sky_scene_state.fog_only_texture_multiview_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(sky_scene_state.fog_only_texture_multiview_uniform_set)) {
		RD::get_singleton()->free_rid(sky_scene_state.fog_only_texture_multiview_uniform_set);
	}
}

void SkyRD::setup_sky(const RenderDataRD *p_render_data, const Size2i p_screen_size) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	ERR_FAIL_COND(p_render_data->environment.is_null());

	ERR_FAIL_COND(p_render_data->render_buffers.is_null());

	// make sure we support our view count
	ERR_FAIL_COND(p_render_data->scene_data->view_count == 0);
	ERR_FAIL_COND(p_render_data->scene_data->view_count > RendererSceneRender::MAX_RENDER_VIEWS);

	SkyMaterialData *material_data = _get_sky_material_data(p_render_data->environment);
	ERR_FAIL_NULL(material_data);

	SkyShaderData *shader_data = material_data->shader_data;
	ERR_FAIL_NULL(shader_data);

	material_data->set_as_used();

	Sky *sky = get_sky(RendererSceneRenderRD::get_singleton()->environment_get_sky(p_render_data->environment));
	if (sky) {
		// Save our screen size; our buffers will already have been cleared.
		sky->screen_size.x = p_screen_size.x < 4 ? 4 : p_screen_size.x;
		sky->screen_size.y = p_screen_size.y < 4 ? 4 : p_screen_size.y;

		RSE::SkyMode sky_mode = sky->mode;

		if (sky_mode == RSE::SKY_MODE_AUTOMATIC) {
			bool sun_scatter_enabled = RendererSceneRenderRD::get_singleton()->environment_get_fog_enabled(p_render_data->environment) && RendererSceneRenderRD::get_singleton()->environment_get_fog_sun_scatter(p_render_data->environment) > 0.001;

			if ((shader_data->uses_time || shader_data->uses_position) && sky->radiance_size == Sky::REAL_TIME_SIZE) {
				sky_mode = RSE::SKY_MODE_REALTIME;
			} else if (shader_data->uses_light || sun_scatter_enabled || shader_data->ubo_size > 0) {
				sky_mode = RSE::SKY_MODE_INCREMENTAL;
			} else {
				sky_mode = RSE::SKY_MODE_QUALITY;
			}

			if (sky_mode != sky->internal_mode && !sky->managed_capture) {
				sky->internal_mode = sky_mode;

				sky->free_radiance();
				sky->reflection.clear_reflection_data();
			}
		} else {
			sky->internal_mode = sky_mode;
		}

		// Trigger updating radiance buffers.
		if (sky->radiance.is_null() && !sky->managed_capture) {
			invalidate_sky(sky);
			update_dirty_skys();
		}

		if (shader_data->uses_time && p_render_data->scene_data->time - sky->prev_time > 0.00001) {
			sky->prev_time = p_render_data->scene_data->time;
			sky->reflection.dirty = true;
			RenderingServerDefault::redraw_request();
		}

		// Native panorama/procedural skies have no application capture generation.
		// Their fogged radiance must still follow resolved fog and capture altitude.
		if (!sky->managed_capture) {
			RendererEnvironmentStorage::HeightFogData height_fog = RendererEnvironmentStorage::get_singleton()->environment_get_height_fog_data(p_render_data->environment);
			float origin_y = p_render_data->scene_data->cam_transform.origin.y;
			Basis fog_orientation = height_fog.options[2] > 0.0f ? RendererSceneRenderRD::get_singleton()->environment_get_sky_orientation(p_render_data->environment) : Basis();
			if (memcmp(&height_fog, &sky->prev_height_fog, sizeof(height_fog)) != 0 || (height_fog.control[3] > 0.0f && origin_y != sky->prev_height_fog_origin_y) || fog_orientation != sky->prev_height_fog_orientation) {
				sky->prev_height_fog = height_fog;
				sky->prev_height_fog_origin_y = origin_y;
				sky->prev_height_fog_orientation = fog_orientation;
				sky->reflection.dirty = true;
				RenderingServerDefault::redraw_request();
			}
		}

		if (RendererSceneRenderRD::get_singleton()->environment_get_fog_aerial_perspective(p_render_data->environment) != sky->prev_fog_aerial_perspective) {
			sky->prev_fog_aerial_perspective = RendererSceneRenderRD::get_singleton()->environment_get_fog_aerial_perspective(p_render_data->environment);
			sky->reflection.dirty = true;
			RenderingServerDefault::redraw_request();
		}

		if (RendererSceneRenderRD::get_singleton()->environment_get_fog_light_color(p_render_data->environment) != sky->prev_fog_light_color) {
			sky->prev_fog_light_color = RendererSceneRenderRD::get_singleton()->environment_get_fog_light_color(p_render_data->environment);
			sky->reflection.dirty = true;
			RenderingServerDefault::redraw_request();
		}

		if (RendererSceneRenderRD::get_singleton()->environment_get_fog_sun_scatter(p_render_data->environment) != sky->prev_fog_sun_scatter) {
			sky->prev_fog_sun_scatter = RendererSceneRenderRD::get_singleton()->environment_get_fog_sun_scatter(p_render_data->environment);
			sky->reflection.dirty = true;
			RenderingServerDefault::redraw_request();
		}

		if (RendererSceneRenderRD::get_singleton()->environment_get_fog_enabled(p_render_data->environment) != sky->prev_fog_enabled) {
			sky->prev_fog_enabled = RendererSceneRenderRD::get_singleton()->environment_get_fog_enabled(p_render_data->environment);
			sky->reflection.dirty = true;
			RenderingServerDefault::redraw_request();
		}

		if (RendererSceneRenderRD::get_singleton()->environment_get_fog_density(p_render_data->environment) != sky->prev_fog_density) {
			sky->prev_fog_density = RendererSceneRenderRD::get_singleton()->environment_get_fog_density(p_render_data->environment);
			sky->reflection.dirty = true;
			RenderingServerDefault::redraw_request();
		}

		if (RendererSceneRenderRD::get_singleton()->environment_get_fog_sky_affect(p_render_data->environment) != sky->prev_fog_sky_affect) {
			sky->prev_fog_sky_affect = RendererSceneRenderRD::get_singleton()->environment_get_fog_sky_affect(p_render_data->environment);
			sky->reflection.dirty = true;
			RenderingServerDefault::redraw_request();
		}

		if (RendererSceneRenderRD::get_singleton()->environment_get_fog_light_energy(p_render_data->environment) != sky->prev_fog_light_energy) {
			sky->prev_fog_light_energy = RendererSceneRenderRD::get_singleton()->environment_get_fog_light_energy(p_render_data->environment);
			sky->reflection.dirty = true;
			RenderingServerDefault::redraw_request();
		}

		if (material_data != sky->prev_material_data) {
			sky->prev_material_data = material_data;
			sky->reflection.dirty = true;
		}

		if (material_data->uniform_set_updated) {
			material_data->uniform_set_updated = false;
			sky->reflection.dirty = true;
		}

		if (!p_render_data->scene_data->cam_transform.origin.is_equal_approx(sky->prev_position) && shader_data->uses_position) {
			sky->prev_position = p_render_data->scene_data->cam_transform.origin;
			sky->reflection.dirty = true;
		}
	}

	bool sun_scatter_enabled = RendererSceneRenderRD::get_singleton()->environment_get_fog_enabled(p_render_data->environment) && RendererSceneRenderRD::get_singleton()->environment_get_fog_sun_scatter(p_render_data->environment) > 0.001;
	sky_scene_state.ubo.directional_light_count = 0;
	if (shader_data->uses_light || sun_scatter_enabled) {
		const PagedArray<RID> &lights = *p_render_data->lights;
		// Run through the list of lights in the scene and pick out the Directional Lights.
		// This can't be done in RenderSceneRenderRD::_setup lights because that needs to be called
		// after the depth prepass, but this runs before the depth prepass.
		for (int i = 0; i < (int)lights.size(); i++) {
			if (!light_storage->owns_light_instance(lights[i])) {
				continue;
			}
			RID base = light_storage->light_instance_get_base_light(lights[i]);

			ERR_CONTINUE(base.is_null());

			RSE::LightType type = light_storage->light_get_type(base);
			if (type == RSE::LIGHT_DIRECTIONAL && light_storage->light_directional_get_sky_mode(base) != RSE::LIGHT_DIRECTIONAL_SKY_MODE_LIGHT_ONLY) {
				SkyDirectionalLightData &sky_light_data = sky_scene_state.directional_lights[sky_scene_state.ubo.directional_light_count];
				Transform3D light_transform = light_storage->light_instance_get_base_transform(lights[i]);
				Vector3 world_direction = light_transform.basis.xform(Vector3(0, 0, 1)).normalized();

				sky_light_data.direction[0] = world_direction.x;
				sky_light_data.direction[1] = world_direction.y;
				sky_light_data.direction[2] = world_direction.z;

				float sign = light_storage->light_is_negative(base) ? -1 : 1;
				sky_light_data.energy = sign * light_storage->light_get_param(base, RSE::LIGHT_PARAM_ENERGY);

				if (RendererSceneRenderRD::get_singleton()->is_using_physical_light_units()) {
					sky_light_data.energy *= light_storage->light_get_param(base, RSE::LIGHT_PARAM_INTENSITY);
				}

				if (p_render_data->camera_attributes.is_valid()) {
					sky_light_data.energy *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
				}

				Color linear_col = light_storage->light_get_color(base).srgb_to_linear();
				sky_light_data.color[0] = linear_col.r;
				sky_light_data.color[1] = linear_col.g;
				sky_light_data.color[2] = linear_col.b;

				sky_light_data.enabled = true;

				float angular_diameter = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SIZE);
				sky_light_data.size = Math::deg_to_rad(angular_diameter);
				sky_scene_state.ubo.directional_light_count++;
				if (sky_scene_state.ubo.directional_light_count >= sky_scene_state.max_directional_lights) {
					break;
				}
			}
		}
		// Check whether the directional_light_buffer changes.
		bool light_data_dirty = false;

		// Light buffer is dirty if we have fewer or more lights.
		// If we have fewer lights, make sure that old lights are disabled.
		if (sky_scene_state.ubo.directional_light_count != sky_scene_state.last_frame_directional_light_count) {
			light_data_dirty = true;
			for (uint32_t i = sky_scene_state.ubo.directional_light_count; i < sky_scene_state.max_directional_lights; i++) {
				sky_scene_state.directional_lights[i] = {};
				sky_scene_state.directional_lights[i].enabled = false;
				sky_scene_state.last_frame_directional_lights[i] = {};
				sky_scene_state.last_frame_directional_lights[i].enabled = false;
			}
		}

		if (!light_data_dirty) {
			for (uint32_t i = 0; i < sky_scene_state.ubo.directional_light_count; i++) {
				if (sky_scene_state.directional_lights[i].direction[0] != sky_scene_state.last_frame_directional_lights[i].direction[0] ||
						sky_scene_state.directional_lights[i].direction[1] != sky_scene_state.last_frame_directional_lights[i].direction[1] ||
						sky_scene_state.directional_lights[i].direction[2] != sky_scene_state.last_frame_directional_lights[i].direction[2] ||
						sky_scene_state.directional_lights[i].energy != sky_scene_state.last_frame_directional_lights[i].energy ||
						sky_scene_state.directional_lights[i].color[0] != sky_scene_state.last_frame_directional_lights[i].color[0] ||
						sky_scene_state.directional_lights[i].color[1] != sky_scene_state.last_frame_directional_lights[i].color[1] ||
						sky_scene_state.directional_lights[i].color[2] != sky_scene_state.last_frame_directional_lights[i].color[2] ||
						sky_scene_state.directional_lights[i].enabled != sky_scene_state.last_frame_directional_lights[i].enabled ||
						sky_scene_state.directional_lights[i].size != sky_scene_state.last_frame_directional_lights[i].size) {
					light_data_dirty = true;
					break;
				}
			}
		}

		if (light_data_dirty) {
			RD::get_singleton()->buffer_update(sky_scene_state.directional_light_buffer, 0, sizeof(SkyDirectionalLightData) * sky_scene_state.max_directional_lights, sky_scene_state.directional_lights);

			SkyDirectionalLightData *temp = sky_scene_state.last_frame_directional_lights;
			sky_scene_state.last_frame_directional_lights = sky_scene_state.directional_lights;
			sky_scene_state.directional_lights = temp;
			sky_scene_state.last_frame_directional_light_count = sky_scene_state.ubo.directional_light_count;
			if (sky) {
				sky->reflection.dirty = true;
			}
		}
	}

	// Setup fog variables.
	sky_scene_state.ubo.volumetric_fog_enabled = false;
	if (p_render_data->render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = p_render_data->render_buffers->get_custom_data(RB_SCOPE_FOG);
		sky_scene_state.ubo.volumetric_fog_enabled = true;

		float fog_end = fog->length;
		if (fog_end > 0.0) {
			sky_scene_state.ubo.volumetric_fog_inv_length = 1.0 / fog_end;
		} else {
			sky_scene_state.ubo.volumetric_fog_inv_length = 1.0;
		}

		float fog_detail_spread = fog->spread; // Reverse lookup.
		if (fog_detail_spread > 0.0) {
			sky_scene_state.ubo.volumetric_fog_detail_spread = 1.0 / fog_detail_spread;
		} else {
			sky_scene_state.ubo.volumetric_fog_detail_spread = 1.0;
		}

		sky_scene_state.fog_uniform_set = fog->sky_uniform_set;
	}

	sky_scene_state.view_count = p_render_data->scene_data->view_count;
	sky_scene_state.cam_transform = p_render_data->scene_data->cam_transform;

	Projection correction;
	correction.set_depth_correction(p_render_data->scene_data->flip_y, true);
	correction.add_jitter_offset(p_render_data->scene_data->taa_jitter);

	Projection projection = p_render_data->scene_data->cam_projection;

	float custom_fov = RendererSceneRenderRD::get_singleton()->environment_get_sky_custom_fov(p_render_data->environment);

	if (custom_fov && sky_scene_state.view_count == 1) {
		// With custom fov we don't support stereo...
		float near_plane = projection.get_z_near();
		float far_plane = projection.get_z_far();
		float aspect = projection.get_aspect();

		projection.set_perspective(custom_fov, aspect, near_plane, far_plane);
	}

	sky_scene_state.cam_projection = correction * projection;

	// Our info in our UBO is only used if we're rendering stereo.
	for (uint32_t i = 0; i < p_render_data->scene_data->view_count; i++) {
		Projection view_inv_projection = (correction * p_render_data->scene_data->view_projection[i]).inverse();
		if (p_render_data->scene_data->view_count > 1) {
			// Reprojection is used when we need to have things in combined space.
			RendererRD::MaterialStorage::store_camera(sky_scene_state.cam_projection * view_inv_projection, sky_scene_state.ubo.combined_reprojection[i]);
		} else {
			// This is unused so just reset to identity.
			Projection ident;
			RendererRD::MaterialStorage::store_camera(ident, sky_scene_state.ubo.combined_reprojection[i]);
		}

		RendererRD::MaterialStorage::store_camera(view_inv_projection, sky_scene_state.ubo.view_inv_projections[i]);
		sky_scene_state.ubo.view_eye_offsets[i][0] = p_render_data->scene_data->view_eye_offset[i].x;
		sky_scene_state.ubo.view_eye_offsets[i][1] = p_render_data->scene_data->view_eye_offset[i].y;
		sky_scene_state.ubo.view_eye_offsets[i][2] = p_render_data->scene_data->view_eye_offset[i].z;
		sky_scene_state.ubo.view_eye_offsets[i][3] = 0.0;
	}

	sky_scene_state.ubo.z_far = p_render_data->scene_data->view_projection[0].get_z_far(); // Should be the same for all projection.
	sky_scene_state.ubo.height_fog = RendererEnvironmentStorage::get_singleton()->environment_get_height_fog_data(p_render_data->environment);
	RendererRD::MaterialStorage::store_transform(p_render_data->scene_data->cam_transform, sky_scene_state.ubo.height_fog_world_from_view);
	RendererRD::MaterialStorage::store_transform(Transform3D(RendererSceneRenderRD::get_singleton()->environment_get_sky_orientation(p_render_data->environment)), sky_scene_state.ubo.height_fog_sky_orientation);
	RendererRD::MaterialStorage::store_camera((correction * p_render_data->scene_data->cam_projection).inverse(), sky_scene_state.ubo.height_fog_inv_projection);
	sky_scene_state.ubo.height_fog_view[0] = p_render_data->scene_data->cam_orthogonal ? 1.0f : 0.0f;
	sky_scene_state.ubo.height_fog_view[1] = p_render_data->reflection_probe.is_valid() ? 1.0f : 0.0f;
	sky_scene_state.ubo.height_fog_view[2] = sky && sky->managed_capture && sky->published_complete ? 1.0f : 0.0f;
	sky_scene_state.ubo.height_fog_view[3] = p_render_data->camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes) : 1.0f;
	for (int i = 0; i < 4; i++) {
		sky_scene_state.ubo.height_fog_capture[i] = 0;
		sky_scene_state.ubo.height_fog_fallback[i] = 0;
	}
	sky_get_capture_sampling(RendererSceneRenderRD::get_singleton()->environment_get_sky(p_render_data->environment), p_render_data->reflection_probe.is_valid(), sky_scene_state.ubo.height_fog_capture, sky_scene_state.ubo.height_fog_fallback);
	Basis old_capture_correction, next_capture_correction;
	sky_get_capture_orientation_corrections(RendererSceneRenderRD::get_singleton()->environment_get_sky(p_render_data->environment), RendererSceneRenderRD::get_singleton()->environment_get_sky_orientation(p_render_data->environment), old_capture_correction, next_capture_correction);
	RendererRD::MaterialStorage::store_transform_3x3(old_capture_correction, sky_scene_state.ubo.height_fog_capture_old_xform);
	RendererRD::MaterialStorage::store_transform_3x3(next_capture_correction, sky_scene_state.ubo.height_fog_capture_next_xform);
	sky_scene_state.ubo.height_fog_capture[3] = roughness_layers - 1;
	sky_scene_state.ubo.height_fog_radiance[0] = sky ? sky->uv_border_size : 0.0f;
	sky_scene_state.ubo.fog_enabled = RendererSceneRenderRD::get_singleton()->environment_get_fog_enabled(p_render_data->environment);
	sky_scene_state.ubo.fog_density = RendererSceneRenderRD::get_singleton()->environment_get_fog_density(p_render_data->environment);
	sky_scene_state.ubo.fog_aerial_perspective = RendererSceneRenderRD::get_singleton()->environment_get_fog_aerial_perspective(p_render_data->environment);
	Color fog_color = RendererSceneRenderRD::get_singleton()->environment_get_fog_light_color(p_render_data->environment).srgb_to_linear();
	float fog_energy = RendererSceneRenderRD::get_singleton()->environment_get_fog_light_energy(p_render_data->environment);
	sky_scene_state.ubo.fog_light_color[0] = fog_color.r * fog_energy;
	sky_scene_state.ubo.fog_light_color[1] = fog_color.g * fog_energy;
	sky_scene_state.ubo.fog_light_color[2] = fog_color.b * fog_energy;
	sky_scene_state.ubo.fog_sun_scatter = RendererSceneRenderRD::get_singleton()->environment_get_fog_sun_scatter(p_render_data->environment);

	sky_scene_state.ubo.fog_sky_affect = RendererSceneRenderRD::get_singleton()->environment_get_fog_sky_affect(p_render_data->environment);
	sky_scene_state.ubo.volumetric_fog_sky_affect = RendererSceneRenderRD::get_singleton()->environment_get_volumetric_fog_sky_affect(p_render_data->environment);
	sky_scene_state.ubo.fog_use_legacy_blending = RendererSceneRenderRD::get_singleton()->fog_use_legacy_blending_get();

	RD::get_singleton()->buffer_update(sky_scene_state.uniform_buffer, 0, sizeof(SkySceneState::UBO), &sky_scene_state.ubo);
}

void SkyRD::update_radiance_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_env, const Vector3 &p_global_pos, double p_time, float p_luminance_multiplier, float p_brightness_multiplier) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_env.is_null());

	SkyMaterialData *material_data = _get_sky_material_data(p_env);
	ERR_FAIL_NULL(material_data);

	SkyShaderData *shader_data = material_data->shader_data;
	ERR_FAIL_NULL(shader_data);

	Sky *sky = get_sky(RendererSceneRenderRD::get_singleton()->environment_get_sky(p_env));
	if (sky->managed_capture) {
		return;
	}
	RSE::SkyMode sky_mode = sky->internal_mode;
	bool update_single_frame = sky_mode == RSE::SKY_MODE_REALTIME || sky_mode == RSE::SKY_MODE_QUALITY;

	if (sky->processing_layer == 0 && sky_mode == RSE::SKY_MODE_INCREMENTAL) {
		// On the first frame after creating sky, rebuild in single frame
		update_single_frame = true;
		sky_mode = RSE::SKY_MODE_QUALITY;
	}

	int max_processing_layer = sky_use_octmap_array ? sky->reflection.layers.size() : sky->reflection.layers[0].mipmaps.size();

	// Update radiance octmap
	if (sky->reflection.dirty && (sky->processing_layer >= max_processing_layer || update_single_frame)) {
		Projection cm;
		cm.set_perspective(90, 1, 0.01, 10.0);
		Projection correction;
		correction.set_depth_correction(false);
		cm = correction * cm;

		// Note, we ignore environment_get_sky_orientation here as this is applied when we do our lookup in our scene shader.

		if (shader_data->uses_quarter_res && roughness_layers >= 3) {
			RD::get_singleton()->draw_command_begin_label("Render Sky to Quarter-Resolution Cubemap");
			PipelineCacheRD *pipeline = &shader_data->pipelines[SKY_VERSION_OCTMAP_QUARTER_RES];

			Vector<Color> clear_colors;
			clear_colors.push_back(Color(0.0, 0.0, 0.0));
			RD::DrawListID octmap_draw_list;

			RID texture_uniform_set = sky->get_textures(SKY_TEXTURE_SET_OCTMAP_QUARTER_RES, sky_shader.default_shader_rd, false, p_render_buffers);

			octmap_draw_list = RD::get_singleton()->draw_list_begin(sky->reflection.layers[0].mipmaps[2].framebuffer, RD::DRAW_IGNORE_COLOR_ALL);
			_render_sky(octmap_draw_list, p_time, sky->reflection.layers[0].mipmaps[2].framebuffer, pipeline, material_data->uniform_set, texture_uniform_set, cm, Basis(), p_global_pos, p_luminance_multiplier, p_brightness_multiplier, sky->uv_border_size);
			RD::get_singleton()->draw_list_end();

			RD::get_singleton()->draw_command_end_label();
		} else if (shader_data->uses_quarter_res && roughness_layers < 3) {
			ERR_PRINT_ED("Cannot use quarter res buffer in sky shader when roughness layers is less than 3. Please increase rendering/reflections/sky_reflections/roughness_layers.");
		}

		if (shader_data->uses_half_res && roughness_layers >= 2) {
			RD::get_singleton()->draw_command_begin_label("Render Sky to Half-Resolution Cubemap");
			PipelineCacheRD *pipeline = &shader_data->pipelines[SKY_VERSION_OCTMAP_HALF_RES];

			Vector<Color> clear_colors;
			clear_colors.push_back(Color(0.0, 0.0, 0.0));

			RD::DrawListID octmap_draw_list;
			RID texture_uniform_set = sky->get_textures(SKY_TEXTURE_SET_OCTMAP_HALF_RES, sky_shader.default_shader_rd, false, p_render_buffers);

			octmap_draw_list = RD::get_singleton()->draw_list_begin(sky->reflection.layers[0].mipmaps[1].framebuffer, RD::DRAW_IGNORE_COLOR_ALL);
			_render_sky(octmap_draw_list, p_time, sky->reflection.layers[0].mipmaps[1].framebuffer, pipeline, material_data->uniform_set, texture_uniform_set, cm, Basis(), p_global_pos, p_luminance_multiplier, p_brightness_multiplier, sky->uv_border_size);
			RD::get_singleton()->draw_list_end();

			RD::get_singleton()->draw_command_end_label();
		} else if (shader_data->uses_half_res && roughness_layers < 2) {
			ERR_PRINT_ED("Cannot use half res buffer in sky shader when roughness layers is less than 2. Please increase rendering/reflections/sky_reflections/roughness_layers.");
		}

		RD::DrawListID octmap_draw_list;
		PipelineCacheRD *pipeline = &shader_data->pipelines[SKY_VERSION_OCTMAP];

		RD::get_singleton()->draw_command_begin_label("Render Sky Octmap");

		RID texture_uniform_set = sky->get_textures(SKY_TEXTURE_SET_OCTMAP, sky_shader.default_shader_rd, false, p_render_buffers);

		octmap_draw_list = RD::get_singleton()->draw_list_begin(sky->reflection.layers[0].mipmaps[0].framebuffer, RD::DRAW_IGNORE_COLOR_ALL, Vector<Color>(), 1.0f, 0, Rect2(), RDD::BreadcrumbMarker::SKY_PASS);
		_render_sky(octmap_draw_list, p_time, sky->reflection.layers[0].mipmaps[0].framebuffer, pipeline, material_data->uniform_set, texture_uniform_set, cm, Basis(), p_global_pos, p_luminance_multiplier, p_brightness_multiplier, sky->uv_border_size);
		RD::get_singleton()->draw_list_end();

		RD::get_singleton()->draw_command_end_label();

		if (sky_mode == RSE::SKY_MODE_REALTIME) {
			sky->reflection.create_reflection_fast_filter(sky_use_octmap_array);
			if (sky_use_octmap_array) {
				sky->reflection.update_reflection_mipmaps(0, sky->reflection.layers.size());
			}
		} else {
			if (update_single_frame) {
				for (int i = 1; i < max_processing_layer; i++) {
					sky->reflection.create_reflection_importance_sample(sky_use_octmap_array, i, sky_ggx_samples_quality);
				}
				if (sky_use_octmap_array) {
					sky->reflection.update_reflection_mipmaps(0, sky->reflection.layers.size());
				}
			} else {
				if (sky_use_octmap_array) {
					// Multi-Frame so just update the first array level
					sky->reflection.update_reflection_mipmaps(0, 1);
				}
			}
			sky->processing_layer = 1;
		}
		sky->baked_exposure = p_luminance_multiplier;
		sky->reflection.dirty = false;

	} else {
		if (sky_mode == RSE::SKY_MODE_INCREMENTAL && sky->processing_layer < max_processing_layer) {
			sky->reflection.create_reflection_importance_sample(sky_use_octmap_array, sky->processing_layer, sky_ggx_samples_quality);

			if (sky_use_octmap_array) {
				sky->reflection.update_reflection_mipmaps(sky->processing_layer, sky->processing_layer + 1);
			}

			sky->processing_layer++;
		}
	}
}

void SkyRD::update_res_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_env, double p_time, float p_luminance_multiplier, float p_brightness_multiplier) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_env.is_null());

	RSE::EnvironmentBG background = RendererSceneRenderRD::get_singleton()->environment_get_background(p_env);
	SkyMaterialData *material_data = (background == RSE::ENV_BG_CLEAR_COLOR || background == RSE::ENV_BG_COLOR) ? _get_flat_color_sky_material_data(p_env) : _get_sky_material_data(p_env);
	ERR_FAIL_NULL(material_data);

	SkyShaderData *shader_data = material_data->shader_data;
	ERR_FAIL_NULL(shader_data);

	if (!shader_data->uses_quarter_res && !shader_data->uses_half_res) {
		return;
	}

	material_data->set_as_used();

	RENDER_TIMESTAMP("Setup Sky Resolution Buffers");
	RD::get_singleton()->draw_command_begin_label("Setup Sky Resolution Buffers");

	Basis sky_transform = RendererSceneRenderRD::get_singleton()->environment_get_sky_orientation(p_env);
	sky_transform.invert();

	// Camera
	Projection projection = sky_scene_state.cam_projection;
	sky_transform = sky_transform * sky_scene_state.cam_transform.basis;
	bool is_multiview = sky_scene_state.view_count > 1;

	Sky *sky = get_sky(RendererSceneRenderRD::get_singleton()->environment_get_sky(p_env));

	if (shader_data->uses_quarter_res) {
		PipelineCacheRD *pipeline = &shader_data->pipelines[is_multiview ? SKY_VERSION_QUARTER_RES_MULTIVIEW : SKY_VERSION_QUARTER_RES];

		// Grab texture and framebuffer from cache, create if needed...
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
		Size2i quarter_size = sky->screen_size / 4;
		RID texture = p_render_buffers->create_texture(RB_SCOPE_SKY, RB_QUARTER_TEXTURE, texture_format, usage_bits, RD::TEXTURE_SAMPLES_1, quarter_size);
		RID framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multiview(sky_scene_state.view_count, texture);

		RID texture_uniform_set = sky->get_textures(SKY_TEXTURE_SET_QUARTER_RES, sky_shader.get_default_shader_rd(is_multiview), is_multiview, p_render_buffers);

		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(framebuffer, RD::DRAW_IGNORE_COLOR_ALL);
		_render_sky(draw_list, p_time, framebuffer, pipeline, material_data->uniform_set, texture_uniform_set, projection, sky_transform, sky_scene_state.cam_transform.origin, p_luminance_multiplier, p_brightness_multiplier, sky->uv_border_size);
		RD::get_singleton()->draw_list_end();
	}

	if (shader_data->uses_half_res) {
		PipelineCacheRD *pipeline = &shader_data->pipelines[is_multiview ? SKY_VERSION_HALF_RES_MULTIVIEW : SKY_VERSION_HALF_RES];

		// Grab texture and framebuffer from cache, create if needed...
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
		Size2i half_size = sky->screen_size / 2;
		RID texture = p_render_buffers->create_texture(RB_SCOPE_SKY, RB_HALF_TEXTURE, texture_format, usage_bits, RD::TEXTURE_SAMPLES_1, half_size);
		RID framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multiview(sky_scene_state.view_count, texture);

		RID texture_uniform_set = sky->get_textures(SKY_TEXTURE_SET_HALF_RES, sky_shader.get_default_shader_rd(is_multiview), is_multiview, p_render_buffers);

		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(framebuffer, RD::DRAW_IGNORE_COLOR_ALL);
		_render_sky(draw_list, p_time, framebuffer, pipeline, material_data->uniform_set, texture_uniform_set, projection, sky_transform, sky_scene_state.cam_transform.origin, p_luminance_multiplier, p_brightness_multiplier, sky->uv_border_size);
		RD::get_singleton()->draw_list_end();
	}

	RD::get_singleton()->draw_command_end_label(); // Setup Sky resolution buffers
}

void SkyRD::draw_sky(RD::DrawListID p_draw_list, Ref<RenderSceneBuffersRD> p_render_buffers, RID p_env, RID p_fb, double p_time, float p_luminance_multiplier, float p_brightness_multiplier) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_env.is_null());

	Sky *sky = get_sky(RendererSceneRenderRD::get_singleton()->environment_get_sky(p_env));

	RSE::EnvironmentBG background = RendererSceneRenderRD::get_singleton()->environment_get_background(p_env);
	SkyMaterialData *material_data = (background == RSE::ENV_BG_CLEAR_COLOR || background == RSE::ENV_BG_COLOR) ? _get_flat_color_sky_material_data(p_env) : _get_sky_material_data(p_env);
	ERR_FAIL_NULL(material_data);

	SkyShaderData *shader_data = material_data->shader_data;
	ERR_FAIL_NULL(shader_data);

	material_data->set_as_used();

	Basis sky_transform = RendererSceneRenderRD::get_singleton()->environment_get_sky_orientation(p_env);
	sky_transform.invert();

	// Camera
	Projection projection = sky_scene_state.cam_projection;

	sky_transform = sky_transform * sky_scene_state.cam_transform.basis;

	bool is_multiview = sky_scene_state.view_count > 1;

	PipelineCacheRD *pipeline = &shader_data->pipelines[is_multiview ? SKY_VERSION_BACKGROUND_MULTIVIEW : SKY_VERSION_BACKGROUND];

	RID default_shader_rd = sky_shader.get_default_shader_rd(is_multiview);

	RID texture_uniform_set;
	float border_size = 0.0;
	if (sky) {
		texture_uniform_set = sky->get_textures(SKY_TEXTURE_SET_BACKGROUND, default_shader_rd, is_multiview, p_render_buffers);
		border_size = sky->uv_border_size;
	} else {
		texture_uniform_set = sky_scene_state.get_fog_only_texture_uniform_set(default_shader_rd, is_multiview);
	}

	_render_sky(p_draw_list, p_time, p_fb, pipeline, material_data->uniform_set, texture_uniform_set, projection, sky_transform, sky_scene_state.cam_transform.origin, p_luminance_multiplier, p_brightness_multiplier, border_size);
}

void SkyRD::invalidate_sky(Sky *p_sky) {
	if (!p_sky->dirty) {
		p_sky->dirty = true;
		p_sky->dirty_list = dirty_sky_list;
		dirty_sky_list = p_sky;
	}
}

void SkyRD::update_dirty_skys() {
	bool use_raster_effect = RendererRD::CopyEffects::get_singleton()->get_raster_effects().has_flag(RendererRD::CopyEffects::RASTER_EFFECT_OCTMAP);
	Sky *sky = dirty_sky_list;

	while (sky) {
		//update sky configuration if texture is missing

		// TODO See if we can move this into `update_radiance_buffers` and remove our dirty_sky logic.
		// As this is basically a duplicate of the logic in reflection probes we could move this logic
		// into RenderSceneBuffersRD and use that from both places.
		if (sky->radiance.is_null() && !sky->managed_capture) {
			int mipmaps = Image::get_image_required_mipmaps(sky->radiance_size, sky->radiance_size, Image::FORMAT_RGBAH) + 1;

			int layers = roughness_layers;
			bool use_realtime = sky->mode == RSE::SKY_MODE_REALTIME || sky->internal_mode == RSE::SKY_MODE_REALTIME;
			if (use_realtime) {
				layers = Sky::REAL_TIME_ROUGHNESS_LAYERS;
			}

			if (sky_use_octmap_array) {
				mipmaps -= 2; //  reduce the number of mipmaps to keep the border size reasonable.
				// Double size to approximate texel density of cubemaps + add border for proper filtering/mipmapping.
				uint32_t padding_pixels = (1 << (mipmaps - 1));
				uint32_t w = sky->radiance_size * 2 + padding_pixels * 2;
				uint32_t h = w;
				sky->uv_border_size = float(padding_pixels) / float(w);

				// Array (higher quality, more memory).
				RD::TextureFormat tf;
				tf.array_layers = layers;
				tf.format = texture_format;
				tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
				tf.mipmaps = mipmaps;
				tf.width = w;
				tf.height = h;
				tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
				if (!use_raster_effect) {
					tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
				}

				sky->radiance = RD::get_singleton()->texture_create(tf, RD::TextureView());

				// Create view into the first layer slice for user shaders.
				sky->radiance_first_layer_slice = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), sky->radiance, 0, 0, mipmaps, RD::TEXTURE_SLICE_2D, 1);

				sky->reflection.update_reflection_data(w, mipmaps, true, sky->radiance, 0, use_realtime, roughness_layers, texture_format, sky->uv_border_size);
			} else {
				// Double size to approximate texel density of cubemaps + add border for proper filtering/mipmapping.
				uint32_t padding_pixels = (1 << (MIN(mipmaps, layers) - 1));
				uint32_t w = sky->radiance_size * 2 + padding_pixels * 2;
				uint32_t h = w;
				sky->uv_border_size = float(padding_pixels) / float(w);

				// Single texture (lower quality, less memory).
				RD::TextureFormat tf;
				tf.format = texture_format;
				tf.mipmaps = MIN(mipmaps, layers);
				tf.width = w;
				tf.height = h;
				tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
				if (!use_raster_effect) {
					tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
				}

				sky->radiance = RD::get_singleton()->texture_create(tf, RD::TextureView());

				DEV_ASSERT(sky->radiance_first_layer_slice.is_null());

				sky->reflection.update_reflection_data(w, MIN(mipmaps, layers), false, sky->radiance, 0, use_realtime, roughness_layers, texture_format, sky->uv_border_size);
			}
		}

		sky->reflection.dirty = true;
		sky->processing_layer = 0;

		Sky *next = sky->dirty_list;
		sky->dirty_list = nullptr;
		sky->dirty = false;
		sky = next;
	}

	dirty_sky_list = nullptr;
}

SkyRD::SkyMaterialData *SkyRD::_get_flat_color_sky_material_data(RID p_env) {
	ERR_FAIL_COND_V(p_env.is_null(), nullptr);

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	SkyMaterialData *material_data = nullptr;
	RID sky_material = sky_scene_state.fog_material;

	material_data = static_cast<SkyMaterialData *>(material_storage->material_get_data(sky_material, RendererRD::MaterialStorage::SHADER_TYPE_SKY));

	if (!material_data) {
		sky_material = sky_shader.default_material;
		material_data = static_cast<SkyMaterialData *>(material_storage->material_get_data(sky_material, RendererRD::MaterialStorage::SHADER_TYPE_SKY));
	}

	return material_data;
}

SkyRD::SkyMaterialData *SkyRD::_get_sky_material_data(RID p_env) {
	ERR_FAIL_COND_V(p_env.is_null(), nullptr);

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	Sky *sky = get_sky(RendererSceneRenderRD::get_singleton()->environment_get_sky(p_env));

	SkyMaterialData *material_data = nullptr;
	RID sky_material;

	if (sky) {
		sky_material = sky_get_material(RendererSceneRenderRD::get_singleton()->environment_get_sky(p_env));

		if (sky_material.is_valid()) {
			material_data = static_cast<SkyMaterialData *>(material_storage->material_get_data(sky_material, RendererRD::MaterialStorage::SHADER_TYPE_SKY));
			if (!material_data || !material_data->shader_data->valid) {
				material_data = nullptr;
			}
		}
	}

	if (!material_data) {
		sky_material = sky_shader.default_material;
		material_data = static_cast<SkyMaterialData *>(material_storage->material_get_data(sky_material, RendererRD::MaterialStorage::SHADER_TYPE_SKY));
	}

	return material_data;
}

RID SkyRD::sky_get_material(RID p_sky) const {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL_V(sky, RID());

	return sky->material;
}

float SkyRD::sky_get_baked_exposure(RID p_sky) const {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL_V(sky, 1.0);

	return sky->baked_exposure;
}

RID SkyRD::allocate_sky_rid() {
	return sky_owner.allocate_rid();
}

void SkyRD::initialize_sky_rid(RID p_rid) {
	sky_owner.initialize_rid(p_rid, Sky());
}

SkyRD::Sky *SkyRD::get_sky(RID p_sky) const {
	return sky_owner.get_or_null(p_sky);
}

void SkyRD::free_sky(RID p_sky) {
	capture_queue.erase(p_sky);
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL(sky);

	sky->free();
	sky_owner.free(p_sky);
}

void SkyRD::sky_set_radiance_size(RID p_sky, int p_radiance_size) {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL(sky);

	if (sky->set_radiance_size(p_radiance_size)) {
		invalidate_sky(sky);
	}
}

int SkyRD::sky_get_radiance_size(RID p_sky) const {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL_V(sky, 0);

	return sky->get_radiance_size();
}

void SkyRD::sky_set_mode(RID p_sky, RSE::SkyMode p_mode) {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL(sky);

	if (sky->set_mode(p_mode)) {
		invalidate_sky(sky);
	}
}

void SkyRD::sky_set_material(RID p_sky, RID p_material) {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL(sky);

	if (sky->set_material(p_material)) {
		invalidate_sky(sky);
	}
}

Ref<Image> SkyRD::sky_bake_panorama(RID p_sky, float p_energy, bool p_bake_irradiance, const Size2i &p_size) {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL_V(sky, Ref<Image>());

	update_dirty_skys();

	return sky->bake_panorama(p_energy, p_bake_irradiance ? roughness_layers : 0, sky_use_octmap_array, p_size);
}

RID SkyRD::sky_get_radiance_texture_rd(RID p_sky) const {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL_V(sky, RID());

	return sky->managed_capture && !sky->published_complete ? RID() : sky->radiance;
}

float SkyRD::sky_get_uv_border_size(RID p_sky) {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL_V(sky, 1.0);

	return sky->uv_border_size;
}

// Managed capture inputs are dedicated immutable application resources. Only
// these explicit requests drive IBL; live view dirty flags/TIME/EV do not.
void SkyRD::sky_request_capture(RID p_sky, RID p_material, int64_t p_generation, const Vector3 &p_origin, double p_capture_time, const Color &p_fallback, RID p_environment) {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL(sky);
	if (!sky->managed_capture) {
		sky->free_radiance();
		sky->reflection.clear_reflection_data();
		sky->managed_capture = true;
		capture_queue.push_back(p_sky);
	}
	if (p_generation <= 0 || !p_origin.is_finite() || !Math::is_finite(p_capture_time) || (!Math::is_finite(p_fallback.r) || !Math::is_finite(p_fallback.g) || !Math::is_finite(p_fallback.b))) {
		sky->capture_error = "invalid_capture_input";
		return;
	}
	if (p_generation < sky->published_generation || p_generation < sky->active_input.generation || p_generation < sky->pending_input.generation) {
		sky->capture_error = "stale_generation";
		return;
	}
	auto *renderer = RendererSceneRenderRD::get_singleton();
	if (!renderer->is_environment(p_environment)) {
		sky->capture_error = "invalid_snapshot_environment";
		return;
	}
	CaptureInput input;
	input.material = p_material;
	input.generation = p_generation;
	input.origin = p_origin;
	input.time = p_capture_time;
	input.fallback = p_fallback;
	input.requested_at = double(OS::get_singleton()->get_ticks_usec()) * 1e-6;
	input.brightness = renderer->environment_get_bg_energy_multiplier(p_environment) * renderer->environment_get_bg_intensity(p_environment);
	input.scene_ubo.height_fog = RendererEnvironmentStorage::get_singleton()->environment_get_height_fog_data(p_environment);
	// Radiance remains in artwork space, matching the existing inverse sky
	// orientation lookup. Only captured physical air follows the world ray O*q.
	// Without active height fog rotation stays a lookup-only, zero-work change.
	input.fog_orientation = input.scene_ubo.height_fog.options[2] > 0.0f ? renderer->environment_get_sky_orientation(p_environment) : Basis();
	RendererRD::MaterialStorage::store_transform(Transform3D(input.fog_orientation), input.scene_ubo.height_fog_sky_orientation);
	input.scene_ubo.height_fog_view[1] = 1.0f;
	input.scene_ubo.height_fog_view[3] = 1.0f;
	input.scene_ubo.fog_enabled = renderer->environment_get_fog_enabled(p_environment);
	input.scene_ubo.fog_density = renderer->environment_get_fog_density(p_environment);
	input.scene_ubo.fog_aerial_perspective = renderer->environment_get_fog_aerial_perspective(p_environment);
	input.scene_ubo.fog_sky_affect = renderer->environment_get_fog_sky_affect(p_environment);
	input.scene_ubo.fog_use_legacy_blending = renderer->fog_use_legacy_blending_get();
	input.scene_ubo.z_far = 100000.0;
	Color fog_color = renderer->environment_get_fog_light_color(p_environment).srgb_to_linear();
	float fog_energy = renderer->environment_get_fog_light_energy(p_environment);
	for (int i = 0; i < 3; i++) {
		input.scene_ubo.fog_light_color[i] = fog_color[i] * fog_energy;
	}
	// Directional fog needs an explicit immutable directional-light snapshot.
	if (input.scene_ubo.fog_enabled && renderer->environment_get_fog_sun_scatter(p_environment) > 0.0) {
		sky->capture_error = "unsupported_directional_fog_snapshot";
		return;
	}
	if (sky->active_input.generation == p_generation && sky->active_input.origin == p_origin && sky->active_input.fog_orientation == input.fog_orientation) {
		sky->pending_input = CaptureInput();
		sky->capture_error = String();
		return;
	}
	if (!sky->active_input.generation && sky->published_complete && p_generation == sky->published_generation && p_origin == sky->published_origin && sky->published_fog_orientation == input.fog_orientation) {
		sky->pending_input = CaptureInput();
		sky->capture_error = String();
		return;
	}
	if (sky->pending_input.generation == p_generation && sky->pending_input.origin == p_origin && sky->pending_input.fog_orientation == input.fog_orientation) {
		sky->capture_error = String();
		return;
	}
	sky->pending_input = input;
	if (!sky->published_complete && !sky->active_input.generation) {
		sky->fallback = input.fallback;
	}
	RenderingServerDefault::redraw_request();
	sky->capture_error = String();
}

void SkyRD::_allocate_capture_radiance(Sky *sky) {
	bool use_raster_effect = RendererRD::CopyEffects::get_singleton()->get_raster_effects().has_flag(CopyEffects::RASTER_EFFECT_OCTMAP);
	int mipmaps = Image::get_image_required_mipmaps(sky->radiance_size, sky->radiance_size, Image::FORMAT_RGBAH) + 1;

	int layers = roughness_layers;
	bool use_realtime = sky->mode == RSE::SKY_MODE_REALTIME || sky->internal_mode == RSE::SKY_MODE_REALTIME;
	if (use_realtime) {
		layers = Sky::REAL_TIME_ROUGHNESS_LAYERS;
	}

	if (sky_use_octmap_array) {
		mipmaps -= 2; //  reduce the number of mipmaps to keep the border size reasonable.
		// Double size to approximate texel density of cubemaps + add border for proper filtering/mipmapping.
		uint32_t padding_pixels = (1 << (mipmaps - 1));
		uint32_t w = sky->radiance_size * 2 + padding_pixels * 2;
		uint32_t h = w;
		sky->uv_border_size = float(padding_pixels) / float(w);

		// Array (higher quality, more memory).
		RD::TextureFormat tf;
		tf.array_layers = layers;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		tf.mipmaps = mipmaps;
		tf.width = w;
		tf.height = h;
		tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		if (!use_raster_effect) {
			tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
		}

		sky->radiance = RD::get_singleton()->texture_create(tf, RD::TextureView());

		// Create view into the first layer slice for user shaders.
		sky->radiance_first_layer_slice = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), sky->radiance, 0, 0, mipmaps, RD::TEXTURE_SLICE_2D, 1);

		sky->reflection.update_reflection_data(w, mipmaps, true, sky->radiance, 0, use_realtime, roughness_layers, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, sky->uv_border_size);
	} else {
		// Double size to approximate texel density of cubemaps + add border for proper filtering/mipmapping.
		uint32_t padding_pixels = (1 << (MIN(mipmaps, layers) - 1));
		uint32_t w = sky->radiance_size * 2 + padding_pixels * 2;
		uint32_t h = w;
		sky->uv_border_size = float(padding_pixels) / float(w);

		// Single texture (lower quality, less memory).
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.mipmaps = MIN(mipmaps, layers);
		tf.width = w;
		tf.height = h;
		tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		if (!use_raster_effect) {
			tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
		}

		sky->radiance = RD::get_singleton()->texture_create(tf, RD::TextureView());

		DEV_ASSERT(sky->radiance_first_layer_slice.is_null());

		sky->reflection.update_reflection_data(w, MIN(mipmaps, layers), false, sky->radiance, 0, use_realtime, roughness_layers, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, sky->uv_border_size);
	}
}

void SkyRD::_release_capture_work(Sky *sky) {
	if (sky->capture_work) {
		sky->capture_work->free();
		memdelete(sky->capture_work);
		sky->capture_work = nullptr;
	}
	for (RID rid : { sky->capture_scene_buffer, sky->capture_light_buffer }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	sky->capture_scene_buffer = RID();
	sky->capture_light_buffer = RID();
	sky->capture_scene_set = RID();
	sky->active_input = CaptureInput();
	sky->blend_started = -1.0;
	sky->blend_weight = 0.0;
}

void SkyRD::sky_cancel_capture(RID p_sky) {
	Sky *sky = get_sky(p_sky);
	ERR_FAIL_NULL(sky);
	_release_capture_work(sky);
	sky->pending_input = CaptureInput();
	sky->capture_error = "cancelled";
}

bool SkyRD::_capture_step(Sky *sky) {
	Sky *work = sky->capture_work;
	auto *material = static_cast<SkyMaterialData *>(MaterialStorage::get_singleton()->material_get_data(sky->active_input.material, MaterialStorage::SHADER_TYPE_SKY));
	if (!material || !material->shader_data || !material->shader_data->valid) {
		sky->capture_error = "invalid_snapshot_shader";
		return false;
	}
	SkyShaderData *shader = material->shader_data;
	if (shader->uses_light) {
		sky->capture_error = "unsupported_builtin_lights";
		return false;
	}
	for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &uniform : shader->uniforms) {
		if (uniform.value.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
			sky->capture_error = "unsupported_global_uniform";
			return false;
		}
	}
	if ((shader->ubo_size > 0 || !shader->texture_uniforms.is_empty()) && (!material->uniform_set.is_valid() || !RD::get_singleton()->uniform_set_is_valid(material->uniform_set))) {
		sky->capture_error = "invalid_snapshot_parameters";
		return false;
	}
	if (sky->capture_phase < 3) {
		while (sky->capture_phase < 2 && !(sky->capture_phase == 0 ? shader->uses_quarter_res : shader->uses_half_res)) {
			sky->capture_phase++;
		}
		int mip = sky->capture_phase == 0 ? 2 : (sky->capture_phase == 1 ? 1 : 0);
		if (work->reflection.layers.is_empty() || mip >= (int)work->reflection.layers[0].mipmaps.size()) {
			sky->capture_error = "capture_layout_missing";
			return false;
		}
		SkyVersion version = mip == 2 ? SKY_VERSION_OCTMAP_QUARTER_RES : (mip == 1 ? SKY_VERSION_OCTMAP_HALF_RES : SKY_VERSION_OCTMAP);
		SkyTextureSetVersion textures = mip == 2 ? SKY_TEXTURE_SET_OCTMAP_QUARTER_RES : (mip == 1 ? SKY_TEXTURE_SET_OCTMAP_HALF_RES : SKY_TEXTURE_SET_OCTMAP);
		Projection projection, correction;
		projection.set_perspective(90, 1, 0.01, 10.0);
		correction.set_depth_correction(false);
		projection = correction * projection;
		RID fb = work->reflection.layers[0].mipmaps[mip].framebuffer;
		// Resolve the exact capture variant before opening or counting its draw.
		// Sky pipelines compile synchronously; an invalid pipeline is a failed
		// capture, never a successfully captured black radiance map.
		RID pipeline = shader->pipelines[version].get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(fb), false, 0);
		if (pipeline.is_null() || !RD::get_singleton()->render_pipeline_is_valid(pipeline)) {
			sky->capture_error = "capture_pipeline_creation_failed";
			return false;
		}
		RID texture_set = work->get_textures(textures, sky_shader.default_shader_rd, false, Ref<RenderSceneBuffersRD>());
		if (!RD::get_singleton()->uniform_set_is_valid(texture_set) || !RD::get_singleton()->uniform_set_is_valid(sky->capture_scene_set)) {
			sky->capture_error = "capture_bindings_invalid";
			return false;
		}
		SkySceneState::UBO pass_ubo = sky->active_input.scene_ubo;
		if (mip != 0) {
			pass_ubo.fog_enabled = false;
			pass_ubo.height_fog.control[3] = 0.0f;
		}
		RD::get_singleton()->buffer_update(sky->capture_scene_buffer, 0, sizeof(SkySceneState::UBO), &pass_ubo);
		RD::DrawListID draw = RD::get_singleton()->draw_list_begin(fb, RD::DRAW_IGNORE_COLOR_ALL);
		_render_sky(draw, sky->active_input.time, fb, &shader->pipelines[version], material->uniform_set, texture_set, projection, Basis(), sky->active_input.origin, 1.0, mip == 0 ? sky->active_input.brightness : 1.0, work->uv_border_size, sky->capture_scene_set);
		RD::get_singleton()->draw_list_end();
		sky->capture_phase++;
		sky->total_capture_passes++;
		return true;
	}
	const int max_layer = sky_use_octmap_array ? work->reflection.layers.size() : work->reflection.layers[0].mipmaps.size();
	if (sky->capture_filter < max_layer) {
		work->reflection.create_reflection_importance_sample(sky_use_octmap_array, sky->capture_filter, sky_ggx_samples_quality);
		if (sky_use_octmap_array) {
			work->reflection.update_reflection_mipmaps(sky->capture_filter, sky->capture_filter + 1);
		}
		sky->capture_filter++;
		sky->total_filter_steps++;
		if (sky->capture_filter < max_layer) {
			return true;
		}
	}
	if (sky_use_octmap_array) {
		work->reflection.update_reflection_mipmaps(0, 1);
	}
	sky->blend_started = double(OS::get_singleton()->get_ticks_usec()) * 1e-6;
	sky->blend_weight = 0.0;
	return true;
}

void SkyRD::process_captures() {
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	if (frame == last_capture_frame) {
		return;
	}
	last_capture_frame = frame;
	const double now = double(OS::get_singleton()->get_ticks_usec()) * 1e-6;
	// Publication/CPU time advance is not capture work, and must reach every sky.
	for (RID rid : capture_queue) {
		Sky *sky = get_sky(rid);
		if (!sky || sky->blend_started < 0.0) {
			continue;
		}
		sky->blend_weight = CLAMP((now - sky->blend_started) / 0.2, 0.0, 1.0);
		if (sky->blend_weight < 1.0) {
			RenderingServerDefault::redraw_request();
			continue;
		}
		Sky *work = sky->capture_work;
		SWAP(sky->radiance, work->radiance);
		SWAP(sky->radiance_first_layer_slice, work->radiance_first_layer_slice);
		SWAP(sky->reflection, work->reflection);
		SWAP(sky->uv_border_size, work->uv_border_size);
		sky->published_generation = sky->active_input.generation;
		sky->published_origin = sky->active_input.origin;
		sky->published_fog_orientation = sky->active_input.fog_orientation;
		sky->published_has_height_fog = sky->active_input.scene_ubo.height_fog.options[2] > 0.0f;
		sky->published_complete = true;
		sky->baked_exposure = 1.0;
		_release_capture_work(sky);
	}
	for (uint32_t offset = 0; offset < capture_queue.size(); offset++) {
		const uint32_t index = (capture_cursor + offset) % capture_queue.size();
		Sky *sky = get_sky(capture_queue[index]);
		if (!sky || sky->blend_started >= 0.0) {
			continue;
		}
		if (!sky->capture_work) {
			if (!sky->pending_input.generation) {
				continue;
			}
			if (sky->last_accept >= 0.0 && now < sky->last_accept + 0.25) {
				RenderingServerDefault::redraw_request();
				continue;
			}
			sky->active_input = sky->pending_input;
			sky->pending_input = CaptureInput();
			sky->last_accept = now;
			sky->capture_error = String();
			sky->fallback = sky->active_input.fallback;
			sky->capture_work = memnew(Sky);
			sky->capture_work->radiance_size = sky->radiance_size;
			_allocate_capture_radiance(sky->capture_work);
			if (!sky->published_complete) {
				sky->uv_border_size = sky->capture_work->uv_border_size;
			}
			if (sky->capture_work->radiance.is_null()) {
				sky->capture_error = "allocation_failed";
				_release_capture_work(sky);
				continue;
			}
			sky->capture_scene_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(SkySceneState::UBO), Vector<uint8_t>());
			RD::get_singleton()->buffer_update(sky->capture_scene_buffer, 0, sizeof(SkySceneState::UBO), &sky->active_input.scene_ubo);
			Vector<uint8_t> lights;
			lights.resize(sizeof(SkyDirectionalLightData) * sky_scene_state.max_directional_lights);
			lights.fill(0);
			sky->capture_light_buffer = RD::get_singleton()->uniform_buffer_create(lights.size(), lights);
			Vector<RD::Uniform> uniforms;
			RD::Uniform global;
			global.binding = 1;
			global.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			global.append_id(MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer());
			uniforms.push_back(global);
			RD::Uniform scene;
			scene.binding = 2;
			scene.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			scene.append_id(sky->capture_scene_buffer);
			uniforms.push_back(scene);
			RD::Uniform light;
			light.binding = 3;
			light.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			light.append_id(sky->capture_light_buffer);
			uniforms.push_back(light);
			MaterialStorage::get_singleton()->samplers_rd_get_default().append_uniforms(uniforms, SAMPLERS_BINDING_FIRST_INDEX);
			sky->capture_scene_set = RD::get_singleton()->uniform_set_create(uniforms, sky_shader.default_shader_rd, SKY_SET_UNIFORMS);
			if (sky->capture_scene_buffer.is_null() || sky->capture_light_buffer.is_null() || sky->capture_scene_set.is_null()) {
				sky->capture_error = "snapshot_uniform_allocation_failed";
				_release_capture_work(sky);
				continue;
			}
			sky->capture_phase = 0;
			sky->capture_filter = 1;
		}
		if (!_capture_step(sky)) {
			_release_capture_work(sky);
		}
		sky->last_work_frame = frame;
		capture_cursor = (index + 1) % capture_queue.size();
		RenderingServerDefault::redraw_request();
		break;
	}
}

Dictionary SkyRD::sky_get_capture_status(RID p_sky) const {
	Dictionary result;
	const Sky *sky = get_sky(p_sky);
	if (!sky) {
		result["state"] = "INVALID_SKY";
		return result;
	}
	result["state"] = !sky->managed_capture ? "UNMANAGED" : (!sky->capture_error.is_empty() ? "FAILED" : (sky->blend_started >= 0.0 ? "BLENDING" : (sky->capture_work ? "CAPTURING" : (sky->pending_input.generation ? "QUEUED" : (sky->published_complete ? "READY" : "FALLBACK")))));
	result["reason"] = sky->capture_error;
	result["active_generation"] = sky->active_input.generation;
	result["pending_generation"] = sky->pending_input.generation;
	result["published_generation"] = sky->published_generation;
	result["active_fog_orientation"] = sky->active_input.fog_orientation;
	result["pending_fog_orientation"] = sky->pending_input.fog_orientation;
	result["published_fog_orientation"] = sky->published_fog_orientation;
	result["published_has_height_fog"] = sky->published_has_height_fog;
	result["active_has_height_fog"] = sky->active_input.scene_ubo.height_fog.options[2] > 0.0f;
	result["capture_passes"] = sky->total_capture_passes;
	result["filter_steps"] = sky->total_filter_steps;
	result["active_material"] = sky->active_input.material;
	result["pending_material"] = sky->pending_input.material;
	result["requested_generation"] = MAX(sky->published_generation, MAX(sky->active_input.generation, sky->pending_input.generation));
	result["ready_generation"] = sky->blend_started >= 0.0 ? sky->active_input.generation : sky->published_generation;
	result["last_work_frame"] = sky->last_work_frame;
	const double requested_at = sky->active_input.generation ? sky->active_input.requested_at : sky->pending_input.requested_at;
	result["age_seconds"] = requested_at > 0.0 ? double(OS::get_singleton()->get_ticks_usec()) * 1e-6 - requested_at : 0.0;
	result["blend_weight"] = sky->blend_started >= 0.0 ? sky->blend_weight : (sky->published_complete ? 1.0 : 0.0);
	result["origin"] = sky->active_input.generation ? sky->active_input.origin : sky->published_origin;
	result["baked_exposure"] = sky->baked_exposure;
	result["complete_resources"] = int(sky->published_complete) + int(sky->blend_started >= 0.0);
	result["retained_radiance_resources"] = int(sky->radiance.is_valid()) + int(sky->capture_work != nullptr);
	return result;
}

RID SkyRD::sky_get_next_radiance_texture_rd(RID p_sky, bool p_capture_view) const {
	const Sky *sky = get_sky(p_sky);
	return sky && !p_capture_view && sky->blend_started >= 0.0 ? sky->capture_work->radiance : RID();
}

bool SkyRD::sky_get_capture_sampling(RID p_sky, bool p_capture_view, float *r_data, float *r_fallback) const {
	const Sky *sky = get_sky(p_sky);
	if (!sky || !sky->managed_capture) {
		return false;
	}
	r_data[0] = 1.0;
	r_data[1] = p_capture_view ? 0.0 : sky->blend_weight;
	r_data[2] = sky->published_complete ? 1.0 : 0.0;
	r_data[3] = (sky->published_complete && sky->published_has_height_fog ? 1.0f : 0.0f) + (!p_capture_view && sky->blend_started >= 0.0 && sky->active_input.scene_ubo.height_fog.options[2] > 0.0f ? 2.0f : 0.0f);
	for (int i = 0; i < 3; i++) {
		r_fallback[i] = sky->fallback[i];
	}
	r_fallback[3] = 0.0;
	return true;
}

void SkyRD::sky_get_capture_orientation_corrections(RID p_sky, const Basis &p_live_orientation, Basis &r_old, Basis &r_next) const {
	r_old = Basis();
	r_next = Basis();
	const Sky *sky = get_sky(p_sky);
	if (!sky || !sky->managed_capture) {
		return;
	}
	// Each complete fogged map has its own artwork basis. Undo the live lookup
	// basis and enter that map's basis before octahedral encoding. Thus baked
	// physical air stays world-fixed even while a newer orientation is pending.
	if (sky->published_complete && sky->published_has_height_fog) {
		r_old = sky->published_fog_orientation.inverse() * p_live_orientation;
	}
	if (sky->blend_started >= 0.0 && sky->active_input.scene_ubo.height_fog.options[2] > 0.0f) {
		r_next = sky->active_input.fog_orientation.inverse() * p_live_orientation;
	}
}
