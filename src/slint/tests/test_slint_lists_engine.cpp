// test_slint_lists_engine.cpp - THE SCREEN'S LISTS ARE THE SELECTED RADIO'S (screen stage S6, B1; docs/multi-radio-records/screen-s6.md).
//
// The strip's GAIN slider, the SIGNAL bank's gain / antenna / sample-rate lists and the DECODE bank's decoders used to be read with read_list, which answers for the session's focus (radio 1 until
// RADIO_FOCUS): with radio 2 selected they could show radio 1's stages, and a gain set from them went to radio 2 with radio 1's stage names. The host now asks for the SELECTED radio by number
// (host_logic readListFor -> read_radio_list) and aims the set at it. This test runs the real engine (radio 1 on the generator, which has no gain stage, radio 2 on the fake RTL-SDR with one
// stage TUNER 0..49.6 dB, six rates and an antenna, radio 3 on the generator) and the host's own reader:
//
//   A  with the session's focus on radio 1 the reader still gives radio 2's lists for radio 2 and radio 1's (none) for radio 1: the lists do not depend on where the focus is;
//   B  a gain set aimed at radio 2 lands on radio 2's TUNER; the same stage name sent to radio 1 (the old hazard) is refused and moves nothing;
//   C  a rate set aimed at radio 2 moves radio 2's marked row only;
//   D  the edges: radio 0 reads the focus, a list that is not one radio's reads as ever, a radio that is not there reads nothing.
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
#include "host_logic.hpp"

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
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr_slint_lists_" + std::to_string(pid));
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

using foxsdr::slinthost::readListFor;

std::int32_t hostCount(Rig& r, std::uint32_t radio, std::uint32_t list) { return readListFor(api(), r.s->raw(), radio, list, 0, nullptr); }
FoxListItem hostItem(Rig& r, std::uint32_t radio, std::uint32_t list, std::uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    readListFor(api(), r.s->raw(), radio, list, index, &it);
    return it;
}

void testTheReaderAsksForTheRadioNotTheFocus() {
    std::printf("  A: the reader gives each radio its own lists whatever the session's focus is\n");
    Access::installFakeRadio();
    {
        Rig r;
        radioTwoOnTheFakeDevice(r);
        // the focus is on radio 1 (never moved): the focused read sees radio 1's lists, the host's reader for radio 2 sees radio 2's
        CHECK_EQ(r.focusCount(FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(hostCount(r, 1, FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(hostCount(r, 2, FOXAPI_LIST_GAINS), 1);
        CHECK_EQ(std::string(hostItem(r, 2, FOXAPI_LIST_GAINS, 0).name), std::string("TUNER"));
        CHECK_EQ(hostItem(r, 2, FOXAPI_LIST_GAINS, 0).value[1], 49.6);
        CHECK_EQ(hostCount(r, 2, FOXAPI_LIST_SAMPLE_RATES), 6);
        CHECK_EQ(hostCount(r, 1, FOXAPI_LIST_SAMPLE_RATES), 0);
        CHECK_EQ(hostCount(r, 2, FOXAPI_LIST_ANTENNAS), 1);
        CHECK_EQ(std::string(hostItem(r, 2, FOXAPI_LIST_ANTENNAS, 0).name), std::string("RX"));
        CHECK_EQ(hostCount(r, 3, FOXAPI_LIST_GAINS), 0);
        // and with the focus on radio 3 the answers for radio 2 are the same
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 3)).status(), FOXAPI_OK);
        CHECK_EQ(hostCount(r, 2, FOXAPI_LIST_GAINS), 1);
        CHECK_EQ(r.focusCount(FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 1)).status(), FOXAPI_OK);
    }
    Access::removeFakeRadio();
}

void testAGainSetLandsOnTheRightStageOfTheRightRadio() {
    std::printf("  B: a gain set lands on radio 2's stage; the same name sent to radio 1 moves nothing\n");
    Access::installFakeRadio();
    {
        Rig r;
        radioTwoOnTheFakeDevice(r);
        const std::string stage = hostItem(r, 2, FOXAPI_LIST_GAINS, 0).name;   // the name the strip would hold with radio 2 selected
        CHECK_EQ(stage, std::string("TUNER"));
        // the old hazard: the strip's set with no target goes to the focus (radio 1), which has no such stage
        const testing::Outcome wrong = r.send(r.aimed(FOXAPI_OP_SET_GAIN, 0, 21.0, 0, stage.c_str()));
        CHECK(wrong.status() != FOXAPI_OK || (wrong.landed && wrong.result.status != FOXAPI_OK));
        r.pump(10);
        CHECK(std::fabs(hostItem(r, 2, FOXAPI_LIST_GAINS, 0).value[3] - 21.0) > 0.5);
        // the host's set, aimed at the selected radio
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_GAIN, 2, 33.3, 0, stage.c_str())).status(), FOXAPI_OK);
        CHECK(r.until([&] { return std::fabs(hostItem(r, 2, FOXAPI_LIST_GAINS, 0).value[3] - 33.3) < 0.051; }));
        CHECK_EQ(hostCount(r, 1, FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(hostCount(r, 3, FOXAPI_LIST_GAINS), 0);
    }
    Access::removeFakeRadio();
}

void testARateSetMovesOnlyThatRadiosRow() {
    std::printf("  C: a rate set aimed at radio 2 moves radio 2's marked row\n");
    Access::installFakeRadio();
    {
        Rig r;
        radioTwoOnTheFakeDevice(r);
        std::uint32_t before = 99;
        for (std::uint32_t i = 0; i < 6; ++i) { if ((hostItem(r, 2, FOXAPI_LIST_SAMPLE_RATES, i).flags & FOXAPI_ITEM_ACTIVE) != 0u) { before = i; } }
        CHECK(before < 6u);
        const double want = before == 4u ? 1024000.0 : 2400000.0;
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_SAMPLE_RATE, 2, want)).status(), FOXAPI_OK);
        CHECK(r.until([&] {
            for (std::uint32_t i = 0; i < 6; ++i) {
                const FoxListItem it = hostItem(r, 2, FOXAPI_LIST_SAMPLE_RATES, i);
                if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u && it.value[0] == want) { return true; }
            }
            return false;
        }));
        CHECK_EQ(hostCount(r, 1, FOXAPI_LIST_SAMPLE_RATES), 0);
    }
    Access::removeFakeRadio();
}

void testTheEdges() {
    std::printf("  D: radio 0 reads the focus, other lists read as ever, a missing radio reads nothing\n");
    Access::installFakeRadio();
    {
        Rig r;
        radioTwoOnTheFakeDevice(r);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 2)).status(), FOXAPI_OK);
        CHECK_EQ(hostCount(r, 0, FOXAPI_LIST_GAINS), 1);   // radio 0 = the session's focus (radio 2 now)
        CHECK_EQ(hostCount(r, 1, FOXAPI_LIST_GAINS), 0);
        CHECK_EQ(hostCount(r, 1, FOXAPI_LIST_MODES), 8);   // not one radio's: read_list, the same for every radio
        CHECK_EQ(hostCount(r, 2, FOXAPI_LIST_MODES), 8);
        CHECK(hostCount(r, 5, FOXAPI_LIST_GAINS) < 0);     // no radio 5
        CHECK(foxsdr::slinthost::isPerRadioList(FOXAPI_LIST_GAINS) && foxsdr::slinthost::isPerRadioList(FOXAPI_LIST_ANTENNAS) &&
              foxsdr::slinthost::isPerRadioList(FOXAPI_LIST_SAMPLE_RATES) && foxsdr::slinthost::isPerRadioList(FOXAPI_LIST_DECODERS));
        CHECK(!foxsdr::slinthost::isPerRadioList(FOXAPI_LIST_MODES) && !foxsdr::slinthost::isPerRadioList(FOXAPI_LIST_BOOKMARKS));
    }
    Access::removeFakeRadio();
}

}  // namespace

int main() {
    std::printf("test_slint_lists_engine\n");
    isolate();
    testTheReaderAsksForTheRadioNotTheFocus();
    testAGainSetLandsOnTheRightStageOfTheRightRadio();
    testARateSetMovesOnlyThatRadiosRow();
    testTheEdges();
    return check::finish("test_slint_lists_engine");
}
