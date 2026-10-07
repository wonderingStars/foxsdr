// Whose console this is: none, unless the program that started us has one.
//
// WHY THERE IS A DECISION AT ALL. cascade.exe used to be linked as a console-
// subsystem program so that the tool modes - --version, --selftest, the bench
// checks - print to the terminal that ran them. The price was that every launch
// NOT from a terminal - the Start Menu, Explorer, the Store's tile, the
// installer's "Launch now" - made Windows create a console for the process, and on
// a desktop where Windows Terminal is the default console host that console is a
// Windows Terminal window, black, titled cascade.exe, beside the real one for as
// long as the program ran. Closing it killed the process until 0.99.67
// (console_close.hpp); and it is a window that has no business being there at all.
//
// RELEASING THAT CONSOLE FROM main() (FreeConsole) WAS TRIED FIRST AND MEASURED, and
// is not enough: the Windows Terminal window's visible time fell from about 1950 ms,
// the whole of a `--frames 60` launch started the way the Start Menu starts it, to
// about 255 ms, and no further, because the console host is created by process
// start-up, before main runs (main was entered 447-515 ms after the process was
// created - the 16 MB image loads first - and the window was visible from about
// 300 ms). The only way to have no window is to have no console at start-up, which
// is what the windows subsystem is. So cascade.exe is linked as one (CMakeLists.txt:
// WIN32 and /ENTRY:mainCRTStartup, so main() stays main()), and a launch from the
// Start Menu or Explorer now creates no console at all - and no FreeConsole is
// needed: a windows-subsystem image does not get one even from a parent that asks
// (CREATE_NEW_CONSOLE), which tests/test_console_owner_app.cpp shows.
//
// This file is the other half: giving the tool modes their terminal back. A
// windows-subsystem process has no console and no standard handles unless its
// starter redirected them, so a `cascade --version` typed into a shell would print
// nowhere. GetConsoleProcessList() at the top of main tells the cases apart:
//
//   0   no console - the normal start of a windows-subsystem image, whoever started
//       it. Try AttachConsole(ATTACH_PARENT_PROCESS): when the parent is a shell the
//       process borrows its console, and Ctrl+C, a closing terminal window and the
//       tool modes' output all work as they did; when the parent is Explorer or the
//       Start Menu there is no parent console, the call fails, and the process
//       carries on without one, which is the point.
//   >0  a console is already there (something in the process, or a debugger, made
//       one). It is not ours to touch.
//
// The decision is a pure function so both branches are a unit test
// (tests/test_console_owner.cpp), and the Win32 call lives in one thin function.
//
// STANDARD HANDLES. A stream that already has something behind it is never
// replaced: a pipe or a file the caller redirected to is somebody's capture, and
// ctest reading "rendered 3 frames" from the child depends on it. Only a CRT
// stream with nothing behind it is bound to the console just attached.
//
// Not Windows: there is no console to own, and applying it does nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_CONSOLE_OWNER_HPP
#define CASCADE_CORE_CONSOLE_OWNER_HPP

namespace cascade::core {

enum class ConsoleAction {
    Keep,    // a console is already there: leave it as it is
    Attach,  // no console: borrow the parent's, if it has one
};

struct ConsoleDecision {
    ConsoleAction action = ConsoleAction::Keep;
    // One line for the diagnostics log, static storage, never null.
    const char* reason = "";
};

// The pure decision. `processCount` is what GetConsoleProcessList returned at the
// top of main: 0 when the process has no console (or the call failed), more when it
// is attached to one.
ConsoleDecision decideConsole(unsigned processCount);

// What applyConsoleOwnership did, for the diagnostics log once it is running.
struct ConsoleOutcome {
    ConsoleDecision decision;
    bool attached = false;  // Attach, and there was a parent console to attach to
};

// Reads the process count, decides, and carries it out (Windows). Call it first
// thing in main(), before anything is printed. On other platforms: Keep, and
// nothing is done.
ConsoleOutcome applyConsoleOwnership();

// The outcome applyConsoleOwnership returned; Keep with an empty reason if it was
// never called.
ConsoleOutcome consoleOutcome();

}  // namespace cascade::core

#endif  // CASCADE_CORE_CONSOLE_OWNER_HPP
