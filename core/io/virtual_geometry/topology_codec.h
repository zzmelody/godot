#ifndef VIRTUAL_GEOMETRY_TOPOLOGY_CODEC_H
#define VIRTUAL_GEOMETRY_TOPOLOGY_CODEC_H

#include "cluster_codec.h"

namespace VirtualGeometry {

struct ClusterStripLayout {
	// Four blocks of 32 triangles: Start, Left/count-high, Ref/count-low.
	std::array<std::array<uint32_t, 3>, 4> masks = {};
	// Three cumulative ten-bit counts before blocks 1, 2 and 3.
	uint32_t new_vertices_before = 0;
	uint32_t references_before = 0;
	uint32_t triangles = 0;
	uint32_t vertices = 0;
};

struct PackedClusterTopology {
	ClusterStripLayout layout;
	// Five-bit backward references, low bits first; zero-padded to four bytes.
	std::vector<uint8_t> references;
};

enum class TopologyCodecError {
	OK,
	INVALID_LAYOUT,
	INVALID_MASKS,
	INVALID_PREFIX,
	INVALID_LENGTH,
	INVALID_PADDING,
	INVALID_REFERENCE,
	INVALID_TRIANGLE,
	TAIL_WINDOW,
	INVALID_BATCHES,
	MEMORY,
};

// A bounded CPU reference for the generalized Start/Left/Right strip format.
// Temporary offline strips may have <=384 vertices; published clusters must
// separately satisfy the 256-vertex position/layout contract. Every 32-triangle
// block begins with Start. Material boundaries also start a new strip in Cooker.
// Full decode validates every reference; random access validates metadata and
// the requested triangle. Neither over-fetches or reads negative byte offsets.
// Both preserve old output on refusal. No objects, file access or GPU work.
TopologyCodecError unpack_cluster_topology(const ClusterStripLayout &p_layout, const uint8_t *p_bytes,
		size_t p_size, std::vector<uint32_t> &r_indices);
TopologyCodecError unpack_cluster_triangle(const ClusterStripLayout &p_layout, const uint8_t *p_bytes,
		size_t p_size, uint32_t p_triangle, std::array<uint32_t, 3> &r_indices);

// Per-material batches contain <=32 triangles. Cooker also checks <=32 unique
// constrained vertex IDs. Counts use four bits, lengths-minus-one use five;
// at least three count nibbles are reserved, matching the material fast path.
TopologyCodecError pack_vertex_reuse_batches(const std::vector<ClusterMaterialRange> &p_ranges,
		const std::vector<std::vector<uint8_t>> &p_batches, std::vector<uint32_t> &r_words);
TopologyCodecError unpack_vertex_reuse_batches(const std::vector<ClusterMaterialRange> &p_ranges,
		const std::vector<uint32_t> &p_words, std::vector<std::vector<uint8_t>> &r_batches);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_TOPOLOGY_CODEC_H
