// sys_portable.hpp - portable mode: settings, data and logs beside the program, so FoxSDR runs from a USB stick and leaves nothing on the PC.
//
// THE RULE. A marker file named `foxsdr.portable` sitting in the same folder as the executable switches portable mode on. Its content is empty (the data folder is then
// `FoxSDR-data`) or one line naming the data folder (a plain name, never a path). The resolver is a FUNCTION: it takes the executable's folder and returns the three roots
//   settingsRoot  = <exeDir>/<name>/settings   (what %APPDATA% would hold: <settingsRoot>/foxsdr/config.json, bookmarks.json, ...)
//   dataRoot      = <exeDir>/<name>/data       (what %LOCALAPPDATA% would hold: plugins, caches, reports)
//   logRoot       = <exeDir>/<name>/logs       (diagnostics; under dataRoot's own `foxsdr` folder the program writes its log today, so this root is informational for the
//                                               wiring: pointing LOCALAPPDATA at dataRoot already moves the log, logRoot names where it ends up)
// and portableEnvOverrides() turns that into the two environment variables the existing code already reads (APPDATA, LOCALAPPDATA), which is the whole wiring: two
// _putenv_s calls at the very top of main(), before anything resolves a path (hand-off in the record; config.cpp, diag_log.cpp and plugin_host.cpp need no edit).
//
// NEVER WRITES OUTSIDE. resolvePortable() touches nothing (it reads the marker and checks the folder name). ensurePortableRoots() creates the three folders and ONLY them, after
// proving that each resolves (symbolic links and junctions followed) to a place inside the executable's folder. A marker whose name is not a plain folder name (separators, "..",
// drive letters, reserved device names, trailing dots or spaces, over 64 characters) makes portable mode INVALID with a reason, and the caller must not silently fall back
// to %APPDATA%: a stick that was meant to be portable must say so.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_PORTABLE_HPP
#define CASCADE_CORE_SYS_PORTABLE_HPP

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace cascade::core::sys {

inline constexpr const char* kPortableMarkerName = "foxsdr.portable";
inline constexpr const char* kPortableDefaultFolder = "FoxSDR-data";

struct PortablePaths {
    bool markerFound = false;     // the marker file exists beside the exe
    bool portable = false;        // marker found AND valid: the roots below are to be used
    std::string error;            // why a found marker is not usable ("" when fine or absent)
    std::filesystem::path root;   // <exeDir>/<name>
    std::filesystem::path settingsRoot, dataRoot, logRoot;
};

// True for a plain folder name this module accepts (see above).
bool validPortableFolderName(const std::string& name);

// Pure apart from reading the marker (and checking it is a regular file of at most 256 bytes). Writes nothing.
PortablePaths resolvePortable(const std::filesystem::path& exeDir);

// {"APPDATA", settingsRoot}, {"LOCALAPPDATA", dataRoot}; empty when not portable.
std::vector<std::pair<std::string, std::string>> portableEnvOverrides(const PortablePaths& p);

// Creates root/settings/data/logs (nothing else), verifying containment inside exeDir first and again after creation. false with `error` set on any failure
// (a read-only stick, a file where a folder belongs, a link that leads outside); nothing outside exeDir is ever created.
bool ensurePortableRoots(const std::filesystem::path& exeDir, PortablePaths& p, std::string& error);

// The folder the running executable lives in ("" when it cannot be found). Windows: GetModuleFileNameW.
std::filesystem::path currentExeDir();

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_PORTABLE_HPP
