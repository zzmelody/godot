#ifndef VIRTUAL_GEOMETRY_MODEL_CODEC_H
#define VIRTUAL_GEOMETRY_MODEL_CODEC_H

#include "../virtual_geometry_page_directory.h"
#include "hierarchy.h"
#include "page_codec.h"

namespace VirtualGeometry {

constexpr uint32_t MODEL_ROOT_MAGIC = 0x524d4756; // VGMR
constexpr uint32_t MODEL_FIXUP_MAGIC = 0x58464756; // VGFX
constexpr uint32_t MODEL_FORMAT_VERSION = 2;
constexpr uint32_t MODEL_ROOT_HEADER_BYTES = 208;
constexpr uint32_t MODEL_NODE_BYTES = 256;
constexpr uint32_t MODEL_PART_BYTES = 32;
constexpr uint32_t MODEL_FIXUP_HEADER_BYTES = 32;
constexpr uint32_t MAX_PAGE_FIXUPS = MAX_CLUSTER_VERTICES * MAX_GROUP_PAGE_PARTS;

struct ModelGroupPages {
	uint32_t first = 0;
	uint32_t count = 0;
};

struct GeometryModelRoot {
	std::array<uint8_t, 32> build_key = {};
	std::array<float, 3> lower = {};
	std::array<float, 3> upper = {};
	ClusterAttributeFormat attributes;
	int32_t position_precision = 0;
	float position_error = 0;
	uint32_t cluster_count = 0;
	uint32_t root_group = 0;
	// Sorted external material identities; page palettes store dense indices.
	std::vector<uint32_t> materials;
	std::vector<ModelGroupPages> groups;
	// Root metadata only. No offline cluster_flags, page fixups or duplicate
	// dependency vectors are serialized here. Resident chunks have separate owners.
	GeometryHierarchy hierarchy;
	PageDirectory directory;
};

enum class ModelCodecError {
	OK,
	INVALID_HEADER,
	INVALID_LENGTH,
	INVALID_METADATA,
	INVALID_TREE,
	INVALID_PARTS,
	INVALID_DIRECTORY,
	INVALID_FIXUPS,
	INVALID_PAGE,
	MEMORY_LIMIT,
	MEMORY,
};

struct ModelCodecResult {
	ModelCodecError error = ModelCodecError::OK;
	uint32_t index = UINT32_MAX;
};

// Explicit little-endian, canonical offsets/zero padding, fixed record strides.
// Decoder borrows only the root and actual logical container length; detail
// bytes and fixups are not read at startup. Budget bounds owned array payloads,
// root validation scratch and a conservative directory-validation allowance.
// Caller separately owns input and old output. Refusal preserves old output.
ModelCodecResult encode_geometry_model_root(const GeometryModelRoot &p_root, uint64_t p_max_memory_bytes, std::vector<uint8_t> &r_bytes);
ModelCodecResult decode_geometry_model_root(const uint8_t *p_bytes, size_t p_size, uint64_t p_container_bytes,
		uint64_t p_max_memory_bytes, GeometryModelRoot &r_root);

// Borrow a root validated by the root codec or produced by model assembly.
// Chunks travel with individual pages, not the resident root. Encoded fixups are
// sorted by destination, unique, bounded and checked against root metadata.
ModelCodecResult encode_geometry_model_fixups(const GeometryModelRoot &p_root, uint32_t p_page,
		const HierarchyPage &p_chunk, std::vector<uint8_t> &r_bytes);
ModelCodecResult decode_geometry_model_fixups(const GeometryModelRoot &p_root, uint32_t p_page,
		const uint8_t *p_bytes, size_t p_size, HierarchyPage &r_chunk);

// One demanded container block = disk geometry followed by its fixup chunk.
// Runtime installation check: canonical byte ranges, strip metadata, model
// identity and fixups, without expanding positions, attributes or triangles.
// Borrows immutable publication-admitted content; this is not asset admission.
ModelCodecResult validate_geometry_model_page_layout(const GeometryModelRoot &p_root, uint32_t p_page,
		const uint8_t *p_bytes, size_t p_size, HierarchyPage &r_chunk);

// Checks metadata identity, material slots, attributes, static flags and part
// membership. No I/O, hashing, runtime admission or GPU installation occurs.
ModelCodecResult decode_geometry_model_page(const GeometryModelRoot &p_root, uint32_t p_page,
		const uint8_t *p_bytes, size_t p_size, GeometryPage &r_page, HierarchyPage &r_chunk);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_MODEL_CODEC_H
