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
#include <memory>
#include <string>

#include "core/i18n.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_plan.hpp"
#include "core/plugin_host.hpp"
#include "core/serial_port.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver_tables.hpp"
#include "source/airspy_source.hpp"

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

std::string deviceId(const std::string& kind, const std::string& args) { return kind + ":" + args; }

// A GAIN STAGE ITS OWN AGC IS DRIVING (API 0.3 8.3, FOXAPI_ITEM_AGC): an
// Airspy's LNA or MIXER with its automatic gain control on. The stage is named
// by the Airspy driver, so the rule is the panel's own (the "driven" test it
// drew greyed sliders by).
bool gainDrivenByAgc(const cascade::source::AirspySource* airspy, const std::string& stage) {
    if (airspy == nullptr) { return false; }
    return (stage == "LNA" && airspy->lnaAgc()) || (stage == "MIXER" && airspy->mixerAgc());
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
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        apiPluginKeys_.push_back(cascade::core::pluginKey(p));
    }
}

std::uint64_t Engine::apiListsFingerprint() {
    Fingerprint f;
    f.value(modeIndex_);
    f.value(vfoBandwidthHz_);
    f.value(freqMgr_.version());
    f.value(freqMgr_.list().size());
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
        f.value(pluginIsStopped(apiPluginKeys_[i]));
    }
    // Which decoders are FED: the keys the pass's publish already took from
    // the runner in ONE acquisition of its lock (the lock its DSP thread
    // takes per block) - never asked again here.
    const bool running = pipeline_.running();
    f.value(running);
    if (running) {
        for (const std::string& k : feedingKeysScratch_) { f.text(k); }
    }
    // --- API 0.3's, and the 0.2 lists step 1 left out ----------------------
    // The store.
    f.value(catalog_.size());
    for (const cascade::core::PluginCatalogEntry& e : catalog_) {
        f.text(e.id);
        f.text(e.version);
        f.value(e.legalNotice.size());
    }
    f.value(pluginBlocked_.size());
    for (const cascade::core::BlockedPlugin& b : pluginBlocked_) { f.text(b.installed.file); }
    f.value(catalogPending_);
    f.value(installPending_);
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
    // The status lines.
    for (const std::string* t : {&sourceError_, &gpsRefusal_, &catalogError_, &bandPlanError_, &tuneMismatchNote_,
                                 &transmitError_, &soundCardMissing_, &sdrPlayApiDetail_, &sdrPlayAdvice_,
                                 &recordNotice_, &recordError_, &presetNote_, &pluginEnforceError_,
                                 &restoreKeepLabel_, &audioHealthNote_}) {  // (audioHealth: API 0.3 8.4)
        f.text(*t);
    }
    // The patch page: its graph, what the plan said, its radios.
    f.value(graphHash(patchGraph_));
    f.value(patchGraphEpoch_);
    f.value(patchRunning_);
    for (const auto& pr : patchPlan_.problems) {
        f.value(pr.node);
        f.value(pr.problem);
    }
    for (cascade::core::patch::NodeId n : patchRefused_) { f.value(n); }
    f.value(patchCatalogue_.size());
    for (const auto& r : patchRadios_) {
        f.value(r.first);
        f.value(r.second->rateHz());
        f.value(r.second->fault().empty());
    }
    for (const auto& e : patchRadioError_) {
        f.value(e.first);
        f.text(e.second);
    }
    f.value(patchRecordings_.size());
    // The recorders: whether each is taking, and to which file (the bytes
    // are read live, never fingerprinted).
    f.value(iqRecorder_.recording());
    f.value(audioRecorder_.recording());
    f.value(iqRecordStartS_);
    f.value(audioRecordStartS_);
    f.text(recordDir_);
    // An Airspy's own choices.
    if (airspy != nullptr) {
        f.value(airspy->gainMode());
        f.value(airspy->decimation());
        f.value(airspy->hardwareSampleRateHz());
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
    b->devices.push_back({"Signal generator", "siggen", sourceKind_ == "siggen"});
    for (const auto& d : nativeDevices_) {
        b->devices.push_back({d.label, deviceId(d.driver, d.args), sourceKind_ == d.driver && deviceArgs_ == d.args});
    }
    for (const auto& d : soapyDevices_) {
        b->devices.push_back({d.label, deviceId("soapy", d.args), sourceKind_ == "soapy" && deviceArgs_ == d.args});
    }
    if (device_ != nullptr) {
        b->rates = deviceRatesHz_;
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
    // DECODERS: every decoder plugin, running or not, and why not.
    {
        const std::vector<cascade::core::DecoderStatus> status = pluginRunner_.status();
        for (const ApiListBlock::Plugin& p : b.plugins) {
            if (!p.decoder) { continue; }
            ApiListBlock::Decoder d;
            d.name = p.name;
            d.key = p.key;
            d.running = p.running;
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
    // THE STORE: the catalogue, what is refused, what would update - and the
    // long texts an entry carries, as facts.
    for (const cascade::core::PluginCatalogEntry& e : catalog_) {
        b.store.push_back({e.id, e.name, catalogEntryInstalled(e), e.compatible});
        const std::string k = "store." + e.id + ".";
        fact(k + "legal", e.legalNotice);
        fact(k + "summary", e.summary);
        fact(k + "description", e.description);
        fact(k + "homepage", e.homepage);
        fact(k + "version", e.version);
    }
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        fact("store." + catalog_[static_cast<std::size_t>(i)].id + ".blocked", pluginInstallBlockedReason(i, true));
    }
    for (const cascade::core::BlockedPlugin& x : pluginBlocked_) { b.storeBlocked.emplace_back(x.installed.file, x.message); }
    for (const cascade::core::PluginUpdate& u : plannedPluginUpdates()) {
        b.storeUpdates.emplace_back(u.id, u.fromVersion + " -> " + u.toVersion);
    }
    // THE TRANSMITTER's one "device": the address TX_OPEN is given.
    // Until the page has seeded (or somebody typed) one, the seed it would use.
    b.txArgs = transmitArgs_.empty() ? transmitArgsSeed() : transmitArgs_;
    b.txOpen = transmitter_.haveSink();
    fact("transmit.args", b.txArgs, true);
    // PLUGIN MARKS on the spectrum.
    for (const cascade::core::HostMarker& m : pluginMarkers_) {
        b.markers.push_back({m.m.id, m.m.label, m.name, m.m.freqHz, m.m.widthHz, m.m.colourRgba});
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
                              ip.preset.demodMode, ip.index});
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
        b.patchParts.push_back({d.name, d.key, static_cast<double>(d.feed), d.requiredRateHz});
    }
    for (const auto& r : patchRadios_) {
        ApiListBlock::PatchRadio pr;
        pr.node = r.first;
        pr.device = r.second->label();
        pr.rateHz = r.second->rateHz();
        pr.open = true;
        pr.error = r.second->fault();
        pr.fault = !pr.error.empty();
        b.patchRadios.push_back(std::move(pr));
    }
    for (const auto& e : patchRadioError_) {
        if (patchRadios_.count(e.first) != 0) { continue; }
        ApiListBlock::PatchRadio pr;
        pr.node = e.first;
        pr.error = e.second;
        pr.fault = true;
        b.patchRadios.push_back(std::move(pr));
    }
    fact("patch.graph", cascade::core::patch::graphCommandText(patchGraph_), true);
    b.patchEpoch = patchGraphEpoch_;
    b.patchGraphHash = graphHash(patchGraph_);
    b.patchRunning = patchRunning_;
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

// Once a pass, on the pumping thread, before the state is published: a new
// block for the table when anything in it changed. Returns the fingerprint
// the publish carries into listSeq.
std::uint64_t Engine::publishApiLists() {
    const std::uint64_t fp = apiListsFingerprint();
    if (apiLists_ == nullptr || apiLists_->fingerprint != fp) {
        apiLists_ = buildApiLists(fp);
        api_->setLists(apiLists_);
    }
    // The decoder-output backlog goes to the host's own fixed ring (no block
    // rebuild per line); its count moves listSeq with the rest.
    api_->takeDecoderLines(decoderLog_, decoderLinesTotal_);
    return fp ^ (decoderLinesTotal_ * 0x9E3779B97F4A7C15ull);
}

}  // namespace cascade::engine
