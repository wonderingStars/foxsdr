// engine_consent.cpp - TWO RUNNING COPIES ON ONE PROFILE: the engine's half of core/consent_file.hpp (0.99.115, brief 83, review 4 item D1).
//
// WHAT THE ENGINE DOES, by role:
//
//   THE COPY THAT HOLDS THE SETTINGS LOCK (holder)
//     - looks at the file a few times a second (consentPoll: its time and size, then the fields only when those moved) and when the disk disagrees with what it holds it ADOPTS the disk, at
//       once: a Usage switch the disk says is OFF runs the same OFF edge as a click in this copy (usageSwitchEdge), an ON is adopted from empty under the identifier the disk holds; the
//       Diagnostics and update-check switches follow the disk the same way, and the windows are told through fact.consent.generation (they own the sentinel, the log, the sweep).
//     - is asked before EVERY send (core/consent_file.hpp setSendHook): a report whose switch the disk says is OFF is refused whatever the memory says, and the next pass adopts.
//     - writes the consent fields only as the writer's overlay (the disk's own values) and as a merge on a click.
//   THE COPY WITHOUT THE LOCK (second copy)
//     - keeps both data families off for its whole run: no identifier, no ledgers, no journal or queue, no beat, no failure report, and the interface's half it serves says Diagnostics and the
//       update check are off, so the windows start no sweep and no check (telemetryStartup). It does not touch the holder's files: it deletes none of them.
//     - never writes the consent fields (the writer's overlay lays the disk's over everything of the family). A click in it (the Usage consent command, a moved Diagnostics or update-check
//       switch in the interface's half) is written into the file at once as a merge, for the holder to adopt; this copy itself goes on collecting nothing, and re-checks a click of its own a
//       few times in case a write of another copy met it (ConsentVerify).
//
// Nothing here is for a run without a settings file (setConsentGuard is not called: a hermetic run behaves as it always did).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/consent_file.hpp"
#include "core/crash_upload.hpp"
#include "core/diag_log.hpp"
#include "core/telemetry.hpp"
#include "core/health_events.hpp"
#include "core/version.hpp"
#include "engine/engine.hpp"

namespace cascade::engine {

namespace consent = cascade::core::consent;

namespace {
constexpr double kConsentPollS = 0.5;       // the holder's look at the file (its time and size: one stat)
constexpr int kVerifyLooks = 2;             // a second copy re-checks its own click this many times ...
constexpr double kVerifyEveryS = 1.5;       // ... this far apart
bool startsWith(const std::string& s, const std::string& prefix) { return !prefix.empty() && s.compare(0, prefix.size(), prefix) == 0; }
}  // namespace

void Engine::setConsentGuard(const std::string& configPath, bool holder) {
    consentPath_ = configPath;
    consentHolder_ = holder;
    consentGuarded_ = !configPath.empty();
    consentWatch_.reset();
    consent::clearSendHook(this);
    if (!consentGuarded_) { return; }
    consentWatch_ = std::make_unique<consent::Watch>(configPath);
    if (holder) {
        consent::setSendHook(this, [this](const std::string& url) { return consentAllowsSend(url); });
    } else {
        cascade::core::diagLogf("settings: another FoxSDR program holds the settings lock, so this copy keeps usage reporting and diagnostics off for the whole run and leaves those fields of the file alone");
    }
}

// Asked from any thread, at the one place a report's bytes are handed to the network. Only the two data families' destinations are checked; anything else is not a data switch's.
bool Engine::consentAllowsSend(const std::string& url) {
    if (!consentGuarded_ || !consentWatch_) { return true; }
    const bool crash = startsWith(url, cascade::core::crashUploadEndpoint());
    // (the beta tester's session report is the window's, and the window follows the Usage fact: this file names no tester destination - the host-only rule, tests/test_tester_host_only.cpp)
    const bool usage = !crash && startsWith(url, cascade::core::telemetryEndpoint());
    if (!crash && !usage) { return true; }
    consent::Fields f;
    consentWatch_->read(f);
    if (!f.ok) { return true; }   // the file cannot be read now: what this copy holds stands
    const bool ok = crash ? f.diagnostics : (f.usageEnabled && !f.installId.empty());
    if (!ok) { consentDirty_.store(true); }   // the next pass adopts the OFF at once
    return ok;
}

void Engine::consentFillUsageFamily(cascade::core::AppConfig& c) const {
    c.telemetryEnabled = telemetryEnabled_;
    c.telemetryInstallId = telemetryInstallId_;
    c.telemetryFirstRun = telemetryFirstRun_;
    c.telemetryFirstVersion = telemetryFirstVersion_;
    c.telemetryLaunches = telemetryLaunches_;
    c.telemetryCrashes = telemetryCrashes_;
    c.telemetryExitsDied = telemetryExits_.died;
    c.telemetryExitsKilled = telemetryExits_.killed;
    c.telemetryExitsEnded = telemetryExits_.ended;
    c.telemetryExitsUnknown = telemetryExits_.unknown;
    c.telemetryPending.clear();   // a click leaves no record behind: the next period's record is its own
}

// A click's write, on a worker (no disk on the control thread). `source` carries the new values; only `keys` are written into the file. The counter lets the pass tell a write in flight from
// a file that disagrees with this copy (it must not adopt the disk back over its own click).
void Engine::consentMergeAsync(const cascade::core::AppConfig& source, const std::vector<std::string>& keys, bool verifyAfter) {
    if (!consentGuarded_) { return; }
    const std::string document = cascade::core::ConfigStore::serialize(source);
    const std::string path = consentPath_;
    auto counter = consentWrites_;
    counter->fetch_add(1);
    try {
        std::thread([path, document, keys, counter]() {
            std::string error;
            if (!consent::mergeKeys(path, document, keys, error)) {
                cascade::core::diagWarnf("settings: a switch could not be written into the settings file (%s)", error.c_str());
                cascade::core::health::noteRecovered(cascade::core::health::Recovered::CfgSave);
            }
            counter->fetch_sub(1);
        }).detach();
    } catch (...) {
        counter->fetch_sub(1);
        std::string error;
        (void)consent::mergeKeys(path, document, keys, error);
    }
    if (verifyAfter) {
        consentVerify_.left = kVerifyLooks;
        consentVerify_.nextS = clockS() + kVerifyEveryS;
        consentVerify_.document = document;
        consentVerify_.keys = keys;
    }
}

void Engine::consentStartWrites() {
    if (!consentMintPending_) { return; }
    consentMintPending_ = false;
    if (!consentGuarded_ || !consentHolder_) { return; }
    cascade::core::AppConfig c;
    consentFillUsageFamily(c);
    consentMergeAsync(c, consent::usageKeys(), /*verifyAfter=*/false);
}

void Engine::consentFlush(int boundMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMs);
    while (consentWrites_->load() > 0 && std::chrono::steady_clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}

// THE USAGE SWITCH'S EDGE (the consent command's body, and an adoption of the disk by the holder): ONE function, so the two cannot differ.
void Engine::usageSwitchEdge(bool on, const std::string& idFromDisk, bool fromDisk) {
    if (on) {
        telemetryInstallId_ = fromDisk ? idFromDisk : cascade::core::newInstallId();
        telemetryEnabled_ = !telemetryInstallId_.empty();
    } else {
        telemetryEnabled_ = false;
        telemetryInstallId_.clear();
    }
    // RULE R (0.99.115, brief 79): ONE clearing function for the whole usage family at this edge; the two ledgers follow in the same step: off disarms them and removes their files, on arms
    // them fresh - nothing loaded from a file of an earlier period.
    usageFamilyClear(telemetryEnabled_ ? UsageEdge::On : UsageEdge::Off);
    if (telemetryEnabled_) {
        stallLedger_->arm(telemetryLedgerPath_, telemetryInstallId_, /*loadExisting=*/false);
        healthLedger_->arm(healthLedgerPath_, telemetryInstallId_, /*loadExisting=*/false);
    } else {
        stallLedger_->disarm();
        healthLedger_->disarm();
    }
    // THE HEARTBEAT FOLLOWS THE SWITCH IN THE SAME STEP: off disarms it (configure refuses the now-empty id), on arms it with the new id.
    telemetryHeartbeat_.configure(cascade::core::telemetryEndpoint(), telemetryInstallId_, cascade::versionString());
    // THE RADIO REPORTS FOLLOW THE SWITCH: off deletes EVERYTHING queued and everything held about radios; on starts the journal under the new id.
    radioReportsRefreshIdentity();
    // THE EDGE IS MADE DURABLE AT ONCE (brief 80): the keeper saves the next snapshot without its debounce window.
    switchEdges_.fetch_add(1);
    // A CLICK WRITES THE CONSENT FIELDS INTO THE FILE (brief 83): as a merge, at once. An adoption of the disk has nothing to write: the disk already says it.
    if (!fromDisk && consentGuarded_) {
        cascade::core::AppConfig c;
        consentFillUsageFamily(c);
        consentMergeAsync(c, consent::usageKeys(), /*verifyAfter=*/false);
    }
}

// A click on the Usage switch in a copy without the lock: written into the file for the holder to adopt, and nothing else - this copy keeps both families off.
FoxCommandResult Engine::secondCopyUsageClick(bool on, FoxCommandResult res) {
    consent::Fields f;
    if (consentWatch_) { consentWatch_->read(f, /*force=*/true); }
    const bool diskOn = f.ok && f.usageEnabled && !f.installId.empty();
    if (on == diskOn) {
        res.status = FOXAPI_NO_CHANGE;
        res.flags |= FOXAPI_RESULT_REFUSED;
        std::snprintf(res.message, sizeof(res.message), "%s", on ? "usage reporting is already on" : "usage reporting is already off");
        return res;
    }
    cascade::core::AppConfig c;   // defaults: no counters, no record
    if (on) {
        c.telemetryInstallId = cascade::core::newInstallId();
        c.telemetryEnabled = !c.telemetryInstallId.empty();
        c.telemetryLaunches = 1;
        c.telemetryFirstRun = cascade::core::utcDateToday();
        c.telemetryFirstVersion = cascade::versionString();
    } else {
        c.telemetryEnabled = false;
    }
    c.telemetryPending.clear();
    consentMergeAsync(c, consent::usageKeys(), /*verifyAfter=*/true);
    return res;
}

// The holder follows the disk's Diagnostics and update-check switches (the interface's half holds them). The windows own what these switches start and stop, so they are told by the generation.
void Engine::consentAdoptSwitches(const consent::Fields& f) {
    bool diagMoved = false;
    bool updateMoved = false;
    {
        std::lock_guard<std::mutex> lk(interfaceHalfMutex_);
        if (interfaceCfg_.diagnosticsEnabled != f.diagnostics) { interfaceCfg_.diagnosticsEnabled = f.diagnostics; diagMoved = true; }
        if (interfaceCfg_.updateCheckEnabled != f.updateCheck) { interfaceCfg_.updateCheckEnabled = f.updateCheck; updateMoved = true; }
    }
    if (diagMoved) {
        if (!controlDiagDir_.empty()) { setControlWatchdogReportDir(f.diagnostics ? controlDiagDir_ : std::string()); }
        diagnosticsFamilyEdge(f.diagnostics, /*atStart=*/false);
    }
    if (diagMoved || updateMoved) { consentGen_.fetch_add(1); }
}

void Engine::consentPoll(bool force) {
    if (!consentGuarded_ || !consentWatch_) { return; }
    const double now = clockS();
    const bool dirty = consentDirty_.exchange(false);
    // A MINTED IDENTIFIER THAT IS NOT IN THE FILE YET (consentStartWrites has not run: a run that has not started) is not "OFF on the disk".
    if (consentHolder_ && consentMintPending_) { return; }
    if (!consentHolder_) {
        // The copy without the lock: only the re-check of a click of its own, a few times.
        if (consentVerify_.left > 0 && now >= consentVerify_.nextS && consentWrites_->load() == 0) {
            consentVerify_.left -= 1;
            consentVerify_.nextS = now + kVerifyEveryS;
            consent::Fields f;
            consentWatch_->read(f, /*force=*/true);
            const nlohmann::json want = nlohmann::json::parse(consentVerify_.document, nullptr, false);
            bool lost = false;
            if (want.is_object() && f.ok) {
                for (const std::string& k : consentVerify_.keys) {
                    if (k == "telemetryEnabled") {
                        const bool wantOn = want.value("telemetryEnabled", false) && !want.value("telemetryInstallId", std::string()).empty();
                        lost = lost || (wantOn != (f.usageEnabled && !f.installId.empty()));
                    } else if (k == "diagnosticsEnabled") {
                        lost = lost || (want.value("diagnosticsEnabled", true) != f.diagnostics);
                    } else if (k == "updateCheckEnabled") {
                        lost = lost || (want.value("updateCheckEnabled", true) != f.updateCheck);
                    }
                }
            }
            if (lost) {
                std::vector<std::string> keys = consentVerify_.keys;
                cascade::core::AppConfig c;
                std::string error;
                (void)cascade::core::ConfigStore::parse(consentVerify_.document, "consent", c, error);
                consentMergeAsync(c, keys, /*verifyAfter=*/false);
                consentVerify_.left = 0;
            }
        }
        return;
    }
    if (!force && !dirty && consentLastPollS_ >= 0.0 && now - consentLastPollS_ < kConsentPollS) { return; }
    consentLastPollS_ = now;
    // A click of this copy is still being written: the file is allowed to disagree with the memory until it lands (the disk must not be adopted back over the click).
    if (consentWrites_->load() > 0) { consentLookAgain_ = true; return; }
    consent::Fields f;
    const bool fresh = consentWatch_->read(f, force || dirty);
    if (!fresh && !force && !dirty && !consentLookAgain_) { return; }
    consentLookAgain_ = false;
    if (!f.ok) { return; }
    // USAGE: OFF on the disk wins at once; an ON the disk holds that this copy does not is adopted from empty.
    const bool diskOn = f.usageEnabled && !f.installId.empty();
    if (telemetryEnabled_ && (!diskOn || f.installId != telemetryInstallId_)) {
        cascade::core::diagLogf("settings: usage reporting was switched %s in another running copy; this copy follows", diskOn ? "to another identity" : "off");
        usageSwitchEdge(false, std::string(), /*fromDisk=*/true);
    }
    if (!telemetryEnabled_ && diskOn) {
        cascade::core::diagLogf("settings: usage reporting was switched on in another running copy; this copy follows, from empty");
        usageSwitchEdge(true, f.installId, /*fromDisk=*/true);
    }
    consentAdoptSwitches(f);
}

bool Engine::consentFact(const std::string& key, std::string& value) const {
    if (key == "holder") { value = consentHolder_ ? "1" : "0"; return true; }
    if (key == "generation") { value = std::to_string(consentGen_.load()); return true; }
    std::lock_guard<std::mutex> lk(interfaceHalfMutex_);
    if (key == "diagnostics") { value = interfaceCfg_.diagnosticsEnabled ? "1" : "0"; return true; }
    if (key == "updateCheck") { value = interfaceCfg_.updateCheckEnabled ? "1" : "0"; return true; }
    return false;
}

}  // namespace cascade::engine
