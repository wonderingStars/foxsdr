// gaps_win.hpp - THE WIN32 SIDE OF THE GAPS (docs/multi-radio-records/slint-gaps.md): opening an address, finding the window, the file drop and the text clipboard. Kept apart from gaps_logic
// so the rules are tested without a window; nothing here is called from a test except through the hooks the host sets.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <string>

namespace foxsdr::slintgaps {

bool openUrl(const std::string& url);                          // the system's default handler (item 10); false when it could not be asked
void* findMainWindow();                                        // the process's visible top-level window (an HWND), nullptr when there is none yet
// Item 8: the window accepts files dropped on it; `onDrop` is called on the UI thread with the first dropped path. Replaces the window backend's own drop target. True when installed.
bool installFileDrop(void* window, std::function<void(const std::string&)> onDrop);
bool setClipboardText(const std::string& utf8);                // items 6 and 9
std::string clipboardText();
bool windowsHighContrast();                                    // item 11: Windows' own high-contrast setting is on                                   // "" when there is no text on the clipboard

}  // namespace foxsdr::slintgaps
