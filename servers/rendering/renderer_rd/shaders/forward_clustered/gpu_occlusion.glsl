#[compute]

#version 450

#VERSION_DEFINES

#ifdef USE_DEVICE_ADDRESSES
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
// A GLSL reference type needs its semicolon without an instance declarator.
// clang-format off
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TransformReference {
	vec4 data[];
};
// clang-format on
layout(set = 0, binding = 7, std430) readonly buffer TransformAddresses {
	uvec2 data[];
}
transform_addresses;
#endif

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D hierarchy;
layout(set = 0, binding = 1, std430) readonly buffer SourceTransforms {
	vec4 data[];
}
source;
layout(set = 0, binding = 2, std430) buffer VisibleIndices {
	uint data[];
}
visible;
layout(set = 0, binding = 3, std430) buffer PendingInstances {
	uint data[];
}
pending;
layout(set = 0, binding = 4, std430) buffer DrawCommands {
	uint data[];
}
commands;

struct BatchParameters {
	mat4 view_projection;
	vec4 bounds_min_margin;
	vec4 bounds_max_bias;
	uvec4 source_info; // count, vec4 stride, transform offset, candidate offset
	uvec4 draw_info; // command word offset, phase, test enabled, multimesh
};
layout(set = 0, binding = 5, std430) readonly buffer Parameters {
	BatchParameters data[];
}
parameters;
layout(set = 0, binding = 6, std430) readonly buffer Tasks {
	uvec2 data[];
}
tasks;
layout(push_constant, std430) uniform Dispatch {
	uvec4 task_range; // absolute first task, task count, reserved, reserved
}
dispatch;

BatchParameters params;
#ifdef USE_DEVICE_ADDRESSES
uvec2 source_address;
#endif

vec4 source_transform(uint offset) {
#ifdef USE_DEVICE_ADDRESSES
	if (any(notEqual(source_address, uvec2(0u)))) {
		return TransformReference(source_address).data[offset];
	}
#endif
	return source.data[offset];
}

void instance_bounds(uint id, out vec3 object_min, out vec3 object_max) {
	object_min = params.bounds_min_margin.xyz;
	object_max = params.bounds_max_bias.xyz;
	if (params.draw_info.w != 0u) {
		uint offset = (id + params.source_info.z) * params.source_info.y;
		mat4 transform = transpose(mat4(source_transform(offset), source_transform(offset + 1u), source_transform(offset + 2u), vec4(0.0, 0.0, 0.0, 1.0)));
		// Halve before adding/subtracting to avoid overflowing finite bounds.
		vec3 center = object_min * 0.5 + object_max * 0.5;
		vec3 half_size = abs(object_max * 0.5 - object_min * 0.5);
		vec3 absolute_x = abs(transform[0].xyz);
		vec3 absolute_y = abs(transform[1].xyz);
		vec3 absolute_z = abs(transform[2].xyz);
		vec3 transformed_center = (transform * vec4(center, 1.0)).xyz;
		vec3 transformed_half_size = absolute_x * half_size.x + absolute_y * half_size.y + absolute_z * half_size.z;
		// Account for cancellation and float rounding before testing occlusion.
		// The absolute basis also preserves non-uniform scale and shear.
		vec3 largest_point = max(abs(object_min), abs(object_max));
		vec3 magnitude = absolute_x * largest_point.x + absolute_y * largest_point.y + absolute_z * largest_point.z + abs(transform[3].xyz);
		vec3 rounding_margin = max(magnitude, vec3(1e-30)) * 1.9073486328125e-6; // 16 * float epsilon.
		object_min = transformed_center - transformed_half_size - rounding_margin;
		object_max = transformed_center + transformed_half_size + rounding_margin;
	}
	object_min -= vec3(params.bounds_min_margin.w);
	object_max += vec3(params.bounds_min_margin.w);
}

bool is_visible(uint id) {
	vec3 object_min;
	vec3 object_max;
	instance_bounds(id, object_min, object_max);
	if (any(isnan(object_min)) || any(isinf(object_min)) || any(isnan(object_max)) || any(isinf(object_max))) {
		return true;
	}
	vec2 rect_min = vec2(1.0);
	vec2 rect_max = vec2(-1.0);
	float nearest_depth = 0.0;
	for (uint corner = 0u; corner < 8u; ++corner) {
		vec3 point = mix(object_min, object_max, bvec3((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
		vec4 clip = params.view_projection * vec4(point, 1.0);
		// Bounds crossing the eye/near plane and non-finite inputs fail open.
		if (any(isnan(clip)) || any(isinf(clip)) || clip.w <= 0.0 || clip.z >= clip.w) {
			return true;
		}
		vec3 ndc = clip.xyz / clip.w;
		rect_min = min(rect_min, ndc.xy);
		rect_max = max(rect_max, ndc.xy);
		nearest_depth = max(nearest_depth, ndc.z);
	}
	// A history test is only a scheduling prediction. Current-frame recovery
	// repeats this test with the current camera before discarding anything.
	if (any(lessThan(rect_max, vec2(-1.0))) || any(greaterThan(rect_min, vec2(1.0)))) {
		return false;
	}
	if (params.draw_info.z == 0u) {
		return true;
	}
	vec2 base_size = vec2(textureSize(hierarchy, 0));
	vec2 uv_min = clamp(rect_min * 0.5 + 0.5 - 1.0 / base_size, vec2(0.0), vec2(1.0));
	vec2 uv_max = clamp(rect_max * 0.5 + 0.5 + 1.0 / base_size, vec2(0.0), vec2(1.0));
	float extent = max((uv_max.x - uv_min.x) * base_size.x, (uv_max.y - uv_min.y) * base_size.y);
	int level = clamp(int(ceil(log2(max(extent, 1.0)))), 0, textureQueryLevels(hierarchy) - 1);
	ivec2 size = textureSize(hierarchy, level);
	ivec2 lo = clamp(ivec2(floor(uv_min * vec2(size))), ivec2(0), size - 1);
	ivec2 hi = clamp(ivec2(floor(uv_max * vec2(size))), ivec2(0), size - 1);
	for (int y = lo.y; y <= hi.y; ++y) {
		for (int x = lo.x; x <= hi.x; ++x) {
			float farthest_occluder = texelFetch(hierarchy, ivec2(x, y), level).r;
			// Reverse Z: a zero/clear sample makes the entire block non-occluding.
			if (nearest_depth + params.bounds_max_bias.w >= farthest_occluder) {
				return true;
			}
		}
	}
	return false;
}

void main() {
	if (gl_WorkGroupID.x >= dispatch.task_range.y) {
		return;
	}
	uvec2 task = tasks.data[dispatch.task_range.x + gl_WorkGroupID.x];
	params = parameters.data[task.x];
#ifdef USE_DEVICE_ADDRESSES
	source_address = transform_addresses.data[task.x];
#endif
	uint id = task.y + gl_LocalInvocationIndex;
	uint count = params.source_info.x;
	if (id >= count) {
		return;
	}
	uint candidate = params.source_info.w + id;
	uint base = params.source_info.w * 3u;
	uint command = params.draw_info.x;
	if (params.draw_info.y == 0u) {
		bool draw = is_visible(id);
		pending.data[candidate] = draw ? 0u : 1u;
		if (draw) {
			uint index = atomicAdd(commands.data[command + 1u], 1u);
			visible.data[base + index] = id;
			uint color_index = atomicAdd(commands.data[command + 11u], 1u);
			visible.data[base + count * 2u + color_index] = id;
		}
	} else if (pending.data[candidate] != 0u && is_visible(id)) {
		uint index = atomicAdd(commands.data[command + 6u], 1u);
		visible.data[base + count + index] = id;
		uint color_index = atomicAdd(commands.data[command + 11u], 1u);
		visible.data[base + count * 2u + color_index] = id;
	}
}
