// GLFW + Dear ImGui application shell — implementation.
//
// SPDX-License-Identifier: MIT
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// After the ImGui backends: glfw3.h pulls in GL/gl.h on Windows, which the
// opengl3 backend must not see before its own embedded loader.
#include <GLFW/glfw3.h>

#include "core/version.hpp"
#include "gui/spectrum_view.hpp"
#include "gui/waterfall_view.hpp"
#include "source/iq_file_source.hpp"

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

// Source-menu error color: readable red on the dark theme, used for
// open()/setter failures surfaced from IqSource::lastError().
constexpr ImVec4 kErrorRed{1.0f, 0.35f, 0.35f, 1.0f};

// Soapy sample-rate choices. 2 MS/s (index 1) is the default because it is
// the rate the DSP chain was configured at (kSampleRateHz); the other rates
// are offered per spec, with the ACTUAL device readback displayed alongside
// — frequency-axis labeling that would make the other rates fully coherent
// is P5 work.
constexpr const char* kSoapyRateLabels[] = {"1 MS/s", "2 MS/s", "4 MS/s", "8 MS/s"};
constexpr double kSoapyRateHz[] = {1.0e6, 2.0e6, 4.0e6, 8.0e6};
constexpr int kSoapyRateCount = 4;
constexpr int kSoapyRateDefaultIndex = 1;  // 2 MS/s

// Gain sliders display 0..60 dB for every element (documented simplification:
// true per-element ranges vary per driver — the B200's PGA spans 0..76 dB —
// but SoapySDR clamps out-of-range requests, so a fixed display range costs
// only the top of the dial, not correctness). 30 dB start: mid-dial, and the
// value is PUSHED to the device at open so slider and hardware agree from the
// first frame — there is no per-element gain getter on SoapySource to read
// the boot value back from.
constexpr float kSoapyGainMinDb = 0.0f;
constexpr float kSoapyGainMaxDb = 60.0f;
constexpr float kSoapyGainDefaultDb = 30.0f;

// The frequency readout's 10 digit places, most significant first
// (digit i steps by kPlaceHz[i] on a wheel tick over it).
constexpr double kPlaceHz[10] = {1e9, 1e8, 1e7, 1e6, 1e5, 1e4, 1e3, 1e2, 1e1, 1e0};

// Largest value the fixed 10-digit field can show; the display clamps here
// (a device readback cannot exceed it in practice — 9.99 GHz).
constexpr double kMaxDisplayHz = 9999999999.0;

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
    // One enumeration up front so the Source combo is populated from the
    // first frame; the Refresh button re-runs it for hot-plug. enumerate()
    // never throws and is simply empty on a machine with no vendor modules.
    soapyDevices_ = cascade::source::SoapySource::enumerate();
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
    //
    // The value shown is ALWAYS the active source's centerFrequencyHz()
    // readback — nominal for the generator/file, real device readback for
    // Soapy — so the display can never disagree with the hardware. Tuning:
    // the mouse wheel over a digit steps the frequency by that digit's place
    // value (clamped at 0) through setCenterFrequencyHz(); the change shows
    // up via the same readback on the next frame. activeSource() is a
    // GUI/control-thread call per the IqSource contract, and this IS that
    // thread — the same one that performs source swaps.
    cascade::source::IqSource& src = pipeline_.activeSource();
    const double hz = std::max(0.0, src.centerFrequencyHz());

    char digits[16];
    std::snprintf(digits, sizeof(digits), "%010llu",
                  static_cast<unsigned long long>(
                      std::llround(std::min(hz, kMaxDisplayHz))));

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

        // Per-digit wheel tuning. A trailing separator belongs to the digit
        // cell it follows, so hovering it tunes that digit — the natural
        // reading. Fractional wheel deltas (touchpads) below one notch still
        // step once, in the delta's direction.
        if (ImGui::IsItemHovered()) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                double ticks = static_cast<double>(static_cast<long long>(wheel));
                if (ticks == 0.0) { ticks = (wheel > 0.0f) ? 1.0 : -1.0; }
                const double next = std::max(0.0, hz + ticks * kPlaceHz[i]);
                // Failure (e.g. a tune the driver refuses) needs no handling
                // here: the display follows the readback, which won't move.
                src.setCenterFrequencyHz(next);
            }
        }
        if (i != 9) { ImGui::SameLine(0.0f, 0.0f); }
    }
    ImGui::PopFont();
}

void AppWindow::drawMenuColumn() {
    // The sections scroll inside an inner child sized to leave room for the
    // status footer, so the footer stays pinned to the bottom of the column
    // regardless of how many sections are open.
    const float footerHeight = 2.0f * ImGui::GetTextLineHeightWithSpacing() +
                               ImGui::GetStyle().ItemSpacing.y + 4.0f;
    ImGui::BeginChild("##menu_sections", ImVec2(0.0f, -footerHeight),
                      ImGuiChildFlags_None);
    drawSourceSection();
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
    ImGui::EndChild();

    // Status footer: active source identity, its sample rate (device readback
    // for Soapy, nominal otherwise), and the audio sink's cumulative underrun
    // count — the buffer-health readout the parity spec's status bar calls
    // for. Two clipped lines rather than one wrapped one: a long device name
    // must not push the numbers out of the reserved footer space.
    ImGui::Separator();
    cascade::source::IqSource& src = pipeline_.activeSource();
    ImGui::TextUnformatted(src.name());
    ImGui::Text("%.4g MS/s | underruns %llu", src.sampleRateHz() / 1.0e6,
                static_cast<unsigned long long>(pipeline_.audio().underruns()));
}

void AppWindow::drawSourceSection() {
    if (!ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) { return; }

    // Row label for a combo index; -1 (active device dropped by a Refresh)
    // falls back to the live source name so the preview is never a lie.
    const auto rowLabel = [this](int idx) -> const char* {
        if (idx == 0) { return "Signal generator"; }
        if (idx == 1) { return "IQ file"; }
        const int d = idx - 2;
        if (d >= 0 && d < static_cast<int>(soapyDevices_.size())) {
            return soapyDevices_[static_cast<std::size_t>(d)].label.c_str();
        }
        return pipeline_.activeSourceName();
    };

    const int rowCount = 2 + static_cast<int>(soapyDevices_.size());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##source_select", rowLabel(sourceSel_))) {
        for (int i = 0; i < rowCount; ++i) {
            // PushID: two identical devices (same model, no serial in the
            // label) must still be distinct rows.
            ImGui::PushID(i);
            if (ImGui::Selectable(rowLabel(i), i == sourceSel_)) { selectSource(i); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Refresh")) {
        soapyDevices_ = cascade::source::SoapySource::enumerate();
        if (sourceSel_ >= 2 || sourceSel_ < 0) {
            // Re-find the open device by its args (labels can repeat); if it
            // vanished from the scan the device stays open and selected, and
            // the preview falls back to its live name via rowLabel(-1).
            sourceSel_ = -1;
            for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
                if (soapy_ != nullptr && soapyDevices_[i].args == soapyArgs_) {
                    sourceSel_ = 2 + static_cast<int>(i);
                }
            }
        }
    }

    // IQ file controls, shown while the combo sits on "IQ file". The pipeline
    // keeps its current source until Open succeeds: a failed open constructs
    // and destroys a throwaway IqFileSource without ever touching the
    // pipeline, so there is nothing to roll back.
    if (sourceSel_ == 1) {
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("##iq_path", iqPath_, sizeof(iqPath_));
        ImGui::SameLine();
        if (ImGui::Button("Open")) {
            auto file = std::make_unique<cascade::source::IqFileSource>();
            if (!file->open(iqPath_)) {
                sourceError_ = file->lastError();
            } else {
                // Carry the displayed frequency over: a file's center is
                // nominal anyway, and a readout that jumps to 0 on source
                // switch would read as a tuning bug.
                file->setCenterFrequencyHz(
                    pipeline_.activeSource().centerFrequencyHz());
                soapy_ = nullptr;  // before setSource destroys a live Soapy
                soapyArgs_.clear();
                sourceError_.clear();
                pipeline_.setSource(std::move(file));
            }
        }
    }

    // Soapy device panel, shown while a Soapy source is INSTALLED in the
    // pipeline (soapy_ tracks setSource, not the combo row, so the panel
    // stays correct while e.g. the combo previews "IQ file" pre-Open).
    if (soapy_ != nullptr) {
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("Rate", &soapyRateIndex_, kSoapyRateLabels, kSoapyRateCount)) {
            if (!soapy_->setSampleRateHz(kSoapyRateHz[soapyRateIndex_])) {
                sourceError_ = soapy_->lastError();
            }
        }
        // Actual readback beside the request: drivers coerce, the DSP chain
        // and the user must both see the rate the hardware really runs at.
        ImGui::SameLine();
        ImGui::Text("actual %.4g MS/s", soapy_->sampleRateHz() / 1.0e6);

        if (soapyAgcSupported_) {
            if (ImGui::Checkbox("Auto gain", &soapyAgc_)) {
                if (!soapy_->setAutoGain(soapyAgc_)) {
                    sourceError_ = soapy_->lastError();
                    soapyAgc_ = !soapyAgc_;  // the device did not change mode
                }
            }
        } else {
            ImGui::TextDisabled("Auto gain: not supported");
        }

        // Manual gain sliders are meaningless while hardware AGC drives the
        // stages, so grey them out rather than letting them silently fight.
        ImGui::BeginDisabled(soapyAgc_);
        for (std::size_t i = 0; i < soapyGainNames_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SliderFloat(soapyGainNames_[i].c_str(), &soapyGainsDb_[i],
                                   kSoapyGainMinDb, kSoapyGainMaxDb, "%.0f dB")) {
                if (!soapy_->setGainDb(soapyGainNames_[i],
                                       static_cast<double>(soapyGainsDb_[i]))) {
                    sourceError_ = soapy_->lastError();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }

    if (!sourceError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", sourceError_.c_str());
        ImGui::PopStyleColor();
    }
}

void AppWindow::selectSource(int idx) {
    if (idx == sourceSel_) { return; }  // re-click on the current row: no-op
    sourceError_.clear();

    if (idx == 0) {
        // Built-in generator: null restores it, and it cannot fail.
        soapy_ = nullptr;  // before setSource destroys a live Soapy source
        soapyArgs_.clear();
        pipeline_.setSource(nullptr);
        sourceSel_ = 0;
        return;
    }
    if (idx == 1) {
        // Show the path controls only; the switch happens on a successful
        // Open (see drawSourceSection) so a typo can never kill a live source.
        sourceSel_ = 1;
        return;
    }

    const std::size_t d = static_cast<std::size_t>(idx - 2);
    if (d >= soapyDevices_.size()) { return; }  // stale row; next frame redraws

    auto dev = std::make_unique<cascade::source::SoapySource>();
    if (!dev->open(soapyDevices_[d].args)) {
        // Revert-the-combo contract: sourceSel_ was never changed, so the
        // combo stays where it was; the reason lands in red below.
        sourceError_ = dev->lastError();
        return;
    }
    // Default the device to the DSP chain's design rate. A refusal is not
    // fatal (the panel shows the actual readback either way) but is surfaced.
    if (!dev->setSampleRateHz(kSoapyRateHz[kSoapyRateDefaultIndex])) {
        sourceError_ = dev->lastError();
    }
    soapyRateIndex_ = kSoapyRateDefaultIndex;

    // AGC probe doubling as initialization: explicitly select manual gain
    // mode (matching the unchecked box). False means the driver has no gain
    // mode — the documented "grey the checkbox" answer, not an error.
    soapyAgcSupported_ = dev->setAutoGain(false);
    soapyAgc_ = false;

    // Push the sliders' starting gain so hardware and display agree (there
    // is no per-element readback on SoapySource to initialize from).
    soapyGainNames_ = dev->listGainNames();
    soapyGainsDb_.assign(soapyGainNames_.size(), kSoapyGainDefaultDb);
    for (const std::string& g : soapyGainNames_) {
        dev->setGainDb(g, static_cast<double>(kSoapyGainDefaultDb));
    }

    soapy_ = dev.get();
    soapyArgs_ = soapyDevices_[d].args;
    pipeline_.setSource(std::move(dev));
    sourceSel_ = idx;
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
