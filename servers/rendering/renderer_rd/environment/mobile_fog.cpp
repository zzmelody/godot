#include "mobile_fog.h"

#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/storage/environment_storage.h"

namespace RendererRD {
void MobileFog::initialize() {
	if (initialized) {
		return;
	}
	Vector<String> modes;
	for (int array = 0; array < 2; array++) {
		for (int msaa = 0; msaa < 2; msaa++) {
			for (int algorithm = 0; algorithm < 3; algorithm++) {
				modes.push_back(String(array ? "\n#define USE_RADIANCE_OCTMAP_ARRAY\n" : "") + (msaa ? "\n#define MSAA_INPUT\n" : "") + (algorithm == 1 ? "\n#define AO_SSAO\n" : (algorithm == 2 ? "\n#define AO_GTAO\n" : "")));
			}
		}
	}
	Vector<uint64_t> dynamic_buffers;
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(0, 0));
	shader.initialize(modes, String(), Vector<RD::PipelineImmutableSampler>(), dynamic_buffers);
	version = shader.version_create();
	RD::SamplerState state;
	nearest_sampler = RD::get_singleton()->sampler_create(state);
	state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	linear_sampler = RD::get_singleton()->sampler_create(state);
	initialized = true;
}
MobileFog::~MobileFog() {
	RD *rd = RD::get_singleton();
	for (const KeyValue<uint64_t, RID> &entry : pipelines) {
		rd->free_rid(entry.value);
	}
	if (nearest_sampler.is_valid()) {
		rd->free_rid(nearest_sampler);
	}
	if (linear_sampler.is_valid()) {
		rd->free_rid(linear_sampler);
	}
	if (version.is_valid()) {
		shader.version_free(version);
	}
}
bool MobileFog::MobileFogView::composite_mobile_ao(RID p_ao, RID p_guide, int p_algorithm, int p_view, float p_radius, float p_intensity, float p_max_distance) {
	if (!active || !owner || p_algorithm < 1 || p_algorithm > 2) {
		return false;
	}
	return owner->composite(this, p_ao, p_guide, p_algorithm, p_view, p_radius, p_intensity, p_max_distance);
}
bool MobileFog::prepare(const RenderDataRD *p_data, bool p_requested, RID p_radiance, RID p_next, RID p_ap0, RID p_ap1, bool p_array, int p_roughness_layers, float p_luminance) {
	Ref<RenderSceneBuffersRD> rb = p_data->render_buffers;
	Ref<MobileFogView> view;
	if (rb->has_custom_data(SNAME("mobile_fog"))) {
		view = rb->get_custom_data(SNAME("mobile_fog"));
	}
	auto height_fog = p_data->environment.is_valid() ? RendererEnvironmentStorage::get_singleton()->environment_get_height_fog_data(p_data->environment) : RendererEnvironmentStorage::HeightFogData();
	bool native_height = height_fog.options[2] > 0.0f;
	bool height_enabled = native_height && height_fog.control[3] > 0.0f && height_fog.density[0] > 0.0f && height_fog.control[0] > 0.0f;
	bool ap_enabled = p_data->scene_data->aerial_perspective_parameters.x > 0.0f || p_data->scene_data->aerial_perspective_parameters.y > 0.0f;
	bool legacy_fog = p_data->environment.is_valid() && RendererEnvironmentStorage::get_singleton()->environment_get_fog_enabled(p_data->environment) && !native_height;
	if (!p_requested || (!height_enabled && !ap_enabled) || legacy_fog) {
		if (view.is_valid()) {
			view->active = false;
		}
		rb->clear_context(SNAME("mobile_fog"));
		return false;
	}
	initialize();
	if (view.is_null()) {
		view.instantiate();
		rb->set_custom_data(SNAME("mobile_fog"), view);
		view->configure(rb.ptr());
	}
	view->owner = this;
	view->active = true;
	view->applied = 0;
	view->scene = p_data->scene_data->get_uniform_buffer();
	view->array = p_array;
	view->luminance = p_luminance;
	view->roughness_lod = p_roughness_layers - 1;
	view->orthographic = p_data->scene_data->cam_orthogonal;
	TextureStorage *ts = TextureStorage::get_singleton();
	view->radiance = p_radiance.is_valid() ? p_radiance : ts->texture_rd_get_default(p_array ? TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	view->next_radiance = p_next.is_valid() ? p_next : view->radiance;
	view->ap[0] = p_ap0;
	view->ap[1] = p_ap1;
	for (uint32_t eye = 0; eye < rb->get_view_count(); eye++) {
		view->projection[eye] = p_data->scene_data->get_view_projection(eye);
	}
	if (!rb->has_texture(SNAME("mobile_fog"), SNAME("mask"))) {
		rb->create_texture(SNAME("mobile_fog"), SNAME("mask"), RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT, rb->get_texture_samples(), rb->get_internal_size(), rb->get_view_count(), 1);
	}
	return true;
}
RID MobileFog::get_framebuffer(Ref<RenderSceneBuffersRD> rb, bool p_resolve_depth) {
	bool msaa = rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
	Vector<RID> textures;
	textures.push_back(msaa ? rb->get_color_msaa() : rb->get_internal_texture());
	textures.push_back(msaa ? rb->get_depth_msaa() : rb->get_depth_texture());
	textures.push_back(rb->get_texture(SNAME("mobile_fog"), SNAME("mask")));
	if (msaa) {
		textures.push_back(rb->get_internal_texture());
	}
	if (msaa && p_resolve_depth) {
		textures.push_back(rb->get_depth_texture());
	}
	Vector<RD::FramebufferPass> passes;
	RD::FramebufferPass opaque;
	opaque.color_attachments.push_back(0);
	opaque.color_attachments.push_back(2);
	opaque.depth_attachment = 1;
	passes.push_back(opaque);
	RD::FramebufferPass sky;
	sky.color_attachments.push_back(0);
	sky.depth_attachment = 1;
	if (msaa) {
		sky.resolve_attachments.push_back(3);
		if (p_resolve_depth) {
			sky.depth_resolve_attachment = 4;
		}
	}
	passes.push_back(sky);
	return FramebufferCacheRD::get_singleton()->get_cache_multipass(textures, passes, rb->get_view_count());
}
bool MobileFog::composite(MobileFogView *view, RID p_ao, RID p_guide, int p_algorithm, int p_eye, float p_radius, float p_intensity, float p_max_distance) {
	RenderSceneBuffersRD *rb = view->buffers;
	ERR_FAIL_COND_V(p_eye < 0 || uint32_t(p_eye) >= rb->get_view_count(), false);
	// A view is composited exactly once, even if a producer calls twice.
	ERR_FAIL_COND_V(view->applied & (1u << p_eye), false);
	RD *rd = RD::get_singleton();
	if (p_algorithm != 0) {
		ERR_FAIL_COND_V(!rd->texture_is_valid(p_ao) || !rd->texture_is_valid(p_guide), false);
		RD::TextureFormat ao_format = rd->texture_get_format(p_ao);
		RD::TextureFormat guide_format = rd->texture_get_format(p_guide);
		Size2i size = rb->get_internal_size();
		Size2i expected = p_algorithm == 1 ? Size2i((size.x + 1) / 2, (size.y + 1) / 2) : size;
		ERR_FAIL_COND_V(Size2i(ao_format.width, ao_format.height) != expected || Size2i(guide_format.width, guide_format.height) != expected, false);
		ERR_FAIL_COND_V(ao_format.samples != RD::TEXTURE_SAMPLES_1 || guide_format.samples != RD::TEXTURE_SAMPLES_1, false);
		ERR_FAIL_COND_V(ao_format.format != (p_algorithm == 1 ? RD::DATA_FORMAT_R16_SFLOAT : RD::DATA_FORMAT_R8_UINT), false);
		ERR_FAIL_COND_V(guide_format.format != (p_algorithm == 1 ? RD::DATA_FORMAT_R32G32_UINT : RD::DATA_FORMAT_R32_SFLOAT), false);
	}
	bool msaa = rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
	int variant = (view->array ? 6 : 0) + (msaa ? 3 : 0) + p_algorithm;
	RID shader_rid = shader.version_get_shader(version, variant);
	ERR_FAIL_COND_V(shader_rid.is_null(), false);
	Vector<RID> colors;
	colors.push_back((msaa ? rb->get_color_msaa(p_eye) : rb->get_internal_texture(p_eye)));
	RD::FramebufferPass pass;
	pass.color_attachments.push_back(0);
	if (msaa) {
		colors.push_back(rb->get_internal_texture(p_eye));
		pass.resolve_attachments.push_back(1);
	}
	Vector<RD::FramebufferPass> passes;
	passes.push_back(pass);
	RID framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multipass(colors, passes, 1);
	auto format = rd->framebuffer_get_format(framebuffer);
	uint64_t key = (uint64_t(format) << 8) | variant;
	RID *cached = pipelines.getptr(key);
	RID pipeline;
	if (cached) {
		pipeline = *cached;
	} else {
		RD::PipelineRasterizationState raster;
		raster.cull_mode = RD::POLYGON_CULL_DISABLED;
		RD::PipelineMultisampleState samples;
		samples.sample_count = rb->get_texture_samples();
		RD::PipelineColorBlendState blend = RD::PipelineColorBlendState::create_disabled(1);
		auto &a = blend.attachments.write[0];
		a.enable_blend = true;
		a.src_color_blend_factor = RD::BLEND_FACTOR_ONE;
		a.dst_color_blend_factor = RD::BLEND_FACTOR_SRC_ALPHA;
		a.src_alpha_blend_factor = RD::BLEND_FACTOR_ZERO;
		a.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
		pipeline = rd->render_pipeline_create(shader_rid, format, RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, raster, samples, RD::PipelineDepthStencilState(), blend);
		ERR_FAIL_COND_V(pipeline.is_null(), false);
		pipelines.insert(key, pipeline);
	}
	LocalVector<RD::Uniform> uniforms;
	auto uniform = [&](int binding, RD::UniformType type, RID a, RID b = RID()) {
		RD::Uniform u;
		u.binding = binding;
		u.uniform_type = type;
		u.append_id(a);
		if (b.is_valid()) {
			u.append_id(b);
		}
		uniforms.push_back(u);
	};
	// Scene data is allocated by MultiUmaBuffer. Binding it as a static UBO
	// ignores the current persistent-ring offset and can read empty/old fog state.
	uniform(0, RD::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC, view->scene);
	uniform(1, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, nearest_sampler, (msaa ? rb->get_depth_msaa(p_eye) : rb->get_depth_texture(p_eye)));
	uniform(2, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, nearest_sampler, rb->get_texture_slice(SNAME("mobile_fog"), SNAME("mask"), p_eye, 0, 1, 1));
	if (p_algorithm) {
		uniform(3, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, nearest_sampler, p_ao);
		uniform(4, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, nearest_sampler, p_guide);
	}
	uniform(5, RD::UNIFORM_TYPE_SAMPLER, linear_sampler);
	uniform(6, RD::UNIFORM_TYPE_TEXTURE, view->radiance);
	uniform(7, RD::UNIFORM_TYPE_TEXTURE, view->next_radiance);
	uniform(8, RD::UNIFORM_TYPE_TEXTURE, view->ap[0]);
	uniform(9, RD::UNIFORM_TYPE_TEXTURE, view->ap[1]);
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, uniforms);
	ERR_FAIL_COND_V(set.is_null(), false);
	struct Push {
		float projection[16];
		float effect[4];
		float camera[4];
	} push;
	Projection inverse = view->projection[p_eye].inverse();
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			push.projection[c * 4 + r] = inverse[c][r];
		}
	}
	push.effect[0] = p_radius;
	push.effect[1] = p_intensity;
	push.effect[2] = p_max_distance;
	push.effect[3] = view->luminance;
	push.camera[0] = view->orthographic;
	push.camera[1] = view->projection[p_eye][1][1];
	push.camera[2] = p_eye;
	push.camera[3] = view->roughness_lod;
	RD::DrawListID list = rd->draw_list_begin(framebuffer);
	rd->draw_list_bind_render_pipeline(list, pipeline);
	rd->draw_list_bind_uniform_set(list, set, 0);
	rd->draw_list_set_push_constant(list, &push, sizeof(push));
	rd->draw_list_draw(list, false, 1, 3);
	rd->draw_list_end();
	view->applied |= 1u << p_eye;
	return true;
}
void MobileFog::finish(Ref<RenderSceneBuffersRD> rb) {
	Ref<MobileFogView> view = rb->get_custom_data(SNAME("mobile_fog"));
	if (view.is_null() || !view->active) {
		return;
	}
	// Missing/disabled/failed AO must never leave opaque geometry unfogged.
	for (uint32_t eye = 0; eye < rb->get_view_count(); eye++) {
		if (!(view->applied & (1u << eye))) {
			composite(view.ptr(), RID(), RID(), 0, eye, 0, 0, 0);
		}
	}
	view->active = false;
}
} //namespace RendererRD
