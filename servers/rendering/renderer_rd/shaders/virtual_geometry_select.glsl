#[compute]

#version 450

#VERSION_DEFINES

#include "virtual_geometry_source_reads_inc.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#ifndef VG_DIRECT_SOURCE_READS
layout(set = 0, binding = 0, std430) readonly buffer Metadata { uint data[]; } metadata;
layout(set = 0, binding = 2, std430) readonly buffer Transforms { vec4 data[]; } transforms;
#else
layout(set = 0, binding = 9, std430) readonly buffer SourceAddresses { uvec4 data[]; } source_addresses;
#endif
layout(set = 0, binding = 1, std430) readonly buffer Geometry { uint data[]; } geometry;
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
uint vg_metadata_word(uint word) {
#ifdef VG_DIRECT_SOURCE_READS
	return VGWordSource(source_addresses.data[vg_batch_index].xy).data[word - VG_SELECTION_INPUT.addresses.x];
#else
	return metadata.data[word];
#endif
}
vec4 vg_transform_vector(uint word) {
#ifdef VG_DIRECT_SOURCE_READS
	return VGTransformSource(source_addresses.data[vg_batch_index].zw).data[word - VG_SELECTION_INPUT.source.z];
#else
	return transforms.data[word];
#endif
}
const uint INVALID = 0xffffffffu;
const uint VG_ROOT_PATH_SINGLE_INTERNAL = 2u;
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
shared uvec2 group_scan[128];
shared uvec2 group_first;
shared uvec4 group_ranges[64];
// Source, bounded lane demand, granted first record, granted count.
shared uvec4 group_hits[64];

uvec2 reserve_hit_prefix(uint count) {
	uint previous = atomicOr(VG_SELECTION_REQUEST(12u), 0u);
	for (;;) {
		if (previous >= 128u) { return uvec2(INVALID, 0u); }
		uint granted = min(count, 128u - previous);
		uint observed = atomicCompSwap(VG_SELECTION_REQUEST(12u), previous, previous + granted);
		if (observed == previous) { return uvec2(previous, granted); }
		previous = observed;
	}
}

// Called by lane zero between the existing reservation barriers. Hit traffic
// remains bounded per source; missing-page requests keep their own contract.
void reserve_group_hits() {
	uint saved_source = vg_batch_index;
	uint first = 0u;
	while (first < 64u) {
		uint source = group_hits[first].x;
		uint end = first + 1u;
		uint count = group_hits[first].y;
		while (end < 64u && group_hits[end].x == source) {
			count += group_hits[end].y;
			++end;
		}
		uvec2 range = uvec2(INVALID, 0u);
		if (count != 0u) {
			vg_batch_index = source;
			range = reserve_hit_prefix(count);
		}
		uint prefix = 0u;
		for (uint member = first; member < end; ++member) {
			uint demand = group_hits[member].y;
			uint granted = prefix < range.y ? min(demand, range.y - prefix) : 0u;
			group_hits[member].zw = uvec2(granted != 0u ? range.x + prefix : INVALID, granted);
			prefix += demand;
		}
		first = end;
	}
	// The leader still publishes its own node/candidate work after this call.
	vg_batch_index = saved_source;
}

// Each lane retains at most four original branch requests. Only the leader
// contends globally; the boundary path preserves each complete part request.
void reserve_group_work(uint parity, uvec4 node_counts, uvec4 cluster_counts, uint hit_count, out uvec4 node_first, out uvec4 cluster_first, out uvec2 hit_range) {
	uint lane = gl_LocalInvocationIndex;
	// Validated parts fit one page (at most 256 clusters). Four requests per
	// lane and 64 lanes therefore sum to at most 65536, without uint overflow.
	uvec2 count = uvec2(node_counts.x + node_counts.y + node_counts.z + node_counts.w,
			cluster_counts.x + cluster_counts.y + cluster_counts.z + cluster_counts.w);
	group_scan[lane * 2u] = count;
	group_hits[lane] = uvec4(vg_batch_index, hit_count, INVALID, 0u);
	barrier();
	uint source = 0u;
	for (uint step = 1u; step < 64u; step <<= 1u) {
		uvec2 sum = group_scan[lane * 2u + source];
		if (lane >= step) {
			sum += group_scan[(lane - step) * 2u + source];
		}
		group_scan[lane * 2u + 1u - source] = sum;
		barrier();
		source = 1u - source;
	}
	uvec2 prefix = group_scan[lane * 2u + source] - count;
	uvec2 total = group_scan[126u + source];
	uvec2 words = uvec2(1u - parity, 2u);
	uvec2 capacities = params.limits.xy;
	if (lane == 0u) {
		for (uint kind = 0u; kind < 2u; ++kind) {
			group_first[kind] = total[kind] == 0u ? INVALID : reserve_work(words[kind], total[kind], capacities[kind]);
		}
		reserve_group_hits();
	}
	barrier();
	node_first = uvec4(INVALID);
	cluster_first = uvec4(INVALID);
	for (uint branch = 0u; branch < 4u; ++branch) {
		if (group_first.x != INVALID && node_counts[branch] != 0u) {
			node_first[branch] = group_first.x + prefix.x;
		}
		if (group_first.y != INVALID && cluster_counts[branch] != 0u) {
			cluster_first[branch] = group_first.y + prefix.y;
		}
		prefix += uvec2(node_counts[branch], cluster_counts[branch]);
	}
	// Both independent queues share one prefix scan. If either aggregate
	// does not fit, retain ordered, whole-part admission for that queue only.
	bvec2 partial = bvec2(total.x != 0u && group_first.x == INVALID, total.y != 0u && group_first.y == INVALID);
	for (uint kind = 0u; kind < 2u; ++kind) {
		if (!partial[kind]) {
			continue;
		}
		group_ranges[lane] = kind == 0u ? node_counts : cluster_counts;
		barrier();
		if (lane == 0u) {
			for (uint member = 0u; member < 64u; ++member) {
				for (uint branch = 0u; branch < 4u; ++branch) {
					uint requested = group_ranges[member][branch];
					group_ranges[member][branch] = requested == 0u ? INVALID : reserve_work(words[kind], requested, capacities[kind]);
				}
			}
		}
		barrier();
		if (kind == 0u) {
			node_first = group_ranges[lane];
		} else {
			cluster_first = group_ranges[lane];
		}
		// A second saturated queue reuses this storage after all lanes read it.
		barrier();
	}
	hit_range = group_hits[lane].zw;
}
uint reserve_result(uint word, uint capacity) {
	uint previous = atomicOr(VG_SELECTION_WORK(word), 0u);
	for (;;) {
		if (previous >= capacity) {
			return INVALID;
		}
		uint observed = atomicCompSwap(VG_SELECTION_WORK(word), previous, previous + 1u);
		if (observed == previous) {
			return previous;
		}
		previous = observed;
	}
}
uint reserve_request(uint word, uint capacity) {
	uint previous = atomicOr(VG_SELECTION_REQUEST(word), 0u);
	for (;;) {
		if (previous >= capacity) {
			return INVALID;
		}
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

bool instance_in_frustum(mat4 transform);
bool fine_branches_rejected(mat4 transform, float minimum, float maximum);

void initialize_instance(uint id) {
	uint task = VG_SELECTION_INPUT.addresses.w + id;
	if (params.source.w == 0u && task < params.limits.x) {
		// The initial frontier has one slot per instance, including hidden ones.
		// Clear every slot before any early exit; this arena survives frames.
		work.data[queue_base(0u) + task * 4u] = INVALID;
	}
	mat4 local = mat4(1.0);
	if (VG_SELECTION_INPUT.source.y != 0u) {
		uint offset = id * VG_SELECTION_INPUT.source.y + VG_SELECTION_INPUT.source.z;
		local = transpose(mat4(vg_transform_vector(offset), vg_transform_vector(offset + 1u), vg_transform_vector(offset + 2u), vec4(0.0, 0.0, 0.0, 1.0)));
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
		// Mode10 clears this source-owned header. Only initialization writes
		// it; the dispatch barrier publishes an immutable admission result.
		// Keep word4 atomic because later stages still report other failures.
		atomicOr(VG_SELECTION_WORK(8u), 1u);
		mark_failure(16u);
		return;
	}
	mat4 view_transform = params.world_to_view * world;
	if (!instance_in_frustum(view_transform)) {
		// Both ordinary selection and mandatory roots must ignore this instance.
		for (uint word = 0u; word < 16u; ++word) { VG_SELECTION_WORK(base + word) = 0u; }
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
	mat4 rows = transpose(view_transform);
	for (uint row = 0u; row < 3u; ++row) {
		for (uint column = 0u; column < 4u; ++column) {
			VG_SELECTION_WORK(base + row * 4u + column) = floatBitsToUint(rows[row][column]);
		}
	}
	VG_SELECTION_WORK(base + 12u) = floatBitsToUint(minimum);
	VG_SELECTION_WORK(base + 13u) = floatBitsToUint(maximum);
	VG_SELECTION_WORK(base + 14u) = floatBitsToUint(sign(det));
	VG_SELECTION_WORK(base + 15u) = fine_branches_rejected(view_transform, minimum, maximum) ? 4u : 0u;
	if (params.source.w != 0u) { return; }
	if (task < params.limits.x) {
		uint word = queue_base(0u) + task * 4u;
		work.data[word] = vg_batch_index;
		work.data[word + 1u] = id;
		work.data[word + 2u] = 0u;
		work.data[word + 3u] = 0u;
	} else { mark_failure(1u); }
}

bool outside_frustum_plane(vec4 plane, vec4 magnitude, vec3 lower, vec3 upper, vec4 coordinate_bound, float padding) {
	vec3 support = mix(lower, upper, greaterThanEqual(plane.xyz, vec3(0.0)));
	float maximum = dot(plane, vec4(support, 1.0)) + padding;
	// Bound rounding from combining clip rows and evaluating the support point.
	// Use the uncombined row magnitudes: abs(plane) would hide cancellation.
	// Sixteen float epsilons cover both this dot and the former corner test.
	// A nonfinite bound also retains the box instead of making a false rejection.
	float error = 1.9073486328125e-6 * (dot(magnitude, coordinate_bound) + abs(padding));
	// Keep an outward bound when the device flushes subnormal intermediates.
	error = max(error, 3.76158192263132e-37);
	return maximum < -error;
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
	// A linear plane reaches its AABB maximum at the support corner. Work in
	// homogeneous coordinates so boxes crossing the eye plane keep the same rule.
	mat4 rows = transpose(params.projection * transform);
	vec4 extent = (1.0 + params.lod.w) * rows[3];
	vec4 coordinate_bound = vec4(max(abs(lower), abs(upper)), 1.0);
	for (uint axis = 0u; axis < 2u; ++axis) {
		vec4 magnitude = abs(extent) + abs(rows[axis]);
		if (outside_frustum_plane(extent + rows[axis], magnitude, lower, upper, coordinate_bound, side_padding[axis]) ||
				outside_frustum_plane(extent - rows[axis], magnitude, lower, upper, coordinate_bound, side_padding[axis])) {
			return false;
		}
	}
	if (outside_frustum_plane(rows[2], abs(rows[2]), lower, upper, coordinate_bound, displacement.z)) {
		return false;
	}
	if ((uint(params.lod.z) & 2u) == 0u &&
			outside_frustum_plane(rows[3] - rows[2], abs(rows[3]) + abs(rows[2]), lower, upper, coordinate_bound, displacement.z + displacement.w)) {
		return false;
	}
	return true;
}

vec3 meta3(uint word) { return uintBitsToFloat(uvec3(vg_metadata_word(word), vg_metadata_word(word + 1u), vg_metadata_word(word + 2u))); }
vec4 meta4(uint word) { return vec4(meta3(word), uintBitsToFloat(vg_metadata_word(word + 3u))); }
vec3 geom3(uint word) { return uintBitsToFloat(uvec3(geometry.data[word], geometry.data[word + 1u], geometry.data[word + 2u])); }
vec4 geom4(uint word) { return vec4(geom3(word), uintBitsToFloat(geometry.data[word + 3u])); }

bool fine_branches_rejected(mat4 transform, float minimum, float maximum) {
	uint word = VG_SELECTION_INPUT.prefix.z;
	uint flags = uint(params.lod.z);
	if (word == 0u || params.source.w != 0u || (flags & 4u) != 0u) {
		return false;
	}
	float threshold = params.lod.y * minimum * uintBitsToFloat(vg_metadata_word(word + 4u));
	if (isnan(threshold) || isinf(threshold) || threshold < 0.0) {
		return false;
	}
	if ((flags & 1u) != 0u) {
		return 1.0 > threshold * 1.0001 + 0.000001;
	}
	vec4 sphere = meta4(word);
	vec3 center = (transform * vec4(sphere.xyz, 1.0)).xyz;
	// A single scale bounds both transformed center offsets and the radius
	// used by projected_edge, including a nonorthogonal camera or instance.
	float scale = max(maximum, norm_bound(mat3(transform)));
	float radius = sphere.w * scale + uintBitsToFloat(VG_SELECTION_INPUT.offsets.w) * 1.7320509;
	vec3 magnitude = abs(transform[0].xyz) * abs(sphere.x) + abs(transform[1].xyz) * abs(sphere.y) +
			abs(transform[2].xyz) * abs(sphere.z) + abs(transform[3].xyz);
	// Enclose float center/radius arithmetic and subnormal flushes outward.
	// Ill-conditioned, near-plane and extreme inputs retain ordinary tests.
	radius += max(1e-30, 0.0001 * (max(max(magnitude.x, magnitude.y), magnitude.z) + radius));
	float z = -center.z;
	float upper = length(center) + radius;
	float lower_z = z - radius;
	if (any(isnan(center)) || any(isinf(center)) || isnan(radius) || isinf(radius) ||
			isnan(upper) || isinf(upper) || radius < 0.0 || radius >= z * 0.125 ||
			lower_z <= max(params.lod.x, 0.0) * 1.0001 + 0.000001 || upper <= 0.0) {
		return false;
	}
	// Every point of every fine LOD ball has z>=lower_z and length<=upper.
	// Thus projected_edge >= lower_z^2/upper. The strict radius/z bound
	// avoids cancellation in its tangent formula. Keep a further factor-four
	// margin for that formula's float operations, rather than testing an
	// approximate enclosing sphere as if its edge were an exact minimum.
	float lower_edge = 0.25 * (lower_z / upper) * lower_z;
	return lower_edge > threshold * 1.0001 + 0.000001;
}

bool instance_in_frustum(mat4 transform) {
	// The hierarchy root encloses every part, including nonresident detail.
	// Use the existing view-specific displacement and shadow-plane rules.
	vec3 lower = vec3(3.402823e38);
	vec3 upper = vec3(-3.402823e38);
	for (uint child = 0u; child < 4u; ++child) {
		uint branch = VG_SELECTION_INPUT.addresses.x + 52u + child * 16u;
		if (vg_metadata_word(branch + 12u) == INVALID && vg_metadata_word(branch + 13u) == INVALID) { continue; }
		lower = min(lower, meta3(branch));
		upper = max(upper, meta3(branch + 3u));
	}
	return in_frustum(lower, upper, transform);
}

void visit_node(uint id) {
	vg_batch_index = INVALID;
	uint parity = phase.round & 1u;
	bool has_task = id < min(work.data[parity], params.limits.x);
	uint instance = 0u;
	uint node = 0u;
	mat4 transform = mat4(1.0);
	float error_scale = 0.0;
	bool far_fine = false;
	uvec4 nodes = uvec4(INVALID);
	uvec4 node_counts = uvec4(0u);
	uvec4 references = uvec4(INVALID);
	uvec4 cluster_counts = uvec4(0u);
	uvec4 hit_pages = uvec4(0u);
	uvec4 hit_counts = uvec4(0u);
	uint hit_count = 0u;
	if (has_task) {
		uint task = queue_base(parity) + id * 4u;
		vg_batch_index = work.data[task];
		has_task = vg_batch_index != INVALID;
		if (has_task) {
			has_task = VG_SELECTION_WORK(8u) == 0u;
		}
		if (has_task) {
			instance = work.data[task + 1u];
			node = work.data[task + 2u];
			transform = instance_transform(instance);
			error_scale = params.lod.y * uintBitsToFloat(VG_SELECTION_WORK(instance_base() + instance * 16u + 12u));
			far_fine = (VG_SELECTION_WORK(instance_base() + instance * 16u + 15u) & 4u) != 0u;
		}
	}
	// Bit 1 is derived once for a sole internal root edge. Fine branches are
	// already excluded by the far proof. Keep every ancestor's frustum test;
	// branching roots and leaf parts retain ordinary feedback and selection.
	for (uint step = 0u; far_fine && has_task && step < VG_SELECTION_INPUT.prefix.y; ++step) {
		uint next_node = INVALID;
		for (uint child = 0u; child < 4u; ++child) {
			uint branch = VG_SELECTION_INPUT.addresses.x + 52u + node * 64u + child * 16u;
			if ((vg_metadata_word(branch + 15u) & VG_ROOT_PATH_SINGLE_INTERNAL) == 0u) {
				continue;
			}
			has_task = in_frustum(meta3(branch), meta3(branch + 3u), transform);
			next_node = vg_metadata_word(branch + 12u);
			break;
		}
		if (next_node == INVALID) {
			break;
		}
		node = next_node;
	}
	// Empty and refused lanes still reach every group reservation barrier.
	for (uint child = 0u; child < 4u && has_task; ++child) {
		uint branch = VG_SELECTION_INPUT.addresses.x + 52u + node * 64u + child * 16u;
		uint child_node = vg_metadata_word(branch + 12u);
		uint part = vg_metadata_word(branch + 13u);
		if (child_node == INVALID && part == INVALID) {
			continue;
		}
		bool root_path = vg_metadata_word(branch + 15u) != 0u;
		if (far_fine && !root_path) {
			continue;
		}
		float edge = projected_edge(instance, meta4(branch + 6u), transform);
		float threshold = error_scale * uintBitsToFloat(vg_metadata_word(branch + 11u));
		if ((!root_path && edge > threshold) || !in_frustum(meta3(branch), meta3(branch + 3u), transform)) {
			continue;
		}
		if (child_node != INVALID) {
			nodes[child] = child_node;
			node_counts[child] = 1u;
			continue;
		}
		uint reference = vg_metadata_word(branch + 14u);
		uint part_word = VG_SELECTION_INPUT.offsets.x + part * 8u;
		uint group_word = VG_SELECTION_INPUT.offsets.z + vg_metadata_word(part_word) * 2u;
		uint page_first = vg_metadata_word(group_word);
		uint page_count = vg_metadata_word(group_word + 1u);
		if (reference == INVALID) {
			for (uint page = page_first; page < page_first + page_count; ++page) {
				if (vg_metadata_word(VG_SELECTION_INPUT.offsets.y + page) != INVALID) {
					continue;
				}
				uint index = reserve_request(5u, VG_SELECTION_INPUT.limits.w);
				if (index < VG_SELECTION_INPUT.limits.w) {
					VG_SELECTION_REQUEST(16u + index * 2u) = page;
					// A camera inside the LOD sphere yields edge zero. Saturate the
					// ratio so the CPU's finite-priority admission keeps this demand.
					VG_SELECTION_REQUEST(17u + index * 2u) = floatBitsToUint(min(threshold / max(edge, 1e-20), 3.402823e38));
				} else {
					mark_failure(4u);
				}
			}
			continue;
		}
		// A single lane cannot contribute more than the whole source capacity.
		// Thus 64 lane demands sum to at most 8192 without uint overflow.
		hit_pages[child] = page_first;
		hit_counts[child] = min(page_count, 128u - hit_count);
		hit_count += hit_counts[child];
		references[child] = reference;
		cluster_counts[child] = vg_metadata_word(part_word + 3u);
	}
	uvec4 node_first, cluster_first;
	uvec2 hit_range;
	reserve_group_work(parity, node_counts, cluster_counts, hit_count, node_first, cluster_first, hit_range);
	uint hit = hit_range.x;
	uint remaining_hits = hit_range.y;
	for (uint child = 0u; child < 4u && remaining_hits != 0u; ++child) {
		uint count = min(hit_counts[child], remaining_hits);
		for (uint page = 0u; page < count; ++page) {
			uint word = 16u + VG_SELECTION_INPUT.limits.w * 2u + hit * 2u;
			VG_SELECTION_REQUEST(word) = hit_pages[child] + page;
			VG_SELECTION_REQUEST(word + 1u) = floatBitsToUint(-1.0);
			++hit;
		}
		remaining_hits -= count;
	}
	for (uint child = 0u; child < 4u; ++child) {
		if (node_counts[child] == 0u) {
			continue;
		}
		if (node_first[child] == INVALID) {
			mark_failure(1u);
			continue;
		}
		uint word = queue_base(1u - parity) + node_first[child] * 4u;
		work.data[word] = vg_batch_index;
		work.data[word + 1u] = instance;
		work.data[word + 2u] = nodes[child];
		work.data[word + 3u] = 0u;
	}
	for (uint child = 0u; child < 4u; ++child) {
		if (cluster_counts[child] == 0u) {
			continue;
		}
		if (cluster_first[child] == INVALID) {
			mark_failure(2u);
			continue;
		}
		for (uint cluster = 0u; cluster < cluster_counts[child]; ++cluster) {
			uint word = candidate_base() + (cluster_first[child] + cluster) * 4u;
			work.data[word] = vg_batch_index;
			work.data[word + 1u] = instance;
			work.data[word + 2u] = references[child] + cluster;
			work.data[word + 3u] = 0u;
		}
	}
}

bool cluster_visible(uint instance, uint reference, bool root_only) {
	if (VG_SELECTION_WORK(instance_base() + instance * 16u + 14u) == 0u) { return false; }
	uint record = (reference >> 8u) * 8192u + 8u + (reference & 255u) * 72u;
	uint flags = geometry.data[record + 7u];
	if (root_only && (flags & 9u) != 9u) { return false; }
	mat4 transform = instance_transform(instance);
	if (!in_frustum(geom3(record + 18u), geom3(record + 21u), transform)) { return false; }
	if (!root_only) {
		float edge = projected_edge(instance, geom4(record + 12u), transform);
		float error_scale = params.lod.y * uintBitsToFloat(VG_SELECTION_WORK(instance_base() + instance * 16u + 12u));
		if (edge <= error_scale * uintBitsToFloat(geometry.data[record + 16u]) && (flags & 2u) == 0u) { return false; }
	}
	return true;
}

void select_cluster(uint instance, uint reference, bool root_only) {
	if (!cluster_visible(instance, reference, root_only)) { return; }
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

// Grant the remaining prefix when a source cannot fit the whole request.
// The counter stays bounded, and only excess lanes refuse their own source.
uvec2 reserve_result_prefix(uint count, uint capacity) {
	uint previous = atomicOr(VG_SELECTION_WORK(3u), 0u);
	for (;;) {
		if (previous >= capacity) { return uvec2(INVALID, 0u); }
		uint granted = min(count, capacity - previous);
		uint observed = atomicCompSwap(VG_SELECTION_WORK(3u), previous, previous + granted);
		if (observed == previous) { return uvec2(previous, granted); }
		previous = observed;
	}
}

void select_cluster_group(uint id) {
	uint lane = gl_LocalInvocationIndex;
	bool has_task = id < min(work.data[2u], params.limits.y);
	uint source_index = INVALID;
	uint instance = 0u;
	uint reference = 0u;
	bool selected = false;
	if (has_task) {
		uint word = candidate_base() + id * 4u;
		source_index = work.data[word];
		instance = work.data[word + 1u];
		reference = work.data[word + 2u];
		vg_batch_index = source_index;
		if (VG_SELECTION_WORK(8u) == 0u) {
			selected = cluster_visible(instance, reference, false);
		}
	}
	group_scan[lane] = uvec2(source_index, selected ? 1u : 0u);
	// Inactive lanes participate too. Root coverage keeps its independent
	// per-cluster path because its page loops are not workgroup-uniform.
	barrier();
	if (lane == 0u) {
		uint first_lane = 0u;
		while (first_lane < 64u) {
			uint source = group_scan[first_lane].x;
			uint last_lane = first_lane + 1u;
			while (last_lane < 64u && group_scan[last_lane].x == source) {
				++last_lane;
			}
			uint count = 0u;
			for (uint member = first_lane; member < last_lane; ++member) {
				count += group_scan[member].y;
			}
			uvec2 range = uvec2(INVALID, 0u);
			if (count != 0u) {
				vg_batch_index = source;
				range = reserve_result_prefix(count, VG_SELECTION_INPUT.limits.y);
			}
			uint prefix = 0u;
			for (uint member = first_lane; member < last_lane; ++member) {
				uint valid = group_scan[member].y;
				group_scan[member].y = valid != 0u && prefix < range.y ? range.x + prefix : INVALID;
				prefix += valid;
			}
			first_lane = last_lane;
		}
	}
	barrier();
	// The leader borrowed each segment's source while reserving its range.
	vg_batch_index = source_index;
	if (!selected) { return; }
	uint index = group_scan[lane].y;
	if (index == INVALID) { mark_failure(2u); return; }
	uint base = (16u + VG_SELECTION_INPUT.source.x * 16u) + index * 2u;
	VG_SELECTION_WORK(base) = instance;
	VG_SELECTION_WORK(base + 1u) = reference;
	atomicOr(VG_SELECTION_WORK(instance_base() + instance * 16u + 15u), 2u);
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
		if (vg_batch_index == INVALID) { return; }
		mark_failure(1u);
	} else if (mode == 4u) {
		select_cluster_group(id);
	} else if (mode == 5u) {
		if (id >= params.source.z) { return; }
		vg_batch_index = source_for_task(id, true);
		uint flags = atomicOr(VG_SELECTION_WORK(4u), 0u);
		if ((flags & 16u) != 0u || (params.source.w == 0u && (flags & 3u) == 0u)) { return; }
		uint local = id - VG_SELECTION_INPUT.prefix.x;
		uint page = local % VG_SELECTION_INPUT.source.w;
		uint slot = vg_metadata_word(VG_SELECTION_INPUT.offsets.y + page);
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
