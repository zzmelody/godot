/*<<----- VEYA_COOKER: one writer per generated output across Cooker processes. */
#include "output_lock.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
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
	const String directory = OS::get_singleton()->get_cache_path().path_join("veya-cooker-output-locks");
	if (DirAccess::make_dir_recursive_absolute(directory) != OK) {
		return;
	}
	const String absolute = ProjectSettings::get_singleton()->globalize_path(p_output).simplify_path();
	const CharString path = directory.path_join(absolute.sha256_text() + ".lock").utf8();
	const int file = open(path.get_data(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (file < 0) {
		return;
	}
	int result = 0;
	do {
		result = flock(file, LOCK_EX);
	} while (result != 0 && errno == EINTR);
	if (result != 0) {
		close(file);
		return;
	}
	descriptor = file;
#endif
}

Guard::~Guard() {
#ifdef WINDOWS_ENABLED
	if (handle) {
		ReleaseMutex((HANDLE)handle);
		CloseHandle((HANDLE)handle);
	}
#else
	if (descriptor >= 0) {
		flock(descriptor, LOCK_UN);
		close(descriptor);
	}
#endif
}

} // namespace CookerOutputLock
/*>>----- VEYA_COOKER */
