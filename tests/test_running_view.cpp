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

    return testSummary("test_running_view");
}
