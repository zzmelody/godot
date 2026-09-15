/*<<----- VEYA_COOKER: native recipe job boundary; not a Godot ScriptLanguage or game VM. */
#pragma once

#include "core/error/error_list.h"
#include "core/variant/dictionary.h"

namespace CookerRecipe {
Dictionary capabilities();
Error run(const Dictionary &p_job, Dictionary &r_result);
}
/*>>----- VEYA_COOKER */
