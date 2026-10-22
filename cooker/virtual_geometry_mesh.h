#ifndef COOKER_VIRTUAL_GEOMETRY_MESH_H
#define COOKER_VIRTUAL_GEOMETRY_MESH_H

#include "virtual_geometry/cluster_encoding.h"

#include "core/variant/dictionary.h"
#include "scene/resources/3d/virtual_geometry_mesh.h"

#include <vector>
#include <map>

namespace CookerVirtualGeometry {

struct Settings {
	VirtualGeometry::DagBuildSettings dag;
	VirtualGeometry::ClusterEncodingSettings encoding;
	bool interpolate_uvs = true;
	uint32_t max_source_vertices = UINT32_MAX;
	uint64_t max_output_bytes = UINT64_MAX;
};

// Shared import policy for USD and the ordinary scene importers. An absent
// explicit request retains the automatic dense-static conversion policy.
Error parse_import_settings(const Dictionary &p_options, bool p_runtime_scene,
		Settings &r_settings, bool &r_explicit_geometry);

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

// Publish sidecars for meshes directly owned by a Mesh/MultiMesh/PackedScene.
// Existing input roots copy their admitted immutable container to the new
// owner directory. No source resource path or acquired file is changed.
Error publish_resource_containers(const Ref<Resource> &p_resource, const std::map<ObjectID, BuildOutput> &p_builds,
		const String &p_resource_path, uint64_t p_max_output_bytes, Array &r_files);

// Default PackedScene import policy. Dense static opaque/masked meshes use the same
// builder as the explicit recipe. Unsupported/deforming/skinned inputs retain
// their ordinary representation and receive an explicit policy report. Explicit
// recipe settings convert all static triangle meshes and preserve shared
// MultiMesh placements; invalid geometry refuses the complete output.
Error convert_imported_scene(const Ref<Resource> &p_source, Ref<Resource> &r_resource,
		std::map<ObjectID, BuildOutput> &r_builds, Dictionary &r_report, String &r_message,
		const Settings *p_explicit_settings = nullptr);

} // namespace CookerVirtualGeometry

#endif // COOKER_VIRTUAL_GEOMETRY_MESH_H
