// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/version.hpp"

namespace cascade {

// The version is injected by the build (CMake passes CASCADE_VERSION_STRING,
// derived from the project() version) rather than written here, so that a
// nightly can identify itself as one.
//
// THIS MATTERS FOR BUG REPORTS, which is the whole reason for it. A nightly
// installer named 0.56.0-nightly.<date>.<sha> that contained a binary calling
// itself "0.56.0" would produce reports naming a version that does not exist
// as a release, and nobody could tell which build the reporter actually ran.
// The name of the file, the version in the About line, and the version the
// usage report and bug form carry are now the same string by construction.
//
// The fallback keeps a plain compiler invocation (an IDE indexer, a one-off
// syntax check) building without the define; it is not what a real build uses.
#ifndef CASCADE_VERSION_STRING
#define CASCADE_VERSION_STRING "0.0.0-unconfigured"
#endif

const char* versionString() { return CASCADE_VERSION_STRING; }

const char* appName() { return "FoxSDR"; }

}  // namespace cascade
