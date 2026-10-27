// app_window_report.cpp - REQUEST A FEATURE and REPORT A BUG / DISLIKE: the two small pages under the status column's keys (core/feature_request.hpp, core/problem_report.hpp).
// AppWindow members, kept out of app_window.cpp because they are one subject (and because that file is at the object-file limit). Moved here in stub campaign B14, which also
// restored the rest of master's 0.99.69 pages (PROBLEM-REPORT: the recognised-report line, the attach-the-diagnostics rows and the preview, `upload local-only` said in words, and
// the no-email nudge on both pages) from master's text of 0.99.90 - drawProblemReportPage and drawFeatureRequestPage are unchanged to 0.99.95.
//
// WHAT THIS FILE ADDS TO MASTER'S TEXT, and nothing else: the UI census's notes (tests/test_problem_report_app.cpp reads them: the page, the kind, why SEND is dead, the payload
// as built, the signature-id and recovered lines of the attachment), and ONE behaviour - an attachment built while the reports folder was still being read ("still being read") is
// rebuilt, four times a second at most, until the answer is in, so that is not the text that goes (the evidence lines are only worth sending with their ids in them).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/diag_log.hpp"
#include "core/diag_history.hpp"
#include "core/feature_request.hpp"
#include "core/i18n.hpp"
#include "core/pasted_report.hpp"
#include "core/problem_report.hpp"
#include "core/utf8_text.hpp"
#include "core/version.hpp"
#include "gui/page_geometry.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// The rectangle of the item just drawn, for the UI census (a no-op unless FOXSDR_UI_CENSUS names a file): where a test aims its press.
void censusRect(const char* name) {
    if (!cascade::gui::census::enabled()) { return; }
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    cascade::gui::census::rect(name, a.x, a.y, b.x, b.y);
}

// The lines of the attachment the page has built that a report's reader leans on - the last run's state, the signature ids of the reports on this machine and the
// words of what the program recovered from - as the census sees them (a no-op unless FOXSDR_UI_CENSUS names a file): the test reads what the real window built.
void censusDiagnosticsLines(const std::string& text) {
    if (!cascade::gui::census::enabled()) { return; }
    for (const char* name : {"last-run-unclean", "recent-faults", "recovered"}) {
        const std::string key = std::string("\n") + name + ": ";
        const std::size_t at = text.find(key);
        if (at == std::string::npos) { continue; }
        std::size_t eol = text.find('\n', at + 1);
        if (eol == std::string::npos) { eol = text.size(); }
        cascade::gui::census::note("problem:diag-line:", text.substr(at + 1, eol - at - 1));
    }
}

}  // namespace

// --- REQUEST A FEATURE (see core/feature_request.hpp) -----------------------
//
// A SMALL PAGE, on purpose: a text box, an optional contact line, the one
// sentence naming exactly what is sent, a SEND key, and a status line. Opened
// by the STATUS column's own key (drawStatusColumn) and by nothing else -
// never restored at start-up, matching every other torn-off page's rule.
void AppWindow::drawFeatureRequestPage() {
    if (!featureRequestOpen_) { return; }
    constexpr float kW = 480.0f;
    // 420 -> 460 (2026-09-28): the no-email warning line under the contact
    // field is a new row above the box-gives-way boundary.
    constexpr float kH = 460.0f;
    float px = 0.0f;
    float py = 0.0f;
    float pw = kW;
    float ph = kH;
    {
        // OPENS INSIDE THE MAIN WINDOW, centred - the same arithmetic the
        // plugin store and the demod scope use, and for the same reason: a
        // key that opens nothing visible on a maximised single monitor has
        // told the person who pressed it nothing at all.
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        cascade::gui::pageOpenInside(mv->Pos.x, mv->Pos.y, mv->Size.x, mv->Size.y, kW, kH, px,
                                     py, pw, ph);
    }
    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_FirstUseEver);
    if (!beginPage("Feature request###featurerequestwindow", tr("FEATURE REQUEST"),
                   &featureRequestOpen_, 0, pw, ph)) {
        endPage();
        return;
    }

    cascade::gui::census::note("feature:page");
    const std::uint64_t nowEpoch = static_cast<std::uint64_t>(std::time(nullptr));
    const cascade::core::FeatureRequestState state = featureRequestSender_.state();
    const bool sending = (state == cascade::core::FeatureRequestState::Sending);

    // --- the text --------------------------------------------------------
    // Wrapped at the page's edge: the English question fits one line, a
    // translation need not.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(tr("What would you like FoxSDR to do?"));
    ImGui::PopTextWrapPos();
    // THE BOX GIVES WAY TO WHAT IS UNDER IT (gui::boxGivingWay). On this
    // 420-pixel page a translation's longer sentence and the reason wrapped
    // under SEND pushed that reason below the fold - a dead key whose reason
    // had to be scrolled to. The box is shortened by exactly the overflow,
    // measured on the previous frame; the English page fits, and its box is
    // the 160 it always was.
    const float boxH = cascade::gui::boxGivingWay(160.0f, 80.0f, ImGui::GetContentRegionAvail().y,
                                                  featureRequestBelowBoxH_);
    float belowTop = 0.0f;
    {
        // NO FIXED BUFFER: the widget edits the string itself (imgui_stdlib
        // grows it as the person types), so nothing they write is cut by the
        // widget - the ceiling is 100000 characters, and reaching it is said
        // in words by validation, never done silently.
        ImGui::BeginDisabled(sending);
        ImGui::InputTextMultiline("##featurerequesttext", &featureRequestText_,
                                  ImVec2(-1.0f, boxH));
        ImGui::EndDisabled();
        belowTop = ImGui::GetItemRectMax().y;
        censusRect("feature:text");
    }
    // THE SAME COUNT validateFeatureRequestText() JUDGES THE BOUNDARY
    // AGAINST - after trimming and after dropping control characters the way
    // the server does before IT counts them - so the number on screen never
    // disagrees with the number that decides whether SEND is enabled.
    const std::size_t chars = cascade::core::featureRequestTextCharCount(featureRequestText_);
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkFaint));
    ImGui::Text(tr("%zu / %zu characters"), chars, cascade::core::kFeatureRequestMaxChars);
    ImGui::PopStyleColor();

    // --- the optional contact line ----------------------------------------
    ImGui::TextUnformatted(tr("Email or callsign (optional)"));
    {
        char buf[cascade::core::kFeatureRequestContactBufferBytes];
        cascade::core::formatUtf8(buf, sizeof(buf), "%s", featureRequestContact_.c_str());
        ImGui::BeginDisabled(sending);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##featurerequestcontact", buf, sizeof(buf))) {
            featureRequestContact_ = buf;
        }
        ImGui::EndDisabled();
        censusRect("feature:contact");
    }
    // A callsign alone cannot be written back to - said plainly, but never
    // blocking SEND: the field stays entirely optional.
    if (!cascade::core::featureRequestContactHasEmailAddress(featureRequestContact_)) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
        ImGui::TextWrapped(
            "%s", tr("Without an email address we cannot reply or tell you when this is added."));
        ImGui::PopStyleColor();
        cascade::gui::census::note("feature:no-email-note");
    } else {
        cascade::gui::census::note("feature:contact-has-email");
    }

    ImGui::Separator();

    // --- THE SENTENCE, exactly what leaves the machine -----------------------
    //
    // Built from what is actually about to be sent, never a static string
    // that could drift from the payload beside it: the contact line only
    // appears here when there is one to send, exactly as it only appears in
    // the JSON when featureRequestJson() trims it to something non-empty.
    {
        // TWO WHOLE SENTENCES, not one assembled from fragments, so a
        // translation can order its clauses the way its language does.
        const bool haveContact =
            !cascade::core::validateFeatureRequestContact(featureRequestContact_).empty()
                ? false  // an over-length contact is not going to be sent at all
                : cascade::core::featureRequestContactCharCount(featureRequestContact_) > 0;
        const char* sentence =
            haveContact
                ? tr("Sends your message above, the contact line below it, the FoxSDR version, "
                     "and whether this is running on Windows, Linux or Android, x64 or arm64. "
                     "Nothing else - no identifier, no log, no settings, no frequency.")
                : tr("Sends your message above, the FoxSDR version, and whether this is "
                     "running on Windows, Linux or Android, x64 or arm64. Nothing else - no "
                     "identifier, no log, no settings, no frequency.");
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
        ImGui::TextWrapped("%s", sentence);
        ImGui::PopStyleColor();
    }

    // --- whether SEND is allowed, and why not when it is not ------------------
    const std::string textError = cascade::core::validateFeatureRequestText(featureRequestText_);
    const std::string contactError =
        cascade::core::validateFeatureRequestContact(featureRequestContact_);
    const std::uint64_t blockedUntil = featureRequestSender_.blockedUntil();
    const bool cooling = !sending && blockedUntil > nowEpoch;

    // DRAWN ONLY - disabledReason is tested for emptiness and printed, never
    // sent or logged - so it is translated as it is filled.
    std::string disabledReason;
    const char* reasonWord = "";  // for the census only: why, in English, whatever the language
    if (sending) {
        disabledReason = tr("sending");
        reasonWord = "sending";
    } else if (!textError.empty()) {
        disabledReason = tr(textError.c_str());
        reasonWord = "text";
    } else if (!contactError.empty()) {
        disabledReason = tr(contactError.c_str());
        reasonWord = "contact";
    } else if (cooling) {
        const std::uint64_t left = blockedUntil - nowEpoch;
        cascade::core::formatUtf8(disabledReason, tr("wait %llus before sending another"),
                      static_cast<unsigned long long>(left));
        reasonWord = "cooling";
    }
    const bool canSend = disabledReason.empty();

    ImGui::BeginDisabled(!canSend);
    const bool pressed = ImGui::Button(trId("SEND"), ImVec2(120.0f, 0.0f));
    ImGui::EndDisabled();
    censusRect("feature:send");
    if (canSend) {
        cascade::gui::census::note("feature:send:enabled");
    } else {
        cascade::gui::census::note("feature:send:disabled:", reasonWord);
    }
    if (!disabledReason.empty()) {
        // BESIDE THE SEND KEY WHEN IT FITS THE PAGE, under it and wrapped when
        // it does not. The English reason the page opens with sits beside the
        // key; its translations ran off the page's right edge in five
        // languages of six, and the box above makes room for the wrapped
        // line (see boxH). The problem-report
        // page below always wraps its reason under the key, because there
        // the kind's own sentence is longer than any of these.
        //
        // AND BESIDE IT AT A SMALLER SIZE before it is moved under the key
        // (text_fit.hpp, sameLineFittedText): "(write at least 10
        // characters - 0 so far)" in Russian, Ukrainian, Greek and Japanese
        // was a few pixels too long and dropped under SEND, where it read as a
        // line of the page rather than as the key's reason. Only a reason
        // that does not fit even at seven tenths goes below, wrapped.
        std::string shown = "(" + disabledReason + ")";
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkFaint));
        if (!cascade::gui::sameLineFittedText(shown.c_str())) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(shown.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::PopStyleColor();
    }

    if (pressed && canSend) {
        cascade::core::FeatureRequestPayload payload;
        payload.text = featureRequestText_;
        payload.contact = featureRequestContact_;
        payload.version = cascade::versionString();
        payload.platform = cascade::core::featureRequestPlatform();
        payload.arch = cascade::core::featureRequestArch();
        // The count LOGGED is the same one the SEND key's boundary and the
        // on-page counter already use - the server's own count, after
        // trimming and after dropping control characters - never a raw
        // std::string::size() that could differ from it by a stray blank
        // line or an embedded control character.
        featureRequestSentChars_ =
            cascade::core::featureRequestTextCharCount(featureRequestText_);
        featureRequestSender_.send(cascade::core::featureRequestEndpoint(), payload, nowEpoch);
    }

    // --- the status line -------------------------------------------------
    switch (state) {
        case cascade::core::FeatureRequestState::Idle:
            break;
        case cascade::core::FeatureRequestState::Sending:
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
            ImGui::TextUnformatted(tr("Sending..."));
            ImGui::PopStyleColor();
            break;
        case cascade::core::FeatureRequestState::Sent:
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
            ImGui::TextUnformatted(tr("Thank you - your request was sent."));
            ImGui::PopStyleColor();
            break;
        case cascade::core::FeatureRequestState::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAlarmHot));
            ImGui::TextWrapped("%s", tr(featureRequestSender_.failureMessage().c_str()));
            ImGui::PopStyleColor();
            break;
        case cascade::core::FeatureRequestState::CoolingDown: {
            std::string msg = featureRequestSender_.failureMessage();
            if (msg.empty()) {
                msg = FOX_TR_NOOP("The server asked us to wait before sending another request.");
            }
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
            ImGui::TextWrapped("%s", tr(msg.c_str()));
            ImGui::PopStyleColor();
            break;
        }
    }
    // Everything under the box, for next frame's boxGivingWay: from the box's
    // foot to the foot of the last line drawn.
    featureRequestBelowBoxH_ =
        ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y - belowTop;

    endPage();

    // THE LOG LINE, exactly once per send, on the frame the transition out of
    // Sending is first observed - never the text, never the contact, exactly
    // as PRIVACY.md and feature_request.hpp both promise.
    if (featureRequestLastLoggedState_ == cascade::core::FeatureRequestState::Sending &&
        state != cascade::core::FeatureRequestState::Sending) {
        if (state == cascade::core::FeatureRequestState::Sent) {
            cascade::core::diagLogf("feature request: sent, %zu characters, HTTP %d",
                                    featureRequestSentChars_, featureRequestSender_.lastStatus());
        } else {
            cascade::core::diagLogf("feature request: failed, %zu characters, HTTP %d - %s",
                                    featureRequestSentChars_, featureRequestSender_.lastStatus(),
                                    featureRequestSender_.failureMessage().c_str());
        }
    }
    // Cleared on the ONE frame the SEND that just succeeded is first seen -
    // never before, and never again while the thank-you is still showing (see
    // featureRequestClearsTextNow for what "whenever it is Sent" did to a
    // second request). A send still in flight, or one that failed, leaves the
    // words on screen so the person is not asked to retype them. The contact
    // line is deliberately NOT cleared: the contract calls for keeping it, so
    // filing a second request does not mean typing an email address twice.
    if (cascade::core::featureRequestClearsTextNow(featureRequestLastLoggedState_, state)) {
        featureRequestText_.clear();
    }
    featureRequestLastLoggedState_ = state;
}

// --- REPORT A BUG / DISLIKE (see core/problem_report.hpp) -------------------
//
// The feature-request page's twin, with one thing more at the top: the choice
// of what this is. Two radio buttons and NEITHER chosen when the page opens -
// a default would file every hurried report as whichever came first, and the
// site triages the two kinds apart. Everything below the choice is the same
// page as drawFeatureRequestPage(): the same bounds and counter, the same
// sentence naming exactly what is sent, the same SEND rules and status line,
// the same one log line that never carries the words.
void AppWindow::drawProblemReportPage() {
    if (!problemReportOpen_) { return; }
    constexpr float kW = 480.0f;
    // TALLER THAN THE FEATURE PAGE by the kind row, its question and the
    // wrapped reason line under SEND. At 470 (rendered check, 2026-09-23) the
    // SEND key was cut by the page's foot and the reason it was disabled sat
    // below the fold, so the page opened looking like a key that did nothing.
    // 540 -> 660 (2026-09-28): the no-email warning line, the "Attach the
    // diagnostics log" checkbox and its "Show what will be sent" toggle are
    // all new rows above the box-gives-way boundary; the text box already
    // shrinks to make room (gui::boxGivingWay), but a page that opens without
    // ever needing to shrink is the page it always was.
    constexpr float kH = 660.0f;
    float px = 0.0f;
    float py = 0.0f;
    float pw = kW;
    float ph = kH;
    {
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        cascade::gui::pageOpenInside(mv->Pos.x, mv->Pos.y, mv->Size.x, mv->Size.y, kW, kH, px,
                                     py, pw, ph);
    }
    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_FirstUseEver);
    if (!beginPage("Report a bug or dislike###problemreportwindow", tr("REPORT A BUG / DISLIKE"),
                   &problemReportOpen_, 0, pw, ph)) {
        endPage();
        return;
    }

    cascade::gui::census::note("problem:page");
    const std::uint64_t nowEpoch = static_cast<std::uint64_t>(std::time(nullptr));
    const cascade::core::FeatureRequestState state = problemReportSender_.state();
    const bool sending = (state == cascade::core::FeatureRequestState::Sending);

    // --- what this is ------------------------------------------------------
    ImGui::TextUnformatted(tr("What are you reporting?"));
    ImGui::BeginDisabled(sending);
    if (ImGui::RadioButton(trId("Something is broken (bug)"),
                           problemReportKind_ == cascade::core::kProblemKindBug)) {
        problemReportKind_ = cascade::core::kProblemKindBug;
        // The checkbox follows the kind's own default every time it is
        // (re)chosen - ticked for a bug - but the person can still change it
        // afterwards; this only decides what it shows the moment the kind
        // changes.
        problemReportAttachDiag_ =
            cascade::core::problemReportDefaultAttachDiagnostics(problemReportKind_);
    }
    censusRect("problem:kind:bug");
    // Beside the first choice when both fit the page, under it when a
    // translation makes the pair too wide (gui/text_fit.hpp).
    cascade::gui::sameLineIfFits(cascade::gui::checkWidth(trId("Something I dislike")));
    if (ImGui::RadioButton(trId("Something I dislike"),
                           problemReportKind_ == cascade::core::kProblemKindDislike)) {
        problemReportKind_ = cascade::core::kProblemKindDislike;
        problemReportAttachDiag_ =
            cascade::core::problemReportDefaultAttachDiagnostics(problemReportKind_);
    }
    censusRect("problem:kind:dislike");
    ImGui::EndDisabled();
    cascade::gui::census::note("problem:kind-is:", problemReportKind_.empty() ? "none" : problemReportKind_);

    // --- the text --------------------------------------------------------
    // Wrapped at the page's edge: "Was ist schiefgegangen, und was haben Sie
    // in dem Moment getan?" was cut at the page's edge in German.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(problemReportKind_ == cascade::core::kProblemKindDislike
                               ? tr("What do you dislike, and what would you rather it did?")
                               : tr("What went wrong, and what were you doing when it did?"));
    ImGui::PopTextWrapPos();
    // The box gives way to what is under it, as on the feature page: the
    // translated sentence and the reason under SEND ran below this page's
    // foot in Portuguese and French.
    const float boxH = cascade::gui::boxGivingWay(160.0f, 80.0f, ImGui::GetContentRegionAvail().y,
                                                  problemReportBelowBoxH_);
    float belowTop = 0.0f;
    {
        // No fixed buffer, as in the feature-request box above.
        ImGui::BeginDisabled(sending);
        ImGui::InputTextMultiline("##problemreporttext", &problemReportText_,
                                  ImVec2(-1.0f, boxH));
        ImGui::EndDisabled();
        belowTop = ImGui::GetItemRectMax().y;
        censusRect("problem:text");
    }
    const std::size_t chars = cascade::core::featureRequestTextCharCount(problemReportText_);
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkFaint));
    ImGui::Text(tr("%zu / %zu characters"), chars, cascade::core::kFeatureRequestMaxChars);
    ImGui::PopStyleColor();

    // --- a report pasted into the box, recognised (0.99.69) -----------------
    //
    // A tester pasted a whole sentinel report here by hand, because the diagnostics
    // bundle said `upload local-only` and nothing said what that meant. The text is
    // already going with the message; recognising it lets it travel as a structured
    // attachment (core/pasted_report.hpp), so the site can file it as the report it
    // is. One line, drawn BEFORE SEND, so the person knows it was understood.
    if (!problemReportPastedValid_ || problemReportPastedFor_ != problemReportText_) {
        problemReportPasted_ = cascade::core::detectPastedReport(problemReportText_);
        problemReportPastedFor_ = problemReportText_;
        problemReportPastedValid_ = true;
    }
    if (problemReportPasted_) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
        ImGui::TextWrapped(tr("A report was recognised and attached as %s"),
                           problemReportPasted_->reportClass.c_str());
        ImGui::PopStyleColor();
        cascade::gui::census::note("problem:recognised:", problemReportPasted_->reportClass);
    }

    // --- the optional contact line ----------------------------------------
    ImGui::TextUnformatted(tr("Email or callsign (optional)"));
    {
        char buf[cascade::core::kFeatureRequestContactBufferBytes];
        cascade::core::formatUtf8(buf, sizeof(buf), "%s", problemReportContact_.c_str());
        ImGui::BeginDisabled(sending);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##problemreportcontact", buf, sizeof(buf))) {
            problemReportContact_ = buf;
        }
        ImGui::EndDisabled();
        censusRect("problem:contact");
    }
    // As on the feature-request page: said plainly, never blocking SEND.
    if (!cascade::core::problemReportContactHasEmailAddress(problemReportContact_)) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
        ImGui::TextWrapped(
            "%s", tr("Without an email address we cannot reply or tell you when this is fixed "
                     "or ready."));
        ImGui::PopStyleColor();
        cascade::gui::census::note("problem:no-email-note");
    } else {
        cascade::gui::census::note("problem:contact-has-email");
    }

    // --- "Attach the diagnostics log" and its preview -----------------------
    //
    // Ticked by default for a bug, unticked for a dislike (set where the kind
    // is chosen, above) - the owner asked for this because users believed a
    // report already carried their log, and it never did. Now it can, but
    // only when this box is left ticked at the moment SEND is pressed: the
    // page's sentence below says so truthfully either way, and "Show what
    // will be sent" lets a person read the exact text before deciding.
    std::string preparedDiag;
    ImGui::BeginDisabled(sending);
    ImGui::Checkbox(trId("Attach the diagnostics log"), &problemReportAttachDiag_);
    ImGui::EndDisabled();
    cascade::gui::census::note("problem:attach");
    censusRect("problem:attach");
    cascade::gui::census::note(problemReportAttachDiag_ ? "problem:attach:on" : "problem:attach:off");
    if (problemReportAttachDiag_) {
        // Rebuilt only when the log has grown or a second has passed - see
        // the cache members' own comment - never on every frame this box is
        // left ticked.
        const std::uint64_t linesNow = cascade::core::DiagLog::instance().linesWritten();
        // AND SOON AFTER THE REPORTS FOLDER HAS BEEN READ (stub campaign B14): the first build of the attachment is made while the worker that lists the reports
        // on this machine is still at it, and says "still being read" in their place. That is not the text to send - the signature ids are what a triager reads
        // it for - so a cache built that way is rebuilt, four times a second at most, until the answer is in.
        const double nowTime = ImGui::GetTime();
        const bool retryPending = problemReportDiagCachePending_ && nowTime >= problemReportDiagRetryAt_;
        if (retryPending || cascade::core::problemReportDiagCacheStale(problemReportDiagCacheValid_,
                                                        problemReportDiagCacheLines_, linesNow,
                                                        problemReportDiagCacheEpoch_, nowEpoch)) {
            problemReportDiagCache_ =
                cascade::core::prepareDiagnosticsForReport(currentDiagnosticsBundle());
            problemReportDiagCacheLines_ = linesNow;
            problemReportDiagCacheEpoch_ = nowEpoch;
            problemReportDiagCacheValid_ = true;
            problemReportDiagCachePending_ =
                problemReportDiagCache_.find("(still being read") != std::string::npos;
            problemReportDiagRetryAt_ = nowTime + 0.25;
        }
        censusDiagnosticsLines(problemReportDiagCache_);
        preparedDiag = problemReportDiagCache_;
        // THE SDRPLAY DIAGNOSTIC, WHEN ONE HAS BEEN RUN THIS SESSION (0.99.50):
        // appended to the same attachment, so it travels in the one optional
        // field the site already accepts, is shown by the same preview, and is
        // held to the same 64 KB - the log gives up its oldest lines first.
        if (!sdrplayProbeReport_.empty()) {
            ImGui::BeginDisabled(sending);
            ImGui::Checkbox(trId("Attach the SDRplay diagnostic too"), &problemReportAttachProbe_);
            ImGui::EndDisabled();
            if (problemReportAttachProbe_) {
                preparedDiag = cascade::core::appendProbeToDiagnosticsForReport(
                    preparedDiag, sdrplayProbeReport_);
            }
        }
        ImGui::BeginDisabled(sending);
        ImGui::Checkbox(trId("Show what will be sent"), &problemReportShowDiag_);
        ImGui::EndDisabled();
        censusRect("problem:show");
        if (problemReportShowDiag_) {
            ImGui::InputTextMultiline("##problemreportdiagpreview", &preparedDiag,
                                      ImVec2(-1.0f, 140.0f), ImGuiInputTextFlags_ReadOnly);
            cascade::gui::census::note("problem:preview");
            // WHAT `upload local-only` IN THAT TEXT MEANS (0.99.69), in a sentence the
            // person can read in their own language and with the folder the file is
            // in. The bundle's own list says it once per reason in English; this is the
            // newest such report's reason, from the same function.
            const cascade::core::DiagHistory hist = diagHistoryForBundle(false);
            std::string why;
            for (const cascade::core::ReportSummary& r : hist.reports.newest) {
                why = cascade::core::localOnlyReason(r);
                if (!why.empty()) { break; }
            }
            if (!why.empty()) {
                cascade::gui::census::note("problem:local-only");
                const std::string dir = cascade::core::diagCrashDir();
                std::string where;
                cascade::core::formatUtf8(where, tr("The file is in the reports folder: %s"),
                                          dir.empty() ? tr("(unavailable)") : dir.c_str());
                if (!dir.empty()) { cascade::gui::census::note("problem:local-only:folder"); }
                ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
                ImGui::TextWrapped("%s %s", tr(why.c_str()), where.c_str());
                ImGui::PopStyleColor();
            }
        }
    } else {
        problemReportShowDiag_ = false;
    }

    ImGui::Separator();

    // --- THE SENTENCE, exactly what leaves the machine ---------------------
    {
        // Four whole sentences: with or without the contact line, as on the
        // feature-request page, crossed with whether the log is attached.
        const bool haveContact =
            cascade::core::validateProblemReportContact(problemReportContact_).empty() &&
            cascade::core::featureRequestContactCharCount(problemReportContact_) > 0;
        const bool attach = problemReportAttachDiag_;
        const char* sentence = nullptr;
        if (attach && haveContact) {
            sentence =
                tr("Sends whether this is a bug or a dislike, your message above, the contact "
                   "line below it, the FoxSDR version, whether this is running on Windows, "
                   "Linux or Android, x64 or arm64, and the diagnostics log attached above - "
                   "untick \"Attach the diagnostics log\" to leave it out, or use \"Show what "
                   "will be sent\" to read it first. No identifier, no crash report.");
        } else if (attach) {
            sentence =
                tr("Sends whether this is a bug or a dislike, your message above, the FoxSDR "
                   "version, whether this is running on Windows, Linux or Android, x64 or "
                   "arm64, and the diagnostics log attached above - untick \"Attach the "
                   "diagnostics log\" to leave it out, or use \"Show what will be sent\" to "
                   "read it first. No identifier, no crash report.");
        } else if (haveContact) {
            sentence =
                tr("Sends whether this is a bug or a dislike, your message above, the contact "
                   "line below it, the FoxSDR version, and whether this is running on "
                   "Windows, Linux or Android, x64 or arm64. Nothing else - no identifier, no "
                   "log, no crash report, no settings, no frequency.");
        } else {
            sentence =
                tr("Sends whether this is a bug or a dislike, your message above, the FoxSDR "
                   "version, and whether this is running on Windows, Linux or Android, x64 "
                   "or arm64. Nothing else - no identifier, no log, no crash report, no "
                   "settings, no frequency.");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
        ImGui::TextWrapped("%s", sentence);
        ImGui::PopStyleColor();
    }

    // --- whether SEND is allowed, and why not when it is not ----------------
    const std::string kindError = cascade::core::validateProblemReportKind(problemReportKind_);
    const std::string textError = cascade::core::validateProblemReportText(problemReportText_);
    const std::string contactError =
        cascade::core::validateProblemReportContact(problemReportContact_);
    const std::uint64_t blockedUntil = problemReportSender_.blockedUntil();
    const bool cooling = !sending && blockedUntil > nowEpoch;

    // Drawn only, as on the feature-request page: translated as it is filled.
    std::string disabledReason;
    const char* reasonWord = "";  // for the census only: why, in English, whatever the language
    if (sending) {
        disabledReason = tr("sending");
        reasonWord = "sending";
    } else if (!kindError.empty()) {
        disabledReason = tr(kindError.c_str());
        reasonWord = "kind";
    } else if (!textError.empty()) {
        disabledReason = tr(textError.c_str());
        reasonWord = "text";
    } else if (!contactError.empty()) {
        disabledReason = tr(contactError.c_str());
        reasonWord = "contact";
    } else if (cooling) {
        const std::uint64_t left = blockedUntil - nowEpoch;
        cascade::core::formatUtf8(disabledReason, tr("wait %llus before sending another"),
                      static_cast<unsigned long long>(left));
        reasonWord = "cooling";
    }
    const bool canSend = disabledReason.empty();

    ImGui::BeginDisabled(!canSend);
    const bool pressed = ImGui::Button(trId("SEND"), ImVec2(120.0f, 0.0f));
    ImGui::EndDisabled();
    censusRect("problem:send");
    if (canSend) {
        cascade::gui::census::note("problem:send:enabled");
    } else {
        cascade::gui::census::note("problem:send:disabled:", reasonWord);
    }
    if (!disabledReason.empty()) {
        // Wrapped rather than on the SEND key's line: the kind's sentence is
        // longer than the feature page's reasons, and an unwrapped one ran
        // off the page's right edge.
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkFaint));
        ImGui::TextWrapped("(%s)", disabledReason.c_str());
        ImGui::PopStyleColor();
    }

    if (pressed && canSend) {
        cascade::core::ProblemReportPayload payload;
        payload.kind = problemReportKind_;
        payload.text = problemReportText_;
        payload.contact = problemReportContact_;
        payload.version = cascade::versionString();
        payload.platform = cascade::core::featureRequestPlatform();
        payload.arch = cascade::core::featureRequestArch();
        if (problemReportAttachDiag_) {
            // `preparedDiag` was already built earlier this same frame (the
            // "Attach the diagnostics log" section above) - the exact text
            // "Show what will be sent" would have shown, sent byte for byte.
            payload.diagnostics = preparedDiag;
        }
        // The report the line above the contact box said was recognised: the
        // same words as the message, sent once more with their class (see
        // core/problem_report.hpp, `attachments`). Re-read from the text as it is
        // NOW, not from the cache, so what is sent is what the box holds.
        if (const auto pasted = cascade::core::detectPastedReport(problemReportText_)) {
            payload.attachments.push_back(*pasted);
        }
        problemReportSentChars_ = cascade::core::featureRequestTextCharCount(problemReportText_);
        problemReportSentKind_ = problemReportKind_;
        if (cascade::gui::census::enabled()) {
            // The payload as built, for the test: that it was built, of which kind, and whether the log and a recognised report rode with it.
            cascade::gui::census::note("problem:sent");
            cascade::gui::census::note("problem:sent:kind:", payload.kind);
            if (!payload.diagnostics.empty()) { cascade::gui::census::note("problem:sent:with-diagnostics"); }
            for (const cascade::core::PastedReport& a : payload.attachments) {
                cascade::gui::census::note("problem:sent:attachment:", a.reportClass);
            }
        }
        problemReportSender_.send(cascade::core::problemReportEndpoint(), payload, nowEpoch);
    }

    // --- the status line -------------------------------------------------
    switch (state) {
        case cascade::core::FeatureRequestState::Idle:
            break;
        case cascade::core::FeatureRequestState::Sending:
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
            ImGui::TextUnformatted(tr("Sending..."));
            ImGui::PopStyleColor();
            break;
        case cascade::core::FeatureRequestState::Sent:
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
            ImGui::TextUnformatted(tr("Thank you - your report was sent."));
            ImGui::PopStyleColor();
            break;
        case cascade::core::FeatureRequestState::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAlarmHot));
            ImGui::TextWrapped("%s", tr(problemReportSender_.failureMessage().c_str()));
            ImGui::PopStyleColor();
            break;
        case cascade::core::FeatureRequestState::CoolingDown: {
            std::string msg = problemReportSender_.failureMessage();
            if (msg.empty()) {
                msg = FOX_TR_NOOP("The server asked us to wait before sending another report.");
            }
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
            ImGui::TextWrapped("%s", tr(msg.c_str()));
            ImGui::PopStyleColor();
            break;
        }
    }
    // THE OLDER-SITE FALLBACK, said plainly - but only when it is the reason
    // anything is missing. This site did not accept the diagnostics field,
    // so it was dropped and the same report retried once without it
    // (ProblemReportSendFlow); shown beside the "sent" line above, never in
    // place of it. If that RETRY also fails, the log was never the problem -
    // the retry carried the same text and contact the first attempt did, so
    // whatever it failed on this time is a real reason, and the status line
    // above already names it. Saying "the log could not be sent" on top of
    // that would point at the one thing that was already given up on.
    if (state == cascade::core::FeatureRequestState::Sent &&
        problemReportSender_.diagnosticsDropped()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
        ImGui::TextWrapped(
            "%s", tr("The diagnostics log could not be sent with this report."));
        ImGui::PopStyleColor();
        cascade::gui::census::note("problem:diag-dropped");
    }
    problemReportBelowBoxH_ =
        ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y - belowTop;

    endPage();

    // THE LOG LINE, exactly once per send - the kind the person chose, how
    // many characters and what the server answered, never the words and
    // never the contact, as PRIVACY.md promises.
    if (problemReportLastLoggedState_ == cascade::core::FeatureRequestState::Sending &&
        state != cascade::core::FeatureRequestState::Sending) {
        if (state == cascade::core::FeatureRequestState::Sent) {
            cascade::core::diagLogf("problem report (%s): sent, %zu characters, HTTP %d",
                                    problemReportSentKind_.c_str(), problemReportSentChars_,
                                    problemReportSender_.lastStatus());
        } else {
            cascade::core::diagLogf("problem report (%s): failed, %zu characters, HTTP %d - %s",
                                    problemReportSentKind_.c_str(), problemReportSentChars_,
                                    problemReportSender_.lastStatus(),
                                    problemReportSender_.failureMessage().c_str());
        }
    }
    // The same one-frame clear as the feature page (featureRequestClearsTextNow
    // is about the sender's states, not about which body it carried). The
    // kind and the contact stay.
    if (cascade::core::featureRequestClearsTextNow(problemReportLastLoggedState_, state)) {
        problemReportText_.clear();
    }
    problemReportLastLoggedState_ = state;
}

}  // namespace cascade::gui
