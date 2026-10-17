#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 2, std430) readonly buffer Transforms { vec4 data[]; } transforms;
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
#define VG_SCENE_WRITE
#include "virtual_geometry_scene_inc.glsl"
layout(push_constant, std430) uniform SourceParameters {
	uint first;
	uint material_map;
	uint has_color;
	uint has_custom;
	uint shader_uniforms;
	uint layers;
} source_info;

void main() {
	uint instance = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	if (instance >= params.source.x || (work.data[4] & 16u) != 0u) { return; }
	mat4 local = mat4(1.0);
	vec4 color = vec4(1.0);
	vec4 custom = vec4(0.0);
	if (params.source.y != 0u) {
		uint word = (instance + params.source.z) * params.source.y;
		local = transpose(mat4(transforms.data[word], transforms.data[word + 1u], transforms.data[word + 2u], vec4(0.0, 0.0, 0.0, 1.0)));
		if (source_info.has_color != 0u) { color = transforms.data[word + 3u]; }
		if (source_info.has_custom != 0u) { custom = transforms.data[word + 3u + source_info.has_color]; }
	}
	mat4 world = params.model * local;
	mat4 world_rows = transpose(world);
	mat3 normal_rows = inverse(mat3(world)); // Rows of inverse transpose.
	uint output_word = vg_source_word(source_info.first + instance);
	for (uint row = 0u; row < 3u; ++row) {
		for (uint column = 0u; column < 4u; ++column) {
			scene.data[output_word + row * 4u + column] = floatBitsToUint(world_rows[row][column]);
			scene.data[output_word + 12u + row * 4u + column] = work.data[16u + instance * 16u + row * 4u + column];
			scene.data[output_word + 24u + row * 4u + column] = column < 3u ? floatBitsToUint(normal_rows[row][column]) : 0u;
		}
	}
	// Reuse selection's determinant sign; normal-matrix XYZ ignores padding.
	scene.data[output_word + 35u] = work.data[16u + instance * 16u + 14u];
	for (uint channel = 0u; channel < 4u; ++channel) {
		scene.data[output_word + 36u + channel] = floatBitsToUint(color[channel]);
		scene.data[output_word + 40u + channel] = floatBitsToUint(custom[channel]);
	}
	scene.data[output_word + 44u] = instance;
	scene.data[output_word + 45u] = source_info.shader_uniforms;
	scene.data[output_word + 46u] = source_info.layers;
	scene.data[output_word + 47u] = source_info.material_map;
}
