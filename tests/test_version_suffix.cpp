// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// Pins the version-suffix composition core/version.cpp's versionString()
// uses: CASCADE_VERSION_SUFFIX (set by android/app/build.gradle's
// externalNativeBuild arguments to "-android.<buildNumber>" so an Android
// alpha report is always distinguishable from a desktop release of the same
// version) is appended to the base CASCADE_VERSION_STRING, and an EMPTY
// suffix — the desktop release's default — must leave the base untouched.
//
// This tests the pure appendVersionSuffix() helper directly rather than
// versionString() itself: the suffix is a compile definition baked in at
// CMake configure time, so a single test binary cannot be configured twice
// in one run to exercise both the empty and non-empty cases against the
// real compiled-in value. The helper is exactly the logic versionString()
// runs, so this pins the same behavior.

#include "core/version.hpp"

#include <cstdio>
#include <string>

int main() {
    // Empty suffix: the base string is returned unchanged.
    {
        const std::string result = cascade::appendVersionSuffix("0.98.0", "");
        if (result != "0.98.0") {
            std::fprintf(stderr,
                         "empty suffix must leave the base unchanged: got \"%s\"\n",
                         result.c_str());
            return 1;
        }
    }

    // Null suffix (the macro fallback when CASCADE_VERSION_SUFFIX is not
    // defined at all) behaves the same as an empty one.
    {
        const std::string result = cascade::appendVersionSuffix("0.98.0", nullptr);
        if (result != "0.98.0") {
            std::fprintf(stderr,
                         "null suffix must leave the base unchanged: got \"%s\"\n",
                         result.c_str());
            return 1;
        }
    }

    // A real suffix is appended verbatim - this is the Android alpha shape.
    {
        const std::string result =
            cascade::appendVersionSuffix("0.98.0", "-android.1");
        if (result != "0.98.0-android.1") {
            std::fprintf(stderr,
                         "suffix must be appended: got \"%s\"\n",
                         result.c_str());
            return 1;
        }
    }

    std::printf("test_version_suffix: OK\n");
    return 0;
}
