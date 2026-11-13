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

#include <functional>
#include <string>
#include <vector>

namespace cascade::source::needed {

// The owner's rule names six states; a seventh exists for the needs a PC cannot be asked about (a program that is running, software on another machine, a
// firmware file the user keeps): the six keep their names and meaning.
enum class State {
    Working,       // found, 64-bit, loads, offers every call FoxSDR needs. A row whose check reads file headers only (Check::depth "headers") says "working" for
                   // "found, 64-bit, well formed"; it has not been loaded and the sentence says so.
    NotFound,      // no such file where FoxSDR looked
    TooOld,        // found and loads, but does not offer a call FoxSDR needs
    Wrong32Bit,    // found, but a 32-bit file (FoxSDR is 64-bit)
    WouldNotLoad,  // found, 64-bit, and the loader refused it (a dependency missing, damaged)
    NotALibrary,   // found, but not a library at all (a saved web page, a truncated file, another processor)
    CannotCheck,   // nothing on this PC to look at (see Kind): opening the radio says whether it works
};
const char* stateWord(State s);  // "working", "not-found", "too-old", "wrong-32-bit", "would-not-load", "not-a-library", "cannot-check"

// What the user must have, which decides how (and whether) it can be checked on this PC.
enum class Kind {
    LibraryFile,      // one library file found by name: in a folder or file the person gave, or on the PATH
    SoapyModule,      // a SoapySDR module, found by a fragment of its file name in the module folder of a PothosSDR or radioconda install
    RunningProgram,   // a vendor program that serves the radio on the network: nothing to find on disk by name
    RemoteSoftware,   // software on ANOTHER machine: nothing on this PC
    UserFile,         // a file the user keeps (a firmware image): nothing to look for by name
};
const char* kindWord(Kind k);  // "library-file", "soapy-module", "running-program", "remote-software", "user-file"

struct Info;
struct Check;

// What a check may touch, so a test can give a fabricated machine (only the SoapySDR-module check reads it).
struct Env {
    std::function<std::string(const char*)> getenv;
    std::function<bool(const std::string&)> isDirectory;
    std::function<std::vector<std::string>(const std::string&)> listEntries;   // the names (files and folders) in a folder; empty when it cannot be read
};
Env defaultEnv();

struct Info {
    std::string libraryId;                  // key of the link registry, e.g. "ftdi-d2xx"
    std::string sourceKind;                 // the SELECT_SOURCE key this need serves ("g39ddc", "sdriq"), "" when it serves none by itself (a Soapy module)
    Kind kind = Kind::LibraryFile;
    std::string name;                       // "FTDI D2XX driver"
    std::vector<std::string> fileNames;     // looked for, in this order (SoapyModule: a lower-case fragment of the module file name)
    std::string minVersion;                 // "" = none known; the text says what "too old" means then
    bool needs64Bit = true;
    std::string why;                        // one line: what works once it is there
    std::string publisher;                  // "FTDI"
    std::string officialAddress;            // ONLY an address read in the publisher's own documentation, else "" (unverified: not shown to users)
    std::string pageStatus = "unverified";  // "verified" only when the address was read in the publisher's own documentation; an unverified address is shown to nobody
    std::string installKind;                // "installer", "copy-in", "running program", "software on another machine" or "your own file"
    std::string copyInFolder;               // for a copy-in library: where the file goes ("" for an installer)
    std::string folderNote;                 // one sentence about where it goes (or why there is no folder), "" when none
    std::vector<std::string> steps;         // numbered install steps, English
    std::string licence;                    // the licence name; "(unverified)" when the lane did not read it
    std::string notSuppliedLine = "FoxSDR does not include or supply it.";
    bool fetchAllowed = false;              // one-click fetch: false unless the row of the NEEDED LIBRARIES table proves it
    std::string fetchNote = "no (unverified)";   // what a screen shows while fetchAllowed is false: "no (unverified)", "no (terms)", "no (patent)"
    // "Check again" for this row: looks NOW, without a restart. `where` is "" (look where FoxSDR normally looks) or a folder or file the user picked. A row's
    // check loads vendor code only if its own function says so (today only ftdi-d2xx does: through the silent loader); null = cannot be checked.
    Check (*check)(const Info& row, const std::string& where, const Env& env) = nullptr;
};

struct Check {
    State state = State::NotFound;
    std::string fileName;       // the file found ("" when none)
    std::string version;        // the file's own version resource, "" when it states none
    std::string lookedIn;       // "the folder you chose and the folders on your PATH" (categories, never folders)
    std::string foundFolder;    // structured only, for "Open that folder"; never put in a sentence
    std::string missing;        // the calls it does not offer ("too old"), joined with ", "
    std::string sentence;       // one plain sentence for the page
    std::string depth;          // how far the check went: "headers" (file headers read, nothing loaded), "loaded" (loaded and unloaded) or "" (nothing to look at)
    bool working() const { return state == State::Working; }
};

// THE ONE TABLE: every library, module, program, remote software and file a source needs, in a fixed order (library ids are stable). The ftdi-d2xx row is
// the one the SDR-IQ bridge defines (source/sdriq_d2xx.hpp); the rest are written here.
const std::vector<Info>& neededTable();
const Info* findNeeded(const std::string& libraryId);   // nullptr when there is no such id

// Runs the row's check; a row without one answers CannotCheck. Never throws.
Check runCheck(const Info& row, const std::string& where, const Env& env);

// The version resource of a file ("10.0.22621.1"), read without running any of its code; "" when the file has none or cannot be read.
std::string fileVersion(const std::string& utf8Path);

}  // namespace cascade::source::needed

#endif  // CASCADE_SOURCE_NEEDED_LIBRARY_HPP
