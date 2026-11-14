// window_handover.hpp - LEGACY IS A CHOICE INSIDE THE NEW WINDOW (engine fixes 7; the owner, 2026-11-14: "people might still want the old layout, that is all").
//
// There are two windows on one engine: the new Slint window (foxsdr-slint.exe) and the old one, named Legacy wherever a person reads it (cascade.exe). Choosing "Legacy" in the
// new window's Layouts menu closes the new window and opens the Legacy window in its place, on the same settings file and the same radio; "Switch to the new window" in the
// Legacy window does the same the other way. It is the SAME install, engine and settings: the choice is a layout, not a second product.
//
// THE RULE THIS FOLLOWS (src/main.cpp, the software-GL relaunch): a second process starts only after the first has let go of everything it held - the settings lock beside the
// settings file (core/config_lock.hpp), the radios, the sound device, the listening ports. So a window that hands over
//     1. checks BEFORE it closes that the other program is there (preflight) and, when a recording or a patch is running, asks (the window's own question);
//     2. ends its run in the ordinary way (the engine stops: the radios close, the last save is made, the settings lock goes);
//     3. only THEN calls launch(), whose first act is to try to take the lock itself: a lock that is still held by this very process means the order was broken, and nothing is started;
//     4. starts the other program, marks it (kFromEnv, so it takes the primary-instance claim this process still holds for a moment, and does not hand back), and does not wait.
// The window calls launch() from the destructor of an object declared FIRST in main(), i.e. after everything else of the window - its engine service, its plugin kit, its screen -
// has been destroyed.
//
// WHEN IT FAILS. The preflight failure leaves the person where they are, with a sentence. A failure after the window is gone (the program would not start, the lock was still held)
// starts THIS window's own program again, marked with the failure, and the window that opens says the sentence (the note in kFromEnv). Nobody is left with no window.
//
// THE CHOICE IS REMEMBERED in a one-word file beside the settings file (window-choice: "legacy" or "new"), written when a hand-over has started the other program. The new window's
// start reads it: a person who last chose Legacy gets Legacy at the next start of the main entry. The Legacy program (cascade.exe) always opens Legacy, so the "FoxSDR (Legacy)"
// Start-menu entry opens what it says; a window started by a hand-over (kFromEnv present) never hands back, so there is no loop.
//
// TEST SEAMS (environment, read here and nowhere else; a person never sets them):
//   FOXSDR_HANDOVER_RECORD=<file>     launch() appends one line saying what it WOULD start (after the lock check) and starts nothing: a drive script asserts the request;
//   FOXSDR_HANDOVER_AT=<n>            the window chooses the other window at its n-th tick (new window) or frame (Legacy window) - the Layouts menu's entry / the button;
//   FOXSDR_HANDOVER_NEW_EXE / _LEGACY_EXE   the path of the other program (a build folder keeps the two programs apart; an install keeps them side by side);
//   FOXSDR_HANDOVER_CHILD_ARGS=<args> appended to the started program's command line (a test starts it bounded: "--frames 60");
//   FOXSDR_HANDOVER_CHILD_LOG=<file>  the started program's standard output and error are appended to this file (a test reads what the second window printed; a window that has
//                                     handed over has often changed its own standard handles, so they are not relied on for it).
//   None of the seams is passed on to the started program.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_WINDOW_HANDOVER_HPP
#define CASCADE_CORE_WINDOW_HANDOVER_HPP

#include <string>

namespace cascade::core::handover {

enum class Window { New, Legacy };

// "new" / "legacy": the words in the choice file, the marker and the record.
const char* windowWord(Window w);
// The program that opens a window: foxsdr-slint.exe / cascade.exe.
const char* programName(Window w);
Window other(Window w);

inline constexpr const char* kFromEnv = "FOXSDR_HANDOVER_FROM";   // on the started program: "<parent pid>:<window it came from>:<note>"
inline constexpr const char* kRecordEnv = "FOXSDR_HANDOVER_RECORD";
inline constexpr const char* kAtEnv = "FOXSDR_HANDOVER_AT";
inline constexpr const char* kNewExeEnv = "FOXSDR_HANDOVER_NEW_EXE";
inline constexpr const char* kLegacyExeEnv = "FOXSDR_HANDOVER_LEGACY_EXE";
inline constexpr const char* kChildArgsEnv = "FOXSDR_HANDOVER_CHILD_ARGS";
inline constexpr const char* kChildLogEnv = "FOXSDR_HANDOVER_CHILD_LOG";

// ---- the remembered choice ---------------------------------------------------------------------------------------------------------------------------------------------
// The file beside the settings file. "" for no settings file (a hermetic bounded run keeps nothing).
std::string choicePath(const std::string& configPath);
// New when there is no file, it cannot be read, or it holds anything but "legacy".
Window readChoice(const std::string& configPath);
// Written whole and renamed into place (never a half-written word). False when there is no settings file or the folder takes no file.
bool writeChoice(const std::string& configPath, Window w);

// ---- the mark on a started window ---------------------------------------------------------------------------------------------------------------------------------------
struct Marker {
    bool present = false;
    unsigned long parentPid = 0;
    Window from = Window::New;   // the window the person came from
    std::string note;            // "" (all went well) or "missing" / "failed:<windows error>" / "locked": the sentence this window shows
};
// Once, in process boot: reads the mark and clears it, so nothing this process starts inherits it.
Marker takeMarker();
// The sentence a window shows for `m.note`, in the window it IS now (`now`); "" when there is nothing to say.
std::string noteSentence(const Marker& m, Window now);

// ---- before closing --------------------------------------------------------------------------------------------------------------------------------------------------
struct Busy {
    bool recording = false;
    bool patch = false;
    bool any() const { return recording || patch; }
};
// The one plain sentence a window asks before leaving while something is running; "" when nothing is.
std::string busyQuestion(Busy b, Window target);
// "" when the other program can be started from here; else the sentence (the window stays where it is).
std::string preflight(Window target);

// ---- after closing -----------------------------------------------------------------------------------------------------------------------------------------------------------
enum class Lock { Free, HeldBySelf, HeldByOther };
// Tries to take the settings lock and lets go at once. Free when it was taken (or there is no settings file).
Lock probeLock(const std::string& configPath);
const char* lockWord(Lock l);

struct Request {
    Window from = Window::New;     // the window that is closing
    Window target = Window::Legacy;
    std::string configPath;        // the settings file this run owned ("" for none)
    bool safePlugins = false;      // carried over: --safe-plugins
};
struct Outcome {
    bool started = false;      // the target was started (or, with the record seam, recorded)
    bool recorded = false;     // the seam took it
    bool returned = false;     // the target was NOT started, and this window's own program was started again with the sentence
    std::string line;          // the recorded line, or the line a log keeps
    std::string sentence;      // why not, in the person's words (empty when started)
};
// The whole step, called once the window and its engine are gone. See the file comment for the order; never throws.
Outcome launch(const Request& r);

// TEST ONLY: what actually starts a program. The default sets kFromEnv to `marker` for the child and starts `exe` with `args` (CreateProcess, nothing waited for); a test hands in a
// function that records the call and may fail, so the order and the sentences are held without a second process. nullptr restores the default.
using Starter = bool (*)(const std::string& exe, const std::string& args, const std::string& marker, unsigned long& error);
void setStarterForTest(Starter s);

// THE OBJECT THAT DOES IT AT THE RIGHT TIME. Declared FIRST in a window's main(), so it is destroyed LAST: when a window has asked for a hand-over (set), the launch happens in its
// destructor, after every other object of the window - the engine service with its lock and radios, the plugin kit, the screen - is gone. (The Legacy window's main() holds its
// service in an inner scope; the object is outside it.) A process that never asks does nothing.
class HandoverAtExit {
public:
    HandoverAtExit() = default;
    HandoverAtExit(const HandoverAtExit&) = delete;
    HandoverAtExit& operator=(const HandoverAtExit&) = delete;
    void set(const Request& r) {
        req_ = r;
        has_ = true;
    }
    bool pending() const { return has_; }
    const Outcome& outcome() const { return outcome_; }
    ~HandoverAtExit() {
        if (has_) { outcome_ = launch(req_); }
    }

private:
    Request req_;
    bool has_ = false;
    Outcome outcome_;
};

}  // namespace cascade::core::handover

#endif  // CASCADE_CORE_WINDOW_HANDOVER_HPP
