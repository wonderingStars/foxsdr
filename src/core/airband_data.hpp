// airband_data.hpp - "type the airport, get its frequencies".
//
// THE REQUEST (a US tester, 2026-10-01): to listen to air traffic control he
// searched the web for the frequencies of his nearest airport, Chicago O'Hare,
// and copied a list of twenty-three by hand. This is that list, for every
// airport, without the web: a table compiled into the binary
// (core/airband_assets.hpp, built by tools/make-airband-data.py from the FAA's
// NASR frequency file for US airports and OurAirports for the rest of the
// world, both public domain), parsed once on first use.
//
// WHAT AN AIRPORT CODE IS. People type what they know: the ICAO code (KORD,
// EGLL), the three letters on a luggage tag (ORD, LHR), or in the US the FAA's
// own location id (ORD again, or 3-character ids for small fields that have no
// ICAO code at all). findAirports() matches any of the three, case-blind, and
// returns the exact-ICAO match first when one code means more than one place.
//
// WHAT A CHANNEL IS. On the 8.33 kHz raster used across Europe a published
// "frequency" is a channel NAME: 118.010 is the second 8.33 kHz channel above
// 118.000, which is 118.00833 MHz. airbandChannelHz() is the one place that
// conversion lives. On the 25 kHz raster (the US, and most of the world's 25
// kHz channels) the name and the frequency are the same number.
//
// Pure apart from the one-time parse of the embedded table: no ImGui, no
// AppWindow. tests/test_airband_data.cpp pins the parse, the lookup, the
// channel rule and the nearest-airport ordering.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/freq_manager.hpp"

namespace cascade::core {

struct AirbandFreq {
    int channelKhz = 0;   // as published: 121600, or 121935 for an 8.33 channel
    double hz = 0.0;      // what the radio is tuned to: airbandChannelHz(channelKhz)
    std::string type;     // TWR, GND, CLD, APP, DEP, ATIS, CTAF ...
    std::string desc;     // sector and use, as the source words it; may be empty
};

struct Airport {
    std::string ident;    // ICAO where there is one (KORD), else the source's id
    std::string iata;     // ORD; may be empty
    std::string local;    // the FAA location id in the US; may be empty
    std::string country;  // ISO 3166 alpha-2
    std::string name;
    double latDeg = 0.0;
    double lonDeg = 0.0;
    std::vector<AirbandFreq> freqs;   // ascending by channel
};

// The frequency an airband channel name is tuned to. 25 kHz names map to
// themselves; an 8.33 kHz name (kHz mod 25 is 5, 10 or 15) maps to the 25 kHz
// slot below it plus 0, 8.333 or 16.667 kHz. Anything else (a value off both
// rasters, as some sources list) is taken literally.
double airbandChannelHz(int channelKhz);

// Whether a channel name is on the 8.33 kHz raster rather than the 25 kHz one.
bool airband833(int channelKhz);

// Parses the table format tools/make-airband-data.py writes. Lines that do
// not parse are skipped (and counted in `skipped`); an F line before any A
// line is skipped. Airports end up in file order, frequencies ascending.
std::vector<Airport> parseAirbandTable(std::string_view text, std::size_t* skipped = nullptr);

// The table compiled into this build, parsed on first call (thread-safe).
const std::vector<Airport>& airbandTable();

// Every airport whose ident, IATA code or FAA local id equals `code`, ignoring
// case and surrounding spaces. An exact ident match comes first, then IATA,
// then local id; one airport appears once. Empty for an empty code.
std::vector<const Airport*> findAirports(const std::vector<Airport>& table, std::string_view code);

// Great-circle distance in kilometres.
double airbandDistanceKm(double lat1Deg, double lon1Deg, double lat2Deg, double lon2Deg);

// The `count` airports nearest a position, nearest first, with their distance.
// Ties keep table order. Airports with no frequency are never in the table.
std::vector<std::pair<const Airport*, double>> nearestAirports(const std::vector<Airport>& table,
                                                               double latDeg, double lonDeg,
                                                               std::size_t count);

// The frequency-list group an airport's entries are filed under:
// "KORD Chicago O'Hare International Airport".
std::string airportGroupName(const Airport& a);

// One frequency-list entry per frequency, ready for FreqManager::addMany: mode
// AM, the bandwidth a channel of that raster needs (10 kHz on 25 kHz channels,
// 6 kHz on 8.33 kHz ones), named "<type> <channel> <desc>", filed under
// airportGroupName(), and ticked (scan = true) - the user asked for the list
// so that they could choose from it, and unticking the few they do not want
// is less work than ticking the many they do.
//
// EXCEPT THE BROADCASTS. ATIS, AWOS, ASOS and VOLMET transmit without a pause,
// all day: ticked, one would hold the monitor on its block for ever (a scan
// moves on only when a block goes quiet) and play a weather report under
// every conversation. They are added unticked, ready for the user who wants
// one.
std::vector<Bookmark> airportBookmarks(const Airport& a);

// Whether a frequency's service is a continuous broadcast (see above).
bool airbandContinuous(const AirbandFreq& f);

// The description with the type it repeats taken off: a description equal to
// the type is empty, one that starts or ends with the type as a whole word
// loses it ("NORTH CLASS B" under CLASS B is "NORTH"). Case-blind.
std::string airbandDescWithoutType(const AirbandFreq& f);

// The receiver bandwidth for a channel of each raster, exposed for the tests
// and the monitor.
inline constexpr double kAirbandBw25Hz = 10000.0;
inline constexpr double kAirbandBw833Hz = 6000.0;

}  // namespace cascade::core
