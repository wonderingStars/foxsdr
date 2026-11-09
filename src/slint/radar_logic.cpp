// radar_logic.cpp - the radar scope's rules (see radar_logic.hpp). A port of gui/scope_view.cpp's ScopeView::draw (2380-3000: the wheel, drag, click and double-click rules and the dragged_ guard, the
// target loop, the trails, the corner readouts, the rings and bearing ticks), of its LCD panel (361-845: HOME, SYS, FLIGHT) and of app_window.cpp's drawScopeMode (13564-14212: the gauges, the
// drums, the GAIN knob, POWER) over the classic's own ImGui-free arithmetic (gui/scope_view.hpp, gui/scope_gain.hpp, gui/track_metrics.hpp). Nothing here draws.
//
// TWO PIECES OF THE CLASSIC ARE RESTATED, not called, because the unit that defines them cannot be linked into this target's logic test:
//   * aircraftMarkerRadius (gui/aircraft_icons.cpp, which draws with GL): max(14, 0.6 * icon px) - radarMarkerRadius below; test_slint_host_radar reads the classic's line.
//   * trackPresentation, the AIRCRAFT branch (core/plugin_ui.cpp, which links the plugin runtime): fresh to 30 s, fading to kTrackMinAlpha, gone at 60 s (180 s for an ESTIMATED fix) - aircraftPresentation
//     below, over the same constants from core/plugin_ui_data.hpp.
// There is no font here: text widths are estimated at 6 px a character and a line at 14 px (the view places the words; these numbers only decide which labels would collide).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radar_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "core/i18n.hpp"

namespace foxsdr::slintscope {

namespace gui = cascade::gui;
using cascade::i18n::tr;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kTrailMaxPoints = 256;     // scope_view.hpp kTrailMaxPoints
constexpr std::size_t kTrailMaxTracks = 256;     // scope_view.hpp kTrailMaxTracks
constexpr double kTrailMinMetres = 50.0;         // scope_view.cpp 2862
constexpr float kCharPx = 6.0f, kLineH = 14.0f;  // the text estimate (no font here)
constexpr float kMinTubePx = 40.0f;              // under this the tube draws nothing
constexpr int kGaugeSegments = 18;
constexpr double kAltFullScaleM = 13000.0;       // aw.cpp 13919: a little above the ceiling of everything with a transponder
constexpr double kMetresToFeet = 3.28084;        // the classic's literal in FL%03d

// gui/aircraft_icons.cpp:269 `float aircraftMarkerRadius(float sizePx) { return std::max(14.0f, sizePx * 0.6f); }`
float radarMarkerRadius(float sizePx) { return std::max(14.0f, sizePx * 0.6f); }

struct Presentation {
    bool visible = true;
    float alpha = 1.0f;
};
// core/plugin_ui.cpp trackPresentation(ageMs, CASCADE_TRACK_AIRCRAFT, flags)
Presentation aircraftPresentation(const CascadeTrack& t) {
    const std::uint64_t fadeMs = cascade::core::kTrackFadeMsAircraft;
    const std::uint64_t dropMs = (t.flags & CASCADE_TRACK_FLAG_ESTIMATED) != 0u ? cascade::core::kTrackDropMsAircraftEstimated : cascade::core::kTrackDropMsAircraft;
    Presentation p;
    if (t.ageMs >= dropMs) { p.visible = false; p.alpha = 0.0f; return p; }
    if (t.ageMs < fadeMs) { return p; }
    const double u = static_cast<double>(t.ageMs - fadeMs) / static_cast<double>(dropMs - fadeMs);
    p.alpha = static_cast<float>(1.0 - u * (1.0 - static_cast<double>(cascade::core::kTrackMinAlpha)));
    return p;
}

std::uint32_t packRgba(int r, int g, int b, int a) {
    const auto c = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
    return (c(r) << 24) | (c(g) << 16) | (c(b) << 8) | c(a);
}
int alphaOf(float alpha) { return static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f); }

// scope_view.cpp targetColour + fadedColour: an emergency is the alert red, an unknown altitude a neutral grey, else the altitude band; faded by the age.
std::uint32_t targetColour(const CascadeTrack& t, float alpha) {
    if ((t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u) { return packRgba(255, 45, 45, alphaOf(alpha)); }
    const int band = gui::altitudeBandIndex(t.altM);
    if (band < 0) { return packRgba(196, 204, 212, alphaOf(alpha)); }
    const gui::AltBandStyle& s = gui::altBandStyle(band);
    return packRgba(s.r, s.g, s.b, alphaOf(alpha));
}

std::string idOf(const CascadeTrack& t) { return std::string(t.id, ::strnlen(t.id, sizeof(t.id))); }
std::string labelOf(const CascadeTrack& t) { return std::string(t.label, ::strnlen(t.label, sizeof(t.label))); }
std::string nameOf(const CascadeTrack& t) { const std::string l = labelOf(t); return l.empty() ? idOf(t) : l; }

struct Geom {
    bool ok = false;
    float cx = 0.f, cy = 0.f, radius = 0.f;
};
// scope_view.cpp 2397-2431: the tube is a square bounded by the shorter side, centred, with two pixels of margin so the outer ring is not shaved.
Geom geomOf(float w, float h) {
    Geom g;
    const float side = std::min(w, h);
    g.ok = side >= kMinTubePx;
    g.cx = w * 0.5f;
    g.cy = h * 0.5f;
    g.radius = side * 0.5f - 2.0f;
    return g;
}

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, std::size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h = (h ^ b[i]) * 1099511628211ull; }
    }
    template <class T> void pod(const T& v) { bytes(&v, sizeof(v)); }
    void str(const std::string& s) { bytes(s.data(), s.size()); pod(static_cast<unsigned char>(0)); }
};

// "FL%03d" of the altitude in hundreds of feet (the classic truncates)
std::string flightLevel(double altM) { return gui::detail::scopePrintf("FL%03d", static_cast<int>(altM * kMetresToFeet / 100.0)); }

std::string zeroPadded(int value, int digits) {
    int cap = 1;
    for (int i = 0; i < digits; ++i) { cap *= 10; }
    const int v = std::clamp(value, 0, cap - 1);   // all-nines rather than the low digits (scope_view.cpp 1024-1029)
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*d", digits, v);
    return buf;
}

struct KnobState {
    bool haveGain = false, live = false;
    float lo = 0.f, hi = 60.f, db = 0.f;
};
// aw.cpp 14079-14111. `clearStale` drops a kept figure that no longer stands (the classic does it every frame the cabinet draws).
KnobState knobOf(const RadarInputs& in, gui::ScopeGainLocal& local, bool clearStale) {
    KnobState k;
    k.haveGain = in.deviceOpen && in.haveGain;
    k.live = k.haveGain && !in.deviceAgc;
    const gui::ScopeKnobTravel t = gui::scopeKnobTravel(k.haveGain ? in.gainLoDb : 0.0f, k.haveGain ? in.gainHiDb : 0.0f, 0.0f, 60.0f);   // kSoapyGainMinDb / kSoapyGainMaxDb: the fallback 0..60
    k.lo = t.loDb;
    k.hi = t.hiDb;
    if (k.haveGain) { k.db = gui::scopeGainShown(local, in.gainStage, in.gainDb, in.nowS); }
    if (clearStale && local.valid && (!k.haveGain || !gui::scopeGainLocalStands(local, in.gainStage, in.gainDb, in.nowS))) { local.valid = false; }
    return k;
}

}  // namespace

float radarSweepDegrees(double nowS) {
    if (!std::isfinite(nowS) || nowS < 0.0) { return 0.0f; }
    return static_cast<float>(std::fmod(nowS / 4.2, 1.0) * 360.0);   // scope_view.cpp 2741
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the settings and the gestures
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RadarScope::stepRange(int steps) {
    const int next = gui::scopeRangeStepped(rangeNm_, steps);
    const bool changed = next != rangeNm_;
    rangeNm_ = next;
    if (changed) { dirty_ = true; }
    return changed;
}

void RadarScope::setTubeSize(float w, float h) {
    const float nw = (std::isfinite(w) && w > 0.0f) ? w : 0.0f;
    const float nh = (std::isfinite(h) && h > 0.0f) ? h : 0.0f;
    if (nw != tubeW_ || nh != tubeH_) { dirty_ = true; }
    tubeW_ = nw;
    tubeH_ = nh;
}

void RadarScope::wheel(int dir) {
    if (dir > 0) { stepRange(-1); }
    if (dir < 0) { stepRange(+1); }
}

void RadarScope::dragBy(float dx, float dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.0f && dy == 0.0f)) { return; }
    const Geom g = geomOf(tubeW_, tubeH_);
    if (!g.ok || !last_.hasRx) { return; }
    if (!hasView_) {
        viewLat_ = last_.rxLatDeg;
        viewLon_ = last_.rxLonDeg;
    }
    const double pxPerNm = static_cast<double>(g.radius) / static_cast<double>(rangeNm_);
    const gui::ScopeLatLon moved = gui::scopeViewMoved(viewLat_, viewLon_, static_cast<double>(dx), static_cast<double>(dy), pxPerNm);
    viewLat_ = moved.latDeg;
    viewLon_ = moved.lonDeg;
    hasView_ = true;
    dragged_ = true;
    dirty_ = true;
}

void RadarScope::releasePointer(float x, float y, bool wasDrag) {
    const bool swallow = wasDrag || dragged_;
    dragged_ = false;
    if (swallow) { return; }
    const Geom g = geomOf(tubeW_, tubeH_);
    if (!g.ok || !last_.hasRx || !std::isfinite(x) || !std::isfinite(y)) { return; }
    // the nearest marker within its own hit radius (scope_view.cpp 2819, 2901-2909)
    const RadarTarget* hit = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (const RadarTarget& t : frame_.targets) {
        const float d = std::hypot(x - t.x, y - t.y);
        if (d < t.markR && d < bestDist) {
            bestDist = d;
            hit = &t;
        }
    }
    if (hit != nullptr) {
        selectedId_ = hit->id;
    } else {
        selectedId_.clear();
        // the point under the cursor becomes the point under the middle dot (scope_view.cpp 2942-2952)
        if (!hasView_) {
            viewLat_ = last_.rxLatDeg;
            viewLon_ = last_.rxLonDeg;
        }
        const double pxPerNm = static_cast<double>(g.radius) / static_cast<double>(rangeNm_);
        const gui::ScopeLatLon at = gui::scopeViewMoved(viewLat_, viewLon_, static_cast<double>(g.cx - x), static_cast<double>(g.cy - y), pxPerNm);
        viewLat_ = at.latDeg;
        viewLon_ = at.lonDeg;
        hasView_ = true;
    }
    dirty_ = true;
}

void RadarScope::doubleClick() { resetPan(); }

void RadarScope::setLcdScreen(int screen) {
    const int s = std::clamp(screen, 0, 2);
    if (s != lcdScreen_) { dirty_ = true; }
    lcdScreen_ = s;
}

void RadarScope::selectContact(const std::string& id) {
    selectedId_ = id;
    lcdScreen_ = 1;
    dirty_ = true;
}

void RadarScope::cycleOption(int index) {
    switch (index) {
        case 0: optPhosphor_ = !optPhosphor_; break;
        case 1: optSweep_ = !optSweep_; break;
        case 2: optTrail_ = (optTrail_ + 1) % 3; break;
        case 3: optFilter_ = (optFilter_ + 1) % 3; break;
        default: return;
    }
    dirty_ = true;
}

double RadarScope::knobDetent(int steps, const RadarInputs& in) {
    const KnobState k = knobOf(in, gainLocal_, true);
    if (steps == 0 || !k.live) { return std::numeric_limits<double>::quiet_NaN(); }
    const float db = gui::scopeKnobNextDb(k.db, steps, k.lo, k.hi);
    if (db == k.db) { return std::numeric_limits<double>::quiet_NaN(); }
    // the figure asked for is the base of the next detent until the readback shows it (scope_gain.hpp)
    gainLocal_.valid = true;
    gainLocal_.stage = in.gainStage;
    gainLocal_.db = db;
    gainLocal_.sentAtS = in.nowS;
    return static_cast<double>(db);
}

std::size_t RadarScope::trailPointCount() const {
    std::size_t n = 0;
    for (const auto& kv : trails_) { n += kv.second.size(); }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the tick
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RadarScope::update(const std::vector<cascade::core::HostTrack>& tracks, const RadarInputs& in, RadarInfoSource* info) {
    last_ = in;
    const bool hasRx = in.hasRx && std::isfinite(in.rxLatDeg) && std::isfinite(in.rxLonDeg);
    last_.hasRx = hasRx;
    if (!hasView_ && hasRx) {
        viewLat_ = in.rxLatDeg;
        viewLon_ = in.rxLonDeg;
    }
    const Geom g = geomOf(tubeW_, tubeH_);
    const bool draw = hasRx && g.ok;
    const double fullNm = static_cast<double>(rangeNm_);
    const int iconPx = gui::clampAircraftIconPx(in.aircraftIconPx);
    const float markR = radarMarkerRadius(static_cast<float>(iconPx));

    RadarFrame f;
    f.hasRx = hasRx;
    f.rangeNm = rangeNm_;
    f.rangeText = gui::scopeRangeReadout(rangeNm_);
    f.rangeCornerText = f.rangeText;
    f.canStepDown = gui::scopeRangeStepped(rangeNm_, -1) != rangeNm_;
    f.canStepUp = gui::scopeRangeStepped(rangeNm_, +1) != rangeNm_;
    f.tubeW = tubeW_;
    f.tubeH = tubeH_;
    f.trailWidthPx = in.trailWidthPx;
    f.panned = hasView_;
    f.modeText = tr("MODE ADS-B");
    f.lcdScreen = lcdScreen_;
    f.lcdShown = lcdShown_;
    f.phosphor = optPhosphor_;
    f.sweep = optSweep_;
    f.trail = optTrail_;
    f.filter = optFilter_;
    f.selectedId = selectedId_;
    if (hasRx) {
        f.rxText = gui::detail::scopePrintf("%.1f%s %05.1f%s", std::fabs(in.rxLatDeg), cascade::i18n::hemisphereLetter(true, in.rxLatDeg >= 0.0), std::fabs(in.rxLonDeg),
                                            cascade::i18n::hemisphereLetter(false, in.rxLonDeg >= 0.0));
    }
    if (draw) {
        f.centreX = g.cx;
        f.centreY = g.cy;
        f.radiusPx = g.radius;
        if (hasView_) {
            const gui::ScopePoint rx = gui::scopeGroundPoint(g.cx, g.cy, g.radius, fullNm, viewLat_, viewLon_, in.rxLatDeg, in.rxLonDeg);
            f.rxMarkX = static_cast<float>(rx.x);
            f.rxMarkY = static_cast<float>(rx.y);
        }
    }

    // ---- the target loop (scope_view.cpp 2823-2910) ----
    int tracked = 0;
    const cascade::core::HostTrack* selectedTrack = nullptr;       // the PLOTTED track the selection names
    const cascade::core::HostTrack* selectedAny = nullptr;         // the aircraft the selection names, plotted or not (the gauge and the drum look at every aircraft)
    std::set<std::string> live;                                    // every id the host holds, built when the trail store has to evict
    bool liveBuilt = false;
    const auto buildLive = [&]() {
        if (liveBuilt) { return; }
        for (const cascade::core::HostTrack& ht : tracks) { live.insert(idOf(ht.t)); }
        liveBuilt = true;
    };
    for (const cascade::core::HostTrack& ht : tracks) {
        if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
        ++tracked;
        const std::string id = idOf(ht.t);
        if (selectedAny == nullptr && !selectedId_.empty() && id == selectedId_) { selectedAny = &ht; }
        if (!draw) { continue; }
        const Presentation pres = aircraftPresentation(ht.t);
        if (!pres.visible) { continue; }
        // FROM THE MIDDLE OF THE VIEW, where every ring is measured from
        const gui::ScopePolar pol = gui::scopeRelative(viewLat_, viewLon_, ht.t.latDeg, ht.t.lonDeg);
        if (!gui::scopeInRange(pol.rangeNm, fullNm)) { continue; }
        if (optFilter_ == 1 && (ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) == 0u) { continue; }
        if (optFilter_ == 2 && ht.t.label[0] == '\0') { continue; }
        ++f.plotted;

        // WHERE IT HAS BEEN, recorded whether or not the trail is drawn
        {
            auto it = trails_.find(id);
            if (it == trails_.end() && trails_.size() >= kTrailMaxTracks) {
                buildLive();
                for (auto e = trails_.begin(); e != trails_.end();) { e = (live.count(e->first) == 0u) ? trails_.erase(e) : std::next(e); }
                ++trailStamp_;
            }
            if (it == trails_.end() && trails_.size() < kTrailMaxTracks) { it = trails_.emplace(id, std::vector<TrailPoint>()).first; }
            if (it != trails_.end()) {
                std::vector<TrailPoint>& tr = it->second;
                bool add = tr.empty();
                if (!add) {
                    const TrailPoint& last = tr.back();
                    // the same fifty-metre gate the map's altitude store uses
                    const double dLat = (ht.t.latDeg - last.lat) * 111320.0;
                    const double dLon = (ht.t.lonDeg - last.lon) * 111320.0 * std::cos(ht.t.latDeg * kPi / 180.0);
                    add = (dLat * dLat + dLon * dLon) > (kTrailMinMetres * kTrailMinMetres);
                }
                if (add) {
                    tr.push_back(TrailPoint{ht.t.latDeg, ht.t.lonDeg, ht.t.altM});
                    if (tr.size() > kTrailMaxPoints) { tr.erase(tr.begin()); }
                    ++trailStamp_;
                }
            }
        }

        const gui::ScopePoint p = gui::scopeProject(g.cx, g.cy, g.radius, pol.rangeNm, pol.bearingDeg, fullNm);
        RadarTarget t;
        t.id = id;
        t.x = static_cast<float>(p.x);
        t.y = static_cast<float>(p.y);
        t.headingDeg = std::isfinite(ht.t.courseDeg) ? static_cast<float>(ht.t.courseDeg) : 0.0f;
        t.rgba = targetColour(ht.t, pres.alpha);
        t.altKnown = gui::altitudeBandIndex(ht.t.altM) >= 0;
        t.emergency = (ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u;
        t.selected = !selectedId_.empty() && id == selectedId_;
        t.markR = markR;
        t.label = nameOf(ht.t);
        if (t.selected) { selectedTrack = &ht; }
        f.targets.push_back(std::move(t));
    }

    // ---- the trails (scope_view.cpp 2764-2815): re-projected only when what they depend on changed ----
    {
        Fnv k;
        k.pod(trailStamp_);
        k.pod(rangeNm_);
        k.pod(g.cx); k.pod(g.cy); k.pod(g.radius);
        k.pod(draw);
        k.pod(viewLat_); k.pod(viewLon_);
        k.pod(optTrail_);
        k.str(selectedId_);
        std::vector<const cascade::core::HostTrack*> drawn;
        if (draw && optTrail_ != 0) {
            std::set<std::string> seen;
            for (const cascade::core::HostTrack& ht : tracks) {
                if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
                const std::string id = idOf(ht.t);
                const auto it = trails_.find(id);
                if (it == trails_.end() || it->second.size() < 2 || !seen.insert(id).second) { continue; }
                drawn.push_back(&ht);
                k.str(id);
            }
        }
        if (k.h != trailKey_) {
            trailKey_ = k.h;
            trailsOut_.clear();
            trailKeys_.clear();
            for (const auto& kv : trails_) { trailKeys_.push_back(kv.first); }
            for (const cascade::core::HostTrack* ht : drawn) {
                const std::string id = idOf(ht->t);
                const std::vector<TrailPoint>& tr = trails_.find(id)->second;
                const bool picked = !selectedId_.empty() && selectedId_ == id;
                struct Placed { bool inRange; float x, y; };
                std::vector<Placed> placed(tr.size());
                for (std::size_t j = 0; j < tr.size(); ++j) {
                    const gui::ScopePolar pp = gui::scopeRelative(viewLat_, viewLon_, tr[j].lat, tr[j].lon);
                    placed[j].inRange = gui::scopeInRange(pp.rangeNm, fullNm);
                    const gui::ScopePoint q = gui::scopeProject(g.cx, g.cy, g.radius, pp.rangeNm, pp.bearingDeg, fullNm);
                    placed[j].x = static_cast<float>(q.x);
                    placed[j].y = static_cast<float>(q.y);
                }
                RadarTrail run;
                const auto flush = [&]() {
                    if (run.points.size() >= 2) {
                        run.points.front().rgba = run.points[1].rgba;   // the first point carries its leg's colour
                        trailsOut_.push_back(std::move(run));
                    }
                    run = RadarTrail();
                };
                run.id = id;
                run.selected = picked;
                for (std::size_t j = 1; j < tr.size(); ++j) {
                    // A leg with either end outside the range is dropped rather than clipped, for the same reason a target is.
                    if (!placed[j - 1].inRange || !placed[j].inRange) {
                        flush();
                        run.id = id;
                        run.selected = picked;
                        continue;
                    }
                    // the MEAN of the two ends, so a climbing leg changes colour half way up
                    const double midAlt = (std::isfinite(tr[j - 1].altM) && std::isfinite(tr[j].altM)) ? 0.5 * (tr[j - 1].altM + tr[j].altM) : (std::isfinite(tr[j].altM) ? tr[j].altM : tr[j - 1].altM);
                    const int band = gui::altitudeBandIndex(midAlt);
                    const gui::AltBandStyle& bs = gui::altBandStyle(band < 0 ? 0 : band);
                    // older legs fade, so which end is now needs no arrowhead
                    const float u = static_cast<float>(j) / static_cast<float>(tr.size());
                    const int alpha = static_cast<int>((60.0f + 150.0f * u) * (picked ? 1.0f : 0.75f));
                    const std::uint32_t col = band < 0 ? packRgba(134, 214, 74, alpha) : packRgba(bs.r, bs.g, bs.b, alpha);   // kPhosphor when the altitude is unknown
                    if (run.points.empty()) { run.points.push_back({placed[j - 1].x, placed[j - 1].y, col}); }
                    run.points.push_back({placed[j].x, placed[j].y, col});
                }
                flush();
            }
        }
    }

    // ---- the corner readouts, rings and bearing ticks (scope_view.cpp 3022-3152) ----
    f.plottedText = gui::scopeTracksReadout(f.plotted, tr(gui::kScopeTracksFormat));
    if (draw) {
        for (int ring = 1; ring <= gui::kScopeRingCount; ++ring) {
            const float r = g.radius * static_cast<float>(ring) / static_cast<float>(gui::kScopeRingCount);
            const bool outermost = ring == gui::kScopeRingCount;
            RadarLabel l;
            l.text = gui::scopeRingLabel(rangeNm_, ring);
            const float w = kCharPx * static_cast<float>(l.text.size());
            // at the 12 o'clock radial, a few pixels right of it; the outer ring's label inside the ring, the others above theirs (centred here on the estimated box)
            l.x = g.cx + 4.0f + w * 0.5f;
            l.y = (outermost ? g.cy - r + 3.0f : g.cy - r - kLineH - 1.0f) + kLineH * 0.5f;
            l.strong = outermost;
            f.ringLabels.push_back(std::move(l));
        }
        const float tickLabelW = kCharPx * 3.0f;
        f.showThreeDigitBearings = g.radius >= std::max(150.0f, 2.2f * (tickLabelW + 8.0f));
        for (int b = 0; b < 360; b += 30) {
            const bool cardinal = (b % 90) == 0;
            if (!cardinal && !f.showThreeDigitBearings) { continue; }
            const double a = static_cast<double>(b) * kPi / 180.0;
            const float lr = g.radius * 0.90f;
            RadarLabel l;
            const std::string key = gui::scopeBearingLabel(b);
            l.text = tr(key.c_str());
            l.x = g.cx + static_cast<float>(std::sin(a)) * lr;
            l.y = g.cy - static_cast<float>(std::cos(a)) * lr;
            l.strong = cardinal;
            f.bearingLabels.push_back(std::move(l));
        }
    }

    // ---- the cabinet (aw.cpp 13862-13925, 14022-14045, 14079-14208) ----
    const bool haveSig = in.running && std::isfinite(in.signalDb);
    if (haveSig) {
        f.sigFrac = static_cast<float>(std::clamp((in.signalDb + 100.0) / 100.0, 0.0, 1.0));   // -100..0 dBFS across the scale
        f.sigLit = static_cast<int>(static_cast<double>(f.sigFrac) * kGaugeSegments + 0.5);
        f.sigText = gui::detail::scopePrintf("%.0f", in.signalDb);
    } else {
        f.sigText = "--";
    }
    const bool haveAlt = selectedAny != nullptr && std::isfinite(selectedAny->t.altM);
    if (haveAlt) {
        f.altFrac = static_cast<float>(std::clamp(selectedAny->t.altM / kAltFullScaleM, 0.0, 1.0));
        f.altLit = static_cast<int>(static_cast<double>(f.altFrac) * kGaugeSegments + 0.5);
        f.altText = flightLevel(selectedAny->t.altM);
    } else if (!selectedId_.empty()) {
        f.altText = tr("NO ALT");   // selected, but the aircraft reports none: a different statement from nothing selected
    } else {
        f.altText = "--";
    }
    {
        int drumNm = rangeNm_;
        f.drumCaption = tr("SET RANGE NM");
        if (selectedAny != nullptr && hasRx) {
            const gui::ScopePolar p = gui::scopeRelative(in.rxLatDeg, in.rxLonDeg, selectedAny->t.latDeg, selectedAny->t.lonDeg);
            if (std::isfinite(p.rangeNm)) {
                drumNm = static_cast<int>(p.rangeNm + 0.5);
                f.drumCaption = tr("TGT RANGE NM");
            }
        }
        f.drumRange = zeroPadded(drumNm, 4);               // four drums since the ladder reached 1600 NM
        f.drumTracks = zeroPadded(tracked, 3);             // every aircraft held, ungated
    }
    {
        const KnobState k = knobOf(in, gainLocal_, true);
        f.knobDead = !k.live;
        gui::scopeKnobTicks(k.lo, k.hi, f.knobTicks);
        f.knobLit = gui::scopeKnobLitTick(k.db, k.lo, k.hi, k.live);
        f.knobFrac = gui::scopeKnobFraction(k.haveGain, k.db, k.lo, k.hi);
        if (!k.haveGain) {
            f.knobReadout = tr("N/A");
            f.knobNote = tr("no device");
        } else if (in.deviceAgc) {
            f.knobReadout = tr("AUTO");
            f.knobNote = tr("auto gain is on");
        } else {
            // in the first stage's own unit: "30 dB", or the bare "7" of a stage measured in steps
            f.knobReadout = in.gainUnit == "dB" ? gui::detail::scopePrintf("%.0f dB", static_cast<double>(k.db)) : gui::detail::scopePrintf("%.0f", static_cast<double>(k.db));
            f.knobNote = in.gainStage;
        }
    }
    f.powerOn = in.running;
    f.powerText = in.running ? tr("ON LINE") : tr("STANDBY");

    // ---- HOME (scope_view.cpp 479-608) ----
    f.homeCount = gui::detail::scopePrintf("%d", f.plotted);
    {
        static const char* const kFilterSuffix[3] = {"", FOX_TR_NOOP("   ALERT ONLY"), FOX_TR_NOOP("   NAMED ONLY")};
        const int fi = (optFilter_ >= 0 && optFilter_ <= 2) ? optFilter_ : 0;
        const std::string suffix = tr(kFilterSuffix[fi]);
        f.homeSub = gui::detail::scopePrintf(tr("of %d tracked   %d NM%s"), tracked, rangeNm_, suffix.c_str());
    }
    {
        struct Row {
            const cascade::core::HostTrack* t;
            double rangeNm, bearingDeg;
        };
        std::vector<Row> rows;
        for (const cascade::core::HostTrack& ht : tracks) {
            if (ht.t.kind != CASCADE_TRACK_AIRCRAFT || !aircraftPresentation(ht.t).visible) { continue; }
            Row r{&ht, 0.0, 0.0};
            if (hasRx) {
                const gui::ScopePolar p = gui::scopeRelative(in.rxLatDeg, in.rxLonDeg, ht.t.latDeg, ht.t.lonDeg);   // from the ANTENNA, not the (possibly dragged) view
                r.rangeNm = p.rangeNm;
                r.bearingDeg = p.bearingDeg;
            }
            rows.push_back(r);
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.rangeNm < b.rangeNm; });
        for (const Row& r : rows) {
            RadarContact c;
            c.id = idOf(r.t->t);
            c.name = nameOf(r.t->t);
            c.alert = (r.t->t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u;
            c.alt = std::isfinite(r.t->t.altM) ? flightLevel(r.t->t.altM) : std::string(tr("NO ALT"));
            const int band = gui::altitudeBandIndex(r.t->t.altM);
            if (band >= 0) {
                const gui::AltBandStyle& bs = gui::altBandStyle(band);
                c.rgba = packRgba(bs.r, bs.g, bs.b, 255);
            }
            if (hasRx) {
                const int brg = std::isfinite(r.bearingDeg) ? static_cast<int>(r.bearingDeg + 0.5) % 360 : 0;   // overhead has no bearing; the classic casts the NaN
                c.detail = gui::detail::scopePrintf("%.0f NM   %03d deg", r.rangeNm, brg);
            } else {
                c.detail = tr("no receiver position");
            }
            f.contacts.push_back(std::move(c));
        }
        if (f.contacts.empty()) {
            if (tracked > 0) {
                f.homeEmptyTitle = tr("NOTHING CURRENT");
                // singular and plural as two whole sentences
                const unsigned long long secs = static_cast<unsigned long long>(cascade::core::kTrackDropMsAircraft / 1000ull);
                f.homeEmptyBody = gui::detail::scopePrintf(
                    tracked == 1 ? tr("%d aircraft has been heard and none has reported inside the last "
                                      "%llu seconds, which is the age the host drops a target at. They "
                                      "have gone out of range, or the decoder has stopped being fed - the "
                                      "Fitted modules window says whether it is still being fed.")
                                 : tr("%d aircraft have been heard and none has reported inside the last "
                                      "%llu seconds, which is the age the host drops a target at. They "
                                      "have gone out of range, or the decoder has stopped being fed - the "
                                      "Fitted modules window says whether it is still being fed."),
                    tracked, secs);
            } else {
                f.homeEmptyTitle = tr("NOTHING BEING HEARD");
                f.homeEmptyBody = tr("No aircraft position has reached this face. If a 1090 MHz decoder "
                                     "is fitted and being fed, check the antenna and try more gain; the "
                                     "Fitted modules window says whether it is.");
            }
        }
    }

    // ---- SYS (scope_view.cpp 686-767) ----
    {
        static const char* const kTrailNames[3] = {FOX_TR_NOOP("OFF"), FOX_TR_NOOP("LINE"), FOX_TR_NOOP("RIBBON")};
        static const char* const kFilterNames[3] = {FOX_TR_NOOP("ALL"), FOX_TR_NOOP("ALERT"), FOX_TR_NOOP("NAMED")};
        f.options.push_back({tr("PHOSPHOR"), tr(optPhosphor_ ? "ON" : "OFF")});
        f.options.push_back({tr("SWEEP"), tr(optSweep_ ? "ON" : "OFF")});
        f.options.push_back({tr("TRAILS"), tr(kTrailNames[std::clamp(optTrail_, 0, 2)])});
        f.options.push_back({tr("FILTER"), tr(kFilterNames[std::clamp(optFilter_, 0, 2)])});
        f.sysRange = gui::detail::scopePrintf("%d NM", rangeNm_);
        f.sysAntenna = hasRx ? gui::detail::scopePrintf("%.3f %.3f", in.rxLatDeg, in.rxLonDeg) : std::string(tr("NOT SET"));
        f.sysHelp = tr("Drag the face to move the view, double-click to recentre, "
                       "and the wheel steps the range. FILTER hides everything but "
                       "emergencies, or everything without a callsign. A trail "
                       "starts when the scope is opened, not when the aircraft was "
                       "first heard.");
    }

    // ---- FLIGHT (scope_view.cpp 775-845): built only while that screen shows, because building it asks the registry ----
    if (lcdScreen_ == 1) {
        if (selectedTrack == nullptr) {
            if (selectedId_.empty()) {
                f.flightTitle = tr("NOTHING SELECTED");
                f.flightBody = tr("Click an aircraft on the scope to see its details.");
            } else {
                // KEPT, NOT CLEARED: the details come back by themselves if the aircraft does
                f.flightTitle = tr("TARGET LOST");
                f.flightBody = gui::detail::scopePrintf(tr("%s is no longer being plotted - it has gone quiet or left the selected range."), selectedId_.c_str());
            }
        } else {
            gui::ScopeDetailInput d;
            d.flight = nameOf(selectedTrack->t);
            d.altM = selectedTrack->t.altM;
            d.speedMps = selectedTrack->t.speedMps;
            d.courseDeg = selectedTrack->t.courseDeg;
            d.emergency = (selectedTrack->t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u;
            d.hasRx = hasRx;
            if (hasRx) {
                const gui::ScopePolar p = gui::scopeRelative(in.rxLatDeg, in.rxLonDeg, selectedTrack->t.latDeg, selectedTrack->t.lonDeg);   // range and bearing are the ANTENNA's
                d.rangeNm = p.rangeNm;
                d.bearingDeg = p.bearingDeg;
            }
            if (info != nullptr) {
                const RadarInfo ri = info->lookup(*selectedTrack);
                if (ri.active) {
                    d.infoActive = true;
                    if (ri.pending) {
                        d.infoPending = true;
                    } else {
                        d.registration = ri.registration;
                        d.typeName = !ri.typeName.empty() ? ri.typeName : ri.typeCode;   // the spelled-out type where the source has one, the code otherwise
                        d.operatorName = ri.operatorName;
                    }
                }
            }
            for (const gui::ScopeDetailLine& l : gui::buildScopeDetailLines(d)) {
                RadarLine line;
                line.label = tr(l.label.c_str());
                line.value = tr(l.value.c_str());
                line.dim = !l.known;
                line.alert = l.alert;
                f.flight.push_back(std::move(line));
            }
            f.flightFoot = tr(gui::scopeUnavailableNote());
        }
    }

    // ---- what the ground picture depends on ----
    {
        Fnv k;
        k.pod(rangeNm_);
        k.pod(tubeW_); k.pod(tubeH_);
        k.pod(hasRx);
        k.pod(viewLat_); k.pod(viewLon_); k.pod(hasView_);
        k.pod(optPhosphor_);
        k.pod(optTrail_);
        k.pod(in.trailWidthPx);
        k.pod(trailKey_);
        if (k.h != pictureKey_) {
            pictureKey_ = k.h;
            dirty_ = true;
        }
    }
    frame_ = std::move(f);
}

}  // namespace foxsdr::slintscope
