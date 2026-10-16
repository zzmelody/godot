#ifndef VIRTUAL_GEOMETRY_CLUSTER_DAG_H
#define VIRTUAL_GEOMETRY_CLUSTER_DAG_H

#include "leaf_clusters.h"
#include "mesh_simplifier.h"

#include <array>
#include <cstdint>
#include <vector>

namespace VirtualGeometry {

struct GeometryBuildInput {
	TriangleMeshInput mesh;
	AttributeLayout layout;
	std::vector<float> attributes;
};

struct DagBuildSettings {
	double max_edge_length_factor = 0;
	bool preserve_area = false;
};

struct GeometrySphere {
	std::array<double, 3> center = {};
	double radius = 0;
};

struct GeometryCluster {
	TriangleMeshInput mesh;
	std::vector<float> attributes;
	std::vector<uint32_t> external_edges;
	std::array<float, 3> lower = {};
	std::array<float, 3> upper = {};
	GeometrySphere bounds;
	GeometrySphere lod_bounds;
	double lod_error = 0;
	uint32_t level = 0;
	uint32_t group = UINT32_MAX;
	uint32_t generating_group = UINT32_MAX;
};

struct GeometryGroup {
	std::vector<uint32_t> children;
	std::vector<uint32_t> parents;
	GeometrySphere bounds;
	GeometrySphere lod_bounds;
	double min_child_error = -1;
	double max_parent_error = 0;
	uint32_t level = 0;
	uint32_t target_parents = 0;
	uint32_t attempts = 0;
	uint32_t clamped_external_edges = 0;
	bool root = false;
	SimplificationReport simplification;
};

struct GeometryDag {
	AttributeLayout layout;
	std::vector<GeometryCluster> clusters;
	std::vector<GeometryGroup> groups;
	// Offline provenance only; this is not a runtime entity/instance identity.
	std::vector<uint32_t> source_triangle_clusters;
	std::array<double, 3> lower = {};
	std::array<double, 3> upper = {};
	uint32_t leaf_count = 0;
	uint32_t root_cluster = UINT32_MAX;
	uint32_t root_group = UINT32_MAX;
};

enum class DagBuildError {
	OK,
	INVALID_INPUT,
	LEAF_BUILD,
	PARTITION,
	SIMPLIFICATION,
	BOUNDARY_CHANGE,
	FALLBACK_REMOVAL,
	RETRY_EXHAUSTED,
	NO_PROGRESS,
	LIMIT,
	MEMORY,
};

struct DagBuildResult {
	DagBuildError error = DagBuildError::OK;
	uint32_t group = UINT32_MAX;
	LeafResult leaf_result;
	PartitionResult partition_result;
	SimplificationResult simplification_result;
	SimplificationReport simplification_report;
};

// Builds the complete raw cluster replacement DAG from one high-detail mesh.
// Groups use 8/32 strict graph targets. Reduction retries start from the same
// merged input, with target cluster sizes 126,124,...,66. Every parent of one
// group shares its LOD sphere/error; error inherits max(child errors).
// Boundary movement or unaccounted fallback removal refuses the whole result.
// Input and old output survive refusal. This owns CPU build geometry only;
// encoded vertex/material limits, pages and GPU traversal are separate stages.
DagBuildResult build_cluster_dag(const GeometryBuildInput &p_input, const DagBuildSettings &p_settings, GeometryDag &r_dag);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_CLUSTER_DAG_H
