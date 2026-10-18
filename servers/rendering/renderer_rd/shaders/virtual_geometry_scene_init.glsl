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
layout(set = 0, binding = 26, std430) buffer PreviousSources {
	uint data[];
}
previous_sources;
layout(set = 0, binding = 27, std140) uniform PreviousBatch {
	mat4 model;
	uvec4 info; // Previous transform offset, history enabled, reserved[2].
}
previous_batch;
layout(push_constant, std430) uniform SourceParameters {
	uint first;
	uint material_map;
	uint has_color;
	uint has_custom;
	uint shader_uniforms;
	uint layers;
	uint visible_base;
	uint visible_capacity;
	uint material_slots;
	uint render_flags;
	uint gi_offset;
	uint work_word;
	vec4 lightmap_uv_scale;
	uvec4 identity;
	uint batch_word;
} source_info;

void vg_initialize_source(uint instance) {
	if (instance >= params.source.x || (work.data[source_info.work_word + 4u] & 16u) != 0u) { return; }
	uint selected_cut = (work.data[source_info.work_word + 4u] & 3u) != 0u ? 1u : 2u;
	if ((work.data[source_info.work_word + 16u + instance * 16u + 15u] & selected_cut) == 0u) { return; }
	mat4 local = mat4(1.0);
	vec4 color = vec4(1.0);
	vec4 custom = vec4(0.0);
	mat4 previous_local = mat4(1.0);
	vec4 previous_color = vec4(1.0);
	vec4 previous_custom = vec4(0.0);
	if (params.source.y != 0u) {
		uint word = (instance + params.source.z) * params.source.y;
		local = transpose(mat4(transforms.data[word], transforms.data[word + 1u], transforms.data[word + 2u], vec4(0.0, 0.0, 0.0, 1.0)));
		if (source_info.has_color != 0u) { color = transforms.data[word + 3u]; }
		if (source_info.has_custom != 0u) { custom = transforms.data[word + 3u + source_info.has_color]; }
		if (previous_batch.info.y != 0u) {
			uint previous_word = (instance + previous_batch.info.x) * params.source.y;
			previous_local = transpose(mat4(transforms.data[previous_word], transforms.data[previous_word + 1u], transforms.data[previous_word + 2u], vec4(0.0, 0.0, 0.0, 1.0)));
			if (source_info.has_color != 0u) {
				previous_color = transforms.data[previous_word + 3u];
			}
			if (source_info.has_custom != 0u) {
				previous_custom = transforms.data[previous_word + 3u + source_info.has_color];
			}
		}
	}
	mat4 world = params.model * local;
	mat4 world_rows = transpose(world);
	mat3 normal_rows = inverse(mat3(world)); // Rows of inverse transpose.
	uint output_word = vg_source_word(source_info.first + instance);
	for (uint row = 0u; row < 3u; ++row) {
		for (uint column = 0u; column < 4u; ++column) {
			scene.data[output_word + row * 4u + column] = floatBitsToUint(world_rows[row][column]);
			scene.data[output_word + 12u + row * 4u + column] = work.data[source_info.work_word + 16u + instance * 16u + row * 4u + column];
			scene.data[output_word + 24u + row * 4u + column] = column < 3u ? floatBitsToUint(normal_rows[row][column]) : 0u;
		}
	}
	// Reuse selection's determinant sign; normal-matrix XYZ ignores padding.
	scene.data[output_word + 35u] = work.data[source_info.work_word + 16u + instance * 16u + 14u];
	for (uint channel = 0u; channel < 4u; ++channel) {
		scene.data[output_word + 36u + channel] = floatBitsToUint(color[channel]);
		scene.data[output_word + 40u + channel] = floatBitsToUint(custom[channel]);
	}
	scene.data[output_word + 44u] = source_info.batch_word;
	scene.data[output_word + 45u] = instance;
	scene.data[output_word + 46u] = work.data[source_info.work_word + 16u + instance * 16u + 12u];
	scene.data[output_word + 47u] = work.data[source_info.work_word + 16u + instance * 16u + 13u];
	if (previous_batch.info.y == 0u) {
		return;
	}
	mat4 previous_rows = transpose(previous_batch.model * previous_local);
	uint history_word = (source_info.first + instance) * 24u;
	for (uint row = 0u; row < 3u; ++row) {
		for (uint column = 0u; column < 4u; ++column) {
			previous_sources.data[history_word + row * 4u + column] = floatBitsToUint(previous_rows[row][column]);
		}
	}
	for (uint channel = 0u; channel < 4u; ++channel) {
		previous_sources.data[history_word + 12u + channel] = floatBitsToUint(previous_color[channel]);
		previous_sources.data[history_word + 16u + channel] = floatBitsToUint(previous_custom[channel]);
		previous_sources.data[history_word + 20u + channel] = 0u;
	}
}

void main() {
	uint index = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	if (index == 0u) {
		uint word = source_info.batch_word;
		for (uint field = 0u; field < 20u; ++field) { scene.data[word + field] = 0u; }
		scene.data[word + 1u] = source_info.shader_uniforms;
		scene.data[word + 2u] = source_info.layers;
		scene.data[word + 3u] = source_info.material_map;
		scene.data[word + 4u] = params.offsets.w;
		scene.data[word + 8u] = source_info.material_slots;
		scene.data[word + 9u] = source_info.render_flags;
		scene.data[word + 10u] = source_info.gi_offset;
		for (uint channel = 0u; channel < 4u; ++channel) {
			scene.data[word + 12u + channel] = floatBitsToUint(source_info.lightmap_uv_scale[channel]);
			scene.data[word + 16u + channel] = source_info.identity[channel];
		}
	}
	vg_initialize_source(index);
	if (index < source_info.visible_capacity) {
		uint word = scene.data[1] + (source_info.visible_base + index) * 2u;
		if ((work.data[source_info.work_word + 4u] & 16u) == 0u && index < min(work.data[source_info.work_word + 7u], source_info.visible_capacity)) {
			uint selected = source_info.work_word + work.data[source_info.work_word + 11u] + index * 2u;
			scene.data[word] = source_info.first + work.data[selected];
			scene.data[word + 1u] = work.data[selected + 1u];
		} else {
			scene.data[word] = 0xffffffffu;
			scene.data[word + 1u] = 0xffffffffu;
		}
	}
}
