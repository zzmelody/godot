#pragma once

// Private cook-time entry; no ClassDB API, script, editor UI or gameplay host.
// Returns false when no bake was requested. r_status follows Main::start().
class MainLoop;
namespace VeyaLightmapBake {
bool start(MainLoop *&r_loop, int &r_status);
}
