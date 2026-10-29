// paste_files.hpp - a file copied in Explorer, pasted into a text box as its
// path.
//
// WHY THIS EXISTS (a tester's report, 2026-10-29, FoxSDR 0.99.101 on Windows
// 10): "Not possible to paste in a WAV-file in the IQ-file field." A file
// copied in Explorer (Ctrl+C on the file) is on the clipboard as a LIST OF
// FILES (the CF_HDROP format), not as text; the window library's clipboard read
// asks the clipboard for text only (glfwGetClipboardString: CF_UNICODETEXT),
// finds none, and Dear ImGui's Ctrl+V then pastes nothing - into the I/Q file's
// box or any other. Notepad behaves the same way, which is why a person who
// has just copied a recording and pressed Ctrl+V reports the box as broken
// rather than the clipboard.
//
// WHAT THIS DOES. installFilePaste() wraps the clipboard reader Dear ImGui's
// GLFW backend installed (Platform_GetClipboardTextFn): the backend's text is
// returned when there is any; when there is none and the clipboard holds a
// list of files, the FIRST file's path is returned instead, as UTF-8. So a
// copied file pastes as its path, wherever text can be pasted. Nothing else
// about the clipboard changes: text still wins, and a clipboard holding
// neither still pastes nothing. Windows only; on other platforms the wrapper
// adds only the scripted clipboard below.
//
// THE SCRIPTED CLIPBOARD (bounded --frames runs, gui/input_script.hpp's "paste"
// verb). A test cannot put something on the system clipboard without taking it
// from whoever is using the desk, and the suite runs eight tests at a time, so
// a scripted paste sets this text instead and taps Ctrl+V; the wrapper returns
// it before asking the backend. Only applyInputScript sets it, and only a
// bounded run has a script, so an interactive session never pastes from it.
//
// firstDroppedFile() is the pure part: a DROPFILES block (what CF_HDROP's
// memory holds) to the first path in it. Pinned in tests/test_iq_file_box.cpp
// with blocks the test writes itself; the clipboard read around it is a few
// Win32 calls no test can drive without the desk's clipboard, and is said so.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PASTE_FILES_HPP
#define CASCADE_GUI_PASTE_FILES_HPP

#include <cstddef>
#include <string>

namespace cascade::gui {

// The first path in a DROPFILES block of `bytes` bytes, as UTF-8, or "" when
// the block is not one (too short, its offset outside it, no path, a path that
// runs off the end). The block's layout is Windows': a 20-byte header (the
// offset of the list, a point, a flag, and fWide), then the list - NUL-separated
// paths ending in a double NUL - in UTF-16 when fWide is set, else in the
// system's narrow code page (converted on Windows, taken as it is elsewhere).
std::string firstDroppedFile(const unsigned char* block, std::size_t bytes);

// Wraps the clipboard reader of the current Dear ImGui context (call it once,
// after the platform backend is up). See the file comment.
void installFilePaste();

// The scripted clipboard: what the wrapper returns first while this is set.
// "" (the default) is not set.
void setScriptedClipboard(const std::string& text);

// The wrapper's answer, as Dear ImGui would get it: the scripted text, else the
// backend's text, else the first file on the clipboard; nullptr for nothing.
// Exposed so a test can ask the question the paste asks.
const char* clipboardTextForPaste();

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PASTE_FILES_HPP
