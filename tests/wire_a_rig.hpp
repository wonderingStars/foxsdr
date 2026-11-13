// wire_a_rig.hpp - the real engine, one local session with every grant, for the tests of wiring lane wire-a (tests/test_engine_api_wire_a_*.cpp). A hermetic engine keeps
// its lists off the disk; `persist(dir)` gives it the per-user folder a front end gives it (the path of markers.json), which is where the lane's files go.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/freq_manager.hpp"
#include "engine/engine.hpp"
#include "source/siggen.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void recordDir(Engine& e, const std::string& dir) { e.recordDir_ = dir; }
    static void markerPath(Engine& e, const std::string& path) { e.markerPath_ = path; }
    static cascade::core::FreqManager& bookmarks(Engine& e) { return e.freqMgr_; }
    static void bandPlanDir(Engine& e, const std::string& dir) { e.bandPlanDirForTest_ = dir; e.loadBandPlan(); }
    // The NEXT I/Q take's file open (a worker's: core/recorder.hpp Opener) returns `ms` later than the file is on disk, once, so the take is "starting" - its file
    // exists, the engine has not collected the open - for that long. The window a real take has for a few milliseconds, made wide enough to act in.
    static void slowIqOpenOnce(Engine& e, int ms) {
        cascade::core::Recorder& rec = e.rx().iqRecorder_;
        cascade::core::Recorder::Opener inner = rec.opener();
        auto done = std::make_shared<std::atomic<bool>>(false);
        rec.bindOpener([inner, ms, done](const cascade::core::Recorder::OpenRequest& q, cascade::core::Recorder::OpenedFile& f, std::string& err) {
            const bool ok = inner(q, f, err);
            if (!done->exchange(true)) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
            return ok;
        });
    }
    // The generator source of radio 1 (nullptr when it is not the generator): a tone `slot` at `offsetHz` from its centre.
    static bool genTone(Engine& e, int slot, double offsetHz, float amplitudeDb) {
        cascade::source::SigGen* g = &e.rx().pipeline_.sigGen();
        if (g == nullptr) { return false; }
        g->setTone(slot, offsetHz, amplitudeDb);
        return true;
    }
};
}  // namespace cascade::gui

namespace wire_a_rig {

namespace fs = std::filesystem;
using Access = cascade::gui::AppWindowTestAccess;

constexpr const char* kHooks = "testHooks=1;token=s3cret";
inline const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

struct Rig {
    cascade::engine::Engine eng;
    std::unique_ptr<testing::Session> local;
    explicit Rig(const fs::path& userDir = fs::path(), const fs::path& recDir = fs::path()) {
        if (!userDir.empty()) {
            std::error_code ec;
            fs::create_directories(userDir, ec);
            Access::markerPath(eng, (userDir / "markers.json").string());
        }
        eng.configureApi(kHooks);
        eng.initialise();
        if (!recDir.empty()) { Access::recordDir(eng, recDir.string()); }
        eng.controlPass();
        local = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
        CHECK(local->ok());
    }
    // Submits one command (long text beside it when it does not fit) and passes until its result lands.
    testing::Outcome send(const FoxCommand& c, const std::string* longText = nullptr, testing::Session* who = nullptr, int passes = 40) {
        testing::Session& s = who != nullptr ? *who : *local;
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        int32_t n = 0;
        if (longText != nullptr) {
            n = api()->submit_text(s.raw(), &c, longText->data(), static_cast<uint32_t>(longText->size()), &sr);
        } else {
            n = api()->submit(s.raw(), &c, 1, &sr);
        }
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int i = 0; i < passes && !o.landed; ++i) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s.raw(), r, 16);
            for (int32_t k = 0; k < got; ++k) {
                if (r[k].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[k];
                }
            }
        }
        return o;
    }
    const FoxEngineApi* api() const { return wire_a_rig::api(); }
    // The id of the bookmark at `hz` (0 when there is none), from the API's list.
    std::uint64_t bookmarkId(double hz) {
        const int32_t n = api()->read_list(local->raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr);
        for (int32_t i = 0; i < n; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            api()->read_list(local->raw(), FOXAPI_LIST_BOOKMARKS, static_cast<uint32_t>(i), &it);
            if (std::fabs(it.value[0] - hz) < 1.0) { return it.id; }
        }
        return 0;
    }
    // Radio 1's live state in the words the lane's tests use ("tuned.1.hz" / ".centreHz" / ".mode" / ".bandwidthHz"): read_state follows every pass; the fact block does not follow a retune.
    std::string tunedFact(const std::string& key) {
        eng.controlPass();   // (the state is published by a pass)
        const FoxReceiverState st = local->state();
        static const char* const names[8] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
        auto whole = [](double v) { return std::to_string(static_cast<long long>(std::llround(v))); };
        if (key == "tuned.1.hz") { return whole(st.tunedHz); }
        if (key == "tuned.1.centreHz") { return whole(st.centreHz); }
        if (key == "tuned.1.bandwidthHz") { return whole(st.bandwidthHz); }
        if (key == "tuned.1.mode") { return st.demodMode >= 1 && st.demodMode <= 8 ? names[st.demodMode - 1] : ""; }
        return std::string();
    }
    std::string fact(const std::string& key) {
        if (key.rfind("tuned.1.", 0) == 0) { return tunedFact(key); }
        char buf[8192];
        const int32_t n = api()->get_setting(local->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    template <class F>
    bool until(F done, int ms = 8000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void settle(int ms) { until([] { return false; }, ms); }
};

}  // namespace wire_a_rig
