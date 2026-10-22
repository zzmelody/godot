#pragma once

#include "core/error/error_list.h"
#include "core/variant/dictionary.h"

namespace CookerMotionMatching {
Error Build(const Dictionary& job, Dictionary& result);
}
