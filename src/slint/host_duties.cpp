// The Slint host's housekeeping; see host_duties.hpp for what it is, what it mirrors and what it leaves out.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "host_duties.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "core/app_commands.hpp"
#include "core/config_halves.hpp"
#include "core/config_lock.hpp"
#include "core/console_close.hpp"
#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "core/package_identity.hpp"
#include "core/sentinel.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace foxsdr::slinthost {

namespace core = cascade::core;

namespace {

bool samePolicy(const core::UploadPolicyState& a, const core::UploadPolicyState& b) {
    return a.recent == b.recent && a.windowStart == b.windowStart && a.windowCount == b.windowCount && a.blockedUntil == b.blockedUntil;
}

// The phrase for the size of an offered installer, as the ImGui banner words it (drawUpdateBanner).
std::string sizeNote(std::uint64_t bytes) {
    return cascade::core::formatText(cascade::i18n::tr("%.1f MB, verified against its published checksum before anything runs"), static_cast<double>(bytes) / 1.0e6);
}

#if defined(_WIN32)
// THE DOWNLOADED INSTALLER, STARTED THE WAY THE FOXSDR WINDOW STARTS IT (app_window.cpp launchInstaller, gui/shell_open.*), and not
// through those: this program links none of the GUI library. ShellExecute rather than CreateProcess, because the installer asks for
// elevation through its manifest and only the shell shows that prompt; the call does not return until the prompt has been answered, so
// the caller runs it on a worker. The path is WIDENED THE WAY IT WAS NARROWED (core::downloadUpdate returns fs::path::string(), which
// MSVC narrows through the ANSI code page): a byte-for-byte copy into wchar_t made "Install now and restart" fail for every account
// whose profile folder is not plain ASCII (the 2026-09-24 fix, gui/shell_open.hpp installerPathWide). COM is initialised on the worker
// as the shell's extensions may need it. EMPTY path text, or a path that cannot be decoded, is "could not start", not a launch of nothing.
bool shellOpenInstaller(const std::string& path) {
    std::wstring wide;
    try {
        wide = std::filesystem::path(path).wstring();
    } catch (...) {
        return false;
    }
    if (wide.empty()) { return false; }
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    // The worker exits after this call; finish any asynchronous shell activation first.
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = wide.c_str();
    sei.nShow = SW_SHOWNORMAL;
    const bool started = ::ShellExecuteExW(&sei) != FALSE;
    if (SUCCEEDED(com)) { ::CoUninitialize(); }
    return started;
}
#endif

}  // namespace

// ---- who this program is ----------------------------------------------------------------------------------------------------
std::string programLine(const std::string& version) {
    return std::string("program: ") + kProgramId + " (" + kProgramExe + " " + version + ")";
}

// ---- the bounded run --------------------------------------------------------------------------------------------------------
std::string BoundedRun::exitLine(int exitCode) const {
    return std::string("foxsdr-slint: bounded run: ") + std::to_string(ticks_) + " of " + std::to_string(frames_) + " ticks, exit " +
           std::to_string(exitCode);
}

// ---- the settings file's lock -----------------------------------------------------------------------------------------------
// The shape core/config_lock.cpp's holder writes and its reader accepts: "role=<name> pid=<n>\n", the role a short run of [a-z-].
unsigned long holderPid(const std::string& text) {
    const std::size_t roleAt = text.find("role=");
    const std::size_t pidAt = text.find(" pid=");
    if (roleAt != 0 || pidAt == std::string::npos || pidAt < 5) { return 0; }
    const std::string role = text.substr(5, pidAt - 5);
    if (role.empty() || role.size() > 32) { return 0; }
    for (const char c : role) {
        if (!((c >= 'a' && c <= 'z') || c == '-')) { return 0; }
    }
    char* end = nullptr;
    return std::strtoul(text.c_str() + pidAt + 5, &end, 10);
}

unsigned long currentProcessId() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

LockReport lockReportFor(bool asked, bool accepted, const std::string& refusal, const std::string& holderText, unsigned long ownPid) {
    LockReport r;
    if (!asked) { return r; }
    if (!accepted) {
        r.state = LockState::Refused;
        r.sentence = refusal;
        return r;
    }
    const unsigned long pid = holderPid(holderText);
    if (pid == 0) {
        r.state = LockState::Unknown;
    } else {
        r.state = pid == ownPid ? LockState::Held : LockState::Elsewhere;
    }
    return r;
}

std::string lockLine(const LockReport& report) {
    switch (report.state) {
        case LockState::NotTaken: return "not taken (a bounded run, or a run with no settings file)";
        case LockState::Held: return "held by this window";
        case LockState::Elsewhere: return "held by another FoxSDR program; this window carries on without it";
        case LockState::Unknown: return "asked for; who holds it could not be read";
        case LockState::Refused: return "refused: " + report.sentence;
    }
    return std::string();
}

std::string readLockHolder(const std::string& configPath) {
    if (configPath.empty()) { return std::string(); }
    std::ifstream in(std::filesystem::path(core::ConfigLock::lockPathFor(configPath)), std::ios::binary);
    if (!in) { return std::string(); }
    char buf[128] = {0};
    in.read(buf, sizeof(buf) - 1);
    return std::string(buf, static_cast<std::size_t>(in.gcount()));
}

// ---- the About text ---------------------------------------------------------------------------------------------------------
std::string aboutText(const AboutFacts& f) {
    std::string t = "The Slint window over the FoxSDR engine.\n";
    t += "Version: " + f.version + "\n";
    t += std::string("Program: ") + kProgramId + " (" + kProgramExe + ")\n";
    t += "Settings file: " + (f.configPath.empty() ? std::string("none: this run keeps no settings file") : f.configPath) + "\n";
    t += "Diagnostics folder: " + (f.diagnosticsDir.empty() ? std::string("none: this run does not write diagnostics") : f.diagnosticsDir) + "\n";
    t += "Settings lock: " + lockLine(f.lock);
    return t;
}

// ---- the decisions ----------------------------------------------------------------------------------------------------------
UpdatePlan updatePlanFor(bool bounded, bool manifestSeam, bool packaged, bool userEnabled) {
    if (bounded && !manifestSeam) { return UpdatePlan::Hermetic; }
    switch (core::updateCheckDisposition(packaged, userEnabled)) {
        case core::UpdateCheckDisposition::OffByChoice: return UpdatePlan::OffByChoice;
        case core::UpdateCheckDisposition::AskStore: return UpdatePlan::StoreDeferred;
        case core::UpdateCheckDisposition::Run: break;
    }
    return UpdatePlan::Run;
}

const char* updatePlanLine(UpdatePlan plan) {
    switch (plan) {
        case UpdatePlan::Run: return "update: asking foxsdr.com whether a newer FoxSDR exists";
        case UpdatePlan::OffByChoice: return "update: asking nobody (the Updates tick is off)";
        case UpdatePlan::StoreDeferred:
            return "update: this copy came from the Microsoft Store; its own check and install are a later phase of the Slint window, so "
                   "nobody is asked (foxsdr.com never is)";
        case UpdatePlan::Hermetic: return "update: not asking (a bounded run stays hermetic)";
    }
    return "";
}

SweepPlan sweepPlanFor(bool diagnosticsEnabled, const std::string& diagnosticsDir) {
    if (!diagnosticsEnabled) { return SweepPlan::OffBySwitch; }
    if (diagnosticsDir.empty()) { return SweepPlan::NoFolder; }
    return SweepPlan::Armed;
}

const char* sweepPlanLine(SweepPlan plan) {
    switch (plan) {
        case SweepPlan::Armed: return "crash upload: armed (it starts when the engine has published its first facts)";
        case SweepPlan::OffBySwitch: return "crash upload: off (the Diagnostics switch is off)";
        case SweepPlan::NoFolder: return "crash upload: off (this run may not write diagnostics)";
    }
    return "";
}

// ---- the update card --------------------------------------------------------------------------------------------------------
UpdateCardModel updateCardFor(const UpdateView& v) {
    UpdateCardModel c;
    // HIDDEN, as drawUpdateBanner hides it: the Updates tick is off, "Not now" was pressed, a Store copy (its own banner), or nothing
    // newer is offered. A failed CHECK is never shown either: the user did not ask.
    if (!v.enabled || v.dismissed || v.packaged || !v.info.newer) { return c; }
    c.shown = true;
    c.critical = v.info.critical;
    // Amber for an ordinary update, red for one that fixes a build which could not do its job: the server's `critical` flag.
    c.headline = v.info.critical ? cascade::core::formatText(cascade::i18n::tr("Important update: FoxSDR %s"), v.info.version.c_str()) : cascade::core::formatText(cascade::i18n::tr("%s is available"), ("FoxSDR " + v.info.version).c_str());
    // WHAT IT FIXES, in the author's words, for every release between the one running and the one offered: a version, then its lines.
    for (const core::ReleaseNote& n : v.info.notes) {
        c.notes.push_back(n.version + (n.critical ? std::string(cascade::i18n::tr("  (important)")) : std::string()));
        for (const std::string& line : n.notes) { c.notes.push_back(line); }
    }
    if (!v.installerHere) {
        c.note = cascade::i18n::tr("There is no in-app updater for Linux yet - download it from foxsdr.com.");
        c.primary = cascade::i18n::tr("Open foxsdr.com");
    } else {
        switch (v.stage) {
            case UpdateStage::Idle:
                c.primary = cascade::i18n::tr("Download update");
                if (v.info.sizeBytes > 0) { c.note = sizeNote(v.info.sizeBytes); }
                break;
            case UpdateStage::Downloading:
                c.primary = cascade::i18n::tr("Downloading...");
                c.progress = std::clamp(v.progress, 0.f, 1.f);
                break;
            case UpdateStage::Ready:
                // Downloaded AND verified. Running it closes this window, which the key says outright: an installer cannot replace a
                // binary that is still running.
                c.primary = cascade::i18n::tr("Install now and restart");
                c.note = cascade::i18n::tr("Downloaded and verified.");
                break;
            case UpdateStage::Installing: c.primary = cascade::i18n::tr("Starting the installer..."); break;
        }
    }
    c.error = v.error;
    return c;
}

// ---- the duties -------------------------------------------------------------------------------------------------------------
HostDuties::HostDuties(DutyWorld& world, std::function<void()> requestClose) : w_(world), requestClose_(std::move(requestClose)) {}

void HostDuties::start(const DutyStart& s) {
    cfg_ = s;
    const bool bounded = s.frames >= 0;

    // WHO THIS IS, in the log first (the ring is what a report carries) and on stderr.
    const std::string program = programLine(s.version);
    w_.say(program);
    w_.diag(program);
    w_.say("settings: " + (s.configPath.empty() ? std::string("no settings file (a hermetic run)") : s.configPath));
    w_.say("diagnostics: " + (s.diagnosticsDir.empty() ? std::string("no folder (this run may not write diagnostics)") : s.diagnosticsDir));
    w_.say("lock: " + lockLine(s.lock));

    // THE WINDOW'S HALF OF THE SETTINGS, from the table, not from a file (app_window.cpp ~652-690: only when there is a settings file and the
    // engine loaded it; a hermetic run keeps the member defaults, which are AppConfig's).
    iface_ = core::AppConfig{};
    ifaceServed_ = false;
    if (!s.configPath.empty() && s.configLoaded) {
        std::string document;
        if (!w_.fact("fact.config.interface", document) || document.empty()) {
            w_.say("settings: the engine did not serve the interface's half of them; the defaults stand and nothing is handed back");
        } else {
            core::AppConfig parsed;
            std::string error;
            if (core::parseInterfaceHalf(document, parsed, error)) {
                iface_ = parsed;
                ifaceServed_ = true;
            } else {
                w_.say("settings: the interface's half of them could not be read (" + error + "); the defaults stand and nothing is handed back");
            }
        }
    }
    // The crash-upload limiter's memory, VALIDATED rather than trusted (diagnosticsStartup): it is user-editable text on disk, and the one
    // thing it controls is how much this machine may send.
    crashState_ = core::decodePolicyState(iface_.crashUploadRecent, iface_.crashUploadWindowStart, iface_.crashUploadWindowCount,
                                          iface_.crashUploadBlockedUntil);
    crashAtStart_ = crashState_;

    // The last run's marker was false: the ImGui window opens its "did not close normally" offer here. This window says so in the log,
    // and the offer is a companion screen of a later phase.
    if (s.lastRunUnclean) {
        w_.say(std::string("the last run did not end normally") +
               (iface_.diagnosticsEnabled ? "; the offer to send a report is a companion screen of a later phase" : ""));
    }

    // THE USER'S DIAGNOSTICS CHOICES, applied the way run() applies them (applyDiagnosticsEnabled): boot armed capture from the stored
    // switch with the minidump off ("a dump is process memory and is never armed on a guess"); this adds the user's minidump choice.
    w_.applyDiagnosticsSwitches(iface_.diagnosticsEnabled, iface_.diagnosticsMinidump);

    // THE UPDATE CHECK, once (app_window.cpp ~1405).
    const bool canned = w_.manifestSeam();
    const UpdatePlan plan = updatePlanFor(bounded, canned, w_.inPackage(), iface_.updateCheckEnabled);
    // (With the canned answer set, "asking foxsdr.com" would be a lie: the answer is a file and foxsdr.com is not asked.)
    w_.say(plan == UpdatePlan::Run && canned ? "update: reading the canned answer of FOXSDR_FAKE_UPDATE_MANIFEST; foxsdr.com is not asked"
                                             : updatePlanLine(plan));
    if (plan == UpdatePlan::Run) { startUpdateCheck(); }

    // THE CRASH SWEEP, armed here and started by the first poll that can say whose install id it carries (app_window.cpp ~1444).
    const SweepPlan sweep = sweepPlanFor(iface_.diagnosticsEnabled, s.diagnosticsDir);
    w_.say(sweepPlanLine(sweep));
    sweepArmed_ = sweep == SweepPlan::Armed;

    w_.diag(bounded ? "event loop starting (bounded)" : "event loop starting (interactive)");
    w_.phase(core::breadcrumb::Phase::CreatingWindow);
}

void HostDuties::windowUp() { w_.phase(core::breadcrumb::Phase::AwaitingFirstFrame); }

void HostDuties::poll(const TickInput& in) {
    // THE HEARTBEAT of the sentinel's page: the host's frame loop is its ONE writer (core/breadcrumb.hpp). What else is going on is mirrored
    // from the state each tick, and about every five seconds one non-blocking look at whether the watcher is still there.
    w_.beat();
    w_.radioOpening(in.deviceOpening);
    if (ticks_ % kSentinelPollEveryTicks == 0) { w_.sentinelPoll(); }
    ++ticks_;

    // THE CONSOLE'S CLOSE REQUEST (Ctrl+C, Ctrl+Break, its window closing): boot installed the handler for a real session, and it only ASKS
    // for the close; it is the frame loop that ends. Once.
    if (!closeAsked_ && w_.consoleCloseRequested()) {
        closeAsked_ = true;
        w_.say("the console asked for the close (Ctrl+C, Ctrl+Break or its window closing); closing");
        requestClose_();
    }

    // The two senders: a worker that has finished is collected, and a terminal state's cooldown ends (nothing here blocks).
    w_.sendersPoll(w_.nowEpoch());

    if (sweepArmed_ && !sweepRunning_) { trySweepStart(); }
    pollUpdate();
}

void HostDuties::trySweepStart() {
    // THE SAME anonymous id the usage report uses, and empty when usage reporting is off - because then it does not exist. A crash report
    // must not be what mints an identifier the user switched off (PRIVACY.md). Both come from the engine as facts, and a fact answers NOTHING
    // before the engine's first publish, which would read as "reporting is off" and start the sweep with an empty id for the whole run: an
    // unanswered switch is "not yet", never "off", and the next poll asks again.
    std::string enabled;
    if (!w_.fact("fact.telemetry.enabled", enabled) || enabled.empty()) { return; }
    std::string installId;
    (void)w_.fact("fact.telemetry.installId", installId);

    core::SweepParams p;
    p.crashDir = cfg_.diagnosticsDir;
    p.url = w_.crashEndpoint();
    p.installId = installId;
    p.enabled = true;
    p.state = crashState_;
    w_.sweepStart(p);
    sweepRunning_ = true;
    w_.say("crash upload: the sweep of the reports folder has started");
}

void HostDuties::startUpdateCheck() {
    if (updateAsked_) { return; }
    updateAsked_ = true;
    updatePending_ = true;
    updateError_.clear();
    // Stable ("") unless the person chose every release (0.99.88).
    w_.updateCheckStart(cfg_.version, core::updateChannelParam(iface_.updateChannel));
}

void HostDuties::pollUpdate() {
    if (updatePending_ && stage_ != UpdateStage::Downloading) {
        core::UpdateCheckOutcome checked;
        if (w_.updateCheckPoll(checked)) {
            updatePending_ = false;
            if (checked.ok) {
                update_ = checked.info;
                w_.say(update_.newer ? "update: FoxSDR " + update_.version + " is available"
                                     : "update: this is the newest version (the service offers " + update_.version + ")");
            } else {
                // A failed check is NOT shown: the user did not ask, and a program that interrupts listening to say it could not reach a
                // server is worse than one that quietly tries again next launch.
                updateError_ = checked.error;
                w_.say("update: the check failed (" + checked.error + "); nothing is shown");
            }
            refreshCard();
        }
    }
    if (stage_ == UpdateStage::Downloading) {
        DownloadOutcome done;
        if (w_.downloadPoll(done)) {
            updatePending_ = false;
            if (done.ok) {
                readyPath_ = done.path;
                stage_ = UpdateStage::Ready;
                w_.say("update: downloaded and verified");
            } else {
                // A download failure IS shown: the user pressed a key and is waiting for it. PluginRepo's words, verbatim, which for a digest
                // mismatch name both digests - the one case where the detail matters more than the tidiness.
                stage_ = UpdateStage::Idle;
                updateError_ = done.error;
                w_.say("update: the download failed: " + done.error);
            }
        } else {
            progress_ = w_.downloadProgress();
        }
        refreshCard();
    }
    if (stage_ == UpdateStage::Installing) {
        bool started = false;
        if (w_.installerPoll(started)) {
            if (started) {
                w_.say("update: the installer was started; this window closes for it");
                if (!closeAsked_) {
                    closeAsked_ = true;
                    requestClose_();
                }
            } else {
                stage_ = UpdateStage::Ready;
                updateError_ = "could not start the installer at " + readyPath_;
                w_.say("update: " + updateError_);
                refreshCard();
            }
        }
    }
}

void HostDuties::startDownload() {
    if (updatePending_ || !update_.newer) { return; }
    updatePending_ = true;
    stage_ = UpdateStage::Downloading;
    progress_ = 0.f;
    updateError_.clear();
    w_.say("update: downloading " + update_.version);
    w_.downloadStart(update_);
    refreshCard();
}

void HostDuties::startInstaller() {
    if (readyPath_.empty()) { return; }
    stage_ = UpdateStage::Installing;
    updateError_.clear();
    w_.say("update: starting the installer");
    w_.installerStart(readyPath_);
    refreshCard();
}

void HostDuties::updatePrimaryClicked() {
    if (!card_.shown) { return; }
    if (!cfg_.installerHere) {
        w_.say("update: there is no installer for this platform; the download is at https://foxsdr.com (this window opens no browser yet)");
        return;
    }
    if (stage_ == UpdateStage::Idle) {
        startDownload();
    } else if (stage_ == UpdateStage::Ready) {
        startInstaller();
    }
    // Downloading and Installing have no key to press: the bar and the launch answer on their own.
}

void HostDuties::updateDismissed() {
    updateDismissed_ = true;     // "Not now" hides it until the next launch
    refreshCard();
}

void HostDuties::refreshCard() {
    UpdateView v;
    v.enabled = iface_.updateCheckEnabled;
    v.dismissed = updateDismissed_;
    v.packaged = w_.inPackage();
    v.installerHere = cfg_.installerHere;
    v.info = update_;
    v.stage = stage_;
    v.progress = progress_;
    v.error = updateError_;
    UpdateCardModel next = updateCardFor(v);
    if (!(next == card_)) {
        card_ = std::move(next);
        cardChanged_ = true;
    }
}

bool HostDuties::takeCardChange() {
    const bool changed = cardChanged_;
    cardChanged_ = false;
    return changed;
}

void HostDuties::finish() {
    // 1. THE SENTINEL'S FIRST SHUTDOWN STEP: the event loop has ended (run(): presentGrace.release(), then setPhase(ShutdownBegun)).
    w_.phase(core::breadcrumb::Phase::ShutdownBegun);

    // 2. THE CRASH-REPORT SWEEP STOPPED, and its memory kept - BEFORE the settings are handed over, so the dedup and the rate limit reach the
    // file, and before anything else on this path, so a transfer in flight is cancelled rather than waited out (crashUploadFinish). This
    // is the line that makes "a hanging endpoint does not delay shutdown" true; it is measured against the real binary in
    // tests/test_crash_upload.cpp.
    if (sweepRunning_) {
        const core::SweepOutcome out = w_.sweepStop();
        sweepRunning_ = false;
        crashState_ = out.state;
        if (out.considered > 0) {
            w_.diag("crash upload: " + std::to_string(out.considered) + " considered, " + std::to_string(out.sent) + " sent, " +
                    std::to_string(out.duplicate) + " duplicate, " + std::to_string(out.limited) + " held, " + std::to_string(out.failed) +
                    " failed, " + std::to_string(out.abandoned) + " abandoned, " + std::to_string(out.refused) + " refused");
            for (const std::string& note : out.notes) { w_.diag("crash upload: " + note); }
        }
    }
    // 3. THE TWO SENDERS CANCELLED: a feature request or a problem report sent moments before the close must not delay it either. Nothing
    // is kept of their outcome - there is no sweep and nothing to retry (core/feature_request.hpp).
    w_.sendersCancel();
    // 3b. A download in flight is told to stop now (a flag; ~AppWindow sets it at the end): the transfer polls it between chunks.
    w_.downloadCancel();

    // 4. THE LAST HALF OF THE SETTINGS to the engine, BEFORE its final-state save, which saves what the engine holds.
    handBackSettings();

    w_.diag("event loop ended after " + std::to_string(ticks_) + " ticks; shutting down");
}

void HostDuties::handBackSettings() {
    // ONLY A HALF THE ENGINE SERVED, and only when the sweep moved its memory. A half built from defaults would put the defaults over the
    // engine's copy of every window rectangle and switch; this window changes nothing else of the half, so what it hands back is what it was
    // handed with the crash-upload limiter's memory brought up to date.
    if (!ifaceServed_ || samePolicy(crashState_, crashAtStart_)) { return; }
    core::AppConfig half = iface_;
    half.crashUploadRecent = core::encodePolicyRecent(crashState_);
    half.crashUploadWindowStart = crashState_.windowStart;
    half.crashUploadWindowCount = crashState_.windowCount;
    half.crashUploadBlockedUntil = crashState_.blockedUntil;
    if (!w_.submitInterfaceHalf(core::serializeInterfaceHalf(half))) {
        const std::string line = "config: the last half of the settings was not taken by the engine; the file keeps the half the engine last had";
        w_.say(line);
        w_.diag(line);
    }
}

void HostDuties::closed() {
    // The host has stopped the service: the engine's three shutdown steps are set; these are the window's (closeWindow, the end of run()).
    w_.phase(core::breadcrumb::Phase::ShutdownClosingWindow);
    w_.phase(core::breadcrumb::Phase::Finished);
    // ~AppWindow's: a check stalled in WinHTTP is let go of after a short grace instead of waited for.
    w_.updateCheckReap();
}

AboutFacts HostDuties::about() const {
    AboutFacts f;
    f.version = cfg_.version;
    f.configPath = cfg_.configPath;
    f.diagnosticsDir = cfg_.diagnosticsDir;
    f.lock = cfg_.lock;
    return f;
}

// ---- the real world ---------------------------------------------------------------------------------------------------------
LiveWorld::LiveWorld(const FoxEngineApi* api, FoxSession* session) : api_(api), session_(session) {
#if defined(_WIN32)
    launcher_ = &shellOpenInstaller;
#else
    // Nothing to start on a platform with no installer; the card never offers it.
    launcher_ = [](const std::string&) { return false; };
#endif
}

LiveWorld::~LiveWorld() {
    // A std::async future's destructor waits for its worker, and this flag is what makes that wait short: the download polls it between
    // chunks and deletes its ".part" file on the way out.
    cancel_.store(true, std::memory_order_relaxed);
}

bool LiveWorld::fact(const char* key, std::string& value) {
    value.clear();
    if (api_ == nullptr || session_ == nullptr || !FOXAPI_HAS(api_, get_setting)) { return false; }
    char buf[512];
    int32_t n = api_->get_setting(session_, key, buf, sizeof(buf));
    if (n >= static_cast<int32_t>(sizeof(buf))) {
        // The value is longer than the buffer (the interface's half is several kilobytes): asked again with room for the whole of it.
        value.resize(static_cast<std::size_t>(n) + 1u);
        n = api_->get_setting(session_, key, value.data(), value.size());
        value.resize(n >= 0 ? std::min(static_cast<std::size_t>(n), value.size() - 1u) : 0u);
    } else if (n >= 0) {
        value.assign(buf, static_cast<std::size_t>(n));
    }
    return n >= 0;
}

bool LiveWorld::submitInterfaceHalf(const std::string& document) {
    if (api_ == nullptr || session_ == nullptr || !FOXAPI_HAS(api_, submit) || !FOXAPI_HAS(api_, submit_text)) { return false; }
    // FOXAPP_OP_INTERFACE_CONFIG: the document is long text beside the command (flushInterfaceHalf, app_window.cpp ~2349).
    const core::cmd::QueuedCommand q = core::cmd::makeText(FOXAPP_OP_INTERFACE_CONFIG, document);
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = q.longText.empty() ? api_->submit(session_, &q.c, 1, &sr)
                                         : api_->submit_text(session_, &q.c, q.longText.data(), static_cast<uint32_t>(q.longText.size()), &sr);
    return !(n < 0 || (sr.status != FOXAPI_OK && sr.status != FOXAPI_NO_CHANGE));
}

std::uint64_t LiveWorld::nowEpoch() { return static_cast<std::uint64_t>(std::time(nullptr)); }

std::string LiveWorld::crashEndpoint() { return core::crashUploadEndpoint(); }
void LiveWorld::sweepStart(const core::SweepParams& params) { crash_.start(params); }

core::SweepOutcome LiveWorld::sweepStop() {
    // Cancels first, then joins: the cancel closes the live request handle, so a transfer blocked on a server that never answers returns
    // at once instead of sitting out its receive timeout.
    crash_.stop();
    return crash_.outcome();
}

bool LiveWorld::inPackage() { return core::runningInPackage(); }
bool LiveWorld::manifestSeam() { return core::updateManifestFakeSet(); }

void LiveWorld::updateCheckStart(const std::string& version, const std::string& channel) {
    const std::string endpoint = core::updateEndpoint();
    // NOTHING OF `this` IS CAPTURED: the outcome comes back by value, which is what lets the check be abandoned at the close instead of
    // waited for (core::UpdateCheckTask).
    check_.start([endpoint, version, channel]() {
        core::UpdateCheckOutcome o;
        o.ok = core::checkForUpdate(endpoint, version, channel, o.info, o.error);
        return o;
    });
}

bool LiveWorld::updateCheckPoll(core::UpdateCheckOutcome& outcome) { return check_.poll(outcome); }
void LiveWorld::updateCheckReap() { (void)check_.reap(); }

void LiveWorld::downloadStart(const core::UpdateInfo& info) {
    // Cleared on entry, as the plugin installer clears its own: a cancel left over from an abandoned attempt must not kill this one, and
    // the bar starts at zero rather than where the last try stopped.
    progress_.store(0.f, std::memory_order_relaxed);
    cancel_.store(false, std::memory_order_relaxed);
    download_ = std::async(std::launch::async, [this, info]() { return core::downloadUpdate(info, resultPath_, resultError_, &progress_, &cancel_); });
}

bool LiveWorld::downloadPoll(DownloadOutcome& outcome) {
    if (!download_.valid() || download_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }
    const bool ok = download_.get();
    outcome.ok = ok;
    outcome.path = ok ? resultPath_ : std::string();
    outcome.error = ok ? std::string() : resultError_;
    return true;
}

float LiveWorld::downloadProgress() { return progress_.load(std::memory_order_relaxed); }
void LiveWorld::downloadCancel() { cancel_.store(true, std::memory_order_relaxed); }

void LiveWorld::installerStart(const std::string& path) {
    InstallerLauncher launch = launcher_;
    installer_ = std::async(std::launch::async, [launch, path]() -> bool { return launch ? launch(path) : false; });
}

bool LiveWorld::installerPoll(bool& started) {
    if (!installer_.valid() || installer_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }
    started = installer_.get();
    // An installer that could not be started is COUNTED, anonymously (core/health_events.hpp): the user pressed the key and nothing ran.
    if (!started) { core::health::noteUpdateRunFailed(); }
    return true;
}

void LiveWorld::sendersPoll(std::uint64_t nowEpoch) {
    feature_.poll(nowEpoch);
    problem_.poll(nowEpoch);
}

void LiveWorld::sendersCancel() {
    feature_.cancel();
    problem_.cancel();
}

void LiveWorld::phase(core::breadcrumb::Phase phase) { core::breadcrumb::setPhase(phase); }
void LiveWorld::beat() { core::breadcrumb::beat(); }
void LiveWorld::radioOpening(bool on) { core::breadcrumb::setActivity(core::breadcrumb::kOpeningRadio, on); }
void LiveWorld::sentinelPoll() { core::sentinelPoll(); }
bool LiveWorld::consoleCloseRequested() { return core::consoleCloseRequested(); }
void LiveWorld::applyDiagnosticsSwitches(bool enabled, bool minidump) { core::setCrashCaptureEnabled(enabled, minidump); }

void LiveWorld::say(const std::string& line) { std::fprintf(stderr, "foxsdr-slint: %s\n", line.c_str()); }
void LiveWorld::diag(const std::string& line) { core::diagLogf("%s", line.c_str()); }

}  // namespace foxsdr::slinthost
