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
} raster_info;

uint instance_base() { return 16u; }

#include "virtual_geometry_view_inc.glsl"
#include "virtual_geometry_page_inc.glsl"

uvec2 vg_selected(uint index) {
	uint word = work.data[11] + index * 2u;
	return uvec2(work.data[word], work.data[word + 1u]);
}

vec4 vg_clip(uint instance, uint reference, uint vertex) {
	vec4 clip = params.projection * instance_transform(instance) * vec4(vg_position(reference, vertex), 1.0);
	clip.xy += raster_info.jitter * clip.w;
	return clip;
}

vec2 vg_screen(vec4 clip) { return vg_screen_position(clip, vec2(raster_info.raster.xy)); }
