/*<<----- VEYA_COOKER: one writer per generated output across Cooker processes. */
#include "output_lock.h"
#include "core/config/project_settings.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#endif

namespace CookerOutputLock {

Guard::Guard(const String &p_output) {
#ifdef WINDOWS_ENABLED
	// Mutex names cannot contain backslashes and are case-sensitive; hash the
	// normalized absolute path so every checkout path maps to one stable name.
	const String absolute = ProjectSettings::get_singleton()->globalize_path(p_output).replace("\\", "/").to_lower();
	const String name = "Global\\Veya.Cook." + absolute.sha256_text();
	HANDLE mutex = CreateMutexW(nullptr, FALSE, (LPCWSTR)name.utf16().get_data());
	if (mutex == nullptr) {
		// A session without Global namespace rights falls back to Local.
		const String local = "Local\\Veya.Cook." + absolute.sha256_text();
		mutex = CreateMutexW(nullptr, FALSE, (LPCWSTR)local.utf16().get_data());
	}
	if (mutex == nullptr) return;
	const DWORD wait = WaitForSingleObject(mutex, INFINITE);
	// An abandoned mutex means the previous owner crashed; ownership transfers.
	if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
		CloseHandle(mutex);
		return;
	}
	handle = mutex;
#else
	(void)p_output;
#endif
}

Guard::~Guard() {
#ifdef WINDOWS_ENABLED
	if (handle) {
		ReleaseMutex((HANDLE)handle);
		CloseHandle((HANDLE)handle);
	}
#endif
}

} // namespace CookerOutputLock
/*>>----- VEYA_COOKER */
