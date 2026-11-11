// test_engine_api_decode_radio.cpp - REAL DECODERS ON OTHER RADIOS, FED BY THEIR OWN SOURCES (multi-radio engine, stage 7; docs/multi-radio-records/engine-stage-7.md).
//
// The proof the radio probe could not give: real decoder modules (the user's installed cw, pocsag, rtty and two-tone decoders) put on radios 2 to 5 with PLUGIN_RADIO, each radio on
// its OWN I/Q file (the fixtures of the slint drive set, whose expected decodes are in src/slint/tests/drive/fixture-*.txt), while radio 1 runs a different file and a fifth decoder
// (EAS) that has nothing to hear. Asserted:
//   1  every decoder's expected line arrives, and arrives attributed to ITS radio (DECODER_LINES value[1]);
//   2  no decoder line is attributed to radio 1, and no radio carries another radio's decoder's text (the cw text is only ever radio 2's, and so on);
//   3  fact.plugin.<file>.radio and fact.radio.<n>.plugins say where each one runs;
//   4  radio 1's source, rate and spectrum are what they were.
// The modules are the installed ones (argument 2, else $FOXSDR_DRIVE_PLUGINS_DIR, else %LOCALAPPDATA%\FoxSDR\plugins, read before the test points the profile at a scratch folder); a
// machine without them SKIPS and says so - the decoder text is the plugin's own, not this tree's. The fixture folder is argument 1.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
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
#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;

namespace {
std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
fs::path g_plugins;
std::string pluginDirText() { return g_plugins.string(); }
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDirText;
    }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

struct Line {
    std::uint64_t id = 0;
    std::string plugin;
    std::string text;
    int radio = -1;
};

struct Rig {
    Engine eng;
    std::unique_ptr<testing::Session> s;
    Rig() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
        s = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kGrants);
        CHECK(s->ok());
        pump(3);
    }
    void pump(int n = 1) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    template <class F>
    bool until(F done, int timeoutMs = 20000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return false;
    }
    testing::Outcome send(const FoxCommand& c) {
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s->raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 40 && !o.landed; ++pass) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s->raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[8192];
        const int32_t n = api()->get_setting(s->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    FoxReceiverState radio(std::uint32_t n) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        (void)api()->read_radio_state(s->raw(), n, &st);
        return st;
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
        FoxCommand c = testing::cmd(op, n0, i0, text);
        c.target = target;
        return c;
    }
    std::vector<Line> lines() {
        std::vector<Line> out;
        const int32_t count = api()->read_list(s->raw(), FOXAPI_LIST_DECODER_LINES, 0, nullptr);
        for (int32_t i = 0; i < count; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (api()->read_list(s->raw(), FOXAPI_LIST_DECODER_LINES, static_cast<std::uint32_t>(i), &it) <= i) { break; }
            out.push_back({it.id, it.name, it.detail, static_cast<int>(it.value[1])});
        }
        return out;
    }
};

struct Case {
    const char* plugin;        // module file prefix, e.g. "cw-decoder-"
    const char* fixture;       // wav in the fixtures folder
    double centreHz;
    double vfoHz;
    long long mode;
    double bandwidthHz;
    const char* expect;        // a substring of the decoded line
    int radio;                 // the radio it runs on
    std::string file;          // resolved module file name
};

// Finds the module file for `prefix` in `dir`.
std::string moduleFor(const fs::path& dir, const char* prefix) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(prefix, 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".dll") { return n; }
    }
    return std::string();
}

void testDecodersOnOtherRadios(const fs::path& fixtures) {
    std::printf("real decoders on radios 2 to 5, each on its own file\n");
    std::vector<Case> cases = {
        {"cw-decoder-", "cw-decoder.wav", 14037500.0, 22500.0, FOXAPI_DEMOD_CW, 500.0, "FOXSDR", 2, ""},
        {"pocsag-decoder-", "pocsag-decoder.wav", 153332250.0, 17750.0, FOXAPI_DEMOD_NFM, 12500.0, "FOXSDR FIXTURE", 3, ""},
        {"rtty-decoder-", "rtty-decoder.wav", 14057500.0, 22500.0, FOXAPI_DEMOD_LSB, 3000.0, "FOXSDR FIXTURE 1", 4, ""},
        {"twotone-decoder-", "twotone-decoder.wav", 153958250.0, 41750.0, FOXAPI_DEMOD_NFM, 12500.0, "code 258", 5, ""},
    };
    for (Case& c : cases) {
        c.file = moduleFor(g_plugins, c.plugin);
        CHECK(!c.file.empty());
        if (c.file.empty()) { return; }
    }
    const std::string bystander = moduleFor(g_plugins, "eas-decoder-");
    CHECK(!bystander.empty());

    Rig r;
    // radio 1: a different file entirely (an AM carrier), with a decoder of its own that has nothing to hear
    {
        const std::string scope = (fixtures / "scope.wav").string();
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SELECT_SOURCE, 1, 0.0, 0, ("file:" + scope).c_str())).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.1.source") == "file" && r.radio(1).sampleRateHz == 48000.0; }));
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    }
    const FoxReceiverState one0 = r.radio(1);
    CHECK_EQ(one0.sampleRateHz, 48000.0);
    {
        FoxCommand p = testing::cmd(FOXAPI_OP_PLUGIN_RADIO, 0.0, 1, bystander.c_str());
        p.ival[1] = 1;
        const int32_t st = r.send(p).status();
        CHECK(st == FOXAPI_OK || st == FOXAPI_NO_CHANGE);
    }
    // radios 2 to 5, one file each, tuned the way the drive scripts tune them
    for (const Case& c : cases) {
        FoxCommand add = testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, c.radio, ("file:" + (fixtures / c.fixture).string()).c_str());
        add.ival[1] = 1;
        CHECK_EQ(r.send(add).status(), FOXAPI_OK);
    }
    CHECK(r.until([&] {
        for (const Case& c : cases) {
            if (r.fact("radio." + std::to_string(c.radio) + ".source") != "file") { return false; }
        }
        return true;
    }));
    for (const Case& c : cases) {
        const std::uint32_t n = static_cast<std::uint32_t>(c.radio);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, n, c.centreHz)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, n, 0.0, c.mode)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_BANDWIDTH, n, c.bandwidthHz)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VFO_OFFSET, n, c.vfoHz)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VOLUME, n, 0.0)).status(), FOXAPI_OK);   // nothing plays
        FoxCommand p = testing::cmd(FOXAPI_OP_PLUGIN_RADIO, 0.0, c.radio, c.file.c_str());
        p.ival[1] = 1;
        CHECK_EQ(r.send(p).status(), FOXAPI_OK);
    }
    for (const Case& c : cases) {
        CHECK_EQ(r.fact("plugin." + c.file + ".radio"), std::to_string(c.radio));
        CHECK(r.fact("radio." + std::to_string(c.radio) + ".plugins").find(c.file) != std::string::npos);
        CHECK(r.fact("radio.1.plugins").find(c.file) == std::string::npos);
    }
    CHECK(r.fact("radio.1.plugins").find(bystander) != std::string::npos);

    // 1: every decoder's expected text arrives, attributed to its own radio. The fixtures loop, so a decode is a matter of seconds.
    std::vector<bool> got(cases.size(), false);
    CHECK(r.until([&] {
        for (const Line& l : r.lines()) {
            for (std::size_t i = 0; i < cases.size(); ++i) {
                if (l.radio == cases[i].radio && l.text.find(cases[i].expect) != std::string::npos) { got[i] = true; }
            }
        }
        return std::all_of(got.begin(), got.end(), [](bool b) { return b; });
    }, 90000));
    for (std::size_t i = 0; i < cases.size(); ++i) {
        std::printf("    %-17s radio %d decoded %s\n", cases[i].plugin, cases[i].radio, got[i] ? "yes" : "NO");
        CHECK(got[i]);
    }
    // let the loop run on a while longer so a leak would have time to show
    r.pump(300);
    // 2: nothing is attributed to radio 1, and no decoder's text turns up on another radio
    int onRadioOne = 0, crossed = 0, total = 0;
    std::map<std::string, int> radioOfPlugin;   // each plugin's lines carry ONE radio number
    for (const Line& l : r.lines()) {
        ++total;
        if (l.radio == 1) { ++onRadioOne; }
        const auto it = radioOfPlugin.find(l.plugin);
        if (it == radioOfPlugin.end()) {
            radioOfPlugin[l.plugin] = l.radio;
        } else if (it->second != l.radio) {
            ++crossed;
        }
    }
    for (const auto& [name, radio] : radioOfPlugin) { std::printf("    plugin '%s' spoke on radio %d\n", name.c_str(), radio); }
    // four decoders, four radios: no two plugins share a radio number
    {
        std::set<int> radios;
        for (const auto& kv : radioOfPlugin) { radios.insert(kv.second); }
        CHECK_EQ(radios.size(), radioOfPlugin.size());
    }
    std::printf("    %d decoder lines in the backlog, %d attributed to radio 1, %d crossed\n", total, onRadioOne, crossed);
    CHECK(total >= 4);
    CHECK_EQ(onRadioOne, 0);
    CHECK_EQ(crossed, 0);
    // every line names a radio that has a decoder of that plugin: the radio number is one of 2..5
    for (const Line& l : r.lines()) { CHECK(l.radio >= 2 && l.radio <= 5); }
    // 4: radio 1 is what it was
    const FoxReceiverState one1 = r.radio(1);
    CHECK_EQ(one1.sampleRateHz, one0.sampleRateHz);
    CHECK_EQ(r.fact("radio.1.source"), std::string("file"));
    CHECK(r.fact("radio.1.plugins").find(bystander) != std::string::npos);
    CHECK_EQ(r.fact("radio.count"), std::string("5"));

    // 5: THE POCSAG FIXTURE THAT "DOES NOT DECODE ON RADIO 2" (the screen lane's open engine item, screen-s6.md B2). The cause is the TUNING, not the radio number:
    // the fixture's carrier is 17.75 kHz above the file's own centre, so the receiver must be tuned to centre + 17750 Hz of VFO offset (as the cases above do).
    // `radio tune <n> <hz>` of the drive scripts is SET_FREQUENCY - "tuned Hz; the VFO is kept, the centre follows" - which with the VFO at 0 puts the CENTRE at
    // 153.350 MHz and the VFO on it, 17.75 kHz away from the carrier. Mistuned that way the decoder is fed, the file plays and nothing decodes; given the offset
    // it decodes at once, on the very same radio.
    {
        const Case& p = cases[1];   // pocsag, expect "FOXSDR FIXTURE"
        FoxCommand add = testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 6, ("file:" + (fixtures / p.fixture).string()).c_str());
        add.ival[1] = 1;
        CHECK_EQ(r.send(add).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.6.source") == "file"; }));
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 6, 153350000.0)).status(), FOXAPI_OK);   // what `radio tune 6 153350000` sends
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 6, 0.0, p.mode)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_BANDWIDTH, 6, p.bandwidthHz)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VOLUME, 6, 0.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.radio(6).vfoOffsetHz, 0.0);
        FoxCommand mv = testing::cmd(FOXAPI_OP_PLUGIN_RADIO, 0.0, 6, p.file.c_str());
        mv.ival[1] = 1;
        CHECK_EQ(r.send(mv).status(), FOXAPI_OK);
        const auto heardOn6 = [&] {
            for (const Line& l : r.lines()) {
                if (l.radio == 6 && l.text.find(p.expect) != std::string::npos) { return true; }
            }
            return false;
        };
        (void)r.until([] { return false; }, 15000);   // fifteen seconds of passes: long enough for the fixture to loop several times
        const bool mistunedDecoded = heardOn6();
        std::printf("    pocsag on radio 6, tuned by SET_FREQUENCY alone (VFO offset 0): decoded %s\n", mistunedDecoded ? "YES" : "no");
        CHECK(!mistunedDecoded);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 6, p.centreHz)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VFO_OFFSET, 6, p.vfoHz)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return heardOn6(); }, 90000));
        std::printf("    ...and with the centre and the 17750 Hz offset the same radio decodes: %s\n", heardOn6() ? "yes" : "NO");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_decode_radio\n");
    if (argc < 2) {
        std::printf("usage: test_engine_api_decode_radio <fixtures folder> [plugin folder]\n");
        return 2;
    }
    const fs::path fixtures = argv[1];
    // The installed modules, found BEFORE the profile is pointed at a scratch folder.
    fs::path from;
    if (argc >= 3) {
        from = argv[2];
    } else if (const char* o = std::getenv("FOXSDR_DRIVE_PLUGINS_DIR"); o != nullptr && *o != '\0') {
        from = o;
    } else if (const char* la = std::getenv("LOCALAPPDATA"); la != nullptr && *la != '\0') {
        from = fs::path(la) / "FoxSDR" / "plugins";
    }
    engine_api_test::hermetic("decode_radio");
    std::error_code ec;
    if (from.empty() || !fs::is_directory(from, ec) || moduleFor(from, "cw-decoder-").empty()) {
        std::printf("SKIPPED: the installed decoder modules are not in '%s' (set FOXSDR_DRIVE_PLUGINS_DIR)\n", from.string().c_str());
        return 0;
    }
    g_plugins = fs::temp_directory_path() / ("foxsdr_decode_radio_plugins_" + std::to_string(
#if defined(_WIN32)
                                                                                     _getpid()
#else
                                                                                     getpid()
#endif
                                                                                         ));
    fs::remove_all(g_plugins, ec);
    fs::create_directories(g_plugins, ec);
    for (const char* prefix : {"cw-decoder-", "pocsag-decoder-", "rtty-decoder-", "twotone-decoder-", "eas-decoder-"}) {
        const std::string f = moduleFor(from, prefix);
        CHECK(!f.empty());
        if (!f.empty()) { fs::copy_file(from / f, g_plugins / f, fs::copy_options::overwrite_existing, ec); }
    }
    Access::installHooks();
    testDecodersOnOtherRadios(fixtures);
    fs::remove_all(g_plugins, ec);
    return check::finish("test_engine_api_decode_radio");
}
