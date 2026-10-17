#[compute]

#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 1, std430) readonly buffer Geometry { uint data[]; } geometry;
layout(set = 0, binding = 6, std430) readonly buffer Visibility { uint64_t data[]; } visibility;
#define VG_MATERIAL_LOOKUP
#include "virtual_geometry_scene_inc.glsl"
layout(set = 0, binding = 9, std430) buffer Shading { uint data[]; } shading;
layout(push_constant, std430) uniform BinParameters {
	uint width;
	uint height;
	uint materials;
	uint entries_word;
	uint scatter;
} params;

void main() {
	uint quad = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	uint columns = (params.width + 1u) / 2u;
	if (quad >= columns * ((params.height + 1u) / 2u)) { return; }
	uvec2 top_left = uvec2(quad % columns, quad / columns) * 2u;
	uint payloads[4];
	for (uint pixel = 0u; pixel < 4u; ++pixel) {
		uvec2 point = top_left + uvec2(pixel & 1u, pixel >> 1u);
		payloads[pixel] = all(lessThan(point, uvec2(params.width, params.height))) ? uint(visibility.data[point.y * params.width + point.x]) : 0u;
	}
	for (uint pixel = 0u; pixel < 4u; ++pixel) {
		uint payload = payloads[pixel];
		if ((payload >> 7u) == 0u) { continue; }
		uint mask = 0u;
		for (uint lane = pixel; lane < 4u; ++lane) {
			if (payloads[lane] == payload) { mask |= 1u << lane; payloads[lane] = 0u; }
		}
		uint material = vg_material(payload);
		if (material >= params.materials) { atomicOr(shading.data[1], 1u); continue; }
		if (params.scatter == 0u) {
			atomicAdd(shading.data[2u + material], 1u);
		} else {
			uint position = atomicAdd(shading.data[2u + params.materials * 2u + material], 1u);
			uint count = shading.data[2u + material];
			if (position >= count || shading.data[0] > params.width * params.height) { atomicOr(shading.data[1], 2u); continue; }
			uint entry = shading.data[2u + params.materials + material] + position;
			uint word = params.entries_word + entry * 3u;
			shading.data[word] = top_left.y * params.width + top_left.x;
			shading.data[word + 1u] = payload;
			shading.data[word + 2u] = mask;
		}
	}
}
