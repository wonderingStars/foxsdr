// test_engine_api_device_ledger.cpp - the device ledger in the real engine (multi-radio stage 2), through the C table and the engine's own passes, on a
// fake radio (no USB, no driver): the receiver holds a radio, a patch Radio node names it, and the engine says WHO HOLDS IT.
//
//   A  A NODE NAMING THE RECEIVER'S OPEN RADIO is refused while the patch is stopped, in words that name the holder (fact.patch.refusal.<node> =
//      "in use by Radio 1"); the ledger says the receiver holds the radio;
//   B  THE BORROW, THROUGH THE LEDGER: the patch runs, the receiver lends its radio to it (the receiver runs on the generator, as it always did), the
//      loan is on the ledger's books, the node is granted and the refusal is gone;
//   C  A SECOND NODE NAMING THE SAME RADIO under the other driver stack (the one serial) is refused and names the first node
//      ("in use by patch radio <id>") - the DeviceTwice rule, now with the holder in it - and is not opened;
//   D  THE PATCH STOPS: the receiver gets its radio back (the loan ends), and the first node is refused again, naming Radio 1.
//
// The ledger is the engine's own state (rebuilt each patch pass from what the engine holds), so a mutant that leaves it empty, or that never lends,
// turns A, B and D red (docs/multi-radio-records/engine-stages-1-3.md).
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
#include "core/patch_devices.hpp"
#include "core/patch_graph.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
namespace pc = cascade::core::patch;

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is.
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
    // A Radio node on the patch graph, switched off (so nothing is opened for it), naming `device`.
    static std::uint32_t addRadio(Engine& e, const std::string& device) {
        const pc::NodeId id = e.patchGraph_.addNode(pc::NodeKind::Radio, "Radio");
        pc::Node* n = e.patchGraph_.mutableNode(id);
        n->device = device;
        n->on = false;
        return id;
    }
    static void setPatchRunning(Engine& e, bool on) { e.patchRunning_ = on; }
    static bool patchMainKept(const Engine& e) { return e.patchMainKeep_.valid; }
    static cascade::engine::DeviceHolder holderOf(const Engine& e, const std::string& key) { return e.deviceLedger_.holderOf(key); }
    static bool receiverLent(const Engine& e, const std::string& key) {
        const auto l = e.deviceLedger_.lent(cascade::engine::DeviceHolder::receiver(0));
        return l.has_value() && *l == key;
    }
    static std::string refusal(const Engine& e, std::uint32_t node) {
        const auto it = e.patchLedgerRefusal_.find(node);
        return it == e.patchLedgerRefusal_.end() ? std::string() : it->second;
    }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;
using cascade::engine::DeviceHolder;

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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_device_ledger_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
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

// The fake radio open as the receiver's source.
bool openFakeRadioAsReceiver(Direct& d, testing::Session& s) {
    Access::installFakeRadio();
    if (pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status() != FOXAPI_OK) { return false; }
    const auto listed = [&] {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_DEVICES); ++i) {
            if (std::string(item(s, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i)).detail) == engine_api_test::kFakeRadioId) { return true; }
        }
        return false;
    };
    if (!passUntil(d.eng, listed)) { return false; }
    if (pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).status() != FOXAPI_OK) { return false; }
    return passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; });
}

std::string refusalFact(testing::Session& s, std::uint32_t node) { return fact(s, "patch.refusal." + std::to_string(node)); }

void testTheLedgerNamesWhoHoldsTheRadio() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(openFakeRadioAsReceiver(d, s));

    const std::string key = pc::makeDeviceKey(engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs);
    const std::string keyViaSoapy = pc::makeDeviceKey("soapy", std::string("driver=") + engine_api_test::kFakeRadioDriver + "," + engine_api_test::kFakeRadioArgs);
    CHECK(pc::sameDevice(key, keyViaSoapy));

    // A. The receiver holds the radio; a node that names it is refused, and told whom by.
    const std::uint32_t a = Access::addRadio(d.eng, key);
    passes(d.eng, 3);
    CHECK(Access::holderOf(d.eng, key) == DeviceHolder::receiver(0));
    CHECK_EQ(Access::refusal(d.eng, a), std::string("in use by Radio 1"));
    CHECK_EQ(refusalFact(s, a), std::string("in use by Radio 1"));

    // B. The patch runs: the receiver lends its radio (and runs on the generator), the node is granted.
    Access::setPatchRunning(d.eng, true);
    CHECK(passUntil(d.eng, [&] { return Access::patchMainKept(d.eng); }));
    passes(d.eng, 3);
    CHECK(Access::receiverLent(d.eng, key));
    CHECK(Access::holderOf(d.eng, key) == DeviceHolder::patchNode(a));
    CHECK_EQ(Access::refusal(d.eng, a), std::string());
    CHECK_EQ(refusalFact(s, a), std::string());

    // C. A second node naming it through the other driver stack: refused, naming the first node.
    const std::uint32_t b = Access::addRadio(d.eng, keyViaSoapy);
    passes(d.eng, 3);
    CHECK_EQ(Access::refusal(d.eng, b), "in use by patch radio " + std::to_string(a));
    CHECK_EQ(refusalFact(s, b), "in use by patch radio " + std::to_string(a));
    CHECK_EQ(Access::refusal(d.eng, a), std::string());
    CHECK(Access::holderOf(d.eng, keyViaSoapy) == DeviceHolder::patchNode(a));

    // D. The patch stops: the loan ends, the receiver opens its radio again, and the node is refused once more, naming Radio 1.
    Access::setPatchRunning(d.eng, false);
    CHECK(passUntil(d.eng, [&] { return !Access::patchMainKept(d.eng) && (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
    passes(d.eng, 3);
    CHECK(!Access::receiverLent(d.eng, key));
    CHECK(Access::holderOf(d.eng, key) == DeviceHolder::receiver(0));
    CHECK_EQ(Access::refusal(d.eng, a), std::string("in use by Radio 1"));
    CHECK_EQ(Access::refusal(d.eng, b), std::string("in use by Radio 1"));
}

}  // namespace

int main() {
    std::printf("test_engine_api_device_ledger\n");
    isolate();
    testTheLedgerNamesWhoHoldsTheRadio();
    Access::removeFakeRadio();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_device_ledger");
}
