// test_engine_api_radio_holder.cpp - A STOPPED PATCH PART NO LONGER HOLDS A RADIO (0.99.114), in the real engine through the C table and its own passes, on a fake radio
// (no USB, no driver). The review that fixed the shape of this change is D:/foxsdr-build/prep/REVIEW-radio-holder-APPROACH-2026-11-24.md.
//
// THE RULE. A patch Radio part that is not OPEN (the patch is stopped, or the part's switch is off) only names a radio: a RECEIVER may take it (Radio 1's source choice,
// RADIO_ADD, a re-point of Radio 2 to 8). A part that is open still holds it against a receiver, in the same words as before; "named twice" between two parts, and a
// receiver already holding the radio against a part, are untouched.
//
// THE THREE RACES that rule exposes, each closed and each pinned:
//   R1  "open" is what is TRUE, not the switch: STOP (or a part switched off) and a radio choice landing in one pass, while the patch radio is still open, is refused;
//   R2  a receiver's open that is still on its worker (Radio 1's, which claimed nothing) is on the books: START while it opens does not open the part beside it;
//   R3  an open still in flight keeps its hold after the one who asked for it let go: a patch open handed to a detached thread by STOP, and an open the receiver set
//       aside for another.
//
//   9  radioAddWithAStoppedPatchNode                  10 radioAddWithARunningPatchNode           11 stopAndChooseInOnePass
//   12 chooseAndStartInOnePass                        13 startWhileTheReceiverIsStillOpening     14 stopWithAPartStillOpeningThenChoose
//   15 radio2HoldsItThenPatchStarts                   16 radio1HoldsItThenPatchStarts            17 partSwitchedOffWhileRunningThenRadio2Takes
//   18 indexKeys                                      19 twoDonglesOneDefaultSerial              20 handBackPendingStillHolds
//   21 radioAddPlutoWithStoppedNode + openPlutoIsNotAskedOfTheLedger   (22 transmitOpenUnaffected: the ledger test of the transmitter entry, and the transmit path never
//      asks the ledger; 23 existingABCDStillHold: tests/test_engine_api_device_ledger.cpp, unchanged)
//   and: aStoppedPartDoesNotRefuseTheChoice (the owner's B200), aRetiredOpenKeepsItsHold (R3, the receiver's own set-aside open), radio1DuringARunIsStillRefusedForAnOffPart.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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
    // A Radio part on the patch graph naming `device`, switched on or off.
    static std::uint32_t addPart(Engine& e, const std::string& device, bool on, const std::string& caption = "Radio") {
        const pc::NodeId id = e.patchGraph_.addNode(pc::NodeKind::Radio, caption);
        pc::Node* n = e.patchGraph_.mutableNode(id);
        n->device = device;
        n->on = on;
        return id;
    }
    static void setPartOn(Engine& e, std::uint32_t id, bool on) {
        if (pc::Node* n = e.patchGraph_.mutableNode(id)) { n->on = on; }
    }
    static void setPatchRunning(Engine& e, bool on) { e.patchRunning_ = on; }
    static std::size_t patchRadiosOpen(const Engine& e) { return e.patchRadios_.size(); }
    static std::size_t patchOpensPending(const Engine& e) { return e.patchRadioPending_.size(); }
    static std::string patchError(const Engine& e, std::uint32_t id) {
        const auto it = e.patchRadioError_.find(id);
        return it == e.patchRadioError_.end() ? std::string() : it->second;
    }
    static bool patchMainKept(const Engine& e) { return e.patchMainKeep_.valid; }
    static std::string refusal(const Engine& e, std::uint32_t node) {
        const auto it = e.patchLedgerRefusal_.find(node);
        return it == e.patchLedgerRefusal_.end() ? std::string() : it->second;
    }
    static std::uint64_t openFailures(const Engine& e) { return e.radioOpenFailures_; }
    static bool receiverHasDevice(const Engine& e, std::size_t slot) { return e.receivers_[slot] != nullptr && e.receivers_[slot]->device_ != nullptr; }
    static void setRecordDir(Engine& e, const std::string& dir) { e.recordDir_ = dir; }
    // An open of the first radio on its worker, as a source pick makes it (launchDeviceOpen); a second one while the first is pending sets the first aside.
    static void launchOpen(Engine& e, const std::string& kind, const std::string& args) {
        cascade::engine::DeviceOpenResult r;
        r.kind = kind;
        r.args = args;
        e.launchDeviceOpen(std::move(r), "test radio");
    }
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_radio_holder_" + std::to_string(pid));
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

// The command goes in and ONE pass runs: the answer is whatever that one pass gave (a command is applied at the head of the pass, before the patch's own work).
testing::Outcome onePass(Engine& eng, testing::Session& s, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
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
    return o;
}

// Several commands go in, in this order, and ONE pass applies them all (at its head, in order) before the patch's own work: what a window does when two things are
// pressed between two passes. The answers come back in the same order.
std::vector<testing::Outcome> onePassOf(Engine& eng, testing::Session& s, const std::vector<FoxCommand>& cmds) {
    std::vector<testing::Outcome> out(cmds.size());
    std::vector<std::uint64_t> tickets(cmds.size(), 0);
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s.raw(), &cmds[i], 1, &sr);
        out[i].submitStatus = n < 0 ? n : sr.status;
        tickets[i] = sr.ticket;
    }
    eng.controlPass();
    FoxCommandResult r[32];
    for (auto& x : r) { x.structSize = sizeof(x); }
    const int32_t got = api()->poll_results(s.raw(), r, 32);
    for (int32_t k = 0; k < got; ++k) {
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            if (r[k].ticket == tickets[i]) {
                out[i].landed = true;
                out[i].result = r[k];
            }
        }
    }
    return out;
}

struct Direct {
    Engine eng;
    Direct() {
        engine_api_test::resetFakeRadioCounters();
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};

const std::string kKey = pc::makeDeviceKey(engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs);

FoxCommand selectFake() { return testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId); }
FoxCommand addRadioCmd(long long radio, const char* device) {
    FoxCommand c = testing::cmd(FOXAPI_OP_RADIO_ADD, 0, radio, device);
    c.ival[1] = 1;   // started
    return c;
}

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

FoxCommand patchRun(bool on) { return testing::cmd(FOXAPI_OP_PATCH_RUN, 0, on ? 1 : 0); }

bool radio1Open(testing::Session& s) { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }
bool radio1Opening(testing::Session& s) { return (s.state().flags & FOXAPI_RX_DEVICE_OPENING) != 0u; }

bool openFakeRadioAsReceiver(Direct& d, testing::Session& s) {
    if (pumped(d.eng, s, selectFake()).status() != FOXAPI_OK) { return false; }
    return passUntil(d.eng, [&] { return radio1Open(s); });
}

FoxReceiverState readRadio(testing::Session& s, std::uint32_t radio) {
    FoxReceiverState st{};
    st.structSize = sizeof(st);
    (void)api()->read_radio_state(s.raw(), radio, &st);
    return st;
}

// A part switched on, the patch started, and the part's radio open (the fake radio, through the engine's own hook).
bool startPartAndWaitForItsRadio(Direct& d, std::uint32_t part) {
    (void)part;
    Access::setPatchRunning(d.eng, true);
    return passUntil(d.eng, [&] { return Access::patchRadiosOpen(d.eng) == 1u; });
}

// The part has no ERROR of its own: the fake radio coerces the rate the patch asks for and the node says so ("Sample rate coerced"), which is not a failure to open.
bool partHasNoOpenError(Engine& e, std::uint32_t part) {
    const std::string err = Access::patchError(e, part);
    return err.empty() || err.find("coerced") != std::string::npos;
}

std::string partWords(std::uint32_t part) { return "in use by patch radio " + std::to_string(part); }

// ---- 9 and the owner's case --------------------------------------------------------------------------------------------------------------------

void testAStoppedPartDoesNotRefuseTheChoice() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    // the owner's saved, stopped patch: a part naming his radio, switched off (and the same with the switch on, the patch being stopped)
    const std::uint32_t off = Access::addPart(d.eng, kKey, false, "Aerial B200");
    const std::uint32_t on = Access::addPart(d.eng, pc::makeDeviceKey("soapy", std::string("driver=rtlsdr,") + engine_api_test::kFakeRadioArgs), true, "Second");
    passes(d.eng, 3);
    const std::uint64_t failsBefore = Access::openFailures(d.eng);
    const testing::Outcome o = pumped(d.eng, s, selectFake());
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_OK));   // BUSY "in use by patch radio N" before this change
    CHECK(passUntil(d.eng, [&] { return radio1Open(s); }));
    passes(d.eng, 5);
    CHECK(fact(s, "status.sourceError").find("Patch page") == std::string::npos);   // (the fake radio's own "sample rate coerced" note may be there: it is not a refusal)
    CHECK_EQ(Access::openFailures(d.eng), failsBefore);
    // the parts now say who has the radio
    CHECK_EQ(Access::refusal(d.eng, off), std::string("in use by Radio 1"));
    CHECK_EQ(Access::refusal(d.eng, on), std::string("in use by Radio 1"));
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
}

void testRadioAddWithAStoppedPatchNode() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, kKey, false);
    passes(d.eng, 3);
    const testing::Outcome o = pumped(d.eng, s, addRadioCmd(2, engine_api_test::kFakeRadioId));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_OK));   // BUSY before
    CHECK(passUntil(d.eng, [&] { return Access::receiverHasDevice(d.eng, 1); }));
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.count"), std::string("2"));
    CHECK_EQ(Access::refusal(d.eng, part), std::string("in use by Radio 2"));
}

void testRadioAddWithARunningPatchNode() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    CHECK(startPartAndWaitForItsRadio(d, part));
    const testing::Outcome o = pumped(d.eng, s, addRadioCmd(2, engine_api_test::kFakeRadioId));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), partWords(part));
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), static_cast<int>(part));
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));
    CHECK_EQ(Access::patchRadiosOpen(d.eng), std::size_t{1});   // the running part is untouched
}

// ---- R1 ----------------------------------------------------------------------------------------------------------------------------------------

void testStopAndChooseInOnePass() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    CHECK(startPartAndWaitForItsRadio(d, part));
    // STOP and the choice land in ONE pass: the patch radio is still open when the choice is judged (the pass closes it later), so the choice is refused...
    const std::vector<testing::Outcome> both = onePassOf(d.eng, s, {patchRun(false), selectFake()});
    CHECK(both[0].landed && both[1].landed);
    CHECK_EQ(both[0].result.status, static_cast<int32_t>(FOXAPI_OK));
    const testing::Outcome o = both[1];
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), partWords(part));
    // ...the patch radio closes, and the same choice a pass later is accepted
    CHECK(passUntil(d.eng, [&] { return Access::patchRadiosOpen(d.eng) == 0u; }));
    passes(d.eng, 3);
    const testing::Outcome again = pumped(d.eng, s, selectFake());
    CHECK_EQ(again.result.status, static_cast<int32_t>(FOXAPI_OK));
    CHECK(passUntil(d.eng, [&] { return radio1Open(s); }));
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);   // never two opens of the one radio at once
}

void testPartSwitchedOffAndChooseInOnePass() {
    // the same race by the other door: the part's switch goes off (the patch keeps running), and a choice for its radio lands in the same pass
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    CHECK(startPartAndWaitForItsRadio(d, part));
    CHECK(pumped(d.eng, s, addRadioCmd(2, nullptr)).status() == FOXAPI_OK);   // a test radio on the generator: the choice below is Radio 2's
    Access::setPartOn(d.eng, part, false);
    FoxCommand c = selectFake();
    c.target = 2;
    const testing::Outcome o = onePass(d.eng, s, c);
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK(passUntil(d.eng, [&] { return Access::patchRadiosOpen(d.eng) == 0u; }));
    passes(d.eng, 3);
    c = selectFake();
    c.target = 2;
    CHECK_EQ(pumped(d.eng, s, c).result.status, static_cast<int32_t>(FOXAPI_OK));
    CHECK(passUntil(d.eng, [&] { return Access::receiverHasDevice(d.eng, 1); }));
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
}

// ---- R2 ----------------------------------------------------------------------------------------------------------------------------------------

void testChooseAndStartInOnePass() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    engine_api_test::fakeRadioOpenDelayMs() = 300;   // a B200 takes seconds to open
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);   // switched on, the patch stopped
    passes(d.eng, 3);
    const std::uint64_t failsBefore = Access::openFailures(d.eng);
    // the choice and START in one pass: the choice is accepted (the part is not open), the receiver's open is on its worker, and the part must wait for it
    const std::vector<testing::Outcome> both = onePassOf(d.eng, s, {selectFake(), patchRun(true)});
    CHECK(both[0].landed && both[1].landed);
    CHECK_EQ(both[0].result.status, static_cast<int32_t>(FOXAPI_OK));
    CHECK_EQ(both[1].result.status, static_cast<int32_t>(FOXAPI_OK));
    CHECK_EQ(Access::patchRadiosOpen(d.eng), std::size_t{0});
    CHECK_EQ(Access::patchOpensPending(d.eng), std::size_t{0});   // the part was not given a worker beside the receiver's
    CHECK_EQ(Access::refusal(d.eng, part), std::string("in use by Radio 1"));
    // afterwards the loan runs the part: the receiver's open lands, the receiver hands the radio over, the part opens it
    CHECK(passUntil(d.eng, [&] { return Access::patchRadiosOpen(d.eng) == 1u && Access::patchMainKept(d.eng); }));
    passes(d.eng, 3);
    CHECK(partHasNoOpenError(d.eng, part));
    CHECK_EQ(Access::refusal(d.eng, part), std::string());
    CHECK_EQ(Access::openFailures(d.eng), failsBefore);   // no failed-open report for a part that only had to wait
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
}

void testStartWhileTheReceiverIsStillOpening() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    engine_api_test::fakeRadioOpenDelayMs() = 600;
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    passes(d.eng, 3);
    const std::uint64_t failsBefore = Access::openFailures(d.eng);
    CHECK_EQ(pumped(d.eng, s, selectFake()).result.status, static_cast<int32_t>(FOXAPI_OK));
    CHECK(passUntil(d.eng, [&] { return radio1Opening(s); }, 2000));
    // START while the receiver's open is on its worker
    CHECK_EQ(pumped(d.eng, s, patchRun(true)).result.status, static_cast<int32_t>(FOXAPI_OK));
    passes(d.eng, 6);
    CHECK(radio1Opening(s));   // (the open really was still in flight: the check below means something)
    CHECK_EQ(Access::patchRadiosOpen(d.eng), std::size_t{0});
    CHECK_EQ(Access::patchOpensPending(d.eng), std::size_t{0});
    CHECK_EQ(Access::refusal(d.eng, part), std::string("in use by Radio 1"));
    CHECK(passUntil(d.eng, [&] { return Access::patchRadiosOpen(d.eng) == 1u && Access::patchMainKept(d.eng); }));
    passes(d.eng, 3);
    CHECK(partHasNoOpenError(d.eng, part));
    CHECK_EQ(Access::openFailures(d.eng), failsBefore);
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
}

void testHandBackPendingStillHolds() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    engine_api_test::fakeRadioOpenDelayMs() = 400;
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    passes(d.eng, 3);
    CHECK(openFakeRadioAsReceiver(d, s));            // Radio 1 has the radio (the part is not open)
    Access::setPatchRunning(d.eng, true);            // START: Radio 1 lends it, the part opens it
    CHECK(passUntil(d.eng, [&] { return Access::patchRadiosOpen(d.eng) == 1u && Access::patchMainKept(d.eng); }));
    Access::setPatchRunning(d.eng, false);           // STOP: the part closes, Radio 1 gets its radio back through an open on a worker
    CHECK(passUntil(d.eng, [&] { return radio1Opening(s); }, 4000));
    // during that hand-back open the radio is Radio 1's, and Radio 2 may not have it
    const testing::Outcome o = pumped(d.eng, s, addRadioCmd(2, engine_api_test::kFakeRadioId));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), std::string("in use by Radio 1"));
    CHECK(passUntil(d.eng, [&] { return radio1Open(s); }));
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
    (void)part;
}

// ---- R3 ----------------------------------------------------------------------------------------------------------------------------------------

void testStopWithAPartStillOpeningThenChoose() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    engine_api_test::fakeRadioOpenDelayMs() = 1500;
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    Access::setPatchRunning(d.eng, true);
    CHECK(passUntil(d.eng, [&] { return Access::patchOpensPending(d.eng) == 1u; }, 3000));   // the part's open is on its worker
    // STOP: the open is handed to a detached thread and forgotten by the patch
    Access::setPatchRunning(d.eng, false);
    passes(d.eng, 4);
    CHECK_EQ(Access::patchOpensPending(d.eng), std::size_t{0});
    CHECK_EQ(engine_api_test::fakeRadioLive().load(), 0);   // (the worker has not opened it yet: the open is a second and a half)
    // the choice now would be a second open of the radio the first is still opening: it is refused, in the part's words
    const testing::Outcome o = pumped(d.eng, s, selectFake());
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), partWords(part));
    // when the detached open has returned (and dropped the radio it opened), the choice is accepted
    CHECK(passUntil(d.eng, [&] { return engine_api_test::fakeRadioOpens().load() == 1 && engine_api_test::fakeRadioLive().load() == 0; }, 6000));
    engine_api_test::fakeRadioOpenDelayMs() = 0;
    const testing::Outcome again = pumped(d.eng, s, selectFake());
    CHECK_EQ(again.result.status, static_cast<int32_t>(FOXAPI_OK));
    CHECK(passUntil(d.eng, [&] { return radio1Open(s); }));
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
}

void testARetiredOpenKeepsItsHold() {
    // Radio 1 chooses the radio, then another source, while the open is still on its worker: the open is SET ASIDE (its worker is inside the driver and cannot be
    // interrupted), and until that worker returns the radio is still being opened - Radio 2 may not have it.
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    engine_api_test::fakeRadioOpenDelayMs() = 1500;
    Access::launchOpen(d.eng, engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs);
    CHECK(passUntil(d.eng, [&] { return radio1Opening(s); }, 2000));
    // a second open (the person pressed Open again, with a correction) sets the first aside without waiting for it
    Access::launchOpen(d.eng, engine_api_test::kFakeRadioDriver, "serial=00000003");
    passes(d.eng, 3);
    // the first open is retired and still on its worker: its radio is still held, by the first radio, in its words
    const testing::Outcome o = pumped(d.eng, s, addRadioCmd(2, engine_api_test::kFakeRadioId));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), std::string("in use by Radio 1"));
    // once its worker has returned and its radio has been closed, the radio is free again
    CHECK(passUntil(d.eng, [&] { return engine_api_test::fakeRadioOpens().load() == 2 && engine_api_test::fakeRadioLive().load() == 1 && !radio1Opening(s); }, 8000));
    engine_api_test::fakeRadioOpenDelayMs() = 0;
    CHECK_EQ(pumped(d.eng, s, addRadioCmd(2, engine_api_test::kFakeRadioId)).result.status, static_cast<int32_t>(FOXAPI_OK));
}

// ---- 15 to 17 ----------------------------------------------------------------------------------------------------------------------------------

void testRadio2HoldsItThenPatchStarts() {
    Direct d;
    Access::setRecordDir(d.eng, (g_scratch / "takes15").string());
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    CHECK_EQ(pumped(d.eng, s, addRadioCmd(2, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return Access::receiverHasDevice(d.eng, 1); }));
    FoxCommand rec = testing::cmd(FOXAPI_OP_RECORD_IQ, 0.0, 1);
    rec.target = 2;
    CHECK_EQ(pumped(d.eng, s, rec).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (readRadio(s, 2).flags & FOXAPI_RX_RECORDING_IQ) != 0u; }));
    // the patch starts with a part naming Radio 2's radio: the part is refused in words, never opened; Radio 2 and its take are untouched
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    Access::setPatchRunning(d.eng, true);
    passes(d.eng, 6);
    CHECK_EQ(Access::refusal(d.eng, part), std::string("in use by Radio 2"));
    CHECK_EQ(fact(s, "patch.refusal." + std::to_string(part)), std::string("in use by Radio 2"));
    CHECK_EQ(Access::patchRadiosOpen(d.eng), std::size_t{0});
    CHECK(Access::receiverHasDevice(d.eng, 1));
    CHECK((readRadio(s, 2).flags & FOXAPI_RX_RUNNING) != 0u);
    CHECK((readRadio(s, 2).flags & FOXAPI_RX_RECORDING_IQ) != 0u);
    rec.ival[0] = 0;
    CHECK_EQ(pumped(d.eng, s, rec).status(), FOXAPI_OK);
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
}

void testRadio1HoldsItThenPatchStarts() {
    Direct d;
    Access::setRecordDir(d.eng, (g_scratch / "takes16").string());
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    CHECK(openFakeRadioAsReceiver(d, s));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RECORDING_IQ) != 0u; }));
    // START with a part naming Radio 1's radio: the loan, as it always was - Radio 1 runs on the generator, the part opens the radio. Nobody is refused. (An I/Q take
    // of the radio ends when the source changes: pinned here, not changed.)
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    Access::setPatchRunning(d.eng, true);
    CHECK(passUntil(d.eng, [&] { return Access::patchRadiosOpen(d.eng) == 1u && Access::patchMainKept(d.eng); }));
    passes(d.eng, 3);
    CHECK_EQ(Access::refusal(d.eng, part), std::string());
    CHECK(!radio1Open(s));
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RECORDING_IQ) == 0u; }, 3000));
    // STOP: Radio 1 gets its radio back
    Access::setPatchRunning(d.eng, false);
    CHECK(passUntil(d.eng, [&] { return radio1Open(s) && !Access::patchMainKept(d.eng); }));
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
}

void testPartSwitchedOffWhileRunningThenRadio2Takes() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, kKey, true);
    CHECK(startPartAndWaitForItsRadio(d, part));
    Access::setPartOn(d.eng, part, false);
    CHECK(passUntil(d.eng, [&] { return Access::patchRadiosOpen(d.eng) == 0u; }));
    passes(d.eng, 3);
    // the patch is still running, the part is switched off: Radio 2 may have the radio
    const testing::Outcome o = pumped(d.eng, s, addRadioCmd(2, engine_api_test::kFakeRadioId));
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_OK));
    CHECK(passUntil(d.eng, [&] { return Access::receiverHasDevice(d.eng, 1); }));
    // switching the part on again is refused in words, and Radio 2 is untouched
    Access::setPartOn(d.eng, part, true);
    passes(d.eng, 6);
    CHECK_EQ(Access::refusal(d.eng, part), std::string("in use by Radio 2"));
    CHECK_EQ(Access::patchRadiosOpen(d.eng), std::size_t{0});
    CHECK(Access::receiverHasDevice(d.eng, 1));
    CHECK_EQ(engine_api_test::fakeRadioPeak().load(), 1);
}

void testRadio1DuringARunIsStillRefusedForAnOffPart() {
    // The loan means a running patch takes whatever radio Radio 1 holds, so Radio 1 keeps today's answer while the patch RUNS (review F8): a part that names the radio
    // holds it for Radio 1 even when it is switched off. Radio 2 to 8 are judged by the new rule (test 17).
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, kKey, false);
    Access::setPatchRunning(d.eng, true);
    passes(d.eng, 4);
    const testing::Outcome o = pumped(d.eng, s, selectFake());
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), partWords(part));
}

// ---- 18, 19, 21 --------------------------------------------------------------------------------------------------------------------------------

void testIndexKeys() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, pc::makeDeviceKey("rtlsdr", "index=0"), false);
    passes(d.eng, 3);
    // stopped: index 0 and index 1 are both free for Radio 2
    CHECK_EQ(pumped(d.eng, s, addRadioCmd(2, "rtlsdr:index=0")).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.count") == "2"; }));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    // running: index 0 is held by the open part, index 1 is not
    Access::setPartOn(d.eng, part, true);
    CHECK(startPartAndWaitForItsRadio(d, part));
    const testing::Outcome busy = pumped(d.eng, s, addRadioCmd(2, "rtlsdr:index=0"));
    CHECK_EQ(busy.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(busy.result.message), partWords(part));
    CHECK_EQ(pumped(d.eng, s, addRadioCmd(2, "rtlsdr:index=1")).status(), FOXAPI_OK);
}

void testTwoDonglesOneDefaultSerial() {
    // two dongles that both say serial 00000001 are ONE radio to the ledger (a default serial is not an identity): a stopped part naming it does not block the
    // first receiver, and the second receiver is refused by the first - the safe side
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    (void)Access::addPart(d.eng, kKey, false);
    passes(d.eng, 3);
    CHECK(openFakeRadioAsReceiver(d, s));
    const testing::Outcome o = pumped(d.eng, s, addRadioCmd(2, engine_api_test::kFakeRadioId));
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_BUSY));
    CHECK_EQ(std::string(o.result.message), std::string("in use by Radio 1"));
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 1);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 1);
}

void testRadioAddPlutoWithStoppedNode() {
    // a Pluto is held by its address as typed; the fix is the same for it. (The address is this machine's own: the open goes nowhere, and is not looked at.)
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, pc::makeDeviceKey("pluto", "uri=ip:127.0.0.1"), false);
    passes(d.eng, 3);
    const testing::Outcome o = pumped(d.eng, s, addRadioCmd(2, "open-pluto:ip:127.0.0.1"));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, static_cast<int32_t>(FOXAPI_OK));   // BUSY before
    (void)part;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
}

void testOpenPlutoIsNotAskedOfTheLedger() {
    // Radio 1's choice of a Pluto by its address ("open-pluto:") never asks the ledger (engine.cpp), so a RUNNING part naming that Pluto does not refuse it: unchanged by the
    // fix, pinned here. (Every open in this test is the fake radio, through the engine's hook: nothing is sent to any address.)
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(scanFakeRadio(d, s));
    const std::uint32_t part = Access::addPart(d.eng, pc::makeDeviceKey("pluto", "uri=ip:127.0.0.1"), true);
    CHECK(startPartAndWaitForItsRadio(d, part));
    const testing::Outcome o = pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "open-pluto:ip:127.0.0.1"));
    CHECK(o.landed);
    CHECK(o.result.status != static_cast<int32_t>(FOXAPI_BUSY));
    CHECK(std::string(o.result.message).find("in use by") == std::string::npos);
}

}  // namespace

int main() {
    std::printf("test_engine_api_radio_holder\n");
    isolate();
    testAStoppedPartDoesNotRefuseTheChoice();
    testRadioAddWithAStoppedPatchNode();
    testRadioAddWithARunningPatchNode();
    testStopAndChooseInOnePass();
    testPartSwitchedOffAndChooseInOnePass();
    testChooseAndStartInOnePass();
    testStartWhileTheReceiverIsStillOpening();
    testHandBackPendingStillHolds();
    testStopWithAPartStillOpeningThenChoose();
    testARetiredOpenKeepsItsHold();
    testRadio2HoldsItThenPatchStarts();
    testRadio1HoldsItThenPatchStarts();
    testPartSwitchedOffWhileRunningThenRadio2Takes();
    testRadio1DuringARunIsStillRefusedForAnOffPart();
    testIndexKeys();
    testTwoDonglesOneDefaultSerial();
    testRadioAddPlutoWithStoppedNode();
    testOpenPlutoIsNotAskedOfTheLedger();
    Access::removeFakeRadio();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_radio_holder");
}
