// radar_logic.hpp - THE RADAR SCOPE'S RULES (docs/engine-stage4.md 6.57): the ADS-B plan-position indicator the ImGui window draws in gui/scope_view.cpp (ScopeView::draw 2380-3000, the
// LCD panel 361-845) and app_window.cpp drawScopeMode (13564-14212: the bar, the gauges, the drums, the knob, the POWER key), as one state object and plain output structs - no Slint, no
// ImGui, no engine. The pure arithmetic is the classic's own (gui/scope_view.hpp's ladder, polar maths, rings, labels, detail lines; gui/scope_gain.hpp's knob rules; gui/track_metrics.hpp's
// altitude bands), included rather than copied: scope_view.hpp's only ImGui dependency is the include of gui/aircraft_icons.hpp, so the Slint target and its host test add third_party/imgui as
// a header-only include path (nothing of ImGui is linked) - docs 6.57 records it.
//
// THE FRAME. update() takes this tick's tracks, the receiver and the instrument's inputs and builds a RadarFrame: every number and every (already translated) string the Slint view shows,
// and every position in the TUBE's pixel space (origin at the tube's top-left, y down). The Slint view only places what the frame says. Gestures arrive in the same pixel space.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/plugin_ui_data.hpp"   // HostTrack, trackPresentation
#include "gui/scope_gain.hpp"
#include "gui/scope_view.hpp"

namespace foxsdr::slintscope {

// ---- the seams ----
// What a track-info source knows about one aircraft (the kit's TrackInfoCache::lookup in the window, a fake in the tests).
struct RadarInfo {
    bool active = false;     // a source is attached and instantiated
    bool pending = false;    // asked, not answered yet
    std::string registration, typeCode, typeName, operatorName;
};
struct RadarInfoSource {
    virtual ~RadarInfoSource() = default;
    virtual RadarInfo lookup(const cascade::core::HostTrack& track) = 0;
};

// What the window knows besides the tracks.
struct RadarInputs {
    double nowS = 0.0;
    bool hasRx = false;                 // FOXAPI_RX_POSITION_SET
    double rxLatDeg = 0.0, rxLonDeg = 0.0;
    bool running = false;               // FOXAPI_RX_RUNNING
    double signalDb = -200.0;           // state.signalDb (channel power, dB full scale); drawn only while running and finite
    bool deviceOpen = false, deviceAgc = false;           // FOXAPI_RX_DEVICE_OPEN, FOXAPI_RX_DEVICE_AGC
    bool haveGain = false;              // the GAINS list has an entry 0
    std::string gainStage, gainUnit;    // its name and detail (the unit); "" unit means the knob shows the bare figure
    float gainLoDb = 0.0f, gainHiDb = 0.0f, gainDb = 0.0f;   // value[0], value[1], value[3]; lo == hi means the travel is unknown (the fallback 0..60 applies)
    int aircraftIconPx = cascade::gui::kAircraftIconDefaultPx;   // the Interface half's aircraftIconPx (already clamped by the caller)
    int trailWidthPx = 2;               // mapTrailWidthPx
    std::size_t trackSourcesLive = 0;   // unused by the frame; kept for the empty-state note's census (the publisher words it)
};

// ---- the frame: everything the view draws ----
struct RadarTarget {
    std::string id;
    float x = 0.f, y = 0.f;             // tube pixels
    float headingDeg = 0.f;             // the course, 0 when unknown
    std::uint32_t rgba = 0xffffffffu;   // altitude band colour (grey-blue when unknown, alert red for an emergency), already faded by age
    bool altKnown = false, emergency = false, selected = false;
    float markR = 9.f;                  // the marker's hit radius (cascade::gui::aircraftMarkerRadius(iconPx))
    std::string label;                  // the callsign, else the id
};
struct RadarLabel {                     // a ring's or a bearing's text, centred on (x, y)
    float x = 0.f, y = 0.f;
    std::string text;
    bool strong = false;                // the outer ring, a cardinal
};
struct RadarTrail {                     // a trail, oldest first, in tube pixels, each point with its altitude band colour
    std::string id;
    bool selected = false;
    struct Point { float x, y; std::uint32_t rgba; };
    std::vector<Point> points;
};
struct RadarLine { std::string label, value; bool dim = false, alert = false; };   // a detail row (cascade::gui::ScopeDetailLine)
struct RadarContact { std::string id, name, alt, detail; std::uint32_t rgba = 0; bool alert = false; };   // a HOME list row (nearest first); rgba = the altitude band colour of `alt` (0 when unknown: the view's dim ink), alert = the name is drawn in the alert red
struct RadarOption { std::string name, value; };                                       // a SYS row

struct RadarFrame {
    bool hasRx = false;                 // false: the empty state (the view shows the position offers instead of a tube)
    int rangeNm = cascade::gui::kScopeDefaultRangeNm;
    std::string rangeText;              // "200 NM" (scopeRangeReadout)
    bool canStepDown = false, canStepUp = false;
    std::string plottedText;            // "%d PLOTTED"
    std::string modeText;               // "MODE ADS-B"
    std::string rxText;                 // the receiver position with hemisphere letters, "" when unknown
    std::string rangeCornerText;        // the top-right "%d NM" ("" when it would not fit beside the count: the view decides, this is the text)
    int plotted = 0;
    float tubeW = 0.f, tubeH = 0.f;     // what update() was told
    float centreX = 0.f, centreY = 0.f, radiusPx = 0.f;   // the polar frame's centre (the view centre) and the full-scale radius
    int trailWidthPx = 2;               // the user's trail width (RadarInputs::trailWidthPx), for the painter
    bool panned = false;                // the view has been moved off the antenna
    float rxMarkX = 0.f, rxMarkY = 0.f; // where the antenna is while panned (a green cross + "RX" label)
    std::vector<RadarTarget> targets;   // only what is plotted: aircraft, visible, in range, passing the FILTER
    std::vector<RadarLabel> ringLabels, bearingLabels;
    bool showThreeDigitBearings = false;
    // the cabinet's readings
    float sigFrac = 0.f; int sigLit = 0; std::string sigText;      // 18 segments; sigText "--" when stopped or non-finite
    float altFrac = 0.f; int altLit = 0; std::string altText;      // "FL350", "NO ALT", "--"
    std::string drumCaption;            // "TGT RANGE NM" or "SET RANGE NM"
    std::string drumRange;              // 4 digits
    std::string drumTracks;             // 3 digits: every aircraft heard, ungated
    bool knobDead = true;               // no radio open or its AGC on
    float knobFrac = 0.f;               // 0..1 along the travel
    int knobTicks[cascade::gui::kScopeKnobTickCount] = {0, 0, 0, 0, 0};
    int knobLit = -1;                   // the tick nearest the figure, -1 when dark
    std::string knobReadout;            // "30 dB", "7", "AUTO", "N/A"
    std::string knobNote;               // the stage name, "no device", "auto gain is on"
    bool powerOn = false;
    std::string powerText;              // "ON LINE" / "STANDBY"
    // the LCD panel
    int lcdScreen = 1;                  // 0 HOME, 1 FLIGHT, 2 SYS
    bool lcdShown = true;               // false when the window is too narrow for it (the caller decides: see setLayout)
    std::string homeCount, homeSub;     // the big figure and "of %d tracked   %d NM%s"
    std::vector<RadarContact> contacts;
    std::string homeEmptyTitle, homeEmptyBody;      // "NOTHING CURRENT" / "NOTHING BEING HEARD" and its paragraph, "" when the list has rows
    std::vector<RadarLine> flight;      // buildScopeDetailLines for the selection, empty when none
    std::string flightTitle, flightBody;            // "NOTHING SELECTED" + hint, or "TARGET LOST" + sentence, "" when a flight shows
    std::string flightFoot;             // scopeUnavailableNote while a flight shows
    std::vector<RadarOption> options;   // PHOSPHOR, SWEEP, TRAILS, FILTER with their value words
    std::string sysRange, sysAntenna;   // the facts
    std::string sysHelp;
    bool phosphor = true, sweep = true; int trail = 1, filter = 0;
    std::string selectedId;
};

// The sweep fan's leading edge in degrees clockwise from north: the classic's fmod(time / 4.2, 1) * 360 (scope_view.cpp 2741), one turn every 4.2 s. A negative or non-finite clock gives 0.
float radarSweepDegrees(double nowS);

// ---- the instrument ----
class RadarScope {
public:
    // One kept point of a trail: where and how high (the 3D view draws the altitude the tube's flat trail drops).
    struct TrailPoint { double lat, lon, altM; };
    // THE SETTINGS that persist (the Interface half's scopeRangeNm): snapped to the ladder.
    void setRangeNm(int nm) { rangeNm_ = cascade::core::clampScopeRangeNm(nm); }
    int rangeNm() const { return rangeNm_; }
    bool stepRange(int steps);                    // the bar's -/+ and the wheel: one rung, clamped, never wraps; true when it changed
    // THE TUBE'S SIZE in pixels (the Slint view reports it); the frame's positions are in it. A size under 40 px draws nothing (an empty frame with hasRx as given).
    void setTubeSize(float w, float h);
    // THE PANEL: the LCD is shown only if the window can afford it (the classic: panelW >= 260 and the tube keeps >= 260); the view tells us.
    void setLcdShown(bool shown) { lcdShown_ = shown; }

    // ---- the tick ----
    void update(const std::vector<cascade::core::HostTrack>& tracks, const RadarInputs& in, RadarInfoSource* info);
    const RadarFrame& frame() const { return frame_; }
    bool dirty() const { return dirty_; }          // a gesture, an option or a new frame changed what the picture shows
    void markPainted() { dirty_ = false; }

    // ---- gestures, in tube pixels (the classic's one InvisibleButton over the tube) ----
    void wheel(int dir);                           // dir > 0: wheel up = range DOWN one rung; dir < 0: range UP (scope_view.cpp 2380-2528)
    void dragBy(float dx, float dy);               // a left drag over 2 px pans the VIEW centre (scopeViewMoved); the range stays anchored to the view centre
    void releasePointer(float x, float y, bool wasDrag);   // a click that ends a drag is swallowed; a plain click on an aircraft selects the nearest within its markR, on empty glass clears the selection AND recentres the view on the click
    void doubleClick();                            // resetPan
    // ---- the LCD ----
    void setLcdScreen(int screen);                 // 0..2 (tabs HOME / FLIGHT / SYS)
    void selectContact(const std::string& id);     // a HOME row: selects and switches to FLIGHT
    void cycleOption(int index);                   // the SYS rows: PHOSPHOR on/off, SWEEP on/off, TRAILS off/line/ribbon, FILTER all/alert/named
    void clearSelection() { selectedId_.clear(); dirty_ = true; }
    void setSelection(const std::string& id) { selectedId_ = id; dirty_ = true; }
    void resetPan() { hasView_ = false; dirty_ = true; }
    // ---- the knob (app_window.cpp drawScopeKnob, scope_gain.hpp): returns the dB to send with FOXAPP_OP_SET_GAIN_NO_READBACK, or NaN for no send ----
    double knobDetent(int steps, const RadarInputs& in);   // 2 units a detent, clamped to the travel, kept 0.5 s locally so two detents before the readback are two detents
    // ---- the read-back for drive scripts and tests ----
    const std::string& selectedId() const { return selectedId_; }
    bool panned() const { return hasView_; }
    double viewLatDeg() const { return viewLat_; }
    double viewLonDeg() const { return viewLon_; }
    bool optPhosphor() const { return optPhosphor_; }
    bool optSweep() const { return optSweep_; }
    int optTrail() const { return optTrail_; }
    int optFilter() const { return optFilter_; }
    int lcdScreen() const { return lcdScreen_; }
    const std::vector<RadarTrail>& trails() const { return trailsOut_; }   // as of the last update, in tube pixels
    const std::vector<std::string>& trailKeys() const { return trailKeys_; }
    const std::map<std::string, std::vector<TrailPoint>>& rawTrails() const { return trails_; }   // every kept trail by track id, as of the last update (docs 6.64: the 3D airspace)
    std::size_t trailPointCount() const;           // every point of every trail kept (<= 256 per track, <= 256 tracks)

private:
    int rangeNm_ = cascade::gui::kScopeDefaultRangeNm;
    float tubeW_ = 0.f, tubeH_ = 0.f;
    bool lcdShown_ = true, dirty_ = true;
    bool hasView_ = false;
    double viewLat_ = 0.0, viewLon_ = 0.0;
    bool dragged_ = false;
    int lcdScreen_ = 1;
    bool optPhosphor_ = true, optSweep_ = true;
    int optFilter_ = 0, optTrail_ = 1;
    std::string selectedId_;
    std::map<std::string, std::vector<TrailPoint>> trails_;
    std::vector<RadarTrail> trailsOut_;
    std::vector<std::string> trailKeys_;
    cascade::gui::ScopeGainLocal gainLocal_;
    RadarFrame frame_;
    RadarInputs last_;
    // Caches (implementation detail, added with the .cpp): the trails' projection is redone only when what it depends on changed, and dirty() compares the picture's inputs.
    std::uint64_t trailStamp_ = 0, trailKey_ = 0, pictureKey_ = 0;
};

}  // namespace foxsdr::slintscope
