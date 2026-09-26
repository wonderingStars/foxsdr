// engine_link_check.cpp - the executable that proves the ENGINE stands on its
// own: cascade_engine linked WHOLE ARCHIVE, with nothing of the window (no
// cascade_gui, no imgui, no GLFW, no OpenGL). See the "line between them"
// block in the top-level CMakeLists.txt.
//
// The proof is the link itself: every engine object is pulled in, so an
// engine file that calls into src/gui - even one nothing else reaches - leaves
// an unresolved symbol and fails the build. Running it (ctest
// engine_link_check) only confirms the binary that link produced starts.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>

#include "core/version.hpp"

int main() {
    std::printf("cascade_engine links on its own: %s %s (%s)\n", cascade::appName(),
                cascade::versionString(), cascade::gitCommit());
    return 0;
}
