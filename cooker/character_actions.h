/*<<----- VEYA_COOKER: character assembly reuses the existing retargeter's cooked resources. */
#pragma once
#include "core/error/error_list.h"
#include "core/variant/dictionary.h"

namespace CookerCharacter {
// Expand a Luau source substitution from the immutable compiled catalog before
// cache hashing, so every replacement resource is an explicit dependency.
Error prepare_compose_job(Dictionary &r_job);
Error compose_animation(const Dictionary &p_job, Dictionary &r_result);
Error assemble(const Dictionary &p_job, Dictionary &r_result);
Error validate(const Dictionary &p_job, Dictionary &r_result);
}
/*>>----- VEYA_COOKER */
