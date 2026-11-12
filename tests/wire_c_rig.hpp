// wire_c_rig.hpp - the engine test rig of wiring lane C: a real Engine on a scratch per-user folder, one LOCAL session through the C table, ops sent and
// answered on the control pass, facts read back. Header-only; included by every test_engine_api_wire_c_*.cpp (one TU each, so the friend struct below is
// defined once per program).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void useFolder(Engine& e, const std::string& path) { e.wireCFolderForTest_ = path; }
    static cascade::engine::wirec::State& wirec(Engine& e) { return e.wireC(); }
};
}  // namespace cascade::gui

namespace wirec_test {

namespace fs = std::filesystem;
using Access = cascade::gui::AppWindowTestAccess;

constexpr const char* kHooks = "testHooks=1;token=s3cret";
inline const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

inline fs::path scratch(const char* tag) {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    fs::path p = fs::temp_directory_path() / ("foxsdr_wc_" + std::string(tag) + "_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

inline std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Rig {
    cascade::engine::Engine eng;
    std::unique_ptr<testing::Session> local;
    explicit Rig(const fs::path& folder) {
        Access::useFolder(eng, folder.string());
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
        local = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
        CHECK(local->ok());
    }
    void pass(int n = 3) {
        for (int i = 0; i < n; ++i) { eng.controlPass(); }
    }
    testing::Outcome send(const FoxCommand& c) { return sendOn(*local, c); }
    testing::Outcome sendOn(testing::Session& session, const FoxCommand& c) {
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(session.raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int i = 0; i < 40 && !o.landed; ++i) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(session.raw(), r, 16);
            for (int32_t k = 0; k < got; ++k) {
                if (r[k].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[k];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[16384];
        const int32_t n = api()->get_setting(local->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    // The status the engine answered one op with (a refusal's status or OK), the sentence beside it in `msg`.
    std::int32_t status(const FoxCommand& c, std::string* msg = nullptr) {
        const testing::Outcome o = send(c);
        if (msg != nullptr) { *msg = o.landed ? std::string(o.result.message) : std::string("<no answer>"); }
        if (o.submitStatus != FOXAPI_OK) { return o.submitStatus; }
        return o.landed ? o.result.status : -9999;
    }
};

inline FoxCommand textOp(std::uint32_t op, const std::string& text) { return testing::cmd(op, 0.0, 0, text.c_str()); }
inline FoxCommand setting(const std::string& key, const std::string& value) { return textOp(FOXAPP_OP_WIREC_SETTING, key + "\x1F" + value); }

}  // namespace wirec_test
