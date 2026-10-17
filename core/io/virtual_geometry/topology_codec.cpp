#include "topology_codec.h"

#include <algorithm>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

uint32_t popcount(uint32_t p_bits) {
	uint32_t count = 0;
	while (p_bits != 0) {
		p_bits &= p_bits - 1;
		++count;
	}
	return count;
}

uint32_t flag(const ClusterStripLayout &p_layout, uint32_t p_triangle, uint32_t p_mask) {
	return (p_layout.masks[p_triangle / 32][p_mask] >> (p_triangle % 32)) & 1u;
}

uint32_t count_before(const ClusterStripLayout &p_layout, uint32_t p_triangle, bool p_references) {
	const uint32_t block = p_triangle / 32;
	const uint32_t local = p_triangle % 32;
	const uint32_t prefix = block == 0 ? 0 : ((p_references ? p_layout.references_before : p_layout.new_vertices_before) >> ((block - 1) * 10)) & 1023;
	const uint32_t previous = (1u << local) - 1;
	const auto &masks = p_layout.masks[block];
	const uint32_t references = 2 * popcount(masks[0] & masks[1] & previous) + popcount(masks[2] & previous);
	return prefix + (p_references ? references : 2 * popcount(masks[0] & previous) + local - references);
}

uint32_t read_five(const uint8_t *p_bytes, uint32_t p_reference) {
	const uint32_t offset = p_reference * 5;
	uint32_t value = 0;
	for (uint32_t bit = 0; bit < 5; ++bit) {
		value |= static_cast<uint32_t>((p_bytes[(offset + bit) / 8] >> ((offset + bit) % 8)) & 1u) << bit;
	}
	return value;
}

TopologyCodecError reference_vertex(const uint8_t *p_bytes, uint32_t p_reference, uint32_t p_created, uint32_t &r_vertex) {
	const uint32_t delta = read_five(p_bytes, p_reference);
	if (p_created == 0 || delta >= p_created) {
		return TopologyCodecError::INVALID_REFERENCE;
	}
	r_vertex = p_created - 1 - delta;
	return TopologyCodecError::OK;
}

TopologyCodecError validate(const ClusterStripLayout &p_layout, const uint8_t *p_bytes, size_t p_size) {
	if (p_layout.triangles == 0 || p_layout.triangles > MAX_CLUSTER_TRIANGLES || p_layout.vertices == 0 || p_layout.vertices > MAX_CLUSTER_TRIANGLES * 3) {
		return TopologyCodecError::INVALID_LAYOUT;
	}
	if ((p_layout.new_vertices_before >> 30) != 0 || (p_layout.references_before >> 30) != 0) {
		return TopologyCodecError::INVALID_PREFIX;
	}
	uint32_t created = 0;
	uint32_t references = 0;
	for (uint32_t block = 0; block < 4; ++block) {
		if (block != 0 && (((p_layout.new_vertices_before >> ((block - 1) * 10)) & 1023) != created || ((p_layout.references_before >> ((block - 1) * 10)) & 1023) != references)) {
			return TopologyCodecError::INVALID_PREFIX;
		}
		const uint32_t count = std::min(32u, p_layout.triangles > block * 32 ? p_layout.triangles - block * 32 : 0);
		const uint32_t active = count == 32 ? UINT32_MAX : (1u << count) - 1;
		const auto &masks = p_layout.masks[block];
		if (((masks[0] | masks[1] | masks[2]) & ~active) != 0 || (count != 0 && (masks[0] & 1u) == 0)) {
			return TopologyCodecError::INVALID_MASKS;
		}
		const uint32_t block_references = 2 * popcount(masks[0] & masks[1]) + popcount(masks[2]);
		references += block_references;
		created += 2 * popcount(masks[0]) + count - block_references;
	}
	if (created != p_layout.vertices) {
		return TopologyCodecError::INVALID_LAYOUT;
	}
	const size_t bits = references * 5;
	const size_t bytes = ((bits + 31) / 32) * 4;
	if (p_size != bytes || (bytes != 0 && p_bytes == nullptr)) {
		return TopologyCodecError::INVALID_LENGTH;
	}
	for (size_t bit = bits; bit < bytes * 8; ++bit) {
		if (((p_bytes[bit / 8] >> (bit % 8)) & 1u) != 0) {
			return TopologyCodecError::INVALID_PADDING;
		}
	}
	return TopologyCodecError::OK;
}

TopologyCodecError triangle_valid(const std::array<uint32_t, 3> &p_indices, uint32_t p_created) {
	if (p_indices[0] == p_indices[1] || p_indices[1] == p_indices[2] || p_indices[2] == p_indices[0]) {
		return TopologyCodecError::INVALID_TRIANGLE;
	}
	for (uint32_t index : p_indices) {
		if (index >= p_created) {
			return TopologyCodecError::INVALID_REFERENCE;
		}
		if (p_created - 1 - index >= 32) {
			return TopologyCodecError::TAIL_WINDOW;
		}
	}
	return TopologyCodecError::OK;
}

TopologyCodecError head_at(const ClusterStripLayout &p_layout, const uint8_t *p_bytes, uint32_t p_triangle, uint32_t &r_head) {
	const uint32_t created = count_before(p_layout, p_triangle, false);
	const uint32_t reference = count_before(p_layout, p_triangle, true);
	if (flag(p_layout, p_triangle, 0) != 0) {
		const uint32_t count = flag(p_layout, p_triangle, 1) * 2 + flag(p_layout, p_triangle, 2);
		if (count < 3) {
			r_head = created + 2 - count;
			return TopologyCodecError::OK;
		}
		return reference_vertex(p_bytes, reference + 2, created, r_head);
	}
	if (flag(p_layout, p_triangle, 2) != 0) {
		return reference_vertex(p_bytes, reference, created, r_head);
	}
	r_head = created;
	return TopologyCodecError::OK;
}

bool valid_batch_ranges(const std::vector<ClusterMaterialRange> &p_ranges) {
	if (p_ranges.empty() || p_ranges.size() > MAX_CLUSTER_MATERIALS) {
		return false;
	}
	uint32_t end = 0;
	for (const auto &range : p_ranges) {
		if (range.start != end || range.count == 0 || range.count > MAX_CLUSTER_TRIANGLES - end) {
			return false;
		}
		end += range.count;
	}
	return true;
}

void write_word_bits(std::vector<uint32_t> &r_words, size_t &r_cursor, uint32_t p_value, uint32_t p_bits) {
	for (uint32_t bit = 0; bit < p_bits; ++bit, ++r_cursor) {
		r_words[r_cursor / 32] |= ((p_value >> bit) & 1u) << (r_cursor % 32);
	}
}

uint32_t read_word_bits(const std::vector<uint32_t> &p_words, size_t &r_cursor, uint32_t p_bits) {
	uint32_t value = 0;
	for (uint32_t bit = 0; bit < p_bits; ++bit, ++r_cursor) {
		value |= ((p_words[r_cursor / 32] >> (r_cursor % 32)) & 1u) << bit;
	}
	return value;
}

} // namespace

TopologyCodecError unpack_cluster_topology(const ClusterStripLayout &p_layout, const uint8_t *p_bytes,
		size_t p_size, std::vector<uint32_t> &r_indices) {
	const auto valid = validate(p_layout, p_bytes, p_size);
	if (valid != TopologyCodecError::OK) {
		return valid;
	}
	try {
		std::vector<uint32_t> candidate;
		candidate.reserve(p_layout.triangles * 3);
		std::array<uint32_t, 3> previous = {};
		uint32_t created = 0;
		uint32_t reference = 0;
		for (uint32_t triangle = 0; triangle < p_layout.triangles; ++triangle) {
			std::array<uint32_t, 3> canonical;
			const bool start = flag(p_layout, triangle, 0) != 0;
			const bool left = flag(p_layout, triangle, 1) != 0;
			if (start) {
				const uint32_t count = static_cast<uint32_t>(left) * 2 + flag(p_layout, triangle, 2);
				for (uint32_t corner = 0; corner < count; ++corner) {
					const auto error = reference_vertex(p_bytes, reference++, created, canonical[corner]);
					if (error != TopologyCodecError::OK) {
						return error;
					}
				}
				for (uint32_t corner = count; corner < 3; ++corner) {
					canonical[corner] = created++;
				}
			} else {
				canonical[0] = left ? previous[2] : previous[0];
				canonical[1] = left ? previous[1] : previous[2];
				if (flag(p_layout, triangle, 2) != 0) {
					const auto error = reference_vertex(p_bytes, reference++, created, canonical[2]);
					if (error != TopologyCodecError::OK) {
						return error;
					}
				} else {
					canonical[2] = created++;
				}
			}
			const auto error = triangle_valid(canonical, created);
			if (error != TopologyCodecError::OK) {
				return error;
			}
			previous = canonical;
			if (!start && left) {
				candidate.insert(candidate.end(), { canonical[1], canonical[2], canonical[0] });
			} else {
				candidate.insert(candidate.end(), canonical.begin(), canonical.end());
			}
		}
		r_indices = std::move(candidate);
		return TopologyCodecError::OK;
	} catch (const std::bad_alloc &) {
		return TopologyCodecError::MEMORY;
	}
}

TopologyCodecError unpack_cluster_triangle(const ClusterStripLayout &p_layout, const uint8_t *p_bytes,
		size_t p_size, uint32_t p_triangle, std::array<uint32_t, 3> &r_indices) {
	const auto valid = validate(p_layout, p_bytes, p_size);
	if (valid != TopologyCodecError::OK) {
		return valid;
	}
	if (p_triangle >= p_layout.triangles) {
		return TopologyCodecError::INVALID_TRIANGLE;
	}
	const uint32_t created = count_before(p_layout, p_triangle, false);
	const uint32_t reference = count_before(p_layout, p_triangle, true);
	std::array<uint32_t, 3> candidate;
	uint32_t after = created;
	if (flag(p_layout, p_triangle, 0) != 0) {
		const uint32_t count = flag(p_layout, p_triangle, 1) * 2 + flag(p_layout, p_triangle, 2);
		for (uint32_t corner = 0; corner < count; ++corner) {
			const auto error = reference_vertex(p_bytes, reference + corner, created, candidate[corner]);
			if (error != TopologyCodecError::OK) {
				return error;
			}
		}
		for (uint32_t corner = count; corner < 3; ++corner) {
			candidate[corner] = after++;
		}
	} else {
		const bool left = flag(p_layout, p_triangle, 1) != 0;
		uint32_t anchor_triangle = p_triangle - 1;
		while (flag(p_layout, anchor_triangle, 0) == 0 && (flag(p_layout, anchor_triangle, 1) != 0) == left) {
			--anchor_triangle;
		}
		uint32_t anchor;
		TopologyCodecError error;
		if (flag(p_layout, anchor_triangle, 0) != 0) {
			const uint32_t corner = left ? 1u : 0u;
			const uint32_t count = flag(p_layout, anchor_triangle, 1) * 2 + flag(p_layout, anchor_triangle, 2);
			const uint32_t first = count_before(p_layout, anchor_triangle, false);
			if (count > corner) {
				error = reference_vertex(p_bytes, count_before(p_layout, anchor_triangle, true) + corner, first, anchor);
			} else {
				anchor = first + corner - count;
				error = TopologyCodecError::OK;
			}
		} else {
			error = head_at(p_layout, p_bytes, anchor_triangle - 1, anchor);
		}
		if (error != TopologyCodecError::OK) {
			return error;
		}
		uint32_t previous_head;
		uint32_t head;
		error = head_at(p_layout, p_bytes, p_triangle - 1, previous_head);
		if (error != TopologyCodecError::OK) {
			return error;
		}
		error = head_at(p_layout, p_bytes, p_triangle, head);
		if (error != TopologyCodecError::OK) {
			return error;
		}
		after += flag(p_layout, p_triangle, 2) == 0 ? 1u : 0u;
		candidate = left ? std::array<uint32_t, 3>{ anchor, head, previous_head } : std::array<uint32_t, 3>{ anchor, previous_head, head };
	}
	const auto error = triangle_valid(candidate, after);
	if (error != TopologyCodecError::OK) {
		return error;
	}
	r_indices = candidate;
	return TopologyCodecError::OK;
}

TopologyCodecError pack_vertex_reuse_batches(const std::vector<ClusterMaterialRange> &p_ranges,
		const std::vector<std::vector<uint8_t>> &p_batches, std::vector<uint32_t> &r_words) {
	if (!valid_batch_ranges(p_ranges) || p_batches.size() != p_ranges.size()) {
		return TopologyCodecError::INVALID_BATCHES;
	}
	size_t bits = std::max(size_t{ 3 }, p_ranges.size()) * 4;
	for (size_t range = 0; range < p_ranges.size(); ++range) {
		const auto &batches = p_batches[range];
		if (batches.empty() || batches.size() >= 16) {
			return TopologyCodecError::INVALID_BATCHES;
		}
		uint32_t count = 0;
		for (uint8_t length : batches) {
			if (length == 0 || length > 32) {
				return TopologyCodecError::INVALID_BATCHES;
			}
			count += length;
		}
		if (count != p_ranges[range].count) {
			return TopologyCodecError::INVALID_BATCHES;
		}
		bits += batches.size() * 5;
	}
	try {
		std::vector<uint32_t> candidate((bits + 31) / 32, 0);
		size_t counts = 0;
		size_t lengths = std::max(size_t{ 3 }, p_ranges.size()) * 4;
		for (const auto &range : p_batches) {
			write_word_bits(candidate, counts, static_cast<uint32_t>(range.size()), 4);
			for (uint8_t length : range) {
				write_word_bits(candidate, lengths, length - 1, 5);
			}
		}
		r_words = std::move(candidate);
		return TopologyCodecError::OK;
	} catch (const std::bad_alloc &) {
		return TopologyCodecError::MEMORY;
	}
}

TopologyCodecError unpack_vertex_reuse_batches(const std::vector<ClusterMaterialRange> &p_ranges,
		const std::vector<uint32_t> &p_words, std::vector<std::vector<uint8_t>> &r_batches) {
	if (!valid_batch_ranges(p_ranges)) {
		return TopologyCodecError::INVALID_BATCHES;
	}
	const size_t header_bits = std::max(size_t{ 3 }, p_ranges.size()) * 4;
	if (p_words.size() < (header_bits + 31) / 32 || p_words.size() > 28) {
		return TopologyCodecError::INVALID_LENGTH;
	}
	std::array<uint32_t, MAX_CLUSTER_MATERIALS> counts = {};
	size_t cursor = 0;
	size_t bits = header_bits;
	for (size_t range = 0; range < p_ranges.size(); ++range) {
		counts[range] = read_word_bits(p_words, cursor, 4);
		if (counts[range] == 0) {
			return TopologyCodecError::INVALID_BATCHES;
		}
		bits += counts[range] * 5;
	}
	if (p_words.size() != (bits + 31) / 32) {
		return TopologyCodecError::INVALID_LENGTH;
	}
	for (size_t bit = cursor; bit < header_bits; ++bit) {
		if (((p_words[bit / 32] >> (bit % 32)) & 1u) != 0) {
			return TopologyCodecError::INVALID_PADDING;
		}
	}
	for (size_t bit = bits; bit < p_words.size() * 32; ++bit) {
		if (((p_words[bit / 32] >> (bit % 32)) & 1u) != 0) {
			return TopologyCodecError::INVALID_PADDING;
		}
	}
	try {
		std::vector<std::vector<uint8_t>> candidate(p_ranges.size());
		cursor = header_bits;
		for (size_t range = 0; range < p_ranges.size(); ++range) {
			uint32_t count = 0;
			for (uint32_t batch = 0; batch < counts[range]; ++batch) {
				const uint8_t length = static_cast<uint8_t>(read_word_bits(p_words, cursor, 5) + 1);
				candidate[range].push_back(length);
				count += length;
			}
			if (count != p_ranges[range].count) {
				return TopologyCodecError::INVALID_BATCHES;
			}
		}
		r_batches = std::move(candidate);
		return TopologyCodecError::OK;
	} catch (const std::bad_alloc &) {
		return TopologyCodecError::MEMORY;
	}
}

} // namespace VirtualGeometry
