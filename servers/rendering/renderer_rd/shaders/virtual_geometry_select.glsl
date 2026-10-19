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
	vec4 lod;
	uvec4 source; // sources, instances, root-page tasks, root-only
	uvec4 limits; // global nodes, global candidates, reserved, reserved
	uvec4 offsets; // two frontiers, candidates, displacement bits
} params;
layout(set = 0, binding = 4, std430) buffer Work { uint data[]; } work;
layout(set = 0, binding = 6, std430) buffer Result { uint data[]; } result;
layout(set = 0, binding = 8, std430) buffer RequestWork { uint data[]; } request_work;
#include "virtual_geometry_selection_parameters_inc.glsl"
#define VG_SELECTION_BATCH_PARAMETERS
const uint INVALID = 0xffffffffu;
uint instance_base() { return 16u; }
uint queue_base(uint parity) { return params.offsets[parity]; }
uint candidate_base() { return params.offsets.z; }
void mark_failure(uint bits) {
	atomicOr(VG_SELECTION_WORK(4u), bits);
	atomicOr(VG_SELECTION_REQUEST(4u), bits);
}
// Capacity refusal marks only the source that attempted this reservation.
// Saturation must never wrap a shared counter and overwrite another source.
uint reserve_work(uint word, uint count, uint capacity) {
	uint previous = atomicOr(work.data[word], 0u);
	for (;;) {
		if (previous > capacity || count > capacity - previous) { return INVALID; }
		uint observed = atomicCompSwap(work.data[word], previous, previous + count);
		if (observed == previous) { return previous; }
		previous = observed;
	}
}
uint reserve_result(uint word, uint capacity) {
	uint previous = atomicOr(VG_SELECTION_WORK(word), 0u);
	for (;;) {
		if (previous >= capacity) { return INVALID; }
		uint observed = atomicCompSwap(VG_SELECTION_WORK(word), previous, previous + 1u);
		if (observed == previous) { return previous; }
		previous = observed;
	}
}
uint reserve_request(uint word, uint capacity) {
	uint previous = atomicOr(VG_SELECTION_REQUEST(word), 0u);
	for (;;) {
		if (previous >= capacity) { return INVALID; }
		uint observed = atomicCompSwap(VG_SELECTION_REQUEST(word), previous, previous + 1u);
		if (observed == previous) { return previous; }
		previous = observed;
	}
}
#include "virtual_geometry_view_inc.glsl"

uint source_for_task(uint task, bool root_task) {
	uint lower = 0u;
	uint upper = params.source.x;
	while (lower < upper) {
		uint middle = lower + (upper - lower) / 2u;
		uint first = root_task ? selection_inputs.data[middle].prefix.x : selection_inputs.data[middle].addresses.w;
		if (first <= task) { lower = middle + 1u; }
		else { upper = middle; }
	}
	return lower - 1u;
}

float norm_bound(mat3 value) {
	mat3 a = mat3(abs(value[0]), abs(value[1]), abs(value[2]));
	vec3 rows = a[0] + a[1] + a[2];
	vec3 columns = vec3(dot(a[0], vec3(1.0)), dot(a[1], vec3(1.0)), dot(a[2], vec3(1.0)));
	return sqrt(max(max(rows.x, rows.y), rows.z) * max(max(columns.x, columns.y), columns.z));
}

void initialize_instance(uint id) {
	mat4 local = mat4(1.0);
	if (VG_SELECTION_INPUT.source.y != 0u) {
		uint offset = id * VG_SELECTION_INPUT.source.y + VG_SELECTION_INPUT.source.z;
		local = transpose(mat4(transforms.data[offset], transforms.data[offset + 1u], transforms.data[offset + 2u], vec4(0.0, 0.0, 0.0, 1.0)));
	}
	mat4 world = VG_SELECTION_INPUT.model * local;
	mat3 basis = mat3(world);
	float det = determinant(basis);
	bool valid = !isnan(det) && !isinf(det);
	for (uint column = 0u; column < 4u; ++column) {
		valid = valid && !any(isnan(world[column])) && !any(isinf(world[column]));
	}
	uint base = instance_base() + id * 16u;
	// MultiMesh uses a zero basis to hide a harvested member. It contributes
	// neither a frontier task nor root coverage; it must not refuse its batch.
	if (valid && all(equal(basis[0], vec3(0.0))) && all(equal(basis[1], vec3(0.0))) && all(equal(basis[2], vec3(0.0)))) {
		for (uint word = 0u; word < 16u; ++word) { VG_SELECTION_WORK(base + word) = 0u; }
		return;
	}
	if (!valid || det == 0.0) {
		for (uint word = 0u; word < 16u; ++word) { VG_SELECTION_WORK(base + word) = 0u; }
		mark_failure(16u);
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
	for (uint row = 0u; row < 3u; ++row) {
		for (uint column = 0u; column < 4u; ++column) {
			VG_SELECTION_WORK(base + row * 4u + column) = floatBitsToUint(rows[row][column]);
		}
	}
	VG_SELECTION_WORK(base + 12u) = floatBitsToUint(minimum);
	VG_SELECTION_WORK(base + 13u) = floatBitsToUint(maximum);
	VG_SELECTION_WORK(base + 14u) = floatBitsToUint(sign(det));
	VG_SELECTION_WORK(base + 15u) = 0u;
	if (params.source.w != 0u) { return; }
	uint task = reserve_work(0u, 1u, params.limits.x);
	if (task < params.limits.x) {
		uint word = queue_base(0u) + task * 4u;
		work.data[word] = vg_batch_index;
		work.data[word + 1u] = id;
		work.data[word + 2u] = 0u;
		work.data[word + 3u] = 0u;
	} else { mark_failure(1u); }
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
	float maximum = uintBitsToFloat(VG_SELECTION_INPUT.offsets.w);
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
	if (id >= min(work.data[parity], params.limits.x)) { return; }
	uint task = queue_base(parity) + id * 4u;
	vg_batch_index = work.data[task];
	if ((atomicOr(VG_SELECTION_WORK(4u), 0u) & 16u) != 0u) { return; }
	uint instance = work.data[task + 1u];
	uint node = work.data[task + 2u];
	mat4 transform = instance_transform(instance);
	float error_scale = params.lod.y * uintBitsToFloat(VG_SELECTION_WORK(instance_base() + instance * 16u + 12u));
	for (uint child = 0u; child < 4u; ++child) {
		uint branch = VG_SELECTION_INPUT.addresses.x + 52u + node * 64u + child * 16u;
		uint child_node = metadata.data[branch + 12u];
		uint part = metadata.data[branch + 13u];
		if (child_node == INVALID && part == INVALID) { continue; }
		float edge = projected_edge(instance, meta4(branch + 6u), transform);
		float threshold = error_scale * uintBitsToFloat(metadata.data[branch + 11u]);
		bool root_path = metadata.data[branch + 15u] != 0u;
		if ((!root_path && edge > threshold) || !in_frustum(meta3(branch), meta3(branch + 3u), transform)) { continue; }
		if (child_node != INVALID) {
			uint index = reserve_work(1u - parity, 1u, params.limits.x);
			if (index < params.limits.x) {
				uint output_word = queue_base(1u - parity) + index * 4u;
				work.data[output_word] = vg_batch_index;
				work.data[output_word + 1u] = instance;
				work.data[output_word + 2u] = child_node;
				work.data[output_word + 3u] = 0u;
			} else { mark_failure(1u); }
			continue;
		}
		uint reference = metadata.data[branch + 14u];
		uint part_word = VG_SELECTION_INPUT.offsets.x + part * 8u;
		uint group_word = VG_SELECTION_INPUT.offsets.z + metadata.data[part_word] * 2u;
		uint page_first = metadata.data[group_word];
		uint page_count = metadata.data[group_word + 1u];
		if (reference == INVALID) {
			for (uint page = page_first; page < page_first + page_count; ++page) {
				if (metadata.data[VG_SELECTION_INPUT.offsets.y + page] != INVALID) { continue; }
				uint index = reserve_request(5u, VG_SELECTION_INPUT.limits.w);
				if (index < VG_SELECTION_INPUT.limits.w) {
					VG_SELECTION_REQUEST(16u + index * 2u) = page;
					VG_SELECTION_REQUEST(17u + index * 2u) = floatBitsToUint(threshold / max(edge, 1e-20));
				} else { mark_failure(4u); }
			}
			continue;
		}
		for (uint page = page_first; page < page_first + page_count; ++page) {
			uint hit = reserve_request(12u, 128u);
			if (hit < 128u) {
				uint word = 16u + VG_SELECTION_INPUT.limits.w * 2u + hit * 2u;
				VG_SELECTION_REQUEST(word) = page;
				VG_SELECTION_REQUEST(word + 1u) = floatBitsToUint(-1.0);
			}
		}
		uint count = metadata.data[part_word + 3u];
		uint first = reserve_work(2u, count, params.limits.y);
		if (first > params.limits.y || count > params.limits.y - first) { mark_failure(2u); continue; }
		for (uint cluster = 0u; cluster < count; ++cluster) {
			uint word = candidate_base() + (first + cluster) * 4u;
			work.data[word] = vg_batch_index;
			work.data[word + 1u] = instance;
			work.data[word + 2u] = reference + cluster;
			work.data[word + 3u] = 0u;
		}
	}
}

void select_cluster(uint instance, uint reference, bool root_only) {
	if (VG_SELECTION_WORK(instance_base() + instance * 16u + 14u) == 0u) { return; }
	uint record = (reference >> 8u) * 8192u + 8u + (reference & 255u) * 72u;
	uint flags = geometry.data[record + 7u];
	if (root_only && (flags & 9u) != 9u) { return; }
	mat4 transform = instance_transform(instance);
	if (!in_frustum(geom3(record + 18u), geom3(record + 21u), transform)) { return; }
	if (!root_only) {
		float edge = projected_edge(instance, geom4(record + 12u), transform);
		float error_scale = params.lod.y * uintBitsToFloat(VG_SELECTION_WORK(instance_base() + instance * 16u + 12u));
		if (edge <= error_scale * uintBitsToFloat(geometry.data[record + 16u]) && (flags & 2u) == 0u) { return; }
	}
	uint capacity = root_only ? VG_SELECTION_INPUT.limits.z : VG_SELECTION_INPUT.limits.y;
	uint index = reserve_result(root_only ? 6u : 3u, capacity);
	if (index < capacity) {
		uint base = (16u + VG_SELECTION_INPUT.source.x * 16u) + index * 2u;
		VG_SELECTION_WORK(base) = instance;
		VG_SELECTION_WORK(base + 1u) = reference;
		// Keep independent marks for the fine and mandatory-root cuts. A later
		// root fallback must not expose an uninitialized material source.
		atomicOr(VG_SELECTION_WORK(instance_base() + instance * 16u + 15u), root_only ? 1u : 2u);
	} else { mark_failure(root_only ? 8u : 2u); }
}

void main() {
	uint id = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	uint mode = vg_selection_mode();
	if (mode == 10u) {
		if (id >= params.source.x) { return; }
		vg_batch_index = id;
		for (uint word = 0u; word < 16u; ++word) {
			VG_SELECTION_WORK(word) = 0u;
			VG_SELECTION_REQUEST(word) = 0u;
		}
		VG_SELECTION_WORK(11u) = 16u + VG_SELECTION_INPUT.source.x * 16u;
	} else if (mode == 0u) {
		if (id >= params.source.y) { return; }
		vg_batch_index = source_for_task(id, false);
		initialize_instance(id - VG_SELECTION_INPUT.addresses.w);
	} else if (mode == 2u) {
		visit_node(id);
	} else if (mode == 11u) {
		uint parity = 1u - (phase.round & 1u);
		if (id >= min(work.data[parity], params.limits.x)) { return; }
		vg_batch_index = work.data[queue_base(parity) + id * 4u];
		mark_failure(1u);
	} else if (mode == 4u) {
		if (id >= min(work.data[2u], params.limits.y)) { return; }
		uint word = candidate_base() + id * 4u;
		vg_batch_index = work.data[word];
		if ((atomicOr(VG_SELECTION_WORK(4u), 0u) & 16u) == 0u) {
			select_cluster(work.data[word + 1u], work.data[word + 2u], false);
		}
	} else if (mode == 5u) {
		if (id >= params.source.z) { return; }
		vg_batch_index = source_for_task(id, true);
		uint flags = atomicOr(VG_SELECTION_WORK(4u), 0u);
		if ((flags & 16u) != 0u || (params.source.w == 0u && (flags & 3u) == 0u)) { return; }
		uint local = id - VG_SELECTION_INPUT.prefix.x;
		uint page = local % VG_SELECTION_INPUT.source.w;
		uint slot = metadata.data[VG_SELECTION_INPUT.offsets.y + page];
		if (slot == INVALID) { mark_failure(8u); return; }
		uint count = geometry.data[slot * 8192u + 3u];
		for (uint cluster = 0u; cluster < count; ++cluster) {
			select_cluster(local / VG_SELECTION_INPUT.source.w, (slot << 8u) | cluster, true);
		}
	} else if (mode == 6u || mode == 7u) {
		if (id >= params.source.x) { return; }
		vg_batch_index = id;
		if (mode == 7u) { mark_failure(1u); }
		uint flags = VG_SELECTION_WORK(4u);
		bool root_only = (flags & 3u) != 0u;
		VG_SELECTION_WORK(7u) = (flags & 24u) != 0u ? 0u : VG_SELECTION_WORK(root_only ? 6u : 3u);
	}
}
