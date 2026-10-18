#include "page_builder.h"

#include "metadata_bounds.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <utility>

namespace VirtualGeometry {
namespace {

bool page_cluster(const EncodableGeometry &p_geometry, uint32_t p_id, GeometryPageCluster &r_cluster) {
	const auto &source = p_geometry.clusters[p_id];
	if (source.geometry.lod_error < 0 || p_geometry.max_position_error < 0 ||
			!round_metadata_sphere(source.geometry.bounds, r_cluster.bounds) || !round_metadata_sphere(source.geometry.lod_bounds, r_cluster.lod_bounds) ||
			!round_metadata_error(source.geometry.lod_error, true, r_cluster.lod_error) || !round_metadata_error(p_geometry.max_position_error, true, r_cluster.position_error)) {
		return false;
	}
	r_cluster.cluster = p_id;
	r_cluster.group = source.geometry.group;
	r_cluster.generating_group = source.geometry.generating_group;
	r_cluster.level = source.geometry.level;
	r_cluster.lower = source.geometry.lower;
	r_cluster.upper = source.geometry.upper;
	r_cluster.positions = source.positions.layout;
	r_cluster.position_bytes = source.position_bytes;
	r_cluster.attributes = source.encoded_attributes;
	r_cluster.topology = source.topology;
	r_cluster.materials = source.materials;
	r_cluster.material_palette = source.material_palette;
	r_cluster.reuse_batch_words = source.reuse_batch_words;
	return true;
}

uint32_t morton(const std::array<uint32_t, 3> &p_point) {
	uint32_t result = 0;
	for (uint32_t bit = 0; bit < 10; ++bit) {
		for (uint32_t axis = 0; axis < 3; ++axis) {
			result |= ((p_point[axis] >> bit) & 1u) << (bit * 3 + axis);
		}
	}
	return result;
}

bool group_order(const EncodableGeometry &p_geometry, std::vector<uint32_t> &r_order) {
	std::array<double, 3> low{ INFINITY, INFINITY, INFINITY };
	std::array<double, 3> high{ -INFINITY, -INFINITY, -INFINITY };
	for (const auto &group : p_geometry.groups) {
		for (uint32_t axis = 0; axis < 3; ++axis) {
			if (!std::isfinite(group.lod_bounds.center[axis])) {
				return false;
			}
			low[axis] = std::min(low[axis], group.lod_bounds.center[axis]);
			high[axis] = std::max(high[axis], group.lod_bounds.center[axis]);
		}
	}
	const double extent = std::max({ high[0] - low[0], high[1] - low[1], high[2] - low[2] });
	if (!std::isfinite(extent)) {
		return false;
	}
	std::vector<uint32_t> keys(p_geometry.groups.size());
	for (size_t group = 0; group < keys.size(); ++group) {
		std::array<uint32_t, 3> point;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const double value = extent > 0 ? (p_geometry.groups[group].lod_bounds.center[axis] - low[axis]) / extent * 1023 : 0;
			point[axis] = static_cast<uint32_t>(std::clamp(std::floor(value + 0.5), 0.0, 1023.0));
		}
		keys[group] = morton(point) ^ ((p_geometry.groups[group].level & 1) != 0 ? UINT32_MAX : 0);
	}
	r_order.resize(keys.size());
	std::iota(r_order.begin(), r_order.end(), 0u);
	std::sort(r_order.begin(), r_order.end(), [&](uint32_t p_left, uint32_t p_right) {
		const auto &left = p_geometry.groups[p_left];
		const auto &right = p_geometry.groups[p_right];
		if (left.root != right.root) {
			return left.root;
		}
		if (left.level != right.level) {
			return left.level > right.level;
		}
		return keys[p_left] == keys[p_right] ? p_left < p_right : keys[p_left] < keys[p_right];
	});
	return true;
}

PageBuildResult flush_page(GeometryPage &r_page, uint64_t p_limit, GeometryPageSet &r_pages) {
	if (r_page.clusters.empty()) {
		return {};
	}
	std::vector<uint8_t> bytes;
	const auto encoded = encode_geometry_page(r_page, bytes);
	if (encoded.error != PageCodecError::OK) {
		const uint32_t cluster = encoded.cluster < r_page.clusters.size() ? r_page.clusters[encoded.cluster].cluster : UINT32_MAX;
		return { PageBuildError::PAGE_CODEC, cluster, encoded };
	}
	if (r_pages.container.size() > p_limit || bytes.size() > p_limit - r_pages.container.size()) {
		return { PageBuildError::OUTPUT_LIMIT };
	}
	GeometryPageSizes sizes;
	const auto measured = measure_geometry_page(r_page, sizes);
	if (measured.error != PageCodecError::OK) {
		return { PageBuildError::PAGE_CODEC, UINT32_MAX, measured };
	}
	PageLocation location;
	location.container_offset = r_pages.container.size();
	location.disk_bytes = static_cast<uint32_t>(bytes.size());
	location.gpu_bytes = sizes.gpu_bytes;
	location.flags = r_page.root ? PAGE_FLAG_ROOT : 0;
	r_pages.directory.pages.push_back(location);
	r_pages.container.insert(r_pages.container.end(), bytes.begin(), bytes.end());
	r_page.clusters.clear();
	return {};
}

PageBuildResult add_dependencies(const EncodableGeometry &p_geometry, GeometryPageSet &r_pages) {
	std::vector<std::vector<uint32_t>> dependencies(r_pages.directory.pages.size());
	for (const auto &part : r_pages.parts) {
		for (uint32_t parent : p_geometry.groups[part.group].parents) {
			if (parent >= r_pages.clusters.size() || r_pages.clusters[parent].page == INVALID_PAGE ||
					p_geometry.clusters[parent].geometry.generating_group != part.group ||
					p_geometry.clusters[parent].geometry.level <= p_geometry.groups[part.group].level) {
				return { PageBuildError::INVALID_GEOMETRY, parent };
			}
			const uint32_t page = r_pages.clusters[parent].page;
			if (page != part.page) {
				if (page > part.page) {
					return { PageBuildError::INVALID_GEOMETRY, parent };
				}
				dependencies[part.page].push_back(page);
			}
		}
	}
	for (size_t page = 0; page < dependencies.size(); ++page) {
		auto &list = dependencies[page];
		std::sort(list.begin(), list.end());
		list.erase(std::unique(list.begin(), list.end()), list.end());
		if (list.size() > UINT32_MAX - r_pages.directory.dependencies.size()) {
			return { PageBuildError::OUTPUT_LIMIT };
		}
		auto &record = r_pages.directory.pages[page];
		record.dependency_start = static_cast<uint32_t>(r_pages.directory.dependencies.size());
		record.dependency_count = static_cast<uint32_t>(list.size());
		r_pages.directory.dependencies.insert(r_pages.directory.dependencies.end(), list.begin(), list.end());
	}
	return {};
}

} // namespace

PageBuildResult build_geometry_pages(const EncodableGeometry &p_geometry, uint64_t p_max_output_bytes, GeometryPageSet &r_pages) {
	if (p_geometry.clusters.empty() || p_geometry.clusters.size() >= UINT32_MAX || p_geometry.groups.empty() ||
			p_geometry.groups.size() >= UINT32_MAX || p_geometry.root_group >= p_geometry.groups.size() || p_geometry.root_clusters.empty() ||
			!p_geometry.groups[p_geometry.root_group].root || p_geometry.groups[p_geometry.root_group].children != p_geometry.root_clusters) {
		return { PageBuildError::INVALID_GEOMETRY };
	}
	try {
		GeometryPageSet candidate;
		candidate.clusters.resize(p_geometry.clusters.size());
		std::vector<uint32_t> order;
		if (!group_order(p_geometry, order)) {
			return { PageBuildError::METADATA_RANGE };
		}
		GeometryPage page;
		page.root = true;
		for (uint32_t group : order) {
			const auto &source_group = p_geometry.groups[group];
			if (source_group.children.empty() || source_group.root != (group == p_geometry.root_group) ||
					(source_group.root ? !source_group.parents.empty() : source_group.parents.empty())) {
				return { PageBuildError::INVALID_GEOMETRY };
			}
			std::vector<uint32_t> children = source_group.children;
			for (uint32_t child : children) {
				if (child >= candidate.clusters.size() || p_geometry.clusters[child].geometry.group != group || candidate.clusters[child].page != INVALID_PAGE) {
					return { PageBuildError::INVALID_GEOMETRY, child };
				}
				const auto &geometry = p_geometry.clusters[child];
				if (geometry.positions.layout.precision != p_geometry.position_precision ||
						!std::all_of(geometry.geometry.bounds.center.begin(), geometry.geometry.bounds.center.end(), [](double p_value) {
							return std::isfinite(p_value) && std::abs(p_value) <= std::numeric_limits<float>::max();
						})) {
					return { PageBuildError::METADATA_RANGE, child };
				}
			}
			std::sort(children.begin(), children.end(), [&](uint32_t p_left, uint32_t p_right) {
				const auto &left = p_geometry.clusters[p_left].geometry.bounds.center;
				const auto &right = p_geometry.clusters[p_right].geometry.bounds.center;
				const double left_sum = left[0] / 3 + left[1] / 3 + left[2] / 3;
				const double right_sum = right[0] / 3 + right[1] / 3 + right[2] / 3;
				return left_sum == right_sum ? p_left < p_right : left_sum < right_sum;
			});
			for (uint32_t child : children) {
				if (candidate.clusters[child].page != INVALID_PAGE) {
					return { PageBuildError::INVALID_GEOMETRY, child };
				}
				GeometryPageCluster encoded;
				if (!page_cluster(p_geometry, child, encoded)) {
					return { PageBuildError::METADATA_RANGE, child };
				}
				page.clusters.push_back(std::move(encoded));
				GeometryPageSizes sizes;
				auto measured = measure_geometry_page(page, sizes);
				if (measured.error == PageCodecError::CAPACITY && page.clusters.size() > 1) {
					encoded = std::move(page.clusters.back());
					page.clusters.pop_back();
					const auto flushed = flush_page(page, p_max_output_bytes, candidate);
					if (flushed.error != PageBuildError::OK) {
						return flushed;
					}
					// UE's default minimum residency is zero KiB: use one root
					// page and fill its spare space across group boundaries.
					// The mandatory coarse cover must still remain root-resident.
					page.root = source_group.root;
					page.clusters.push_back(std::move(encoded));
					measured = measure_geometry_page(page, sizes);
				}
				if (measured.error != PageCodecError::OK) {
					return { PageBuildError::PAGE_CODEC, child, measured };
				}
				const uint32_t page_id = static_cast<uint32_t>(candidate.directory.pages.size());
				const uint32_t cluster_id = static_cast<uint32_t>(page.clusters.size() - 1);
				candidate.clusters[child] = { page_id, cluster_id };
				if (candidate.parts.empty() || candidate.parts.back().page != page_id || candidate.parts.back().group != group) {
					candidate.parts.push_back({ group, page_id, cluster_id, 0 });
				}
				++candidate.parts.back().cluster_count;
			}
		}
		for (const auto &cluster : candidate.clusters) {
			if (cluster.page == INVALID_PAGE) {
				return { PageBuildError::INVALID_GEOMETRY };
			}
		}
		const auto flushed = flush_page(page, p_max_output_bytes, candidate);
		if (flushed.error != PageBuildError::OK) {
			return flushed;
		}
		const auto dependencies = add_dependencies(p_geometry, candidate);
		if (dependencies.error != PageBuildError::OK) {
			return dependencies;
		}
		candidate.directory.container_bytes = candidate.container.size();
		const auto directory = encode_page_directory(candidate.directory, p_max_output_bytes, candidate.directory_bytes);
		if (directory.error != PageDirectoryError::OK) {
			return { PageBuildError::DIRECTORY, UINT32_MAX, {}, directory };
		}
		if (candidate.container.size() > p_max_output_bytes || candidate.directory_bytes.size() > p_max_output_bytes - candidate.container.size()) {
			return { PageBuildError::OUTPUT_LIMIT };
		}
		r_pages = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { PageBuildError::MEMORY };
	}
}

} // namespace VirtualGeometry
