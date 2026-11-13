// engine_wire_c.cpp - the engine's side of wiring lane C: the op block 0x8A00-0x8AFF, its facts, its once-a-pass pump.
// The settings registry and the state are in wire_c.{hpp,cpp}; each feature of the lane adds its own file next to this one and one `case` here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"
#include "core/plugin_repo_helper.hpp"   // wire-c
#include "core/safe_file_io.hpp"
#include "core/store_sort.hpp"
#include "engine/wire_c.hpp"

namespace cascade::engine {

namespace wc = cascade::engine::wirec;

wc::State& Engine::wireC() {
    std::lock_guard<std::mutex> lk(wireCMutex_);   // (the first caller may be the control pass or the fact builder: whichever asks first builds it)
    if (!wireC_) {
        const std::filesystem::path folder = wireCFolderForTest_.empty()
            ? std::filesystem::path(cascade::core::ConfigStore::defaultPath()).parent_path()
            : std::filesystem::path(wireCFolderForTest_);
        // the data folder (plugins, logs): the per-user one (%LOCALAPPDATA%\foxsdr, or the XDG data folder), or - in a test - a sibling of the scratch folder; both are named foxsdr
        std::filesystem::path data;
        if (!wireCFolderForTest_.empty()) {
            data = folder.parent_path() / "wcdata" / "foxsdr";
        } else if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
            data = std::filesystem::path(local) / "foxsdr";
        } else if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
            data = std::filesystem::path(xdg) / "foxsdr";
        } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            data = std::filesystem::path(home) / ".local" / "share" / "foxsdr";
        }
        wireC_ = std::make_unique<wc::State>(folder, data);
        {   // what the last start did with a staged import or reset (written by bootProcess step 2c)
            std::string boot;
            if (cascade::core::safeio::readCapped(folder / "wirec-last-boot.txt", 4096, boot) == cascade::core::safeio::ReadResult::Ok) { wireC_->sysLastBoot = boot; }
        }
    }
    return *wireC_;
}

std::int32_t Engine::applyWireC(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    (void)res;
    switch (c.op) {
        case FOXAPP_OP_WIREC_SETTING: {
            const std::size_t sep = text.find('\x1F');
            if (sep == std::string::npos || sep == 0) {
                why = "the text is \"<key>\\x1F<value>\"";
                return FOXAPI_BAD_ARGUMENT;
            }
            switch (wireC().settings().set(text.substr(0, sep), text.substr(sep + 1), why)) {
                case wc::SetResult::Changed: ++wireCVersion_; return FOXAPI_OK;
                case wc::SetResult::NoChange: return FOXAPI_NO_CHANGE;
                case wc::SetResult::NotFound: return FOXAPI_NOT_FOUND;
                case wc::SetResult::BadValue: return FOXAPI_BAD_ARGUMENT;
                case wc::SetResult::OutOfRange: return FOXAPI_OUT_OF_RANGE;
                case wc::SetResult::SaveFailed: return FOXAPI_FAILED;
            }
            return FOXAPI_FAILED;
        }
        case FOXAPP_OP_SYS_FIRSTRUN:
        case FOXAPP_OP_SYS_EXPORT:
        case FOXAPP_OP_SYS_IMPORT_PLAN:
        case FOXAPP_OP_SYS_IMPORT_COMMIT:
        case FOXAPP_OP_SYS_BACKUP:
        case FOXAPP_OP_SYS_RESET_PLAN:
        case FOXAPP_OP_SYS_RESET_COMMIT:
        case FOXAPP_OP_SYS_PENDING_CANCEL: {
            wc::State& s = wireC();
            wc::SysOps& sys = s.sys();
            s.sysError.clear();
            bool ok = false;
            std::int32_t bad = FOXAPI_BAD_ARGUMENT;
            switch (c.op) {
                case FOXAPP_OP_SYS_FIRSTRUN: {
                    ok = sys.wizardVerb(text, why);
                    if (ok && sys.wizardFinished()) {
                        const cascade::core::sys::WizardOutcome out = sys.wizardOutcome();
                        if (out.applied) {
                            if (!out.bookmarks.empty()) {
                                freqMgr_.addMany(out.bookmarks);
                                saveBookmarks();
                            }
                            if (sys.telemetryChosen()) { (void)applyCommand(cascade::core::cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, out.telemetryEnabled ? 1 : 0)); }
                        }
                        (void)sys.markDone();
                    }
                    break;
                }
                case FOXAPP_OP_SYS_EXPORT: {
                    std::size_t stripped = 0;
                    ok = sys.exportTo(text, why, &stripped);
                    if (ok) { s.sysExportLast = "exported to the file you chose (" + std::to_string(stripped) + " secret or identity item(s) left out)"; }
                    break;
                }
                case FOXAPP_OP_SYS_IMPORT_PLAN:
                    ok = sys.importPlan(text, s.sysImportPlan, s.sysImportToken, s.sysImportNotes, why);
                    if (!ok) { s.sysImportPlan.clear(); s.sysImportToken.clear(); s.sysImportNotes.clear(); }
                    break;
                case FOXAPP_OP_SYS_IMPORT_COMMIT:
                    ok = sys.importCommit(text, why);
                    if (ok) { s.sysImportToken.clear(); }
                    break;
                case FOXAPP_OP_SYS_BACKUP: {
                    std::string name;
                    const std::size_t keep = c.ival[0] > 0 ? static_cast<std::size_t>(std::min<std::int64_t>(c.ival[0], 50)) : static_cast<std::size_t>(s.settings().getInt("sys.backup_keep"));
                    ok = sys.backupNow(keep, name, why);
                    if (ok) { s.sysBackupLast = name; }
                    break;
                }
                case FOXAPP_OP_SYS_RESET_PLAN:
                    ok = sys.resetPlan(c.ival[0] != 0, c.ival[1] != 0, s.sysResetPlan, s.sysResetToken, why);
                    if (!ok) { s.sysResetPlan.clear(); s.sysResetToken.clear(); }
                    break;
                case FOXAPP_OP_SYS_RESET_COMMIT:
                    ok = sys.resetStage(text, why);
                    if (ok) { s.sysResetToken.clear(); }
                    break;
                default:   // SYS_PENDING_CANCEL
                    ok = sys.cancelPending(why);
                    bad = FOXAPI_NO_CHANGE;
                    break;
            }
            ++wireCVersion_;
            if (!ok) {
                s.sysError = why;
                return bad;
            }
            return FOXAPI_OK;
        }
        case FOXAPP_OP_QSO_ADD:
        case FOXAPP_OP_QSO_REMOVE:
        case FOXAPP_OP_QSO_CLEAR:
        case FOXAPP_OP_CABRILLO_HEADER:
        case FOXAPP_OP_CABRILLO_EXPORT:
        case FOXAPP_OP_ADIF_EXPORT: {
            wc::State& s = wireC();
            wc::QsoBook& book = s.qsos();
            s.hamError.clear();
            bool ok = false;
            std::int32_t bad = FOXAPI_BAD_ARGUMENT;
            switch (c.op) {
                case FOXAPP_OP_QSO_ADD:
                    ok = book.add(text, static_cast<std::int64_t>(std::time(nullptr)), why);
                    if (!ok && why == "the log is full") { bad = FOXAPI_LIMIT; }
                    break;
                case FOXAPP_OP_QSO_REMOVE:
                    ok = book.remove(c.ival[0] > 0 ? static_cast<std::size_t>(c.ival[0]) : 0u, why);
                    bad = FOXAPI_NOT_FOUND;
                    break;
                case FOXAPP_OP_QSO_CLEAR:
                    ok = book.clear();
                    if (!ok) { why = "the log is empty"; }
                    bad = FOXAPI_NO_CHANGE;
                    break;
                case FOXAPP_OP_CABRILLO_HEADER:
                    ok = book.setHeader(text, why);
                    break;
                case FOXAPP_OP_CABRILLO_EXPORT: {
                    int lines = 0, skipped = 0;
                    ok = book.exportCabrillo(text, s.hamCabrilloProblems, lines, skipped, why);
                    if (ok) { s.hamExportLast = "Cabrillo: " + std::to_string(lines) + " QSO line(s) written, " + std::to_string(skipped) + " skipped"; }
                    break;
                }
                default: {   // ADIF_EXPORT
                    int written = 0, skipped = 0;
                    ok = book.exportAdif(text, written, skipped, why);
                    if (ok) { s.hamExportLast = "ADIF: " + std::to_string(written) + " record(s) written, " + std::to_string(skipped) + " skipped"; }
                    break;
                }
            }
            ++wireCVersion_;
            if (!ok) {
                s.hamError = why;
                return bad;
            }
            return FOXAPI_OK;
        }
        case FOXAPP_OP_STORE_HELPER_CONSENT: {
            if (commandFromRemote_) {
                why = "the plugin store's helper programs are fitted from this computer only";
                return FOXAPI_DENIED;
            }
            for (const cascade::core::PluginCatalogEntry& ce : catalog_) {
                if (cascade::core::PluginRepo::catalogueKey(ce.source, ce.id) != text) { continue; }
                if (!ce.helper.present) {
                    why = "that plugin does not run a separate program";
                    return FOXAPI_NOT_FOUND;
                }
                const bool had = wireCHelperConsent_.count(text) != 0;
                if (c.ival[0] != 0) { wireCHelperConsent_.insert(text); } else { wireCHelperConsent_.erase(text); }
                if ((c.ival[0] != 0) == had) {
                    why = "already so";
                    return FOXAPI_NO_CHANGE;
                }
                ++wireCVersion_;
                return FOXAPI_OK;
            }
            why = "no catalogue entry with that key";
            return FOXAPI_NOT_FOUND;
        }
        default:
            break;
    }
    why = "not an operation of this block";
    return FOXAPI_UNSUPPORTED;
}

void Engine::wireCFacts(const std::function<void(const std::string&, const std::string&)>& fact) {
    wc::State& s = wireC();
    s.settings().each([&fact](const std::string& key, const std::string& value) { fact("wirec." + key, value); });
    fact("wirec.version", std::to_string(wireCVersion()));
    fact("wirec.error", s.settings().error());
    // POWER AND RESOURCES (wire_c_power.hpp). "" is "not available" - a reading that could not be made is never shown as 0.
    // CAT EMULATION (row L7)
    fact("cat.emulation.running", s.catEmu.running() ? "1" : "0");
    fact("cat.emulation.port", s.catEmu.running() ? std::to_string(s.catEmu.boundPort()) : std::string());
    fact("cat.emulation.clients", std::to_string(s.catEmu.clients()));
    fact("cat.emulation.commands", std::to_string(s.catEmu.commands()));
    fact("cat.emulation.error", s.catEmu.error());
    // THE LOGGER'S RADIO INFORMATION (row H6): what the program we follow says its radios are doing; LOCAL only like every fact of this lane
    fact("ham.radioinfo.running", s.radioInfo.running() ? "1" : "0");
    fact("ham.radioinfo.port", s.radioInfo.running() ? std::to_string(s.radioInfo.boundPort()) : std::string());
    fact("ham.radioinfo.error", s.radioInfo.error());
    for (int n = 1; n <= 4; ++n) {
        for (const char* what : {"freq_hz", "tx_hz", "mode", "split", "transmitting", "app"}) { fact("ham.radioinfo." + std::to_string(n) + "." + what, s.radioInfo.value(n, what)); }
    }
    // THE QSO LOG (wire_c_ham.hpp): counts and sentences only - never a call sign, a frequency or an exchange
    fact("ham.qso.count", std::to_string(s.qsos().size()));
    fact("ham.error", s.hamError);
    fact("ham.log.error", s.qsos().error());
    fact("ham.export.last", s.hamExportLast);
    fact("ham.cabrillo.problems", s.hamCabrilloProblems);
    {
        std::string ids;
        for (const cascade::core::ham::ContestDef& d : cascade::core::ham::cabrilloContests()) { ids += (ids.empty() ? "" : "\n") + d.id; }
        fact("ham.cabrillo.contests", ids);
    }
    // THE SYSTEM OPERATIONS (wire_c_sys.hpp): what they last said, and the first-run wizard's state
    fact("sys.error", s.sysError);
    fact("sys.export.last", s.sysExportLast);
    fact("sys.import.plan", s.sysImportPlan);
    fact("sys.import.token", s.sysImportToken);
    fact("sys.import.notes", s.sysImportNotes);
    fact("sys.reset.plan", s.sysResetPlan);
    fact("sys.reset.token", s.sysResetToken);
    fact("sys.backup.last", s.sysBackupLast);
    fact("sys.lastboot", s.sysLastBoot);
    fact("sys.pending", s.sys().pendingKind());
    fact("sys.firstrun.needed", s.sys().firstRunNeeded() ? "1" : "0");
    fact("sys.firstrun.step", s.sys().wizardStepName());
    fact("sys.firstrun.done", s.sys().wizardFinished() ? "1" : "0");
    fact("sys.firstrun.radios", s.sys().radioLines());
    fact("sys.firstrun.presets", s.sys().presetLines());
    // (always published, so a client can read them before the first pass has made the power object; the perf.* facts are read at the call: wireCLiveFact)
    fact("sys.keepawake", s.hasPower() && s.power().keepAwakeHeld() && s.power().keepAwakeActive() ? "1" : "0");
    fact("sys.resume.count", std::to_string(s.resumeActions));
}

// The resource read-out and the reduced-effects switches, read at the call from the sampler's last published figures. "" is "not available" (never 0).
bool Engine::wireCLiveFact(const std::string& key, std::string& value) {
    if (key.rfind("perf.", 0) != 0) { return false; }
    wc::State& s = wireC();
    wc::PerfFacts f;
    if (s.hasPower()) { f = s.power().perf(); }
    if (key == "perf.cpu_percent") { value = f.cpuPercent; }
    else if (key == "perf.memory_mb") { value = f.memoryMb; }
    else if (key == "perf.system_memory_percent") { value = f.systemMemoryPercent; }
    else if (key == "perf.gpu_percent") { value = f.gpuPercent; }
    else if (key == "perf.reduced") { value = f.reduced ? "1" : "0"; }
    else if (key == "perf.switch.ui_frame_cap_hz") { value = std::to_string(f.switches.uiFrameCapHz); }
    else if (key == "perf.switch.waterfall_rows_per_second") { value = std::to_string(f.switches.waterfallRowsPerSecond); }
    else if (key == "perf.switch.animations") { value = f.switches.animations ? "1" : "0"; }
    else if (key == "perf.switch.glow_and_shadows") { value = f.switches.glowAndShadows ? "1" : "0"; }
    else if (key == "perf.switch.three_d_views_live") { value = f.switches.threeDViewsLive ? "1" : "0"; }
    else if (key == "perf.switch.background_map_redraw") { value = f.switches.backgroundMapRedraw ? "1" : "0"; }
    else { return false; }
    return true;
}

// The engine's teardown: every listener and session of this lane is let go (called from shutdownQuiesce, before the control thread stops).
void Engine::wireCShutdown() {
    std::lock_guard<std::mutex> lk(wireCMutex_);
    if (wireC_) { wireC_->shutdown(); }
}

std::uint64_t Engine::wireCVersion() {
    return wireCVersion_ + wireC().settings().version();
}

// Anything recording right now: an audio or I/Q take, the IF or multiplex take of any radio, or a scheduled capture that is starting or recording. The state, not the
// start and stop sites, so a take that ends any way at all ends the hold (wire_c_power.hpp).
bool Engine::wireCRecordingBusy() const {
    for (const auto& r : receivers_) {
        if (r && (r->iqRecorder_.recording() || r->audioRecorder_.recording() || r->ifRecorder_.recording() || r->mpxRecorder_.recording())) { return true; }
    }
    for (const auto& [id, rt] : scheduleRt_) {
        (void)id;
        if (rt.state == "recording" || rt.state == "starting") { return true; }
    }
    return false;
}

// What the engine does after the PC wakes: the radios are looked for again (USB ones re-enumerate while it sleeps) and a saved radio that was waiting is tried at
// once, the same two steps the hot-plug notice takes. It does NOT close and reopen a radio that is open and healthy: whether that is wanted is not settled.
void Engine::wireCResumeAction() {
    scanNative();
    scanSoapy();
    if (rx().savedRadioRetryBusy_) {
        rx().savedRadioRetryAttempts_ = 0;
        rx().savedRadioRetryLastFailSec_ = -1.0e9;
    }
}

// A take a plugin asked for: its label first in the file name ("tg-1234_foxsdr_audio_....wav"). Only while a plugin's take is being started.
void Engine::wireCLabelTake(cascade::core::Recorder::OpenRequest& req) {
    if (wireCStartLabel_.empty() || req.path.empty()) { return; }
    const std::filesystem::path p(req.path);
    req.path = (p.parent_path() / (wireCStartLabel_ + "_" + p.filename().string())).string();
}

// THE RECORDER A PLUGIN MAY ASK FOR (row P1). The request was checked against the SETTINGS grant when it was made and is checked again here (a grant taken away, a plugin
// stopped or unloaded in between). The take is the radio's own audio take, started and stopped exactly as the user's Record key does; one take per radio, never one
// the user started (a start while the radio is recording is refused and logged), and a plugin can end only the take it started. A plugin that goes away has its take ended.
void Engine::wireCPluginRecorder() {
    cascade::core::PluginApiCore& api = *pluginApi_;
    std::vector<cascade::core::PluginApiCore::RecorderRequest> reqs;
    std::vector<int> ages;
    for (const WireCDeferredStart& d : wireCDeferred_) {
        reqs.push_back(d.req);
        ages.push_back(d.passes);
    }
    wireCDeferred_.clear();
    api.takeRecorderRequests(reqs);
    ages.resize(reqs.size(), 0);
    for (std::size_t qi = 0; qi < reqs.size(); ++qi) {
        const auto& r = reqs[qi];
        if (!api.recorderRequestStillAllowed(r)) { continue; }
        const std::uint32_t radio = api.clientRadio(r.client);
        if (!receiverLive(radio)) { continue; }
        RadioWork asked(*this, static_cast<std::size_t>(radio) - 1u);
        const std::size_t slot = static_cast<std::size_t>(radio) - 1u;
        if (r.start) {
            if (rx().audioRecorder_.recording() || rx().audioStart_.pending() || wireCPluginTakes_.count(slot) != 0) {
                cascade::core::diagLogf("recorder: a plugin's take was refused, this radio is already recording");
                continue;
            }
            if (takeFinishing(false)) {
                // the previous file is still being closed: the recorder would queue the start and forget the label, so it waits here instead
                if (ages[qi] < 120) { wireCDeferred_.push_back({r, ages[qi] + 1}); }
                else { cascade::core::diagLogf("recorder: a plugin's take was dropped, the previous file was not closed in time"); }
                continue;
            }
            wireCStartLabel_ = r.label;
            const bool started = startAudioRecording();
            wireCStartLabel_.clear();
            if (started) { wireCPluginTakes_[slot] = r.client; }
            else { cascade::core::diagLogf("recorder: a plugin's take could not start"); }
        } else {
            const auto it = wireCPluginTakes_.find(slot);
            if (it != wireCPluginTakes_.end() && it->second == r.client) {
                stopAudioRecording();
                wireCPluginTakes_.erase(it);
            }
        }
    }
    // a take whose plugin has gone (stopped, unloaded, detached), or that the user ended: ended / forgotten
    for (auto it = wireCPluginTakes_.begin(); it != wireCPluginTakes_.end();) {
        const std::size_t slot = it->first;
        const cascade::core::PluginApiCore::RecorderRequest probe{it->second, true, {}};   // (a START is allowed only while the plugin is live, not stopped and holds the grant)
        bool drop = false;
        if (slot >= receivers_.size() || !receivers_[slot]) {
            drop = true;
        } else {
            RadioWork asked(*this, slot);
            if (!api.recorderRequestStillAllowed(probe)) {
                stopAudioRecording();
                drop = true;
            } else if (!rx().audioRecorder_.recording() && !rx().audioStart_.pending()) {
                drop = true;   // the take is over (the user's Stop, or an error)
            }
        }
        it = drop ? wireCPluginTakes_.erase(it) : std::next(it);
    }
}

// THE ORDER OF THE STORE'S LIST (row W9; core/store_sort.hpp): the catalogue's own order by default (today's), or by name, or newest first. The sort is stable and
// total (ties fall back to name, id, site), so a list never shuffles between two builds of the block. The popularity and rating orders need data from a server that
// does not exist, so they are not offered.
std::vector<std::size_t> Engine::wireCStoreOrder() {
    const std::string key = wireC().settings().get("store.sort");
    if (key == "name") { return cascade::core::sortStoreEntries(catalog_, cascade::core::StoreStatsMap{}, cascade::core::StoreSortKey::Name); }
    if (key == "updated") { return cascade::core::sortStoreEntries(catalog_, cascade::core::StoreStatsMap{}, cascade::core::StoreSortKey::Updated); }
    std::vector<std::size_t> idx(catalog_.size());
    for (std::size_t i = 0; i < idx.size(); ++i) { idx[i] = i; }
    return idx;
}

void Engine::pumpWireC() {
    wireCPluginRecorder();
    wc::State& s = wireC();
    wc::Power& p = s.power();
    if (!s.powerStarted) {
        s.powerStarted = true;
        p.startSampler();
        p.startResumeWatch();
    }
    const std::string mode = s.settings().get("perf.mode");
    p.setPerfMode(mode == "on" ? cascade::core::sys::EffectsMode::On : mode == "off" ? cascade::core::sys::EffectsMode::Off : cascade::core::sys::EffectsMode::Auto);
    if (p.keepAwakePass(s.settings().getBool("sys.keep_awake") && wireCRecordingBusy())) { ++wireCVersion_; }
    // WEB: the cross-origin allow-list (net/web_wire_c.hpp). The server takes a new list at any time; a list it refuses is logged and the old one stays.
    {
        const std::string origins = s.settings().get("web.cors_origins");
        if (origins != s.corsApplied) {
            std::vector<std::string> lines;
            std::size_t from = 0;
            while (from <= origins.size()) {
                std::size_t to = origins.find('\n', from);
                if (to == std::string::npos) { to = origins.size(); }
                std::string line = origins.substr(from, to - from);
                if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                if (!line.empty()) { lines.push_back(line); }
                from = to + 1;
            }
            std::string err;
            if (servers().webServer().setCors(lines, err)) { s.corsApplied = origins; }
            else { s.corsApplied = origins; cascade::core::diagLogf("web: the cross-origin list was refused (%s)", err.c_str()); }
        }
    }
    // THE RADIO INFORMATION OF A LOGGER (row H6; wire_c_net.hpp): a UDP listener polled here, receive only, off by default; a change of settings reopens it
    {
        const bool on = s.settings().getBool("hamops.radioinfo.enabled");
        const std::string addr = s.settings().get("hamops.radioinfo.address");
        const int port = static_cast<int>(s.settings().getInt("hamops.radioinfo.port"));
        const bool other = s.settings().getBool("hamops.radioinfo.allow_other");
        const std::string sig = std::string(on ? "1" : "0") + "|" + addr + "|" + std::to_string(port) + "|" + (other ? "1" : "0");
        if (sig != s.radioInfoApplied) {
            s.radioInfoApplied = sig;
            s.radioInfo.configure(on, addr, port, other);
            ++wireCVersion_;
        }
        if (s.radioInfo.poll(static_cast<std::int64_t>(std::time(nullptr)))) { ++wireCVersion_; }
    }
    // CAT EMULATION (row L7; wire_c_cat.hpp): a TCP port that answers like a rig, receive only, off by default; a change of settings reopens it
    {
        const bool on = s.settings().getBool("cat.emulation.enabled");
        const std::string dialect = s.settings().get("cat.emulation.dialect");
        const int port = static_cast<int>(s.settings().getInt("cat.emulation.port"));
        const bool all = s.settings().getBool("cat.emulation.bind_all");
        const std::string sig = std::string(on ? "1" : "0") + "|" + dialect + "|" + std::to_string(port) + "|" + (all ? "1" : "0");
        if (sig != s.catEmuApplied) {
            s.catEmuApplied = sig;
            wc::CatEmulation::Opener opener;
            opener.table = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
            opener.open = [this]() -> FoxSession* {
                std::int32_t st = FOXAPI_OK;
                return api().openInternalSession(InternalKind::Cat, "CAT emulation", &st);
            };
            s.catEmu.configure(on, dialect, port, all, opener);
            ++wireCVersion_;
        }
        if (s.catEmu.running()) {
            const std::uint64_t seen = static_cast<std::uint64_t>(s.catEmu.clients()) + (s.catEmu.commands() << 8);
            if (seen != s.catEmuSeen) {
                s.catEmuSeen = seen;
                ++wireCVersion_;
            }
        }
    }
    // THE METRICS PAGE (row L3): switched with web.metrics; while on, about once a second a snapshot of counts and levels goes to the page (never a decoded text, a callsign, a
    // position or an address: the decode tally counts events per plugin NAME).
    {
        const bool metricsOn = s.settings().getBool("web.metrics");
        if (metricsOn != s.metricsApplied) {
            servers().webServer().setMetricsEnabled(metricsOn);
            s.metricsApplied = metricsOn;
            s.metricsLastMs = 0;
        }
        if (metricsOn) {
            const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowMs - s.metricsLastMs >= 1000) {
                s.metricsLastMs = nowMs;
                for (const CascadeEvent& ev : pluginApi_->hostEvents(s.eventsTallied)) {
                    s.eventsTallied = std::max<std::uint64_t>(s.eventsTallied, ev.seq);
                    ++s.decodeTally[ev.plugin];
                }
                cascade::net::metrics::EngineSnapshot snap;
                snap.decodesByDecoder = s.decodeTally;
                snap.droppedSamples = rx().pipeline_.ringDroppedSamples();
                const double level = static_cast<double>(rx().pipeline_.signalPowerDb());
                if (std::isfinite(level) && level > -199.0) { snap.signalLevelDbfs = level; }
                servers().webServer().publishMetrics(snap);
            }
        }
    }
    if (p.takeResume()) {
        if (s.settings().getBool("sys.resume_reopen")) {
            cascade::core::diagLogf("power: the PC woke from sleep; looking for the radios again");
            wireCResumeAction();
            ++s.resumeActions;
        }
        ++wireCVersion_;
    }
}

}  // namespace cascade::engine
