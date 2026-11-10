// globe_logic.hpp - THE GLOBE VIEWS' RULES (docs/engine-stage4.md 6.65): which plugin page offers which 3D view, what each view draws from what the host and the
// plugins actually publish, and nothing else. Pure: no Slint, no renderer, no I/O, no clock (every function takes the time). The geometry is globe_scene.hpp, the
// planets are solar_ephemeris.hpp, the pictures are globe_compositor.hpp, the models are globe_publish.hpp.
//
// LIVE DATA ONLY. Each reader below takes the rows, lines or tracks the host already holds and returns only what they state; a value the host cannot obtain is
// absent (an optional, an empty vector, a false flag), never a default that looks like a measurement, and the compositor draws an honest empty state for it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "globe_scene.hpp"
#include "solar_ephemeris.hpp"
#include "view3d_prefs.hpp"

namespace foxsdr::globe {

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// which plugin page offers which view
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
enum class ViewKind { None = 0, Propagation, Ft8, Goes, SolarSystem, Satellites };
// By the plugin's display name (PluginRow::name): "Propagation Tools", "FT8", "GOES HRIT / LRIT", "Solar System View", "Satellites".
ViewKind viewKindFor(const std::string& pluginDisplayName);
// The English words of the view toggle's two segments: what the page shows now / the globe. The window translates them through its catalogue (@tr).
const char* flatViewLabel(ViewKind k);      // PICTURE, LIST, FLAT, PAGES, FLAT MAP
const char* globeViewLabel(ViewKind k);     // 3D GLOBE, 3D ORBITS

// The camera of an orbiting view: the point the camera looks at (a longitude and latitude on a globe) and the zoom, 1 = the whole globe in the well.
struct GlobeCamera {
    double centreLonDeg = 0.0;
    double centreLatDeg = 20.0;
    double zoom = 1.0;
};
inline constexpr double kZoomMin = 0.6, kZoomMax = 4.0;
GlobeCamera defaultCamera(ViewKind k);
// A drag of (dxPx, dyPx) pixels in a well of `wellPx` pixels across: the globe follows the pointer (the surface under the pointer stays under it), so dragging
// right turns the centre WEST. The latitude is held to +/-85 degrees.
void dragCamera(GlobeCamera& cam, double dxPx, double dyPx, double wellPx);
void zoomCamera(GlobeCamera& cam, double steps);     // one step is a factor of 1.25 in or out (positive in), held to kZoomMin..kZoomMax

// What every view remembers while the window runs (the sibling map pages' toggles are session state too): per plugin.
struct ViewState {
    bool globeOn = false;                 // the 3D view is showing (the default is the page's own view)
    GlobeCamera camera{};
    bool cameraSet = false;               // false until the first use: the default for the kind
    // propagation
    int bandId = 0;                       // 0 = every band
    bool greyline = true;
    // FT8
    int windowMinutes = 60;               // 15, 60 or 1440
    // solar system
    bool logScale = true;
    bool showMoon = true;
    int dayOffset = 0;                    // whole days from today, -366..+366
    double tiltDeg = 55.0;                // degrees above the ecliptic plane the camera looks down from
    double spinDeg = 20.0;                // turn of the system about the ecliptic pole
    // satellites
    bool footprints = true;
    bool groundTracks = true;
    // GOES: the spacecraft the picture came from
    int goesSatellite = 0;                // 0 East, 1 West
};
inline constexpr int kDayOffsetLimit = 366;

// WHAT A VIEW REMEMBERS (the framework's one setting, AppConfig::view3d, view3d_prefs.hpp): the chosen view, the camera and up to four small choices of each. The framework keeps a
// camera's elevation in 4..89 degrees, so a globe's centre latitude (-85..85) is stored on that scale and the orbits view's tilt (5..85) as it is; option 3 says whether the camera
// was ever touched (a camera never touched follows the home, the receiver or the spacecraft, and is not frozen into the setting). The options: propagation band index, greyline;
// FT8 band index, window index; GOES spacecraft; solar system log scale, moon; satellites footprint, ground tracks.
const char* prefsName(ViewKind k);                                   // "propagation", "ft8", "goes", "solar", "satellites"
slintview3d::ViewState prefsDefaults(ViewKind k);                    // off, with the view's own default choices
slintview3d::ViewState toPrefs(const ViewState& v, ViewKind k);
void fromPrefs(ViewState& v, ViewKind k, const slintview3d::ViewState& p);
float latitudeToElevation(double latDeg);                            // -85..85 -> 4..89
double elevationToLatitude(float elevationDeg);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// numbers and words
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// "5 570 km", "16 994 km": whole kilometres, thousands separated by a space (the artboards' form).
std::string kmText(double km);
// "288°" for a bearing in [0, 360).
std::string bearingText(double deg);
// "42.3" -> "42 N 71 W": two decimals of degrees, hemisphere letters, for readouts.
std::string latLonText(const LatLon& p);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// bands
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
struct Band {
    int id;                 // 1.. in the table's order; 0 is "no band"
    const char* name;       // "20 m"
    double loMhz, hiMhz;
};
const Band* bandOfMhz(double mhz);              // the amateur band holding the frequency (160 m to 2 m); null outside
const Band* bandById(int id);
int bandCount();
// ARGB of a band's paths: 20 m orange, 40 m cyan, 15 m pink, 10 m green (the FT8 artboard's legend), the others from a fixed palette.
std::uint32_t bandColour(int bandId);
// ARGB of a skywave path by its number of hops: 1 orange, 2 cyan, 3 pink, more green (the propagation artboard's legend).
std::uint32_t hopColour(int hops);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// a panel's rows as the readers take them (converted from CascadePanelRow by the publisher; kind 0 cells, 1 heading, 2 separator)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
struct PanelRowText {
    int kind = 0;
    std::vector<std::string> cells;
};

// A Maidenhead locator embedded in text: the first token that is 4 or 6 characters, field AA..RR, square 00..99, optional subsquare aa..xx. Empty when none.
std::string locatorIn(const std::string& text);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// 1. the HF propagation globe (Propagation Tools)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The 18 beacons of the NCDXF / IARU International Beacon Project, call, place and Maidenhead grid as the project's page ncdxf.org/beacon/beaconlocations.html lists them
// (read 2026-11-10). The plugin's panel names the beacon on air by call; the host looks the place up here.
struct Beacon {
    const char* call;
    const char* place;
    const char* grid;
};
inline constexpr int kBeaconCount = 18;
const Beacon* beaconByCall(const std::string& call);
const Beacon& beaconAt(int index);

struct BeaconOnAir {
    const Beacon* beacon = nullptr;
    double freqMhz = 0.0;
    int secondsLeft = -1;       // -1 = the panel gave none
};
struct PropagationFacts {
    std::string homeLocator;    // the panel's "Locator" row when it holds one ("" = not set or not a locator)
    std::string targetLocator;  // the panel's "Target" row, same
    std::vector<BeaconOnAir> onAir;
    bool panelSeen = false;     // a "Beacons on air now" heading or a "Locator" row was found
};
// The plugin's panel (inventory prop_core.cpp 808-913): the "Locator" and "Target" rows of the Status block and the rows under the heading
// "Beacons on air now (NCDXF/IARU)": a "%.3f MHz" cell, the beacon's call and place, " s left". The reader takes the cells by what they hold, not by column.
PropagationFacts readPropagationPanel(const std::vector<PanelRowText>& rows);

struct GlobePath {
    std::string label;          // "4U1UN New York", "TARGET IO91wm"
    LatLon from, to;
    int hops = 1;
    std::uint32_t argb = 0;
    double distKm = 0.0, bearingDeg = 0.0;
    int bandId = 0;             // 0 = not a beacon (the target path)
    double freqMhz = 0.0;
    bool beacon = false;
};
struct GlobeMarker {
    std::string label;
    LatLon at;
    int shape = 0;              // 0 dot, 1 triangle (the receiver), 2 square
    std::uint32_t argb = 0;
};
struct PropagationScene {
    bool homeKnown = false;
    LatLon home;
    std::string homeSource;             // "the plugin's locator", "the receiver position", "" none
    std::vector<GlobePath> paths;       // the target first, then the beacons on air in the order of the panel
    std::vector<GlobeMarker> markers;   // home, target, each beacon
    int beaconsListed = 0;              // before the band filter
};
// The longest single hop used to count hops of a path: about 4 000 km off the F2 layer. Every readout that says "hops" says "about".
inline constexpr double kMaxHopKm = 4000.0;
// home: the plugin's locator when it is one, else the receiver position when `rxKnown`. bandFilter: 0 every band, else a Band id (only the beacons on that band show).
PropagationScene buildPropagationScene(const PropagationFacts& facts, bool rxKnown, const LatLon& rx, int bandFilter);
std::string pathChipText(const GlobePath& p);       // "4U1UN New York 5 570 km 288°"

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// 2. the FT8 reception globe
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
struct Ft8Decode {
    int hour = 0, minute = 0, second = 0;
    int snr = 0;
    double dt = 0.0;
    int hz = 0;
    std::string message;
};
// One decoder line as the plugin prints it, after the host's plugin tag is stripped: "%02d%02d%02d %3d %4.1f %4d ~  %s" (the plugin's format string), e.g.
// "120015 -12  0.3 1243 ~  CQ DX K1ABC FN42". False for any other line (the start line, the diagnostics).
bool parseFt8Line(const std::string& text, Ft8Decode& out);
// The wall-clock time (UTC milliseconds since 1970) of a decode: its HHMMSS is the most recent such time of day that is not more than a minute after `nowUnixMs`
// (the clock's slack), so a line read after midnight but stamped 23:59:45 is yesterday's.
std::int64_t ft8LineTimeMs(const Ft8Decode& d, std::int64_t nowUnixMs);
struct Ft8Heard {
    std::string call, locator;    // the station that SENT the message and the locator it carried (4 or 6 characters)
};
// The sender and the locator of a message: "CQ [DX|NA|POTA...] CALL LOC" and "TO FROM LOC" carry a locator, a report ("-09", "R-14"), "RR73" and "73" do not (RR73 looks
// like a locator and is not one). False when there is no placeable sender.
bool ft8Heard(const std::string& message, Ft8Heard& out);
bool looksLikeCallsign(const std::string& token);

struct Ft8Spot {
    std::int64_t atMs = 0;
    int bandId = 0;
    std::string call, locator;
    LatLon at;
    int snr = 0;
    double km = 0.0, bearingDeg = 0.0;     // from the receiver
};
// The stations HEARD (the host has only what the decoder printed - not who reported whom). One entry per callsign per band, the newest kept; at most kCap entries.
class Ft8SpotStore {
public:
    static constexpr std::size_t kCap = 3000;
    // Adds what a decoder line says (if it places a station), stamped `atMs` on the band of `dialMhz`, measured from `rx`. Returns whether a spot was stored.
    bool addLine(const std::string& text, std::int64_t atMs, double dialMhz, const LatLon& rx);
    void add(const Ft8Spot& s);
    void clear() { spots_.clear(); }
    std::size_t size() const { return spots_.size(); }
    // The spots newer than `windowMs` before nowMs, on `bandFilter` (0 = every band), oldest first. Also drops what is older than 24 h.
    std::vector<Ft8Spot> visible(std::int64_t nowMs, std::int64_t windowMs, int bandFilter);

private:
    std::vector<Ft8Spot> spots_;
};
struct Ft8Summary {
    int stations = 0;
    bool hasFurthest = false;
    Ft8Spot furthest;
    int bands = 0;                // how many bands the stations are on
};
Ft8Summary summariseSpots(const std::vector<Ft8Spot>& spots);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// 3. the GOES disk
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The two operational spacecraft (NOAA OSPO, status page and the GOES-19 announcement of 2025-04-07): GOES-East at 75.2 W, GOES-West at 137.0 W.
struct GoesSatellite {
    const char* name;
    double lonDeg;
};
inline constexpr int kGoesSatelliteCount = 2;
const GoesSatellite& goesSatellite(int index);
// The preset the operator tuned decides nothing about the spacecraft (East and West share 1694.1 MHz), so the choice is the operator's, defaulting to East.
inline constexpr double kGoesAltitudeKm = 35786.023;
// The scan angle (radians) of a point on the ground as seen from a geostationary satellite over satLonDeg, the GOES-R Product User Guide's fixed-grid navigation:
// x east of the sub-satellite meridian, y north. False when the point is hidden behind the limb.
bool goesScanAngle(const LatLon& ground, double satLonDeg, double& xRad, double& yRad);
// The half-width, in radians, of the ABI full-disk frame (5424 pixels of 56 microradians): the disc's bounding square. The received picture is taken to be that frame.
inline constexpr double kGoesFrameHalfRad = 0.151872;
// Where in a picture (0..1 across, 0..1 down, the top-left the north-west corner) the point appears; false when behind the limb or outside the frame.
bool goesPicturePoint(const LatLon& ground, double satLonDeg, double& u, double& v);
// The look angle (elevation, azimuth) of the satellite from `rx`, by the same ellipsoid-free geometry the other views use, and its slant range.
LookAngle goesLook(const LatLon& rx, double satLonDeg);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// 4. the solar system
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
struct PanelPlanet {
    int planet = -1;              // solar::Planet index
    bool haveEarthAu = false;  double earthAu = 0.0;
    bool haveElong = false;    double elongDeg = 0.0;  bool evening = false;
    bool haveSunAu = false;    double sunAu = 0.0;
};
struct SolarPanelFacts {
    std::vector<PanelPlanet> planets;
    bool haveEarthSun = false; double earthSunAu = 0.0;
    bool haveMoon = false;     double moonKm = 0.0;  bool haveMoonLit = false;  double moonLitPct = 0.0;
};
// The plugin's "Planets from the Earth" block (the plugin's strings "%.3f AU", "elong %.0f %c", "r %.2f AU", "%.2f AU", "%.0f %% lit"): a row per body, found by the
// body's name in the first cell and read by what the other cells hold.
SolarPanelFacts readSolarPanel(const std::vector<PanelRowText>& rows);
// The panel against the host's own figures for the same instant: the worst difference in distance and elongation after allowing for the panel's rounding
// ("%.2f" and "%.0f"). `ok` is false when any figure differs by more than the rounding plus the method's own error.
struct PanelCheck {
    int compared = 0;
    double worstAuError = 0.0, worstElongError = 0.0;
    bool ok = true;
    std::string worst;
};
PanelCheck checkAgainstPanel(const SolarPanelFacts& panel, double jd);

// The scene the orbits view draws: positions at a date, in display units (the outermost ring is 1).
struct BodyView {
    std::string name;
    solar::Xyz pos;             // display position
    solar::PlanetFacts facts;
    std::uint32_t argb = 0;
    double radiusPx = 4.0;
};
struct OrbitScene {
    double jd = 0.0;
    bool logScale = true;
    double outerAu = 30.5;
    std::vector<std::vector<solar::Xyz>> rings;   // display units, Mercury .. Neptune, then the Earth's
    std::vector<BodyView> bodies;                 // Mercury .. Neptune
    solar::Xyz earth;                             // display position
    solar::MoonFacts moon;
    solar::Xyz moonDisplay;                       // on a ring about the Earth, not to scale
    bool showMoon = true;
};
OrbitScene buildOrbitScene(double jd, bool logScale, bool showMoon);
std::string planetChipText(const BodyView& b);    // "Mars 1.36 AU 85° W"
// The calendar date of a Julian date, "2026-11-10"; and the date `dayOffset` days from the Julian date `todayJd`.
std::string dateText(double jd);
double dateOfOffset(double todayJd, int dayOffset);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// 5. the satellite tracking globe
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
struct SatIn {
    std::string id, label;
    LatLon at;
    double altKm = 0.0;
    bool hasAlt = false;
};
struct SatPathIn {
    std::string id;
    bool dashed = false, closed = false;
    std::vector<LatLon> points;
};
struct SatReadout {
    bool valid = false;
    std::string label;
    LatLon sub;
    double altKm = 0.0;
    bool hasAlt = false;
    LookAngle look;
    bool hasLook = false;       // needs the receiver position and the altitude
    double footprintDeg = 0.0;  // angular radius, 0 without an altitude
};
// The selected satellite's readout from the receiver position (`rxKnown`).
SatReadout satReadout(const SatIn& sat, bool rxKnown, const LatLon& rx);
// "LEO" below 2 000 km, "MEO" below 35 000, "GEO" within 500 km of 35 786, else "HEO" (the artboards' altitude-colour classes).
const char* orbitClass(double altKm);
std::uint32_t orbitClassColour(double altKm);

}  // namespace foxsdr::globe
