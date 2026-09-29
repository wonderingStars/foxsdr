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
#include <cstring>
#include <memory>
#include <string>

#include "core/plugin_host.hpp"
#include "engine/engine.hpp"
#include "engine/receiver_tables.hpp"

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
    template <class T>
    void value(const T& v) {
        bytes(&v, sizeof(v));
    }
};

std::string deviceId(const std::string& kind, const std::string& args) { return kind + ":" + args; }

}  // namespace

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
    if (haveDevice) {
        for (double r : deviceRatesHz_) { f.value(r); }
        f.value(pipeline_.activeSource().sampleRateHz());
        for (std::size_t i = 0; i < deviceGainRanges_.size(); ++i) {
            const auto& g = deviceGainRanges_[i];
            f.text(g.name);
            f.value(g.minDb);
            f.value(g.maxDb);
            f.value(g.stepDb);
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
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        f.text(p.name);
        f.text(p.path);
        f.value(p.loaded);
        f.value(pluginIsStopped(cascade::core::pluginKey(p)));
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
        for (std::size_t i = 0; i < deviceGainRanges_.size(); ++i) {
            const auto& g = deviceGainRanges_[i];
            b->gains.push_back({g.name, g.minDb, g.maxDb, g.stepDb,
                                i < deviceGainsDb_.size() ? static_cast<double>(deviceGainsDb_[i]) : 0.0});
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
    return b;
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
    return fp;
}

}  // namespace cascade::engine
