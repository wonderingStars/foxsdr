// app_window_sdrplay.cpp - THE SDRPLAY API SERVICE CARD'S RESTART KEY and THE SDRPLAY DIAGNOSTIC'S DIALOG (stub campaign B18; ledger SDRPLAY-SERVICE, 0.99.55, and 0.99.51). AppWindow members, kept out of
// app_window.cpp (which is at the object-file limit, src/gui_pending/app_window_disk_state.hpp) and put back from master's text of 0.99.90 (drawSdrPlayServiceKey, finishSdrPlayServiceRestart,
// pollSdrPlayService, startSdrPlayProbe, drawSdrPlayProbeDialog), with the UI census's notes added (tests/test_sdrplay_service_app.cpp and tests/test_sdrplay_probe_app.cpp read them).
//
// THE SERVICE. An RSP is driven through sdrplay_api.dll, and the DLL talks to a Windows SERVICE that holds the radio's USB handle. When the service is stopped or wedged every Open answers "fail"
// in no time at all; since 0.99.55 the Source section says what Windows says the service is doing and, while the driver has trouble reaching it, offers RESTART SDRPLAY SERVICE - one Windows
// administrator prompt - and the sentence for what the restart came to (source/sdrplay_service.hpp). The trouble is the driver's own record in the process's API table (the engine counts it:
// Engine::pollSdrPlayTrouble); the card is host glue - it asks Windows, and Windows' elevation prompt belongs to this window - so it reads that table as the window's own process holds it.
//
// WHAT IS NOT PUT BACK, and why. After a restart that leaves the process with no session of its own to lose (ReopenInProcess), master listed the radios again AND reopened the saved SDRplay radio by
// matching its arguments (restoreKeep_.nativeArgs). Those arguments are the engine's and are not on the table; this lists the radios again (FOXAPI_OP_SCAN_DEVICES: the radio appears in the Source
// combo, which the engine's own saved-radio rule marks "(saved, not open)") and leaves the pick to the person. Stated in docs/engine-stage4.md 6.22, B18.
//
// THE DIAGNOSTIC. "Run SDRplay diagnostic" (SYSTEM > Diagnostics) steps an RSP through every sample rate, band, LNA state and antenna once, times every call to the SDRplay API and writes one text
// file (source/sdrplay_probe.hpp). The probe runs in a child process of its own, so a vendor call that hangs there cannot take this process's SDRplay session with it; this file is the key's dialog -
// which asks first, and asks a second time before the bias tee is switched on - the child's start and the poll that reads what it wrote.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <imgui.h>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/hang_watchdog.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "source/sdrplay_probe.hpp"
#include "source/sdrplay_service.hpp"
#include "source/sdrplay_source.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

// --- RESTART SDRPLAY SERVICE (0.99.55, source/sdrplay_service.hpp) ---------
//
// The field report behind this: an RSP2 Pro owner whose SDRplay API Service was stopped or wedged saw "Check that the SDRplay API service is running" on seven launches and never found the service.
// Windows can say which state it is in without administrator rights, and can restart it with one UAC prompt.

void AppWindow::pollSdrPlayService() {
    if (sdrPlayRestart_.takeFinished()) { finishSdrPlayServiceRestart(sdrPlayRestart_.outcome()); }
    const cascade::source::SdrPlayServiceTrouble trouble = cascade::source::sdrPlayServiceTrouble(cascade::source::processSdrPlayApi());
    if (trouble == sdrPlayTroubleSeen_) { return; }
    sdrPlayTroubleSeen_ = trouble;
    // (THE TWO TROUBLES THAT ARE A RECOVERY are counted on the change to them, and the count is the engine's: Engine::pollSdrPlayTrouble.)
    if (trouble == cascade::source::SdrPlayServiceTrouble::None) {
        // The service answered an Open: the radio is its own evidence, and a note about an earlier restart would only be stale.
        if (!sdrPlayRestart_.running()) { sdrPlayRestartNote_.clear(); }
        return;
    }
    // ONCE PER CHANGE OF TROUBLE, never per frame: what Windows says the service is doing, which decides whether the key has a service to name.
    sdrPlayService_ = cascade::source::querySdrPlayServiceAndNote();
}

void AppWindow::finishSdrPlayServiceRestart(const cascade::source::SdrPlayRestartOutcome& o) {
    using cascade::source::SdrPlayAfterRestart;
    using cascade::source::SdrPlayRestartPhase;
    using cascade::source::SdrPlayServiceState;
    if (o.phase == SdrPlayRestartPhase::Cancelled) {
        // Said, and nothing else done: the user chose not to.
        cascade::core::diagLogf("source: SDRplay API Service restart cancelled at the Windows prompt");
        sdrPlayRestartNote_ = tr("Restart cancelled.");
        return;
    }
    if (o.phase == SdrPlayRestartPhase::NotStarted) {
        cascade::core::diagWarnf("source: SDRplay API Service restart not started - error %lu", o.win32Error);
        sdrPlayRestartNote_ = cascade::core::formatText(tr("Windows would not run the restart (error %lu)."), o.win32Error);
        return;
    }
    sdrPlayService_ = o.after;
    cascade::source::noteSdrPlayServiceStatus(o.after);
    const char* how = (o.phase == SdrPlayRestartPhase::Done)       ? "ended"
                      : (o.phase == SdrPlayRestartPhase::Failed)   ? "failed"
                      : (o.phase == SdrPlayRestartPhase::TimedOut) ? "timed out"
                                                                   : "finished";
    cascade::core::diagLogf("source: SDRplay API Service restart %s (exit code %lu); now %s", how, o.exitCode, cascade::source::sdrPlayServiceSummary(o.after).c_str());

    const bool running = o.after.state == SdrPlayServiceState::Running;
    const SdrPlayAfterRestart what = cascade::source::sdrPlayApplyServiceRestart(cascade::source::processSdrPlayApi(), running);
    if (what == SdrPlayAfterRestart::ServiceNotRunning) {
        std::string note;
        if (o.phase == SdrPlayRestartPhase::TimedOut) {
            static_assert(cascade::source::kSdrPlayRestartLimit == std::chrono::milliseconds(45000), "the sentence below quotes 45 seconds");
            note = tr("The restart did not finish within 45 seconds.");
        } else if (o.phase == SdrPlayRestartPhase::Failed) {
            note = cascade::core::formatText(tr("The restart ended with exit code %lu."), o.exitCode);
        }
        const std::string advice = cascade::source::sdrPlayServiceAdvice(o.after);
        if (!advice.empty()) { note += (note.empty() ? "" : " ") + advice; }
        sdrPlayRestartNote_ = note;
        return;
    }
    if (what == SdrPlayAfterRestart::RestartFoxSdr) {
        // THE LOST SESSION STAYS LOST (see markSessionLost): a worker of ours may still be inside the vendor DLL for the orphaned device, and a new service does not bring it back. So nothing here
        // reopens.
        cascade::core::diagLogf("source: SDRplay API Service is running again; this session's SDRplay API use stays stopped until FoxSDR is restarted");
        sdrPlayRestartNote_ = tr("The SDRplay API Service was restarted and is running. Restart FoxSDR to use the SDRplay radio again.");
        return;
    }
    // THIS PROCESS NEVER HAD A SESSION, so nothing of ours is inside the DLL: the radios are listed again, here and now (see the head of this file for what is not put back).
    sdrPlayRestartNote_ = tr("The SDRplay API Service was restarted and is running.");
    cascade::core::diagLogf("source: SDRplay API Service is running again - rescanning in this session");
    uiSubmit(cascade::core::cmd::make(FOXAPI_OP_SCAN_DEVICES));
}

void AppWindow::drawSdrPlayServiceKey() {
#if defined(_WIN32)
    constexpr bool kWindows = true;
#else
    constexpr bool kWindows = false;
#endif
    const cascade::source::SdrPlayRestartPhase phase = sdrPlayRestart_.phase();
    if (!cascade::source::sdrPlayRestartKeyShown(kWindows, sdrPlayService_, sdrPlayTroubleSeen_, phase)) { return; }
    const bool busy = phase == cascade::source::SdrPlayRestartPhase::Running;
    ImGui::BeginDisabled(busy);
    const bool pressed = ImGui::Button(trId("RESTART SDRPLAY SERVICE"));
    ImGui::EndDisabled();
    if (cascade::gui::census::enabled()) {
        const ImVec2 k0 = ImGui::GetItemRectMin();
        const ImVec2 k1 = ImGui::GetItemRectMax();
        cascade::gui::census::note("sdrplay:service-key");
        cascade::gui::census::note(busy ? "sdrplay:service-key:disabled" : "sdrplay:service-key:enabled");
        cascade::gui::census::rect("sdrplay:service-key", k0.x, k0.y, k1.x, k1.y);
    }
    if (pressed && !busy) {
        sdrPlayRestartNote_.clear();
        cascade::core::diagLogf("source: RESTART SDRPLAY SERVICE pressed - restarting %s elevated", sdrPlayService_.serviceName.c_str());
        // The main window owns the UAC prompt, so it comes up in front.
        sdrPlayRestart_.start(sdrPlayService_.serviceName, ImGui::GetMainViewport()->PlatformHandleRaw);
    }
    if (busy) {
        cascade::gui::census::note("sdrplay:service-busy");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", tr("Restarting the SDRplay API Service - answer the Windows prompt."));
        ImGui::PopStyleColor();
    } else if (!sdrPlayRestartNote_.empty()) {
        cascade::gui::census::note("sdrplay:service-note:", sdrPlayRestartNote_);
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("%s", sdrPlayRestartNote_.c_str());
        ImGui::PopStyleColor();
    }
}

// --- THE SDRPLAY DIAGNOSTIC (0.99.51, source/sdrplay_probe.hpp) -------------

void AppWindow::startSdrPlayProbe() {
    // LEFT ON THIS THREAD, UNDER A PAUSE (0.99.64, docs/DIAGNOSTICS.md, "The window does no disk work"): one create_directories in the reports folder, on the press of a button that then starts a child
    // process for a minute or two. A slow reports folder is a freeze here, not a reported hang.
    cascade::core::WatchdogPause holdWatchdog(watchdog_);
    std::string dir = cascade::core::diagCrashDir();
    if (dir.empty()) { dir = cascade::core::diagLogDir(); }
    if (dir.empty()) {
        std::error_code tec;
        dir = std::filesystem::temp_directory_path(tec).string();
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char name[64];
    std::snprintf(name, sizeof(name), "sdrplay-diagnostic-%04d%02d%02d-%02d%02d%02d.txt", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
#if defined(_WIN32)
    sdrplayProbePath_ = dir + "\\" + name;
#else
    sdrplayProbePath_ = dir + "/" + name;
#endif
    auto child = std::make_unique<cascade::source::SdrPlayProbeChild>();
    std::string error;
    // THE ONLY WAY THE BIAS TEE REACHES THE PROBE: sdrplayProbeBiasOn_, which only the separate confirmation sets.
    if (!child->start(sdrplayProbePath_, sdrplayProbeBiasOn_, error)) {
        cascade::core::formatUtf8(sdrplayProbeStatus_, tr("SDRplay diagnostic could not start: %s"), error.c_str());
        cascade::core::diagWarnf("SDRplay diagnostic could not start - %s", error.c_str());
        return;
    }
    sdrplayProbeChild_ = std::move(child);
    // A NEW DIAGNOSTIC'S REPORT REPLACES THE LAST ONE'S, and the bug page's tick was for the other file.
    sdrplayProbeReport_.clear();
    problemReportAttachProbe_ = false;
    sdrplayProbeStatus_ = tr("SDRplay diagnostic running - this takes a minute or two...");
    cascade::core::diagLogf("SDRplay diagnostic started (bias tee test %s)", sdrplayProbeBiasOn_ ? "confirmed" : "off");
}

void AppWindow::drawSdrPlayProbeDialog() {
    // THE CHILD IS POLLED EVERY FRAME, whether or not any window is showing: a non-blocking look, never a wait.
    if (sdrplayProbeChild_ != nullptr && !sdrplayProbeChild_->running()) {
        const int code = sdrplayProbeChild_->exitCode();
        sdrplayProbeChild_.reset();
        std::string text;
        {
            // LEFT ON THIS THREAD, UNDER A PAUSE (0.99.64): one read of the small text file the child wrote, on the frame it is first seen to have exited - once per diagnostic the user asked for.
            cascade::core::WatchdogPause holdWatchdog(watchdog_);
            std::ifstream f(sdrplayProbePath_, std::ios::binary);
            if (f) { text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
        }
        if (!text.empty()) {
            sdrplayProbeReport_ = text;
            cascade::core::formatUtf8(sdrplayProbeStatus_, tr("SDRplay diagnostic finished: %s"), sdrplayProbePath_.c_str());
            cascade::core::diagLogf("SDRplay diagnostic finished - exit %d, %zu bytes", code, text.size());
        } else {
            const std::string why = "no file was written (exit " + std::to_string(code) + ")";
            cascade::core::formatUtf8(sdrplayProbeStatus_, tr("SDRplay diagnostic could not start: %s"), why.c_str());
            cascade::core::diagWarnf("SDRplay diagnostic wrote no file - exit %d", code);
        }
    }

    if (!sdrplayProbeConfirmOpen_) { return; }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin(trId("SDRplay diagnostic"), &sdrplayProbeConfirmOpen_, ImGuiWindowFlags_AlwaysAutoResize)) {
        cascade::gui::census::note("sdrplay:probe-dialog");
        // The rectangle of the item just drawn, for the census (named; a state that does not draw the item leaves it out).
        const auto noteRect = [](const char* name) {
            if (!cascade::gui::census::enabled()) { return; }
            const ImVec2 k0 = ImGui::GetItemRectMin();
            const ImVec2 k1 = ImGui::GetItemRectMax();
            cascade::gui::census::rect(name, k0.x, k0.y, k1.x, k1.y);
        };
        cascade::gui::census::note("sdrplay:probe-warning");
        ImGui::TextWrapped("%s", tr("This takes the SDRplay radio for a minute or two. Close SDRuno or any other program that is using it first."));
        ImGui::Spacing();
        if (sdrplayProbeBiasAsk_) {
            // THE SEPARATE CONFIRMATION. The bias tee puts DC power on the antenna socket; nothing switches it on until this is answered.
            cascade::gui::census::note("sdrplay:probe-bias-ask");
            ImGui::TextWrapped("%s", tr("The bias tee puts DC power on the antenna socket. Only switch it on if what is connected is meant to be powered that way, such as an active antenna. Switch it on "
                                        "for a moment during the test?"));
            if (ImGui::Button(trId("Yes, switch it on"))) {
                sdrplayProbeBiasOn_ = true;
                sdrplayProbeBiasAsk_ = false;
            }
            noteRect("sdrplay:probe-bias-yes");
            ImGui::SameLine();
            if (ImGui::Button(trId("Cancel"))) {
                sdrplayProbeBiasOn_ = false;
                sdrplayProbeBiasAsk_ = false;
            }
            noteRect("sdrplay:probe-cancel");
        } else {
            // Ticking only ASKS; the box shows ticked once the question was answered yes.
            bool want = sdrplayProbeBiasOn_;
            if (ImGui::Checkbox(trId("Also test the bias tee"), &want)) {
                if (want) {
                    sdrplayProbeBiasAsk_ = true;
                } else {
                    sdrplayProbeBiasOn_ = false;
                }
            }
            noteRect("sdrplay:probe-bias");
            if (sdrplayProbeBiasOn_) { cascade::gui::census::note("sdrplay:probe-bias:on"); }
            if (ImGui::Button(trId("Run the diagnostic"))) {
                sdrplayProbeConfirmOpen_ = false;
                startSdrPlayProbe();
            }
            noteRect("sdrplay:probe-run");
            ImGui::SameLine();
            if (ImGui::Button(trId("Cancel"))) { sdrplayProbeConfirmOpen_ = false; }
            noteRect("sdrplay:probe-cancel");
        }
    }
    ImGui::End();
}

}  // namespace cascade::gui
