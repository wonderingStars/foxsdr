// A monitor that cannot be queried for minutes is ONE display change, not one a
// frame (gui/display_query_episode.hpp, the 2026-10-28 tester log: GLFW error
// 65544 on every frame, two log lines a frame, the hang watchdog's grace restarted
// every frame for the whole episode).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/display_query_episode.hpp"

#include "test_check.hpp"

#include <cmath>

namespace {

using cascade::gui::DisplayQueryEpisode;

constexpr double kFrame = 1.0 / 30.0;

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

// Nothing fails: no edges, ever - and a counter that already stood at five when the
// frame loop started is not an episode either.
void checkQuiet() {
    DisplayQueryEpisode ep(5u);
    for (int f = 0; f < 100; ++f) {
        const DisplayQueryEpisode::Edge e = ep.update(f * kFrame, 5u);
        CHECK(!e.began);
        CHECK(!e.ended);
    }
    CHECK(!ep.open());
    CHECK(ep.failures() == 0u);
}

// THE FIELD FLOOD: a failure on every frame. One beginning, on the first failed
// frame; nothing more while it goes on; and the end, with the whole count and the
// span, only after kQuietS without a failure.
void checkFloodIsOneEpisode() {
    DisplayQueryEpisode ep(0u);
    unsigned total = 0;
    int begins = 0;
    int ends = 0;
    double t = 0.0;
    for (int f = 0; f < 200; ++f, t += kFrame) {
        ++total;
        const DisplayQueryEpisode::Edge e = ep.update(t, total);
        if (e.began) {
            ++begins;
            CHECK(f == 0);
        }
        if (e.ended) { ++ends; }
    }
    CHECK(begins == 1);
    CHECK(ends == 0);
    CHECK(ep.open());
    CHECK(ep.failures() == 200u);
    const double last = t - kFrame;

    // The queries answer again. A frame 0.9 s on - the longest gap inside the
    // field flood - is not the end; the end comes at the quiet period.
    DisplayQueryEpisode::Edge e = ep.update(last + 0.9, total);
    CHECK(!e.began);
    CHECK(!e.ended);
    CHECK(ep.open());
    // A hair past the quiet period: 200 summed frame times do not land on an
    // exact double, and the boundary itself is checked below with plain values.
    e = ep.update(last + DisplayQueryEpisode::kQuietS + 1e-9, total);
    CHECK(e.ended);
    CHECK(!e.began);
    CHECK(e.failures == 200u);
    CHECK(near(e.seconds, last));
    CHECK(!ep.open());

    // ...and said once: the frame after the end has nothing to say.
    e = ep.update(last + DisplayQueryEpisode::kQuietS + 1e-9 + kFrame, total);
    CHECK(!e.began);
    CHECK(!e.ended);
}

// A GAP SHORTER THAN THE QUIET PERIOD DOES NOT SPLIT AN EPISODE: failures at 0.0,
// 0.5 and 1.4 s (a 0.9 s gap) are one episode of three, ended 2 s after the last.
void checkShortGapKeepsItOpen() {
    DisplayQueryEpisode ep(0u);
    CHECK(ep.update(0.0, 1u).began);
    CHECK(!ep.update(0.25, 1u).began);
    DisplayQueryEpisode::Edge e = ep.update(0.5, 2u);
    CHECK(!e.began);
    CHECK(!e.ended);
    e = ep.update(1.4, 3u);
    CHECK(!e.began);
    CHECK(!e.ended);
    e = ep.update(1.4 + DisplayQueryEpisode::kQuietS - 0.01, 3u);
    CHECK(!e.ended);
    e = ep.update(1.4 + DisplayQueryEpisode::kQuietS, 3u);
    CHECK(e.ended);
    CHECK(e.failures == 3u);
    CHECK(near(e.seconds, 1.4));
}

// A SECOND EPISODE after the first ended begins afresh, with its own count.
void checkSecondEpisode() {
    DisplayQueryEpisode ep(0u, 1.0);
    CHECK(ep.update(0.0, 4u).began);  // several failures in one frame all count
    CHECK(ep.failures() == 4u);
    DisplayQueryEpisode::Edge e = ep.update(1.0, 4u);
    CHECK(e.ended);
    CHECK(e.failures == 4u);
    CHECK(near(e.seconds, 0.0));
    e = ep.update(5.0, 5u);
    CHECK(e.began);
    CHECK(!e.ended);
    CHECK(ep.failures() == 1u);
    e = ep.update(6.0, 5u);
    CHECK(e.ended);
    CHECK(e.failures == 1u);
}

// A WRAPPED COUNTER still yields the new failures (unsigned difference).
void checkWrap() {
    DisplayQueryEpisode ep(0xFFFFFFFEu);
    CHECK(!ep.update(0.0, 0xFFFFFFFEu).began);
    CHECK(ep.update(0.1, 1u).began);  // three new failures across the wrap
    CHECK(ep.failures() == 3u);
}

}  // namespace

int main() {
    checkQuiet();
    checkFloodIsOneEpisode();
    checkShortGapKeepsItOpen();
    checkSecondEpisode();
    checkWrap();
    return testSummary("test_display_query_episode");
}
