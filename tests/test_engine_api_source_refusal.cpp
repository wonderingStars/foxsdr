// test_engine_api_source_refusal.cpp - A REFUSED RADIO CHOICE SAYS WHY (0.99.113), in the real engine through the C table and its own passes, on a fake radio.
//
// Choosing a radio the device ledger says somebody holds (the owner's case: a Radio part of a saved, STOPPED patch names his B200) is refused FOXAPI_BUSY "in use by
// patch radio N". Before this change nothing was shown and nothing logged. Now (WHO MAY HOLD A RADIO is not touched: the radio is still refused):
//
//   A  A PATCH PART HOLDS IT: still BUSY with the API's unchanged words, the answer carries the holder as data (applied[0] = 2, applied[1] = the part), the Source error
//      line (fact.status.sourceError, which both windows and remote clients read) carries the sentence naming the part, and one log line says what was refused and
//      who holds the radio, with the serial as its last four characters only.
//   B  ANOTHER RECEIVER HOLDS IT (Radio 2): the receiver sentence; applied[0] = 1, applied[1] = 2.
//   C  THE LINE CLEARS on the next accepted choice that changes the source (the I/Q file row), by the clearing rule that was there already.
//   D  NOTHING IS COUNTED OR ASKED: no failed open is counted (the engine's own counter, the radio reporter's queue), and the hand-off question (fact.radio.handoff)
//      is not raised - the radio never failed to open, the product answered correctly about the user's own other radio.
//   E  RADIO_ADD for a device a patch part names: BUSY as before (words unchanged) and the same data on the answer; nothing is added.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/patch_devices.hpp"
#include "core/patch_graph.hpp"
#include "core/radio_refusal.hpp"
#include "core/radio_reporter.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
namespace pc = cascade::core::patch;

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installFakeRadio() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    static void removeFakeRadio() {
        Engine::testHooks_.makeDevice = nullptr;
        Engine::testHooks_.nativeScan = nullptr;
        Engine::testHooks_.soapyScan = nullptr;
    }
    // A Radio node on the patch graph, switched off and the patch stopped (the owner's saved, stopped patch), naming `device`.
    static std::uint32_t addRadio(Engine& e, const std::string& device, const std::string& caption = "Radio") {
        const pc::NodeId id = e.patchGraph_.addNode(pc::NodeKind::Radio, caption);
        pc::Node* n = e.patchGraph_.mutableNode(id);
        n->device = device;
        n->on = false;
        return id;
    }
    static std::uint64_t openFailures(const Engine& e) { return e.radioOpenFailures_; }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kLocalGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

fs::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_source_refusal_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

std::string fact(testing::Session& s, const std::string& key) {
    char buf[4096];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
}

template <class F>
bool passUntil(Engine& eng, F done, int timeoutMs = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void passes(Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
}

testing::Outcome pumped(Engine& eng, testing::Session& s, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}

struct Direct {
    Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};

// The fake radio scanned and listed (so a choice can name it); nothing opened.
bool scanFakeRadio(Direct& d, testing::Session& s) {
    Access::installFakeRadio();
    if (pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status() != FOXAPI_OK) { return false; }
    return passUntil(d.eng, [&] {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_DEVICES); ++i) {
            if (std::string(item(s, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i)).detail) == engine_api_test::kFakeRadioId) { return true; }
        }
        return false;
    });
}

bool logHas(const std::string& needle) {
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { return true; }
    }
    return false;
}
int logCount(const std::string& needle) {
    int n = 0;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) { n += line.find(needle) != std::string::npos ? 1 : 0; }
    return n;
}

const std::string kKey = pc::makeDeviceKey(engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs);

std::string partSentence(const std::string& caption) {
    return "This radio is set as the radio of the part \"" + caption +
           "\" on the Patch page. Change or remove that part, or start the patch to use the radio there.";
}

// A, C, D: the owner's case.
void testAPatchPartHoldsTheRadio() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addRadio(d.eng, kKey, "Aerial B200");
    passes(d.eng, 3);
    const std::uint64_t failsBefore = Access::openFailures(d.eng);
    const auto queuedBefore = cascade::core::radioreport::global().stats().failuresQueued;
    const int refusalsBefore = logCount("was refused: the radio is held by");

    const testing::Outcome o = pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId));
    CHECK(o.landed);
    // the API answer, its words unchanged
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK((o.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK_EQ(std::string(o.result.message), "in use by patch radio " + std::to_string(part));
    // who holds it, as data
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), static_cast<int>(part));
    // the sentence on the Source error line, and still there after the passes that follow (nothing overwrites it)
    passes(d.eng, 5);
    CHECK_EQ(fact(s, "status.sourceError"), partSentence("Aerial B200"));
    // still refused: the radio is not opened
    CHECK((s.state().flags & FOXAPI_RX_DEVICE_OPEN) == 0u);
    // one log line, naming the holder and only the last four of the serial
    CHECK_EQ(logCount("was refused: the radio is held by") - refusalsBefore, 1);
    CHECK(logHas("held by patch radio " + std::to_string(part)));
    CHECK(logHas("(serial ending 0001)"));
    CHECK(!logHas("00000001"));
    // D: nothing counted, nothing filed, nothing asked
    CHECK_EQ(Access::openFailures(d.eng), failsBefore);
    CHECK_EQ(cascade::core::radioreport::global().stats().failuresQueued, queuedBefore);
    CHECK_EQ(fact(s, "radio.handoff"), std::string());
    CHECK_EQ(fact(s, "radio.handoffMoved"), std::string());

    // C: the next ACCEPTED choice that changes the source (the I/Q file row; the generator is what this radio is on, and picking the row a radio is on is a no-op that
    // clears nothing - the engine's own rule, unchanged) clears the line, as it clears any other.
    const testing::Outcome ok = pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "row:iqfile"));
    CHECK_EQ(ok.result.status, static_cast<int32_t>(FOXAPI_OK));
    passes(d.eng, 5);
    CHECK_EQ(fact(s, "status.sourceError"), std::string());
}

// B: another receiver holds it.
void testAnotherReceiverHoldsTheRadio() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const testing::Outcome add = pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_ADD, 0, 2, engine_api_test::kFakeRadioId));
    CHECK_EQ(add.result.status, static_cast<int32_t>(FOXAPI_OK));
    passes(d.eng, 3);
    const std::uint64_t failsBefore = Access::openFailures(d.eng);

    const testing::Outcome o = pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), std::string("in use by Radio 2"));
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 1);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 2);
    passes(d.eng, 5);
    CHECK_EQ(fact(s, "status.sourceError"), std::string("This radio is in use by Radio 2. Choose another radio there first."));
    CHECK(logHas("held by Radio 2"));
    CHECK_EQ(Access::openFailures(d.eng), failsBefore);
    CHECK_EQ(fact(s, "radio.handoff"), std::string());
}

// E: an added radio for a device a part names.
void testRadioAddForAPartsDevice() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addRadio(d.eng, kKey, "Aerial B200");
    passes(d.eng, 3);
    const testing::Outcome o = pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_ADD, 0, 2, engine_api_test::kFakeRadioId));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), "in use by patch radio " + std::to_string(part));
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), static_cast<int>(part));
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));   // nothing was added
}

}  // namespace

int main() {
    std::printf("test_engine_api_source_refusal\n");
    isolate();
    testAPatchPartHoldsTheRadio();
    testAnotherReceiverHoldsTheRadio();
    testRadioAddForAPartsDevice();
    Access::removeFakeRadio();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_source_refusal");
}
