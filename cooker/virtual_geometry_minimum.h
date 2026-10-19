#ifndef COOKER_VIRTUAL_GEOMETRY_MINIMUM_H
#define COOKER_VIRTUAL_GEOMETRY_MINIMUM_H

#include "scene/resources/3d/virtual_geometry_mesh.h"
#include "scene/resources/packed_scene.h"

#include <cstdint>
#include <map>
#include <vector>

namespace CookerVirtualGeometry {

// Offline only. Reads the published root pages, reconstructs their complete
// root-group coverage and returns an independent ordinary Mesh. No fine pages
// or runtime renderer are used. Slots map output surfaces to source overrides.
// Refusal leaves both prior outputs unchanged.
Error minimum_root_mesh(const Ref<VirtualGeometryMesh> &p_source, Ref<ArrayMesh> &r_mesh, std::vector<uint32_t> &r_slots);

// Explicit offline far representation for one static asset. Combines the
// complete root cut of each source instance in asset coordinates. Refuses
// harvest-field metadata and incompatible instance-color/custom-data semantics.
// Callers build a new virtual hierarchy; fine source assets remain independent.
Error minimum_asset_roots(const Ref<PackedScene> &p_source, Ref<PackedScene> &r_scene);

struct BakeMesh {
	Ref<ArrayMesh> mesh;
	std::vector<uint32_t> source_slots;
};

// One explicit offline bake owns this cache. Repeated source instances reuse
// one decoded root. It is never installed as a live rendering fallback.
class BakeMeshCache {
	std::map<ObjectID, BakeMesh> meshes;
	uint64_t vertices = 0;

public:
	Error get(const Ref<VirtualGeometryMesh> &p_source, BakeMesh &r_mesh);
};

} // namespace CookerVirtualGeometry

#endif // COOKER_VIRTUAL_GEOMETRY_MINIMUM_H
