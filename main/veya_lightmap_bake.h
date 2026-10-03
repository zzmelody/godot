#pragma once

// Private cook-time entry; no ClassDB API, script, editor UI or gameplay host.
// Returns false when no bake was requested. r_status follows Main::start().
namespace VeyaLightmapBake {
bool start(int &r_status);
}
