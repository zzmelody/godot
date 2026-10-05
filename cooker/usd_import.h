// Static USD foliage transport. Geometry and budgets are declared by Luau jobs.
#pragma once
#include "core/variant/dictionary.h"
#include "core/templates/hash_set.h"
namespace CookerUsd {
Error dependencies(const String &source, HashSet<String> &files);
Error import_scene(const Dictionary &job, Dictionary &result);
}
