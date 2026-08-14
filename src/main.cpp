// SPDX-License-Identifier: MIT
//
// Pre-GUI stub. The gui-shell agent replaces this with the GLFW/ImGui shell.
// Contract already honored: `--frames N` is accepted and the process exits 0,
// which is what the app_smoke ctest entry relies on.
#include <cstdio>

#include "core/version.hpp"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::printf("cascade %s\n", cascade::versionString());
    return 0;
}
