// desk_logic.hpp - THE NEW WINDOW'S HANDS ON THE DESKTOP, OFF WINDOWS (docs/multi-radio-records/linux-local.md section 11): open an address, open a folder or a file with the
// system's own handler, and copy text to or read it from the clipboard. On Windows the window does these through the shell and the Win32 clipboard (gaps_win.cpp and the
// publishers); here, on Linux and the other POSIX systems, the same duties go to the programs a desktop has: `xdg-open`, and `wl-copy`/`wl-paste`, `xclip` or `xsel`.
//
// Rules, each held by test_slint_host_desk:
//   - a program is found on PATH by its bare name, never run through a shell: the command is an ARGUMENT ARRAY, so nothing in an address or a path is ever read as shell text;
//   - an address is the Windows side's own rule: https only, a bounded length, no blank, control character, quote or angle bracket;
//   - a path that begins with '-' is made absolute first, so no opener reads it as an option;
//   - nothing happens when the program is not there: the call returns false and says why, and the caller reports it (never a silent no-op);
//   - every start goes through ONE runner that a test replaces, so no test starts a browser, a file manager or a clipboard tool.
// The pure rules (addressOk, findOnPath, the argument arrays) build and run on every platform; only the default runner is POSIX.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace foxsdr::slintdesk {

// What to start: the program (a full path found on PATH) and its arguments, the text to write on its standard input (the clipboard tools), whether to capture its standard
// output (the paste tools), and whether the caller waits for it (the clipboard tools) or lets it run on its own (the openers).
struct Command {
    std::vector<std::string> argv;   // argv[0] is the program's full path
    std::string input;
    bool capture = false;
    bool wait = false;
};

// The one start-a-program seam. Returns true when the program was started (and, for a waiting command, exited with 0); `out` is filled for a capturing one.
using Runner = std::function<bool(const Command&, std::string& out)>;
void setRunner(Runner r);                 // a test's stand-in; an empty function puts the real runner back
void setPathOverride(const std::string& path);   // a test's PATH (the real PATH is read when this is empty)
void clearPathOverride();

// ---- the pure rules ----
bool addressOk(const std::string& url);   // https://..., 9 to 511 bytes, no blank, control character, quote, backslash or angle bracket
std::string findOnPath(const std::string& name, const std::string& pathList);   // the full path of the first executable file called `name` in the ':'-separated list, "" when none; `name` must have no '/'
std::string absoluteForOpener(const std::string& path);   // an absolute form of a path (a leading '-' can never be read as an option)

enum class ClipboardTool { None, WlClipboard, Xclip, Xsel };
// The tool a session uses: wl-clipboard under a Wayland session (WAYLAND_DISPLAY non-empty), else xclip or xsel under X11 (DISPLAY non-empty); the first one found on `pathList`.
ClipboardTool chooseClipboardTool(const std::string& waylandDisplay, const std::string& display, const std::string& pathList, std::string* fullPath = nullptr);
Command copyCommand(ClipboardTool tool, const std::string& fullPath, const std::string& text);
Command pasteCommand(ClipboardTool tool, const std::string& fullPath);

// ---- the duties (each reads the environment for PATH and the display; each returns false and fills `why` when it did nothing) ----
bool openAddress(const std::string& url, std::string* why = nullptr);
bool openFolder(const std::string& dir, std::string* why = nullptr);    // a directory
bool openFile(const std::string& path, std::string* why = nullptr);     // a file in the system's own viewer or player
bool copyText(const std::string& utf8, std::string* why = nullptr);
std::string pasteText();   // "" when there is no tool or no text

}  // namespace foxsdr::slintdesk
