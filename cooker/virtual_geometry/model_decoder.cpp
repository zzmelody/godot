#include "model_decoder.h"

#include "core/io/virtual_geometry/page_entropy.h"

#include <new>
#include <utility>

namespace VirtualGeometry {

ModelCodecResult GeometryModelPageDecoder::initialize(const GeometryModelRoot &p_root, uint32_t p_pages) {
	if (p_pages == 0 || p_pages > p_root.directory.pages.size()) {
		return { ModelCodecError::INVALID_DIRECTORY };
	}
	try {
		GeometryModelPageDecoder candidate;
		candidate.root = &p_root;
		candidate.users.resize(p_pages, 0);
		std::vector<uint32_t> pending(p_pages);
		std::vector<std::vector<uint32_t>> children(p_pages);
		for (uint32_t page = 0; page < p_pages; ++page) {
			const auto &location = p_root.directory.pages[page];
			if (location.dependency_start > p_root.directory.dependencies.size() || location.dependency_count > p_root.directory.dependencies.size() - location.dependency_start) {
				return { ModelCodecError::INVALID_DIRECTORY, page };
			}
			pending[page] = location.dependency_count;
			for (uint32_t index = 0; index < location.dependency_count; ++index) {
				const uint32_t dependency = p_root.directory.dependencies[location.dependency_start + index];
				if (dependency >= p_pages || dependency == page) {
					return { ModelCodecError::INVALID_DIRECTORY, page };
				}
				++candidate.users[dependency];
				children[dependency].push_back(page);
			}
		}
		for (uint32_t page = 0; page < p_pages; ++page) {
			if (pending[page] == 0) {
				candidate.order.push_back(page);
			}
		}
		for (size_t next_page = 0; next_page < candidate.order.size(); ++next_page) {
			for (uint32_t child : children[candidate.order[next_page]]) {
				if (--pending[child] == 0) {
					candidate.order.push_back(child);
				}
			}
		}
		if (candidate.order.size() != p_pages) {
			return { ModelCodecError::INVALID_DIRECTORY };
		}
		*this = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { ModelCodecError::MEMORY };
	}
}

ModelCodecResult GeometryModelPageDecoder::decode(uint32_t p_page, const uint8_t *p_bytes, size_t p_size,
		const GeometryPage *&r_geometry, HierarchyPage &r_fixups, std::vector<uint8_t> *r_gpu) {
	if (root == nullptr || next >= order.size() || order[next] != p_page || p_size != root->directory.pages[p_page].disk_bytes) {
		return { ModelCodecError::INVALID_PAGE, p_page };
	}
	try {
		const auto &location = root->directory.pages[p_page];
		std::vector<GeometryPageDependency> dependencies;
		for (uint32_t index = 0; index < location.dependency_count; ++index) {
			const uint32_t dependency = root->directory.dependencies[location.dependency_start + index];
			const auto source = parents.find(dependency);
			if (source == parents.end()) {
				return { ModelCodecError::INVALID_PAGE, p_page };
			}
			dependencies.push_back({ dependency, &source->second });
		}
		std::vector<uint8_t> decoded;
		if (decode_geometry_page_block(p_bytes, p_size, size_t(location.fixup_offset) + location.fixup_bytes, decoded).error != PageCodecError::OK) {
			return { ModelCodecError::INVALID_PAGE, p_page };
		}
		HierarchyPage fixups;
		const auto checked = validate_geometry_model_page_layout(*root, p_page, decoded.data(), decoded.size(), fixups);
		if (checked.error != ModelCodecError::OK) {
			return checked;
		}
		GeometryPage geometry;
		if (decode_geometry_page(decoded.data(), location.fixup_offset, geometry, dependencies).error != PageCodecError::OK) {
			return { ModelCodecError::INVALID_PAGE, p_page };
		}
		std::vector<uint8_t> gpu;
		if (r_gpu != nullptr && (transcode_geometry_page_cpu(decoded.data(), location.fixup_offset, gpu, dependencies).error != PageCodecError::OK || gpu.size() != location.gpu_bytes)) {
			return { ModelCodecError::INVALID_PAGE, p_page };
		}
		const GeometryPage *result;
		if (users[p_page] != 0) {
			result = &parents.emplace(p_page, std::move(geometry)).first->second;
		} else {
			current = std::move(geometry);
			result = &current;
		}
		for (const auto &dependency : dependencies) {
			if (--users[dependency.page] == 0) {
				parents.erase(dependency.page);
			}
		}
		++next;
		r_geometry = result;
		r_fixups = std::move(fixups);
		if (r_gpu != nullptr) {
			*r_gpu = std::move(gpu);
		}
		return {};
	} catch (const std::bad_alloc &) {
		return { ModelCodecError::MEMORY };
	}
}

} // namespace VirtualGeometry
