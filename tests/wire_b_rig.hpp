// wire_b_rig.hpp - the engine rig the wire-b tests share: a real Engine on its signal generator, driven through the C API table (local session, every grant)
// the way test_engine_api_oirt does. Header only, test-side.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace wireb {

inline const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

struct Rig {
    cascade::engine::Engine eng;
    std::unique_ptr<testing::Session> local;
    Rig() {
        eng.configureApi("testHooks=1;token=s3cret");
        eng.initialise();
        eng.controlPass();
        local = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
        CHECK(local->ok());
    }
    // Sends `c` (with `longText` through submit_text when it is not empty) and passes the control pass until the result lands.
    testing::Outcome send(const FoxCommand& c, const std::string& longText = std::string(), testing::Session* who = nullptr) {
        testing::Session& s = who != nullptr ? *who : *local;
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = longText.empty() ? api()->submit(s.raw(), &c, 1, &sr)
                                           : api()->submit_text(s.raw(), &c, longText.data(), static_cast<uint32_t>(longText.size()), &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int i = 0; i < 60 && !o.landed; ++i) {
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
    // A fact, whole (a result can be several kilobytes).
    std::string fact(const std::string& key) {
        std::string buf(1 << 16, '\0');
        const int32_t n = api()->get_setting(local->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
        if (n < 0) { return "<absent>"; }
        return std::string(buf.c_str());
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t radio, double n0 = 0.0, long long i0 = 0) {
        FoxCommand c = testing::cmd(op, n0, i0);
        c.target = radio;
        return c;
    }
    bool untilFact(const std::string& key, const std::string& want, int ms = 8000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (fact(key) == want) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return fact(key) == want;
    }
    void passes(int n, int sleepMs = 5) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
};

}  // namespace wireb
