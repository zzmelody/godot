#ifndef VIRTUAL_GEOMETRY_METADATA_BOUNDS_H
#define VIRTUAL_GEOMETRY_METADATA_BOUNDS_H

#include "cluster_dag.h"

namespace VirtualGeometry {

// Preserve a directed bound when converting finite metadata to float32.
bool round_metadata_error(double p_value, bool p_upper, float &r_value);
bool round_metadata_sphere(const GeometrySphere &p_sphere, std::array<float, 4> &r_sphere);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_METADATA_BOUNDS_H
