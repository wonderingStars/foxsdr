// SPDX-License-Identifier: MIT
#pragma once

namespace cascade {

// Bump in version.cpp on every behavioral change.
const char* versionString();

// Product display name ("cascade" stays the internal/binary name — '+' is
// hostile to filesystems and build targets).
const char* appName();

}  // namespace cascade
