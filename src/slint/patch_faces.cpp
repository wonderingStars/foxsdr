// patch_faces.cpp - the PATCH page's Spectrum and Map faces (patch_faces.hpp). SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "patch_faces.hpp"

#include <algorithm>
#include <cmath>

#include "gui/patch_scope_math.hpp"
#include "kit_logic.hpp"

namespace foxsdr::slintpatch {

namespace pgs = cascade::gui::patch;
using foxsdr::slinthost::Rgba;

// ---------------------------------------------------------------------------------------------------------------------------------
// THE SPECTRUM WELL
// ---------------------------------------------------------------------------------------------------------------------------------

WellSize wellSizeOf(float nodeW, float nodeH) {
    WellSize s;
    // The face is the well (the node less 6 px each side, 20 px of header and 16 of grip) with the face's own 5 px inset: 22 across and 40 down.
    const float faceW = nodeW - 22.0f, faceH = nodeH - 40.0f;
    if (!(faceW >= 16.0f) || !(faceH >= 12.0f)) { return s; }
    const float traceH = faceH * 0.38f;
    const float wfH = faceH - traceH - 2.0f;
    s.w = std::min(kWellMaxW, static_cast<std::uint32_t>(faceW));
    s.traceH = std::min(kWellMaxTraceH, static_cast<std::uint32_t>(std::max(0.0f, traceH)));
    s.wfH = std::min(kWellMaxWfH, static_cast<std::uint32_t>(std::max(0.0f, wfH)));
    return s;
}

bool SpectrumWell::shape(const WellSize& size, double loHz, double hiHz, double rateHz) {
    if (size == size_ && loHz == lo_ && hiHz == hi_ && rateHz == rate_ && !picture_.empty()) { return false; }
    size_ = size;
    lo_ = loHz;
    hi_ = hiHz;
    rate_ = rateHz;
    clear();
    return true;
}

void SpectrumWell::clear() {
    const std::size_t t = static_cast<std::size_t>(size_.w) * size_.traceH, p = static_cast<std::size_t>(size_.w) * size_.wfH;
    trace_.assign(t, Rgba{0, 0, 0, 0});
    picture_.assign(p, Rgba{0x06, 0x07, 0x0a, 255});   // the receiver's waterfall ground
    foxsdr::slinthost::resetTrace(traceState_);
    frames_ = 0;
    peakColumn_ = -1;
    peakDb_ = floorDb_ = 0.0f;
    rangeLo_ = rangeHi_ = 0.0f;
}

bool SpectrumWell::push(const std::vector<float>& frame) {
    if (!size_.usable() || trace_.empty() || picture_.empty()) { return false; }
    if (!pgs::binsToColumns(frame, rate_, lo_, hi_, static_cast<int>(size_.w), -200.0f, cols_)) { return false; }
    const pgs::DbRange range = pgs::autoRange(cols_);
    rangeLo_ = range.lo;
    rangeHi_ = range.hi;
    // the trace: the receiver's painter, on a picture of this part's size (a transparent ground, the live trace in the main trace's own colour)
    foxsdr::slinthost::paintTrace(trace_.data(), size_.w, size_.traceH, cols_.data(), cols_.size(), range.lo, range.hi, traceState_, foxsdr::slinthost::TraceMode::Normal,
                                  Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{0xff, 0x9f, 0x43, 255}, Rgba{0xf1, 0xea, 0xd6, 255});
    // the waterfall: the receiver's scroll and colour map, one row a frame
    foxsdr::slinthost::scrollWaterfallRow(picture_.data(), size_.w, size_.wfH, cols_.data(), cols_.size(), range.lo, range.hi, nullptr);
    ++frames_;
    // the figures a reader checks: the loudest column and the median
    peakColumn_ = 0;
    peakDb_ = cols_[0];
    for (std::size_t i = 1; i < cols_.size(); ++i) {
        if (cols_[i] > peakDb_) { peakDb_ = cols_[i]; peakColumn_ = static_cast<int>(i); }
    }
    std::vector<float> s(cols_);
    std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2), s.end());
    floorDb_ = s[s.size() / 2];
    return true;
}

namespace {
std::uint64_t fnv(const std::vector<Rgba>& v) {
    std::uint64_t h = 1469598103934665603ull;
    for (const Rgba& p : v) {
        h = (h ^ p.r) * 1099511628211ull;
        h = (h ^ p.g) * 1099511628211ull;
        h = (h ^ p.b) * 1099511628211ull;
        h = (h ^ p.a) * 1099511628211ull;
    }
    return h;
}
}  // namespace

std::uint64_t SpectrumWell::traceHash() const { return fnv(trace_); }
std::uint64_t SpectrumWell::waterfallHash() const { return fnv(picture_); }

int SpectrumWell::litWaterfallRows() const {
    if (size_.w == 0) { return 0; }
    int rows = 0;
    for (std::uint32_t y = 0; y < size_.wfH; ++y) {
        bool lit = false;
        for (std::uint32_t x = 0; x < size_.w && !lit; ++x) {
            const Rgba& p = picture_[static_cast<std::size_t>(y) * size_.w + x];
            lit = !(p.r == 0x06 && p.g == 0x07 && p.b == 0x0a);
        }
        if (lit) { ++rows; }
    }
    return rows;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// THE MAP FACE
// ---------------------------------------------------------------------------------------------------------------------------------

double wholeWorldSpan(double w, double h) {
    if (!(h > 0.0) || !(w > 0.0)) { return 360.0; }
    const double byHeight = 180.0 * w / h;
    return byHeight > 360.0 ? byHeight : 360.0;
}

void wholeWorld(MapFaceView& v, double w, double h, bool mercator) {
    v.centreLat = 0.0;
    v.centreLon = 0.0;
    v.spanDeg = mercator ? 360.0 : wholeWorldSpan(w, h);
    v.fitted = true;
}

bool fitToTargets(MapFaceView& v, const std::vector<cascade::core::HostTrack>& tracks, double w, double h, PresentFn present) {
    std::vector<cascade::core::HostTrack> shown;
    for (const cascade::core::HostTrack& t : tracks) {
        if (present == nullptr || present(t.t).visible) { shown.push_back(t); }
    }
    const double aspect = h > 0.0 ? w / h : 1.6;
    if (!foxsdr::slintkit::fitView(shown, v.centreLat, v.centreLon, v.spanDeg, aspect)) { return false; }
    v.fitted = true;
    return true;
}

std::vector<MapFaceTarget> projectTargets(const std::vector<cascade::core::HostTrack>& tracks, const foxsdr::slintmap::TileCompositor& comp, const foxsdr::slintmap::MapLook& look, PresentFn present) {
    std::vector<MapFaceTarget> out;
    for (const cascade::core::HostTrack& tr : tracks) {
        const cascade::core::TrackPresentation pres = present != nullptr ? present(tr.t) : cascade::core::TrackPresentation{};
        if (!pres.visible || !cascade::core::trackHasPosition(tr.t)) { continue; }
        double fx = -1.0, fy = -1.0;
        if (!comp.project(tr.t.latDeg, tr.t.lonDeg, fx, fy)) { continue; }
        if (out.size() >= kMaxFaceTargets) { break; }
        MapFaceTarget m;
        m.id = tr.t.id;
        m.label = tr.t.label[0] != '\0' ? std::string(tr.t.label) : std::string(tr.t.id);
        m.x = static_cast<float>(fx);
        m.y = static_cast<float>(fy);
        m.heading = std::isfinite(tr.t.courseDeg) ? static_cast<float>(tr.t.courseDeg) : -1.0f;
        m.kind = static_cast<int>(tr.t.kind);
        m.alpha = pres.alpha;
        m.estimated = cascade::core::trackIsEstimated(tr.t);
        m.emergency = (tr.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u;
        m.size = static_cast<float>(foxsdr::slintmap::targetIconPx(static_cast<int>(tr.t.kind), look));
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<cascade::core::HostTrack> tracksOfSources(const std::vector<cascade::core::HostTrack>& all, const std::vector<std::string>& sources) {
    std::vector<cascade::core::HostTrack> out;
    for (const cascade::core::HostTrack& t : all) {
        if (std::find(sources.begin(), sources.end(), t.plugin) != sources.end()) { out.push_back(t); }
    }
    return out;
}

void panView(MapFaceView& v, const foxsdr::slintmap::TileCompositor& comp, float dx, float dy) {
    double lat0 = 0.0, lon0 = 0.0, lat1 = 0.0, lon1 = 0.0;
    comp.unproject(0.5, 0.5, lat0, lon0);
    comp.unproject(0.5 - dx, 0.5 - dy, lat1, lon1);
    v.centreLat = std::clamp(v.centreLat + (lat1 - lat0), -85.0, 85.0);
    double lon = v.centreLon + (lon1 - lon0);
    while (lon > 180.0) { lon -= 360.0; }
    while (lon < -180.0) { lon += 360.0; }
    v.centreLon = lon;
    v.fitted = true;
}

void zoomView(MapFaceView& v, const foxsdr::slintmap::TileCompositor& comp, double factor, float fx, float fy) {
    double lat = 0.0, lon = 0.0;
    comp.unproject(fx, fy, lat, lon);
    const double before = v.spanDeg;
    v.spanDeg = std::clamp(v.spanDeg * factor, foxsdr::slintmap::TileCompositor::kMinSpanDeg, foxsdr::slintmap::TileCompositor::kMaxSpanDeg);
    const double k = before > 0.0 ? v.spanDeg / before : 1.0;
    v.centreLon = lon + (v.centreLon - lon) * k;
    v.centreLat = std::clamp(lat + (v.centreLat - lat) * k, -85.0, 85.0);
    v.fitted = true;
}

}  // namespace foxsdr::slintpatch
