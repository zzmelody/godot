#ifndef VIRTUAL_GEOMETRY_MESH_SIMPLIFIER_H
#define VIRTUAL_GEOMETRY_MESH_SIMPLIFIER_H

#include "attribute_policy.h"
#include "attribute_quadric.h"

#include <cstdint>
#include <vector>

namespace VirtualGeometry {

struct SimplificationInput {
	TriangleMeshInput mesh;
	AttributeLayout layout;
	std::vector<float> attributes;
	// Empty means no external locks. Otherwise, one neighbor count per corner.
	std::vector<uint32_t> external_edges;
	TriangleValidation validation = TriangleValidation::SOURCE;
};

struct SimplificationSettings {
	uint32_t target_triangles = 1;
	uint32_t minimum_triangles = 1;
	double max_edge_length_factor = 0;
	bool preserve_area = false;
};

struct SimplificationReport {
	double lod_error = 0;
	uint32_t collapsed_pairs = 0;
	uint32_t fallback_removed_triangles = 0;
	uint32_t locked_position_moves = 0;
	uint64_t evaluated_pairs = 0;
	uint64_t recomputed_triangles = 0;
	bool reached_target = false;
	bool area_dilation_applied = false;
};

struct SimplifiedMesh {
	TriangleMeshInput mesh;
	std::vector<float> attributes;
	std::vector<uint32_t> external_edges;
	SimplificationReport report;
};

enum class SimplificationError {
	OK,
	INVALID_SETTINGS,
	INVALID_EXTERNAL_EDGES,
	ATTRIBUTE_POLICY,
	NUMERIC,
	EMPTY,
	MINIMUM_VIOLATION,
	MEMORY,
};

struct SimplificationResult {
	SimplificationError error = SimplificationError::OK;
	AttributePolicyError policy_error = AttributePolicyError::OK;
	QuadricError quadric_error = QuadricError::OK;
};

// Offline, value-only. Owns all work storage for this call; no references escape.
// Uses UE-style wedge quadrics, candidate penalties and local indexed-heap updates.
// As in the reference, two locked endpoints get a penalty rather than a hard ban;
// movement and fallback face removal are reported for the DAG caller to assess.
// lod_error is the reference heuristic, not a strict surface-distance bound.
// Failure preserves input and the complete previous output.
SimplificationResult simplify_mesh(const SimplificationInput &p_input, const SimplificationSettings &p_settings, SimplifiedMesh &r_output);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_MESH_SIMPLIFIER_H
