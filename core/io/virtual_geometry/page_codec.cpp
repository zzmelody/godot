#include "page_codec.h"

#include "../virtual_geometry_page_directory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

constexpr size_t MAX_DISK_BYTES = STREAMING_PAGE_GPU_CAPACITY * 4;
constexpr uint32_t SLOW_MATERIAL_MASK = 0x7ffff;
using Record = std::array<uint32_t, GEOMETRY_PAGE_CLUSTER_BYTES / 4>;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
Record make_record(const GeometryPageCluster &p_cluster);

struct VertexChannel {
	uint32_t bits = 0;
	uint32_t base = 0;
	uint32_t offset = 0;
	bool attribute = false;
};

struct VertexLayout {
	std::array<VertexChannel, 18> channels;
	uint32_t count = 0;
	uint32_t position_bits = 0;
	uint32_t attribute_bits = 0;
	std::array<uint32_t, 4> plane_values = {};
};

uint32_t bit_mask(uint32_t p_bits) {
	return p_bits == 32 ? UINT32_MAX : (1u << p_bits) - 1;
}

bool vertex_layout(const Record &p_record, VertexLayout &r_layout) {
	if (p_record[33] == 0 || p_record[33] > 15 || p_record[34] > 12 || p_record[36] > 4 ||
			(p_record[35] & ~7u) != 0 || ((p_record[35] & 1u) != 0 && p_record[34] == 0)) {
		return false;
	}
	auto add = [&](uint32_t p_bits, uint32_t p_base, bool p_attribute) {
		uint32_t &offset = p_attribute ? r_layout.attribute_bits : r_layout.position_bits;
		r_layout.channels[r_layout.count++] = { p_bits, p_base, offset, p_attribute };
		offset += p_bits;
		for (uint32_t plane = 0; plane < (p_bits + 7) / 8; ++plane) {
			++r_layout.plane_values[plane];
		}
	};
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (p_record[28 + axis] > MAX_POSITION_BITS) {
			return false;
		}
		add(p_record[28 + axis], p_record[25 + axis], false);
	}
	add(p_record[33], 0, true);
	add(p_record[33], 0, true);
	if ((p_record[35] & 1u) != 0) {
		add(p_record[34] + 1, 0, true);
	}
	if ((p_record[35] & 2u) != 0) {
		for (uint32_t channel = 0; channel < 4; ++channel) {
			const uint32_t bits = (p_record[40] >> (channel * 4)) & 15u;
			if (bits > 8) {
				return false;
			}
			add(bits, (p_record[39] >> (channel * 8)) & 255u, true);
		}
	}
	for (uint32_t uv = 0; uv < p_record[36] * 2; ++uv) {
		const uint32_t code = p_record[41 + uv];
		const uint32_t bits = code & 63u;
		if ((code >> 26) != 0 || bits > 32 || ((p_record[35] & 4u) != 0 ? bits != 32 || (code >> 6) != 0 : bits > 20)) {
			return false;
		}
		add(bits, code >> 6, true);
	}
	return true;
}

uint32_t get_vertex_bits(const std::vector<uint8_t> &p_bytes, uint32_t p_offset, uint32_t p_bits) {
	uint32_t value = 0;
	for (uint32_t bit = 0; bit < p_bits; ++bit) {
		value |= uint32_t((p_bytes[(p_offset + bit) / 8] >> ((p_offset + bit) & 7)) & 1u) << bit;
	}
	return value;
}

void put_vertex_bits(std::vector<uint8_t> &r_bytes, uint32_t p_offset, uint32_t p_bits, uint32_t p_value) {
	for (uint32_t bit = 0; bit < p_bits; ++bit) {
		r_bytes[(p_offset + bit) / 8] |= uint8_t(((p_value >> bit) & 1u) << ((p_offset + bit) & 7));
	}
}

struct DiskVertices {
	std::vector<uint8_t> planes;
	std::vector<uint8_t> references;
};

// Keys contain absolute integer values, not decoded/requantized floats. This
// permits references across different cluster origins and UV/color minima.
using VertexKey = std::array<uint32_t, 24>;

struct VertexAddress {
	uint32_t dependency = 0; // Zero is this page; otherwise directory ordinal + 1.
	uint16_t vertex = 0;
};

VertexKey vertex_key(const GeometryPageCluster &p_cluster, const Record &p_record, const VertexLayout &p_layout,
		uint32_t p_vertex, std::array<uint32_t, 18> &r_values) {
	VertexKey key = {};
	key[0] = p_record[24];
	for (uint32_t field = 1; field < 5; ++field) {
		key[field] = p_record[32 + field];
	}
	key[5] = p_layout.count;
	for (uint32_t channel = 0; channel < p_layout.count; ++channel) {
		const auto &description = p_layout.channels[channel];
		r_values[channel] = get_vertex_bits(description.attribute ? p_cluster.attributes.bytes : p_cluster.position_bytes,
				p_vertex * (description.attribute ? p_layout.attribute_bits : p_layout.position_bits) + description.offset, description.bits);
		key[6 + channel] = description.base + r_values[channel];
	}
	return key;
}

std::vector<DiskVertices> encode_page_vertices(const GeometryPage &p_page, const std::vector<GeometryPageDependency> &p_dependencies) {
	std::map<VertexKey, VertexAddress> coded;
	for (uint32_t dependency = 0; dependency < p_page.dependencies.size(); ++dependency) {
		for (const auto &borrowed : p_dependencies) {
			if (borrowed.page != p_page.dependencies[dependency]) {
				continue;
			}
			for (uint32_t cluster = 0; cluster < borrowed.geometry->clusters.size(); ++cluster) {
				const auto &source = borrowed.geometry->clusters[cluster];
				const Record record = make_record(source);
				VertexLayout layout;
				vertex_layout(record, layout);
				for (uint32_t vertex = 0; vertex < record[4]; ++vertex) {
					std::array<uint32_t, 18> values;
					coded.emplace(vertex_key(source, record, layout, vertex, values), VertexAddress{ dependency + 1, uint16_t((cluster << 8) | vertex) });
				}
			}
			break;
		}
	}
	std::vector<DiskVertices> output(p_page.clusters.size());
	for (uint32_t cluster = 0; cluster < p_page.clusters.size(); ++cluster) {
		const auto &source = p_page.clusters[cluster];
		const Record record = make_record(source);
		VertexLayout layout;
		vertex_layout(record, layout); // encode_geometry_page admitted the layout.
		auto &encoded = output[cluster];
		encoded.references.resize(32, 0);
		std::array<std::vector<uint8_t>, 4> planes;
		std::array<uint32_t, 18> previous = {};
		std::vector<uint32_t> dependency_references;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			previous[axis] = (1u << layout.channels[axis].bits) >> 1;
		}
		for (uint32_t vertex = 0; vertex < record[4]; ++vertex) {
			std::array<uint32_t, 18> values;
			const VertexKey key = vertex_key(source, record, layout, vertex, values);
			const auto found = coded.find(key);
			if (found != coded.end()) {
				encoded.references[vertex / 8] |= uint8_t(1u << (vertex & 7));
				// The temporary pairs are separated into byte planes below.
				encoded.references.push_back(uint8_t(found->second.vertex >> 8));
				encoded.references.push_back(uint8_t(found->second.vertex));
				dependency_references.push_back(found->second.dependency);
				continue;
			}
			coded.emplace(key, VertexAddress{ 0, uint16_t((cluster << 8) | vertex) });
			for (uint32_t channel = 0; channel < layout.count; ++channel) {
				const uint32_t bits = layout.channels[channel].bits;
				const uint32_t mask = bit_mask(bits);
				const uint32_t delta = (values[channel] - previous[channel]) & mask;
				const uint32_t sign = bits == 0 ? 0 : delta >> (bits - 1);
				const uint32_t zigzag = ((delta << 1) ^ (0u - sign)) & mask;
				for (uint32_t plane = 0; plane < (bits + 7) / 8; ++plane) {
					planes[plane].push_back(uint8_t(zigzag >> (plane * 8)));
				}
				previous[channel] = values[channel];
			}
		}
		for (const auto &plane : planes) {
			encoded.planes.insert(encoded.planes.end(), plane.begin(), plane.end());
		}
		const size_t refs = (encoded.references.size() - 32) / 2;
		std::vector<uint8_t> reference_planes(32 + refs * 6);
		std::copy_n(encoded.references.begin(), 32, reference_planes.begin());
		uint32_t previous_reference = 0;
		for (size_t index = 0; index < refs; ++index) {
			reference_planes[32 + index] = encoded.references[32 + index * 2];
			const uint32_t vertex = encoded.references[33 + index * 2];
			const uint32_t delta = (vertex - previous_reference) & 255u;
			reference_planes[32 + refs + index] = uint8_t((delta << 1) ^ (0u - (delta >> 7)));
			previous_reference = vertex;
			for (uint32_t plane = 0; plane < 4; ++plane) {
				reference_planes[32 + refs * (2 + plane) + index] = uint8_t(dependency_references[index] >> (plane * 8));
			}
		}
		encoded.references = std::move(reference_planes);
	}
	return output;
}

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

PageCodecResult write_page(const GeometryPage &p_page, bool p_gpu, const GeometryPageSizes &p_sizes, std::vector<uint8_t> &r_bytes,
		const std::vector<GeometryPageDependency> &p_dependencies = {}) {
	std::vector<uint8_t> candidate(p_gpu ? p_sizes.gpu_bytes : p_sizes.disk_bytes, 0);
	const std::vector<DiskVertices> vertices = p_gpu ? std::vector<DiskVertices>() : encode_page_vertices(p_page, p_dependencies);
	write_word(candidate, 0, p_gpu ? GEOMETRY_GPU_PAGE_MAGIC : GEOMETRY_DISK_PAGE_MAGIC);
	write_word(candidate, 4, GEOMETRY_PAGE_VERSION);
	write_word(candidate, 8, p_page.root ? PAGE_FLAG_ROOT : 0);
	write_word(candidate, 12, static_cast<uint32_t>(p_page.clusters.size()));
	write_word(candidate, 16, static_cast<uint32_t>(candidate.size()));
	write_word(candidate, 20, p_sizes.gpu_bytes);
	write_word(candidate, 24, GEOMETRY_PAGE_HEADER_BYTES);
	size_t cursor = GEOMETRY_PAGE_HEADER_BYTES + p_page.clusters.size() * GEOMETRY_PAGE_CLUSTER_BYTES;
	size_t gpu_cursor = cursor;
	if (!p_gpu) {
		write_word(candidate, 28, static_cast<uint32_t>(p_page.dependencies.size()));
		write_words(candidate, cursor, p_page.dependencies);
	}
	for (size_t cluster = 0; cluster < p_page.clusters.size(); ++cluster) {
		const auto &source = p_page.clusters[cluster];
		Record record = make_record(source);
		// The installer dispatches independent cluster workgroups. Store the
		// canonical destination once offline, rather than prefix-scanning all
		// earlier clusters on every GPU installation. GPU records keep zero.
		record[71] = p_gpu ? 0 : static_cast<uint32_t>(gpu_cursor);
		gpu_cursor += gpu_payload_bytes(source);
		if (p_gpu) {
			write_bytes(candidate, cursor, record, 31, 32, source.position_bytes);
			write_bytes(candidate, cursor, record, 37, 38, source.attributes.bytes);
			std::vector<uint32_t> indices;
			std::vector<uint8_t> gpu_indices;
			if (unpack_cluster_topology(source.topology.layout, source.topology.references.data(), source.topology.references.size(), indices) != TopologyCodecError::OK ||
					pack_cluster_gpu_indices(indices, source.positions.vertices, gpu_indices) != TopologyCodecError::OK) {
				return { PageCodecError::TOPOLOGY, static_cast<uint32_t>(cluster) };
			}
			write_bytes(candidate, cursor, record, 63, 64, gpu_indices);
		} else {
			record[32] = static_cast<uint32_t>(source.position_bytes.size());
			record[38] = static_cast<uint32_t>(source.attributes.bytes.size());
			record[31] = static_cast<uint32_t>(cursor);
			std::copy(vertices[cluster].planes.begin(), vertices[cluster].planes.end(), candidate.begin() + cursor);
			cursor += aligned(vertices[cluster].planes.size());
			record[37] = static_cast<uint32_t>(cursor);
			std::copy(vertices[cluster].references.begin(), vertices[cluster].references.end(), candidate.begin() + cursor);
			cursor += aligned(vertices[cluster].references.size());
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
	if (cursor > candidate.size() || (p_gpu && cursor != candidate.size()) || gpu_cursor != p_sizes.gpu_bytes) {
		return { PageCodecError::INVALID_LENGTH };
	}
	candidate.resize(cursor);
	write_word(candidate, 16, static_cast<uint32_t>(cursor));
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

Record disk_record(const uint8_t *p_bytes, uint32_t p_cluster) {
	Record record;
	for (uint32_t word = 0; word < record.size(); ++word) {
		record[word] = read_word(p_bytes, GEOMETRY_PAGE_HEADER_BYTES + size_t(p_cluster) * GEOMETRY_PAGE_CLUSTER_BYTES + word * 4);
	}
	return record;
}

bool is_vertex_reference(const uint8_t *p_bytes, const Record &p_record, uint32_t p_vertex) {
	return (p_bytes[p_record[37] + p_vertex / 8] & (1u << (p_vertex & 7))) != 0;
}

uint32_t reference_dependency(const uint8_t *p_bytes, const Record &p_record, uint32_t p_count, uint32_t p_reference) {
	uint32_t result = 0;
	for (uint32_t plane = 0; plane < 4; ++plane) {
		result |= uint32_t(p_bytes[p_record[37] + 32 + p_count * (2 + plane) + p_reference]) << (plane * 8);
	}
	return result;
}

bool disk_dependencies(const uint8_t *p_bytes, size_t p_size, uint32_t p_clusters, size_t &r_cursor) {
	const uint32_t count = read_word(p_bytes, 28);
	const size_t offset = GEOMETRY_PAGE_HEADER_BYTES + size_t(p_clusters) * GEOMETRY_PAGE_CLUSTER_BYTES;
	if (offset > p_size || count > (p_size - offset) / 4) {
		return false;
	}
	for (uint32_t index = 0; index < count; ++index) {
		const uint32_t page = read_word(p_bytes, offset + size_t(index) * 4);
		if (page == UINT32_MAX || (index != 0 && page <= read_word(p_bytes, offset + size_t(index - 1) * 4))) {
			return false;
		}
	}
	r_cursor = offset + size_t(count) * 4;
	return true;
}

bool disk_vertex_layout(const Record &p_record, const uint8_t *p_bytes, size_t p_size, uint32_t p_cluster,
		VertexLayout &r_layout, uint32_t &r_references, uint32_t &r_coded) {
	if (!vertex_layout(p_record, r_layout) || p_record[4] < 3 || p_record[4] > 256 ||
			p_record[32] != (p_record[4] * r_layout.position_bits + 7) / 8 ||
			p_record[38] != ((p_record[4] * r_layout.attribute_bits + 31) / 32) * 4 ||
			p_record[37] > p_size || 32 > p_size - p_record[37]) {
		return false;
	}
	r_references = 0;
	for (uint32_t vertex = 0; vertex < 256; ++vertex) {
		if (is_vertex_reference(p_bytes, p_record, vertex)) {
			if (vertex >= p_record[4]) {
				return false;
			}
			++r_references;
		}
	}
	if (r_references * 6 + 32 > p_size - p_record[37]) {
		return false;
	}
	r_coded = p_record[4] - r_references;
	uint32_t source_vertex = 0;
	uint32_t reference = 0;
	for (uint32_t vertex = 0; vertex < p_record[4]; ++vertex) {
		if (!is_vertex_reference(p_bytes, p_record, vertex)) {
			continue;
		}
		const uint32_t source_cluster = p_bytes[p_record[37] + 32 + reference];
		const uint32_t dependency = reference_dependency(p_bytes, p_record, r_references, reference);
		const uint32_t delta = p_bytes[p_record[37] + 32 + r_references + reference++];
		source_vertex = (source_vertex + ((delta >> 1) ^ (0u - (delta & 1u)))) & 255u;
		if (dependency != 0) {
			if (dependency > read_word(p_bytes, 28)) {
				return false;
			}
			continue;
		}
		if (source_cluster > p_cluster || (source_cluster == p_cluster && source_vertex >= vertex)) {
			return false;
		}
		const Record source = source_cluster == p_cluster ? p_record : disk_record(p_bytes, source_cluster);
		if (source_vertex >= source[4] || source[37] > p_size || 32 > p_size - source[37] ||
				is_vertex_reference(p_bytes, source, source_vertex) || source[24] != p_record[24] ||
				source[33] != p_record[33] || source[34] != p_record[34] || source[35] != p_record[35] || source[36] != p_record[36]) {
			return false;
		}
	}
	return true;
}

bool read_disk_vertices(const Record &p_record, const uint8_t *p_bytes, size_t p_size, size_t &r_cursor,
		uint32_t p_cluster, std::vector<GeometryPageCluster> &r_clusters,
		const std::vector<GeometryPageDependency> &p_dependencies) {
	VertexLayout layout;
	uint32_t references, coded;
	if (!disk_vertex_layout(p_record, p_bytes, p_size, p_cluster, layout, references, coded)) {
		return false;
	}
	uint32_t plane_values = 0;
	std::array<size_t, 4> cursors;
	for (uint32_t plane = 0; plane < 4; ++plane) {
		cursors[plane] = size_t(coded) * plane_values;
		plane_values += layout.plane_values[plane];
	}
	std::vector<uint8_t> planes, reference_bytes;
	if (!read_bytes(p_bytes, p_size, r_cursor, p_record[31], coded * plane_values, planes) ||
			!read_bytes(p_bytes, p_size, r_cursor, p_record[37], 32 + references * 6, reference_bytes)) {
		return false;
	}
	auto &cluster = r_clusters[p_cluster];
	cluster.position_bytes.resize(p_record[32], 0);
	cluster.attributes.bytes.resize(p_record[38], 0);
	std::array<uint32_t, 18> previous = {};
	for (uint32_t axis = 0; axis < 3; ++axis) {
		previous[axis] = (1u << layout.channels[axis].bits) >> 1;
	}
	uint32_t reference = 0;
	uint32_t source_vertex = 0;
	for (uint32_t vertex = 0; vertex < p_record[4]; ++vertex) {
		const bool referred = is_vertex_reference(p_bytes, p_record, vertex);
		uint32_t source_cluster = 0;
		const GeometryPageCluster *source_geometry = nullptr;
		VertexLayout source_layout;
		if (referred) {
			source_cluster = reference_bytes[32 + reference];
			const uint32_t dependency = reference_dependency(p_bytes, p_record, references, reference);
			const uint32_t delta = reference_bytes[32 + references + reference++];
			source_vertex = (source_vertex + ((delta >> 1) ^ (0u - (delta & 1u)))) & 255u;
			if (dependency == 0) {
				source_geometry = &r_clusters[source_cluster];
			} else {
				const size_t start = GEOMETRY_PAGE_HEADER_BYTES + size_t(read_word(p_bytes, 12)) * GEOMETRY_PAGE_CLUSTER_BYTES;
				const uint32_t page = read_word(p_bytes, start + size_t(dependency - 1) * 4);
				for (const auto &borrowed : p_dependencies) {
					if (borrowed.page == page && borrowed.geometry != nullptr &&
							(read_word(p_bytes, 8) == 0 || borrowed.geometry->root) && source_cluster < borrowed.geometry->clusters.size()) {
						source_geometry = &borrowed.geometry->clusters[source_cluster];
						break;
					}
				}
			}
			if (source_geometry == nullptr || source_vertex >= source_geometry->positions.vertices ||
					!vertex_layout(make_record(*source_geometry), source_layout) || source_layout.count != layout.count) {
				return false;
			}
			const Record source_record = make_record(*source_geometry);
			if (source_record[24] != p_record[24] || source_record[33] != p_record[33] ||
					source_record[34] != p_record[34] || source_record[35] != p_record[35] || source_record[36] != p_record[36] ||
					source_geometry->position_bytes.size() != (size_t(source_record[4]) * source_layout.position_bits + 7) / 8 ||
					source_geometry->attributes.bytes.size() != ((size_t(source_record[4]) * source_layout.attribute_bits + 31) / 32) * 4) {
				return false;
			}
		}
		for (uint32_t channel = 0; channel < layout.count; ++channel) {
			const auto &description = layout.channels[channel];
			uint32_t value;
			if (referred) {
				const auto &source = *source_geometry;
				const auto &source_channel = source_layout.channels[channel];
				value = source_channel.base + get_vertex_bits(source_channel.attribute ? source.attributes.bytes : source.position_bytes, source_vertex * (source_channel.attribute ? source_layout.attribute_bits : source_layout.position_bits) + source_channel.offset, source_channel.bits) - description.base;
				if ((value & ~bit_mask(description.bits)) != 0) {
					return false;
				}
			} else {
				uint32_t zigzag = 0;
				for (uint32_t plane = 0; plane < (description.bits + 7) / 8; ++plane) {
					zigzag |= uint32_t(planes[cursors[plane]++]) << (plane * 8);
				}
				if ((zigzag & ~bit_mask(description.bits)) != 0) {
					return false;
				}
				value = (previous[channel] + ((zigzag >> 1) ^ (0u - (zigzag & 1u)))) & bit_mask(description.bits);
				previous[channel] = value;
			}
			put_vertex_bits(description.attribute ? cluster.attributes.bytes : cluster.position_bytes,
					vertex * (description.attribute ? layout.attribute_bits : layout.position_bits) + description.offset, description.bits, value);
		}
	}
	return true;
}

PageCodecError read_cluster(const Record &p_record, const uint8_t *p_bytes, size_t p_size, size_t &r_cursor,
		uint32_t p_cluster, std::vector<GeometryPageCluster> &r_clusters,
		const std::vector<GeometryPageDependency> &p_dependencies) {
	GeometryPageCluster &r_cluster = r_clusters[p_cluster];
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
	if (!read_disk_vertices(p_record, p_bytes, p_size, r_cursor, p_cluster, r_clusters, p_dependencies) ||
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
	if (p_page.dependencies.size() > MAX_DISK_BYTES / 4) {
		return { PageCodecError::CAPACITY };
	}
	disk += p_page.dependencies.size() * 4;
	for (size_t index = 0; index < p_page.clusters.size(); ++index) {
		const auto &cluster = p_page.clusters[index];
		if (!valid_shape(cluster)) {
			return { PageCodecError::INVALID_LAYOUT, static_cast<uint32_t>(index) };
		}
		VertexLayout layout;
		if (!vertex_layout(make_record(cluster), layout)) {
			return { PageCodecError::INVALID_LAYOUT, static_cast<uint32_t>(index) };
		}
		size_t vertex_bytes = 0;
		for (uint32_t values : layout.plane_values) {
			vertex_bytes += size_t(values) * cluster.positions.vertices;
		}
		disk += aligned(std::max(vertex_bytes, size_t(cluster.positions.vertices) * 6)) + 36 + aligned(cluster.topology.references.size()) +
				(cluster.materials.table.size() + cluster.material_palette.size() + cluster.reuse_batch_words.size()) * 4;
		gpu += gpu_payload_bytes(cluster);
	}
	if (disk > MAX_DISK_BYTES || gpu > (p_page.root ? ROOT_PAGE_GPU_CAPACITY : STREAMING_PAGE_GPU_CAPACITY)) {
		return { PageCodecError::CAPACITY };
	}
	r_sizes = { static_cast<uint32_t>(disk), static_cast<uint32_t>(gpu) };
	return {};
}

PageCodecResult encode_geometry_page(const GeometryPage &p_page, std::vector<uint8_t> &r_bytes,
		const std::vector<GeometryPageDependency> &p_dependencies) {
	GeometryPageSizes sizes;
	const auto measured = measure_geometry_page(p_page, sizes);
	if (measured.error != PageCodecError::OK) {
		return measured;
	}
	try {
		for (size_t index = 0; index < p_page.dependencies.size(); ++index) {
			if (p_page.dependencies[index] == UINT32_MAX || (index != 0 && p_page.dependencies[index] <= p_page.dependencies[index - 1])) {
				return { PageCodecError::INVALID_METADATA };
			}
		}
		for (const auto &dependency : p_dependencies) {
			if (dependency.geometry == nullptr || (p_page.root && !dependency.geometry->root)) {
				return { PageCodecError::INVALID_METADATA };
			}
			GeometryPageSizes dependency_sizes;
			if (measure_geometry_page(*dependency.geometry, dependency_sizes).error != PageCodecError::OK) {
				return { PageCodecError::INVALID_LAYOUT };
			}
			for (const auto &cluster : dependency.geometry->clusters) {
				if (validate_cluster(cluster) != PageCodecError::OK) {
					return { PageCodecError::INVALID_LAYOUT };
				}
			}
		}
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
		return write_page(p_page, false, sizes, r_bytes, p_dependencies);
	} catch (const std::bad_alloc &) {
		return { PageCodecError::MEMORY };
	}
}

PageCodecResult decode_geometry_page(const uint8_t *p_bytes, size_t p_size, GeometryPage &r_page,
		const std::vector<GeometryPageDependency> &p_dependencies) {
	if (p_bytes == nullptr || p_size < GEOMETRY_PAGE_HEADER_BYTES || p_size > MAX_DISK_BYTES ||
			read_word(p_bytes, 0) != GEOMETRY_DISK_PAGE_MAGIC || read_word(p_bytes, 4) != GEOMETRY_PAGE_VERSION ||
			read_word(p_bytes, 8) > PAGE_FLAG_ROOT || read_word(p_bytes, 24) != GEOMETRY_PAGE_HEADER_BYTES) {
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
	if (read_word(p_bytes, 16) != p_size || !disk_dependencies(p_bytes, p_size, count, cursor)) {
		return { PageCodecError::INVALID_LENGTH };
	}
	try {
		GeometryPage candidate;
		candidate.root = root;
		for (uint32_t index = 0; index < read_word(p_bytes, 28); ++index) {
			candidate.dependencies.push_back(read_word(p_bytes, GEOMETRY_PAGE_HEADER_BYTES + size_t(count) * GEOMETRY_PAGE_CLUSTER_BYTES + size_t(index) * 4));
		}
		candidate.clusters.resize(count);
		for (uint32_t cluster = 0; cluster < count; ++cluster) {
			Record record;
			for (size_t word = 0; word < record.size(); ++word) {
				record[word] = read_word(p_bytes, GEOMETRY_PAGE_HEADER_BYTES + cluster * GEOMETRY_PAGE_CLUSTER_BYTES + word * 4);
			}
			const auto valid = read_cluster(record, p_bytes, p_size, cursor, cluster, candidate.clusters, p_dependencies);
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
		if (cursor != p_size || p_size > sizes.disk_bytes || sizes.gpu_bytes != gpu_bytes || gpu_cursor != gpu_bytes) {
			return { PageCodecError::INVALID_LENGTH };
		}
		r_page = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { PageCodecError::MEMORY };
	}
}

PageCodecResult validate_geometry_page_layout(const uint8_t *p_bytes, size_t p_size) {
	if (p_bytes == nullptr || p_size < GEOMETRY_PAGE_HEADER_BYTES || p_size > MAX_DISK_BYTES ||
			read_word(p_bytes, 0) != GEOMETRY_DISK_PAGE_MAGIC || read_word(p_bytes, 4) != GEOMETRY_PAGE_VERSION ||
			read_word(p_bytes, 8) > PAGE_FLAG_ROOT || read_word(p_bytes, 24) != GEOMETRY_PAGE_HEADER_BYTES) {
		return { PageCodecError::INVALID_HEADER };
	}
	const bool root = read_word(p_bytes, 8) != 0;
	const uint32_t count = read_word(p_bytes, 12);
	const uint32_t gpu_bytes = read_word(p_bytes, 20);
	if (count == 0 || count > (root ? 64u : 256u) || gpu_bytes > (root ? ROOT_PAGE_GPU_CAPACITY : STREAMING_PAGE_GPU_CAPACITY)) {
		return { PageCodecError::CAPACITY };
	}
	size_t cursor = GEOMETRY_PAGE_HEADER_BYTES + size_t(count) * GEOMETRY_PAGE_CLUSTER_BYTES;
	size_t gpu_cursor = cursor;
	if (read_word(p_bytes, 16) != p_size || !disk_dependencies(p_bytes, p_size, count, cursor)) {
		return { PageCodecError::INVALID_LENGTH };
	}
	const auto range = [&](uint32_t p_offset, size_t p_bytes_count) {
		if (p_offset != cursor || cursor > p_size || aligned(p_bytes_count) > p_size - cursor) {
			return false;
		}
		for (size_t byte = p_bytes_count; byte < aligned(p_bytes_count); ++byte) {
			if (p_bytes[cursor + byte] != 0) {
				return false;
			}
		}
		cursor += aligned(p_bytes_count);
		return true;
	};
	for (uint32_t cluster = 0; cluster < count; ++cluster) {
		Record record;
		for (size_t word = 0; word < record.size(); ++word) {
			record[word] = read_word(p_bytes, GEOMETRY_PAGE_HEADER_BYTES + size_t(cluster) * GEOMETRY_PAGE_CLUSTER_BYTES + word * 4);
		}
		if ((record[7] & ~15u) != 0 || (record[35] & ~7u) != 0 || (record[40] >> 16) != 0 ||
				record[33] == 0 || record[33] > 15 || record[34] > 12 || record[36] > 4 ||
				record[28] > 21 || record[29] > 21 || record[30] > 21 ||
				record[4] < 3 || record[4] > MAX_CLUSTER_VERTICES || record[5] == 0 || record[5] > MAX_CLUSTER_TRIANGLES ||
				record[6] == 0 || record[6] > MAX_CLUSTER_MATERIALS || record[32] > 2016 || record[38] > 10592 ||
				record[64] > 240 || record[67] > MAX_CLUSTER_MATERIALS || record[70] > 28) {
			return { PageCodecError::INVALID_LAYOUT, cluster };
		}
		VertexLayout layout;
		uint32_t references, coded;
		if (!disk_vertex_layout(record, p_bytes, p_size, cluster, layout, references, coded)) {
			return { PageCodecError::INVALID_LAYOUT, cluster };
		}
		uint32_t plane_values = 0;
		for (uint32_t values : layout.plane_values) {
			plane_values += values;
		}
		if (record[71] != gpu_cursor || !range(record[31], coded * plane_values) ||
				!range(record[37], 32 + references * 6) || !range(record[63], record[64])) {
			return { PageCodecError::INVALID_OFFSET, cluster };
		}
		ClusterStripLayout strip;
		strip.vertices = record[4];
		strip.triangles = record[5];
		strip.new_vertices_before = record[61];
		strip.references_before = record[62];
		for (uint32_t block = 0; block < 4; ++block) {
			for (uint32_t mask = 0; mask < 3; ++mask) {
				strip.masks[block][mask] = record[49 + block * 3 + mask];
			}
		}
		if (validate_cluster_strip_layout(strip, p_bytes + record[63], record[64]) != TopologyCodecError::OK) {
			return { PageCodecError::TOPOLOGY, cluster };
		}
		if ((slow_material(record[65]) && (record[65] & SLOW_MATERIAL_MASK) != record[66] / 4) ||
				!range(record[66], size_t(record[67]) * 4) || !range(record[68], size_t(record[6]) * 4) || !range(record[69], size_t(record[70]) * 4)) {
			return { PageCodecError::INVALID_OFFSET, cluster };
		}
		gpu_cursor += aligned(record[32]) + aligned(record[38]) + gpu_index_bytes(record[4], record[5]) + size_t(record[67] + record[6] + record[70]) * 4;
	}
	if (cursor != p_size || gpu_cursor != gpu_bytes) {
		return { PageCodecError::INVALID_LENGTH };
	}
	return {};
}

GeometryPageVertexLayout geometry_page_vertex_layout(const uint8_t *p_bytes, uint32_t p_cluster) {
	const Record record = disk_record(p_bytes, p_cluster);
	return { record[4], record[24], record[33], record[34], record[35], record[36] };
}

PageCodecResult validate_geometry_page_dependencies(const uint8_t *p_bytes, size_t p_size,
		const GeometryPageDependencyLayout *p_dependencies, size_t p_dependency_count) {
	const auto valid = validate_geometry_page_layout(p_bytes, p_size);
	if (valid.error != PageCodecError::OK) {
		return valid;
	}
	const uint32_t clusters = read_word(p_bytes, 12);
	const size_t dependencies_start = GEOMETRY_PAGE_HEADER_BYTES + size_t(clusters) * GEOMETRY_PAGE_CLUSTER_BYTES;
	for (uint32_t cluster = 0; cluster < clusters; ++cluster) {
		const Record record = disk_record(p_bytes, cluster);
		uint32_t references = 0;
		for (uint32_t vertex = 0; vertex < record[4]; ++vertex) {
			references += is_vertex_reference(p_bytes, record, vertex);
		}
		uint32_t source_vertex = 0;
		for (uint32_t reference = 0; reference < references; ++reference) {
			const uint32_t delta = p_bytes[record[37] + 32 + references + reference];
			source_vertex = (source_vertex + ((delta >> 1) ^ (0u - (delta & 1u)))) & 255u;
			const uint32_t dependency = reference_dependency(p_bytes, record, references, reference);
			if (dependency == 0) {
				continue;
			}
			const uint32_t page = read_word(p_bytes, dependencies_start + size_t(dependency - 1) * 4);
			const GeometryPageDependencyLayout *source = nullptr;
			for (size_t index = 0; index < p_dependency_count; ++index) {
				if (p_dependencies[index].page == page) {
					source = &p_dependencies[index];
					break;
				}
			const uint32_t source_cluster = p_bytes[record[37] + 32 + reference];
			if (source == nullptr || source->clusters == nullptr || source_cluster >= source->cluster_count) {
				return { PageCodecError::INVALID_METADATA, cluster };
			}
			const auto &layout = source->clusters[source_cluster];
			if (source_vertex >= layout.vertices || layout.precision != record[24] ||
					layout.normal_bits != record[33] || layout.tangent_bits != record[34] ||
					layout.flags != record[35] || layout.uv_sets != record[36]) {
				return { PageCodecError::INVALID_METADATA, cluster };
			}
		}
	}
	return {};
}

PageCodecResult transcode_geometry_page_cpu(const uint8_t *p_bytes, size_t p_size, std::vector<uint8_t> &r_gpu_bytes,
		const std::vector<GeometryPageDependency> &p_dependencies) {
	GeometryPage page;
	const auto decoded = decode_geometry_page(p_bytes, p_size, page, p_dependencies);
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
