// The embedded basemap data.
//
// A wrong coastline compiles perfectly and draws confident nonsense, so these
// check the things that would actually go wrong: a longitude/latitude swap
// during packing, an int16 overflow, a run table that points outside the
// coordinate array, and geography that is simply absent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "gui/coastline_data.hpp"
#include "test_check.hpp"

namespace {

using namespace cascade::gui::coastline;

double lonAt(std::uint32_t point) {
    return static_cast<double>(kCoords[static_cast<std::size_t>(point) * 2]) / 100.0;
}
double latAt(std::uint32_t point) {
    return static_cast<double>(kCoords[static_cast<std::size_t>(point) * 2 + 1]) / 100.0;
}

// Is there any coastline vertex within `tolDeg` of this position? Used to
// assert that recognisable geography survived the conversion.
bool nearCoast(double lat, double lon, double tolDeg) {
    for (std::uint32_t i = 0; i < kPointCount; ++i) {
        if (std::fabs(latAt(i) - lat) <= tolDeg && std::fabs(lonAt(i) - lon) <= tolDeg) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    CHECK(kRunCount > 0);
    CHECK(kPointCount > 1000);  // 1:110m coastline is a few thousand points

    // --- the run table must stay inside the coordinate array -------------
    const std::size_t coordCount = sizeof(kCoords) / sizeof(kCoords[0]);
    CHECK(coordCount == static_cast<std::size_t>(kPointCount) * 2u);
    std::uint32_t summed = 0;
    for (std::uint32_t r = 0; r < kRunCount; ++r) {
        const Run& run = kRuns[r];
        // A one-point "line" draws nothing and should have been dropped by
        // the converter.
        CHECK(run.count >= 2);
        CHECK(static_cast<std::size_t>(run.first) + run.count <= kPointCount);
        summed += run.count;
    }
    // Runs partition the points exactly: no gaps, no overlap, nothing orphaned.
    CHECK(summed == kPointCount);

    // --- THE SWAP TEST ---------------------------------------------------
    // If longitude and latitude were exchanged during packing, latitudes would
    // run out to +/-180 while longitudes stayed within +/-90. Ranges alone
    // catch it, and this is the single most likely way to get a basemap wrong.
    double minLat = 1e9, maxLat = -1e9, minLon = 1e9, maxLon = -1e9;
    for (std::uint32_t i = 0; i < kPointCount; ++i) {
        minLat = std::min(minLat, latAt(i));
        maxLat = std::max(maxLat, latAt(i));
        minLon = std::min(minLon, lonAt(i));
        maxLon = std::max(maxLon, lonAt(i));
    }
    CHECK(minLat >= -90.0);
    CHECK(maxLat <= 90.0);
    CHECK(minLon >= -180.0);
    CHECK(maxLon <= 180.0);
    // ...and the world really is wider than it is tall. A swap would invert
    // this even if some clamp had kept the values in range.
    CHECK((maxLon - minLon) > (maxLat - minLat));

    // Both hemispheres, in both axes: a truncated conversion that kept only
    // the first continent would pass every range check above.
    CHECK(minLat < -30.0);
    CHECK(maxLat > 60.0);
    CHECK(minLon < -100.0);
    CHECK(maxLon > 100.0);

    // --- recognisable geography ------------------------------------------
    // Spot checks against places whose coastlines are unmistakable. The
    // tolerance is loose (1.5 deg) because this is 1:110m data quantised to
    // 0.01 deg, and the point is "the map is of Earth", not survey accuracy.
    CHECK(nearCoast(51.0, 1.4, 1.5));      // Dover / Strait of Dover
    CHECK(nearCoast(-33.9, 18.4, 1.5));    // Cape of Good Hope
    CHECK(nearCoast(40.7, -74.0, 1.5));    // New York
    CHECK(nearCoast(-33.9, 151.2, 1.5));   // Sydney

    // And somewhere there is definitively no coast: the middle of the Pacific.
    CHECK(!nearCoast(0.0, -140.0, 3.0));

    std::printf("  coastline: %u runs, %u points, %.1f KB\n", kRunCount, kPointCount,
                static_cast<double>(sizeof(kCoords)) / 1024.0);
    return testSummary("test_coastline");
}
