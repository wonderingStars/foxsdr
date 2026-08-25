// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade {

// Bump in version.cpp on every behavioral change.
const char* versionString();

// The short git SHA this binary was built from, or "unknown" outside a
// checkout. A version names a release; only this names a build, which is what
// a crash report needs to be reproducible.
const char* gitCommit();

// Product display name ("cascade" stays the internal/binary name — '+' is
// hostile to filesystems and build targets).
const char* appName();

}  // namespace cascade
