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

StartFit fitStartWindow(float wantW, float wantH, float minW, float minH, float scale, const WorkRect& work, int curX, int curY) {
    const double sf = static_cast<double>(std::clamp(scale, kMinScale, kMaxScale));
    const auto phys = [sf](float logical) { return static_cast<std::uint32_t>(std::max(1.0, std::ceil(static_cast<double>(logical) * sf - 1e-3))); };   // (the 1e-3: a logical size made from a physical one comes back whole)
    StartFit f;
    f.minW = minW;
    f.minH = minH;
    f.x = curX;
    f.y = curY;
    if (work.right <= work.left || work.bottom <= work.top) {   // no work area known: the wanted size, left where it is
        f.w = phys(wantW);
        f.h = phys(wantH);
        return f;
    }
    const std::uint32_t areaW = static_cast<std::uint32_t>(work.right - work.left);
    const std::uint32_t areaH = static_cast<std::uint32_t>(work.bottom - work.top);
    const WindowFit fit = fitWindowToScreen(phys(wantW), phys(wantH), static_cast<float>(sf), minW, minH, static_cast<int>(areaW), static_cast<int>(areaH));
    f.maximise = areaW < phys(minW) || areaH < phys(minH);
    if (f.maximise) {   // the screen cannot hold the smallest window: fill it, and the minimum for this session is what the screen has
        f.minW = std::min(minW, static_cast<float>(static_cast<double>(areaW) / sf));
        f.minH = std::min(minH, static_cast<float>(static_cast<double>(areaH) / sf));
        f.w = areaW;
        f.h = areaH;
        f.x = work.left;
        f.y = work.top;
        return f;
    }
    f.w = fit.w;
    f.h = fit.h;
    const WindowPlace at = placeInWorkArea(curX, curY, f.w, f.h, work);
    f.x = at.x;
    f.y = at.y;
    return f;
}

void StartHold::begin(const HoldLook& before, const StartFit& fit) {
    before_ = before;
    fit_ = fit;
    startMs_ = -1;
    looks_ = 0;
    stable_ = 0;
    over_ = false;
    matched_ = false;
    last_ = HoldAction::Kind::Finished;
}

HoldAction StartHold::look(const HoldLook& now, std::int64_t nowMs) {
    HoldAction a;
    if (over_) { a.kind = last_; return a; }
    const auto end = [&](HoldAction::Kind k) { over_ = true; last_ = k; a.kind = k; return a; };
    if (startMs_ < 0) { startMs_ = nowMs; }
    // the user's, or the system's by the user's shortcut: a window that is minimised, or (on the first look) already maximised, is left exactly as it is
    if (now.minimised) { return end(HoldAction::Kind::User); }
    if (looks_ == 0 && now.maximised) { return end(HoldAction::Kind::User); }
    if (++looks_ > kMaxTicks || nowMs - startMs_ > kMaxMs) { return end(HoldAction::Kind::Finished); }
    const bool sizeOurs = now.w == fit_.w && now.h == fit_.h;
    const bool placeOurs = now.x == fit_.x && now.y == fit_.y;
    if (fit_.maximise) {
        if (now.maximised) { return end(HoldAction::Kind::Finished); }   // maximised: the fit's work is done (a Restore later is the user's: the restore rectangle is the fitted one)
        // the size and place are made the fit's FIRST and seen as such on two looks in a row (the system maps the window at its own size and place after the first ask, and the maximise keeps
        // whatever the window is at that moment as the rectangle a Restore returns to): only then the maximise is asked, and again each look until the window is maximised
        if (!(sizeOurs && placeOurs)) { stable_ = 0; a.setSize = !sizeOurs; a.setPlace = !placeOurs; return a; }
        if (++stable_ >= 2) { a.setMaximised = true; }
        return a;
    }
    if (now.maximised) { return end(HoldAction::Kind::User); }   // maximised since the fit looked: that is not the fit's doing
    if (sizeOurs && placeOurs) {
        if (++stable_ >= 2) { matched_ = true; }
        return a;
    }
    // Until the system has TAKEN the fit (the window's size and place seen as the fit's on two looks in a row) a size or place that is not the fit's is the system settling (it maps the window at
    // its own default place and size after the first ask): asked again. After that, any other size or place is the user's (a Win+Arrow snap, a drag): the hold ends.
    if (matched_) { return end(HoldAction::Kind::User); }
    stable_ = 0;
    a.setSize = !sizeOurs;
    a.setPlace = !placeOurs;
    return a;
}

void EdgeDrag::begin(int edge, float px, float py, std::uint32_t w, std::uint32_t h) {
    active_ = true; edge_ = edge; px_ = px; py_ = py; w_ = w; h_ = h;
}

WindowFit EdgeDrag::at(float px, float py, float scale, float minW, float minH, std::uint32_t maxW, std::uint32_t maxH) const {
    const double sf = static_cast<double>(std::max(kMinScale, scale));
    const auto axis = [sf](std::uint32_t start, float from, float to, bool moves, float minLogical, std::uint32_t maxPhysical) {
        double v = static_cast<double>(start);
        if (moves && std::isfinite(from) && std::isfinite(to)) { v += (static_cast<double>(to) - static_cast<double>(from)) * sf; }
        const double lo = static_cast<double>(std::llround(static_cast<double>(minLogical) * sf));
        const double hi = std::max(lo, static_cast<double>(maxPhysical));   // a limit below the smallest window is not a limit
        return static_cast<std::uint32_t>(std::llround(std::min(hi, std::max(lo, v))));
    };
    WindowFit f;
    f.w = axis(w_, px_, px, edge_ != 2, minW, maxW);   // the bottom edge leaves the width alone
    f.h = axis(h_, py_, py, edge_ != 1, minH, maxH);   // the right edge leaves the height alone
    return f;
}

WindowFit tornResizeAt(const EdgeDrag& drag, float dx, float dy) {
    return drag.at(dx, dy, 1.0f, kTornMinW, kTornMinH, 16384u, 16384u);
}

TornPlace tornMoveAt(float baseX, float baseY, float dx, float dy) {
    TornPlace p;
    p.x = std::max(0.0f, baseX + (std::isfinite(dx) ? dx : 0.0f));
    p.y = std::max(0.0f, baseY + (std::isfinite(dy) ? dy : 0.0f));
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

AutoFit autoScaleToFit(int workW, int workH, float osScale, const std::string& choice, float minW, float minH) {
    AutoFit r;
    r.scale = scaleWanted(choice, osScale);
    for (const int step : cascade::gui::uiscale::kSteps) {
        if (choice == std::to_string(step)) { return r; }   // a size the user chose: never overridden
    }
    if (workW <= 0 || workH <= 0 || !(minW > 0.0f) || !(minH > 0.0f)) { return r; }   // nothing known to fit to
    const auto holds = [&](int percent) {   // the work area, in logical pixels at `percent`, is at least the layout (in whole physical pixels, as fitStartWindow asks for them)
        return static_cast<double>(workW) * 100.0 >= std::ceil(static_cast<double>(minW) * percent - 1e-3) && static_cast<double>(workH) * 100.0 >= std::ceil(static_cast<double>(minH) * percent - 1e-3);
    };
    const int osPercent = static_cast<int>(std::lround(static_cast<double>(r.scale) * 100.0));
    r.holds = holds(osPercent);
    if (r.holds) { return r; }
    int best = 0;
    for (const int step : cascade::gui::uiscale::kSteps) {
        if (step < osPercent && holds(step)) { best = step; }   // kSteps ascends: the last that holds is the largest
    }
    if (best > 0) {
        r.scale = static_cast<float>(best) / 100.0f;
        r.lowered = true;
        r.holds = true;
    }
    return r;
}

ScalePlan ScaleFollower::plan(const std::string& choice, float current, int workW, int workH, float minW, float minH, float osHint) {
    // The first look, or a window that reports a number this follower did not dispatch: the operating system set it. Where the monitor's own scale can be asked, that is the number.
    if (osHint > 0.0f) { os_ = osHint; }
    else if (applied_ <= 0.0f || std::fabs(current - applied_) > kSame) { os_ = current; }
    ScalePlan p;
    p.os = os_;
    const AutoFit fit = autoScaleToFit(workW, workH, os_, choice, minW, minH);
    p.want = std::clamp(fit.scale, kMinScale, kMaxScale);
    p.lowered = fit.lowered;
    p.change = std::fabs(p.want - current) > kSame;
    return p;
}

}  // namespace foxsdr::slintlook
