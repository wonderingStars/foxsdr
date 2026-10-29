// status_text.hpp - the status sentences the panels letter (shell retirement R10, 2026-10-29).
//
// EngineStatusText was the biggest struct of "the vocabulary" in the shell header (engine_shell.hpp, deleted): the engine fills it in its pass (Engine::StatusText is this,
// Engine::publishStatusText) and the window keeps its own copy, filled from the table's fact.status.* (statusLines_). The shell is deleted; the struct is
// plain data and lives in core/, where the interface may include it. engine.hpp names it back into cascade::engine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace cascade::core {

// The status sentences the panels letter (Engine::StatusText is this) - see Engine::publishStatusText.
struct EngineStatusText {
    std::string sourceError;
    std::string gpsRefusal;
    std::string catalogError;
    std::string bandPlanError;
    std::string tuneMismatchNote;
    std::string transmitError;
    std::string soundCardMissing;
    std::string sdrPlayApiDetail;
    std::string sdrPlayAdvice;
    std::string recordNotice;
    std::string recordError;
    std::string presetNote;
    std::string pluginEnforceError;
    std::string restoreKeepLabel;
    // API 0.3 8.4: the Sinks section's watchdog note (STATUS_LINES key
    // audioHealth).
    std::string audioHealth;
    // API 0.3 8.6: the Bookmarks section's note (what the last import, export or group
    // removal did: bookmarkImportNote_) and its red line (bookmarkError_).
    std::string bookmarkNote;
    std::string bookmarkError;
    // API 0.3 8.8: what the last install or removal of a plugin did (installReport_, green: STATUS_LINES key
    // installReport, level 1) and why the last one failed (installError_, red, verbatim: installError, level 3).
    // The store window and the fitted-modules window show the same pair.
    std::string installReport;
    std::string installError;
    // API 0.3 8.9, what the plugin store window draws beside them: the catalogue status line ("N plugins in the
    // catalogue", catalogStatus_; STATUS_LINES key catalogStatus, level 1) and what the last ADD ALL run did
    // (addAllSummary_; key addAllSummary, level 1, or 3 when the run had failures: addAllFailed_, which is the
    // level's own source and is handed over with it).
    std::string catalogStatus;
    std::string addAllSummary;
    bool addAllFailed = false;
    // API 0.3 8.10: the radio's fault sentence WHOLE (STATUS_LINES key deviceFault, level 3): the pipeline's, or the
    // test stand-in's. FoxReceiverState::faultMessage is the same text cut at 127 bytes, and the zombie-source
    // sentence is 134 - a drawing that looks for the end of it ("restart FoxSDR") must read this, not that.
    std::string deviceFault;
};

}  // namespace cascade::core
