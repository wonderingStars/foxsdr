// The headless render harness - see render_harness.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <thread>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace foxsdr::slinttest {
namespace {

// One window adapter per Slint window: it holds the software renderer and the size the test asked for.
class HeadlessWindow final : public slint::platform::WindowAdapter {
public:
    explicit HeadlessWindow(slint::PhysicalSize size) : size_(size) {}
    ~HeadlessWindow() override {
        auto& all = registry();
        all.erase(std::remove(all.begin(), all.end(), this), all.end());
    }

    slint::PhysicalSize size() override { return size_; }
    slint::platform::AbstractRenderer& renderer() override { return renderer_; }
    void set_visible(bool visible) override {
        visible_ = visible;
        if (visible) {
            window().dispatch_scale_factor_change_event(1.0f);
            window().dispatch_resize_event(slint::LogicalSize({static_cast<float>(size_.width), static_cast<float>(size_.height)}));
        }
    }
    void request_redraw() override { dirty_ = true; }
    void set_size(slint::PhysicalSize size) override {
        if (std::getenv("FOXSDR_SLINT_TRACE_SIZE") != nullptr) { std::fprintf(stderr, "headless: slint asks for %u x %u\n", size.width, size.height); }
        size_ = size;
        window().dispatch_resize_event(slint::LogicalSize({static_cast<float>(size.width), static_cast<float>(size.height)}));
    }

    void resize(std::uint32_t w, std::uint32_t h) {
        if (size_.width == w && size_.height == h) { return; }
        set_size(slint::PhysicalSize({w, h}));
    }

    Frame paint() {
        Frame f;
        f.width = size_.width;
        f.height = size_.height;
        f.px.assign(static_cast<std::size_t>(f.width) * f.height, slint::Rgb8Pixel{0, 0, 0});
        renderer_.render(std::span<slint::Rgb8Pixel>(f.px), f.width);
        dirty_ = false;
        return f;
    }

    static std::vector<HeadlessWindow*>& registry() {
        static std::vector<HeadlessWindow*> r;
        return r;
    }
    // A component's slint::Window and the adapter's window() are two C++ wrappers of ONE reference-counted
    // window, so they are matched by that pointer, not by their own addresses. The SDK makes the same layout
    // assumption the other way round (WindowAdapter::window() reinterpret_casts its handle to a Window).
    static const void* sharedWindow(const slint::Window& w) {
        static_assert(sizeof(slint::Window) == sizeof(slint::cbindgen_private::WindowAdapterRcOpaque));
        return reinterpret_cast<const slint::cbindgen_private::WindowAdapterRcOpaque*>(&w)->_0;
    }
    static HeadlessWindow* forWindow(const slint::Window& w) {
        for (HeadlessWindow* hw : registry()) {
            if (sharedWindow(hw->window()) == sharedWindow(w)) { return hw; }
        }
        return nullptr;
    }

private:
    slint::platform::SoftwareRenderer renderer_{slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};
    slint::PhysicalSize size_;
    bool visible_ = false;
    bool dirty_ = true;
};

class HeadlessPlatform final : public slint::platform::Platform {
public:
    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
        auto w = std::make_unique<HeadlessWindow>(slint::PhysicalSize({800, 600}));
        HeadlessWindow::registry().push_back(w.get());
        return w;
    }
};

bool g_installed = false;

void runPendingTasks() {
    slint::platform::update_timers_and_animations();
}

}  // namespace

std::string hex(Rgb c) {
    char b[8];
    std::snprintf(b, sizeof(b), "#%02x%02x%02x", c.r, c.g, c.b);
    return b;
}

bool nearColour(Rgb a, Rgb b, int tol) {
    return std::abs(int(a.r) - int(b.r)) <= tol && std::abs(int(a.g) - int(b.g)) <= tol && std::abs(int(a.b) - int(b.b)) <= tol;
}

Rgb over(Rgb fg, double alpha, Rgb bg) {
    const auto mix = [alpha](std::uint8_t f, std::uint8_t b) {
        return static_cast<std::uint8_t>(std::lround(f * alpha + b * (1.0 - alpha)));
    };
    return {mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b)};
}

Rgb Frame::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) { return {}; }
    const auto& p = px[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
    return {p.r, p.g, p.b};
}
bool Frame::near(int x, int y, Rgb want, int tol) const { return nearColour(at(x, y), want, tol); }
int Frame::countNear(int x0, int y0, int x1, int y1, Rgb want, int tol) const {
    int n = 0;
    for (int y = y0; y < y1; ++y) { for (int x = x0; x < x1; ++x) { if (near(x, y, want, tol)) { ++n; } } }
    return n;
}
int Frame::countInk(int x0, int y0, int x1, int y1, Rgb background, int tol) const {
    int n = 0;
    for (int y = y0; y < y1; ++y) { for (int x = x0; x < x1; ++x) { if (!near(x, y, background, tol)) { ++n; } } }
    return n;
}
bool Frame::rowNear(int y, int x0, int x1, Rgb want, int tol) const {
    for (int x = x0; x < x1; ++x) { if (!near(x, y, want, tol)) { return false; } }
    return x1 > x0;
}
bool Frame::colNear(int x, int y0, int y1, Rgb want, int tol) const {
    for (int y = y0; y < y1; ++y) { if (!near(x, y, want, tol)) { return false; } }
    return y1 > y0;
}
int Frame::firstRowNear(int x, int y0, int y1, Rgb want, int tol) const {
    for (int y = y0; y < y1; ++y) { if (near(x, y, want, tol)) { return y; } }
    return -1;
}
int Frame::firstColNear(int y, int x0, int x1, Rgb want, int tol) const {
    for (int x = x0; x < x1; ++x) { if (near(x, y, want, tol)) { return x; } }
    return -1;
}

bool Frame::savePng(const std::string& path) const {
    std::vector<unsigned char> out;
    const auto sink = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* b = static_cast<const unsigned char*>(data);
        v->insert(v->end(), b, b + size);
    };
    if (stbi_write_png_to_func(sink, &out, static_cast<int>(width), static_cast<int>(height), 3, px.data(), static_cast<int>(width) * 3) == 0) {
        std::fprintf(stderr, "render harness: could not encode %s\n", path.c_str());
        return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { std::fprintf(stderr, "render harness: could not open %s for writing\n", path.c_str()); return false; }
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "render harness: short write to %s\n", path.c_str()); }
    return ok;
}

void installHeadlessPlatform() {
    if (g_installed) { return; }
    slint::platform::set_platform(std::make_unique<HeadlessPlatform>());
    g_installed = true;
}

Frame render(slint::Window& window, std::uint32_t width, std::uint32_t height) {
    HeadlessWindow* hw = HeadlessWindow::forWindow(window);
    if (hw == nullptr) {
        std::fprintf(stderr, "render harness: the window was not created by the headless platform (call installHeadlessPlatform() first)\n");
        return {};
    }
    hw->resize(width, height);
    window.show();
    hw->resize(width, height);  // show() may have asked for the component's own size; the test's size wins
    runPendingTasks();
    return hw->paint();
}

void settle(std::chrono::milliseconds advance) {
    const auto until = std::chrono::steady_clock::now() + advance;
    do {
        runPendingTasks();
        if (advance.count() > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    } while (std::chrono::steady_clock::now() < until);
    runPendingTasks();
}

void moveTo(slint::Window& w, float x, float y) { w.dispatch_pointer_move_event(slint::LogicalPosition({x, y})); runPendingTasks(); }
void press(slint::Window& w, float x, float y, slint::PointerEventButton b) { w.dispatch_pointer_press_event(slint::LogicalPosition({x, y}), b); runPendingTasks(); }
void release(slint::Window& w, float x, float y, slint::PointerEventButton b) { w.dispatch_pointer_release_event(slint::LogicalPosition({x, y}), b); runPendingTasks(); }
void click(slint::Window& w, float x, float y, slint::PointerEventButton b) { moveTo(w, x, y); press(w, x, y, b); release(w, x, y, b); }
void scroll(slint::Window& w, float x, float y, float dx, float dy) { w.dispatch_pointer_scroll_event(slint::LogicalPosition({x, y}), dx, dy); runPendingTasks(); }
void keyDown(slint::Window& w, std::u8string_view t) { w.dispatch_key_press_event(slint::SharedString(t)); runPendingTasks(); }
void keyUp(slint::Window& w, std::u8string_view t) { w.dispatch_key_release_event(slint::SharedString(t)); runPendingTasks(); }
void keyTap(slint::Window& w, std::u8string_view t) { keyDown(w, t); keyUp(w, t); }
void ctrlTap(slint::Window& w, std::u8string_view t) {
    keyDown(w, slint::platform::key_codes::Control);
    keyTap(w, t);
    keyUp(w, slint::platform::key_codes::Control);
}

std::string shotDir() {
    std::string dir;
    if (const char* e = std::getenv("FOXSDR_SLINT_SHOTS"); e != nullptr && *e != '\0') { dir = e; }
    else { dir = "slint-shots"; }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (dir.back() != '/' && dir.back() != '\\') { dir += '/'; }
    return dir;
}

}  // namespace foxsdr::slinttest
