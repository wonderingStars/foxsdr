// file_pick.hpp - the system's "open a file" dialog, for the BROWSE key a PATH
// settings field draws (CASCADE_SETTING_PATH, 0.99.74).
//
// WHY THIS EXISTS (owner, 2026-10-12): "have you made it easy for the user to
// add the nrsc5 path to the sdr software". A plugin that drives a program the
// user installs themselves needs that program's location, and until now the
// only way to give it one was to type or paste a full path into a text box.
// The host now draws a BROWSE key beside such a field, and the key opens the
// operating system's own file dialog.
//
// WHAT IS HERE AND WHAT IS NOT. This file is the dialog and the decisions
// around it that need no graphics context: which folder it opens in, whether
// it offers programs first, how a Linux helper (zenity, kdialog) is asked, and
// which of those this machine has. The key itself, and what happens to a chosen
// path, are gui/plugin_settings_form.cpp's. Where the dialog is called from,
// and the watchdog bracket it must be called inside, are AppWindow::pickFile's.
//
// A DIALOG BLOCKS THE THREAD THAT OPENS IT. That is by design (it is modal to
// the application window, the owner passed in FilePickRequest), and it is why
// the caller holds the hang watchdog's user-paced pause around pickFile()
// (gui/shell_open.hpp: "a NATIVE MODAL DIALOG ... MUST take one") - a person
// choosing a file is not a frozen window.
//
// NOTHING HERE OPENS, READS OR CHECKS THE CHOSEN FILE. The dialog is asked for a
// file that exists (a path the person typed by hand is not held to that - the
// text box is still there), and what is returned is its path as UTF-8 text.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FILE_PICK_HPP
#define CASCADE_CORE_FILE_PICK_HPP

#include <string>
#include <utility>
#include <vector>

namespace cascade::core {

// What to ask the dialog for. Every string is UTF-8; the caller has already
// translated the words (this file knows nothing of languages).
struct FilePickRequest {
    std::string title;         // the dialog's title, "Choose a file"
    std::string startFrom;     // the field's current value: the dialog opens in
                               // THAT file's folder if the folder exists, else
                               // wherever the system would have opened it
    bool programs = false;     // offer "Programs" (*.exe) first, "All files" second
    std::string programsName;  // "Programs (*.exe)" - the filter's words
    bool lists = false;        // offer "Frequency lists" (*.xml;*.csv) first (0.99.80,
                               // the IMPORT window's Browse key); with `programs` too,
                               // programs come first, then lists, then all files
    std::string listsName;     // "Frequency lists (*.xml, *.csv)" - the filter's words
    std::string allFilesName;  // "All files (*.*)"
    void* owner = nullptr;     // the application window's native handle (an HWND
                               // on Windows) so the dialog is modal to it; may be
                               // null, and is ignored where there is no such thing
};

// True when `placeholder` ends in ".exe" (case does not matter): the plugin
// hinted that the field names a program, so the dialog offers programs first.
// The placeholder is the only hint a PATH field carries; it is what the author
// already writes to show the user what a good value looks like.
bool placeholderNamesProgram(const std::string& placeholder);

// The folder holding `value` when that folder exists, else "": where the dialog
// opens. "C:\tools\nrsc5.exe" gives "C:\tools" if it is a folder. A bare name
// ("nrsc5"), an empty value and a folder that is not there all give "". ONE
// stat of the disk, so it is called only from inside pickFile(), under the
// caller's watchdog pause, never per frame.
std::string pickStartFolder(const std::string& value);

// The path to hand FilePickRequest::startFrom for a box whose text is `typed` (0.99.80, the
// IMPORT window's Browse key): what was typed when it names a folder (one pair of quotes, as
// Explorer's "Copy as path" puts them, is taken off); a BARE name - or nothing - is taken to be in
// `folder`, where the program looks for such a name (the recordings folder), the empty box as
// `fallbackName`. So pickStartFolder() of the answer is `folder` even when no such file is there
// yet. Path arithmetic only: no disk is touched. "" when there is neither a path nor a folder.
std::string pickStartPath(const std::string& typed, const std::string& folder,
                          const std::string& fallbackName);

// The (name, pattern) pairs the dialog's file-type list is built from, in the
// order shown: the first is what is selected when it opens. A request with
// `programs` set lists {programsName, "*.exe"} first; one with `lists` set lists
// {listsName, "*.xml;*.csv"} next (after the programs, if both are set); the
// last is always {allFilesName, "*.*"}. An empty name falls back to its pattern
// so the list is never blank. (Windows turns these into the dialog's
// double-NUL-terminated filter string; the pairs are kept apart so a test can
// read them.)
std::vector<std::pair<std::string, std::string>> openFileFilters(const FilePickRequest& r);

// The argument vector (argv[0] first) that asks a Linux helper for one file, or
// an empty vector for a tool this does not know. NEVER a shell command line:
// each element is one argument, handed to exec as it is, so a title or a folder
// holding a quote, a space or a `;` is never reinterpreted.
//   "zenity":  zenity --file-selection --title=<title> [--filename=<folder>/]
//   "kdialog": kdialog --title <title> --getopenfilename [<folder>]
// `folder` is pickStartFolder()'s answer, "" for none. This is a pure function
// on every platform so it is tested everywhere; only the code that RUNS it is
// Linux's.
std::vector<std::string> pickerCommand(const std::string& tool, const std::string& title,
                                       const std::string& folder);

// True when this machine can show a file dialog: always on Windows; on Linux
// when zenity or kdialog is on PATH. When false the host draws the text box
// alone, with no BROWSE key - never a key that does nothing.
bool filePickAvailable();

// Shows the dialog and waits for the answer. True with `path` set (UTF-8) when a
// file was chosen; false when the person cancelled or the dialog could not be
// shown (`path` is empty either way). Blocks the calling thread until the
// dialog closes: call it only on a thread that may wait (the GUI thread, inside
// the watchdog's user-paced pause).
bool pickFile(const FilePickRequest& request, std::string& path);

}  // namespace cascade::core

#endif  // CASCADE_CORE_FILE_PICK_HPP
