#ifndef VIRTUAL_GEOMETRY_MODEL_BUILDER_H
#define VIRTUAL_GEOMETRY_MODEL_BUILDER_H

#include "hierarchy_builder.h"

#include "core/io/virtual_geometry/model_codec.h"

namespace VirtualGeometry {

struct GeometryModelBundle {
	std::vector<uint8_t> root;
	std::vector<uint8_t> container;
};

enum class ModelBuildError {
	OK,
	INVALID_INPUT,
	METADATA_RANGE,
	PAGE_CODEC,
	FIXUP_CODEC,
	ROOT_CODEC,
	OUTPUT_LIMIT,
	MEMORY,
};

struct ModelBuildResult {
	ModelBuildError error = ModelBuildError::OK;
	uint32_t page = UINT32_MAX;
	ModelCodecResult codec_result;
	PageCodecResult page_result;
};

// Assemble already prepared geometry/pages/hierarchy from the same build.
// Publishes static flags, dense model material slots, per-page fixup bytes,
// directory depths/dependencies and the standalone model-root protocol.
// Encodes vertex sharing against declared dependency pages, then wraps each
// complete geometry/fixup block in one checksummed Zstd frame.
// Caller supplies the build/receipt identity; no source hashes are invented.
// Limit covers final root+container, not borrowed input or offline staging.
// No file writes or Resource objects. Whole old output survives refusal.
ModelBuildResult pack_geometry_model(const EncodableGeometry &p_geometry, const GeometryPageSet &p_pages,
		const GeometryHierarchy &p_hierarchy, const std::array<uint8_t, 32> &p_build_key,
		uint64_t p_max_output_bytes, GeometryModelBundle &r_bundle);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_MODEL_BUILDER_H
