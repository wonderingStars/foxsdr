// app_window_radio_setup.cpp - the RADIO SETUP page (core/radio_setup.hpp).
//
// WHAT IT IS FOR. 578 of 813 installs in a month reported no radio at all, and
// the few reasons a radio is invisible each have one fix. The page checks this
// computer - what is on the USB bus, which driver Windows bound to it, whether
// the SDRplay API and UHD are installed - and says exactly what to install, with
// the vendor's own link. It opens by itself ONCE A LAUNCH for a person whose
// machine shows no radio the program can use, and by a key in the Source section
// whenever anyone wants it.
//
// THE WORK IS NOT HERE. assess() is pure and tested without a window
// (tests/test_radio_setup.cpp); the probe runs on a worker of its own with a
// time budget (radiosetup::Probe), so this file only polls it, once a frame,
// and draws what it found. Nothing in it blocks the frame loop and nothing in it
// opens a radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/radio_setup.hpp"
#include "gui/device_scan_plan.hpp"
#include "gui/page_geometry.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;
namespace rs = cascade::core::radiosetup;

void AppWindow::radioSetupStartProbe() {
    if (!radioSetupFake_.empty()) {
        // THE BOUNDED RUN'S SYNTHETIC MACHINE: answers at once and touches no
        // bus, so the page can be raised with a chosen finding on any desk.
        const std::string name = radioSetupFake_;
        cascade::core::diagLogf("radio setup: FOXSDR_FAKE_RADIO_SETUP=%s - a synthetic machine, "
                                "the bus is not read",
                                name.c_str());
        radioSetupProbe_.start([name]() {
            rs::Inventory inv;
            if (!rs::fakeInventory(name, inv)) {
                inv.probed = true;
                inv.usbListed = false;
                inv.note = "FOXSDR_FAKE_RADIO_SETUP names no known machine";
            }
            return inv;
        });
        return;
    }
    cascade::core::diagLogf("radio setup: checking the USB bus and what is installed for it");
    radioSetupProbe_.start(&rs::collectInventory);
}

// What the probe saw, plus what the application's own lists hold now. The
// probe cannot know the second half: the Source list is filled by the GUI
// thread (and the SoapySDR half only when someone scans, on purpose), and a
// radio the receiver or a patch has OPEN is a radio whatever any list says.
rs::Inventory AppWindow::radioSetupInventory() const {
    rs::Inventory inv = radioSetupProbe_.inventory();
    // A synthetic machine is the whole truth: it replaces the lists too.
    if (!radioSetupFake_.empty() || !inv.probed || !inv.usbListed) { return inv; }

    int listed = 0;
    const auto note = [&inv](const std::string& family) {
        if (family.empty()) { return; }
        if (std::find(inv.visible.begin(), inv.visible.end(), family) == inv.visible.end()) {
            inv.visible.push_back(family);
        }
    };
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        // The Pluto's row is always in the list (a network cannot be walked,
        // so it asks for an address): it says nothing about what is plugged in.
        if (d.driver == "pluto") { continue; }
        ++listed;
        note(rs::familyForDriver(d.driver));
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        ++listed;
        note(rs::familyForDriver(cascade::gui::detail::driverOf(d.args)));
    }
    if (device_ != nullptr) {
        ++listed;
        const std::string key = device_->driverKey();
        note(rs::familyForDriver(key == "soapy" ? cascade::gui::detail::driverOf(deviceArgs_)
                                                : key));
    }
    listed += static_cast<int>(patchHeldRadioNames().size());
    inv.visibleCount = std::max(inv.visibleCount, listed);
    // The SoapySDR families are judged only against a scan that actually ran
    // and covered every driver.
    inv.soapyChecked = soapyScanned_ && !soapyScanPartial_ && !soapyScanPending_;
    return inv;
}

// Once a frame, from the frame loop: starts the probe on the first frame,
// collects it without waiting, and makes the once-a-launch decision to raise
// the page.
void AppWindow::radioSetupFrame() {
    if (!radioSetupStarted_) {
        radioSetupStarted_ = true;
        radioSetupStartProbe();
    }
    if (radioSetupProbe_.poll()) {
        if (radioSetupProbe_.state() == rs::Probe::State::TimedOut) {
            cascade::core::diagWarnf("radio setup: the probe did not finish in %lld s - not "
                                     "waiting for it any longer",
                                     static_cast<long long>(rs::kProbeBudget.count() / 1000));
        }
        const rs::Inventory inv = radioSetupInventory();
        // One line a fact, each short enough for the log's line (see
        // summaryLines): what the bus and the lists held, what is installed,
        // one per finding, and whether the page is wanted.
        for (const std::string& line : rs::summaryLines(inv, rs::assess(inv))) {
            cascade::core::diagLogf("%s", line.c_str());
        }
    }

    if (radioSetupDecided_ || radioSetupProbe_.running() || radioSetupProbe_.finished() == 0) {
        return;
    }
    // The application's own lists are still moving: a saved radio is opening,
    // or a scan is out. Judging now would call a radio that is about to appear
    // missing.
    if (deviceOpenPending_ || soapyScanPending_) { return; }
    radioSetupDecided_ = true;

    const rs::Findings f = rs::assess(radioSetupInventory());
    if (radioSetupDontShow_) {
        cascade::core::diagLogf("radio setup: page not raised - \"Don't show this again\" is ticked");
    } else if (radioSetupBounded_ && radioSetupFake_.empty()) {
        // A bounded run on the real bus: probed and logged above, never raised
        // (see radioSetupBounded_). The line still says what the decision WOULD
        // have been, which is what a --frames run on a desk wants to know.
        cascade::core::diagLogf("radio setup: page not raised - a bounded run raises it only "
                                "for a synthetic machine (it would %s)",
                                f.showPage ? "have been raised" : "not have been raised");
    } else if (!f.showPage) {
        cascade::core::diagLogf("radio setup: page not raised - %s",
                                (f.items.size() == 1 && f.items[0].kind == rs::Kind::NotChecked)
                                    ? "nothing could be checked here"
                                    : "a radio is usable or the application lists one");
    } else {
        radioSetupOpen_ = true;
        cascade::core::diagLogf("radio setup: page raised on its own - %zu finding(s), first: %s",
                                f.items.size(), rs::kindName(f.items[0].kind));
    }
}

// CHECK AGAIN: what Refresh does for the lists, then a new probe. The lists
// come first so the new answer is judged against what the application sees
// now; a synthetic machine reads nothing and scans nothing.
void AppWindow::radioSetupCheckAgain() {
    if (radioSetupFake_.empty()) {
        scanNative();
        scanSoapy();  // defers itself, and says so once, while a radio is open
    }
    radioSetupStartProbe();
}

void AppWindow::drawRadioSetupPage() {
    if (!radioSetupOpen_) { return; }
    constexpr float kW = 580.0f;
    constexpr float kH = 520.0f;
    float px = 0.0f;
    float py = 0.0f;
    float pw = kW;
    float ph = kH;
    {
        // OPENS INSIDE THE MAIN WINDOW, centred: a page that opens by itself
        // somewhere a maximised single monitor cannot show has told nobody
        // anything.
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        cascade::gui::pageOpenInside(mv->Pos.x, mv->Pos.y, mv->Size.x, mv->Size.y, kW, kH, px,
                                     py, pw, ph);
    }
    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_FirstUseEver);
    if (!beginPage("Radio setup###radiosetupwindow", tr("RADIO SETUP"), &radioSetupOpen_, 0, pw,
                   ph)) {
        endPage();
        return;
    }
    cascade::gui::census::note("radio-setup:page");

    const bool running = radioSetupProbe_.running();
    const bool haveAnswer = radioSetupProbe_.finished() > 0;
    const ImGuiStyle& style = ImGui::GetStyle();

    // The footer keeps its place: two rows - the key, then the tick.
    const float footerH = ImGui::GetFrameHeightWithSpacing() * 2.0f + style.ItemSpacing.y;
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
        ImGui::TextUnformatted(tr("Checking this computer..."));
        ImGui::PopStyleColor();
    }
    if (haveAnswer) {
        const rs::Inventory inv = radioSetupInventory();
        const rs::Findings f = rs::assess(inv);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(
            f.showPage ? tr("FoxSDR found no radio it can use. This is what this computer shows, "
                            "and what to do about it:")
                       : tr("This is what this computer shows about your radios:"));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::BeginChild("##radiosetupfindings", ImVec2(0.0f, -footerH), false);
        int index = 0;
        for (const rs::Finding& x : f.items) {
            ImGui::PushID(index);
            cascade::gui::census::note("radio-setup:finding:", rs::kindName(x.kind));
            // Green for a radio that is fine, amber for anything that needs
            // doing, dim for "could not check".
            ImVec4 colour = cascade::gui::theme::warning();
            if (x.kind == rs::Kind::Usable) {
                colour = cascade::gui::theme::vec(cascade::gui::theme::kPhosphor);
            } else if (x.kind == rs::Kind::NotChecked) {
                colour = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextWrapped("%s", x.title.c_str());
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", x.explanation.c_str());
            ImGui::TextWrapped("%s", x.install.c_str());
            // THE LINK: the vendor's own page, shown as the address itself so a
            // person sees where the key goes before pressing it. Under the
            // watchdog pause like every shell call (gui/shell_open.hpp).
            std::string shown = x.url;
            if (shown.rfind("https://", 0) == 0) { shown.erase(0, 8); }
            const std::string label = shown + "###radiosetuplink";
            if (ImGui::SmallButton(label.c_str())) {
                cascade::core::diagLogf("radio setup: opening %s", x.url.c_str());
                shellOpen(x.url);
            }
            cascade::gui::census::note("radio-setup:link");
            {
                const ImVec2 k0 = ImGui::GetItemRectMin();
                const ImVec2 k1 = ImGui::GetItemRectMax();
                cascade::gui::census::rect("radio-setup:link", index, k0.x, k0.y, k1.x, k1.y);
            }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", x.url.c_str()); }
            ImGui::Separator();
            ImGui::PopID();
            ++index;
        }
        ImGui::EndChild();
    } else {
        // Nothing yet: keep the footer where it will be.
        ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, ImGui::GetContentRegionAvail().y - footerH)));
    }

    ImGui::BeginDisabled(running);
    if (ImGui::Button(trId("CHECK AGAIN"))) { radioSetupCheckAgain(); }
    ImGui::EndDisabled();
    cascade::gui::census::note("radio-setup:check-again");
    {
        const ImVec2 k0 = ImGui::GetItemRectMin();
        const ImVec2 k1 = ImGui::GetItemRectMax();
        cascade::gui::census::rect("radio-setup:check-again", k0.x, k0.y, k1.x, k1.y);
    }
    // THE TICK IS SAVED (AppConfig::radioSetupDontShow, through configsEqual's
    // debounce): it stops the page raising itself, and nothing else.
    ImGui::Checkbox(trId("Don't show this again"), &radioSetupDontShow_);
    cascade::gui::census::note("radio-setup:dont-show");
    {
        const ImVec2 k0 = ImGui::GetItemRectMin();
        const ImVec2 k1 = ImGui::GetItemRectMax();
        cascade::gui::census::rect("radio-setup:dont-show", k0.x, k0.y, k1.x, k1.y);
    }
    endPage();
}

}  // namespace cascade::gui
