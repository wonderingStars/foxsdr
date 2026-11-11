// test_engine_api_radio_lists.cpp - THE LISTS THAT DESCRIBE ONE RADIO, PER RADIO (multi-radio engine, stage 6; docs/multi-radio-records/engine-stages-5-6.md).
//
// GAINS, ANTENNAS, SAMPLE_RATES and DECODERS described the first radio only. read_radio_list(session, radio, list, index, out) gives any radio's copy, behind FOXAPI_CAP_RADIOS;
// read_list of the same four lists follows the session's focus (radio 1 until RADIO_FOCUS), so a client that knows nothing of several radios sees what it always saw.
//
//   A  EACH RADIO'S OWN: a radio on the fake RTL-SDR lists its gain stage, its six rates and its antenna; the generator radios list none; a gain set on radio 2 moves radio 2's
//      list and no other, and so does a rate;
//   B  THE FOCUS: read_list follows the session's focus for exactly these four lists and for no other; a second session keeps its own;
//   C  THE EDGES: a radio that is not there is NO_DEVICE (also after it is removed), radio 0 is the focus, a list that is not one radio's is BAD_ARGUMENT, the count form (out NULL);
//   D  THE CAPABILITY WORD names the set (FOXAPI_CAP_RADIOS) and the function is in the table.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
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
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
using cascade::engine::Engine;

namespace cascade::gui {
struct AppWindowTestAccess {
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
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

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
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr_radio_lists_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, scratch.string()); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

struct Rig {
    Engine eng;
    std::unique_ptr<testing::Session> s;
    Rig() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
        s = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kGrants);
        CHECK(s->ok());
    }
    void pump(int n = 1) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    template <class F>
    bool until(F done, int timeoutMs = 10000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return false;
    }
    testing::Outcome send(const FoxCommand& c, testing::Session* as = nullptr) {
        testing::Session& sess = as != nullptr ? *as : *s;
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(sess.raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 40 && !o.landed; ++pass) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(sess.raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
        FoxCommand c = testing::cmd(op, n0, i0, text);
        c.target = target;
        return c;
    }
    // per-radio list reads
    int32_t count(std::uint32_t radio, std::uint32_t list, testing::Session* as = nullptr) {
        return api()->read_radio_list((as != nullptr ? *as : *s).raw(), radio, list, 0, nullptr);
    }
    FoxListItem item(std::uint32_t radio, std::uint32_t list, std::uint32_t index, std::int32_t* status = nullptr) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        const std::int32_t r = api()->read_radio_list(s->raw(), radio, list, index, &it);
        if (status != nullptr) { *status = r; }
        return it;
    }
    int32_t focusCount(std::uint32_t list, testing::Session* as = nullptr) { return api()->read_list((as != nullptr ? *as : *s).raw(), list, 0, nullptr); }
    FoxListItem focusItem(std::uint32_t list, std::uint32_t index, testing::Session* as = nullptr) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        api()->read_list((as != nullptr ? *as : *s).raw(), list, index, &it);
        return it;
    }
};

// Radio 2 on the fake RTL-SDR (six rates, a gain stage TUNER of 0..49.6 dB, an antenna RX), radio 3 on the generator.
void radioTwoOnTheFakeDevice(Rig& r) {
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
    CHECK(r.until([&] {
        for (int32_t i = 0; i < api()->read_list(r.s->raw(), FOXAPI_LIST_DEVICES, 0, nullptr); ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            api()->read_list(r.s->raw(), FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i), &it);
            if (std::string(it.detail) == engine_api_test::kFakeRadioId) { return true; }
        }
        return false;
    }));
    FoxCommand add = testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 2, engine_api_test::kFakeRadioId);
    CHECK_EQ(r.send(add).status(), FOXAPI_OK);
    CHECK(r.until([&] {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        return api()->read_radio_state(r.s->raw(), 2, &st) == FOXAPI_OK && (st.flags & FOXAPI_RX_DEVICE_OPEN) != 0u;
    }));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 30000.0, 3)).status(), FOXAPI_OK);
    r.pump(5);
}

void testEachRadiosOwn() {
    std::printf("  A: each radio's own gains, rates and antennas\n");
    Access::installFakeRadio();
    {
        Rig r;
        radioTwoOnTheFakeDevice(r);
        // radio 2: one gain stage with its range, six rates with the current one marked, one antenna
        CHECK_EQ(r.count(2, FOXAPI_LIST_GAINS), 1);
        const FoxListItem g = r.item(2, FOXAPI_LIST_GAINS, 0);
        CHECK_EQ(std::string(g.name), std::string("TUNER"));
        CHECK_EQ(g.value[0], 0.0);
        CHECK_EQ(g.value[1], 49.6);
        CHECK_EQ(r.count(2, FOXAPI_LIST_SAMPLE_RATES), 6);
        int active = 0;
        for (std::uint32_t i = 0; i < 6; ++i) { active += (r.item(2, FOXAPI_LIST_SAMPLE_RATES, i).flags & FOXAPI_ITEM_ACTIVE) != 0u ? 1 : 0; }
        CHECK_EQ(active, 1);
        CHECK_EQ(r.count(2, FOXAPI_LIST_ANTENNAS), 1);
        const FoxListItem ant = r.item(2, FOXAPI_LIST_ANTENNAS, 0);
        CHECK_EQ(std::string(ant.name), std::string("RX"));
        CHECK((ant.flags & FOXAPI_ITEM_ACTIVE) != 0u);
        // the generator radios list none of the three; the focused (first) radio's read_list says the same
        for (const std::uint32_t radio : {1u, 3u}) {
            CHECK_EQ(r.count(radio, FOXAPI_LIST_GAINS), 0);
            CHECK_EQ(r.count(radio, FOXAPI_LIST_SAMPLE_RATES), 0);
            CHECK_EQ(r.count(radio, FOXAPI_LIST_ANTENNAS), 0);
        }
        CHECK_EQ(r.focusCount(FOXAPI_LIST_GAINS), 0);
        // a gain set on radio 2 moves radio 2's list and no other
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_GAIN, 2, 33.3, 0, "TUNER")).status(), FOXAPI_OK);
        CHECK(r.until([&] { return std::fabs(r.item(2, FOXAPI_LIST_GAINS, 0).value[3] - 33.3) < 0.051; }));
        CHECK_EQ(r.count(1, FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(r.count(3, FOXAPI_LIST_GAINS), 0);
        // a rate set on radio 2 moves the marked row of radio 2's list
        std::uint32_t before = 99;
        for (std::uint32_t i = 0; i < 6; ++i) {
            if ((r.item(2, FOXAPI_LIST_SAMPLE_RATES, i).flags & FOXAPI_ITEM_ACTIVE) != 0u) { before = i; }
        }
        const double want = before == 4u ? 1024000.0 : 2400000.0;
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_SAMPLE_RATE, 2, want)).status(), FOXAPI_OK);
        CHECK(r.until([&] {
            for (std::uint32_t i = 0; i < 6; ++i) {
                const FoxListItem it = r.item(2, FOXAPI_LIST_SAMPLE_RATES, i);
                if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u && it.value[0] == want) { return true; }
            }
            return false;
        }));
        CHECK_EQ(r.count(1, FOXAPI_LIST_SAMPLE_RATES), 0);
        // the lists of radio 1 and of radio 2 are not each other's: the focused read after FOCUS 2 is radio 2's
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.focusCount(FOXAPI_LIST_GAINS), 1);
        CHECK_EQ(std::string(r.focusItem(FOXAPI_LIST_GAINS, 0).name), std::string("TUNER"));
        CHECK_EQ(r.focusCount(FOXAPI_LIST_SAMPLE_RATES), 6);
        CHECK_EQ(r.focusCount(FOXAPI_LIST_ANTENNAS), 1);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 3)).status(), FOXAPI_OK);
        CHECK_EQ(r.focusCount(FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.focusCount(FOXAPI_LIST_GAINS), 0);
    }
    Access::removeFakeRadio();
}

void testTheFocusIsPerSessionAndForTheFourListsOnly() {
    std::printf("  B: read_list follows the session's focus for these four lists, and only these\n");
    Access::installFakeRadio();
    {
        Rig r;
        radioTwoOnTheFakeDevice(r);
        testing::Session other(api(), r.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kGrants);
        CHECK(other.ok());
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.focusCount(FOXAPI_LIST_GAINS), 1);
        CHECK_EQ(r.focusCount(FOXAPI_LIST_GAINS, &other), 0);   // the other session never moved
        // a list that is not one radio's is the same whatever the focus: the devices list has the same rows for both sessions
        CHECK_EQ(r.focusCount(FOXAPI_LIST_DEVICES), r.focusCount(FOXAPI_LIST_DEVICES, &other));
        CHECK_EQ(r.focusCount(FOXAPI_LIST_MODES), 8);
    }
    Access::removeFakeRadio();
}

void testTheEdges() {
    std::printf("  C: the edges\n");
    Access::installFakeRadio();
    {
        Rig r;
        radioTwoOnTheFakeDevice(r);
        std::int32_t status = 0;
        (void)r.item(5, FOXAPI_LIST_GAINS, 0, &status);   // no radio 5
        CHECK_EQ(status, FOXAPI_NO_DEVICE);
        CHECK_EQ(r.count(5, FOXAPI_LIST_GAINS), FOXAPI_NO_DEVICE);
        CHECK_EQ(r.count(99, FOXAPI_LIST_GAINS), FOXAPI_NO_DEVICE);
        // radio 0 is the session's focus
        CHECK_EQ(r.count(0, FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.count(0, FOXAPI_LIST_GAINS), 1);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 1)).status(), FOXAPI_OK);
        // an index past the end is NO_CHANGE-free: the count form says how many; a read past the end fills nothing
        (void)r.item(2, FOXAPI_LIST_GAINS, 7, &status);
        CHECK(status >= 0);
        // a list that is not one radio's
        CHECK_EQ(r.count(2, FOXAPI_LIST_BOOKMARKS), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.count(2, FOXAPI_LIST_PLUGINS), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.count(1, FOXAPI_LIST_DEVICES), FOXAPI_BAD_ARGUMENT);
        // a removed radio is NO_DEVICE at once, and a radio added in its place lists its own
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        r.pump(3);
        CHECK_EQ(r.count(2, FOXAPI_LIST_GAINS), FOXAPI_NO_DEVICE);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 50000.0, 2)).status(), FOXAPI_OK);
        r.pump(5);
        CHECK_EQ(r.count(2, FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(r.count(2, FOXAPI_LIST_SAMPLE_RATES), 0);
    }
    Access::removeFakeRadio();
}

std::string fact(Rig& r, const std::string& key) {
    char buf[2048];
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string("<absent>");
}

std::uint64_t listSeqOf(Rig& r, std::uint32_t radio) {
    FoxReceiverState st{};
    st.structSize = sizeof(st);
    (void)api()->read_radio_state(r.s->raw(), radio, &st);
    return st.listSeq;
}

void testSourceKindsSoloSequenceAndFocus() {
    std::printf("  E: every source kind radio 1 can be, the solo read-back, the list sequence on add and remove, the focus after a remove\n");
    {
        Rig r;
        // THE LIST SEQUENCE MOVES when a radio is added and when one is removed (the fingerprint holds the slot mask)
        const std::uint64_t s0 = listSeqOf(r, 1);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 20000.0, 2)).status(), FOXAPI_OK);
        r.pump(3);
        const std::uint64_t s1 = listSeqOf(r, 1);
        CHECK(s1 != s0);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 30000.0, 3)).status(), FOXAPI_OK);
        r.pump(3);
        // THE SOLO IS READ BACK
        CHECK_EQ(fact(r, "radio.solo"), std::string("0"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, 3)).status(), FOXAPI_OK);
        r.pump(3);
        CHECK_EQ(fact(r, "radio.solo"), std::string("3"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, 2)).status(), FOXAPI_OK);
        r.pump(3);
        CHECK_EQ(fact(r, "radio.solo"), std::string("2"));
        // THE FOCUS FOLLOWS A REMOVE: a session focused on radio 2 is on radio 1 after it, with its seq groups moved; the solo of a removed radio is cleared and said
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 2)).status(), FOXAPI_OK);
        const std::uint64_t before = [&] { FoxReceiverState st{}; st.structSize = sizeof(st); (void)api()->read_state(r.s->raw(), &st); return st.seq; }();
        const std::uint64_t seqBefore = listSeqOf(r, 1);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        r.pump(3);
        CHECK(listSeqOf(r, 1) != seqBefore);
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        CHECK_EQ(api()->read_state(r.s->raw(), &st), FOXAPI_OK);   // the focused read answers (radio 1), not NO_DEVICE
        CHECK(st.seq != before);
        CHECK_EQ(api()->read_radio_state(r.s->raw(), 0, &st), FOXAPI_OK);
        CHECK_EQ(fact(r, "radio.solo"), std::string("0"));
        // a session focused on another radio is not moved
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 3)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 40000.0, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        FoxCommand a3 = testing::cmd(FOXAPI_OP_RADIO_AUDIO, 0.0, 0);
        a3.ival[1] = 2;   // radio 0 = the focus: radio 3 if the focus stayed
        CHECK_EQ(r.send(a3).status(), FOXAPI_OK);
        CHECK_EQ(fact(r, "radio.3.audio"), std::string("right"));
        // EVERY SOURCE RADIO 1 CAN BE: an I/Q file, an rtl_tcp server and a Pluto are accepted as a second radio (they open through the same worker; with nothing behind them the
        // radio stays on the generator and says why); the sound card stays the first radio's and the refusal says so
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 3)).status(), FOXAPI_OK);
        const testing::Outcome file = r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 0, "file:C:/no/such/recording.iq"));
        CHECK_EQ(file.status(), FOXAPI_OK);
        const testing::Outcome tcp = r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 0, "open-rtltcp:127.0.0.1:1"));
        CHECK_EQ(tcp.status(), FOXAPI_OK);
        const testing::Outcome pluto = r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 0, "open-pluto:uri=ip:127.0.0.1"));
        CHECK_EQ(pluto.status(), FOXAPI_OK);
        const testing::Outcome card = r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 0, "soundcard:open"));
        CHECK_EQ(card.status(), FOXAPI_UNSUPPORTED);
        CHECK(std::string(card.result.message).find("sound card") != std::string::npos);
        CHECK_EQ(fact(r, "radio.count"), std::string("4"));
    }
}

void testTheCapabilityWord() {
    std::printf("  D: the capability word names the set\n");
    const FoxEngineApi* a = api();
    CHECK((a->capabilities & FOXAPI_CAP_RADIOS) != 0ull);
    CHECK(FOXAPI_HAS(a, read_radio_state));
    CHECK(FOXAPI_HAS(a, read_radio_spectrum));
    CHECK(FOXAPI_HAS(a, read_radio_list));
}

}  // namespace

int main() {
    std::printf("test_engine_api_radio_lists\n");
    isolate();
    testEachRadiosOwn();
    testTheFocusIsPerSessionAndForTheFourListsOnly();
    testTheEdges();
    testSourceKindsSoloSequenceAndFocus();
    testTheCapabilityWord();
    return check::finish("test_engine_api_radio_lists");
}
