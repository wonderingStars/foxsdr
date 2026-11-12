// mapj_logic.hpp - SCREEN LANE J (docs/multi-radio-records/screen-j.md): the map page's added rows as pure rules, with no Slint, no engine and
// no ImGui: the layer switches and their legend and credit line, the heat and range-outline layers (over core/adsb_coverage.hpp), the signal
// level against position cells (core/rf_position_log.hpp), the Maidenhead grid, labels and worked / heard shading (core/locator.hpp), the APRS
// path lines, the timeline replay (core/track_store.hpp: a clock and a recorder), the aircraft filter, the unit choice applied to the map's
// words (core/access_units.hpp), the Sun / Moon / radio-source table and the polar sky plot (core/sky_position.hpp), the antenna calculators
// (core/antenna_calc.hpp) and the tile cache's words (core/tile_cache.hpp). kit_publish.cpp calls these; tests/test_slint_host_mapj.cpp holds them.
//
// WORDS: every English sentence here is a key of the string tables (FOX_TR_NOOP); the caller translates it with cascade::i18n::tr.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/access_units.hpp"
#include "core/adsb_coverage.hpp"
#include "core/rf_position_log.hpp"
#include "core/tile_cache.hpp"
#include "core/track_store.hpp"
#include "map_compositor.hpp"

namespace foxsdr::slintmapj {

// the plugin ABI's CASCADE_TRACK_* kinds (src/core/plugin_abi.h): copied so this file includes no ABI header; tests read the header and hold them equal
inline constexpr int kKindUnknown = 0;
inline constexpr int kKindAircraft = 1;
inline constexpr int kKindVessel = 2;
inline constexpr int kKindStation = 3;
inline constexpr int kKindSatellite = 4;

// ------------------------------------------------------------------------------------------------------------------------
// M1a, M1b, M2, Q5: the layers
// ------------------------------------------------------------------------------------------------------------------------
enum class Layer : int { Airspaces = 0, WeatherRadar, ReceiverMaps, Repeaters, AdsbHeat, AdsbRange, AprsPaths, Locators, RfLevel };
inline constexpr int kLayerCount = 9;

struct LayerInfo {
    const char* label;          // English key
    const char* tip;            // English key
    const char* credit;         // English key, "" for none: what the credit line says while the layer is on
    bool needsSource;           // a data source whose terms (and often a key) nobody has read: the switch cannot be turned on
};
const LayerInfo& layerInfo(Layer l);
// The sentence for a blocked layer (needsSource): why it is off.
const char* blockedNote();

struct LayerSet {
    std::array<bool, kLayerCount> on{};
    static LayerSet defaults();                      // Airspaces and APRS paths on (they draw what the map already receives), the rest off
    bool get(Layer l) const { return on[static_cast<std::size_t>(l)]; }
    // Turns a layer on or off; a blocked layer cannot be turned on (false, nothing changed).
    bool set(Layer l, bool value);
};

struct LegendEntry {
    std::string label;          // English key
    std::uint32_t rgba = 0;     // 0xRRGGBBAA
};
// The legend rows the layers that are on add (after the altitude rows the map already has).
std::vector<LegendEntry> layerLegend(const LayerSet& set, bool workedHeardShown);
// The attribution line: `base` (the basemap's or the coastline's), then the credit of each layer that is on, joined with " | ".
std::string creditLine(const std::string& base, const LayerSet& set, const std::function<std::string(const std::string&)>& tr);

// A plugin path (id, kind, flags) as the layers see it.
bool isAprsPath(const std::string& id, std::uint32_t kind);        // id ends "~path", kind STATION (kKindStation)
bool isClosedPath(std::uint32_t flags);                            // CASCADE_PATH_FLAG_CLOSED: an airspace, a footprint

std::uint32_t heatRgba(double t01);                                // 0..1 cold to hot, with its own alpha
std::uint32_t outlineRgba(int band);                               // the range outline's colour per altitude band (0..6, -1 = all bands)
std::vector<slintmap::Cell> heatCells(const std::vector<cascade::core::adsbcov::HeatCell>& cells, double cellDeg);
slintmap::Trail outlineTrail(const std::vector<std::pair<double, double>>& polygon, int band);
slintmap::Trail aprsPathTrail(const std::vector<std::pair<double, double>>& points);
std::uint32_t levelRgba(double db, double minDb, double maxDb);   // a signal level against position: weak blue to strong red
std::vector<slintmap::Cell> levelCells(const std::vector<cascade::core::rfpos::Cell>& cells, double cellMeters);

// ------------------------------------------------------------------------------------------------------------------------
// the Maidenhead grid, its labels and the worked / heard shading
// ------------------------------------------------------------------------------------------------------------------------
// The grid level a longitude span (degrees across the pane) is drawn at: 1 fields (20 x 10 degrees), 2 squares (2 x 1), 3 subsquares.
int gridLevelFor(double spanDeg);
struct GridLabel {
    double lat = 0.0, lon = 0.0;       // the centre of the cell
    std::string text;                  // "JO" (level 1), "JO02" (2), "JO02ab" (3)
};
// The cells whose centre lies in the view, at most `cap` of them, nearest the view centre first. latSpan follows the aspect.
std::vector<GridLabel> gridLabels(double centreLat, double centreLon, double spanLonDeg, double spanLatDeg, int level, std::size_t cap);
enum class MarkState : int { Heard = 1, Worked = 2 };
struct LocatorMark {
    std::string locator;
    MarkState state = MarkState::Heard;
};
// The shaded cells of the marks: a locator that does not decode is dropped; a locator both heard and worked is worked.
std::vector<slintmap::Cell> locatorCells(const std::vector<LocatorMark>& marks);

// ------------------------------------------------------------------------------------------------------------------------
// M3: timeline replay
// ------------------------------------------------------------------------------------------------------------------------
inline constexpr int kReplaySpeeds = 4;
inline constexpr std::array<double, kReplaySpeeds> kReplaySpeedFactor{1.0, 4.0, 16.0, 64.0};
struct Replay {
    bool active = false;                // the map shows the past; false = live
    bool playing = false;
    int speedIndex = 0;
    double t = 0.0, t0 = 0.0, t1 = 0.0; // the instant shown, the oldest and the newest instant the store holds (unix seconds)
    // Starts a replay over [first, last], a little behind the newest instant, paused.
    void open(double first, double last);
    void close();                       // back to live
    void setRange(double first, double last);   // the store grew or shed: t stays inside
    void play();
    void pause();
    void toggle();
    void setSpeed(int index);
    void faster();                      // the next speed, wrapping to the slowest
    void scrub(double fraction01);
    void tick(double dtSec);            // advances t while playing; reaching the newest instant stops
    double fraction() const;            // 0..1 along the range
};
std::string clockText(double unixSec);                  // "14:32:05 UTC"
std::string speedText(int speedIndex);                  // "1x", "4x", "16x", "64x"
std::string behindText(double newestSec, double tSec, const std::function<std::string(const std::string&)>& tr);   // "5 min 30 s behind", "live edge"

// A track the page holds, as the recorder takes it.
struct FeedItem {
    int kind = 0;                       // kKind*
    std::string id;
    double lat = 0.0, lon = 0.0, altM = 0.0;
    bool hasAlt = false;
};
// Takes the page's tracks once a tick into a TrackStore: at most one point per track every minGapSec, aircraft / vessels / stations only (the
// store has no satellite kind), an id the store refuses (more than 24 characters, a character outside [0-9A-Za-z_./-]) is counted and left out.
class TrackRecorder {
public:
    explicit TrackRecorder(cascade::core::tracks::Config cfg = {}) : cfg_(cfg), store_(std::make_unique<cascade::core::tracks::TrackStore>(cfg)) {}
    int feed(double nowSec, const std::vector<FeedItem>& items, double minGapSec = 2.0);
    cascade::core::tracks::TrackStore& store() { return *store_; }
    const cascade::core::tracks::TrackStore& store() const { return *store_; }
    std::uint64_t skippedIds() const { return skippedIds_; }
    void clear();
private:
    cascade::core::tracks::Config cfg_;
    std::unique_ptr<cascade::core::tracks::TrackStore> store_;
    std::unordered_map<std::string, double> last_;
    std::uint64_t skippedIds_ = 0;
};

// ------------------------------------------------------------------------------------------------------------------------
// the aircraft filter
// ------------------------------------------------------------------------------------------------------------------------
struct FilterSubject {
    std::string id, label;              // the callsign is the label
    double altM = 0.0;
    bool hasAlt = false;
    double speedMps = 0.0;
    bool hasSpeed = false;
    int category = -1;                  // CASCADE_TRACK_CATEGORY, -1 unknown
    // what only the ADS-B plugin knows (docs: the hand-off): a test double fills them today
    bool known = false;                 // the three below are known
    bool military = false;
    std::string typeCode, operatorName;
};
struct AircraftFilter {
    bool enabled = false;
    bool altMinOn = false, altMaxOn = false;
    double altMinFt = 0.0, altMaxFt = 45000.0;
    bool hideGround = false;
    std::string callsign;               // a case-insensitive piece of the callsign or the address
    bool militaryOnly = false;
    std::string typeText, operatorText; // case-insensitive pieces
};
bool isOnGround(const FilterSubject& s);                                   // altitude under 50 ft and (no speed, or under 25 m/s)
bool filterPasses(const AircraftFilter& f, const FilterSubject& s);        // a disabled filter passes everything
// True when the filter asks something only the plugin can answer (military, type, operator).
bool filterNeedsPluginFields(const AircraftFilter& f);
std::string filterSummary(const AircraftFilter& f, const std::function<std::string(const std::string&)>& tr);   // "3 conditions"
int filterConditionCount(const AircraftFilter& f);
bool parseNumber(const std::string& text, double lo, double hi, double& out);   // "12 000" / "12000" / "12,5": a finite number in [lo, hi]

// ------------------------------------------------------------------------------------------------------------------------
// the unit choice applied to the map's words
// ------------------------------------------------------------------------------------------------------------------------
cascade::core::access::ObjectKind objectKindOf(int trackKind);
std::string altitudeText(cascade::core::access::UnitSystem sys, int trackKind, double altM);
std::string speedTextOf(cascade::core::access::UnitSystem sys, int trackKind, double speedMps);
std::string distanceText(cascade::core::access::UnitSystem sys, int trackKind, double metres);
std::string unitChoiceName(cascade::core::access::UnitSystem sys);        // "Automatic" ...: English keys
cascade::core::access::UnitSystem nextUnitSystem(cascade::core::access::UnitSystem sys);
struct ScaleBar {
    double frac = 0.0;                  // the bar's length as a fraction of the pane's width; 0 = no bar
    std::string text;                   // "5 mi", "800 ft", "2 nmi", "10 km"
};
ScaleBar scaleBar(cascade::core::access::UnitSystem sys, double metresPerPixel, double paneWidthPx);

// ------------------------------------------------------------------------------------------------------------------------
// M8 and the polar plot: the sky
// ------------------------------------------------------------------------------------------------------------------------
struct SkyRow {
    std::string name;
    double azDeg = 0.0, elDeg = 0.0;
    bool below = false;
    std::string azText, elText;         // "123.4", "-12.5"
};
std::vector<SkyRow> skyRows(double latDeg, double lonDeg, double unixSec);   // empty for a position that is not a place
struct PolarPoint {
    double x = 0.0, y = 0.0;            // the unit disc, y DOWN (north up the screen, east to the right); the horizon is radius 1
    bool below = false;                 // under the horizon: placed on the rim
};
PolarPoint polarPoint(double azDeg, double elDeg);
struct PassSample {
    double tSec = 0.0, azDeg = 0.0, elDeg = 0.0;
};
struct PassSummary {
    bool valid = false;
    double aosAzDeg = 0.0, losAzDeg = 0.0, maxElDeg = 0.0, maxElAzDeg = 0.0, durationSec = 0.0;
};
PassSummary summarisePass(const std::vector<PassSample>& pass);              // over the samples at or above the horizon
std::vector<PolarPoint> passTrace(const std::vector<PassSample>& pass);       // the samples at or above the horizon, in order
std::vector<PassSample> madeUpPass();                                         // a generated overhead-ish pass for the test seam and the drive scripts

// ------------------------------------------------------------------------------------------------------------------------
// Q7a: the antenna calculators
// ------------------------------------------------------------------------------------------------------------------------
struct AntennaInput {
    double fMHz = 14.2;
    double distanceKm = 100.0;
    double coaxFraction = 0.25;         // of a wavelength
    double vf = 0.66;
    int yagiElements = 3;
};
struct AntennaRow {
    std::string item;                   // English key
    std::string metres, feet;           // "10.05 m", "32.97 ft"; the path loss row has its dB in `metres`
    std::string note;                   // English key or ""
};
std::vector<AntennaRow> antennaRows(const AntennaInput& in);                   // empty when the frequency is out of range
std::string antennaError(const AntennaInput& in);                              // "" when the input is good, else an English key

// ------------------------------------------------------------------------------------------------------------------------
// M9: the tile cache's words
// ------------------------------------------------------------------------------------------------------------------------
std::string tileCacheText(std::size_t tiles, std::uint64_t bytes, std::uint64_t maxBytes, const cascade::core::tilecache::Stats& stats,
                          const std::function<std::string(const std::string&)>& tr);

}  // namespace foxsdr::slintmapj
