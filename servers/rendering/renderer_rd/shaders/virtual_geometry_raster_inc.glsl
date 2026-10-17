// GPU-page random access and the current view's selected source instances.
layout(set = 0, binding = 1, std430) readonly buffer Geometry { uint data[]; } geometry;
layout(set = 0, binding = 3, std140) uniform Parameters {
	mat4 model;
	mat4 world_to_view;
	mat4 projection;
	vec4 lod;
	uvec4 source;
	uvec4 limits;
	uvec4 offsets;
} params;
layout(set = 0, binding = 4, std430) readonly buffer Work { uint data[]; } work;
#ifdef VG_BIN_BUILD
layout(set = 0, binding = 5, std430) buffer Bins { uint data[]; } bins;
#else
layout(set = 0, binding = 5, std430) readonly buffer Bins { uint data[]; } bins;
#endif
layout(push_constant, std430) uniform RasterParameters {
	uvec4 raster; // width, height, global visible-cluster base, bin capacity
	vec2 jitter;
	uint source_base;
	uint material;
} raster_info;

uint instance_base() { return 16u; }

#define VG_MATERIAL_LOOKUP
/* clang-format off */
#include "virtual_geometry_scene_inc.glsl"
#include "virtual_geometry_view_inc.glsl"
#include "virtual_geometry_page_inc.glsl"
#include "virtual_geometry_projection_inc.glsl"
#include "virtual_geometry_status_inc.glsl"
#include "virtual_geometry_raster_cull_inc.glsl"
/* clang-format on */

#ifdef VG_GLOBAL_RASTER
uint vg_raster_bucket() {
	return raster_info.material == 0xffffffffu ? 0u : raster_info.material + 1u;
}
uint vg_raster_bucket_count() {
	return work.data[2u + vg_raster_bucket()];
}
uint vg_raster_bucket_prefix() {
	return work.data[3u + scene.data[3] + vg_raster_bucket()];
}
uint vg_raster_bin_header() {
	return vg_raster_bucket() * 4u;
}
uint vg_raster_bin_base() {
	return raster_info.source_base + vg_raster_bucket_prefix() * 2u;
}
uint vg_raster_input(uint job) {
	return work.data[raster_info.raster.z + vg_raster_bucket_prefix() + job];
}
uint vg_raster_hardware(uint job) {
	return bins.data[vg_raster_bin_base() + job];
}
uint vg_raster_software(uint job) {
	return bins.data[vg_raster_bin_base() + vg_raster_bucket_count() + job];
}
#endif

uvec2 vg_selected(uint index) {
#ifdef VG_GLOBAL_RASTER
	return vg_visible_reference(index);
#else
	uint word = work.data[11] + index * 2u;
	return uvec2(work.data[word], work.data[word + 1u]);
#endif
}

uint vg_pixel_payload(uint index, uint triangle) {
#ifdef VG_GLOBAL_RASTER
	return ((index + 1u) << 7u) | triangle;
#else
	return ((raster_info.raster.z + index + 1u) << 7u) | triangle;
#endif
}
#ifdef VG_PROGRAMMABLE
#include "virtual_geometry_attributes_inc.glsl"
#define VG_GLOBALS_BINDING 14
#define VG_MATERIAL_INDEX raster_info.material
#include "virtual_geometry_material_vertex_inc.glsl"
#endif
vec3 vg_raster_position(uint instance, uint reference, uint vertex_index) {
#ifdef VG_PROGRAMMABLE
	vg_setup_vertex(
#ifdef VG_GLOBAL_RASTER
			instance
#else
			raster_info.source_base + instance
#endif
	);
	VGVertex value = vg_vertex(reference, vertex_index);
	vg_evaluate_vertex(value, vg_source, 0u);
	return value.position;
#else
	return vg_position(reference, vertex_index);
#endif
}
vec4 vg_project(uint instance, vec3 point) {
	vec3 view_point = (instance_transform(instance) * vec4(point, 1.0)).xyz;
	vec4 clip = vg_view_clip(view_point, params.projection, uint(params.lod.z), params.lod.x);
	clip.xy += raster_info.jitter * clip.w;
	return clip;
}

float vg_shadow_clip(uint instance, vec3 point) {
	uint flags = uint(params.lod.z);
	if ((flags & 4u) == 0u) {
		return -1.0;
	}
	float side = (flags & 8u) != 0u ? -1.0 : 1.0;
	return (instance_transform(instance) * vec4(point, 1.0)).z * side;
}
vec4 vg_clip(uint instance, uint reference, uint vertex_index) {
	return vg_project(instance, vg_raster_position(instance, reference, vertex_index));
}

vec2 vg_screen(vec4 clip) { return vg_screen_position(clip, vec2(raster_info.raster.xy)); }
