// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/scope_view.hpp"

#include "gui/scope_face.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>

#include "gui/basemap_cache.hpp"
#include "gui/coastline_data.hpp"
#include "gui/track_info_cache.hpp"
#include "imgui.h"

namespace cascade::gui {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0;
// The equator's length on the same sphere greatCircleKm measures on. It is
// what turns "pixels per kilometre at the receiver" into "pixels for the whole
// world", which is the only number the Web Mercator tile grid is defined in.
constexpr double kWorldKm = 2.0 * kPi * kEarthRadiusKm;

// --- the phosphor -------------------------------------------------------------
//
// GREEN, BECAUSE THAT IS WHAT THIS INSTRUMENT LOOKS LIKE. The chrome - the
// rings, the bearing ticks, the corner readouts, the panel's labels - takes the
// long-persistence phosphor of the display this pattern comes from. It is not
// decoration: a scope face is read by shape and by radius, and holding the
// whole of the furniture in one hue is what leaves the targets as the only
// coloured things on it.
//
// AND THE AIRCRAFT ARE NOT GREEN, which is the trade-off this file makes
// deliberately and would be wrong to make the other way. This product's design
// brief says colour is never the only cue and that an emergency outranks every
// other colour rule; it also says an aircraft's colour carries its ALTITUDE,
// which is the single thing that makes a busy picture readable - an approach
// and a cruise are four bands apart at a glance. Recolouring every target
// green would buy a prettier scope by throwing away the one reading that makes
// it useful, and would put an aircraft squawking 7700 in the same hue as the
// range rings. So: green furniture, banded targets, and the emergency hue
// above both. The silhouette and the hollow-vs-filled cue carry the same
// information as the colour for anyone who cannot separate the hues, exactly
// as they do on the map.
// EVERY VALUE BELOW IS THE DESIGN'S OWN, taken from resources/web/radar's CSS
// custom properties rather than chosen here, so the native face and the
// reference the design was signed off on are the same instrument and not two
// interpretations of one. --acc #86d64a, --acc-bright #b7f56a, --acc-dim
// #5f8a3c, --acc-lamp #9ad84f, the tube's own #0a1c0d, the bezel's #1a1b14 and
// #34362a, and the corner readouts' #7c8a5f and #5d6a45.
//
// The palette this replaced was a TEAL green (46,190,110). It read perfectly
// well on its own and was completely wrong beside the reference - which is the
// whole lesson: a colour is only right relative to the thing it is meant to
// match, and "looks like a radar" is not the specification.
constexpr ImU32 kSurround = IM_COL32(16, 17, 8, 255);      // the bay behind the tube
constexpr ImU32 kBezelIn = IM_COL32(26, 27, 20, 255);      // #1a1b14, 10 px
constexpr ImU32 kBezelOut = IM_COL32(52, 54, 42, 255);     // #34362a, 2 px
constexpr ImU32 kGround = IM_COL32(10, 28, 13, 255);       // #0a1c0d, the face
constexpr ImU32 kCoast = IM_COL32(90, 160, 70, 210);
constexpr ImU32 kRing = IM_COL32(134, 214, 74, 36);        // rgba(--acc,.14)
constexpr ImU32 kRim = IM_COL32(134, 214, 74, 77);         // rgba(--acc,.30)
constexpr ImU32 kTick = IM_COL32(134, 214, 74, 90);
constexpr ImU32 kChrome = IM_COL32(124, 138, 95, 255);     // #7c8a5f
constexpr ImU32 kChromeDim = IM_COL32(93, 106, 69, 255);   // #5d6a45
constexpr ImU32 kPanelLabel = IM_COL32(95, 138, 60, 255);  // --acc-dim
constexpr ImU32 kPanelValue = IM_COL32(183, 245, 106, 255);  // --acc-bright
constexpr ImU32 kPanelDim = IM_COL32(111, 122, 92, 255);
// The one hue nothing else on the scope uses, which is the whole reason it is
// reserved. Matches the map's emergency colour exactly, so a target that is
// red on one is red on the other.
constexpr ImU32 kAlert = IM_COL32(255, 45, 45, 255);


// --- the tube's own optics ----------------------------------------------------
//
// Everything below exists because a radar tube is not a map in a circular hole.
// The design these are taken from (resources/web/radar) expresses them as CSS
// filters, conic gradients and repeating gradients, none of which Dear ImGui
// has; each one here is the closest thing the draw list can build, and the
// comment on each says what it is standing in for.

// THE PHOSPHOR WASH. The design runs the basemap through
// grayscale(1) brightness(.95) sepia(1) hue-rotate(58deg) saturate(1.9) at
// opacity .85, which turns full-colour OpenStreetMap into one green hue ramp.
// A per-channel tint cannot reproduce the grayscale step - fixed-function
// blending has no cross-channel term, so a blue sea and a green field of the
// same lightness stay different hues however they are multiplied. What IS
// reproducible is the rest of the chain: tint the tile towards the phosphor,
// then lay a single translucent wash of the tube's own colour over the whole
// face, which collapses the remaining hue spread towards green the way the
// saturate/hue-rotate pair does. Sea reads darker than land, coasts stay
// legible, and nothing on the face is the wrong colour by more than the
// residual chroma the wash does not cover.
constexpr ImU32 kTileTint = IM_COL32(196, 226, 150, 217);   // .85 alpha
constexpr ImU32 kPhosphorWash = IM_COL32(24, 74, 26, 120);

constexpr ImU32 kPhosphor = IM_COL32(134, 214, 74, 255);
constexpr ImU32 kRingLine = IM_COL32(134, 214, 74, 36);     // rgba(...,.14)
constexpr ImU32 kRingOuter = IM_COL32(134, 214, 74, 77);    // rgba(...,.30)
constexpr ImU32 kCrossHair = IM_COL32(134, 214, 74, 41);    // rgba(...,.16)
constexpr ImU32 kHomeDot = IM_COL32(224, 95, 208, 255);     // #e05fd0
constexpr ImU32 kRingText = IM_COL32(134, 214, 74, 140);
constexpr ImU32 kScanLine = IM_COL32(0, 0, 0, 56);          // rgba(0,0,0,.22)

// A wedge of the sweep, as a fan of independent triangles so the alpha ramp is
// carried per vertex. The design's conic-gradient runs from .30 alpha at the
// leading edge to nothing 55 degrees behind it, once every 4.2 seconds.
void addSweepFan(ImDrawList* dl, const ImVec2& c, float radius, double leadDeg) {
    constexpr int kSteps = 44;
    constexpr double kSpanDeg = 55.0;
    for (int i = 0; i < kSteps; ++i) {
        const double t0 = static_cast<double>(i) / kSteps;
        const double t1 = static_cast<double>(i + 1) / kSteps;
        // Trailing edge of the wedge is the faint end; the lead is brightest.
        const double a0 = (leadDeg - kSpanDeg * t0) * kPi / 180.0;
        const double a1 = (leadDeg - kSpanDeg * t1) * kPi / 180.0;
        const int alpha0 = static_cast<int>(77.0 * (1.0 - t0));
        const int alpha1 = static_cast<int>(77.0 * (1.0 - t1));
        const ImVec2 p0(c.x + static_cast<float>(std::sin(a0)) * radius,
                        c.y - static_cast<float>(std::cos(a0)) * radius);
        const ImVec2 p1(c.x + static_cast<float>(std::sin(a1)) * radius,
                        c.y - static_cast<float>(std::cos(a1)) * radius);
        // PrimVtx rather than AddTriangleFilled: the ramp is per-vertex, and a
        // flat-filled triangle per step banks the wedge into visible stripes.
        dl->PrimReserve(3, 3);
        // The public accessor, not dl->_Data->TexUvWhitePixel: the latter needs
        // imgui_internal.h, and nothing else in this file does.
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(dl->_VtxCurrentIdx));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(dl->_VtxCurrentIdx + 1));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(dl->_VtxCurrentIdx + 2));
        dl->PrimWriteVtx(c, uv, IM_COL32(134, 214, 74, (alpha0 + alpha1) / 2));
        dl->PrimWriteVtx(p0, uv, IM_COL32(134, 214, 74, alpha0));
        dl->PrimWriteVtx(p1, uv, IM_COL32(134, 214, 74, alpha1));
    }
}

// The CRT's scan lines: one dark row every three pixels. Clipped to the tube by
// shortening each row to the circle's own chord at that height, which is
// cheaper and exact where a rectangular clip would square the face off.
void addScanLines(ImDrawList* dl, const ImVec2& c, float radius) {
    const int r = static_cast<int>(radius);
    for (int dy = -r; dy <= r; dy += 3) {
        const float half = std::sqrt(std::max(0.0f, radius * radius -
                                              static_cast<float>(dy) * static_cast<float>(dy)));
        if (half < 1.0f) { continue; }
        const float y = c.y + static_cast<float>(dy) + 0.5f;
        dl->AddLine(ImVec2(c.x - half, y), ImVec2(c.x + half, y), kScanLine, 1.0f);
    }
}

// The vignette, as concentric annuli. A radial gradient is a per-vertex fan in
// principle, but the tube already has the sweep fan over it and a second one
// would double the vertex cost of the whole face for a gradient nothing is
// measured against - twenty rings are indistinguishable at this radius.
void addVignette(ImDrawList* dl, const ImVec2& c, float radius) {
    constexpr int kRings = 20;
    for (int i = 0; i < kRings; ++i) {
        const float t0 = static_cast<float>(i) / kRings;
        const float t1 = static_cast<float>(i + 1) / kRings;
        // Transparent-green at the middle to .34 black at 82% and beyond, as
        // the design's radial-gradient specifies.
        const float f = std::min(1.0f, t1 / 0.82f);
        const int alpha = static_cast<int>(87.0f * f * f);
        if (alpha <= 0) { continue; }
        dl->AddCircle(c, radius * (t0 + t1) * 0.5f, IM_COL32(0, 0, 0, alpha), 64,
                      radius / kRings + 1.0f);
    }
}

// --- projection ---------------------------------------------------------------
//
// Web Mercator, normalised so the whole world is 0..1 in both axes - the form
// the tile grid is defined in, so a tile index is the normalised coordinate
// times 2^z. Lifted from map_view.cpp rather than shared with it: the scope is
// its own renderer by design, and a common file underneath both would tie two
// pictures with different geometries to one set of decisions.
double mercY(double latDeg) {
    const double lat = std::clamp(latDeg, -85.05112878, 85.05112878);
    const double s = std::sin(lat * kPi / 180.0);
    return 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPi);
}

double mercLat(double y) {
    return std::atan(std::sinh(kPi * (1.0 - 2.0 * y))) * 180.0 / kPi;
}

// --- target colour ------------------------------------------------------------

// The same precedence the map states, and it has to be the same or the two
// pictures would describe one aircraft differently:
//   1. an EMERGENCY takes the reserved hue and outranks everything;
//   2. the ALTITUDE BAND when the altitude is known, which is what makes an
//      approach and a cruise read differently at a glance;
//   3. the aircraft kind colour when it is not - and the marker is drawn
//      HOLLOW in that case, so "no reported altitude" and "at sea level" are
//      not two shades of one thing.
ImU32 targetColour(const CascadeTrack& t) {
    if ((t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u) { return kAlert; }
    const int band = altitudeBandIndex(t.altM);
    if (band < 0) {
        // NOT A RED. The map's kind colour for an aircraft is a red one hue
        // away from the alert red reserved for a 7500/7600/7700 squawk, and on
        // a scope face - small marks, phosphor surround, no legend beside them
        // - a reviewer could not tell the two apart. The product's own rule is
        // that an emergency must never be mistakable for anything else, so on
        // this view an unknown altitude takes a neutral grey and keeps the
        // hollow outline that already distinguishes it from sea level. Red on
        // this scope means one thing.
        return IM_COL32(196, 204, 212, 255);
    }
    const AltBandStyle& s = altBandStyle(band);
    return IM_COL32(s.r, s.g, s.b, 255);
}

// Applies a 0..1 fade to a colour's alpha, so a target that has gone quiet
// dims as one thing rather than vanishing the instant it stops reporting.
ImU32 fadedColour(ImU32 c, float alpha) {
    const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFFu) * alpha;
    unsigned int v = static_cast<unsigned int>(a + 0.5f);
    if (v > 255u) { v = 255u; }
    return (c & ~(0xFFu << IM_COL32_A_SHIFT)) | (v << IM_COL32_A_SHIFT);
}

// The plane silhouette, lifted from the map so the two pictures draw the same
// aircraft. The table is the RIGHT half of the outline, nose up, in a unit box
// (y negative toward the nose); the left half is the mirror walked backwards,
// which keeps the two sides identical by construction. The shape is concave,
// hence AddConcavePolyFilled.
constexpr float kPlaneHalf[][2] = {
    {0.00f, -1.00f},  // nose
    {0.13f, -0.70f},  // cockpit taper
    {0.13f, -0.26f},  // wing root, leading edge
    {0.98f, 0.16f},   // wing tip, leading edge
    {0.98f, 0.38f},   // wing tip, trailing edge
    {0.13f, 0.20f},   // wing root, trailing edge
    {0.13f, 0.62f},   // fuselage ahead of the tail
    {0.48f, 0.88f},   // tailplane tip, leading edge
    {0.48f, 1.02f},   // tailplane tip, trailing edge
    {0.08f, 0.94f},   // tail root
    {0.00f, 0.96f},   // tail, on the centreline
};
constexpr int kPlaneHalfCount = static_cast<int>(sizeof(kPlaneHalf) / sizeof(kPlaneHalf[0]));

// `filled` false draws the same silhouette as an OUTLINE, which is how an
// aircraft with NO REPORTED ALTITUDE is told apart from one at sea level:
// those are different facts, and a hue comparison at nine pixels is not a
// reliable way to separate them where a hollow shape against a solid one is.
void addPlane(ImDrawList* dl, const ImVec2& c, double courseDeg, float scale, ImU32 col,
              bool filled = true) {
    // Course 0 is north, which on a scope face is straight up; an unknown
    // course (NaN by ABI contract) draws the plane pointing north rather than
    // inventing a heading.
    const double a = (std::isnan(courseDeg) ? 0.0 : courseDeg) * kPi / 180.0;
    const float ca = static_cast<float>(std::cos(a));
    const float sa = static_cast<float>(std::sin(a));
    ImVec2 pts[2 * kPlaneHalfCount - 2];
    int n = 0;
    const auto put = [&](float x, float y) {
        pts[n++] = ImVec2(c.x + (x * ca - y * sa) * scale, c.y + (x * sa + y * ca) * scale);
    };
    for (int i = 0; i < kPlaneHalfCount; ++i) { put(kPlaneHalf[i][0], kPlaneHalf[i][1]); }
    for (int i = kPlaneHalfCount - 2; i >= 1; --i) { put(-kPlaneHalf[i][0], kPlaneHalf[i][1]); }
    // A dark rim on every plane, taking its alpha FROM the fill so an ageing
    // target fades as one thing. On a scope it does more work than on the map:
    // a small orange or lime silhouette sitting on a green ring needs an edge
    // or it reads as part of the furniture.
    const ImU32 rim = IM_COL32(0, 0, 0, (col >> IM_COL32_A_SHIFT) & 0xFFu);
    if (filled) {
        dl->AddConcavePolyFilled(pts, n, col);
        dl->AddPolyline(pts, n, rim, ImDrawFlags_Closed, 1.5f);
    } else {
        dl->AddPolyline(pts, n, rim, ImDrawFlags_Closed, 3.25f);
        dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 1.5f);
    }
}

// --- the detail panel ----------------------------------------------------------

// What the SYS screen can change, passed by reference so the panel and the face
// are looking at one set of switches rather than two copies of them.
struct ScopeOptions {
    bool phosphor;
    bool sweep;
    int filter;
    int trail;
};

// Everything the panel says about one aircraft, gathered from the track and
// from the track-info cache. ASKING THE CACHE IS WHAT STARTS THE LOOKUP, which
// is why this takes the cache rather than an already-fetched answer: selecting
// an aircraft is exactly the moment its registration should be queued for.
ScopeDetailInput makeScopeDetailInput(const cascade::core::HostTrack& ht,
                                      TrackInfoCache* info, bool hasRx, double rxLat,
                                      double rxLon) {
    ScopeDetailInput in;
    // The callsign where one has been decoded, the ICAO address where none has.
    // A target with no label must never render as an unidentified row.
    in.flight = (ht.t.label[0] != '\0') ? ht.t.label : ht.t.id;
    in.altM = ht.t.altM;
    in.speedMps = ht.t.speedMps;
    in.courseDeg = ht.t.courseDeg;
    in.emergency = (ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u;
    in.hasRx = hasRx;
    if (hasRx) {
        const ScopePolar p = scopeRelative(rxLat, rxLon, ht.t.latDeg, ht.t.lonDeg);
        in.rangeNm = p.rangeNm;
        in.bearingDeg = p.bearingDeg;
    }
    if (info != nullptr && info->active()) {
        in.infoActive = true;
        const TrackInfoCache::Info* d = info->get(std::string(ht.t.id), ht.t.kind);
        if (d == nullptr) {
            // Asked, nothing back yet. Distinct from an answer of "not in my
            // data", which is a d that exists with known == false and which
            // leaves the fields empty for buildScopeDetailLines to report.
            in.infoPending = true;
        } else if (d->known) {
            in.registration = d->registration;
            // The spelled-out type where the source has one, the code
            // otherwise: "737NG 8K5/W" tells a user what is overhead and
            // "B738" does not.
            in.typeName = !d->typeName.empty() ? d->typeName : d->typeCode;
            in.operatorName = d->operatorName;
        }
    }
    return in;
}

void textColoured(ImU32 col, const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

// The right-hand panel. `sel` is the selected aircraft, or null - which means
// either that nothing is selected or that what was selected is no longer being
// plotted, and those two are told apart by `selectedId`.
// THE TAB STRIP. Three raised keys, the live one lit. They are real buttons
// rather than painted ones: a tab that looked pressable and was not would be
// the same lie the design brief bans everywhere else on this panel.
//
// Returns the screen index chosen this frame, or `current` when none was.
int drawLcdTabs(const ImVec2& tl, float width, int current) {
    static const char* kNames[3] = {"HOME", "FLIGHT", "SYS"};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float gap = 8.0f;
    const float w = (width - gap * 2.0f) / 3.0f;
    const float h = ImGui::GetTextLineHeight() + 14.0f;
    int chosen = current;
    for (int i = 0; i < 3; ++i) {
        const ImVec2 a(tl.x + static_cast<float>(i) * (w + gap), tl.y);
        const ImVec2 b(a.x + w, a.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton(kNames[i], ImVec2(w, h));
        if (ImGui::IsItemClicked()) { chosen = i; }
        const bool on = (i == current);
        // The key's own lip, then the cap: two rects, which is what makes it
        // read as a key standing proud rather than a coloured rectangle.
        dl->AddRectFilled(ImVec2(a.x, a.y + 2.0f), ImVec2(b.x, b.y + 2.0f),
                          IM_COL32(16, 17, 9, 255), 6.0f);
        dl->AddRectFilled(a, b,
                          on ? IM_COL32(60, 74, 42, 255) : IM_COL32(45, 46, 36, 255),
                          6.0f);
        dl->AddLine(ImVec2(a.x + 6.0f, a.y + 1.0f), ImVec2(b.x - 6.0f, a.y + 1.0f),
                    IM_COL32(255, 255, 255, on ? 46 : 20), 1.0f);
        const ImVec2 sz = ImGui::CalcTextSize(kNames[i]);
        dl->AddText(ImVec2((a.x + b.x) * 0.5f - sz.x * 0.5f,
                           (a.y + b.y) * 0.5f - sz.y * 0.5f),
                    on ? IM_COL32(220, 240, 182, 255) : IM_COL32(141, 147, 121, 255),
                    kNames[i]);
    }
    return chosen;
}

void drawScopePanel(float width, float height, const cascade::core::HostTrack* sel,
                    const std::string& selectedId, TrackInfoCache* info, bool hasRx,
                    double rxLat, double rxLon, int& screen, int plotted,
                    int tracked, double topAltM, bool haveTopAlt, int rangeNm,
                    ScopeOptions& opts,
                    const std::vector<cascade::core::HostTrack>& tracks,
                    std::string& selectedIdInOut) {
    const ImVec2 bayTL = ImGui::GetCursorScreenPos();
    const ImVec2 bayBR(bayTL.x + width, bayTL.y + height);
    ImDrawList* bdl = ImGui::GetWindowDrawList();
    addScopeBay(bdl, bayTL, bayBR, false);

    const float pad = 12.0f;
    screen = drawLcdTabs(ImVec2(bayTL.x + pad, bayTL.y + pad), width - pad * 2.0f, screen);
    const float tabsH = ImGui::GetTextLineHeight() + 14.0f;

    // THE GLASS. A near-black green with its own recess and scan lines, so the
    // readout reads as something lit behind a window rather than text on the
    // case - which is the distinction the whole design is built on: brass is
    // what a hand may touch, glass is what the radio received.
    const ImVec2 gTL(bayTL.x + pad, bayTL.y + pad + tabsH + 10.0f);
    const ImVec2 gBR(bayBR.x - pad, bayBR.y - pad);
    bdl->AddRectFilled(gTL, gBR, IM_COL32(8, 22, 11, 255), 8.0f);
    bdl->AddRect(gTL, gBR, IM_COL32(37, 48, 32, 255), 8.0f, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(gTL.x + 10.0f, gTL.y + 8.0f));
    ImGui::BeginChild("##scopepanel", ImVec2(gBR.x - gTL.x - 20.0f, gBR.y - gTL.y - 16.0f),
                      false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // --- HOME: what the whole face is holding -------------------------------
    if (screen == 0) {
        // HOME IS THE REGISTER: every aircraft the face is plotting, nearest
        // first, with what is known about each. A screen of counts answers
        // "how many" and leaves "which ones" to hunting round the glass for a
        // silhouette small enough to miss - and on a busy sky that is the
        // question actually being asked.
        textColoured(kPanelLabel, "CONTACTS IN RANGE");
        {
            char big[16];
            std::snprintf(big, sizeof(big), "%d", plotted);
            ImGui::PushStyleColor(ImGuiCol_Text, kPanelValue);
            ImGui::SetWindowFontScale(2.0f);
            ImGui::TextUnformatted(big);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
        }
        {
            char sub[64];
            std::snprintf(sub, sizeof(sub), "of %d tracked   %d NM", tracked, rangeNm);
            textColoured(kPanelDim, sub);
        }
        ImGui::Separator();

        // Nearest first. Sorted by range from the ANTENNA, because "what is
        // closest to me" is what that ordering is for - the face's own centre
        // may have been dragged somewhere else entirely.
        struct Row {
            const cascade::core::HostTrack* t;
            double rangeNm;
            double bearingDeg;
        };
        std::vector<Row> rows;
        rows.reserve(tracks.size());
        for (const cascade::core::HostTrack& ht : tracks) {
            if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
            if (!cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) {
                continue;
            }
            Row r{&ht, 0.0, 0.0};
            if (hasRx) {
                const ScopePolar p =
                    scopeRelative(rxLat, rxLon, ht.t.latDeg, ht.t.lonDeg);
                r.rangeNm = p.rangeNm;
                r.bearingDeg = p.bearingDeg;
            }
            rows.push_back(r);
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.rangeNm < b.rangeNm;
        });

        if (rows.empty()) {
            ImGui::Spacing();
            // NOT AN EMPTY TABLE. A grid of headings with nothing under them
            // reads as a decoder that failed; saying the sky is quiet is the
            // honest answer, and it is a different one.
            textColoured(kPanelDim, "NOTHING BEING HEARD");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kPanelDim);
            ImGui::TextWrapped("No aircraft have been decoded. Check the antenna is "
                               "on 1090 MHz and try more gain.");
            ImGui::PopStyleColor();
        } else {
            ImGui::BeginChild("##homelist", ImVec2(0.0f, 0.0f), false);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const Row& r = rows[i];
                const cascade::core::HostTrack& ht = *r.t;
                const bool picked = selectedIdInOut == ht.t.id;
                ImGui::PushID(static_cast<int>(i));
                const ImVec2 tl = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                const float rowH = ImGui::GetTextLineHeight() * 2.0f + 6.0f;
                const bool hit = ImGui::InvisibleButton("##row", ImVec2(w, rowH));
                // CLICKING A ROW SELECTS THAT AIRCRAFT AND OPENS ITS PAGE,
                // which is the whole reason for showing the list rather than a
                // count: the register is a way IN to a target, not a readout.
                if (hit) {
                    selectedIdInOut = ht.t.id;
                    screen = 1;
                }
                ImDrawList* d = ImGui::GetWindowDrawList();
                if (picked || ImGui::IsItemHovered()) {
                    d->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + rowH),
                                     IM_COL32(134, 214, 74, picked ? 34 : 16), 3.0f);
                }
                // The callsign where one has been heard, the ICAO address
                // where none has - never a blank, which would read as an
                // aircraft with no identity rather than one not yet decoded.
                const char* name = (ht.t.label[0] != '\0') ? ht.t.label : ht.t.id;
                const bool alert = (ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u;
                d->AddText(ImVec2(tl.x + 4.0f, tl.y + 2.0f),
                           alert ? kAlert : kPanelValue, name);
                // Altitude beside it, banded by the same rule the face uses.
                char altTxt[24];
                if (std::isfinite(ht.t.altM)) {
                    std::snprintf(altTxt, sizeof(altTxt), "FL%03d",
                                  static_cast<int>(ht.t.altM * 3.28084 / 100.0));
                } else {
                    std::snprintf(altTxt, sizeof(altTxt), "NO ALT");
                }
                const ImVec2 asz = ImGui::CalcTextSize(altTxt);
                const int band = altitudeBandIndex(ht.t.altM);
                const AltBandStyle& bs = altBandStyle(band < 0 ? 0 : band);
                d->AddText(ImVec2(tl.x + w - asz.x - 4.0f, tl.y + 2.0f),
                           band < 0 ? kPanelDim
                                    : IM_COL32(bs.r, bs.g, bs.b, 255),
                           altTxt);
                // Range and bearing from the aerial on the second line.
                char sub[48];
                if (hasRx) {
                    std::snprintf(sub, sizeof(sub), "%.0f NM   %03d deg", r.rangeNm,
                                  static_cast<int>(r.bearingDeg + 0.5) % 360);
                } else {
                    std::snprintf(sub, sizeof(sub), "no receiver position");
                }
                d->AddText(ImVec2(tl.x + 4.0f, tl.y + ImGui::GetTextLineHeight() + 3.0f),
                           kPanelDim, sub);
                d->AddLine(ImVec2(tl.x, tl.y + rowH), ImVec2(tl.x + w, tl.y + rowH),
                           IM_COL32(134, 214, 74, 28), 1.0f);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    } else if (screen == 2) {
        // --- SYS: the switches, and they are switches -----------------------
        //
        // Every row here changes something visible on the face the moment it is
        // clicked. A settings screen of read-only facts would be a worse lie
        // than no settings screen: it looks like a place where things can be
        // changed and is not one.
        textColoured(kPanelLabel, "SYSTEM OPTIONS");
        ImGui::Spacing();
        // One clickable row: the name on the left, the current value as a pill
        // on the right. Returns true on the frame it was clicked.
        const auto option = [](const char* k, const char* v, bool on) {
            ImGui::PushID(k);
            const ImVec2 tl = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float rowH = ImGui::GetTextLineHeight() + 10.0f;
            const bool hit = ImGui::InvisibleButton("##opt", ImVec2(w, rowH));
            ImDrawList* d = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered()) {
                d->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + rowH),
                                 IM_COL32(134, 214, 74, 18), 3.0f);
            }
            d->AddText(ImVec2(tl.x + 2.0f, tl.y + 5.0f), kPanelLabel, k);
            const ImVec2 vs = ImGui::CalcTextSize(v);
            const ImVec2 pTL(tl.x + w - vs.x - 16.0f, tl.y + 3.0f);
            const ImVec2 pBR(tl.x + w - 2.0f, tl.y + rowH - 3.0f);
            d->AddRectFilled(pTL, pBR,
                             on ? IM_COL32(134, 214, 74, 40) : IM_COL32(120, 140, 90, 30),
                             3.0f);
            d->AddText(ImVec2(pTL.x + 7.0f, tl.y + 5.0f),
                       on ? kPanelValue : kPanelDim, v);
            d->AddLine(ImVec2(tl.x, tl.y + rowH), ImVec2(tl.x + w, tl.y + rowH),
                       IM_COL32(134, 214, 74, 33), 1.0f);
            ImGui::PopID();
            return hit;
        };

        if (option("PHOSPHOR", opts.phosphor ? "ON" : "OFF", opts.phosphor)) {
            opts.phosphor = !opts.phosphor;
        }
        if (option("SWEEP", opts.sweep ? "ON" : "OFF", opts.sweep)) {
            opts.sweep = !opts.sweep;
        }
        static const char* kTrailNames[3] = {"OFF", "LINE", "RIBBON"};
        if (option("TRAILS", kTrailNames[opts.trail], opts.trail != 0)) {
            opts.trail = (opts.trail + 1) % 3;
        }
        static const char* kFilterNames[3] = {"ALL", "ALERT", "NAMED"};
        if (option("FILTER", kFilterNames[opts.filter], opts.filter != 0)) {
            opts.filter = (opts.filter + 1) % 3;
        }
        ImGui::Spacing();
        ImGui::Spacing();
        const auto fact = [](const char* k, const char* v) {
            textColoured(kPanelLabel, k);
            textColoured(kPanelValue, v);
        };
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d NM", rangeNm);
        fact("RANGE", buf);
        if (hasRx) {
            std::snprintf(buf, sizeof(buf), "%.3f %.3f", rxLat, rxLon);
            fact("ANTENNA", buf);
        } else {
            fact("ANTENNA", "NOT SET");
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kPanelDim);
        ImGui::TextWrapped("Drag the face to move the view, double-click to recentre, "
                           "and the wheel steps the range. FILTER hides everything but "
                           "emergencies, or everything without a callsign. A trail "
                           "starts when the scope is opened, not when the aircraft was "
                           "first heard.");
        ImGui::PopStyleColor();
    } else {

    // THE FOOT OF THE PANEL IS RESERVED BEFORE THE ROWS ARE DRAWN, not left
        // over after them. A child sized to the remaining height takes all of it,
        // so the note below would land outside the panel and never be seen -
        // which for a line whose entire job is to be honest about what is
        // missing would be worse than not writing it. Four lines: the separator
        // plus a note that wraps to three at the narrowest panel this layout
        // allows.
        const float rowsH = tableHeightReservingLines(
            ImGui::GetContentRegionAvail().y, ImGui::GetTextLineHeightWithSpacing(), 4);
        ImGui::BeginChild("##scoperows", ImVec2(0.0f, rowsH), false);

    if (sel == nullptr) {
        if (selectedId.empty()) {
            // NOT AN EMPTY GRID OF LABELS. A panel showing FLIGHT, OPERATOR,
            // TYPE and the rest with nothing beside any of them reads as a
            // receiver that failed to decode them; saying there is no
            // selection, and how to make one, is the only honest thing here.
            textColoured(kPanelDim, "NOTHING SELECTED");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kPanelDim);
            ImGui::TextWrapped("Click an aircraft on the scope to see its details.");
            ImGui::PopStyleColor();
        } else {
            // THE SELECTION IS KEPT, NOT CLEARED. The aircraft has gone stale,
            // left the range, or the plugin stopped reporting it; a panel that
            // silently emptied itself would look like a fault, and holding the
            // id means the details come back by themselves if the aircraft
            // does.
            textColoured(kPanelDim, "TARGET LOST");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kPanelDim);
            ImGui::TextWrapped("%s is no longer being plotted - it has gone quiet or "
                               "left the selected range.",
                               selectedId.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        const std::vector<ScopeDetailLine> lines =
            buildScopeDetailLines(makeScopeDetailInput(*sel, info, hasRx, rxLat, rxLon));
        // MEASURED WITH THE FONT IN USE, not assumed. The label column has to
        // hold the longest label actually present, and a constant here is a
        // promise about a font the host chooses at runtime - which is how the
        // map's altitude legend came to clip half its own labels.
        float labelW = 0.0f;
        for (const ScopeDetailLine& l : lines) {
            labelW = std::max(labelW, ImGui::CalcTextSize(l.label.c_str()).x);
        }
        labelW += 12.0f;
        for (const ScopeDetailLine& l : lines) {
            textColoured(kPanelLabel, l.label.c_str());
            ImGui::SameLine(labelW);
            // An emergency outranks the known/unknown styling, which is the
            // same precedence the colour rule at the top of this file states.
            const ImU32 col = l.alert ? kAlert : (l.known ? kPanelValue : kPanelDim);
            textColoured(col, l.value.c_str());
        }
    }

        ImGui::EndChild();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kPanelDim);
        ImGui::TextWrapped("%s", scopeUnavailableNote());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

}  // namespace

// --- the instrument face ------------------------------------------------------

namespace {

// The enclosure's own colours, kept apart from the scope's phosphor palette
// above: the bezel is a moulded case in the photograph, not a lit surface, and
// giving it the screen's greens would make the whole window read as one glowing
// slab with no depth to it.
constexpr ImU32 kCaseFill = IM_COL32(26, 28, 30, 255);
constexpr ImU32 kCaseEdge = IM_COL32(58, 62, 66, 255);
constexpr ImU32 kCaseInset = IM_COL32(16, 18, 20, 255);
constexpr ImU32 kScrewRim = IM_COL32(74, 78, 82, 255);
constexpr ImU32 kScrewFill = IM_COL32(38, 41, 44, 255);
constexpr ImU32 kPowerLit = IM_COL32(64, 226, 106, 255);
constexpr ImU32 kPowerDark = IM_COL32(48, 58, 50, 255);

}  // namespace

// The bay every group of controls is recessed into.
void addScopeBay(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, bool lower) {
    if (dl == nullptr || br.x - tl.x < 4.0f || br.y - tl.y < 4.0f) { return; }
    const ImU32 top = lower ? IM_COL32(34, 35, 27, 255) : IM_COL32(28, 29, 22, 255);
    const ImU32 bot = lower ? IM_COL32(19, 20, 9, 255) : IM_COL32(16, 17, 8, 255);
    const ImU32 edge = lower ? IM_COL32(51, 52, 42, 255) : IM_COL32(47, 48, 38, 255);
    constexpr float kRound = 10.0f;
    // AddRectFilledMultiColor cannot round its corners, so the rounded shape is
    // laid down first in the mid tone and the gradient is inset by the corner
    // radius. What shows in the four corner arcs is the mid tone, which at this
    // contrast is indistinguishable from the ramp continuing through them.
    const ImU32 mid = IM_COL32(23, 24, 15, 255);
    dl->AddRectFilled(tl, br, mid, kRound);
    if (br.x - tl.x > kRound * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(tl.x + kRound, tl.y),
                                    ImVec2(br.x - kRound, br.y), top, top, bot, bot);
    }
    dl->AddRect(tl, br, edge, kRound, 0, 1.0f);
    // The 1 px top highlight, which is what makes the panel read as lit from
    // above rather than as a flat hole.
    dl->AddLine(ImVec2(tl.x + kRound, tl.y + 1.0f), ImVec2(br.x - kRound, tl.y + 1.0f),
                IM_COL32(255, 255, 255, 14), 1.0f);
}

// THE SEGMENTED BARGRAPH, which is what the design actually specifies and what
// the tick-scale this replaced was not. Eighteen segments filling from the
// BOTTOM, lit in the lamp green with a glow and unlit in a barely-there olive,
// a tracked capital label above and the reading below.
//
// STILL NO BAR AT ALL WITHOUT A READING. The segments are drawn unlit, which
// says "this meter has no input"; lighting none of them would say "we measured
// and it is zero", and those are different statements. The product has been
// bitten by that exact conflation before, in a panel that rendered clean
// zeroes when it had no data at all.
void drawScopeGauge(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                    const char* label, double frac, bool haveReading,
                    const char* readout) {
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 40.0f) { return; }
    addScopeBay(dl, tl, br, false);

    const float lineH = ImGui::GetTextLineHeight();
    const float top = tl.y + lineH + 8.0f;
    const float bot = br.y - lineH - 8.0f;
    if (bot <= top) { return; }

    constexpr int kSegments = 18;
    const float pitch = (bot - top) / static_cast<float>(kSegments);
    const float segH = std::max(2.0f, pitch - 3.0f);
    const float x0 = tl.x + 8.0f;
    const float x1 = br.x - 8.0f;
    if (x1 <= x0) { return; }

    double f = frac;
    if (!(f >= 0.0)) { f = 0.0; }
    if (f > 1.0) { f = 1.0; }
    const int lit = haveReading
                        ? static_cast<int>(f * static_cast<double>(kSegments) + 0.5)
                        : 0;

    for (int i = 0; i < kSegments; ++i) {
        // Segment zero at the BOTTOM: the design's column-reverse, and the
        // direction every meter in this application already fills.
        const float y1s = bot - static_cast<float>(i) * pitch;
        const float y0s = y1s - segH;
        const bool on = i < lit;
        if (on) {
            // The glow, as two grown rectangles rather than a blur.
            dl->AddRectFilled(ImVec2(x0 - 1.0f, y0s - 1.0f), ImVec2(x1 + 1.0f, y1s + 1.0f),
                              IM_COL32(154, 216, 79, 40), 2.0f);
        }
        dl->AddRectFilled(ImVec2(x0, y0s), ImVec2(x1, y1s),
                          on ? IM_COL32(154, 216, 79, 255) : IM_COL32(120, 140, 90, 33),
                          1.0f);
    }

    const ImVec2 lsz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2((tl.x + br.x) * 0.5f - lsz.x * 0.5f, tl.y + 4.0f),
                IM_COL32(111, 117, 97, 255), label);
    const char* rd = (haveReading && readout != nullptr) ? readout : "--";
    const ImVec2 rsz = ImGui::CalcTextSize(rd);
    dl->AddText(ImVec2((tl.x + br.x) * 0.5f - rsz.x * 0.5f, br.y - lineH - 4.0f),
                haveReading ? IM_COL32(154, 216, 79, 255) : kChromeDim, rd);
}

// The odometer drums. Each digit sits in its own machined aperture with the
// shading top and bottom that makes a printed digit read as one painted on a
// drum behind a window.
void drawScopeDrums(ImDrawList* dl, const ImVec2& tl, float cellW, float cellH,
                    int digits, int value, const char* caption) {
    if (dl == nullptr || digits <= 0 || digits > 8) { return; }
    const float lineH = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(tl.x, tl.y), IM_COL32(118, 124, 100, 255), caption);
    const float rowY = tl.y + lineH + 4.0f;

    int v = value;
    if (v < 0) { v = 0; }
    // Clamped rather than wrapped, and clamped to all-nines: a three-digit
    // counter shown a four-digit number must read "as high as I go", not the
    // bottom three digits of it, which would be a smaller number than the truth.
    int cap = 1;
    for (int i = 0; i < digits; ++i) { cap *= 10; }
    if (v > cap - 1) { v = cap - 1; }

    for (int i = 0; i < digits; ++i) {
        const float x = tl.x + static_cast<float>(i) * (cellW + 3.0f);
        const ImVec2 cTL(x, rowY);
        const ImVec2 cBR(x + cellW, rowY + cellH);
        dl->AddRectFilled(cTL, cBR, IM_COL32(14, 15, 10, 255), 3.0f);
        dl->AddRect(cTL, cBR, IM_COL32(58, 59, 47, 255), 3.0f, 0, 1.0f);
        // The recess, top and bottom, as one band each.
        dl->AddRectFilledMultiColor(cTL, ImVec2(cBR.x, cTL.y + cellH * 0.28f),
                                    IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190),
                                    IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
        dl->AddRectFilledMultiColor(ImVec2(cTL.x, cBR.y - cellH * 0.28f), cBR,
                                    IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                    IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190));
        int place = 1;
        for (int k = 0; k < digits - 1 - i; ++k) { place *= 10; }
        const int digit = (v / place) % 10;
        char txt[2] = {static_cast<char>('0' + digit), '\0'};
        const ImVec2 sz = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2((cTL.x + cBR.x) * 0.5f - sz.x * 0.5f,
                           (cTL.y + cBR.y) * 0.5f - sz.y * 0.5f),
                    IM_COL32(231, 234, 212, 255), txt);
    }
}

// The ladder's numbers on the knob's own arc, +/-135 degrees as the design
// sweeps it.
void drawScopeKnobTicks(ImDrawList* dl, const ImVec2& centre, float radius,
                        const int* values, int count, int selectedIndex) {
    if (dl == nullptr || values == nullptr || count < 2) { return; }
    const float rx = radius * 1.45f;
    const float ry = radius * 1.30f;
    for (int i = 0; i < count; ++i) {
        const double deg = -135.0 + 270.0 * static_cast<double>(i) /
                                        static_cast<double>(count - 1);
        const double a = deg * kPi / 180.0;
        char txt[8];
        std::snprintf(txt, sizeof(txt), "%d", values[i]);
        const ImVec2 sz = ImGui::CalcTextSize(txt);
        const float x = centre.x + static_cast<float>(std::sin(a)) * rx - sz.x * 0.5f;
        const float y = centre.y - static_cast<float>(std::cos(a)) * ry - sz.y * 0.5f;
        dl->AddText(ImVec2(x, y),
                    i == selectedIndex ? IM_COL32(223, 226, 205, 255)
                                       : IM_COL32(107, 112, 89, 255),
                    txt);
    }
}

// --- the 1960s bench --------------------------------------------------------
//
// Every value here is the handoff's own: the amber #F0A840 and its dimmed
// #8a5a2a, the cell's #0d0b07 -> #181410 -> #0d0b07 gradient and its #3b3529
// hairline, the well's #100d09 and #2a251c, and the dial's #8b8069 brass with
// a #EFE7D2 pointer.

constexpr ImU32 kAmber = IM_COL32(240, 168, 64, 255);
constexpr ImU32 kAmberDim = IM_COL32(138, 90, 42, 255);
constexpr ImU32 kBrass = IM_COL32(139, 128, 105, 255);
constexpr ImU32 kBenchInk = IM_COL32(59, 53, 41, 255);
constexpr ImU32 kIvory = IM_COL32(239, 231, 210, 255);

void drawFreqDrumWell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr) { return; }
    dl->AddRectFilled(tl, br, IM_COL32(16, 13, 9, 255), 3.0f);
    // The recess: two inset rings rather than a blur, which ImGui has not.
    dl->AddRect(tl, br, IM_COL32(42, 37, 28, 255), 3.0f, 0, 2.0f);
    dl->AddRectFilledMultiColor(ImVec2(tl.x + 2.0f, tl.y + 2.0f),
                                ImVec2(br.x - 2.0f, tl.y + 10.0f),
                                IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
}

void drawFreqDrumCell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, char digit,
                      bool bright, float fontPx) {
    if (dl == nullptr) { return; }
    // The drum face: a three-stop vertical gradient, lighter across the
    // middle, which is what makes a flat rectangle read as a curved surface
    // seen through a window.
    const float midY = (tl.y + br.y) * 0.5f;
    dl->AddRectFilledMultiColor(tl, ImVec2(br.x, midY), IM_COL32(13, 11, 7, 255),
                                IM_COL32(13, 11, 7, 255), IM_COL32(24, 20, 16, 255),
                                IM_COL32(24, 20, 16, 255));
    dl->AddRectFilledMultiColor(ImVec2(tl.x, midY), br, IM_COL32(24, 20, 16, 255),
                                IM_COL32(24, 20, 16, 255), IM_COL32(13, 11, 7, 255),
                                IM_COL32(13, 11, 7, 255));
    dl->AddRect(tl, br, kBenchInk, 0.0f, 0, 1.0f);

    const char txt[2] = {digit, '\0'};
    ImFont* font = ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, txt);
    const ImVec2 at((tl.x + br.x) * 0.5f - sz.x * 0.5f,
                    (tl.y + br.y) * 0.5f - sz.y * 0.5f);
    // The glow, as offset copies. A lit digit on a dark drum is most of what
    // makes this read as a nixie rather than as text on a box.
    if (bright) {
        const ImU32 halo = IM_COL32(240, 168, 64, 40);
        dl->AddText(font, fontPx, ImVec2(at.x - 1.0f, at.y), halo, txt);
        dl->AddText(font, fontPx, ImVec2(at.x + 1.0f, at.y), halo, txt);
        dl->AddText(font, fontPx, ImVec2(at.x, at.y - 1.0f), halo, txt);
        dl->AddText(font, fontPx, ImVec2(at.x, at.y + 1.0f), halo, txt);
    }
    dl->AddText(font, fontPx, at, bright ? kAmber : kAmberDim, txt);
}

float drawBrassVolumeKnob(ImDrawList* dl, const ImVec2& centre, float radius,
                          float value) {
    if (dl == nullptr || radius < 6.0f) { return -1.0f; }
    ImGui::SetCursorScreenPos(ImVec2(centre.x - radius, centre.y - radius));
    ImGui::InvisibleButton("##volknob", ImVec2(radius * 2.0f, radius * 2.0f));
    float out = -1.0f;

    // THE SAME TWIST THE RANGE KNOB USES, for the same reason: a dial that
    // answered a vertical slide would be a dial in appearance only. Volume is
    // continuous, so unlike the range knob this one reports the angle itself
    // rather than detents.
    static float lastAngle = 0.0f;
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float a =
            std::atan2(m.x - centre.x, centre.y - m.y) * 180.0f / 3.14159265f;
        if (ImGui::IsItemActivated()) { lastAngle = a; }
        float d = a - lastAngle;
        while (d > 180.0f) { d -= 360.0f; }
        while (d < -180.0f) { d += 360.0f; }
        lastAngle = a;
        out = std::clamp(value + d / 270.0f, 0.0f, 1.0f);
    }
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) { out = std::clamp(value + wheel * 0.05f, 0.0f, 1.0f); }
    }

    // Nine ticks on the same 270-degree arc the pointer sweeps.
    for (int i = 0; i < 9; ++i) {
        const float deg = -135.0f + 270.0f * static_cast<float>(i) / 8.0f;
        const float a = deg * 3.14159265f / 180.0f;
        const float sx = std::sin(a);
        const float sy = -std::cos(a);
        dl->AddLine(ImVec2(centre.x + sx * radius * 1.22f, centre.y + sy * radius * 1.22f),
                    ImVec2(centre.x + sx * radius * 1.38f, centre.y + sy * radius * 1.38f),
                    kBenchInk, 1.4f);
    }

    dl->AddCircleFilled(ImVec2(centre.x, centre.y + radius * 0.10f), radius * 1.02f,
                        IM_COL32(0, 0, 0, 110), 32);
    dl->AddCircleFilled(centre, radius, IM_COL32(13, 11, 7, 255), 32);
    dl->AddCircleFilled(ImVec2(centre.x - radius * 0.14f, centre.y - radius * 0.22f),
                        radius * 0.80f, IM_COL32(25, 21, 16, 255), 32);
    dl->AddCircleFilled(ImVec2(centre.x - radius * 0.22f, centre.y - radius * 0.30f),
                        radius * 0.48f, IM_COL32(47, 42, 33, 255), 32);
    // The brass ring, which is the whole character of this control.
    dl->AddCircle(centre, radius - 1.0f, kBrass, 32, std::max(2.0f, radius * 0.14f));

    float f = value;
    if (!(f >= 0.0f)) { f = 0.0f; }
    if (f > 1.0f) { f = 1.0f; }
    const float deg = -135.0f + 270.0f * f;
    const float a = deg * 3.14159265f / 180.0f;
    const float sx = std::sin(a);
    const float sy = -std::cos(a);
    dl->AddLine(ImVec2(centre.x + sx * radius * 0.18f, centre.y + sy * radius * 0.18f),
                ImVec2(centre.x + sx * radius * 0.78f, centre.y + sy * radius * 0.78f),
                kIvory, std::max(2.0f, radius * 0.16f));
    return out;
}

void drawBenchLamp(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 colour,
                   bool lit, const char* caption) {
    if (dl == nullptr || radius < 2.0f) { return; }
    // The bezel it is set into, then the lens.
    dl->AddCircleFilled(centre, radius + 1.5f, IM_COL32(42, 37, 28, 255), 20);
    if (lit) {
        // The bloom, as three grown discs rather than a blur.
        for (int i = 3; i >= 1; --i) {
            dl->AddCircleFilled(centre, radius + static_cast<float>(i) * 1.6f,
                                (colour & 0x00FFFFFFu) | (static_cast<ImU32>(26 - i * 6)
                                                          << IM_COL32_A_SHIFT),
                                20);
        }
        dl->AddCircleFilled(centre, radius, colour, 20);
        // The highlight, off-centre, which is what makes a disc read as glass.
        dl->AddCircleFilled(ImVec2(centre.x - radius * 0.28f, centre.y - radius * 0.30f),
                            radius * 0.38f, IM_COL32(255, 255, 255, 70), 12);
    } else {
        // Unlit is the lamp's own hue taken right down - not grey, because an
        // unlit red lamp and an unlit green one are different objects and a
        // panel reads better when you can tell which is which cold.
        dl->AddCircleFilled(centre, radius,
                            (colour & 0x00FFFFFFu) | (static_cast<ImU32>(48)
                                                      << IM_COL32_A_SHIFT),
                            20);
    }
    dl->AddCircle(centre, radius + 1.5f, IM_COL32(0x8B, 0x80, 0x69, 190), 20, 1.0f);

    if (caption != nullptr && caption[0] != '\0') {
        const ImVec2 sz = ImGui::CalcTextSize(caption);
        dl->AddText(ImVec2(centre.x - sz.x * 0.5f, centre.y + radius + 4.0f),
                    lit ? IM_COL32(0xEF, 0xE7, 0xD2, 255) : IM_COL32(0x9C, 0x90, 0x78, 255),
                    caption);
    }
}

void drawBenchMeter(ImDrawList* dl, const ImVec2& tl, float width, float height,
                    const char* caption, float frac01, bool haveReading,
                    const char* valueLine) {
    if (dl == nullptr || width < 40.0f || height < 40.0f) { return; }
    const float lineH = ImGui::GetTextLineHeight();

    // The caption is ENGRAVED into the brass above the face; the value line is
    // ivory on the dark strip below it. That split is the design's own rule -
    // a caption may be cut into metal, a figure must be on glass - and it is
    // the reason the value is not simply dark-on-brass like the caption: at
    // this size that is about 2.3:1 contrast and not readable at arm's length.
    if (caption != nullptr) {
        const ImVec2 cs = ImGui::CalcTextSize(caption);
        dl->AddText(ImVec2(tl.x + width * 0.5f - cs.x * 0.5f, tl.y),
                    IM_COL32(0x3B, 0x35, 0x29, 255), caption);
    }

    const float faceTop = tl.y + lineH + 3.0f;
    const float faceH = height - lineH * 2.0f - 8.0f;
    if (faceH < 20.0f) { return; }
    const ImVec2 fTL(tl.x, faceTop);
    const ImVec2 fBR(tl.x + width, faceTop + faceH);

    // The tombstone face: cream, with a brass bezel.
    dl->AddRectFilledMultiColor(fTL, fBR, IM_COL32(0xF3, 0xEC, 0xD6, 255),
                                IM_COL32(0xF3, 0xEC, 0xD6, 255),
                                IM_COL32(0xD8, 0xCF, 0xB4, 255),
                                IM_COL32(0xD8, 0xCF, 0xB4, 255));
    dl->AddRect(fTL, fBR, IM_COL32(0x8B, 0x80, 0x69, 255), 3.0f, 0, 2.0f);

    // The pivot sits below the face so the needle sweeps the top of it, which
    // is how a moving-coil meter is actually built.
    const ImVec2 pivot(tl.x + width * 0.5f, fBR.y - 4.0f);
    const float armR = faceH * 0.78f;
    constexpr float kHalfSweepDeg = 52.0f;

    // Nine ticks, the last two in the alarm colour: the top of any meter's
    // travel is where it should be uncomfortable to sit.
    for (int i = 0; i < 9; ++i) {
        const float t = static_cast<float>(i) / 8.0f;
        const float deg = -kHalfSweepDeg + 2.0f * kHalfSweepDeg * t;
        const float a = deg * 3.14159265f / 180.0f;
        const float sx = std::sin(a);
        const float sy = -std::cos(a);
        const ImU32 col = (i >= 7) ? IM_COL32(0xB8, 0x55, 0x2F, 255)
                                   : IM_COL32(0x3B, 0x35, 0x29, 255);
        dl->AddLine(ImVec2(pivot.x + sx * armR * 0.80f, pivot.y + sy * armR * 0.80f),
                    ImVec2(pivot.x + sx * armR * 0.94f, pivot.y + sy * armR * 0.94f), col,
                    (i % 4 == 0) ? 1.8f : 1.0f);
    }

    if (haveReading) {
        float f = frac01;
        if (!(f >= 0.0f)) { f = 0.0f; }
        if (f > 1.0f) { f = 1.0f; }
        const float deg = -kHalfSweepDeg + 2.0f * kHalfSweepDeg * f;
        const float a = deg * 3.14159265f / 180.0f;
        const float sx = std::sin(a);
        const float sy = -std::cos(a);
        dl->AddLine(pivot, ImVec2(pivot.x + sx * armR * 0.88f, pivot.y + sy * armR * 0.88f),
                    IM_COL32(0xB8, 0x55, 0x2F, 255), 1.8f);
        dl->AddCircleFilled(pivot, 3.4f, IM_COL32(0x2A, 0x25, 0x1C, 255), 12);
    } else {
        // NO NEEDLE AT ALL. See the header: a needle at rest would be a
        // measurement of zero, and there is no measurement.
        dl->AddCircleFilled(pivot, 3.4f, IM_COL32(0x9C, 0x90, 0x78, 255), 12);
    }

    if (valueLine != nullptr) {
        const ImVec2 vs = ImGui::CalcTextSize(valueLine);
        dl->AddText(ImVec2(tl.x + width * 0.5f - vs.x * 0.5f, fBR.y + 3.0f),
                    haveReading ? IM_COL32(0xEF, 0xE7, 0xD2, 255)
                                : IM_COL32(0x9C, 0x90, 0x78, 255),
                    valueLine);
    }
}

void drawRailChip(ImDrawList* dl, const ImVec2& headerMin, const ImVec2& headerMax,
                  const char* chipText, ImU32 lampColour, bool lampLit) {
    if (dl == nullptr) { return; }
    const float h = headerMax.y - headerMin.y;
    const float cy = (headerMin.y + headerMax.y) * 0.5f;

    // The lamp sits hard against the right edge of the plate, and the chip
    // just inboard of it - so a glance down the rail reads as a column of
    // states rather than as a list of names.
    const float lampR = std::max(3.0f, h * 0.20f);
    const ImVec2 lampC(headerMax.x - lampR - 6.0f, cy);
    drawBenchLamp(dl, lampC, lampR, lampColour, lampLit, nullptr);

    if (chipText != nullptr && chipText[0] != '\0') {
        const ImVec2 ts = ImGui::CalcTextSize(chipText);
        const float padX = 5.0f;
        const ImVec2 cBR(lampC.x - lampR - 7.0f, cy + ts.y * 0.5f + 2.0f);
        const ImVec2 cTL(cBR.x - ts.x - padX * 2.0f, cy - ts.y * 0.5f - 2.0f);
        if (cTL.x > headerMin.x + 60.0f) {
            // A chip is a READING about that section, so it goes on glass in
            // amber rather than being engraved into the plate.
            dl->AddRectFilled(cTL, cBR, IM_COL32(0x14, 0x11, 0x0C, 220), 2.0f);
            dl->AddRect(cTL, cBR, IM_COL32(0x8B, 0x80, 0x69, 120), 2.0f, 0, 1.0f);
            dl->AddText(ImVec2(cTL.x + padX, cy - ts.y * 0.5f),
                        IM_COL32(0xF0, 0xA8, 0x40, 255), chipText);
        }
    }
}

int drawScopeKnob(ImDrawList* dl, const ImVec2& centre, float radius,
                  const char* label, const char* valueText, bool interactive,
                  float fraction) {
    if (dl == nullptr || radius < 8.0f) { return 0; }
    int steps = 0;

    // The hit area is an ImGui item so the knob takes part in the same input
    // arbitration as everything else - without one it would swallow drags
    // meant for whatever is underneath.
    ImGui::SetCursorScreenPos(ImVec2(centre.x - radius, centre.y - radius));
    ImGui::InvisibleButton("##scopeknob", ImVec2(radius * 2.0f, radius * 2.0f));
    const bool hovered = ImGui::IsItemHovered();
    if (interactive) {
        // IT TWISTS. The hand goes round the knob and the knob follows it,
        // which is what a knob is; the vertical slide this replaced worked but
        // felt like nothing on the panel it is drawn on, and was reported as
        // exactly that.
        //
        // The angle from the middle to the cursor is tracked frame to frame
        // and the DIFFERENCE accumulated, so the knob never jumps to wherever
        // the hand happened to grab it - a real one does not either. Steps stay
        // discrete because the value is a ladder, so a continuous angle would
        // be reporting a precision the control does not have.
        static float dragAccum = 0.0f;
        static float lastAngle = 0.0f;
        if (ImGui::IsItemActive()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float a = std::atan2(m.x - centre.x, centre.y - m.y) * 180.0f /
                            3.14159265f;
            if (ImGui::IsItemActivated()) {
                lastAngle = a;
                dragAccum = 0.0f;
            }
            float d = a - lastAngle;
            // Across the twelve-o'clock seam the raw difference is ~360; the
            // short way round is always the one the hand actually travelled.
            while (d > 180.0f) { d -= 360.0f; }
            while (d < -180.0f) { d += 360.0f; }
            dragAccum += d;
            lastAngle = a;
            constexpr float kDegPerDetent = 20.0f;
            while (dragAccum >= kDegPerDetent) { steps += 1; dragAccum -= kDegPerDetent; }
            while (dragAccum <= -kDegPerDetent) { steps -= 1; dragAccum += kDegPerDetent; }
        } else {
            dragAccum = 0.0f;
        }
        if (hovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel > 0.0f) { steps += 1; }
            if (wheel < 0.0f) { steps -= 1; }
        }
    }

    // THE KNOB, FROM THE PHOTOGRAPH OF THE REAL UNIT: a dark machined disc lit
    // from the upper left, sunk in its own shadow, with one bright pointer
    // running from the middle to the edge. What this replaced was a flat grey
    // disc with a short stub near its rim - the same control, and nothing like
    // the same object.
    //
    // The eccentric highlight is what does most of the work. A disc shaded
    // concentrically reads as a hole; the same disc lit off-centre reads as
    // something turned on a lathe.
    dl->AddCircleFilled(ImVec2(centre.x, centre.y + radius * 0.06f), radius * 1.04f,
                        IM_COL32(0, 0, 0, 120), 48);
    dl->AddCircleFilled(centre, radius, IM_COL32(12, 13, 15, 255), 48);
    dl->AddCircleFilled(centre, radius * 0.93f, IM_COL32(30, 32, 35, 255), 48);
    dl->AddCircleFilled(ImVec2(centre.x - radius * 0.16f, centre.y - radius * 0.20f),
                        radius * 0.74f, IM_COL32(46, 49, 53, 255), 48);
    dl->AddCircleFilled(ImVec2(centre.x - radius * 0.24f, centre.y - radius * 0.30f),
                        radius * 0.46f, IM_COL32(62, 66, 71, 255), 48);
    // The rim, and the one part that answers the pointer.
    dl->AddCircle(centre, radius * 0.93f, hovered ? kRim : IM_COL32(78, 82, 88, 255), 48,
                  1.6f);
    // THE POINTER RUNS THE WHOLE RADIUS, as it does in the photograph - a full
    // white index line from the middle out, not a tick near the edge - AND IT
    // POINTS AT THE SETTING. The 270-degree sweep is the same one the scale
    // around the knob is laid out on, so the line and the numbers agree by
    // construction rather than by being tuned to look right.
    {
        float f = fraction;
        if (!(f >= 0.0f)) { f = 0.0f; }
        if (f > 1.0f) { f = 1.0f; }
        const float deg = -135.0f + 270.0f * f;
        const float a = deg * 3.14159265f / 180.0f;
        const float sx = std::sin(a);
        const float sy = -std::cos(a);
        dl->AddLine(ImVec2(centre.x + sx * radius * 0.04f, centre.y + sy * radius * 0.04f),
                    ImVec2(centre.x + sx * radius * 0.86f, centre.y + sy * radius * 0.86f),
                    IM_COL32(236, 240, 245, 255), std::max(2.0f, radius * 0.09f));
    }

    const float lineH = ImGui::GetTextLineHeight();
    // ABOVE THE TICK ARC, not just above the knob. The scale's middle tick sits
    // straight up at radius * 1.30, so a caption placed off the knob's own
    // radius lands on top of it - which it did, rendering "GAIN" and "30"
    // through each other as "G30N".
    const ImVec2 lsz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(centre.x - lsz.x * 0.5f,
                       centre.y - radius * 1.30f - lineH * 2.0f - 4.0f),
                kChrome, label);
    if (valueText != nullptr) {
        const ImVec2 vsz = ImGui::CalcTextSize(valueText);
        dl->AddText(ImVec2(centre.x - vsz.x * 0.5f,
                           centre.y + radius * 1.30f + 6.0f),
                    kChrome, valueText);
    }
    return steps;
}

bool drawScopePowerButton(ImDrawList* dl, const ImVec2& centre, float radius,
                          bool running) {
    if (dl == nullptr || radius < 8.0f) { return false; }
    ImGui::SetCursorScreenPos(ImVec2(centre.x - radius, centre.y - radius));
    ImGui::InvisibleButton("##scopepower", ImVec2(radius * 2.0f, radius * 2.0f));
    const bool pressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();

    // THE LAMP, FROM THE PHOTOGRAPH: a bright green ring standing proud of a
    // black well, throwing a halo onto the plate around it, with the IEC power
    // mark cut out of the middle in the same green.
    //
    // The halo is drawn as a handful of grown circles rather than a blur,
    // which ImGui has not - six steps is enough that the banding is invisible
    // at this radius and cheap enough to do every frame.
    if (running) {
        for (int i = 6; i >= 1; --i) {
            const float g = radius * (1.0f + 0.10f * static_cast<float>(i));
            const int alpha = 26 - i * 3;
            if (alpha <= 0) { continue; }
            dl->AddCircleFilled(centre, g, IM_COL32(110, 226, 74, alpha), 44);
        }
    }

    // The well the lamp sits in, and its own shadow.
    dl->AddCircleFilled(centre, radius, IM_COL32(10, 11, 12, 255), 48);
    dl->AddCircle(centre, radius, IM_COL32(52, 56, 52, 255), 48, 1.4f);

    // The lit ring. Bright when the receiver runs, a dead olive when it does
    // not - and the state is spelled out in words beside it by the caller, so
    // the colour is never carrying it alone.
    const float ringR = radius * 0.66f;
    const ImU32 lit = hovered ? IM_COL32(150, 255, 110, 255) : IM_COL32(110, 226, 74, 255);
    const ImU32 dark = IM_COL32(38, 52, 34, 255);
    dl->AddCircle(centre, ringR, running ? lit : dark, 48,
                  std::max(2.5f, radius * 0.12f));

    // The IEC mark: a ring broken at the top with a stem through the gap. Drawn
    // in the same colour as the lit ring, because on the real unit it is the
    // same piece of lit plastic.
    const float markR = radius * 0.34f;
    dl->PathArcTo(centre, markR, -1.15f, 4.29f, 28);
    dl->PathStroke(running ? lit : dark, 0, std::max(2.0f, radius * 0.09f));
    dl->AddLine(ImVec2(centre.x, centre.y - markR * 1.35f),
                ImVec2(centre.x, centre.y + markR * 0.10f), running ? lit : dark,
                std::max(2.0f, radius * 0.09f));

    return pressed;
}

void ScopeView::draw(float width, float height,
                     const std::vector<cascade::core::HostTrack>& tracks,
                     BasemapCache* tiles, TrackInfoCache* info) {
    plotted_ = 0;
    askedTiles_ = false;
    if (width < 64.0f || height < 64.0f) { return; }
    // NO RECEIVER POSITION, NO SCOPE. Every mark on this face is a distance and
    // a direction from one place, so without that place there is nothing to
    // draw and nothing sensible to draw it around. The CALLER owns that empty
    // state rather than this class, because the fix for it is the receiver
    // position entry - which lives with the application's own rx fields, is
    // shared with the map, and has side effects (every map page's home moves,
    // the coverage accumulator resets) that a renderer must not reach into.
    if (!hasRx_) { return; }

    // --- layout -----------------------------------------------------------
    // The scope is a SQUARE as large as the window allows, and the panel takes
    // a share of the width beside it. The panel is dropped entirely rather
    // than squeezed when the window is too narrow to hold both: a detail panel
    // 90 px wide shows nothing, and it would be taking that width from the one
    // thing this mode exists to show.
    // THE REFERENCE DESIGN'S OWN PROPORTIONS, not a fill-the-box layout.
    //
    // In the design the upper deck gives the scope bay 530 px and the LCD 330
    // with a 14 px gap between them - the LCD is 0.377 of the pair - and the
    // tube inside that bay is 424 px across including its bezel, which is 0.84
    // of the 505 px the deck is tall. It does NOT fill its bay, and that gap
    // is most of what makes the instrument read as machined rather than as a
    // circle cut in a panel.
    //
    // The first version of this layout used 0.28 for the panel and let the
    // tube take the whole height. Every colour matched the reference and the
    // thing still looked wrong, because a component set in the wrong
    // proportion to its neighbours is a different object.
    // THE TUBE IS AS BIG AS THE DECK IS TALL, AND THE PANEL TAKES THE REST.
    //
    // Sizing the panel as a fraction of the width and then fitting a square
    // tube into what was left made the tube HEIGHT-limited on any wide window,
    // so the deck carried a band of empty case either side of the circle -
    // which is exactly the dead space this was reported as, twice. A tube is a
    // square and the deck's height is what bounds it, so the height sets its
    // size and every remaining pixel of width belongs to the LCD, which can
    // use it. Below 300 px the panel is dropped rather than squeezed: at that
    // width it shows nothing worth the space it is taking, and the tube can
    // have it instead.
    // THE TUBE GETS THE SLACK, NOT THE PANEL.
    //
    // The LCD is a fixed column in the reference - 330 px beside a 1fr scope
    // bay - and letting it absorb spare width instead just moved the dead
    // space from beside the circle to inside a panel that had nothing to put
    // there. So the panel is held near its designed width and everything else
    // belongs to the tube, which is the thing worth making bigger and the
    // thing that was asked for: expand up, left and right.
    //
    // The tube is then a square bounded by whichever of the two runs out
    // first, and it is CENTRED in what is left over on the other axis - a
    // circle pinned to one edge of its bay reads as a mistake even when the
    // size is right.
    constexpr float kPanelDesignW = 340.0f;
    const float gapPx = 14.0f;
    float panelW = std::min(kPanelDesignW, width * 0.34f);
    const bool showPanel = panelW >= 260.0f && (width - panelW - gapPx) >= 260.0f;
    if (!showPanel) { panelW = 0.0f; }
    const float gap = showPanel ? gapPx : 0.0f;
    const float scopeW = width - panelW - gap;
    const float side = std::min(scopeW, height);
    if (side < 64.0f) { return; }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##scopecanvas", ImVec2(scopeW, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    // THE WHEEL STEPS THE RANGE, and it steps it up the ladder rather than
    // scaling anything: this is a scope with a range SETTING, not a map with a
    // zoom. Wheel forward is towards the operator's own position - a shorter
    // range, a tighter picture - which is the direction every zoom control in
    // this application already moves in.
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f) { rangeNm_ = scopeRangeStepped(rangeNm_, -1); }
        if (wheel < 0.0f) { rangeNm_ = scopeRangeStepped(rangeNm_, +1); }
    }


    const float cx = origin.x + scopeW * 0.5f;
    const float cy = origin.y + height * 0.5f;
    // (The bay is taller than the tube by design; centring keeps the bezel's
    // margin equal top and bottom, which is what the reference does.)
    // Two pixels of margin so the outermost ring's stroke is not shaved by the
    // clip rectangle.
    const float radius = side * 0.5f - 2.0f;
    const ImVec2 centre(cx, cy);
    // THE POLAR ORIGIN, which is the receiver and is only at the middle of the
    // tube while the view is unpanned. Everything measured from the antenna -
    // the map, the rings, the ticks, the targets, the sweep - is placed from
    // here; everything belonging to the GLASS - the ground, the corner mask,
    // the bezel, the scan lines, the vignette and the corner readouts - stays
    // on `centre`. Keeping those two apart is what lets the view move without
    // the instrument starting to lie about distance.
    // WHERE THE ANTENNA IS ON SCREEN. At the middle of the tube until the view
    // is panned, and wherever the pan has put it after that.
    //
    // THE GRATICULE STAYS ON THE TUBE. Rings, bearing ticks, the sweep and the
    // crosshairs are all centred on `centre` and never move: they are the
    // instrument's own scale, and a scale that slides off its own face when
    // the view is dragged is unreadable at exactly the moment it is being
    // used. The first version moved the whole polar frame together, which kept
    // "200 NM from the antenna" literally true and produced a set of rings
    // mostly off the glass with a bite taken out of the picture.
    //
    // So a ring now reads as a distance from the MIDDLE OF THE VIEW, the
    // antenna gets a marker of its own that travels with the map, and the
    // range and bearing in the detail panel are still measured from the
    // antenna - which is where those two numbers have to come from.

    const ImVec2 sqTL(cx - side * 0.5f, cy - side * 0.5f);
    const ImVec2 sqBR(cx + side * 0.5f, cy + side * 0.5f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(sqTL, sqBR, true);
    dl->AddRectFilled(sqTL, sqBR, kSurround);
    dl->AddCircleFilled(centre, radius, kGround, 96);

    // --- the basemap, beneath the face -------------------------------------
    //
    // SCALE MATCHED AT THE RECEIVER, WHICH IS THE ONLY PLACE IT CAN BE MATCHED.
    // The scope's own geometry is polar - distance from the middle is
    // proportional to ground range, which is an azimuthal equidistant picture -
    // and map tiles are Web Mercator by construction. The two agree exactly at
    // the centre and diverge with distance (about 8% at the north edge of a
    // 400 NM scope in British latitudes), so the basemap under this face is a
    // LOCATING AID and the rings are what a range is read from. Reprojecting
    // the tiles into the polar frame is not an option: AddImage places an
    // axis-aligned rectangle, which is exactly what a reprojected tile is not.
    //
    // pixPerWorld is how many screen pixels one whole world-width spans, and it
    // is derived from the scope's own scale rather than chosen: pixels per
    // kilometre at the receiver, times the length of the equator, divided by
    // the meridian convergence at this latitude.
    bool haveTiles = false;
    const double mercYRx = mercY(rxLat_);
    const double cosLat = std::cos(rxLat_ * kPi / 180.0);
    const double pxPerKm =
        static_cast<double>(radius) / (static_cast<double>(rangeNm_) * kKmPerNm);
    // A receiver AT a pole has no meridian convergence to divide by, and the
    // Mercator grid has no tiles there either. The scope still works - range
    // and bearing are fine at the pole - it simply has no basemap under it.
    const double pixPerWorld = (cosLat > 1.0e-6) ? (pxPerKm * kWorldKm * cosLat) : 0.0;

    // WHERE THE MIDDLE OF THE GLASS IS, in degrees. The antenna until the view
    // is dragged, and somewhere else afterwards.
    //
    // EVERYTHING ON THE FACE IS MEASURED FROM HERE, targets included. That is
    // not a stylistic choice - it is the fix for aircraft VANISHING when the
    // view was panned and then zoomed in. The cull and the projection were
    // still antenna-relative while the rings, the map and the centre dot had
    // moved to the view, so a contact sitting in the middle of a 10 NM picture
    // could be 50 NM from the antenna, fail scopeInRange against 10, and never
    // be drawn. The range and bearing PRINTED in the detail panel stay
    // antenna-relative, because those two numbers are only meaningful from the
    // aerial - so the face answers "where is it on this picture" and the panel
    // answers "how far is it from me", and neither has to lie to do it.
    // THE VIEW IS A PLACE, and the input below moves that place rather than a
    // pixel offset - which is what makes a range change keep the same ground
    // under the middle of the glass instead of walking off it.
    if (!hasView_) {
        viewLat_ = rxLat_;
        viewLon_ = rxLon_;
    }

    // DRAG MOVES THE VIEW. Tracked on the item rather than on the mouse alone,
    // so a drag that began somewhere else - on the range stepper, on a window
    // edge - cannot pan the face as it passes over it. The pixel delta is
    // turned into degrees HERE, against this frame's scale, and only the
    // result is kept.
    if (pixPerWorld > 0.0 && ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        const ScopeLatLon moved = scopeViewCentre(viewLat_, viewLon_,
                                                  static_cast<double>(d.x),
                                                  static_cast<double>(d.y), pixPerWorld);
        viewLat_ = moved.latDeg;
        viewLon_ = moved.lonDeg;
        hasView_ = true;
        dragged_ = true;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { dragged_ = false; }

    // DOUBLE-CLICK PUTS THE ANTENNA BACK IN THE MIDDLE. A view that can be
    // moved needs a way home that is not dragging it back by eye, and the
    // caller carries a button for the same reason the knob has one beside it:
    // a gesture is not reachable without a mouse.
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { resetPan(); }
    if (!hasView_) {
        viewLat_ = rxLat_;
        viewLon_ = rxLon_;
    }

    const double viewLat = viewLat_;
    const double viewLon = viewLon_;
    // The antenna's own place on the glass, which is the middle until the view
    // has been moved off it.
    float rcx = cx;
    float rcy = cy;
    if (pixPerWorld > 0.0) {
        rcx = cx + static_cast<float>(((rxLon_ - viewLon) / 360.0) * pixPerWorld);
        rcy = cy + static_cast<float>((mercY(rxLat_) - mercY(viewLat)) * pixPerWorld);
    }
    const ImVec2 rxCentre(rcx, rcy);

    if (tiles != nullptr && tiles->active() && pixPerWorld > 0.0) {
        askedTiles_ = true;
        const double want =
            std::log2(pixPerWorld / static_cast<double>(tiles->tileSize()));
        int z = static_cast<int>(std::floor(want + 0.5));
        z = std::clamp(z, static_cast<int>(tiles->minZoom()),
                       static_cast<int>(tiles->maxZoom()));
        const double n = std::pow(2.0, static_cast<double>(z));

        // TILE EDGES COME FROM A CONTINUOUS LONGITUDE AND ARE NEVER WRAPPED,
        // and this is not tidiness - it is the fix the map needed after it
        // rendered in MIRROR WRITING. Wrapping a longitude into +/-180 is
        // correct for a POINT and wrong for a RECTANGLE, because the two edges
        // wrap independently: a tile running from +178 to +182 keeps its west
        // edge at +178 and folds its east edge to -178, AddImage is handed
        // p_min.x > p_max.x, and it draws the texture back to front. The loop
        // below therefore works entirely in the unwrapped longitude the tile
        // index implies, and only the tile INDEX is wrapped.
        const auto tileX = [&](double lon) {
            return static_cast<float>(rcx + (lon - rxLon_) / 360.0 * pixPerWorld);
        };
        const auto tileY = [&](double lat) {
            return static_cast<float>(rcy + (mercY(lat) - mercYRx) * pixPerWorld);
        };

        // The visible square in normalised Mercator, from its own corners.
        //
        // MEASURED ABOUT THE MIDDLE OF THE TUBE, not about the antenna. The
        // antenna is only at the middle while the view is unpanned; taking the
        // square from it meant that after a drag the tiles fetched were the
        // ones around the RECEIVER while the glass was showing somewhere else,
        // so the map simply ran out - most visibly after zooming in, where the
        // covered area is smallest. The pan is subtracted here, in world
        // units, which is the same offset the draw uses.
        const double halfWorldX = static_cast<double>(side) * 0.5 / pixPerWorld;
        const double xc = (viewLon + 180.0) / 360.0;
        const double x0 = xc - halfWorldX;
        const double x1 = xc + halfWorldX;
        // Same correction on the vertical: the window follows the view, not
        // the antenna.
        const double ycView = mercY(viewLat);
        const double y0 = ycView - static_cast<double>(side) * 0.5 / pixPerWorld;
        const double y1 = ycView + static_cast<double>(side) * 0.5 / pixPerWorld;

        long tx0 = static_cast<long>(std::floor(x0 * n));
        long tx1 = static_cast<long>(std::floor(x1 * n));
        long ty0 = static_cast<long>(std::floor(y0 * n));
        long ty1 = static_cast<long>(std::floor(y1 * n));
        ty0 = std::max(ty0, 0L);
        ty1 = std::min(ty1, static_cast<long>(n) - 1);

        // A hard cap on tiles per frame. The zoom choice already keeps this
        // near the square's area divided by the tile size, but a degenerate
        // view must not be able to ask for thousands.
        const long maxSpan = 64;
        if (tx1 - tx0 > maxSpan) { tx1 = tx0 + maxSpan; }
        if (ty1 - ty0 > maxSpan) { ty1 = ty0 + maxSpan; }

        for (long ty = ty0; ty <= ty1; ++ty) {
            for (long txRaw = tx0; txRaw <= tx1; ++txRaw) {
                // Wrap the INDEX in x so a receiver near the antimeridian still
                // gets tiles; y does not wrap, because the world ends at the
                // poles.
                long tx = txRaw % static_cast<long>(n);
                if (tx < 0) { tx += static_cast<long>(n); }
                const unsigned int tex = tiles->texture(static_cast<std::uint32_t>(z),
                                                        static_cast<std::uint32_t>(tx),
                                                        static_cast<std::uint32_t>(ty));
                if (tex == 0u) { continue; }
                // Placed from the tile's OWN edges rather than from a computed
                // size, so rounding cannot leave hairline gaps between
                // neighbours. a is the top-left and b the bottom-right by
                // construction (east > west, north > south), which is what
                // AddImage's implicit texture coordinates require.
                const double west = static_cast<double>(txRaw) / n * 360.0 - 180.0;
                const double east = static_cast<double>(txRaw + 1) / n * 360.0 - 180.0;
                const double north = mercLat(static_cast<double>(ty) / n);
                const double south = mercLat(static_cast<double>(ty + 1) / n);
                dl->AddImage(static_cast<ImTextureID>(static_cast<std::uintptr_t>(tex)),
                             ImVec2(tileX(west), tileY(north)),
                             ImVec2(tileX(east), tileY(south)),
                             ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                             optPhosphor_ ? kTileTint : IM_COL32(255, 255, 255, 217));
                haveTiles = true;
            }
        }
    }

    // --- the compiled-in coastline, when there are no tiles ----------------
    //
    // WHICH IS THE SHIPPED CONFIGURATION: a basemap plugin is something a user
    // installs and points at their own tile server, so most scopes will never
    // see one. Natural Earth 1:110m, public domain, 20 KB of int16 hundredths
    // of a degree compiled into the binary (see coastline_data.hpp) - so a
    // scope with no network and no plugin still has a coast to place an
    // aircraft against, which is most of what makes it readable.
    //
    // Skipped once tiles are on screen, for the same reason the map skips it:
    // a rendered map already draws its own coastlines, and a 1:110m outline
    // over street-level imagery is a wrong-coloured line a few hundred metres
    // off the real one.
    if (!haveTiles && pixPerWorld > 0.0) {
        const double halfLon = static_cast<double>(side) * 0.5 / pixPerWorld * 360.0;
        const double lonCentre = viewLon;
        const double west = lonCentre - halfLon;
        const double east = lonCentre + halfLon;
        const double ycCoast = mercY(viewLat);
        const double northLat = mercLat(ycCoast - static_cast<double>(side) * 0.5 / pixPerWorld);
        const double southLat = mercLat(ycCoast + static_cast<double>(side) * 0.5 / pixPerWorld);
        const auto project = [&](double lat, double lon) {
            return ImVec2(
                static_cast<float>(rcx + (lon - rxLon_) / 360.0 * pixPerWorld),
                static_cast<float>(rcy + (mercY(lat) - mercYRx) * pixPerWorld));
        };
        for (std::uint32_t r = 0; r < coastline::kRunCount; ++r) {
            const coastline::Run& run = coastline::kRuns[r];
            ImVec2 prev(0.0f, 0.0f);
            bool havePrev = false;
            double prevLon = 0.0;
            double prevLat = 0.0;
            for (std::uint32_t i = 0; i < run.count; ++i) {
                const std::size_t k = (static_cast<std::size_t>(run.first) + i) * 2u;
                const double lon = static_cast<double>(coastline::kCoords[k]) / 100.0;
                const double lat = static_cast<double>(coastline::kCoords[k + 1]) / 100.0;
                if (havePrev) {
                    // A segment that jumps more than half the world is a wrap,
                    // not a coast; drawing it streaks a line across the face.
                    const bool wrap = std::fabs(lon - prevLon) > 180.0;
                    // Cheap reject of everything outside the square. With 5127
                    // points this is not about frame rate so much as about not
                    // asking ImGui to clip thousands of lines nobody can see -
                    // and on a scope almost the whole world is outside.
                    const bool visible =
                        !(std::max(lon, prevLon) < west || std::min(lon, prevLon) > east ||
                          std::max(lat, prevLat) < southLat ||
                          std::min(lat, prevLat) > northLat);
                    if (!wrap && visible) { dl->AddLine(prev, project(lat, lon), kCoast, 1.0f); }
                }
                prev = project(lat, lon);
                prevLon = lon;
                prevLat = lat;
                havePrev = true;
            }
        }
    }

    // --- the tube's optics, over the map and under everything read from it ---
    //
    // ORDER IS THE DESIGN'S OWN: the wash sits on the imagery so the map reads
    // as something seen THROUGH the phosphor rather than beside it; the
    // crosshairs and the sweep sit on the wash because they are the tube's
    // marks, not the map's.
    // THE GREEN IS A SWITCH. With the phosphor off the tiles are drawn in
    // their own colours and no wash goes over them, which is the readable
    // choice when the map itself is what is being used - street names on a
    // green field are a lot harder than street names on a map.
    if (haveTiles && optPhosphor_) {
        dl->AddCircleFilled(centre, radius, kPhosphorWash, 96);
    }
    dl->AddLine(ImVec2(cx, cy - radius), ImVec2(cx, cy + radius), kCrossHair, 1.0f);
    dl->AddLine(ImVec2(cx - radius, cy), ImVec2(cx + radius, cy), kCrossHair, 1.0f);

    // THE SWEEP, driven by the frame clock rather than a counter, so it turns
    // at the same rate whatever the frame rate is - and stops turning if the
    // application does, which is the honest signal that drawing has stalled.
    if (optSweep_) {
        addSweepFan(dl, centre, radius,
                    std::fmod(ImGui::GetTime() / 4.2, 1.0) * 360.0);
    }

    // --- the circular clip --------------------------------------------------
    //
    // ImGui clips to RECTANGLES, so the round face is made by painting the
    // corners back out: a ring of quads from the rim to past the square's
    // corners, in the surround colour. Ninety-six segments, whose inner edge is
    // a chord about a fifth of a pixel inside the true circle at any radius
    // this layout produces - and the rim stroke drawn immediately below covers
    // that seam exactly.
    // --- targets ------------------------------------------------------------
    //
    // AIRCRAFT ONLY. This is the ADS-B scope and every mark on it is a range
    // and a bearing from the antenna; a satellite five hundred kilometres up or
    // a ship on the other side of an ocean would be a mark that meant something
    // different from the ones beside it. The staleness rule is the host's
    // single one (see core/plugin_ui.hpp), so a plugin that never evicts cannot
    // leave an hour-old aircraft sitting on the face at full confidence.
    // THE TRAILS, UNDER THE TARGETS. Drawn first so a marker is never hidden
    // by the line leading to it, and coloured by the altitude the aircraft
    // actually HAD along each leg - the same rule and the same bands the map
    // uses, so a climb reads as a change of colour on both.
    if (optTrail_ != 0) {
        for (const cascade::core::HostTrack& ht : tracks) {
            if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
            const auto it = trails_.find(std::string(ht.t.id));
            if (it == trails_.end() || it->second.size() < 2) { continue; }
            const std::vector<TrailPoint>& tr = it->second;
            const bool picked = !selectedId_.empty() && selectedId_ == ht.t.id;
            for (std::size_t j = 1; j < tr.size(); ++j) {
                const ScopePolar pa =
                    scopeRelative(viewLat, viewLon, tr[j - 1].lat, tr[j - 1].lon);
                const ScopePolar pb = scopeRelative(viewLat, viewLon, tr[j].lat, tr[j].lon);
                // A leg with either end outside the range is dropped rather
                // than clipped to the rim, for the same reason a target is.
                if (!scopeInRange(pa.rangeNm, static_cast<double>(rangeNm_)) ||
                    !scopeInRange(pb.rangeNm, static_cast<double>(rangeNm_))) {
                    continue;
                }
                const ScopePoint qa = scopeProject(cx, cy, radius, pa.rangeNm,
                                                   pa.bearingDeg,
                                                   static_cast<double>(rangeNm_));
                const ScopePoint qb = scopeProject(cx, cy, radius, pb.rangeNm,
                                                   pb.bearingDeg,
                                                   static_cast<double>(rangeNm_));
                // The MEAN of the two ends, so a climbing leg changes colour
                // half way up rather than stepping at one end.
                const double midAlt =
                    (std::isfinite(tr[j - 1].altM) && std::isfinite(tr[j].altM))
                        ? 0.5 * (tr[j - 1].altM + tr[j].altM)
                        : (std::isfinite(tr[j].altM) ? tr[j].altM : tr[j - 1].altM);
                const int band = altitudeBandIndex(midAlt);
                const AltBandStyle& bs = altBandStyle(band < 0 ? 0 : band);
                // Older legs fade, so which end is now needs no arrowhead.
                const float t = static_cast<float>(j) / static_cast<float>(tr.size());
                const int alpha = static_cast<int>((60.0f + 150.0f * t) *
                                                  (picked ? 1.0f : 0.75f));
                const ImU32 col = (band < 0)
                                      ? IM_COL32(134, 214, 74, alpha)
                                      : IM_COL32(bs.r, bs.g, bs.b, alpha);
                const ImVec2 A(static_cast<float>(qa.x), static_cast<float>(qa.y));
                const ImVec2 B(static_cast<float>(qb.x), static_cast<float>(qb.y));
                // RIBBON is the same path drawn wide. It is not a different
                // reading - it is the same one made legible over a busy map,
                // which is why it shares every colour rule with the line.
                dl->AddLine(A, B, col,
                            optTrail_ == 2 ? (picked ? 6.0f : 4.5f)
                                           : (picked ? 2.2f : 1.4f));
            }
        }
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float bestDist = 14.0f;  // hit radius in pixels
    const cascade::core::HostTrack* hit = nullptr;
    const cascade::core::HostTrack* selected = nullptr;

    for (const cascade::core::HostTrack& ht : tracks) {
        if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
        const cascade::core::TrackPresentation pres =
            cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind);
        if (!pres.visible) { continue; }
        // FROM THE MIDDLE OF THE VIEW, which is where every ring on this face
        // is measured from. See the note where viewLat/viewLon are computed:
        // culling against the antenna while drawing rings about the view is
        // what made aircraft disappear as soon as a panned view was zoomed in.
        const ScopePolar pol = scopeRelative(viewLat, viewLon, ht.t.latDeg, ht.t.lonDeg);
        // OUTSIDE THE RANGE IS NOT DRAWN AT ALL - not clamped to the rim, not
        // dimmed at the edge. A mark on the rim would say "this aircraft is at
        // two hundred miles" about one that is at four hundred, which is a
        // worse answer than saying nothing.
        if (!scopeInRange(pol.rangeNm, static_cast<double>(rangeNm_))) { continue; }
        // THE FILTER, and it hides rather than dims: a display that is showing
        // a subset must show that subset cleanly, or the filter has not done
        // the thing it was turned on for. What is being hidden is stated on the
        // SYS screen and the count in the corner falls with it, so a filtered
        // face cannot be mistaken for a quiet sky.
        if (optFilter_ == 1 &&
            (ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) == 0u) { continue; }
        if (optFilter_ == 2 && ht.t.label[0] == '\0') { continue; }
        ++plotted_;

        // WHERE IT HAS BEEN, recorded whether or not the trail is being drawn:
        // switching trails on should show where the traffic has been since the
        // scope was opened, not start every trail from that moment.
        {
            std::vector<TrailPoint>& tr = trails_[std::string(ht.t.id)];
            bool add = tr.empty();
            if (!add) {
                const TrailPoint& last = tr.back();
                // The same fifty-metre gate the map's altitude store uses: a
                // stationary contact must not fill its trail with copies of
                // one point.
                const double dLat = (ht.t.latDeg - last.lat) * 111320.0;
                const double dLon = (ht.t.lonDeg - last.lon) * 111320.0 *
                                    std::cos(ht.t.latDeg * kPi / 180.0);
                add = (dLat * dLat + dLon * dLon) > (50.0 * 50.0);
            }
            if (add) {
                tr.push_back(TrailPoint{ht.t.latDeg, ht.t.lonDeg, ht.t.altM});
                if (tr.size() > kTrailMaxPoints) { tr.erase(tr.begin()); }
            }
        }

        const ScopePoint p = scopeProject(cx, cy, radius, pol.rangeNm, pol.bearingDeg,
                                          static_cast<double>(rangeNm_));
        const ImVec2 s(static_cast<float>(p.x), static_cast<float>(p.y));
        const ImU32 col = fadedColour(targetColour(ht.t), pres.alpha);
        const bool picked = !selectedId_.empty() && selectedId_ == ht.t.id;
        if (picked) { selected = &ht; }
        const bool altKnown = altitudeBandIndex(ht.t.altM) >= 0;

        // The emergency ring, so the state survives being selected (where the
        // silhouette is knocked out of a disc and the emergency hue is no
        // longer the fill) and survives a colour-blind reading.
        if ((ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u) {
            dl->AddCircle(s, 16.0f, fadedColour(kAlert, pres.alpha), 0, 2.0f);
        }
        if (picked) {
            dl->AddCircleFilled(s, 13.65f, col);
            dl->AddCircle(s, 13.65f, IM_COL32(0, 0, 0, (col >> IM_COL32_A_SHIFT) & 0xFFu),
                          0, 1.5f);
            addPlane(dl, s, ht.t.courseDeg, 8.4f, kGround);
        } else {
            addPlane(dl, s, ht.t.courseDeg, 9.45f, col, altKnown);
        }

        const char* lbl = ht.t.label[0] != '\0' ? ht.t.label : ht.t.id;
        dl->AddText(ImVec2(s.x + (picked ? 15.0f : 8.0f), s.y - 6.0f), col, lbl);

        if (hovered) {
            const float dx = mouse.x - s.x;
            const float dy = mouse.y - s.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < bestDist) {
                bestDist = d;
                hit = &ht;
            }
        }
    }

    // A CLICK ON EMPTY FACE CLEARS THE SELECTION, which is the other half of
    // clicking an aircraft to select one: without it the only way to stop
    // watching a target would be to find another one to watch.
    if (clicked) {
        if (hit != nullptr) {
            selectedId_ = hit->t.id;
            selected = hit;
        } else {
            selectedId_.clear();
            selected = nullptr;
        }
    }

    // TRAILS FOR AIRCRAFT THAT ARE NO LONGER REPORTED ARE DROPPED, and the
    // store is capped, so a long session cannot grow it without bound. Done
    // after the draw so a trail survives the frame its aircraft went quiet in.
    if (trails_.size() > kTrailMaxTracks) {
        std::set<std::string> live;
        for (const cascade::core::HostTrack& ht : tracks) {
            live.insert(std::string(ht.t.id));
        }
        for (auto it = trails_.begin(); it != trails_.end();) {
            it = (live.count(it->first) == 0u) ? trails_.erase(it) : std::next(it);
        }
    }

    // CLICK PUTS THAT POINT IN THE MIDDLE, but only a click that selected
    // nothing and moved nothing: a click on an aircraft is a selection, and the
    // release at the end of a drag is the end of a pan. Both would otherwise
    // jump the view out from under the hand that just finished using it.
    if (clicked && !dragged_ && hit == nullptr && pixPerWorld > 0.0) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        // The point under the cursor becomes the point under the dot. Computed
        // as a PLACE, so a later range change keeps it there.
        const ScopeLatLon at = scopeViewCentre(viewLat_, viewLon_,
                                               static_cast<double>(cx - m.x),
                                               static_cast<double>(cy - m.y), pixPerWorld);
        viewLat_ = at.latDeg;
        viewLon_ = at.lonDeg;
        hasView_ = true;
    }

    // --- the receiver, at the middle ----------------------------------------
    // A cross rather than a dot: the centre of a scope is the one place a mark
    // has to be locatable to the pixel, and a filled disc hides the very point
    // every range on the face is measured from.
    // A MAGENTA DOT, and magenta on purpose: it is the one hue on this face
    // that means nothing else - the targets walk an altitude ramp from orange
    // to indigo and the chrome is all phosphor green, so the antenna's own
    // position can never be mistaken for a contact or for a graticule mark.
    // The glow is three grown circles rather than a blur, which ImGui has not.
    // THE MIDDLE OF THE VIEW, always at the middle of the tube. This is the
    // point every ring is a distance from, so it is the one mark that must be
    // locatable to the pixel wherever the view has been dragged to.
    dl->AddCircleFilled(centre, 7.0f, IM_COL32(224, 95, 208, 40), 16);
    dl->AddCircleFilled(centre, 5.0f, IM_COL32(224, 95, 208, 90), 16);
    dl->AddCircleFilled(centre, 3.5f, kHomeDot, 16);

    // AND THE ANTENNA, WHEREVER THE MAP HAS CARRIED IT. Drawn only once the
    // view has actually been moved: while the two coincide a second mark on
    // the same pixel is noise, and drawing it anyway would suggest the scope
    // had two origins when it has one.
    if (panned()) {
        // A cross rather than a disc, and in the phosphor rather than the
        // magenta: it is a different KIND of thing from the view centre, and
        // the shape says so without relying on the colour.
        dl->AddLine(ImVec2(rcx - 8.0f, rcy), ImVec2(rcx + 8.0f, rcy), kPhosphor, 1.6f);
        dl->AddLine(ImVec2(rcx, rcy - 8.0f), ImVec2(rcx, rcy + 8.0f), kPhosphor, 1.6f);
        dl->AddCircle(rxCentre, 5.0f, kPhosphor, 16, 1.4f);
        const ImVec2 sz = ImGui::CalcTextSize("RX");
        dl->AddText(ImVec2(rcx - sz.x * 0.5f, rcy + 9.0f), kPhosphor, "RX");
    }

    // TARGETS BEFORE THE MASK, and that ordering IS the circular clip. ImGui
    // clips to rectangles only, so the corner mask below - which paints the
    // surround over everything outside the radius - is the only thing that
    // makes this face round. Drawn after it, a marker, its label or an
    // emergency ring near the rim spilled onto the bezel and the scope stopped
    // being a circle at exactly the edge a scope is read from. Underneath it,
    // the overhang is trimmed for free. The rings and readouts stay above,
    // where chrome belongs.
    {
        constexpr int kMaskSegments = 96;
        const float outer = side;  // past every corner, and the clip rect trims it
        ImVec2 prevIn(0.0f, 0.0f);
        ImVec2 prevOut(0.0f, 0.0f);
        for (int i = 0; i <= kMaskSegments; ++i) {
            const double a = 2.0 * kPi * static_cast<double>(i) /
                             static_cast<double>(kMaskSegments);
            const float sx = static_cast<float>(std::sin(a));
            const float sy = static_cast<float>(-std::cos(a));
            const ImVec2 in(cx + sx * radius, cy + sy * radius);
            const ImVec2 out(cx + sx * outer, cy + sy * outer);
            if (i > 0) {
                const ImVec2 quad[4] = {prevIn, in, out, prevOut};
                dl->AddConvexPolyFilled(quad, 4, kSurround);
            }
            prevIn = in;
            prevOut = out;
        }
    }

    // --- the bezel -----------------------------------------------------------
    // Two rings outside the face, as the design draws them: 10 px of the dark
    // case colour and 2 px of a lighter machined edge. They are strokes rather
    // than filled discs because the corner mask has already painted everything
    // beyond the radius, so all that is wanted here is the lip.
    dl->AddCircle(centre, radius + 5.0f, kBezelIn, 96, 10.0f);
    dl->AddCircle(centre, radius + 11.0f, kBezelOut, 96, 2.0f);

    // --- corner readouts ----------------------------------------------------
    // AFTER THE MASK, not before it. The mask paints the surround colour over
    // everything outside the radius, which is precisely the four corners these
    // sit in - so drawn earlier they were painted out every frame, and the
    // scope had no track count and no range on its face at all.
    //
    // COUNTED THE WAY THEY ARE DRAWN. plotted_ is incremented in the target
    // loop, so the number in the corner is exactly the number of silhouettes
    // on the face - not what the plugin reported, which includes stale targets
    // and everything beyond the range.
    {
        const std::string tracksText = scopeTracksReadout(plotted_);
        const std::string rangeText = scopeRangeReadout(rangeNm_);
        dl->AddText(ImVec2(sqTL.x + 8.0f, sqTL.y + 6.0f), kChrome, tracksText.c_str());
        const ImVec2 rangeSize = ImGui::CalcTextSize(rangeText.c_str());
        dl->AddText(ImVec2(sqBR.x - 8.0f - rangeSize.x, sqTL.y + 6.0f), kChrome,
                    rangeText.c_str());
        // MODE and the receiver's own position, on the bottom corners, which is
        // where the design puts them and where they stay out of the way of the
        // range ladder along the top.
        char pos[48];
        std::snprintf(pos, sizeof(pos), "%.1f%c %05.1f%c", std::fabs(rxLat_),
                      rxLat_ >= 0.0 ? 'N' : 'S', std::fabs(rxLon_),
                      rxLon_ >= 0.0 ? 'E' : 'W');
        const ImVec2 posSize = ImGui::CalcTextSize(pos);
        const float bottomY = sqBR.y - 6.0f - ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(sqTL.x + 8.0f, bottomY), kChromeDim, "MODE ADS-B");
        dl->AddText(ImVec2(sqBR.x - 8.0f - posSize.x, bottomY), kChromeDim, pos);
    }

    // --- the glass, over everything the tube shows ---------------------------
    // Last inside the face, because a scan line that ran under a contact would
    // be a scan line on the map rather than on the tube, and the whole point of
    // both of these is that they belong to the SCREEN.
    addScanLines(dl, centre, radius);
    addVignette(dl, centre, radius);

    // --- range rings --------------------------------------------------------
    for (int ring = 1; ring <= kScopeRingCount; ++ring) {
        const float r = radius * static_cast<float>(ring) /
                        static_cast<float>(kScopeRingCount);
        const bool outermost = (ring == kScopeRingCount);
        dl->AddCircle(centre, r, outermost ? kRim : kRing, 96, outermost ? 1.8f : 1.0f);
        // LABELLED AT ITS EDGE, on the north radial and just inside the ring,
        // which is where the eye already is when it is reading a radius. Offset
        // a few pixels right of the radial so the text does not sit on the
        // bearing tick above it.
        const std::string lbl = scopeRingLabel(rangeNm_, ring);
        // INSIDE THE RING, NOT ABOVE IT, for the outermost one. The outer ring
        // sits within two pixels of the clip rect, so a label placed above it
        // was cut to a sliver of its own ascenders - and that ring is the
        // scope's full-scale reference, the one number the face exists to
        // state. The inner rings keep the outside placement, where there is
        // room and the eye is already travelling.
        const float labelY = outermost
                                 ? cy - r + 3.0f
                                 : cy - r - ImGui::GetTextLineHeight() - 1.0f;
        dl->AddText(ImVec2(cx + 4.0f, labelY),
                    outermost ? kChrome : kChromeDim, lbl.c_str());
    }

    // --- bearing ticks ------------------------------------------------------
    //
    // Every thirty degrees, with the four cardinals spelled out. THE NUMBERED
    // TICKS ARE DROPPED ON A SMALL FACE and the cardinals kept: twelve labels
    // round a 130 px circle collide with each other and with the ring labels,
    // and a scope whose furniture is unreadable is worse than one with less of
    // it. The ticks themselves are always drawn, so the thirty-degree grid
    // survives at every size.
    const bool numberTicks = radius >= 150.0f;
    for (int b = 0; b < 360; b += 30) {
        const double a = static_cast<double>(b) * kPi / 180.0;
        const float sx = static_cast<float>(std::sin(a));
        const float sy = static_cast<float>(-std::cos(a));
        const bool cardinal = (b % 90) == 0;
        const float inner = radius * (cardinal ? 0.955f : 0.975f);
        dl->AddLine(ImVec2(cx + sx * inner, cy + sy * inner),
                    ImVec2(cx + sx * radius, cy + sy * radius), kTick,
                    cardinal ? 1.8f : 1.0f);
        if (!cardinal && !numberTicks) { continue; }
        const std::string lbl = scopeBearingLabel(b);
        const ImVec2 sz = ImGui::CalcTextSize(lbl.c_str());
        // Centred on its own tick at a fixed inset, so the twelve labels sit on
        // one circle rather than drifting with the length of the text.
        const float lr = radius * 0.90f;
        dl->AddText(ImVec2(cx + sx * lr - sz.x * 0.5f, cy + sy * lr - sz.y * 0.5f),
                    cardinal ? kChrome : kChromeDim, lbl.c_str());
    }

    dl->PopClipRect();

    // --- the detail panel ----------------------------------------------------
    if (showPanel) {
        ImGui::SameLine(0.0f, gap);
        // The counts the HOME screen reports are the ones the FACE holds, not
        // what the plugin claims: plotted_ is incremented as each silhouette is
        // drawn, so the panel and the picture cannot disagree.
        int tracked = 0;
        double topAltM = 0.0;
        bool haveTopAlt = false;
        for (const cascade::core::HostTrack& ht : tracks) {
            if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
            ++tracked;
            if (std::isfinite(ht.t.altM) && (!haveTopAlt || ht.t.altM > topAltM)) {
                topAltM = ht.t.altM;
                haveTopAlt = true;
            }
        }
        ScopeOptions opts{optPhosphor_, optSweep_, optFilter_, optTrail_};
        drawScopePanel(panelW, height, selected, selectedId_, info, hasRx_, rxLat_,
                       rxLon_, lcdScreen_, plotted_, tracked, topAltM, haveTopAlt,
                       rangeNm_, opts, tracks, selectedId_);
        optPhosphor_ = opts.phosphor;
        optSweep_ = opts.sweep;
        optFilter_ = opts.filter;
        optTrail_ = opts.trail;
    }
}

}  // namespace cascade::gui
