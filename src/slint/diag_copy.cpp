// diag_copy.cpp - see diag_copy.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "diag_copy.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/i18n.hpp"
#include "core/problem_report.hpp"
#include "core/utf8_text.hpp"
#include "gaps_win.hpp"

namespace foxsdr::slintdiag {

namespace cc = cascade::core;

DiagCopyResult copyDiagnostics(const DiagCopyInput& in) {
    DiagCopyResult r;
    cc::DiagBundleInput b;
    b.contextBlock = cc::diagContextBlock();
    b.logLines = cc::DiagLog::instance().ringSnapshot();
    b.logLinesTotal = cc::DiagLog::instance().linesWritten();
    b.logPath = cc::DiagLog::instance().filePath();
    b.crashDir = in.reportsPath;
    b.lastRunUnclean = in.lastRunUnclean;
    b.launches = in.launches;
    b.crashes = in.crashes;
    cc::ringCounters(b.ringDroppedSamples, b.sourceSamplesRead);
    r.bundle = cc::prepareDiagnosticsForReport(cc::buildDiagnosticsBundle(b));
    r.copied = in.clipboard ? in.clipboard(r.bundle) : foxsdr::slintgaps::setClipboardText(r.bundle);
    r.status = cascade::i18n::tr("Copied to the clipboard.");
    // saved beside the reports as well, because a clipboard does not survive the next copy; one small text file, on the caller's thread as the classic does under its watchdog pause
    const std::string dir = in.saveDir.empty() ? cc::diagCrashDir() : in.saveDir;
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        const std::string path = dir + "/diagnostics.txt";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
            out << r.bundle;
            out.close();
            r.savedPath = path;
            r.status = cc::formatText(cascade::i18n::tr("Copied to the clipboard, and saved as %s"), path.c_str());
        }
    }
    cc::diagLogf("diagnostics bundle produced (%zu bytes)", r.bundle.size());
    return r;
}

}  // namespace foxsdr::slintdiag
