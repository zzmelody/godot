/*<<----- VEYA_COOKER: character assembly reuses the existing retargeter's cooked resources. */
#pragma once
#include "core/error/error_list.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class Node;

namespace CookerCharacter {
// Cook-time only: preserve skin bind space while pruning source helper bones
// and adding the declared runtime bones. No runtime or editor script is used.
Error PrepareRig(Node *root, const Array &bones, Dictionary &report);
// Convert immutable topology and sparse facial targets into a head-only mesh.
Error ComposeFace(const Dictionary &job, Dictionary &result);
// Expand a Luau source substitution from the immutable compiled catalog before
// cache hashing, so every replacement resource is an explicit dependency.
Error prepare_compose_job(Dictionary &r_job);
Error compose_animation(const Dictionary &p_job, Dictionary &r_result);
Error assemble(const Dictionary &p_job, Dictionary &r_result);
Error validate(const Dictionary &p_job, Dictionary &r_result);
}
/*>>----- VEYA_COOKER */
