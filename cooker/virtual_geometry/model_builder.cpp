#include "model_builder.h"

#include "metadata_bounds.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace VirtualGeometry {

ModelBuildResult pack_geometry_model(const EncodableGeometry &p_geometry, const GeometryPageSet &p_pages,
		const GeometryHierarchy &p_hierarchy, const std::array<uint8_t, 32> &p_build_key,
		uint64_t p_max_output_bytes, GeometryModelBundle &r_bundle) {
	if (p_geometry.clusters.empty() || p_geometry.clusters.size() > UINT32_MAX || p_geometry.groups.empty() ||
			p_geometry.groups.size() > UINT32_MAX || p_geometry.root_group >= p_geometry.groups.size() ||
			p_hierarchy.cluster_flags.size() != p_geometry.clusters.size() || p_pages.clusters.size() != p_geometry.clusters.size() ||
			p_hierarchy.parts.size() != p_pages.parts.size() || p_hierarchy.pages.size() != p_pages.directory.pages.size() || p_geometry.max_position_error < 0) {
		return { ModelBuildError::INVALID_INPUT };
	}
	try {
		GeometryModelRoot root;
		root.build_key = p_build_key;
		root.cluster_count = static_cast<uint32_t>(p_geometry.clusters.size());
		root.root_group = p_geometry.root_group;
		root.position_precision = p_geometry.position_precision;
		root.attributes = p_geometry.clusters.front().encoded_attributes.layout.format;
		if (!round_metadata_error(p_geometry.max_position_error, true, root.position_error)) {
			return { ModelBuildError::METADATA_RANGE };
		}
		for (uint32_t axis = 0; axis < 3; ++axis) {
			if (!round_metadata_error(p_geometry.lower[axis], false, root.lower[axis]) || !round_metadata_error(p_geometry.upper[axis], true, root.upper[axis])) {
				return { ModelBuildError::METADATA_RANGE };
			}
		}
		for (const auto &cluster : p_geometry.clusters) {
			root.materials.insert(root.materials.end(), cluster.material_palette.begin(), cluster.material_palette.end());
		}
		std::sort(root.materials.begin(), root.materials.end());
		root.materials.erase(std::unique(root.materials.begin(), root.materials.end()), root.materials.end());
		root.hierarchy.nodes = p_hierarchy.nodes;
		root.hierarchy.parts = p_hierarchy.parts;
		root.hierarchy.root_pages = p_hierarchy.root_pages;
		root.hierarchy.pages.resize(p_hierarchy.pages.size());
		root.groups.resize(p_geometry.groups.size(), { UINT32_MAX, 0 });
		for (const auto &part : root.hierarchy.parts) {
			if (part.group >= root.groups.size()) {
				return { ModelBuildError::INVALID_INPUT };
			}
			auto &group = root.groups[part.group];
			group.first = std::min(group.first, part.page);
			++group.count;
		}
		for (size_t page = 0; page < p_hierarchy.pages.size(); ++page) {
			const auto &source = p_hierarchy.pages[page];
			auto &metadata = root.hierarchy.pages[page];
			metadata.clusters = source.clusters;
			metadata.depth = source.depth;
			metadata.first_part = source.first_part;
			metadata.part_count = source.part_count;
		}
		GeometryModelBundle candidate;
		root.directory.pages.resize(p_pages.directory.pages.size());
		for (size_t page = 0; page < p_hierarchy.pages.size(); ++page) {
			const auto &dependencies = p_hierarchy.pages[page].dependencies;
			if (root.directory.dependencies.size() > UINT32_MAX || dependencies.size() > UINT32_MAX - root.directory.dependencies.size()) {
				return { ModelBuildError::OUTPUT_LIMIT };
			}
			root.directory.pages[page].dependency_start = static_cast<uint32_t>(root.directory.dependencies.size());
			root.directory.pages[page].dependency_count = static_cast<uint32_t>(dependencies.size());
			root.directory.dependencies.insert(root.directory.dependencies.end(), dependencies.begin(), dependencies.end());
		}
		std::vector<uint8_t> seen(p_geometry.clusters.size(), 0);
		for (uint32_t page = 0; page < p_pages.directory.pages.size(); ++page) {
			const auto &source = p_pages.directory.pages[page];
			if (source.fixup_bytes != 0 || source.container_offset > p_pages.container.size() || source.disk_bytes > p_pages.container.size() - source.container_offset) {
				return { ModelBuildError::INVALID_INPUT, page };
			}
			GeometryPage decoded;
			if (decode_geometry_page(p_pages.container.data() + source.container_offset, source.disk_bytes, decoded).error != PageCodecError::OK ||
					decoded.root != (page < p_hierarchy.root_pages) || decoded.clusters.size() != p_hierarchy.pages[page].clusters) {
				return { ModelBuildError::PAGE_CODEC, page };
			}
			for (uint32_t local = 0; local < decoded.clusters.size(); ++local) {
				auto &cluster = decoded.clusters[local];
				if (cluster.cluster >= seen.size() || seen[cluster.cluster]++ != 0 || p_pages.clusters[cluster.cluster].page != page || p_pages.clusters[cluster.cluster].cluster != local ||
						cluster.group != p_geometry.clusters[cluster.cluster].geometry.group || cluster.generating_group != p_geometry.clusters[cluster.cluster].geometry.generating_group) {
					return { ModelBuildError::INVALID_INPUT, page };
				}
				cluster.flags = p_hierarchy.cluster_flags[cluster.cluster];
				for (uint32_t &material : cluster.material_palette) {
					const auto found = std::lower_bound(root.materials.begin(), root.materials.end(), material);
					if (found == root.materials.end() || *found != material) {
						return { ModelBuildError::INVALID_INPUT, page };
					}
					material = static_cast<uint32_t>(found - root.materials.begin());
				}
			}
			std::vector<uint8_t> geometry_bytes;
			if (encode_geometry_page(decoded, geometry_bytes).error != PageCodecError::OK) {
				return { ModelBuildError::PAGE_CODEC, page };
			}
			std::vector<uint8_t> fixup_bytes;
			const auto fixups = encode_geometry_model_fixups(root, page, p_hierarchy.pages[page], fixup_bytes);
			if (fixups.error != ModelCodecError::OK) {
				return { ModelBuildError::FIXUP_CODEC, page, fixups };
			}
			const uint64_t extra = static_cast<uint64_t>(geometry_bytes.size()) + fixup_bytes.size();
			if (candidate.container.size() > p_max_output_bytes || extra > p_max_output_bytes - candidate.container.size() || extra > UINT32_MAX) {
				return { ModelBuildError::OUTPUT_LIMIT, page };
			}
			auto &location = root.directory.pages[page];
			location.container_offset = candidate.container.size();
			location.disk_bytes = static_cast<uint32_t>(extra);
			location.gpu_bytes = source.gpu_bytes;
			location.flags = source.flags;
			location.fixup_offset = static_cast<uint32_t>(geometry_bytes.size());
			location.fixup_bytes = static_cast<uint32_t>(fixup_bytes.size());
			location.hierarchy_depth = p_hierarchy.pages[page].depth;
			candidate.container.insert(candidate.container.end(), geometry_bytes.begin(), geometry_bytes.end());
			candidate.container.insert(candidate.container.end(), fixup_bytes.begin(), fixup_bytes.end());
		}
		if (std::find(seen.begin(), seen.end(), 0) != seen.end()) {
			return { ModelBuildError::INVALID_INPUT };
		}
		root.directory.container_bytes = candidate.container.size();
		const auto encoded = encode_geometry_model_root(root, p_max_output_bytes, candidate.root);
		if (encoded.error != ModelCodecError::OK) {
			return { ModelBuildError::ROOT_CODEC, UINT32_MAX, encoded };
		}
		if (candidate.root.size() > p_max_output_bytes - candidate.container.size()) {
			return { ModelBuildError::OUTPUT_LIMIT };
		}
		r_bundle = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { ModelBuildError::MEMORY };
	}
}

} // namespace VirtualGeometry
