// wire_b_needed_libs.hpp - the NEEDED LIBRARIES of the sources the wire-b lane registered, as STRUCTURED data (the owner's needed-library rule, 2026-11-12;
// docs/multi-radio-records/wire-b.md "NEEDED LIBRARIES"). One row per thing the USER must install (or run) before a source can work: a vendor library, a
// SoapySDR module, a vendor program that serves the radio. FoxSDR ships none of them.
//
// WHAT A ROW SAYS (rule items 1, 2, 5, 6; the buttons of items 3 and 4 belong to the shared component the dedicated lane builds, not to this file):
//   libraryId, sourceKind (the SELECT_SOURCE key it serves), name, fileNames FoxSDR looks for, minVersion ("" = no document read by this lane states one), bits64,
//   why, officialPage + pageStatus (an address is shown ONLY when pageStatus is "verified": nothing here is, so every row says "unverified" and shows no address),
//   installKind, folderNote, steps (numbered, English), licence, notSupplied (one line), fetchAllowed ("no (unverified)": no terms were read).
// CHECK AGAIN (rule item 5) = recheckNeeded(): looks again NOW, without restarting FoxSDR, and answers one of
//   found | not-found | wrong-32-bit | unusable | cannot-check
// with a sentence and where it looked. The check reads FILE HEADERS only (source/vendor_lib.hpp checkFile): it never loads the vendor's code. "found but too old"
// and "found but would not load" are NOT reported by this check: no version is read from the file, and loading happens when the radio is opened (the source says why
// it refused then). A user-chosen file ("Choose the file...") is judged with the same check by passing it as `where`; nothing is copied here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "engine/wire_b_sigmeas.hpp"   // wireb::Json

namespace cascade::engine::wireb {

enum class NeededKind : unsigned char {
    VendorLibrary,    // one DLL, found by file name (the G39DDC / G33DDC libraries)
    SoapyModule,      // a SoapySDR module, found by a fragment of its file name in a vendor install's module folder
    RunningProgram,   // a vendor program that serves the radio on the network: nothing to find on disk by name
    RemoteSoftware,   // software on ANOTHER machine: nothing on this PC
    UserFile          // a file the user chooses (a firmware image): nothing to look for by name
};

struct NeededLibrary {
    std::string libraryId;                 // stable, lower-case, hyphens: shared with the other lanes' tables
    std::string sourceKind;                // the SELECT_SOURCE key (g39ddc ...) or "" when the need is a Soapy module
    std::string name;                      // English
    NeededKind kind = NeededKind::VendorLibrary;
    std::vector<std::string> fileNames;    // VendorLibrary: the exact DLL names; SoapyModule: a fragment of the module file name (lower case)
    std::string minVersion;                // "" = not stated by any document this lane read
    bool bits64 = true;
    std::string why;                       // one line: what works once it is there
    std::string officialPage;              // "" until verified
    std::string pageStatus = "unverified"; // "verified" only when the lane read it in the publisher's own documentation
    std::string installKind;               // "copy-in" | "installer" | "running program" | "software on another machine" | "your own file"
    std::string folderNote;                // where a copy-in library goes
    std::vector<std::string> steps;
    std::string licence;
    std::string notSupplied = "FoxSDR does not include or supply it.";
    std::string fetchAllowed = "no (unverified)";
};

const std::vector<NeededLibrary>& neededLibraries();
const NeededLibrary* findNeededLibrary(const std::string& id);

// What the check may touch, so a test can give a fabricated machine.
struct NeededEnv {
    std::function<std::string(const char*)> getenv;
    std::function<bool(const std::string&)> isDirectory;
    std::function<std::vector<std::string>(const std::string&)> listEntries;   // the names (files and folders) in a folder; empty when it cannot be read
};
NeededEnv defaultNeededEnv();

// Looks again now. `where` may be empty, a folder or a file the user picked (VendorLibrary rows only; for any other row it is refused with BAD in the answer).
// The answer: {state, sentence, looked_in, found_in (a folder, never a file path), file}.
Json recheckNeeded(const NeededLibrary& row, const std::string& where, const NeededEnv& env);

// One row as the fact shows it: every field above plus "check" (the last answer, or null when never checked).
Json neededToJson(const NeededLibrary& row, const Json& lastCheck);

}  // namespace cascade::engine::wireb
