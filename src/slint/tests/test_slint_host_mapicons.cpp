// Which aircraft silhouette a track draws (window-gaps lane 4, row 493).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mapicons_logic.hpp"
#include "test_check.hpp"

#include "core/plugin_abi.h"

using namespace foxsdr::slinthost;

int main() {
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_NONE, nullptr) == 9);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_LIGHT, nullptr) == 2);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_MEDIUM1, nullptr) == 0);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_MEDIUM2, nullptr) == 0);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_HIGH_VORTEX, nullptr) == 0);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_HEAVY, nullptr) == 1);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_HIGH_PERF, nullptr) == 0);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_ROTORCRAFT, nullptr) == 8);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_GLIDER, nullptr) == 3);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_LIGHTER_THAN_AIR, nullptr) == 6);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_PARACHUTIST, nullptr) == 6);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_ULTRALIGHT, nullptr) == 4);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_UAV, nullptr) == 5);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_SPACE, nullptr) == 0);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_SURFACE_EMERGENCY, nullptr) == 7);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_SURFACE_SERVICE, nullptr) == 7);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_GROUND_OBSTRUCTION, nullptr) == 7);
    CHECK(aircraftIconIndex(99u, nullptr) == 9);                       // a category a newer plugin invented: no type stated
    for (std::uint32_t c = 0; c < CASCADE_AIRCRAFT_CATEGORY_COUNT; ++c) { const int i = aircraftIconIndex(c, nullptr); CHECK(i >= 0 && i < kAircraftIconCount); }
    // a single-engine helicopter type draws the light helicopter, any other rotorcraft the five-blade one, and only a rotorcraft changes
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_ROTORCRAFT, "R44") == 10);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_ROTORCRAFT, " r 44 ") == 10);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_ROTORCRAFT, "EC35") == 8);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_ROTORCRAFT, "") == 8);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_ROTORCRAFT, "TOOLONGTYPE") == 8);
    CHECK(aircraftIconIndex(CASCADE_AIRCRAFT_LIGHT, "R44") == 2);
    return testSummary("test_slint_host_mapicons");
}
