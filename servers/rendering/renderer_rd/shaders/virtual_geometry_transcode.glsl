#[compute]

#version 450

#VERSION_DEFINES

// VGPG -> VGGP. Each workgroup owns one cluster's complete output ranges.
// Inputs are immutable, publication-validated pages; offsets are page-relative.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer DiskPages {
	uint words[];
} disk_pages;

layout(set = 0, binding = 1, std430) buffer GpuPages {
	uint words[];
} gpu_pages;

layout(push_constant, std430) uniform Parameters {
	uint source_word;
	uint destination_word;
	uint clusters;
	uint gpu_bytes;
} params;

uint record_word;
uint reference_word;

uint record(uint word) {
	return disk_pages.words[record_word + word];
}

uint flag(uint triangle, uint mask_index) {
	return (record(49u + (triangle >> 5u) * 3u + mask_index) >> (triangle & 31u)) & 1u;
}

uint count_before(uint triangle, bool references) {
	uint block = triangle >> 5u;
	uint local = triangle & 31u;
	uint prefix = block == 0u ? 0u : (record(references ? 62u : 61u) >> ((block - 1u) * 10u)) & 1023u;
	uint previous = (1u << local) - 1u;
	uint start_mask = record(49u + block * 3u);
	uint left_mask = record(50u + block * 3u);
	uint reference_mask = record(51u + block * 3u);
	uint count = 2u * uint(bitCount(start_mask & left_mask & previous)) + uint(bitCount(reference_mask & previous));
	return prefix + (references ? count : 2u * uint(bitCount(start_mask & previous)) + local - count);
}

uint reference_vertex(uint index, uint created) {
	uint bit = index * 5u;
	uint shift = bit & 31u;
	uint value = disk_pages.words[reference_word + (bit >> 5u)] >> shift;
	if (shift > 27u) {
		value |= disk_pages.words[reference_word + (bit >> 5u) + 1u] << (32u - shift);
	}
	return created - 1u - (value & 31u);
}

uint head_at(uint triangle) {
	uint created = count_before(triangle, false);
	uint reference = count_before(triangle, true);
	if (flag(triangle, 0u) != 0u) {
		uint count = flag(triangle, 1u) * 2u + flag(triangle, 2u);
		return count < 3u ? created + 2u - count : reference_vertex(reference + 2u, created);
	}
	return flag(triangle, 2u) == 0u ? created : reference_vertex(reference, created);
}

uvec3 triangle_at(uint triangle) {
	uint created = count_before(triangle, false);
	if (flag(triangle, 0u) != 0u) {
		uint reference = count_before(triangle, true);
		uint count = flag(triangle, 1u) * 2u + flag(triangle, 2u);
		uvec3 indices;
		for (uint corner = 0u; corner < 3u; ++corner) {
			indices[corner] = corner < count ? reference_vertex(reference + corner, created) : created + corner - count;
		}
		return indices;
	}
	bool left = flag(triangle, 1u) != 0u;
	uint block = triangle >> 5u;
	uint local = triangle & 31u;
	uint start_mask = record(49u + block * 3u);
	uint left_mask = record(50u + block * 3u);
	uint stop_mask = start_mask | (left ? ~left_mask : left_mask);
	// Every block starts a strip. The nearest earlier Start/opposite turn is
	// therefore in this block, without a serial walk along the strip.
	uint anchor_triangle = block * 32u + uint(findMSB(stop_mask & ((1u << local) - 1u)));
	uint anchor;
	if (flag(anchor_triangle, 0u) != 0u) {
		uint corner = left ? 1u : 0u;
		uint count = flag(anchor_triangle, 1u) * 2u + flag(anchor_triangle, 2u);
		uint first = count_before(anchor_triangle, false);
		anchor = count > corner ? reference_vertex(count_before(anchor_triangle, true) + corner, first) : first + corner - count;
	} else {
		anchor = head_at(anchor_triangle - 1u);
	}
	uint previous = head_at(triangle - 1u);
	uint head = head_at(triangle);
	return left ? uvec3(anchor, head, previous) : uvec3(anchor, previous, head);
}

void copy_words(uint source_byte, uint destination_byte, uint count) {
	for (uint word = gl_LocalInvocationIndex; word < count; word += 64u) {
		gpu_pages.words[params.destination_word + (destination_byte >> 2u) + word] =
				disk_pages.words[params.source_word + (source_byte >> 2u) + word];
	}
}

void main() {
	uint cluster = gl_WorkGroupID.x;
	if (cluster >= params.clusters) {
		return;
	}
	uint lane = gl_LocalInvocationIndex;
	record_word = params.source_word + 8u + cluster * 72u;
	reference_word = params.source_word + (record(63u) >> 2u);
	uint position = record(71u);
	uint attribute_byte = position + ((record(32u) + 3u) & ~3u);
	uint indices = attribute_byte + ((record(38u) + 3u) & ~3u);
	uint base_bits = uint(findMSB(record(4u) - 1u)) + 1u;
	uint bits_per_triangle = base_bits + 10u;
	uint index_words = (record(5u) * bits_per_triangle + 31u) >> 5u;
	uint materials = indices + index_words * 4u;
	uint palette = materials + record(67u) * 4u;
	uint batches = palette + record(6u) * 4u;
	if (cluster == 0u && lane < 8u) {
		uint value = disk_pages.words[params.source_word + lane];
		if (lane == 0u) {
			value = 0x50474756u;
		} else if (lane == 3u) {
			value = params.clusters;
		} else if (lane == 4u || lane == 5u) {
			value = params.gpu_bytes;
		}
		gpu_pages.words[params.destination_word + lane] = value;
	}
	for (uint word = lane; word < 72u; word += 64u) {
		uint value = record(word);
		switch (word) {
			case 31u: value = position; break;
			case 37u: value = attribute_byte; break;
			case 63u: value = indices; break;
			case 64u: value = index_words * 4u; break;
			case 65u:
				if ((value >> 25u) == 127u) {
					value = (value & ~0x7ffffu) | (materials >> 2u);
				}
				break;
			case 66u: value = materials; break;
			case 68u: value = palette; break;
			case 69u: value = batches; break;
			case 71u: value = 0u; break;
		}
		gpu_pages.words[params.destination_word + 8u + cluster * 72u + word] = value;
	}
	copy_words(record(31u), position, (record(32u) + 3u) >> 2u);
	copy_words(record(37u), attribute_byte, (record(38u) + 3u) >> 2u);
	copy_words(record(66u), materials, record(67u) + record(6u) + record(70u));
	for (uint word = lane; word < index_words; word += 64u) {
		gpu_pages.words[params.destination_word + (indices >> 2u) + word] = 0u;
	}
	// All triangle writers in this cluster must observe the cleared words.
	memoryBarrierBuffer();
	barrier();
	for (uint triangle = lane; triangle < record(5u); triangle += 64u) {
		uvec3 vertices = triangle_at(triangle);
		if (vertices.y < min(vertices.x, vertices.z)) {
			vertices = vertices.yzx;
		} else if (vertices.z < min(vertices.x, vertices.y)) {
			vertices = vertices.zxy;
		}
		uint value = vertices.x | ((vertices.y - vertices.x) << base_bits) | ((vertices.z - vertices.x) << (base_bits + 5u));
		uint bit = triangle * bits_per_triangle;
		uint shift = bit & 31u;
		uint word = params.destination_word + (indices >> 2u) + (bit >> 5u);
		atomicOr(gpu_pages.words[word], value << shift);
		if (shift + bits_per_triangle > 32u) {
			atomicOr(gpu_pages.words[word + 1u], value >> (32u - shift));
		}
	}
}
