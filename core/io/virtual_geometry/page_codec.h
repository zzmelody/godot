#ifndef VIRTUAL_GEOMETRY_PAGE_CODEC_H
#define VIRTUAL_GEOMETRY_PAGE_CODEC_H

#include "attribute_codec.h"
#include "topology_codec.h"

namespace VirtualGeometry {

constexpr uint32_t GEOMETRY_DISK_PAGE_MAGIC = 0x47504756; // VGPG
constexpr uint32_t GEOMETRY_GPU_PAGE_MAGIC = 0x50474756; // VGGP
constexpr uint32_t GEOMETRY_PAGE_VERSION = 2;
constexpr uint32_t GEOMETRY_PAGE_HEADER_BYTES = 32;
constexpr uint32_t GEOMETRY_PAGE_CLUSTER_BYTES = 288;

struct GeometryPageCluster {
	uint32_t cluster = 0;
	uint32_t group = UINT32_MAX;
	uint32_t generating_group = UINT32_MAX;
	uint32_t level = 0;
	// Static full/root/streaming leaf flags, finalized by model assembly.
	uint32_t flags = 0;
	std::array<float, 4> bounds = {};
	std::array<float, 4> lod_bounds = {};
	float lod_error = 0;
	float position_error = 0;
	std::array<float, 3> lower = {};
	std::array<float, 3> upper = {};
	ClusterPositionLayout positions;
	std::vector<uint8_t> position_bytes;
	PackedClusterAttributes attributes;
	PackedClusterTopology topology;
	// Slow material info is local (offset zero), rebased during page writing.
	PackedClusterMaterials materials;
	std::vector<uint32_t> material_palette;
	std::vector<uint32_t> reuse_batch_words;
};

struct GeometryPage {
	bool root = false;
	std::vector<GeometryPageCluster> clusters;
};

struct GeometryPageSizes {
	uint32_t disk_bytes = 0;
	uint32_t gpu_bytes = 0;
};

enum class PageCodecError {
	OK,
	INVALID_HEADER,
	INVALID_LAYOUT,
	INVALID_LENGTH,
	INVALID_OFFSET,
	INVALID_PADDING,
	INVALID_METADATA,
	POSITIONS,
	ATTRIBUTES,
	TOPOLOGY,
	MATERIALS,
	REUSE_BATCHES,
	CAPACITY,
	MEMORY,
};

struct PageCodecResult {
	PageCodecError error = PageCodecError::OK;
	// Local index within the page, not the global cluster identity.
	uint32_t cluster = UINT32_MAX;
};

// Structural sizing for offline page packing, without decoding the payloads.
// Counts and byte lengths are bounded; encode/decode additionally check values.
// Root pages fit 32 KiB/64 clusters, detail pages 128 KiB/256 clusters after
// transcode. Disk bytes and GPU bytes are deliberately separate quantities.
PageCodecResult measure_geometry_page(const GeometryPage &p_page, GeometryPageSizes &r_sizes);

// Explicit little-endian records, canonical four-byte-aligned payload ranges,
// zero padding, no serialized native structs. All clusters are self-contained
// in this initial disk representation; cross-cluster vertex references and
// byte-plane deltas are not implemented yet. Input is borrowed for the call.
// Disk record word 71 is the canonical GPU payload byte offset. The GPU record
// keeps word 71 zero; no physical page/pool address is stored in an asset.
// Old output survives every refusal. No file, engine object or GPU operations.
PageCodecResult encode_geometry_page(const GeometryPage &p_page, std::vector<uint8_t> &r_bytes);
PageCodecResult decode_geometry_page(const uint8_t *p_bytes, size_t p_size, GeometryPage &r_page);

// Bounded upload-layout check for publication-admitted bytes. Checks canonical
// input/output ranges and strip headers without unpacking positions, attributes
// or triangle arrays. This is not content admission or a CPU transcode path.
PageCodecResult validate_geometry_page_layout(const uint8_t *p_bytes, size_t p_size);

// Bounded CPU oracle for the future GPU installer. Converts disk strips into
// minimum-base/delta triangles and copies compressed positions/attributes.
// Material table offsets are relocated to the GPU page's word addresses.
// This does not install a page or provide a CPU runtime rendering path.
PageCodecResult transcode_geometry_page_cpu(const uint8_t *p_bytes, size_t p_size, std::vector<uint8_t> &r_gpu_bytes);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_PAGE_CODEC_H
