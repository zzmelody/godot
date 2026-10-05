/*<<----- VEYA_COOKER: one writer per generated output across Cooker processes. */
#pragma once
#include "core/string/ustring.h"

namespace CookerOutputLock {

// Process-wide named lock keyed by the output's absolute path. Two Cookers
// that would rebuild the same output serialize; the second one then sees a
// fresh receipt and reuses the result. Released on destruction or process exit.
class Guard {
public:
	explicit Guard(const String &p_output);
	~Guard();
	Guard(const Guard &) = delete;
	Guard &operator=(const Guard &) = delete;
	bool acquired() const { return handle != nullptr; }

private:
	void *handle = nullptr;
};

} // namespace CookerOutputLock
/*>>----- VEYA_COOKER */
