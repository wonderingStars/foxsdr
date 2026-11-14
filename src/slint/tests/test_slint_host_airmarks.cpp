// Where the Airband monitor's channels lie on the spectrum (window-gaps lane 4, rows 264 to 267).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include "airmarks_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slinthost;

int main() {
    const double centre = 119.0e6, span = 2.0e6;
    {   // a 10 kHz channel on a 2 MHz span in 1000 px: the true width is 0.005 of the plot (5 px), more than the 3 px floor
        const auto m = airbandMarks({{119.0e6, 10e3, true}}, centre, span, 1000);
        CHECK(m.size() == 1);
        CHECK(m[0].open);
        CHECK_NEAR(m[0].from, 0.5 - 0.0025, 1e-6);
        CHECK_NEAR(m[0].to, 0.5 + 0.0025, 1e-6);
    }
    {   // a channel narrower than 3 px is widened to 3 px, centred
        const auto m = airbandMarks({{119.0e6, 1e3, false}}, centre, span, 1000);
        CHECK(m.size() == 1);
        CHECK(!m[0].open);
        CHECK_NEAR(m[0].to - m[0].from, 0.003, 1e-6);
        CHECK_NEAR((m[0].to + m[0].from) / 2, 0.5, 1e-6);
    }
    {   // zoomed in far enough the true width wins
        const auto m = airbandMarks({{119.0e6, 10e3, true}}, centre, 20e3, 1000);
        CHECK_NEAR(m[0].to - m[0].from, 0.5, 1e-6);
    }
    {   // a centre outside the view has no mark; the edges are inside; NaN is outside
        CHECK(airbandMarks({{117.9e6, 10e3, true}}, centre, span, 1000).empty());
        CHECK(airbandMarks({{120.1e6, 10e3, true}}, centre, span, 1000).empty());
        CHECK(airbandMarks({{118.0e6, 10e3, true}}, centre, span, 1000).size() == 1);
        CHECK(airbandMarks({{std::nan(""), 10e3, true}}, centre, span, 1000).empty());
        const auto edge = airbandMarks({{120.0e6, 10e3, true}}, centre, span, 1000);
        CHECK(edge.size() == 1 && edge[0].to <= 1.0f);
    }
    {   // an unusable plot or span, an empty list
        CHECK(airbandMarks({{119e6, 1e4, true}}, centre, 0, 1000).empty());
        CHECK(airbandMarks({{119e6, 1e4, true}}, centre, span, 0).empty());
        CHECK(airbandMarks({}, centre, span, 1000).empty());
        const auto two = airbandMarks({{118.1e6, 10e3, true}, {119.9e6, 10e3, false}}, centre, span, 1000);
        CHECK(two.size() == 2 && two[0].open && !two[1].open);
    }
    return testSummary("test_slint_host_airmarks");
}
