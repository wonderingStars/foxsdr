// wire1_rig.hpp - the engine rig shared by the engine-wiring lane's tests (engine wiring lane 1; docs/multi-radio-records/engine-wiring-1.md).
//
// One real Engine driven through the C table and its own control passes, as test_engine_api_source_kinds.cpp does; the helpers are that file's, moved here so the wiring
// tests do not each carry a copy. Include ONCE per translation unit, after the engine and API headers.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

using json = nlohmann::json;

namespace {
std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &noSoapyRadios;
    }
    static cascade::core::AppConfig snapshot(Engine& e) {
        e.publishConfig();
        return e.configSnapshot();
    }
    static void restore(Engine& e, const cascade::core::AppConfig& c) { e.applyConfig(c); }
    static std::size_t transmitSlot(const Engine& e) { return e.transmitRadioSlot(); }
    static std::string transmitArgs(const Engine& e) { return e.transmitArgs_; }
    static std::string deviceArgs(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->deviceArgs_ : std::string("<no radio>"); }
    static std::string kind(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->sourceKind_ : std::string("<no radio>"); }
    static std::string seed(const Engine& e) { return e.transmitArgsSeed(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO | FOXAPI_GRANT_TRANSMIT;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }


// --- one engine ---------------------------------------------------------------------------------------------------------------------------

struct Rig {
    Engine eng;
    std::unique_ptr<testing::Session> s;

    explicit Rig(const AppConfig* cfg = nullptr) {
        eng.configureApi(kHooks);
        eng.initialise();
        if (cfg != nullptr) { Access::restore(eng, cascade::core::startupState(*cfg)); }
        eng.controlPass();
        s = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kGrants);
        CHECK(s->ok());
        pump(3);
    }
    void pump(int n = 1) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    template <class F>
    bool until(F done, int timeoutMs = 15000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return false;
    }
    testing::Outcome send(const FoxCommand& c) {
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s->raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 40 && !o.landed; ++pass) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s->raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[8192];
        const int32_t n = api()->get_setting(s->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    FoxReceiverState radio(std::uint32_t n, std::int32_t* status = nullptr) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        const std::int32_t r = api()->read_radio_state(s->raw(), n, &st);
        if (status != nullptr) { *status = r; }
        return st;
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
        FoxCommand c = testing::cmd(op, n0, i0, text);
        c.target = target;
        return c;
    }
    // RADIO_ADD with a source id; the radio is started as soon as it is open.
    testing::Outcome add(long long number, const std::string& id, bool run = true) {
        FoxCommand c = testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, number, id.c_str());
        c.ival[1] = run ? 1 : 0;
        return send(c);
    }
    struct Spectrum {
        std::int32_t status = FOXAPI_NO_CHANGE;
        FoxSpectrumInfo info{};
        std::vector<float> bins;
    };
    Spectrum spectrum(std::uint32_t n) {
        Spectrum sp;
        sp.bins.assign(4096, 0.0f);
        sp.info.structSize = sizeof(sp.info);
        sp.status = api()->read_radio_spectrum(s->raw(), n, 0, &sp.info, sp.bins.data(), static_cast<std::uint32_t>(sp.bins.size()));
        if (sp.status == FOXAPI_OK) { sp.bins.resize(sp.info.binCount); }
        return sp;
    }
};

// The bin a tone `offsetHz` from the centre lands in, and the strongest bin; how far the strongest stands above the median.
std::size_t binFor(const Rig::Spectrum& sp, double offsetHz) {
    const double n = static_cast<double>(sp.info.binCount);
    return static_cast<std::size_t>(std::llround((offsetHz / sp.info.spanHz + 0.5) * n)) % sp.info.binCount;
}
std::size_t peakBin(const Rig::Spectrum& sp) { return static_cast<std::size_t>(std::max_element(sp.bins.begin(), sp.bins.end()) - sp.bins.begin()); }
float standout(const Rig::Spectrum& sp) {
    std::vector<float> v = sp.bins;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return sp.bins[peakBin(sp)] - v[v.size() / 2];
}
// The spectrum has a clear peak within `tol` bins of the one `offsetHz` names.
bool peakAt(const Rig::Spectrum& sp, double offsetHz, int tol = 3) {
    if (sp.status != FOXAPI_OK || sp.bins.empty()) { return false; }
    const long long want = static_cast<long long>(binFor(sp, offsetHz));
    const long long got = static_cast<long long>(peakBin(sp));
    return std::llabs(want - got) <= tol && standout(sp) > 15.0f;
}

AppConfig throughTheFile(Engine& e) {
    AppConfig out;
    std::string error;
    const std::string text = ConfigStore::serialize(Access::snapshot(e));
    CHECK(ConfigStore::parse(text, "test", out, error));
    return out;
}



}  // namespace
