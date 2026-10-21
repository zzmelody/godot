#include "virtual_geometry_page_directory.h"

#include <algorithm>
#include <numeric>
#include <utility>

namespace VirtualGeometry {
namespace {

uint32_t read_u32(const uint8_t *p_bytes) {
	return static_cast<uint32_t>(p_bytes[0]) |
			(static_cast<uint32_t>(p_bytes[1]) << 8) |
			(static_cast<uint32_t>(p_bytes[2]) << 16) |
			(static_cast<uint32_t>(p_bytes[3]) << 24);
}

uint64_t read_u64(const uint8_t *p_bytes) {
	return read_u32(p_bytes) | (static_cast<uint64_t>(read_u32(p_bytes + 4)) << 32);
}

void write_u32(uint8_t *p_bytes, uint32_t p_value) {
	for (uint32_t byte = 0; byte < 4; ++byte) {
		p_bytes[byte] = static_cast<uint8_t>(p_value >> (byte * 8));
	}
}

void write_u64(uint8_t *p_bytes, uint64_t p_value) {
	write_u32(p_bytes, static_cast<uint32_t>(p_value));
	write_u32(p_bytes + 4, static_cast<uint32_t>(p_value >> 32));
}

uint64_t serialized_bytes(uint64_t p_pages, uint64_t p_dependencies) {
	return PAGE_DIRECTORY_HEADER_BYTES + p_pages * PAGE_DIRECTORY_RECORD_BYTES + p_dependencies * 4;
}

struct VisitFrame {
	uint32_t page = 0;
	uint32_t next_dependency = 0;
};

bool fits_memory(uint64_t p_pages, uint64_t p_owned_bytes, uint64_t p_limit) {
	// Sorting and graph walking run in separate scopes. Charge the larger peak.
	constexpr uint64_t scratch_per_page = sizeof(VisitFrame) + sizeof(uint8_t);
	static_assert(scratch_per_page >= sizeof(uint32_t));
	return p_owned_bytes <= p_limit && p_pages <= (p_limit - p_owned_bytes) / scratch_per_page;
}

PageDirectoryResult validate_directory(const PageDirectory &p_directory) {
	const size_t page_count = p_directory.pages.size();
	if (page_count == 0) {
		return { PageDirectoryError::INVALID_ROOT_SET };
	}
	uint64_t dependency_cursor = 0;
	bool has_root = false;
	for (uint32_t page_id = 0; page_id < page_count; ++page_id) {
		const PageLocation &page = p_directory.pages[page_id];
		if ((page.flags & ~PAGE_FLAG_ROOT) != 0) {
			return { PageDirectoryError::INVALID_FLAGS, page_id };
		}
		const bool is_root = (page.flags & PAGE_FLAG_ROOT) != 0;
		has_root |= is_root;
		const uint32_t capacity = is_root ? ROOT_PAGE_GPU_CAPACITY : STREAMING_PAGE_GPU_CAPACITY;
		if (page.gpu_bytes == 0 || page.gpu_bytes > capacity || (page.gpu_bytes & 3) != 0) {
			return { PageDirectoryError::INVALID_GPU_SIZE, page_id };
		}
		if (page.disk_bytes == 0 || page.container_offset > p_directory.container_bytes || page.disk_bytes > p_directory.container_bytes - page.container_offset) {
			return { PageDirectoryError::INVALID_PAGE_RANGE, page_id };
		}
		if (page.fixup_bytes > UINT32_MAX - page.fixup_offset) {
			return { PageDirectoryError::INVALID_FIXUP_RANGE, page_id };
		}
		if (page.hierarchy_depth > MAX_HIERARCHY_DEPTH) {
			return { PageDirectoryError::INVALID_HIERARCHY_DEPTH, page_id };
		}
		if (page.dependency_start != dependency_cursor || page.dependency_count > p_directory.dependencies.size() - dependency_cursor) {
			return { PageDirectoryError::INVALID_DEPENDENCY_RANGE, page_id };
		}
		for (uint32_t index = 0; index < page.dependency_count; ++index) {
			const uint32_t dependency = p_directory.dependencies[dependency_cursor + index];
			if (dependency >= page_count || dependency == page_id || (index != 0 && dependency <= p_directory.dependencies[dependency_cursor + index - 1])) {
				return { PageDirectoryError::INVALID_DEPENDENCY, page_id };
			}
			if (is_root && (p_directory.pages[dependency].flags & PAGE_FLAG_ROOT) == 0) {
				return { PageDirectoryError::ROOT_DEPENDS_ON_STREAMING_PAGE, page_id };
			}
		}
		dependency_cursor += page.dependency_count;
	}
	if (dependency_cursor != p_directory.dependencies.size()) {
		return { PageDirectoryError::INVALID_DEPENDENCY_RANGE };
	}
	if (!has_root) {
		return { PageDirectoryError::INVALID_ROOT_SET };
	}
	{
		std::vector<uint32_t> ordered(page_count);
		std::iota(ordered.begin(), ordered.end(), 0u);
		std::sort(ordered.begin(), ordered.end(), [&p_directory](uint32_t p_left, uint32_t p_right) {
			return p_directory.pages[p_left].container_offset < p_directory.pages[p_right].container_offset;
		});
		uint64_t previous_end = 0;
		for (uint32_t page_id : ordered) {
			const PageLocation &page = p_directory.pages[page_id];
			if (page.container_offset < previous_end) {
				return { PageDirectoryError::OVERLAPPING_PAGES, page_id };
			}
			previous_end = page.container_offset + page.disk_bytes;
		}
	}
	// Iterative DFS avoids consuming the process stack on an external graph.
	std::vector<uint8_t> state(page_count, 0);
	std::vector<VisitFrame> stack(page_count);
	for (uint32_t page_id = 0; page_id < page_count; ++page_id) {
		if (state[page_id] != 0) {
			continue;
		}
		state[page_id] = 1;
		stack[0] = { page_id, 0 };
		size_t depth = 1;
		while (depth != 0) {
			VisitFrame &frame = stack[depth - 1];
			const PageLocation &page = p_directory.pages[frame.page];
			if (frame.next_dependency == page.dependency_count) {
				state[frame.page] = 2;
				--depth;
				continue;
			}
			const uint32_t dependency = p_directory.dependencies[static_cast<size_t>(page.dependency_start) + frame.next_dependency++];
			if (state[dependency] == 1) {
				return { PageDirectoryError::DEPENDENCY_CYCLE, frame.page };
			}
			if (state[dependency] == 0) {
				state[dependency] = 1;
				stack[depth++] = { dependency, 0 };
			}
		}
	}
	return {};
}

} // namespace

PageDirectoryResult decode_page_directory(const uint8_t *p_bytes, size_t p_size, uint64_t p_container_bytes, uint64_t p_max_memory_bytes, PageDirectory &r_directory) {
	if (p_bytes == nullptr || p_size < PAGE_DIRECTORY_HEADER_BYTES) {
		return { PageDirectoryError::TRUNCATED };
	}
	if (read_u32(p_bytes) != PAGE_DIRECTORY_MAGIC) {
		return { PageDirectoryError::INVALID_MAGIC };
	}
	if (read_u32(p_bytes + 4) != PAGE_DIRECTORY_VERSION) {
		return { PageDirectoryError::UNSUPPORTED_VERSION };
	}
	const uint32_t page_count = read_u32(p_bytes + 8);
	const uint32_t dependency_count = read_u32(p_bytes + 12);
	const uint64_t expected_bytes = serialized_bytes(page_count, dependency_count);
	if (read_u64(p_bytes + 16) != expected_bytes || p_size != expected_bytes || read_u64(p_bytes + 24) != p_container_bytes) {
		return { PageDirectoryError::INVALID_LENGTH };
	}
	const uint64_t owned_bytes = static_cast<uint64_t>(page_count) * sizeof(PageLocation) + static_cast<uint64_t>(dependency_count) * sizeof(uint32_t);
	if (!fits_memory(page_count, owned_bytes, p_max_memory_bytes)) {
		return { PageDirectoryError::MEMORY_LIMIT };
	}
	PageDirectory candidate;
	candidate.container_bytes = p_container_bytes;
	candidate.pages.resize(page_count);
	candidate.dependencies.resize(dependency_count);
	size_t offset = PAGE_DIRECTORY_HEADER_BYTES;
	for (PageLocation &page : candidate.pages) {
		page.container_offset = read_u64(p_bytes + offset);
		page.disk_bytes = read_u32(p_bytes + offset + 8);
		page.gpu_bytes = read_u32(p_bytes + offset + 12);
		page.dependency_start = read_u32(p_bytes + offset + 16);
		page.dependency_count = read_u32(p_bytes + offset + 20);
		page.fixup_offset = read_u32(p_bytes + offset + 24);
		page.fixup_bytes = read_u32(p_bytes + offset + 28);
		page.flags = read_u32(p_bytes + offset + 32);
		page.hierarchy_depth = read_u32(p_bytes + offset + 36);
		offset += PAGE_DIRECTORY_RECORD_BYTES;
	}
	for (uint32_t &dependency : candidate.dependencies) {
		dependency = read_u32(p_bytes + offset);
		offset += 4;
	}
	PageDirectoryResult result = validate_directory(candidate);
	if (result.error == PageDirectoryError::OK) {
		r_directory = std::move(candidate);
	}
	return result;
}

PageDirectoryResult encode_page_directory(const PageDirectory &p_directory, uint64_t p_max_memory_bytes, std::vector<uint8_t> &r_bytes) {
	if (p_directory.pages.size() > std::numeric_limits<uint32_t>::max() || p_directory.dependencies.size() > std::numeric_limits<uint32_t>::max()) {
		return { PageDirectoryError::INVALID_LENGTH };
	}
	const uint64_t file_bytes = serialized_bytes(p_directory.pages.size(), p_directory.dependencies.size());
	if (file_bytes > std::numeric_limits<size_t>::max() || !fits_memory(p_directory.pages.size(), file_bytes, p_max_memory_bytes)) {
		return { PageDirectoryError::MEMORY_LIMIT };
	}
	PageDirectoryResult result = validate_directory(p_directory);
	if (result.error != PageDirectoryError::OK) {
		return result;
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(file_bytes));
	write_u32(bytes.data(), PAGE_DIRECTORY_MAGIC);
	write_u32(bytes.data() + 4, PAGE_DIRECTORY_VERSION);
	write_u32(bytes.data() + 8, static_cast<uint32_t>(p_directory.pages.size()));
	write_u32(bytes.data() + 12, static_cast<uint32_t>(p_directory.dependencies.size()));
	write_u64(bytes.data() + 16, file_bytes);
	write_u64(bytes.data() + 24, p_directory.container_bytes);
	size_t offset = PAGE_DIRECTORY_HEADER_BYTES;
	for (const PageLocation &page : p_directory.pages) {
		write_u64(bytes.data() + offset, page.container_offset);
		write_u32(bytes.data() + offset + 8, page.disk_bytes);
		write_u32(bytes.data() + offset + 12, page.gpu_bytes);
		write_u32(bytes.data() + offset + 16, page.dependency_start);
		write_u32(bytes.data() + offset + 20, page.dependency_count);
		write_u32(bytes.data() + offset + 24, page.fixup_offset);
		write_u32(bytes.data() + offset + 28, page.fixup_bytes);
		write_u32(bytes.data() + offset + 32, page.flags);
		write_u32(bytes.data() + offset + 36, page.hierarchy_depth);
		offset += PAGE_DIRECTORY_RECORD_BYTES;
	}
	for (uint32_t dependency : p_directory.dependencies) {
		write_u32(bytes.data() + offset, dependency);
		offset += 4;
	}
	r_bytes = std::move(bytes);
	return {};
}

} // namespace VirtualGeometry
