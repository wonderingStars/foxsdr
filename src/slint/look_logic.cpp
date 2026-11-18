// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "look_logic.hpp"

#include <algorithm>
#include <cmath>

#include "gui/ui_scale.hpp"

namespace foxsdr::slintlook {

namespace {
constexpr float kMinScale = 0.5f;
constexpr float kMaxScale = 4.0f;
constexpr float kSame = 0.001f;
}  // namespace

int stepUiSizePercent(int nowPercent, int dir) {
    const int* steps = cascade::gui::uiscale::kSteps;
    const int count = cascade::gui::uiscale::kStepCount;
    if (dir > 0) {
        for (int i = 0; i < count; ++i) { if (steps[i] > nowPercent) { return steps[i]; } }
        return 0;
    }
    for (int i = count - 1; i >= 0; --i) { if (steps[i] < nowPercent) { return steps[i]; } }
    return 0;
}

float fitCounterScale(float requested, float readings, float windowWidth) {
    if (!(requested > 1.0f)) { return 1.0f; }
    constexpr float kPerScale = 379.0f;          // the counter's width grows by this for each whole step of scale
    constexpr float kLanesNeed = 525.0f;         // three lanes whose headers (caption, reading, AUTO key) do not touch
    constexpr float kLanesNeedPerReading = 150.0f;
    const float lanesAtNormal = windowWidth >= 1760.0f ? windowWidth - 1307.0f : windowWidth - 1000.0f;
    const float need = kLanesNeed + kLanesNeedPerReading * (std::max(readings, 1.0f) - 1.0f);
    const float cap = 1.0f + (lanesAtNormal - need) / kPerScale;
    return std::clamp(cap, 1.0f, requested);
}

SkinId skinOfTheme(const std::string& uiTheme) {
    if (uiTheme == "night") { return SkinId::NightWatch; }
    if (uiTheme == "glass") { return SkinId::GlassCockpit; }
    if (uiTheme == "daylight") { return SkinId::DaylightLab; }
    if (uiTheme == "field") { return SkinId::FieldRadio; }
    return SkinId::Workbench;   // today, classic-xl (the same palette; its figures are the counter scale) and anything unknown
}
const char* skinName(SkinId id) {
    switch (id) {
        case SkinId::NightWatch: return "night";
        case SkinId::HighContrast: return "highcontrast";
        case SkinId::GlassCockpit: return "glass";
        case SkinId::DaylightLab: return "daylight";
        case SkinId::FieldRadio: return "field";
        default: return "workbench";
    }
}

CounterFace faceOfStyle(const std::string& style) {
    if (style == "neon") { return CounterFace::Drum; }
    if (style == "plain") { return CounterFace::Plain; }
    return CounterFace::Nixie;
}
const char* faceStyleName(CounterFace f) { return f == CounterFace::Drum ? "neon" : f == CounterFace::Plain ? "plain" : "nixie"; }

float scaleWanted(const std::string& choice, float osScale) {
    float s = osScale;
    for (const int step : cascade::gui::uiscale::kSteps) {
        if (choice == std::to_string(step)) { s = static_cast<float>(step) / 100.0f; break; }
    }
    return std::clamp(s, kMinScale, kMaxScale);
}

std::pair<std::uint32_t, std::uint32_t> sizeForScale(std::uint32_t width, std::uint32_t height, float from, float to) {
    const double k = from > 0.0f ? static_cast<double>(to) / static_cast<double>(from) : 1.0;
    const auto scaled = [k](std::uint32_t v) { return static_cast<std::uint32_t>(std::max(1.0, std::floor(static_cast<double>(v) * k + 0.5))); };
    return {scaled(width), scaled(height)};
}

WindowFit fitWindowToScreen(std::uint32_t w, std::uint32_t h, float scale, float minW, float minH, int screenW, int screenH) {
    const auto up = [](float v) { return static_cast<std::uint32_t>(std::max(1.0, std::ceil(static_cast<double>(v)))); };
    WindowFit f;
    f.w = std::max(w, up(minW * scale));
    f.h = std::max(h, up(minH * scale));
    if (screenW > 0) { f.w = std::min(f.w, static_cast<std::uint32_t>(screenW)); }
    if (screenH > 0) { f.h = std::min(f.h, static_cast<std::uint32_t>(screenH)); }
    return f;
}

WindowPlace placeInWorkArea(int x, int y, std::uint32_t w, std::uint32_t h, const WorkRect& r) {
    WindowPlace p{x, y};
    if (r.right <= r.left || r.bottom <= r.top) { return p; }   // no work area known: the window stays where it is
    const auto fit = [](int pos, std::uint32_t size, int lo, int hi) {
        const long long room = static_cast<long long>(hi) - lo;
        if (static_cast<long long>(size) >= room) { return lo; }
        return static_cast<int>(std::max<long long>(lo, std::min<long long>(pos, hi - static_cast<long long>(size))));
    };
    p.x = fit(x, w, r.left, r.right);
    p.y = fit(y, h, r.top, r.bottom);
    return p;
}

void SizeMirror::asked(std::uint32_t w, std::uint32_t h) {
    want_ = true; wantW_ = w; wantH_ = h; waited_ = 0;
    guard_ = false;
}

SizeMirror::Step SizeMirror::tick(std::uint32_t w, std::uint32_t h, float scale, float os) {
    Step s;
    if (w == 0 || h == 0) { return s; }
    const float sf = std::max(0.5f, scale);
    if (want_) {   // a size was asked for: nothing is written until it is there (or the system has plainly refused it: a maximised window). A number written from the size the window had would be
        // carried to the system and could land after the ask and undo it.
        if (w == wantW_ && h == wantH_) { want_ = false; }
        else if (++waited_ > 10) { want_ = false; }
        else { return s; }
    }
    if (std::fabs(scale - os) <= kSame) {   // the system's own scale: a written number comes back as the size it came from
        guard_ = false;
        s.write = true; s.winW = static_cast<float>(w) / sf; s.winH = static_cast<float>(h) / sf;
        wrote_ = true; lastW_ = s.winW; lastH_ = s.winH;
        return s;
    }
    if (guard_) {   // the last write was a change: if it moved the window, the window goes back
        if (w == guardW_ && h == guardH_) { guard_ = false; }
        else if (guardLeft_-- > 0) { s.repair = true; s.repairW = guardW_; s.repairH = guardH_; return s; }
        else { guard_ = false; }
    }
    const float nw = static_cast<float>(w) / sf, nh = static_cast<float>(h) / sf;
    if (!wrote_ || std::fabs(nw - lastW_) > 0.01f || std::fabs(nh - lastH_) > 0.01f) {
        s.write = true; s.winW = nw; s.winH = nh;
        wrote_ = true; lastW_ = nw; lastH_ = nh;
        guard_ = true; guardW_ = w; guardH_ = h; guardLeft_ = 3;
    }
    return s;
}

std::pair<std::uint32_t, std::uint32_t> ScaleFollower::sizeAfter(std::uint32_t w, std::uint32_t h, float from, float to) {
    if (grown_ && to < from && w <= askedW_ && h <= askedH_) {   // going back down: from what the window was before the grow, not from what the system left of it
        const auto back = sizeForScale(growW_, growH_, growFrom_, to);
        if (std::fabs(to - growFrom_) <= kSame) { grown_ = false; }
        return back;
    }
    const auto ns = sizeForScale(w, h, from, to);
    if (to > from) {
        if (!grown_ || w > askedW_ || h > askedH_) {   // a first grow, or the window was resized by hand since the last: remember this size
            grown_ = true; growFrom_ = from; growW_ = w; growH_ = h;
        }
        askedW_ = ns.first; askedH_ = ns.second;
    } else {
        grown_ = false;
    }
    return ns;
}

ScalePlan ScaleFollower::plan(const std::string& choice, float current) {
    // The first look, or a window that reports a number this follower did not dispatch: the operating system set it.
    if (applied_ <= 0.0f || std::fabs(current - applied_) > kSame) { os_ = current; }
    ScalePlan p;
    p.os = os_;
    p.want = scaleWanted(choice, os_);
    p.change = std::fabs(p.want - current) > kSame;
    return p;
}

}  // namespace foxsdr::slintlook
