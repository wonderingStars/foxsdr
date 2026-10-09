// Stopping a decoder from the screen you are already looking at.
//
// THE REPORT THIS EXISTS FOR, through the owner (2026-09-21): a beta tester
// ends up with several decoders running that are not applicable, "and then it
// can start to hiccup and stutter on other decodes. So I currently go into the
// plugins tab and manually stop the other decoders."
//
// The rail now carries a STOP key per decoder and a STOP ALL key, so the whole
// of that trip is one press. What is asserted here is the decision half - who
// counts as running, which key a row offers, when STOP ALL appears and exactly
// which modules it would stop - because that is the half that can be wrong
// without looking wrong.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/running_view.hpp"

#include "test_check.hpp"

#include <string>
#include <vector>

namespace {

using cascade::gui::RunnableDecoder;

RunnableDecoder mk(const std::string& name, bool stopped, bool feeding) {
    RunnableDecoder d;
    d.name = name;
    d.key = name + "-decoder.dll";
    d.stopped = stopped;
    d.feeding = feeding;
    return d;
}

std::vector<std::string> namesOf(const std::vector<RunnableDecoder>& v) {
    std::vector<std::string> out;
    for (const RunnableDecoder& d : v) { out.push_back(d.name); }
    return out;
}

}  // namespace

int main() {
    using namespace cascade::gui;

    // --- RUNNING MEANS BEING FED ------------------------------------------
    //
    // All four cells of the truth table, because three of them look like
    // "running" from some angle: a stopped module is still loaded, an unfed
    // one is still switched on, and only the pair means the machine is
    // actually doing work for it.
    CHECK(decoderIsRunning(mk("ADS-B", false, true), true));
    CHECK(!decoderIsRunning(mk("ADS-B", true, true), true));    // stopped by the user
    CHECK(!decoderIsRunning(mk("ADS-B", false, false), true));  // armed, no samples
    CHECK(!decoderIsRunning(mk("ADS-B", true, false), true));

    // AND NOTHING RUNS WHILE THE RECEIVER IS STOPPED. Found by the first
    // self-capture of this feature, not by reasoning: the runner keeps its
    // instances when the radio stops, so isFeeding() still answers true, and
    // the rail drew "2 decoders are running at once" immediately above its own
    // DECODERS chip reading "0 FED". The chip gates on the pipeline; so does
    // this, or one rail says two different things in one glance.
    CHECK(!decoderIsRunning(mk("ADS-B", false, true), false));

    // The tester's own situation: four fitted, two of them actually running.
    const std::vector<RunnableDecoder> bench{
        mk("ACARS", false, true),
        mk("POCSAG", false, true),
        mk("FLEX", true, false),    // already stopped by hand
        mk("SSTV", false, false),   // fitted, not being fed
    };
    CHECK(runningCount(bench, true) == 2);
    CHECK(runningCount(bench, false) == 0);  // receiver stopped: nothing is running
    const std::vector<std::string> wantRunning{"ACARS", "POCSAG"};
    CHECK(namesOf(runningDecoders(bench, true)) == wantRunning);
    CHECK(runningDecoders(bench, false).empty());

    // --- THE KEY ON A ROW --------------------------------------------------
    //
    // One key, and it is the one that changes what the user is looking at. An
    // armed-but-unfed decoder still offers STOP: it will start the moment the
    // receiver does, and saying "I do not want this" must not require starting
    // the radio first.
    CHECK(std::string(rowKeyLabel(mk("ACARS", false, true))) == "STOP");
    CHECK(std::string(rowKeyLabel(mk("SSTV", false, false))) == "STOP");
    CHECK(std::string(rowKeyLabel(mk("FLEX", true, false))) == "START");
    CHECK(rowOffersStop(mk("ACARS", false, true)));
    CHECK(!rowOffersStop(mk("FLEX", true, false)));

    // --- STOP ALL ----------------------------------------------------------
    //
    // Only when there is more than one thing to stop: beside a single STOP key
    // a second key that does the same thing is noise.
    CHECK(!showStopAll(0));
    CHECK(!showStopAll(1));
    CHECK(showStopAll(2));
    CHECK(showStopAll(7));
    CHECK(stopAllLabel(2) == "STOP ALL 2 RUNNING");
    CHECK(stopAllLabel(11) == "STOP ALL 11 RUNNING");

    // AND IT STOPS EXACTLY WHAT IS RUNNING. Not the one the user already
    // stopped (which would be a restart in disguise if the caller toggled),
    // and not the fitted-but-idle one, which is not what is eating the
    // machine. Keys, not display names: two modules may print the same name.
    const std::vector<std::string> wantKeys{"ACARS-decoder.dll", "POCSAG-decoder.dll"};
    CHECK(stopAllKeys(bench, true) == wantKeys);
    CHECK(stopAllKeys(bench, false).empty());
    CHECK(stopAllKeys({}, true).empty());
    const std::vector<RunnableDecoder> noneRunning{mk("FLEX", true, false)};
    CHECK(stopAllKeys(noneRunning, true).empty());

    // --- WHAT THE RAIL SAYS ABOUT THE COST ---------------------------------
    //
    // Silent at one decoder - one running decoder is the ordinary case and a
    // warning about it would be crying wolf - and in the tester's terms above
    // it.
    CHECK(runningCostNote(0).empty());
    CHECK(runningCostNote(1).empty());
    CHECK(runningCostNote(3).find("3 decoders are running") != std::string::npos);
    CHECK(runningCostNote(3).find("stutter") != std::string::npos);

    // --- AN IDLE DECODER (0.99.73) -----------------------------------------
    //
    // A decoder set to AUTO that nothing is using has no instance: it is not stopped by the user
    // and not failing to be fed, it simply is not running, and costs nothing. Three consequences,
    // each of which would read wrong if it slipped:
    //   - it is never counted among the RUNNING ones, in either receiver state (the "N decoders are
    //     running at once" warning and STOP ALL must not count a sleeping module);
    //   - its row offers START, not STOP - a press pins it ("keep running"), as it does for a
    //     stopped one;
    //   - STOP ALL does not touch it (it is not what is eating the machine) and does not wake it.
    RunnableDecoder idle = mk("DMR", false, false);
    idle.idle = true;
    CHECK(!decoderIsRunning(idle, true));
    CHECK(!decoderIsRunning(idle, false));
    CHECK(!rowOffersStop(idle));
    CHECK(rowOffersStop(mk("DMR", false, false)));  // the same module without the flag: armed, STOP
    CHECK(std::string(rowKeyLabel(idle)) == "START");
    // a record cannot be idle and stopped, but if it were, START is still the only honest key
    RunnableDecoder idleStopped = mk("DMR", true, false);
    idleStopped.idle = true;
    CHECK(!rowOffersStop(idleStopped));
    CHECK(std::string(rowKeyLabel(idleStopped)) == "START");
    // The tester's bench again, with twenty-eight idle decoders beside the two that run: the
    // warning is about the two, and the key stops the two.
    std::vector<RunnableDecoder> many = bench;
    for (int i = 0; i < 28; ++i) {
        RunnableDecoder d = mk("IDLE" + std::to_string(i), false, false);
        d.idle = true;
        many.push_back(d);
    }
    CHECK(many.size() == 32u);
    CHECK(runningCount(many, true) == 2);
    CHECK(runningCostNote(runningCount(many, true)).find("2 decoders are running") != std::string::npos);
    CHECK(stopAllKeys(many, true) == wantKeys);
    CHECK(runningDecoders(many, true).size() == 2u);
    // ...and a bench of nothing but idle decoders has nothing running and nothing to stop.
    std::vector<RunnableDecoder> allIdle(many.begin() + 4, many.end());
    CHECK(runningCount(allIdle, true) == 0);
    CHECK(!showStopAll(runningCount(allIdle, true)));
    CHECK(stopAllKeys(allIdle, true).empty());
    CHECK(runningCostNote(runningCount(allIdle, true)).empty());

    return testSummary("test_running_view");
}
