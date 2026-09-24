/*<<----- VEYA_COOKER: native, renderer-free humanoid animation retarget jobs. */
#pragma once

#include "core/variant/dictionary.h"
#include "core/error/error_list.h"

namespace CookerRetarget {
Error make_bone_map(const Dictionary &p_job, Dictionary &r_result);
Error retarget_animations(const Dictionary &p_job, Dictionary &r_result);
Error retarget_model(const Dictionary &p_job, Dictionary &r_result);
Error make_animation_preview(const Dictionary &p_job, Dictionary &r_result);
}
/*>>----- VEYA_COOKER */
