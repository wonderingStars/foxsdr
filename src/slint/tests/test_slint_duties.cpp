// The Slint host's housekeeping (src/slint/host_duties.*): the program's name, the bounded run's arithmetic, the settings lock's words,
// the About text, the update check's decisions and the card the update result becomes, the start and the poll and the ORDER OF THE EXIT
// as sequences of calls on a recording world, and the real world's own pieces that need no network and no profile (the sweep over an
// empty folder, the update check over a canned manifest, the download that refuses before it connects, the lock file read back).
//
// No Slint and no window: host_duties.cpp is linked into this test with cascade_engine and nothing else. Every file the test makes is
// in a folder under %TEMP% with the process id in its name, and every environment variable it sets is put back. Nothing here touches
// the network (the canned manifest is a file, the sweep folder is empty, the download is refused before it connects) or the user's
// profile.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "host_duties.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/config_halves.hpp"
#include "core/config_lock.hpp"
#include "engine/engine_service.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/window_service_options.hpp"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace foxsdr::slinthost;
namespace core = cascade::core;
namespace fs = std::filesystem;

namespace {

using Lines = std::vector<std::string>;

unsigned long ownPid() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(name, value.c_str());   // an empty value removes the variable
#else
    if (value.empty()) { ::unsetenv(name); } else { ::setenv(name, value.c_str(), 1); }
#endif
}

fs::path scratchDir() {
    static const fs::path dir = fs::temp_directory_path() / ("foxsdr-slint-duties-" + std::to_string(ownPid()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void writeFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool anyContains(const Lines& v, const std::string& needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

int countOf(const Lines& v, const std::string& exact) { return static_cast<int>(std::count(v.begin(), v.end(), exact)); }

// The calls with the noisy ones left out: a call is dropped when what precedes its first ':' is in `noise`.
Lines without(const Lines& v, std::initializer_list<const char*> noise) {
    std::set<std::string> drop(noise.begin(), noise.end());
    Lines out;
    for (const std::string& s : v) {
        if (drop.count(s.substr(0, s.find(':'))) == 0) { out.push_back(s); }
    }
    return out;
}

const std::string& at(const Lines& v, std::size_t i) {
    static const std::string none = "<missing>";
    return i < v.size() ? v[i] : none;
}

std::string phaseName(core::breadcrumb::Phase p) {
    using P = core::breadcrumb::Phase;
    switch (p) {
        case P::CreatingWindow: return "creating-window";
        case P::AwaitingFirstFrame: return "awaiting-first-frame";
        case P::Running: return "running";
        case P::ShutdownBegun: return "shutdown-begun";
        case P::ShutdownClosingWindow: return "shutdown-closing-window";
        case P::Finished: return "finished";
        default: return "other";
    }
}

// ---- the recording world ----
struct FakeWorld final : DutyWorld {
    Lines calls, said, diagged, submitted;
    std::map<std::string, std::string> facts;
    bool submitOk = true;
    std::uint64_t now = 1'800'000'000ull;
    std::string endpoint = "https://crash.example.test/api/crash";
    bool packaged = false, seam = false, consoleClose = false;
    bool checkReady = false;
    core::UpdateCheckOutcome checkOutcome;
    bool downloadReady = false;
    DownloadOutcome downloadOutcome;
    float progress = 0.f;
    bool installerReady = false, installerStarted = true;
    core::SweepOutcome sweepOutcome;
    core::SweepParams lastSweep;

    bool fact(const char* key, std::string& value) override {
        calls.push_back(std::string("fact:") + key);
        const auto it = facts.find(key);
        if (it == facts.end()) { value.clear(); return false; }
        value = it->second;
        return true;
    }
    bool submitInterfaceHalf(const std::string& document) override {
        calls.push_back("submitInterfaceHalf");
        submitted.push_back(document);
        return submitOk;
    }
    std::uint64_t nowEpoch() override { return now; }
    std::string crashEndpoint() override { return endpoint; }
    void sweepStart(const core::SweepParams& p) override { calls.push_back("sweepStart"); lastSweep = p; }
    core::SweepOutcome sweepStop() override { calls.push_back("sweepStop"); return sweepOutcome; }
    bool inPackage() override { return packaged; }
    bool manifestSeam() override { return seam; }
    void updateCheckStart(const std::string& version, const std::string& channel) override {
        calls.push_back("updateCheckStart:" + version + ":" + channel);
    }
    bool updateCheckPoll(core::UpdateCheckOutcome& out) override {
        if (!checkReady) { return false; }
        checkReady = false;
        out = checkOutcome;
        return true;
    }
    void updateCheckReap() override { calls.push_back("updateCheckReap"); }
    void downloadStart(const core::UpdateInfo& info) override { calls.push_back("downloadStart:" + info.version); }
    bool downloadPoll(DownloadOutcome& out) override {
        if (!downloadReady) { return false; }
        downloadReady = false;
        out = downloadOutcome;
        return true;
    }
    float downloadProgress() override { return progress; }
    void downloadCancel() override { calls.push_back("downloadCancel"); }
    void installerStart(const std::string& path) override { calls.push_back("installerStart:" + path); }
    bool installerPoll(bool& started) override {
        if (!installerReady) { return false; }
        installerReady = false;
        started = installerStarted;
        return true;
    }
    void sendersPoll(std::uint64_t) override { calls.push_back("sendersPoll"); }
    void sendersCancel() override { calls.push_back("sendersCancel"); }
    void phase(core::breadcrumb::Phase p) override { calls.push_back("phase:" + phaseName(p)); }
    void beat() override { calls.push_back("beat"); }
    void radioOpening(bool on) override { calls.push_back(std::string("radioOpening:") + (on ? "1" : "0")); }
    void sentinelPoll() override { calls.push_back("sentinelPoll"); }
    bool consoleCloseRequested() override { return consoleClose; }
    void applyDiagnosticsSwitches(bool enabled, bool minidump) override {
        calls.push_back(std::string("applyDiagnosticsSwitches:") + (enabled ? "1" : "0") + ":" + (minidump ? "1" : "0"));
    }
    void say(const std::string& line) override { said.push_back(line); }
    void diag(const std::string& line) override { diagged.push_back(line); }
};

// The interface's half as the engine would serve it.
std::string interfaceDoc(bool diagnostics, bool updates, const std::string& channel = "stable", const core::UploadPolicyState* memory = nullptr) {
    core::AppConfig c;
    c.diagnosticsEnabled = diagnostics;
    c.updateCheckEnabled = updates;
    c.updateChannel = channel;
    if (memory != nullptr) {
        c.crashUploadRecent = core::encodePolicyRecent(*memory);
        c.crashUploadWindowStart = memory->windowStart;
        c.crashUploadWindowCount = memory->windowCount;
        c.crashUploadBlockedUntil = memory->blockedUntil;
    }
    return core::serializeInterfaceHalf(c);
}

core::UploadPolicyState someMemory() {
    core::UploadPolicyState s;
    core::noteSent(s, "AAAAAAAAAAAAAAAA", 1'799'990'000ull);
    return s;
}

core::UpdateInfo newerInfo() {
    core::UpdateInfo info;
    info.newer = true;
    info.version = "9.9.9";
    info.url = "https://foxsdr.com/download/foxsdr-setup-9.9.9.exe";
    info.sha256 = std::string(64, 'a');
    info.sizeBytes = 30'000'000;
    core::ReleaseNote a;
    a.version = "9.9.9";
    a.notes = {"one", "two"};
    core::ReleaseNote b;
    b.version = "9.9.8";
    b.critical = true;
    b.notes = {"three"};
    info.notes = {a, b};
    info.critical = true;
    return info;
}

DutyStart interactive() {
    DutyStart s;
    s.configPath = "C:\\profile\\foxsdr\\config.json";
    s.configLoaded = true;
    s.diagnosticsDir = "C:\\profile\\foxsdr\\crashes";
    s.version = "0.99.104";
    s.frames = -1;
    s.lock.state = LockState::Held;
    s.installerHere = true;   // the card's words are the Windows ones whatever this test is built on
    return s;
}

DutyStart bounded(int frames) {
    DutyStart s = interactive();
    s.frames = frames;
    s.diagnosticsDir.clear();     // a bounded run may not write diagnostics
    s.lock.state = LockState::NotTaken;
    return s;
}

void tellFacts(FakeWorld& w) {
    w.facts["fact.telemetry.enabled"] = "1";
    w.facts["fact.telemetry.installId"] = "0123456789abcdef0123456789abcdef";
}

}  // namespace

int main() {
    // ---- who this program is ----
    {
        CHECK(programLine("0.99.104") == "program: slint (foxsdr-slint 0.99.104)");
        CHECK(std::string(kProgramId) == "slint");
        CHECK(std::string(kProgramExe) == "foxsdr-slint");
        CHECK(programLine("x").find('\\') == std::string::npos);   // a line of the diagnostics ring carries no path
    }

    // ---- the bounded run: N ticks, then exit 0 ----
    {
        BoundedRun live(-1);
        CHECK(!live.bounded());
        bool ever = false;
        for (int i = 0; i < 1000; ++i) { ever = ever || live.noteTick(); }
        CHECK(!ever);                       // a real session never ends itself
        CHECK(!live.skipsLoop());

        BoundedRun three(3);
        CHECK(three.bounded());
        CHECK(!three.skipsLoop());
        CHECK(!three.noteTick());
        CHECK(!three.noteTick());
        CHECK(three.noteTick());            // the third tick is the last
        CHECK(three.ticks() == 3);
        CHECK(three.exitLine(0) == "foxsdr-slint: bounded run: 3 of 3 ticks, exit 0");

        BoundedRun zero(0);
        CHECK(zero.bounded());
        CHECK(zero.skipsLoop());            // --frames 0 produces none
        CHECK(zero.exitLine(0) == "foxsdr-slint: bounded run: 0 of 0 ticks, exit 0");

        BoundedRun one(1);
        CHECK(one.noteTick());

        BoundedRun cut(5);                  // the installer ended the run early
        cut.noteTick();
        CHECK(cut.exitLine(0) == "foxsdr-slint: bounded run: 1 of 5 ticks, exit 0");
        CHECK(cut.exitLine(1) == "foxsdr-slint: bounded run: 1 of 5 ticks, exit 1");
    }

    // ---- the settings lock's words ----
    {
        CHECK(holderPid("role=cascade pid=1234\n") == 1234ul);
        CHECK(holderPid("role=foxsdr-engine pid=7") == 7ul);
        CHECK(holderPid("") == 0ul);
        CHECK(holderPid("garbage") == 0ul);
        CHECK(holderPid("role=cascade pid=abc") == 0ul);
        CHECK(holderPid("role=Cascade! pid=5") == 0ul);          // a role is a short run of [a-z-]; anything else is somebody's edit
        CHECK(holderPid("pid=5 role=cascade") == 0ul);
        CHECK(currentProcessId() == ownPid());
        CHECK(currentProcessId() != 0ul);

        CHECK(lockReportFor(false, true, "", "", 1).state == LockState::NotTaken);
        CHECK(lockReportFor(true, true, "", "role=cascade pid=77\n", 77).state == LockState::Held);
        CHECK(lockReportFor(true, true, "", "role=cascade pid=77\n", 78).state == LockState::Elsewhere);
        CHECK(lockReportFor(true, true, "", "", 77).state == LockState::Unknown);
        const LockReport refused = lockReportFor(true, false, "foxsdr-engine holds the settings file", "", 77);
        CHECK(refused.state == LockState::Refused);
        CHECK(refused.sentence == "foxsdr-engine holds the settings file");

        CHECK(lockLine({LockState::NotTaken, ""}) == "not taken (a bounded run, or a run with no settings file)");
        CHECK(lockLine({LockState::Held, ""}) == "held by this window");
        CHECK(lockLine({LockState::Elsewhere, ""}) == "held by another FoxSDR program; this window carries on without it");
        CHECK(lockLine({LockState::Unknown, ""}) == "asked for; who holds it could not be read");
        CHECK(lockLine({LockState::Refused, "the sentence"}) == "refused: the sentence");

        // THE REAL REFUSAL SENTENCE, from the engine's own function, and the real lock file read back by this process.
        const fs::path lockDir = scratchDir() / "lock";
        const std::string cfgPath = (lockDir / "config.json").string();
        core::ConfigLock holder;
        CHECK(holder.acquire(cfgPath, "foxsdr-engine") == core::ConfigLock::State::Held);
        core::ConfigLock second;
        CHECK(second.acquire(cfgPath, "cascade") == core::ConfigLock::State::Contended);
        const std::string sentence = cascade::engine::EngineService::refusal(second, cfgPath);
        CHECK(!sentence.empty());
        CHECK(lockLine({LockState::Refused, sentence}) == "refused: " + sentence);
        const std::string holderText = readLockHolder(cfgPath);
        CHECK(holderPid(holderText) == ownPid());
        CHECK(lockReportFor(true, true, "", holderText, ownPid()).state == LockState::Held);
        CHECK(lockReportFor(true, true, "", holderText, ownPid() + 1).state == LockState::Elsewhere);
        CHECK(readLockHolder((lockDir / "no-such-folder" / "config.json").string()).empty());
        holder.release();
    }

    // ---- the About text ----
    {
        AboutFacts f;
        f.version = "0.99.104";
        f.configPath = "C:\\profile\\foxsdr\\config.json";
        f.diagnosticsDir = "C:\\profile\\foxsdr\\crashes";
        f.lock.state = LockState::Held;
        const std::string t = aboutText(f);
        CHECK(t.find("FoxSDR") != std::string::npos);
        CHECK(t.find("Version: 0.99.104") != std::string::npos);
        CHECK(t.find("Program: slint (foxsdr-slint)") != std::string::npos);
        CHECK(t.find("Settings file: C:\\profile\\foxsdr\\config.json") != std::string::npos);
        CHECK(t.find("Diagnostics folder: C:\\profile\\foxsdr\\crashes") != std::string::npos);
        CHECK(t.find("Settings lock: held by this window") != std::string::npos);

        AboutFacts none;
        none.version = "0.99.104";
        const std::string u = aboutText(none);
        CHECK(u.find("Settings file: none") != std::string::npos);
        CHECK(u.find("Diagnostics folder: none") != std::string::npos);
        CHECK(u.find("Settings lock: not taken") != std::string::npos);

        AboutFacts refusedFacts = f;
        refusedFacts.lock = {LockState::Refused, "another program holds it"};
        CHECK(aboutText(refusedFacts).find("Settings lock: refused: another program holds it") != std::string::npos);
    }

    // ---- the update check's decisions ----
    {
        // a bounded run reaches no network unless the canned answer (a file) is set
        CHECK(updatePlanFor(true, false, false, true) == UpdatePlan::Hermetic);
        CHECK(updatePlanFor(true, false, true, true) == UpdatePlan::Hermetic);
        CHECK(updatePlanFor(true, false, false, false) == UpdatePlan::Hermetic);
        CHECK(updatePlanFor(true, true, false, true) == UpdatePlan::Run);
        CHECK(updatePlanFor(true, true, false, false) == UpdatePlan::OffByChoice);
        CHECK(updatePlanFor(true, true, true, true) == UpdatePlan::StoreDeferred);
        // an interactive run: the user's tick first, then the kind of copy
        CHECK(updatePlanFor(false, false, false, true) == UpdatePlan::Run);
        CHECK(updatePlanFor(false, false, false, false) == UpdatePlan::OffByChoice);
        CHECK(updatePlanFor(false, false, true, false) == UpdatePlan::OffByChoice);   // unticked asks nobody, the Store included
        CHECK(updatePlanFor(false, false, true, true) == UpdatePlan::StoreDeferred);  // a Store copy never runs foxsdr-setup
        const std::set<std::string> lines = {updatePlanLine(UpdatePlan::Run), updatePlanLine(UpdatePlan::OffByChoice),
                                             updatePlanLine(UpdatePlan::StoreDeferred), updatePlanLine(UpdatePlan::Hermetic)};
        CHECK(lines.size() == 4);
        CHECK(lines.count("") == 0);

        // OFF MEANS OFF for uploading too
        CHECK(sweepPlanFor(true, "C:\\crashes") == SweepPlan::Armed);
        CHECK(sweepPlanFor(false, "C:\\crashes") == SweepPlan::OffBySwitch);
        CHECK(sweepPlanFor(true, "") == SweepPlan::NoFolder);
        CHECK(sweepPlanFor(false, "") == SweepPlan::OffBySwitch);
        const std::set<std::string> sweepLines = {sweepPlanLine(SweepPlan::Armed), sweepPlanLine(SweepPlan::OffBySwitch), sweepPlanLine(SweepPlan::NoFolder)};
        CHECK(sweepLines.size() == 3);
    }

    // ---- the update result as the card ----
    {
        UpdateView v;
        v.enabled = true;
        v.installerHere = true;
        v.info = newerInfo();
        const UpdateCardModel c = updateCardFor(v);
        CHECK(c.shown);
        CHECK(c.headline == "Important update: FoxSDR 9.9.9");   // newerInfo is critical
        CHECK(c.critical);
        CHECK((c.notes == Lines{"9.9.9", "one", "two", "9.9.8  (important)", "three"}));
        CHECK(c.primary == "Download update");
        CHECK(c.progress < 0.f);
        CHECK(c.note == "30.0 MB, verified against its published checksum before anything runs");
        CHECK(c.error.empty());

        UpdateView plain = v;
        plain.info.critical = false;
        const UpdateCardModel p = updateCardFor(plain);
        CHECK(p.headline == "FoxSDR 9.9.9 is available");
        CHECK(!p.critical);
        plain.info.sizeBytes = 0;
        CHECK(updateCardFor(plain).note.empty());                // no size, no promise about it

        UpdateView hidden = v;
        hidden.enabled = false;
        CHECK(!updateCardFor(hidden).shown);
        hidden = v;
        hidden.dismissed = true;
        CHECK(!updateCardFor(hidden).shown);                      // "Not now"
        hidden = v;
        hidden.packaged = true;
        CHECK(!updateCardFor(hidden).shown);                      // a Store copy has its own banner
        hidden = v;
        hidden.info.newer = false;
        CHECK(!updateCardFor(hidden).shown);                      // nothing newer is offered

        UpdateView dl = v;
        dl.stage = UpdateStage::Downloading;
        dl.progress = 0.4f;
        const UpdateCardModel d = updateCardFor(dl);
        CHECK(d.primary == "Downloading...");
        CHECK_NEAR(d.progress, 0.4, 1e-6);
        CHECK(d.note.empty());
        dl.progress = 1.7f;
        CHECK_NEAR(updateCardFor(dl).progress, 1.0, 1e-6);
        dl.progress = -0.2f;
        CHECK_NEAR(updateCardFor(dl).progress, 0.0, 1e-6);        // downloading is 0..1, never the "no bar" -1

        UpdateView ready = v;
        ready.stage = UpdateStage::Ready;
        const UpdateCardModel r = updateCardFor(ready);
        CHECK(r.primary == "Install now and restart");
        CHECK(r.note == "Downloaded and verified.");
        CHECK(r.progress < 0.f);

        UpdateView inst = v;
        inst.stage = UpdateStage::Installing;
        CHECK(updateCardFor(inst).primary == "Starting the installer...");

        UpdateView failed = v;
        failed.error = "could not start the installer at C:\\x.exe";
        CHECK(updateCardFor(failed).error == "could not start the installer at C:\\x.exe");

        UpdateView linuxView = v;
        linuxView.installerHere = false;
        const UpdateCardModel l = updateCardFor(linuxView);
        CHECK(l.primary == "Open foxsdr.com");
        CHECK(l.note == "There is no in-app updater for Linux yet - download it from foxsdr.com.");

        CHECK(updateCardFor(v) == updateCardFor(v));
        CHECK(!(updateCardFor(v) == updateCardFor(plain)));
    }

    // ---- THE KILL SWITCH on the card (docs 6.63): a withdrawn copy is told so; a withdrawn release is never offered ----
    {
        // running version withdrawn, a good build offered: the amber card names both, and the key is still Download
        UpdateView v;
        v.enabled = true;
        v.installerHere = true;
        v.runningVersion = "0.99.105";
        v.info = newerInfo();
        v.info.runningWithdrawn = true;
        UpdateCardModel c = updateCardFor(v);
        CHECK(c.shown && c.withdrawn);
        CHECK(c.headline == "FoxSDR 0.99.105 has been withdrawn: the FoxSDR team found a problem with it.");
        CHECK(c.note == "We recommend updating to FoxSDR 9.9.9.\n30.0 MB, verified against its published checksum before anything runs");
        CHECK(c.primary == "Download update");
        CHECK(!c.notes.empty() && c.notes.front() == "9.9.9");   // the release notes still ride along

        // running version withdrawn, nothing good yet: the sentence, no key but the card's own Not now (the row carries Open foxsdr.com)
        UpdateView none;
        none.enabled = true;
        none.installerHere = true;
        none.runningVersion = "0.99.105";
        none.info.newer = false;
        none.info.version = "0.99.104";
        none.info.runningWithdrawn = true;
        c = updateCardFor(none);
        CHECK(c.shown && c.withdrawn && !c.critical);
        CHECK(c.headline == "FoxSDR 0.99.105 has been withdrawn: the FoxSDR team found a problem with it.");
        CHECK(c.note == "A fixed version is not ready yet. foxsdr.com will have it as soon as it is.");
        CHECK(c.primary.empty() && c.progress < 0.f && c.notes.empty());

        // ...and the person can put it away, as the classic's Not now does; a Store copy and a switched-off check never see it
        UpdateView away = none;
        away.dismissed = true;
        CHECK(!updateCardFor(away).shown);
        UpdateView store = none;
        store.packaged = true;
        CHECK(!updateCardFor(store).shown);
        UpdateView off = none;
        off.enabled = false;
        CHECK(!updateCardFor(off).shown);

        // the withdrawn RELEASE is never offered, even when a hand-built answer still says newer
        UpdateView bad;
        bad.enabled = true;
        bad.installerHere = true;
        bad.info = newerInfo();
        bad.info.offerWithdrawn = true;
        CHECK(!updateCardFor(bad).shown);
        bad.info.runningWithdrawn = true;
        bad.runningVersion = "0.99.105";
        c = updateCardFor(bad);
        CHECK(c.shown && c.primary.empty() && c.note == "A fixed version is not ready yet. foxsdr.com will have it as soon as it is.");

        // the ordinary card is unchanged
        UpdateView plain;
        plain.enabled = true;
        plain.installerHere = true;
        plain.info = newerInfo();
        c = updateCardFor(plain);
        CHECK(c.shown && !c.withdrawn && c.headline == "Important update: FoxSDR 9.9.9");
    }

    // ---- an interactive start ----
    {
        FakeWorld w;
        int closed = 0;
        HostDuties d(w, [&] { ++closed; });
        w.facts["fact.config.interface"] = interfaceDoc(true, true, "beta");
        d.start(interactive());
        CHECK((without(w.calls, {}) == Lines{"fact:fact.config.interface", "applyDiagnosticsSwitches:1:0", "updateCheckStart:0.99.104:beta",
                                             "phase:creating-window"}));
        CHECK(anyContains(w.said, "program: slint (foxsdr-slint 0.99.104)"));
        CHECK(anyContains(w.diagged, "program: slint (foxsdr-slint 0.99.104)"));
        CHECK(anyContains(w.said, updatePlanLine(UpdatePlan::Run)));
        CHECK(anyContains(w.said, sweepPlanLine(SweepPlan::Armed)));
        CHECK(anyContains(w.said, "lock"));
        CHECK(d.about().configPath == "C:\\profile\\foxsdr\\config.json");
        CHECK(d.about().diagnosticsDir == "C:\\profile\\foxsdr\\crashes");
        CHECK(d.about().lock.state == LockState::Held);
        CHECK(d.about().version == "0.99.104");

        // the sweep waits for the engine's first publish: no answer to the telemetry fact is "not yet", never "off"
        d.poll({});
        CHECK(countOf(w.calls, "sweepStart") == 0);
        CHECK(!d.sweepRunning());
        tellFacts(w);
        d.poll({});
        CHECK(countOf(w.calls, "sweepStart") == 1);
        CHECK(d.sweepRunning());
        CHECK(w.lastSweep.crashDir == "C:\\profile\\foxsdr\\crashes");
        CHECK(w.lastSweep.url == "https://crash.example.test/api/crash");
        CHECK(w.lastSweep.installId == "0123456789abcdef0123456789abcdef");
        CHECK(w.lastSweep.enabled);
        d.poll({});
        d.poll({});
        CHECK(countOf(w.calls, "sweepStart") == 1);               // started once
        CHECK(closed == 0);

        // an install id is blank while usage reporting is off, and a blank id is sent as blank, never minted
        FakeWorld off;
        HostDuties e(off, [] {});
        off.facts["fact.config.interface"] = interfaceDoc(true, true);
        e.start(interactive());
        off.facts["fact.telemetry.enabled"] = "0";
        off.facts["fact.telemetry.installId"] = "";
        e.poll({});
        CHECK(countOf(off.calls, "sweepStart") == 1);
        CHECK(off.lastSweep.installId.empty());
    }

    // ---- the crash memory the sweep starts from is the one the engine served ----
    {
        FakeWorld w;
        HostDuties d(w, [] {});
        const core::UploadPolicyState mem = someMemory();
        w.facts["fact.config.interface"] = interfaceDoc(true, true, "stable", &mem);
        tellFacts(w);
        d.start(interactive());
        d.poll({});
        CHECK(w.lastSweep.state.recent == mem.recent);
        CHECK(w.lastSweep.state.windowStart == mem.windowStart);
        CHECK(w.lastSweep.state.windowCount == mem.windowCount);
        CHECK(!mem.recent.empty());
    }

    // ---- OFF MEANS OFF: no folder, no switch, no sweep ----
    {
        FakeWorld w;
        HostDuties d(w, [] {});
        w.facts["fact.config.interface"] = interfaceDoc(false, true);
        tellFacts(w);
        d.start(interactive());
        for (int i = 0; i < 5; ++i) { d.poll({}); }
        CHECK(countOf(w.calls, "sweepStart") == 0);
        CHECK(countOf(w.calls, "applyDiagnosticsSwitches:0:0") == 1);
        CHECK(anyContains(w.said, sweepPlanLine(SweepPlan::OffBySwitch)));
        d.finish();
        CHECK(countOf(w.calls, "sweepStop") == 0);                // nothing was started, nothing is stopped or kept

        FakeWorld n;
        HostDuties e(n, [] {});
        n.facts["fact.config.interface"] = interfaceDoc(true, true);
        tellFacts(n);
        DutyStart noFolder = interactive();
        noFolder.diagnosticsDir.clear();
        e.start(noFolder);
        for (int i = 0; i < 5; ++i) { e.poll({}); }
        CHECK(countOf(n.calls, "sweepStart") == 0);
        CHECK(anyContains(n.said, sweepPlanLine(SweepPlan::NoFolder)));
    }

    // ---- a bounded run stays hermetic, except for the canned answer ----
    {
        FakeWorld w;
        HostDuties d(w, [] {});
        w.facts["fact.config.interface"] = interfaceDoc(true, true);
        d.start(bounded(30));
        CHECK(countOf(w.calls, "updateCheckStart:0.99.104:") == 0);
        CHECK(anyContains(w.said, updatePlanLine(UpdatePlan::Hermetic)));

        FakeWorld s;
        s.seam = true;
        HostDuties e(s, [] {});
        s.facts["fact.config.interface"] = interfaceDoc(true, true);
        e.start(bounded(30));
        CHECK(countOf(s.calls, "updateCheckStart:0.99.104:") == 1);
        CHECK(anyContains(s.said, "canned answer"));              // and it says so: foxsdr.com is not asked
        CHECK(!anyContains(s.said, "asking foxsdr.com"));

        // a run with no settings file reads no half and keeps the defaults (updates on, diagnostics on)
        FakeWorld h;
        h.seam = true;
        HostDuties f(h, [] {});
        DutyStart hermetic = bounded(3);
        hermetic.configPath.clear();
        hermetic.configLoaded = false;
        f.start(hermetic);
        CHECK(countOf(h.calls, "fact:fact.config.interface") == 0);
        CHECK(countOf(h.calls, "updateCheckStart:0.99.104:") == 1);
    }

    // ---- the Updates tick, the Store and the channel ----
    {
        FakeWorld w;
        HostDuties d(w, [] {});
        w.facts["fact.config.interface"] = interfaceDoc(true, false);
        d.start(interactive());
        CHECK(countOf(w.calls, "updateCheckStart:0.99.104:") == 0);
        CHECK(anyContains(w.said, updatePlanLine(UpdatePlan::OffByChoice)));

        FakeWorld p;
        p.packaged = true;
        HostDuties e(p, [] {});
        p.facts["fact.config.interface"] = interfaceDoc(true, true);
        e.start(interactive());
        CHECK(countOf(p.calls, "updateCheckStart:0.99.104:") == 0);   // a Store copy never asks foxsdr.com
        CHECK(anyContains(p.said, "Microsoft Store"));
        CHECK(!e.card().shown);

        FakeWorld stable;
        HostDuties f(stable, [] {});
        stable.facts["fact.config.interface"] = interfaceDoc(true, true, "stable");
        f.start(interactive());
        CHECK(countOf(stable.calls, "updateCheckStart:0.99.104:") == 1);   // stable is the empty channel parameter
    }

    // ---- the update flow: check, card, download, install ----
    {
        FakeWorld w;
        int closed = 0;
        HostDuties d(w, [&] { ++closed; });
        w.facts["fact.config.interface"] = interfaceDoc(true, true);
        d.start(interactive());
        CHECK(!d.card().shown);
        CHECK(!d.takeCardChange());

        w.checkReady = true;
        w.checkOutcome.ok = true;
        w.checkOutcome.info = newerInfo();
        d.poll({});
        CHECK(d.takeCardChange());
        CHECK(!d.takeCardChange());                               // once
        CHECK(d.card().shown);
        CHECK(d.card().headline == "Important update: FoxSDR 9.9.9");
        CHECK(d.card().primary == "Download update");
        CHECK(anyContains(w.said, "9.9.9"));

        d.updatePrimaryClicked();
        CHECK(countOf(w.calls, "downloadStart:9.9.9") == 1);
        CHECK(d.takeCardChange());
        CHECK(d.card().primary == "Downloading...");
        CHECK_NEAR(d.card().progress, 0.0, 1e-6);
        d.updatePrimaryClicked();                                  // nothing to press while it downloads
        CHECK(countOf(w.calls, "downloadStart:9.9.9") == 1);

        w.progress = 0.5f;
        d.poll({});
        CHECK(d.takeCardChange());
        CHECK_NEAR(d.card().progress, 0.5, 1e-6);

        w.downloadReady = true;
        w.downloadOutcome.ok = true;
        w.downloadOutcome.path = "C:\\temp\\foxsdr-setup-9.9.9.exe";
        d.poll({});
        CHECK(d.takeCardChange());
        CHECK(d.card().primary == "Install now and restart");
        CHECK(d.card().note == "Downloaded and verified.");

        d.updatePrimaryClicked();
        CHECK(countOf(w.calls, "installerStart:C:\\temp\\foxsdr-setup-9.9.9.exe") == 1);
        CHECK(d.card().primary == "Starting the installer...");
        d.updatePrimaryClicked();                                  // a second press while it starts does nothing
        CHECK(countOf(w.calls, "installerStart:C:\\temp\\foxsdr-setup-9.9.9.exe") == 1);
        CHECK(closed == 0);                                        // not until the launch has answered
        w.installerReady = true;
        w.installerStarted = true;
        d.poll({});
        CHECK(closed == 1);                                        // the installer is running: this window closes for it
        d.poll({});
        CHECK(closed == 1);
    }

    // ---- a launch that fails, a download that fails, "Not now", a check that fails ----
    {
        FakeWorld w;
        int closed = 0;
        HostDuties d(w, [&] { ++closed; });
        w.facts["fact.config.interface"] = interfaceDoc(true, true);
        d.start(interactive());
        w.checkReady = true;
        w.checkOutcome.ok = true;
        w.checkOutcome.info = newerInfo();
        d.poll({});
        d.updatePrimaryClicked();
        w.downloadReady = true;
        w.downloadOutcome.ok = true;
        w.downloadOutcome.path = "C:\\temp\\foxsdr-setup-9.9.9.exe";
        d.poll({});
        d.updatePrimaryClicked();
        w.installerReady = true;
        w.installerStarted = false;
        d.poll({});
        CHECK(closed == 0);                                        // an installer that did not start does not close the window
        CHECK(d.card().error == "could not start the installer at C:\\temp\\foxsdr-setup-9.9.9.exe");
        CHECK(d.card().primary == "Install now and restart");      // it can be pressed again

        FakeWorld f;
        HostDuties e(f, [] {});
        f.facts["fact.config.interface"] = interfaceDoc(true, true);
        e.start(interactive());
        f.checkReady = true;
        f.checkOutcome.ok = true;
        f.checkOutcome.info = newerInfo();
        e.poll({});
        e.updatePrimaryClicked();
        f.downloadReady = true;
        f.downloadOutcome.ok = false;
        f.downloadOutcome.error = "the download's digest did not match the published one";
        e.poll({});
        CHECK(e.card().error == "the download's digest did not match the published one");   // a download failure IS shown, verbatim
        CHECK(e.card().primary == "Download update");              // and the key tries again
        e.updatePrimaryClicked();
        CHECK(countOf(f.calls, "downloadStart:9.9.9") == 2);
        CHECK(e.card().error.empty());                             // a new attempt clears the old failure

        FakeWorld g;
        HostDuties h(g, [] {});
        g.facts["fact.config.interface"] = interfaceDoc(true, true);
        h.start(interactive());
        g.checkReady = true;
        g.checkOutcome.ok = true;
        g.checkOutcome.info = newerInfo();
        h.poll({});
        CHECK(h.card().shown);
        h.updateDismissed();
        CHECK(h.takeCardChange());
        CHECK(!h.card().shown);                                    // "Not now" hides it for this launch
        h.poll({});
        CHECK(!h.card().shown);

        // THE KILL SWITCH through the duties (docs 6.63): the status the Updates row reads, the card, and no download of a withdrawn release
        {
            FakeWorld wf;
            HostDuties wd(wf, [] {});
            wf.facts["fact.config.interface"] = interfaceDoc(true, true);
            wd.start(interactive());
            wf.checkReady = true;
            wf.checkOutcome.ok = true;
            wf.checkOutcome.info = newerInfo();
            wf.checkOutcome.info.runningWithdrawn = true;
            wf.checkOutcome.info.offerWithdrawn = true;   // an inconsistent answer: the offer is withdrawn too, `newer` still set
            wd.poll({});
            const UpdateStatus st = wd.updateStatus();
            CHECK(st.runningWithdrawn);
            CHECK(!st.newer);                                      // the row is never told to offer it
            CHECK(wd.card().shown && wd.card().withdrawn && wd.card().primary.empty());
            wd.updatePrimaryClicked();                             // a key press that reaches startDownload is refused
            CHECK(countOf(wf.calls, "downloadStart:9.9.9") == 0);
            wd.updateDismissed();
            CHECK(!wd.card().shown);                               // Not now puts the notice away
            wd.updateShowAgain();
            CHECK(wd.card().shown && wd.card().withdrawn);         // and Show it again brings it back
        }

        FakeWorld k;
        HostDuties m(k, [] {});
        k.facts["fact.config.interface"] = interfaceDoc(true, true);
        m.start(interactive());
        k.checkReady = true;
        k.checkOutcome.ok = false;
        k.checkOutcome.error = "no network";
        m.poll({});
        CHECK(!m.card().shown);                                    // a failed check is never a banner: the user did not ask
        CHECK(!m.takeCardChange());
        CHECK(anyContains(k.said, "no network"));

        FakeWorld o;
        HostDuties q(o, [] {});
        o.facts["fact.config.interface"] = interfaceDoc(true, true);
        q.start(interactive());
        o.checkReady = true;
        o.checkOutcome.ok = true;
        o.checkOutcome.info = newerInfo();
        o.checkOutcome.info.newer = false;                         // the newest version is already running
        q.poll({});
        CHECK(!q.card().shown);
    }

    // ---- the console's close request, the sentinel's beat and the radio ----
    {
        FakeWorld w;
        int closed = 0;
        HostDuties d(w, [&] { ++closed; });
        w.facts["fact.config.interface"] = interfaceDoc(true, true);
        d.start(interactive());
        d.windowUp();
        CHECK(at(w.calls, w.calls.empty() ? 0 : w.calls.size() - 1) == "phase:awaiting-first-frame");
        TickInput opening;
        opening.deviceOpening = true;
        d.poll(opening);
        d.poll({});
        CHECK(countOf(w.calls, "radioOpening:1") == 1);
        CHECK(countOf(w.calls, "radioOpening:0") == 1);
        CHECK(countOf(w.calls, "beat") == 2);
        CHECK(countOf(w.calls, "sendersPoll") == 2);
        w.consoleClose = true;
        d.poll({});
        d.poll({});
        CHECK(closed == 1);                                        // Ctrl+C or a closing console ends the event loop, once
        CHECK(anyContains(w.said, "console"));

        FakeWorld c;
        HostDuties e(c, [] {});
        c.facts["fact.config.interface"] = interfaceDoc(true, true);
        e.start(interactive());
        for (int i = 0; i < 301; ++i) { e.poll({}); }
        CHECK(countOf(c.calls, "sentinelPoll") == 3);              // ticks 0, 150 and 300
        CHECK(countOf(c.calls, "beat") == 301);
        CHECK(e.ticks() == 301);
    }

    // ---- the exit: the order, and what is kept ----
    {
        FakeWorld w;
        HostDuties d(w, [] {});
        const core::UploadPolicyState mem = someMemory();
        w.facts["fact.config.interface"] = interfaceDoc(true, true, "beta", &mem);
        tellFacts(w);
        d.start(interactive());
        d.windowUp();
        d.poll({});
        CHECK(d.sweepRunning());
        // the sweep sent a report: the dedup and rate-limit memory moved
        core::UploadPolicyState after = mem;
        core::noteSent(after, "BBBBBBBBBBBBBBBB", 1'800'000'100ull);
        w.sweepOutcome.considered = 1;
        w.sweepOutcome.sent = 1;
        w.sweepOutcome.state = after;
        w.calls.clear();
        d.finish();
        w.calls.push_back("service.stop");                         // the host stops the service between finish() and closed()
        d.closed();
        CHECK((w.calls == Lines{"phase:shutdown-begun", "sweepStop", "sendersCancel", "downloadCancel", "submitInterfaceHalf", "service.stop",
                                "phase:shutdown-closing-window", "phase:finished", "updateCheckReap"}));
        CHECK(anyContains(w.diagged, "crash upload: 1 considered, 1 sent"));
        CHECK(anyContains(w.diagged, "event loop ended after 1 ticks; shutting down"));

        // the document the engine is handed is the half it served, with the sweep's memory in it
        CHECK(w.submitted.size() == 1);
        core::AppConfig back;
        std::string error;
        CHECK(core::parseInterfaceHalf(at(w.submitted, 0), back, error));
        CHECK(back.crashUploadRecent == core::encodePolicyRecent(after));
        CHECK(back.crashUploadWindowStart == after.windowStart);
        CHECK(back.crashUploadWindowCount == after.windowCount);
        CHECK(back.crashUploadBlockedUntil == after.blockedUntil);
        CHECK(back.updateChannel == "beta");                       // everything else of the half is as it was served
        CHECK(back.diagnosticsEnabled);
        CHECK(back.updateCheckEnabled);
        CHECK(d.crashMemory().windowCount == after.windowCount);
    }
    {
        // a sweep that changed nothing hands nothing back: the engine's copy stands
        FakeWorld w;
        HostDuties d(w, [] {});
        const core::UploadPolicyState mem = someMemory();
        w.facts["fact.config.interface"] = interfaceDoc(true, true, "stable", &mem);
        tellFacts(w);
        d.start(interactive());
        d.poll({});
        w.sweepOutcome.state = mem;
        d.finish();
        CHECK(countOf(w.calls, "sweepStop") == 1);
        CHECK(countOf(w.calls, "submitInterfaceHalf") == 0);

        // a run that never swept: nothing to stop, nothing to hand back, and the order is the order
        FakeWorld n;
        HostDuties e(n, [] {});
        n.facts["fact.config.interface"] = interfaceDoc(false, true);
        e.start(interactive());
        n.calls.clear();
        e.finish();
        n.calls.push_back("service.stop");
        e.closed();
        CHECK((n.calls == Lines{"phase:shutdown-begun", "sendersCancel", "downloadCancel", "service.stop", "phase:shutdown-closing-window",
                                "phase:finished", "updateCheckReap"}));

        // THE TABLE SERVED NO HALF: there is no baseline to hand back, and a half built from defaults would put the defaults over the
        // engine's copy of every window rectangle and switch, so nothing is handed back however the sweep ended
        FakeWorld u;
        HostDuties f(u, [] {});
        tellFacts(u);
        f.start(interactive());
        CHECK(anyContains(u.said, "did not serve"));
        f.poll({});
        CHECK(f.sweepRunning());                                   // the defaults stand: Diagnostics on, with a folder
        core::UploadPolicyState moved;
        core::noteSent(moved, "CCCCCCCCCCCCCCCC", 1'800'000'200ull);
        u.sweepOutcome.state = moved;
        f.finish();
        CHECK(countOf(u.calls, "sweepStop") == 1);
        CHECK(countOf(u.calls, "submitInterfaceHalf") == 0);
        CHECK(f.crashMemory().windowCount == moved.windowCount);   // still kept in memory for the log

        // THE ENGINE REFUSES THE DOCUMENT (a full queue, a session without the grant): said, never fatal
        FakeWorld r;
        HostDuties g(r, [] {});
        const core::UploadPolicyState served = someMemory();
        r.facts["fact.config.interface"] = interfaceDoc(true, true, "stable", &served);
        tellFacts(r);
        g.start(interactive());
        g.poll({});
        r.sweepOutcome.state = moved;
        r.submitOk = false;
        g.finish();
        CHECK(countOf(r.calls, "submitInterfaceHalf") == 1);
        CHECK(anyContains(r.said, "not taken by the engine"));
    }

    // ---- an unclean last run is said ----
    {
        FakeWorld w;
        HostDuties d(w, [] {});
        w.facts["fact.config.interface"] = interfaceDoc(true, true);
        DutyStart s = interactive();
        s.lastRunUnclean = true;
        d.start(s);
        CHECK(anyContains(w.said, "did not end normally"));
        FakeWorld c;
        HostDuties e(c, [] {});
        c.facts["fact.config.interface"] = interfaceDoc(true, true);
        e.start(interactive());
        CHECK(!anyContains(c.said, "did not end normally"));
    }

    // ---- the real world, without a table ----
    {
        setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", "");
        LiveWorld w(nullptr, nullptr);
        std::string v = "stale";
        CHECK(!w.fact("fact.config.interface", v));
        CHECK(v.empty());
        CHECK(!w.submitInterfaceHalf("{}"));
        CHECK(w.nowEpoch() > 1'600'000'000ull);
        CHECK(!w.inPackage());
        CHECK(!w.manifestSeam());
        CHECK(!w.consoleCloseRequested());                         // no handler is installed in a test
        CHECK(w.crashEndpoint().rfind("http", 0) == 0);
        w.sendersPoll(w.nowEpoch());                               // idle senders: nothing to poll, nothing to cancel, nothing thrown
        w.sendersCancel();
        w.sendersCancel();
        w.phase(core::breadcrumb::Phase::Running);                 // no page attached in a test: every call is a pointer test
        w.beat();
        w.radioOpening(true);
        w.radioOpening(false);
        CHECK(w.sweepStop().considered == 0);                      // nothing was started
    }

    // ---- the real sweep over an empty folder: no report, no request ----
    {
        const fs::path crashDir = scratchDir() / "crashes";
        std::error_code ec;
        fs::create_directories(crashDir, ec);
        LiveWorld w(nullptr, nullptr);
        core::SweepParams p;
        p.crashDir = crashDir.string();
        p.url = "http://127.0.0.1:9/never";                        // nothing listens there, and there is nothing to send
        p.enabled = true;
        w.sweepStart(p);
        const core::SweepOutcome out = w.sweepStop();
        CHECK(out.considered == 0);
        CHECK(out.sent == 0);
        CHECK(out.failed == 0);
        CHECK(w.sweepStop().considered == 0);                      // stopping twice is safe
    }

    // ---- the real update check over a canned manifest, and the card it makes ----
    {
        const fs::path manifest = scratchDir() / "update.json";
        const std::string body = std::string("{\"version\":\"9.9.9\",\"url\":\"https://foxsdr.com/download/foxsdr-setup-9.9.9.exe\",\"sha256\":\"") +
                                 std::string(64, 'a') +
                                 "\",\"size\":30000000,\"notes\":[{\"version\":\"9.9.9\",\"date\":\"2026-11-03\",\"critical\":false,"
                                 "\"notes\":[\"one\",\"two\"]}]}";
        writeFile(manifest, body);
        setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", manifest.string());
        {
            LiveWorld w(nullptr, nullptr);
            CHECK(w.manifestSeam());
            w.updateCheckStart("0.99.104", "");
            core::UpdateCheckOutcome out;
            bool got = false;
            for (int i = 0; i < 500 && !got; ++i) {
                got = w.updateCheckPoll(out);
                if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            }
            CHECK(got);
            CHECK(out.ok);
            CHECK(out.info.newer);
            CHECK(out.info.version == "9.9.9");
            UpdateView v;
            v.enabled = true;
            v.installerHere = true;
            v.info = out.info;
            const UpdateCardModel c = updateCardFor(v);
            CHECK(c.shown);
            CHECK(c.headline == "FoxSDR 9.9.9 is available");
            CHECK((c.notes == Lines{"9.9.9", "one", "two"}));
            CHECK(c.note == "30.0 MB, verified against its published checksum before anything runs");
        }
        {
            // THE WHOLE PATH, with real threads and no network: the duties over the real world in a bounded run that set the seam
            LiveWorld w(nullptr, nullptr);
            int closed = 0;
            HostDuties d(w, [&] { ++closed; });
            DutyStart s = bounded(5);
            s.configPath.clear();
            s.configLoaded = false;
            d.start(s);
            bool shown = false;
            for (int i = 0; i < 500 && !shown; ++i) {
                d.poll({});
                shown = d.card().shown;
                if (!shown) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            }
            CHECK(shown);
            CHECK(d.takeCardChange());
            CHECK(d.card().headline == "FoxSDR 9.9.9 is available");
            d.finish();
            d.closed();
            CHECK(closed == 0);
        }
        // a canned answer that is not newer, and one that cannot be read
        writeFile(manifest, std::string("{\"version\":\"0.0.1\",\"url\":\"https://foxsdr.com/download/foxsdr-setup-0.0.1.exe\",\"sha256\":\"") +
                                std::string(64, 'b') + "\"}");
        {
            LiveWorld w(nullptr, nullptr);
            w.updateCheckStart("0.99.104", "");
            core::UpdateCheckOutcome out;
            bool got = false;
            for (int i = 0; i < 500 && !got; ++i) {
                got = w.updateCheckPoll(out);
                if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            }
            CHECK(got);
            CHECK(out.ok);
            CHECK(!out.info.newer);
            UpdateView v;
            v.enabled = true;
            v.installerHere = true;
            v.info = out.info;
            CHECK(!updateCardFor(v).shown);
        }
        setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", (scratchDir() / "no-such-manifest.json").string());
        {
            LiveWorld w(nullptr, nullptr);
            w.updateCheckStart("0.99.104", "");
            core::UpdateCheckOutcome out;
            bool got = false;
            for (int i = 0; i < 500 && !got; ++i) {
                got = w.updateCheckPoll(out);
                if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            }
            CHECK(got);
            CHECK(!out.ok);
            CHECK(out.error == "the canned update manifest could not be read");
        }
        setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", "");
    }

    // ---- the real download refuses before it connects: another host, and nothing newer ----
    {
        LiveWorld w(nullptr, nullptr);
        core::UpdateInfo elsewhere = newerInfo();
        elsewhere.url = "https://evil.example.test/foxsdr-setup-9.9.9.exe";
        w.downloadStart(elsewhere);
        DownloadOutcome out;
        bool got = false;
        for (int i = 0; i < 500 && !got; ++i) {
            got = w.downloadPoll(out);
            if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }
        CHECK(got);
        CHECK(!out.ok);
        CHECK(out.error.find("not on foxsdr.com") != std::string::npos);
        CHECK(out.path.empty());
        CHECK(!w.downloadPoll(out));                               // answered once

        core::UpdateInfo nothing = newerInfo();
        nothing.newer = false;
        w.downloadStart(nothing);
        got = false;
        for (int i = 0; i < 500 && !got; ++i) {
            got = w.downloadPoll(out);
            if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }
        CHECK(got);
        CHECK(!out.ok);
        CHECK(out.error == "there is no update to download");
        w.downloadCancel();
    }

    // ---- the installer's launch is on a worker and answers once (the launcher is the test's: nothing is started) ----
    {
        LiveWorld w(nullptr, nullptr);
        std::vector<std::string> launched;
        w.setInstallerLauncher([&](const std::string& path) {
            launched.push_back(path);
            return path.find("good") != std::string::npos;
        });
        bool started = true;
        CHECK(!w.installerPoll(started));                          // nothing launched yet
        w.installerStart("C:\\temp\\good.exe");
        bool got = false;
        for (int i = 0; i < 500 && !got; ++i) {
            got = w.installerPoll(started);
            if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }
        CHECK(got);
        CHECK(started);
        CHECK(!w.installerPoll(started));
        w.installerStart("C:\\temp\\bad.exe");
        got = false;
        for (int i = 0; i < 500 && !got; ++i) {
            got = w.installerPoll(started);
            if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }
        CHECK(got);
        CHECK(!started);
        CHECK((launched == Lines{"C:\\temp\\good.exe", "C:\\temp\\bad.exe"}));
    }

    // ---- main.cpp holds the contract's order, from its text (as tests/test_engine_service_order.cpp holds src/main.cpp's) ----
    {
        std::ifstream in(std::string(FOXSDR_SLINT_SOURCE_DIR) + "/main.cpp", std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string raw = buffer.str();
        CHECK(!raw.empty());
        // the code without its // comments (a string literal's // is left alone)
        std::string code;
        {
            std::istringstream lines(raw);
            std::string line;
            while (std::getline(lines, line)) {
                bool quoted = false;
                std::size_t cut = std::string::npos;
                for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                    if (line[i] == '\\' && quoted) { ++i; continue; }
                    if (line[i] == '"') { quoted = !quoted; }
                    if (!quoted && line[i] == '/' && line[i + 1] == '/') { cut = i; break; }
                }
                code += (cut == std::string::npos ? line : line.substr(0, cut)) + "\n";
            }
        }
        const char* order[] = {"bootProcess(",       "EngineService service(", "service.acquireConfig(", "service.restore()",
                               "open_session(",      "service.start(",         "duties.start(",          "duties.windowUp()",
                               "run_event_loop()",   "duties.finish()",        "service.stop()",         "duties.closed()",
                               "api->close_session(session)"};   // the session last: closed earlier, it would drop the half finish() queued
        std::size_t last = 0;
        bool increasing = true;
        for (const char* token : order) {
            const std::size_t at = code.find(token, last);
            if (at == std::string::npos) {
                std::printf("main.cpp: \"%s\" is missing or out of order\n", token);
                increasing = false;
                break;
            }
            last = at;
        }
        CHECK(increasing);
        // what bootProcess owns now is gone from the window's main
        CHECK(code.find("FOXSDR_SLINT_CONFIG") == std::string::npos);
        CHECK(code.find("FOXSDR_SLINT_REAL") == std::string::npos);
        CHECK(code.find("slint-preview") == std::string::npos);
        CHECK(code.find("set_preview(true)") == std::string::npos);
        CHECK(code.find("set_preview(false)") != std::string::npos);
        // the window does not clear the update card behind the duties' back
        CHECK(code.find("set_update_shown(false)") == std::string::npos);
    }

    // ---- THE REAL TABLE: the half the engine serves is handed back with the sweep's memory, and the file the stop writes holds it ----
    // A real EngineService on a settings file of its own (main.cpp's order: restore, session, start), the real LiveWorld over its table, and a
    // sweep that is the test's (nothing is sent): what is real is the read of fact.config.interface, the facts that start the sweep, the
    // FOXAPP_OP_INTERFACE_CONFIG submit, the service's final pass and the save that follows it. `closeEarly` closes the session between
    // finish() and the stop, which main.cpp does not do - to see what that order would cost.
    struct MovedSweepWorld : LiveWorld {
        using LiveWorld::LiveWorld;
        bool started = false;
        core::SweepParams params;
        core::SweepOutcome outcome;
        void sweepStart(const core::SweepParams& p) override { started = true; params = p; }
        core::SweepOutcome sweepStop() override { return outcome; }
    };
    struct RealTable {
        bool ran = false;
        bool swept = false;
        bool memoryFromFile = false;
        bool newSignatureInFile = false;
        bool earlierSignatureInFile = false;
        bool windowCountInFile = false;
        bool cleanMarker = false;
    };
    const auto realTable = [&](const char* dirName, bool closeEarly) {
        RealTable r;
        const fs::path settings = scratchDir() / dirName / "config.json";
        std::error_code ec;
        fs::create_directories(settings.parent_path(), ec);
        // the file the engine will load: the crash-upload memory as an earlier run left it
        const core::UploadPolicyState earlier = someMemory();
        {
            core::AppConfig seeded;
            seeded.crashUploadRecent = core::encodePolicyRecent(earlier);
            seeded.crashUploadWindowStart = earlier.windowStart;
            seeded.crashUploadWindowCount = earlier.windowCount;
            seeded.volume = 0.0;
            std::string error;
            CHECK(core::ConfigStore::save(settings.string(), seeded, error));
        }
        cascade::engine::WindowServiceRun run;
        run.configPath = settings.string();
        run.frames = 0;                        // a bounded run: no receiver, no heartbeat, no network
        cascade::engine::ServiceOptions options = cascade::engine::windowServiceOptions(run);
        options.stepped = false;               // as main.cpp: this window has no pass to step
        cascade::engine::EngineService service(options);
        const cascade::engine::EngineService::Restored restored = service.restore();
        CHECK(restored.loaded);
        const FoxEngineApi* api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        FoxEngine* handle = service.engine().apiHandle();
        CHECK(api != nullptr);
        CHECK(handle != nullptr);
        FoxSession* session = nullptr;
        if (api != nullptr && handle != nullptr) {
            FoxSessionParams sp{};
            sp.structSize = sizeof(sp);
            sp.flags = FOXAPI_SESSION_LOCAL | FOXAPI_SESSION_FRAME;
            sp.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;
            sp.clientName = "test_slint_duties";
            sp.clientVersion = "0";
            CHECK(api->open_session(handle, &sp, &session) == FOXAPI_OK);
        }
        std::string why;
        const bool started = session != nullptr && service.start(why);
        CHECK(started);
        if (!started) {
            if (session != nullptr) { service.stop(); api->close_session(session); }
            return r;
        }
        r.ran = true;
        MovedSweepWorld world(api, session);
        HostDuties duties(world, [] {});
        DutyStart s = interactive();
        s.configPath = settings.string();
        s.configLoaded = restored.loaded;
        s.diagnosticsDir = (scratchDir() / "crashes").string();
        s.frames = 0;
        s.lastRunUnclean = restored.lastRunUnclean;
        duties.start(s);
        // the engine's first publish makes the telemetry facts answer; the sweep starts on the poll that sees them
        for (int i = 0; i < 500 && !world.started; ++i) {
            duties.poll({});
            if (!world.started) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }
        r.swept = world.started;
        // the memory the sweep starts from is the file's, through the table
        r.memoryFromFile = world.params.state.recent == earlier.recent && world.params.state.windowCount == earlier.windowCount &&
                           world.params.crashDir == s.diagnosticsDir;
        core::UploadPolicyState moved = earlier;
        core::noteSent(moved, "DDDDDDDDDDDDDDDD", 1'800'000'300ull);
        world.outcome.considered = 1;
        world.outcome.sent = 1;
        world.outcome.state = moved;
        duties.finish();
        if (closeEarly) { api->close_session(session); }
        service.stop();                        // one whole pass first, which applies the half; then the final-state save
        duties.closed();
        if (!closeEarly) { api->close_session(session); }
        std::string text;
        {
            std::ifstream in(settings, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            text = buffer.str();
        }
        r.newSignatureInFile = text.find("DDDDDDDDDDDDDDDD 1800000300") != std::string::npos;
        r.earlierSignatureInFile = text.find("AAAAAAAAAAAAAAAA 1799990000") != std::string::npos;
        core::AppConfig reread;
        std::string error;
        CHECK(core::ConfigStore::load(settings.string(), reread, error));
        r.windowCountInFile = reread.crashUploadWindowCount == moved.windowCount;
        r.cleanMarker = reread.telemetryCleanExit;
        return r;
    };
    {
        const RealTable good = realTable("engine", false);
        CHECK(good.ran);
        CHECK(good.swept);
        CHECK(good.memoryFromFile);
        CHECK(good.newSignatureInFile);        // the new signature is in the file the stop wrote
        CHECK(good.earlierSignatureInFile);    // and the earlier one, kept
        CHECK(good.windowCountInFile);
        CHECK(good.cleanMarker);               // the stop wrote the clean-exit marker
        // WHY MAIN CLOSES THE SESSION LAST (measured here, against the real engine): a session closed before the stop drops what it had queued
        // (the table says so: only the commands that make the transmitter safer are still applied), and the half finish() handed over is one
        // of the commands that is dropped - the sweep's memory would never reach the file.
        const RealTable early = realTable("engine-early", true);
        CHECK(early.ran);
        CHECK(early.swept);
        CHECK(!early.newSignatureInFile);
    }

    // ---- the SYSTEM bank's door (docs 6.39): the half the bank reads and saves, the diagnostics switches, the update check re-asked ----
    {
        const auto docOf = [](const std::string& document) {
            core::AppConfig parsed;
            std::string error;
            CHECK(core::parseInterfaceHalf(document, parsed, error));
            return parsed;
        };
        FakeWorld w;
        HostDuties d(w, [] {});
        w.facts["fact.config.interface"] = interfaceDoc(true, true);
        d.start(interactive());
        CHECK(d.interfaceHalf().diagnosticsEnabled && d.interfaceHalf().updateCheckEnabled && d.interfaceHalf().uiTheme == "today");
        CHECK(w.submitted.empty());

        // the first save goes at once: the whole half, with the one change in it
        core::AppConfig half = d.interfaceHalf();
        half.uiTheme = "night";
        CHECK(d.saveInterfaceHalf(half));
        CHECK(w.submitted.size() == 1);
        CHECK(d.interfaceHalf().uiTheme == "night");
        CHECK(docOf(w.submitted.back()).uiTheme == "night" && docOf(w.submitted.back()).diagnosticsEnabled);
        // a second inside the gap is kept and held: the next poll that is a gap later sends it, with both changes
        half.interfaceScale = "150";
        CHECK(!d.saveInterfaceHalf(half));
        CHECK(w.submitted.size() == 1 && d.interfaceHalf().interfaceScale == "150");
        for (int i = 0; i < HostDuties::kInterfaceSendGapTicks - 2; ++i) { d.poll({}); }
        CHECK(w.submitted.size() == 1);
        d.poll({});
        d.poll({});
        CHECK(w.submitted.size() == 2);
        CHECK(docOf(w.submitted.back()).interfaceScale == "150" && docOf(w.submitted.back()).uiTheme == "night");
        for (int i = 0; i < HostDuties::kInterfaceSendGapTicks * 2; ++i) { d.poll({}); }
        CHECK(w.submitted.size() == 2);   // nothing held, nothing sent
        // a slider dragged every tick sends once per gap, and the last value is never lost
        for (int i = 0; i < 30; ++i) {
            half.dbMin = -150.f + static_cast<float>(i);
            (void)d.saveInterfaceHalf(half);
            d.poll({});
        }
        const std::size_t afterDrag = w.submitted.size();
        CHECK(afterDrag >= 4 && afterDrag <= 2 + 30 / HostDuties::kInterfaceSendGapTicks + 3);   // about one document per gap, not thirty
        for (int i = 0; i < HostDuties::kInterfaceSendGapTicks * 3; ++i) { d.poll({}); }
        CHECK(docOf(w.submitted.back()).dbMin == -121.f);
        // the Diagnostics switch is not made to wait
        half.dbMin = -100.f;
        CHECK(d.saveInterfaceHalf(half));   // the gap has passed: at once
        const std::size_t before = w.submitted.size();
        half.dbMin = -99.f;
        CHECK(!d.saveInterfaceHalf(half));  // inside the gap: held
        CHECK(w.submitted.size() == before);
        half.diagnosticsEnabled = false;
        CHECK(d.saveInterfaceHalf(half));   // a flip of the switch goes at once, with everything held
        CHECK(w.submitted.size() == before + 1 && !docOf(w.submitted.back()).diagnosticsEnabled && docOf(w.submitted.back()).dbMin == -99.f);
        // what is still held when the loop ends goes with the exit's last half
        half.uiTheme = "glass";
        (void)d.saveInterfaceHalf(half);
        const std::size_t beforeFinish = w.submitted.size();
        d.finish();
        CHECK(w.submitted.size() == beforeFinish + 1 && docOf(w.submitted.back()).uiTheme == "glass");
        // ...and nothing is sent twice
        FakeWorld q;
        HostDuties e(q, [] {});
        q.facts["fact.config.interface"] = interfaceDoc(true, true);
        e.start(interactive());
        e.finish();
        CHECK(q.submitted.empty());   // nothing changed, the sweep's memory did not move: nothing to hand back
    }
    {
        // a half the engine did not serve is kept for the window and never handed over (the defaults would go over the engine's copy)
        FakeWorld w;
        HostDuties d(w, [] {});
        DutyStart hermetic = bounded(3);
        hermetic.configPath.clear();
        hermetic.configLoaded = false;
        d.start(hermetic);
        core::AppConfig half = d.interfaceHalf();
        half.uiTheme = "field";
        CHECK(!d.saveInterfaceHalf(half));
        CHECK(d.interfaceHalf().uiTheme == "field");
        CHECK(w.submitted.empty() && anyContains(w.said, "kept for this run and not saved"));
        d.finish();
        CHECK(w.submitted.empty());
        // an engine that refuses the document is asked again a gap later, and the refusal is said once
        FakeWorld r;
        HostDuties f(r, [] {});
        r.facts["fact.config.interface"] = interfaceDoc(true, true);
        f.start(interactive());
        r.submitOk = false;
        core::AppConfig h2 = f.interfaceHalf();
        h2.uiTheme = "daylight";
        CHECK(!f.saveInterfaceHalf(h2));
        CHECK(r.submitted.size() == 1);
        for (int i = 0; i < HostDuties::kInterfaceSendGapTicks * 3; ++i) { f.poll({}); }
        CHECK(r.submitted.size() >= 3 && countOf(r.said, "config: the interface's half of the settings was not taken by the engine; asking again later") == 1);
        r.submitOk = true;
        for (int i = 0; i < HostDuties::kInterfaceSendGapTicks; ++i) { f.poll({}); }
        const std::size_t taken = r.submitted.size();
        for (int i = 0; i < HostDuties::kInterfaceSendGapTicks * 2; ++i) { f.poll({}); }
        CHECK(r.submitted.size() == taken);   // taken: no longer held
    }
    {
        // the diagnostics switches reach the crash handler with the user's minidump choice
        FakeWorld w;
        HostDuties d(w, [] {});
        w.facts["fact.config.interface"] = interfaceDoc(true, true);
        d.start(interactive());
        core::AppConfig half = d.interfaceHalf();
        half.diagnosticsEnabled = true;
        half.diagnosticsMinidump = true;
        (void)d.saveInterfaceHalf(half);
        d.diagnosticsChanged();
        CHECK(countOf(w.calls, "applyDiagnosticsSwitches:1:1") == 1);
        CHECK(anyContains(w.diagged, "diagnostics: recording on, memory dump on"));
        half.diagnosticsEnabled = false;
        (void)d.saveInterfaceHalf(half);
        d.diagnosticsChanged();
        CHECK(countOf(w.calls, "applyDiagnosticsSwitches:0:1") == 1);
    }
    {
        // the Updates tick and the channel: off discards the answer, on asks again, another channel asks again, a check in flight is left alone
        FakeWorld w;
        HostDuties d(w, [] {});
        w.facts["fact.config.interface"] = interfaceDoc(true, true);
        d.start(interactive());
        CHECK(countOf(w.calls, "updateCheckStart:0.99.104:") == 1);
        UpdateStatus s = d.updateStatus();
        CHECK(s.enabled && s.started && s.pending && !s.newer && !s.packaged && s.unseenMessages == 0);
        core::AppConfig half = d.interfaceHalf();
        half.updateChannel = "beta";
        (void)d.saveInterfaceHalf(half);
        d.updateSettingChanged();   // the first check is still out: its answer is the one wanted
        CHECK(countOf(w.calls, "updateCheckStart:0.99.104:beta") == 0);
        // the answer comes: an update, two team messages (one already marked Seen)
        core::UpdateInfo info = newerInfo();
        core::TeamMessage m1, m2;
        m1.id = "m1";
        m1.lines = {"hello"};
        m2.id = "m2";
        m2.lines = {"again"};
        info.messages = {m1, m2};
        w.checkReady = true;
        w.checkOutcome.ok = true;
        w.checkOutcome.info = info;
        half = d.interfaceHalf();
        half.messagesSeen = {"m1"};
        (void)d.saveInterfaceHalf(half);
        d.poll({});
        s = d.updateStatus();
        CHECK(s.started && !s.pending && s.newer && s.critical && s.newVersion == "9.9.9" && s.error.empty() && s.unseenMessages == 1 && !s.dismissed);
        CHECK(d.card().shown);
        // "Not now" hides the card; "Show it again" brings it back
        d.updateDismissed();
        CHECK(d.updateStatus().dismissed && !d.card().shown);
        d.updateShowAgain();
        CHECK(!d.updateStatus().dismissed && d.card().shown);
        // the channel changed with nothing in flight: the answer is dropped and the check is asked again, for the channel chosen
        d.updateSettingChanged();
        CHECK(countOf(w.calls, "updateCheckStart:0.99.104:beta") == 1);
        s = d.updateStatus();
        CHECK(s.pending && !s.newer && s.newVersion.empty() && !d.card().shown);
        // off: the answer is discarded and nobody is asked
        w.checkReady = true;
        w.checkOutcome.ok = true;
        w.checkOutcome.info = info;
        d.poll({});
        CHECK(d.updateStatus().newer);
        half = d.interfaceHalf();
        half.updateCheckEnabled = false;
        (void)d.saveInterfaceHalf(half);
        d.updateSettingChanged();
        CHECK(!d.updateStatus().enabled && !d.updateStatus().newer && !d.card().shown && countOf(w.calls, "updateCheckStart:0.99.104:beta") == 1);
        // on again: a re-tick asks again
        half.updateCheckEnabled = true;
        (void)d.saveInterfaceHalf(half);
        d.updateSettingChanged();
        CHECK(countOf(w.calls, "updateCheckStart:0.99.104:beta") == 2 && d.updateStatus().pending);
        // a check that failed is said in the status (the card never shows it)
        w.checkReady = true;
        w.checkOutcome.ok = false;
        w.checkOutcome.error = "no route";
        d.poll({});
        s = d.updateStatus();
        CHECK(!s.pending && s.started && s.error == "no route" && !d.card().shown);

        // a Store copy and a bounded run ask nobody, however often the tick is toggled
        FakeWorld p;
        p.packaged = true;
        HostDuties e(p, [] {});
        p.facts["fact.config.interface"] = interfaceDoc(true, true);
        e.start(interactive());
        core::AppConfig ph = e.interfaceHalf();
        ph.updateCheckEnabled = false;
        (void)e.saveInterfaceHalf(ph);
        e.updateSettingChanged();
        ph.updateCheckEnabled = true;
        (void)e.saveInterfaceHalf(ph);
        e.updateSettingChanged();
        CHECK(countOf(p.calls, "updateCheckStart:0.99.104:") == 0 && e.updateStatus().packaged && !e.updateStatus().pending);
        FakeWorld b;
        HostDuties f(b, [] {});
        b.facts["fact.config.interface"] = interfaceDoc(true, true);
        f.start(bounded(30));
        core::AppConfig bh = f.interfaceHalf();
        bh.updateChannel = "beta";
        (void)f.saveInterfaceHalf(bh);
        f.updateSettingChanged();
        CHECK(countOf(b.calls, "updateCheckStart:0.99.104:beta") == 0);
    }

    std::error_code cleanup;
    fs::remove_all(scratchDir(), cleanup);
    return testSummary("test_slint_duties");
}
