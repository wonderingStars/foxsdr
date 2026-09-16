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

// CASCADE_VERSION_SUFFIX is appended to CASCADE_VERSION_STRING - empty for a
// normal desktop build, "-android.<buildNumber>" for the Android alpha (set
// by android/app/build.gradle's externalNativeBuild arguments, via the
// CASCADE_VERSION_SUFFIX cache variable in the root CMakeLists.txt), so a
// sideloaded alpha's crash and usage reports are never mistaken for a
// desktop release of the same version. The site's version comparator treats
// "-<anything>" as a pre-release, which is what an alpha needs.
#ifndef CASCADE_VERSION_SUFFIX
#define CASCADE_VERSION_SUFFIX ""
#endif

std::string appendVersionSuffix(std::string base, const char* suffix) {
    if (suffix != nullptr && suffix[0] != '\0') {
        base += suffix;
    }
    return base;
}

const char* versionString() {
    static const std::string s =
        appendVersionSuffix(CASCADE_VERSION_STRING, CASCADE_VERSION_SUFFIX);
    return s.c_str();
}

// The commit - a sharper version of the same reason. A version identifies a
// RELEASE; a commit identifies a BUILD. An engineer handed a crash report has
// to check out the exact tree the offsets in it were produced from, and
// "0.61.0" is the release, the nightly heading towards it, and every rebuild
// in between.
//
// FROM A HEADER REGENERATED ON EVERY BUILD, not from a -D fixed at configure
// time. That is not a style preference: CMake re-configures when CMakeLists.txt
// changes, not when HEAD moves, so a configure-time SHA is the PREVIOUS commit
// for every build after the next one - and a silently wrong commit is worse
// than "unknown", because it sends the reader to a tree that exists and is not
// the one the offsets came from. See cmake/git-commit.cmake, and
// tests/test_diagnostics.cpp, which holds this to what git says HEAD is now.
//
// The __has_include fallback keeps a plain compiler invocation (an IDE indexer,
// a one-off syntax check) building without the generated header; it is not what
// a real build uses.
#if defined(__has_include)
#if __has_include("cascade_git_commit.h")
#include "cascade_git_commit.h"
#endif
#endif

#ifndef CASCADE_GIT_COMMIT
#define CASCADE_GIT_COMMIT "unknown"
#endif

const char* gitCommit() { return CASCADE_GIT_COMMIT; }

const char* appName() { return "FoxSDR"; }

}  // namespace cascade
