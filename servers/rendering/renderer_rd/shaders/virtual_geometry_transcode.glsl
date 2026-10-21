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

layout(set = 0, binding = 2, std430) readonly buffer DependencyPages {
	uint words[];
} dependency_pages;

layout(push_constant, std430) uniform Parameters {
	uint source_word;
	uint destination_word;
	uint clusters;
	uint gpu_bytes;
	uint phase;
	uint dependency_word;
	uint dependencies;
} params;

uint record_word;
uint reference_word;

uint record(uint word) {
	return disk_pages.words[record_word + word];
}

uint cluster_record(uint cluster, uint word) {
	return disk_pages.words[params.source_word + 8u + cluster * 72u + word];
}

uint layout_record(uint page, bool from_gpu, uint cluster, uint word) {
	uint address = page + 8u + cluster * 72u + word;
	return from_gpu ? gpu_pages.words[address] : disk_pages.words[address];
}

uint disk_byte(uint offset) {
	return (disk_pages.words[params.source_word + (offset >> 2u)] >> ((offset & 3u) * 8u)) & 255u;
}

uint mask_bits(uint bits) {
	return bits == 32u ? 0xffffffffu : (1u << bits) - 1u;
}

uint channels(uint cluster) {
	uint flags = cluster_record(cluster, 35u);
	return 5u + (flags & 1u) + ((flags & 2u) != 0u ? 4u : 0u) + cluster_record(cluster, 36u) * 2u;
}

// bits, integer origin, bit offset in a vertex, attribute-stream selector.
uvec4 channel_at(uint page, bool from_gpu, uint cluster, uint index) {
	if (index < 3u) {
		uint offset = 0u;
		for (uint axis = 0u; axis < index; ++axis) {
			offset += layout_record(page, from_gpu, cluster, 28u + axis);
		}
		return uvec4(layout_record(page, from_gpu, cluster, 28u + index), layout_record(page, from_gpu, cluster, 25u + index), offset, 0u);
	}
	index -= 3u;
	uint normal_bits = layout_record(page, from_gpu, cluster, 33u);
	if (index < 2u) {
		return uvec4(normal_bits, 0u, index * normal_bits, 1u);
	}
	index -= 2u;
	uint offset = normal_bits * 2u;
	uint flags = layout_record(page, from_gpu, cluster, 35u);
	if ((flags & 1u) != 0u) {
		uint bits = layout_record(page, from_gpu, cluster, 34u) + 1u;
		if (index == 0u) {
			return uvec4(bits, 0u, offset, 1u);
		}
		--index;
		offset += bits;
	}
	if ((flags & 2u) != 0u) {
		for (uint component = 0u; component < 4u; ++component) {
			uint bits = (layout_record(page, from_gpu, cluster, 40u) >> (component * 4u)) & 15u;
			if (index == 0u) {
				return uvec4(bits, (layout_record(page, from_gpu, cluster, 39u) >> (component * 8u)) & 255u, offset, 1u);
			}
			--index;
			offset += bits;
		}
	}
	for (uint component = 0u; component < layout_record(page, from_gpu, cluster, 36u) * 2u; ++component) {
		uint code = layout_record(page, from_gpu, cluster, 41u + component);
		uint bits = code & 63u;
		if (index == 0u) {
			return uvec4(bits, code >> 6u, offset, 1u);
		}
		--index;
		offset += bits;
	}
	return uvec4(0u);
}

uvec4 channel_at(uint cluster, uint index) {
	return channel_at(params.source_word, false, cluster, index);
}

uint vertex_stride(uint page, bool from_gpu, uint cluster, bool p_attribute) {
	if (!p_attribute) {
		return layout_record(page, from_gpu, cluster, 28u) + layout_record(page, from_gpu, cluster, 29u) + layout_record(page, from_gpu, cluster, 30u);
	}
	uint flags = layout_record(page, from_gpu, cluster, 35u);
	uint result = layout_record(page, from_gpu, cluster, 33u) * 2u;
	if ((flags & 1u) != 0u) {
		result += layout_record(page, from_gpu, cluster, 34u) + 1u;
	}
	if ((flags & 2u) != 0u) {
		for (uint component = 0u; component < 4u; ++component) {
			result += (layout_record(page, from_gpu, cluster, 40u) >> (component * 4u)) & 15u;
		}
	}
	for (uint component = 0u; component < layout_record(page, from_gpu, cluster, 36u) * 2u; ++component) {
		result += layout_record(page, from_gpu, cluster, 41u + component) & 63u;
	}
	return result;
}

uint vertex_stride(uint cluster, bool p_attribute) {
	return vertex_stride(params.source_word, false, cluster, p_attribute);
}

bool vertex_reference(uint vertex) {
	return (disk_byte(record(37u) + (vertex >> 3u)) & (1u << (vertex & 7u))) != 0u;
}

uint reference_count(uint before_vertex) {
	uint result = 0u;
	for (uint word = 0u; word < 8u; ++word) {
		uint before = min(32u, before_vertex > word * 32u ? before_vertex - word * 32u : 0u);
		result += uint(bitCount(disk_pages.words[params.source_word + (record(37u) >> 2u) + word] & mask_bits(before)));
	}
	return result;
}

void write_vertex_value(uint cluster, uint vertex, uvec4 description, uint value) {
	if (description.x == 0u) {
		return;
	}
	uint start = cluster_record(cluster, 71u);
	if (description.w != 0u) {
		start += (cluster_record(cluster, 32u) + 3u) & ~3u;
	}
	uint bit = vertex * vertex_stride(cluster, description.w != 0u) + description.z;
	uint word = params.destination_word + (start >> 2u) + (bit >> 5u);
	uint shift = bit & 31u;
	atomicOr(gpu_pages.words[word], value << shift);
	if (shift + description.x > 32u) {
		atomicOr(gpu_pages.words[word + 1u], value >> (32u - shift));
	}
}

uint read_vertex_value(uint cluster, uint vertex, uvec4 description) {
	if (description.x == 0u) {
		return 0u;
	}
	uint start = cluster_record(cluster, 71u);
	if (description.w != 0u) {
		start += (cluster_record(cluster, 32u) + 3u) & ~3u;
	}
	uint bit = vertex * vertex_stride(cluster, description.w != 0u) + description.z;
	uint word = params.destination_word + (start >> 2u) + (bit >> 5u);
	uint shift = bit & 31u;
	// Reference fields can share a word with this immutable coded field.
	// Atomic reads keep that word's concurrent atomic updates well-defined.
	uint value = atomicOr(gpu_pages.words[word], 0u) >> shift;
	if (shift + description.x > 32u) {
		value |= atomicOr(gpu_pages.words[word + 1u], 0u) << (32u - shift);
	}
	return value & mask_bits(description.x);
}

uint read_dependency_vertex(uint page, uint cluster, uint vertex, uvec4 description) {
	if (description.x == 0u) {
		return 0u;
	}
	uint start = layout_record(page, true, cluster, description.w != 0u ? 37u : 31u);
	uint bit = vertex * vertex_stride(page, true, cluster, description.w != 0u) + description.z;
	uint word = page + (start >> 2u) + (bit >> 5u);
	uint shift = bit & 31u;
	uint value = gpu_pages.words[word] >> shift;
	if (shift + description.x > 32u) {
		value |= gpu_pages.words[word + 1u] << (32u - shift);
	}
	return value & mask_bits(description.x);
}

void decode_coded_vertices(uint cluster) {
	uint channel = gl_LocalInvocationIndex;
	if (channel >= channels(cluster)) {
		return;
	}
	uvec4 description = channel_at(cluster, channel);
	if (description.x == 0u) {
		return;
	}
	uvec4 plane_values = uvec4(0u);
	uvec4 rank = uvec4(0u);
	for (uint other = 0u; other < channels(cluster); ++other) {
		uint bits = channel_at(cluster, other).x;
		for (uint plane = 0u; plane < 4u; ++plane) {
			if (bits > plane * 8u) {
				++plane_values[plane];
				if (other < channel) {
					++rank[plane];
				}
			}
		}
	}
	uint coded = record(4u) - reference_count(record(4u));
	uvec4 starts = uvec4(record(31u));
	for (uint plane = 1u; plane < 4u; ++plane) {
		starts[plane] = starts[plane - 1u] + coded * plane_values[plane - 1u];
	}
	uint previous = channel < 3u ? (1u << description.x) >> 1u : 0u;
	uint unique_vertex = 0u;
	for (uint vertex = 0u; vertex < record(4u); ++vertex) {
		if (vertex_reference(vertex)) {
			continue;
		}
		uint zigzag = 0u;
		for (uint plane = 0u; plane < (description.x + 7u) / 8u; ++plane) {
			zigzag |= disk_byte(starts[plane] + unique_vertex * plane_values[plane] + rank[plane]) << (plane * 8u);
		}
		previous = (previous + ((zigzag >> 1u) ^ (0u - (zigzag & 1u)))) & mask_bits(description.x);
		write_vertex_value(cluster, vertex, description, previous);
		++unique_vertex;
	}
}

void decode_referenced_vertices(uint cluster) {
	uint count = reference_count(record(4u));
	for (uint vertex = gl_LocalInvocationIndex; vertex < record(4u); vertex += 64u) {
		if (!vertex_reference(vertex)) {
			continue;
		}
		uint ordinal = reference_count(vertex);
		uint source_cluster = disk_byte(record(37u) + 32u + ordinal);
		uint dependency = 0u;
		for (uint plane = 0u; plane < 4u; ++plane) {
			dependency |= disk_byte(record(37u) + 32u + count * (2u + plane) + ordinal) << (plane * 8u);
		}
		if (dependency > params.dependencies) {
			return; // Host rejects this before dispatch; retain the shader guard.
		}
		uint source_page = dependency == 0u ? params.source_word : dependency_pages.words[params.dependency_word + dependency - 1u];
		uint source_vertex = 0u;
		for (uint reference = 0u; reference <= ordinal; ++reference) {
			uint zigzag = disk_byte(record(37u) + 32u + count + reference);
			source_vertex = (source_vertex + ((zigzag >> 1u) ^ (0u - (zigzag & 1u)))) & 255u;
		}
		for (uint channel = 0u; channel < channels(cluster); ++channel) {
			uvec4 destination = channel_at(cluster, channel);
			uvec4 source = channel_at(source_page, dependency != 0u, source_cluster, channel);
			uint encoded = dependency == 0u ? read_vertex_value(source_cluster, source_vertex, source) : read_dependency_vertex(source_page, source_cluster, source_vertex, source);
			uint value = source.y + encoded - destination.y;
			write_vertex_value(cluster, vertex, destination, value);
		}
	}
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
	if (params.phase != 0u) {
		decode_referenced_vertices(cluster);
		return;
	}
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
		} else if (lane == 7u) {
			value = 0u;
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
	for (uint word = lane; word < (indices - position) / 4u; word += 64u) {
		gpu_pages.words[params.destination_word + (position >> 2u) + word] = 0u;
	}
	copy_words(record(66u), materials, record(67u) + record(6u) + record(70u));
	for (uint word = lane; word < index_words; word += 64u) {
		gpu_pages.words[params.destination_word + (indices >> 2u) + word] = 0u;
	}
	// All triangle writers in this cluster must observe the cleared words.
	memoryBarrierBuffer();
	barrier();
	decode_coded_vertices(cluster);
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
