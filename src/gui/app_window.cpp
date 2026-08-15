// GLFW + Dear ImGui application shell — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
// Generated window-icon pixels. Reached by a path relative to this file
// because resources/ is deliberately not on any target's include path — the
// icon is an asset, not a source root, and adding an include directory for one
// generated header would be a worse trade than a two-segment relative include.
#include "../../resources/icon/foxsdr_icon_rgba.hpp"
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

// FM de-emphasis choices. Broadcast FM pre-emphasises treble at the
// transmitter; the receiver must apply the matching inverse or the audio comes
// out bright and hissy. 50 us is the standard across Europe, Africa, Asia and
// Australia; 75 us in the Americas and South Korea. "Off" is for measurement
// and for feeding flat audio to an external decoder.
constexpr const char* kDeemphLabels[3] = {"50 us (EU/world)", "75 us (Americas)", "Off"};
constexpr double kDeemphUs[3] = {50.0, 75.0, 0.0};
constexpr int kDeemphCount = 3;

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

// SoapyAudio advertises every sound card on the machine as a SoapySDR device.
// They are not receivers: no tuner (centerFrequencyHz reads 0), no RF, and
// selecting one silently swaps your radio for a microphone input — which then
// gets persisted to config and restored on the next launch, so the real SDR
// appears to have "stopped being detected". They are filtered out of the
// Source list entirely, matching the policy --soapy-check already applies.
bool isAudioDriver(const std::string& args) {
    return args.find("driver=audio") != std::string::npos;
}

// Parses a typed frequency into Hz. Accepts what someone actually types at a
// radio: "100.3", "100.3 MHz", "433920k", "1.003e8", "100,300,000".
// Separators (space, comma, underscore) are ignored; a k/M/G suffix wins.
// With NO suffix the value is read as MHz when it is <= kBareMhzCutoff and as
// Hz above it — 7500 sits above every band a consumer SDR tunes (in MHz) and
// far below any plausible bare-Hz entry, so neither reading is ambiguous in
// practice. Returns false on junk, leaving the caller's value untouched.
constexpr double kBareMhzCutoff = 7500.0;

bool parseFrequencyHz(const char* text, double& outHz) {
    char clean[48];
    std::size_t n = 0;
    for (const char* p = text; *p != '\0' && n + 1 < sizeof(clean); ++p) {
        if (*p == ' ' || *p == ',' || *p == '_' || *p == '\'') { continue; }
        clean[n++] = *p;
    }
    clean[n] = '\0';
    if (n == 0) { return false; }

    char* end = nullptr;
    const double value = std::strtod(clean, &end);
    if (end == clean || !std::isfinite(value) || value < 0.0) { return false; }

    // Skip a trailing "Hz"/"hz" so "100.3MHz" and "100.3M" agree.
    while (*end == 'h' || *end == 'H' || *end == 'z' || *end == 'Z') {
        if ((*end == 'h' || *end == 'H') && end != clean) { break; }
        ++end;
    }
    double scale = 0.0;
    switch (*end) {
        case 'k': case 'K': scale = 1.0e3; break;
        case 'm': case 'M': scale = 1.0e6; break;
        case 'g': case 'G': scale = 1.0e9; break;
        case 'h': case 'H': scale = 1.0; break;  // explicit "100300000 Hz"
        case '\0': scale = (value <= kBareMhzCutoff) ? 1.0e6 : 1.0; break;
        default: return false;                   // trailing junk: reject
    }
    const double hz = value * scale;
    if (!std::isfinite(hz) || hz < 0.0 || hz > kMaxDisplayHz) { return false; }
    outHz = hz;
    return true;
}

// GLFW reports failures through this callback *before* glfwInit/CreateWindow
// return their error codes, so printing here is what gives the user an actual
// reason instead of a bare "init failed".
void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "cascade: GLFW error %d: %s\n", code,
                 description ? description : "(no description)");
}

// Applies the FoxSDR mark to the window's title-bar, taskbar and Alt-Tab
// slots. The executable's own RT_GROUP_ICON (resources/icon/foxsdr.rc) is what
// Explorer shows; this is the separate, runtime-owned window icon, and setting
// both is what stops the shipped app ever showing the blank default.
//
// The pixels are COMPILED IN from resources/icon/foxsdr_icon_rgba.hpp rather
// than decoded from a .ico at runtime or pulled back out of the executable's
// resources with LoadImage/GetIconInfo. A raw RGBA array needs no
// image-decoding dependency (this tree's whole premise is a small,
// licence-audited dependency set), no Win32-only code path in an otherwise
// portable shell, and no GDI/DIB handle lifetime to leak. GLFW copies the
// pixel data before returning, so the arrays need no lifetime management.
//
// Best-effort by construction: glfwSetWindowIcon returns void, and any
// platform-level refusal surfaces through glfwErrorCallback as one printed
// line. Nothing here can fail the caller — a missing icon must never stop the
// app starting.
void applyWindowIcon(GLFWwindow* window) {
    if (window == nullptr) { return; }
    // GLFWimage::pixels is a non-const unsigned char*; the cast is safe
    // because GLFW only reads the buffer (it copies it during the call).
    const GLFWimage images[] = {
        {icon::kSize16, icon::kSize16, const_cast<unsigned char*>(icon::kPixels16)},
        {icon::kSize32, icon::kSize32, const_cast<unsigned char*>(icon::kPixels32)},
        {icon::kSize48, icon::kSize48, const_cast<unsigned char*>(icon::kPixels48)},
    };
    glfwSetWindowIcon(window, static_cast<int>(sizeof(images) / sizeof(images[0])),
                      images);
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
           a.sampleRateHz == b.sampleRateHz &&
           a.stereoEnabled == b.stereoEnabled &&
           a.deemphasisIndex == b.deemphasisIndex &&
           a.nrEnabled == b.nrEnabled && a.nrStrength == b.nrStrength &&
           a.notchEnabled == b.notchEnabled && a.notchFreqHz == b.notchFreqHz &&
           a.notchQ == b.notchQ && a.autoNotch == b.autoNotch &&
           a.bandPlanOverlay == b.bandPlanOverlay &&
           a.pluginCatalogueUrl == b.pluginCatalogueUrl &&
           a.pluginBrowserOpen == b.pluginBrowserOpen;
}

// --- Plugin browser helpers (P9) ---------------------------------------------

// ASCII case-insensitive equality. Used only to compare plugin FILE NAMES,
// which sanitiseFileName() has already restricted to [A-Za-z0-9._-] — so a
// byte-wise ASCII fold is the whole of the correct comparison here, with no
// locale or Unicode case-folding question to get wrong.
bool equalsFileNameAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') { ca = static_cast<char>(ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = static_cast<char>(cb - 'A' + 'a'); }
        if (ca != cb) { return false; }
    }
    return true;
}

// Reads a catalogue index from the LOCAL filesystem and parses it with the
// same parseIndex() the network path uses. See AppConfig::pluginCatalogueUrl
// for why this form exists: a catalogue on a corporate share, and the only
// way the success path of this UI can be exercised without a live server.
//
// It grants nothing the network path does not already allow. parseIndex still
// refuses every non-https download URL and every malformed sha256, and
// install() re-checks both before it opens a socket — so the worst a hostile
// local index can do is offer an entry that install() then refuses.
// The same kMaxIndexBytes cap applies, because "it is on our own disk" is not
// a reason to read a 4 GB document into memory.
bool readLocalCatalogue(const std::string& path,
                        std::vector<cascade::core::PluginCatalogEntry>& out,
                        std::string& error) {
    out.clear();
    error.clear();
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) {
        error = "cannot read the catalogue file \"" + path + "\": " + ec.message();
        return false;
    }
    if (size > cascade::core::PluginRepo::kMaxIndexBytes) {
        error = "the catalogue file \"" + path + "\" is larger than the " +
                std::to_string(cascade::core::PluginRepo::kMaxIndexBytes) + "-byte limit";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open the catalogue file \"" + path + "\"";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    return cascade::core::PluginRepo::parseIndex(text, out, error);
}

// --- Band plan overlay constants (P7) ----------------------------------------
//
// The plan's own palette carries alpha 0x60 for a fill the spectrum trace has
// to stay readable through. It is dimmed further here for one structural
// reason: SpectrumView paints its own opaque panel background before the
// trace, so a rectangle drawn BEFORE it would be invisible and these are
// therefore painted after — over the trace rather than behind it. At this
// alpha the trace still reads through cleanly, and the band edges stay
// legible; the visual result is the intended "service bands behind the
// spectrum" without reaching inside a module this task may not modify.
// A full-height fill was tried first and rejected on sight: the FM broadcast
// band alone is 20.5 MHz wide, so at any normal zoom the whole view sits
// inside ONE band and the "overlay" became a wash tinting the entire spectrum
// and waterfall amber. The band is now a RIBBON along the top edge at full
// palette alpha, plus faint full-height edge lines — same information (extent,
// boundaries, name), none of the damage to the trace underneath.
constexpr float kBandRibbonPx = 6.0f;
constexpr float kBandEdgeAlphaScale = 0.55f;
// A band narrower than this many pixels gets no label — there is nowhere to
// put one that would not spill over its neighbours.
constexpr float kBandLabelMinPx = 46.0f;

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
    // Push every P7 mirror once so the pipeline and the panels start in
    // agreement even when no config file exists (the pipeline's own defaults
    // match these, so this is belt and braces rather than a fix-up).
    pipeline_.setStereoEnabled(stereoEnabled_);
    pipeline_.setNoiseReductionEnabled(nrEnabled_);
    pipeline_.setNoiseReductionStrength(nrStrength_);
    pipeline_.setNotchEnabled(notchEnabled_);
    pipeline_.setNotchFrequencyHz(static_cast<double>(notchFreqHz_));
    pipeline_.setNotchQ(static_cast<double>(notchQ_));
    pipeline_.setAutoNotchEnabled(autoNotch_);
    // Optional program data / optional user code. Both are silent no-ops when
    // their directory is absent, which is the normal case when running out of
    // a build tree — and every bounded --frames CI run takes this path.
    loadBandPlan();
    rescanPlugins();
    // The catalogue URL starts at the published default and is overwritten by
    // a config restore if the user (or an enterprise deployment) changed it.
    // Setting it here is NOT a fetch: nothing contacts the origin until the
    // Browse button is pressed.
    pluginCatalogueUrl_ = cascade::core::AppConfig{}.pluginCatalogueUrl;
    std::snprintf(pluginUrlBuf_, sizeof(pluginUrlBuf_), "%s", pluginCatalogueUrl_.c_str());
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
    // A catalogue fetch or a plugin download may still be in flight. The
    // std::async futures below block in their own destructors until the
    // worker returns, so without this an app closed mid-download would sit
    // there, apparently hung, for as long as the transfer took. cancel() is
    // thread-safe by contract and makes the worker fail out with "cancelled",
    // deleting its temp file on the way — so teardown stays bounded and no
    // partial DLL is left behind. Harmless when nothing is running.
    pluginRepo_.cancel();

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
    // Before the context is made current: purely a window-manager property,
    // independent of GL, so even a run that fails at backend init has already
    // shown the right icon.
    applyWindowIcon(window);
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

    // Bounded-run plugin-catalogue hook (see app_window.hpp). Read HERE, not
    // in the constructor, because only run() knows whether this is a bounded
    // CI run — and the hook is honored in no other mode, so an interactive
    // session can never be pointed at a different catalogue by a stray
    // environment variable.
    pluginTestHook_.clear();
    pluginTestStarted_ = false;
    if (frames >= 0) {
        const char* hook = std::getenv("CASCADE_PLUGIN_TEST");
        if (hook != nullptr && *hook != '\0') { pluginTestHook_ = hook; }
    }

    int rendered = 0;
    frameCounter_ = 0;
    while (!glfwWindowShouldClose(window)) {
        // Exact-count contract: check before rendering so --frames N produces
        // N frames, and --frames 0 produces none.
        if (frames >= 0 && rendered >= frames) { break; }

        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        frameCounter_ = rendered;
        // The hook's ONE fetch, started on the first frame so the rest of the
        // bounded run proves the window keeps rendering while it is in
        // flight. Everything else about the browser is unchanged — this is
        // the button press a headless run cannot make.
        if (!pluginTestHook_.empty() && !pluginTestStarted_) {
            pluginTestStarted_ = true;
            pluginCatalogueUrl_ = pluginTestHook_;
            std::snprintf(pluginUrlBuf_, sizeof(pluginUrlBuf_), "%s",
                          pluginCatalogueUrl_.c_str());
            pluginBrowseOpen_ = true;
            startCatalogFetch();
        }

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

    // Apply any finished SoapySDR scan/open. Last in the frame so the result
    // lands before the next draw reads the device list.
    pollSoapyAsync();
    // Same contract for the catalogue fetch / plugin download.
    pollPluginAsync();
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

    // --- Typed entry (double-click the readout) -----------------------------
    // Enter commits, Escape or clicking away cancels. The field is seeded in
    // MHz because that is how frequencies are spoken; parseFrequencyHz still
    // accepts Hz, kHz and GHz with an explicit suffix.
    if (freqEditing_) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.2f);
        ImGui::SetNextItemWidth(300.0f);
        if (freqEditFocus_) { ImGui::SetKeyboardFocusHere(); }
        const bool commit = ImGui::InputText(
            "##freq_edit", freqEditBuf_, sizeof(freqEditBuf_),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                ImGuiInputTextFlags_CharsNoBlank);
        const bool active = ImGui::IsItemActive();
        ImGui::PopFont();
        // SetKeyboardFocusHere only takes effect at the END of the frame, so
        // the field is NOT active on its first frame. Cancelling on "not
        // active" without this latch closed the editor in the same frame it
        // opened — which is exactly what made double-click look dead.
        if (active) { freqEditWasActive_ = true; }
        freqEditFocus_ = false;

        if (commit) {
            double typed = 0.0;
            if (parseFrequencyHz(freqEditBuf_, typed)) {
                tuneAbsoluteHz(typed);
                sourceError_.clear();
            } else {
                sourceError_ = std::string("could not read frequency \"") + freqEditBuf_ + "\"";
            }
            freqEditing_ = false;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (freqEditWasActive_ && !active)) {
            freqEditing_ = false;  // cancelled: the readback never moved
        }
        return;
    }

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
    bool hoveredDigit = false;
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
                retuneSourceHz(next);
            }
            hoveredDigit = true;  // tooltip is drawn after PopFont (see below)
            // SINGLE click opens the editor. Double-click was tried first and
            // is a trap here: the digits are Text items, and a synthetic or
            // fast double-click can collapse into one registered click, so it
            // silently did nothing. Single click also matches what SDR++
            // does, and wheel tuning is unaffected because that needs only
            // hover, never a click.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                std::snprintf(freqEditBuf_, sizeof(freqEditBuf_), "%.6f", hz / 1.0e6);
                freqEditing_ = true;
                freqEditFocus_ = true;
                freqEditWasActive_ = false;
            }
        }
        if (i != 9) { ImGui::SameLine(0.0f, 0.0f); }
    }
    ImGui::PopFont();

    // Tooltip AFTER PopFont: raised inside the 2.2x scope it inherited that
    // scale and painted a banner across the spectrum.
    if (hoveredDigit) {
        ImGui::SetTooltip("Scroll a digit to tune  |  double-click to type");
    }
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
        // Only meaningful for the FM modes; shown greyed elsewhere so the
        // setting is discoverable without implying it does anything to SSB.
        const bool fmMode = (modeIndex_ == 0 || modeIndex_ == 1);  // NFM, WFM
        ImGui::BeginDisabled(!fmMode);
        if (ImGui::Combo("De-emph", &deemphIndex_, kDeemphLabels, kDeemphCount)) {
            pipeline_.setDeemphasisUs(kDeemphUs[deemphIndex_]);
        }
        ImGui::EndDisabled();
        if (fmMode && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Broadcast FM pre-emphasises treble; the receiver undoes it.\n"
                              "50 us: Europe, Africa, Asia, Australia. 75 us: Americas, South Korea.");
        }

        // Stereo indicator + force-mono toggle + the RDS readout. Only in
        // WFM: nothing below it exists on any other demodulator.
        if (modeIndex_ == 1) { drawStereoRdsControls(); }

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
                    // so this is the whole device-switch operation. Through
                    // the pipeline, not the sink, so the DSP thread learns
                    // the new channel layout (stereo first, mono fallback).
                    pipeline_.openAudioDevice(dev.index);
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

        // Band plan overlay (P7). Always offered, even with no plan
        // installed — the checkbox is a display preference that persists, and
        // hiding it when resources/bandplans is missing would make the
        // feature look broken rather than simply idle.
        ImGui::Checkbox("Band plan", &bandPlanOverlay_);
        if (bandPlanOverlay_) {
            if (!bandPlanError_.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
                ImGui::TextWrapped("%s", bandPlanError_.c_str());
                ImGui::PopStyleColor();
            } else if (bandPlan_.entries().empty()) {
                ImGui::TextDisabled("no band plan installed");
            } else {
                ImGui::TextDisabled("%s (%d bands)", bandPlan_.name().c_str(),
                                    static_cast<int>(bandPlan_.entries().size()));
            }
        }
    }
    // P6/P7 sections. Closed by default (unlike the always-needed sections
    // above): all of them are occasional-use tools, and opening them by
    // default would push the Display controls off a 720p column.
    drawAudioFilterSection();
    drawRecorderSection();
    drawBookmarksSection();
    drawScannerSection();
    drawPluginsSection();
    ImGui::EndChild();

    // Status footer: active source identity, its sample rate (device readback
    // for Soapy, nominal otherwise), and the audio sink's cumulative underrun
    // count — the buffer-health readout the parity spec's status bar calls
    // for. Two clipped lines rather than one wrapped one: a long device name
    // must not push the numbers out of the reserved footer space.
    ImGui::Separator();
    cascade::source::IqSource& src = pipeline_.activeSource();
    // Line 1: the active source, and — when a band plan is loaded — the band
    // the TUNED frequency (source centre + VFO offset, i.e. what the VFO
    // marker sits on) falls in. BandPlan::at returns the narrowest match, so
    // this names "ISS Downlink" rather than the 2 m band containing it.
    const cascade::core::BandEntry* band =
        bandPlan_.entries().empty() ? nullptr : bandPlan_.at(currentAbsoluteHz());
    if (band != nullptr) {
        ImGui::Text("%s | %s", src.name(), band->name.c_str());
    } else {
        ImGui::TextUnformatted(src.name());
    }
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

    // While discovery or an open is in flight the controls are disabled and
    // the state is spelled out: the work is on a worker thread, so the window
    // keeps redrawing and the spectrum keeps running underneath.
    // A worker thread died on a driver exception — almost always the device
    // being unplugged mid-stream. Say so plainly: the spectrum has frozen and
    // without this the app just looks hung.
    if (pipeline_.faulted()) {
        ImGui::TextColored(kErrorRed, "Device stopped: %s", pipeline_.faultMessage().c_str());
        ImGui::TextWrapped("Reconnect it and pick the source again, or switch to the signal generator.");
    }

    const bool soapyBusy = soapyScanPending_ || soapyOpenPending_;
    if (soapyBusy) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%s",
                           soapyOpenPending_
                               ? ("Opening " + soapyBusyLabel_ + "...").c_str()
                               : "Scanning for devices...");
    }
    ImGui::BeginDisabled(soapyBusy);
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
    ImGui::EndDisabled();

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
    // Kick the enumeration onto a worker and return immediately — see the
    // header for why this may not run inline. One at a time: a second scan
    // while one is in flight would race the result into soapyDevices_.
    if (soapyScanPending_ || soapyOpenPending_) { return; }
    soapyScanned_ = true;  // claimed now so the combo does not re-request
    soapyScanPending_ = true;
    // enumerate() never throws and is simply empty on a machine with no
    // vendor modules; this is also the hot-plug refresh path.
    soapyScanFuture_ =
        std::async(std::launch::async, [] { return cascade::source::SoapySource::enumerate(); });
}

void AppWindow::pollSoapyAsync() {
    constexpr auto kNoWait = std::chrono::seconds(0);

    if (soapyScanPending_ && soapyScanFuture_.valid() &&
        soapyScanFuture_.wait_for(kNoWait) == std::future_status::ready) {
        auto found = soapyScanFuture_.get();
        soapyDevices_.clear();
        for (auto& d : found) {
            if (!isAudioDriver(d.args)) { soapyDevices_.push_back(std::move(d)); }
        }
        soapyScanPending_ = false;
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

    if (soapyOpenPending_ && soapyOpenFuture_.valid() &&
        soapyOpenFuture_.wait_for(kNoWait) == std::future_status::ready) {
        finishSoapyOpen(soapyOpenFuture_.get());
        soapyOpenPending_ = false;
        soapyBusyLabel_.clear();
    }
}

void AppWindow::finishSoapyOpen(SoapyOpenResult r) {
    // Failure: the combo was never moved, so it still shows the previous
    // source; the reason lands in red under the control.
    if (!r.dev) {
        sourceError_ = r.error.empty() ? "device open failed" : r.error;
        return;
    }
    // A non-fatal rate refusal still carries its reason.
    if (!r.error.empty()) { sourceError_ = r.error; }

    // Panel mirrors, then gain priming — all quick register writes, unlike
    // the make() that just finished on the worker.
    soapyRateIndex_ = nearestIndex(kSoapyRateHz, kSoapyRateCount, r.requestRateHz);
    soapyAgcSupported_ = r.dev->setAutoGain(false);
    soapyAgc_ = false;
    soapyGainNames_ = r.dev->listGainNames();
    soapyGainsDb_.assign(soapyGainNames_.size(), kSoapyGainDefaultDb);
    for (const std::string& g : soapyGainNames_) {
        r.dev->setGainDb(g, static_cast<double>(kSoapyGainDefaultDb));
    }

    soapy_ = r.dev.get();
    soapyArgs_ = r.args;
    pipeline_.setSource(std::move(r.dev));
    sourceKind_ = "soapy";
    sourceSel_ = r.row;
    followInputRate();  // DSP chain follows the device's actual readback
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
    if (soapyOpenPending_ || soapyScanPending_) { return; }  // one at a time

    // The open runs on a worker: Device::make() is the multi-second, USB-bus
    // -walking call that used to freeze the GUI here. sourceSel_ is left
    // alone until it resolves, which preserves the revert-the-combo contract
    // for free — a failure never moved the selection in the first place.
    const std::string args = soapyDevices_[d].args;
    const double rate = kSoapyRateHz[kSoapyRateDefaultIndex];
    soapyBusyLabel_ = soapyDevices_[d].label;
    soapyOpenPending_ = true;
    soapyOpenFuture_ = std::async(std::launch::async, [args, rate, idx] {
        SoapyOpenResult r;
        r.args = args;
        r.row = idx;
        r.requestRateHz = rate;
        auto dev = std::make_unique<cascade::source::SoapySource>();
        if (!dev->open(args)) {
            r.error = dev->lastError();
            return r;  // r.dev stays null: the GUI thread reports the failure
        }
        // A rate refusal is not fatal (the panel shows the actual readback
        // either way) but is surfaced.
        if (!dev->setSampleRateHz(rate)) { r.error = dev->lastError(); }
        r.dev = std::move(dev);
        return r;
    });
}

std::unique_ptr<cascade::source::SoapySource> AppWindow::openSoapy(
    const std::string& args, double requestRateHz) {
    // Also guards CONFIG RESTORE, not just the dropdown: a sound card saved by
    // an older build must not come back as the radio on every launch.
    if (isAudioDriver(args)) {
        sourceError_ = "saved source was a sound card (driver=audio), not a radio - ignored";
        return nullptr;
    }
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

    // Band plan behind the trace (see kBandFillAlphaScale for why "behind"
    // is achieved with a translucent fill painted after it). Before the
    // gridlines and the VFO overlay so those stay the topmost furniture.
    if (bandPlanOverlay_) {
        drawBandPlanOverlay(specPos.x, specPos.y, width, spectrumHeight);
    }

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
        } else if (ImGui::IsMouseClicked(0) && hit == SpectrumView::VfoHit::None) {
            // Click anywhere off the band: jump the VFO there. Dragging the
            // band still works (handled above); this is the "just take me to
            // that signal" gesture, and it needs no grab-and-drop.
            setVfoToAbsoluteHz(scale_.xToHz(mouseFrac), !io.KeyShift);
        }
        if (ImGui::IsMouseDoubleClicked(0) && hit == SpectrumView::VfoHit::None) {
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
    // A press that never crosses kDragSlopPx is a CLICK: tune the VFO to the
    // frequency under the cursor (mode raster unless Shift). Crossing the
    // threshold turns the same press into a pan, and no tune happens on
    // release. Both gestures live on the left button, which is why the
    // decision can only be made when the button comes back up.
    constexpr float kDragSlopPx = 4.0f;
    if (wfPanning_) {
        if (!ImGui::IsMouseDown(0)) {
            wfPanning_ = false;
            if (!wfMoved_) { setVfoToAbsoluteHz(scale_.xToHz(mouseFrac), !io.KeyShift); }
        } else {
            if (!wfMoved_ && std::fabs(io.MousePos.x - wfPressX_) > kDragSlopPx) {
                wfMoved_ = true;
            }
            if (wfMoved_ && io.MouseDelta.x != 0.0f) {
                scale_.pan(-static_cast<double>(io.MouseDelta.x) / width);
            }
        }
    } else if (wfHovered && ImGui::IsMouseClicked(0)) {
        wfPanning_ = true;
        wfPressX_ = io.MousePos.x;
        wfMoved_ = false;
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

void AppWindow::setVfoToAbsoluteHz(double wantAbsHz, bool snap) {
    if (snap) {
        // Same raster as the drag path (kModeSnapHz); Shift bypasses it.
        const double s = kModeSnapHz[modeIndex_];
        wantAbsHz = std::round(wantAbsHz / s) * s;
    }
    double off = wantAbsHz - pipeline_.activeSource().centerFrequencyHz();
    // Keep the whole band inside the baseband +/- inputRate/2, exactly as the
    // drag path does — clicking near the panel edge must not park the filter
    // half outside the spectrum we actually receive.
    const double lim = 0.5 * pipeline_.inputRateHz() - 0.5 * vfoBandwidthHz_;
    off = (lim > 0.0) ? std::clamp(off, -lim, lim) : 0.0;
    pipeline_.setVfoOffsetHz(off);
    vfoOffsetKhz_ = static_cast<float>(off / 1000.0);
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

// --- Stereo / RDS (P7) ---------------------------------------------------------

void AppWindow::drawStereoRdsControls() {
    // Force-mono toggle. The pipeline routes WFM through the stereo decoder
    // either way, so this only opens or closes its difference-channel gate —
    // which is what makes the switch click-free and tone-neutral.
    if (ImGui::Checkbox("Stereo", &stereoEnabled_)) {
        pipeline_.setStereoEnabled(stereoEnabled_);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Decode the 38 kHz difference channel when a 19 kHz\n"
                          "pilot is present. Unticked forces mono.");
    }
    ImGui::SameLine();
    // The indicator reports the DECODER, not the checkbox: lit only when a
    // pilot is actually locked AND stereo is enabled, dim-but-present when a
    // pilot is locked while the user forced mono, and greyed with no pilot.
    const bool locked = pipeline_.pilotLocked();
    const bool active = pipeline_.stereoActive();
    if (active) {
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "ST");
    } else if (locked) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "ST (forced mono)");
    } else {
        ImGui::TextDisabled("MONO");
    }
    if (locked) {
        ImGui::SameLine();
        ImGui::TextDisabled("pilot %.2f", static_cast<double>(pipeline_.pilotLevel()));
    }

    // --- RDS ------------------------------------------------------------------
    const cascade::core::RdsSnapshot rds = pipeline_.rdsSnapshot();
    ImGui::SeparatorText("RDS");
    if (!rds.state.piValid && !rds.state.psValid && rds.state.radioText.empty()) {
        // Distinguish "listening, nothing yet" from "not receiving at all":
        // synced means the block decoder found the 1187.5 bit/s stream and is
        // simply waiting for the first complete field.
        ImGui::TextDisabled(rds.synced ? "RDS: syncing..." : "RDS: no data");
        return;
    }
    if (rds.state.psValid) {
        ImGui::Text("PS  %s", rds.state.ps.c_str());
    } else {
        ImGui::TextDisabled("PS  --------");
    }
    if (rds.state.piValid) {
        ImGui::Text("PI  %04X   PTY %u%s%s", rds.state.pi,
                    static_cast<unsigned>(rds.state.pty),
                    rds.state.tp ? "  TP" : "", rds.state.ta ? "  TA" : "");
    } else {
        ImGui::TextDisabled("PI  ----");
    }
    if (!rds.state.radioText.empty()) {
        ImGui::TextWrapped("%s", rds.state.radioText.c_str());
    }
    ImGui::TextDisabled("groups %u | block errors %u", rds.state.groupsDecoded,
                        rds.state.blockErrors);
}

// --- Audio filters: noise reduction + notch (P7) --------------------------------

void AppWindow::drawAudioFilterSection() {
    if (!ImGui::CollapsingHeader("Audio filters")) { return; }

    // The order is the pipeline's, spelled out because it is the part a user
    // cannot infer from the controls.
    ImGui::TextDisabled("chain: notch -> auto-notch -> noise reduction");

    if (ImGui::Checkbox("Noise reduction", &nrEnabled_)) {
        pipeline_.setNoiseReductionEnabled(nrEnabled_);
    }
    ImGui::BeginDisabled(!nrEnabled_);
    if (ImGui::SliderFloat("Strength", &nrStrength_, 0.0f, 1.0f, "%.2f")) {
        pipeline_.setNoiseReductionStrength(nrStrength_);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Checkbox("Notch", &notchEnabled_)) {
        pipeline_.setNotchEnabled(notchEnabled_);
    }
    ImGui::BeginDisabled(!notchEnabled_);
    // Logarithmic: a linear 10 Hz..20 kHz slider spends 90% of its travel
    // above 2 kHz, where almost no heterodyne a user wants to remove lives.
    if (ImGui::SliderFloat("Freq", &notchFreqHz_, 10.0f, 20000.0f, "%.0f Hz",
                           ImGuiSliderFlags_Logarithmic)) {
        pipeline_.setNotchFrequencyHz(static_cast<double>(notchFreqHz_));
    }
    if (ImGui::SliderFloat("Q", &notchQ_, 1.0f, 200.0f, "%.0f")) {
        pipeline_.setNotchQ(static_cast<double>(notchQ_));
    }
    ImGui::EndDisabled();

    if (ImGui::Checkbox("Auto notch", &autoNotch_)) {
        pipeline_.setAutoNotchEnabled(autoNotch_);
    }
    if (autoNotch_) {
        ImGui::SameLine();
        if (pipeline_.autoNotchEngaged()) {
            ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "on %.0f Hz",
                               pipeline_.autoNotchFrequencyHz());
        } else {
            ImGui::TextDisabled("searching");
        }
    }
}

// --- Plugins (P7) ---------------------------------------------------------------

void AppWindow::rescanPlugins() {
    // A missing plugins directory is the normal case and yields an empty list
    // without an error — the host's documented behaviour, and the reason
    // nothing here reports a failure.
    pluginDir_ = cascade::core::PluginHost::defaultPluginDir();
    pluginHost_.scan(pluginDir_);
}

void AppWindow::drawPluginsSection() {
    if (!ImGui::CollapsingHeader("Plugins")) { return; }

    ImGui::TextDisabled("%s", pluginDir_.c_str());
    const float half = 0.5f * (ImGui::GetContentRegionAvail().x -
                               ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("Rescan", ImVec2(half, 0.0f))) { rescanPlugins(); }
    ImGui::SameLine();
    if (ImGui::Button(pluginBrowseOpen_ ? "Hide browser" : "Get plugins",
                      ImVec2(half, 0.0f))) {
        // Opening the browser is NOT a fetch. The view appears with an empty
        // list and a Browse button; the catalogue origin is contacted only
        // when that button is pressed.
        pluginBrowseOpen_ = !pluginBrowseOpen_;
    }

    if (pluginBrowseOpen_) { drawPluginBrowser(); }

    ImGui::SeparatorText("Installed");
    const std::vector<cascade::core::LoadedPlugin>& list = pluginHost_.plugins();
    if (list.empty()) {
        ImGui::TextDisabled("No plugins installed");
        // Still report a remove that just emptied the list — or one that
        // failed, which is exactly when the user needs to be told.
        if (!pluginBrowseOpen_) { drawPluginResultText(); }
        return;
    }
    // Removal is deferred past the loop: removeInstalledPlugin() rescans,
    // which replaces the very vector this loop is walking.
    std::string removeFile;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const cascade::core::LoadedPlugin& p = list[i];
        const std::string file = std::filesystem::path(p.path).filename().string();
        ImGui::PushID(static_cast<int>(i));
        if (p.loaded) {
            ImGui::Text("%s %s", p.name.c_str(), p.version.c_str());
            if (!p.author.empty()) { ImGui::TextDisabled("by %s", p.author.c_str()); }
            // The LICENCE is displayed, always and unconditionally. A plugin
            // is third-party code loaded into a commercially sold binary:
            // what terms it arrived under is the user's business and must not
            // require digging.
            ImGui::TextDisabled("licence: %s", p.licence.c_str());
        } else {
            // A refused candidate is shown WITH ITS REASON rather than
            // omitted — "my plugin does not appear" with no explanation is
            // the support ticket the host was designed to prevent.
            ImGui::TextColored(kErrorRed, "%s", file.c_str());
            ImGui::TextWrapped("%s", p.error.c_str());
        }
        // Two-step removal. Deleting a plugin is deleting a file the user
        // downloaded and may not be able to get back (a catalogue entry can
        // disappear), so a single mis-click must not do it.
        if (removeConfirmIdx_ == static_cast<int>(i)) {
            ImGui::TextWrapped("Delete %s?", file.c_str());
            if (ImGui::Button("Confirm delete")) {
                removeFile = file;
                removeConfirmIdx_ = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##rm")) { removeConfirmIdx_ = -1; }
        } else if (ImGui::SmallButton("Remove")) {
            removeConfirmIdx_ = static_cast<int>(i);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (!removeFile.empty()) { removeInstalledPlugin(removeFile); }
    // Only when the browser is collapsed: with it open the same text has
    // already been drawn under the Install button, and printing it twice in
    // one column reads as two separate failures.
    if (!pluginBrowseOpen_) { drawPluginResultText(); }
}

// --- The catalogue browser (P9) --------------------------------------------

void AppWindow::drawPluginBrowser() {
    ImGui::SeparatorText("Get plugins");
    // Stated in the UI, not just in the code: this is a promise to the user,
    // and a promise nobody can see is worth nothing.
    ImGui::TextWrapped("Nothing is downloaded, and the catalogue is not "
                       "contacted, until you press Browse.");

    // Catalogue URL. Committed on deactivate-after-edit rather than per
    // keystroke, so a half-typed host is never what a Browse would use.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##catalogue_url", "catalogue index.json URL",
                             pluginUrlBuf_, sizeof(pluginUrlBuf_));
    if (ImGui::IsItemDeactivatedAfterEdit()) { pluginCatalogueUrl_ = pluginUrlBuf_; }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("An https:// catalogue, or a path to a local index.json.\n"
                          "Plugin downloads are always https and always sha256-verified.");
    }

    const bool busy = catalogPending_ || installPending_;
    if (busy) {
        // Progress + Cancel. PluginRepo::progress() stays at 0 when the
        // server sends no Content-Length, which is deliberate on its side —
        // the bar then simply does not move rather than inventing a figure.
        if (installPending_) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Downloading %s...",
                               installBusyName_.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Fetching catalogue...");
        }
        ImGui::ProgressBar(pluginRepo_.progress(), ImVec2(-FLT_MIN, 0.0f));
        if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0.0f))) { pluginRepo_.cancel(); }
    } else if (ImGui::Button(catalog_.empty() ? "Browse catalogue"
                                              : "Refresh catalogue",
                             ImVec2(-FLT_MIN, 0.0f))) {
        startCatalogFetch();
    }

    // A fetch failure is shown verbatim and in red. The published catalogue
    // repository may be private, in which case this is an HTTP 404 — an
    // ordinary, supported state, so the app carries on exactly as before.
    if (!catalogError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", catalogError_.c_str());
        ImGui::PopStyleColor();
    }
    if (!catalogStatus_.empty()) { ImGui::TextDisabled("%s", catalogStatus_.c_str()); }

    // --- The list ------------------------------------------------------------
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        const cascade::core::PluginCatalogEntry& e =
            catalog_[static_cast<std::size_t>(i)];
        const bool installed = catalogEntryInstalled(e);
        ImGui::PushID(i);
        char label[256];
        std::snprintf(label, sizeof(label), "%s %s%s", e.name.c_str(), e.version.c_str(),
                      installed ? "  [installed]" : (e.compatible ? "" : "  [incompatible]"));
        if (ImGui::Selectable(label, i == catalogSel_)) {
            catalogSel_ = i;
            // The acknowledgement belongs to ONE entry. Carrying a tick from
            // the plugin the user just read about over to the next one would
            // hand out consent nobody gave.
            legalAck_ = false;
            installError_.clear();
            installReport_.clear();
        }
        // Author and LICENCE on every row, not only in the detail pane: the
        // terms a plugin arrives under are part of choosing it, not a detail
        // to discover after installing.
        ImGui::TextDisabled("by %s | licence: %s",
                            e.author.empty() ? "(unknown)" : e.author.c_str(),
                            e.licence.empty() ? "(none declared)" : e.licence.c_str());
        if (!e.compatible) {
            ImGui::TextColored(kErrorRed, "not compatible with this version");
        }
        ImGui::PopID();
    }

    // --- Detail pane + install gate -----------------------------------------
    if (catalogSel_ >= 0 && catalogSel_ < static_cast<int>(catalog_.size())) {
        const cascade::core::PluginCatalogEntry& e =
            catalog_[static_cast<std::size_t>(catalogSel_)];
        ImGui::Separator();
        ImGui::Text("%s %s", e.name.c_str(), e.version.c_str());
        ImGui::TextDisabled("by %s", e.author.empty() ? "(unknown)" : e.author.c_str());
        // NOT TextDisabled. The licence is the one line in this pane the user
        // is legally obliged to have seen before installing, so it is drawn
        // at full contrast, above the button, every time.
        ImGui::TextWrapped("Licence: %s",
                           e.licence.empty() ? "(none declared)" : e.licence.c_str());
        const std::string& body = e.description.empty() ? e.summary : e.description;
        if (!body.empty()) { ImGui::TextWrapped("%s", body.c_str()); }
        if (!e.homepage.empty()) { ImGui::TextDisabled("%s", e.homepage.c_str()); }
        const cascade::core::PluginPlatform* plat = e.thisPlatform();
        if (plat != nullptr && plat->sizeBytes > 0) {
            ImGui::TextDisabled("%s | %.2f MB", plat->file.c_str(),
                                static_cast<double>(plat->sizeBytes) / 1.0e6);
        }

        // THE LEGAL NOTICE. Some decoders demodulate transmissions whose
        // interception is an offence in some countries; the notice is the
        // author telling the user that, and it is not decoration. Install
        // stays disabled until the box is ticked (pluginInstallBlockedReason).
        if (!e.legalNotice.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Legal notice");
            ImGui::TextWrapped("%s", e.legalNotice.c_str());
            ImGui::Checkbox("I have read this notice and accept responsibility",
                            &legalAck_);
        }

        const std::string blocked = pluginInstallBlockedReason(catalogSel_, legalAck_);
        ImGui::BeginDisabled(!blocked.empty());
        if (ImGui::Button("Install", ImVec2(-FLT_MIN, 0.0f))) { startInstall(e); }
        ImGui::EndDisabled();
        if (!blocked.empty()) { ImGui::TextDisabled("Install disabled: %s", blocked.c_str()); }
    }

    // Directly under the Install button, which is where the user is looking.
    drawPluginResultText();
}

void AppWindow::drawPluginResultText() {
    // The failure is PluginRepo's own text, verbatim and in red — a sha256
    // mismatch names both digests and must be the most visible thing on the
    // panel, because it means the bytes that arrived were not the bytes the
    // catalogue vouched for. Paraphrasing it, or reducing it to "install
    // failed", would throw away the only evidence the user has.
    if (!installError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", installError_.c_str());
        ImGui::PopStyleColor();
    }
    if (!installReport_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 1.0f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", installReport_.c_str());
        ImGui::PopStyleColor();
    }
}

bool AppWindow::catalogEntryInstalled(const cascade::core::PluginCatalogEntry& e) const {
    const cascade::core::PluginPlatform* p = e.thisPlatform();
    if (p == nullptr) { return false; }
    // Compare the SANITISED name — the exact name install() would write — so
    // a catalogue that spells its file oddly cannot make an already-installed
    // plugin look absent (and offer a second install that would then land
    // under a different name).
    std::string want;
    std::string err;
    if (!cascade::core::PluginRepo::sanitiseFileName(p->file, want, err)) { return false; }
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        const std::string have = std::filesystem::path(lp.path).filename().string();
        if (equalsFileNameAscii(have, want)) { return true; }
    }
    return false;
}

std::string AppWindow::pluginInstallBlockedReason(int idx, bool acknowledged) const {
    if (idx < 0 || idx >= static_cast<int>(catalog_.size())) {
        return "no plugin selected";
    }
    // One transfer at a time. PluginRepo has a single progress/cancel pair,
    // so two concurrent operations would share one progress bar and one
    // Cancel button — and a cancel would hit whichever happened to look.
    if (catalogPending_ || installPending_) { return "a transfer is already in progress"; }

    const cascade::core::PluginCatalogEntry& e = catalog_[static_cast<std::size_t>(idx)];
    // Same exact-match rule as the loader and as install() itself: a near-miss
    // ABI is how a struct layout change becomes memory corruption days later.
    if (e.abiVersion != static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION)) {
        return "not compatible with this version (built for plugin ABI " +
               std::to_string(e.abiVersion) + ", this build requires exactly " +
               std::to_string(CASCADE_PLUGIN_ABI_VERSION) + ")";
    }
    if (e.thisPlatform() == nullptr) {
        return std::string("no build for ") + cascade::core::PluginRepo::hostOs() + "/" +
               cascade::core::PluginRepo::hostArch();
    }
    // No licence, no install. The plugin host already refuses to LOAD a module
    // that declares no licence (PluginRejection::MissingLicence), so an entry
    // with no licence in the catalogue is at best a download that could never
    // run — and at worst code whose terms nobody can see. Either way the user
    // cannot have "seen the licence" if there is not one.
    if (e.licence.empty()) { return "the catalogue entry declares no licence"; }
    if (catalogEntryInstalled(e)) { return "already installed"; }
    // THE ACKNOWLEDGEMENT GATE, last so it is the final thing standing between
    // a compatible, licensed, not-yet-installed plugin and the download.
    if (!e.legalNotice.empty() && !acknowledged) {
        return "the legal notice must be acknowledged first";
    }
    return {};
}

void AppWindow::startCatalogFetch() {
    if (catalogPending_ || installPending_) { return; }
    // A catalogue that could not be verified this time must not keep offering
    // installs from last time — the same all-or-nothing stance fetchIndex
    // takes with its own entries().
    catalog_.clear();
    catalogSel_ = -1;
    legalAck_ = false;
    catalogError_.clear();
    catalogStatus_.clear();
    installError_.clear();
    installReport_.clear();
    catalogPending_ = true;

    const std::string url = pluginCatalogueUrl_;
    catalogFuture_ = std::async(std::launch::async, [this, url] {
        CatalogFetchResult r;
        if (url.find("://") == std::string::npos) {
            // No scheme at all: a local file (see readLocalCatalogue). A URL
            // WITH a scheme — including http:// — goes to fetchIndex, which
            // is the single place the https-only rule is enforced.
            r.ok = readLocalCatalogue(url, r.entries, r.error);
        } else {
            r.ok = pluginRepo_.fetchIndex(url, r.error);
            if (r.ok) { r.entries = pluginRepo_.entries(); }
        }
        return r;
    });
}

void AppWindow::startInstall(cascade::core::PluginCatalogEntry entry) {
    if (catalogPending_ || installPending_) { return; }
    installError_.clear();
    installReport_.clear();
    installBusyName_ = entry.name;
    installPending_ = true;
    const std::string dir = pluginDir_;
    installFuture_ = std::async(
        std::launch::async, [this, e = std::move(entry), dir]() {
            PluginInstallResult r;
            r.name = e.name;
            // Every security rule lives inside install(): ABI match, platform
            // match, file-name sanitisation, https, the byte cap, and the
            // sha256 that decides whether the temp file ever becomes a plugin.
            r.ok = pluginRepo_.install(e, dir, r.installedPath, r.error);
            return r;
        });
}

void AppWindow::pollPluginAsync() {
    constexpr auto kNoWait = std::chrono::seconds(0);

    if (catalogPending_ && catalogFuture_.valid() &&
        catalogFuture_.wait_for(kNoWait) == std::future_status::ready) {
        CatalogFetchResult r = catalogFuture_.get();
        catalogPending_ = false;
        if (r.ok) {
            catalog_ = std::move(r.entries);
            catalogStatus_ = std::to_string(catalog_.size()) +
                             (catalog_.size() == 1u ? " plugin in the catalogue"
                                                    : " plugins in the catalogue");
        } else {
            catalog_.clear();
            catalogError_ = r.error;
        }
        if (!pluginTestHook_.empty()) { reportPluginTestResult(); }
    }

    if (installPending_ && installFuture_.valid() &&
        installFuture_.wait_for(kNoWait) == std::future_status::ready) {
        PluginInstallResult r = installFuture_.get();
        installPending_ = false;
        installBusyName_.clear();
        if (r.ok) {
            // The point of the rescan: the file is on disk, but it is not a
            // PLUGIN until the host has loaded and validated it — and if it
            // fails validation the user needs to see that here, immediately,
            // rather than after a restart.
            rescanPlugins();
            installReport_ = "Installed " + r.name + " to " + r.installedPath;
        } else {
            installError_ = r.error;
        }
    }
}

void AppWindow::removeInstalledPlugin(const std::string& fileName) {
    installError_.clear();
    installReport_.clear();

    // WINDOWS CANNOT DELETE A MAPPED IMAGE. A loaded plugin's DLL is open in
    // this process, and fs::remove on it fails with a sharing violation. Two
    // honest answers were available: unload first, or report the failure. We
    // unload — a Remove button that only works after a restart is not a
    // feature — and the cost is bounded because nothing in the product holds
    // a decoder instance across frames yet (the host's unloadAll() contract
    // requires exactly that; see plugin_host.hpp). The rescan below reloads
    // every survivor in the same frame, so the visible effect is that ONE
    // plugin disappears.
    //
    // If the delete still fails — the file is open in another process, or
    // permissions changed — PluginRepo's reason is shown verbatim in red and
    // the rescan puts everything back exactly as it was. Nothing is lost and
    // nothing is claimed that did not happen.
    pluginHost_.unloadAll();
    std::string err;
    if (pluginRepo_.remove(pluginDir_, fileName, err)) {
        installReport_ = "Removed " + fileName;
    } else {
        installError_ = err;
    }
    rescanPlugins();
}

void AppWindow::reportPluginTestResult() {
    // Bounded-run diagnostic only (CASCADE_PLUGIN_TEST). Machine-readable on
    // purpose: the install gate is a UI decision, and this is the only way a
    // headless run can prove which way it went.
    if (!catalogError_.empty()) {
        std::printf("plugin catalogue: FAILED frame=%d %s\n", frameCounter_,
                    catalogError_.c_str());
        return;
    }
    std::printf("plugin catalogue: frame=%d entries=%d\n", frameCounter_,
                static_cast<int>(catalog_.size()));
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        const cascade::core::PluginCatalogEntry& e =
            catalog_[static_cast<std::size_t>(i)];
        // Both answers: what the gate says with the acknowledgement UNTICKED
        // (what the user first sees) and with it ticked. A gate that stopped
        // requiring the tick would print install=ok on the first of these.
        const std::string noAck = pluginInstallBlockedReason(i, false);
        const std::string ack = pluginInstallBlockedReason(i, true);
        const std::string noAckText = noAck.empty() ? "ok" : ("blocked(" + noAck + ")");
        const std::string ackText = ack.empty() ? "ok" : ("blocked(" + ack + ")");
        std::printf("plugin entry: id=%s abi=%u compatible=%d installed=%d legal=%d "
                    "licence=\"%s\" install=%s installAcked=%s\n",
                    e.id.c_str(), static_cast<unsigned>(e.abiVersion),
                    e.compatible ? 1 : 0, catalogEntryInstalled(e) ? 1 : 0,
                    e.legalNotice.empty() ? 0 : 1, e.licence.c_str(),
                    noAckText.c_str(), ackText.c_str());
    }
}

// --- Band plan overlay (P7) -----------------------------------------------------

void AppWindow::loadBandPlan() {
    const std::string dir = cascade::core::BandPlan::defaultDir();
    std::error_code ec;
    // Existence is checked FIRST so the overwhelmingly common "no band plans
    // installed" case stays completely silent, per the feature's contract;
    // only a directory that is really there can produce an error worth
    // showing.
    if (!std::filesystem::is_directory(std::filesystem::path(dir), ec)) { return; }
    std::string err;
    if (!bandPlan_.loadDirectory(dir, err)) { bandPlanError_ = err; }
}

void AppWindow::drawBandPlanOverlay(float x0, float y0, float width, float height) {
    if (bandPlan_.entries().empty()) { return; }
    const std::vector<const cascade::core::BandEntry*> vis =
        bandPlan_.visible(scale_.viewLowHz(), scale_.viewHighHz());
    if (vis.empty()) { return; }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);
    for (const cascade::core::BandEntry* b : vis) {
        // entries()/visible() are ordered widest-first on a shared start, so
        // walking the vector paints containing bands before the narrower ones
        // nested inside them — the draw order the model already guarantees.
        float bx0 = x0 + static_cast<float>(scale_.hzToX(b->startHz)) * width;
        float bx1 = x0 + static_cast<float>(scale_.hzToX(b->endHz)) * width;
        bx0 = std::max(bx0, x0);
        bx1 = std::min(bx1, x0 + width);
        if (!(bx1 > bx0)) { continue; }

        const std::uint32_t rgba = b->colorRgba;  // 0xRRGGBBAA
        const int r = static_cast<int>((rgba >> 24) & 0xFFu);
        const int g = static_cast<int>((rgba >> 16) & 0xFFu);
        const int bl = static_cast<int>((rgba >> 8) & 0xFFu);
        const int a = static_cast<int>(rgba & 0xFFu);
        // A RIBBON along the top edge, not a full-height wash. Zoomed into a
        // single band — the normal case, since FM broadcast alone spans
        // 20 MHz — a full-panel fill tints the entire spectrum and waterfall
        // and destroys the trace's readability. A ribbon says exactly the same
        // thing (where the band starts, ends and what it is called) while
        // leaving the signal untouched.
        const float ribbonH = std::min(kBandRibbonPx, height * 0.25f);
        drawList->AddRectFilled(ImVec2(bx0, y0), ImVec2(bx1, y0 + ribbonH),
                                IM_COL32(r, g, bl, a));
        // Faint full-height edges still mark the boundaries down the panel, so
        // a band edge remains findable next to a signal without washing the
        // area between them.
        const int edgeA = static_cast<int>(static_cast<float>(a) * kBandEdgeAlphaScale);
        drawList->AddLine(ImVec2(bx0, y0), ImVec2(bx0, y0 + height),
                          IM_COL32(r, g, bl, edgeA));
        drawList->AddLine(ImVec2(bx1, y0), ImVec2(bx1, y0 + height),
                          IM_COL32(r, g, bl, edgeA));

        if (bx1 - bx0 >= kBandLabelMinPx) {
            const ImVec2 sz = ImGui::CalcTextSize(b->name.c_str());
            if (sz.x <= bx1 - bx0 - 4.0f) {
                // Label sits just under its ribbon, in near-white: coloured
                // text on the coloured ribbon was the least legible part of
                // the first attempt.
                drawList->AddText(ImVec2(bx0 + 3.0f, y0 + ribbonH + 1.0f),
                                  IM_COL32(235, 235, 235, 200), b->name.c_str());
            }
        }
    }
    drawList->PopClipRect();
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
    retuneSourceHz(absHz - pipeline_.vfoOffsetHz());
}

void AppWindow::retuneSourceHz(double centerHz) {
    // ONE place where the source centre moves. The pipeline cannot observe a
    // device retune (the source owns the tuner), so the RDS/stereo decoders
    // have to be told explicitly — otherwise the previous station's PS name
    // stays on screen over the new one, which is a wrong readout, not a
    // cosmetic lag. No-op when the tune does not actually move anything, so
    // a repeated command cannot keep the decoders permanently reset.
    cascade::source::IqSource& src = pipeline_.activeSource();
    if (src.centerFrequencyHz() == centerHz) { return; }
    src.setCenterFrequencyHz(centerHz);
    pipeline_.resetRds();
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

    // P7 settings. All are pure DSP switches with no failure mode, and the
    // loader has already clamped every one of them into range.
    deemphIndex_ = cfg.deemphasisIndex;
    pipeline_.setDeemphasisUs(kDeemphUs[deemphIndex_]);
    stereoEnabled_ = cfg.stereoEnabled;
    pipeline_.setStereoEnabled(stereoEnabled_);
    nrEnabled_ = cfg.nrEnabled;
    nrStrength_ = cfg.nrStrength;
    pipeline_.setNoiseReductionStrength(nrStrength_);
    pipeline_.setNoiseReductionEnabled(nrEnabled_);
    notchFreqHz_ = static_cast<float>(cfg.notchFreqHz);
    notchQ_ = static_cast<float>(cfg.notchQ);
    pipeline_.setNotchFrequencyHz(cfg.notchFreqHz);
    pipeline_.setNotchQ(cfg.notchQ);
    notchEnabled_ = cfg.notchEnabled;
    pipeline_.setNotchEnabled(notchEnabled_);
    autoNotch_ = cfg.autoNotch;
    pipeline_.setAutoNotchEnabled(autoNotch_);
    bandPlanOverlay_ = cfg.bandPlanOverlay;

    // Plugin browser. Restoring the URL and the open/closed state does NOT
    // start a fetch — see AppConfig::pluginCatalogueUrl. The user still has
    // to press Browse, on this launch as on every other.
    pluginCatalogueUrl_ = cfg.pluginCatalogueUrl;
    std::snprintf(pluginUrlBuf_, sizeof(pluginUrlBuf_), "%s", pluginCatalogueUrl_.c_str());
    pluginBrowseOpen_ = cfg.pluginBrowserOpen;

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
    cfg.stereoEnabled = stereoEnabled_;
    cfg.deemphasisIndex = deemphIndex_;
    cfg.nrEnabled = nrEnabled_;
    cfg.nrStrength = nrStrength_;
    cfg.notchEnabled = notchEnabled_;
    cfg.notchFreqHz = static_cast<double>(notchFreqHz_);
    cfg.notchQ = static_cast<double>(notchQ_);
    cfg.autoNotch = autoNotch_;
    cfg.bandPlanOverlay = bandPlanOverlay_;
    cfg.pluginCatalogueUrl = pluginCatalogueUrl_;
    cfg.pluginBrowserOpen = pluginBrowseOpen_;
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
