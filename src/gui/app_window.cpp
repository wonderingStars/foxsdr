// GLFW + Dear ImGui application shell — implementation.
//
// SPDX-License-Identifier: MIT
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// After the ImGui backends: glfw3.h pulls in GL/gl.h on Windows, which the
// opengl3 backend must not see before its own embedded loader.
#include <GLFW/glfw3.h>

#include "core/version.hpp"
#include "gui/spectrum_view.hpp"
#include "gui/waterfall_view.hpp"

namespace cascade::gui {

namespace {

constexpr float kMenuWidth = 260.0f;  // left column width per parity spec

// Pipeline configuration. 2 MS/s at FFT 1024 publishes far more frames than
// the GUI's ~60 fps polls; the latest-frame slot in Pipeline absorbs the
// difference by design. Alpha 0.5 smooths the trace without visible lag.
constexpr double kSampleRateHz = 2'000'000.0;
constexpr std::size_t kFftSize = 1024;
constexpr float kAveragingAlpha = 0.5f;
// Waterfall history depth: 512 lines at one line per GUI frame is ~8.5 s of
// scroll-back, plenty for a demo signal while keeping the texture small.
constexpr int kWaterfallHistory = 512;

// The Display sliders keep at least this many dB between min and max: a
// thinner span renders as a near-solid waterfall and a wall-to-wall trace,
// and a zero/inverted span would degrade to the widgets' flat-line fallback.
constexpr float kMinDbSpan = 10.0f;

// GLFW reports failures through this callback *before* glfwInit/CreateWindow
// return their error codes, so printing here is what gives the user an actual
// reason instead of a bare "init failed".
void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "cascade: GLFW error %d: %s\n", code,
                 description ? description : "(no description)");
}

}  // namespace

AppWindow::AppWindow()
    : pipeline_(cascade::core::Pipeline::Config{kSampleRateHz, kFftSize, kAveragingAlpha,
                                                /*audioEnabled=*/true}),
      spectrum_(std::make_unique<SpectrumView>()),
      waterfall_(std::make_unique<WaterfallView>(static_cast<int>(kFftSize), kWaterfallHistory)) {
    // Demo signal until real sources land (P4): two tones at distinct offsets
    // and levels over a noise floor, so both display axes are visibly
    // exercised — frequency (two peaks left and right of center) and
    // amplitude (different heights / waterfall colors).
    cascade::source::SigGen& gen = pipeline_.sigGen();
    gen.setTone(0, 300000.0, -30.0f);
    gen.setTone(1, -500000.0, -45.0f);
    gen.setNoiseFloorDb(-90.0f);
    spectrum_->setRange(dbMin_, dbMax_);

    // Park the VFO on demo tone 0 so the receiver is tuned to something from
    // the first Play: WFM (the default mode) renders an unmodulated carrier
    // as near-silence, and switching to CW yields the 700 Hz sidetone.
    pipeline_.setVfoOffsetHz(1000.0 * static_cast<double>(vfoOffsetKhz_));
    pipeline_.audio().setVolume(volume_);
    devices_ = pipeline_.audio().listOutputDevices();
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        if (devices_[static_cast<std::size_t>(i)].isDefault) { deviceIndex_ = i; }
    }
    if (deviceIndex_ < 0 && !devices_.empty()) { deviceIndex_ = 0; }
}

AppWindow::~AppWindow() = default;

int AppWindow::run(int frames) {
    glfwSetErrorCallback(&glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "cascade: glfwInit failed\n");
        return 1;
    }

    const std::string title =
        std::string(cascade::appName()) + " " + cascade::versionString();
    GLFWwindow* window = glfwCreateWindow(1280, 720, title.c_str(), nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "cascade: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync: the GUI thread paces itself off the display

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Layout is fully code-driven; a stray imgui.ini next to the exe would
    // silently override it and make runs non-reproducible.
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        std::fprintf(stderr, "cascade: ImGui GLFW backend init failed\n");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        std::fprintf(stderr, "cascade: ImGui OpenGL3 backend init failed\n");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // A previous run() tore the waterfall down with its GL context (see the
    // teardown below); re-create it against the new context so run() stays
    // callable more than once.
    if (!waterfall_) {
        waterfall_ = std::make_unique<WaterfallView>(static_cast<int>(kFftSize),
                                                     kWaterfallHistory);
    }

    int rendered = 0;
    while (!glfwWindowShouldClose(window)) {
        // Exact-count contract: check before rendering so --frames N produces
        // N frames, and --frames 0 produces none.
        if (frames >= 0 && rendered >= frames) { break; }

        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawUi();

        ImGui::Render();
        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);
        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        ++rendered;
    }

    // Closing the window while receiving must not leave DSP threads pacing a
    // dead display; stop before teardown so the join happens while the object
    // graph is still fully alive.
    pipeline_.stop();

    // The waterfall owns a GL texture whose deletion requires the creating
    // context to be current. AppWindow outlives that context (main() destroys
    // it after run() returns), so the view is destroyed explicitly here, not
    // left to ~AppWindow.
    waterfall_.reset();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    // Printed on every clean exit: this is what makes the exact-count half of
    // the --frames contract externally observable — app_smoke matches
    // "rendered 3 frames" via PASS_REGULAR_EXPRESSION, so an off-by-one in the
    // frame bound goes red instead of shipping silently.
    std::printf("cascade: rendered %d frames\n", rendered);
    return 0;
}

void AppWindow::drawUi() {
    // One borderless window pinned to the viewport: the app IS the layout, so
    // nothing is movable or collapsible at this level.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags rootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##cascade_root", nullptr, rootFlags);

    drawToolbar();
    ImGui::Separator();

    ImGui::BeginChild("##menu_column", ImVec2(kMenuWidth, 0.0f), ImGuiChildFlags_None);
    drawMenuColumn();
    ImGui::EndChild();

    ImGui::SameLine();

    // The center area owns its scrolling (none): the spectrum/waterfall pair
    // always fills whatever space the splitter hands it.
    ImGui::BeginChild("##center", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawCenterPanels();
    ImGui::EndChild();

    ImGui::End();
}

void AppWindow::drawToolbar() {
    // The label reads the pipeline, not a local flag, so the button can never
    // disagree with the actual thread state.
    const bool running = pipeline_.running();
    if (ImGui::Button(running ? "Stop" : "Play", ImVec2(64.0f, 0.0f))) {
        if (running) {
            // Joins both pipeline threads; they exit within ~10 ms, which is
            // an acceptable one-off hitch on the GUI thread for a Stop click.
            pipeline_.stop();
        } else {
            pipeline_.start();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("##volume", &volume_, 0.0f, 1.0f, "Vol %.2f")) {
        pipeline_.audio().setVolume(volume_);
    }
    ImGui::SameLine(0.0f, 24.0f);
    drawFrequencyReadout();
}

void AppWindow::drawFrequencyReadout() {
    // Fixed 10-digit field grouped in thousands ("0.100.000.000" at 100 MHz).
    // The field width is constant so digits never shift as the tuned
    // frequency changes; the zeros (and separators) ahead of the first
    // significant digit are dimmed so the eye reads only the live value.
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%010llu", frequencyHz_);

    const ImVec4 bright = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImVec4 dim = bright;
    dim.w *= 0.25f;

    // Large monospace look: scale the default font up. Base size (not
    // GetFontSize()) so global scale factors are not applied twice.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.2f);
    bool significant = false;
    for (int i = 0; i < 10; ++i) {
        if (digits[i] != '0') { significant = true; }
        // Separators after digits 0, 3 and 6 group the field d.ddd.ddd.ddd; a
        // separator inherits the dim state of the run it terminates.
        const bool sepAfter = (i == 0 || i == 3 || i == 6);
        const char cell[3] = {digits[i], sepAfter ? '.' : '\0', '\0'};
        ImGui::PushStyleColor(ImGuiCol_Text, significant ? bright : dim);
        ImGui::TextUnformatted(cell);
        ImGui::PopStyleColor();
        if (i != 9) { ImGui::SameLine(0.0f, 0.0f); }
    }
    ImGui::PopFont();
}

void AppWindow::drawMenuColumn() {
    if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Signal generator (demo tones)");
    }
    if (ImGui::CollapsingHeader("Radio", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kModes[] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
        // Button order is the SDR++ parity layout; the enum order differs
        // (LSB before CW), so the mapping is by name, never by index.
        static constexpr cascade::dsp::DemodMode kModeMap[] = {
            cascade::dsp::DemodMode::NFM, cascade::dsp::DemodMode::WFM,
            cascade::dsp::DemodMode::AM,  cascade::dsp::DemodMode::DSB,
            cascade::dsp::DemodMode::USB, cascade::dsp::DemodMode::CW,
            cascade::dsp::DemodMode::LSB, cascade::dsp::DemodMode::RAW};
        // Bandwidth options offered in the combo, widest first.
        static const char* kBwLabels[] = {"200k", "150k", "12.5k", "10k", "6k", "3k"};
        static constexpr double kBwHz[] = {200000.0, 150000.0, 12500.0,
                                           10000.0,  6000.0,   3000.0};
        // Per-mode default bandwidth (index into kBwHz), applied when a mode
        // button is clicked; the combo below still allows any override.
        // Rationale: WFM broadcast channel 150k; NFM two-way channel 12.5k;
        // AM/DSB broadcast channel ~10k (both sidebands); SSB/CW voice/keying
        // fits in 3k; RAW passes the full 200k channel for diagnostics.
        static constexpr int kModeDefaultBw[] = {2, 1, 3, 3, 5, 5, 5, 0};
        constexpr int kColumns = 4;
        const float cellWidth =
            (ImGui::GetContentRegionAvail().x -
             static_cast<float>(kColumns - 1) * ImGui::GetStyle().ItemSpacing.x) /
            static_cast<float>(kColumns);
        for (int i = 0; i < 8; ++i) {
            if (i % kColumns != 0) { ImGui::SameLine(); }
            const bool selected = (i == modeIndex_);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button(kModes[i], ImVec2(cellWidth, 0.0f))) {
                modeIndex_ = i;
                pipeline_.setDemodMode(kModeMap[i]);
                bandwidthIndex_ = kModeDefaultBw[i];
                pipeline_.setVfoBandwidthHz(kBwHz[bandwidthIndex_]);
            }
            if (selected) { ImGui::PopStyleColor(); }
        }

        // VFO offset from the input center. The slider edits kHz (a 1 Hz-per-
        // pixel float slider over a 1 MHz span would be unusable); the
        // pipeline takes Hz.
        if (ImGui::SliderFloat("VFO", &vfoOffsetKhz_, -500.0f, 500.0f, "%.0f kHz")) {
            pipeline_.setVfoOffsetHz(1000.0 * static_cast<double>(vfoOffsetKhz_));
        }
        if (ImGui::Combo("Bandwidth", &bandwidthIndex_, kBwLabels,
                         static_cast<int>(sizeof(kBwLabels) / sizeof(kBwLabels[0])))) {
            pipeline_.setVfoBandwidthHz(kBwHz[bandwidthIndex_]);
        }
        if (ImGui::SliderFloat("Squelch", &squelchDb_, -120.0f, 0.0f, "%.0f dB")) {
            pipeline_.setSquelchDb(squelchDb_);
        }

        // S-meter: channel power mapped over the squelch slider's own
        // [-120, 0] dB span, so the bar and the threshold share a scale.
        const float sDb = pipeline_.signalPowerDb();
        float frac = (sDb + 120.0f) / 120.0f;
        frac = std::clamp(frac, 0.0f, 1.0f);
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.1f dB", static_cast<double>(sDb));
        ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), overlay);
    }
    if (ImGui::CollapsingHeader("Sinks", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (devices_.empty()) {
            ImGui::TextDisabled("No audio output devices");
        } else if (ImGui::BeginCombo(
                       "Device",
                       devices_[static_cast<std::size_t>(deviceIndex_)].name.c_str())) {
            for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
                const auto& dev = devices_[static_cast<std::size_t>(i)];
                // PushID: PortAudio lists one physical device once per host
                // API with an identical name; the index keeps ImGui IDs unique.
                ImGui::PushID(i);
                if (ImGui::Selectable(dev.name.c_str(), i == deviceIndex_)) {
                    deviceIndex_ = i;
                    // Re-open on change: open() closes the old stream first,
                    // so this is the whole device-switch operation.
                    pipeline_.audio().open(dev.index,
                                           cascade::core::Pipeline::kAudioRateHz);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        // One shared dB range drives both the spectrum axis and the waterfall
        // colormap so the two panels always agree on what "hot" means.
        const bool minChanged =
            ImGui::SliderFloat("Min dB", &dbMin_, -160.0f, -20.0f, "%.0f");
        const bool maxChanged =
            ImGui::SliderFloat("Max dB", &dbMax_, -100.0f, 20.0f, "%.0f");
        // Keep at least kMinDbSpan between the endpoints by pushing back the
        // slider the user is actually dragging — correcting the *other* value
        // would make an untouched slider jump under the user's eyes.
        if (minChanged && dbMin_ > dbMax_ - kMinDbSpan) { dbMin_ = dbMax_ - kMinDbSpan; }
        if (maxChanged && dbMax_ < dbMin_ + kMinDbSpan) { dbMax_ = dbMin_ + kMinDbSpan; }
        if (minChanged || maxChanged) { spectrum_->setRange(dbMin_, dbMax_); }
    }
}

void AppWindow::drawCenterPanels() {
    // Poll for a new spectrum frame every GUI frame. getLatestFrame compares
    // against lastFrame_.seq, so this is one mutex lock returning false when
    // nothing new arrived — cheap enough to run unconditionally, which also
    // catches a frame published between the last poll and a Stop click.
    if (pipeline_.getLatestFrame(lastFrame_)) {
        // One waterfall line per *new* frame (not per GUI frame): duplicate
        // lines would fake scroll speed while the pipeline is stalled.
        waterfall_->addLine(lastFrame_.dbBins.data(),
                            static_cast<int>(lastFrame_.dbBins.size()), dbMin_, dbMax_);
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterThickness = 6.0f;
    const float usable = avail.y - splitterThickness;
    // A squeezed window can drive the region to zero; drawing into negative
    // sizes asserts inside ImGui, so just skip the panels that frame.
    if (usable < 40.0f || avail.x < 40.0f) { return; }

    const float spectrumHeight = splitRatio_ * usable;
    // Before the first frame lastFrame_.dbBins is empty; SpectrumView renders
    // the background + grid for null bins, which is the wanted idle look.
    const float* bins = lastFrame_.dbBins.empty() ? nullptr : lastFrame_.dbBins.data();
    spectrum_->draw(bins, static_cast<int>(lastFrame_.dbBins.size()), avail.x, spectrumHeight);

    // Splitter: an invisible button whose vertical drag re-balances the
    // spectrum/waterfall split. Ratio (not pixels) so a window resize keeps
    // the user's proportions.
    ImGui::InvisibleButton("##vsplitter", ImVec2(avail.x, splitterThickness));
    if (ImGui::IsItemActive()) {
        splitRatio_ = std::clamp(splitRatio_ + ImGui::GetIO().MouseDelta.y / usable,
                                 0.1f, 0.9f);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    ImGuiCol gripColor = ImGuiCol_Separator;
    if (ImGui::IsItemActive()) {
        gripColor = ImGuiCol_SeparatorActive;
    } else if (ImGui::IsItemHovered()) {
        gripColor = ImGuiCol_SeparatorHovered;
    }
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(gripColor));

    waterfall_->draw(avail.x, usable - spectrumHeight);
}

}  // namespace cascade::gui
