// engine_client.hpp - a thin test-side client of the in-process API: open an
// engine, open a session, submit one command and wait for what landed.
// Everything goes through the C table; nothing here reaches inside the engine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "foxsdr_api.h"

namespace testing {

inline FoxCommand cmd(uint32_t op, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.num[0] = n0;
    c.ival[0] = i0;
    if (text != nullptr) {
        std::strncpy(c.text, text, sizeof(c.text) - 1);
    }
    return c;
}

struct Outcome {
    int32_t submitStatus = FOXAPI_FAILED;  // what submit() said
    bool landed = false;                   // a result arrived
    FoxCommandResult result{};
    // The engine's answer wherever it was given: the submit refusal if there
    // was one, else what applying it produced.
    int32_t status() const { return submitStatus != FOXAPI_OK ? submitStatus : result.status; }
};

class Session {
public:
    Session(const FoxEngineApi* api, FoxEngine* engine, uint32_t flags, uint64_t grants,
            const char* token = nullptr)
        : api_(api) {
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = flags;
        p.grants = grants;
        p.clientName = "test";
        p.clientVersion = "0";
        p.token = token;
        openStatus = api_->open_session(engine, &p, &s_);
    }
    ~Session() { close(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void close() {
        if (s_ != nullptr) {
            api_->close_session(s_);
            s_ = nullptr;
        }
    }

    FoxSession* raw() const { return s_; }
    bool ok() const { return openStatus == FOXAPI_OK && s_ != nullptr; }

    FoxReceiverState state() {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        for (int i = 0; i < 100; ++i) {
            if (api_->read_state(s_, &st) == FOXAPI_OK) {
                break;
            }
        }
        return st;
    }

    void beat() { api_->heartbeat(s_); }

    // Submits one command and waits up to timeoutMs for its result.
    Outcome run(const FoxCommand& c, int timeoutMs = 2000) {
        Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api_->submit(s_, &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) {
            return o;
        }
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            FoxCommandResult r[16];
            for (auto& x : r) {
                x.structSize = sizeof(x);
            }
            const int32_t got = api_->poll_results(s_, r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                } else {
                    stray_.push_back(r[i]);
                }
            }
            if (o.landed) {
                return o;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return o;
    }

    std::vector<FoxEvent> events() {
        std::vector<FoxEvent> out;
        FoxEvent e[32];
        while (true) {
            for (auto& x : e) {
                x.structSize = sizeof(x);
            }
            const int32_t n = api_->poll_events(s_, e, 32);
            if (n <= 0) {
                break;
            }
            out.insert(out.end(), e, e + n);
        }
        return out;
    }

    int32_t openStatus = FOXAPI_FAILED;

private:
    const FoxEngineApi* api_;
    FoxSession* s_ = nullptr;
    std::vector<FoxCommandResult> stray_;
};

class Engine {
public:
    Engine(const FoxEngineApi* api, const char* options = nullptr) : api_(api) {
        FoxEngineParams p{};
        p.structSize = sizeof(p);
        p.options = options;
        createStatus = api_->create(&p, &e_);
    }
    ~Engine() {
        if (e_ != nullptr) {
            api_->destroy(e_);
        }
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    FoxEngine* raw() const { return e_; }
    int32_t createStatus = FOXAPI_FAILED;

private:
    const FoxEngineApi* api_;
    FoxEngine* e_ = nullptr;
};

constexpr uint64_t kAllGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS |
                                FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_ADMIN |
                                FOXAPI_GRANT_AUDIO | FOXAPI_GRANT_IQ;

}  // namespace testing
