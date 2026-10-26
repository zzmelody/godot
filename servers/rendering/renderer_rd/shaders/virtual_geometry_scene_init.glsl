#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 2, std430) readonly buffer Transforms { vec4 data[]; } transforms;
layout(set = 0, binding = 4, std430) readonly buffer Work { uint data[]; } work;
#define VG_SCENE_WRITE
#include "virtual_geometry_scene_inc.glsl"
layout(set = 0, binding = 26, std430) buffer PreviousSources {
	uint data[];
}
previous_sources;
layout(set = 0, binding = 27, std430) readonly buffer BatchParameters { uint data[]; } batch_parameters;
struct SourceParameters {
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
	uint input_word;
};
SourceParameters source_info;

layout(set = 0, binding = 28, std430) buffer SourceTasks { uint data[]; } source_tasks;
layout(set = 0, binding = 29, std430) readonly buffer PreviousTransforms { vec4 data[]; } previous_transforms;
layout(push_constant, std430) uniform Phase {
	uint sources;
	uint mode;
	uint items;
	uint input_word;
} phase;

void vg_select_source(uint batch) {
	uint word = phase.input_word + batch * 66u + 44u;
	source_info.first = batch_parameters.data[word];
	source_info.material_map = batch_parameters.data[word + 1u];
	source_info.has_color = batch_parameters.data[word + 2u];
	source_info.has_custom = batch_parameters.data[word + 3u];
	source_info.shader_uniforms = batch_parameters.data[word + 4u];
	source_info.layers = batch_parameters.data[word + 5u];
	source_info.visible_base = batch_parameters.data[word + 6u];
	source_info.visible_capacity = batch_parameters.data[word + 7u];
	source_info.material_slots = batch_parameters.data[word + 8u];
	source_info.render_flags = batch_parameters.data[word + 9u];
	source_info.gi_offset = batch_parameters.data[word + 10u];
	source_info.work_word = batch_parameters.data[word + 11u];
	source_info.lightmap_uv_scale = uintBitsToFloat(uvec4(batch_parameters.data[word + 12u], batch_parameters.data[word + 13u], batch_parameters.data[word + 14u], batch_parameters.data[word + 15u]));
	source_info.identity = uvec4(batch_parameters.data[word + 16u], batch_parameters.data[word + 17u], batch_parameters.data[word + 18u], batch_parameters.data[word + 19u]);
	source_info.batch_word = batch_parameters.data[word + 20u];
	source_info.input_word = phase.input_word + batch * 66u;
}

uint vg_source_for_item(uint item, bool references) {
	uint lower = 0u;
	uint upper = phase.sources;
	while (lower < upper) {
		uint middle = lower + (upper - lower) / 2u;
		uint first = references ? source_tasks.data[16u + phase.sources + middle] : batch_parameters.data[phase.input_word + middle * 66u + 44u];
		if (first <= item) { lower = middle + 1u; }
		else { upper = middle; }
	}
	return lower == 0u ? 0u : lower - 1u;
}

mat4 vg_batch_model(uint offset) {
	uint word = source_info.input_word + offset;
	return mat4(uintBitsToFloat(uvec4(batch_parameters.data[word], batch_parameters.data[word + 1u], batch_parameters.data[word + 2u], batch_parameters.data[word + 3u])),
			uintBitsToFloat(uvec4(batch_parameters.data[word + 4u], batch_parameters.data[word + 5u], batch_parameters.data[word + 6u], batch_parameters.data[word + 7u])),
			uintBitsToFloat(uvec4(batch_parameters.data[word + 8u], batch_parameters.data[word + 9u], batch_parameters.data[word + 10u], batch_parameters.data[word + 11u])),
			uintBitsToFloat(uvec4(batch_parameters.data[word + 12u], batch_parameters.data[word + 13u], batch_parameters.data[word + 14u], batch_parameters.data[word + 15u])));
}

void vg_initialize_source(uint instance) {
	uint source_word = source_info.input_word + 16u;
	uvec4 source = uvec4(batch_parameters.data[source_word], batch_parameters.data[source_word + 1u], batch_parameters.data[source_word + 2u], batch_parameters.data[source_word + 3u]);
	uvec3 previous_info = uvec3(batch_parameters.data[source_info.input_word + 40u], batch_parameters.data[source_info.input_word + 41u], batch_parameters.data[source_info.input_word + 42u]);
	if (instance >= source.x || (work.data[source_info.work_word + 4u] & 16u) != 0u) { return; }
	uint selected_cut = (work.data[source_info.work_word + 4u] & 3u) != 0u ? 1u : 2u;
	if ((work.data[source_info.work_word + 16u + instance * 16u + 15u] & selected_cut) == 0u) { return; }
	mat4 local = mat4(1.0);
	vec4 color = vec4(1.0);
	vec4 custom = vec4(0.0);
	mat4 previous_local = mat4(1.0);
	vec4 previous_color = vec4(1.0);
	vec4 previous_custom = vec4(0.0);
	if (source.y != 0u) {
		uint word = instance * source.y + source.z;
		local = transpose(mat4(transforms.data[word], transforms.data[word + 1u], transforms.data[word + 2u], vec4(0.0, 0.0, 0.0, 1.0)));
		if (source_info.has_color != 0u) { color = transforms.data[word + 3u]; }
		if (source_info.has_custom != 0u) { custom = transforms.data[word + 3u + source_info.has_color]; }
		if (previous_info.y != 0u && previous_info.z != 0u) {
			previous_local = local;
			previous_color = color;
			previous_custom = custom;
		} else if (previous_info.y != 0u) {
			uint previous_word = instance * source.y + previous_info.x;
			previous_local = transpose(mat4(previous_transforms.data[previous_word], previous_transforms.data[previous_word + 1u], previous_transforms.data[previous_word + 2u], vec4(0.0, 0.0, 0.0, 1.0)));
			if (source_info.has_color != 0u) {
				previous_color = previous_transforms.data[previous_word + 3u];
			}
			if (source_info.has_custom != 0u) {
				previous_custom = previous_transforms.data[previous_word + 3u + source_info.has_color];
			}
		}
	}
	mat4 world = vg_batch_model(0u) * local;
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
	if (previous_info.y == 0u) {
		return;
	}
	mat4 previous_rows = transpose(vg_batch_model(24u) * previous_local);
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
	if (phase.mode == 3u) {
		if (index != 0u) { return; }
		uint count = source_tasks.data[0];
		uint groups = count / 64u + (count % 64u == 0u ? 0u : 1u);
		source_tasks.data[4u] = min(65535u, groups);
		source_tasks.data[5u] = max(1u, (groups + 65534u) / 65535u);
		source_tasks.data[6u] = 1u;
		return;
	}
	if (phase.mode == 0u) {
		if (index >= phase.sources) { return; }
		vg_select_source(index);
		uint word = source_info.batch_word;
		for (uint field = 0u; field < 20u; ++field) { scene.data[word + field] = 0u; }
		scene.data[word + 1u] = source_info.shader_uniforms;
		scene.data[word + 2u] = source_info.layers;
		scene.data[word + 3u] = source_info.material_map;
		scene.data[word + 4u] = batch_parameters.data[source_info.input_word + 20u];
		scene.data[word + 8u] = source_info.material_slots;
		scene.data[word + 9u] = source_info.render_flags;
		scene.data[word + 10u] = source_info.gi_offset;
		for (uint channel = 0u; channel < 4u; ++channel) {
			scene.data[word + 12u + channel] = floatBitsToUint(source_info.lightmap_uv_scale[channel]);
			scene.data[word + 16u + channel] = source_info.identity[channel];
		}
		uint flags = work.data[source_info.work_word + 4u];
		source_tasks.data[16u + index] = (flags & 16u) != 0u ? 0u : min(work.data[source_info.work_word + 7u], source_info.visible_capacity);
	} else if (phase.mode == 1u) {
		if (index >= phase.items) { return; }
		vg_select_source(vg_source_for_item(index, false));
		if (index >= source_info.first) { vg_initialize_source(index - source_info.first); }
	} else if (phase.mode == 2u) {
		if (index >= source_tasks.data[0]) { return; }
		uint batch = vg_source_for_item(index, true);
		vg_select_source(batch);
		uint local = index - source_tasks.data[16u + phase.sources + batch];
		uint selected = source_info.work_word + work.data[source_info.work_word + 11u] + local * 2u;
		uint word = scene.data[1] + index * 2u;
		scene.data[word] = source_info.first + work.data[selected];
		scene.data[word + 1u] = work.data[selected + 1u];
	}
}
