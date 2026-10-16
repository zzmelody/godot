#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D hierarchy;
layout(set = 0, binding = 1, std430) readonly buffer SourceTransforms { vec4 data[]; } source;
layout(set = 0, binding = 2, std430) buffer VisibleIndices { uint data[]; } visible;
layout(set = 0, binding = 3, std430) buffer PendingInstances { uint data[]; } pending;
layout(set = 0, binding = 4, std430) buffer DrawCommands { uint data[]; } commands;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec4 bounds_min_margin;
	vec4 bounds_max_bias;
	uvec4 source_info; // count, vec4 stride, transform offset, candidate offset
	uvec4 draw_info; // command word offset, phase, test enabled, multimesh
} params;

bool is_visible(uint id) {
	mat4 transform = mat4(1.0);
	if (params.draw_info.w != 0u) {
		uint offset = (id + params.source_info.z) * params.source_info.y;
		transform = transpose(mat4(source.data[offset], source.data[offset + 1u], source.data[offset + 2u], vec4(0.0, 0.0, 0.0, 1.0)));
	}
	vec3 object_min = vec3(3.402823e38);
	vec3 object_max = vec3(-3.402823e38);
	for (uint corner = 0u; corner < 8u; ++corner) {
		vec3 point = mix(params.bounds_min_margin.xyz, params.bounds_max_bias.xyz, bvec3((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
		vec3 object_point = (transform * vec4(point, 1.0)).xyz;
		object_min = min(object_min, object_point);
		object_max = max(object_max, object_point);
	}
	object_min -= vec3(params.bounds_min_margin.w);
	object_max += vec3(params.bounds_min_margin.w);
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
	uint id = gl_GlobalInvocationID.x;
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
