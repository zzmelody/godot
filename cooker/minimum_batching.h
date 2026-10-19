#pragma once

#include "core/error/error_list.h"

class Node3D;

namespace CookerMinimum {

// Mutates only Builder's detached, flattened minimum tree. Published inputs
// remain borrowed and unchanged. A failure leaves the candidate unpublished.
Error CoalesceMinimumBatches(Node3D& root);

}  // namespace CookerMinimum
