// GLFW + Dear ImGui application shell — implementation.
//
// SPDX-License-Identifier: MIT
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Mode tables, shared by the Radio section, the band-snap logic and the
// config store (the mode is persisted by NAME so a saved file survives any
// future enum reorder). Button order is the SDR++ parity layout; the enum
// order differs (LSB before CW), so the mapping is by name, never by index.
constexpr const char* kModeNames[8] = {"NFM", "WFM", "AM", "DSB",
                                       "USB", "CW",  "LSB", "RAW"};
constexpr cascade::dsp::DemodMode kModeMap[8] = {
    cascade::dsp::DemodMode::NFM, cascade::dsp::DemodMode::WFM,
    cascade::dsp::DemodMode::AM,  cascade::dsp::DemodMode::DSB,
    cascade::dsp::DemodMode::USB, cascade::dsp::DemodMode::CW,
    cascade::dsp::DemodMode::LSB, cascade::dsp::DemodMode::RAW};
// Bandwidth options offered in the combo, widest first.
constexpr const char* kBwLabels[6] = {"200k", "150k", "12.5k", "10k", "6k", "3k"};
constexpr double kBwHz[6] = {200000.0, 150000.0, 12500.0, 10000.0, 6000.0, 3000.0};
// Per-mode default bandwidth (index into kBwHz), applied when a mode button
// is clicked; the combo still allows any override. Rationale: WFM broadcast
// channel 150k; NFM two-way channel 12.5k; AM/DSB broadcast channel ~10k
// (both sidebands); SSB/CW voice/keying fits in 3k; RAW passes the full
// 200k channel for diagnostics.
constexpr int kModeDefaultBw[8] = {2, 1, 3, 3, 5, 5, 5, 0};

// Band-snap intervals for dragging the VFO CENTER on the spectrum, indexed
// in kModeNames order. The snap applies to the ABSOLUTE tuned frequency
// (source center + VFO offset), not the raw offset, so snapped stations land
// on real channel rasters; holding Shift bypasses it (free tuning).
//
//   mode | snap     | rationale
//   -----+----------+------------------------------------------
//   NFM  | 12.5 kHz | narrowband two-way channel raster
//   WFM  | 100 kHz  | broadcast FM channel raster
//   AM   | 9 kHz    | LW/MW broadcast raster
//   DSB  | 1 kHz    | free-form carrier work: round numbers
//   USB  | 1 kHz    | ham SSB convention
//   CW   | 1 kHz    | ham CW convention
//   LSB  | 1 kHz    | ham SSB convention
//   RAW  | 1 kHz    | diagnostics; snap kept for predictable steps
constexpr double kModeSnapHz[8] = {12500.0, 100000.0, 9000.0, 1000.0,
                                   1000.0,  1000.0,   1000.0, 1000.0};

// Height of the frequency tick strip. Placement decision (the spec left it
// open): BETWEEN the spectrum and the waterfall, so one strip labels both
// panels and stays clear of the spectrum's dB labels in the top-left corner.
constexpr float kAxisHeight = 18.0f;

// Tick capacity. FreqScale spaces ticks >= 80 px apart, so 128 slots cover a
// panel over 10K pixels wide before the HIGH end of the axis would truncate.
constexpr int kMaxTicks = 128;

// Wheel zoom factor per notch, applied as factor^notches so fractional
// touchpad deltas zoom proportionally. Zoom is always about the cursor.
constexpr double kZoomPerNotch = 1.3;

// VFO band-edge grab tolerance, pixels either side of the edge line.
constexpr float kVfoEdgeTolPx = 6.0f;

// VFO bandwidth clamp for edge drags and config restore:
// [3 kHz, 90% of the channel rate]. The lower bound keeps the band visible,
// grabbable and audible; the upper bound leaves the Vfo's decimating filter
// a transition band instead of demanding a brick wall at Nyquist.
constexpr double kVfoBwMinHz = 3000.0;
constexpr double kVfoBwMaxChanFrac = 0.9;

// Debounce for runtime config saves: a crash loses at most ~2 s of changes,
// while an in-progress drag never spams the disk (the timer restarts on
// every observed change).
constexpr double kConfigDebounceS = 2.0;

// Panel-furniture colors. kPanelBackground matches SpectrumView's background
// so the axis strip reads as part of the same instrument face; the vertical
// tick gridlines are fainter than the spectrum's 10 dB grid (alpha 18 vs 26)
// so the two grids stay visually separable; the waterfall marker reuses the
// spectrum overlay's warm center-line color.
constexpr ImU32 kPanelBackground = IM_COL32(8, 10, 14, 255);
constexpr ImU32 kAxisTickColor = IM_COL32(255, 255, 255, 140);
constexpr ImU32 kAxisLabelColor = IM_COL32(255, 255, 255, 110);
constexpr ImU32 kTickGridColor = IM_COL32(255, 255, 255, 18);
constexpr ImU32 kWfMarkerColor = IM_COL32(255, 170, 60, 200);

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

// Field-wise AppConfig comparison for the save debounce. Exact float
// compares are correct here: both sides come from the same currentConfig()
// code path, so any difference is a real user-visible change, never noise.
bool configsEqual(const cascade::core::AppConfig& a, const cascade::core::AppConfig& b) {
    return a.sourceKind == b.sourceKind && a.soapyArgs == b.soapyArgs &&
           a.iqFilePath == b.iqFilePath && a.centerHz == b.centerHz &&
           a.mode == b.mode && a.bandwidthHz == b.bandwidthHz &&
           a.squelchDb == b.squelchDb && a.volume == b.volume &&
           a.dbMin == b.dbMin && a.dbMax == b.dbMax &&
           a.splitRatio == b.splitRatio && a.vfoOffsetHz == b.vfoOffsetHz &&
           a.sampleRateHz == b.sampleRateHz;
}

// Index of the value in arr[0..n) closest to x (ties resolve low). Used to
// point preset combos at whatever a config file or device readback holds.
int nearestIndex(const double* arr, int n, double x) {
    int best = 0;
    for (int i = 1; i < n; ++i) {
        if (std::fabs(arr[i] - x) < std::fabs(arr[best] - x)) { best = i; }
    }
    return best;
}

// --- Recorder / Bookmarks / Scanner constants (P6) ---------------------------

// Recording destination: %USERPROFILE%/Documents/SDR-recordings per spec.
// Computed once at construction; the directory itself is created by
// Recorder::start on the first take, never at startup. An unset USERPROFILE
// (deliberately stripped environment) falls back to a relative directory —
// the same "stay writable" philosophy as ConfigStore::defaultPath's ".".
std::string defaultRecordDir() {
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr || *home == '\0') { home = std::getenv("HOME"); }
    if (home == nullptr || *home == '\0') { return "SDR-recordings"; }
    return std::string(home) + "/Documents/SDR-recordings";
}

// Scanner user-tune detection slack, Hz. Far above double rounding through
// (absHz - offset) + offset (nano-Hz at 9.99 GHz) and far below the smallest
// manual tuning action (the readout's 1 Hz digit), so it can neither
// false-trigger on arithmetic noise nor miss a real user tune.
constexpr double kScanUserTuneEpsHz = 0.5;

const char* scannerStateName(cascade::core::Scanner::State s) {
    switch (s) {
    case cascade::core::Scanner::State::Idle: return "Idle";
    case cascade::core::Scanner::State::Scanning: return "Scanning";
    case cascade::core::Scanner::State::Paused: return "Paused (signal)";
    case cascade::core::Scanner::State::Holding: return "Holding";
    }
    return "?";  // unreachable; keeps /W4 return-path analysis happy
}

}  // namespace

AppWindow::AppWindow(std::string configPath, bool announceConfig)
    : pipeline_(cascade::core::Pipeline::Config{kSampleRateHz, kFftSize, kAveragingAlpha,
                                                /*audioEnabled=*/true}),
      spectrum_(std::make_unique<SpectrumView>()),
      waterfall_(std::make_unique<WaterfallView>(static_cast<int>(kFftSize), kWaterfallHistory)) {
    configPath_ = std::move(configPath);
    configAnnounce_ = announceConfig;
    recordDir_ = defaultRecordDir();
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
    // DELIBERATELY no SoapySDR enumeration here. Enumeration loads vendor
    // modules (SoapyUHD -> uhd.dll -> libusb) whose USB discovery faulted
    // in-process in ~2% of measured `--frames 1` runs (0xC0000005 inside
    // libusb-1.0.dll during uhd::device::find — P6a, 2026-08-15). The scan
    // now runs only on the user's explicit request (first Source-dropdown
    // open, or Refresh — scanSoapy()), so sessions that never touch Soapy —
    // including every bounded --frames CI run — never execute that code.
    devices_ = pipeline_.audio().listOutputDevices();
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        if (devices_[static_cast<std::size_t>(i)].isDefault) { deviceIndex_ = i; }
    }
    if (deviceIndex_ < 0 && !devices_.empty()) { deviceIndex_ = 0; }

    // --- Config restore (P5) ------------------------------------------------
    // Load semantics per ConfigStore: missing file -> defaults + true; a
    // corrupt/unreadable file -> defaults + false. On false the construction
    // defaults above are KEPT (nothing applied) and the reason goes to
    // stderr plus, under the test hook, the diagnostic line.
    if (!configPath_.empty()) {
        cascade::core::AppConfig cfg;
        std::string err;
        const bool loaded = cascade::core::ConfigStore::load(configPath_, cfg, err);
        if (loaded) {
            applyConfig(cfg);
        } else {
            std::fprintf(stderr, "cascade: %s\n", err.c_str());
        }
        if (configAnnounce_) {
            // ONE diagnostic line, printed only under CASCADE_CONFIG_TEST
            // (normal runs stay byte-identical). Values are READBACK — the
            // demod mode the pipeline mirrors actually hold and the center
            // the active source reports — not an echo of the file.
            if (loaded) {
                std::printf("config applied: mode=%s center=%.0f\n",
                            kModeNames[modeIndex_],
                            pipeline_.activeSource().centerFrequencyHz());
            } else {
                std::printf("config applied: defaults (%s)\n", err.c_str());
            }
        }
        // Baseline for the debounce: what the file holds (or would hold).
        savedCfg_ = currentConfig();
        pendingCfg_ = savedCfg_;

        // Bookmarks ride the same persistence gate as the config: hermetic
        // runs (empty configPath_ — every --frames/--selftest CI run) never
        // read or write the user's bookmark file. Load semantics per
        // FreqManager: a missing file is a clean first run (true, empty
        // list); a damaged file surfaces its reason in red in the Bookmarks
        // section, exactly like Source errors — no stdout/stderr, so the
        // config-test diagnostic contract stays byte-identical.
        bookmarkPath_ = cascade::core::FreqManager::defaultPath();
        std::string bmErr;
        if (!freqMgr_.load(bookmarkPath_, bmErr)) { bookmarkError_ = bmErr; }
    }
}

AppWindow::~AppWindow() {
    // Safety net (run()'s teardown already does this on the normal path):
    // the recorder members are destroyed before pipeline_ (reverse
    // declaration order), so any tap still installed must be uninstalled
    // first — stop*Recording clears the pipeline pointer, then finalizes.
    stopIqRecording();
    stopAudioRecording();
}

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

    // Interactive runs start receiving immediately. A radio that opens with
    // dead black panels and no hint that a button must be pressed reads as
    // broken — it was the single biggest first-run complaint. Bounded
    // --frames runs stay stopped so CI keeps its old timing and never spawns
    // DSP threads it does not need.
    if (frames < 0) { pipeline_.start(); }

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

        // Debounced runtime persistence: the config file follows the session
        // ~2 s after the last change, so a crash loses almost nothing.
        // Hermetic runs (empty configPath_) never touch the disk.
        if (!configPath_.empty()) { maybeSaveConfig(glfwGetTime()); }

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

    // Closing the window mid-take finalizes both recordings cleanly (same
    // contract as the toolbar Stop): taps out, then headers patched — before
    // the pipeline teardown below ends the sample flow they were taping.
    stopIqRecording();
    stopAudioRecording();

    // Clean-exit save, unconditional: cheap, and it guarantees the on-disk
    // config matches the final session state even when the debounce never
    // fired (e.g. a change made less than 2 s before closing the window).
    // Runs before pipeline_.stop() so the snapshot reads live state.
    if (!configPath_.empty()) { saveConfigNow(); }

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

    // Scanner driver, AFTER all widgets: any manual tune the user made this
    // frame (digit wheel, VFO drag, bookmark click) is already applied, so
    // the user-wins detection inside sees this frame's state, not last
    // frame's. Inert while the scanner is Idle — including every hermetic
    // --frames run.
    scannerFrame();
}

void AppWindow::drawToolbar() {
    // The label reads the pipeline, not a local flag, so the button can never
    // disagree with the actual thread state.
    const bool running = pipeline_.running();
    if (ImGui::Button(running ? "Stop" : "Play", ImVec2(64.0f, 0.0f))) {
        if (running) {
            // Play-stop while recording stops the recording cleanly (spec):
            // taps uninstalled and both WAVs finalized BEFORE the DSP
            // threads join, so a take can never outlive the sample flow it
            // was taping. No-ops when nothing is recording.
            stopIqRecording();
            stopAudioRecording();
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
        // Mode/bandwidth tables live at namespace scope (kModeNames &co):
        // the band-snap logic and the config store share them.
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
            if (ImGui::Button(kModeNames[i], ImVec2(cellWidth, 0.0f))) {
                modeIndex_ = i;
                pipeline_.setDemodMode(kModeMap[i]);
                bandwidthIndex_ = kModeDefaultBw[i];
                vfoBandwidthHz_ = kBwHz[bandwidthIndex_];
                pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
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
            vfoBandwidthHz_ = kBwHz[bandwidthIndex_];
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
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
    // P6 sections. Closed by default (unlike the always-needed sections
    // above): all three are occasional-use tools, and opening them by
    // default would push the Display controls off a 720p column.
    drawRecorderSection();
    drawBookmarksSection();
    drawScannerSection();
    ImGui::EndChild();

    // Status footer: active source identity, its sample rate (device readback
    // for Soapy, nominal otherwise), and the audio sink's cumulative underrun
    // count — the buffer-health readout the parity spec's status bar calls
    // for. Two clipped lines rather than one wrapped one: a long device name
    // must not push the numbers out of the reserved footer space.
    ImGui::Separator();
    cascade::source::IqSource& src = pipeline_.activeSource();
    ImGui::TextUnformatted(src.name());
    // Source rate | DSP channel rate (the Vfo's output rate the demodulator
    // runs at — this is what makes rate-follow visible) | buffer health.
    ImGui::Text("%.4g MS/s | ch %.4g kHz | underruns %llu",
                src.sampleRateHz() / 1.0e6, pipeline_.channelRateHz() / 1.0e3,
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

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##source_select", rowLabel(sourceSel_))) {
        // Lazy first scan: opening the dropdown IS the user asking to see
        // devices, and it is the earliest moment the list is needed (the
        // closed combo's preview never reads it). The one-off enumeration
        // hitch lands here instead of at startup — see the constructor
        // comment for why the eager scan was removed.
        if (!soapyScanned_) { scanSoapy(); }
        const int rowCount = 2 + static_cast<int>(soapyDevices_.size());
        for (int i = 0; i < rowCount; ++i) {
            // PushID: two identical devices (same model, no serial in the
            // label) must still be distinct rows.
            ImGui::PushID(i);
            if (ImGui::Selectable(rowLabel(i), i == sourceSel_)) { selectSource(i); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Refresh")) { scanSoapy(); }

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
                sourceKind_ = "file";
                iqOpenPath_ = iqPath_;
                followInputRate();  // DSP chain + frequency axis track the file's rate
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
            } else {
                // Rate-follow (P5): rebuild the DSP chain for the ACTUAL
                // device readback so demod/audio and the frequency axis all
                // track the hardware, not the request.
                followInputRate();
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

void AppWindow::scanSoapy() {
    // enumerate() never throws and is simply empty on a machine with no
    // vendor modules; this is also the hot-plug refresh path.
    soapyDevices_ = cascade::source::SoapySource::enumerate();
    soapyScanned_ = true;
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

void AppWindow::selectSource(int idx) {
    if (idx == sourceSel_) { return; }  // re-click on the current row: no-op
    sourceError_.clear();

    if (idx == 0) {
        // Built-in generator: null restores it, and it cannot fail.
        soapy_ = nullptr;  // before setSource destroys a live Soapy source
        soapyArgs_.clear();
        pipeline_.setSource(nullptr);
        sourceKind_ = "siggen";
        sourceSel_ = 0;
        followInputRate();  // back to the generator's fixed 2 MS/s
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

    // Default the device to the DSP chain's design rate; openSoapy also
    // primes gains/AGC and the panel mirrors. On failure the revert-the-
    // combo contract holds: sourceSel_ was never changed, so the combo stays
    // where it was and the reason lands in red below (sourceError_).
    auto dev = openSoapy(soapyDevices_[d].args, kSoapyRateHz[kSoapyRateDefaultIndex]);
    if (!dev) { return; }

    soapy_ = dev.get();
    soapyArgs_ = soapyDevices_[d].args;
    pipeline_.setSource(std::move(dev));
    sourceKind_ = "soapy";
    sourceSel_ = idx;
    followInputRate();  // DSP chain follows the device's actual readback
}

std::unique_ptr<cascade::source::SoapySource> AppWindow::openSoapy(
    const std::string& args, double requestRateHz) {
    auto dev = std::make_unique<cascade::source::SoapySource>();
    if (!dev->open(args)) {
        sourceError_ = dev->lastError();
        return nullptr;
    }
    // A rate refusal is not fatal (the panel shows the actual readback
    // either way) but is surfaced.
    if (!dev->setSampleRateHz(requestRateHz)) {
        sourceError_ = dev->lastError();
    }
    // Point the Rate combo at the preset nearest the request (exact for the
    // Source-menu path, best-effort for an arbitrary rate from a config).
    soapyRateIndex_ = nearestIndex(kSoapyRateHz, kSoapyRateCount, requestRateHz);

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
    return dev;
}

void AppWindow::followInputRate() {
    const double rate = pipeline_.activeSource().sampleRateHz();
    if (!(rate > 0.0)) { return; }  // never-opened source; nothing to follow
    if (!pipeline_.setInputRateHz(rate)) {
        // The chain kept its old rate (fractional channel rate, or out of
        // the supported range). The display span then reflects the OLD rate,
        // which is exactly what the DSP is still doing — surface why.
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "DSP rate-follow refused %.0f S/s; chain stays at %.0f",
                      rate, pipeline_.inputRateHz());
        sourceError_ = buf;
    }
    // An ACCEPTED rate change finalizes an in-flight IQ take: the WAV header
    // rate is fixed at start(), so recording on across a rate switch would
    // produce a file that replays detuned/off-speed. (A refusal above kept
    // the old rate, so the compare — not the call — decides.) The audio take
    // is untouched: its 48 kHz output rate survives every rate switch.
    if (iqRecorder_.recording() && pipeline_.inputRateHz() != iqRecordRateHz_) {
        stopIqRecording();
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

    // The frequency scale follows the tuned center (active source readback)
    // and the DSP input rate every frame; setSpan preserves the user's zoom
    // window whenever it still fits the new baseband, so a small retune or a
    // rate change does not silently throw the view away.
    cascade::source::IqSource& src = pipeline_.activeSource();
    scale_.setSpan(src.centerFrequencyHz(), pipeline_.inputRateHz());

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterThickness = 6.0f;
    const float usable = avail.y - splitterThickness - kAxisHeight;
    // A squeezed window can drive the region to zero; drawing into negative
    // sizes asserts inside ImGui, so just skip the panels that frame.
    if (usable < 40.0f || avail.x < 40.0f) { return; }

    const float width = avail.x;
    const float spectrumHeight = splitRatio_ * usable;
    const float waterfallHeight = usable - spectrumHeight;

    // Visible slice of the fftshifted spectrum, shared by BOTH panels so
    // they can never disagree about the zoom window.
    double firstBin = 0.0;
    double lastBin = 0.0;
    scale_.visibleBinRange(static_cast<int>(kFftSize), firstBin, lastBin);

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // --- Spectrum -----------------------------------------------------------
    const ImVec2 specPos = ImGui::GetCursorScreenPos();
    // Before the first frame lastFrame_.dbBins is empty; SpectrumView renders
    // the background + grid for null bins, which is the wanted idle look.
    const float* bins = lastFrame_.dbBins.empty() ? nullptr : lastFrame_.dbBins.data();
    spectrum_->drawBinRange(bins, static_cast<int>(lastFrame_.dbBins.size()),
                            firstBin, lastBin, width, spectrumHeight);
    const bool specHovered = ImGui::IsItemHovered();

    // Axis ticks, computed once and shared by the spectrum's vertical
    // gridlines and the tick strip below.
    double tickHz[kMaxTicks];
    char tickLabels[kMaxTicks][16];
    const int tickCount = scale_.ticks(static_cast<double>(width), tickHz,
                                       tickLabels, kMaxTicks);
    for (int i = 0; i < tickCount; ++i) {
        // ticks() only returns in-view frequencies, so x stays in-panel.
        const float x =
            specPos.x + static_cast<float>(scale_.hzToX(tickHz[i])) * width;
        drawList->AddLine(ImVec2(x, specPos.y), ImVec2(x, specPos.y + spectrumHeight),
                          kTickGridColor);
    }

    // --- VFO band: interaction first, then the overlay ----------------------
    // Band edges in absolute Hz -> panel-width fractions via the shared
    // scale. vfoBandwidthHz_ is the REQUESTED bandwidth (the Vfo clamps its
    // filter internally; the overlay shows what the user asked for).
    double bandCenterAbs = src.centerFrequencyHz() + pipeline_.vfoOffsetHz();
    SpectrumView::VfoBand band;
    band.x0Frac = scale_.hzToX(bandCenterAbs - 0.5 * vfoBandwidthHz_);
    band.x1Frac = scale_.hzToX(bandCenterAbs + 0.5 * vfoBandwidthHz_);
    band.dragging = (vfoDrag_ != VfoDrag::None);

    const double mouseFrac =
        static_cast<double>(io.MousePos.x - specPos.x) / static_cast<double>(width);
    if (vfoDrag_ == VfoDrag::None && specHovered) {
        const auto hit = SpectrumView::hitTest(
            mouseFrac, band, static_cast<double>(kVfoEdgeTolPx) / width);
        if (hit == SpectrumView::VfoHit::EdgeLow ||
            hit == SpectrumView::VfoHit::EdgeHigh) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else if (hit == SpectrumView::VfoHit::Center) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (ImGui::IsMouseClicked(0) && hit != SpectrumView::VfoHit::None) {
            vfoDrag_ = (hit == SpectrumView::VfoHit::Center)  ? VfoDrag::Center
                       : (hit == SpectrumView::VfoHit::EdgeLow) ? VfoDrag::EdgeLow
                                                                : VfoDrag::EdgeHigh;
            vfoGrabDeltaHz_ = scale_.xToHz(mouseFrac) - bandCenterAbs;
        } else if (ImGui::IsMouseDoubleClicked(0) &&
                   hit == SpectrumView::VfoHit::None) {
            scale_.resetView();  // double-click on empty spectrum: unzoom
        }
    }
    if (vfoDrag_ != VfoDrag::None) {
        if (!ImGui::IsMouseDown(0)) {
            vfoDrag_ = VfoDrag::None;
        } else {
            // xToHz is deliberately unclamped, so dragging past the panel
            // edge keeps working; the offset/bandwidth clamps below are what
            // bound the actual tuning.
            const double mouseHz = scale_.xToHz(mouseFrac);
            if (vfoDrag_ == VfoDrag::Center) {
                double wantAbs = mouseHz - vfoGrabDeltaHz_;
                if (!io.KeyShift) {
                    // Snap the ABSOLUTE tuned frequency to the mode's raster
                    // (kModeSnapHz table above); Shift = free tuning.
                    const double snap = kModeSnapHz[modeIndex_];
                    wantAbs = std::round(wantAbs / snap) * snap;
                }
                double off = wantAbs - src.centerFrequencyHz();
                // Keep the whole band inside the baseband +/- inputRate/2.
                // (After this clamp an extreme position may sit off-raster;
                // the raster loses to the hard band-inside-span rule.)
                const double lim =
                    0.5 * pipeline_.inputRateHz() - 0.5 * vfoBandwidthHz_;
                off = (lim > 0.0) ? std::clamp(off, -lim, lim) : 0.0;
                pipeline_.setVfoOffsetHz(off);
                vfoOffsetKhz_ = static_cast<float>(off / 1000.0);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            } else {
                // Either edge adjusts the bandwidth SYMMETRICALLY about the
                // band center (the VFO filter is symmetric by construction).
                double bw = 2.0 * std::fabs(mouseHz - bandCenterAbs);
                const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
                bw = std::max(kVfoBwMinHz, std::min(bw, bwHi));
                vfoBandwidthHz_ = bw;
                pipeline_.setVfoBandwidthHz(bw);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            // Recompute the band from post-drag state so the overlay tracks
            // the cursor within the same frame instead of lagging one behind.
            bandCenterAbs = src.centerFrequencyHz() + pipeline_.vfoOffsetHz();
            band.x0Frac = scale_.hzToX(bandCenterAbs - 0.5 * vfoBandwidthHz_);
            band.x1Frac = scale_.hzToX(bandCenterAbs + 0.5 * vfoBandwidthHz_);
            band.dragging = true;
        }
    }
    spectrum_->drawVfoOverlay(band, width, spectrumHeight);

    // --- Frequency tick strip ------------------------------------------------
    drawFreqAxis(width, tickHz, tickLabels, tickCount);

    // Splitter: an invisible button whose vertical drag re-balances the
    // spectrum/waterfall split. Ratio (not pixels) so a window resize keeps
    // the user's proportions.
    ImGui::InvisibleButton("##vsplitter", ImVec2(width, splitterThickness));
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
    drawList->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                            ImGui::GetColorU32(gripColor));

    // --- Waterfall -----------------------------------------------------------
    // The u-window derives from the same visible bin range as the spectrum,
    // in the waterfall's bin-center convention: texture column b covers
    // u in [b/W, (b+1)/W] with its center at (b+0.5)/W, while the spectrum
    // puts bin b's vertex AT the panel x for (b - first)/(last - first) —
    // so mapping bin centers to the panel edges keeps a signal's trace peak
    // and its waterfall stripe on the same pixel column at any zoom.
    const ImVec2 wfPos = ImGui::GetCursorScreenPos();
    const double u0 = (firstBin + 0.5) / static_cast<double>(kFftSize);
    const double u1 = (lastBin + 0.5) / static_cast<double>(kFftSize);
    waterfall_->draw(width, waterfallHeight, u0, u1);
    const bool wfHovered = ImGui::IsItemHovered();

    // Thin VFO marker on the waterfall (the parity spec's "where am I tuned"
    // line), culled when the tuned frequency is scrolled out of view.
    const double markFrac = scale_.hzToX(bandCenterAbs);
    if (markFrac >= 0.0 && markFrac <= 1.0) {
        const float x = wfPos.x + static_cast<float>(markFrac) * width;
        drawList->AddLine(ImVec2(x, wfPos.y), ImVec2(x, wfPos.y + waterfallHeight),
                          kWfMarkerColor);
    }

    // Horizontal click-drag on the waterfall pans the view: the content
    // follows the cursor, so the window shifts OPPOSITE the mouse delta.
    // Tracked manually (not via item-active state) because the waterfall's
    // layout item is a Dummy, which never becomes the active item.
    if (wfPanning_) {
        if (!ImGui::IsMouseDown(0)) {
            wfPanning_ = false;
        } else if (io.MouseDelta.x != 0.0f) {
            scale_.pan(-static_cast<double>(io.MouseDelta.x) / width);
        }
    } else if (wfHovered && ImGui::IsMouseClicked(0)) {
        wfPanning_ = true;
    }
    if (wfHovered && ImGui::IsMouseDoubleClicked(0)) { scale_.resetView(); }

    // Wheel over EITHER panel zooms about the cursor (1.3x per notch; the
    // zoom floor and full-span clamp live in FreqScale). Both panels share
    // the same x extent, so the spectrum-relative fraction serves both.
    if ((specHovered || wfHovered) && io.MouseWheel != 0.0f) {
        scale_.zoomAt(mouseFrac,
                      std::pow(kZoomPerNotch, static_cast<double>(io.MouseWheel)));
    }
}

void AppWindow::drawFreqAxis(float width, const double* tickHz,
                             const char (*labels)[16], int count) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + kAxisHeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(p0, p1, kPanelBackground);
    drawList->PushClipRect(p0, p1, true);
    for (int i = 0; i < count; ++i) {
        const float x = p0.x + static_cast<float>(scale_.hzToX(tickHz[i])) * width;
        drawList->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + 4.0f), kAxisTickColor);
        // Label centered under its tick, shifted (not clipped) at the strip
        // ends so the first/last label stays fully readable.
        const ImVec2 sz = ImGui::CalcTextSize(labels[i]);
        float tx = x - 0.5f * sz.x;
        if (tx < p0.x + 2.0f) { tx = p0.x + 2.0f; }
        if (tx + sz.x > p1.x - 2.0f) { tx = p1.x - 2.0f - sz.x; }
        drawList->AddText(ImVec2(tx, p0.y + 4.0f), kAxisLabelColor, labels[i]);
    }
    drawList->PopClipRect();
    ImGui::Dummy(ImVec2(width, kAxisHeight));
}

// --- Recorder (P6) -------------------------------------------------------------

void AppWindow::drawRecorderSection() {
    if (!ImGui::CollapsingHeader("Recorder")) { return; }

    // Destination, always visible so the user knows where takes land. The
    // directory is created by Recorder::start on the first record.
    ImGui::TextDisabled("%s", recordDir_.c_str());

    // IQ take: baseband at the DSP input rate through the pipeline's raw
    // tap. Toggle button: label and action swap with the recorder state.
    if (!iqRecorder_.recording()) {
        if (ImGui::Button("Record IQ", ImVec2(-FLT_MIN, 0.0f))) {
            const double rate = pipeline_.inputRateHz();
            std::string err;
            if (iqRecorder_.start(cascade::core::RecordKind::BasebandIq,
                                  recordDir_, rate, err)) {
                recordError_.clear();
                iqRecordRateHz_ = rate;
                iqRecordStartS_ = ImGui::GetTime();
                // Install AFTER start(): the tap must never feed a recorder
                // that is not accepting (Pipeline::setIqRecorder contract).
                pipeline_.setIqRecorder(&iqRecorder_);
            } else {
                recordError_ = err;
            }
        }
    } else {
        if (ImGui::Button("Stop IQ", ImVec2(-FLT_MIN, 0.0f))) { stopIqRecording(); }
        // Elapsed is wall time since the take started; samples/MB are the
        // recorder's own accepted-byte counters, so they never overclaim.
        ImGui::Text("IQ %.1f s | %llu samples | %.1f MB",
                    ImGui::GetTime() - iqRecordStartS_,
                    static_cast<unsigned long long>(iqRecorder_.samplesWritten()),
                    static_cast<double>(iqRecorder_.bytesWritten()) / 1.0e6);
    }

    // Audio take: the post-chain 48 kHz output (same point audioTap uses).
    if (!audioRecorder_.recording()) {
        if (ImGui::Button("Record audio", ImVec2(-FLT_MIN, 0.0f))) {
            std::string err;
            if (audioRecorder_.start(cascade::core::RecordKind::Audio, recordDir_,
                                     cascade::core::Pipeline::kAudioRateHz, err)) {
                recordError_.clear();
                audioRecordStartS_ = ImGui::GetTime();
                pipeline_.setAudioRecorder(&audioRecorder_);
            } else {
                recordError_ = err;
            }
        }
    } else {
        if (ImGui::Button("Stop audio", ImVec2(-FLT_MIN, 0.0f))) {
            stopAudioRecording();
        }
        ImGui::Text("Audio %.1f s | %llu samples | %.1f MB",
                    ImGui::GetTime() - audioRecordStartS_,
                    static_cast<unsigned long long>(audioRecorder_.samplesWritten()),
                    static_cast<double>(audioRecorder_.bytesWritten()) / 1.0e6);
    }

    // Recording is allowed while stopped (the file just stays empty until
    // samples flow), but say so instead of leaving frozen counters to read
    // like a bug. Toolbar Stop DURING a take finalizes it (drawToolbar).
    if (!pipeline_.running() &&
        (iqRecorder_.recording() || audioRecorder_.recording())) {
        ImGui::TextDisabled("(press Play to feed the recorders)");
    }

    if (!recordError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", recordError_.c_str());
        ImGui::PopStyleColor();
    }
}

void AppWindow::stopIqRecording() {
    // Order per the Pipeline::setIqRecorder contract: after the setter
    // returns no writeIq against this recorder is in flight or can begin
    // (the pointer swap serializes on the mutex the DSP thread holds across
    // writes), so stop() — which patches the header and closes the file —
    // cannot overlap a write. Both calls are no-ops when already idle.
    pipeline_.setIqRecorder(nullptr);
    iqRecorder_.stop();
}

void AppWindow::stopAudioRecording() {
    pipeline_.setAudioRecorder(nullptr);
    audioRecorder_.stop();
}

// --- Bookmarks (P6) --------------------------------------------------------------

void AppWindow::drawBookmarksSection() {
    if (!ImGui::CollapsingHeader("Bookmarks")) { return; }

    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputTextWithHint("##bm_name", "name", bookmarkName_,
                             sizeof(bookmarkName_));
    ImGui::SameLine();
    if (ImGui::Button("Add current")) {
        cascade::core::Bookmark b;
        b.name = bookmarkName_;
        if (b.name.empty()) {
            // A nameless row would render blank; default to the frequency.
            char def[32];
            std::snprintf(def, sizeof(def), "%.4f MHz",
                          currentAbsoluteHz() / 1.0e6);
            b.name = def;
        }
        // "Current" is the tuned station: center readback + VFO offset (the
        // band the spectrum overlay marks), with the live mode and the
        // REQUESTED bandwidth (the overlay's value, pre any Vfo clamp).
        b.freqHz = currentAbsoluteHz();
        b.mode = kModeNames[modeIndex_];
        b.bandwidthHz = vfoBandwidthHz_;
        freqMgr_.add(std::move(b));
        saveBookmarks();
    }

    // Rows: click-to-tune selectable + per-row delete. The delete is
    // deferred past the loop so removeAt can never invalidate an index the
    // same frame still iterates.
    int deleteIdx = -1;
    const std::vector<cascade::core::Bookmark>& list = freqMgr_.list();
    const float delW = ImGui::GetFrameHeight();
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        const cascade::core::Bookmark& b = list[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        char label[192];
        std::snprintf(label, sizeof(label), "%s  %.4f MHz", b.name.c_str(),
                      b.freqHz / 1.0e6);
        const float rowW = ImGui::GetContentRegionAvail().x - delW -
                           ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_None,
                              ImVec2(rowW, 0.0f))) {
            // Click-to-tune: frequency through the shared absolute-tune
            // path (same as scanner retunes), then mode and bandwidth. An
            // unknown mode name — a newer build's file, kept verbatim by
            // FreqManager on purpose — leaves the current mode untouched.
            tuneAbsoluteHz(b.freqHz);
            for (int m = 0; m < 8; ++m) {
                if (b.mode == kModeNames[m]) {
                    modeIndex_ = m;
                    pipeline_.setDemodMode(kModeMap[m]);
                    break;
                }
            }
            // Same clamp as the config restore: [3 kHz, 90% of channel rate].
            const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
            vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(b.bandwidthHz, bwHi));
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
            bandwidthIndex_ = nearestIndex(kBwHz, 6, vfoBandwidthHz_);
        }
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(delW, 0.0f))) { deleteIdx = i; }
        ImGui::PopID();
    }
    if (deleteIdx >= 0) {
        freqMgr_.removeAt(static_cast<std::size_t>(deleteIdx));
        saveBookmarks();
    }

    if (!bookmarkError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", bookmarkError_.c_str());
        ImGui::PopStyleColor();
    }
}

void AppWindow::saveBookmarks() {
    if (bookmarkPath_.empty()) { return; }  // hermetic run: never touch disk
    std::string err;
    if (freqMgr_.save(bookmarkPath_, err)) {
        bookmarkError_.clear();  // a successful save clears a stale error
    } else {
        bookmarkError_ = err;
    }
}

// --- Shared absolute tuning (P6) -----------------------------------------------

void AppWindow::tuneAbsoluteHz(double absHz) {
    // The same setter + readback path the toolbar digit wheel uses, with the
    // VFO offset preserved: command the SOURCE center so the VFO band lands
    // on absHz. A refusal (a tune the driver rejects) needs no handling —
    // every display, and the scanner's user-tune baseline, follows the
    // readback, which simply won't move.
    pipeline_.activeSource().setCenterFrequencyHz(absHz - pipeline_.vfoOffsetHz());
}

double AppWindow::currentAbsoluteHz() {
    return pipeline_.activeSource().centerFrequencyHz() + pipeline_.vfoOffsetHz();
}

// --- Scanner (P6) ----------------------------------------------------------------

void AppWindow::drawScannerSection() {
    if (!ImGui::CollapsingHeader("Scanner")) { return; }

    // Mirrors -> Params. Scanner::configure sanitizes (swap, step floor,
    // negative times), so the raw edit values can be handed over as-is.
    const auto paramsFromMirrors = [this]() {
        cascade::core::Scanner::Params p;
        p.startHz = scanStartMhz_ * 1.0e6;
        p.stopHz = scanStopMhz_ * 1.0e6;
        p.stepHz = scanStepKhz_ * 1.0e3;
        p.dwellMs = scanDwellMs_;
        p.holdMs = scanHoldMs_;
        p.resumeMs = scanResumeMs_;
        return p;
    };

    // Commit on deactivate-after-edit (not per keystroke): while the scan is
    // ACTIVE a commit reconfigures it, which per the Scanner contract resets
    // to the new startHz — correct for new parameters, but far too jumpy to
    // fire on every typed digit.
    bool edited = false;
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputDouble("Start MHz", &scanStartMhz_, 0.0, 0.0, "%.4f");
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputDouble("Stop MHz", &scanStopMhz_, 0.0, 0.0, "%.4f");
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputDouble("Step kHz", &scanStepKhz_, 0.0, 0.0, "%.2f");
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputDouble("Dwell ms", &scanDwellMs_, 0.0, 0.0, "%.0f");
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputDouble("Hold ms", &scanHoldMs_, 0.0, 0.0, "%.0f");
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputDouble("Resume ms", &scanResumeMs_, 0.0, 0.0, "%.0f");
    edited |= ImGui::IsItemDeactivatedAfterEdit();

    if (edited && scanner_.active()) {
        scanner_.configure(paramsFromMirrors());
        // The reconfigure re-emits a first tune on the next tick; the
        // user-tune baseline re-arms from that retune's readback.
        scannerHasExpected_ = false;
    }

    if (!scanner_.active()) {
        if (ImGui::Button("Start scan", ImVec2(-FLT_MIN, 0.0f))) {
            scanner_.configure(paramsFromMirrors());
            scanner_.start(ImGui::GetTime() * 1000.0);
            scannerHasExpected_ = false;
        }
    } else {
        if (ImGui::Button("Stop scan", ImVec2(-FLT_MIN, 0.0f))) { scanner_.stop(); }
    }

    // State + frequency readout. currentHz() keeps reporting the last scan
    // frequency after a stop (Scanner contract), which reads naturally here.
    ImGui::Text("%s | %.4f MHz", scannerStateName(scanner_.state()),
                scanner_.currentHz() / 1.0e6);
}

void AppWindow::scannerFrame() {
    if (!scanner_.active()) { return; }

    // User wins: any tune this frame that the scanner did not command —
    // digit wheel, VFO drag/slider (the offset is part of the absolute
    // frequency), bookmark click, source switch — leaves the readback off
    // the last commanded value, and the scan stops rather than fight the
    // user's hands. Checked BEFORE tick so the stale squelch state of a
    // just-abandoned frequency can never emit one more retune.
    if (scannerHasExpected_ &&
        std::fabs(currentAbsoluteHz() - scannerExpectedAbsHz_) > kScanUserTuneEpsHz) {
        scanner_.stop();
        return;
    }

    // Squelch-open per the squelch's own OPEN comparison (Squelch::process
    // opens at channel power > threshold; > , not >=): same power quantity,
    // same threshold value (squelchDb_ mirrors what setSquelchDb pushed).
    // Documented approximation: the reading comes from the pipeline's
    // S-meter snapshot — an EMA ~100x slower than the squelch's internal
    // meter — and the close-side hysteresis/hold are not replicated. For
    // the scan decision only "is a signal present now" matters, and the
    // S-meter is the one channel-power readout that is lock-free from the
    // GUI thread.
    const bool squelchOpen = pipeline_.signalPowerDb() > squelchDb_;
    const std::optional<double> retune =
        scanner_.tick(ImGui::GetTime() * 1000.0, squelchOpen);
    if (retune.has_value()) {
        tuneAbsoluteHz(*retune);
        // Baseline from READBACK, not the request: a device that coerces
        // the tune must not read as a user action next frame.
        scannerExpectedAbsHz_ = currentAbsoluteHz();
        scannerHasExpected_ = true;
    }
}

// --- Config persistence (P5) -------------------------------------------------

void AppWindow::applyConfig(const cascade::core::AppConfig& cfg) {
    // Panel mirrors + always-safe DSP settings first (none of these can
    // fail; load() already range-sanitized volume/split/db*).
    volume_ = cfg.volume;
    pipeline_.audio().setVolume(volume_);
    dbMin_ = cfg.dbMin;
    dbMax_ = cfg.dbMax;
    spectrum_->setRange(dbMin_, dbMax_);
    splitRatio_ = cfg.splitRatio;
    squelchDb_ = cfg.squelchDb;
    pipeline_.setSquelchDb(squelchDb_);
    for (int i = 0; i < 8; ++i) {
        if (cfg.mode == kModeNames[i]) {
            modeIndex_ = i;
            pipeline_.setDemodMode(kModeMap[i]);
            break;  // an unknown mode name keeps the construction default
        }
    }

    // Source restore. The generator is always safe (it is already active);
    // a file is restored only if the path still opens; a Soapy device only
    // if its args re-open. Any failure falls back to the generator silently
    // except for lastError shown once in the Source section (sourceError_).
    if (cfg.sourceKind == "file") {
        auto file = std::make_unique<cascade::source::IqFileSource>();
        if (file->open(cfg.iqFilePath)) {
            file->setCenterFrequencyHz(cfg.centerHz);
            std::snprintf(iqPath_, sizeof(iqPath_), "%s", cfg.iqFilePath.c_str());
            iqOpenPath_ = cfg.iqFilePath;
            pipeline_.setSource(std::move(file));
            sourceKind_ = "file";
            sourceSel_ = 1;
            followInputRate();
        } else {
            sourceError_ = file->lastError();
        }
    } else if (cfg.sourceKind == "soapy" && !cfg.soapyArgs.empty()) {
        auto dev = openSoapy(cfg.soapyArgs, cfg.sampleRateHz);
        if (dev) {
            dev->setCenterFrequencyHz(cfg.centerHz);
            soapy_ = dev.get();
            soapyArgs_ = cfg.soapyArgs;
            pipeline_.setSource(std::move(dev));
            sourceKind_ = "soapy";
            // Point the combo at the restored device if this machine still
            // enumerates it; -1 otherwise (preview falls back to live name).
            sourceSel_ = -1;
            for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
                if (soapyDevices_[i].args == cfg.soapyArgs) {
                    sourceSel_ = 2 + static_cast<int>(i);
                }
            }
            followInputRate();
        }
        // openSoapy already set sourceError_ on failure.
    }
    if (sourceKind_ == "siggen") {
        // Generator kept (or fallen back to): carry the saved center so the
        // readout matches the last session. Nominal-center set cannot fail.
        pipeline_.activeSource().setCenterFrequencyHz(cfg.centerHz);
    }

    // VFO after the source/rate restore so the clamps use the REAL rates the
    // chain ended up with, not whatever the file claimed.
    const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
    vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(cfg.bandwidthHz, bwHi));
    pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
    bandwidthIndex_ = nearestIndex(kBwHz, 6, vfoBandwidthHz_);  // combo display
    double off = cfg.vfoOffsetHz;
    const double lim = 0.5 * pipeline_.inputRateHz() - 0.5 * vfoBandwidthHz_;
    off = (lim > 0.0) ? std::clamp(off, -lim, lim) : 0.0;
    pipeline_.setVfoOffsetHz(off);
    vfoOffsetKhz_ = static_cast<float>(off / 1000.0);
}

cascade::core::AppConfig AppWindow::currentConfig() {
    cascade::core::AppConfig cfg;
    cfg.sourceKind = sourceKind_;
    cfg.soapyArgs = soapyArgs_;
    cfg.iqFilePath = iqOpenPath_;
    cfg.centerHz = pipeline_.activeSource().centerFrequencyHz();
    cfg.mode = kModeNames[modeIndex_];
    cfg.bandwidthHz = vfoBandwidthHz_;
    cfg.squelchDb = squelchDb_;
    cfg.volume = volume_;
    cfg.dbMin = dbMin_;
    cfg.dbMax = dbMax_;
    cfg.splitRatio = splitRatio_;
    cfg.vfoOffsetHz = pipeline_.vfoOffsetHz();
    cfg.sampleRateHz = pipeline_.activeSource().sampleRateHz();
    return cfg;
}

void AppWindow::maybeSaveConfig(double nowS) {
    cascade::core::AppConfig cur = currentConfig();
    if (configsEqual(cur, savedCfg_)) {
        lastChangeTimeS_ = -1.0;  // clean again (e.g. change was undone)
        return;
    }
    if (lastChangeTimeS_ < 0.0 || !configsEqual(cur, pendingCfg_)) {
        // First difference, or the state moved again: restart the debounce
        // window so an in-progress drag never writes mid-gesture.
        pendingCfg_ = cur;
        lastChangeTimeS_ = nowS;
        return;
    }
    if (nowS - lastChangeTimeS_ >= kConfigDebounceS) {
        std::string err;
        if (cascade::core::ConfigStore::save(configPath_, cur, err)) {
            savedCfg_ = cur;
            lastChangeTimeS_ = -1.0;
        } else {
            // Retry no sooner than the next debounce window — a locked file
            // must not turn into one save attempt per rendered frame.
            lastChangeTimeS_ = nowS;
            std::fprintf(stderr, "cascade: %s\n", err.c_str());
        }
    }
}

void AppWindow::saveConfigNow() {
    cascade::core::AppConfig cur = currentConfig();
    std::string err;
    if (cascade::core::ConfigStore::save(configPath_, cur, err)) {
        savedCfg_ = cur;
    } else {
        std::fprintf(stderr, "cascade: %s\n", err.c_str());
    }
}

}  // namespace cascade::gui
