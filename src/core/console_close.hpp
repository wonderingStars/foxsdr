// The console's own way of ending a program, turned into the window's close.
//
// Until 0.99.69 cascade.exe was a console-subsystem program (so that the tool modes
// print to the terminal that ran them), which meant a Start Menu launch brought a
// console window of its own - a black window, or a Windows Terminal tab, titled
// cascade.exe - beside the real one (diag_log.hpp, stderrIsWatchedConsole). It is a
// windows-subsystem program now and a Start Menu launch has no console at all
// (console_owner.hpp), but a session started from a terminal still borrows the
// terminal's console, and the three events below are delivered to it just the same.
// A console delivers three events to the
// programs on it: Ctrl+C, Ctrl+Break, and its window being closed. A program that
// has installed no handler gets kernel32's default one, which answers each with
// ExitProcess(STATUS_CONTROL_C_EXIT): the process is gone at once, mid-frame, with
// nothing saved and the radio left open, and the sentinel files an ending from
// outside. That is the 0.99.66 field report of 2026-10-06 (code 0xC000013A, "window
// was drawing - Ctrl+C or a console close"): someone closed the console window
// the program had then.
//
// This handler answers the three events by asking the frame loop to end, which is
// what the window's own close button does, so the ordinary shutdown runs: settings
// written, the radio closed, the sentinel told. For Ctrl+C and Ctrl+Break the
// process then simply carries on until the loop ends it. For a closing console the
// system ends the process the moment the handler returns, so the handler waits for
// the shutdown instead - up to kConsoleCloseWaitMs, under the 5000 ms the system
// allows (HandlerRoutine's Timeouts table, SPI_GETHUNGAPPTIMEOUT) - and on a clean
// finish the process exits with 0 during that wait, taking the handler's thread
// with it. A shutdown still going when the wait ends is ended by the system as
// before, and the sentinel says so.
//
// Logoff and shutdown events never reach a console program that loads user32.dll
// (docs/DIAGNOSTICS.md, "What Windows gives a process when the session ends"); the
// window's WM_ENDSESSION covers those.
//
// Windows only. Elsewhere installing is a no-op that returns false and nothing is
// ever requested.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_CONSOLE_CLOSE_HPP
#define CASCADE_CORE_CONSOLE_CLOSE_HPP

namespace cascade::core {

// How long the handler for a closing console waits for the shutdown before it
// hands the process back to the system, which then ends it. Under the system's
// own 5000 ms.
inline constexpr unsigned kConsoleCloseWaitMs = 4500;

// Installs the handler, once; later calls do nothing and return true. False when
// the operating system refused it, or on a platform with no console events.
bool installConsoleCloseHandler();

// True once a console event has asked the session to end. The frame loop polls
// it beside the window's own close flag, every frame; it never resets.
bool consoleCloseRequested();

}  // namespace cascade::core

#endif  // CASCADE_CORE_CONSOLE_CLOSE_HPP
