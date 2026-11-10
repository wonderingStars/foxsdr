// signal_publish.cpp - the SIGNAL bank's publisher (signal_publish.hpp): the host's models (signal_logic.hpp) as the window's generated structs, the
// window's gestures as the ImGui drawers' own commands. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "signal_publish.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#include "core/config_halves.hpp"
#include "core/device_scan_plan.hpp"
#include "core/diag_log.hpp"
#include "core/disk_job.hpp"
#include "core/engine_vocab.hpp"
#include "core/file_pick.hpp"
#include "core/freq_converter.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/radio_setup.hpp"
#include "core/thread_beats.hpp"
#include "core/utf8_text.hpp"
#include "core/winusb_install.hpp"
#include "gui/radio_setup_inventory.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
// clang-format on
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace foxsdr::slintsignal {

namespace rs = cascade::core::radiosetup;
namespace wu = cascade::core::winusb;
using cascade::i18n::tr;

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

::ChipTone tone(Tone t) {
    switch (t) {
        case Tone::Good: return ::ChipTone::Good;
        case Tone::Warn: return ::ChipTone::Warn;
        case Tone::Err: return ::ChipTone::Err;
        default: return ::ChipTone::Neutral;
    }
}

// ---- the signature of a host model: every field the mapping reads, joined ----
void put(std::string& s, const std::string& v) { s += v; s += '\x1f'; }
void put(std::string& s, bool v) { s += v ? '1' : '0'; s += '\x1f'; }
void put(std::string& s, int v) { s += std::to_string(v); s += '\x1f'; }
void put(std::string& s, double v) { char b[40]; std::snprintf(b, sizeof(b), "%.5g", v); s += b; s += '\x1f'; }
void put(std::string& s, float v) { put(s, static_cast<double>(v)); }
void put(std::string& s, Tone v) { put(s, static_cast<int>(v)); }
void put(std::string& s, const SelectItem& v) { put(s, v.label); put(s, v.id); put(s, v.enabled); put(s, v.note); }
void put(std::string& s, const GainStage& v) { put(s, v.name); put(s, v.text); put(s, v.norm); put(s, v.enabled); put(s, v.unit); put(s, v.lo); put(s, v.hi); }
void put(std::string& s, const OptionRow& v) { put(s, v.name); put(s, v.label); put(s, v.on); put(s, v.enabled); put(s, v.tooltip); }
void put(std::string& s, const UnboundRow& v) { put(s, v.name); put(s, v.family); put(s, v.vid); put(s, v.pid); put(s, v.advice); }
void put(std::string& s, const SetupKey& v) { put(s, v.id); put(s, v.label); put(s, v.enabled); put(s, v.tooltip); }
template <class T> void put(std::string& s, const std::vector<T>& v) {
    s += '[';
    for (const T& x : v) { put(s, x); }
    s += ']';
}
template <class... A> std::string cat(const A&... a) {
    std::string s;
    (put(s, a), ...);
    return s;
}

std::vector<::SelectItem> items(const std::vector<SelectItem>& v) {
    std::vector<::SelectItem> out;
    out.reserve(v.size());
    for (const SelectItem& s : v) {
        ::SelectItem o{};
        o.label = ss(s.label);
        o.id = ss(s.id);
        o.enabled = s.enabled;
        o.note = ss(s.note);
        out.push_back(std::move(o));
    }
    return out;
}

// "Device stopped: ...\nReconnect it ..." -> the red line and the dim advice under it (the UI has a field for each).
void splitFirstLine(const std::string& both, std::string& first, std::string& rest) {
    const std::size_t nl = both.find('\n');
    if (nl == std::string::npos) { first = both; rest.clear(); return; }
    first = both.substr(0, nl);
    rest = both.substr(nl + 1);
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) { ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); }
    return w;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
template <class T>
std::shared_ptr<slint::Model<T>> SignalPublisher::Synced<T>::set(const std::vector<T>& rows) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i < model->row_count()) {
            if (!(*model->row_data(i) == rows[i])) { model->set_row_data(i, rows[i]); }
        } else {
            model->push_back(rows[i]);
        }
    }
    while (model->row_count() > rows.size()) { model->erase(model->row_count() - 1u); }
    return model;
}

struct SignalPublisher::Models {
    Synced<::SelectItem> devices, rates, antennas, cards, cardRates, decimations, sinkDevices, sinkBuffers, importKinds;
    Synced<::GainStage> stages;
    Synced<::OptionRow> options;
    Synced<slint::SharedString> setupLines;
    Synced<::UnboundRow> unbound;
    Synced<::SetupKey> keys;
};

// THE RADIO SETUP PAGE'S STATE: the probe (a worker with a time budget), what it found, and the one-click driver install's job.
struct SignalPublisher::SetupState {
    rs::Probe probe;
    bool started = false;
    std::string fake;   // FOXSDR_FAKE_RADIO_SETUP: a synthetic machine (bounded runs)
    struct Install {
        std::atomic<bool> done{false};
        bool consumed = false;
        std::string resultPath;
        bool haveLaunchResult = false;
        wu::Result launchResult;
    };
    std::shared_ptr<Install> install;
    std::string installStatus;
};

struct SignalPublisher::ImportState {
    struct Lookup { std::string tried; };
    cascade::core::DiskJob<Lookup> lookup{"Import file lookup"};
    std::string path;            // what the lookup in flight was asked to find
    std::string againPath;       // asked for while one was out
    bool again = false;
    bool seqKnown = false;
    unsigned long long seqSeen = 0;
};

SignalPublisher::SignalPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, const FoxEngineApi* api, FoxSession* session)
    : ui_(ui), kit_(kit), api_(api), session_(session), table_(api, session), setup_(std::make_unique<SetupState>()), import_(std::make_unique<ImportState>()),
      lists_(std::make_unique<Models>()) {
    if (const char* fake = std::getenv("FOXSDR_FAKE_RADIO_SETUP"); fake != nullptr && fake[0] != '\0') { setup_->fake = fake; }
    // The stored "Don't show this again" tick, from the interface's half of the settings the engine serves (only when there is one).
    std::string document;
    if (table_.fact("fact.config.interface", document) && !document.empty()) {
        cascade::core::AppConfig half;
        std::string error;
        if (cascade::core::parseInterfaceHalf(document, half, error)) { opts_.setup_dont_show = half.radioSetupDontShow; }
    }
}

SignalPublisher::~SignalPublisher() = default;

double SignalPublisher::nowS() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string SignalPublisher::fact(const std::string& key) {
    std::string out;
    if (!table_.fact(key, out)) { out.clear(); }
    return out;
}

void SignalPublisher::send(const Command& c) {
    forceRead_ = true;
    if (!c.valid) { return; }
    if (kit_.submit(c.q) != 0u) { ++sent_; }
}

void SignalPublisher::sendAll(const std::vector<Command>& cs) {
    for (const Command& c : cs) { send(c); }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the Radio setup page

void SignalPublisher::startProbe() {
    if (!setup_->fake.empty()) {
        const std::string name = setup_->fake;
        cascade::core::diagLogf("radio setup: FOXSDR_FAKE_RADIO_SETUP=%s - a synthetic machine, the bus is not read", name.c_str());
        setup_->probe.start([name]() {
            rs::Inventory inv;
            if (!rs::fakeInventory(name, inv)) {
                inv.probed = true;
                inv.usbListed = false;
                inv.note = "FOXSDR_FAKE_RADIO_SETUP names no known machine";
            }
            return inv;
        });
    } else {
        cascade::core::diagLogf("radio setup: checking the USB bus and what is installed for it");
        setup_->probe.start(&rs::collectInventory);
    }
    setup_->started = true;
}

void SignalPublisher::pollSetup() {
    SetupState& s = *setup_;
    // a finished one-click install is read once: said on the page and followed by CHECK AGAIN so the radio appears
    if (s.install && s.install->done.load() && !s.install->consumed) {
        s.install->consumed = true;
        const wu::Result r = s.install->launchResult;
        s.installStatus = wu::resultSentence(r);
        cascade::core::diagLogf("radio setup: driver install %s - %s", wu::outcomeName(r.outcome), r.detail.c_str());
        if (r.outcome == wu::Outcome::Installed || r.outcome == wu::Outcome::AlreadyWinUsb) {
            if (s.fake.empty()) { send(opSetupCheckAgain()); }
            startProbe();
        }
    }
    if (s.probe.poll() && s.probe.state() == rs::Probe::State::TimedOut) {
        cascade::core::diagWarnf("radio setup: the probe did not finish in %lld s - not waiting for it any longer", static_cast<long long>(rs::kProbeBudget.count() / 1000));
    }
    opts_.setup_checking = s.probe.running();
    opts_.setup_have_answer = s.probe.finished() > 0;
    opts_.setup_installing = s.install && !s.install->done.load();
    opts_.setup_install_status = s.installStatus;
    opts_.findings.clear();
    opts_.setup_show_page = false;
    if (!opts_.setup_shown || !opts_.setup_have_answer) { return; }
    // what the probe saw, plus what the application's own lists hold now (app_window_radio_setup.cpp radioSetupInventory)
    rs::Inventory inv = s.probe.inventory();
    if (s.fake.empty() && inv.probed && inv.usbListed) {
        cascade::gui::RadioSetupLists lists;
        const std::int32_t n = std::max<std::int32_t>(0, table_.readList(FOXAPI_LIST_DEVICES, 0, nullptr));
        for (std::int32_t i = 0; i < n; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (table_.readList(FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i), &it) <= i) { break; }
            if (i < cascade::core::kNativeRowBase) { continue; }
            const bool soapy = std::strncmp(it.detail, "soapy:", 6) == 0;
            const bool pluto = std::strncmp(it.detail, "pluto:", 6) == 0;
            const bool rtltcp = std::strncmp(it.detail, "rtltcp:", 7) == 0;
            std::string driver;
            if (soapy) {
                driver = cascade::gui::detail::driverOf(std::string(it.detail + 6));
            } else {
                const char* colon = std::strchr(it.detail, ':');
                driver.assign(it.detail, colon != nullptr ? static_cast<std::size_t>(colon - it.detail) : std::strlen(it.detail));
            }
            lists.rows.push_back({driver, pluto || rtltcp});
        }
        const std::uint32_t src = state_.sourceFlags;
        lists.deviceOpen = (state_.flags & FOXAPI_RX_DEVICE_OPEN) != 0u;
        lists.patchHeldRadios = static_cast<int>(opts_.patch_held.size());
        lists.soapyScanned = (src & FOXAPI_SRC_SCANNED) != 0u;
        lists.soapyScanPartial = (src & FOXAPI_SRC_SCAN_PARTIAL) != 0u;
        lists.soapyScanPending = (src & FOXAPI_SRC_SCAN_PENDING) != 0u;
        lists.uhdNoImages = fact("fact.source.uhdNoImages") == "1";
        inv = cascade::gui::radioSetupInventoryFrom(std::move(inv), lists);
    }
    const rs::Findings f = rs::assess(inv);
    opts_.setup_show_page = f.showPage;
    for (const rs::Finding& x : f.items) {
        SetupFinding o;
        o.title = x.title;
        o.explanation = x.explanation;
        o.install = x.install;
        o.url = x.url;
        o.install_target = x.installTarget;
        o.usable = x.kind == rs::Kind::Usable;
        o.not_checked = x.kind == rs::Kind::NotChecked;
        opts_.findings.push_back(std::move(o));
    }
}

// INSTALL DRIVER: `foxsdr-slint --install-winusb <id> --result <file>` through the shell's "runas" verb - one Windows permission prompt - and the wait for it,
// both on a thread of their own (the prompt is a person reading a dialog). The job outlives the window harmlessly: the thread touches only its own state.
void SignalPublisher::startInstall(const std::string& instanceId) {
    SetupState& s = *setup_;
    if (s.install && !s.install->done.load()) { return; }
    auto job = std::make_shared<SetupState::Install>();
    s.install = job;
    s.installStatus = tr("Installing the driver - answer the Windows permission prompt...");
    cascade::core::diagLogf("radio setup: one-click driver install requested");   // never the instance id: it carries the dongle's serial
    // A synthetic machine has nothing to install onto, and a test run must never put a permission prompt on somebody's desktop.
    if (!s.fake.empty()) {
        job->launchResult.outcome = wu::Outcome::NotFound;
        job->launchResult.detail = "a synthetic machine: nothing was started";
        job->haveLaunchResult = true;
        job->done = true;
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
    std::snprintf(name, sizeof(name), "foxsdr-winusb-%lu-%llu.txt", static_cast<unsigned long>(::GetCurrentProcessId()), static_cast<unsigned long long>(::GetTickCount64()));
    const std::wstring resultW = std::wstring(tempDir) + std::wstring(name, name + std::strlen(name));
    {
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, resultW.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string r(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) { ::WideCharToMultiByte(CP_UTF8, 0, resultW.c_str(), -1, r.data(), n, nullptr, nullptr); }
        job->resultPath = r;
    }
    const std::string params = wu::helperParameters(instanceId, job->resultPath);
    if (params.empty()) {
        job->launchResult.outcome = wu::Outcome::Refused;
        job->launchResult.detail = "the device instance id could not be passed safely";
        job->haveLaunchResult = true;
        job->done = true;
        return;
    }
    const std::wstring paramsW = widen(params);
    const std::wstring exeW = exe;
    std::thread([job, paramsW, exeW]() {
        cascade::core::beats::Registration beat(cascade::core::beats::kThreadDriverInstall);
        beat.beat("launched");
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = exeW.c_str();
        sei.lpParameters = paramsW.c_str();
        sei.nShow = SW_HIDE;
        beat.wait(cascade::core::beats::kWaitInstaller);   // the prompt and the installer are a person's and somebody else's: a declared wait
        const bool ok = ::ShellExecuteExW(&sei) != FALSE;
        const DWORD launchError = ok ? 0 : ::GetLastError();
        if (!ok || sei.hProcess == nullptr) {
            job->launchResult.outcome = launchError == ERROR_CANCELLED ? wu::Outcome::Cancelled : wu::Outcome::Failed;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "the elevated installer did not start (Windows error %lu)", static_cast<unsigned long>(launchError));
            job->launchResult.detail = buf;
        } else {
            ::WaitForSingleObject(sei.hProcess, INFINITE);
            beat.beat("result");
            ::CloseHandle(sei.hProcess);
            wu::Result r;
            {
                std::ifstream in(job->resultPath, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (!wu::parseResult(text, r)) {
                    r.outcome = wu::Outcome::Failed;
                    r.detail = "the installer left no readable result";
                }
            }
            std::remove(job->resultPath.c_str());
            job->launchResult = r;
        }
        if (SUCCEEDED(com)) { ::CoUninitialize(); }
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

// A vendor link of the page: only an https address, opened by the shell.
void SignalPublisher::openLink(const std::string& url) {
    if (url.rfind("https://", 0) != 0) { return; }
    cascade::core::diagLogf("radio setup: opening %s", url.c_str());
#if defined(_WIN32)
    ::ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void SignalPublisher::raiseSetupForCapture() {
    opts_.setup_shown = true;
    startProbe();
    forceRead_ = true;
}

void SignalPublisher::raiseImportForCapture() {
    opts_.import_shown = true;
    opts_.import_reason.clear();
    forceRead_ = true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the import dialog

// The path typed, pasted, dropped or chosen (app_window_import_dialog.cpp importBookmarkFile for the frequency list, FOXAPI_OP_SELECT_SOURCE for an I/Q file).
void SignalPublisher::importPath(const std::string& typed) {
    const std::string p = cascade::core::cleanImportPath(typed);
    opts_.import_path = p;
    opts_.import_reason.clear();
    forceRead_ = true;
    if (p.empty()) { return; }
    if (!opts_.import_preset.empty()) {
        // A PRESET'S IMPORT is the Airband section's (app_window_import_dialog.cpp importBookmarkFile -> airbandImportFile): its rows take the preset's name and tick.
        // The section reads the file on its worker and says how it went in its own note; the dialog's job is done.
        if (presetImport_) { presetImport_(p, opts_.import_preset); }
        opts_.import_shown = false;
        opts_.import_preset.clear();
        return;
    }
    if (opts_.import_kind == 1) {
        send(opImportPath(p, 1));
        opts_.import_shown = false;   // the source is opened: the dialog's job is done
        return;
    }
    const std::filesystem::path given{std::u8string(p.begin(), p.end())};
    if (given.has_root_path()) {
        send(opImportPath(p, 0));   // a path with a root is opened as it stands; the engine reads it on a worker and the fact says what it found
        return;
    }
    // ANY OTHER PATH is looked for where an import box looks (the working directory, then the recordings folder): disk work, so on a worker; one lookup at
    // a time and the last one asked for while it is out is sent when it is back.
    ImportState& im = *import_;
    if (im.lookup.pending()) {
        im.againPath = p;
        im.again = true;
        return;
    }
    const std::string dir = fact("fact.recorder.dir");
    im.path = p;
    im.lookup.request([p, dir]() -> ImportState::Lookup {
        ImportState::Lookup o;
        (void)cascade::core::importFrequencyFileIn(p, dir, o.tried);
        return o;
    });
}

// The dialog, opened for a preset (the Airband Import key over an empty box) or plain (preset ""): the preset's rows join it, as the ImGui dialog's forPreset says.
void SignalPublisher::openImportForPreset(const std::string& preset) {
    opts_.import_shown = true;
    opts_.import_reason.clear();
    opts_.import_kind = 0;
    opts_.import_preset = preset;
    forceRead_ = true;
}

void SignalPublisher::importBrowse() {
    if (!cascade::core::filePickAvailable()) { return; }
    cascade::core::FilePickRequest rq;
    std::string recordDir = fact("fact.recorder.dir");
    if (opts_.import_kind == 0) {
        rq.title = tr("Choose a frequency list");
        rq.startFrom = cascade::core::pickStartPath(opts_.import_path, recordDir, "frequencies.xml");
        rq.lists = true;
        rq.listsName = tr("Frequency lists (*.xml, *.csv)");
    } else {
        rq.title = tr("Choose a file");
        rq.startFrom = cascade::core::pickStartPath(opts_.import_path, recordDir, "recording.wav");
    }
    rq.allFilesName = tr("All files (*.*)");
    std::string chosen;
    if (cascade::core::pickFile(rq, chosen) && !chosen.empty()) { importPath(chosen); }
}

void SignalPublisher::pollImport() {
    ImportState& im = *import_;
    ImportState::Lookup o;
    if (im.lookup.poll(o)) {
        if (im.lookup.threw() || o.tried.empty()) {
            const std::string reason = cascade::core::formatText(tr("the file \"%s\" could not be read"), im.path.c_str());
            opts_.import_reason = cascade::core::formatText(tr("Could not import: %s"), reason.c_str());
            opts_.import_shown = true;
        } else {
            send(opImportPath(o.tried, 0));
        }
        if (im.again) {
            im.again = false;
            std::string next;
            next.swap(im.againPath);
            importPath(next);
        }
    }
    // The engine's answer: an import it has finished reading (fact.bookmarks.import.seq moved). Rows close the dialog; none open it with the engine's note.
    const std::string seqText = fact("fact.bookmarks.import.seq");
    if (!seqText.empty()) {
        const unsigned long long seq = std::strtoull(seqText.c_str(), nullptr, 10);
        if (!im.seqKnown) {
            im.seqKnown = true;   // whatever was read before this window looked is not news
            im.seqSeen = seq;
        } else if (seq != im.seqSeen) {
            im.seqSeen = seq;
            if (fact("fact.bookmarks.import.ok") == "1") {
                opts_.import_shown = false;
                opts_.import_reason.clear();
            } else {
                opts_.import_reason = fact("fact.bookmarks.import.note");
                opts_.import_shown = true;
            }
        }
    }
    opts_.import_reading = im.lookup.pending() || fact("fact.bookmarks.importing") == "1";
}

// The clocks of the disk-wait lines: a read is out while its fact says so; the line shows from a second (diskWaitLine).
void SignalPublisher::updateClocks() {
    const double now = nowS();
    const auto clock = [&](bool on, double& since) {
        if (!on) { since = -1.0; return; }
        if (since < 0.0) { since = now; }
    };
    const std::string start = fact("fact.recorder.start");
    clock(fact("fact.source.iqOpening") == "1", iqOpenSince_);
    clock(recorderStartOf(start, Take::Iq) == 1, iqStartSince_);
    clock(recorderStartOf(start, Take::Audio) == 1, audioStartSince_);
    clock(opts_.import_reading, importSince_);
    opts_.iq_open_wait_s = iqOpenSince_ < 0.0 ? 0.0 : now - iqOpenSince_;
    opts_.iq_start_wait_s = iqStartSince_ < 0.0 ? 0.0 : now - iqStartSince_;
    opts_.audio_start_wait_s = audioStartSince_ < 0.0 ? 0.0 : now - audioStartSince_;
    opts_.import_wait_s = importSince_ < 0.0 ? 0.0 : now - importSince_;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the models

::SourceModel SignalPublisher::mapSource(const SourceModel& m) {
    ::SourceModel o{};
    o.device_label = ss(m.device_label);
    o.devices = lists_->devices.set(items(m.devices));
    o.device_index = m.device_index;
    o.refresh_enabled = m.refresh_enabled;
    o.look_for_usrps = m.look_for_usrps;
    o.setup_key_label = ss(m.setup_key_label);
    o.rate_text = ss(m.rate_text);
    o.rates = lists_->rates.set(items(m.rates));
    o.actual_rate = ss(m.actual_rate);
    o.antenna_text = ss(m.antenna_text);
    o.antennas = lists_->antennas.set(items(m.antennas));
    o.single_port = m.single_port;
    o.auto_gain = m.auto_gain;
    o.auto_gain_shown = m.auto_gain_shown;
    o.agc_note = ss(m.agc_note);
    std::vector<::GainStage> stages;
    for (const GainStage& g : m.stages) {
        ::GainStage x{};
        x.name = ss(g.name);
        x.text = ss(g.text);
        x.norm = g.norm;
        x.enabled = g.enabled;
        x.unit = ss(g.unit);
        stages.push_back(std::move(x));
    }
    o.stages = lists_->stages.set(stages);
    o.bias_shown = m.bias_shown;
    o.bias_on = m.bias_on;
    o.bias_enabled = m.bias_enabled;
    o.ppm_on = m.ppm_on;
    o.ppm_radio_line = ss(m.ppm_radio_line);
    o.ppm_text = ss(m.ppm_text);
    o.ppm_note = ss(m.ppm_note);
    o.ppm_refusal = ss(m.ppm_refusal);
    o.error_line = ss(m.error_line);
    o.mismatch_note = ss(m.mismatch_note);
    o.advisory = ss(m.advisory);
    o.advisory_tone = tone(m.advisory_tone);
    std::string stopped, advice;
    splitFirstLine(m.stopped_line, stopped, advice);
    o.stopped_line = ss(stopped);
    o.stopped_advice = ss(advice);
    o.variant = m.variant;
    o.iq_path = ss(m.iq_path);
    o.iq_note = ss(m.iq_note);
    o.pluto_address = ss(m.pluto_address);
    o.pluto_help = ss(m.pluto_help);
    o.rtltcp_address = ss(m.rtltcp_address);
    o.rtltcp_help = ss(m.rtltcp_help);
    o.card_busy = ss(m.card_busy);
    o.card_none = ss(m.card_none);
    o.cards = lists_->cards.set(items(m.cards));
    o.card_index = m.card_index;
    o.card_format_index = m.card_format_index;
    o.card_channel_index = m.card_channel_index;
    o.card_swap_iq = m.card_swap_iq;
    o.card_centre_mhz = ss(m.card_centre_mhz);
    o.card_rates = lists_->cardRates.set(items(m.card_rates));
    o.card_rate_index = m.card_rate_index;
    o.card_receives = ss(m.card_receives);
    o.card_can_open = m.card_can_open;   // greyed with no sound card chosen (docs 6.61): the effect sent nothing then
    o.card_warning = ss(m.card_warning);
    o.airspy_decimations = lists_->decimations.set(items(m.airspy_decimations));
    o.airspy_decimation_index = m.airspy_decimation_index;
    o.airspy_hardware_rate = ss(m.airspy_hardware_rate);
    o.airspy_gain_mode = m.airspy_gain_mode;
    o.airspy_gain_norm = m.airspy_gain_norm;
    o.airspy_lna_agc = m.airspy_lna_agc;
    o.airspy_mixer_agc = m.airspy_mixer_agc;
    o.airspy_lna = m.airspy_lna;
    o.airspy_mixer = m.airspy_mixer;
    o.airspy_vga = m.airspy_vga;
    std::vector<::OptionRow> options;
    for (const OptionRow& r : m.options) {
        ::OptionRow x{};
        x.name = ss(r.name);
        x.label = ss(r.label);
        x.on = r.on;
        x.enabled = r.enabled;
        x.tooltip = ss(r.tooltip);
        options.push_back(std::move(x));
    }
    o.options = lists_->options.set(options);
    o.converter_shown = m.converter_shown;
    o.converter_mode = m.converter_mode;
    o.converter_alias = ss(m.converter_alias);
    o.converter_lo = ss(m.converter_lo);   // "125 MHz": the host's converterHzText; the UI lights the quick key whose label equals it
    o.converter_inverts = m.converter_inverts;
    o.converter_error = ss(m.converter_error);
    o.converter_on_air = ss(m.converter_on_air);
    return o;
}

::RadioExtrasModel SignalPublisher::mapRadio(const RadioExtrasModel& m) {
    ::RadioExtrasModel o{};
    o.wfm = m.wfm;
    o.stereo = m.stereo;
    o.stereo_word = ss(m.stereo_word);
    o.stereo_tone = tone(m.stereo_tone);
    o.rds_shown = m.rds_shown;
    o.rds_ps = ss(m.rds_ps);
    o.rds_rt = ss(m.rds_rt);
    o.rds_pi = ss(m.rds_pi);
    return o;
}

::AudioFiltersModel SignalPublisher::mapFilters(const AudioFiltersModel& m) {
    ::AudioFiltersModel o{};
    o.chain_note = ss(m.chain_note);
    o.nr_on = m.nr_on;
    o.nr_strength = m.nr_strength;
    o.nr_text = ss(m.nr_text);
    o.notch_on = m.notch_on;
    o.notch_freq = m.notch_freq;
    o.notch_freq_text = ss(m.notch_freq_text);
    o.notch_q = m.notch_q;
    o.notch_q_text = ss(m.notch_q_text);
    o.auto_notch_on = m.auto_notch_on;
    o.auto_notch_status = ss(m.auto_notch_status);
    o.auto_notch_locked = m.auto_notch_locked;
    return o;
}

::SinksModel SignalPublisher::mapSinks(const SinksModel& m) {
    ::SinksModel o{};
    o.device_text = ss(m.device_text);
    o.devices = lists_->sinkDevices.set(items(m.devices));
    o.device_index = m.device_index;
    o.none_line = ss(m.none_line);
    o.health_note = ss(m.health_note);
    o.buffer_text = ss(m.buffer_text);
    o.buffers = lists_->sinkBuffers.set(items(m.buffers));
    o.buffer_index = m.buffer_index;
    o.lead_notice = ss(m.lead_notice);
    o.muted_by = ss(m.muted_by);
    o.muted_note = ss(m.muted_note);
    return o;
}

::TransmitModel SignalPublisher::mapTransmit(const TransmitModel& m) {
    ::TransmitModel o{};
    o.chip = ss(m.chip);
    o.tone = tone(m.tone);
    o.available = m.available;
    o.armed = m.armed;
    o.keyed = m.keyed;
    o.fault = ss(m.fault);
    return o;
}

::RecorderModel SignalPublisher::mapRecorder(const RecorderModel& m) {
    ::RecorderModel o{};
    o.folder = ss(m.folder);
    o.iq_label = ss(m.iq_label);
    o.iq_enabled = m.iq_enabled;
    o.iq_progress = ss(m.iq_progress);
    o.iq_running = m.iq_running;
    o.audio_label = ss(m.audio_label);
    o.audio_enabled = m.audio_enabled;
    o.audio_progress = ss(m.audio_progress);
    o.audio_running = m.audio_running;
    o.muted_warning = ss(m.muted_warning);
    o.hint = ss(m.hint);
    o.error_line = ss(m.error_line);
    o.notice = ss(m.notice);
    o.import_label = ss(m.import_label);
    return o;
}

::RadioSetupModel SignalPublisher::mapSetup(const RadioSetupModel& m) {
    ::RadioSetupModel o{};
    o.shown = m.shown;
    o.title = ss(m.title);
    std::vector<slint::SharedString> lines;
    for (const std::string& l : m.lines) { lines.push_back(ss(l)); }
    o.lines = lists_->setupLines.set(lines);
    std::vector<::UnboundRow> unbound;
    for (const UnboundRow& r : m.unbound) {
        ::UnboundRow x{};
        x.name = ss(r.name);
        x.family = ss(r.family);
        x.vid = ss(r.vid);
        x.pid = ss(r.pid);
        x.advice = ss(r.advice);
        unbound.push_back(std::move(x));
    }
    o.unbound = lists_->unbound.set(unbound);
    // The UI draws "Don't show this again" as its own tick (RadioSetupModel::dont-show), so the host's key of that id is not offered twice as a button.
    std::vector<::SetupKey> keys;
    for (const SetupKey& k : m.keys) {
        if (k.id == "dont-show") { continue; }
        ::SetupKey x{};
        x.id = ss(k.id);
        x.label = ss(k.label);
        x.enabled = k.enabled;
        x.tooltip = ss(k.tooltip);
        keys.push_back(std::move(x));
    }
    o.keys = lists_->keys.set(keys);
    o.busy = ss(m.busy);
    o.dont_show = opts_.setup_dont_show;
    return o;
}

::ImportDialogModel SignalPublisher::mapImport(const ImportDialogModel& m) {
    ::ImportDialogModel o{};
    o.shown = m.shown;
    o.path = ss(m.path);
    // The "Waiting for the disk..." line has no field of its own in the UI: it follows the engine's sentence in the one note.
    std::string note = m.error;
    if (!m.reading_line.empty()) { note += note.empty() ? m.reading_line : "\n" + m.reading_line; }
    o.error = ss(note);
    o.kinds = lists_->importKinds.set(items(m.kinds));
    o.kind_index = m.kind_index;
    o.preset = ss(m.preset);
    o.preset_line = ss(m.preset_line);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------------------
void SignalPublisher::publish(const FoxReceiverState& state) {
    state_ = state;
    const std::int64_t nowMs = static_cast<std::int64_t>(nowS() * 1000.0);
    pollSetup();
    pollImport();
    // The lists and facts are read at 10 Hz (and at once after a gesture): the ImGui window reads them every frame, this one does not need a thirtieth of a second.
    if (!forceRead_ && haveModels_ && nowMs - lastReadMs_ < 100) { return; }
    forceRead_ = false;
    lastReadMs_ = nowMs;
    updateClocks();
    SignalModels m = readSignal(table_, state, opts_);
    last_ = m;
    haveModels_ = true;
    if (sigs_.changed("source", cat(m.source.device_label, m.source.devices, m.source.device_index, m.source.refresh_enabled, m.source.look_for_usrps, m.source.setup_key_label,
                                    m.source.rate_text, m.source.rates, m.source.actual_rate, m.source.antenna_text, m.source.antennas, m.source.single_port,
                                    m.source.auto_gain, m.source.auto_gain_shown, m.source.agc_note, m.source.stages, m.source.bias_shown, m.source.bias_on,
                                    m.source.bias_enabled, m.source.ppm_on, m.source.ppm_radio_line, m.source.ppm_text, m.source.ppm_note, m.source.ppm_refusal,
                                    m.source.error_line, m.source.mismatch_note, m.source.advisory, m.source.advisory_tone, m.source.stopped_line, m.source.variant,
                                    m.source.iq_path, m.source.iq_note, m.source.pluto_address, m.source.pluto_help, m.source.rtltcp_address, m.source.rtltcp_help,
                                    m.source.card_busy, m.source.card_none, m.source.cards, m.source.card_index, m.source.card_format_index, m.source.card_channel_index,
                                    m.source.card_swap_iq, m.source.card_centre_mhz, m.source.card_rates, m.source.card_rate_index, m.source.card_receives, m.source.card_can_open,
                                    m.source.card_warning, m.source.airspy_decimations, m.source.airspy_decimation_index, m.source.airspy_hardware_rate,
                                    m.source.airspy_gain_mode, m.source.airspy_gain_norm, m.source.airspy_lna_agc, m.source.airspy_mixer_agc, m.source.airspy_lna,
                                    m.source.airspy_mixer, m.source.airspy_vga, m.source.options, m.source.converter_shown, m.source.converter_mode,
                                    m.source.converter_alias, m.source.converter_lo, m.source.converter_inverts, m.source.converter_error, m.source.converter_on_air))) {
        ui_.set_source_model(mapSource(m.source));
    }
    if (sigs_.changed("radio", cat(m.radio.wfm, m.radio.stereo, m.radio.stereo_word, m.radio.stereo_tone, m.radio.rds_shown, m.radio.rds_ps, m.radio.rds_rt, m.radio.rds_pi))) {
        ui_.set_radio_extras(mapRadio(m.radio));
    }
    if (sigs_.changed("filters", cat(m.filters.chain_note, m.filters.nr_on, m.filters.nr_strength, m.filters.nr_text, m.filters.notch_on, m.filters.notch_freq,
                                     m.filters.notch_freq_text, m.filters.notch_q, m.filters.notch_q_text, m.filters.auto_notch_on, m.filters.auto_notch_status,
                                     m.filters.auto_notch_locked))) {
        ui_.set_filters_model(mapFilters(m.filters));
    }
    if (sigs_.changed("sinks", cat(m.sinks.device_text, m.sinks.devices, m.sinks.device_index, m.sinks.none_line, m.sinks.health_note, m.sinks.buffer_text, m.sinks.buffers,
                                   m.sinks.buffer_index, m.sinks.lead_notice, m.sinks.muted_by, m.sinks.muted_note))) {
        ui_.set_sinks_model(mapSinks(m.sinks));
    }
    if (sigs_.changed("transmit", cat(m.transmit.chip, m.transmit.tone, m.transmit.available, m.transmit.armed, m.transmit.keyed, m.transmit.fault))) {
        ui_.set_transmit_model(mapTransmit(m.transmit));
    }
    if (sigs_.changed("recorder", cat(m.recorder.folder, m.recorder.iq_label, m.recorder.iq_enabled, m.recorder.iq_progress, m.recorder.iq_running, m.recorder.audio_label,
                                      m.recorder.audio_enabled, m.recorder.audio_progress, m.recorder.audio_running, m.recorder.muted_warning, m.recorder.hint,
                                      m.recorder.error_line, m.recorder.notice, m.recorder.import_label))) {
        ui_.set_recorder_model(mapRecorder(m.recorder));
    }
    if (sigs_.changed("setup", cat(m.setup.shown, m.setup.title, m.setup.lines, m.setup.unbound, m.setup.keys, m.setup.busy, opts_.setup_dont_show))) {
        ui_.set_radio_setup(mapSetup(m.setup));
    }
    if (sigs_.changed("import", cat(m.import.shown, m.import.path, m.import.error, m.import.kinds, m.import.kind_index, m.import.reading_line, m.import.preset, m.import.preset_line))) {
        ui_.set_import_dialog(mapImport(m.import));
    }
}

SignalPublisher::RowFace SignalPublisher::face(int row) const {
    RowFace f;
    if (!haveModels_) { return f; }
    switch (row) {
        case 1: f.chip = last_.source.chip; f.tone = tone(last_.source.chip_tone); break;
        case 3: f.chip = last_.filters.chip; f.tone = last_.filters.lamp_lit ? ::ChipTone::Good : ::ChipTone::Neutral; break;
        case 4: f.chip = last_.sinks.chip; f.tone = tone(last_.sinks.chip_tone); break;
        case 5: f.chip = last_.transmit.chip; f.tone = tone(last_.transmit.tone); break;
        case 6: f.chip = last_.recorder.chip; f.tone = tone(last_.recorder.chip_tone); break;
        case 7: f.chip = last_.source.ppm_chip; f.tone = last_.source.ppm_on ? ::ChipTone::Good : ::ChipTone::Neutral; break;
        default: return f;
    }
    f.valid = !f.chip.empty();
    return f;
}

std::string SignalPublisher::summaryLine() const {
    if (!haveModels_) { return "signal: no models read"; }
    return "signal: source chip=" + last_.source.chip + " label=\"" + last_.source.device_label + "\" devices=" + std::to_string(last_.source.devices.size()) + " variant=" +
           std::to_string(last_.source.variant) + " rate=\"" + last_.source.rate_text + "\" rates=" + std::to_string(last_.source.rates.size()) + " stages=" +
           std::to_string(last_.source.stages.size()) + "; sinks device=\"" + last_.sinks.device_text + "\" devices=" + std::to_string(last_.sinks.devices.size()) +
           " chip=" + last_.sinks.chip + "; recorder iq=\"" + last_.recorder.iq_label + "\" audio=\"" + last_.recorder.audio_label + "\" chip=" + last_.recorder.chip +
           "; filters chip=" + last_.filters.chip + "; transmit chip=" + last_.transmit.chip + "; ppm chip=" + last_.source.ppm_chip + "; commands sent=" + std::to_string(sent_);
}

// ---------------------------------------------------------------------------------------------------------------------------------
void SignalPublisher::wire() {
    auto&& ev = ui_.global<::SignalEvents>();
    // ---- Source ----
    ev.on_source_device_selected([this](int i) { send(opDeviceSelected(table_, last_.source, i)); });
    ev.on_source_refresh([this] { if (last_.source.refresh_enabled) { send(opRefresh()); } });
    ev.on_source_usrps([this](bool on) { send(opUsrps(on)); });
    ev.on_source_setup([this] { opts_.setup_shown = true; startProbe(); forceRead_ = true; });   // [local]: shows the page and starts the probe
    ev.on_source_rate_selected([this](int i) { send(opRateSelected(last_.source, i)); });
    ev.on_source_antenna_selected([this](int i) { send(opAntennaSelected(last_.source, i)); });
    ev.on_source_auto_gain([this](bool on) { send(opAutoGain(on)); });
    ev.on_source_gain_changed([this](int stage, float norm) { send(opGainChanged(last_.source, stage, norm)); });
    // The bias tee: ON asks the question (the window's own sheet, whose "Turn it on" is on_bias_tee_confirmed below), OFF switches off at once.
    ev.on_source_bias([this](bool on) {
        if (on) { ui_.set_bias_question_open(true); } else { send(opBiasTee(false, std::string())); }
    });
    ui_.on_bias_tee_clicked([this] { send(opBiasTee(false, std::string())); });
    ui_.on_bias_tee_confirmed([this] { send(opBiasTee(true, fact("fact.biasTee.radioKey"))); });
    ev.on_ppm_toggled([this](bool on) { send(opPpmToggled(on, last_.source.ppm_value)); });
    ev.on_ppm_committed([this](slint::SharedString t) { send(opPpmCommitted(std::string(t), last_.source.ppm_value)); });
    // ---- the variants ----
    ev.on_iq_path_committed([this](slint::SharedString p) { opts_.iq_path = std::string(p); forceRead_ = true; });   // [local]: the draft
    ev.on_iq_open([this] { send(opIqOpen(opts_.iq_path.empty() ? last_.source.iq_path : opts_.iq_path)); });
    ev.on_pluto_address_committed([this](slint::SharedString t) { plutoDraft_ = std::string(t); plutoDraftSet_ = true; send(opPlutoAddress(plutoDraft_)); });
    ev.on_pluto_open([this] { send(opPlutoOpen(plutoDraftSet_ ? plutoDraft_ : last_.source.pluto_address)); });
    ev.on_rtltcp_address_committed([this](slint::SharedString t) { rtltcpDraft_ = std::string(t); rtltcpDraftSet_ = true; send(opRtlTcpAddress(rtltcpDraft_)); });
    ev.on_rtltcp_open([this] { send(opRtlTcpOpen(rtltcpDraftSet_ ? rtltcpDraft_ : last_.source.rtltcp_address)); });
    ev.on_card_selected([this](int i) { send(opCardSelected(last_.source, i)); });
    ev.on_card_format([this](int i) { send(opCardFormat(i)); });
    ev.on_card_channel([this](int i) { send(opCardChannel(i)); });
    ev.on_card_swap([this](bool on) { send(opCardSwap(on)); });
    ev.on_card_centre_committed([this](slint::SharedString t) { sendAll(opCardCentre(std::string(t))); });   // two commands, in order
    ev.on_card_rate_selected([this](int i) { send(opCardRate(last_.source, i)); });
    ev.on_card_open([this] { send(opCardOpen(last_.source)); });
    ev.on_airspy_decimation([this](int i) { send(opAirspyDecimation(last_.source, i)); });
    ev.on_airspy_gain_mode([this](int i) { send(opAirspyGainMode(last_.source, i)); });
    ev.on_airspy_gain([this](float n) { send(opAirspyGain(last_.source, n)); });
    ev.on_airspy_lna_agc([this](bool on) { send(opAirspyAgc(false, on)); });
    ev.on_airspy_mixer_agc([this](bool on) { send(opAirspyAgc(true, on)); });
    ev.on_airspy_lna([this](float n) { send(opAirspyStage(last_.source, "LNA", n)); });
    ev.on_airspy_mixer([this](float n) { send(opAirspyStage(last_.source, "MIXER", n)); });
    ev.on_airspy_vga([this](float n) { send(opAirspyStage(last_.source, "VGA", n)); });
    ev.on_option_toggled([this](int i, bool on) { send(opOptionToggled(last_.source, i, on)); });
    // ---- the converter ----
    ev.on_converter_mode([this](int code) { send(opConverterMode(converterStored(last_.source), code)); });
    ev.on_converter_lo_committed([this](slint::SharedString t) {
        const std::string text(t);
        double hz = 0.0;
        if (!cascade::core::parseConverterLoHz(text, hz)) {
            opts_.converter_lo_bad = text;   // nothing is sent; the sentence stays until the next good commit
            forceRead_ = true;
            return;
        }
        opts_.converter_lo_bad.clear();
        send(opConverterLo(converterStored(last_.source), text));
    });
    // The three quick keys are 125 MHz, 100 MHz and 2 MHz by index, the host's own table (the ImGui window's kQuickLoHz).
    ev.on_converter_quick([this](int i) {
        opts_.converter_lo_bad.clear();
        send(opConverterQuick(converterStored(last_.source), i));
    });
    ev.on_converter_inverts([this](bool on) { send(opConverterInverts(converterStored(last_.source), on)); });
    // ---- Radio, Audio filters, Sinks, Transmit ----
    ev.on_radio_stereo([this](bool on) { send(opStereo(on)); });
    ev.on_filter_nr([this](bool on) { send(opNr(on)); });
    ev.on_filter_nr_strength([this](float n) { send(opNrStrength(n)); });
    ev.on_filter_notch([this](bool on) { send(opNotch(on)); });
    ev.on_filter_notch_freq([this](float n) { send(opNotchFreq(n)); });
    ev.on_filter_notch_q([this](float n) { send(opNotchQ(n)); });
    ev.on_filter_auto_notch([this](bool on) { send(opAutoNotch(on)); });
    ev.on_sink_device_selected([this](int i) { send(opSinkDevice(last_.sinks, i)); });
    ev.on_sink_buffer_selected([this](int i) { send(opSinkBuffer(last_.sinks, i)); });
    ev.on_transmit_clicked([this] { txPageOpen_ = !txPageOpen_; send(opTransmitPage(txPageOpen_)); });   // the open flag is this object's
    // ---- Recorder ----
    ev.on_recorder_iq([this] { send(opRecorderIq(last_.recorder)); });
    ev.on_recorder_audio([this] { send(opRecorderAudio(last_.recorder)); });
    ev.on_recorder_import([this] { openImportForPreset(std::string()); });   // [local]: the import dialog (the plain frequency-list import)
    // ---- the Radio setup page ----
    ev.on_setup_key([this](slint::SharedString id) {
        const std::string key(id);
        if (key == "check-again") {
            if (setup_->fake.empty()) { send(opSetupCheckAgain()); }
            startProbe();
        } else if (key.rfind("install-driver:", 0) == 0) {
            startInstall(key.substr(15));
        } else if (key == "dont-show") {
            opts_.setup_dont_show = !opts_.setup_dont_show;
        } else if (key.rfind("link:", 0) == 0) {
            openLink(key.substr(5));
        }
        forceRead_ = true;
    });
    ev.on_setup_closed([this] { opts_.setup_shown = false; forceRead_ = true; });
    ev.on_setup_dont_show([this](bool on) { opts_.setup_dont_show = on; forceRead_ = true; });   // not saved yet: see the report (the interface half is the duties')
    // ---- the import dialog ----
    ev.on_import_path_committed([this](slint::SharedString t) { importPath(std::string(t)); });
    ev.on_import_kind([this](int i) { opts_.import_kind = std::clamp(i, 0, 1); opts_.import_reason.clear(); forceRead_ = true; });
    ev.on_import_open([this] { importBrowse(); });
    ev.on_import_closed([this] { opts_.import_shown = false; opts_.import_reason.clear(); opts_.import_preset.clear(); forceRead_ = true; });
}

}  // namespace foxsdr::slintsignal
