// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace cascade {

// Bump in version.cpp on every behavioral change.
const char* versionString();

// Appends a build suffix (e.g. "-android.1") to a base version string. An
// empty (or null) suffix leaves the base unchanged. This is the pure logic
// versionString() uses to compose CASCADE_VERSION_STRING with
// CASCADE_VERSION_SUFFIX at static-init time; it is exposed separately so it
// can be unit-tested without reconfiguring CMake twice in one test run — see
// tests/test_version_suffix.cpp.
std::string appendVersionSuffix(std::string base, const char* suffix);

// The short git SHA this binary was built from, or "unknown" outside a
// checkout. A version names a release; only this names a build, which is what
// a crash report needs to be reproducible.
const char* gitCommit();

// Product display name ("cascade" stays the internal/binary name — '+' is
// hostile to filesystems and build targets).
const char* appName();

}  // namespace cascade
