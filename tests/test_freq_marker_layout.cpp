// Tests for gui/freq_marker_layout.hpp - which frequency markers are on the
// waterfall and where their tabs sit along its foot.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/freq_marker_layout.hpp"

#include <cstdio>
#include <vector>

#include "test_check.hpp"

using cascade::gui::layoutMarkerTabs;
using cascade::gui::MarkerTabIn;
using cascade::gui::markerScreenX;

namespace {

void testScreenX() {
    std::printf("  on-panel markers map linearly; off-panel and degenerate views draw nothing\n");
    float x = -1.0f;
    CHECK(markerScreenX(145.0e6, 144.0e6, 146.0e6, 100.0f, 1000.0f, x));
    CHECK_NEAR(x, 600.0f, 1e-3);
    CHECK(markerScreenX(144.0e6, 144.0e6, 146.0e6, 100.0f, 1000.0f, x));
    CHECK_NEAR(x, 100.0f, 1e-3);
    CHECK(markerScreenX(146.0e6, 144.0e6, 146.0e6, 100.0f, 1000.0f, x));
    CHECK_NEAR(x, 1100.0f, 1e-3);
    CHECK(!markerScreenX(143.9e6, 144.0e6, 146.0e6, 100.0f, 1000.0f, x));
    CHECK(!markerScreenX(146.1e6, 144.0e6, 146.0e6, 100.0f, 1000.0f, x));
    CHECK(!markerScreenX(145.0e6, 146.0e6, 146.0e6, 100.0f, 1000.0f, x));  // no span
    CHECK(!markerScreenX(145.0e6, 144.0e6, 146.0e6, 100.0f, 0.0f, x));     // no width
}

void testFlipAndStack() {
    std::printf("  tabs read right, flip left at the edge, stack upward, and give up past the budget\n");
    // Panel 0..500; tabs 80 wide.
    const std::vector<MarkerTabIn> in = {
        {100.0f, 80.0f},  // row 0, [100, 180]
        {150.0f, 80.0f},  // touches the first: row 1
        {160.0f, 80.0f},  // touches both: row 2
        {170.0f, 80.0f},  // touches all three rows: no tab
        {300.0f, 80.0f},  // clear: row 0 again
        {480.0f, 80.0f},  // would run off: flips to [400, 480], row 0
        {20.0f, 600.0f},  // wider than the room either side: no tab
    };
    const auto out = layoutMarkerTabs(in, 0.0f, 500.0f, 3);
    CHECK(out.size() == in.size());
    if (out.size() != in.size()) { return; }
    CHECK(out[0].row == 0 && out[0].a == 100.0f && out[0].e == 180.0f);
    CHECK(out[1].row == 1);
    CHECK(out[2].row == 2);
    CHECK(out[3].row == -1);
    CHECK(out[4].row == 0 && out[4].a == 300.0f);
    CHECK(out[5].row == 0 && out[5].a == 400.0f && out[5].e == 480.0f);
    CHECK(out[6].row == -1);

    // One row only: the second touching tab loses its plate, not its place.
    const auto one = layoutMarkerTabs({{100.0f, 80.0f}, {150.0f, 80.0f}}, 0.0f, 500.0f, 1);
    CHECK(one.size() == 2u && one[0].row == 0 && one[1].row == -1);
    // No rows at all: nothing placed.
    const auto none = layoutMarkerTabs({{100.0f, 80.0f}}, 0.0f, 500.0f, 0);
    CHECK(none.size() == 1u && none[0].row == -1);
}

void testOccupied() {
    std::printf("  a row already carrying the foot plate is taken: tabs go above it, or beside it\n");
    // The foot plate covers [10, 240] on row 0 only.
    const std::vector<std::vector<std::pair<float, float>>> plate = {{{10.0f, 240.0f}}};
    const auto out = layoutMarkerTabs({{100.0f, 80.0f}, {300.0f, 80.0f}}, 0.0f, 500.0f, 3, 3.0f, plate);
    CHECK(out.size() == 2u);
    if (out.size() != 2u) { return; }
    CHECK(out[0].row == 1);  // over the plate: moved up a row
    CHECK(out[1].row == 0);  // clear of it: stays on the foot
    // Occupied rows past the budget are ignored, not indexed.
    const std::vector<std::vector<std::pair<float, float>>> tall = {
        {{0.0f, 500.0f}}, {{0.0f, 500.0f}}, {{0.0f, 500.0f}}, {{0.0f, 500.0f}}};
    const auto blocked = layoutMarkerTabs({{100.0f, 80.0f}}, 0.0f, 500.0f, 2, 3.0f, tall);
    CHECK(blocked.size() == 1u && blocked[0].row == -1);
}

}  // namespace

int main() {
    testScreenX();
    testFlipAndStack();
    testOccupied();
    return testSummary("test_freq_marker_layout");
}
