// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "screen_first.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "gui/fonts.hpp"
#include "gui/theme.hpp"

namespace cascade::platform::android {
namespace {

namespace theme = cascade::gui::theme;
namespace fonts = cascade::gui::fonts;

// The well the trace sits in. Taken from gui/spectrum_view.cpp's own
// background constant rather than invented here, so the phone's display well
// is the same near-black green the desktop's is.
constexpr ImU32 kWellBg = IM_COL32(5, 10, 6, 255);
constexpr ImU32 kGridLine = IM_COL32(255, 255, 255, 26);

// The dB range the display covers. The estimator floors at -200 dB and a
// -90 dB noise floor with tones between -10 and -40 dB sits comfortably inside
// this, so the trace fills the well instead of hugging one edge.
constexpr float kTopDb = 0.0f;
constexpr float kBottomDb = -120.0f;

float dbToY(float db, float top, float height) {
    const float t = (kTopDb - db) / (kTopDb - kBottomDb);
    return top + std::clamp(t, 0.0f, 1.0f) * height;
}

}  // namespace

FirstScreen::FirstScreen() {
    // The scene the generator plays. Three tones at levels a receiver would
    // actually see, spread across the band so the trace has shape, plus a
    // noise floor so it looks like a spectrum rather than a line drawing.
    // Deterministic by construction (SigGen is fixed-seed), which means a
    // screenshot from one phone can be compared with one from another.
    toneSlots_[0] = {-620.0e3, -14.0f, true};
    toneSlots_[1] = {-150.0e3, -28.0f, true};
    toneSlots_[2] = {310.0e3, -8.0f, true};
    toneSlots_[3] = {745.0e3, -36.0f, true};

    source::SigGen& g = gen_.sigGen();
    for (int i = 0; i < 4; ++i) {
        const ToneSlot& t = toneSlots_[static_cast<std::size_t>(i)];
        g.setTone(i, t.freqHz, t.amplitudeDb);
    }
    g.setNoiseFloorDb(-72.0f);

    // Smoothing in the linear power domain, as on the desktop. 0.35 settles
    // the noise floor without making a tone appear slowly.
    estimator_.setAlpha(0.35f);

    bins_.assign(kFftSize, kBottomDb);
    drawBins_.assign(kFftSize, kBottomDb);
}

FirstScreen::~FirstScreen() { setRunning(false); }

void FirstScreen::setRunning(bool running) {
    if (running == run_.load(std::memory_order_relaxed)) { return; }
    if (running) {
        gen_.start();
        run_.store(true, std::memory_order_relaxed);
        worker_ = std::thread([this] { workerBody(); });
    } else {
        run_.store(false, std::memory_order_relaxed);
        if (worker_.joinable()) { worker_.join(); }
        gen_.stop();
        estimator_.reset();
    }
}

void FirstScreen::workerBody() {
    using clock = std::chrono::steady_clock;

    std::vector<std::complex<float>> block(kFftSize);
    std::vector<float> db(kFftSize);

    // THE GENERATOR IS FREE-RUNNING AND THE CALLER PACES IT. That is
    // SigGenSource's documented contract (selfPaced() == false): read() always
    // fills the whole request immediately, so without this the worker would
    // spin a core flat out producing spectra nobody can see. The desktop's
    // Pipeline paces the identical source the identical way.
    const auto period = std::chrono::nanoseconds(
        static_cast<long long>(1.0e9 * static_cast<double>(kFftSize) / kSampleRateHz));
    auto next = clock::now();

    while (run_.load(std::memory_order_relaxed)) {
        gen_.read(block.data(), kFftSize);
        estimator_.process(block.data(), db.data());
        {
            std::lock_guard<std::mutex> lk(binsMutex_);
            bins_ = db;  // reuses capacity after the first block
            seq_ += 1;
        }
        blocks_.fetch_add(1, std::memory_order_relaxed);

        next += period;
        const auto now = clock::now();
        if (now > next + std::chrono::milliseconds(250)) {
            next = now;  // resync after a stall instead of bursting
        }
        std::this_thread::sleep_until(next);
    }
}

void FirstScreen::draw(const ShellStatus& status) {
    {
        std::lock_guard<std::mutex> lk(binsMutex_);
        drawBins_ = bins_;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    // No move, no resize, no collapse: there is one window and it is the
    // screen. A phone has nowhere to move it to.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("FoxSDR", nullptr, flags);

    const float scale = status.densityScale;

    // FONT SIZES ARE PUSHED UNSCALED, ON PURPOSE. The density factor is
    // applied once, globally, through ImGuiStyle::FontScaleDpi (see
    // frame.cpp's applyScaledTheme); ImGui multiplies the size handed to
    // PushFont by it. Multiplying here as well would square the density and
    // put 60-pixel captions on a 420 dpi phone. Draw-list geometry below is a
    // different matter - those are raw pixels and do take the scale.
    ImGui::PushFont(fonts::legend(), fonts::kLegendSize);
    ImGui::TextColored(theme::vec(theme::kCream), "SIGNAL GENERATOR  2.048 MS/s  1024-POINT BLACKMAN-HARRIS");
    ImGui::PopFont();

    // --- the well ------------------------------------------------------------
    const ImVec2 wellMin = ImGui::GetCursorScreenPos();
    // What the plate below the well needs: a row of tone keys (a button is the
    // font height plus two lots of frame padding) and three lines of text.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineH = fonts::kUiSize * scale + style.ItemSpacing.y;
    const float reserved = lineH * 3.0f + (lineH + style.FramePadding.y * 2.0f);
    const float wellW = ImGui::GetContentRegionAvail().x;
    const float wellH = std::max(80.0f * scale, ImGui::GetContentRegionAvail().y - reserved);
    const ImVec2 wellMax(wellMin.x + wellW, wellMin.y + wellH);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(wellMin, wellMax, kWellBg, theme::kPanelRounding);
    dl->AddRect(wellMin, wellMax, theme::kBrassDark, theme::kPanelRounding, 0, theme::kHairline);

    // Horizontal dB rules every 20 dB, with an engraved figure on each.
    ImGui::PushFont(fonts::reading(), fonts::kTinySize);
    for (float db = kTopDb; db >= kBottomDb; db -= 20.0f) {
        const float y = dbToY(db, wellMin.y, wellH);
        dl->AddLine(ImVec2(wellMin.x, y), ImVec2(wellMax.x, y), kGridLine, 1.0f);
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f", static_cast<double>(db));
        dl->AddText(ImVec2(wellMin.x + 4.0f * scale, y + 2.0f * scale),
                    theme::withAlpha(theme::kInkFaint, 0.9f), label);
    }
    // Vertical rules every 500 kHz either side of centre.
    for (int k = -2; k <= 2; ++k) {
        const float t = 0.5f + static_cast<float>(k) * 500.0e3f / static_cast<float>(kSampleRateHz);
        const float x = wellMin.x + t * wellW;
        dl->AddLine(ImVec2(x, wellMin.y), ImVec2(x, wellMax.y), kGridLine, 1.0f);
    }
    ImGui::PopFont();

    // --- the trace -----------------------------------------------------------
    //
    // One screen column per pixel, taking the PEAK of the bins that fall in it.
    // A 1024-bin spectrum on a 1080-pixel-wide phone is close to one bin per
    // column, but the same code has to be right when the well is 400 px wide in
    // portrait, and a decimation that samples instead of peak-holding makes
    // narrow signals flicker in and out as the window moves.
    const int columns = std::max(2, static_cast<int>(wellW));
    points_.resize(static_cast<std::size_t>(columns));
    const std::size_t bins = drawBins_.size();
    for (int c = 0; c < columns; ++c) {
        const std::size_t lo = static_cast<std::size_t>(
            static_cast<double>(c) * static_cast<double>(bins) / static_cast<double>(columns));
        std::size_t hi = static_cast<std::size_t>(static_cast<double>(c + 1) *
                                                  static_cast<double>(bins) /
                                                  static_cast<double>(columns));
        hi = std::clamp(hi, lo + 1, bins);
        float peak = kBottomDb;
        for (std::size_t i = lo; i < hi; ++i) { peak = std::max(peak, drawBins_[i]); }
        const float x = wellMin.x + (static_cast<float>(c) + 0.5f) * wellW /
                                        static_cast<float>(columns);
        points_[static_cast<std::size_t>(c)] = ImVec2(x, dbToY(peak, wellMin.y, wellH));
    }
    dl->PushClipRect(wellMin, wellMax, true);
    dl->AddPolyline(points_.data(), columns, theme::kPhosphor, ImDrawFlags_None,
                    std::max(1.0f, 1.0f * scale));
    dl->PopClipRect();

    ImGui::Dummy(ImVec2(wellW, wellH));

    // --- the plate -----------------------------------------------------------
    //
    // What a person holding the phone needs in order to say whether the shell
    // is working, and what to put in a bug report if it is not. Everything here
    // is measured on this device rather than assumed: the GL strings come from
    // the live context, the ABI from the compiler, the density from the
    // activity's configuration.
    // --- the tone keys -------------------------------------------------------
    ImGui::PushFont(fonts::ui(), fonts::kUiSize);
    for (int i = 0; i < 4; ++i) {
        ToneSlot& slot = toneSlots_[static_cast<std::size_t>(i)];
        if (i > 0) { ImGui::SameLine(); }
        char label[32];
        std::snprintf(label, sizeof(label), "%s %+.0f kHz", slot.on ? "ON " : "OFF",
                      slot.freqHz / 1000.0);
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              theme::vec(slot.on ? theme::kBrassMid : theme::kBrassDark));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              theme::vec(slot.on ? theme::kIvory : theme::kInkFaint));
        if (ImGui::Button(label)) {
            slot.on = !slot.on;
            if (slot.on) {
                gen_.sigGen().setTone(i, slot.freqHz, slot.amplitudeDb);
            } else {
                gen_.sigGen().clearTone(i);
            }
        }
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }
    ImGui::PopFont();

    ImGui::PushFont(fonts::ui(), fonts::kUiSize);
    ImGui::TextColored(theme::good(), "%llu blocks  %s",
                       static_cast<unsigned long long>(blocks_.load(std::memory_order_relaxed)),
                       running() ? "running" : "paused");
    ImGui::PopFont();

    ImGui::PushFont(fonts::reading(), fonts::kTinySize);
    ImGui::TextColored(theme::vec(theme::kInkMuted), "%.2f ms/frame   %dx%d px   %d dpi (x%.2f)",
                       static_cast<double>(status.frameMs), status.surfaceWidth,
                       status.surfaceHeight, status.densityDpi,
                       static_cast<double>(status.densityScale));
    ImGui::TextColored(theme::vec(theme::kInkMuted), "%s   %s   %s   FoxSDR %s", status.abi,
                       status.glVersion, status.glRenderer, status.appVersion);
    ImGui::PopFont();

    ImGui::End();
}

}  // namespace cascade::platform::android
