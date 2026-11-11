// Renders strings with Slint's software renderer into memory and measures the ink.
// Nothing is shown on screen: the platform below hands Slint a window adapter that
// only owns a renderer. Prints facts; the feature record quotes them.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "probe.h"
#include "slint-platform.h"

namespace {

constexpr int kW = 600, kH = 100;

class Adapter final : public slint::platform::WindowAdapter {
public:
    slint::PhysicalSize size() override { return size_; }
    slint::platform::AbstractRenderer& renderer() override { return renderer_; }
    void set_visible(bool v) override {
        if (v) {
            window().dispatch_scale_factor_change_event(1.0f);
            window().dispatch_resize_event(slint::LogicalSize({static_cast<float>(kW), static_cast<float>(kH)}));
        }
    }
    void request_redraw() override {}
    std::vector<slint::Rgb8Pixel> paint() {
        std::vector<slint::Rgb8Pixel> px(static_cast<std::size_t>(kW) * kH, slint::Rgb8Pixel{0, 0, 0});
        renderer_.render(std::span<slint::Rgb8Pixel>(px), kW);
        return px;
    }

private:
    slint::platform::SoftwareRenderer renderer_{slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};
    slint::PhysicalSize size_{{kW, kH}};
};

Adapter* g_last = nullptr;

class Platform final : public slint::platform::Platform {
public:
    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
        auto a = std::make_unique<Adapter>();
        g_last = a.get();
        return a;
    }
};

struct Img {
    std::vector<std::uint8_t> ink;
    bool at(int x, int y) const { return x >= 0 && y >= 0 && x < kW && y < kH && ink[static_cast<std::size_t>(y) * kW + x] != 0; }
};

Img renderLine(const slint::ComponentHandle<Probe>& ui, const std::string& text, int mode) {
    ui->set_line(slint::SharedString(text));
    ui->set_align_mode(mode);
    slint::platform::update_timers_and_animations();
    const std::vector<slint::Rgb8Pixel> px = g_last->paint();
    Img im;
    im.ink.resize(px.size());
    for (std::size_t i = 0; i < px.size(); ++i) { im.ink[i] = (px[i].r > 100) ? 1 : 0; }
    return im;
}

struct Box { int x0 = 9999, x1 = -1, y0 = 9999, y1 = -1, n = 0; int w() const { return x1 - x0 + 1; } int h() const { return y1 - y0 + 1; } };
Box bbox(const Img& im, int xa = 0, int xb = kW) {
    Box b;
    for (int y = 0; y < kH; ++y) {
        for (int x = xa; x < xb; ++x) {
            if (im.at(x, y)) { b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x); b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y); ++b.n; }
        }
    }
    return b;
}

int components(const Img& im) {
    std::vector<std::uint8_t> seen(im.ink.size(), 0);
    int count = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (!im.at(x, y) || seen[static_cast<std::size_t>(y) * kW + x]) { continue; }
            ++count;
            std::vector<std::pair<int, int>> st{{x, y}};
            seen[static_cast<std::size_t>(y) * kW + x] = 1;
            while (!st.empty()) {
                const auto [cx, cy] = st.back();
                st.pop_back();
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx, ny = cy + dy;
                        if (im.at(nx, ny) && !seen[static_cast<std::size_t>(ny) * kW + nx]) { seen[static_cast<std::size_t>(ny) * kW + nx] = 1; st.push_back({nx, ny}); }
                    }
                }
            }
        }
    }
    return count;
}

// Ink of `im` inside columns [xa, xb) cut to its bounding box.
struct Sprite { std::vector<std::uint8_t> bits; int w = 0, h = 0; };
Sprite cut(const Img& im, int xa, int xb) {
    const Box b = bbox(im, xa, xb);
    Sprite s;
    s.w = b.w(); s.h = b.h();
    if (b.n == 0) { s.w = s.h = 0; return s; }
    s.bits.assign(static_cast<std::size_t>(s.w) * s.h, 0);
    for (int y = 0; y < s.h; ++y) { for (int x = 0; x < s.w; ++x) { s.bits[static_cast<std::size_t>(y) * s.w + x] = im.at(b.x0 + x, b.y0 + y) ? 1 : 0; } }
    return s;
}
// Intersection over union of two same-sized sprites; 0 when the sizes differ.
double iou(const Sprite& a, const Sprite& b) {
    if (a.w != b.w || a.h != b.h || a.w == 0) { return 0.0; }
    int both = 0, either = 0;
    for (std::size_t i = 0; i < a.bits.size(); ++i) { if (a.bits[i] || b.bits[i]) { ++either; if (a.bits[i] && b.bits[i]) { ++both; } } }
    return either ? static_cast<double>(both) / either : 0.0;
}

}  // namespace

int main() {
    slint::platform::set_platform(std::make_unique<Platform>());
    auto ui = Probe::create();
    ui->show();   // calls our adapter's set_visible: no operating-system window exists

    // 1. HEBREW ORDER. Logical order alef, bet, gimel. A right-to-left-aware engine draws
    //    gimel on the LEFT and alef on the RIGHT; an engine that only knows left-to-right
    //    draws alef on the left. The glyphs are identified by matching their rendered shapes.
    const std::string alef = "\xD7\x90", bet = "\xD7\x91", gimel = "\xD7\x92";
    const Sprite tA = cut(renderLine(ui, alef, 0), 0, kW), tG = cut(renderLine(ui, gimel, 0), 0, kW);
    const Img three = renderLine(ui, alef + bet + gimel, 0);
    const Box all = bbox(three);
    // The glyph that starts at the left end, cut by the width of alef / of gimel.
    Img leftOnly = three, rightOnly = three;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (x > all.x0 + std::max(tA.w, tG.w) - 1) { leftOnly.ink[static_cast<std::size_t>(y) * kW + x] = 0; }
            if (x < all.x1 - std::max(tA.w, tG.w) + 1) { rightOnly.ink[static_cast<std::size_t>(y) * kW + x] = 0; }
        }
    }
    const Sprite sl = cut(leftOnly, 0, kW), sr = cut(rightOnly, 0, kW);
    std::printf("HEBREW  alef %dx%d  gimel %dx%d  three-letter ink x %d..%d\n", tA.w, tA.h, tG.w, tG.h, all.x0, all.x1);
    std::printf("  leftmost glyph:  matches alef %.2f, gimel %.2f\n", iou(sl, tA), iou(sl, tG));
    std::printf("  rightmost glyph: matches alef %.2f, gimel %.2f\n", iou(sr, tA), iou(sr, tG));
    const bool rtlOrder = iou(sr, tA) > iou(sr, tG);
    std::printf("  => the first logical letter is drawn at the %s: %s\n", rtlOrder ? "RIGHT" : "LEFT",
                rtlOrder ? "bidirectional ordering IS applied" : "no right-to-left ordering");

    // 2. ARABIC SHAPING. Four BEH (U+0628). Isolated forms side by side leave gaps; joined
    //    (initial/medial/final) forms touch, so the ink is narrower and has fewer components.
    const std::string beh = "\xD8\xA8";
    const Img one = renderLine(ui, beh, 0), four = renderLine(ui, beh + beh + beh + beh, 0);
    const Box b1 = bbox(one), b4 = bbox(four);
    std::printf("ARABIC  one beh: ink width %d, %d connected parts\n", b1.w(), components(one));
    std::printf("  four beh: ink width %d, %d connected parts\n", b4.w(), components(four));
    std::printf("  isolated forms would be at least %d wide with %d parts; %s\n", 4 * b1.w(), 4 * components(one),
                (b4.w() < 4 * b1.w() - 8 && components(four) < 4 * components(one)) ? "=> letters are JOINED (shaping applied)"
                                                                                    : "=> letters are NOT joined");

    // 3. PARAGRAPH DIRECTION and the alignment values. 0 left, 1 center, 2 right, 3 start, 4 end.
    const char* names[] = {"left", "center", "right", "start", "end"};
    for (int a = 0; a < 5; ++a) {
        const Box bh = bbox(renderLine(ui, alef + bet + gimel, a)), bl = bbox(renderLine(ui, "abc", a));
        std::printf("ALIGN %-6s hebrew ink x %3d..%3d   latin ink x %3d..%3d\n", names[a], bh.x0, bh.x1, bl.x0, bl.x1);
    }
    return 0;
}
