// app_window_updates.cpp - THE UPDATE CHECK AND WHAT IT SHOWS: the site's banner, the Microsoft Store's own banner and install key, a message from the FoxSDR team, and the
// Updates section of the rail. AppWindow members, kept out of app_window.cpp because they are one subject (and because that file is at the object-file limit).
// Restored in stub campaign B14 (UPDATES: 0.99.53 a Store copy asks the Store; 0.99.79 a message from the team is shown with or without an update), from master's text of
// 0.99.90 - these functions are unchanged to 0.99.95. The window's own updater: nothing here is the engine's and the headless engine kind has none of it.
//
// THE THREE THINGS THIS FILE MUST NOT DO (tests/test_store_update_app.cpp, tests/test_team_message_app.cpp hold each): a Store copy asks the STORE and never the canned
// answer (and an unpackaged copy never asks the Store, whatever the seam says); an unticked box asks nobody; and a message marked Seen stays Seen across a launch, while an
// id the last answer no longer carries is forgotten.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cfloat>
#include <chrono>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/package_identity.hpp"
#include "core/store_update.hpp"
#include "core/updater.hpp"
#include "core/utf8_text.hpp"
#include "core/version.hpp"
#include "gui/win_frame.hpp"
#include "gui/bench_rail.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// THE ONE RED (app_window.cpp's kErrorRed, which this file cannot see): the theme's, read when it is used.
struct ErrorRed {
    operator ImVec4() const { return cascade::gui::theme::bad(); }
};
constexpr ErrorRed kErrorRed{};

// FOR THE UI CENSUS ONLY (a no-op unless FOXSDR_UI_CENSUS names a file): the rectangle of the item just drawn, so a test aims its press.
void censusRect(const char* name) {
    if (!cascade::gui::census::enabled()) { return; }
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    cascade::gui::census::rect(name, a.x, a.y, b.x, b.y);
}

// ...and the frame a banner was LAST drawn on. A census item is "ever seen", which cannot say that a banner stopped; a rectangle keeps the newest value, so the frame number
// of the last draw does (tests/test_store_update_app.cpp: pressed "Not now" at frame N, the banner is last drawn by about N and not drawn again).
void censusBannerFrame() {
    if (!cascade::gui::census::enabled()) { return; }
    cascade::gui::census::rect("update:banner-frame", static_cast<float>(ImGui::GetFrameCount()), 0.0f, 0.0f, 0.0f);
}

}  // namespace

void AppWindow::startUpdateCheck() {
    if (updateStarted_ || updatePending_) { return; }
    // WHICH CHANNEL, decided by one pure function (core/package_identity.hpp).
    //
    // Unticked asks nobody. An ordinary install asks foxsdr.com, below. A copy
    // installed from the Microsoft Store must never run foxsdr-setup-<ver>.exe
    // (that would put a SECOND, unpackaged FoxSDR beside the packaged one), and
    // until 0.99.53 it therefore asked nobody at all - and the Store's own
    // background updating left copies a day or more behind without a word. It
    // now asks the Store, through Windows (core/store_update.hpp): Microsoft's
    // service only, nothing of ours sent, nothing installed unless the user
    // presses the key and confirms Windows' own dialog.
    const cascade::core::UpdateCheckDisposition disposition =
        cascade::core::updateCheckDisposition(cascade::core::runningInPackage(),
                                              updateCheckEnabled_);
    if (disposition == cascade::core::UpdateCheckDisposition::OffByChoice) { return; }
    if (disposition == cascade::core::UpdateCheckDisposition::AskStore) {
        updateStarted_ = true;
        updatePending_ = true;
        updateError_.clear();
        storeUpdate_ = cascade::core::StoreUpdateCheck{};
        cascade::core::diagLogf("%s", cascade::core::storeUpdateAskLine());
        // The main window is the owner Windows needs for a desktop app's Store
        // calls (see store_update.cpp). A handle is a number, not a borrowed
        // object: nothing of `this` crosses to the worker.
        void* const owner = cascade::gui::frame::nativeHandle(mainWindow_);
        storeCheck_.start([owner]() { return cascade::core::checkStoreForUpdates(owner); });
        return;
    }
    updateStarted_ = true;
    updatePending_ = true;
    updateError_.clear();
    const std::string endpoint = cascade::core::updateEndpoint();
    const std::string version = cascade::versionString();
    // Stable ("") unless the person chose every release (0.99.88).
    const std::string channel = cascade::core::updateChannelParam(updateChannel_);
    // NOTHING OF `this` IS CAPTURED: the outcome comes back by value, which is
    // what lets ~AppWindow abandon a check stalled inside WinHTTP instead of
    // waiting out its timeouts (see core::UpdateCheckTask).
    updateCheck_.start([endpoint, version, channel]() {
        cascade::core::UpdateCheckOutcome o;
        o.ok = cascade::core::checkForUpdate(endpoint, version, channel, o.info, o.error);
        return o;
    });
}

void AppWindow::startStoreInstall() {
    if (storeInstallPending_ || storeInstall_.running()) { return; }
    storeInstallPending_ = true;
    storeInstallDone_ = false;
    storeInstallOutcome_ = cascade::core::StoreInstallOutcome{};
    cascade::core::diagLogf("store update: install requested from the banner");
    // ON A WORKER, NOT THE GUI THREAD: the request blocks until Windows' two
    // dialogs have been answered and the package installed, and the frame
    // loop has to keep drawing (and the hang watchdog keep seeing it draw)
    // all that time. See store_update.cpp for what Microsoft documents about
    // the thread and the owner window.
    void* const owner = cascade::gui::frame::nativeHandle(mainWindow_);
    storeInstall_.start([owner]() { return cascade::core::requestStoreUpdateInstall(owner); });
}

void AppWindow::pollUpdateAsync() {
    constexpr auto kNoWait = std::chrono::seconds(0);

    cascade::core::UpdateCheckOutcome checked;
    if (updatePending_ && !updateDownloading_ && updateCheck_.poll(checked)) {
        updatePending_ = false;
        if (checked.ok) {
            update_ = checked.info;
            // THE SEEN LIST FORGETS WHAT THE SERVICE NO LONGER CARRIES (0.99.79): only ids
            // the last SUCCESSFUL check still named stay, so the list cannot grow for ever
            // and a message the owner withdraws and later sends again is shown again - a
            // withdrawn message is gone, not "read". A failed check leaves the list alone
            // (below): not hearing from the service is not the service withdrawing
            // anything. A change is saved at once (a hermetic run has no file and skips
            // it); configsEqual sees it too, so the periodic writer covers a lost write.
            std::vector<std::string> kept;
            for (const std::string& id : messagesSeen_) {
                for (const cascade::core::TeamMessage& m : update_.messages) {
                    if (m.id == id) {
                        kept.push_back(id);
                        break;
                    }
                }
            }
            if (kept.size() != messagesSeen_.size()) {
                messagesSeen_ = std::move(kept);
                saveConfigFromFrame();
            }
        } else {
            // A failed check is NOT shown. The user did not ask, and an
            // application that interrupts listening to say it could not reach a
            // server is worse than one that quietly tries again next launch.
            updateError_ = checked.error;
        }
    }

    // The Microsoft Store's answer, on exactly the same terms: a failure goes
    // to updateError_, which only the Updates section shows, never a banner.
    cascade::core::StoreUpdateCheck storeChecked;
    if (storeCheck_.poll(storeChecked)) {
        updatePending_ = false;
        if (storeChecked.ok) {
            storeUpdate_ = storeChecked;
        } else {
            storeUpdate_ = cascade::core::StoreUpdateCheck{};
            updateError_ = storeChecked.error;
        }
    }
    cascade::core::StoreInstallOutcome installed;
    if (storeInstall_.poll(installed)) {
        storeInstallPending_ = false;
        storeInstallDone_ = true;
        storeInstallOutcome_ = installed;
    }

    if (updatePending_ && updateDownloading_ && updateDownloadFuture_.valid() &&
        updateDownloadFuture_.wait_for(kNoWait) == std::future_status::ready) {
        const bool ok = updateDownloadFuture_.get();
        updatePending_ = false;
        updateDownloading_ = false;
        if (ok) {
            updateReadyPath_ = updateResultPath_;
        } else {
            // A download failure IS shown: the user pressed a button and is
            // waiting for it. The message is PluginRepo's verbatim, which for a
            // digest mismatch names both digests - the one case where the
            // detail matters more than the tidiness.
            updateError_ = updateResultError_;
        }
    }
}

void AppWindow::saveConfigFromFrame() {
    // (See the declaration for why this is not saveConfigNow.) The damaged-file rule is maybeSaveConfig's own: a settings file that could not be kept aside is never
    // saved over.
    if (configPath_.empty() || configWriter_.writesForbidden()) { return; }
    requestConfigSave(currentConfig());
}

bool AppWindow::teamMessageSeen(const std::string& id) const {
    return std::find(messagesSeen_.begin(), messagesSeen_.end(), id) != messagesSeen_.end();
}

// A MESSAGE FROM THE TEAM (0.99.79). The owner's request (2026-10-14): "the broadcast system
// for the software should send the broadcast out even if there isn't an update; it should post
// to the user, then they click Seen and it doesn't show up again". Until now the website's
// broadcast rode inside the release notes of an update and was drawn only by the update banner
// below, which exists only when a newer build does - so a copy on the current version never
// saw one. The update service now sends them as their own list (core/updater.hpp,
// TeamMessage), and this draws a card for each one not yet marked Seen.
//
// ABOVE THE UPDATE BANNER and independent of it: "Not now" there (updateDismissed_) does not
// hide a message, and Seen here does not dismiss the banner. Red for the owner's "Important"
// tick, amber otherwise - the split the banner makes for a critical update.
//
// NOT DRAWN when the Updates tick is off (nobody was asked, so there is nothing to show), nor
// for a Microsoft Store copy, which asks the Store and not foxsdr.com and so never hears one.
void AppWindow::drawTeamMessages() {
    if (!updateCheckEnabled_ || cascade::core::runningInPackage()) { return; }
    int index = 0;
    for (const cascade::core::TeamMessage& m : update_.messages) {
        if (teamMessageSeen(m.id)) { continue; }
        // The id is each card's ImGui identity: two cards both carry a key labelled "Seen",
        // and without this the second's press would land on the first.
        ImGui::PushID(m.id.c_str());
        cascade::gui::census::note("message:card");
        cascade::gui::census::note("message:card:", index);
        cascade::gui::census::note("message:id:", m.id);
        if (m.important) { cascade::gui::census::note("message:important"); }

        ImGui::PushStyleColor(ImGuiCol_Text, m.important ? kErrorRed : cascade::gui::theme::warning());
        ImGui::TextWrapped("%s", tr("Message from the FoxSDR team"));
        ImGui::PopStyleColor();
        if (!m.date.empty()) { ImGui::TextDisabled("%s", m.date.c_str()); }
        for (const std::string& line : m.lines) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button(trId("Seen"), ImVec2(-FLT_MIN, 0.0f))) {
            // REMEMBERED, AND SAVED AT ONCE: "it doesn't show up again" has to survive a quit
            // or a crash that comes straight after the press. The list is oldest first and
            // bounded the way the config loads it, dropping the oldest.
            messagesSeen_.push_back(m.id);
            while (messagesSeen_.size() > cascade::core::AppConfig::kMaxMessagesSeen) {
                messagesSeen_.erase(messagesSeen_.begin());
            }
            saveConfigFromFrame();
        }
        const ImVec2 k0 = ImGui::GetItemRectMin();
        const ImVec2 k1 = ImGui::GetItemRectMax();
        cascade::gui::census::rect("message:seen:", index, k0.x, k0.y, k1.x, k1.y);
        // The first card's key under a name of its own, for the test that has only one.
        if (index == 0) { cascade::gui::census::rect("message:seen", k0.x, k0.y, k1.x, k1.y); }
        ImGui::Separator();
        ImGui::PopID();
        ++index;
    }
}

void AppWindow::drawUpdateBanner() {
    if (!updateCheckEnabled_ || updateDismissed_) { return; }
    if (cascade::core::runningInPackage()) {
        drawStoreUpdateBanner();
        return;
    }
    if (!update_.newer && updateError_.empty()) { return; }
    if (!update_.newer) { return; }
    // For the census (0.99.79): the foxsdr.com banner was drawn - the Store's has had
    // "update:store-banner" since 0.99.53. tests/test_team_message_app pins that a message
    // and an update are drawn together, and that a message alone draws no update banner.
    cascade::gui::census::note("update:site-banner");
    censusBannerFrame();

    // Amber for an ordinary update, red for one that fixes a build which could
    // not do its job. The distinction is the server's `critical` flag, and it
    // is the difference between a notice and a warning.
    const ImVec4 accent = update_.critical ? kErrorRed : cascade::gui::theme::warning();
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    if (update_.critical) {
        ImGui::TextWrapped(tr("Important update: FoxSDR %s"), update_.version.c_str());
    } else {
        ImGui::TextWrapped(tr("FoxSDR %s is available"), update_.version.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::TextDisabled(tr("you are running %s"), cascade::versionString());

    // WHAT IT FIXES, in the author's words, for every release between the one
    // running and the one offered. Somebody several versions behind sees all of
    // them, which is the case that matters: the people who most need this are
    // the furthest back.
    for (const cascade::core::ReleaseNote& n : update_.notes) {
        ImGui::Spacing();
        ImGui::Text("%s%s", n.version.c_str(), n.critical ? tr("  (important)") : "");
        for (const std::string& line : n.notes) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line.c_str());
        }
    }

    ImGui::Spacing();
#if defined(_WIN32)
    if (updateDownloading_) {
        ImGui::TextColored(cascade::gui::theme::warning(), tr("Downloading..."));
        // updateProgress_, not pluginRepo_.progress(): this transfer does not
        // go through a PluginRepo instance, so that value is whatever the
        // plugin browser last did — which for the common case of never having
        // opened it is 0, for the whole download.
        ImGui::ProgressBar(updateProgress_.load(std::memory_order_relaxed),
                           ImVec2(-FLT_MIN, 0.0f));
    } else if (!updateReadyPath_.empty()) {
        // Downloaded AND verified. Running it closes this application, which
        // the button says outright rather than surprising anyone: an installer
        // cannot replace a binary that is still running.
        ImGui::TextWrapped(tr("Downloaded and verified."));
        if (ImGui::Button(trId("Install now and restart"), ImVec2(-FLT_MIN, 0.0f))) {
            if (launchInstaller(updateReadyPath_)) {
                requestClose();
            } else {
                std::string msg;
                cascade::core::formatUtf8(msg, tr("could not start the installer at %s"),
                              updateReadyPath_.c_str());
                updateError_ = msg;
            }
        }
        ImGui::TextDisabled("%s", updateReadyPath_.c_str());
    } else {
        ImGui::BeginDisabled(updatePending_);
        if (ImGui::Button(trId("Download update"), ImVec2(-FLT_MIN, 0.0f))) { startUpdateDownload(); }
        ImGui::EndDisabled();
        if (update_.sizeBytes > 0) {
            // Wrapped, not TextDisabled: the sidebar is narrow and the
            // unwrapped line was cut off mid-word at "verified against its
            // publis", which reads as a rendering fault rather than a promise.
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped(tr("%.1f MB, verified against its published checksum before anything runs"),
                               static_cast<double>(update_.sizeBytes) / 1.0e6);
            ImGui::PopStyleColor();
        }
    }
#else
    // NO LINUX INSTALLER EXISTS YET. The manifest at foxsdr.com/api/update
    // names a Windows .exe (foxsdr-setup-<version>.exe) whether the asking
    // client is Windows or Linux - it has no notion of a client platform, and
    // as of this port there is nothing else on the site to offer a Linux
    // build. Downloading that .exe here and then failing to launch it (the
    // #if defined(_WIN32) branch above) would be a working-looking button
    // that quietly cannot succeed. The honest behaviour is to say so and hand
    // the person to the one place that will eventually have the right file.
    ImGui::TextWrapped(
        tr("There is no in-app updater for Linux yet - download it from foxsdr.com."));
    if (ImGui::Button(trId("Open foxsdr.com"), ImVec2(-FLT_MIN, 0.0f))) {
        shellOpen(cascade::core::kHomepageUrl);
    }
#endif
    if (ImGui::SmallButton(trId("Not now"))) { updateDismissed_ = true; }
    censusRect("update:notnow");

    if (!updateError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", updateError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

void AppWindow::drawStoreUpdateBanner() {
    // SHOWN ONLY WHEN THE STORE HAS SAID "yes, there is one" - or while an
    // install request it started is with Windows, or has an outcome to report.
    // A failed check is never a banner (the Updates section shows it), for the
    // reason the foxsdr.com banner gives: the user did not ask.
    const bool offered = storeUpdate_.ok && storeUpdate_.updates > 0;
    if (!offered && !storeInstallPending_ && !storeInstallDone_) { return; }
    cascade::gui::census::note("update:store-banner");
    censusBannerFrame();

    // Red for a package marked mandatory in Partner Center, amber otherwise -
    // the same split as the foxsdr.com banner's critical flag. NO VERSION
    // NUMBER: the Store does not say which version it is offering, and this
    // banner does not guess.
    const bool mandatory = storeUpdate_.mandatory;
    if (mandatory) { cascade::gui::census::note("update:store-mandatory"); }
    ImGui::PushStyleColor(ImGuiCol_Text, mandatory ? kErrorRed : cascade::gui::theme::warning());
    if (mandatory) {
        ImGui::TextWrapped("%s", tr("Important FoxSDR update available from the Microsoft Store"));
    } else {
        ImGui::TextWrapped("%s", tr("FoxSDR update available from the Microsoft Store"));
    }
    ImGui::PopStyleColor();
    ImGui::TextDisabled(tr("you are running %s"), cascade::versionString());
    ImGui::Spacing();

    using cascade::core::StoreInstallResult;
    bool offerKey = offered && !storeInstallPending_;
    if (storeInstallPending_) {
        cascade::gui::census::note("update:store-waiting");
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", tr("Waiting for Windows..."));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", tr("Windows shows its own dialogs for the download and the "
                                    "install - answer them to carry on."));
        ImGui::PopStyleColor();
    } else if (storeInstallDone_) {
        switch (storeInstallOutcome_.result) {
        case StoreInstallResult::Installed:
            cascade::gui::census::note("update:store-outcome:installed");
            ImGui::TextWrapped("%s", tr("Windows reports the update installed. If FoxSDR is still "
                                        "open, close it and open it again to run the new version."));
            offerKey = false;
            break;
        case StoreInstallResult::Cancelled:
            cascade::gui::census::note("update:store-outcome:cancelled");
            ImGui::TextWrapped("%s", tr("You cancelled the update, so nothing was changed."));
            break;
        case StoreInstallResult::NothingToInstall:
            cascade::gui::census::note("update:store-outcome:nothing");
            ImGui::TextWrapped("%s", tr("The Microsoft Store has no update to install now."));
            offerKey = false;
            break;
        case StoreInstallResult::Failed: {
            cascade::gui::census::note("update:store-outcome:failed");
            std::string reason = storeInstallOutcome_.reasonKey != nullptr
                                     ? std::string(tr(storeInstallOutcome_.reasonKey))
                                     : std::string();
            if (!storeInstallOutcome_.detail.empty()) {
                reason += reason.empty() ? storeInstallOutcome_.detail
                                         : " (" + storeInstallOutcome_.detail + ")";
            }
            std::string msg;
            cascade::core::formatUtf8(msg, tr("could not install: %s"), reason.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
            ImGui::TextWrapped("%s", msg.c_str());
            ImGui::PopStyleColor();
            break;
        }
        }
    }

    if (offerKey) {
        // Pressed again after a cancel or a failure, it simply asks again:
        // Windows' dialog is the confirmation, so there is no second one here.
        if (ImGui::Button(trId("Install from the Microsoft Store"), ImVec2(-FLT_MIN, 0.0f))) {
            startStoreInstall();
        }
        cascade::gui::census::note("update:store-install");
        const ImVec2 k0 = ImGui::GetItemRectMin();
        const ImVec2 k1 = ImGui::GetItemRectMax();
        cascade::gui::census::rect("update:store-install", k0.x, k0.y, k1.x, k1.y);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", tr("Windows will ask you to confirm, and FoxSDR may close while "
                                    "the update installs."));
        ImGui::PopStyleColor();
    }
    if (ImGui::SmallButton(trId("Not now"))) { updateDismissed_ = true; }
    censusRect("update:notnow");
    ImGui::Separator();
}

void AppWindow::drawUpdatesSection() {
    // THE FOUR STATES THIS SECTION ACTUALLY HAS, and no fifth invented one.
    // "OK" is only said once a check has completed without error - before
    // that it is IDLE, because "up to date" and "we have not asked" are
    // different statements and the second one must not wear the first one's
    // clothes.
    // THE SAME STATES FOR A COPY FROM THE MICROSOFT STORE (0.99.53), asked of
    // the Store instead of foxsdr.com, plus one only the Store has: STORE,
    // while an install request is with Windows and its dialogs. (Until 0.99.53
    // STORE was this row's permanent chip for a packaged copy, which asked
    // nobody; it now asks the Store when the box is ticked.)
    const bool packaged = cascade::core::runningInPackage();
    const bool storeOffered = packaged && storeUpdate_.ok && storeUpdate_.updates > 0;
    const bool offered = packaged ? storeOffered : update_.newer;
    const bool important = packaged ? storeUpdate_.mandatory : update_.critical;

    const char* updateChip = tr("OFF");
    // For the census only: the state in English, whatever the language.
    const char* state = "off";
    ImU32 updateLamp = cascade::gui::theme::kPhosphor;
    bool updateLit = false;
    if (updateCheckEnabled_) {
        if (packaged && storeInstallPending_) {
            updateChip = tr("STORE");
            state = "installing";
            updateLamp = cascade::gui::theme::kAmber;
            updateLit = true;
        } else if (offered) {
            updateChip = important ? tr("IMPT") : tr("NEW");
            state = "available";
            updateLamp = important ? cascade::gui::theme::kAlarm : cascade::gui::theme::kAmber;
            updateLit = true;
        } else if (updatePending_) {
            updateChip = tr("CHECK");
            state = "checking";
        } else if (updateStarted_ && updateError_.empty()) {
            updateChip = tr("OK");
            state = "uptodate";
        } else {
            updateChip = tr("IDLE");
            state = updateError_.empty() ? "idle" : "failed";
        }
    }
    cascade::gui::census::note(packaged ? "updates:store-" : "updates:site-", state);
    if (!railSection(trId("Updates"), false, updateChip, updateLamp, updateLit)) { return; }
    uiNotePanel("updates");

    const char* const tickLabel = packaged ? trId("Ask the Microsoft Store for updates at startup")
                                           : trId("Check for updates at startup");
    if (ImGui::Checkbox(tickLabel, &updateCheckEnabled_)) {
        // Off means off immediately: a check already in flight is not waited
        // for, and none is started again this launch.
        if (!updateCheckEnabled_) {
            update_ = cascade::core::UpdateInfo{};
            storeUpdate_ = cascade::core::StoreUpdateCheck{};
            updateError_.clear();
        } else if (!updatePending_) {
            // A RE-TICK ASKS AGAIN. This was `else if (!updateStarted_)`, and
            // updateStarted_ - "one check per launch, no retry storm" - is
            // never cleared, so unticking and re-ticking left the section
            // reading "up to date" and the chip reading OK about an answer the
            // untick had just thrown away. Re-arming is what makes the tick
            // mean anything; the storm the flag guards against cannot come
            // from here, because only a click reaches this line.
            //
            // A check already IN FLIGHT is left alone: startUpdateCheck would
            // refuse it anyway, and it is about to deliver the very answer the
            // re-tick is asking for.
            updateStarted_ = false;
            startUpdateCheck();
        }
    }
    // WHICH RELEASES (0.99.88). Not for a Store copy: its updates are the
    // Store's, which certifies every one and is the slower channel already.
    if (!packaged) {
        const bool wasBeta = updateChannel_ == "beta";
        bool beta = wasBeta;
        if (ImGui::RadioButton(trId("Stable releases"), !beta)) { beta = false; }
        ImGui::SameLine();
        if (ImGui::RadioButton(trId("Every release (beta)"), beta)) { beta = true; }
        cascade::gui::census::note("updates:channel-", beta ? "beta" : "stable");
        if (beta != wasBeta) {
            updateChannel_ = beta ? "beta" : "stable";
            cascade::core::diagLogf("updates: channel set to %s", updateChannel_.c_str());
            // A CHANGE ASKS AGAIN, as a re-tick does: the answer on screen was
            // for the other channel.
            if (updateCheckEnabled_ && !updatePending_) {
                update_ = cascade::core::UpdateInfo{};
                updateError_.clear();
                updateStarted_ = false;
                startUpdateCheck();
            }
        }
        ImGui::TextDisabled("%s", tr("Stable is the release the FoxSDR team recommends to "
                                        "everyone; beta is every release the day it is "
                                        "published."));
    }
    if (packaged) {
        // HONEST ABOUT WHO IS ASKED: Microsoft's Store service, through
        // Windows, identifying the app by the package identity Windows holds.
        ImGui::TextWrapped(
            "%s",
            tr("This copy came from the Microsoft Store, so FoxSDR asks the Microsoft Store once "
               "per launch whether it has a newer version. Nothing is sent to foxsdr.com. Nothing "
               "is downloaded or installed unless you press the key and confirm the dialog "
               "Windows shows."));
    } else {
        ImGui::TextWrapped(
            "%s",
            tr("Asks foxsdr.com once per launch whether a newer version exists, and sends the "
               "version you are running and nothing else - no identifier, no cookie. Nothing is "
               "downloaded or installed unless you press the button. This is not the usage report "
               "below; the two share nothing."));
    }

    ImGui::TextDisabled(tr("this build: %s"), cascade::versionString());
    if (packaged) {
        ImGui::TextDisabled(tr("package: %s"), cascade::core::packageIdentity().fullName.c_str());
    }
    // HOW MANY MESSAGES FROM THE TEAM ARE WAITING (0.99.79), under the version line: the
    // cards are at the top of the column, and this says there is something there when the
    // section is read on its own. Only the UNSEEN ones count, and nothing at all is drawn
    // when none are - a "0 messages" line would be noise on every launch. Not for a Store
    // copy, which never hears one.
    if (!packaged && updateCheckEnabled_) {
        int unseen = 0;
        for (const cascade::core::TeamMessage& m : update_.messages) {
            if (!teamMessageSeen(m.id)) { ++unseen; }
        }
        if (unseen > 0) {
            cascade::gui::census::note("updates:messages");
            ImGui::TextDisabled(tr("%d message(s) from the FoxSDR team"), unseen);
        }
    }
    if (!updateCheckEnabled_) {
        ImGui::TextDisabled("%s", tr("checking is off, so you will not be told about a new version"));
    } else if (updatePending_) {
        ImGui::TextDisabled("%s", tr("checking..."));
    } else if (storeOffered) {
        // No version number: the Store does not give one.
        ImGui::TextColored(important ? kErrorRed : cascade::gui::theme::warning(), "%s",
                           tr("the Microsoft Store has an update for this copy"));
        if (updateDismissed_) {
            cascade::gui::census::note("updates:dismissed");
            if (ImGui::SmallButton(trId("Show it again"))) { updateDismissed_ = false; }
            censusRect("updates:show-again");
        }
    } else if (update_.newer) {
        ImGui::TextColored(update_.critical ? kErrorRed : cascade::gui::theme::warning(),
                           tr("%s is available"), update_.version.c_str());
        if (updateDismissed_) {
            cascade::gui::census::note("updates:dismissed");
            if (ImGui::SmallButton(trId("Show it again"))) { updateDismissed_ = false; }
            censusRect("updates:show-again");
        }
    } else if (updateStarted_ && updateError_.empty()) {
        ImGui::TextDisabled("%s", tr("up to date"));
    } else if (!updateError_.empty()) {
        // Only here, never as a banner: a failed check is not the user's
        // problem and must not interrupt them.
        ImGui::TextDisabled(tr("last check did not complete: %s"), updateError_.c_str());
    }
}

}  // namespace cascade::gui
