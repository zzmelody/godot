#include "model_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <tuple>
#include <utility>

namespace VirtualGeometry {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

uint32_t read32(const uint8_t *p_bytes, size_t p_offset) {
	uint32_t word = 0;
	for (uint32_t byte = 0; byte < 4; ++byte) {
		word |= static_cast<uint32_t>(p_bytes[p_offset + byte]) << (byte * 8);
	}
	return word;
}

uint64_t read64(const uint8_t *p_bytes, size_t p_offset) {
	return read32(p_bytes, p_offset) | (static_cast<uint64_t>(read32(p_bytes, p_offset + 4)) << 32);
}

void write32(std::vector<uint8_t> &r_bytes, size_t p_offset, uint32_t p_word) {
	for (uint32_t byte = 0; byte < 4; ++byte) {
		r_bytes[p_offset + byte] = static_cast<uint8_t>(p_word >> (byte * 8));
	}
}

void write64(std::vector<uint8_t> &r_bytes, size_t p_offset, uint64_t p_word) {
	write32(r_bytes, p_offset, static_cast<uint32_t>(p_word));
	write32(r_bytes, p_offset + 4, static_cast<uint32_t>(p_word >> 32));
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

uint64_t align16(uint64_t p_bytes) {
	return (p_bytes + 15) & ~uint64_t{ 15 };
}

struct Layout {
	uint64_t nodes;
	uint64_t parts;
	uint64_t pages;
	uint64_t materials;
	uint64_t groups;
	uint64_t directory;
	uint64_t directory_bytes;
	uint64_t total;
};

Layout layout(uint32_t p_nodes, uint32_t p_parts, uint32_t p_pages, uint32_t p_materials, uint32_t p_groups, uint32_t p_dependencies) {
	Layout result;
	result.nodes = MODEL_ROOT_HEADER_BYTES;
	result.parts = result.nodes + static_cast<uint64_t>(p_nodes) * MODEL_NODE_BYTES;
	result.pages = result.parts + static_cast<uint64_t>(p_parts) * MODEL_PART_BYTES;
	result.materials = result.pages + static_cast<uint64_t>(p_pages) * 16;
	result.groups = align16(result.materials + static_cast<uint64_t>(p_materials) * 4);
	result.directory = align16(result.groups + static_cast<uint64_t>(p_groups) * 8);
	result.directory_bytes = PAGE_DIRECTORY_HEADER_BYTES + static_cast<uint64_t>(p_pages) * PAGE_DIRECTORY_RECORD_BYTES + static_cast<uint64_t>(p_dependencies) * 4;
	result.total = result.directory + result.directory_bytes;
	return result;
}

uint64_t scratch_bytes(uint32_t p_nodes, uint32_t p_parts, uint32_t p_pages, uint32_t p_groups) {
	// Incoming references/depths, part visits, group accounting and directory walk.
	return static_cast<uint64_t>(p_nodes) * 8 + p_parts + static_cast<uint64_t>(p_groups) * 32 + static_cast<uint64_t>(p_pages) * 128;
}

bool same_format(const ClusterAttributeFormat &p_first, const ClusterAttributeFormat &p_second) {
	return p_first.has_tangents == p_second.has_tangents && p_first.has_colors == p_second.has_colors && p_first.uv_sets == p_second.uv_sets &&
			p_first.lossless_uvs == p_second.lossless_uvs && p_first.normal_bits == p_second.normal_bits && p_first.tangent_bits == p_second.tangent_bits;
}

bool valid_format(const ClusterAttributeFormat &p_format) {
	return p_format.uv_sets <= 4 && p_format.normal_bits >= 1 && p_format.normal_bits <= 15 &&
			(p_format.has_tangents ? p_format.tangent_bits >= 1 && p_format.tangent_bits <= 12 : p_format.tangent_bits == 0) &&
			(p_format.uv_sets != 0 || !p_format.lossless_uvs);
}

std::pair<uint32_t, uint32_t> streaming_range(const ModelGroupPages &p_group, uint32_t p_roots) {
	const uint32_t first = std::max(p_group.first, p_roots);
	const uint32_t end = p_group.first + p_group.count;
	return first < end ? std::make_pair(first, end - first) : std::make_pair(0u, 0u);
}

bool without_page(uint32_t p_page, std::pair<uint32_t, uint32_t> &r_range) {
	if (r_range.second != 0 && p_page >= r_range.first && p_page - r_range.first < r_range.second) {
		if (p_page == r_range.first) {
			++r_range.first;
		} else if (p_page != r_range.first + r_range.second - 1) {
			return false;
		}
		--r_range.second;
		if (r_range.second == 0) {
			r_range.first = 0;
		}
	}
	return true;
}

bool occupied(const HierarchyBranch &p_branch) {
	return p_branch.node != UINT32_MAX || p_branch.part != UINT32_MAX;
}

bool finite_branch(const HierarchyBranch &p_branch) {
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(p_branch.lower[axis]) || !std::isfinite(p_branch.upper[axis]) || p_branch.lower[axis] > p_branch.upper[axis]) {
			return false;
		}
	}
	return std::all_of(p_branch.lod_bounds.begin(), p_branch.lod_bounds.end(), [](float p_value) { return std::isfinite(p_value); }) &&
			p_branch.lod_bounds[3] >= 0 && std::isfinite(p_branch.min_error) && p_branch.min_error >= -1 &&
			std::isfinite(p_branch.max_error) && p_branch.max_error >= 0 && p_branch.min_error <= p_branch.max_error;
}

bool contains(const HierarchyBranch &p_parent, const HierarchyBranch &p_child) {
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (p_parent.lower[axis] > p_child.lower[axis] || p_parent.upper[axis] < p_child.upper[axis]) {
			return false;
		}
	}
	return p_parent.min_error <= p_child.min_error && p_parent.max_error >= p_child.max_error &&
			std::hypot(static_cast<double>(p_parent.lod_bounds[0]) - p_child.lod_bounds[0], static_cast<double>(p_parent.lod_bounds[1]) - p_child.lod_bounds[1],
					static_cast<double>(p_parent.lod_bounds[2]) - p_child.lod_bounds[2]) +
					p_child.lod_bounds[3] <=
			p_parent.lod_bounds[3];
}

ModelCodecResult validate_root(const GeometryModelRoot &p_root) {
	const auto &hierarchy = p_root.hierarchy;
	if (hierarchy.nodes.empty() || hierarchy.parts.empty() || hierarchy.pages.empty() || p_root.groups.empty() || p_root.groups.size() > hierarchy.parts.size() ||
			p_root.materials.empty() || p_root.cluster_count == 0 || p_root.root_group >= p_root.groups.size() ||
			hierarchy.root_pages == 0 || hierarchy.root_pages > hierarchy.pages.size() || p_root.directory.pages.size() != hierarchy.pages.size() ||
			!hierarchy.cluster_flags.empty() || !valid_format(p_root.attributes) || p_root.position_precision < MIN_POSITION_PRECISION || p_root.position_precision > MAX_POSITION_PRECISION ||
			!std::isfinite(p_root.position_error) || p_root.position_error < 0 ||
			std::all_of(p_root.build_key.begin(), p_root.build_key.end(), [](uint8_t p_value) { return p_value == 0; })) {
		return { ModelCodecError::INVALID_METADATA };
	}
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(p_root.lower[axis]) || !std::isfinite(p_root.upper[axis]) || p_root.lower[axis] > p_root.upper[axis]) {
			return { ModelCodecError::INVALID_METADATA };
		}
	}
	if (std::adjacent_find(p_root.materials.begin(), p_root.materials.end(), [](uint32_t p_first, uint32_t p_second) { return p_first >= p_second; }) != p_root.materials.end()) {
		return { ModelCodecError::INVALID_METADATA };
	}
	std::vector<uint32_t> depth(hierarchy.nodes.size(), 0);
	std::vector<uint8_t> incoming(hierarchy.nodes.size(), 0);
	std::vector<uint8_t> part_visits(hierarchy.parts.size(), 0);
	std::vector<uint32_t> page_depth(hierarchy.pages.size(), 0);
	for (uint32_t node = 0; node < hierarchy.nodes.size(); ++node) {
		if ((node != 0 && incoming[node] != 1) || depth[node] > MAX_HIERARCHY_DEPTH) {
			return { ModelCodecError::INVALID_TREE, node };
		}
		bool empty = false;
		for (uint32_t child = 0; child < HIERARCHY_FANOUT; ++child) {
			const auto &branch = hierarchy.nodes[node].children[child];
			if (!occupied(branch)) {
				empty = true;
				if (branch.lower != std::array<float, 3>{} || branch.upper != std::array<float, 3>{} || branch.lod_bounds != std::array<float, 4>{} || branch.min_error != 0 || branch.max_error != 0) {
					return { ModelCodecError::INVALID_TREE, node };
				}
				continue;
			}
			if (empty || !finite_branch(branch) || (branch.node != UINT32_MAX) == (branch.part != UINT32_MAX)) {
				return { ModelCodecError::INVALID_TREE, node };
			}
			if (branch.part != UINT32_MAX) {
				if (branch.part >= hierarchy.parts.size() || part_visits[branch.part]++ != 0) {
					return { ModelCodecError::INVALID_TREE, node };
				}
				const auto &part = hierarchy.parts[branch.part];
				if (part.node != node || part.child != child || part.page >= hierarchy.pages.size()) {
					return { ModelCodecError::INVALID_PARTS, branch.part };
				}
				page_depth[part.page] = std::max(page_depth[part.page], depth[node]);
				for (uint32_t axis = 0; axis < 3; ++axis) {
					if (branch.lower[axis] < p_root.lower[axis] || branch.upper[axis] > p_root.upper[axis]) {
						return { ModelCodecError::INVALID_TREE, node };
					}
				}
			} else {
				if (branch.node <= node || branch.node >= hierarchy.nodes.size() || incoming[branch.node]++ != 0) {
					return { ModelCodecError::INVALID_TREE, node };
				}
				depth[branch.node] = depth[node] + 1;
				for (const auto &next : hierarchy.nodes[branch.node].children) {
					if (occupied(next) && (!finite_branch(next) || !contains(branch, next))) {
						return { ModelCodecError::INVALID_TREE, node };
					}
				}
			}
		}
		if (!occupied(hierarchy.nodes[node].children[0])) {
			return { ModelCodecError::INVALID_TREE, node };
		}
	}
	if (std::find(part_visits.begin(), part_visits.end(), 0) != part_visits.end()) {
		return { ModelCodecError::INVALID_PARTS };
	}
	struct GroupVisit {
		uint32_t first_part = UINT32_MAX;
		uint32_t previous_page = UINT32_MAX;
		uint32_t pages = 0;
	};
	std::vector<GroupVisit> groups(p_root.groups.size());
	uint64_t clusters = 0;
	uint64_t container = 0;
	uint32_t first_part = 0;
	uint32_t first_dependency = 0;
	for (uint32_t page = 0; page < hierarchy.pages.size(); ++page) {
		const auto &metadata = hierarchy.pages[page];
		const auto &location = p_root.directory.pages[page];
		if (metadata.clusters == 0 || metadata.clusters > (page < hierarchy.root_pages ? 64u : 256u) || metadata.first_part != first_part || metadata.part_count == 0 ||
				first_part > hierarchy.parts.size() || metadata.part_count > hierarchy.parts.size() - first_part ||
				!metadata.cluster_fixups.empty() || !metadata.hierarchy_fixups.empty() || !metadata.dependencies.empty() ||
				metadata.depth != page_depth[page] || location.hierarchy_depth != metadata.depth ||
				location.flags != (page < hierarchy.root_pages ? PAGE_FLAG_ROOT : 0u) || location.container_offset != container ||
				location.fixup_offset < GEOMETRY_PAGE_HEADER_BYTES || location.fixup_bytes < MODEL_FIXUP_HEADER_BYTES ||
				location.fixup_offset > STREAMING_PAGE_GPU_CAPACITY * 2 || location.fixup_bytes > MODEL_FIXUP_HEADER_BYTES + MAX_PAGE_FIXUPS * 40 ||
				(location.fixup_offset & 3) != 0 || (location.fixup_bytes & 3) != 0 || (location.container_offset & 3) != 0 ||
				location.gpu_bytes < GEOMETRY_PAGE_HEADER_BYTES + metadata.clusters * GEOMETRY_PAGE_CLUSTER_BYTES ||
				location.dependency_start != first_dependency || first_dependency > p_root.directory.dependencies.size() || location.dependency_count > p_root.directory.dependencies.size() - first_dependency ||
				location.fixup_offset > location.disk_bytes || location.fixup_bytes != location.disk_bytes - location.fixup_offset) {
			return { ModelCodecError::INVALID_DIRECTORY, page };
		}
		uint32_t local = 0;
		for (uint32_t id = first_part; id < first_part + metadata.part_count; ++id) {
			const auto &part = hierarchy.parts[id];
			if (part.group >= groups.size() || part.page != page || part.cluster_start != local || part.cluster_count == 0 || part.cluster_count > metadata.clusters - local ||
					(part.group == p_root.root_group && page >= hierarchy.root_pages)) {
				return { ModelCodecError::INVALID_PARTS, id };
			}
			local += part.cluster_count;
			auto &visit = groups[part.group];
			if (visit.pages == 0) {
				visit.first_part = id;
			} else if (visit.previous_page + 1 != page) {
				return { ModelCodecError::INVALID_PARTS, id };
			}
			visit.previous_page = page;
			++visit.pages;
			const auto &first = hierarchy.parts[visit.first_part];
			const auto &first_branch = hierarchy.nodes[first.node].children[first.child];
			const auto &branch = hierarchy.nodes[part.node].children[part.child];
			const auto &group = p_root.groups[part.group];
			const auto range = streaming_range(group, hierarchy.root_pages);
			if (branch.lod_bounds != first_branch.lod_bounds || branch.min_error != first_branch.min_error || branch.max_error != first_branch.max_error ||
					part.dependency_first != range.first || part.dependency_count != range.second) {
				return { ModelCodecError::INVALID_PARTS, id };
			}
		}
		if (local != metadata.clusters) {
			return { ModelCodecError::INVALID_PARTS, first_part };
		}
		first_part += metadata.part_count;
		first_dependency += location.dependency_count;
		clusters += metadata.clusters;
		container += location.disk_bytes;
	}
	for (uint32_t group = 0; group < groups.size(); ++group) {
		const auto &range = p_root.groups[group];
		if (groups[group].pages == 0 || range.count != groups[group].pages || range.count > MAX_GROUP_PAGE_PARTS ||
				range.first >= hierarchy.pages.size() || range.count > hierarchy.pages.size() - range.first ||
				range.first != hierarchy.parts[groups[group].first_part].page) {
			return { ModelCodecError::INVALID_PARTS, group };
		}
	}
	if (first_part != hierarchy.parts.size() || first_dependency != p_root.directory.dependencies.size() || clusters != p_root.cluster_count || container != p_root.directory.container_bytes) {
		return { ModelCodecError::INVALID_METADATA };
	}
	return {};
}

void write_branch(std::vector<uint8_t> &r_bytes, size_t p_offset, const HierarchyBranch &p_branch) {
	for (uint32_t axis = 0; axis < 3; ++axis) {
		write32(r_bytes, p_offset + axis * 4, float_word(p_branch.lower[axis]));
		write32(r_bytes, p_offset + (3 + axis) * 4, float_word(p_branch.upper[axis]));
	}
	for (uint32_t axis = 0; axis < 4; ++axis) {
		write32(r_bytes, p_offset + (6 + axis) * 4, float_word(p_branch.lod_bounds[axis]));
	}
	write32(r_bytes, p_offset + 40, float_word(p_branch.min_error));
	write32(r_bytes, p_offset + 44, float_word(p_branch.max_error));
	write32(r_bytes, p_offset + 48, p_branch.node);
	write32(r_bytes, p_offset + 52, p_branch.part);
	// Physical page references are published only after GPU installation.
	write32(r_bytes, p_offset + 56, UINT32_MAX);
}

bool read_branch(const uint8_t *p_bytes, size_t p_offset, HierarchyBranch &r_branch) {
	for (uint32_t axis = 0; axis < 3; ++axis) {
		r_branch.lower[axis] = word_float(read32(p_bytes, p_offset + axis * 4));
		r_branch.upper[axis] = word_float(read32(p_bytes, p_offset + (3 + axis) * 4));
	}
	for (uint32_t axis = 0; axis < 4; ++axis) {
		r_branch.lod_bounds[axis] = word_float(read32(p_bytes, p_offset + (6 + axis) * 4));
	}
	r_branch.min_error = word_float(read32(p_bytes, p_offset + 40));
	r_branch.max_error = word_float(read32(p_bytes, p_offset + 44));
	r_branch.node = read32(p_bytes, p_offset + 48);
	r_branch.part = read32(p_bytes, p_offset + 52);
	return read32(p_bytes, p_offset + 56) == UINT32_MAX && read32(p_bytes, p_offset + 60) == 0;
}

bool zero_bytes(const uint8_t *p_bytes, size_t p_first, size_t p_end) {
	for (size_t byte = p_first; byte < p_end; ++byte) {
		if (p_bytes[byte] != 0) {
			return false;
		}
	}
	return true;
}

auto hierarchy_key(const HierarchyFixup &p_fixup) {
	return std::make_tuple(p_fixup.node, p_fixup.child, p_fixup.page, p_fixup.cluster_start, p_fixup.dependency_first, p_fixup.dependency_count);
}

auto cluster_key(const ClusterFixup &p_fixup) {
	return std::make_tuple(p_fixup.page, p_fixup.cluster, p_fixup.dependency_first, p_fixup.dependency_count);
}

ModelCodecResult validate_chunk(const GeometryModelRoot &p_root, uint32_t p_page, const HierarchyPage &p_chunk) {
	if (p_page >= p_root.hierarchy.pages.size() || p_chunk.clusters != p_root.hierarchy.pages[p_page].clusters ||
			p_chunk.hierarchy_fixups.size() > MAX_PAGE_FIXUPS || p_chunk.cluster_fixups.size() > MAX_PAGE_FIXUPS) {
		return { ModelCodecError::INVALID_FIXUPS, p_page };
	}
	const auto &metadata = p_root.hierarchy.pages[p_page];
	std::vector<HierarchyFixup> expected;
	for (uint32_t id = metadata.first_part; id < metadata.first_part + metadata.part_count; ++id) {
		const auto &group = p_root.groups[p_root.hierarchy.parts[id].group];
		for (uint32_t page = group.first; page < group.first + group.count; ++page) {
			const auto &target = p_root.hierarchy.pages[page];
			for (uint32_t part_id = target.first_part; part_id < target.first_part + target.part_count; ++part_id) {
				const auto &part = p_root.hierarchy.parts[part_id];
				if (part.group == p_root.hierarchy.parts[id].group) {
					expected.push_back({ part.page, part.node, part.child, part.cluster_start, part.dependency_first, part.dependency_count });
				}
			}
		}
	}
	std::sort(expected.begin(), expected.end(), [](const auto &p_first, const auto &p_second) { return hierarchy_key(p_first) < hierarchy_key(p_second); });
	if (expected.size() != p_chunk.hierarchy_fixups.size() || expected.size() > MAX_PAGE_FIXUPS) {
		return { ModelCodecError::INVALID_FIXUPS, p_page };
	}
	for (size_t index = 0; index < expected.size(); ++index) {
		if (hierarchy_key(expected[index]) != hierarchy_key(p_chunk.hierarchy_fixups[index])) {
			return { ModelCodecError::INVALID_FIXUPS, p_page };
		}
	}
	std::vector<uint32_t> dependencies;
	for (size_t index = 0; index < p_chunk.cluster_fixups.size(); ++index) {
		const auto &fixup = p_chunk.cluster_fixups[index];
		if (fixup.page > p_page || fixup.cluster >= p_root.hierarchy.pages[fixup.page].clusters ||
				(index != 0 && std::tie(p_chunk.cluster_fixups[index - 1].page, p_chunk.cluster_fixups[index - 1].cluster) >= std::tie(fixup.page, fixup.cluster))) {
			return { ModelCodecError::INVALID_FIXUPS, p_page };
		}
		bool found = false;
		for (uint32_t id = metadata.first_part; id < metadata.first_part + metadata.part_count; ++id) {
			const auto &part = p_root.hierarchy.parts[id];
			auto range = streaming_range(p_root.groups[part.group], p_root.hierarchy.root_pages);
			if (without_page(fixup.page, range) && range.second != 0 && range.first == fixup.dependency_first && range.second == fixup.dependency_count) {
				found = true;
				break;
			}
		}
		if (!found) {
			return { ModelCodecError::INVALID_FIXUPS, p_page };
		}
		if (fixup.page != p_page) {
			dependencies.push_back(fixup.page);
		}
	}
	std::sort(dependencies.begin(), dependencies.end());
	dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
	const auto &location = p_root.directory.pages[p_page];
	if (dependencies.size() != location.dependency_count || location.dependency_start > p_root.directory.dependencies.size() ||
			location.dependency_count > p_root.directory.dependencies.size() - location.dependency_start ||
			!std::equal(dependencies.begin(), dependencies.end(), p_root.directory.dependencies.begin() + location.dependency_start)) {
		return { ModelCodecError::INVALID_FIXUPS, p_page };
	}
	return {};
}

} // namespace

ModelCodecResult encode_geometry_model_root(const GeometryModelRoot &p_root, uint64_t p_max_memory_bytes, std::vector<uint8_t> &r_bytes) {
	const auto &h = p_root.hierarchy;
	if (h.nodes.size() > UINT32_MAX || h.parts.size() > UINT32_MAX || h.pages.size() > UINT32_MAX || p_root.groups.size() > UINT32_MAX ||
			p_root.materials.size() > UINT32_MAX || p_root.directory.dependencies.size() > UINT32_MAX) {
		return { ModelCodecError::INVALID_LENGTH };
	}
	const auto sizes = layout(static_cast<uint32_t>(h.nodes.size()), static_cast<uint32_t>(h.parts.size()), static_cast<uint32_t>(h.pages.size()),
			static_cast<uint32_t>(p_root.materials.size()), static_cast<uint32_t>(p_root.groups.size()), static_cast<uint32_t>(p_root.directory.dependencies.size()));
	const uint64_t scratch = scratch_bytes(static_cast<uint32_t>(h.nodes.size()), static_cast<uint32_t>(h.parts.size()), static_cast<uint32_t>(h.pages.size()), static_cast<uint32_t>(p_root.groups.size()));
	if (sizes.total > std::numeric_limits<size_t>::max() || sizes.total + sizes.directory_bytes + scratch > p_max_memory_bytes) {
		return { ModelCodecError::MEMORY_LIMIT };
	}
	try {
		std::vector<uint8_t> directory;
		if (encode_page_directory(p_root.directory, p_max_memory_bytes, directory).error != PageDirectoryError::OK) {
			return { ModelCodecError::INVALID_DIRECTORY };
		}
		const auto valid = validate_root(p_root);
		if (valid.error != ModelCodecError::OK) {
			return valid;
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(sizes.total), 0);
		const auto &format = p_root.attributes;
		const uint32_t header[] = { MODEL_ROOT_MAGIC, MODEL_FORMAT_VERSION, MODEL_ROOT_HEADER_BYTES, 0, p_root.cluster_count, static_cast<uint32_t>(p_root.groups.size()),
			p_root.root_group, h.root_pages, static_cast<uint32_t>(h.nodes.size()), static_cast<uint32_t>(h.parts.size()), static_cast<uint32_t>(h.pages.size()),
			static_cast<uint32_t>(p_root.materials.size()), static_cast<uint32_t>(p_root.position_precision), format.normal_bits, format.tangent_bits,
			(format.has_tangents ? 1u : 0u) | (format.has_colors ? 2u : 0u) | (format.lossless_uvs ? 4u : 0u), format.uv_sets, float_word(p_root.position_error) };
		for (size_t word = 0; word < sizeof(header) / sizeof(header[0]); ++word) {
			write32(bytes, word * 4, header[word]);
		}
		for (uint32_t axis = 0; axis < 3; ++axis) {
			write32(bytes, (18 + axis) * 4, float_word(p_root.lower[axis]));
			write32(bytes, (21 + axis) * 4, float_word(p_root.upper[axis]));
		}
		std::copy(p_root.build_key.begin(), p_root.build_key.end(), bytes.begin() + 96);
		const uint64_t offsets[] = { sizes.total, sizes.nodes, sizes.parts, sizes.pages, sizes.materials, sizes.directory, sizes.directory_bytes, p_root.directory.container_bytes, sizes.groups, static_cast<uint64_t>(p_root.groups.size()) * 8 };
		for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
			write64(bytes, 128 + index * 8, offsets[index]);
		}
		for (size_t node = 0; node < h.nodes.size(); ++node) {
			for (uint32_t child = 0; child < HIERARCHY_FANOUT; ++child) {
				write_branch(bytes, static_cast<size_t>(sizes.nodes) + node * MODEL_NODE_BYTES + child * 64, h.nodes[node].children[child]);
			}
		}
		for (size_t id = 0; id < h.parts.size(); ++id) {
			const auto &part = h.parts[id];
			const uint32_t words[] = { part.group, part.page, part.cluster_start, part.cluster_count, part.node, part.child, part.dependency_first, part.dependency_count };
			for (size_t word = 0; word < 8; ++word) {
				write32(bytes, static_cast<size_t>(sizes.parts) + id * MODEL_PART_BYTES + word * 4, words[word]);
			}
		}
		for (size_t page = 0; page < h.pages.size(); ++page) {
			write32(bytes, static_cast<size_t>(sizes.pages) + page * 16, h.pages[page].clusters);
			write32(bytes, static_cast<size_t>(sizes.pages) + page * 16 + 4, h.pages[page].first_part);
			write32(bytes, static_cast<size_t>(sizes.pages) + page * 16 + 8, h.pages[page].part_count);
		}
		for (size_t material = 0; material < p_root.materials.size(); ++material) {
			write32(bytes, static_cast<size_t>(sizes.materials) + material * 4, p_root.materials[material]);
		}
		for (size_t group = 0; group < p_root.groups.size(); ++group) {
			write32(bytes, static_cast<size_t>(sizes.groups) + group * 8, p_root.groups[group].first);
			write32(bytes, static_cast<size_t>(sizes.groups) + group * 8 + 4, p_root.groups[group].count);
		}
		std::copy(directory.begin(), directory.end(), bytes.begin() + sizes.directory);
		r_bytes = std::move(bytes);
		return {};
	} catch (const std::bad_alloc &) {
		return { ModelCodecError::MEMORY };
	}
}

ModelCodecResult decode_geometry_model_root(const uint8_t *p_bytes, size_t p_size, uint64_t p_container_bytes,
		uint64_t p_max_memory_bytes, GeometryModelRoot &r_root) {
	if (p_bytes == nullptr || p_size < MODEL_ROOT_HEADER_BYTES || read32(p_bytes, 0) != MODEL_ROOT_MAGIC || read32(p_bytes, 4) != MODEL_FORMAT_VERSION ||
			read32(p_bytes, 8) != MODEL_ROOT_HEADER_BYTES || read32(p_bytes, 12) != 0 || (read32(p_bytes, 60) & ~7u) != 0) {
		return { ModelCodecError::INVALID_HEADER };
	}
	const uint32_t nodes = read32(p_bytes, 32);
	const uint32_t parts = read32(p_bytes, 36);
	const uint32_t pages = read32(p_bytes, 40);
	const uint32_t materials = read32(p_bytes, 44);
	const uint32_t groups = read32(p_bytes, 20);
	const uint64_t directory_offset = read64(p_bytes, 168);
	if (directory_offset > p_size || p_size - directory_offset < PAGE_DIRECTORY_HEADER_BYTES) {
		return { ModelCodecError::INVALID_LENGTH };
	}
	const uint32_t dependencies = read32(p_bytes, static_cast<size_t>(directory_offset) + 12);
	const auto sizes = layout(nodes, parts, pages, materials, groups, dependencies);
	const uint64_t offsets[] = { sizes.total, sizes.nodes, sizes.parts, sizes.pages, sizes.materials, sizes.directory, sizes.directory_bytes, p_container_bytes, sizes.groups, static_cast<uint64_t>(groups) * 8 };
	for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
		if (read64(p_bytes, 128 + index * 8) != offsets[index]) {
			return { ModelCodecError::INVALID_LENGTH };
		}
	}
	if (sizes.total != p_size || nodes == 0 || parts == 0 || pages == 0 || materials == 0 || groups == 0 || groups > parts ||
			read32(p_bytes, static_cast<size_t>(directory_offset) + 8) != pages) {
		return { ModelCodecError::INVALID_LENGTH };
	}
	const uint64_t owned = static_cast<uint64_t>(nodes) * sizeof(HierarchyNode) + static_cast<uint64_t>(parts) * sizeof(HierarchyPart) +
			static_cast<uint64_t>(pages) * (sizeof(HierarchyPage) + sizeof(PageLocation)) + static_cast<uint64_t>(materials + static_cast<uint64_t>(dependencies)) * 4 + static_cast<uint64_t>(groups) * sizeof(ModelGroupPages);
	if (owned + scratch_bytes(nodes, parts, pages, groups) > p_max_memory_bytes) {
		return { ModelCodecError::MEMORY_LIMIT };
	}
	try {
		GeometryModelRoot candidate;
		candidate.cluster_count = read32(p_bytes, 16);
		candidate.root_group = read32(p_bytes, 24);
		candidate.hierarchy.root_pages = read32(p_bytes, 28);
		const uint32_t precision = read32(p_bytes, 48);
		std::memcpy(&candidate.position_precision, &precision, 4);
		candidate.attributes.normal_bits = read32(p_bytes, 52);
		candidate.attributes.tangent_bits = read32(p_bytes, 56);
		const uint32_t flags = read32(p_bytes, 60);
		candidate.attributes.has_tangents = (flags & 1) != 0;
		candidate.attributes.has_colors = (flags & 2) != 0;
		candidate.attributes.lossless_uvs = (flags & 4) != 0;
		candidate.attributes.uv_sets = read32(p_bytes, 64);
		candidate.position_error = word_float(read32(p_bytes, 68));
		for (uint32_t axis = 0; axis < 3; ++axis) {
			candidate.lower[axis] = word_float(read32(p_bytes, (18 + axis) * 4));
			candidate.upper[axis] = word_float(read32(p_bytes, (21 + axis) * 4));
		}
		std::copy_n(p_bytes + 96, 32, candidate.build_key.begin());
		if (decode_page_directory(p_bytes + sizes.directory, static_cast<size_t>(sizes.directory_bytes), p_container_bytes, p_max_memory_bytes, candidate.directory).error != PageDirectoryError::OK) {
			return { ModelCodecError::INVALID_DIRECTORY };
		}
		candidate.hierarchy.nodes.resize(nodes);
		candidate.hierarchy.parts.resize(parts);
		candidate.hierarchy.pages.resize(pages);
		candidate.materials.resize(materials);
		candidate.groups.resize(groups);
		for (uint32_t node = 0; node < nodes; ++node) {
			for (uint32_t child = 0; child < HIERARCHY_FANOUT; ++child) {
				if (!read_branch(p_bytes, static_cast<size_t>(sizes.nodes) + static_cast<size_t>(node) * MODEL_NODE_BYTES + child * 64, candidate.hierarchy.nodes[node].children[child])) {
					return { ModelCodecError::INVALID_TREE, node };
				}
			}
		}
		for (uint32_t id = 0; id < parts; ++id) {
			const size_t offset = static_cast<size_t>(sizes.parts) + static_cast<size_t>(id) * MODEL_PART_BYTES;
			candidate.hierarchy.parts[id] = { read32(p_bytes, offset), read32(p_bytes, offset + 4), read32(p_bytes, offset + 8), read32(p_bytes, offset + 12),
				read32(p_bytes, offset + 16), read32(p_bytes, offset + 20), read32(p_bytes, offset + 24), read32(p_bytes, offset + 28) };
		}
		for (uint32_t page = 0; page < pages; ++page) {
			const size_t offset = static_cast<size_t>(sizes.pages) + static_cast<size_t>(page) * 16;
			auto &metadata = candidate.hierarchy.pages[page];
			metadata.clusters = read32(p_bytes, offset);
			metadata.first_part = read32(p_bytes, offset + 4);
			metadata.part_count = read32(p_bytes, offset + 8);
			metadata.depth = candidate.directory.pages[page].hierarchy_depth;
			if (read32(p_bytes, offset + 12) != 0) {
				return { ModelCodecError::INVALID_METADATA, page };
			}
		}
		for (uint32_t material = 0; material < materials; ++material) {
			candidate.materials[material] = read32(p_bytes, static_cast<size_t>(sizes.materials) + static_cast<size_t>(material) * 4);
		}
		for (uint32_t group = 0; group < groups; ++group) {
			const size_t offset = static_cast<size_t>(sizes.groups) + static_cast<size_t>(group) * 8;
			candidate.groups[group] = { read32(p_bytes, offset), read32(p_bytes, offset + 4) };
		}
		if (!zero_bytes(p_bytes, static_cast<size_t>(sizes.materials) + static_cast<size_t>(materials) * 4, static_cast<size_t>(sizes.groups)) ||
				!zero_bytes(p_bytes, static_cast<size_t>(sizes.groups) + static_cast<size_t>(groups) * 8, static_cast<size_t>(sizes.directory))) {
			return { ModelCodecError::INVALID_METADATA };
		}
		const auto valid = validate_root(candidate);
		if (valid.error != ModelCodecError::OK) {
			return valid;
		}
		r_root = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { ModelCodecError::MEMORY };
	}
}

ModelCodecResult encode_geometry_model_fixups(const GeometryModelRoot &p_root, uint32_t p_page,
		const HierarchyPage &p_chunk, std::vector<uint8_t> &r_bytes) {
	if (p_chunk.hierarchy_fixups.size() > MAX_PAGE_FIXUPS || p_chunk.cluster_fixups.size() > MAX_PAGE_FIXUPS) {
		return { ModelCodecError::INVALID_FIXUPS, p_page };
	}
	try {
		HierarchyPage chunk;
		chunk.clusters = p_chunk.clusters;
		chunk.hierarchy_fixups = p_chunk.hierarchy_fixups;
		chunk.cluster_fixups = p_chunk.cluster_fixups;
		std::sort(chunk.hierarchy_fixups.begin(), chunk.hierarchy_fixups.end(), [](const auto &p_first, const auto &p_second) { return hierarchy_key(p_first) < hierarchy_key(p_second); });
		std::sort(chunk.cluster_fixups.begin(), chunk.cluster_fixups.end(), [](const auto &p_first, const auto &p_second) { return cluster_key(p_first) < cluster_key(p_second); });
		const auto valid = validate_chunk(p_root, p_page, chunk);
		if (valid.error != ModelCodecError::OK) {
			return valid;
		}
		const size_t size = MODEL_FIXUP_HEADER_BYTES + chunk.hierarchy_fixups.size() * 24 + chunk.cluster_fixups.size() * 16;
		std::vector<uint8_t> bytes(size, 0);
		const uint32_t header[] = { MODEL_FIXUP_MAGIC, MODEL_FORMAT_VERSION, p_page, chunk.clusters, static_cast<uint32_t>(chunk.hierarchy_fixups.size()),
			static_cast<uint32_t>(chunk.cluster_fixups.size()), static_cast<uint32_t>(size), 0 };
		for (size_t word = 0; word < 8; ++word) {
			write32(bytes, word * 4, header[word]);
		}
		size_t offset = MODEL_FIXUP_HEADER_BYTES;
		for (const auto &fixup : chunk.hierarchy_fixups) {
			const uint32_t words[] = { fixup.page, fixup.node, fixup.child, fixup.cluster_start, fixup.dependency_first, fixup.dependency_count };
			for (size_t word = 0; word < 6; ++word) {
				write32(bytes, offset + word * 4, words[word]);
			}
			offset += 24;
		}
		for (const auto &fixup : chunk.cluster_fixups) {
			const uint32_t words[] = { fixup.page, fixup.cluster, fixup.dependency_first, fixup.dependency_count };
			for (size_t word = 0; word < 4; ++word) {
				write32(bytes, offset + word * 4, words[word]);
			}
			offset += 16;
		}
		r_bytes = std::move(bytes);
		return {};
	} catch (const std::bad_alloc &) {
		return { ModelCodecError::MEMORY };
	}
}

ModelCodecResult decode_geometry_model_fixups(const GeometryModelRoot &p_root, uint32_t p_page,
		const uint8_t *p_bytes, size_t p_size, HierarchyPage &r_chunk) {
	if (p_page >= p_root.hierarchy.pages.size() || p_bytes == nullptr || p_size < MODEL_FIXUP_HEADER_BYTES || p_size != p_root.directory.pages[p_page].fixup_bytes || read32(p_bytes, 0) != MODEL_FIXUP_MAGIC ||
			read32(p_bytes, 4) != MODEL_FORMAT_VERSION || read32(p_bytes, 8) != p_page || read32(p_bytes, 12) != p_root.hierarchy.pages[p_page].clusters || read32(p_bytes, 28) != 0) {
		return { ModelCodecError::INVALID_FIXUPS, p_page };
	}
	const uint32_t hierarchy_count = read32(p_bytes, 16);
	const uint32_t cluster_count = read32(p_bytes, 20);
	if (hierarchy_count > MAX_PAGE_FIXUPS || cluster_count > MAX_PAGE_FIXUPS || read32(p_bytes, 24) != p_size ||
			MODEL_FIXUP_HEADER_BYTES + static_cast<uint64_t>(hierarchy_count) * 24 + static_cast<uint64_t>(cluster_count) * 16 != p_size) {
		return { ModelCodecError::INVALID_LENGTH, p_page };
	}
	try {
		HierarchyPage chunk;
		chunk.clusters = p_root.hierarchy.pages[p_page].clusters;
		chunk.depth = p_root.hierarchy.pages[p_page].depth;
		chunk.first_part = p_root.hierarchy.pages[p_page].first_part;
		chunk.part_count = p_root.hierarchy.pages[p_page].part_count;
		chunk.hierarchy_fixups.resize(hierarchy_count);
		chunk.cluster_fixups.resize(cluster_count);
		size_t offset = MODEL_FIXUP_HEADER_BYTES;
		for (auto &fixup : chunk.hierarchy_fixups) {
			fixup = { read32(p_bytes, offset), read32(p_bytes, offset + 4), read32(p_bytes, offset + 8), read32(p_bytes, offset + 12), read32(p_bytes, offset + 16), read32(p_bytes, offset + 20) };
			offset += 24;
		}
		for (auto &fixup : chunk.cluster_fixups) {
			fixup = { read32(p_bytes, offset), read32(p_bytes, offset + 4), read32(p_bytes, offset + 8), read32(p_bytes, offset + 12) };
			offset += 16;
		}
		const auto valid = validate_chunk(p_root, p_page, chunk);
		if (valid.error != ModelCodecError::OK) {
			return valid;
		}
		r_chunk = std::move(chunk);
		return {};
	} catch (const std::bad_alloc &) {
		return { ModelCodecError::MEMORY };
	}
}

ModelCodecResult decode_geometry_model_page(const GeometryModelRoot &p_root, uint32_t p_page,
		const uint8_t *p_bytes, size_t p_size, GeometryPage &r_page, HierarchyPage &r_chunk) {
	if (p_page >= p_root.directory.pages.size() || p_bytes == nullptr) {
		return { ModelCodecError::INVALID_PAGE, p_page };
	}
	const auto &location = p_root.directory.pages[p_page];
	if (p_size != location.disk_bytes || location.fixup_offset > p_size || location.fixup_bytes != p_size - location.fixup_offset) {
		return { ModelCodecError::INVALID_LENGTH, p_page };
	}
	try {
		GeometryPage page;
		HierarchyPage chunk;
		if (decode_geometry_page(p_bytes, location.fixup_offset, page).error != PageCodecError::OK || page.root != (p_page < p_root.hierarchy.root_pages) ||
				page.clusters.size() != p_root.hierarchy.pages[p_page].clusters || read32(p_bytes, 20) != location.gpu_bytes) {
			return { ModelCodecError::INVALID_PAGE, p_page };
		}
		const auto fixed = decode_geometry_model_fixups(p_root, p_page, p_bytes + location.fixup_offset, location.fixup_bytes, chunk);
		if (fixed.error != ModelCodecError::OK) {
			return fixed;
		}
		const auto &metadata = p_root.hierarchy.pages[p_page];
		for (uint32_t id = metadata.first_part; id < metadata.first_part + metadata.part_count; ++id) {
			const auto &part = p_root.hierarchy.parts[id];
			const auto &branch = p_root.hierarchy.nodes[part.node].children[part.child];
			for (uint32_t local = part.cluster_start; local < part.cluster_start + part.cluster_count; ++local) {
				const auto &cluster = page.clusters[local];
				if (cluster.cluster >= p_root.cluster_count || cluster.group != part.group ||
						cluster.positions.precision != p_root.position_precision || cluster.position_error != p_root.position_error || !same_format(cluster.attributes.layout.format, p_root.attributes)) {
					return { ModelCodecError::INVALID_PAGE, p_page };
				}
				uint32_t flags = CLUSTER_FLAG_ROOT_LEAF | CLUSTER_FLAG_STREAMING_LEAF;
				if (streaming_range(p_root.groups[part.group], p_root.hierarchy.root_pages).second == 0) {
					flags |= CLUSTER_FLAG_ROOT_GROUP;
				}
				if (cluster.generating_group == UINT32_MAX) {
					flags |= CLUSTER_FLAG_FULL_LEAF;
				} else {
					if (cluster.generating_group >= cluster.group || cluster.generating_group >= p_root.groups.size()) {
						return { ModelCodecError::INVALID_PAGE, p_page };
					}
					auto range = streaming_range(p_root.groups[cluster.generating_group], p_root.hierarchy.root_pages);
					if (range.second == 0) {
						flags &= ~CLUSTER_FLAG_ROOT_LEAF;
					}
					if (!without_page(p_page, range)) {
						return { ModelCodecError::INVALID_PAGE, p_page };
					}
					if (range.second == 0) {
						flags &= ~CLUSTER_FLAG_STREAMING_LEAF;
					}
				}
				if (cluster.flags != flags || std::any_of(cluster.material_palette.begin(), cluster.material_palette.end(), [&](uint32_t p_slot) { return p_slot >= p_root.materials.size(); })) {
					return { ModelCodecError::INVALID_PAGE, p_page };
				}
				for (uint32_t axis = 0; axis < 3; ++axis) {
					if (cluster.lower[axis] < branch.lower[axis] || cluster.upper[axis] > branch.upper[axis]) {
						return { ModelCodecError::INVALID_PAGE, p_page };
					}
				}
			}
		}
		r_page = std::move(page);
		r_chunk = std::move(chunk);
		return {};
	} catch (const std::bad_alloc &) {
		return { ModelCodecError::MEMORY };
	}
}

} // namespace VirtualGeometry
