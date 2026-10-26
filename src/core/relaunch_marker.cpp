// relaunch_marker.cpp - see relaunch_marker.hpp. The body is master's takeRelaunchMarker
// (gui/software_gl.cpp, 0.99.90), unchanged.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/relaunch_marker.hpp"

#if defined(_WIN32)
#include <cstdlib>
#endif

namespace cascade::core {

#if defined(_WIN32)

bool takeSoftwareGlRelaunchMarker(std::string& parent) {
    const char* v = std::getenv(kSoftwareGlRelaunchEnv);
    if (v == nullptr || *v == '\0') { return false; }
    parent.clear();
    for (const char* p = v; *p != '\0' && parent.size() < 10; ++p) {
        if (*p >= '0' && *p <= '9') { parent.push_back(*p); }
    }
    _putenv_s(kSoftwareGlRelaunchEnv, "");  // removes it from both the C runtime's copy and the process's
    return true;
}

#else

bool takeSoftwareGlRelaunchMarker(std::string&) { return false; }

#endif

}  // namespace cascade::core
