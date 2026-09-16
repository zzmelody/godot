#include "physical_sky_view.h"

#include "atmosphere_shader_library.h"
#include "sky_view_shader_library.h"

#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"

#include <cstring>

namespace RendererRD {
namespace {
constexpr uint64_t VIEW_BYTES = 192 * 104 * 8;
#ifdef DEBUG_ENABLED
// Explicit process-local failure seams enter the real allocation failure arms.
// No production API, validation exemption, or release-build behavior change.
bool physical_sky_test_failure(const char *p_point) {
	return OS::get_singleton()->get_environment("ANGE_TEST_PHYSICAL_SKY_FAILURE") == p_point;
}
#endif
} //namespace
PhysicalSkyView *PhysicalSkyView::singleton = nullptr;
PhysicalSkyView::PhysicalSkyView() {
	singleton = this;
}
PhysicalSkyView::~PhysicalSkyView() {
	finish();
	probes.clear();
	descriptors.clear();
	free_compute_resources();
	singleton = nullptr;
}
PhysicalSkyView::Source::~Source() {
	if (!RD::get_singleton()) {
		return;
	}
	for (RID texture : textures) {
		if (texture.is_valid()) {
			RD::get_singleton()->free(texture);
		}
	}
}
PhysicalSkyView::View::~View() {
	if (texture.is_valid() && RD::get_singleton()) {
		RD::get_singleton()->free(texture);
	}
}
String PhysicalSkyView::get_shader_source() {
	return String(SKY_VIEW_SHADER_DECLARATIONS) + ATMOSPHERE_SHADER_LIBRARY + SKY_VIEW_SHADER_BODY;
}
Dictionary PhysicalSkyView::get_capabilities() {
	Dictionary d;
	RD *rd = RD::get_singleton();
	bool supported = rd && OS::get_singleton()->get_current_rendering_method() == "mobile" && rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
	d["supported"] = supported;
	d["version"] = 1;
	d["renderer"] = OS::get_singleton()->get_current_rendering_method();
	d["reason"] = supported ? "" : "mobile_storage_rgba16f_required";
	d["width"] = 192;
	d["height"] = 104;
	d["view_bytes"] = VIEW_BYTES;
	d["native_probe_modes"] = true;
	return d;
}
void PhysicalSkyView::detach(RID p_sky) {
	Vector<RID> remove;
	for (const KeyValue<RID, Probe> &entry : probes) {
		if (entry.value.sky == p_sky) {
			remove.push_back(entry.key);
		}
	}
	for (RID probe : remove) {
		probes.erase(probe);
	}
	if (current.sky == p_sky) {
		finish();
	}
}
void PhysicalSkyView::free_sky(RID p_sky) {
	detach(p_sky);
	descriptors.erase(p_sky);
}
void PhysicalSkyView::free_probe(RID p_probe) {
	probes.erase(p_probe);
	// Atlas reallocation can release a slot after prepare. The active lease is
	// retained until the synchronous six-face job ends, then reattached.
}
void PhysicalSkyView::fail(RID p_sky, Descriptor &d, const String &p_reason) {
	if (d.state != "FAILED" || d.reason != p_reason) {
		ERR_PRINT("RENDER-PHYSICAL-SKY-FAILED: " + p_reason);
	}
	d.enabled = false;
	d.state = "FAILED";
	d.reason = p_reason;
	d.source.unref();
	detach(p_sky);
}
void PhysicalSkyView::set_source(RID p_sky, const Dictionary &p_source) {
	if (p_source.is_empty()) {
		free_sky(p_sky);
		return;
	}
	Descriptor &d = descriptors[p_sky];
	Variant generation = p_source.get("generation", Variant());
	if (generation.get_type() == Variant::INT && int64_t(generation) >= 0 && int64_t(generation) < d.generation) {
		return;
	}
	const String allowed[] = { "version", "generation", "source_generation", "enabled", "medium", "transmittance", "multiscatter", "fallback" };
	for (const Variant *key = p_source.next(nullptr); key; key = p_source.next(key)) {
		bool known = false;
		if (key->get_type() == Variant::STRING || key->get_type() == Variant::STRING_NAME) {
			for (const String &name : allowed) {
				known |= String(*key) == name;
			}
		}
		if (!known) {
			fail(p_sky, d, "unknown_parameter");
			return;
		}
	}
	Variant version = p_source.get("version", Variant()), enabled = p_source.get("enabled", Variant());
	if (version.get_type() != Variant::INT || int64_t(version) != 1 || generation.get_type() != Variant::INT || int64_t(generation) < 0 || enabled.get_type() != Variant::BOOL) {
		fail(p_sky, d, "invalid_contract");
		return;
	}
	d.generation = generation;
	if (!bool(enabled)) {
		detach(p_sky);
		d.source.unref();
		d.enabled = false;
		d.state = "DISABLED";
		d.reason = String();
		return;
	}
	if (!bool(get_capabilities()["supported"])) {
		fail(p_sky, d, "unsupported_renderer_or_format");
		d.state = "UNSUPPORTED";
		return;
	}
	Variant packed = p_source.get("medium", Variant()), fallback = p_source.get("fallback", Variant());
	if (packed.get_type() != Variant::PACKED_FLOAT32_ARRAY || PackedFloat32Array(packed).size() != 32 || fallback.get_type() != Variant::COLOR) {
		fail(p_sky, d, "invalid_parameters");
		return;
	}
	PackedFloat32Array medium = packed;
	for (float f : medium) {
		if (!Math::is_finite(f)) {
			fail(p_sky, d, "nonfinite_medium");
			return;
		}
	}
	const int positive[] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20, 21, 22, 23, 24, 25, 30, 31 };
	for (int i : positive) {
		if (medium[i] < 0) {
			fail(p_sky, d, "negative_medium");
			return;
		}
	}
	Color color = fallback;
	if (medium[0] <= 0 || medium[1] <= 0 || medium[27] < 4 || medium[27] > 128 || (!Math::is_finite(color.r) || !Math::is_finite(color.g) || !Math::is_finite(color.b) || !Math::is_finite(color.a)) || color.r < 0 || color.g < 0 || color.b < 0) {
		fail(p_sky, d, "invalid_medium_or_fallback");
		return;
	}
	medium.set(26, 0.0f); // Camera altitude never changes the physical source identity.
	Variant sg = p_source.get("source_generation", generation);
	if (sg.get_type() != Variant::INT || int64_t(sg) < 0) {
		fail(p_sky, d, "invalid_source_generation");
		return;
	}
	RID originals[2];
	const char *names[] = { "transmittance", "multiscatter" };
	for (int i = 0; i < 2; i++) {
		Variant v = p_source.get(names[i], Variant());
		if (v.get_type() != Variant::RID || !RID(v).is_valid()) {
			fail(p_sky, d, "invalid_source_texture");
			return;
		}
		originals[i] = v;
	}
	Ref<Source> source;
	for (const KeyValue<RID, Descriptor> &entry : descriptors) {
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
				fail(p_sky, d, "source_texture_not_ready");
				return;
			}
			RD::TextureFormat f = rd->texture_get_format(original);
			if (f.texture_type != RD::TEXTURE_TYPE_2D || f.array_layers != 1 || f.samples != RD::TEXTURE_SAMPLES_1 || !(f.usage_bits & RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT) || (f.format != RD::DATA_FORMAT_R16G16B16A16_SFLOAT && f.format != RD::DATA_FORMAT_R32G32B32A32_SFLOAT)) {
				fail(p_sky, d, "source_texture_format_or_copy_usage");
				return;
			}
			f.mipmaps = 1;
			f.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
#ifdef DEBUG_ENABLED
			source->textures[i] = physical_sky_test_failure("source") ? RID() : rd->texture_create(f, RD::TextureView());
#else
			source->textures[i] = rd->texture_create(f, RD::TextureView());
#endif
			if (!source->textures[i].is_valid() || rd->texture_copy(original, source->textures[i], Vector3(), Vector3(), Vector3(f.width, f.height, 1), 0, 0, 0, 0) != OK) {
				fail(p_sky, d, "source_copy_failed");
				return;
			}
			source->originals[i] = originals[i];
			source->bytes += uint64_t(f.width) * f.height * (f.format == RD::DATA_FORMAT_R16G16B16A16_SFLOAT ? 8 : 16);
			++source_copies;
		}
	}
	bool unchanged = d.enabled && d.source == source && memcmp(d.medium, medium.ptr(), sizeof(d.medium)) == 0;
	// Drop superseded per-probe views immediately; old native reflection atlas
	// pixels remain complete until the native scheduler captures again.
	if (!unchanged) {
		detach(p_sky);
		d.revision = ++revision;
	}
	d.source = source;
	memcpy(d.medium, medium.ptr(), sizeof(d.medium));
	d.enabled = true;
	d.state = "READY";
	d.reason = String();
}
void PhysicalSkyView::free_compute_resources() {
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
	pipeline = RID();
	shader = RID();
	sampler = RID();
}
bool PhysicalSkyView::initialize() {
	if (shader.is_valid() && pipeline.is_valid() && sampler.is_valid()) {
		return true;
	}
	// FAILED descriptors do not call initialize again. A validated re-submission
	// explicitly retries, including after a partial pipeline/sampler failure.
	free_compute_resources();
	shader_error = String();
	RD *rd = RD::get_singleton();
	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	stage.spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_COMPUTE, get_shader_source(), RD::SHADER_LANGUAGE_GLSL, &shader_error);
	if (stage.spirv.is_empty()) {
		shader_error = "sky_view_shader_compilation_failed: " + shader_error;
		return false;
	}
	Vector<RD::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	shader = rd->shader_create_from_spirv(stages, "Native physical probe SkyView");
	if (!shader.is_valid()) {
		shader_error = "shader_creation_failed";
		return false;
	}
	pipeline = rd->compute_pipeline_create(shader);
	RD::SamplerState s;
	s.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	s.min_filter = RD::SAMPLER_FILTER_LINEAR;
	s.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	s.repeat_v = s.repeat_u;
#ifdef DEBUG_ENABLED
	sampler = physical_sky_test_failure("sampler") ? RID() : rd->sampler_create(s);
#else
	sampler = rd->sampler_create(s);
#endif
	if (!pipeline.is_valid() || !sampler.is_valid()) {
		shader_error = "pipeline_or_sampler_allocation_failed";
		free_compute_resources();
		return false;
	}
	return true;
}
bool PhysicalSkyView::prepare(RID p_sky, RID p_probe, const Vector3 &p_origin) {
	finish();
	if (OS::get_singleton()->get_current_rendering_method() != "mobile") {
		return true; // The provider never changes native capture on unsupported renderers.
	}
	Descriptor *d = descriptors.getptr(p_sky);
	if (!d || d->state == "DISABLED") {
		free_probe(p_probe);
		return true;
	}
	if (!d->enabled || d->source.is_null()) {
		return false;
	}
	if (!p_origin.is_finite()) {
		fail(p_sky, *d, "invalid_probe_origin");
		return false;
	}
	if (!initialize()) {
		fail(p_sky, *d, shader_error);
		return false;
	}
	float medium[32];
	memcpy(medium, d->medium, sizeof(medium));
	medium[26] = CLAMP(float(p_origin.y * 0.001), 0.0f, medium[1] * 0.999f);
	Ref<View> view;
	for (const KeyValue<RID, Probe> &entry : probes) {
		const Ref<View> &candidate = entry.value.view;
		if (candidate.is_valid() && candidate->source == d->source && memcmp(candidate->medium, medium, sizeof(medium)) == 0) {
			view = candidate;
			break;
		}
	}
	if (view.is_null()) {
		view.instantiate();
		view->source = d->source;
		memcpy(view->medium, medium, sizeof(medium));
		RD *rd = RD::get_singleton();
		RD::TextureFormat f;
		f.texture_type = RD::TEXTURE_TYPE_2D;
		f.width = 192;
		f.height = 104;
		f.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		f.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
#ifdef DEBUG_ENABLED
		view->texture = physical_sky_test_failure("view") ? RID() : rd->texture_create(f, RD::TextureView());
#else
		view->texture = rd->texture_create(f, RD::TextureView());
#endif
		if (!view->texture.is_valid()) {
			fail(p_sky, *d, "view_allocation_failed");
			return false;
		}
		RD::Uniform u[3];
		for (int i = 0; i < 2; i++) {
			u[i].binding = i;
			u[i].uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u[i].append_id(sampler);
			u[i].append_id(d->source->textures[i]);
		}
		u[2].binding = 2;
		u[2].uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u[2].append_id(view->texture);
		RID set;
#ifdef DEBUG_ENABLED
		if (!physical_sky_test_failure("uniform"))
#endif
		{
			set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u[0], u[1], u[2]);
		}
		if (!set.is_valid()) {
			fail(p_sky, *d, "view_uniform_set_failed");
			return false;
		}
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_set_push_constant(list, medium, sizeof(medium));
		rd->compute_list_dispatch(list, 24, 13, 1);
		rd->compute_list_end();
		// Immutable output: RD orders this storage write before the six faces'
		// sampled reads. No source or output is overwritten while a face uses it.
		++dispatches;
	}
	current_probe = p_probe;
	current.sky = p_sky;
	current.view = view;
	current.origin = p_origin;
	current.generation = d->generation;
	// Only finish(true) admits the view to the retained cache. A failed native
	// atlas begin releases this temporary lease without retaining a new entry.
	return true;
}
void PhysicalSkyView::finish(bool p_completed) {
	if (p_completed && current_probe.is_valid() && current.view.is_valid()) {
		const Descriptor *d = descriptors.getptr(current.sky);
		if (d && d->enabled && d->generation == current.generation && d->source == current.view->source) {
			probes[current_probe] = current;
		}
	}
	current_probe = RID();
	current = Probe();
}
RID PhysicalSkyView::get_texture(RID p_probe) const {
	return p_probe.is_valid() && current_probe == p_probe && current.view.is_valid() ? current.view->texture : RID();
}
Vector4 PhysicalSkyView::get_parameters(RID p_probe) const {
	if (p_probe.is_valid() && current_probe == p_probe && current.view.is_valid()) {
		return Vector4(1, 1, current.view->medium[26], 0);
	}
	return Vector4();
}
Dictionary PhysicalSkyView::get_status(RID p_sky) const {
	Dictionary result;
	const Descriptor *d = descriptors.getptr(p_sky);
	result["state"] = d ? d->state : String("DISABLED");
	result["reason"] = d ? d->reason : String();
	result["generation"] = d ? d->generation : 0;
	result["source_generation"] = d && d->source.is_valid() ? d->source->generation : 0;
	result["source_copies"] = source_copies;
	result["dispatches"] = dispatches;
	HashSet<const Source *> sources;
	HashSet<const View *> views;
	uint64_t source_bytes = 0;
	Array records;
	for (const KeyValue<RID, Descriptor> &entry : descriptors) {
		if (entry.value.source.is_valid() && !sources.has(entry.value.source.ptr())) {
			sources.insert(entry.value.source.ptr());
			source_bytes += entry.value.source->bytes;
		}
	}
	for (const KeyValue<RID, Probe> &entry : probes) {
		const Probe &p = entry.value;
		if (p.view.is_valid()) {
			views.insert(p.view.ptr());
		}
		if (p.sky != p_sky) {
			continue;
		}
		Dictionary r;
		r["probe"] = entry.key;
		r["origin"] = p.origin;
		r["generation"] = p.generation;
		r["altitude_km"] = p.view.is_valid() ? p.view->medium[26] : 0.0f;
		r["state"] = "READY";
		records.push_back(r);
	}
	result["source_bytes"] = source_bytes;
	result["view_bytes"] = uint64_t(views.size()) * VIEW_BYTES;
	result["view_count"] = views.size();
	result["probe_count"] = probes.size();
	result["probe_views"] = records;
	return result;
}
} // namespace RendererRD
