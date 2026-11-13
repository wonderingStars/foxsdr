// needed_library.hpp - what a bridge says about the library the USER must install, as STRUCTURED data (the OWNER's needed-library rule, 2026-11-12 22:00,
// D:/foxsdr-build/prep/queued-briefs/NEEDED-LIBRARY-RULE.md), so the radio-setup page can draw "Open the download page", "Open that folder",
// "Choose the file..." and "Check again" from fields instead of parsing a sentence.
//
// Info is STATIC: the library's id (the key of the link registry: the address shown and opened comes from the registry, never from here), its name, the exact
// file names looked for, the minimum version, why it is needed, the install steps, the licence, and whether FoxSDR may fetch it itself (no: see each table).
// Check is the answer of "Check again": what was found, with the version and one plain sentence. A check LOADS AND UNLOADS the library each time and keeps
// nothing, so a library installed after FoxSDR started is found by the next check without restarting FoxSDR. A check opens no device, downloads nothing,
// opens no web page and copies no file, and raises no Windows dialog (the load goes through vendor_lib's silent loader).
//
// Sentences carry file NAMES, never folders (a folder holds the user name); `foundFolder` is a separate field for the "Open that folder" button only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_NEEDED_LIBRARY_HPP
#define CASCADE_SOURCE_NEEDED_LIBRARY_HPP

#include <string>
#include <vector>

namespace cascade::source::needed {

enum class State {
    Working,       // found, 64-bit, loads, offers every call FoxSDR needs
    NotFound,      // no such file where FoxSDR looked
    TooOld,        // found and loads, but does not offer a call FoxSDR needs
    Wrong32Bit,    // found, but a 32-bit file (FoxSDR is 64-bit)
    WouldNotLoad,  // found, 64-bit, and the loader refused it (a dependency missing, damaged)
    NotALibrary,   // found, but not a library at all (a saved web page, a truncated file, another processor)
};
const char* stateWord(State s);  // "working", "not-found", "too-old", "wrong-32-bit", "would-not-load", "not-a-library"

struct Info {
    std::string libraryId;                  // key of the link registry, e.g. "ftdi-d2xx"
    std::string name;                       // "FTDI D2XX driver"
    std::vector<std::string> fileNames;     // looked for, in this order
    std::string minVersion;                 // "" = none known; the text says what "too old" means then
    bool needs64Bit = true;
    std::string why;                        // one line: what works once it is there
    std::string publisher;                  // "FTDI"
    std::string officialAddress;            // ONLY an address read in the publisher's own documentation, else "" (unverified: not shown to users)
    std::string installKind;                // "installer" or "copy-in"
    std::string copyInFolder;               // for a copy-in library: where the file goes ("" for an installer)
    std::vector<std::string> steps;         // numbered install steps, English
    std::string licence;                    // the licence name; "(unverified)" when the lane did not read it
    std::string notSuppliedLine;            // "FoxSDR does not include or supply it."
    bool fetchAllowed = false;              // one-click fetch: false unless the row of the NEEDED LIBRARIES table proves it
};

struct Check {
    State state = State::NotFound;
    std::string fileName;       // the file found ("" when none)
    std::string version;        // the file's own version resource, "" when it states none
    std::string lookedIn;       // "the folder you chose and the folders on your PATH" (categories, never folders)
    std::string foundFolder;    // structured only, for "Open that folder"; never put in a sentence
    std::string missing;        // the calls it does not offer ("too old"), joined with ", "
    std::string sentence;       // one plain sentence for the page
    bool working() const { return state == State::Working; }
};

// The version resource of a file ("10.0.22621.1"), read without running any of its code; "" when the file has none or cannot be read.
std::string fileVersion(const std::string& utf8Path);

}  // namespace cascade::source::needed

#endif  // CASCADE_SOURCE_NEEDED_LIBRARY_HPP
