#include "aerial_perspective.h"

#include "atmosphere_shader_library.h"

#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"

#include <cfloat>
#include <cstring>

namespace RendererRD {
namespace {
constexpr uint64_t VOLUME_BYTES = 32 * 32 * 16 * 8;
#ifdef DEBUG_ENABLED
// Explicit process-local test seam. No public state key, release code path or
// input-validation exemption: the normal compiler/allocation failure arms run.
bool ap_test_failure(const char *p_point) {
	return OS::get_singleton()->get_environment("ANGE_TEST_AP_FAILURE") == p_point;
}
#endif
const char *AP_DECLARATIONS = R"GLSL(#version 450
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(set=0,binding=0) uniform sampler2D transmittance_lut;
layout(set=0,binding=1) uniform sampler2D multiscatter_lut;
layout(rgba16f,set=0,binding=2) uniform writeonly image3D ap_volume;
layout(std140,set=0,binding=3) uniform AtmosphereParams {
 vec4 planet; vec4 rayleigh; vec4 mie_scattering; vec4 mie_absorption;
 vec4 mie_shape_absorption; vec4 absorption; vec4 art_absorption; vec4 sun;
} params;
layout(std140,set=0,binding=4) uniform ViewParams {
 mat4 inverse_projection; mat4 world_from_view;
 vec4 origin_orthographic; vec4 radiance_scale; vec4 eye_offset;
} view;
)GLSL";
const char *AP_MAIN = R"GLSL(
void main() {
 ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
 if(any(greaterThanEqual(pixel,ivec2(32)))) return;
 vec2 ndc=((vec2(pixel)+0.5)/32.0)*2.0-1.0;
 vec4 a=view.inverse_projection*vec4(ndc,1.0,1.0);
 vec4 b=view.inverse_projection*vec4(ndc,0.5,1.0);
 vec3 near_p=a.xyz/a.w;
 vec3 ray_v=normalize(b.xyz/b.w-near_p);
 vec3 origin_v=view.eye_offset.xyz;
 if(view.origin_orthographic.w>0.0)
  origin_v+=near_p-ray_v*(near_p.z/ray_v.z);
 vec3 ray=normalize(mat3(view.world_from_view)*ray_v);
 // Same local planet convention as SkyView: planet vertically below observer.
 vec3 origin=vec3(0.0,planet_radius()+view.origin_orthographic.y*0.001,0.0)
             +mat3(view.world_from_view)*origin_v*0.001;
 float r0=length(origin);
 float mu=dot(origin,ray)/max(r0,EPS);
 float limit=ray_length(r0,mu);
 // An observer above the atmosphere first travels through vacuum.
 float enter=0.0;
 if(r0>top_radius()) {
  float disc=r0*r0*(mu*mu-1.0)+top_radius()*top_radius();
  if(disc<0.0 || mu>=0.0) limit=0.0;
  else enter=max(0.0,-r0*mu-sqrt(max(disc,0.0)));
 }
 vec3 sun_d=sun_direction(), moon_d=moon_direction();
 vec2 phase_s=vec2(phase_rayleigh(dot(ray,sun_d)),phase_mie(dot(ray,sun_d),params.mie_shape_absorption.y));
 vec2 phase_m=vec2(phase_rayleigh(dot(ray,moon_d)),phase_mie(dot(ray,moon_d),params.mie_shape_absorption.y));
 vec3 T=vec3(1.0), L=vec3(0.0);
 imageStore(ap_volume,ivec3(pixel,0),vec4(0.0,0.0,0.0,1.0));
 float previous=0.1;
 for(int z=1;z<16;++z) {
  float u=float(z)/15.0;
  float distance=0.1+95.9*u*u;
  float begin=max(previous,enter), end=min(distance,limit);
  float dt=max(0.0,end-begin)*0.5;
  for(int step=0;step<2;++step) {
   if(dt<=0.0) break;
   vec3 pos=origin+ray*(begin+(float(step)+0.5)*dt);
   float nr=length(pos), alt=max(0.0,nr-planet_radius());
   float sun_mu=dot(pos,sun_d)/max(nr,EPS);
   float moon_mu=dot(pos,moon_d)/max(nr,EPS);
   vec3 ext;
   vec3 source=atmosphere_sample_source(nr,alt,sun_mu,moon_mu,phase_s,phase_m,ext);
   vec3 step_T=exp(-ext*dt);
   L+=T*source*(vec3(1.0)-step_T)/max(ext,vec3(EPS));
   T*=step_T;
  }
  previous=distance;
  imageStore(ap_volume,ivec3(pixel,z),vec4(min(max(L*view.radiance_scale.rgb,vec3(0.0)),vec3(64000.0)),dot(T,vec3(1.0/3.0))));
 }
}
)GLSL";
struct ViewUniform {
	float inverse_projection[16];
	float world_from_view[16];
	float origin_orthographic[4];
	float radiance_scale[4];
	float eye_offset[4];
};
} //namespace

AerialPerspective *AerialPerspective::singleton = nullptr;
AerialPerspective::AerialPerspective() {
	singleton = this;
}
AerialPerspective::~AerialPerspective() {
	current[0].unref();
	current[1].unref();
	capture_view.unref();
	for (View *view : views) {
		view->free_data();
	}
	environments.clear();
	RD *rd = RD::get_singleton();
	if (pipeline.is_valid()) {
		rd->free(pipeline);
	}
	if (shader.is_valid()) {
		rd->free(shader);
	}
	if (sampler.is_valid()) {
		rd->free(sampler);
	}
	singleton = nullptr;
}
AerialPerspective::Source::~Source() {
	if (!RD::get_singleton()) {
		return;
	}
	for (RID texture : textures) {
		if (texture.is_valid()) {
			RD::get_singleton()->free(texture);
		}
	}
}
AerialPerspective::View::View() {
	if (singleton) {
		singleton->views.insert(this);
	}
}
AerialPerspective::View::~View() {
	free_data();
	if (singleton) {
		singleton->views.erase(this);
	}
}
void AerialPerspective::View::free_data() {
	if (RD::get_singleton()) {
		for (RID &texture : volumes) {
			if (texture.is_valid()) {
				RD::get_singleton()->free(texture);
			}
			texture = RID();
		}
		if (uniform_buffer.is_valid()) {
			RD::get_singleton()->free(uniform_buffer);
		}
		if (medium_buffer.is_valid()) {
			RD::get_singleton()->free(medium_buffer);
		}
	}
	uniform_buffer = RID();
	medium_buffer = RID();
	key.clear();
	revision = 0;
	state = "DISABLED";
}
void AerialPerspective::detach(RID p_environment) {
	for (View *view : views) {
		if (view->environment == p_environment) {
			view->free_data();
		}
	}
}
void AerialPerspective::fail(Environment &r_env, RID p_environment, const String &p_reason) {
	r_env.enabled = false;
	r_env.state = "FAILED";
	if (r_env.reason != p_reason) {
		ERR_PRINT("RENDER-AP-FAILED: " + p_reason);
	}
	r_env.reason = p_reason;
	r_env.source.unref();
	detach(p_environment);
}
void AerialPerspective::free_environment(RID p_environment) {
	detach(p_environment);
	environments.erase(p_environment);
}

Dictionary AerialPerspective::get_capabilities() {
	Dictionary d;
	d["version"] = 1;
	d["renderer"] = OS::get_singleton()->get_current_rendering_method();
	const bool supported = singleton && RD::get_singleton() && RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT);
	d["supported"] = supported;
	d["dimensions"] = Vector3i(32, 32, 16);
	d["volume_pair_bytes"] = int64_t(VOLUME_BYTES * 2);
	d["error"] = supported ? "" : "RENDER-AP-UNSUPPORTED";
	return d;
}

void AerialPerspective::set_state(RID p_environment, const Dictionary &p_state) {
	Environment &env = environments[p_environment];
	if (p_state.is_empty()) {
		free_environment(p_environment);
		return;
	}
	// An older asynchronous publication may arrive after a newer ready view.
	// Reject it before parsing any fields, including disable and pending states.
	const Variant incoming_generation = p_state.get("generation", Variant());
	if (incoming_generation.get_type() == Variant::INT && int64_t(incoming_generation) >= 0 && int64_t(incoming_generation) < env.generation) {
		return;
	}
	const String allowed[] = { "version", "generation", "source_generation", "enabled", "source_ready", "medium", "transmittance", "multiscatter", "radiance_scale", "debug_view" };
	for (const Variant *key = p_state.next(nullptr); key; key = p_state.next(key)) {
		bool known = false;
		if (key->get_type() == Variant::STRING || key->get_type() == Variant::STRING_NAME) {
			for (const String &name : allowed) {
				known |= String(*key) == name;
			}
		}
		if (!known) {
			fail(env, p_environment, "unknown_parameter");
			return;
		}
	}
	Variant version = p_state.get("version", Variant()), generation = p_state.get("generation", Variant()), enabled = p_state.get("enabled", Variant());
	if (version.get_type() != Variant::INT || int64_t(version) != 1 || generation.get_type() != Variant::INT || int64_t(generation) < 0 || enabled.get_type() != Variant::BOOL) {
		fail(env, p_environment, "invalid_contract");
		return;
	}
	if (!bool(enabled)) {
		detach(p_environment);
		env.source.unref();
		env.enabled = false;
		env.state = "DISABLED";
		env.reason = String();
		env.accepted = p_state.duplicate(true);
		env.generation = generation;
		return;
	}
	Variant ready = p_state.get("source_ready", Variant());
	if (ready.get_type() != Variant::BOOL) {
		fail(env, p_environment, "invalid_source_readiness");
		return;
	}
	if (!bool(ready)) {
		detach(p_environment);
		env.source.unref();
		env.enabled = false;
		env.state = "PENDING";
		env.reason = "source_not_ready";
		env.accepted = p_state.duplicate(true);
		env.generation = generation;
		return;
	}
	Variant packed = p_state.get("medium", Variant()), scale = p_state.get("radiance_scale", Variant()), debug = p_state.get("debug_view", Variant());
	if (packed.get_type() != Variant::PACKED_FLOAT32_ARRAY || PackedFloat32Array(packed).size() != 32 || scale.get_type() != Variant::VECTOR3 || debug.get_type() != Variant::INT || int64_t(debug) < 0 || int64_t(debug) > 2) {
		fail(env, p_environment, "invalid_parameters");
		return;
	}
	PackedFloat32Array medium = packed;
	for (float f : medium) {
		if (!Math::is_finite(f)) {
			fail(env, p_environment, "nonfinite_medium");
			return;
		}
	}
	// Coefficients, heights and source illuminances are nonnegative. Arc angles
	// and anisotropy are signed; altitude is observer-owned by the AP view.
	const int positive[] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20, 21, 22, 23, 24, 25, 30, 31 };
	for (int i : positive) {
		if (medium[i] < 0) {
			fail(env, p_environment, "negative_medium");
			return;
		}
	}
	Vector3 art = scale;
	if (medium[0] <= 0 || medium[1] <= 0 || !art.is_finite() || art.x < 0 || art.y < 0 || art.z < 0) {
		fail(env, p_environment, "invalid_medium_or_scale");
		return;
	}
	Variant sg = p_state.get("source_generation", generation);
	if (sg.get_type() != Variant::INT || int64_t(sg) < 0) {
		fail(env, p_environment, "invalid_source_generation");
		return;
	}
	RID originals[2];
	const char *names[] = { "transmittance", "multiscatter" };
	for (int i = 0; i < 2; i++) {
		Variant value = p_state.get(names[i], Variant());
		if (value.get_type() != Variant::RID || !RID(value).is_valid()) {
			fail(env, p_environment, "invalid_source_texture");
			return;
		}
		originals[i] = value;
	}
	Ref<Source> source;
	for (const KeyValue<RID, Environment> &entry : environments) {
		const Ref<Source> &candidate = entry.value.source;
		if (candidate.is_valid() && candidate->generation == int64_t(sg) && candidate->originals[0] == originals[0] && candidate->originals[1] == originals[1]) {
			source = candidate;
			break;
		}
	}
	if (source.is_null()) {
		source.instantiate();
		source->generation = sg;
		RD *rd = RD::get_singleton();
		for (int i = 0; i < 2; i++) {
			RID original = TextureStorage::get_singleton()->texture_get_rd_texture(originals[i]);
			if (!original.is_valid() || !rd->texture_is_valid(original)) {
				fail(env, p_environment, "source_texture_not_ready");
				return;
			}
			RD::TextureFormat format = rd->texture_get_format(original);
			if (format.texture_type != RD::TEXTURE_TYPE_2D || format.array_layers != 1 || format.samples != RD::TEXTURE_SAMPLES_1 || !(format.usage_bits & RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT)) {
				fail(env, p_environment, "source_texture_format_or_copy_usage");
				return;
			}
			if (format.format != RD::DATA_FORMAT_R16G16B16A16_SFLOAT && format.format != RD::DATA_FORMAT_R32G32B32A32_SFLOAT) {
				fail(env, p_environment, "source_texture_must_be_linear_float_rgba");
				return;
			}
			format.mipmaps = 1;
			format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			source->textures[i] = rd->texture_create(format, RD::TextureView());
			if (!source->textures[i].is_valid() || rd->texture_copy(original, source->textures[i], Vector3(), Vector3(), Vector3(format.width, format.height, 1), 0, 0, 0, 0) != OK) {
				fail(env, p_environment, "source_copy_failed");
				return;
			}
			source->originals[i] = originals[i];
			source->bytes += uint64_t(format.width) * format.height * (format.format == RD::DATA_FORMAT_R16G16B16A16_SFLOAT ? 8 : 16);
			++total_source_copies;
		}
	}
	bool unchanged = env.enabled && env.generation == int64_t(generation) && env.source == source && env.debug == int64_t(debug) && env.scale == art && memcmp(env.medium, medium.ptr(), sizeof(env.medium)) == 0;
	env.source = source;
	memcpy(env.medium, medium.ptr(), sizeof(env.medium));
	env.scale = art;
	env.debug = debug;
	env.enabled = true;
	env.generation = generation;
	env.accepted = p_state.duplicate(true);
	env.reason = String();
	if (!unchanged) {
		env.revision = ++serial;
		env.state = "PENDING";
	}
}

bool AerialPerspective::initialize() {
	if (pipeline.is_valid()) {
		return true;
	}
	if (!shader_error.is_empty()) {
		return false;
	}
	RD *rd = RD::get_singleton();
	if (!bool(get_capabilities()["supported"])) {
		shader_error = "format_unsupported";
		return false;
	}
	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	String compute_source = String(AP_DECLARATIONS) + ATMOSPHERE_SHADER_LIBRARY + AP_MAIN;
#ifdef DEBUG_ENABLED
	if (ap_test_failure("shader")) {
		// This is a real compiler rejection, not a fabricated FAILED status.
		compute_source += "\n#error ANGE_TEST_AP_FAILURE_SHADER\n";
	}
#endif
	stage.spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_COMPUTE, compute_source, RD::SHADER_LANGUAGE_GLSL, &shader_error);
	if (stage.spirv.is_empty()) {
		if (shader_error.is_empty()) {
			shader_error = "shader_compilation_failed";
		}
		return false;
	}
	Vector<RD::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	shader = rd->shader_create_from_spirv(stages, "Native aerial perspective");
	if (!shader.is_valid()) {
		shader_error = "shader_creation_failed";
		return false;
	}
	pipeline = rd->compute_pipeline_create(shader);
	RD::SamplerState sam;
	sam.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	sam.min_filter = RD::SAMPLER_FILTER_LINEAR;
	sam.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sam.repeat_v = sam.repeat_u;
	sam.repeat_w = sam.repeat_u;
	sampler = rd->sampler_create(sam);
	if (!pipeline.is_valid() || !sampler.is_valid()) {
		shader_error = "pipeline_or_sampler_allocation_failed";
		return false;
	}
	return true;
}

bool AerialPerspective::render_view(const RenderDataRD *p_data, Ref<View> view, int p_eye) {
	Environment *env = environments.getptr(p_data->environment);
	if (!env || !env->enabled || env->source.is_null()) {
		view->free_data();
		return false;
	}
	view->environment = p_data->environment;
	view->probe = p_data->reflection_probe;
	view->capture_face = p_data->reflection_probe.is_valid() ? p_data->reflection_probe_pass : -1;
	view->eye = p_eye;
	view->origin = p_data->scene_data->cam_transform.origin;
	Projection correction;
	correction.set_depth_correction(p_data->scene_data->flip_y, true);
	correction.add_jitter_offset(p_data->scene_data->taa_jitter);
	Projection projection = correction * (p_data->scene_data->view_count > 1 ? p_data->scene_data->view_projection[p_eye] : p_data->scene_data->cam_projection);
	view->projection = projection;
	view->orthographic = p_data->scene_data->cam_orthogonal;
	ViewUniform data = {};
	MaterialStorage::store_camera(projection.inverse(), data.inverse_projection);
	MaterialStorage::store_transform(p_data->scene_data->cam_transform, data.world_from_view);
	for (int c = 0; c < 3; c++) {
		data.origin_orthographic[c] = view->origin[c];
		data.radiance_scale[c] = env->scale[c];
		data.eye_offset[c] = p_data->scene_data->view_count > 1 ? p_data->scene_data->view_eye_offset[p_eye][c] : 0;
	}
	data.origin_orthographic[3] = p_data->scene_data->cam_orthogonal ? 1 : 0;
	const Size2i internal = p_data->render_buffers->get_internal_size(), target = p_data->render_buffers->get_target_size();
	int32_t dimensions[] = { internal.x, internal.y, target.x, target.y, int32_t(p_data->reflection_probe_pass) };
	PackedByteArray key;
	key.resize(sizeof(data) + sizeof(dimensions));
	memcpy(key.ptrw(), &data, sizeof(data));
	memcpy(key.ptrw() + sizeof(data), dimensions, sizeof(dimensions));
	if (view->revision == env->revision && view->key == key && view->state == "READY") {
		return true;
	}
	view->state = "PENDING";
	view->reason = String();
	if (!initialize()) {
		view->reason = shader_error;
		fail(*env, p_data->environment, shader_error);
		view->state = "FAILED";
		return false;
	}
	RD *rd = RD::get_singleton();
	if (view->volumes[0].is_null()) {
		RD::TextureFormat format;
		format.texture_type = RD::TEXTURE_TYPE_3D;
		format.width = 32;
		format.height = 32;
		format.depth = 16;
		format.array_layers = 1;
		format.mipmaps = 1;
		format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		for (int i = 0; i < 2; i++) {
#ifdef DEBUG_ENABLED
			if (i == 1 && ap_test_failure("volume")) {
				// Inject an allocator's invalid return after one real allocation,
				// so the existing branch must also release partial resources.
				view->volumes[i] = RID();
				continue;
			}
#endif
			view->volumes[i] = rd->texture_create(format, RD::TextureView());
		}

		if (!view->volumes[0].is_valid() || !view->volumes[1].is_valid()) {
			fail(*env, p_data->environment, "view_allocation_failed");
			view->state = "FAILED";
			view->reason = "view_allocation_failed";
			return false;
		}
	}
	const int back = 1 - view->front;
	// buffer_update is deliberately not used: RD may group multiple updates
	// before all draws, which would give serial probe faces the last face's
	// camera. Immutable per-dispatch UBOs retain their own frame data.
	if (view->uniform_buffer.is_valid()) {
		rd->free(view->uniform_buffer);
	}
	if (view->medium_buffer.is_valid()) {
		rd->free(view->medium_buffer);
	}
	view->uniform_buffer = rd->uniform_buffer_create(sizeof(data), Span<uint8_t>(reinterpret_cast<uint8_t *>(&data), sizeof(data)));
#ifdef DEBUG_ENABLED
	if (ap_test_failure("uniform")) {
		// Allocator-return injection, not a claim that the GPU exhausted memory.
		// Both real volumes and the first real UBO must be cleaned by fail().
		view->medium_buffer = RID();
	} else
#endif
	{
		view->medium_buffer = rd->uniform_buffer_create(sizeof(env->medium), Span<uint8_t>(reinterpret_cast<uint8_t *>(env->medium), sizeof(env->medium)));
	}
	if (!view->uniform_buffer.is_valid() || !view->medium_buffer.is_valid()) {
		fail(*env, p_data->environment, "view_uniform_allocation_failed");
		view->state = "FAILED";
		return false;
	}
	RD::Uniform u[5];
	for (int i = 0; i < 2; i++) {
		u[i].binding = i;
		u[i].uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u[i].append_id(sampler);
		u[i].append_id(env->source->textures[i]);
	}
	u[2].binding = 2;
	u[2].uniform_type = RD::UNIFORM_TYPE_IMAGE;
	u[2].append_id(view->volumes[back]);
	u[3].binding = 3;
	u[3].uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	u[3].append_id(view->medium_buffer);
	u[4].binding = 4;
	u[4].uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	u[4].append_id(view->uniform_buffer);
	RID set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u[0], u[1], u[2], u[3], u[4]);
	if (!set.is_valid()) {
		fail(*env, p_data->environment, "view_uniform_set_failed");
		view->state = "FAILED";
		return false;
	}
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_dispatch(list, 4, 4, 1);
	rd->compute_list_end();
	// The same texture RID is subsequently bound as sampled data, so RD's
	// resource graph emits the storage-write -> fragment-read dependency. This
	// also orders a serial probe face before the pool's next compute overwrite.
	view->front = back;
	view->key = key;
	view->revision = env->revision;
	view->generation = env->generation;
	view->state = "READY";
	++view->dispatches;
	++total_dispatches;
	env->state = "READY";
	return true;
}

void AerialPerspective::prepare(const RenderDataRD *p_data) {
	current[0].unref();
	current[1].unref();
	ERR_FAIL_NULL(p_data);
	ERR_FAIL_COND(p_data->render_buffers.is_null());
	Environment *env = environments.getptr(p_data->environment);
	if (!env || !env->enabled) {
		if (p_data->reflection_probe.is_valid()) {
			if (capture_view.is_valid()) {
				capture_view->free_data();
			}
		} else {
			for (const StringName &scope : { SNAME("aerial_perspective_0"), SNAME("aerial_perspective_1") }) {
				if (p_data->render_buffers->has_custom_data(scope)) {
					p_data->render_buffers->get_custom_data(scope)->free_data();
				}
			}
		}
		return;
	}
	if (p_data->reflection_probe.is_valid()) {
		if (capture_view.is_null()) {
			capture_view.instantiate();
		}
		// Only one face renders at a time in the native ONCE/ALWAYS pipeline.
		if (render_view(p_data, capture_view, 0)) {
			current[0] = capture_view;
		}
		return;
	}
	for (uint32_t eye = 0; eye < MIN(p_data->scene_data->view_count, 2u); eye++) {
		const StringName scope = eye == 0 ? SNAME("aerial_perspective_0") : SNAME("aerial_perspective_1");
		Ref<View> view;
		if (p_data->render_buffers->has_custom_data(scope)) {
			view = p_data->render_buffers->get_custom_data(scope);
		}
		if (view.is_null()) {
			view.instantiate();
			p_data->render_buffers->set_custom_data(scope, view);
		}
		if (render_view(p_data, view, eye)) {
			current[eye] = view;
		}
	}
}
RID AerialPerspective::get_texture(int p_eye) const {
	if (p_eye >= 0 && p_eye < 2 && current[p_eye].is_valid() && current[p_eye]->state == "READY") {
		return current[p_eye]->volumes[current[p_eye]->front];
	}
	return TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_3D_BLACK);
}
Vector4 AerialPerspective::get_parameters() const {
	Vector4 result;
	for (int i = 0; i < 2; i++) {
		if (current[i].is_valid() && current[i]->state == "READY") {
			result[i] = 1;
		}
	}
	if (current[0].is_valid()) {
		const Environment *env = environments.getptr(current[0]->environment);
		if (env) {
			result.z = env->debug;
		}
	}
	return result;
}
Dictionary AerialPerspective::get_state(RID p_environment) const {
	const Environment *env = environments.getptr(p_environment);
	return env ? env->accepted.duplicate(true) : Dictionary();
}
Dictionary AerialPerspective::get_status(RID p_environment) const {
	Dictionary result = get_capabilities();
	const Environment *env = environments.getptr(p_environment);
	result["state"] = env ? env->state : String("DISABLED");
	result["reason"] = env ? env->reason : String();
	result["generation"] = env ? env->generation : int64_t(0);
	if (env && env->state == "FAILED") {
		result["error"] = "RENDER-AP-FAILED";
	}
	Array records;
	uint64_t bytes = 0, total_bytes = 0;
	HashSet<const Source *> sources;
	uint64_t source_bytes = 0;
	for (const KeyValue<RID, Environment> &entry : environments) {
		if (entry.value.source.is_valid() && !sources.has(entry.value.source.ptr())) {
			sources.insert(entry.value.source.ptr());
			source_bytes += entry.value.source->bytes;
		}
	}
	for (View *view : views) {
		uint64_t allocated = 0;
		for (RID texture : view->volumes) {
			if (texture.is_valid()) {
				allocated += VOLUME_BYTES;
			}
		}
		total_bytes += allocated;
		if (view->environment != p_environment) {
			continue;
		}
		bytes += allocated;
		Dictionary record;
		record["view_id"] = int64_t(view->get_instance_id());
		record["kind"] = view->capture_face < 0 ? "main" : "capture";
		record["eye"] = view->eye;
		record["capture_face"] = view->capture_face;
		record["probe"] = view->probe;
		record["environment_generation"] = view->generation;
		record["projection"] = view->projection;
		record["orthographic"] = view->orthographic;
		record["state"] = (env && env->enabled && view->revision != env->revision) ? String("PENDING") : view->state;
		record["reason"] = view->reason;
		record["bytes"] = int64_t(allocated);
		record["dispatches"] = int64_t(view->dispatches);
		record["origin"] = view->origin;
		record["texture"] = view->state == "READY" ? view->volumes[view->front] : RID();
		records.push_back(record);
	}
	result["views"] = records;
	result["bytes"] = int64_t(bytes);
	result["total_bytes"] = int64_t(total_bytes);
	result["source_bytes"] = int64_t(source_bytes);
	result["total_dispatches"] = int64_t(total_dispatches);
	result["source_copies"] = int64_t(total_source_copies);
	result["source_generation"] = env && env->source.is_valid() ? env->source->generation : int64_t(0);
	result["scene_depth_copies"] = int64_t(scene_depth_copies);
	result["scene_depth_copy_resolves"] = int64_t(scene_depth_copy_resolves);
	return result;
}
} // namespace RendererRD
