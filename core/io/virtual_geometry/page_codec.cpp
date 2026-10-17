#include "page_codec.h"

#include "../virtual_geometry_page_directory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

constexpr size_t MAX_DISK_BYTES = STREAMING_PAGE_GPU_CAPACITY * 2;
constexpr uint32_t SLOW_MATERIAL_MASK = 0x7ffff;
using Record = std::array<uint32_t, GEOMETRY_PAGE_CLUSTER_BYTES / 4>;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

uint32_t read_word(const uint8_t *p_bytes, size_t p_offset) {
	uint32_t word = 0;
	for (uint32_t byte = 0; byte < 4; ++byte) {
		word |= static_cast<uint32_t>(p_bytes[p_offset + byte]) << (byte * 8);
	}
	return word;
}

void write_word(std::vector<uint8_t> &r_bytes, size_t p_offset, uint32_t p_word) {
	for (uint32_t byte = 0; byte < 4; ++byte) {
		r_bytes[p_offset + byte] = static_cast<uint8_t>(p_word >> (byte * 8));
	}
}

uint32_t float_word(float p_value) {
	uint32_t word;
	std::memcpy(&word, &p_value, 4);
	return word;
}

float word_float(uint32_t p_word) {
	float value;
	std::memcpy(&value, &p_word, 4);
	return value;
}

int32_t word_signed(uint32_t p_word) {
	int32_t value;
	std::memcpy(&value, &p_word, 4);
	return value;
}

size_t aligned(size_t p_bytes) {
	return (p_bytes + 3) & ~size_t{ 3 };
}

size_t gpu_index_bytes(uint32_t p_vertices, uint32_t p_triangles) {
	uint32_t bits = 0;
	for (uint32_t remaining = p_vertices - 1; remaining != 0; remaining >>= 1) {
		++bits;
	}
	return ((static_cast<size_t>(p_triangles) * (bits + 10) + 31) / 32) * 4;
}

size_t gpu_payload_bytes(const GeometryPageCluster &p_cluster) {
	return aligned(p_cluster.position_bytes.size()) + aligned(p_cluster.attributes.bytes.size()) +
			gpu_index_bytes(p_cluster.positions.vertices, p_cluster.topology.layout.triangles) +
			(p_cluster.materials.table.size() + p_cluster.material_palette.size() + p_cluster.reuse_batch_words.size()) * 4;
}

bool slow_material(uint32_t p_info) {
	return p_info >> 25 == 127;
}

bool valid_shape(const GeometryPageCluster &p_cluster) {
	return p_cluster.positions.vertices >= 3 && p_cluster.positions.vertices <= MAX_CLUSTER_VERTICES &&
			p_cluster.attributes.layout.vertices == p_cluster.positions.vertices &&
			p_cluster.topology.layout.vertices == p_cluster.positions.vertices &&
			p_cluster.topology.layout.triangles >= 1 && p_cluster.topology.layout.triangles <= MAX_CLUSTER_TRIANGLES &&
			p_cluster.position_bytes.size() <= 2016 && p_cluster.attributes.bytes.size() <= 10592 &&
			p_cluster.topology.references.size() <= 240 && p_cluster.materials.table.size() <= MAX_CLUSTER_MATERIALS &&
			!p_cluster.material_palette.empty() && p_cluster.material_palette.size() <= MAX_CLUSTER_MATERIALS &&
			p_cluster.reuse_batch_words.size() <= 28;
}

PageCodecError validate_cluster(const GeometryPageCluster &p_cluster) {
	if (!valid_shape(p_cluster)) {
		return PageCodecError::INVALID_LAYOUT;
	}
	if (p_cluster.cluster == UINT32_MAX || p_cluster.group == UINT32_MAX || (p_cluster.flags & ~15u) != 0 ||
			!std::isfinite(p_cluster.lod_error) || p_cluster.lod_error < 0 ||
			!std::isfinite(p_cluster.position_error) || p_cluster.position_error < 0) {
		return PageCodecError::INVALID_METADATA;
	}
	for (uint32_t axis = 0; axis < 4; ++axis) {
		if (!std::isfinite(p_cluster.bounds[axis]) || !std::isfinite(p_cluster.lod_bounds[axis]) ||
				(axis == 3 && (p_cluster.bounds[axis] < 0 || p_cluster.lod_bounds[axis] < 0))) {
			return PageCodecError::INVALID_METADATA;
		}
	}
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(p_cluster.lower[axis]) || !std::isfinite(p_cluster.upper[axis]) || p_cluster.lower[axis] > p_cluster.upper[axis]) {
			return PageCodecError::INVALID_METADATA;
		}
	}
	QuantizedClusterPositions quantized;
	std::vector<std::array<float, 3>> positions;
	if (unpack_cluster_positions(p_cluster.positions, p_cluster.position_bytes.data(), p_cluster.position_bytes.size(), quantized) != PositionCodecError::OK ||
			reconstruct_cluster_positions(quantized, positions) != PositionCodecError::OK) {
		return PageCodecError::POSITIONS;
	}
	for (const auto &position : positions) {
		for (uint32_t axis = 0; axis < 3; ++axis) {
			if (position[axis] < p_cluster.lower[axis] || position[axis] > p_cluster.upper[axis]) {
				return PageCodecError::INVALID_METADATA;
			}
		}
		if (std::hypot(static_cast<double>(position[0]) - p_cluster.bounds[0], static_cast<double>(position[1]) - p_cluster.bounds[1],
					static_cast<double>(position[2]) - p_cluster.bounds[2]) > p_cluster.bounds[3]) {
			return PageCodecError::INVALID_METADATA;
		}
	}
	std::vector<float> attributes;
	if (unpack_cluster_attributes(p_cluster.attributes.layout, p_cluster.attributes.bytes.data(), p_cluster.attributes.bytes.size(), attributes).error != AttributeCodecError::OK) {
		return PageCodecError::ATTRIBUTES;
	}
	std::vector<uint32_t> indices;
	if (unpack_cluster_topology(p_cluster.topology.layout, p_cluster.topology.references.data(), p_cluster.topology.references.size(), indices) != TopologyCodecError::OK) {
		return PageCodecError::TOPOLOGY;
	}
	std::vector<ClusterMaterialRange> ranges;
	if ((slow_material(p_cluster.materials.info) && (p_cluster.materials.info & SLOW_MATERIAL_MASK) != 0) ||
			unpack_cluster_materials(p_cluster.materials.info, p_cluster.materials.table, p_cluster.topology.layout.triangles, ranges) != MaterialCodecError::OK ||
			(!slow_material(p_cluster.materials.info) && !p_cluster.materials.table.empty()) ||
			(slow_material(p_cluster.materials.info) && p_cluster.materials.table.size() != ranges.size()) || ranges.size() != p_cluster.material_palette.size()) {
		return PageCodecError::MATERIALS;
	}
	for (size_t material = 0; material < p_cluster.material_palette.size(); ++material) {
		if ((material != 0 && p_cluster.material_palette[material - 1] >= p_cluster.material_palette[material]) || ranges[material].material >= p_cluster.material_palette.size()) {
			return PageCodecError::MATERIALS;
		}
	}
	std::vector<std::vector<uint8_t>> batches;
	if (unpack_vertex_reuse_batches(ranges, p_cluster.reuse_batch_words, batches) != TopologyCodecError::OK) {
		return PageCodecError::REUSE_BATCHES;
	}
	for (size_t range = 0; range < ranges.size(); ++range) {
		size_t first = ranges[range].start;
		for (uint8_t triangles : batches[range]) {
			std::array<bool, MAX_CLUSTER_VERTICES> used = {};
			uint32_t unique = 0;
			for (size_t index = first * 3; index < (first + triangles) * 3; ++index) {
				if (!used[indices[index]]) {
					used[indices[index]] = true;
					++unique;
				}
			}
			if (unique > 32) {
				return PageCodecError::REUSE_BATCHES;
			}
			first += triangles;
		}
	}
	return PageCodecError::OK;
}

Record make_record(const GeometryPageCluster &p_cluster) {
	Record record = {};
	record[0] = p_cluster.cluster;
	record[1] = p_cluster.group;
	record[2] = p_cluster.generating_group;
	record[3] = p_cluster.level;
	record[4] = p_cluster.positions.vertices;
	record[5] = p_cluster.topology.layout.triangles;
	record[6] = static_cast<uint32_t>(p_cluster.material_palette.size());
	record[7] = p_cluster.flags;
	for (uint32_t axis = 0; axis < 4; ++axis) {
		record[8 + axis] = float_word(p_cluster.bounds[axis]);
		record[12 + axis] = float_word(p_cluster.lod_bounds[axis]);
		record[39] |= p_cluster.attributes.layout.color_min[axis] << (axis * 8);
		record[40] |= p_cluster.attributes.layout.color_bits[axis] << (axis * 4);
	}
	record[16] = float_word(p_cluster.lod_error);
	record[17] = float_word(p_cluster.position_error);
	for (uint32_t axis = 0; axis < 3; ++axis) {
		record[18 + axis] = float_word(p_cluster.lower[axis]);
		record[21 + axis] = float_word(p_cluster.upper[axis]);
		record[25 + axis] = static_cast<uint32_t>(p_cluster.positions.origin[axis]);
		record[28 + axis] = p_cluster.positions.bits[axis];
	}
	record[24] = static_cast<uint32_t>(p_cluster.positions.precision);
	const auto &format = p_cluster.attributes.layout.format;
	record[33] = format.normal_bits;
	record[34] = format.tangent_bits;
	record[35] = (format.has_tangents ? 1u : 0u) | (format.has_colors ? 2u : 0u) | (format.lossless_uvs ? 4u : 0u);
	record[36] = format.uv_sets;
	for (uint32_t uv = 0; uv < 4; ++uv) {
		for (uint32_t axis = 0; axis < 2; ++axis) {
			record[41 + uv * 2 + axis] = (p_cluster.attributes.layout.uv_min[uv][axis] << 6) | p_cluster.attributes.layout.uv_bits[uv][axis];
		}
		for (uint32_t mask = 0; mask < 3; ++mask) {
			record[49 + uv * 3 + mask] = p_cluster.topology.layout.masks[uv][mask];
		}
	}
	record[61] = p_cluster.topology.layout.new_vertices_before;
	record[62] = p_cluster.topology.layout.references_before;
	record[65] = p_cluster.materials.info;
	return record;
}

void write_bytes(std::vector<uint8_t> &r_bytes, size_t &r_cursor, Record &r_record, uint32_t p_offset_word, uint32_t p_size_word, const std::vector<uint8_t> &p_payload) {
	r_record[p_offset_word] = static_cast<uint32_t>(r_cursor);
	r_record[p_size_word] = static_cast<uint32_t>(p_payload.size());
	std::copy(p_payload.begin(), p_payload.end(), r_bytes.begin() + r_cursor);
	r_cursor += aligned(p_payload.size());
}

void write_words(std::vector<uint8_t> &r_bytes, size_t &r_cursor, const std::vector<uint32_t> &p_words) {
	for (uint32_t word : p_words) {
		write_word(r_bytes, r_cursor, word);
		r_cursor += 4;
	}
}

PageCodecResult write_page(const GeometryPage &p_page, bool p_gpu, const GeometryPageSizes &p_sizes, std::vector<uint8_t> &r_bytes) {
	std::vector<uint8_t> candidate(p_gpu ? p_sizes.gpu_bytes : p_sizes.disk_bytes, 0);
	write_word(candidate, 0, p_gpu ? GEOMETRY_GPU_PAGE_MAGIC : GEOMETRY_DISK_PAGE_MAGIC);
	write_word(candidate, 4, GEOMETRY_PAGE_VERSION);
	write_word(candidate, 8, p_page.root ? PAGE_FLAG_ROOT : 0);
	write_word(candidate, 12, static_cast<uint32_t>(p_page.clusters.size()));
	write_word(candidate, 16, static_cast<uint32_t>(candidate.size()));
	write_word(candidate, 20, p_sizes.gpu_bytes);
	write_word(candidate, 24, GEOMETRY_PAGE_HEADER_BYTES);
	size_t cursor = GEOMETRY_PAGE_HEADER_BYTES + p_page.clusters.size() * GEOMETRY_PAGE_CLUSTER_BYTES;
	size_t gpu_cursor = cursor;
	for (size_t cluster = 0; cluster < p_page.clusters.size(); ++cluster) {
		const auto &source = p_page.clusters[cluster];
		Record record = make_record(source);
		// The installer dispatches independent cluster workgroups. Store the
		// canonical destination once offline, rather than prefix-scanning all
		// earlier clusters on every GPU installation. GPU records keep zero.
		record[71] = p_gpu ? 0 : static_cast<uint32_t>(gpu_cursor);
		gpu_cursor += gpu_payload_bytes(source);
		write_bytes(candidate, cursor, record, 31, 32, source.position_bytes);
		write_bytes(candidate, cursor, record, 37, 38, source.attributes.bytes);
		if (p_gpu) {
			std::vector<uint32_t> indices;
			std::vector<uint8_t> gpu_indices;
			if (unpack_cluster_topology(source.topology.layout, source.topology.references.data(), source.topology.references.size(), indices) != TopologyCodecError::OK ||
					pack_cluster_gpu_indices(indices, source.positions.vertices, gpu_indices) != TopologyCodecError::OK) {
				return { PageCodecError::TOPOLOGY, static_cast<uint32_t>(cluster) };
			}
			write_bytes(candidate, cursor, record, 63, 64, gpu_indices);
		} else {
			write_bytes(candidate, cursor, record, 63, 64, source.topology.references);
		}
		record[66] = static_cast<uint32_t>(cursor);
		record[67] = static_cast<uint32_t>(source.materials.table.size());
		if (slow_material(record[65])) {
			record[65] = (record[65] & ~SLOW_MATERIAL_MASK) | static_cast<uint32_t>(cursor / 4);
		}
		write_words(candidate, cursor, source.materials.table);
		record[68] = static_cast<uint32_t>(cursor);
		write_words(candidate, cursor, source.material_palette);
		record[69] = static_cast<uint32_t>(cursor);
		record[70] = static_cast<uint32_t>(source.reuse_batch_words.size());
		write_words(candidate, cursor, source.reuse_batch_words);
		for (size_t word = 0; word < record.size(); ++word) {
			write_word(candidate, GEOMETRY_PAGE_HEADER_BYTES + cluster * GEOMETRY_PAGE_CLUSTER_BYTES + word * 4, record[word]);
		}
	}
	if (cursor != candidate.size() || gpu_cursor != p_sizes.gpu_bytes) {
		return { PageCodecError::INVALID_LENGTH };
	}
	r_bytes = std::move(candidate);
	return {};
}

bool read_bytes(const uint8_t *p_bytes, size_t p_size, size_t &r_cursor, uint32_t p_offset, uint32_t p_length, std::vector<uint8_t> &r_bytes) {
	if (p_offset != r_cursor || r_cursor > p_size || aligned(p_length) > p_size - r_cursor) {
		return false;
	}
	for (size_t byte = p_length; byte < aligned(p_length); ++byte) {
		if (p_bytes[r_cursor + byte] != 0) {
			return false;
		}
	}
	r_bytes.assign(p_bytes + r_cursor, p_bytes + r_cursor + p_length);
	r_cursor += aligned(p_length);
	return true;
}

bool read_words(const uint8_t *p_bytes, size_t p_size, size_t &r_cursor, uint32_t p_offset, uint32_t p_count, std::vector<uint32_t> &r_words) {
	if (p_offset != r_cursor || r_cursor > p_size || p_count > (p_size - r_cursor) / 4) {
		return false;
	}
	r_words.resize(p_count);
	for (uint32_t &word : r_words) {
		word = read_word(p_bytes, r_cursor);
		r_cursor += 4;
	}
	return true;
}

PageCodecError read_cluster(const Record &p_record, const uint8_t *p_bytes, size_t p_size, size_t &r_cursor, GeometryPageCluster &r_cluster) {
	if ((p_record[7] & ~15u) != 0 || (p_record[35] & ~7u) != 0 || (p_record[40] >> 16) != 0 ||
			p_record[4] < 3 || p_record[4] > MAX_CLUSTER_VERTICES || p_record[5] == 0 || p_record[5] > MAX_CLUSTER_TRIANGLES ||
			p_record[6] == 0 || p_record[6] > MAX_CLUSTER_MATERIALS || p_record[32] > 2016 || p_record[38] > 10592 ||
			p_record[64] > 240 || p_record[67] > MAX_CLUSTER_MATERIALS || p_record[70] > 28) {
		return PageCodecError::INVALID_LAYOUT;
	}
	r_cluster.cluster = p_record[0];
	r_cluster.group = p_record[1];
	r_cluster.generating_group = p_record[2];
	r_cluster.level = p_record[3];
	r_cluster.flags = p_record[7];
	r_cluster.positions.vertices = p_record[4];
	r_cluster.attributes.layout.vertices = p_record[4];
	r_cluster.topology.layout.vertices = p_record[4];
	r_cluster.topology.layout.triangles = p_record[5];
	for (uint32_t axis = 0; axis < 4; ++axis) {
		r_cluster.bounds[axis] = word_float(p_record[8 + axis]);
		r_cluster.lod_bounds[axis] = word_float(p_record[12 + axis]);
		r_cluster.attributes.layout.color_min[axis] = (p_record[39] >> (axis * 8)) & 255;
		r_cluster.attributes.layout.color_bits[axis] = (p_record[40] >> (axis * 4)) & 15;
	}
	r_cluster.lod_error = word_float(p_record[16]);
	r_cluster.position_error = word_float(p_record[17]);
	for (uint32_t axis = 0; axis < 3; ++axis) {
		r_cluster.lower[axis] = word_float(p_record[18 + axis]);
		r_cluster.upper[axis] = word_float(p_record[21 + axis]);
		r_cluster.positions.origin[axis] = word_signed(p_record[25 + axis]);
		r_cluster.positions.bits[axis] = p_record[28 + axis];
	}
	r_cluster.positions.precision = word_signed(p_record[24]);
	auto &format = r_cluster.attributes.layout.format;
	format.normal_bits = p_record[33];
	format.tangent_bits = p_record[34];
	format.has_tangents = (p_record[35] & 1) != 0;
	format.has_colors = (p_record[35] & 2) != 0;
	format.lossless_uvs = (p_record[35] & 4) != 0;
	format.uv_sets = p_record[36];
	for (uint32_t uv = 0; uv < 4; ++uv) {
		for (uint32_t axis = 0; axis < 2; ++axis) {
			const uint32_t word = p_record[41 + uv * 2 + axis];
			if ((word >> 26) != 0) {
				return PageCodecError::INVALID_LAYOUT;
			}
			r_cluster.attributes.layout.uv_min[uv][axis] = word >> 6;
			r_cluster.attributes.layout.uv_bits[uv][axis] = word & 63;
		}
		for (uint32_t mask = 0; mask < 3; ++mask) {
			r_cluster.topology.layout.masks[uv][mask] = p_record[49 + uv * 3 + mask];
		}
	}
	r_cluster.topology.layout.new_vertices_before = p_record[61];
	r_cluster.topology.layout.references_before = p_record[62];
	if (!read_bytes(p_bytes, p_size, r_cursor, p_record[31], p_record[32], r_cluster.position_bytes) ||
			!read_bytes(p_bytes, p_size, r_cursor, p_record[37], p_record[38], r_cluster.attributes.bytes) ||
			!read_bytes(p_bytes, p_size, r_cursor, p_record[63], p_record[64], r_cluster.topology.references) ||
			!read_words(p_bytes, p_size, r_cursor, p_record[66], p_record[67], r_cluster.materials.table) ||
			!read_words(p_bytes, p_size, r_cursor, p_record[68], p_record[6], r_cluster.material_palette) ||
			!read_words(p_bytes, p_size, r_cursor, p_record[69], p_record[70], r_cluster.reuse_batch_words)) {
		return PageCodecError::INVALID_OFFSET;
	}
	r_cluster.materials.info = p_record[65];
	if (slow_material(p_record[65])) {
		if ((p_record[65] & SLOW_MATERIAL_MASK) != p_record[66] / 4) {
			return PageCodecError::MATERIALS;
		}
		r_cluster.materials.info &= ~SLOW_MATERIAL_MASK;
	}
	return validate_cluster(r_cluster);
}

} // namespace

PageCodecResult measure_geometry_page(const GeometryPage &p_page, GeometryPageSizes &r_sizes) {
	if (p_page.clusters.empty()) {
		return { PageCodecError::INVALID_LAYOUT };
	}
	if (p_page.clusters.size() > (p_page.root ? 64u : 256u)) {
		return { PageCodecError::CAPACITY };
	}
	size_t disk = GEOMETRY_PAGE_HEADER_BYTES + p_page.clusters.size() * GEOMETRY_PAGE_CLUSTER_BYTES;
	size_t gpu = disk;
	for (size_t index = 0; index < p_page.clusters.size(); ++index) {
		const auto &cluster = p_page.clusters[index];
		if (!valid_shape(cluster)) {
			return { PageCodecError::INVALID_LAYOUT, static_cast<uint32_t>(index) };
		}
		const size_t common = aligned(cluster.position_bytes.size()) + aligned(cluster.attributes.bytes.size()) +
				(cluster.materials.table.size() + cluster.material_palette.size() + cluster.reuse_batch_words.size()) * 4;
		disk += common + aligned(cluster.topology.references.size());
		gpu += gpu_payload_bytes(cluster);
	}
	if (disk > MAX_DISK_BYTES || gpu > (p_page.root ? ROOT_PAGE_GPU_CAPACITY : STREAMING_PAGE_GPU_CAPACITY)) {
		return { PageCodecError::CAPACITY };
	}
	r_sizes = { static_cast<uint32_t>(disk), static_cast<uint32_t>(gpu) };
	return {};
}

PageCodecResult encode_geometry_page(const GeometryPage &p_page, std::vector<uint8_t> &r_bytes) {
	GeometryPageSizes sizes;
	const auto measured = measure_geometry_page(p_page, sizes);
	if (measured.error != PageCodecError::OK) {
		return measured;
	}
	try {
		for (size_t cluster = 0; cluster < p_page.clusters.size(); ++cluster) {
			const auto valid = validate_cluster(p_page.clusters[cluster]);
			if (valid != PageCodecError::OK) {
				return { valid, static_cast<uint32_t>(cluster) };
			}
			for (size_t previous = 0; previous < cluster; ++previous) {
				if (p_page.clusters[previous].cluster == p_page.clusters[cluster].cluster) {
					return { PageCodecError::INVALID_METADATA, static_cast<uint32_t>(cluster) };
				}
			}
		}
		return write_page(p_page, false, sizes, r_bytes);
	} catch (const std::bad_alloc &) {
		return { PageCodecError::MEMORY };
	}
}

PageCodecResult decode_geometry_page(const uint8_t *p_bytes, size_t p_size, GeometryPage &r_page) {
	if (p_bytes == nullptr || p_size < GEOMETRY_PAGE_HEADER_BYTES || p_size > MAX_DISK_BYTES ||
			read_word(p_bytes, 0) != GEOMETRY_DISK_PAGE_MAGIC || read_word(p_bytes, 4) != GEOMETRY_PAGE_VERSION ||
			read_word(p_bytes, 8) > PAGE_FLAG_ROOT || read_word(p_bytes, 24) != GEOMETRY_PAGE_HEADER_BYTES || read_word(p_bytes, 28) != 0) {
		return { PageCodecError::INVALID_HEADER };
	}
	const bool root = read_word(p_bytes, 8) == PAGE_FLAG_ROOT;
	const uint32_t count = read_word(p_bytes, 12);
	const uint32_t gpu_bytes = read_word(p_bytes, 20);
	if (count == 0 || count > (root ? 64u : 256u) || gpu_bytes > (root ? ROOT_PAGE_GPU_CAPACITY : STREAMING_PAGE_GPU_CAPACITY)) {
		return { PageCodecError::CAPACITY };
	}
	size_t cursor = GEOMETRY_PAGE_HEADER_BYTES + static_cast<size_t>(count) * GEOMETRY_PAGE_CLUSTER_BYTES;
	size_t gpu_cursor = cursor;
	if (read_word(p_bytes, 16) != p_size || cursor > p_size) {
		return { PageCodecError::INVALID_LENGTH };
	}
	try {
		GeometryPage candidate;
		candidate.root = root;
		candidate.clusters.resize(count);
		for (uint32_t cluster = 0; cluster < count; ++cluster) {
			Record record;
			for (size_t word = 0; word < record.size(); ++word) {
				record[word] = read_word(p_bytes, GEOMETRY_PAGE_HEADER_BYTES + cluster * GEOMETRY_PAGE_CLUSTER_BYTES + word * 4);
			}
			const auto valid = read_cluster(record, p_bytes, p_size, cursor, candidate.clusters[cluster]);
			if (valid != PageCodecError::OK) {
				return { valid, cluster };
			}
			if (record[71] != gpu_cursor) {
				return { PageCodecError::INVALID_OFFSET, cluster };
			}
			gpu_cursor += gpu_payload_bytes(candidate.clusters[cluster]);
			for (uint32_t previous = 0; previous < cluster; ++previous) {
				if (candidate.clusters[previous].cluster == candidate.clusters[cluster].cluster) {
					return { PageCodecError::INVALID_METADATA, cluster };
				}
			}
		}
		GeometryPageSizes sizes;
		const auto measured = measure_geometry_page(candidate, sizes);
		if (measured.error != PageCodecError::OK) {
			return measured;
		}
		if (cursor != p_size || sizes.disk_bytes != p_size || sizes.gpu_bytes != gpu_bytes || gpu_cursor != gpu_bytes) {
			return { PageCodecError::INVALID_LENGTH };
		}
		r_page = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { PageCodecError::MEMORY };
	}
}

PageCodecResult transcode_geometry_page_cpu(const uint8_t *p_bytes, size_t p_size, std::vector<uint8_t> &r_gpu_bytes) {
	GeometryPage page;
	const auto decoded = decode_geometry_page(p_bytes, p_size, page);
	if (decoded.error != PageCodecError::OK) {
		return decoded;
	}
	GeometryPageSizes sizes;
	const auto measured = measure_geometry_page(page, sizes);
	if (measured.error != PageCodecError::OK) {
		return measured;
	}
	try {
		return write_page(page, true, sizes, r_gpu_bytes);
	} catch (const std::bad_alloc &) {
		return { PageCodecError::MEMORY };
	}
}

} // namespace VirtualGeometry
