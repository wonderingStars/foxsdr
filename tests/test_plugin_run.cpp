// core/plugin_run.hpp: WHEN A FITTED PLUGIN RUNS (0.99.73).
//
// Everything here is pure: the three run states and their words, the plugin ID a state is kept
// under (and that a stop outlives an update because of it), the WANTED rule over every signal, the
// 30-second clock, and the capability questions that say which modules have a run state at all. The
// real application's use of these is tested in test_plugin_inuse_app.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "core/plugin_run.hpp"
#include "test_check.hpp"

using namespace cascade::core;

namespace {

constexpr std::int64_t k30s = kPluginDormantAfterMs;

// Every signal, one at a time, in the order of PluginUse (which is the order the log names them).
struct SignalRow {
    const char* name;
    void (*set)(PluginUse&);
    const char* words;  // what pluginUseSignal says for it alone
};

const SignalRow kSignals[] = {
    {"window", [](PluginUse& u) { u.window = true; }, "window open"},
    {"mapPage", [](PluginUse& u) { u.mapPage = true; }, "map page open"},
    {"radarScope", [](PluginUse& u) { u.radarScope = true; }, "radar scope showing"},
    {"patchNode", [](PluginUse& u) { u.patchNode = true; }, "patch node running"},
    {"playingAudio", [](PluginUse& u) { u.playingAudio = true; }, "playing audio"},
    {"decoderOutput", [](PluginUse& u) { u.decoderOutput = true; }, "decoder output window open"},
    {"browserSession", [](PluginUse& u) { u.browserSession = true; }, "browser connected"},
    {"textSink", [](PluginUse& u) { u.textSink = true; }, "wired to a Text sink"},
    {"standing", [](PluginUse& u) { u.standing = true; }, "standing duty"},
};

}  // namespace

int main() {
    // --- the words ------------------------------------------------------------------------
    {
        CHECK(std::string(pluginRunWord(PluginRun::Auto)) == "auto");
        CHECK(std::string(pluginRunWord(PluginRun::Always)) == "always");
        CHECK(std::string(pluginRunWord(PluginRun::Stopped)) == "stopped");
        for (PluginRun r : {PluginRun::Auto, PluginRun::Always, PluginRun::Stopped}) {
            PluginRun back = PluginRun::Auto;
            CHECK(parsePluginRun(pluginRunWord(r), back));
            CHECK(back == r);
        }
        // Anything else is not a state: refused, and the answer left alone.
        PluginRun keep = PluginRun::Always;
        for (const char* bad : {"", "Auto", "ALWAYS", "stop", "running", " auto", "auto ", "1"}) {
            CHECK(!parsePluginRun(bad, keep));
        }
        CHECK(keep == PluginRun::Always);
    }

    // --- the id a state is kept under -----------------------------------------------------
    {
        CHECK(pluginRunId("pocsag-decoder-1.0.2-abi3-win-x64.dll") == "pocsag-decoder");
        CHECK(pluginRunId("pocsag-decoder-1.0.3-abi3-win-x64.dll") == "pocsag-decoder");
        CHECK(pluginRunId("my-decoder.dll") == "my-decoder");
        CHECK(pluginRunId("probe_a.dll") == "probe_a");
        CHECK(pluginRunId("store-c-1.0.0.dll") == "store-c");
        CHECK(pluginRunId("adsb-1.2.0-abi3-linux-x64.so") == "adsb");
        CHECK(pluginRunId("adsb-1.2.0-abi3-macos-arm64.dylib") == "adsb");
        CHECK(pluginRunId("ADSB-1.2.0.DLL") == "ADSB");  // the extension is matched without case
        CHECK(pluginRunId("C:/plugins/ais-decoder-2.0.0-abi3-win-x64.dll") == "ais-decoder");
        CHECK(pluginRunId("C:\\plugins\\ais-decoder-2.0.0-abi3-win-x64.dll") == "ais-decoder");
        CHECK(pluginRunId("").empty());
        // IDEMPOTENT: an id handed back in is the same id, including one with a dot in it, which a
        // plain "strip the last dot" would shorten.
        CHECK(pluginRunId("pocsag-decoder") == "pocsag-decoder");
        CHECK(pluginRunId("foo.bar") == "foo.bar");
        CHECK(pluginRunId(pluginRunId("foo.bar-1.0.0.dll")) == pluginRunId("foo.bar-1.0.0.dll"));
        CHECK(pluginRunId("foo.bar-1.0.0.dll") == "foo.bar");
        // Two builds of one plugin are one id; two plugins are two.
        CHECK(pluginRunId("a-1.0.0.dll") == pluginRunId("a-9.9.9.dll"));
        CHECK(pluginRunId("a-1.0.0.dll") != pluginRunId("b-1.0.0.dll"));
    }

    // --- the stop set is kept by id: a stop outlives an update ------------------------------
    {
        PluginStopSet s;
        s.set({"pocsag-decoder-1.0.2-abi3-win-x64.dll", "", "pocsag-decoder-1.0.2-abi3-win-x64.dll",
               "ais-decoder"});
        // empties and duplicates dropped; ids held
        CHECK(s.keys().size() == 2u);
        CHECK(s.keys().at(0) == "pocsag-decoder");
        CHECK(s.keys().at(1) == "ais-decoder");
        // THE FILE NAME NO LONGER MATTERS: the 1.0.3 build is the plugin the user stopped.
        CHECK(s.contains("pocsag-decoder-1.0.2-abi3-win-x64.dll"));
        CHECK(s.contains("pocsag-decoder-1.0.3-abi3-win-x64.dll"));
        CHECK(s.contains("pocsag-decoder"));
        CHECK(s.contains("ais-decoder-9.9.9.dll"));
        CHECK(!s.contains("adsb-decoder-1.0.0.dll"));
        // An empty key never matches, and neither does an empty entry.
        CHECK(!s.contains(""));
        PluginStopSet e;
        e.set({""});
        CHECK(e.keys().empty());
        CHECK(!e.contains(""));
        LoadedPlugin p;
        p.path = "C:/plugins/ais-decoder-3.0.0-abi3-win-x64.dll";
        CHECK(s.contains(p));
        LoadedPlugin pathless;
        CHECK(!s.contains(pathless));
    }

    // --- WANTED: every signal alone, none, and all -------------------------------------------
    {
        CHECK(!pluginWanted(PluginUse{}));
        CHECK(std::string(pluginUseSignal(PluginUse{})).empty());
        for (const SignalRow& row : kSignals) {
            PluginUse u;
            row.set(u);
            std::printf("  signal %s\n", row.name);
            CHECK(pluginWanted(u));
            CHECK(std::string(pluginUseSignal(u)) == row.words);
        }
        // The first signal in the struct's order names the want when several are set.
        PluginUse many;
        many.mapPage = true;
        many.browserSession = true;
        many.textSink = true;
        CHECK(std::string(pluginUseSignal(many)) == "map page open");
        PluginUse all;
        for (const SignalRow& row : kSignals) { row.set(all); }
        CHECK(pluginWanted(all));
        CHECK(std::string(pluginUseSignal(all)) == "window open");
        // The nine signals are nine different words.
        for (std::size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i) {
            for (std::size_t j = i + 1; j < sizeof kSignals / sizeof kSignals[0]; ++j) {
                CHECK(std::string(kSignals[i].words) != kSignals[j].words);
            }
        }
    }

    // --- AUTO: a wake is immediate; a sleep waits the whole 30 seconds ----------------------
    {
        PluginLife life;
        // not wanted, not running: nothing happens, ever
        for (std::int64_t t = 0; t < 3 * k30s; t += 1000) {
            CHECK(stepPluginLife(PluginRun::Auto, false, t, k30s, life) == PluginLifeStep::None);
        }
        CHECK(!life.running);
        // wanted: it wakes on THE VERY FRAME, and says so once
        const std::int64_t t0 = 100000;
        CHECK(stepPluginLife(PluginRun::Auto, true, t0, k30s, life) == PluginLifeStep::Wake);
        CHECK(life.running);
        CHECK(life.lastWantedMs == t0);
        CHECK(stepPluginLife(PluginRun::Auto, true, t0 + 16, k30s, life) == PluginLifeStep::None);
        // the want goes: it keeps running through the whole hysteresis, down to the last
        // millisecond before it
        const std::int64_t lastWanted = t0 + 5000;
        CHECK(stepPluginLife(PluginRun::Auto, true, lastWanted, k30s, life) == PluginLifeStep::None);
        CHECK(stepPluginLife(PluginRun::Auto, false, lastWanted + 1, k30s, life) == PluginLifeStep::None);
        CHECK(stepPluginLife(PluginRun::Auto, false, lastWanted + k30s - 1, k30s, life) ==
              PluginLifeStep::None);
        CHECK(life.running);
        // wanted AGAIN inside the window resets the clock (toggling a window does not thrash)
        CHECK(stepPluginLife(PluginRun::Auto, true, lastWanted + k30s - 1, k30s, life) ==
              PluginLifeStep::None);
        CHECK(life.lastWantedMs == lastWanted + k30s - 1);
        CHECK(stepPluginLife(PluginRun::Auto, false, lastWanted + k30s + 29000, k30s, life) ==
              PluginLifeStep::None);
        CHECK(life.running);
        // ...and at exactly 30 s after the LAST want it goes dormant, once
        CHECK(stepPluginLife(PluginRun::Auto, false, lastWanted + k30s - 1 + k30s, k30s, life) ==
              PluginLifeStep::Sleep);
        CHECK(!life.running);
        CHECK(stepPluginLife(PluginRun::Auto, false, lastWanted + 10 * k30s, k30s, life) ==
              PluginLifeStep::None);
        // and wakes again the frame it is wanted again
        CHECK(stepPluginLife(PluginRun::Auto, true, lastWanted + 11 * k30s, k30s, life) ==
              PluginLifeStep::Wake);
        CHECK(life.running);
    }

    // --- the interval is a parameter: the test seam's whole job -------------------------------
    {
        PluginLife life;
        CHECK(stepPluginLife(PluginRun::Auto, true, 0, 300, life) == PluginLifeStep::Wake);
        CHECK(stepPluginLife(PluginRun::Auto, false, 299, 300, life) == PluginLifeStep::None);
        CHECK(stepPluginLife(PluginRun::Auto, false, 300, 300, life) == PluginLifeStep::Sleep);
    }

    // --- ALWAYS never goes dormant, and un-pinning it starts the clock from the last frame ----
    {
        PluginLife life;
        CHECK(stepPluginLife(PluginRun::Always, false, 1000, k30s, life) == PluginLifeStep::Wake);
        CHECK(life.running);
        for (std::int64_t t = 2000; t < 10 * k30s; t += 7000) {
            CHECK(stepPluginLife(PluginRun::Always, false, t, k30s, life) == PluginLifeStep::None);
            CHECK(life.running);
        }
        // un-pinned to AUTO at t = 400000, with nothing wanting it: it is NOT put out that frame
        // (its clock was kept current while it was pinned), and goes 30 s after the last pinned one
        const std::int64_t lastPinned = life.lastWantedMs;
        CHECK(lastPinned > 0);
        CHECK(stepPluginLife(PluginRun::Auto, false, lastPinned + 1, k30s, life) == PluginLifeStep::None);
        CHECK(life.running);
        CHECK(stepPluginLife(PluginRun::Auto, false, lastPinned + k30s, k30s, life) ==
              PluginLifeStep::Sleep);
        // an ALWAYS plugin that is not running (a restart, a rescan) is woken
        PluginLife fresh;
        CHECK(stepPluginLife(PluginRun::Always, false, 5, k30s, fresh) == PluginLifeStep::Wake);
    }

    // --- STOPPED never runs, whatever wants it ------------------------------------------------
    {
        PluginLife life;
        CHECK(stepPluginLife(PluginRun::Stopped, true, 10, k30s, life) == PluginLifeStep::None);
        CHECK(!life.running);
        CHECK(stepPluginLife(PluginRun::Stopped, true, 20, k30s, life) == PluginLifeStep::None);
        // found running (it was stopped while awake): put down at once, once
        life.running = true;
        CHECK(stepPluginLife(PluginRun::Stopped, true, 30, k30s, life) == PluginLifeStep::Sleep);
        CHECK(!life.running);
        CHECK(stepPluginLife(PluginRun::Stopped, false, 40, k30s, life) == PluginLifeStep::None);
    }

    // --- which modules have a run state at all ------------------------------------------------
    {
        const std::uint32_t wake[] = {CASCADE_CAP_DECODER,   CASCADE_CAP_IQ_DECODER, CASCADE_CAP_IMAGE_DECODER,
                                      CASCADE_CAP_TRACK_SOURCE, CASCADE_CAP_PANEL,      CASCADE_CAP_INSTRUMENT};
        for (std::uint32_t bit : wake) {
            CHECK(pluginCapsHaveLifecycle(bit));
            CHECK(!pluginCapsStandingDuty(bit));
            CHECK(!pluginCapsStandingDuty(bit | CASCADE_CAP_HOST_CLIENT));  // something else can wake it
        }
        // an in-chain processor and a bare host client are in use for as long as they are fitted
        CHECK(pluginCapsHaveLifecycle(CASCADE_CAP_AUDIO_PROCESSOR));
        CHECK(pluginCapsStandingDuty(CASCADE_CAP_AUDIO_PROCESSOR));
        CHECK(pluginCapsStandingDuty(CASCADE_CAP_AUDIO_PROCESSOR | CASCADE_CAP_DECODER));
        CHECK(pluginCapsHaveLifecycle(CASCADE_CAP_HOST_CLIENT));
        CHECK(pluginCapsStandingDuty(CASCADE_CAP_HOST_CLIENT));
        CHECK(pluginCapsStandingDuty(CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_SETTINGS_UI));
        // a module with nothing to start has no run state: neither idle nor running
        for (std::uint32_t bit : {CASCADE_CAP_BASEMAP, CASCADE_CAP_TRACK_INFO, CASCADE_CAP_PRESET,
                                  CASCADE_CAP_SETTINGS_UI, CASCADE_CAP_AUDIO_OUT}) {
            CHECK(!pluginCapsHaveLifecycle(bit));
            CHECK(!pluginCapsStandingDuty(bit));
        }
        CHECK(!pluginCapsHaveLifecycle(0u));
        CHECK(!pluginCapsStandingDuty(0u));
    }

    return testSummary("test_plugin_run");
}
