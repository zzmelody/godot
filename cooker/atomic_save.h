/*<<----- VEYA_COOKER: replace stale generated outputs in place; readers never see a missing file. */
#pragma once
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#ifdef WINDOWS_ENABLED
#include <windows.h>
#endif

namespace CookerAtomicSave {

// Outputs whose verified cache receipt is stale in this process. The old bytes
// stay published until the new result replaces them, so another Cooker that
// depends on the file keeps reading a complete (older) version instead of
// failing on a missing asset. Generated outputs are otherwise immutable.
inline HashSet<String> replaceable;

// True when `p_path` exists and this process has not been allowed to replace it.
inline bool occupied(const String &p_path) {
	return FileAccess::exists(p_path) && !replaceable.has(p_path);
}

// Same-directory staging name that keeps the extension, so ResourceSaver
// selects the right format and relative external paths stay valid. The name
// is stable for one output: built-in subresource IDs are seeded from the save
// path, and per-output locking keeps one writer per staging file.
inline String staging_path(const String &p_path) {
	return p_path.get_basename() + ".partial." + p_path.get_extension();
}

inline bool is_staging(const String &p_name) {
	return p_name.contains(".partial.") || p_name.ends_with(".partial") || p_name.contains(".recipe-tmp.");
}

// Publish in one filesystem operation. DirAccessWindows::rename removes the
// old destination first, so use replace-existing directly on Windows instead.
// A reader without delete sharing can block the move; retry with the old file
// still published. POSIX rename already replaces the destination atomically.
inline Error publish(const String &p_staging, const String &p_path) {
#ifdef WINDOWS_ENABLED
	const auto windows_path = [](const String &p_value) {
		const String path = ProjectSettings::get_singleton()->globalize_path(p_value).replace("/", "\\");
		if (path.begins_with("\\\\?\\")) return path;
		if (path.begins_with("\\\\")) return String("\\\\?\\UNC\\") + path.substr(2);
		return String("\\\\?\\") + path;
	};
	const Char16String staging = windows_path(p_staging).utf16();
	const Char16String destination = windows_path(p_path).utf16();
#endif
	Error error = FAILED;
	for (int attempt = 0; attempt < 80; ++attempt) {
#ifdef WINDOWS_ENABLED
		error = MoveFileExW(reinterpret_cast<LPCWSTR>(staging.get_data()), reinterpret_cast<LPCWSTR>(destination.get_data()),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? OK : FAILED;
#else
		error = DirAccess::rename_absolute(p_staging, p_path);
#endif
		if (error == OK) return OK;
		OS::get_singleton()->delay_usec(250000);
	}
	if (FileAccess::exists(p_staging)) DirAccess::remove_absolute(p_staging);
	ERR_FAIL_V_MSG(error, "Cannot publish generated output: " + p_path);
}

inline void discard_stale(const String &p_staging) {
	// Only an interrupted earlier run leaves this file; the per-output lock
	// guarantees no live writer owns it now.
	if (FileAccess::exists(p_staging)) DirAccess::remove_absolute(p_staging);
}

inline Error save_resource(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	const String staging = staging_path(p_path);
	discard_stale(staging);
	Error error = DirAccess::make_dir_recursive_absolute(p_path.get_base_dir());
	ERR_FAIL_COND_V(error != OK, error);
	error = ResourceSaver::save(p_resource, staging, p_flags);
	if (error != OK) {
		discard_stale(staging);
		return error;
	}
	return publish(staging, p_path);
}

inline Error save_text(const String &p_text, const String &p_path) {
	const String staging = p_path + ".partial";
	discard_stale(staging);
	Error error = DirAccess::make_dir_recursive_absolute(p_path.get_base_dir());
	ERR_FAIL_COND_V(error != OK, error);
	Ref<FileAccess> file = FileAccess::open(staging, FileAccess::WRITE, &error);
	ERR_FAIL_COND_V(file.is_null(), error == OK ? ERR_FILE_CANT_WRITE : error);
	file->store_string(p_text);
	file.unref(); // Close before the Windows rename.
	return publish(staging, p_path);
}

} // namespace CookerAtomicSave
/*>>----- VEYA_COOKER */
