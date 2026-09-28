/*<<----- VEYA_COOKER: character assembly reuses the existing retargeter's cooked resources. */
#pragma once
#include "core/error/error_list.h"
#include "core/variant/dictionary.h"

namespace CookerCharacter {
Error compose_animation(const Dictionary &p_job, Dictionary &r_result);
Error assemble(const Dictionary &p_job, Dictionary &r_result);
Error validate(const Dictionary &p_job, Dictionary &r_result);
}
/*>>----- VEYA_COOKER */
