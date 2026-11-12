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

SkinId skinOfTheme(const std::string& uiTheme) { return uiTheme == "night" ? SkinId::NightWatch : SkinId::Workbench; }
const char* skinName(SkinId id) { return id == SkinId::NightWatch ? "night" : id == SkinId::HighContrast ? "highcontrast" : "workbench"; }

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
