#ifndef COOKER_VIRTUAL_GEOMETRY_MESH_H
#define COOKER_VIRTUAL_GEOMETRY_MESH_H

#include "virtual_geometry/cluster_encoding.h"

#include "core/variant/dictionary.h"
#include "scene/resources/3d/virtual_geometry_mesh.h"

#include <vector>

namespace CookerVirtualGeometry {

struct Settings {
	VirtualGeometry::DagBuildSettings dag;
	VirtualGeometry::ClusterEncodingSettings encoding;
	bool interpolate_uvs = true;
	uint32_t max_source_vertices = 500000;
	uint64_t max_output_bytes = 64 * 1024 * 1024;
};

struct BuildOutput {
	Ref<VirtualGeometryMesh> mesh;
	std::vector<uint8_t> container;
	Dictionary report;
	uint32_t source_vertices = 0;
};

// Cooker-only, borrowed immutable Mesh input. Canonicalizes absent attributes,
// then runs the real DAG/encoding/page/hierarchy/model pipeline. No files or
// GPU work. Refusal preserves the old output and describes the failing stage.
Error build(const Ref<Mesh> &p_source, const Settings &p_settings, BuildOutput &r_output, String &r_message);

// Stage the immutable, content-addressed container after root serialization.
// The root rename remains the final publication point. Existing matching
// containers are shared; a published container is never deleted on root failure
// because another output can already reference it. No manifest is published.
Error publish_container(const BuildOutput &p_output, const String &p_resource_path,
		uint64_t p_serialized_resource_bytes, uint64_t p_max_output_bytes, Array &r_files);

} // namespace CookerVirtualGeometry

#endif // COOKER_VIRTUAL_GEOMETRY_MESH_H
