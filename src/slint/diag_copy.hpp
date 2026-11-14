// diag_copy.hpp - COPY DIAGNOSTICS, ONE FUNCTION (window-gaps-2, row 1458): the bundle a bug report attaches, built now, put on the clipboard and saved beside the reports as diagnostics.txt.
// SYSTEM > Diagnostics' "Copy diagnostics" key calls it, and so must the "FoxSDR did not close normally" dialog's key: two keys, one effect, so the two cannot drift.
// Nothing here knows a window: the caller passes the three facts it holds (the reports folder, whether the last run ended badly, the launch and crash counts).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace foxsdr::slintdiag {

struct DiagCopyInput {
    std::string reportsPath;          // the reports folder the bundle names ("" = the diagnostics log's own)
    bool lastRunUnclean = false;
    std::uint64_t launches = 0;       // fact.diag.launches
    std::uint64_t crashes = 0;        // fact.diag.crashes
    // The clipboard. Empty = the system's text clipboard (gaps_win setClipboardText); a bounded run and the tests put their own here so the desk's clipboard is never touched.
    std::function<bool(const std::string&)> clipboard;
    // Where diagnostics.txt goes. Empty = the diagnostics crash folder (core::diagCrashDir()).
    std::string saveDir;
};

struct DiagCopyResult {
    std::string status;               // the line under the key: "Copied to the clipboard, and saved as <path>"
    std::string bundle;               // the text copied
    std::string savedPath;            // "" when no file could be written
    bool copied = false;              // the clipboard took it
};

DiagCopyResult copyDiagnostics(const DiagCopyInput& in);

}  // namespace foxsdr::slintdiag
