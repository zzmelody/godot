#include "hierarchy_builder.h"

#include "metadata_bounds.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

struct Box {
	std::array<float, 3> lower{ INFINITY, INFINITY, INFINITY };
	std::array<float, 3> upper{ -INFINITY, -INFINITY, -INFINITY };
};

void include_box(const Box &p_box, Box &r_box) {
	for (uint32_t axis = 0; axis < 3; ++axis) {
		r_box.lower[axis] = std::min(r_box.lower[axis], p_box.lower[axis]);
		r_box.upper[axis] = std::max(r_box.upper[axis], p_box.upper[axis]);
	}
}

bool valid_box(const Box &p_box) {
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(p_box.lower[axis]) || !std::isfinite(p_box.upper[axis]) || p_box.lower[axis] > p_box.upper[axis]) {
			return false;
		}
	}
	return true;
}

struct IntermediateNode {
	Box bounds;
	uint32_t part = UINT32_MAX;
	std::array<uint32_t, HIERARCHY_FANOUT> children = {};
	uint32_t child_count = 0;
};

struct GroupParts {
	std::vector<uint32_t> parts;
	uint32_t first = 0;
	uint32_t count = 0;
};

struct PageRange {
	uint32_t first = 0;
	uint32_t count = 0;
};

PageRange without_roots(const GroupParts &p_group, uint32_t p_roots) {
	const uint32_t first = std::max(p_group.first, p_roots);
	const uint32_t end = p_group.first + p_group.count;
	return first < end ? PageRange{ first, end - first } : PageRange{};
}

bool remove_page(uint32_t p_page, PageRange &r_range) {
	if (r_range.count != 0 && p_page >= r_range.first && p_page - r_range.first < r_range.count) {
		if (p_page == r_range.first) {
			++r_range.first;
		} else if (p_page != r_range.first + r_range.count - 1) {
			return false;
		}
		--r_range.count;
		if (r_range.count == 0) {
			r_range.first = 0;
		}
	}
	return true;
}

class HierarchyBuilder {
	const EncodableGeometry &geometry;
	const GeometryPageSet &pages;
	GeometryHierarchy output;
	std::vector<GroupParts> groups;
	std::vector<std::vector<uint32_t>> page_clusters;
	std::vector<IntermediateNode> intermediate;
	std::vector<HierarchyBranch> leaves;
	HierarchyBuildResult result;

	bool fail(HierarchyBuildError p_error, uint32_t p_part = UINT32_MAX) {
		result = { p_error, p_part };
		return false;
	}

	bool map_pages() {
		if (geometry.clusters.empty() || geometry.clusters.size() >= UINT32_MAX / 2 || geometry.groups.empty() ||
				geometry.groups.size() >= UINT32_MAX || geometry.root_group >= geometry.groups.size() ||
				pages.clusters.size() != geometry.clusters.size() || pages.parts.empty() || pages.parts.size() > geometry.clusters.size() ||
				pages.directory.pages.empty() || pages.directory.pages.size() >= UINT32_MAX) {
			return fail(HierarchyBuildError::INVALID_GEOMETRY);
		}
		output.pages.resize(pages.directory.pages.size());
		page_clusters.resize(output.pages.size());
		for (uint32_t page = 0; page < output.pages.size(); ++page) {
			const uint32_t flags = pages.directory.pages[page].flags;
			if ((flags & ~PAGE_FLAG_ROOT) != 0 || (flags == PAGE_FLAG_ROOT && output.root_pages != page)) {
				return fail(HierarchyBuildError::INVALID_PAGES);
			}
			output.root_pages += flags == PAGE_FLAG_ROOT ? 1u : 0u;
		}
		if (output.root_pages == 0) {
			return fail(HierarchyBuildError::INVALID_PAGES);
		}
		for (const auto &location : pages.clusters) {
			if (location.page >= output.pages.size() || location.cluster >= (location.page < output.root_pages ? 64u : 256u)) {
				return fail(HierarchyBuildError::INVALID_PAGES);
			}
			auto &clusters = page_clusters[location.page];
			clusters.resize(std::max(clusters.size(), static_cast<size_t>(location.cluster) + 1), UINT32_MAX);
		}
		for (uint32_t cluster = 0; cluster < pages.clusters.size(); ++cluster) {
			const auto &location = pages.clusters[cluster];
			auto &slot = page_clusters[location.page][location.cluster];
			if (slot != UINT32_MAX) {
				return fail(HierarchyBuildError::INVALID_PAGES);
			}
			slot = cluster;
		}
		for (uint32_t page = 0; page < output.pages.size(); ++page) {
			const auto &clusters = page_clusters[page];
			if (clusters.empty() || std::find(clusters.begin(), clusters.end(), UINT32_MAX) != clusters.end()) {
				return fail(HierarchyBuildError::INVALID_PAGES);
			}
			output.pages[page].clusters = static_cast<uint32_t>(clusters.size());
		}
		return true;
	}

	bool map_parts() {
		groups.resize(geometry.groups.size());
		leaves.resize(pages.parts.size());
		intermediate.resize(pages.parts.size());
		output.parts.resize(pages.parts.size());
		std::vector<uint8_t> covered(geometry.clusters.size(), 0);
		for (uint32_t id = 0; id < pages.parts.size(); ++id) {
			const auto &part = pages.parts[id];
			if (part.group >= groups.size() || part.page >= page_clusters.size() || part.cluster_count == 0 ||
					part.cluster_start >= page_clusters[part.page].size() || part.cluster_count > page_clusters[part.page].size() - part.cluster_start) {
				return fail(HierarchyBuildError::INVALID_PAGES, id);
			}
			const auto &group = geometry.groups[part.group];
			if (!std::isfinite(group.min_child_error) || !std::isfinite(group.max_parent_error) || group.min_child_error < -1 ||
					group.max_parent_error < 0 || group.min_child_error > group.max_parent_error) {
				return fail(HierarchyBuildError::INVALID_GEOMETRY, id);
			}
			Box box;
			for (uint32_t local = part.cluster_start; local < part.cluster_start + part.cluster_count; ++local) {
				const uint32_t cluster = page_clusters[part.page][local];
				const auto &source = geometry.clusters[cluster].geometry;
				const Box bound{ source.lower, source.upper };
				if (covered[cluster]++ != 0 || source.group != part.group || !valid_box(bound) ||
						(part.page < output.root_pages) != (part.group == geometry.root_group)) {
					return fail(HierarchyBuildError::INVALID_GEOMETRY, id);
				}
				include_box(bound, box);
			}
			auto &leaf = leaves[id];
			leaf.lower = box.lower;
			leaf.upper = box.upper;
			leaf.part = id;
			if (!round_metadata_sphere(group.lod_bounds, leaf.lod_bounds) ||
					!round_metadata_error(group.min_child_error, false, leaf.min_error) || !round_metadata_error(group.max_parent_error, true, leaf.max_error)) {
				return fail(HierarchyBuildError::METADATA_RANGE, id);
			}
			intermediate[id].bounds = box;
			intermediate[id].part = id;
			output.parts[id] = { part.group, part.page, part.cluster_start, part.cluster_count };
			groups[part.group].parts.push_back(id);
		}
		if (std::find(covered.begin(), covered.end(), 0) != covered.end()) {
			return fail(HierarchyBuildError::INVALID_PAGES);
		}
		std::fill(covered.begin(), covered.end(), 0);
		for (uint32_t id = 0; id < groups.size(); ++id) {
			auto &group = groups[id];
			const auto &source = geometry.groups[id];
			if (group.parts.empty() || source.children.empty() || source.root != (id == geometry.root_group) ||
					(source.root ? !source.parents.empty() : source.parents.empty())) {
				return fail(HierarchyBuildError::INVALID_GEOMETRY);
			}
			std::sort(group.parts.begin(), group.parts.end(), [&](uint32_t p_left, uint32_t p_right) { return output.parts[p_left].page < output.parts[p_right].page; });
			group.first = output.parts[group.parts.front()].page;
			group.count = static_cast<uint32_t>(group.parts.size());
			if (group.count > MAX_GROUP_PAGE_PARTS) {
				return fail(HierarchyBuildError::GROUP_PAGE_LIMIT, group.parts.front());
			}
			uint32_t children = 0;
			for (uint32_t index = 0; index < group.count; ++index) {
				auto &part = output.parts[group.parts[index]];
				if (part.page != group.first + index) {
					return fail(HierarchyBuildError::INVALID_PAGES, group.parts[index]);
				}
				children += part.cluster_count;
				const auto dependency = without_roots(group, output.root_pages);
				part.dependency_first = dependency.first;
				part.dependency_count = dependency.count;
			}
			if (children != source.children.size()) {
				return fail(HierarchyBuildError::INVALID_GEOMETRY);
			}
			for (uint32_t child : source.children) {
				if (child >= covered.size() || covered[child]++ != 0 || geometry.clusters[child].geometry.group != id) {
					return fail(HierarchyBuildError::INVALID_GEOMETRY);
				}
			}
		}
		if (geometry.groups[geometry.root_group].children != geometry.root_clusters) {
			return fail(HierarchyBuildError::INVALID_GEOMETRY);
		}
		return true;
	}

	double cost(const std::vector<uint32_t> &p_order, size_t p_first, size_t p_count) const {
		Box box;
		for (size_t index = p_first; index < p_first + p_count; ++index) {
			include_box(intermediate[p_order[index]].bounds, box);
		}
		const double x = static_cast<double>(box.upper[0]) - box.lower[0];
		const double y = static_cast<double>(box.upper[1]) - box.lower[1];
		const double z = static_cast<double>(box.upper[2]) - box.lower[2];
		return 2 * (x * y + x * z + y * z);
	}

	void sort_buckets(std::vector<uint32_t> &r_order, size_t p_first, const std::array<uint32_t, 4> &p_sizes) {
		for (uint32_t level = 0; level < 2; ++level) {
			const uint32_t bucket_children = HIERARCHY_FANOUT >> level;
			const uint32_t half = bucket_children / 2;
			size_t first = p_first;
			for (uint32_t bucket = 0; bucket < (1u << level); ++bucket) {
				std::array<size_t, 2> counts = {};
				for (uint32_t child = 0; child < bucket_children; ++child) {
					counts[child / half] += p_sizes[bucket * bucket_children + child];
				}
				auto sort_axis = [&](uint32_t p_axis) {
					std::sort(r_order.begin() + first, r_order.begin() + first + counts[0] + counts[1], [&](uint32_t p_left, uint32_t p_right) {
						const auto &left = intermediate[p_left].bounds;
						const auto &right = intermediate[p_right].bounds;
						const double a = static_cast<double>(left.lower[p_axis]) + left.upper[p_axis];
						const double b = static_cast<double>(right.lower[p_axis]) + right.upper[p_axis];
						return a == b ? p_left < p_right : a < b;
					});
				};
				double best_cost = INFINITY;
				uint32_t best_axis = 0;
				for (uint32_t axis = 0; axis < 3; ++axis) {
					sort_axis(axis);
					const double candidate = cost(r_order, first, counts[0]) + cost(r_order, first + counts[0], counts[1]);
					if (candidate < best_cost) {
						best_cost = candidate;
						best_axis = axis;
					}
				}
				if (best_axis != 2) {
					sort_axis(best_axis);
				}
				first += counts[0] + counts[1];
			}
		}
	}

	uint32_t build_tree(std::vector<uint32_t> &r_order, size_t p_first, uint32_t p_count, bool p_sort) {
		if (p_count == 1) {
			return r_order[p_first];
		}
		const uint32_t id = static_cast<uint32_t>(intermediate.size());
		intermediate.emplace_back();
		std::array<uint32_t, 4> sizes = {};
		const uint32_t count = std::min(p_count, HIERARCHY_FANOUT);
		if (p_count <= HIERARCHY_FANOUT) {
			sizes.fill(1);
		} else {
			uint32_t top = HIERARCHY_FANOUT;
			while (top <= p_count / HIERARCHY_FANOUT) {
				top *= HIERARCHY_FANOUT;
			}
			const uint32_t small = top / HIERARCHY_FANOUT;
			uint32_t excess = p_count - top;
			for (uint32_t child = HIERARCHY_FANOUT; child-- > 0;) {
				const uint32_t extra = std::min(excess, top - small);
				sizes[child] = small + extra;
				excess -= extra;
			}
			if (p_sort) {
				sort_buckets(r_order, p_first, sizes);
			}
		}
		size_t first = p_first;
		for (uint32_t child = 0; child < count; ++child) {
			const uint32_t node = build_tree(r_order, first, sizes[child], p_sort);
			intermediate[id].children[child] = node;
			include_box(intermediate[node].bounds, intermediate[id].bounds);
			first += sizes[child];
		}
		intermediate[id].child_count = count;
		return id;
	}

	bool aggregate(uint32_t p_node, HierarchyBranch &r_branch) {
		Box box;
		std::array<double, 3> low{ INFINITY, INFINITY, INFINITY };
		std::array<double, 3> high{ -INFINITY, -INFINITY, -INFINITY };
		r_branch.min_error = INFINITY;
		r_branch.max_error = 0;
		for (const auto &child : output.nodes[p_node].children) {
			if (child.node == UINT32_MAX && child.part == UINT32_MAX) {
				break;
			}
			include_box({ child.lower, child.upper }, box);
			r_branch.min_error = std::min(r_branch.min_error, child.min_error);
			r_branch.max_error = std::max(r_branch.max_error, child.max_error);
			for (uint32_t axis = 0; axis < 3; ++axis) {
				low[axis] = std::min(low[axis], static_cast<double>(child.lod_bounds[axis]) - child.lod_bounds[3]);
				high[axis] = std::max(high[axis], static_cast<double>(child.lod_bounds[axis]) + child.lod_bounds[3]);
			}
		}
		GeometrySphere sphere;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			sphere.center[axis] = 0.5 * low[axis] + 0.5 * high[axis];
		}
		for (const auto &child : output.nodes[p_node].children) {
			if (child.node == UINT32_MAX && child.part == UINT32_MAX) {
				break;
			}
			const double radius = std::hypot(sphere.center[0] - child.lod_bounds[0], sphere.center[1] - child.lod_bounds[1], sphere.center[2] - child.lod_bounds[2]) + child.lod_bounds[3];
			sphere.radius = std::max(sphere.radius, std::nextafter(radius, std::numeric_limits<double>::infinity()));
		}
		r_branch.lower = box.lower;
		r_branch.upper = box.upper;
		r_branch.node = p_node;
		return round_metadata_sphere(sphere, r_branch.lod_bounds) || fail(HierarchyBuildError::METADATA_RANGE);
	}

	bool emit(uint32_t p_source, uint32_t p_depth, uint32_t &r_node) {
		if (p_depth > MAX_HIERARCHY_DEPTH) {
			return fail(HierarchyBuildError::DEPTH_LIMIT);
		}
		const uint32_t id = static_cast<uint32_t>(output.nodes.size());
		output.nodes.emplace_back();
		const auto source = intermediate[p_source];
		for (uint32_t child = 0; child < source.child_count; ++child) {
			const auto &next = intermediate[source.children[child]];
			HierarchyBranch branch;
			if (next.part != UINT32_MAX) {
				branch = leaves[next.part];
				auto &part = output.parts[next.part];
				part.node = id;
				part.child = child;
				output.pages[part.page].depth = std::max(output.pages[part.page].depth, p_depth);
			} else {
				uint32_t node;
				if (!emit(source.children[child], p_depth + 1, node) || !aggregate(node, branch)) {
					return false;
				}
			}
			output.nodes[id].children[child] = branch;
		}
		r_node = id;
		return true;
	}

	bool build_nodes() {
		std::map<uint32_t, std::vector<uint32_t>> levels;
		for (uint32_t part = 0; part < output.parts.size(); ++part) {
			levels[geometry.groups[output.parts[part].group].level].push_back(part);
		}
		std::vector<uint32_t> roots;
		for (auto &level : levels) {
			auto &order = level.second;
			const uint32_t node = build_tree(order, 0, static_cast<uint32_t>(order.size()), true);
			const auto &root = intermediate[node];
			if (root.part != UINT32_MAX || root.child_count == HIERARCHY_FANOUT) {
				roots.push_back(node);
			} else {
				roots.insert(roots.end(), root.children.begin(), root.children.begin() + root.child_count);
			}
		}
		uint32_t root = build_tree(roots, 0, static_cast<uint32_t>(roots.size()), false);
		if (intermediate[root].part != UINT32_MAX) {
			const uint32_t parent = static_cast<uint32_t>(intermediate.size());
			intermediate.emplace_back();
			intermediate[parent].child_count = 1;
			intermediate[parent].children[0] = root;
			root = parent;
		}
		uint32_t root_index;
		return emit(root, 0, root_index);
	}

	bool build_fixups() {
		for (const auto &group : groups) {
			for (uint32_t page = group.first; page < group.first + group.count; ++page) {
				for (uint32_t id : group.parts) {
					const auto &part = output.parts[id];
					output.pages[page].hierarchy_fixups.push_back({ part.page, part.node, part.child, part.cluster_start, part.dependency_first, part.dependency_count });
				}
			}
		}
		output.cluster_flags.resize(geometry.clusters.size(), CLUSTER_FLAG_ROOT_LEAF | CLUSTER_FLAG_STREAMING_LEAF);
		for (uint32_t id = 0; id < geometry.clusters.size(); ++id) {
			const auto &cluster = geometry.clusters[id].geometry;
			auto &flags = output.cluster_flags[id];
			if (without_roots(groups[cluster.group], output.root_pages).count == 0) {
				flags |= CLUSTER_FLAG_ROOT_GROUP;
			}
			if (cluster.generating_group == UINT32_MAX) {
				flags |= CLUSTER_FLAG_FULL_LEAF;
				continue;
			}
			if (cluster.generating_group >= groups.size() || cluster.generating_group == cluster.group) {
				return fail(HierarchyBuildError::INVALID_GEOMETRY);
			}
			const auto &source = geometry.groups[cluster.generating_group];
			if (std::find(source.parents.begin(), source.parents.end(), id) == source.parents.end() || cluster.level <= source.level) {
				return fail(HierarchyBuildError::INVALID_GEOMETRY);
			}
			const auto &group = groups[cluster.generating_group];
			auto dependency = without_roots(group, output.root_pages);
			if (dependency.count == 0) {
				flags &= ~CLUSTER_FLAG_ROOT_LEAF;
			}
			const auto &location = pages.clusters[id];
			if (!remove_page(location.page, dependency)) {
				return fail(HierarchyBuildError::INVALID_PAGES);
			}
			if (dependency.count == 0) {
				flags &= ~CLUSTER_FLAG_STREAMING_LEAF;
				continue;
			}
			for (uint32_t page = group.first; page < group.first + group.count; ++page) {
				output.pages[page].cluster_fixups.push_back({ location.page, location.cluster, dependency.first, dependency.count });
			}
		}
		for (uint32_t page = 0; page < output.pages.size(); ++page) {
			auto &record = output.pages[page];
			for (const auto &fixup : record.cluster_fixups) {
				if (fixup.page > page) {
					return fail(HierarchyBuildError::INVALID_PAGES);
				}
				if (fixup.page != page) {
					record.dependencies.push_back(fixup.page);
				}
			}
			std::sort(record.dependencies.begin(), record.dependencies.end());
			record.dependencies.erase(std::unique(record.dependencies.begin(), record.dependencies.end()), record.dependencies.end());
		}
		return true;
	}

public:
	HierarchyBuilder(const EncodableGeometry &p_geometry, const GeometryPageSet &p_pages) :
			geometry(p_geometry), pages(p_pages) {}

	HierarchyBuildResult build(GeometryHierarchy &r_hierarchy) {
		if (map_pages() && map_parts() && build_nodes() && build_fixups()) {
			r_hierarchy = std::move(output);
		}
		return result;
	}
};

} // namespace

HierarchyBuildResult build_geometry_hierarchy(const EncodableGeometry &p_geometry,
		const GeometryPageSet &p_pages, GeometryHierarchy &r_hierarchy) {
	try {
		HierarchyBuilder builder(p_geometry, p_pages);
		return builder.build(r_hierarchy);
	} catch (const std::bad_alloc &) {
		return { HierarchyBuildError::MEMORY };
	}
}

} // namespace VirtualGeometry
