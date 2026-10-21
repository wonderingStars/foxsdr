// foreign_modules.hpp - naming the OTHER SOFTWARE'S DLLs that are loaded into this
// process, at start and as each one arrives (0.99.69), so that a report of a death
// in the graphics hand-off says what else was in the process.
//
// WHY. Two field deaths - 0.99.64 on an RSP1 after 48 minutes, 0.99.65 on an
// RTL-SDR after twelve seconds - were fast-fails "in present" and "in render": the
// hand-off to the graphics driver, where the overlay DLLs that other software
// injects into every process (an audio driver's on-screen display, a frame-rate
// counter, an input tool, a recorder's hook) put themselves in the path of every
// swap. The only death of that shape ever seen on the developer's own desktop was
// inside NahimicOSD.dll, an audio-driver overlay. The reports carry no stack and
// no module; since 0.99.66 the sentinel adds the FAULTING module from Windows'
// Application Error event, but nothing said what ELSE was loaded, which is the
// question a reader asks next. This answers it: one log line at start and one on
// each arrival, naming every loaded module that is neither Windows' nor ours, and
// the same list in the report's own context block (`foreign-modules:`), because
// the log's tail is 256 lines and a long session's first lines are gone from it.
//
// WHAT IS "OURS", and the rule is deliberately short so that the list is short:
//
//   - the program's own folder and everything beneath it - the executable, the
//     DLLs vcpkg puts beside it, the plugins' folder, the bundled C++ runtime;
//   - the Windows folder and everything beneath it (System32, SysWOW64, WinSxS,
//     the driver store): Windows' own, which is never the unexpected one;
//   - the SoapySDR vendor installs the application found and adopted (the folder
//     of their modules and the folder of the DLLs those modules depend on:
//     source/soapy_modules.hpp) and every folder SOAPY_SDR_PLUGIN_PATH names -
//     SDR vendor code the application loads on purpose, never an overlay;
//   - the C and C++ runtime modules by the exact paths the loader reports for them
//     (a copy under System32 or beside the executable is already covered; this is
//     for a Redist folder a development machine loads them from).
//
// Everything else is FOREIGN: antivirus hooks, overlays, input and capture tools,
// shell extensions, a vendor's own API DLL that is not under a vendor root.
//
// WHAT IS DISCLOSED, and it is a privacy matter: the FILE NAMES of other software's
// components. Never a folder (a folder can hold an account name), never a version,
// never a signature. A file name can say that a piece of software is installed on a
// computer, so PRIVACY.md and the web page say so in plain words; system DLLs are
// excluded so the list is short and means something. Every name is reduced to a plain
// ASCII file name (isPlainModuleName), so nothing but a name can reach a report.
//
// THE CLASSIFIER IS PURE (isForeignModule): a path and the roots in, a bool out. It
// compares CANONICAL STRINGS, never the disk. Why not GetFinalPathNameByHandle: it
// opens each module's file (a module on a disconnected share, or one whose file has
// been deleted since it was mapped, fails or stalls), it resolves junctions and
// symbolic links to somewhere the roots do not name, and it cannot be tested on the
// synthetic paths the property needs (`C:\Program Files\Nahimic\NahimicOSD.dll`
// exists on no machine of ours). So the string is canonicalised by hand - the
// \\?\ and \??\ prefixes dropped, slashes made backslashes, `.` and `..` resolved,
// repeated separators collapsed, case folded the way the file system folds it
// (CompareStringOrdinal) - which is what PathCchCanonicalizeEx does for a string, with
// one difference that matters here: it needs no Windows header and no buffer. The one
// thing a string cannot know is an 8.3 short name, so the Windows half expands a
// `~` path with GetLongPathNameW (local drive letters only; it can block on a network
// path) before it asks, for the roots and for the module alike.
//
// ARRIVALS are caught with LdrRegisterDllNotification from ntdll.dll. It is not in the
// SDK headers (winternl.h declares neither it nor its structures), so the entry points
// are resolved with GetProcAddress and the structures are declared in the .cpp from the
// documented layout. It has been in ntdll since Vista and crash reporters and security
// products have used it for as long. The callback runs INSIDE the loader lock, so it
// does nothing but copy two strings and a tick count into a preallocated slot under a
// lock that nothing ever holds across a call into the loader; it allocates nothing,
// logs nothing and calls nothing that loads. The window's thread drains the slots once
// a frame, decides whether each is foreign, and writes the line. Where the registration
// fails (the entry point is missing or refuses), the same drain is fed by a rescan of
// the loaded modules every ten seconds on a thread of its own, and the log says so once.
//
// LINUX. Nothing here is asked for: the classifier and the text functions compile and
// are tested everywhere, and the watch reports "(not applicable)".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FOREIGN_MODULES_HPP
#define CASCADE_CORE_FOREIGN_MODULES_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cascade::core {

// ---------------------------------------------------------------------------
// The classifier
// ---------------------------------------------------------------------------

// What is "ours". Folders count with everything beneath them; a file is matched
// exactly. Any of them may be empty (a platform that has no Windows folder).
struct ForeignRoots {
    std::wstring programFolder;               // the folder of the program file
    std::wstring windowsFolder;               // %SystemRoot%, from the API and not the environment
    std::vector<std::wstring> vendorFolders;  // SoapySDR vendor module and DLL folders
    std::vector<std::wstring> runtimeFiles;   // the C and C++ runtime, by the paths the loader reports
};

// True when `fullPath` is under none of the roots - the unexpected one. Pure: no
// disk, no Windows. Case-insensitive; accepts \\?\, \\?\UNC\ and \??\ prefixes,
// forward slashes, `.` and `..`. An empty path is not a module (false); a path that
// is not absolute cannot be shown to be ours, so it is foreign. A root that names
// a whole drive is ignored - it would make every path on the drive "ours".
bool isForeignModule(const std::wstring& fullPath, const ForeignRoots& roots);

// The last component of `path`, as plain ASCII, NEVER with a folder in it. Empty
// when the path ends in a separator or names nothing. Any character that is not a
// letter, a digit or one of . _ - + ( ) ~ @ & ! = ; { } [ ] (or a space inside the
// name) is written as '?', one per code point, and the result is cut at
// kForeignNameMaxBytes: a name that can be put in a report is a name that cannot
// carry anything else.
std::string fileNameOnly(const std::wstring& path);

// Whether `name` is what fileNameOnly could have produced: 1 to kForeignNameMaxBytes
// of the characters above, no leading or trailing space, no separator. The reader
// side of the same rule (the uploader and the sentinel apply it to text from a file).
bool isPlainModuleName(const std::string& name);

inline constexpr std::size_t kForeignNameMaxBytes = 63;

// ---------------------------------------------------------------------------
// The words
// ---------------------------------------------------------------------------

// The longest the start line's MESSAGE is (the log adds a stamp and a level, and a
// line cut at the ring's width has every number on it masked by the upload scrub, so
// this stays well under 191 - 18), and the longest the context value is. The context
// block is 4096 bytes for everything, with the plugin list last (it is the part cut).
inline constexpr std::size_t kForeignStartLineMaxChars = 150;
inline constexpr std::size_t kForeignFieldMaxChars = 480;
// The most names kept in memory, and the most arrival lines one session writes.
inline constexpr std::size_t kForeignMaxNames = 256;
inline constexpr unsigned kForeignMaxArrivalLines = 32;
// The fallback's period.
inline constexpr unsigned kForeignRescanMs = 10000;

// The names, alphabetical (ASCII case folded), each once whatever its case.
std::vector<std::string> sortedUniqueNames(std::vector<std::string> names);

// The start line's message: `modules: none foreign`, or
// `modules: 3 foreign - A.dll, B.dll, C.dll`, and when the names do not fit in
// kForeignStartLineMaxChars, as many as do and `, +K more`. `names` is the whole
// list, already sorted and unique (sortedUniqueNames).
std::string foreignStartLineText(const std::vector<std::string>& names);

// The arrival line's message: `module arrived: A.dll (12.3 s)`, the seconds since the
// process started, one decimal.
std::string foreignArrivalLineText(const std::string& name, double sinceStartSec);

// The VALUE of the context line `foreign-modules`: the names, alphabetical, joined by
// ", ", at most kForeignFieldMaxChars, then `, +K more` for those left out and for
// `unlisted` more that the caller knows of but cannot name. `(none)` for no names.
std::string foreignFieldText(const std::vector<std::string>& names, std::size_t unlisted = 0);

// The value as a reader should carry it on: a list re-rendered from the names that
// pass isPlainModuleName (anything else in it is dropped, not repaired), `(none)` for
// the one fixed sentence that means "none were foreign", and an EMPTY string for every
// other value - `(not recorded)`, `(not scanned yet)`, `(not applicable)`, text nobody
// wrote - because those say nothing about the modules.
std::string normaliseForeignField(const std::string& value);

// What a session's log says about the foreign modules: the list its `modules:` line
// named, plus every `module arrived:` line after it. `known` is false when the log
// has neither (diagnostics was switched on part-way, or the start line has been
// rotated away); `unlisted` is the "+K more" of the start line.
struct ForeignList {
    bool known = false;
    std::vector<std::string> names;  // sorted, unique
    std::size_t unlisted = 0;
};
ForeignList foreignListFromLog(const std::vector<std::string>& lines);

// The context value for what a log said: foreignFieldText, or `(not recorded)`.
std::string foreignFieldFromLog(const std::vector<std::string>& lines);

// Whether a log line, as the log file holds it ("12:34:56.789 info modules: ..."), is
// one of the two this file writes. For the reader that picks them out of a whole session.
bool isForeignModulesLogLine(const std::string& line);

// ---------------------------------------------------------------------------
// This process
// ---------------------------------------------------------------------------

// The full paths of the modules loaded right now, short names expanded. Windows only
// (EnumProcessModulesEx with LIST_MODULES_ALL, GetModuleFileNameExW); empty elsewhere.
// Healthy path only, and never from a loader callback: it reads the loader's list.
//
// WHY NOT THE MODULE TABLE THE CRASH HANDLER KEEPS (core/diag_report.hpp). It would have
// been the thing to reuse, and it cannot be: it holds each module's FILE NAME only
// (DiagModule::name, 64 bytes - there is no folder in it to classify by), it is rebuilt
// only when plugins load, and it stops at 256 modules. This list is built for the
// question "whose is it", which is a question about the folder.
std::vector<std::wstring> loadedModulePaths();

// The roots for THIS process: the folder of the program file, the Windows directory,
// the vendor folders registered below and named by SOAPY_SDR_PLUGIN_PATH, and the C
// and C++ runtime modules that are loaded. Windows only; empty elsewhere.
ForeignRoots currentForeignRoots();

// A folder of SDR vendor code the application loads on purpose, to be counted as ours
// (source::ensureVendorModulesVisible calls this for what it adopts). Everything
// beneath it counts. Idempotent. `folder` is in the application's own narrow
// encoding (what std::filesystem::path takes).
void registerOwnModuleFolder(const std::string& folder);

// The names the classifier calls foreign among `paths`: sorted and unique. Pure.
std::vector<std::string> foreignNamesOf(const std::vector<std::wstring>& paths,
                                        const ForeignRoots& roots);

// loadedModulePaths() through currentForeignRoots(): what the start line lists.
std::vector<std::string> snapshotForeignModules();

// ---------------------------------------------------------------------------
// The watch
// ---------------------------------------------------------------------------
struct ForeignWatchOptions {
    // False: do not register the loader notification and rescan instead - exactly
    // what a failed registration does. A test seam for the fallback; nothing in the
    // application sets it.
    bool notification = true;
    // The fallback's period.
    unsigned rescanMs = kForeignRescanMs;
};

// One watch per application. start() returns at once; everything slow happens on a
// thread of its own. poll() is for the GUI thread, once a frame.
//
// THE ORDER OF THE LINES IS GUARANTEED: the start line is written before any arrival
// line, because poll() does nothing until the start scan has finished, and an arrival
// of a name the scan already listed is not written again (the notification is
// registered BEFORE the scan so that no module can fall between the two).
//
// THE DESTRUCTOR NEVER WAITS: it unregisters the notification (which returns once no
// callback is running) and stops the worker, which owns what it touches.
class ForeignModuleWatch {
public:
    explicit ForeignModuleWatch(ForeignWatchOptions options = ForeignWatchOptions());
    ~ForeignModuleWatch();
    ForeignModuleWatch(const ForeignModuleWatch&) = delete;
    ForeignModuleWatch& operator=(const ForeignModuleWatch&) = delete;

    // Registers the notification and starts the scan. Idempotent.
    void start();

    // Writes a line for each foreign module that has arrived since the last call.
    // Cheap when none has: one atomic load.
    void poll();

    // The context value (`foreign-modules:`), always: `(not scanned yet)` until the
    // start scan has finished, `(none)`, the list, or `(not applicable)` off Windows.
    std::string contextValue() const;

    // The scan has finished and the start line has been written.
    bool scanned() const;

    // The names known so far, sorted.
    std::vector<std::string> names() const;

    // The loader notification is registered (false before start(), after a failed
    // registration, and off Windows).
    bool usingNotification() const;

private:
    struct Impl;
    // The thread's body: it holds a reference of its own, so the watch can be destroyed
    // while it is still inside a slow call.
    static void worker(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_FOREIGN_MODULES_HPP
