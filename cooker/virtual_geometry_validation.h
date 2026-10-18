#ifndef COOKER_VIRTUAL_GEOMETRY_VALIDATION_H
#define COOKER_VIRTUAL_GEOMETRY_VALIDATION_H

#include "scene/resources/3d/virtual_geometry_mesh.h"

namespace CookerVirtualGeometry {

// Publication-only validation. Reads one page block at a time, checks the
// content-addressed container and the root/page contract, and runs the CPU
// transcode oracle. Ordinary resource loading never calls this function.
// Main thread only. A process-owned bounded digest cache reuses the full check
// for identical actual root/container bytes; it never skips hashing those bytes.
Error validate_container(const Ref<VirtualGeometryMesh> &p_mesh, const String &p_resource_path);

// Conservative CPU metadata reservation, excluding materials and GPU pages.
uint64_t metadata_bytes(const Ref<VirtualGeometryMesh> &p_mesh);

} // namespace CookerVirtualGeometry

#endif // COOKER_VIRTUAL_GEOMETRY_VALIDATION_H
