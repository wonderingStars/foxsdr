// core/clipboard_files.hpp - the first file of a file list on the system clipboard, and the parse of the list itself.
// No ImGui, no Slint.
//
// WHY THIS IS IN core/ (docs 6.63). A WAV file copied in Explorer sits on the clipboard as a list of files (CF_HDROP), not as text, and the
// window libraries' clipboard reads know only text, so Ctrl+V into the I/Q-file box did nothing (a tester's report, 0.99.101; see
// core/iq_file_box.hpp). The classic window answered with gui/paste_files.* (its clipboard wrapper for Dear ImGui). The Slint window needs the
// same read without ImGui, so the two pure pieces moved here: the DROPFILES parse (a header test pins it) and the clipboard read.
// gui/paste_files.* keeps the ImGui half and re-exports firstDroppedFile.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_CLIPBOARD_FILES_HPP
#define CASCADE_CORE_CLIPBOARD_FILES_HPP

#include <cstddef>
#include <string>

namespace cascade::core {

// The first path in a CF_HDROP block (a DROPFILES header, then a list of paths, each ending in NUL, the list in a further NUL), as UTF-8. `bytes` is the
// block's size. Wide lists are UTF-16 (a lone surrogate becomes U+FFFD); narrow ones are the system code page. "" for a block that is not a list: too
// short, an offset past its end, or a path that runs off the end without its NUL.
std::string firstDroppedFile(const unsigned char* block, std::size_t bytes);

// The first file of a file list on the system clipboard, or "" for none (a text clipboard, an empty one, a non-Windows build). The clipboard is opened
// for the read only - never kept, never written.
std::string firstFileOnClipboard();

}  // namespace cascade::core

#endif  // CASCADE_CORE_CLIPBOARD_FILES_HPP
