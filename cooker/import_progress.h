/*<<----- VEYA_COOKER: noninteractive importer progress; no dialogs, viewport or event loop. */
#pragma once

#include "core/string/print_string.h"

class CookerImportProgress {
public:
	CookerImportProgress(const String &p_task, const String &p_label, int p_steps) {
		print_verbose(p_task + ": " + p_label);
	}
	bool step(const String &p_label, int p_step = -1, bool p_force_refresh = true) {
		print_verbose(p_label);
		return false;
	}
};

// Retain the upstream importer's local progress spelling without linking EditorProgress.
using EditorProgress = CookerImportProgress;
/*>>----- VEYA_COOKER */
