#[vertex]

#version 450

#VERSION_DEFINES

void main() {
	vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(set = 0, binding = 0, std430) readonly buffer Visibility {
	uvec2 data[];
}
visibility;
layout(set = 0, binding = 1, std430) readonly buffer Shading {
	uint data[];
}
shading;
#ifdef VG_SHADOW_DEPTH
layout(push_constant, std430) uniform ShadowRegion {
	ivec2 size;
	ivec2 origin;
}
shadow_region;
#else
layout(set = 0, binding = 2) uniform usampler2D surface_flags;
#endif
#ifdef VG_MOTION
layout(set = 0, binding = 7) uniform sampler2D surface_motion;
layout(location = 0) out vec2 motion_vector;
#endif
#ifdef VG_NORMAL_ROUGHNESS
#include "virtual_geometry_scene_inc.glsl"
layout(set = 0, binding = 3) uniform sampler2D surface_normal;
layout(set = 0, binding = 4) uniform texture2D fitting_table;
layout(set = 0, binding = 5) uniform sampler fitting_sampler;
layout(location = 0) out vec4 normal_roughness;
#include "normal_fit_inc.glsl"
#endif
#ifdef VG_VOXEL_GI
layout(location = 1) out uvec2 voxel_gi;
#endif

layout(set = 0, binding = 6, std430) readonly buffer ViewStatus {
	uint data[];
}
vg_status;

void main() {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
#ifdef VG_SHADOW_DEPTH
	pixel -= shadow_region.origin;
	ivec2 size = shadow_region.size;
	if (vg_status.data[0] != 0u || shading.data[1] != 0u || any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, size))) {
		discard;
	}
#else
	ivec2 size = textureSize(surface_flags, 0);
	if (vg_status.data[0] != 0u || shading.data[1] != 0u || texelFetch(surface_flags, pixel, 0).r == 0u) {
		discard;
	}
#endif
	uvec2 visible = visibility.data[pixel.y * size.x + pixel.x];
	if (visible.x == 0u) {
		discard;
	}
	gl_FragDepth = uintBitsToFloat(visible.y);
#ifdef VG_MOTION
	motion_vector = texelFetch(surface_motion, pixel, 0).xy;
#endif
#ifdef VG_NORMAL_ROUGHNESS
	vec4 surface = texelFetch(surface_normal, pixel, 0);
	normal_roughness = vec4(encode24(normalize(surface.xyz), fitting_table, fitting_sampler) * 0.5 + 0.5, surface.w * (127.0 / 255.0));
	uint source = vg_visible_reference((visible.x >> 7u) - 1u).x;
	uint flags = scene.data[vg_source_word(source) + 53u];
	if ((flags & (1u << 3u)) != 0u) {
		normal_roughness.w = 1.0 - normal_roughness.w;
	}
#endif
#ifdef VG_VOXEL_GI
	uint probes = scene.data[vg_source_word(source) + 54u];
	voxel_gi = (flags & (1u << 10u)) != 0u ? uvec2(probes & 0xffu, (probes >> 16u) & 0xffu) : uvec2(0xffu);
#endif
}
