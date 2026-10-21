#ifndef VIRTUAL_GEOMETRY_PAGE_DIRECTORY_H
#define VIRTUAL_GEOMETRY_PAGE_DIRECTORY_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Value-only format shared by Cooker and rendering storage. No Resource, RID,
// file access or GPU work belongs to this module.
namespace VirtualGeometry {

constexpr uint32_t PAGE_DIRECTORY_MAGIC = 0x44504756; // "VGPD", little endian.
constexpr uint32_t PAGE_DIRECTORY_VERSION = 2;
constexpr uint32_t PAGE_DIRECTORY_HEADER_BYTES = 32;
constexpr uint32_t PAGE_DIRECTORY_RECORD_BYTES = 40;
constexpr uint32_t ROOT_PAGE_GPU_CAPACITY = 32 * 1024;
constexpr uint32_t STREAMING_PAGE_GPU_CAPACITY = 128 * 1024;
constexpr uint32_t MAX_HIERARCHY_DEPTH = 14;
constexpr uint32_t PAGE_FLAG_ROOT = 1;
constexpr uint32_t INVALID_PAGE = std::numeric_limits<uint32_t>::max();

struct PageLocation {
	uint64_t container_offset = 0;
	uint32_t disk_bytes = 0;
	uint32_t gpu_bytes = 0;
	uint32_t dependency_start = 0;
	uint32_t dependency_count = 0;
	// Offsets in the decoded block. disk_bytes is the compressed frame size
	// for published models; raw independent pages exist only during assembly.
	uint32_t fixup_offset = 0;
	uint32_t fixup_bytes = 0;
	uint32_t flags = 0;
	uint32_t hierarchy_depth = 0;
};

struct PageDirectory {
	uint64_t container_bytes = 0;
	std::vector<PageLocation> pages;
	// Concatenated, strictly increasing dependency lists, addressed by page ID.
	std::vector<uint32_t> dependencies;
};

enum class PageDirectoryError {
	OK,
	TRUNCATED,
	INVALID_MAGIC,
	UNSUPPORTED_VERSION,
	INVALID_LENGTH,
	MEMORY_LIMIT,
	INVALID_PAGE_RANGE,
	OVERLAPPING_PAGES,
	INVALID_GPU_SIZE,
	INVALID_FIXUP_RANGE,
	INVALID_FLAGS,
	INVALID_HIERARCHY_DEPTH,
	INVALID_ROOT_SET,
	INVALID_DEPENDENCY_RANGE,
	INVALID_DEPENDENCY,
	ROOT_DEPENDS_ON_STREAMING_PAGE,
	DEPENDENCY_CYCLE,
};

struct PageDirectoryResult {
	PageDirectoryError error = PageDirectoryError::OK;
	uint32_t page = INVALID_PAGE;
};

// The byte input is borrowed only for the call. The container size comes from
// the actual logical container file, without reading its payload or hashing it.
// The mandatory memory limit bounds new array payloads plus validation scratch;
// the caller separately accounts for borrowed inputs and the previous output.
// Output is replaced only on success. No native struct layout is serialized.
PageDirectoryResult decode_page_directory(const uint8_t *p_bytes, size_t p_size, uint64_t p_container_bytes, uint64_t p_max_memory_bytes, PageDirectory &r_directory);
PageDirectoryResult encode_page_directory(const PageDirectory &p_directory, uint64_t p_max_memory_bytes, std::vector<uint8_t> &r_bytes);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_PAGE_DIRECTORY_H
