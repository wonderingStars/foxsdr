// SPDX-License-Identifier: MIT
//
// Thin entry point: parse the command line, hand off to the GUI shell.
// `--frames N` renders exactly N frames then exits 0 — the bounded-run
// contract the app_smoke ctest entry relies on. No flag: run until the
// window is closed.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gui/app_window.hpp"

int main(int argc, char** argv) {
    int frames = -1;  // negative: run until the window is closed
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --frames requires an integer argument\n");
                return 1;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[i + 1], &end, 10);
            // Reject trailing junk and negatives; cap so the int cast below
            // cannot overflow on LP64-style long values.
            if (end == argv[i + 1] || *end != '\0' || value < 0 || value > 1000000L) {
                std::fprintf(stderr, "cascade: invalid --frames value '%s'\n", argv[i + 1]);
                return 1;
            }
            frames = static_cast<int>(value);
            ++i;  // consumed the value argument
        } else {
            std::fprintf(stderr, "cascade: unknown argument '%s' (usage: cascade [--frames N])\n",
                         argv[i]);
            return 1;
        }
    }

    cascade::gui::AppWindow app;
    return app.run(frames);
}
