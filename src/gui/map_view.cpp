// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/map_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"

namespace cascade::gui {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0;

// Per-kind colours. Chosen to stay distinguishable when several kinds share
// the map, which is the case this whole design exists to support.
ImU32 colourFor(std::uint32_t kind, bool stale) {
    ImU32 c;
    switch (kind) {
        case CASCADE_TRACK_AIRCRAFT: c = IM_COL32(120, 200, 255, 255); break;
        case CASCADE_TRACK_VESSEL:   c = IM_COL32(120, 255, 170, 255); break;
        case CASCADE_TRACK_STATION:  c = IM_COL32(255, 210, 120, 255); break;
        case CASCADE_TRACK_SATELLITE:c = IM_COL32(230, 150, 255, 255); break;
        default:                     c = IM_COL32(200, 200, 200, 255); break;
    }
    if (stale) {
        // Faded rather than removed: a target that stopped reporting a minute
        // ago is still information, and making it vanish the instant it goes
        // quiet loses the last known position exactly when it matters.
        c = (c & 0x00FFFFFFu) | (90u << IM_COL32_A_SHIFT);
    }
    return c;
}

double greatCircleKm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kPi / 180.0;
    const double p2 = lat2 * kPi / 180.0;
    const double dp = (lat2 - lat1) * kPi / 180.0;
    const double dl = (lon2 - lon1) * kPi / 180.0;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0 * kEarthRadiusKm * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kPi / 180.0;
    const double p2 = lat2 * kPi / 180.0;
    const double dl = (lon2 - lon1) * kPi / 180.0;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double b = std::atan2(y, x) * 180.0 / kPi;
    if (b < 0.0) { b += 360.0; }
    return b;
}

// Graticule step that keeps roughly 4-10 lines across the view at any zoom.
double graticuleStep(double spanDeg) {
    static const double steps[] = {0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 45};
    for (double s : steps) {
        if (spanDeg / s <= 10.0) { return s; }
    }
    return 45.0;
}

}  // namespace

void MapView::setHome(double latDeg, double lonDeg) {
    homeLat_ = latDeg;
    homeLon_ = lonDeg;
    hasHome_ = true;
}

void MapView::draw(float width, float height,
                   const std::vector<cascade::core::HostTrack>& tracks,
                   const std::vector<cascade::core::HostPath>& paths) {
    if (width < 32.0f || height < 32.0f) { return; }

    // Fit to content the first time there is any, so the map does not open on
    // empty ocean while every target is somewhere else.
    if (fitRequested_ && !tracks.empty()) {
        double minLat = 90.0, maxLat = -90.0, minLon = 180.0, maxLon = -180.0;
        for (const auto& ht : tracks) {
            minLat = std::min(minLat, ht.t.latDeg);
            maxLat = std::max(maxLat, ht.t.latDeg);
            minLon = std::min(minLon, ht.t.lonDeg);
            maxLon = std::max(maxLon, ht.t.lonDeg);
        }
        centreLat_ = 0.5 * (minLat + maxLat);
        centreLon_ = 0.5 * (minLon + maxLon);
        const double need = std::max(maxLon - minLon, (maxLat - minLat) * 2.0);
        spanDeg_ = std::clamp(need * 1.6, 0.5, 360.0);
        fitRequested_ = false;
        fittedOnce_ = true;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##mapcanvas", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                      IM_COL32(16, 20, 26, 255));

    // Equirectangular, with latitude span derived from the aspect ratio so a
    // degree of latitude and a degree of longitude stay the same size on
    // screen. (They are not the same distance on the ground away from the
    // equator; that is the projection's known and accepted distortion.)
    const double lonSpan = spanDeg_;
    const double latSpan = spanDeg_ * static_cast<double>(height) / static_cast<double>(width);
    const auto toScreen = [&](double lat, double lon) {
        const double x = (lon - centreLon_) / lonSpan * width + width * 0.5;
        const double y = (centreLat_ - lat) / latSpan * height + height * 0.5;
        return ImVec2(origin.x + static_cast<float>(x), origin.y + static_cast<float>(y));
    };
    const auto toWorld = [&](const ImVec2& p) {
        const double lon =
            (static_cast<double>(p.x - origin.x) - width * 0.5) / width * lonSpan + centreLon_;
        const double lat =
            centreLat_ - (static_cast<double>(p.y - origin.y) - height * 0.5) / height * latSpan;
        return std::pair<double, double>(lat, lon);
    };

    // --- interaction ------------------------------------------------------
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            // Zoom about the CURSOR, not the centre: zooming about the middle
            // means the thing being examined slides away as you zoom in.
            const auto before = toWorld(ImGui::GetIO().MousePos);
            spanDeg_ = std::clamp(spanDeg_ * std::pow(0.85, static_cast<double>(wheel)),
                                  0.02, 360.0);
            const double lonSpan2 = spanDeg_;
            const double latSpan2 = spanDeg_ * static_cast<double>(height) / width;
            const ImVec2 m = ImGui::GetIO().MousePos;
            const double lonAfter =
                (static_cast<double>(m.x - origin.x) - width * 0.5) / width * lonSpan2 +
                centreLon_;
            const double latAfter =
                centreLat_ -
                (static_cast<double>(m.y - origin.y) - height * 0.5) / height * latSpan2;
            centreLon_ += before.second - lonAfter;
            centreLat_ += before.first - latAfter;
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        centreLon_ -= static_cast<double>(d.x) / width * lonSpan;
        centreLat_ += static_cast<double>(d.y) / height * latSpan;
    }
    centreLat_ = std::clamp(centreLat_, -89.0, 89.0);
    if (centreLon_ > 180.0) { centreLon_ -= 360.0; }
    if (centreLon_ < -180.0) { centreLon_ += 360.0; }

    // --- graticule --------------------------------------------------------
    const double step = graticuleStep(spanDeg_);
    const ImU32 gridCol = IM_COL32(60, 70, 84, 255);
    const double lat0 = std::floor((centreLat_ - latSpan * 0.5) / step) * step;
    for (double lat = lat0; lat <= centreLat_ + latSpan * 0.5; lat += step) {
        const ImVec2 a = toScreen(lat, centreLon_ - lonSpan * 0.5);
        const ImVec2 b = toScreen(lat, centreLon_ + lonSpan * 0.5);
        dl->AddLine(a, b, gridCol);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.4g", lat);
        dl->AddText(ImVec2(origin.x + 3.0f, a.y - 14.0f), IM_COL32(120, 132, 148, 255), buf);
    }
    const double lon0 = std::floor((centreLon_ - lonSpan * 0.5) / step) * step;
    for (double lon = lon0; lon <= centreLon_ + lonSpan * 0.5; lon += step) {
        const ImVec2 a = toScreen(centreLat_ + latSpan * 0.5, lon);
        const ImVec2 b = toScreen(centreLat_ - latSpan * 0.5, lon);
        dl->AddLine(a, b, gridCol);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.4g", lon);
        dl->AddText(ImVec2(a.x + 3.0f, origin.y + height - 16.0f),
                    IM_COL32(120, 132, 148, 255), buf);
    }

    // --- range rings around the receiver ---------------------------------
    if (hasHome_) {
        const ImVec2 h = toScreen(homeLat_, homeLon_);
        // Ring spacing picked so a handful are visible at the current zoom.
        const double viewKm = latSpan * 111.32;
        static const double ringsKm[] = {10, 25, 50, 100, 200, 400, 800, 1600};
        for (double r : ringsKm) {
            if (r > viewKm) { break; }
            const double dLat = r / 111.32;
            const ImVec2 edge = toScreen(homeLat_ + dLat, homeLon_);
            const float px = std::fabs(edge.y - h.y);
            if (px < 8.0f) { continue; }
            dl->AddCircle(h, px, IM_COL32(70, 90, 110, 200), 64);
            char buf[24];
            std::snprintf(buf, sizeof buf, "%.0f km", r);
            dl->AddText(ImVec2(h.x + 3.0f, h.y - px - 14.0f),
                        IM_COL32(90, 110, 132, 255), buf);
        }
        dl->AddCircleFilled(h, 4.0f, IM_COL32(255, 255, 255, 255));
        dl->AddText(ImVec2(h.x + 6.0f, h.y + 2.0f), IM_COL32(220, 220, 220, 255), "RX");
    }

    // --- paths, under the targets ----------------------------------------
    for (const auto& p : paths) {
        if (p.points.size() < 2) { continue; }
        const ImU32 col = (colourFor(p.kind, false) & 0x00FFFFFFu) | (120u << IM_COL32_A_SHIFT);
        for (std::size_t i = 1; i < p.points.size(); ++i) {
            const double lonA = p.points[i - 1].lonDeg;
            const double lonB = p.points[i].lonDeg;
            // Do not draw the segment that wraps the antimeridian: joining
            // +179 to -179 would streak a line straight across the map, which
            // is what a naive ground-track plot always gets wrong.
            if (std::fabs(lonB - lonA) > 180.0) { continue; }
            dl->AddLine(toScreen(p.points[i - 1].latDeg, lonA),
                        toScreen(p.points[i].latDeg, lonB), col, 1.5f);
        }
    }

    // --- targets ----------------------------------------------------------
    hoveredId_.clear();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float bestDist = 14.0f;  // hit radius in pixels
    const cascade::core::HostTrack* best = nullptr;

    for (const auto& ht : tracks) {
        const ImVec2 s = toScreen(ht.t.latDeg, ht.t.lonDeg);
        if (s.x < origin.x - 40 || s.x > origin.x + width + 40 || s.y < origin.y - 40 ||
            s.y > origin.y + height + 40) {
            continue;
        }
        const bool stale = ht.t.ageMs > 60000ull;
        const ImU32 col = colourFor(ht.t.kind, stale);

        // A course, where known, is drawn as a heading tick. It is the
        // difference between a field of dots and a picture of where things
        // are going.
        if (!std::isnan(ht.t.courseDeg)) {
            const double a = ht.t.courseDeg * kPi / 180.0;
            const ImVec2 tip(s.x + static_cast<float>(std::sin(a) * 12.0),
                             s.y - static_cast<float>(std::cos(a) * 12.0));
            dl->AddLine(s, tip, col, 1.5f);
        }
        dl->AddCircleFilled(s, 3.5f, col);

        const char* lbl = ht.t.label[0] != '\0' ? ht.t.label : ht.t.id;
        dl->AddText(ImVec2(s.x + 6.0f, s.y - 6.0f), col, lbl);

        if (hovered) {
            const float dx = mouse.x - s.x;
            const float dy = mouse.y - s.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < bestDist) {
                bestDist = d;
                best = &ht;
            }
        }
    }

    if (best != nullptr) {
        hoveredId_ = best->t.id;
        ImGui::BeginTooltip();
        ImGui::Text("%s", best->t.label[0] != '\0' ? best->t.label : best->t.id);
        ImGui::Separator();
        ImGui::Text("id      %s", best->t.id);
        ImGui::Text("from    %s", best->plugin.c_str());
        ImGui::Text("pos     %.5f, %.5f", best->t.latDeg, best->t.lonDeg);
        // Unknown values are NaN by ABI contract, and are shown as unknown
        // rather than as zero - "0 kt" and "no speed reported" are different
        // facts and must not look the same.
        if (!std::isnan(best->t.altM)) {
            ImGui::Text("alt     %.0f m (%.0f ft)", best->t.altM, best->t.altM * 3.28084);
        } else {
            ImGui::TextDisabled("alt     unknown");
        }
        if (!std::isnan(best->t.speedMps)) {
            ImGui::Text("speed   %.0f kt", best->t.speedMps * 1.94384);
        } else {
            ImGui::TextDisabled("speed   unknown");
        }
        if (!std::isnan(best->t.courseDeg)) {
            ImGui::Text("course  %.0f deg", best->t.courseDeg);
        } else {
            ImGui::TextDisabled("course  unknown");
        }
        if (hasHome_) {
            ImGui::Text("range   %.1f km at %.0f deg",
                        greatCircleKm(homeLat_, homeLon_, best->t.latDeg, best->t.lonDeg),
                        bearingDeg(homeLat_, homeLon_, best->t.latDeg, best->t.lonDeg));
        }
        ImGui::Text("age     %.1f s", static_cast<double>(best->t.ageMs) / 1000.0);
        ImGui::EndTooltip();
    }

    // Scale bar: a map with no basemap and no scale is a scatter plot.
    {
        const double kmPerPx = (latSpan * 111.32) / static_cast<double>(height);
        double barKm = 1.0;
        while (barKm / kmPerPx < 60.0) { barKm *= 2.0; }
        const float barPx = static_cast<float>(barKm / kmPerPx);
        const ImVec2 a(origin.x + 12.0f, origin.y + height - 26.0f);
        const ImVec2 b(a.x + barPx, a.y);
        dl->AddLine(a, b, IM_COL32(220, 220, 220, 220), 2.0f);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f km", barKm);
        dl->AddText(ImVec2(a.x, a.y - 15.0f), IM_COL32(220, 220, 220, 220), buf);
    }

    dl->PopClipRect();
}

}  // namespace cascade::gui
