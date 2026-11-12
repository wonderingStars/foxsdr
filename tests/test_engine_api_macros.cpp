// test_engine_api_macros.cpp - macros in the engine (engine wiring W2; src/engine/engine_macros.cpp, core/macros.hpp), held against the real engine
// through the C table and its own passes, with a clock the test owns and a macro file in a scratch folder.
//
//   M1  a macro file is read when anything asks: MACRO_RELOAD, then the facts name the macros; a file with one bad step is refused WHOLE (the words
//       name the macro and step) and NO macro is available; a missing file is no macros and no error
//   M2  MACRO_RUN plays the steps on the control pass at the steps' times, in order, each a real engine command (the tuned frequency and the mode
//       change); BUSY while one runs; NOT_FOUND for an unknown name; BAD_ARGUMENT for an empty one; MACRO_STOP ends it (NO_CHANGE when none runs)
//   M3  a step the engine refuses ends the run with the refusal's words in fact.macro.last, and later steps do not run
//   M4  what a macro may not do: a file naming a transmit command, a plugin grant or another macro op is refused whole at load
//   M5  a REMOTE session cannot send a macro op ("the application's own commands are for sessions on this machine")
//   M6  RUN_MACRO (the model's pseudo-step) runs a nested macro and the nesting is bounded
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

namespace fs = std::filesystem;

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void useFile(Engine& e, const std::string& path) { e.macrosPathForTest_ = path; }
    static void clock(Engine& e, std::int64_t ms) { e.macroClockMs_.store(ms, std::memory_order_relaxed); }
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

fs::path g_dir;

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

struct Rig {
    cascade::engine::Engine eng;
    std::unique_ptr<testing::Session> local;
    Rig(const fs::path& file) {
        Access::useFile(eng, file.string());
        Access::clock(eng, 1000);
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
        local = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
        CHECK(local->ok());
    }
    void pass(int n = 3) {
        for (int i = 0; i < n; ++i) { eng.controlPass(); }
    }
    // One op, applied: the status the engine answered with.
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
        char buf[4096];
        const int32_t n = api()->get_setting(local->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    double tunedHz() { return local->state().tunedHz; }
};

FoxCommand op(std::uint32_t o, const char* text = nullptr) { return testing::cmd(o, 0.0, 0, text); }

const char* kGood =
    "{\"version\":1,\"macros\":["
    "{\"name\":\"go-airband\",\"description\":\"tune and set the mode\",\"steps\":["
    "{\"cmd\":\"SET_FREQUENCY\",\"num\":[118300000],\"delayMs\":1000},"
    "{\"cmd\":\"SET_MODE\",\"ival\":[2],\"delayMs\":0}]},"
    "{\"name\":\"two-step\",\"steps\":["
    "{\"cmd\":\"SET_FREQUENCY\",\"num\":[100100000],\"delayMs\":500},"
    "{\"cmd\":\"SET_FREQUENCY\",\"num\":[100200000],\"delayMs\":0}]},"
    "{\"name\":\"outer\",\"steps\":[{\"cmd\":\"RUN_MACRO\",\"text\":\"two-step\",\"delayMs\":0}]}"
    "]}";

void testLoad() {
    std::printf("M1: the macro file is read when asked; a bad step refuses it whole\n");
    const fs::path file = g_dir / "m1.json";
    writeFile(file, kGood);
    {
        Rig r(file);
        CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RELOAD)).status(), FOXAPI_OK);
        const std::string names = r.fact("macro.names");
        CHECK(names.find("go-airband") != std::string::npos && names.find("two-step") != std::string::npos);
        CHECK_EQ(r.fact("macro.error"), std::string());
    }
    // a file with one bad step (an unknown command): refused whole, no macro available, the words name the macro and step
    writeFile(file, "{\"version\":1,\"macros\":[{\"name\":\"ok\",\"steps\":[{\"cmd\":\"SET_FREQUENCY\",\"num\":[100000000]}]},"
                    "{\"name\":\"bad\",\"steps\":[{\"cmd\":\"SET_FREQUENCY\",\"num\":[100000000]},{\"cmd\":\"NO_SUCH_COMMAND\"}]}]}");
    {
        Rig r(file);
        const testing::Outcome o = r.send(op(FOXAPP_OP_MACRO_RELOAD));
        CHECK_EQ(o.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(r.fact("macro.error").find("bad") != std::string::npos);
        CHECK_EQ(r.fact("macro.names"), std::string());
        CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "ok")).status(), FOXAPI_BAD_ARGUMENT);   // not even the good one
    }
    // a missing file is no macros and no error
    {
        Rig r(g_dir / "no-such-file.json");
        CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RELOAD)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("macro.names"), std::string());
        CHECK_EQ(r.fact("macro.error"), std::string());
        CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "x")).status(), FOXAPI_NOT_FOUND);
    }
}

void testPlay() {
    std::printf("M2: a macro plays its steps at their times as real commands\n");
    const fs::path file = g_dir / "m2.json";
    writeFile(file, kGood);
    Rig r(file);
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "nonesuch")).status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_STOP)).status(), FOXAPI_NO_CHANGE);   // none runs
    Access::clock(r.eng, 10000);
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "go-airband")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("macro.running"), std::string("go-airband"));
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "two-step")).status(), FOXAPI_BUSY);
    r.pass();
    CHECK(std::abs(r.tunedHz() - 118300000.0) < 1.0);   // the first step is due at once
    const std::string stepsAfterFirst = r.fact("macro.steps");
    CHECK_EQ(stepsAfterFirst, std::string("1"));
    Access::clock(r.eng, 10500);   // the second step is due a second after the first: not yet
    r.pass();
    CHECK_EQ(r.fact("macro.steps"), std::string("1"));
    Access::clock(r.eng, 11000);
    r.pass();
    CHECK_EQ(r.fact("macro.steps"), std::string("2"));
    CHECK_EQ(r.local->state().demodMode, 2u);   // SET_MODE ival[0] = 2 applied
    CHECK_EQ(r.fact("macro.running"), std::string());
    CHECK_EQ(r.fact("macro.last"), std::string("finished"));
    // STOP in mid-run ends it and later steps do not run
    Access::clock(r.eng, 20000);
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "two-step")).status(), FOXAPI_OK);
    r.pass();
    CHECK(std::abs(r.tunedHz() - 100100000.0) < 1.0);
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_STOP)).status(), FOXAPI_OK);
    Access::clock(r.eng, 30000);
    r.pass();
    CHECK(std::abs(r.tunedHz() - 100100000.0) < 1.0);   // the second step never ran
    CHECK_EQ(r.fact("macro.last"), std::string("stopped"));
}

void testRefusedStep() {
    std::printf("M3: a refused step ends the run with its words\n");
    const fs::path file = g_dir / "m3.json";
    // the second step asks for a frequency no tuner has (NaN is refused for every client); the third must not run
    writeFile(file, "{\"version\":1,\"macros\":[{\"name\":\"fails\",\"steps\":["
                    "{\"cmd\":\"SET_FREQUENCY\",\"num\":[100100000],\"delayMs\":0},"
                    "{\"cmd\":\"SET_MODE\",\"ival\":[99],\"delayMs\":0},"
                    "{\"cmd\":\"SET_FREQUENCY\",\"num\":[100500000],\"delayMs\":0}]}]}");
    Rig r(file);
    Access::clock(r.eng, 5000);
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "fails")).status(), FOXAPI_OK);
    r.pass();
    CHECK(r.fact("macro.last").find("stopped: step 2 of 'fails' was refused") == 0u);
    CHECK(std::abs(r.tunedHz() - 100100000.0) < 1.0);   // the first step ran, the third did not
    CHECK_EQ(r.fact("macro.running"), std::string());
}

void testDenied() {
    std::printf("M4: what a macro may not name is refused at load\n");
    const fs::path file = g_dir / "m4.json";
    for (const char* cmdName : {"TX_PTT", "APP_PLUGIN_TX_GRANT", "APP_MACRO_RUN", "SERVER_CONFIG", "STORE_INSTALL"}) {
        writeFile(file, std::string("{\"version\":1,\"macros\":[{\"name\":\"m\",\"steps\":[{\"cmd\":\"") + cmdName + "\"}]}]}");
        Rig r(file);
        const testing::Outcome o = r.send(op(FOXAPP_OP_MACRO_RELOAD));
        if (o.status() != FOXAPI_BAD_ARGUMENT) { std::printf("  %s was not refused (status %d)\n", cmdName, static_cast<int>(o.status())); }
        CHECK_EQ(o.status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.fact("macro.names"), std::string());
    }
}

void testRemote() {
    std::printf("M5: a REMOTE session cannot send a macro op\n");
    const fs::path file = g_dir / "m5.json";
    writeFile(file, kGood);
    Rig r(file);
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    for (const std::uint32_t o : {FOXAPP_OP_MACRO_RUN, FOXAPP_OP_MACRO_STOP, FOXAPP_OP_MACRO_RELOAD}) {
        const testing::Outcome out = r.sendOn(remote, op(o, "go-airband"));
        CHECK_EQ(out.status(), FOXAPI_DENIED);   // the refusal is the engine's result for the command, not the submit's status
    }
    CHECK_EQ(r.fact("macro.running"), std::string());
}

void testNested() {
    std::printf("M6: RUN_MACRO runs a nested macro\n");
    const fs::path file = g_dir / "m6.json";
    writeFile(file, kGood);
    Rig r(file);
    Access::clock(r.eng, 7000);
    CHECK_EQ(r.send(op(FOXAPP_OP_MACRO_RUN, "outer")).status(), FOXAPI_OK);
    r.pass();
    CHECK(std::abs(r.tunedHz() - 100100000.0) < 1.0);
    Access::clock(r.eng, 7500);
    r.pass();
    CHECK(std::abs(r.tunedHz() - 100200000.0) < 1.0);
    CHECK_EQ(r.fact("macro.last"), std::string("finished"));
}

}  // namespace

int main() {
    engine_api_test::hermetic("macros");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    g_dir = fs::temp_directory_path() / ("foxsdr_macros_" + std::to_string(_getpid()));
#else
    g_dir = fs::temp_directory_path() / ("foxsdr_macros_" + std::to_string(getpid()));
#endif
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    testLoad();
    testPlay();
    testRefusedStep();
    testDenied();
    testRemote();
    testNested();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_macros");
}
