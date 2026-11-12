// mapj_state.hpp - SCREEN LANE J: the state the map pages and the Sky and Antenna pages keep (docs/multi-radio-records/screen-j.md). Header only, no Slint.
// MapJSurface is one map page's (inside KitPublisher::MapSurface); MapJShared is the publisher's (the signal log, the worked / heard marks and the seams a later lane wires).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/access_units.hpp"
#include "core/adsb_coverage.hpp"
#include "core/rf_position_log.hpp"
#include "mapj_logic.hpp"

namespace foxsdr::slintmapj {

struct MapJSurface {
    LayerSet layers = LayerSet::defaults();
    Replay replay;
    TrackRecorder recorder;
    AircraftFilter filter;
    cascade::core::access::UnitSystem units = cascade::core::access::UnitSystem::Automatic;
    bool layersOpen = false, replayBar = false, filterBar = false;
    // the ADS-B coverage model of this page: built when the receiver position is known, rebuilt when it moves (session only: owner decision 5 is open)
    std::unique_ptr<cascade::core::adsbcov::AdsbCoverage> coverage;
    double covLat = 0.0, covLon = 0.0;
    double lastFeedSec = -1.0, lastTickSec = -1.0;
    std::string altMinText, altMaxText, altMinError, altMaxError;   // what was typed (echoed back, with the error beside it)
    int hiddenByFilter = 0;
    int cellCount = 0, gridLevel = 0, labelCount = 0, lineCount = 0;   // what the last compose was handed (the drive scripts read them)
};

struct MapJShared {
    cascade::core::rfpos::RfPositionLog rfLog;                       // fed by the GPS reader and the pass thread once an engine lane wires them (hand-off); a bounded run fills it from a generated track
    std::vector<LocatorMark> locatorMarks;                           // the ft8-decoder plugin's worked / heard sets (hand-off); a test double fills it
    std::function<std::vector<PassSample>()> passProvider;           // the satellites plugin's az/el of a pass (hand-off)
    std::function<bool(const std::string& id, FilterSubject& subject)> filterDetail;   // the ADS-B plugin's type, operator and military (hand-off): true when it knew the aircraft
    std::string tileCacheRoot;                                       // "" = no cache folder known: the line says there is none
    AntennaInput antenna;
    std::array<std::string, 5> antennaText{"14.2", "100", "0.25", "0.66", "3"};
    std::string antennaError;
    bool testFeedsDone = false;
    std::string cacheLine;                                           // the tile cache's words, read every few seconds, not every tick
    bool cacheHas = false;
    double cacheReadSec = -100.0;
    int skyRows = 0, antennaRows = 0;
    std::string sunEl;                                               // the Sun elevation as the Sky page last showed it (the drive scripts read it)
};

}  // namespace foxsdr::slintmapj
