// damaged_file.hpp - a file that failed to load is kept aside, never saved over (0.99.65).
//
// WHAT WENT WRONG. config.json, bookmarks.json and markers.json are read once at start-up; one that
// fails to load (cut off by a crash mid-write, edited by hand with a quote missing, written by a
// newer version with a schema this one does not know) left the window on defaults - and the first
// change, or the clean exit, saved a good file over it. The damaged one, which still held the whole
// configuration or the whole list, was gone, and nothing had said it was bad but a line on stderr
// (settings) or a red line the first successful save cleared (the two lists).
//
// THE RULE. When such a file EXISTS, is a regular file and is NOT EMPTY, and fails to load, the
// application renames it - at start-up, where the load failed, before anything can save over it -
// to "<name>.bad-<UTC yyyymmdd-hhmmss>" in the same folder (a second copy in the same second gets
// "-2", "-3" ... after the stamp), and keeps the three newest such copies of that file, removing
// older ones. A missing file (a first run) and a zero-byte file have nothing to keep. A folder
// squatting on the name is not a file of the user's data and is left alone.
//
// WHEN THE RENAME IS REFUSED (another program holds the file, the folder is read-only) the result
// says so and the caller must not save over the file in this session: AppWindow tells that file's
// saver to write nothing (gui/config_writer.hpp, forbidWrites).
//
// The band plan is never written by the application (it is read-only data it ships or the user
// installs), so it has nothing to protect and is not handled here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DAMAGED_FILE_HPP
#define CASCADE_CORE_DAMAGED_FILE_HPP

#include <cstdint>
#include <ctime>
#include <string>

namespace cascade::core {

// How many ".bad-*" copies of one file are kept.
inline constexpr int kKeptDamagedCopies = 3;

struct SetAsideResult {
    enum class Outcome {
        NothingToKeep,  // missing, empty, or not a regular file
        KeptAside,      // renamed to `keptAs`
        CouldNotKeep    // exists and is not empty, and could not be renamed
    };
    Outcome outcome = Outcome::NothingToKeep;
    std::uintmax_t bytes = 0;  // the damaged file's size (not NothingToKeep)
    std::string keptAs;        // the new full path (KeptAside) - never logged
};

// Keeps `path` aside as described above. `now` is the clock the stamp is read from (UTC); `keep`
// the number of copies kept. Never throws.
SetAsideResult setDamagedFileAside(const std::string& path, std::time_t now = std::time(nullptr),
                                   int keep = kKeptDamagedCopies);

}  // namespace cascade::core

#endif  // CASCADE_CORE_DAMAGED_FILE_HPP
