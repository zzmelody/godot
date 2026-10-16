#include "cluster_codec.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

bool valid_ranges(const std::vector<ClusterMaterialRange> &p_ranges, uint32_t p_triangles) {
	if (p_ranges.empty() || p_ranges.size() > MAX_CLUSTER_MATERIALS) {
		return false;
	}
	uint32_t end = 0;
	uint64_t used = 0;
	for (size_t index = 0; index < p_ranges.size(); ++index) {
		const auto &range = p_ranges[index];
		if (range.start != end || range.count == 0 || range.count > p_triangles - end || range.material >= MAX_CLUSTER_MATERIALS) {
			return false;
		}
		const uint64_t bit = uint64_t{ 1 } << range.material;
		if ((used & bit) != 0) {
			return false;
		}
		used |= bit;
		if (index != 0) {
			const auto &previous = p_ranges[index - 1];
			if (range.count > previous.count || (range.count == previous.count && range.material < previous.material)) {
				return false;
			}
		}
		end += range.count;
	}
	return end == p_triangles;
}

bool valid_layout(const ClusterPositionLayout &p_layout) {
	return p_layout.vertices > 0 && p_layout.vertices <= MAX_CLUSTER_VERTICES &&
			p_layout.precision >= MIN_POSITION_PRECISION && p_layout.precision <= MAX_POSITION_PRECISION &&
			std::all_of(p_layout.bits.begin(), p_layout.bits.end(), [](uint32_t p_bits) { return p_bits <= MAX_POSITION_BITS; });
}

uint32_t bit_width(uint32_t p_value) {
	uint32_t bits = 0;
	while (p_value != 0) {
		++bits;
		p_value >>= 1;
	}
	return bits;
}

PositionCodecError validate_positions(const QuantizedClusterPositions &p_positions) {
	const auto &layout = p_positions.layout;
	if (!valid_layout(layout) || p_positions.values.size() != layout.vertices) {
		return PositionCodecError::INVALID_LAYOUT;
	}
	for (size_t axis = 0; axis < 3; ++axis) {
		uint32_t low = UINT32_MAX;
		uint32_t high = 0;
		for (const auto &point : p_positions.values) {
			const uint32_t value = point[axis];
			if (value >= (1u << layout.bits[axis])) {
				return PositionCodecError::INVALID_VALUES;
			}
			low = std::min(low, value);
			high = std::max(high, value);
		}
		if (low != 0 || bit_width(high) != layout.bits[axis]) {
			return PositionCodecError::INVALID_VALUES;
		}
		if (static_cast<int64_t>(layout.origin[axis]) + high > INT32_MAX) {
			return PositionCodecError::COORDINATE_RANGE;
		}
	}
	return PositionCodecError::OK;
}

size_t payload_bits(const ClusterPositionLayout &p_layout) {
	return static_cast<size_t>(p_layout.vertices) * (p_layout.bits[0] + p_layout.bits[1] + p_layout.bits[2]);
}

} // namespace

MaterialCodecError pack_cluster_materials(const std::vector<ClusterMaterialRange> &p_ranges, uint32_t p_triangles,
		uint32_t p_table_offset, PackedClusterMaterials &r_packed) {
	if (p_triangles == 0 || p_triangles > MAX_CLUSTER_TRIANGLES) {
		return MaterialCodecError::INVALID_TRIANGLE_COUNT;
	}
	if (!valid_ranges(p_ranges, p_triangles)) {
		return MaterialCodecError::INVALID_RANGES;
	}
	if (p_ranges.size() > 3 && p_table_offset >= (1u << 19)) {
		return MaterialCodecError::TABLE_OFFSET_LIMIT;
	}
	try {
		PackedClusterMaterials candidate;
		if (p_ranges.size() <= 3) {
			candidate.info = p_ranges[0].material | ((p_ranges[0].count - 1) << 18);
			if (p_ranges.size() >= 2) {
				candidate.info |= (p_ranges[1].material << 6) | (p_ranges[1].count << 25);
			}
			if (p_ranges.size() == 3) {
				candidate.info |= p_ranges[2].material << 12;
			}
		} else {
			candidate.info = 0xfe000000u | (static_cast<uint32_t>(p_ranges.size() - 1) << 19) | p_table_offset;
			for (const auto &range : p_ranges) {
				candidate.table.push_back(range.start | (range.count << 8) | (range.material << 16));
			}
		}
		r_packed = std::move(candidate);
		return MaterialCodecError::OK;
	} catch (const std::bad_alloc &) {
		return MaterialCodecError::MEMORY;
	}
}

MaterialCodecError unpack_cluster_materials(uint32_t p_info, const std::vector<uint32_t> &p_table,
		uint32_t p_triangles, std::vector<ClusterMaterialRange> &r_ranges) {
	if (p_triangles == 0 || p_triangles > MAX_CLUSTER_TRIANGLES) {
		return MaterialCodecError::INVALID_TRIANGLE_COUNT;
	}
	try {
		std::vector<ClusterMaterialRange> candidate;
		if ((p_info >> 25) != 127) {
			const uint32_t first_count = ((p_info >> 18) & 127) + 1;
			const uint32_t second_count = p_info >> 25;
			if (first_count > p_triangles || second_count > p_triangles - first_count) {
				return MaterialCodecError::INVALID_RANGES;
			}
			candidate.push_back({ 0, first_count, p_info & 63 });
			if (second_count != 0) {
				candidate.push_back({ first_count, second_count, (p_info >> 6) & 63 });
			} else if (((p_info >> 6) & 63) != 0) {
				return MaterialCodecError::INVALID_RANGES;
			}
			const uint32_t third_count = p_triangles - first_count - second_count;
			if (third_count != 0) {
				if (second_count == 0) {
					return MaterialCodecError::INVALID_RANGES;
				}
				candidate.push_back({ first_count + second_count, third_count, (p_info >> 12) & 63 });
			} else if (((p_info >> 12) & 63) != 0) {
				return MaterialCodecError::INVALID_RANGES;
			}
		} else {
			const uint32_t offset = p_info & ((1u << 19) - 1);
			const uint32_t count = ((p_info >> 19) & 63) + 1;
			if (count <= 3 || offset > p_table.size() || count > p_table.size() - offset) {
				return MaterialCodecError::INVALID_TABLE;
			}
			for (uint32_t index = 0; index < count; ++index) {
				const uint32_t packed = p_table[offset + index];
				if ((packed >> 22) != 0) {
					return MaterialCodecError::INVALID_TABLE;
				}
				candidate.push_back({ packed & 255, (packed >> 8) & 255, (packed >> 16) & 63 });
			}
		}
		if (!valid_ranges(candidate, p_triangles)) {
			return MaterialCodecError::INVALID_RANGES;
		}
		r_ranges = std::move(candidate);
		return MaterialCodecError::OK;
	} catch (const std::bad_alloc &) {
		return MaterialCodecError::MEMORY;
	}
}

PositionCodecError quantize_cluster_positions(const std::vector<std::array<float, 3>> &p_positions,
		int32_t p_precision, QuantizedClusterPositions &r_positions) {
	ClusterPositionLayout layout;
	if (p_positions.size() > MAX_CLUSTER_VERTICES) {
		return PositionCodecError::INVALID_LAYOUT;
	}
	layout.vertices = static_cast<uint32_t>(p_positions.size());
	layout.precision = p_precision;
	if (!valid_layout(layout)) {
		return PositionCodecError::INVALID_LAYOUT;
	}
	try {
		std::vector<std::array<int32_t, 3>> absolute(p_positions.size());
		std::array<int32_t, 3> low{ INT32_MAX, INT32_MAX, INT32_MAX };
		std::array<int32_t, 3> high{ INT32_MIN, INT32_MIN, INT32_MIN };
		for (size_t vertex = 0; vertex < p_positions.size(); ++vertex) {
			for (size_t axis = 0; axis < 3; ++axis) {
				const float position = p_positions[vertex][axis];
				if (!std::isfinite(position)) {
					return PositionCodecError::NON_FINITE;
				}
				const float scaled = std::ldexp(position, p_precision);
				const double rounded = std::floor(static_cast<double>(scaled + 0.5f));
				if (!std::isfinite(rounded) || rounded < INT32_MIN || rounded > INT32_MAX) {
					return PositionCodecError::COORDINATE_RANGE;
				}
				const int32_t coordinate = static_cast<int32_t>(rounded);
				absolute[vertex][axis] = coordinate;
				low[axis] = std::min(low[axis], coordinate);
				high[axis] = std::max(high[axis], coordinate);
			}
		}
		for (size_t axis = 0; axis < 3; ++axis) {
			const int64_t extent = static_cast<int64_t>(high[axis]) - low[axis];
			if (extent >= (1 << MAX_POSITION_BITS)) {
				return PositionCodecError::COORDINATE_RANGE;
			}
			layout.bits[axis] = bit_width(static_cast<uint32_t>(extent));
		}
		layout.origin = low;
		QuantizedClusterPositions candidate;
		candidate.layout = layout;
		for (const auto &point : absolute) {
			candidate.values.push_back({ static_cast<uint32_t>(static_cast<int64_t>(point[0]) - low[0]),
					static_cast<uint32_t>(static_cast<int64_t>(point[1]) - low[1]),
					static_cast<uint32_t>(static_cast<int64_t>(point[2]) - low[2]) });
		}
		r_positions = std::move(candidate);
		return PositionCodecError::OK;
	} catch (const std::bad_alloc &) {
		return PositionCodecError::MEMORY;
	}
}

PositionCodecError pack_cluster_positions(const QuantizedClusterPositions &p_positions, std::vector<uint8_t> &r_bytes) {
	const auto valid = validate_positions(p_positions);
	if (valid != PositionCodecError::OK) {
		return valid;
	}
	try {
		std::vector<uint8_t> bytes((payload_bits(p_positions.layout) + 7) / 8, 0);
		size_t cursor = 0;
		for (const auto &point : p_positions.values) {
			for (size_t axis = 0; axis < 3; ++axis) {
				for (uint32_t bit = 0; bit < p_positions.layout.bits[axis]; ++bit, ++cursor) {
					bytes[cursor / 8] |= static_cast<uint8_t>(((point[axis] >> bit) & 1u) << (cursor % 8));
				}
			}
		}
		r_bytes = std::move(bytes);
		return PositionCodecError::OK;
	} catch (const std::bad_alloc &) {
		return PositionCodecError::MEMORY;
	}
}

PositionCodecError unpack_cluster_positions(const ClusterPositionLayout &p_layout, const uint8_t *p_bytes,
		size_t p_size, QuantizedClusterPositions &r_positions) {
	if (!valid_layout(p_layout)) {
		return PositionCodecError::INVALID_LAYOUT;
	}
	const size_t bits = payload_bits(p_layout);
	if (p_size != (bits + 7) / 8 || (p_size != 0 && p_bytes == nullptr)) {
		return PositionCodecError::INVALID_LENGTH;
	}
	if (bits % 8 != 0 && (p_bytes[p_size - 1] >> (bits % 8)) != 0) {
		return PositionCodecError::INVALID_PADDING;
	}
	try {
		QuantizedClusterPositions candidate;
		candidate.layout = p_layout;
		candidate.values.resize(p_layout.vertices);
		size_t cursor = 0;
		for (auto &point : candidate.values) {
			for (size_t axis = 0; axis < 3; ++axis) {
				for (uint32_t bit = 0; bit < p_layout.bits[axis]; ++bit, ++cursor) {
					point[axis] |= static_cast<uint32_t>((p_bytes[cursor / 8] >> (cursor % 8)) & 1u) << bit;
				}
			}
		}
		const auto valid = validate_positions(candidate);
		if (valid != PositionCodecError::OK) {
			return valid;
		}
		r_positions = std::move(candidate);
		return PositionCodecError::OK;
	} catch (const std::bad_alloc &) {
		return PositionCodecError::MEMORY;
	}
}

PositionCodecError reconstruct_cluster_positions(const QuantizedClusterPositions &p_positions,
		std::vector<std::array<float, 3>> &r_positions) {
	const auto valid = validate_positions(p_positions);
	if (valid != PositionCodecError::OK) {
		return valid;
	}
	try {
		std::vector<std::array<float, 3>> candidate;
		for (const auto &point : p_positions.values) {
			std::array<float, 3> position;
			for (size_t axis = 0; axis < 3; ++axis) {
				const int64_t coordinate = static_cast<int64_t>(p_positions.layout.origin[axis]) + point[axis];
				position[axis] = std::ldexp(static_cast<float>(coordinate), -p_positions.layout.precision);
			}
			candidate.push_back(position);
		}
		r_positions = std::move(candidate);
		return PositionCodecError::OK;
	} catch (const std::bad_alloc &) {
		return PositionCodecError::MEMORY;
	}
}

} // namespace VirtualGeometry
