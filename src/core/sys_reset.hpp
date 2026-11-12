// sys_reset.hpp - whole-app reset to defaults: a plan (the dry run), a confirm token, and a reversible execution.
//
// WHAT "DEFAULTS" MEANS. The program's defaults are what it does with no files: AppConfig{} and an empty bookmark list. So a reset is the removal of the files that hold the user's
// choices, from the settings folder (config.json, its dated .bak copies, bookmarks.json and its dated .bak copies, markers.json). The plan can also include, only when the caller asks,
// the plugins folder and the log folder of the data folder - they are NOT part of a default reset, because a plugin costs a download and a log is evidence.
//
// SAFE BY CONSTRUCTION:
//   * a DRY RUN: planReset() lists exactly what would move (names and sizes) and writes nothing;
//   * a CONFIRM TOKEN over that listing: executeReset() refuses unless the token matches the listing as it is NOW, so a folder that changed after the plan was shown is not reset blind;
//   * an ALLOW-LIST: only the file names and name shapes above are ever touched; any other file in the folder stays, and the plan says how many files it left alone;
//   * a NAMED-FOLDER GUARD: the settings and data folders must themselves be named "foxsdr" (any case) - so a wrong path (a drive root, Documents) is refused outright;
//   * REVERSIBLE: nothing is deleted. Each item is moved into <folder>/reset-<stamp>/ ; undoReset() moves them back (and refuses to overwrite anything that has appeared since);
//     removing the reset-<stamp> folder for good is a separate, explicit act the caller offers afterwards.
// The wiring (a "Reset everything" button that shows the plan, asks for the token, then restarts the program) is a hand-off in the record.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_RESET_HPP
#define CASCADE_CORE_SYS_RESET_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cascade::core::sys {

struct ResetOptions {
    bool includePlugins = false;   // <dataDir>/plugins
    bool includeLogs = false;      // <dataDir>/logs
};

enum class ResetKind { SettingsFile, PluginsFolder, LogsFolder };

struct ResetItem {
    ResetKind kind = ResetKind::SettingsFile;
    std::string relative;          // relative to its folder (the settings folder, or the data folder)
    bool isFolder = false;
    std::uint64_t bytes = 0;       // for a folder: the total of the files inside
    std::uint64_t files = 1;
};

struct ResetPlan {
    std::string error;             // non-empty = nothing may be done (the folder guard, an unreadable folder)
    std::vector<ResetItem> items;
    std::uint64_t leftAlone = 0;   // files in the settings folder that are NOT ours and stay
    std::string confirmToken;      // "RESET-" + 8 hex
};

bool settingsFileNameIsOurs(const std::string& name);   // the allow-list's name shapes

ResetPlan planReset(const std::filesystem::path& settingsDir, const std::filesystem::path& dataDir, const ResetOptions& opt);

// Moves every planned item aside into <its folder>/reset-<stamp>. false with `error` on any failure, with everything already moved put back.
bool executeReset(const std::filesystem::path& settingsDir, const std::filesystem::path& dataDir, const ResetOptions& opt, const std::string& token, const std::string& stamp, std::string& error);

// Moves reset-<stamp> contents back. Refuses (changing nothing) if any original place is occupied again.
bool undoReset(const std::filesystem::path& settingsDir, const std::filesystem::path& dataDir, const std::string& stamp, std::string& error);

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_RESET_HPP
