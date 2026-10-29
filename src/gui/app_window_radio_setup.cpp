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
// WHAT THE APPLICATION'S OWN LISTS HOLD comes from the table, not from members of
// the window (stub campaign B8; master read nativeDevices_, soapyDevices_,
// device_, patchHeldRadioNames and the scan's flags): the Source combo's rows
// (DEVICES, through sourceLists()), the patch's radios, the state's flags and
// fact.source.uhdNoImages - the arithmetic is gui/radio_setup_inventory.hpp's.
// CHECK AGAIN asks the engine to scan (FOXAPI_OP_SCAN_DEVICES, the Refresh key's op).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <chrono>
#include <string>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/patch_devices.hpp"
#include "core/radio_setup.hpp"
#include "core/thread_beats.hpp"
#include "core/winusb_install.hpp"
#include "gui/radio_setup_inventory.hpp"
#include "gui/shell_open.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif
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
// probe cannot know the second half: the Source list is filled by the engine's
// scans (and the SoapySDR half only when someone scans, on purpose), and a
// radio the receiver or a patch has OPEN is a radio whatever any list says.
rs::Inventory AppWindow::radioSetupInventory() {
    rs::Inventory inv = radioSetupProbe_.inventory();
    // A synthetic machine is the whole truth: it replaces the lists too.
    if (!radioSetupFake_.empty() || !inv.probed || !inv.usbListed) { return inv; }

    RadioSetupLists lists;
    const SourceListCache& c = sourceLists();
    for (std::size_t i = static_cast<std::size_t>(kNativeRowBase); i < c.rows.size(); ++i) {
        const SourceListCache::Row& r = c.rows[i];
        lists.rows.push_back({r.driver, r.pluto || r.rtltcp});
    }
    const std::uint32_t flags = uiState_.flags;
    const std::uint32_t src = uiState_.sourceFlags;
    lists.deviceOpen = (flags & FOXAPI_RX_DEVICE_OPEN) != 0u;
    // The hardware radios the running patch holds: what each Radio node is set to says whether it is hardware (the generator and an I/Q file are not) - the Source row's own
    // reading (patchHeldRadioNames, stub campaign B18). The label PATCH_RADIOS carries cannot say: this counted a generator node as a radio held before.
    lists.patchHeldRadios = static_cast<int>(patchHeldRadioNames().size());
    lists.soapyScanned = (src & FOXAPI_SRC_SCANNED) != 0u;
    lists.soapyScanPartial = (src & FOXAPI_SRC_SCAN_PARTIAL) != 0u;
    lists.soapyScanPending = (src & FOXAPI_SRC_SCAN_PENDING) != 0u;
    lists.uhdNoImages = c.uhdNoImages;
    return radioSetupInventoryFrom(std::move(inv), lists);
}

// Once a frame, from the frame loop: starts the probe on the first frame,
// collects it without waiting, and makes the once-a-launch decision to raise
// the page.
// THE ONE-CLICK DRIVER INSTALL's state, shared with the thread that waits for
// the elevated helper: the thread sets `done` and touches nothing else of the
// window, so the job outliving the window (a helper still running at exit) is
// harmless.
struct AppWindow::DriverInstallJob {
    std::atomic<bool> done{false};
    bool consumed = false;
    std::string resultPath;
    // A SYNTHETIC MACHINE'S INSTALL (FOXSDR_FAKE_DRIVER_INSTALL): finished, with the result below, when the clock reaches this. Set only on the
    // window's thread, and read only there (radioSetupPollDriverInstall).
    bool fake = false;
    std::chrono::steady_clock::time_point fakeDoneAt{};
    // THE OUTCOME, written before `done` is set and read only after: by the
    // launch itself when it never got as far as a process (the prompt was
    // refused, or the shell call failed), otherwise by the waiting thread, which
    // reads and removes the result file - disk work, so never the window's.
    bool haveLaunchResult = false;
    cascade::core::winusb::Result launchResult;
};

namespace wu = cascade::core::winusb;

// INSTALL DRIVER: start `cascade --install-winusb <id> --result <file>` through
// the shell's "runas" verb - one Windows permission prompt - and wait for it on
// a thread of its own. The prompt is a person reading a dialog, so the shell
// call goes through runShellOpenResponsive and the watchdog's user-paced pause,
// exactly as the in-app installer launch does (gui/shell_open.hpp): ShellExecuteEx
// waits for the elevation prompt without taking messages, and made on the GUI
// thread that greyed the window "(Not Responding)" behind the prompt
// (gui/shell_open.cpp).
void AppWindow::radioSetupInstallDriver(const std::string& instanceId) {
    if (driverInstall_ && !driverInstall_->done.load()) { return; }
    auto job = std::make_shared<DriverInstallJob>();
    driverInstall_ = job;
    driverInstallStatus_ = tr("Installing the driver - answer the Windows permission prompt...");
    // Never the instance id: it carries the dongle's serial.
    cascade::core::diagLogf("radio setup: one-click driver install requested");
    // A SYNTHETIC MACHINE (FOXSDR_FAKE_RADIO_SETUP, bounded runs) has nothing to
    // install onto, and a test run must never put a real permission prompt on
    // somebody's desktop: the key answers without starting anything.
    if (!radioSetupFake_.empty()) {
        // FOXSDR_FAKE_DRIVER_INSTALL=<outcome>[,<milliseconds>] (bounded runs): that outcome after that long. Without it: not found, at once.
        wu::Outcome outcome = wu::Outcome::NotFound;
        long long afterMs = 0;
        if (!driverInstallFake_.empty()) {
            const std::size_t comma = driverInstallFake_.find(',');
            if (!wu::outcomeFromName(driverInstallFake_.substr(0, comma), outcome)) { outcome = wu::Outcome::NotFound; }
            if (comma != std::string::npos) { afterMs = std::max(0ll, std::atoll(driverInstallFake_.c_str() + comma + 1)); }
        }
        job->launchResult.outcome = outcome;
        job->launchResult.detail = "a synthetic machine: nothing was started";
        job->haveLaunchResult = true;
        if (afterMs <= 0) {
            job->done = true;
        } else {
            job->fake = true;
            job->fakeDoneAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(afterMs);
        }
        return;
    }
#if defined(_WIN32)
    wchar_t tempDir[MAX_PATH] = {0};
    wchar_t exe[MAX_PATH] = {0};
    const DWORD tl = ::GetTempPathW(MAX_PATH, tempDir);
    const DWORD el = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (tl == 0 || tl >= MAX_PATH || el == 0 || el >= MAX_PATH) {
        job->launchResult.outcome = wu::Outcome::Failed;
        job->launchResult.detail = "the temporary folder or the program path could not be read";
        job->haveLaunchResult = true;
        job->done = true;
        return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "foxsdr-winusb-%lu-%llu.txt",
                  static_cast<unsigned long>(::GetCurrentProcessId()),
                  static_cast<unsigned long long>(::GetTickCount64()));
    std::wstring resultW = std::wstring(tempDir) + std::wstring(name, name + std::strlen(name));
    {
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, resultW.c_str(), -1, nullptr, 0, nullptr,
                                            nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) {
            ::WideCharToMultiByte(CP_UTF8, 0, resultW.c_str(), -1, s.data(), n, nullptr, nullptr);
        }
        job->resultPath = s;
    }
    const std::string params = wu::helperParameters(instanceId, job->resultPath);
    if (params.empty()) {
        job->launchResult.outcome = wu::Outcome::Refused;
        job->launchResult.detail = "the device instance id could not be passed safely";
        job->haveLaunchResult = true;
        job->done = true;
        return;
    }
    const int pn = ::MultiByteToWideChar(CP_UTF8, 0, params.c_str(), -1, nullptr, 0);
    std::wstring paramsW(static_cast<std::size_t>(pn > 0 ? pn - 1 : 0), L'\0');
    if (pn > 1) { ::MultiByteToWideChar(CP_UTF8, 0, params.c_str(), -1, paramsW.data(), pn); }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = paramsW.c_str();
    sei.nShow = SW_HIDE;
    DWORD launchError = 0;
    const bool launched = cascade::gui::runShellOpenResponsive(watchdogShellHooks(), [&]() -> bool {
        const bool ok = ::ShellExecuteExW(&sei) != FALSE;
        if (!ok) { launchError = ::GetLastError(); }
        return ok;
    });
    if (!launched || sei.hProcess == nullptr) {
        job->launchResult.outcome =
            launchError == ERROR_CANCELLED ? wu::Outcome::Cancelled : wu::Outcome::Failed;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the elevated installer did not start (Windows error %lu)",
                      static_cast<unsigned long>(launchError));
        job->launchResult.detail = buf;
        job->haveLaunchResult = true;
        job->done = true;
        return;
    }
    const HANDLE process = sei.hProcess;
    std::thread([job, process]() {
        // THE HEARTBEAT (core/thread_beats.hpp, 6.27): the installer is somebody else's program and a person answers its
        // prompts, so the wait for it is a DECLARED wait with a cap (kWaitInstaller, eight minutes), after which the watchdog
        // names this thread.
        cascade::core::beats::Registration beat(cascade::core::beats::kThreadDriverInstall);
        beat.beat("launched");
        beat.wait(cascade::core::beats::kWaitInstaller);
        ::WaitForSingleObject(process, INFINITE);
        beat.beat("result");
        ::CloseHandle(process);
        wu::Result r;
        {
            std::ifstream in(job->resultPath, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            if (!wu::parseResult(text, r)) {
                r.outcome = wu::Outcome::Failed;
                r.detail = "the installer left no readable result";
            }
        }
        std::remove(job->resultPath.c_str());
        job->launchResult = r;
        job->haveLaunchResult = true;
        job->done = true;
    }).detach();
#else
    (void)instanceId;
    job->launchResult.outcome = wu::Outcome::Failed;
    job->launchResult.detail = "not on this platform";
    job->haveLaunchResult = true;
    job->done = true;
#endif
}

// Once a frame while the page is up: a finished install is read once, said on
// the page and in the log, and followed by CHECK AGAIN so the radio appears.
void AppWindow::radioSetupPollDriverInstall() {
    // (A synthetic machine's install finishes by the clock, here, once a frame.)
    if (driverInstall_ && driverInstall_->fake && !driverInstall_->done.load() && std::chrono::steady_clock::now() >= driverInstall_->fakeDoneAt) {
        driverInstall_->done = true;
    }
    if (!driverInstall_ || !driverInstall_->done.load() || driverInstall_->consumed) { return; }
    driverInstall_->consumed = true;
    const wu::Result r = driverInstall_->launchResult;
    driverInstallStatus_ = wu::resultSentence(r);
    cascade::core::diagLogf("radio setup: driver install %s - %s", wu::outcomeName(r.outcome),
                            r.detail.c_str());
    if (r.outcome == wu::Outcome::Installed || r.outcome == wu::Outcome::AlreadyWinUsb) {
        radioSetupCheckAgain();
    }
}

void AppWindow::radioSetupFrame() {
    radioSetupPollDriverInstall();
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
    if ((uiState_.flags & FOXAPI_RX_DEVICE_OPENING) != 0u || (uiState_.sourceFlags & FOXAPI_SRC_SCAN_PENDING) != 0u) { return; }
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
        // The Refresh key's op: the native walk always, the SoapySDR half deferring itself, and saying so once, while a radio is open.
        uiSubmit(cascade::core::cmd::make(FOXAPI_OP_SCAN_DEVICES));
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
    // ...and, after an install, its sentence above them (two lines' room).
    const float footerH = ImGui::GetFrameHeightWithSpacing() * 2.0f + style.ItemSpacing.y +
                          (driverInstallStatus_.empty()
                               ? 0.0f
                               : ImGui::GetTextLineHeightWithSpacing() * 2.0f);
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
            // THE ONE-CLICK INSTALL (0.99.88): offered where FoxSDR can do the
            // Zadig step itself - an RTL-SDR whose interface 0 is on the bus.
            // The Zadig instructions stay below it, for anyone who prefers them
            // or whose Windows refuses.
            if (!x.installTarget.empty()) {
                const bool busy = driverInstall_ && !driverInstall_->done.load();
                ImGui::BeginDisabled(busy);
                if (ImGui::Button(trId("INSTALL DRIVER"))) {
                    radioSetupInstallDriver(x.installTarget);
                }
                ImGui::EndDisabled();
                cascade::gui::census::note("radio-setup:install-driver");
                {
                    const ImVec2 k0 = ImGui::GetItemRectMin();
                    const ImVec2 k1 = ImGui::GetItemRectMax();
                    cascade::gui::census::rect("radio-setup:install-driver", index, k0.x, k0.y,
                                               k1.x, k1.y);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr("One Windows permission prompt, no Zadig. Or by hand:"));
            }
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
        if (!driverInstallStatus_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
            ImGui::TextWrapped("%s", driverInstallStatus_.c_str());
            ImGui::PopStyleColor();
            cascade::gui::census::note("radio-setup:install-status");
        }
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
