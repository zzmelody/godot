/*<<----- VEYA_COOKER: bounded Luau pipeline descriptions for native Cooker jobs. */
#pragma once

#include "core/error/error_list.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

namespace CookerPipeline {
Dictionary capabilities();
Error load(const String &p_source, Array &r_steps, Dictionary &r_result);
}
/*>>----- VEYA_COOKER */
