#ifndef VIRTUAL_GEOMETRY_CLUSTER_ENCODING_H
#define VIRTUAL_GEOMETRY_CLUSTER_ENCODING_H

#include "cluster_dag.h"

#include "core/io/virtual_geometry/cluster_codec.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace VirtualGeometry {

struct ClusterEncodingSettings {
	// Automatic density heuristic in model meters. One grid serves all levels.
	int32_t position_precision = std::numeric_limits<int32_t>::min();
};

struct EncodableCluster {
	GeometryCluster geometry;
	uint32_t source_cluster = UINT32_MAX;
	// Offline maps into the raw source cluster, before material sorting/splits.
	std::vector<uint32_t> source_triangles;
	std::vector<uint32_t> source_vertices;
	std::vector<uint32_t> material_palette;
	std::vector<ClusterMaterialRange> material_ranges;
	PackedClusterMaterials materials;
	QuantizedClusterPositions positions;
	std::vector<uint8_t> position_bytes;
};

struct EncodableGeometry {
	AttributeLayout layout;
	std::vector<EncodableCluster> clusters;
	// Cluster references expand to every split part. Group identities are stable.
	std::vector<GeometryGroup> groups;
	std::vector<PartitionRange> source_cluster_parts;
	std::vector<uint32_t> root_clusters;
	std::array<double, 3> lower = {};
	std::array<double, 3> upper = {};
	double max_position_error = 0;
	int32_t position_precision = 0;
	uint32_t leaf_count = 0;
	uint32_t root_group = UINT32_MAX;
};

enum class ClusterEncodingError {
	OK,
	INVALID_DAG,
	INVALID_SETTINGS,
	LIMIT,
	MATERIALS,
	POSITIONS,
	QUANTIZATION_COLLAPSE,
	MEMORY,
};

struct ClusterEncodingResult {
	ClusterEncodingError error = ClusterEncodingError::OK;
	uint32_t cluster = UINT32_MAX;
	MaterialCodecError material_error = MaterialCodecError::OK;
	PositionCodecError position_error = PositionCodecError::OK;
};

// Material sorting/splits preserve every source triangle, attribute and boundary.
// Every output cluster fits 128 triangles, 256 vertices and 64 palette materials.
// Splits expand both sides of replacement groups and the necessary root set.
// Quantization uses a common origin-centered power-of-two grid across all parts.
// A separate measured encoding-error bound inflates spheres; simplifier error
// stays unchanged. Input and complete old output survive refusal. This is CPU
// encoding preparation, not a page/resource format or the final strip codec.
ClusterEncodingResult prepare_cluster_encoding(const GeometryDag &p_dag, const ClusterEncodingSettings &p_settings,
		EncodableGeometry &r_geometry);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_CLUSTER_ENCODING_H
