#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer Metadata { uint data[]; } metadata;
layout(set = 0, binding = 1, std430) readonly buffer Geometry { uint data[]; } geometry;
layout(set = 0, binding = 2, std430) readonly buffer Transforms { vec4 data[]; } transforms;
layout(set = 0, binding = 3, std140) uniform Parameters {
	mat4 model;
	mat4 world_to_view;
	mat4 projection;
	vec4 lod; // near plane, pixels per unit error, orthographic, frustum margin
	uvec4 source; // instance count, vec4 stride (zero for Mesh), transform offset, root pages
	uvec4 limits; // node frontier, candidate/selected clusters, root clusters, requests
	uvec4 offsets; // parts, page table, groups (word offsets), unused
} params;
layout(set = 0, binding = 4, std430) buffer Work { uint data[]; } work;
layout(push_constant, std430) uniform Phase { uint mode; uint round; } phase;

// Header: frontier counts[2], candidates, fine count, failure bits, requests,
// root count, selected count, dispatch[3], selected word offset. Remaining
// header words are zero. Queues carry (source instance, node/physical cluster).
const uint HEADER_WORDS = 16u;
const uint INVALID = 0xffffffffu;

uint instance_base() { return HEADER_WORDS; }
uint queue_base(uint parity) { return instance_base() + params.source.x * 16u + parity * params.limits.x * 2u; }
uint candidate_base() { return queue_base(2u); }
uint fine_base() { return candidate_base() + params.limits.y * 2u; }
uint root_base() { return fine_base() + params.limits.y * 2u; }
uint request_base() { return root_base() + params.limits.z * 2u; }

#include "virtual_geometry_view_inc.glsl"

float norm_bound(mat3 value) {
	mat3 a = mat3(abs(value[0]), abs(value[1]), abs(value[2]));
	vec3 rows = a[0] + a[1] + a[2];
	vec3 columns = vec3(dot(a[0], vec3(1.0)), dot(a[1], vec3(1.0)), dot(a[2], vec3(1.0)));
	return sqrt(max(max(rows.x, rows.y), rows.z) * max(max(columns.x, columns.y), columns.z));
}

void initialize_instance(uint id) {
	mat4 local = mat4(1.0);
	if (params.source.y != 0u) {
		uint offset = (id + params.source.z) * params.source.y;
		local = transpose(mat4(transforms.data[offset], transforms.data[offset + 1u], transforms.data[offset + 2u], vec4(0.0, 0.0, 0.0, 1.0)));
	}
	mat4 world = params.model * local;
	mat3 basis = mat3(world);
	float det = determinant(basis);
	bool valid = !isnan(det) && !isinf(det) && det != 0.0;
	for (uint column = 0u; column < 4u; ++column) {
		valid = valid && !any(isnan(world[column])) && !any(isinf(world[column]));
	}
	if (!valid) {
		atomicOr(work.data[4], 16u);
		return;
	}
	vec3 diagonal = vec3(dot(basis[0], basis[0]), dot(basis[1], basis[1]), dot(basis[2], basis[2]));
	vec3 off_diagonal = abs(vec3(dot(basis[0], basis[1]), dot(basis[0], basis[2]), dot(basis[1], basis[2])));
	vec3 radii = vec3(off_diagonal.x + off_diagonal.y, off_diagonal.x + off_diagonal.z, off_diagonal.y + off_diagonal.z);
	// Gershgorin bounds on M^T*M reduce to UE's min/max scale for an
	// orthogonal basis. Shear retains every column and a conservative sphere.
	vec3 lower = diagonal - radii;
	vec3 upper = diagonal + radii;
	float minimum_squared = min(min(lower.x, lower.y), lower.z);
	float minimum = minimum_squared > 0.0 ? sqrt(minimum_squared) : 1.0 / norm_bound(inverse(basis));
	float maximum = sqrt(max(max(upper.x, upper.y), upper.z));
	mat4 rows = transpose(params.world_to_view * world);
	uint base = instance_base() + id * 16u;
	for (uint row = 0u; row < 3u; ++row) {
		for (uint column = 0u; column < 4u; ++column) {
			work.data[base + row * 4u + column] = floatBitsToUint(rows[row][column]);
		}
	}
	work.data[base + 12u] = floatBitsToUint(minimum);
	work.data[base + 13u] = floatBitsToUint(maximum);
	work.data[base + 14u] = floatBitsToUint(sign(det));
	work.data[base + 15u] = 0u;
	if (id < params.limits.x) {
		work.data[queue_base(0u) + id * 2u] = id;
		work.data[queue_base(0u) + id * 2u + 1u] = 0u;
		atomicAdd(work.data[0], 1u);
	} else {
		atomicOr(work.data[4], 1u);
	}
}

bool in_frustum(vec3 lower, vec3 upper, mat4 transform) {
	if ((uint(params.lod.z) & 4u) != 0u) {
		// The existing scene culler owns the hemisphere caster set. Linear clip
		// intervals cannot reject a nonlinear paraboloid projection safely.
		return true;
	}
	// Expand each clip-plane interval by a world-axis displacement cube. This
	// preserves all instance columns and needs no inverse for sheared instances.
	vec4 displacement = vec4(0.0);
	float maximum = uintBitsToFloat(params.offsets.w);
	if (maximum > 0.0) {
		mat4 world_to_clip = params.projection * params.world_to_view;
		displacement = maximum * (abs(world_to_clip[0]) + abs(world_to_clip[1]) + abs(world_to_clip[2]));
	}
	vec2 side_padding = displacement.xy + vec2((1.0 + params.lod.w) * displacement.w);
	uint outside = 63u;
	for (uint corner = 0u; corner < 8u; ++corner) {
		vec3 point = mix(lower, upper, bvec3((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
		vec4 clip = params.projection * transform * vec4(point, 1.0);
		float extent = (1.0 + params.lod.w) * clip.w;
		uint mask = (clip.x < -extent - side_padding.x ? 1u : 0u) | (clip.x > extent + side_padding.x ? 2u : 0u) |
				(clip.y < -extent - side_padding.y ? 4u : 0u) | (clip.y > extent + side_padding.y ? 8u : 0u) |
				(clip.z < -displacement.z ? 16u : 0u) | ((uint(params.lod.z) & 2u) == 0u && clip.z > clip.w + displacement.z + displacement.w ? 32u : 0u);
		outside &= mask;
	}
	return outside == 0u;
}

vec3 meta3(uint word) { return uintBitsToFloat(uvec3(metadata.data[word], metadata.data[word + 1u], metadata.data[word + 2u])); }
vec4 meta4(uint word) { return vec4(meta3(word), uintBitsToFloat(metadata.data[word + 3u])); }
vec3 geom3(uint word) { return uintBitsToFloat(uvec3(geometry.data[word], geometry.data[word + 1u], geometry.data[word + 2u])); }
vec4 geom4(uint word) { return vec4(geom3(word), uintBitsToFloat(geometry.data[word + 3u])); }

void visit_node(uint id) {
	uint parity = phase.round & 1u;
	if (id >= min(work.data[parity], params.limits.x) || (atomicOr(work.data[4], 0u) & 16u) != 0u) {
		return;
	}
	uint task = queue_base(parity) + id * 2u;
	uint instance = work.data[task];
	uint node = work.data[task + 1u];
	mat4 transform = instance_transform(instance);
	float error_scale = params.lod.y * uintBitsToFloat(work.data[instance_base() + instance * 16u + 12u]);
	for (uint child = 0u; child < 4u; ++child) {
		uint branch = 52u + node * 64u + child * 16u;
		uint child_node = metadata.data[branch + 12u];
		uint part = metadata.data[branch + 13u];
		if (child_node == INVALID && part == INVALID) { continue; }
		float edge = projected_edge(instance, meta4(branch + 6u), transform);
		float threshold = error_scale * uintBitsToFloat(metadata.data[branch + 11u]);
		bool root_path = metadata.data[branch + 15u] != 0u;
		if ((!root_path && edge > threshold) || !in_frustum(meta3(branch), meta3(branch + 3u), transform)) { continue; }
		if (child_node != INVALID) {
			uint index = atomicAdd(work.data[1u - parity], 1u);
			if (index < params.limits.x) {
				uint output_word = queue_base(1u - parity) + index * 2u;
				work.data[output_word] = instance;
				work.data[output_word + 1u] = child_node;
			} else { atomicOr(work.data[4], 1u); }
			continue;
		}
		uint reference = metadata.data[branch + 14u];
		uint part_word = params.offsets.x + part * 8u;
		if (reference == INVALID) {
			uint group_word = params.offsets.z + metadata.data[part_word] * 2u;
			uint first = metadata.data[group_word];
			uint count = metadata.data[group_word + 1u];
			for (uint page = first; page < first + count; ++page) {
				if (metadata.data[params.offsets.y + page] != INVALID) { continue; }
				uint index = atomicAdd(work.data[5], 1u);
				if (index < params.limits.w) {
					work.data[request_base() + index * 2u] = page;
					work.data[request_base() + index * 2u + 1u] = floatBitsToUint(threshold / max(edge, 1e-20));
				} else { atomicOr(work.data[4], 4u); }
			}
			continue;
		}
		uint count = metadata.data[part_word + 3u];
		// Bounded selected-page use feedback drives host eviction order. Missing
		// requests retain a separate queue and cannot be displaced by these hits.
		uint group_word = params.offsets.z + metadata.data[part_word] * 2u;
		uint page_first = metadata.data[group_word];
		uint page_count = metadata.data[group_word + 1u];
		for (uint page = page_first; page < page_first + page_count; ++page) {
			uint hit = atomicAdd(work.data[12], 1u);
			if (hit < 128u) {
				uint used_word = request_base() + params.limits.w * 2u + hit * 2u;
				work.data[used_word] = page;
				work.data[used_word + 1u] = floatBitsToUint(-1.0);
			}
		}
		uint first = atomicAdd(work.data[2], count);
		if (first > params.limits.y || count > params.limits.y - first) {
			atomicOr(work.data[4], 2u);
			continue;
		}
		for (uint cluster = 0u; cluster < count; ++cluster) {
			work.data[candidate_base() + (first + cluster) * 2u] = instance;
			work.data[candidate_base() + (first + cluster) * 2u + 1u] = reference + cluster;
		}
	}
}

void select_cluster(uint instance, uint reference, bool root_only) {
	uint record = (reference >> 8u) * 8192u + 8u + (reference & 255u) * 72u;
	uint flags = geometry.data[record + 7u];
	if (root_only && (flags & 9u) != 9u) { return; }
	mat4 transform = instance_transform(instance);
	if (!in_frustum(geom3(record + 18u), geom3(record + 21u), transform)) { return; }
	if (!root_only) {
		float edge = projected_edge(instance, geom4(record + 12u), transform);
		float error_scale = params.lod.y * uintBitsToFloat(work.data[instance_base() + instance * 16u + 12u]);
		if (edge <= error_scale * uintBitsToFloat(geometry.data[record + 16u]) && (flags & 2u) == 0u) { return; }
	}
	uint index = atomicAdd(work.data[root_only ? 6u : 3u], 1u);
	uint capacity = root_only ? params.limits.z : params.limits.y;
	if (index < capacity) {
		uint base = (root_only ? root_base() : fine_base()) + index * 2u;
		work.data[base] = instance;
		work.data[base + 1u] = reference;
		// Keep independent marks for the fine and mandatory-root cuts. A later
		// root fallback must not expose an uninitialized material source.
		atomicOr(work.data[instance_base() + instance * 16u + 15u], root_only ? 1u : 2u);
	} else { atomicOr(work.data[4], root_only ? 8u : 2u); }
}

void main() {
	uint id = gl_GlobalInvocationID.x;
	if (phase.mode == 0u) {
		if (id < params.source.x) { initialize_instance(id); }
	} else if (phase.mode == 2u) {
		visit_node(id);
	} else if (phase.mode == 4u) {
		if (id < min(work.data[2], params.limits.y) && (atomicOr(work.data[4], 0u) & 16u) == 0u) {
			uint base = candidate_base() + id * 2u;
			select_cluster(work.data[base], work.data[base + 1u], false);
		}
	} else if (phase.mode == 5u) {
		uint flags = atomicOr(work.data[4], 0u);
		if (id >= params.source.x * params.source.w || (flags & 16u) != 0u || (phase.round == 0u && (flags & 3u) == 0u)) { return; }
		uint page = id % params.source.w;
		uint slot = metadata.data[params.offsets.y + page];
		if (slot == INVALID) { atomicOr(work.data[4], 8u); return; }
		uint count = geometry.data[slot * 8192u + 3u];
		for (uint cluster = 0u; cluster < count; ++cluster) {
			select_cluster(id / params.source.w, (slot << 8u) | cluster, true);
		}
	} else if (phase.mode == 7u && id == 0u) {
		atomicOr(work.data[4], 1u);
		work.data[7] = (work.data[4] & 24u) != 0u ? 0u : work.data[6];
		work.data[11] = root_base();
	} else if (phase.mode == 6u && id == 0u) {
		// A remaining frontier means the validated depth contract was exceeded.
		if (work.data[1u - (phase.round & 1u)] != 0u) { atomicOr(work.data[4], 1u); }
		bool root_only = (work.data[4] & 3u) != 0u;
		work.data[7] = (work.data[4] & 24u) != 0u ? 0u : work.data[root_only ? 6u : 3u];
		work.data[11] = root_only ? root_base() : fine_base();
	}
}
