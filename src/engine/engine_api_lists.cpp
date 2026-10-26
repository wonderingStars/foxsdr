// engine_api_lists.cpp - the lists the engine API's read_list serves
// (engine/fox_engine_api.hpp, ApiListBlock), built by the engine on its
// pumping thread from its own members - the same members the Source panel,
// the bookmarks page and the Sinks combo are drawn from.
//
// ONLY WHEN SOMETHING CHANGED. The fingerprint is a hash of everything the
// block would carry, computed without allocating; the block itself (and the
// copy of the bookmark list, which can hold tens of thousands) is rebuilt
// only when the fingerprint moves - and the fingerprint is also what moves
// FoxReceiverState::listSeq, in the same publish.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "core/i18n.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_levels.hpp"
#include "core/patch_plan.hpp"
#include "core/plugin_host.hpp"
#include "core/serial_port.hpp"
#include "engine/device_scan_plan.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver_tables.hpp"
#include "engine/server_host.hpp"
#include "engine/soundcard_panel.hpp"
#include "net/web_policy.hpp"
#include "source/airspy_source.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "source/soundcard_source.hpp"

namespace cascade::engine {

namespace {

// FNV-1a, 64 bit: enough to tell one list block from the next.
struct Fingerprint {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void text(const std::string& s) {
        bytes(s.data(), s.size());
        const unsigned char end = 0xFF;  // so "ab","c" differs from "a","bc"
        bytes(&end, 1);
    }
    // A C string, hashed as text() hashes its std::string (no temporary).
    void cstr(const char* s) {
        bytes(s, std::strlen(s));
        const unsigned char end = 0xFF;
        bytes(&end, 1);
    }
    template <class T>
    void value(const T& v) {
        bytes(&v, sizeof(v));
    }
};

// Does a durable list of module file names (the grants the config holds) name `key`? An empty key never matches,
// the rule PluginUi::tuneAllowed and PluginStopSet follow.
bool listHolds(const std::vector<std::string>& list, const std::string& key) {
    return !key.empty() && std::find(list.begin(), list.end(), key) != list.end();
}

// A GAIN STAGE ITS OWN AGC IS DRIVING (API 0.3 8.3, FOXAPI_ITEM_AGC): an
// Airspy's LNA or MIXER with its automatic gain control on. The stage is named
// by the Airspy driver, so the rule is the panel's own (the "driven" test it
// drew greyed sliders by).
bool gainDrivenByAgc(const cascade::source::AirspySource* airspy, const std::string& stage) {
    if (airspy == nullptr) { return false; }
    return (stage == "LNA" && airspy->lnaAgc()) || (stage == "MIXER" && airspy->mixerAgc());
}

// The file name a catalogue entry's build for THIS machine installs under: the sanitised name, exactly what
// Engine::catalogEntryInstalled and installedPluginRecord compare against the host's records. "" with no build for
// this os and architecture, or a name the loader's own rule refuses (API 0.3 8.9, fact.store.<id>.file).
std::string catalogueFileName(const cascade::core::PluginCatalogEntry& e) {
    const cascade::core::PluginPlatform* p = e.thisPlatform();
    if (p == nullptr) { return std::string(); }
    std::string want;
    std::string err;
    if (!cascade::core::PluginRepo::sanitiseFileName(p->file, want, err)) { return std::string(); }
    return want;
}

const char* nodeKindWord(cascade::core::patch::NodeKind k) {
    using K = cascade::core::patch::NodeKind;
    switch (k) {
        case K::Radio: return "radio";
        case K::Channel: return "channel";
        case K::Demod: return "demod";
        case K::Decoder: return "decoder";
        case K::Display: return "display";
        case K::Sink: return "sink";
        case K::Map: return "map";
    }
    return "";
}

const char* idleWord(cascade::core::DecoderIdleReason r) {
    using R = cascade::core::DecoderIdleReason;
    switch (r) {
        case R::Running: return "";
        case R::RateMismatch: return "rate mismatch";
        case R::CreateFailed: return "create failed";
        case R::NoAudioTable: return "no audio table";
        case R::PollFailed: return "poll failed";
    }
    return "";
}

// The patch graph's own fingerprint, from its fields: no document text is
// written for it (graphCommandText allocates; this is once a pass).
std::uint64_t graphHash(const cascade::core::patch::Graph& g) {
    Fingerprint f;
    for (const cascade::core::patch::Node& n : g.nodes()) {
        f.value(n.id);
        f.value(n.kind);
        f.text(n.name);
        f.value(n.x);
        f.value(n.y);
        f.value(n.w);
        f.value(n.h);
        f.value(n.freqHz);
        f.value(n.mode);
        f.text(n.plugin);
        f.text(n.device);
        f.value(n.rateHz);
        f.value(n.squelch);
        f.value(n.squelchDb);
        f.value(n.on);
        f.value(n.centreChosen);
    }
    for (const cascade::core::patch::Wire& w : g.wires()) { f.value(w); }
    f.value(g.nextId());
    return f.h;
}

}  // namespace

void Engine::refreshApiPluginKeys() {
    Fingerprint f;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        f.text(p.path);
        f.text(p.name);
    }
    if (f.h == apiPluginPathsHash_ && apiPluginKeys_.size() == pluginHost_.plugins().size()) { return; }
    apiPluginPathsHash_ = f.h;
    apiPluginKeys_.clear();
    apiPluginIds_.clear();
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        apiPluginKeys_.push_back(cascade::core::pluginKey(p));
        apiPluginIds_.push_back(cascade::core::pluginRunId(apiPluginKeys_.back()));
    }
}

std::uint64_t Engine::apiListsFingerprint() {
    Fingerprint f;
    f.value(modeIndex_);
    f.value(vfoBandwidthHz_);
    f.value(freqMgr_.version());
    f.value(freqMgr_.list().size());
    // The DSP chain's input rate (API 0.3 8.5, fact.input.rateHz): the source's rate
    // unless the chain refused a change of it, and then the one the display is
    // measured against.
    f.value(pipeline_.inputRateHz());
    f.text(sourceKind_);
    f.text(deviceArgs_);
    for (const auto& d : nativeDevices_) {
        f.text(d.driver);
        f.text(d.label);
        f.text(d.args);
    }
    for (const auto& d : soapyDevices_) {
        f.text(d.label);
        f.text(d.args);
    }
    const bool haveDevice = device_ != nullptr;
    f.value(haveDevice);
    cascade::source::AirspySource* const airspy = asAirspyDevice();
    if (haveDevice) {
        for (double r : deviceRatesHz_) { f.value(r); }
        f.value(pipeline_.activeSource().sampleRateHz());
        for (std::size_t i = 0; i < deviceGainRanges_.size(); ++i) {
            const auto& g = deviceGainRanges_[i];
            f.text(g.name);
            f.value(g.minDb);
            f.value(g.maxDb);
            f.value(g.stepDb);
            f.value(g.unit);  // the GAINS entry's unit token (0.3)
            f.value(gainDrivenByAgc(airspy, g.name));  // FOXAPI_ITEM_AGC (0.3, 8.3)
            if (i < deviceGainsDb_.size()) { f.value(deviceGainsDb_[i]); }
        }
        for (const auto& a : deviceAntennas_) { f.text(a); }
        f.text(deviceAntenna_);
    }
    for (const auto& a : devices_) {
        f.text(a.name);
        f.value(a.index);
    }
    f.value(deviceIndex_);
    refreshApiPluginKeys();
    f.value(apiPluginPathsHash_);
    for (std::size_t i = 0; i < pluginHost_.plugins().size(); ++i) {
        const cascade::core::LoadedPlugin& p = pluginHost_.plugins()[i];
        f.value(p.loaded);
        f.value(pluginRunModeById(apiPluginIds_[i]) == cascade::core::PluginRun::Stopped);  // (by the cached id: nothing allocates)
        // How it runs (PLUGINS value[3]): the run state and whether it is asleep - from the cached ids, nothing allocates.
        f.value(static_cast<int>(pluginRunModeById(apiPluginIds_[i])));
        f.value(pluginRunModeById(apiPluginIds_[i]) == cascade::core::PluginRun::Auto && !pluginShouldRunById(p, apiPluginIds_[i]));
        // API 0.3 8.7: what PLUGINS and DECODERS say beyond that - the capability word, the settings grant and the
        // "mute audio while running" setting. The keys are the cached ones: nothing here allocates.
        f.value(p.capabilities);
        f.value(listHolds(pluginSettingsAllowed_, apiPluginKeys_[i]));
        f.value(pluginMutesFor(p, apiPluginKeys_[i]));
        // API 0.3 8.8, what the fitted-modules window draws a module from: the descriptor's strings and the
        // refusal (which only a rescan changes, and are hashed beside the scan count so a refused file replaced
        // by another refused file cannot keep a stale reason), and the module's newest WARN/ERROR - by level and
        // text, so the same warning said again every pass is no change. In place, from the cached keys.
        f.text(p.version);
        f.text(p.author);
        f.text(p.licence);
        f.text(p.error);
        const auto notice = pluginNotices_.find(apiPluginKeys_[i]);
        if (notice != pluginNotices_.end()) {
            f.value(notice->second.level);
            f.text(notice->second.text);
        } else {
            f.value(std::uint32_t{0});
        }
    }
    f.value(pluginScans_);  // the files' sizes, and everything else a rescan can change, as one value
    f.text(pluginDir_);
    // Which modules have asked for a setting, whose command keys changed, and whether any module's idle sentence
    // moved: counters the plugin API and the runner keep, so none of the three builds a vector or copies a row.
    f.value(pluginApi_->settingsRequestersSeq());
    f.value(pluginApi_->commandsSeq());
    f.value(pluginRunner_.statusGeneration());
    // The install records' verdict on each file (integrity: not the file that was installed).
    for (const cascade::core::InstalledPlugin& r : pluginInventory_.plugins) {
        f.text(r.file);
        f.text(r.id);
        f.text(r.source);  // (where the module came from: plugin.origin.<file>, stub campaign B3)
        f.value(r.digestMismatch);
        f.value(static_cast<std::uint64_t>(r.abiVersion));  // (fact.plugin.<file>.record, stub campaign B5: what the Fitted modules window draws of it)
        f.value(static_cast<std::int64_t>(r.installedAtUnix));
    }
    // ...the tune grants held for any module - PLUGINS' TUNE_ALLOWED is a plugin's key in this list, and
    // STALE_GRANTS lists the ones no loaded module holds - the last refused request to tune
    // (fact.plugin.lastDenied), and the plugin-mute cluster (MUTE_CAUSES, fact.mute.*): who is muting, by
    // which module file, and whether the mute is held after a tune-away.
    for (const std::string& g : pluginTuneAllowed_) { f.text(g); }
    f.text(lastDeniedNow());
    for (const std::string& n : mutedBy_) { f.text(n); }
    for (const std::string& k : mutedByKeys_) { f.text(k); }
    f.value(muteKeptRunning_);
    // Which decoders are FED: the keys the pass's publish already took from
    // the runner in ONE acquisition of its lock (the lock its DSP thread
    // takes per block) - never asked again here.
    //
    // HASHED WHETHER OR NOT THE RECEIVER RUNS (API 0.3 8.8): PLUGINS' MATCHED is the runner's answer for any kind
    // of plugin and is read with the receiver stopped. The pass takes the snapshot while stopped as well
    // (fillPublishedState), so this adds no acquisition of the lock.
    f.value(pipeline_.running());
    for (const std::string& k : feedingKeysScratch_) { f.text(k); }
    // --- API 0.3's, and the 0.2 lists step 1 left out ----------------------
    // The store.
    f.value(catalog_.size());
    for (const cascade::core::PluginCatalogEntry& e : catalog_) {
        f.text(e.id);
        f.text(e.version);
        f.value(e.legalNotice.size());
    }
    f.value(pluginBlocked_.size());
    for (const cascade::core::BlockedPlugin& b : pluginBlocked_) {
        f.text(b.installed.file);
        // API 0.3 8.9, fact.blocked.<file>.id and .message: the catalogue id, and the sentence as stored (which
        // the block recomposes in the language in force). A rescan changes both and is hashed itself; they are
        // hashed as well so a row that kept its file name cannot keep a stale id or sentence.
        f.text(b.installed.id);
        f.text(b.message);
    }
    f.value(catalogPending_);
    f.value(installPending_);
    // API 0.3 8.9, the store as its window draws it. EVERYTHING A CATALOGUE ENTRY CARRIES is one counter
    // (catalogSeq_, bumped wherever catalog_ is written; hashing the texts of two dozen entries of up to 3 KB
    // each, every pass, would cost more than the rest of this function), the index the store reads (a path or a
    // URL: nothing else says it changed), and the transfer: the busy NAME is hashed and not only the pending flag,
    // because an ADD ALL run goes from one download to the next inside ONE pump (pollPluginAsync clears
    // installPending_ and pumpAddAll sets it again before the pass publishes), so the flag is the same at both
    // publishes and only the name and the run's position tell them apart.
    f.value(catalogSeq_);
    f.text(pluginCatalogueUrl_);
    f.text(installBusyName_);
    // THE STORE'S OWN (stub campaign B3, engine_store.cpp): the kept copy and how fresh it is, the sites and what each said, the pictures (one
    // counter: the map is walked by nobody here), the clean-up's state and its old copies, whose transfer a card is busy with and whose outcome
    // the page shows. In place, from members: nothing allocates.
    f.value(catalogueFromCache_);
    f.value(catalogueRefreshFailed_);
    f.value(catalogueReadTime_);
    for (const std::string& url : pluginSites_) { f.text(url); }
    for (const SiteStatus& st : siteStatus_) {
        f.text(st.url);
        f.value(st.state);
        f.value(st.listed);
        f.value(st.hiddenSameId);
        f.value(st.hiddenOffHost);
        f.text(st.error);
    }
    f.text(siteError_);
    f.text(regionalState_);
    f.value(regionalAdded_);
    f.value(regionalDropped_);
    f.value(picturesSeq_);
    f.value(screenshotPending_);
    f.value(cleanupPending_);
    for (const cascade::core::SupersededPlugin& o : pluginOldCopies_) {
        f.text(o.file);
        f.text(o.keptFile);
        f.text(o.version);
        f.text(o.keptVersion);
    }
    f.text(pluginCleanupReport_);
    f.text(installBusyId_);
    f.text(installResultId_);
    f.value(addAllRun_.active);
    f.value(addAllRun_.next);
    f.value(addAllRun_.total);
    f.text(addAllRun_.currentName);
    f.value(addAllFailed_);  // the level of the addAllSummary line
    // The transmitter's address and whether it is open.
    f.text(transmitArgs_);
    f.text(sourceKind_);
    f.text(deviceArgs_);
    f.value(transmitter_.haveSink());
    // Plugin marks, the band plan, the presets people saved.
    f.value(pluginMarkersSeq_);
    f.text(bandPlanSelection_);
    f.text(bandPlan_.name());
    f.value(bandPlan_.entries().size());
    f.value(bandPlanChoices_.size());
    for (const cascade::core::UserPreset& u : userPresets_) {
        f.text(u.plugin);
        f.text(u.label);
        f.value(u.frequencyHz);
        f.value(u.demodMode);
        f.value(u.bandwidthHz);
    }
    // The radio's own switches, its bias tee, its converter.
    f.value(deviceRfNotchPresent_);
    f.value(deviceRfNotch_);
    f.value(deviceDabNotchPresent_);
    f.value(deviceDabNotch_);
    f.value(deviceHdrPresent_);
    f.value(deviceHdr_);
    f.value(deviceAdcSwitchesPresent_);
    f.value(deviceDither_);
    f.value(deviceRandomiser_);
    f.value(biasTeePanel_.present);
    f.value(biasTeePanel_.shown);
    for (const auto& c : converters_) {
        f.text(c.first);
        f.value(c.second.mode);
        f.value(c.second.loHz);
        f.value(c.second.inverted);
    }
    // --- the radio panels' facts (API 0.3 8.3, window port group 4) ----------------
    // The bias tee key: a radio that dies changes nothing else (its driver
    // refuses from then on), so the question the key asks is hashed itself; the
    // memory's size decides mayRemember (a full memory keeps no "on"); the test
    // stand-in and the name the dialog shows.
    f.value(biasTeeReachable());
    f.value(biasTeePanel_.remembered.size());
    f.value(biasStandIn_);
    if (device_ != nullptr) { f.cstr(device_->name()); }
    // The converter IN FORCE (what the pipeline applies, which a sound card's
    // format makes differ from the stored setting), what the alias note says,
    // and the sound card the converter key is made from: two cards leave
    // deviceArgs_ empty alike, and the second must still rebuild the lists.
    {
        const cascade::core::ConverterSetting live = pipeline_.converter();
        f.value(live.mode);
        f.value(live.loHz);
        f.value(live.inverted);
    }
    for (const auto& a : converterKeyAlias_) {
        f.text(a.first);
        f.text(a.second);
    }
    for (const std::string& k : converterNotCarried_) { f.text(k); }
    f.text(soundCardLive_.device);
    f.text(soundCardLive_.hostApi);
    f.value(soundCardLive_.format);
    // The converter's name is a translated sentence, built with the block: a
    // language switch has to rebuild it. (The radio's own centre, which moves
    // with every retune, is NOT here: it is read live.)
    f.text(cascade::i18n::current());
    // The serial ports the last scan found.
    f.value(serialPortsScans_);
    // THE AIRBAND MONITOR AND THE SCANNER'S LIST MODE (stub campaign B6): whether it listens or waits, the preset, the block on the air, the
    // squelch and the hold, what it last said, and the list mode's switch. In place, from members: nothing allocates. (The channels' own
    // figures are AIRBAND_CHANNELS, a block of its own; the ticks are BOOKMARKS, hashed with the list's version above.)
    f.value(airbandListening_);
    f.value(airbandStartPending_);
    f.text(airbandGroup_);
    f.text(airbandNote_);
    f.value(airbandSquelchDb_);
    f.value(airbandHoldS_);
    f.value(airbandBlock_);
    f.value(airbandBlocks_.size());
    f.value(airbandChans_.size());
    f.value(scanTicked_);
    // The status lines.
    for (const std::string* t : {&sourceError_, &gpsRefusal_, &catalogError_, &bandPlanError_, &tuneMismatchNote_,
                                 &transmitError_, &soundCardMissing_, &sdrPlayApiDetail_, &sdrPlayAdvice_,
                                 &recordNotice_, &recordError_, &presetNote_, &pluginEnforceError_,
                                 &restoreKeepLabel_, &audioHealthNote_,  // (audioHealth: API 0.3 8.4)
                                 &bookmarkImportNote_, &bookmarkError_,  // (the Bookmarks section's: 8.6)
                                 &installReport_, &installError_,  // (the plugin install/remove outcome: 8.8)
                                 &catalogStatus_, &addAllSummary_}) {  // (the store window's two more: 8.9)
        f.text(*t);
    }
    // The patch page: its graph, what the plan said, its radios.
    f.value(graphHash(patchGraph_));
    f.value(patchGraphEpoch_);
    f.value(patchGraphVersion_);  // (API 0.3 8.12: whatever moved the graph moved the version, the hash apart)
    f.value(patchRunning_);
    for (const auto& pr : patchPlan_.problems) {
        f.value(pr.node);
        f.value(pr.problem);
    }
    // The plan the page draws (API 0.3 8.12: PATCH_PLAN and fact.patch.runnable): every figure a channel or a
    // decoder carries moves the block.
    f.value(patchPlan_.runnable);
    for (const auto& ch : patchPlan_.channels) {
        f.value(ch.node);
        f.value(ch.offsetHz);
        f.value(ch.decimation);
        f.value(ch.outRateHz);
        f.value(ch.radio);
    }
    for (const auto& d : patchPlan_.decoders) {
        f.value(d.node);
        f.value(d.plugin);
        f.value(d.source);
        f.value(d.inRateHz);
        f.value(d.rateHz);
        f.value(d.centreHz);
        f.value(d.radio);
    }
    for (cascade::core::patch::NodeId n : patchRefused_) { f.value(n); }
    // The parts bin: the content of each entry, not their number (a module replaced by another keeps the count).
    f.value(patchCatalogue_.size());
    for (const auto& d : patchCatalogue_) {
        f.text(d.key);
        f.text(d.name);
        f.value(d.feed);
        f.value(d.requiredRateHz);
        f.value(d.image);
        f.value(d.tracks);
    }
    for (const auto& r : patchRadios_) {
        f.value(r.first);
        f.value(r.second->rateHz());
        f.value(r.second->centreHz());  // (API 0.3 8.12: PATCH_RADIOS value[1])
        f.value(r.second->fault().empty());
    }
    for (const auto& e : patchRadioError_) {
        f.value(e.first);
        f.text(e.second);
    }
    // The I/Q recordings the Radio's device list offers (API 0.3 8.12: PATCH_DEVICES, with each one's rate): by what each
    // names, not their number - a relisting that finds other files must rebuild the list.
    f.value(patchRecordings_.size());
    for (const auto& r : patchRecordings_) {
        f.text(r.key);
        f.text(r.fileName);
        f.value(r.rateHz);
    }
    // The receiver's radio as the patch names it (fact.patch.receiverRadio): the lent one - and the open one, which
    // sourceKind_, deviceArgs_ and device_ above already hash - and the labels the lent radio is offered by. Whether
    // the radio has ever been tuned (fact.patch.receiverCentre exists) is hashed, its VALUE is not: a retune would
    // rebuild the block, as the comment on the converter's centre says.
    f.value(patchMainKeep_.valid);
    f.text(patchMainKeep_.kind);
    f.text(patchMainKeep_.args);
    f.text(patchMainKeep_.label);
    f.value(carriedAirCentre().has_value());
    // The recorders: whether each is taking, and to which file (the bytes
    // are read live, never fingerprinted).
    f.value(iqRecorder_.recording());
    f.value(audioRecorder_.recording());
    f.value(iqRecordStartS_);
    f.value(audioRecordStartS_);
    f.text(recordDir_);
    // The usage switch, the install id it minted, and the session counters (API 0.3 8.13: fact.telemetry.* and fact.diag.*).
    f.value(telemetryEnabled_);
    f.text(telemetryInstallId_);
    f.value(telemetryLaunches_);
    f.value(telemetryCrashes_);
    // The servers (API 0.3 8.16): the host's one version moves with the configuration, a listener's report and the password's hash
    // starting and finishing - and with nothing a connection does (the counters are read live).
    f.value(servers_.version());
    // An Airspy's own choices.
    if (airspy != nullptr) {
        f.value(airspy->gainMode());
        f.value(airspy->decimation());
        f.value(airspy->hardwareSampleRateHz());
    }
    // --- API 0.3 8.10, the Source section and the sound card as their pages draw them ----------------------------
    // Every input of the facts, lists and flags below that the rows above do not already hash. In place: nothing here
    // allocates (the sound card list and the unbound dongles are small, and are hashed by what they carry).
    //  - the combo's own row, and what the section says about the radio being kept / being opened
    f.value(sourceSel_);
    f.text(restoreKeep_.kind);
    f.value(deviceOpenPending_);
    f.text(deviceBusyLabel_);
    f.text(deviceModel_);
    //  - the scan's state, and the inputs of the gate (soapyScanGated) and of the plan behind scanSkip that nothing
    //    above hashes: the process-wide count of open SoapySDR radios (an abandoned one keeps the scan gated for the
    //    rest of the session with no radio installed) and the patch radios still opening. The gate's ANSWER is not
    //    hashed: it is a function of these and of members hashed above.
    f.value(soapyScanPending_);
    f.value(soapyScanned_);
    f.value(soapyScanPartial_);
    f.value(cascade::source::SoapySource::openDeviceCount());
    for (const auto& p : patchRadioPending_) { f.value(p.first); }
    f.value(lookForNetworkUsrps_);
    for (const std::string& d : soapyAbsentDrivers_) { f.text(d); }
    //  - what the last scan said about UHD's firmware images (0.99.90 merge: fact.source.uhdNoImages / fact.source.usrpOnUsb)
    f.value(soapyUhdNoImages_);
    f.value(soapyUsrpOnUsb_);
    f.value(sdrPlayRowsFound_);
    //  - the Pluto's address, the dongles that cannot be opened, what the last scans said about drivers and the
    //    runtime (the evidence as one counter: its rows are paths)
    f.cstr(plutoUri_);
    f.cstr(rtlTcpAddr_);
    //  - the audio buffer setting and what automatic last did (0.99.73 / 0.99.83: fact.audio.*)
    f.value(audioBufferMs_);
    f.value(audioLeadNowMs());
    //  - the crystal correction's facts (0.99.56: fact.ppm.*), hashed from the members they are built from: this runs on
    //    every pass and allocates nothing (test_engine_publish_alloc), so the fact strings are not built here.
    f.value(ppmCorrectionOn_);
    f.value(ppmRadioApplied_);
    f.text(ppmError_);
    f.text(sourceKind_);
    f.text(deviceArgs_);
    f.text(deviceModel_);
    for (const auto& kv : ppmValues_) {
        f.text(kv.first);
        f.value(kv.second);
    }
    //  - what automatic last did to the audio buffer (the parts of fact.audio.leadNote)
    f.value(static_cast<int>(audioLeadNote_));
    f.value(audioLeadNoteMs_);
    f.value(audioLeadNoteCount_);
    for (const cascade::usb::UsbDeviceInfo& u : nativeUnbound_) {
        f.value(u.vid);
        f.value(u.pid);
        f.text(u.description);
    }
    for (const std::string& d : faultedDrivers_) { f.text(d); }
    f.value(scanEvidenceSeq_);
    //  - the radio's fault sentence WHOLE (STATUS_LINES deviceFault) and the rates' captions
    f.text(apiFaultText_);
    if (haveDevice) {
        for (const std::string& l : deviceRateLabels_) { f.text(l); }
    }
    //  - the sound card: where the listing stands, the form, and the inputs it listed
    f.value(soundCardListed_);
    f.value(soundCardScanPending_);
    f.value(soundCardOpenPending_);
    f.text(soundCard_.device);
    f.text(soundCard_.hostApi);
    f.value(soundCard_.format);
    f.value(soundCard_.channel);
    f.value(soundCard_.swapIq);
    f.value(soundCard_.iqCentreHz);
    f.value(soundCard_.cardRateHz);
    f.value(soundCard_.pickedFromList);
    for (const cascade::source::SoundCardDevice& d : soundCardDevices_) {
        f.text(d.name);
        f.text(d.hostApi);
        f.value(d.defaultRateHz);
        for (const cascade::source::SoundCardRate& r : d.rates) {
            f.value(r.hz);
            f.value(r.exclusive);
        }
    }
    return f.h;
}

std::shared_ptr<const ApiListBlock> Engine::buildApiLists(std::uint64_t fingerprint) {
    auto b = std::make_shared<ApiListBlock>();
    b->fingerprint = fingerprint;
    b->mode = cascade::gui::abiDemodForModeIndex(modeIndex_);
    b->bandwidthHz = vfoBandwidthHz_;
    // The bookmarks: shared with the previous block unless they changed.
    if (apiLists_ != nullptr && apiListsBookmarkVersion_ == freqMgr_.version() && apiLists_->bookmarks) {
        b->bookmarks = apiLists_->bookmarks;
    } else {
        b->bookmarks = std::make_shared<const std::vector<cascade::core::Bookmark>>(freqMgr_.list());
        apiListsBookmarkVersion_ = freqMgr_.version();
    }
    // The Source list, in the desktop combo's order: the generator, the
    // native rows, the SoapySDR rows (the ids SELECT_SOURCE takes -
    // Engine::selectSourceById matches them against these same lists).
    // API 0.3 8.10: the combo's two fixed rows are entries too (an entry's index is its row in the combo), and every id
    // is the one spelling sourceIdForRow gives the combo.
    b->devices.push_back({"Signal generator", sourceIdForRow(0), sourceKind_ == "siggen"});
    b->devices.push_back({"IQ file", sourceIdForRow(1), sourceKind_ == "file"});
    b->devices.push_back({"Sound card", sourceIdForRow(kSoundCardRow), sourceKind_ == "soundcard"});
    for (std::size_t i = 0; i < nativeDevices_.size(); ++i) {
        const auto& d = nativeDevices_[i];
        b->devices.push_back({d.label, sourceIdForRow(kNativeRowBase + static_cast<int>(i)),
                              sourceKind_ == d.driver && deviceArgs_ == d.args});
    }
    for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
        const auto& d = soapyDevices_[i];
        b->devices.push_back({d.label, sourceIdForRow(soapyRowBase() + static_cast<int>(i)),
                              sourceKind_ == "soapy" && deviceArgs_ == d.args});
    }
    if (device_ != nullptr) {
        b->rates = deviceRatesHz_;
        for (std::size_t i = 0; i < deviceRatesHz_.size(); ++i) {
            b->rateLabels.push_back(i < deviceRateLabels_.size() ? deviceRateLabels_[i] : cascade::gui::rateLabel(deviceRatesHz_[i]));
        }
        b->rateHz = pipeline_.activeSource().sampleRateHz();
        const cascade::source::AirspySource* const airspy = asAirspyDevice();
        for (std::size_t i = 0; i < deviceGainRanges_.size(); ++i) {
            const auto& g = deviceGainRanges_[i];
            b->gains.push_back({g.name, g.minDb, g.maxDb, g.stepDb,
                                i < deviceGainsDb_.size() ? static_cast<double>(deviceGainsDb_[i]) : 0.0,
                                cascade::gui::gainUnitWire(g.unit), gainDrivenByAgc(airspy, g.name)});
        }
        b->antennas = deviceAntennas_;
        b->antenna = deviceAntenna_;
    }
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        const auto& a = devices_[static_cast<std::size_t>(i)];
        b->audioDevices.push_back({a.name, a.index, i == deviceIndex_});
    }
    // API 0.3 8.8: who has asked to change a setting (one vector for the whole block, asked once).
    const std::vector<std::string> settingsAskers = pluginApi_->settingsRequesters();
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        ApiListBlock::Plugin w;
        w.name = p.name;
        const std::size_t slash = p.path.find_last_of("/\\");
        w.fileName = slash == std::string::npos ? p.path : p.path.substr(slash + 1);
        w.key = cascade::core::pluginKey(p);
        w.loaded = p.loaded;
        w.decoder = p.decoder != nullptr || p.iqDecoder != nullptr || p.imageDecoder != nullptr;
        w.running = w.decoder && p.loaded && !pluginIsStopped(w.key);
        w.status = p.loaded ? std::string() : p.error;
        // API 0.3 8.7.
        w.capabilities = p.capabilities;
        w.stopped = p.loaded && pluginIsStopped(w.key);
        // PLUGINS value[3] (stub campaign B2): bits 0-1 the run state (0 auto, 1 always, 2 stopped), bit 2 (4) asleep - AUTO and nothing
        // is using it, so it has no instance. A refused file has neither.
        if (p.loaded) {
            w.runWord = static_cast<std::uint32_t>(pluginRunMode(w.key)) |
                        ((pluginIsIdle(w.key)) ? 4u : 0u);
        }
        w.tuneAllowed = listHolds(pluginTuneAllowed_, w.key);
        w.settingsAllowed = listHolds(pluginSettingsAllowed_, w.key);
        w.mutes = w.decoder && pluginMutesFor(p, w.key);
        // API 0.3 8.8. The size is a stat, taken here because the block is rebuilt when a rescan (or anything
        // else in the fingerprint) moved and never per pass; 0 is "not measured". MATCHED is the runner's own
        // isFeeding answer for every kind of plugin, taken from the snapshot the pass already holds (the
        // receiver running or not).
        {
            std::error_code ec;
            const std::uintmax_t bytes = std::filesystem::file_size(std::filesystem::path(p.path), ec);
            if (!ec) { w.sizeBytes = static_cast<std::uint64_t>(bytes); }
        }
        if (const auto n = pluginNotices_.find(w.key); n != pluginNotices_.end()) { w.noticeLevel = n->second.level; }
        w.matched = !w.key.empty() &&
                    std::find(feedingKeysScratch_.begin(), feedingKeysScratch_.end(), w.key) != feedingKeysScratch_.end();
        w.askedSettings = !w.key.empty() && std::find(settingsAskers.begin(), settingsAskers.end(), w.key) != settingsAskers.end();
        w.digestMismatch = !cascade::core::PluginRepo::changedSinceInstallNote(pluginInventory_.plugins, w.key).empty();
        b->plugins.push_back(std::move(w));
    }
    buildApiLists03(*b);
    return b;
}

// API 0.3's collections, and the 0.2 lists step 1 left out
// (docs/api-0.3-additions.md). The pumping thread, only when the fingerprint
// moved.
void Engine::buildApiLists03(ApiListBlock& b) {
    auto fact = [&b](const std::string& key, std::string value, bool localOnly = false) {
        b.facts[key] = ApiListBlock::Fact{std::move(value), localOnly};
    };
    // The runner's rows, copied once for the block: DECODERS' idle word and the fitted modules' idle sentence.
    const std::vector<cascade::core::DecoderStatus> status = pluginRunner_.status();
    // DECODERS: every decoder plugin, running or not, and why not.
    {
        for (const ApiListBlock::Plugin& p : b.plugins) {
            if (!p.decoder) { continue; }
            ApiListBlock::Decoder d;
            d.name = p.name;
            d.key = p.key;
            d.running = p.running;
            d.mutes = p.mutes;
            d.fed = pipeline_.running() && std::find(feedingKeysScratch_.begin(), feedingKeysScratch_.end(),
                                                     p.key) != feedingKeysScratch_.end();
            for (const cascade::core::DecoderStatus& st : status) {
                if (st.key == p.key && st.reason != cascade::core::DecoderIdleReason::Running) {
                    d.status = st.detail.empty() ? idleWord(st.reason) : st.detail;
                }
            }
            if (!p.loaded) { d.status = p.status; }
            b.decoders.push_back(std::move(d));
        }
    }
    // THE FITTED MODULES (API 0.3 8.8): the plugins folder, and for every record the host holds the whole texts a
    // list item would cut (the file's path, the descriptor's strings, the refusal, the runner's idle sentence,
    // the module's newest notice, the integrity note) - each present for every record, "" when there is none, so
    // "known and empty" is told from NOT_FOUND - and the command keys of the loaded ones. The facts are keyed by
    // the module file name (PLUGINS' detail): the table's reader takes the leaf from the right.
    fact("plugin.dir", pluginDir_, true);
    {
        const auto& records = pluginHost_.plugins();
        for (std::size_t i = 0; i < b.plugins.size() && i < records.size(); ++i) {
            const ApiListBlock::Plugin& w = b.plugins[i];
            if (w.key.empty()) { continue; }
            const cascade::core::LoadedPlugin& p = records[i];
            const std::string k = "plugin." + w.key + ".";
            fact(k + "path", p.path, true);
            fact(k + "version", p.version);
            fact(k + "author", p.author);
            fact(k + "licence", p.licence);
            fact(k + "error", p.error);
            // The runner's own sentence for the FIRST row of this module that is not Running, quoted whole.
            std::string idle;
            for (const cascade::core::DecoderStatus& st : status) {
                if (st.key == w.key && st.reason != cascade::core::DecoderIdleReason::Running) {
                    idle = st.detail;
                    break;
                }
            }
            fact(k + "idle", std::move(idle));
            const auto notice = pluginNotices_.find(w.key);
            fact(k + "notice", notice != pluginNotices_.end() ? notice->second.text : std::string());
            fact(k + "integrity",
                 cascade::core::PluginRepo::changedSinceInstallNote(pluginInventory_.plugins, w.key));
            if (w.loaded) {
                for (const cascade::core::HostCommand& hc : pluginApi_->commands(w.key)) {
                    b.pluginCommands.push_back({hc.id, hc.label, w.key});
                }
            }
        }
    }
    // THE STORE: the catalogue, what is refused, what would update - and the
    // long texts an entry carries, as facts.
    // The update plans, once for the whole block (planUpdates walks the catalogue against the install records):
    // STORE_UPDATES, and each entry's updateReason below.
    const std::vector<cascade::core::PluginUpdate> updates = plannedPluginUpdates();
    for (const cascade::core::PluginCatalogEntry& e : catalog_) {
        const cascade::core::PluginPlatform* const here = e.thisPlatform();
        // BY CATALOGUE KEY (0.99.74): the id for an official plugin - every name and key that existed before sites is unchanged - and
        // "<id>@<site label>" for a plugin from a site the user added.
        const std::string key = cascade::core::PluginRepo::catalogueKey(e.source, e.id);
        ApiListBlock::StoreEntry se;
        se.id = key;
        se.name = e.name;
        se.installed = catalogEntryInstalled(e);
        se.compatible = e.compatible;
        // API 0.3 8.9: the entry's ABI, the size of the build for this machine (0: not stated or no build) and
        // whether there is one.
        se.abiVersion = e.abiVersion;
        se.sizeBytes = here != nullptr ? here->sizeBytes : 0u;
        se.buildHere = here != nullptr;
        b.store.push_back(std::move(se));
        const std::string k = "store." + key + ".";
        fact(k + "legal", e.legalNotice);
        fact(k + "summary", e.summary);
        fact(k + "description", e.description);
        fact(k + "homepage", e.homepage);
        fact(k + "version", e.version);
        // API 0.3 8.9: the rest of what the store's plate letters, every key for every entry ("" when there is
        // none). The builds are joined as the window letters them, "windows/x64, linux/x64"; the file is the name
        // this machine's build installs under, which joins the entry to PLUGINS' detail; the reason is the update
        // plan's own sentence, in English.
        fact(k + "author", e.author);
        fact(k + "licence", e.licence);
        std::string platforms;
        for (const cascade::core::PluginPlatform& pf : e.platforms) {
            if (!platforms.empty()) { platforms += ", "; }
            platforms += pf.os + "/" + pf.arch;
        }
        fact(k + "platforms", std::move(platforms));
        fact(k + "floor", e.minSupportedVersion);
        fact(k + "file", catalogueFileName(e));
        std::string reason;
        for (const cascade::core::PluginUpdate& u : updates) {
            if (cascade::core::PluginRepo::catalogueKey(u.source, u.id) == key) {
                reason = u.reason;
                break;
            }
        }
        fact(k + "updateReason", std::move(reason));
    }
    // The index the store reads: a URL, or a path on this machine (the header of the store window says a local
    // index.json is how it is exercised offline), so LOCAL. No id has a leaf-less key, so it cannot collide.
    fact("store.url", pluginCatalogueUrl_, true);
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        const cascade::core::PluginCatalogEntry& ce = catalog_[static_cast<std::size_t>(i)];
        fact("store." + cascade::core::PluginRepo::catalogueKey(ce.source, ce.id) + ".blocked", pluginInstallBlockedReason(i, true));
    }
    for (const cascade::core::BlockedPlugin& x : pluginBlocked_) {
        b.storeBlocked.emplace_back(x.installed.file, x.message);
        // API 0.3 8.9: what STORE_BLOCKED's detail cannot say. The id the Update plan is matched on, and the
        // sentence WHOLE and composed NOW, in the language in force: x.message was composed when the plugins were
        // loaded, before any language was chosen, and detail cuts it at 159 bytes. The fingerprint hashes the
        // language, so a switch rebuilds the block and with it this sentence (as fact.converter.name).
        const std::string kb = "blocked." + x.installed.file + ".";
        fact(kb + "id", x.installed.id);
        fact(kb + "message", cascade::core::PluginRepo::pluginBlockMessage(x.installed, x.policy));
    }
    for (const cascade::core::PluginUpdate& u : updates) {
        b.storeUpdates.emplace_back(cascade::core::PluginRepo::catalogueKey(u.source, u.id), u.fromVersion + " -> " + u.toVersion);
    }
    // The kept copy, the sites, the pictures, the old copies and each file's origin (engine_store.cpp).
    buildApiListsStore(b);
    // The Airband monitor's facts and the scanner's list mode (engine_airband.cpp).
    buildApiListsAirband(b);
    // API 0.3 8.9, STORE_ACTIVITY: the transfer in flight and the ADD ALL run. What is read live (the progress)
    // is not here: the block keeps the repo.
    {
        ApiListBlock::StoreActivity& a = b.storeActivity;
        a.repo = &pluginRepo_;
        if (catalogPending_ || installPending_) {
            a.transfer = true;
            a.transferKind = installPending_ ? 2 : 1;
            if (installPending_) { a.transferName = installBusyName_; }
        }
        if (addAllRun_.active) {
            a.addAll = true;
            a.addAllTotal = addAllRun_.total;
            // THE MODULE IN FLIGHT, 1-based. `next` is the index of the NEXT id to start and pumpAddAll has already
            // stepped it past the one it just started, so while module k downloads next == k: that is the position.
            // (The window used to letter min(next + 1, total), which reads k + 1: the first module as "2 of 23",
            // and the last two both as "23 of 23". Before the first starts, next is 0 and the name is empty: 1.)
            a.addAllPosition = std::clamp<std::uint64_t>(static_cast<std::uint64_t>(addAllRun_.next), 1u, addAllRun_.total);
            a.addAllName = addAllRun_.currentName;
        }
    }
    // THE TRANSMITTER's one "device": the address TX_OPEN is given.
    // Until the page has seeded (or somebody typed) one, the seed it would use.
    b.txArgs = transmitArgs_.empty() ? transmitArgsSeed() : transmitArgs_;
    b.txOpen = transmitter_.haveSink();
    fact("transmit.args", b.txArgs, true);
    // PLUGIN MARKS on the spectrum.
    for (const cascade::core::HostMarker& m : pluginMarkers_) {
        b.markers.push_back({m.m.id, m.m.label, m.name, m.m.freqHz, m.m.widthHz, m.m.colourRgba,
                             m.m.kind == CASCADE_MARKER_SPAN, (m.m.flags & CASCADE_MARKER_FLAG_DASHED) != 0u,
                             (m.m.flags & CASCADE_MARKER_FLAG_SPECTRUM_ONLY) != 0u});
    }
    // THE PLUGIN-MUTE CLUSTER (API 0.3 8.7): who is muting the receiver's audio (the names and, beside them,
    // the module files the Stop key acts on), whether the mute is held after a tune-away, and the sentence
    // that names them - whole, where the state's audioMutedBy is cut.
    for (std::size_t i = 0; i < mutedBy_.size(); ++i) {
        b.muteCauses.push_back({mutedBy_[i], i < mutedByKeys_.size() ? mutedByKeys_[i] : std::string(), muteKeptRunning_});
    }
    fact("mute.subject", muteSubjectText());
    fact("mute.keptRunning", muteKeptRunning_ ? "1" : "0");
    // THE TUNE GRANTS (API 0.3 8.7): the last request to tune that was refused (the module file name, as the
    // grants are keyed), and the grants no LOADED module holds - removed, renamed aside, or refused - each
    // flagged when the host holds a record of that file that did not load.
    fact("plugin.lastDenied", lastDeniedNow());
    for (const std::string& g : pluginTuneAllowed_) {
        bool loadedHere = false;
        bool present = false;
        for (const ApiListBlock::Plugin& p : b.plugins) {
            if (p.key != g) { continue; }
            present = true;
            loadedHere = loadedHere || p.loaded;
        }
        if (!loadedHere) { b.staleGrants.push_back({g, present}); }
    }
    // THE BAND PLAN in force, and the plans there are.
    for (const cascade::core::BandEntry& e : bandPlan_.entries()) {
        b.bandPlan.push_back({e.name, e.service, e.startHz, e.endHz, e.colorRgba});
    }
    for (const cascade::core::PlanInfo& pl : bandPlanChoices_) {
        b.bandPlans.push_back({pl.id, pl.name, pl.id == bandPlanSelection_});
    }
    // The name of the plan LOADED, as the Display section captions it (8.4): the
    // chain "ITU Region 1 + United Kingdom", which no single BAND_PLANS entry names.
    fact("bandPlan.name", bandPlan_.name());
    // PRESETS: a plugin's own (asked of its code only when the plugins
    // changed - kept from the last block otherwise), and the ones saved here.
    if (apiLists_ != nullptr && apiLists_->presets && apiLists_->presetsFingerprint == apiPluginPathsHash_) {
        b.presets = apiLists_->presets;
    } else {
        auto v = std::make_shared<std::vector<ApiListBlock::Preset>>();
        for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
            if (!p.loaded) { continue; }
            for (const cascade::gui::IndexedPreset& ip : validatedPresets(p)) {
                v->push_back({ip.preset.label, p.name, ip.preset.frequencyHz, ip.preset.bandwidthHz,
                              ip.preset.demodMode, ip.index, (ip.preset.flags & CASCADE_PRESET_DEVICE_CENTRE) != 0u});
            }
        }
        b.presets = std::move(v);
    }
    b.presetsFingerprint = apiPluginPathsHash_;
    for (std::size_t i = 0; i < userPresets_.size(); ++i) {
        const cascade::core::UserPreset& u = userPresets_[i];
        b.userPresets.push_back({u.label, u.plugin, u.frequencyHz, u.bandwidthHz, u.demodMode,
                                 static_cast<std::uint32_t>(i)});
    }
    // THE RADIO'S OWN SWITCHES (SET_DEVICE_OPTION's names), where present.
    if (device_ != nullptr) {
        if (deviceRfNotchPresent_) { b.deviceOptions.emplace_back("rf_notch", deviceRfNotch_); }
        if (deviceDabNotchPresent_) { b.deviceOptions.emplace_back("dab_notch", deviceDabNotch_); }
        if (deviceHdrPresent_) { b.deviceOptions.emplace_back("hdr", deviceHdr_); }
        if (deviceAdcSwitchesPresent_) {
            b.deviceOptions.emplace_back("dither", deviceDither_);
            b.deviceOptions.emplace_back("randomiser", deviceRandomiser_);
        }
    }
    b.serialPorts = serialPorts_;
    buildApiListsSource(b);
    // THE STATUS LINES, every one (the list shows the ones with text).
    {
        struct Line {
            const char* key;
            const std::string* text;
            int level;
        };
        const Line lines[] = {
            {"sourceError", &sourceError_, 3},        {"gpsRefusal", &gpsRefusal_, 3},
            {"catalogError", &catalogError_, 3},      {"bandPlanError", &bandPlanError_, 3},
            {"tuneMismatchNote", &tuneMismatchNote_, 2}, {"transmitError", &transmitError_, 3},
            {"soundCardMissing", &soundCardMissing_, 2}, {"sdrPlayApiDetail", &sdrPlayApiDetail_, 2},
            {"sdrPlayAdvice", &sdrPlayAdvice_, 2},    {"recordNotice", &recordNotice_, 1},
            {"recordError", &recordError_, 3},        {"presetNote", &presetNote_, 1},
            {"pluginEnforceError", &pluginEnforceError_, 3}, {"restoreKeepLabel", &restoreKeepLabel_, 1},
            // 8.4: the audio watchdog's note under the Sinks section's device combo, a warning.
            {"audioHealth", &audioHealthNote_, 2},
            // 8.6: the Bookmarks section's note (what an import, an export or a group removal did)
            // and its red line (the bookmark file could not be read or saved).
            {"bookmarkNote", &bookmarkImportNote_, 1},
            {"bookmarkError", &bookmarkError_, 3},
            // 8.8: what the last plugin install or removal did, and why the last one failed (the store window
            // and the fitted-modules window show the same pair).
            {"installReport", &installReport_, 1},
            {"installError", &installError_, 3},
            // 8.9: the catalogue status line ("N plugins in the catalogue", set by a fetch that landed, cleared when
            // one starts) and what the last ADD ALL run did (red when it had failures).
            {"catalogStatus", &catalogStatus_, 1},
            {"addAllSummary", &addAllSummary_, addAllFailed_ ? 3 : 1},
            // 8.10: the radio's fault sentence WHOLE (FoxReceiverState::faultMessage cuts it at 127 bytes, and the
            // zombie-source sentence is 134): the pipeline's, or the test stand-in's.
            {"deviceFault", &apiFaultText_, 3},
        };
        for (const Line& l : lines) {
            b.statusLines.push_back({l.key, *l.text, l.level});
            fact(std::string("status.") + l.key, *l.text);
        }
    }
    // CONVERTERS: every radio's remembered setting; ACTIVE is the open one's.
    {
        const std::string now = converterRadioKeyNow();
        for (const auto& c : converters_) {
            const bool active = c.first == now;
            b.converters.push_back({c.first, static_cast<int>(c.second.mode), c.second.loHz, c.second.inverted,
                                    active, active ? &converterRadioCentre_ : nullptr});
        }
        fact("converter.radioKey", now, true);
        const cascade::core::ConverterSetting live = pipeline_.converter();
        fact("converter.name", converterName(live));
        // API 0.3 8.3: the converter IN FORCE ("<mode> <LO Hz> <inverted>", the
        // LO to the digit it round-trips by), the note the Source section may
        // add, the modes it offers, and whether a stored Up goes unused.
        char liveText[96];
        std::snprintf(liveText, sizeof(liveText), "%d %.17g %d", static_cast<int>(live.mode), live.loHz,
                      live.inverted ? 1 : 0);
        fact("converter.live", liveText);
        fact("converter.alias", converterAliasState());
        fact("converter.modes", converterModesText());
        fact("converter.upIgnored", converterUpIgnored() ? "1" : "0");
    }
    // THE BIAS TEE's key: which radio it would be remembered for. With the test
    // stand-in active (a bounded run's FOXSDR_FORCE_BIAS_KEY, or the test hook)
    // the key is drawn over it, so it is reachable: the window knows nothing of it.
    fact("biasTee.reachable", biasTeeReachable() || biasStandInActive() ? "1" : "0");
    fact("biasTee.mayRemember", biasKeyMayRememberNow() ? "1" : "0");
    fact("biasTee.radioKey", biasKeyRadioNow(), true);
    fact("biasTee.radioName", biasKeyRadioName());
    // THE TUNING SURFACE (API 0.3 8.5). The bookmark store's own version counter -
    // the one the fingerprint hashes, bumped by every add, edit, star and
    // removal - so an interface copies its marker rows when the STORE changed,
    // not whenever listSeq moves (a decoder printing a line moves it too). And
    // the DSP chain's input rate, to the digit it round-trips by: what the
    // spectrum's axis, the VFO's reach and the click-to-tune are measured
    // against; the source's own rate (sampleRateHz) differs from it after a
    // change of rate the chain refused.
    fact("bookmarks.version", std::to_string(freqMgr_.version()));
    {
        char rateText[40];
        std::snprintf(rateText, sizeof(rateText), "%.17g", pipeline_.inputRateHz());
        fact("input.rateHz", rateText);
    }
    // AN AIRSPY's gain modes and decimation factors.
    if (cascade::source::AirspySource* a = asAirspyDevice()) {
        const int mode = static_cast<int>(a->gainMode());
        const char* const names[] = {"Linearity", "Sensitivity", "Free"};
        for (int i = 0; i < 3; ++i) { b.gainModes.push_back({names[i], static_cast<double>(i), 0.0, i == mode}); }
        for (unsigned d : a->decimationChoices()) {
            b.decimations.push_back({std::string(), static_cast<double>(d), a->hardwareSampleRateHz() / d,
                                     d == a->decimation()});
        }
    }
    // THE PATCH PAGE: the graph (and its document), what the plan said, the
    // parts it may use, its radios.
    for (const cascade::core::patch::Node& n : patchGraph_.nodes()) {
        ApiListBlock::PatchNode pn;
        pn.id = n.id;
        pn.name = n.name;
        pn.kind = nodeKindWord(n.kind);
        pn.x = n.x;
        pn.y = n.y;
        pn.freqHz = n.freqHz;
        pn.rateHz = n.rateHz;
        pn.on = n.on;
        pn.refused = std::find(patchRefused_.begin(), patchRefused_.end(), n.id) != patchRefused_.end();
        const auto r = patchRadios_.find(n.id);
        pn.fault = (r != patchRadios_.end() && !r->second->fault().empty()) || patchRadioError_.count(n.id) != 0;
        b.patchNodes.push_back(std::move(pn));
    }
    for (const cascade::core::patch::Wire& w : patchGraph_.wires()) {
        b.patchWires.push_back({w.from, w.fromPort, w.to, w.toPort});
    }
    for (const auto& pr : patchPlan_.problems) {
        b.patchProblems.push_back({pr.node, std::to_string(static_cast<int>(pr.problem)),
                                   cascade::core::patch::problemText(pr.problem)});
    }
    for (const cascade::core::patch::DecoderInfo& d : patchCatalogue_) {
        b.patchParts.push_back({d.name, d.key, static_cast<double>(d.feed), d.requiredRateHz, d.tracks});
    }
    // THE DEVICES A RADIO CAN BE SET TO (API 0.3 8.12: PATCH_DEVICES), in the order the Radio's Device combo lists them,
    // each key whole in a LOCAL fact beside the entry (its detail is cut at 159 bytes), and a recording's own rate.
    {
        const std::vector<PatchDeviceChoice> choices = patchDeviceChoices();
        for (std::size_t i = 0; i < choices.size(); ++i) {
            ApiListBlock::PatchDevice d;
            d.label = choices[i].label;
            d.key = choices[i].key;
            for (const cascade::core::patch::RecordingInfo& r : patchRecordings_) {
                if (r.key == d.key) { d.rateHz = r.rateHz; }
            }
            fact("patch.device." + std::to_string(i + 1), d.key, true);
            b.patchDevices.push_back(std::move(d));
        }
    }
    // The receiver's radio as a Radio node would name it (the lent one, else the open one), and the air centre it was
    // last tuned to ("" when it never was): what a new Radio starts on.
    fact("patch.receiverRadio", patchReceiverRadioKey(), true);
    {
        std::string centre;
        if (const std::optional<double> c = carriedAirCentre()) {
            char text[40];
            std::snprintf(text, sizeof(text), "%.17g", *c);
            centre = text;
        }
        fact("patch.receiverCentre", std::move(centre));
    }
    // THE CONVERTER IN FORCE FOR EACH RADIO NODE'S DEVICE ("<mode> <LO Hz> <inverted>", as fact.converter.live spells
    // it): what decides whether a centre a user types can be taken (core::radioCentreTakeable).
    for (const cascade::core::patch::Node& n : patchGraph_.nodes()) {
        if (n.kind != cascade::core::patch::NodeKind::Radio || n.device.empty()) { continue; }
        const cascade::core::ConverterSetting conv = converterForKey(n.device);
        char text[96];
        std::snprintf(text, sizeof(text), "%d %.17g %d", static_cast<int>(conv.mode), conv.loHz, conv.inverted ? 1 : 0);
        fact("patch.conv." + std::to_string(n.id), text);
    }
    for (const auto& r : patchRadios_) {
        ApiListBlock::PatchRadio pr;
        pr.node = r.first;
        pr.device = r.second->label();
        pr.rateHz = r.second->rateHz();
        pr.centreHz = r.second->centreHz();
        pr.open = true;
        pr.fault = !r.second->fault().empty();
        pr.error = r.second->fault();
        // API 0.3 8.12: a RUNNING radio can carry a line that is not a fault - the open refused the rate asked for
        // and runs at its own, the device would not take a retune - and the face prints it. The line the face prints
        // (patchRadioError_, which holds a fault too) is the detail, the FAULT flag stays the fault's alone.
        const auto note = patchRadioError_.find(r.first);
        if (note != patchRadioError_.end()) { pr.error = note->second; }
        b.patchRadios.push_back(std::move(pr));
    }
    // A radio whose open is still on its worker: its device, and nothing else yet.
    for (const auto& p : patchRadioPending_) {
        if (patchRadios_.count(p.first) != 0) { continue; }
        ApiListBlock::PatchRadio pr;
        pr.node = p.first;
        pr.pending = true;
        if (const cascade::core::patch::Node* n = patchGraph_.find(p.first)) { pr.device = patchDeviceLabel(n->device); }
        b.patchRadios.push_back(std::move(pr));
    }
    for (const auto& e : patchRadioError_) {
        if (patchRadios_.count(e.first) != 0 || patchRadioPending_.count(e.first) != 0) { continue; }
        ApiListBlock::PatchRadio pr;
        pr.node = e.first;
        pr.error = e.second;
        pr.fault = true;
        b.patchRadios.push_back(std::move(pr));
    }
    // THE PLAN: a channel's offset, rate, decimation and radio; a decoder's rates, centre and source.
    for (const auto& ch : patchPlan_.channels) {
        ApiListBlock::PatchPlanEntry e;
        e.node = ch.node;
        e.v[0] = ch.offsetHz;
        e.v[1] = ch.outRateHz;
        e.v[2] = static_cast<double>(ch.decimation);
        e.v[3] = static_cast<double>(ch.radio);
        b.patchPlan.push_back(std::move(e));
    }
    for (const auto& d : patchPlan_.decoders) {
        ApiListBlock::PatchPlanEntry e;
        e.node = d.node;
        e.decoder = true;
        if (d.plugin < patchCatalogue_.size()) { e.key = patchCatalogue_[d.plugin].key; }
        e.source = d.source == cascade::core::patch::DecoderSource::Radio     ? "decoder:radio"
                   : d.source == cascade::core::patch::DecoderSource::Channel ? "decoder:channel"
                                                                              : "decoder:audio";
        e.v[0] = d.rateHz;
        e.v[1] = d.inRateHz;
        e.v[2] = d.centreHz;
        e.v[3] = static_cast<double>(d.radio);
        b.patchPlan.push_back(std::move(e));
    }
    // THE GRAPH, ITS VERSION AND ITS EPOCH: three facts of ONE block, so an interface that reads all three holds
    // one publish (API 0.3 8.12). `runnable` is the plan's verdict as the page lettered it.
    fact("patch.graph", cascade::core::patch::graphCommandText(patchGraph_), true);
    fact("patch.version", std::to_string(patchGraphVersion_));
    fact("patch.epoch", std::to_string(patchGraphEpoch_));
    fact("patch.runnable", patchPlan_.runnable ? "1" : "0");
    b.patchEpoch = patchGraphEpoch_;
    b.patchVersion = patchGraphVersion_;
    b.patchRunning = patchRunning_;
    // THE USAGE SWITCH AND THE SESSION'S COUNTERS (API 0.3 8.13), all LOCAL: consent to usage reporting is given at this
    // machine (FOXAPI_OP_TELEMETRY_ENABLE is refused from everyone; the desktop's FOXAPP_OP_TELEMETRY_CONSENT is the one
    // door), and the id it mints is the one identifier this product holds - "" while reporting is off, because then it does
    // not exist. The counters are the diagnostics bundle's (launches, and crashes: the runs that never wrote their clean-exit
    // marker).
    fact("telemetry.enabled", telemetryEnabled_ ? "1" : "0", true);
    fact("telemetry.installId", telemetryEnabled_ ? telemetryInstallId_ : std::string(), true);
    fact("diag.launches", std::to_string(telemetryLaunches_), true);
    fact("diag.crashes", std::to_string(telemetryCrashes_), true);
    // THE SERVERS (API 0.3 8.16), all LOCAL: what the local operator configured through SERVER_CONFIG, what the policy makes of it and
    // what the listeners say of themselves. NEVER the password or its record - only whether one is set.
    {
        const ServerConfig sc = servers_.config();
        const ServerHost::Listener wl = servers_.web();
        const ServerHost::Listener cl = servers_.cat();
        cascade::net::WebServerConfig probe = sc.web;
        probe.enabled = true;  // "would it be allowed", whether or not it is switched on
        const cascade::net::BindDecision decision = cascade::net::evaluateBind(probe);
        fact("web.enabled", sc.web.enabled ? "1" : "0", true);
        fact("web.bindAddress", sc.web.bindAddress, true);
        fact("web.port", std::to_string(sc.web.port), true);
        fact("web.username", sc.web.username, true);
        fact("web.hasPassword", sc.web.passwordRecord.empty() ? "0" : "1", true);
        fact("web.passwordPending", servers_.passwordPending() ? "1" : "0", true);
        fact("web.reachable", decision.allowed() && decision.reachableOffMachine ? "1" : "0", true);
        fact("web.authRequired", decision.allowed() && decision.authRequired ? "1" : "0", true);
        fact("web.policy", decision.allowed() ? std::string() : decision.reason, true);
        std::string addresses;
        for (const std::string& a : wl.addresses) { addresses += (addresses.empty() ? "" : "\n") + a; }
        fact("web.addresses", addresses, true);
        fact("cat.enabled", sc.cat.enabled ? "1" : "0", true);
        fact("cat.bindAll", sc.cat.bindAll ? "1" : "0", true);
        fact("cat.port", std::to_string(sc.cat.port), true);
        const auto add = [&b](const char* name, const ServerHost::Listener& l, bool enabled, int configuredPort,
                              const ServerHost::Counters& live) {
            ApiListBlock::Server s;
            s.name = name;
            s.running = l.running;
            s.port = l.running && l.boundPort > 0 ? l.boundPort : configuredPort;
            s.refusal = l.refusal;
            s.fault = enabled && !l.running && !l.refusal.empty();
            s.live = &live;
            b.servers.push_back(std::move(s));
        };
        add("web", wl, sc.web.enabled, sc.web.port, servers_.webCounters());
        add("cat", cl, sc.cat.enabled, sc.cat.port, servers_.catCounters());
    }
    // THE RECORDINGS: the two recorders' takes (current or last), then the
    // patch's files.
    fact("recorder.dir", recordDir_, true);
    for (int k = 0; k < 2; ++k) {
        const cascade::core::Recorder& rec = k == 0 ? iqRecorder_ : audioRecorder_;
        const std::string path = rec.path();
        fact(k == 0 ? "recorder.iq.path" : "recorder.audio.path", path, true);
        if (path.empty()) { continue; }
        ApiListBlock::Recording r;
        const std::size_t slash = path.find_last_of("/\\");
        r.name = slash == std::string::npos ? path : path.substr(slash + 1);
        r.path = path;
        r.kind = k + 1;
        r.rateHz = k == 0 ? iqRecordRateHz_ : cascade::core::Pipeline::kAudioRateHz;
        r.active = rec.recording();
        r.recorder = &rec;
        b.recordings.push_back(std::move(r));
    }
    for (const cascade::core::patch::RecordingInfo& ri : patchRecordings_) {
        ApiListBlock::Recording r;
        r.name = ri.fileName;
        r.path = ri.path;
        r.kind = 3;
        r.rateHz = ri.rateHz;
        b.recordings.push_back(std::move(r));
    }
}

// THE SOURCE SECTION AND THE SOUND CARD (API 0.3 8.10): everything the section draws that the lists above do not
// carry, built with the block and only when the fingerprint moved - the facts (whole texts and ids), the dongles that
// cannot be opened, the runtime's evidence, and the sound card's inputs, rates and form.
void Engine::buildApiListsSource(ApiListBlock& b) {
    const auto fact = [&b](const std::string& key, std::string value, bool localOnly = false) {
        b.facts[key] = ApiListBlock::Fact{std::move(value), localOnly};
    };
    const auto joined = [](const std::vector<std::string>& v) {
        std::string out;
        for (const std::string& x : v) { out += (out.empty() ? "" : ", ") + x; }
        return out;
    };
    // The combo's own row, the kind of radio kept for the config, the radio being opened.
    fact("source.row", std::to_string(sourceSel_));
    fact("source.keep", restoreKeep_.kind);
    fact("source.opening", deviceOpenPending_ ? deviceBusyLabel_ : std::string());
    // The scan beside an open radio: the name the two deferral sentences quote, and the drivers a scan leaves out
    // ("" with the whole SoapySDR scan deferred, and with nothing gating it).
    {
        const bool gated = soapyScanGated();
        std::string gate;
        std::string skip;
        if (gated) {
            gate = soapyScanGateDevice();
            const cascade::gui::SoapyScanPlan plan = soapyScanPlan();
            if (plan.mode == cascade::gui::SoapyScanMode::SkipSome) { skip = joined(plan.skipDrivers); }
        }
        fact("source.scanGate", std::move(gate));
        fact("source.scanSkip", std::move(skip));
    }
    fact("source.faultedDrivers", joined(faultedDrivers_));
    // WHAT THE LAST SCAN SAID ABOUT UHD'S FIRMWARE IMAGES (master 0.99.85, re-expressed in the 0.99.90 merge): "1" when UHD itself
    // reported it has none, and (separately) when the scan's USB listing held a USRP. The Source section draws
    // gui::uhdImagesMissingHint(both) under the list, and the radio-setup page's images finding reads the first.
    fact("source.uhdNoImages", soapyUhdNoImages_ ? "1" : "0");
    fact("source.usrpOnUsb", soapyUsrpOnUsb_ ? "1" : "0");
    fact("source.plutoUri", plutoUri_, true);
    fact("source.rtlTcpAddr", rtlTcpAddr_, true);
    // The Sinks rail's AUDIO BUFFER (0.99.73): the chosen value (0 = AUTOMATIC) and the sentence's parts when automatic last moved the lead
    // ("raised|restored|lowered <ms> <count>", empty when nothing). The lead in force is the state's audioLeadMs.
    fact("audio.bufferMs", std::to_string(audioBufferMs_), true);
    fact("audio.leadNote", audioLeadNoteFact(), true);
    fact("audio.leadMs", std::to_string(audioLeadNowMs()), true);
    // THE CRYSTAL CORRECTION (0.99.56): the switch, how it is applied to the open radio ("na" nothing to correct, "radio", "retune"),
    // this radio's value, whether the radio takes whole ppm only, why it refused, the card's value, and which radio the value is for.
    {
        const PpmFacts p = ppmFacts();
        char num[48];
        std::snprintf(num, sizeof num, "%.17g", p.stored);
        fact("ppm.on", p.on ? "1" : "0");
        fact("ppm.method", p.method);
        fact("ppm.value", num);
        fact("ppm.whole", p.wholeRadio ? std::to_string(p.wholeValue) : std::string());
        fact("ppm.error", p.error);
        fact("ppm.card", p.cardValue);
        fact("ppm.model", p.model, true);
        fact("ppm.serial", p.serial, true);
        fact("ppm.key", p.key, true);
    }   // the lead in force, ms (the rail's AUTOMATIC preview)
    // Every DEVICES entry's whole id, keyed by the entry's own id (index + 1): the item's detail cuts at 159 bytes
    // and the Pluto's, with its address, can run past it.
    for (std::size_t i = 0; i < b.devices.size(); ++i) { fact("source.id." + std::to_string(i + 1), b.devices[i].id); }
    // UNBOUND_RADIOS: the family is the one the Source section's sentence is worded by.
    for (const cascade::usb::UsbDeviceInfo& u : nativeUnbound_) {
        ApiListBlock::UnboundRadio r;
        r.description = u.description;
        r.vid = u.vid;
        r.pid = u.pid;
        for (const cascade::usb::UsbId& id : cascade::source::rtlSdrUsbIds()) {
            if (u.vid == id.vid && u.pid == id.pid) { r.family = "rtl"; }
        }
        for (const cascade::usb::UsbId& id : cascade::source::rx888UsbIds()) {
            if (u.vid == id.vid && u.pid == id.pid) { r.family = "rx888"; }
        }
        b.unbound.push_back(std::move(r));
    }
    // SCAN_EVIDENCE: vendor installs, search paths, modules - the order the section's tree draws them in.
    for (const cascade::source::VendorRoot& v : scanEvidence_.vendors) { b.evidence.push_back({1, v.name, v.root}); }
    for (const std::string& p : scanEvidence_.searchPaths) { b.evidence.push_back({2, std::string(), p}); }
    for (const std::string& m : scanEvidence_.modules) { b.evidence.push_back({3, std::string(), m}); }
    // THE SOUND CARD. The inputs as the last listing found them (ACTIVE: the entry the form's card is, by the same
    // matching the open uses), the rates that entry offers in the form's format, and the form itself - whole texts
    // in the LOCAL facts, the figures and flags in the list entry.
    const int at = cascade::source::matchSoundCard(soundCardDevices_, soundCard_.device, soundCard_.hostApi,
                                                   soundCard_.pickedFromList)
                       .at;
    for (std::size_t i = 0; i < soundCardDevices_.size(); ++i) {
        const cascade::source::SoundCardDevice& d = soundCardDevices_[i];
        b.soundCards.push_back({cascade::source::soundCardDeviceLabel(d), d.hostApi, static_cast<int>(i) == at});
    }
    if (at >= 0) {
        for (const cascade::source::SoundCardRate& r :
             cascade::source::soundCardRatesFor(soundCardDevices_[static_cast<std::size_t>(at)], soundCard_.format)) {
            b.soundCardRates.push_back({r.hz, r.exclusive, r.hz == soundCard_.cardRateHz});
        }
    }
    {
        ApiListBlock::SoundCardForm& f = b.soundCardForm;
        f.device = soundCard_.device;
        f.hostApi = soundCard_.hostApi;
        f.rateHz = soundCard_.cardRateHz;
        f.centreHz = soundCard_.iqCentreHz;
        f.channel = soundCard_.channel == 1 ? 1 : 0;
        f.iqStereo = soundCard_.format == cascade::source::SoundCardFormat::IqStereo;
        f.swapIq = soundCard_.swapIq;
        f.picked = soundCard_.pickedFromList;
    }
    fact("soundcard.device", soundCard_.device, true);
    fact("soundcard.hostApi", soundCard_.hostApi, true);
    // What the FORM's card would receive on the air - through the converter kept for that card (an I/Q card takes
    // none), "<lo Hz> <hi Hz>" to the digit each reads back by - the "Receives ... to ..." line's two figures.
    {
        const cascade::core::ConverterSetting stored = cascade::core::converterFor(
            converters_, resolveConverterKey(cascade::gui::soundCardConverterKey(soundCard_.device, soundCard_.hostApi)));
        const cascade::gui::SoundCardAirSpan span = cascade::gui::soundCardAirSpan(soundCard_, stored);
        char text[80];
        std::snprintf(text, sizeof(text), "%.17g %.17g", span.loHz, span.hiHz);
        fact("soundcard.span", text);
    }
}

// A PICTURE ARRIVED from a decoder's runner (pumpDecoderOutput, the pumping thread): it is moved in WHOLE behind a
// `const` pointer and given the next id, replacing the node's previous picture. Nothing writes it again, and nothing
// here is read from another thread - an interface reads the set syncPatchImages builds from these.
void Engine::takePatchImage(cascade::core::patch::NodeId node, cascade::core::HostImage&& img) {
    if (img.pixels.empty()) { return; }
    PatchImage& slot = patchImages_[node];
    slot.id = ++patchImageLastId_;
    const cascade::core::patch::Node* n = patchGraph_.find(node);
    slot.caption = n != nullptr ? n->name : img.plugin;
    slot.image = std::make_shared<const cascade::core::HostImage>(std::move(img));
    patchImagesDirty_ = true;
    if (std::find(patchImagesFresh_.begin(), patchImagesFresh_.end(), node) == patchImagesFresh_.end()) {
        patchImagesFresh_.push_back(node);
    }
}

// Once a pass, before PATCH_LIVE is taken: the pictures that belong to this graph, the set read_image serves, and the
// events that announce the pass's arrivals - in that order, so an interface that is told can read.
void Engine::syncPatchImages() {
    // A new patch document is a new set of nodes that may carry the old ones' ids: nothing of the old graph's is
    // shown on them.
    if (patchGraphEpoch_ != patchImagesEpoch_) {
        patchImagesEpoch_ = patchGraphEpoch_;
        if (!patchImages_.empty()) {
            patchImages_.clear();
            patchImagesDirty_ = true;
        }
    }
    // ...and a node the graph no longer holds takes its picture with it.
    for (auto it = patchImages_.begin(); it != patchImages_.end();) {
        if (patchGraph_.find(it->first) == nullptr) {
            it = patchImages_.erase(it);
            patchImagesDirty_ = true;
        } else {
            ++it;
        }
    }
    if (patchImagesDirty_) {
        auto set = std::make_shared<ApiImageSet>();
        set->entries.reserve(patchImages_.size());
        for (const auto& pic : patchImages_) {
            ApiImageSet::Entry e;
            e.node = pic.first;
            e.id = pic.second.id;
            e.caption = pic.second.caption;
            e.image = pic.second.image;
            set->entries.push_back(std::move(e));
        }
        apiImages_ = std::move(set);
        api_->setPatchImages(apiImages_);
        patchImagesDirty_ = false;
    }
    for (const cascade::core::patch::NodeId node : patchImagesFresh_) {
        const auto it = patchImages_.find(node);
        if (it != patchImages_.end()) { api_->patchImageArrived(node, it->second.id, it->second.caption); }
    }
    patchImagesFresh_.clear();
}

// PATCH_LIVE (API 0.3 8.12): what the running patch nodes are doing now, taken once a pass. The readings are first
// collected as numbers (patchLiveRaw_, whose room is kept) and fingerprinted with the text that can move without a
// number moving (a speaker's error); only when the fingerprint has moved is a block built, with the texts. A pass that
// finds the same readings builds nothing, allocates nothing and hands the host nothing.
void Engine::refreshPatchLive() {
    namespace pc = cascade::core::patch;
    std::vector<PatchLiveRaw>& raw = patchLiveRaw_;
    raw.clear();
    // A demodulator's squelch gate: the level it judges and whether it is open (the channel it listens to, on the radio
    // that channel is cut from). Keyed by the DEMOD node, which is the node whose face and inspector show it.
    for (const pc::Node& n : patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Demod) { continue; }
        const pc::NodeId chan = cascade::gui::demodChannel(patchGraph_, n.id);
        if (chan == pc::kNoNode) { continue; }
        const auto r = patchRadios_.find(pc::radioOf(patchGraph_, chan));
        if (r == patchRadios_.end()) { continue; }
        float level = 0.0f;
        bool open = false;
        if (!r->second->runner().squelchState(chan, level, open)) { continue; }
        raw.push_back({n.id, 1, static_cast<double>(level), 0.0, open, nullptr});
    }
    // A speaker's output: its newest peak and how much it has written. The destination is immutable once made (its
    // describe() is the same for its life), so only its error is hashed with the figures.
    for (const auto& d : patchDests_) {
        if (!d.second) { continue; }
        raw.push_back({d.first, 2, static_cast<double>(d.second->peak()), static_cast<double>(d.second->samples()), false,
                       d.second.get()});
    }
    // ...and a speaker whose output could not be made.
    for (const auto& e : patchDestError_) { raw.push_back({e.first, 5, 0.0, 0.0, false, &e.second}); }
    // A channel's level, measured off the spectrum of the radio it is cut from.
    for (const auto& ch : patchPlan_.channels) {
        const auto rs = patchSpectra_.find(ch.radio);
        const auto rr = patchRadios_.find(ch.radio);
        if (rs == patchSpectra_.end() || rr == patchRadios_.end()) { continue; }
        const pc::Level lv = pc::channelLevelDb(rs->second.db, rr->second->rateHz(), ch.offsetHz);
        if (lv.valid) { raw.push_back({ch.node, 3, static_cast<double>(lv.db), 0.0, false, nullptr}); }
    }
    // A running decoder's face: how many lines it has said (a decoder that has said none is "0 lines": it is running),
    // and its newest. One that refused to start has none.
    for (const auto& dp : patchPlan_.decoders) {
        if (std::find(patchRefused_.begin(), patchRefused_.end(), dp.node) != patchRefused_.end()) { continue; }
        const auto it = patchDecoderFaces_.find(dp.node);
        raw.push_back({dp.node, 4, it != patchDecoderFaces_.end() ? static_cast<double>(it->second.lines) : 0.0, 0.0,
                       false, it != patchDecoderFaces_.end() ? &it->second : nullptr});
    }
    // A picture decoder's newest picture (read_image takes the id): kept while the node is in the graph, so the row is
    // there with the patch stopped too. The id is its identity - a new picture is a new id - and `open` says the decoder
    // has finished it.
    for (const auto& pic : patchImages_) {
        raw.push_back({pic.first, 6, static_cast<double>(pic.second.id),
                       static_cast<double>(pic.second.image->pixels.size()), pic.second.image->complete, &pic.second});
    }
    Fingerprint f;
    for (const PatchLiveRaw& r : raw) {
        f.value(r.node);
        f.value(r.kind);
        f.value(r.v0);
        f.value(r.v1);
        f.value(r.open);
        if (r.kind == 2) { f.text(static_cast<const cascade::core::patch::AudioDest*>(r.ref)->error()); }
        if (r.kind == 5) { f.text(*static_cast<const std::string*>(r.ref)); }
        if (r.kind == 4 && r.ref != nullptr) { f.text(static_cast<const PatchDecoderFace*>(r.ref)->last); }
    }
    if (apiLive_ != nullptr && apiLive_->fingerprint == f.h) { return; }
    auto b = std::make_shared<ApiLiveBlock>();
    b->fingerprint = f.h;
    b->entries.reserve(raw.size());
    for (const PatchLiveRaw& r : raw) {
        ApiLiveBlock::Entry e;
        e.node = r.node;
        e.kind = r.kind;
        e.v0 = r.v0;
        e.v1 = r.v1;
        e.open = r.open;
        if (r.kind == 2) {
            const auto* d = static_cast<const cascade::core::patch::AudioDest*>(r.ref);
            e.name = d->describe();
            e.detail = d->error();
        } else if (r.kind == 5) {
            e.detail = *static_cast<const std::string*>(r.ref);
        } else if (r.kind == 4 && r.ref != nullptr) {
            e.detail = static_cast<const PatchDecoderFace*>(r.ref)->last;
        } else if (r.kind == 6) {
            e.name = static_cast<const PatchImage*>(r.ref)->caption;
        }
        b->entries.push_back(std::move(e));
    }
    apiLive_ = std::move(b);
    api_->setPatchLive(apiLive_);
}

// AIRBAND_CHANNELS (API 0.3 9.6, stub campaign B6): the channels the Airband monitor plays, taken once a pass. The readings are fingerprinted as
// numbers - a level to half a decibel, the heard time to a tenth of a second, so the block is not rebuilt for a wobble nobody can see - and only
// when the fingerprint has moved is a block built, with the names. A pass that finds the same readings builds nothing, allocates nothing and hands
// the host nothing. Empty while the monitor is not listening.
void Engine::refreshAirbandLive() {
    const cascade::core::AirbandBlock* onAir =
        (airbandListening_ && airbandTuned_ && airbandBlock_ < airbandBlocks_.size()) ? &airbandBlocks_[airbandBlock_] : nullptr;
    const auto inBlock = [onAir](std::size_t i) {
        return onAir != nullptr && std::find(onAir->members.begin(), onAir->members.end(), i) != onAir->members.end();
    };
    const std::size_t n = airbandListening_ ? airbandChans_.size() : 0u;
    Fingerprint f;
    f.value(n);
    for (std::size_t i = 0; i < n; ++i) {
        const AirbandChan& c = airbandChans_[i];
        f.value(c.freqHz);
        f.value(c.bandwidthHz);
        f.value(c.open);
        f.value(inBlock(i));
        f.value(static_cast<long long>(std::lround(static_cast<double>(c.levelDb) * 2.0)));
        f.value(static_cast<long long>(std::llround(c.pendingHeardS * 10.0)));
        f.value(c.rowId);
    }
    if (apiAirband_ != nullptr && apiAirband_->fingerprint == f.h) { return; }
    auto b = std::make_shared<ApiAirbandBlock>();
    b->fingerprint = f.h;
    b->entries.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const AirbandChan& c = airbandChans_[i];
        ApiAirbandBlock::Entry e;
        e.id = static_cast<std::uint64_t>(i) + 1u;
        e.name = c.name;
        e.freqHz = c.freqHz;
        e.bandwidthHz = c.bandwidthHz;
        e.levelDb = static_cast<double>(c.levelDb);
        e.heardS = c.pendingHeardS;
        e.open = c.open;
        e.onAir = inBlock(i);
        b->entries.push_back(std::move(e));
    }
    apiAirband_ = std::move(b);
    api_->setAirbandLive(apiAirband_);
}

// Once a pass, on the pumping thread, before the state is published: a new
// block for the table when anything in it changed. Returns the fingerprint
// the publish carries into listSeq.
std::uint64_t Engine::publishApiLists() {
    // A change made to the patch graph since it was last looked at (the runtime's own, a command applied after
    // pumpPatch) is a version before it is published (API 0.3 8.12).
    syncPatchGraphVersion();
    syncPatchImages();
    refreshPatchLive();
    refreshAirbandLive();
    const std::uint64_t fp = apiListsFingerprint();
    if (apiLists_ == nullptr || apiLists_->fingerprint != fp) {
        apiLists_ = buildApiLists(fp);
        api_->setLists(apiLists_);
    }
    // The decoder-output backlog goes to the host's own fixed ring (no block
    // rebuild per line). Its count does NOT move listSeq (API 0.3 8.7, D3): a busy decoder (ADS-B, tens of
    // lines a second) would otherwise move it once a line and make every interface re-read every list it
    // keeps, once a line. The count is a measurement, decoderLinesTotal, which an interface that wants the
    // backlog watches.
    api_->takeDecoderLines(decoderLog_, decoderLinesTotal_);
    return fp;
}

}  // namespace cascade::engine
