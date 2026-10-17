#ifndef VIRTUAL_GEOMETRY_CLUSTER_STRIPIFIER_H
#define VIRTUAL_GEOMETRY_CLUSTER_STRIPIFIER_H

#include "triangle_topology.h"

#include "core/io/virtual_geometry/topology_codec.h"

namespace VirtualGeometry {

struct StripifiedCluster {
	PackedClusterTopology topology;
	std::vector<uint32_t> indices;
	std::vector<uint32_t> source_vertices;
	std::vector<uint32_t> source_triangles;
	// Cyclic rotation of each source triangle; winding is unchanged.
	std::vector<uint8_t> corner_rotations;
};

enum class StripBuildError {
	OK,
	INVALID_INPUT,
	TOPOLOGY,
	MEMORY,
};

struct StripBuildResult {
	StripBuildError error = StripBuildError::OK;
	TopologyCodecError codec_error = TopologyCodecError::OK;
};

// Source triangles must already form contiguous material ranges. Strip starts
// preserve that range order; extensions use reverse index edges within a range.
// New/duplicated vertices satisfy the trailing 32-vertex window. Starts also
// occur at each 32-triangle block. Up to 384 temporary vertices are allowed;
// encoding preparation must split/reconstrain before publishing >256 vertices.
// This uses an independent deterministic cache/lookahead score, not UE's tuned
// weight table. Geometry, attributes and old output remain caller-owned.
StripBuildResult stripify_cluster(const TriangleMeshInput &p_mesh, StripifiedCluster &r_cluster);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_CLUSTER_STRIPIFIER_H
