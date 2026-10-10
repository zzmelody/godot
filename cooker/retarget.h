/*<<----- VEYA_COOKER: native, renderer-free humanoid animation retarget jobs.
 */
#pragma once

#include "core/error/error_list.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "scene/resources/animation.h"

namespace CookerRetarget {
// Cook-time conversion only. Translation is limited to Root/Hips; all other
// joints keep the target's rest lengths. An empty mask selects body/fingers.
Error RetargetAnimation(const String& reference_model,
                        const String& target_model,
                        const Ref<Animation>& animation, const Array& bones);
Error make_bone_map(const Dictionary& p_job, Dictionary& r_result);
Error retarget_animations(const Dictionary& p_job, Dictionary& r_result);
Error retarget_model(const Dictionary& p_job, Dictionary& r_result);
Error make_animation_preview(const Dictionary& p_job, Dictionary& r_result);
}  // namespace CookerRetarget
/*>>----- VEYA_COOKER */
