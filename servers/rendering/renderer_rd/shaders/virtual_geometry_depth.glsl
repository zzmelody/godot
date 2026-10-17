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
layout(set = 0, binding = 2) uniform usampler2D surface_flags;
#ifdef VG_NORMAL_ROUGHNESS
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
	ivec2 size = textureSize(surface_flags, 0);
	if (vg_status.data[0] != 0u || shading.data[1] != 0u || texelFetch(surface_flags, pixel, 0).r == 0u) {
		discard;
	}
	uvec2 visible = visibility.data[pixel.y * size.x + pixel.x];
	if (visible.x == 0u) {
		discard;
	}
	gl_FragDepth = uintBitsToFloat(visible.y);
#ifdef VG_NORMAL_ROUGHNESS
	vec4 surface = texelFetch(surface_normal, pixel, 0);
	normal_roughness = vec4(encode24(normalize(surface.xyz), fitting_table, fitting_sampler) * 0.5 + 0.5, surface.w * (127.0 / 255.0));
#endif
#ifdef VG_VOXEL_GI
	voxel_gi = uvec2(0xffu);
#endif
}
