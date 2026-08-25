// GLFW + Dear ImGui application shell — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// After the ImGui backends: glfw3.h pulls in GL/gl.h on Windows, which the
// opengl3 backend must not see before its own embedded loader.
#include <GLFW/glfw3.h>

#include "core/version.hpp"
#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
// Generated window-icon pixels. Reached by a path relative to this file
// because resources/ is deliberately not on any target's include path — the
// icon is an asset, not a source root, and adding an include directory for one
// generated header would be a worse trade than a two-segment relative include.
#include "../../resources/icon/foxsdr_icon_rgba.hpp"
#include "core/image_write.hpp"
#include "gui/map_view.hpp"
#include "gui/spectrum_view.hpp"
#include "gui/track_detail_view.hpp"
#include "gui/waterfall_view.hpp"
#include "source/iq_file_source.hpp"

#ifdef _WIN32
// ShellExecuteW, for handing the verified installer to the shell so its
// elevation prompt is shown. Last, and after the library headers, for the same
// reason soapy_source.cpp puts it last: windows.h defines macros that have
// collided with library headers before.
#include <windows.h>

#include <shellapi.h>
#endif

namespace cascade::gui {

namespace {

// Takes a resolved device-open result and lets it go, which is precisely what
// closes the device: the result owns the SoapySource and its destructor is the
// close. Templated only so it can live here, at file scope, without naming
// AppWindow's private result type.
template <typename Fut>
void drainSoapyOpen(Fut& f) {
    try {
        auto r = f.get();
        (void)r;  // destroyed here; ~SoapySource releases the handle
    } catch (...) {
        // A worker that threw has no device to release. Never propagate: this
        // runs during teardown, and on the detached path with no catcher above
        // it at all.
    }
}

// The scan's equivalent, and it is deliberately NOT drainSoapyOpen. That one
// exists to CLOSE a device — the result owns the SoapySource. A scan result
// owns nothing but an enumeration list of strings, so there is no handle at
// stake and this drain has exactly one job: let the future's blocking
// destructor run somewhere that is not the GUI thread.
template <typename Fut>
void drainSoapyScan(Fut& f) {
    try {
        auto r = f.get();
        (void)r;  // just an enumeration list; dropped here
    } catch (...) {
        // Never propagate: this runs during teardown, and on the detached path
        // with no catcher above it at all.
    }
}

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

// The private great-circle helper that used to live here is gone: the track
// table needs a BEARING as well as a distance, the coverage accumulator needs
// both, and a third private copy of the trigonometry would have been a third
// place for one of them to be wrong. Both now come from gui/track_metrics.hpp,
// which is where they are tested against known pairs.

// Field-wise AppConfig comparison for the save debounce. Exact float
// compares are correct here: both sides come from the same currentConfig()
// code path, so any difference is a real user-visible change, never noise.
bool configsEqual(const cascade::core::AppConfig& a, const cascade::core::AppConfig& b) {
    return a.sourceKind == b.sourceKind && a.soapyArgs == b.soapyArgs &&
           a.soapyAntenna == b.soapyAntenna &&
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
           // Map geometry takes part, which is what makes a resize save at all.
           // The debounce restarts on every change, so a drag writes once when
           // it stops rather than once per frame while it is happening.
           a.mapWindowWidth == b.mapWindowWidth &&
           a.mapWindowHeight == b.mapWindowHeight &&
           a.mapWindowX == b.mapWindowX && a.mapWindowY == b.mapWindowY &&
           a.rxPositionSet == b.rxPositionSet && a.rxLatDeg == b.rxLatDeg &&
           a.rxLonDeg == b.rxLonDeg &&
           a.pluginCatalogueUrl == b.pluginCatalogueUrl &&
           a.pluginBrowserOpen == b.pluginBrowserOpen &&
           a.pluginLastUpdateCheck == b.pluginLastUpdateCheck &&
           a.pluginTuneAllowed == b.pluginTuneAllowed &&
           a.pluginsStopped == b.pluginsStopped &&
           a.pluginMuteOverride == b.pluginMuteOverride &&
           a.catEnabled == b.catEnabled && a.catBindAll == b.catBindAll &&
           a.catPort == b.catPort &&
           a.webEnabled == b.webEnabled &&
           a.webBindAddress == b.webBindAddress && a.webPort == b.webPort &&
           a.webUsername == b.webUsername &&
           a.webPasswordRecord == b.webPasswordRecord &&
           // Telemetry: only the DURABLE fields take part. telemetryPending
           // grows a second every second and telemetryCleanExit is pure
           // bookkeeping, so comparing either would make the config
           // permanently "changed" and write the file every two seconds for
           // as long as the application is open. They ride along on whatever
           // save the fields below trigger, and on the unconditional save at
           // exit — which is the one that matters, because it is where the
           // finished report and the clean-exit marker are written.
           //
           // telemetryLaunches changing at start-up is load-bearing: it is
           // what makes the first debounced save happen, and that save is
           // what puts telemetryCleanExit=false on disk. Without it a crash
           // in the first seconds would look like a clean exit.
           a.telemetryEnabled == b.telemetryEnabled &&
           a.telemetryInstallId == b.telemetryInstallId &&
           a.telemetryLaunches == b.telemetryLaunches &&
           a.telemetryCrashes == b.telemetryCrashes &&
           // Diagnostics: both are user switches that change only on a click,
           // so they belong here - without them, turning capture off would
           // not survive a restart unless something else happened to trigger
           // a save.
           a.diagnosticsEnabled == b.diagnosticsEnabled &&
           a.diagnosticsMinidump == b.diagnosticsMinidump;
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
    // Web server providers. Installed once, before any start(), because the
    // server refuses to change them while running. Both do nothing but copy
    // the snapshot the GUI thread publishes each frame — see the note in
    // app_window.hpp for why they must not touch the pipeline directly.
    webServer_.setStatusProvider([this]() {
        std::lock_guard<std::mutex> lock(webMutex_);
        return webStatus_;
    });
    // The CAT server reads the SAME published snapshot, so a frequency read
    // over CAT and one read in the browser can never disagree.
    catServer_.setStatusProvider([this]() {
        std::lock_guard<std::mutex> lock(webMutex_);
        return webStatus_;
    });
    webServer_.setSpectrumProvider([this](cascade::net::SpectrumSnapshot& inOut) {
        std::lock_guard<std::mutex> lock(webMutex_);
        // Same contract as Pipeline::getLatestFrame: nothing newer than the
        // caller's cursor means "no frame", so a polling browser never
        // re-fetches a picture it already drew.
        if (webSeq_ == 0 || inOut.seq >= webSeq_) {
            return false;
        }
        inOut.seq = webSeq_;
        inOut.centerHz = webSnapCenterHz_;
        inOut.spanHz = webSnapSpanHz_;
        inOut.dbBins = webBins_;
        return true;
    });

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
            // AFTER applyConfig, so the crash count and the pending report are
            // read from the file rather than from freshly defaulted members.
            // Sends nothing unless the user previously opted in.
            diagnosticsEnabled_ = cfg.diagnosticsEnabled;
            diagnosticsMinidump_ = cfg.diagnosticsMinidump;
            telemetryStartup(cfg);
            updateCheckEnabled_ = cfg.updateCheckEnabled;
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
    // FIRST, before anything else is torn down: stop serving.
    //
    // The server's provider callbacks capture `this` and read webMutex_,
    // webStatus_ and webBins_, all of which are declared AFTER webServer_ and
    // are therefore destroyed BEFORE it. Relying on ~WebServer to stop the
    // listener would mean a request in flight could touch a destroyed mutex
    // during teardown. stop() joins the listener thread, so once it returns no
    // handler is running or can start.
    webServer_.stop();
    // Same reasoning for the CAT server: its provider captures `this` and
    // locks webMutex_, which is declared after it and destroyed first.
    catServer_.stop();

    // A catalogue fetch or a plugin download may still be in flight. The
    // std::async futures below block in their own destructors until the
    // worker returns, so without this an app closed mid-download would sit
    // there, apparently hung, for as long as the transfer took. cancel() is
    // thread-safe by contract and makes the worker fail out with "cancelled",
    // deleting its temp file on the way — so teardown stays bounded and no
    // partial DLL is left behind. Harmless when nothing is running.
    pluginRepo_.cancel();

    // The app-update download is the same problem and needs its own flag:
    // it runs through the STATIC fetch helper, which has no PluginRepo
    // instance and so was never reachable by the cancel() above.
    // updateDownloadFuture_ is a member, and a std::async future's destructor
    // blocks until the worker returns, so before this existed a quit during an
    // update sat in ~AppWindow for the rest of the transfer — window gone,
    // process still running, indistinguishable from a hang. The flag is polled
    // between chunks, so the worker fails out with "cancelled" and deletes its
    // ".part" file on the way.
    updateCancel_.store(true, std::memory_order_relaxed);

    // Same problem, no cancel to reach for: a device open may still be inside
    // SoapySDR::Device::make(). See reapPendingSoapyOpen for the semantics.
    reapPendingSoapyOpen();

    // And the same again for the lazy device SCAN, which blocks in
    // SoapySDR::Device::enumerate() and is the likelier of the two to be in
    // flight at quit — it starts the moment the source combo is opened.
    reapPendingSoapyScan();

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
    // MULTI-VIEWPORT: the map and each decoded image get a REAL operating
    // system window rather than a panel penned inside this one. A received
    // picture and a target map are things a user wants on a second monitor,
    // beside the radio rather than on top of it, and an ImGui window confined
    // to the main framebuffer can never go there.
    //
    // Docking rides along because the same branch provides both, and without
    // it a window dragged out has no way home: docking is how it gets put
    // back.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    // A window that has been torn off is a real window and should look like
    // one: square corners and an opaque background, not the translucent
    // rounded panel that reads correctly only when floating over the app.
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

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
    if (!map_) {
        map_ = std::make_unique<MapView>();
        // SEEDED HERE AS WELL AS IN applyConfig, because the view does not
        // exist yet when the config is applied: applyConfig runs from the
        // constructor and this runs from run(). Without this the restored
        // receiver position reached the track table (which reads the window's
        // own fields) but not the map, so a relaunch came back with a populated
        // distance column and no range rings, no RX marker and no coverage
        // overlay - found on the running application, which is the only place
        // an ordering bug between a constructor and a lazily-created view can
        // be seen at all.
        if (rxSet_) { map_->setHome(rxLat_, rxLon_); }
    }

    // Interactive runs start receiving immediately. A radio that opens with
    // dead black panels and no hint that a button must be pressed reads as
    // broken — it was the single biggest first-run complaint. Bounded
    // --frames runs stay stopped so CI keeps its old timing and never spawns
    // DSP threads it does not need.
    //
    // CASCADE_DECODE_TEST is the one exception, and it exists because the
    // decoder runner cannot be verified any other way: a decoder that is never
    // fed a sample looks exactly like a decoder that is fed and finds nothing.
    // Bounded-run only, opt-in, and off by default, so the app_smoke timing
    // contract is untouched.
    const char* decodeHook = (frames >= 0) ? std::getenv("CASCADE_DECODE_TEST") : nullptr;
    if (frames < 0 || (decodeHook != nullptr && *decodeHook != '\0')) {
        pipeline_.start();
    }

    // Bounded-run plugin-catalogue hook (see app_window.hpp). Read HERE, not
    // in the constructor, because only run() knows whether this is a bounded
    // CI run — and the hook is honored in no other mode, so an interactive
    // session can never be pointed at a different catalogue by a stray
    // environment variable.
    pluginTestHook_.clear();
    pluginTestStarted_ = false;
    pluginStatusHook_ = false;
    if (frames >= 0) {
        const char* hook = std::getenv("CASCADE_PLUGIN_TEST");
        if (hook != nullptr && *hook != '\0') { pluginTestHook_ = hook; }
        // The enforcement diagnostic (see reportPluginStatus). Same rules: only
        // in a bounded run, and silent unless asked for, so the
        // byte-identical-stdout contract of a plain --frames run is untouched.
        const char* status = std::getenv("CASCADE_PLUGIN_STATUS");
        pluginStatusHook_ = (status != nullptr && *status != '\0');
    }

    // The update check, on a worker, once, and ONLY in an interactive run.
    //
    // A bounded --frames run stays hermetic: it is what ctest and the smoke
    // tests use, and a build that reached the network during them would make
    // the suite depend on a server being up and on what that server happened
    // to say. The same rule the catalogue and config hooks already follow.
    if (frames < 0) { startUpdateCheck(); }

    // DIAGNOSTICS, armed here rather than in the constructor: the context has
    // to describe a window that exists, and the watchdog measures the frame
    // loop, so it starts when the frame loop does. Anything slow before this
    // point (GL context, backend init) is start-up, not a hang, and is
    // deliberately outside what the watchdog watches.
    // The user's stored switches, applied HERE rather than in the constructor:
    // main() decides whether this run may touch the disk at all, and it does
    // that after the constructor has run. A bounded CI run therefore keeps its
    // empty diagCrashDir_ and writes nothing, whatever the config says.
    // The same one call the Settings checkbox makes, so the arming path and
    // the mid-session toggle cannot drift apart again.
    applyDiagnosticsEnabled(diagnosticsEnabled_);
    refreshDiagContext();
    watchdog_.start(diagnosticsEnabled_ ? diagCrashDir_ : std::string());
    // ...and, on the same healthy path, anything the LAST run left on disk.
    // Started here rather than in telemetryStartup because it needs the answer
    // to "may this run touch the disk at all", which is diagCrashDir_ and is
    // only settled by main() after the constructor.
    crashUploadStart();
    cascade::core::diagLogf("frame loop starting (%s)",
                            frames >= 0 ? "bounded" : "interactive");

    // THE UNCLEAN-EXIT MARKER, FORCED TO DISK ONCE, HERE.
    //
    // telemetryCleanExit=false on disk is what tells the NEXT start that this
    // run never shut down - it is the crash counter and, now, the trigger for
    // offering a report. It only reaches the file when the config is saved,
    // and the save is debounced behind a CHANGE. The launch counter was meant
    // to supply that first change, but the debounce baseline (savedCfg_) is
    // taken after the counter has already been incremented, so the two agree
    // and nothing is written. A session that crashed before the user touched
    // anything therefore looked, on the next start, exactly like a clean exit.
    //
    // Found on the running application: launched it, killed it, read the
    // config back and it still said true. One save at the top of the frame
    // loop closes the window; the file is written once per launch either way,
    // at exit, so this costs a write that was already going to happen.
    if (!configPath_.empty()) { saveConfigNow(); }

    int rendered = 0;
    frameCounter_ = 0;
    while (!glfwWindowShouldClose(window) && !closeRequested_) {
        // Exact-count contract: check before rendering so --frames N produces
        // N frames, and --frames 0 produces none.
        if (frames >= 0 && rendered >= frames) { break; }

        // The heartbeat. One relaxed store; the whole hang-detection scheme is
        // "did this line run recently", so it must stay cheap enough that
        // nobody is ever tempted to call it less often than every frame.
        watchdog_.heartbeat(!diagSkipNextGap_);
        diagSkipNextGap_ = false;

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

        // The torn-off windows are rendered here, each into its own operating
        // system window and GL context. The current context has to be saved
        // and restored around it: RenderPlatformWindowsDefault leaves whichever
        // platform window it drew last current, and the next frame's
        // glClear/RenderDrawData above would then paint the main UI into it.
        if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            GLFWwindow* const restore = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(restore);
        }

        glfwSwapBuffers(window);
        ++rendered;

        // The context follows the session rather than being frozen at
        // start-up: a report filed after the user switched to the B200 must
        // say so. Once a second at 60 Hz, out of state that already exists.
        if ((rendered % 60) == 0) { refreshDiagContext(); }

        // --diag-toggle: flip the Settings > Diagnostics switch mid-session,
        // through the SAME function the checkbox calls, on frame 30 - before
        // the --diag-stall wedge on frame 60. There is no way to click a
        // checkbox from ctest, and the defect this exists for lived precisely
        // in the difference between what the checkbox governed and what it
        // did not, so a unit test of the watchdog cannot see it.
        if (diagToggle_ != 0 && rendered == 30) {
            const bool want = diagToggle_ > 0;
            diagToggle_ = 0;
            std::printf("cascade: --diag-toggle diagnostics %s\n", want ? "on" : "off");
            std::fflush(stdout);
            applyDiagnosticsEnabled(want);
        }

        // --diag-stall: the deliberate wedge, once, well clear of start-up.
        // This is the only way to hold the SHIPPED threshold against the REAL
        // frame loop; a unit test of the watchdog proves the class and not the
        // product.
        if (diagStallMs_ > 0 && rendered == 60) {
            const int ms = diagStallMs_;
            diagStallMs_ = 0;
            std::printf("cascade: --diag-stall wedging the frame loop for %d ms\n", ms);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            diagSkipNextGap_ = true;
        }
    }

    // Closing the window mid-take finalizes both recordings cleanly (same
    // contract as the toolbar Stop): taps out, then headers patched — before
    // the pipeline teardown below ends the sample flow they were taping.
    stopIqRecording();
    stopAudioRecording();

    // The enforcement diagnostic, printed at the END so it reports the state
    // the run actually finished in — including any retirement that a catalogue
    // fetch during the run brought into force.
    if (pluginStatusHook_) { reportPluginStatus(); }

    // Clean-exit save, unconditional: cheap, and it guarantees the on-disk
    // config matches the final session state even when the debounce never
    // fired (e.g. a change made less than 2 s before closing the window).
    // Runs before pipeline_.stop() so the snapshot reads live state.
    telemetryCleanExit_ = true;  // reached only on the normal shutdown path
    // BEFORE the save, so the sweep's dedup and rate-limit memory reaches the
    // file - and before anything else on this path, so a transfer in flight is
    // cancelled rather than waited out. This is the line that makes "a hanging
    // endpoint does not delay shutdown" true; it is measured against the real
    // binary in tests/test_crash_upload.cpp.
    crashUploadFinish();
    if (!configPath_.empty()) { saveConfigNow(); }
    cascade::core::diagLogf("frame loop ended after %d frames; shutting down", rendered);

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

    // THE WATCHDOG IS STOPPED LAST, ON PURPOSE. Everything above - the config
    // save, the pipeline join, the GL teardown - runs with no heartbeat, so a
    // shutdown that wedges is reported like any other hang. That is not an
    // oversight to be tidied away: the worst freeze this product ever shipped
    // was 120 s inside a CAT client shutdown, and stopping the watchdog before
    // the teardown would make that exact bug invisible again. A normal
    // teardown is far under the threshold, so a report from here means
    // something really did take five seconds to close.
    watchdog_.stop();

    // Printed on every clean exit: this is what makes the exact-count half of
    // the --frames contract externally observable — app_smoke matches
    // "rendered 3 frames" via PASS_REGULAR_EXPRESSION, so an off-by-one in the
    // frame bound goes red instead of shipping silently.
    std::printf("cascade: rendered %d frames\n", rendered);
    // The measurement the hang threshold is justified against, printed rather
    // than asserted in a comment: tests/test_diag_hang.cpp reads this line back
    // and requires it to be under half the shipped threshold, so a change that
    // makes a frame legitimately slow goes red here instead of arriving as a
    // false hang report on somebody's machine.
    std::printf("cascade: worst frame gap %.1f ms\n", watchdog_.worstGapMs());
    // And how many times the application took a WatchdogPause, for the same
    // reason: a false-positive mitigation that no shipped call site uses is a
    // sentence in a header, not a protection. tests/test_diag_hang.cpp reads
    // this line back and requires at least one — the plugin scan.
    std::printf("cascade: watchdog pauses %u\n", watchdog_.pausesTaken());
    return 0;
}

void AppWindow::setDiagnosticsDir(std::string crashDir) {
    diagCrashDir_ = std::move(crashDir);
}

void AppWindow::setDiagStallMs(int ms) { diagStallMs_ = (ms > 0) ? ms : 0; }

void AppWindow::setDiagToggle(int mode) { diagToggle_ = (mode > 0) ? 1 : ((mode < 0) ? -1 : 0); }

void AppWindow::refreshDiagContext() {
    cascade::core::DiagContext ctx;
    ctx.version = cascade::versionString();
    ctx.commit = cascade::gitCommit();
    ctx.os = cascade::core::osDescription();
    ctx.arch = cascade::core::archDescription();
    ctx.mode = kModeNames[modeIndex_];
    ctx.sourceKind = sourceKind_;
    ctx.sampleRateHz = pipeline_.activeSource().sampleRateHz();
    ctx.deviceOpen = (sourceKind_ == "soapy");
    // MODEL ONLY - sanitiseDevice strips the serial, exactly as the usage
    // report does. A report is a support artefact, not a hardware fingerprint.
    ctx.sdrModel = cascade::core::sanitiseDevice(soapyArgs_);
    std::size_t loaded = 0;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        ++loaded;
        if (ctx.plugins.size() < 32) { ctx.plugins.push_back(p.name + " " + p.version); }
    }
    cascade::core::setDiagContext(ctx);

    // The module table only has to be rebuilt when something LOADED CODE, and
    // a plugin load is the only thing that does so after start-up here. Doing
    // it every frame would mean walking the loader's module list 60 times a
    // second for a table that changes twice a session.
    if (loaded != diagPluginCount_) {
        diagPluginCount_ = loaded;
        cascade::core::refreshModuleTable();
    }
}

void AppWindow::drawUi() {
    // Before anything is drawn: the decoders' output is bounded in the runner
    // and must be collected whether or not the panel that shows it is open.
    pumpDecoderOutput();
    // Same contract for the web server's view of the radio: a browser must be
    // served whether or not the settings panel is expanded, and this is the
    // only thread allowed to read the source identity (see app_window.hpp).
    // Anything a browser asked for since the last frame, applied before the
    // panels are drawn so the window shows the new state this frame rather
    // than one frame late.
    applyWebControls();
    publishWebSnapshot();
    publishWebAudio();
    publishWebImages();
    pumpWebTiles();
    // Where the receiver is now decides whether the audio is silenced, and it
    // has to be decided BEFORE anything draws: the toolbar banner, the Sinks
    // panel line and the popup all report this frame's answer, and a decision
    // taken after them would show the user the previous frame's.
    //
    // After applyWebControls for the same reason that call is where it is: a
    // browser's tune has already landed, so the mute follows a web tune in the
    // same frame rather than one behind it.
    updateAudioMute();
    // Plugin windows are top-level and are drawn OUTSIDE the root window, so
    // they are movable and resizable like any other window. Drawn first so the
    // root layout below owns the remaining space.
    drawPluginWindows();

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

    // The mute dialog LAST, and outside the root window: a modal is a
    // top-level thing and opening it inside a borderless full-viewport window
    // would nest it in that window's ID stack, where the dim overlay it draws
    // would sit under the panels it is meant to block.
    drawMutePopup();

    // ...and the unclean-exit offer beside it, for the same reason: a
    // top-level thing belongs at the top level, not nested in the borderless
    // root window's ID stack.
    drawDiagnosticsOffer();

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
    pollUpdateAsync();
    // And the sink: everything above this line can be working perfectly while
    // the user hears nothing.
    pollAudioHealth();
}

void AppWindow::pollAudioHealth() {
    cascade::sink::AudioOut& out = pipeline_.audio();
    // Never opened means there is no device on this machine (or audio is
    // configured off). That is a steady state, not a fault, and retrying it
    // once a second forever would be noise.
    if (!out.everOpened()) { return; }

    // 1 Hz. Fast enough that a dropout is a hiccup rather than an outage,
    // slow enough that a genuinely absent device is not hammered with open
    // attempts. ImGui's clock is the frame clock, which is what "once per
    // second of running UI" should mean here.
    const double now = ImGui::GetTime();
    if (now - lastAudioProbeSec_ < 1.0) { return; }
    lastAudioProbeSec_ = now;

    if (out.streamAlive()) { return; }

    // The stream is dead. Re-enumerate before choosing a target: the device
    // list is how recoveryDeviceIndex() resolves the remembered NAME to a
    // current index, and the reason it is done by name is that this list can
    // renumber between the open and now.
    devices_ = out.listOutputDevices();
    const int target = cascade::sink::recoveryDeviceIndex(
        out.openedDeviceRequested(), out.openedDeviceName(), devices_);

    if (!pipeline_.openAudioDevice(target)) {
        // Say so rather than failing silently — silent failure is the exact
        // bug this whole path exists to end. The next tick tries again.
        audioHealthNote_ = "audio output stopped and could not be reopened";
        // The list above was replaced whatever happened next, and the reason
        // the stream died is usually that a device went away — so it can be
        // SHORTER than the one deviceIndex_ was chosen against. The Sinks
        // combo subscripts that row guarded only by "not empty", so leaving a
        // stale row here is an out-of-bounds read on the very next frame, on
        // the one path where nothing else touches it.
        deviceIndex_ = cascade::sink::clampDeviceRow(deviceIndex_, devices_);
        return;
    }

    ++audioRecoveries_;
    // Keep the Sinks combo honest about what is actually open. Without this
    // the panel would name the old device while audio came out of another.
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        if (devices_[static_cast<std::size_t>(i)].index == target) {
            deviceIndex_ = i;
            break;
        }
    }
    if (target < 0) {
        for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
            if (devices_[static_cast<std::size_t>(i)].isDefault) { deviceIndex_ = i; }
        }
    }
    // Neither loop above is guaranteed to assign: a target that opened but is
    // not in the list we just enumerated (it renumbered again between the two
    // calls) leaves the old row standing against the new list. Same
    // out-of-bounds, so the same clamp closes it here too.
    deviceIndex_ = cascade::sink::clampDeviceRow(deviceIndex_, devices_);
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "output device stopped and was restarted (%d time%s)",
                  audioRecoveries_, audioRecoveries_ == 1 ? "" : "s");
    audioHealthNote_ = buf;
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
    // Beside the frequency, because the banner is ABOUT the frequency: it
    // appears when the user has tuned away from a decoder's preset and kept
    // the decoder running, and the two things it has to be read together with
    // are the readout that just changed and the volume control that is not
    // the reason there is no sound.
    drawMuteBanner();
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

    // --- Typed entry (click the readout) ------------------------------------
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
        // "click", not "double-click": the handler above is IsMouseClicked and
        // has been since double-click was abandoned as unreliable on Text
        // items. A user who follows the tooltip literally double-clicks, the
        // first click opens the editor and the second lands outside it and
        // cancels — so the tooltip was teaching the one gesture that looks
        // broken.
        ImGui::SetTooltip("Scroll a digit to tune  |  click to type");
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
    drawUpdateBanner();
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
                // The MODE and its bandwidth - a demodulator change, not a
                // tuning change. No frequency reaches the log, here or
                // anywhere else.
                cascade::core::diagLogf("mode: %s, bandwidth %.0f", kModeNames[i],
                                        vfoBandwidthHz_);
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
        // Watchdog result. Shown persistently once it has happened: a user
        // whose audio died and silently came back needs to know it was the
        // output device, not the radio, or the next report reads "the radio
        // keeps going silent" and points the search at the wrong end of the
        // chain.
        if (!audioHealthNote_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s",
                               audioHealthNote_.c_str());
        }
        // WHY THERE IS NO SOUND, said where the sound settings are. This is the
        // panel a user goes to when the speakers are silent, and finding a
        // working device, a healthy watchdog and a volume that is not zero
        // there - with no explanation - is exactly the dead end the 0.58.0
        // audio investigation started from. Wrapped, because the menu column
        // is narrow and a plugin name is the plugin's to choose.
        if (!mutedBy_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
            ImGui::TextWrapped("Muted by %s", muteSubjectText().c_str());
            ImGui::PopStyleColor();
            // Said separately rather than folded into the line above, because
            // the two halves answer different questions: what is doing it, and
            // what makes it stop. The volume is untouched throughout, which is
            // the one thing a user staring at this panel will assume first.
            ImGui::TextDisabled("sound returns when the plugin stops");
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
    // Store first, then the inventory: it keeps the top-to-bottom order the
    // one combined section had (browse above installed), and the store must be
    // drawn first for pluginBrowserDrawnThisFrame_ to mean anything below.
    drawPluginStoreSection();
    drawPluginsSection();
    drawWebSection();
    drawCatSection();
    drawUpdatesSection();
    drawDiagnosticsSection();
    drawUsageReportingSection();
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


// ---------------------------------------------------------------------------
// Update check
//
// The whole point is the SECOND paragraph of what the user is shown: not "an
// update is available", which is easy to ignore, but what it fixes. When
// 0.55.0 restored radio detection there was no way to tell the people running
// a build that could not see their hardware; most of them are still running
// it. "There is a new version" would not have moved them. "Your radio cannot
// be detected on this version" might.
// ---------------------------------------------------------------------------

void AppWindow::startUpdateCheck() {
    if (updateStarted_ || updatePending_ || !updateCheckEnabled_) { return; }
    updateStarted_ = true;
    updatePending_ = true;
    updateError_.clear();
    const std::string endpoint = cascade::core::updateEndpoint();
    const std::string version = cascade::versionString();
    updateCheckFuture_ = std::async(std::launch::async, [this, endpoint, version]() {
        return cascade::core::checkForUpdate(endpoint, version, "", updateResult_,
                                             updateResultError_);
    });
}

void AppWindow::startUpdateDownload() {
    if (updatePending_ || !update_.newer) { return; }
    updatePending_ = true;
    updateDownloading_ = true;
    updateError_.clear();
    // Cleared on entry, exactly as PluginRepo::install() clears its own flags:
    // a cancel from an abandoned earlier attempt must not silently kill this
    // one, and the bar starts at zero rather than where the last try stopped.
    updateProgress_.store(0.0f, std::memory_order_relaxed);
    updateCancel_.store(false, std::memory_order_relaxed);
    const cascade::core::UpdateInfo info = update_;
    updateDownloadFuture_ = std::async(std::launch::async, [this, info]() {
        return cascade::core::downloadUpdate(info, updateResultPath_, updateResultError_,
                                             &updateProgress_, &updateCancel_);
    });
}

void AppWindow::pollUpdateAsync() {
    constexpr auto kNoWait = std::chrono::seconds(0);

    if (updatePending_ && !updateDownloading_ && updateCheckFuture_.valid() &&
        updateCheckFuture_.wait_for(kNoWait) == std::future_status::ready) {
        const bool ok = updateCheckFuture_.get();
        updatePending_ = false;
        if (ok) {
            update_ = updateResult_;
        } else {
            // A failed check is NOT shown. The user did not ask, and an
            // application that interrupts listening to say it could not reach a
            // server is worse than one that quietly tries again next launch.
            updateError_ = updateResultError_;
        }
    }

    if (updatePending_ && updateDownloading_ && updateDownloadFuture_.valid() &&
        updateDownloadFuture_.wait_for(kNoWait) == std::future_status::ready) {
        const bool ok = updateDownloadFuture_.get();
        updatePending_ = false;
        updateDownloading_ = false;
        if (ok) {
            updateReadyPath_ = updateResultPath_;
        } else {
            // A download failure IS shown: the user pressed a button and is
            // waiting for it. The message is PluginRepo's verbatim, which for a
            // digest mismatch names both digests - the one case where the
            // detail matters more than the tidiness.
            updateError_ = updateResultError_;
        }
    }
}

bool AppWindow::launchInstaller(const std::string& path) {
#if defined(_WIN32)
    // ShellExecute rather than CreateProcess: the installer asks for elevation
    // through its manifest, and only the shell will show that prompt. The
    // return is the documented "> 32 means it started" convention.
    const std::wstring wide(path.begin(), path.end());
    const HINSTANCE rc = ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr,
                                         SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(rc) > 32;
#else
    // The installer is a Windows setup program; there is nothing to launch
    // elsewhere, and saying so is better than appearing to succeed.
    (void)path;
    return false;
#endif
}

void AppWindow::drawUpdateBanner() {
    if (!updateCheckEnabled_ || updateDismissed_) { return; }
    if (!update_.newer && updateError_.empty()) { return; }
    if (!update_.newer) { return; }

    // Amber for an ordinary update, red for one that fixes a build which could
    // not do its job. The distinction is the server's `critical` flag, and it
    // is the difference between a notice and a warning.
    const ImVec4 accent = update_.critical ? kErrorRed : ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    if (update_.critical) {
        ImGui::TextWrapped("Important update: FoxSDR %s", update_.version.c_str());
    } else {
        ImGui::TextWrapped("FoxSDR %s is available", update_.version.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::TextDisabled("you are running %s", cascade::versionString());

    // WHAT IT FIXES, in the author's words, for every release between the one
    // running and the one offered. Somebody several versions behind sees all of
    // them, which is the case that matters: the people who most need this are
    // the furthest back.
    for (const cascade::core::ReleaseNote& n : update_.notes) {
        ImGui::Spacing();
        ImGui::Text("%s%s", n.version.c_str(), n.critical ? "  (important)" : "");
        for (const std::string& line : n.notes) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line.c_str());
        }
    }

    ImGui::Spacing();
    if (updateDownloading_) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Downloading...");
        // updateProgress_, not pluginRepo_.progress(): this transfer does not
        // go through a PluginRepo instance, so that value is whatever the
        // plugin browser last did — which for the common case of never having
        // opened it is 0, for the whole download.
        ImGui::ProgressBar(updateProgress_.load(std::memory_order_relaxed),
                           ImVec2(-FLT_MIN, 0.0f));
    } else if (!updateReadyPath_.empty()) {
        // Downloaded AND verified. Running it closes this application, which
        // the button says outright rather than surprising anyone: an installer
        // cannot replace a binary that is still running.
        ImGui::TextWrapped("Downloaded and verified.");
        if (ImGui::Button("Install now and restart", ImVec2(-FLT_MIN, 0.0f))) {
            if (launchInstaller(updateReadyPath_)) {
                requestClose();
            } else {
                updateError_ = "could not start the installer at " + updateReadyPath_;
            }
        }
        ImGui::TextDisabled("%s", updateReadyPath_.c_str());
    } else {
        ImGui::BeginDisabled(updatePending_);
        if (ImGui::Button("Download update", ImVec2(-FLT_MIN, 0.0f))) { startUpdateDownload(); }
        ImGui::EndDisabled();
        if (update_.sizeBytes > 0) {
            // Wrapped, not TextDisabled: the sidebar is narrow and the
            // unwrapped line was cut off mid-word at "verified against its
            // publis", which reads as a rendering fault rather than a promise.
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("%.1f MB, verified against its published checksum before anything runs",
                               static_cast<double>(update_.sizeBytes) / 1.0e6);
            ImGui::PopStyleColor();
        }
    }
    if (ImGui::SmallButton("Not now")) { updateDismissed_ = true; }

    if (!updateError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", updateError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
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

    // NO HARDWARE FOUND, explained.
    //
    // This block is here because its absence was, for several releases, the
    // whole of what a user with a radio was told: the dropdown listed the
    // signal generator and the IQ file, their receiver was simply not there,
    // and nothing anywhere said why or what to do. That is the worst possible
    // failure to present, because it is indistinguishable from the
    // application not supporting their device at all - and it was shown to
    // users whose only mistake was installing FoxSDR where its own module
    // search path could not reach their vendor modules.
    //
    // Shown only after a scan has actually completed and found nothing, so it
    // never flashes up during the first enumeration.
    if (soapyScanned_ && !soapyBusy && soapyDevices_.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "No radio hardware found");
        ImGui::TextWrapped(
            "FoxSDR reaches radios through SoapySDR vendor modules, which are a separate "
            "install. The signal generator and IQ file playback need no hardware.");

        const std::vector<std::string> modules = cascade::source::SoapySource::loadedModules();
        const std::vector<cascade::source::VendorRoot> vendors =
            cascade::source::SoapySource::vendorInstalls();

        if (modules.empty()) {
            // The common case, and the one worth being loudest about.
            ImGui::TextWrapped(
                "No vendor modules are loaded. Install PothosSDR "
                "(downloads.myriadrf.org/builds/PothosSDR) or radioconda "
                "(github.com/ryanvolz/radioconda), then press Refresh. Neither needs FoxSDR "
                "to be reinstalled.");
        } else {
            ImGui::TextWrapped("%d vendor module%s loaded, but none reported a device.",
                               static_cast<int>(modules.size()),
                               modules.size() == 1u ? " is" : "s are");
        }

        // THE RTL-SDR CASE, called out by name. It is the most common first
        // receiver by a wide margin, and on Windows it needs a step that no
        // other device needs and that nothing in this product used to mention:
        // the dongle ships bound to the DVB-T television driver, under which
        // it is invisible to every SDR application, not only this one.
        ImGui::TextWrapped(
            "RTL-SDR dongle? Windows also needs a WinUSB driver bound to it. Run Zadig "
            "(zadig.akeo.ie), tick Options -> List All Devices, select \"Bulk-In, Interface "
            "(Interface 0)\", choose WinUSB and click Replace Driver. Until that is done the "
            "dongle is invisible to every SDR application. In Device Manager an unconfigured "
            "dongle shows as \"Bulk-In, Interface\" with a yellow warning.");

        // The evidence, folded away. A user does not need it; anyone helping
        // them does, and "where did it look" is the first question worth
        // asking - it is the question that found the bug this text exists for.
        if (ImGui::TreeNode("Where FoxSDR looked")) {
            if (!vendors.empty()) {
                for (const cascade::source::VendorRoot& v : vendors) {
                    ImGui::TextWrapped("found %s at %s", v.name.c_str(), v.root.c_str());
                }
            } else {
                ImGui::TextDisabled("no vendor SDR installation detected");
            }
            for (const std::string& p : cascade::source::SoapySource::moduleSearchPaths()) {
                ImGui::TextWrapped("search: %s", p.c_str());
            }
            for (const std::string& m : modules) {
                ImGui::TextWrapped("module: %s", m.c_str());
            }
            ImGui::TreePop();
        }
        ImGui::Separator();
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
                ++sourceGen_;  // a device open still in flight is now stale
                pipeline_.setSource(std::move(file));
                sourceKind_ = "file";
                iqOpenPath_ = iqPath_;
                followInputRate();  // DSP chain + frequency axis track the file's rate
                // The RATE, never the path: a file name is the user's own data
                // and a report is a support artefact, not a listening record.
                cascade::core::diagLogf("source: opened an I/Q file at %.0f S/s",
                                        pipeline_.activeSource().sampleRateHz());
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

        // ANTENNA. Above the gain controls on purpose: no amount of gain
        // rescues the wrong port, and picking the wrong one gives a receiver
        // that looks entirely healthy - spectrum moving, samples flowing -
        // while hearing essentially nothing. Measured on a B200 at 1090 MHz,
        // the difference between the two ports was 16 dB of signal-to-noise
        // and the difference between decoding aircraft and decoding none.
        if (soapyAntennas_.size() > 1) {
            if (ImGui::BeginCombo("Antenna", soapyAntenna_.c_str())) {
                for (const std::string& a : soapyAntennas_) {
                    const bool sel = (a == soapyAntenna_);
                    if (ImGui::Selectable(a.c_str(), sel) && !sel) {
                        if (soapy_->setAntenna(a)) {
                            // Read BACK rather than assuming the request took:
                            // a driver may coerce, and the panel must show the
                            // port actually in use.
                            // The debounced save notices via configsEqual,
                            // which now compares soapyAntenna - no explicit
                            // dirty flag exists, and adding one here would be
                            // a second mechanism doing the same job.
                            soapyAntenna_ = soapy_->antenna();
                        } else {
                            sourceError_ = soapy_->lastError();
                        }
                    }
                    if (sel) { ImGui::SetItemDefaultFocus(); }
                }
                ImGui::EndCombo();
            }
        } else if (!soapyAntenna_.empty()) {
            // One port, nothing to choose - but still shown, because "which
            // antenna am I on" should never be a question the UI cannot answer.
            ImGui::Text("Antenna: %s", soapyAntenna_.c_str());
        }

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

void AppWindow::reapPendingSoapyOpen() {
    if (!soapyOpenPending_ || !soapyOpenFuture_.valid()) { return; }

    // WHY THIS EXISTS. std::future's destructor for a std::async(launch::async)
    // task BLOCKS until the worker returns, and soapyOpenFuture_ is a member,
    // so quitting while a device open was in flight parked the GUI thread
    // inside ~AppWindow for the whole of SoapySDR::Device::make() — seconds on
    // a healthy B200, and unbounded against a device that is wedged or has
    // been unplugged mid-open. The window is already gone by then, so it
    // presents as the application hanging after it closed.
    //
    // Grace period first: an open that is already finished (or a few
    // milliseconds from it) is reaped right here, which keeps the common case
    // free of an extra thread and of the process-exit race below. 250 ms is
    // short enough not to be felt and long enough to cover every open that was
    // not actually stuck.
    constexpr auto kQuitGrace = std::chrono::milliseconds(250);
    if (soapyOpenFuture_.wait_for(kQuitGrace) == std::future_status::ready) {
        drainSoapyOpen(soapyOpenFuture_);
        soapyOpenPending_ = false;
        return;
    }

    // Still inside make(). THE CONSERVATIVE CHOICE, stated explicitly because
    // it is a trade and not a free win: the pending work is moved onto a
    // detached reaper so quit stays responsive, and the reaper's only job is
    // to take the result and DESTROY it — SoapyOpenResult owns the
    // SoapySource, whose destructor closes the device, so an open that
    // completes after quit still releases its handle rather than leaking it
    // for the lifetime of the process.
    //
    // What it does not promise: if the process exits before the reaper
    // finishes, Windows terminates that thread wherever it happens to be and
    // the handle is released by the operating system with the process instead.
    // That is the accepted cost — the alternative on offer is the hang above,
    // and no amount of waiting can bound a driver call that is not going to
    // return.
    std::thread([f = std::move(soapyOpenFuture_)]() mutable {
        drainSoapyOpen(f);
    }).detach();
    soapyOpenPending_ = false;
}

void AppWindow::reapPendingSoapyScan() {
    if (!soapyScanPending_ || !soapyScanFuture_.valid()) { return; }

    // THE SAME BLOCKING DESTRUCTOR AS reapPendingSoapyOpen, on the other
    // future. std::async(launch::async) futures block in ~future until the
    // worker returns, and soapyScanFuture_ is a member, so quitting while the
    // lazy scan was in flight parked the GUI thread inside ~AppWindow for the
    // whole of SoapySDR::Device::enumerate(). That call walks every registered
    // vendor module's discovery routine — it is multi-second on a healthy
    // machine with several drivers installed and unbounded against a wedged
    // one — and the window is already gone by then, so it presents as the
    // application hanging after it closed. The scan is started lazily the
    // first time the source combo is opened, which is a moment away from the
    // user deciding there is no radio here and quitting.
    //
    // Grace period first, for the same reason: a scan a few milliseconds from
    // finishing is reaped right here, keeping the common case free of an extra
    // thread and of the process-exit race below.
    constexpr auto kQuitGrace = std::chrono::milliseconds(250);
    if (soapyScanFuture_.wait_for(kQuitGrace) == std::future_status::ready) {
        drainSoapyScan(soapyScanFuture_);
        soapyScanPending_ = false;
        return;
    }

    // Still inside enumerate(). Moved onto a detached reaper so quit stays
    // responsive. WHERE THIS DIFFERS FROM THE OPEN REAPER, and it is worth
    // being explicit because the two look identical: the open reaper has a
    // real duty after quit — its result owns the device handle, and dropping
    // it is what closes the radio. A scan result is an enumeration list and
    // owns no handle at all, so this reaper releases nothing; it exists purely
    // so the wait happens off the GUI thread. If the process exits first the
    // thread is terminated wherever it stands and nothing is left behind that
    // the operating system would not have reclaimed anyway.
    std::thread([f = std::move(soapyScanFuture_)]() mutable {
        drainSoapyScan(f);
    }).detach();
    soapyScanPending_ = false;
}

void AppWindow::finishSoapyOpen(SoapyOpenResult r) {
    // THE USER MAY HAVE MOVED ON. Opening a device takes seconds and the GUI
    // stays live throughout, so by the time this runs they may have selected
    // the generator, opened an IQ file, or done either from the web UI. Before
    // this check the finished open was applied regardless and the radio
    // changed itself several seconds after being told otherwise — including
    // installing a device over a file the user was already listening to.
    //
    // Dropping `r` here also closes the device: r.dev owns the SoapySource,
    // and its destructor is what releases the handle the worker acquired. The
    // error string is dropped with it on purpose — it describes a device the
    // user is no longer asking about, and showing it under the source they DID
    // choose would read as a fault in that source.
    if (!asyncOpenStillWanted(soapyOpenReqGen_, sourceGen_)) { return; }

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
    // Antenna: restore the saved port if this device still has one by that
    // name, otherwise leave the driver's default alone and just report what
    // it chose. Never guessed at - which port carries an antenna is a fact
    // about the user's cabling that no default can know.
    soapyAntennas_ = r.dev->listAntennas();
    if (!soapyAntenna_.empty()) { r.dev->setAntenna(soapyAntenna_); }
    soapyAntenna_ = r.dev->antenna();

    soapy_ = r.dev.get();
    soapyArgs_ = r.args;
    ++sourceGen_;  // this install is itself a source change
    pipeline_.setSource(std::move(r.dev));
    sourceKind_ = "soapy";
    sourceSel_ = r.row;
    // SERIAL STRIPPED, exactly as everywhere else this string is recorded.
    // "which radio, at what rate" is the single most useful line in the run-up
    // to a fault, because vendor SDR modules are third-party code running
    // in-process and the rate decides whether the chain keeps up.
    cascade::core::diagLogf("source: opened %s at %.0f S/s",
                            cascade::core::sanitiseDevice(soapyArgs_).c_str(),
                            pipeline_.activeSource().sampleRateHz());

    // CARRY THE FREQUENCY ACROSS, which is the whole difference between
    // changing radio and losing what you were listening to.
    //
    // A freshly opened device sits at its driver's default - an RTL-SDR comes
    // up at 100 MHz - so before this, switching from a B200 tuned to 97 MHz
    // put the receiver on 100 MHz without saying so. The spectrum went empty,
    // the level fell to the noise floor, the squelch stayed shut, and the
    // audio stopped: it reads as "changing device breaks the sound" rather
    // than "your radio is now tuned somewhere else".
    if (r.keepCenterHz > 0.0) {
        retuneSourceHz(r.keepCenterHz);
        // Not every radio covers every band, so say so rather than leaving
        // the user on a frequency they did not choose. The readback is the
        // authority - a device may clamp to its range or land on a nearby
        // tuning step.
        const double landed = pipeline_.activeSource().centerFrequencyHz();
        if (std::fabs(landed - r.keepCenterHz) > 1000.0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "this radio could not tune %.6f MHz; it is on %.6f MHz",
                          r.keepCenterHz / 1e6, landed / 1e6);
            sourceError_ = buf;
        }
    }

    followInputRate();  // DSP chain follows the device's actual readback
}

void AppWindow::selectSource(int idx) {
    if (idx == sourceSel_) { return; }  // re-click on the current row: no-op
    sourceError_.clear();

    if (idx == 0) {
        // Built-in generator: null restores it, and it cannot fail.
        soapy_ = nullptr;  // before setSource destroys a live Soapy source
        soapyArgs_.clear();
        ++sourceGen_;  // a device open still in flight is now stale
        pipeline_.setSource(nullptr);
        sourceKind_ = "siggen";
        sourceSel_ = 0;
        followInputRate();  // back to the generator's fixed 2 MS/s
        cascade::core::diagLogf("source: switched to the built-in generator");
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
    // Read the tuned frequency NOW: the old source is destroyed when the new
    // one is installed, and a device that has never been opened reports 0,
    // which finishSoapyOpen treats as "nothing to carry".
    const double keepCenterHz = pipeline_.activeSource().centerFrequencyHz();
    soapyBusyLabel_ = soapyDevices_[d].label;
    soapyOpenPending_ = true;
    // Stamp the request with the selection it belongs to. Nothing is installed
    // yet, so the counter is NOT bumped here — only the answer's right to be
    // applied is recorded.
    soapyOpenReqGen_ = sourceGen_;
    soapyOpenFuture_ = std::async(std::launch::async, [args, rate, idx, keepCenterHz] {
        SoapyOpenResult r;
        r.args = args;
        r.row = idx;
        r.requestRateHz = rate;
        r.keepCenterHz = keepCenterHz;
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
    // Same antenna handling as the Source-menu path: apply a saved port if the
    // device has it, then read back whatever is actually selected so the panel
    // never claims a port the driver did not accept.
    soapyAntennas_ = dev->listAntennas();
    if (!soapyAntenna_.empty()) { dev->setAntenna(soapyAntenna_); }
    soapyAntenna_ = dev->antenna();
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

    // Every source change funnels through here, which makes it the one place
    // that can keep the decoder instances honest about the rate and centre
    // they were built for. Cheap when no plugins are loaded, and it must run
    // even when the rate was REFUSED above: the centre frequency has usually
    // moved regardless, and a decoder told the wrong one reports confidently
    // wrong things.
    refreshPluginRunner();
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
    telemetryNotePanel("audio filters");

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

const char* AppWindow::pluginQuarantineSuffix() { return ".disabled"; }

bool AppWindow::restoreQuarantinedPlugins(std::string& error) {
    error.clear();
    const std::string suffix = pluginQuarantineSuffix();
    std::error_code ec;
    const std::filesystem::path dir(pluginDir_);
    if (!std::filesystem::is_directory(dir, ec)) { return true; }

    // Collected first, renamed after: renaming inside a directory_iterator
    // walk is the classic way to get an implementation-defined half-listing.
    std::vector<std::string> quarantined;
    for (auto it = std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) { continue; }
        const std::string name = it->path().filename().string();
        if (name.size() <= suffix.size()) { continue; }
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) { continue; }
        // The live name this would restore to must be a name install() could
        // itself have written. Anything else is not ours and is left alone —
        // the same "unmanaged means unmanaged" rule the inventory follows.
        const std::string base = name.substr(0, name.size() - suffix.size());
        std::string safe;
        std::string sanErr;
        if (!cascade::core::PluginRepo::sanitiseFileName(base, safe, sanErr)) { continue; }
        quarantined.push_back(safe);
    }

    for (const std::string& file : quarantined) {
        const std::filesystem::path live = dir / file;
        const std::filesystem::path aside = dir / (file + suffix);
        std::error_code rec;
        if (std::filesystem::exists(live, rec)) {
            // An update (or a hand-install) landed while this copy was aside.
            // The live file is the one that counts; the leftover is stale
            // bytes under a hidden name, so it goes.
            std::filesystem::remove(aside, rec);
            if (rec) {
                error = "cannot delete the superseded \"" + aside.string() + "\": " + rec.message();
            }
            continue;
        }
        std::filesystem::rename(aside, live, rec);
        if (rec) {
            error = "cannot re-enable \"" + live.string() + "\": " + rec.message();
        }
    }
    return error.empty();
}

bool AppWindow::quarantineBlockedPlugins(std::string& error) {
    error.clear();
    const std::string suffix = pluginQuarantineSuffix();
    const std::filesystem::path dir(pluginDir_);
    for (const cascade::core::BlockedPlugin& b : pluginBlocked_) {
        std::string safe;
        std::string sanErr;
        if (!cascade::core::PluginRepo::sanitiseFileName(b.installed.file, safe, sanErr)) {
            // A record whose file name is not sanitisable names no file this
            // product could have installed, so there is nothing on disk to
            // move; the row is still shown as blocked.
            continue;
        }
        const std::filesystem::path live = dir / safe;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(live, ec)) { continue; }
        const std::filesystem::path aside = dir / (safe + suffix);
        std::filesystem::remove(aside, ec);  // a leftover from a crashed run
        std::filesystem::rename(live, aside, ec);
        if (ec) {
            error = "\"" + safe + "\" is out of date but could not be disabled (" +
                    ec.message() + "), so NO plugins were loaded this time.";
            return false;
        }
    }
    return true;
}

void AppWindow::detachAndUnloadPlugins() {
    // THE ORDER IS THE FEATURE, and it is why this is a function rather than
    // an open-coded sequence: it was open-coded, removeInstalledPlugin() then
    // called unloadAll() on its own, and the careful ordering below existed in
    // only one of the two places that unmap plugin modules.
    //
    // The RUNNER comes off first, in two steps. Detaching it from the pipeline
    // stops the DSP thread reaching it; clearing it then destroys the decoder
    // instances. Both must complete before unloadAll(), because a live handle
    // is memory inside a module about to be unmapped, and destroy() is code
    // inside that same module. Getting this order wrong is a crash in someone
    // else's DLL with no useful stack.
    pipeline_.setPluginRunner(nullptr);
    pluginRunner_.clear();
    // Same rule as the runner: a track-source or panel handle is memory inside
    // a module whose destroy() is code in that same module.
    pluginUi_.clear();
    // And the basemap, for exactly the same reason - its handle and its tile
    // borrows live in a module about to be unmapped.
    basemap_.detach();
    trackInfo_.detach();

    pluginHost_.unloadAll();
}

void AppWindow::rescanPlugins() {
    // BLOCKING WORK THE APPLICATION ENTERS KNOWINGLY, and the one such path
    // this application actually has on its GUI thread: unloading and then
    // LoadLibrary-ing every installed plugin, off a disk that may be cold, a
    // network profile, or a drive that is spinning up. Twelve modules is a
    // normal count. That is a legitimate multi-second gap in the frame loop
    // and it is not a hang, so the watchdog is paused across it — which is
    // also what keeps the watchdog from suspending a thread that is inside the
    // loader (see the phase-1 note in hang_watchdog.cpp).
    //
    // Scope guard, because there is a `return` in the middle of this function.
    cascade::core::WatchdogPause holdWatchdog(watchdog_);

    // A missing plugins directory is the normal case and yields an empty list
    // without an error — the host's documented behaviour, and the reason
    // nothing here reports a failure.
    pluginDir_ = cascade::core::PluginHost::defaultPluginDir();

    // ONE ordered sequence, and the order is the feature (see the enforcement
    // note in app_window.hpp). Nothing may hold a mapped module while files
    // are renamed, so the unload comes first — the same unload-then-touch-the
    // -file rule removeInstalledPlugin already follows.
    //
    // The RUNNER comes off before any of that, and in two steps. Detaching it
    // from the pipeline stops the DSP thread reaching it; clearing it then
    // destroys the decoder instances. Both must complete before unloadAll(),
    // because a live handle is memory inside a module that is about to be
    // unmapped, and destroy() is code inside that same module. Getting this
    // order wrong is a crash in someone else's DLL with no useful stack.
    detachAndUnloadPlugins();
    pluginEnforceError_.clear();

    // Un-quarantine BEFORE taking the inventory. Reconciliation and
    // planUpdates both key off "is the file there", and a plugin that this
    // code renamed aside last frame would otherwise read as deleted by the
    // user — which planUpdates deliberately refuses to update, taking away the
    // one remedy a retired plugin has.
    std::string restoreError;
    if (!restoreQuarantinedPlugins(restoreError)) { pluginEnforceError_ = restoreError; }

    std::string invError;
    // A corrupt or absent manifest is an ordinary state and its own kind of
    // fail-open: nothing is recorded, so nothing is retired.
    (void)cascade::core::PluginRepo::loadInventory(pluginDir_, pluginInventory_, invError);
    pluginBlocked_ = cascade::core::PluginRepo::blockedPlugins(pluginInventory_.plugins,
                                                               pluginInventory_.policies);

    std::string quarantineError;
    if (!quarantineBlockedPlugins(quarantineError)) {
        // FAIL CLOSED. Scanning now would map the retired plugin along with
        // everything else, which is the precise state this feature exists to
        // prevent, so this run gets no plugins at all and says why.
        pluginEnforceError_ = quarantineError;
        return;
    }

    pluginHost_.scan(pluginDir_);

    // WHICH PLUGINS ARE MAPPED, one line each, because plugins are
    // third-party code running in this process and "which one was loaded" has
    // already been the answer to real faults here. A load that FAILS is worth
    // more than one that succeeds, so both are recorded.
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded) {
            cascade::core::diagLogf("plugin: loaded %s %s", p.name.c_str(),
                                    p.version.c_str());
        } else {
            // A refused plugin has no descriptor, so name and version are
            // empty - the FILE is the only thing that identifies it.
            const std::string leaf =
                std::filesystem::path(p.path).filename().string();
            cascade::core::diagWarnf("plugin: NOT loaded %s (%s)", leaf.c_str(),
                                     p.error.empty() ? "no reason given" : p.error.c_str());
        }
    }
    // The module table has just changed - new code is mapped, and a fault
    // inside it would otherwise resolve to "?" with a bare address.
    cascade::core::refreshModuleTable();

    // Instances are created only after the scan has settled, and the pipeline
    // is only pointed at the runner once they exist — so the DSP thread never
    // sees a half-built set.
    refreshPluginRunner();
}

std::vector<cascade::core::PluginUpdate> AppWindow::plannedPluginUpdates() const {
    // Pure, and empty until the user has fetched a catalogue this session:
    // catalog_ is only ever filled by the Browse button.
    return cascade::core::PluginRepo::planUpdates(catalog_, pluginInventory_.plugins);
}

void AppWindow::drawPluginStoreSection() {
    // Reset BEFORE the header test, not inside it: a collapsed store draws no
    // browser, and the installed section below has to know that so the result
    // text does not fall down the gap between the two sections.
    pluginBrowserDrawnThisFrame_ = false;

    // "###pluginstore" for the same reason the section below fixes its own ID:
    // a label that may grow a suffix later must not take the open/closed state
    // with it. Nothing persists that state between runs — the app runs with
    // ImGui's ini file disabled (see the IniFilename assignment in init) — so
    // both sections simply start closed on every launch, as they always have.
    if (!ImGui::CollapsingHeader("Plugin store###pluginstore")) { return; }
    telemetryNotePanel("plugin store");

    // The toggle stays a toggle rather than becoming the header itself. The
    // header answers "do I want to think about plugins at all"; this answers
    // "may the catalogue origin be contacted", it is persisted across runs
    // (AppConfig::pluginBrowserOpen), and collapsing it is how a user parks the
    // store without losing the section.
    if (ImGui::Button(pluginBrowseOpen_ ? "Hide browser" : "Get plugins",
                      ImVec2(-FLT_MIN, 0.0f))) {
        // Opening the browser is NOT a fetch. The view appears with an empty
        // list and a Browse button; the catalogue origin is contacted only
        // when that button is pressed.
        pluginBrowseOpen_ = !pluginBrowseOpen_;
    }

    if (pluginBrowseOpen_) {
        // Still noted separately from "plugin store": expanding the section is
        // a different act from opening the view that can reach the network, and
        // the second is the one worth counting.
        telemetryNotePanel("plugin browser");
        pluginBrowserDrawnThisFrame_ = true;
        drawPluginBrowser();
    }
}

void AppWindow::drawPluginsSection() {
    // THE BADGE. A user who never expands this section still has to learn that
    // something they installed has stopped working — a silently shorter list
    // is the same mystery the plugin host's per-file reasons exist to avoid.
    // blockedCount() is the badge-cheap form of the same predicate the rows
    // use, so the count and the rows can never disagree.
    const std::size_t blocked = cascade::core::PluginRepo::blockedCount(
        pluginInventory_.plugins, pluginInventory_.policies);
    char header[64];
    if (blocked == 0u) {
        // "###plugins" fixes the widget ID, so the open/closed state survives
        // the label changing when a plugin is retired or updated.
        std::snprintf(header, sizeof(header), "Plugins###plugins");
    } else {
        std::snprintf(header, sizeof(header), "Plugins (%d disabled)###plugins",
                      static_cast<int>(blocked));
    }
    if (!ImGui::CollapsingHeader(header)) { return; }
    telemetryNotePanel("plugins");

    ImGui::TextDisabled("%s", pluginDir_.c_str());
    // Full width now that "Get plugins" has gone to the store section: Rescan
    // is a local, offline re-read of the plugins folder and is the only button
    // this section opens with.
    if (ImGui::Button("Rescan", ImVec2(-FLT_MIN, 0.0f))) { rescanPlugins(); }

    drawBlockedPluginRows();

    // The decoder output has its OWN WINDOW (see drawDecoderWindow); what is
    // left here is the button that opens it and the idle reasons, which
    // belong next to the plugin list because they are about installation
    // rather than about traffic.
    drawDecoderStatusRows();
    drawPluginTuneControls();

    ImGui::SeparatorText("Installed");
    const std::vector<cascade::core::LoadedPlugin>& list = pluginHost_.plugins();
    if (list.empty()) {
        ImGui::TextDisabled("No plugins installed");
        // Still report a remove that just emptied the list — or one that
        // failed, which is exactly when the user needs to be told.
        if (!pluginBrowserDrawnThisFrame_) { drawPluginResultText(); }
        return;
    }
    // Removal is deferred past the loop: removeInstalledPlugin() rescans,
    // which replaces the very vector this loop is walking. The same goes for
    // a stop or a start, which rebuilds every instance in the process.
    std::string removeFile;
    std::string toggleStopFile;
    bool toggleStopTo = false;
    // The mute toggle is deferred by INDEX rather than by file name, because
    // setPluginMutes needs the plugin's capabilities to know what the default
    // it is overriding actually is, and only the record carries those.
    int toggleMuteIdx = -1;
    bool toggleMuteTo = false;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const cascade::core::LoadedPlugin& p = list[i];
        const std::string file = cascade::core::pluginKey(p);
        const bool stopped = pluginIsStopped(file);
        ImGui::PushID(static_cast<int>(i));
        if (p.loaded) {
            ImGui::Text("%s %s", p.name.c_str(), p.version.c_str());
            // STOPPED IS SAID ON THE ROW, in the warning colour the idle
            // reasons use, because the whole failure this feature could
            // introduce is a plugin that produces nothing for a reason the
            // user has forgotten they chose.
            //
            // WRAPPED, like the idle reasons and unlike the name above it.
            // The menu column is narrow and the user can make it narrower;
            // measured on the running application at 264 px, TextColored cut
            // this sentence off mid-word at the panel edge, which is a poor
            // way to deliver the one line that explains why a decoder is
            // silent.
            if (stopped) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
                ImGui::TextWrapped("Stopped - loaded, but running nothing");
                ImGui::PopStyleColor();
            }
            if (!p.author.empty()) { ImGui::TextDisabled("by %s", p.author.c_str()); }
            // The LICENCE is displayed, always and unconditionally. A plugin
            // is third-party code loaded into a commercially sold binary:
            // what terms it arrived under is the user's business and must not
            // require digging.
            ImGui::TextDisabled("licence: %s", p.licence.c_str());
            drawPluginPresets(p);
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
        // STOP / START, beside Remove and in the same small-button idiom,
        // because they answer the same question ("I do not want this plugin
        // running") at two very different costs. Stop is instant and
        // reversible and therefore needs no confirmation; Remove deletes a
        // downloaded file and keeps its two-step.
        //
        // The LABEL IS THE ACTION, never the state: a button saying "Running"
        // leaves the user guessing whether pressing it stops the plugin or
        // is simply a badge. The state is the orange line above.
        //
        // Offered only for a plugin that actually loaded: a refused candidate
        // has no instances to stop, and a Stop button on it would imply the
        // reason it is silent is something the user did.
        if (p.loaded) {
            ImGui::SameLine();
            if (ImGui::SmallButton(stopped ? "Start" : "Stop")) {
                toggleStopFile = file;
                toggleStopTo = !stopped;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(stopped ? "Create this plugin's decoders, map targets "
                                            "and windows again"
                                          : "Destroy this plugin's decoders, map targets "
                                            "and windows. It stays installed, and stays "
                                            "stopped until you start it.");
            }
            // MUTE AUDIO WHILE RUNNING, per plugin, defaulted from what the
            // plugin consumes rather than from a global preference.
            //
            // A checkbox and not a hidden rule, because the right answer
            // differs by plugin and only the user knows which they want. An
            // I/Q decoder leaves behind a demodulated channel that is not the
            // signal being decoded - hiss, at whatever the volume is set to -
            // so it defaults ON; an audio decoder is fed the very audio the
            // speakers get, and SSTV's warble is how people tune it by ear, so
            // it defaults OFF. Neither default is right for everybody, which
            // is the entire reason this is on the row.
            //
            // Deferred past the loop like the stop toggle: recording it
            // rebuilds the mute snapshot, which walks the very list being
            // iterated.
            bool mutes = pluginMutes(p);
            if (ImGui::Checkbox("Mute audio while running", &mutes)) {
                toggleMuteIdx = static_cast<int>(i);
                toggleMuteTo = mutes;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Silence the speakers and the web audio stream while "
                                  "this plugin is actually decoding and the receiver is "
                                  "on one of its presets. A stopped or idle plugin "
                                  "silences nothing. The volume setting is not touched.");
            }
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (toggleMuteIdx >= 0 && static_cast<std::size_t>(toggleMuteIdx) < list.size()) {
        setPluginMutes(list[static_cast<std::size_t>(toggleMuteIdx)], toggleMuteTo);
    }
    if (!toggleStopFile.empty()) { setPluginStopped(toggleStopFile, toggleStopTo); }
    if (!presetNote_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.9f, 0.55f, 1.0f));
        ImGui::TextWrapped("%s", presetNote_.c_str());
        ImGui::PopStyleColor();
    }
    if (!removeFile.empty()) { removeInstalledPlugin(removeFile); }
    // Only when the browser was not drawn this frame: with it on screen the
    // same text has already been drawn under the Install button, and printing
    // it twice in one column reads as two separate failures. The test is
    // "drawn", not "open", because the store section can be collapsed over an
    // open browser — and then this is the only place the text can appear.
    if (!pluginBrowserDrawnThisFrame_) { drawPluginResultText(); }
}

void AppWindow::drawBlockedPluginRows() {
    // An enforcement failure outranks everything else on this panel: it is the
    // one state where a retired plugin might otherwise have been loaded, and
    // the answer taken (load nothing) is drastic enough that it must be said.
    if (!pluginEnforceError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", pluginEnforceError_.c_str());
        ImGui::PopStyleColor();
    }
    if (pluginBlocked_.empty()) { return; }

    ImGui::SeparatorText("Disabled");
    const std::vector<cascade::core::PluginUpdate> updates = plannedPluginUpdates();
    const bool busy = catalogPending_ || installPending_;
    std::string removeBlockedFile;
    for (std::size_t i = 0; i < pluginBlocked_.size(); ++i) {
        const cascade::core::BlockedPlugin& b = pluginBlocked_[i];
        ImGui::PushID(static_cast<int>(1000 + i));
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        // VERBATIM. pluginBlockMessage() is user copy, not a log line: it
        // already names the plugin, the version installed, what is required
        // and what to do, and it deliberately says different things for a
        // version floor and an ABI break. Re-wording it here would give the
        // product two answers to the same question.
        ImGui::TextWrapped("%s", b.message.c_str());
        ImGui::PopStyleColor();

        // Update is offered for EVERY blocked reason for which a plan exists,
        // including an ABI mismatch.
        //
        // That is a change of policy, and worth stating. It used to be that an
        // ABI mismatch got no Update button on the grounds that no catalogue
        // update could fix one - true while the catalogue held a single ABI,
        // because the only build on offer was the one already installed. Now
        // that a rebuilt plugin is published for the new ABI, the catalogue is
        // exactly what fixes it: planUpdates has always treated "same version,
        // wrong ABI" as update-worthy (see its `have->abiVersion` branch), and
        // suppressing the button here was hiding the remedy that existed.
        {
            const cascade::core::PluginUpdate* plan = nullptr;
            for (const cascade::core::PluginUpdate& u : updates) {
                if (u.id == b.installed.id) { plan = &u; break; }
            }
            if (plan != nullptr) {
                ImGui::BeginDisabled(busy);
                if (ImGui::Button("Update")) { startUpdate(*plan); }
                ImGui::EndDisabled();
            } else {
                // No Update button, because there is nothing to click yet: a
                // plan needs a catalogue, and nothing fetches one until the
                // user asks. Say where the button lives rather than showing a
                // dead one — and it now lives in a DIFFERENT SECTION, so the
                // directions have to name that section or they send the user
                // hunting through this one for a button that left it.
                ImGui::TextDisabled("Open \"Plugin store\", press \"Get plugins\", "
                                    "then Browse, to fetch the update.");
            }
        }

        // Remove, on the other hand, must be offered on EVERY blocked row. For
        // an ABI mismatch it is the only action that exists, and without it the
        // user is in a dead end: the plugin cannot load, cannot be updated into
        // loading, and cannot be got rid of without finding the plugins folder
        // and deleting a file whose name ends in ".disabled". That dead end
        // would arrive for every installed plugin at once on the next ABI bump.
        //
        // Same two-step confirmation as the installed list, and a separate
        // index because the two lists are drawn in the same frame and one
        // shared "which row is confirming" would arm a row in both.
        if (blockedRemoveConfirmIdx_ == static_cast<int>(i)) {
            ImGui::TextWrapped("Delete %s?", b.installed.file.c_str());
            if (ImGui::Button("Confirm delete")) {
                removeBlockedFile = b.installed.file;
                blockedRemoveConfirmIdx_ = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##rmblocked")) { blockedRemoveConfirmIdx_ = -1; }
        } else if (ImGui::SmallButton("Remove")) {
            blockedRemoveConfirmIdx_ = static_cast<int>(i);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    // Deferred past the loop for the same reason as the installed list:
    // removeBlockedPlugin() rescans, which rebuilds the very vector being
    // walked here.
    if (!removeBlockedFile.empty()) { removeBlockedPlugin(removeBlockedFile); }
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

    // --- Updates available ---------------------------------------------------
    //
    // OPT-IN, and it stays opt-in. This list is a pure function of the
    // catalogue the user just fetched and what is installed; nothing here
    // starts a fetch, and no plugin updates itself. One button, one plugin,
    // one user decision — which is the same rule install() follows, because an
    // update IS an install and it is the riskiest one.
    const std::vector<cascade::core::PluginUpdate> updates = plannedPluginUpdates();
    if (!updates.empty()) {
        ImGui::SeparatorText("Updates available");
        for (std::size_t i = 0; i < updates.size(); ++i) {
            const cascade::core::PluginUpdate& u = updates[i];
            ImGui::PushID(static_cast<int>(2000 + i));
            const std::string label =
                (u.entry != nullptr && !u.entry->name.empty()) ? u.entry->name : u.id;
            ImGui::Text("%s %s -> %s", label.c_str(), u.fromVersion.c_str(),
                        u.toVersion.c_str());
            ImGui::TextDisabled("%s", u.reason.c_str());
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Update", ImVec2(-FLT_MIN, 0.0f))) { startUpdate(u); }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }

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
        // THE DETAIL PANE OPENS HERE, under the row that was clicked, rather
        // than at the foot of the list. With eleven plugins the old layout put
        // a description and its Install button several screens below the name
        // they belonged to, so the two could not be seen together - and the
        // one thing this pane exists to do is show a licence and a legal
        // notice NEXT TO the decision they govern.
        if (i == catalogSel_) {
            drawPluginCatalogueDetail(i);
            // Still directly under the Install button, which has moved with it.
            drawPluginResultText();
        }
        ImGui::PopID();
    }

    // An install result outlives its entry only when nothing is selected at
    // all (the catalogue was refreshed, say). Drawn once, either way.
    if (catalogSel_ < 0 || catalogSel_ >= static_cast<int>(catalog_.size())) {
        drawPluginResultText();
    }
}

// --- Detail pane + install gate, for ONE catalogue entry ---------------------
//
// Called from inside drawPluginBrowser's list loop, within that row's PushID,
// so the checkbox and the Install button below belong to the row they follow.
void AppWindow::drawPluginCatalogueDetail(int idx) {
    if (idx < 0 || idx >= static_cast<int>(catalog_.size())) { return; }
    {
        const cascade::core::PluginCatalogEntry& e =
            catalog_[static_cast<std::size_t>(idx)];
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

        const std::string blocked = pluginInstallBlockedReason(idx, legalAck_);
        ImGui::BeginDisabled(!blocked.empty());
        if (ImGui::Button("Install", ImVec2(-FLT_MIN, 0.0f))) { startInstall(e); }
        ImGui::EndDisabled();
        if (!blocked.empty()) { ImGui::TextDisabled("Install disabled: %s", blocked.c_str()); }
        ImGui::Separator();
    }
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
    // The manifest too. A RETIRED plugin has been renamed out of the scan, so
    // the host has no record of it — but the file is very much installed, and
    // calling it absent would offer an Install where Update is the remedy.
    // missingFromDisk rows are excluded: those name a file the user deleted,
    // which really is not installed any more.
    for (const cascade::core::InstalledPlugin& ip : pluginInventory_.plugins) {
        if (!ip.missingFromDisk && equalsFileNameAscii(ip.file, want)) { return true; }
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
    const std::string dir = pluginDir_;
    catalogFuture_ = std::async(std::launch::async, [this, url, dir] {
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
        if (r.ok) {
            // THE ONLY MOMENT A RETIREMENT FLOOR IS WRITTEN TO THIS MACHINE.
            // Enforcement reads the cache and never the network, so a floor
            // that is not cached here protects nobody — not the user who goes
            // offline for a year, and not the one who never opens this browser
            // again. It runs on the worker because it re-hashes every
            // installed plugin.
            std::string policyError;
            if (!cascade::core::PluginRepo::cacheCataloguePolicies(dir, r.entries,
                                                                   policyError)) {
                r.policyError = "the catalogue loaded, but its plugin version policy could "
                                "not be saved, so it will not be remembered: " +
                                policyError;
            }
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
            if (r.ok) {
                // MANIFEST UPKEEP, without which the whole retirement feature
                // silently no-ops: an unrecorded plugin has no id, no version
                // and no cached policy, so pluginBlockReason() fails open for
                // it forever. This is where a plain install becomes managed —
                // applyUpdate() records itself, so this is the only install
                // path that needs it.
                (void)cascade::core::PluginRepo::recordInstall(dir, e, r.recordError);
            }
            return r;
        });
}

void AppWindow::startUpdate(const cascade::core::PluginUpdate& u) {
    if (catalogPending_ || installPending_ || u.entry == nullptr) { return; }
    installError_.clear();
    installReport_.clear();
    installBusyName_ = u.entry->name;
    installPending_ = true;
    const std::string dir = pluginDir_;
    // The plan's entry aliases catalog_, which the GUI may replace while this
    // runs, so the worker owns a copy and the plan is re-pointed at it.
    installFuture_ = std::async(
        std::launch::async, [this, plan = u, entry = *u.entry, dir]() mutable {
            PluginInstallResult r;
            r.isUpdate = true;
            r.name = entry.name;
            plan.entry = &entry;
            // applyUpdate is install() plus recordInstall(), in that order and
            // with the same gauntlet — nothing here shortcuts it because "it
            // is only an update".
            r.ok = pluginRepo_.applyUpdate(plan, dir, r.installedPath, r.error);
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
            // "When the user last chose to look" — recorded here, never acted
            // on: no code path reads this to decide whether to fetch (see
            // AppConfig::pluginLastUpdateCheck).
            pluginLastUpdateCheck_ = static_cast<std::int64_t>(std::time(nullptr));
            // The floors the worker just cached only take effect on the next
            // scan, and a user who has just been told a plugin is retired
            // should not have to restart to stop running it.
            rescanPlugins();
            // A cache-write failure is red text next to a catalogue that
            // nevertheless loaded: the list is real, the policy behind it was
            // not remembered, and both facts are shown.
            if (!r.policyError.empty()) { catalogError_ = r.policyError; }
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
        // The point of the rescan: the file is on disk, but it is not a
        // PLUGIN until the host has loaded and validated it — and if it fails
        // validation the user needs to see that here, immediately, rather
        // than after a restart. It also re-runs enforcement, which is what
        // makes an update lift the retirement in the same frame it lands.
        //
        // Run on the ONE failure that still changed the disk too: applyUpdate
        // reports false with installedPath set when the new file installed but
        // the manifest could not be written.
        if (r.ok || !r.installedPath.empty()) { rescanPlugins(); }
        if (r.ok) {
            installReport_ = (r.isUpdate ? "Updated " : "Installed ") + r.name + " to " +
                             r.installedPath;
            if (!r.recordError.empty()) {
                // Honest partial state: verified bytes are installed, but the
                // plugin is unmanaged until a later install or catalogue fetch
                // repairs the record — and an unmanaged plugin is never
                // retired, which is the fail-open rule doing its job.
                installError_ = r.name + " was installed, but its record could not be " +
                                "written: " + r.recordError +
                                ". It will not be version-checked until that is repaired.";
            }
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
    // feature — and the rescan below reloads every survivor in the same
    // frame, so the visible effect is that ONE plugin disappears.
    //
    // Through detachAndUnloadPlugins(), NOT unloadAll() directly. This called
    // unloadAll() on its own, on the since-falsified premise that "nothing in
    // the product holds a decoder instance across frames yet" — the runner,
    // the panel/track-source UI handles, the basemap and the track-info
    // client all do, which is exactly why rescanPlugins() takes them off in a
    // prescribed order first. Unmapping a module out from under live handles
    // is undefined behaviour in third-party code, and whatever it does next it
    // is not "remove one plugin".
    //
    // If the delete still fails — the file is open in another process, or
    // permissions changed — PluginRepo's reason is shown verbatim in red and
    // the rescan puts everything back exactly as it was. Nothing is lost and
    // nothing is claimed that did not happen.
    detachAndUnloadPlugins();
    std::string err;
    if (pluginRepo_.remove(pluginDir_, fileName, err)) {
        installReport_ = "Removed " + fileName;
    } else {
        installError_ = err;
    }
    rescanPlugins();
}

void AppWindow::refreshPluginRunner() {
    // Decoder instances are created FOR a sample rate and a centre frequency,
    // so they are only valid for the source that was active when they were
    // built. Both change when the user switches source, opens a file, or when
    // a saved configuration restores a device at startup.
    //
    // That last case is what made this necessary. rescanPlugins() runs during
    // construction, BEFORE the config restores the source, so the first build
    // saw the generator's defaults - and an ADS-B plugin created against a
    // 100 MHz centre correctly reported "receiver is at 100.000 MHz" while the
    // radio sat on 1090 MHz. The decoder was right and the host had lied to it.
    //
    // Rebuilding rather than retuning, because the RATE cannot be changed on a
    // live instance: the ABI passes it to create() and nothing else.
    //
    // THE STOP LIST GOES DOWN FIRST, before either rebuild, because it decides
    // what those rebuilds create. Pushed on every refresh rather than only when
    // it changes: this function runs after every rescan, and a rescan is
    // exactly when the two halves have been cleared and could otherwise start a
    // plugin the user stopped.
    pluginRunner_.setStopped(pluginsStopped_);
    pluginUi_.setStopped(pluginsStopped_);
    pipeline_.setPluginRunner(nullptr);
    pluginRunner_.rebuild(pluginHost_.plugins(), cascade::core::Pipeline::kAudioRateHz,
                          pipeline_.inputRateHz(),
                          pipeline_.activeSource().centerFrequencyHz());
    pipeline_.setPluginRunner(&pluginRunner_);
    // AND THE MUTE SNAPSHOT AFTER THE REBUILD, not before it. It carries the
    // running state, and running now means the runner is ACTUALLY FEEDING the
    // plugin (see rebuildMuteStates) - a question only the rebuild above can
    // answer, because it is the rebuild that decides which decoders the
    // receiver's current rate can drive. Built first, this would answer it
    // from the receiver the user has just left.
    rebuildMuteStates();

    // The GUI-side capabilities are rebuilt HERE too, and for the same reason:
    // a track source created against one receiver state should not survive a
    // source change. Doing it in this one function means the two halves of the
    // plugin system can never disagree about which plugins are live.
    //
    // Services are installed BEFORE rebuild, because the ABI promises attach()
    // runs before any capability's create() and a tracker may read the
    // receiver while building its initial state.
    cascade::core::HostServices svc;
    svc.centreHz = [this] { return pipeline_.activeSource().centerFrequencyHz(); };
    svc.sampleRateHz = [this] { return pipeline_.inputRateHz(); };
    svc.unixTimeMs = [] {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    svc.tune = [this](double hz) -> std::int32_t {
        // PluginUi has already applied the per-plugin permission; this only
        // has to tune and report what the device said. There is no isOpen() on
        // the IqSource interface, and a source with no usable rate is one
        // nothing has opened - a state a plugin needs told apart from "the
        // device refused".
        if (!(pipeline_.activeSource().sampleRateHz() > 0.0)) {
            return CASCADE_TUNE_NO_DEVICE;
        }
        retuneSourceHz(hz);
        return CASCADE_TUNE_OK;
    };
    pluginUi_.setServices(std::move(svc));
    pluginUi_.rebuild(pluginHost_.plugins());

    // THE BASEMAP, if a plugin supplies one. The first loaded plugin declaring
    // the capability wins: two basemaps cannot both be the map, and picking
    // silently by load order is more predictable than picking by some quality
    // the user cannot see. Detached first so a rescan that removed the plugin
    // takes its tiles - and its textures - with it.
    //
    // A STOPPED plugin is skipped here too, and that is the whole of what
    // "stopped" means for a basemap: the map falls back to its built-in
    // coastlines rather than keeping tiles from a plugin the user has switched
    // off. Skipping it in the runner and the UI half but not here would leave
    // the stopped plugin drawing the entire map background.
    const CascadeBasemapApi* wantBasemap = nullptr;
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (lp.loaded && lp.basemap != nullptr &&
            !pluginIsStopped(cascade::core::pluginKey(lp))) {
            wantBasemap = lp.basemap;
            break;
        }
    }
    basemap_.attach(wantBasemap);
    // Track enrichment, by the same first-wins rule and for the same reason.
    const CascadeTrackInfoApi* wantTrackInfo = nullptr;
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (lp.loaded && lp.trackInfo != nullptr &&
            !pluginIsStopped(cascade::core::pluginKey(lp))) {
            wantTrackInfo = lp.trackInfo;
            break;
        }
    }
    trackInfo_.attach(wantTrackInfo);
    // Grants LAST, and every time. rescanPlugins() calls PluginUi::clear(),
    // which drops the permission set along with the instances, so without this
    // a rescan would silently revoke every permission the user had given — and
    // the tracker that worked a moment ago would go quiet with no explanation.
    applyPluginTuneGrants();
}

// GL_CLAMP_TO_EDGE is OpenGL 1.2; the headers Windows ships stop at 1.1, and
// this build targets 1.1 deliberately. The token is defined rather than
// falling back to GL_CLAMP because GL_CLAMP samples the border COLOUR at the
// edges, which puts a dark fringe around every decoded image. Every driver
// this runs on supports 1.2.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

void AppWindow::separateWindowAnchor(int slot, float& x, float& y) {
    // See the note at the Map window for why overhanging the edge is what
    // produces a separate operating system window.
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const float stagger = 34.0f * static_cast<float>(slot);
    x = mv->Pos.x + mv->Size.x - 140.0f + stagger;
    y = mv->Pos.y + 60.0f + stagger;
}

void AppWindow::placeAsSeparateWindow(int slot) {
    // FirstUseEver throughout, so this is a starting position and never fights
    // the user afterwards.
    float x = 0.0f;
    float y = 0.0f;
    separateWindowAnchor(slot, x, y);
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
}

void AppWindow::mapDefaultSize(float& widthPx, float& heightPx) {
    // DERIVED FROM THE MONITOR, not a constant. 720x520 was the map's opening
    // size on every display ever made, and on anything modern it shows about a
    // dozen of the flight list's rows before the list child grows a scrollbar
    // - which is exactly the "map screen isn't large enough, it needs to
    // display without a scroller" report. Measured on the 5120x1440 desktop
    // this was reproduced on: 30 targets, 12 rows visible, scrollbar.
    //
    // The height is asked for in ROWS rather than pixels, because a row is two
    // lines of text (callsign, then altitude and range) and the whole point is
    // how many targets fit. That also keeps it right if the font is scaled,
    // which a pixel constant would not.
    const float rowH = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    const float chrome = ImGui::GetFrameHeightWithSpacing() * 2.0f +
                         ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    // The list is a fixed 190 px column; the map beside it wants to be several
    // times that or it is a keyhole, not a map.
    float want_w = 1120.0f;

    // NEVER LARGER THAN THE SCREEN IT OPENS ON. The monitor's WORK area, so a
    // taskbar does not end up over the attribution line. ImGui's platform
    // monitor list is populated by the GLFW backend; when it is empty (a
    // headless --frames run) the main viewport's own work size is the only
    // honest answer available.
    float availW = 0.0f;
    float availH = 0.0f;
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const ImVec2 centre(mv->Pos.x + mv->Size.x * 0.5f, mv->Pos.y + mv->Size.y * 0.5f);
    for (int i = 0; i < pio.Monitors.Size; ++i) {
        const ImGuiPlatformMonitor& m = pio.Monitors[i];
        if (centre.x >= m.MainPos.x && centre.x < m.MainPos.x + m.MainSize.x &&
            centre.y >= m.MainPos.y && centre.y < m.MainPos.y + m.MainSize.y) {
            availW = m.WorkSize.x;
            availH = m.WorkSize.y;
            break;
        }
    }
    if (availW < 1.0f || availH < 1.0f) {
        availW = mv->WorkSize.x;
        availH = mv->WorkSize.y;
    }
    // Not the whole thing: see kMapWorkAreaShare. A DEFAULT-ONLY judgement -
    // a restored rectangle is the user's own and is clamped only to what fits
    // where it sits - so the two paths are deliberately not the same rule.
    const float capW = availW * kMapWorkAreaShare;
    const float capH = availH * kMapWorkAreaShare;
    if (want_w > capW) { want_w = capW; }

    // THE ROW COUNT COMES FROM THE CAP, not from a number picked in advance.
    // Asking for a fixed 25 rows and then clamping only ever gives away
    // height: on the 5120x1440 desktop this was reproduced on, 25 rows asked
    // for 930 px of a 1183 px allowance and left 253 px of the screen the user
    // had already agreed to spend unused - so 26 targets, an ordinary ADS-B
    // load, still scrolled. Asking for as many rows as the allowance holds
    // makes the default use the screen the monitor actually offers.
    //
    // Whole rows rather than the raw allowance, because a part-row at the
    // bottom of the list is exactly the sliver that makes it look like
    // something is cut off. At the default font that works out at roughly 23
    // rows on a 1080p display (1032 px of work area) and 32 on a 1440p one
    // (1392 px) - both comfortably past the ~26 an ADS-B session reaches.
    float rows = std::floor((capH - chrome) / rowH);
    if (rows < 1.0f) { rows = 1.0f; }
    float want_h = chrome + rowH * rows;
    // Whole rows fit the cap by construction; this catches only the screen so
    // small that even the forced single row does not.
    if (want_h > capH) { want_h = capH; }
    // A floor below which the window is not a map at all, even on a small
    // screen. Deliberately ABOVE AppConfig::kMapWindowMinPx: that 320 px is
    // the smallest rectangle the sanitizer will ACCEPT from a user who dragged
    // one, and this is the smallest the host will CHOOSE on its own - a size
    // nobody asked for should be a usable one.
    if (want_w < 480.0f) { want_w = 480.0f; }
    if (want_h < 360.0f) { want_h = 360.0f; }
    widthPx = want_w;
    heightPx = want_h;
}

void AppWindow::drawPluginWindows() {
    // One poll per frame feeds both the map and every panel: the plugins are
    // asked once and the answer is shared, so a plugin cannot be charged twice
    // for having two kinds of output.
    pluginUi_.poll();

    // --- coverage accumulation ------------------------------------------------
    // FED HERE RATHER THAN INSIDE THE MAP WINDOW, because the antenna is
    // hearing things whether or not the map is open and whether or not the
    // overlay is switched on. Tying the measurement to a window being visible
    // would make the picture a record of when the user happened to be looking.
    //
    // Costs one distance and one bearing per visible target per frame against a
    // 72-entry array - a few microseconds for a busy sky - and the array is the
    // only thing that grows, which is to say nothing grows. The staleness rule
    // is applied first so a plugin that never evicts cannot keep re-recording
    // an aircraft it last heard an hour ago.
    if (rxSet_) {
        for (const cascade::core::HostTrack& ht : pluginUi_.tracks()) {
            if (!cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) {
                continue;
            }
            coverage_.record(initialBearingDeg(rxLat_, rxLon_, ht.t.latDeg, ht.t.lonDeg),
                             greatCircleKm(rxLat_, rxLon_, ht.t.latDeg, ht.t.lonDeg));
        }
    }

    // Opens itself the first time there is something to show - counted the way
    // it is DRAWN, and only on the FRAME THE PICTURE CHANGES. The raw lists
    // were the wrong question (a source that never evicts keeps reporting
    // targets the staleness rule has dropped, so the window was demanded over
    // a map showing "0 targets"), but asking the right question every frame
    // was still wrong in the ordinary case: a receiver that keeps hearing its
    // targets answers yes for ever, so the close button was cleared and set
    // straight back. See mapSelfOpens.
    const bool haveVisible =
        cascade::core::anyVisibleTarget(pluginUi_.tracks(), pluginUi_.paths());
    if (cascade::core::mapSelfOpens(mapHadVisibleTarget_, haveVisible)) {
        mapOpen_ = true;
    }
    mapHadVisibleTarget_ = haveVisible;

    if (mapOpen_) {
        telemetryNotePanel("map");
        // PLACED SO IT DOES NOT FIT INSIDE THE APPLICATION WINDOW, which is
        // what makes ImGui give it a real operating system window rather than
        // merging it into the main one. There is no "always be a separate
        // window" flag; the rule is that a window which fits inside the main
        // viewport gets merged into it, so the way to ask for a window of its
        // own is to start it overhanging the edge. Anchored just inside the
        // right edge rather than flung off to one side, so it appears next to
        // the radio instead of somewhere the user has to hunt for it - and
        // only on first use, so moving or docking it afterwards sticks.
        // ...WHICH IS NO LONGER ENOUGH ON ITS OWN once a saved size is
        // restored: a window that FITS inside the main viewport is merged into
        // it, and a map restored to a modest size on a large main window would
        // silently stop being its own window. NoAutoMerge says outright what
        // the overhang was only implying, and is what makes restoring an
        // arbitrary rectangle safe.
        ImGuiWindowClass mapClass;
        mapClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&mapClass);
        // ...AND THE SAVED RECTANGLE ONLY COUNTS IF IT IS STILL SOMEWHERE.
        // The config sanitizer checks the numbers are sane; it cannot know
        // what monitors exist. A geometry saved on a display that has since
        // been unplugged restores off-screen, and ImGui's own clamp leaves
        // only a sliver: measured at 19 px of title bar for a rectangle saved
        // at (-1500,300), and for (-16000,-16000) only the 19x19 resize grip,
        // with the title bar off the top - so the map could not be dragged
        // back at all, and the unusable geometry was then re-saved every
        // frame. Before the size was persisted the map always opened beside
        // the main window and this could not happen.
        std::vector<ScreenRect> workAreas;
        const ImGuiPlatformIO& mapPio = ImGui::GetPlatformIO();
        for (int i = 0; i < mapPio.Monitors.Size; ++i) {
            const ImGuiPlatformMonitor& m = mapPio.Monitors[i];
            workAreas.push_back(
                ScreenRect{m.WorkPos.x, m.WorkPos.y, m.WorkSize.x, m.WorkSize.y});
        }
        if (workAreas.empty()) {
            // No platform monitor list: a headless --frames run. The main
            // viewport's own work area is the only honest answer, exactly as
            // in mapDefaultSize.
            const ImGuiViewport* mv = ImGui::GetMainViewport();
            workAreas.push_back(
                ScreenRect{mv->WorkPos.x, mv->WorkPos.y, mv->WorkSize.x, mv->WorkSize.y});
        }
        if (mapWinW_ > 0 && mapWinH_ > 0 &&
            mapGeometryOnScreen(mapWinX_, mapWinY_, mapWinW_, mapWinH_, workAreas)) {
            // ...AND NO BIGGER THAN WHAT FITS WHERE IT SITS. A rectangle saved
            // on a taller display is reachable by its title bar and still too
            // tall here, which puts the resize grip off the bottom: the window
            // can be dragged but not shrunk, and the oversized geometry is then
            // written back to the config every frame. Only that overhang is
            // taken off it - a rectangle that fits is the user's own and is
            // restored untouched.
            mapClampRestoredSize(mapWinX_, mapWinY_, mapWinW_, mapWinH_, workAreas);
            // The user's own rectangle, from the config. FirstUseEver, so it
            // is where the window OPENS and never fights a later drag.
            ImGui::SetNextWindowPos(
                ImVec2(static_cast<float>(mapWinX_), static_cast<float>(mapWinY_)),
                ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(
                ImVec2(static_cast<float>(mapWinW_), static_cast<float>(mapWinH_)),
                ImGuiCond_FirstUseEver);
        } else {
            // THE DEFAULT RECTANGLE, POSITION INCLUDED. The anchor overhangs
            // the main window on purpose, but it knows nothing about the
            // monitor: with a height derived from the screen the bottom - and
            // the resize grip with it - fell off the work area whenever the
            // main window sat low, and that rectangle was then persisted and
            // restored verbatim. Measured at (1248,491) 1120x1168 against a
            // 1392 px work area. See mapPlaceDefaultRect.
            float dx = 0.0f;
            float dy = 0.0f;
            separateWindowAnchor(0, dx, dy);
            float dw = 0.0f;
            float dh = 0.0f;
            mapDefaultSize(dw, dh);
            mapPlaceDefaultRect(dx, dy, dw, dh, workAreas);
            ImGui::SetNextWindowPos(ImVec2(dx, dy), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(dw, dh), ImGuiCond_FirstUseEver);
        }
        if (ImGui::Begin("Map", &mapOpen_)) {
            // READ BACK EVERY FRAME, which is the whole of the persistence:
            // ImGui's own .ini is switched off in this application, so unless
            // the size is copied out here and into AppConfig it exists only
            // until the process ends. Rounded to whole pixels because that is
            // what a window manager deals in and what the config stores.
            const ImVec2 wpos = ImGui::GetWindowPos();
            const ImVec2 wsize = ImGui::GetWindowSize();
            mapWinX_ = static_cast<int>(wpos.x);
            mapWinY_ = static_cast<int>(wpos.y);
            mapWinW_ = static_cast<int>(wsize.x);
            mapWinH_ = static_cast<int>(wsize.y);
            if (ImGui::SmallButton("Fit")) { map_->requestFitToTracks(); }
            ImGui::SameLine();
            // The receiver's own position is asked for, never guessed. It is
            // what range and bearing are measured from, and inferring it from
            // a decoded target would be confidently wrong.
            //
            // DOUBLE, NOT FLOAT, and persisted. A float carries about seven
            // significant digits, which runs out inside the decimal part of a
            // latitude and moves the origin of every range ring by metres for
            // no reason; and the pair used to be static LOCALS, so the position
            // died with the process. Both are now AppConfig fields - see
            // AppConfig::rxPositionSet.
            ImGui::SetNextItemWidth(96.0f);
            ImGui::InputDouble("##homelat", &rxLatInput_, 0.0, 0.0, "%.5f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Receiver latitude, degrees north (-90..90)");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(96.0f);
            ImGui::InputDouble("##homelon", &rxLonInput_, 0.0, 0.0, "%.5f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Receiver longitude, degrees east (-180..180)");
            }
            ImGui::SameLine();
            // REFUSED RATHER THAN CLAMPED, and by the same positive range test
            // the config sanitizer uses, so a typo cannot silently install a
            // receiver at the pole and quietly make every distance wrong. The
            // button simply does not accept the pair.
            const bool rxInputOk = rxLatInput_ >= -90.0 && rxLatInput_ <= 90.0 &&
                                   rxLonInput_ >= -180.0 && rxLonInput_ <= 180.0;
            ImGui::BeginDisabled(!rxInputOk);
            if (ImGui::SmallButton("Set RX here")) {
                rxLat_ = rxLatInput_;
                rxLon_ = rxLonInput_;
                rxSet_ = true;
                map_->setHome(rxLat_, rxLon_);
                // A NEW ORIGIN INVALIDATES THE OLD COVERAGE. Every wedge in it
                // was measured from somewhere else, and keeping them would draw
                // one antenna's pattern around another antenna's position.
                coverage_.reset();
            }
            ImGui::EndDisabled();
            if (!rxInputOk && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Latitude must be -90..90 and longitude -180..180");
            }
            ImGui::SameLine();
            // COUNTED THE WAY THEY ARE DRAWN. A plugin that never evicts keeps
            // reporting targets the staleness rule has dropped, and a count of
            // everything reported over a map showing only what is live is a
            // number that contradicts the picture beside it.
            const std::size_t shown =
                cascade::core::visibleTrackCount(pluginUi_.tracks());
            ImGui::TextDisabled("%d target%s", static_cast<int>(shown),
                                shown == 1 ? "" : "s");

            // --- the coverage overlay's controls ---------------------------
            // A second row, because the first is already the position entry and
            // the two are different jobs: one says where the antenna is, this
            // one says what it has managed to hear from there.
            ImGui::Checkbox("Coverage", &coverageShow_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Furthest anything has been heard, per 5 degrees of bearing.\n"
                    "Measured from the receiver position, this session only.");
            }
            ImGui::SameLine();
            // RESET IS NOT OPTIONAL. A single spurious decode at an impossible
            // range - and a noisy band produces them - would otherwise stretch
            // one wedge to the horizon for the rest of the session and make the
            // whole picture useless. One button undoes it.
            ImGui::BeginDisabled(coverage_.empty());
            if (ImGui::SmallButton("Reset coverage")) { coverage_.reset(); }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (!rxSet_) {
                // DEGRADED, AND SAYING SO. Without a receiver position there is
                // nothing to measure a bearing from, so the accumulator is
                // never fed and the overlay would be an empty circle the user
                // could not explain.
                ImGui::TextDisabled("set the RX position to measure coverage");
            } else if (coverage_.empty()) {
                ImGui::TextDisabled("nothing heard yet");
            } else {
                ImGui::TextDisabled("%d of %d bearings, best %.0f km",
                                    coverage_.filledBuckets(), CoverageMap::kBuckets,
                                    coverage_.peakKm());
            }

            // THE FLIGHT LIST, down the left of the map. A map alone answers
            // "where is everything"; the list answers "what am I hearing" and,
            // clicked, "take me to that one" - which is the question a
            // callsign actually prompts.
            //
            // NARROWED AGAIN, because it no longer has eight columns to fit.
            // It went to 480 px to hold them and still could not show their
            // headings; with callsign, id and a button it needs about 300, and
            // every pixel not spent here is map. Capped at a share of the
            // window as well as at a constant, so a map dragged small still
            // leaves a map.
            const ImVec2 mapAvail = ImGui::GetContentRegionAvail();
            const float listWidth = std::min(300.0f, std::max(190.0f, mapAvail.x * 0.36f));
            ImGui::BeginChild("##tracklist", ImVec2(listWidth, 0.0f), true);
            drawTrackList();
            ImGui::EndChild();
            ImGui::SameLine();

            ImVec2 avail = ImGui::GetContentRegionAvail();
            // ROOM FOR THE ATTRIBUTION IS RESERVED BEFORE the map is sized, not
            // left over after it. The map fills whatever it is given, so
            // drawing the credit afterwards pushed it outside the window and it
            // was never seen - which, for a host that REFUSES a basemap plugin
            // supplying no attribution, would have been hypocrisy rather than a
            // layout bug.
            const bool credit = basemap_.active() && !basemap_.attribution().empty();
            if (credit) { avail.y -= ImGui::GetTextLineHeightWithSpacing(); }
            if (avail.y < 32.0f) { avail.y = 32.0f; }
            // Borrowed for the frame, and null when the overlay is switched off
            // - which is how the map is told to skip it without growing another
            // boolean parameter. The accumulator keeps filling either way.
            map_->setCoverage(coverageShow_ ? &coverage_ : nullptr);
            map_->draw(avail.x, avail.y, pluginUi_.tracks(), pluginUi_.paths(),
                       &basemap_, &trackInfo_);
            if (credit) {
                ImGui::TextDisabled("%s", basemap_.attribution().c_str());
            }
            basemap_.endFrame();
        }
        ImGui::End();
    }

    // OUTSIDE the Map window on purpose, so it is a sibling of the map rather
    // than something drawn inside it: the map window can be small, and a detail
    // view that ate the list would have traded one complaint for another.
    // Drawn whether or not the map window is open - a details window the user
    // has dragged to a second screen must not vanish because the map was shut.
    drawTargetDetailsWindow();

    // --- image windows ----------------------------------------------------
    // The pictures come from the RUNNER, because that is what feeds the image
    // decoders samples. pluginImages_ is this window's own buffer and is
    // rewritten only when a decoder produces something new, so redrawing an
    // image the user is looking at costs nothing.
    pluginRunner_.pollImages(pluginImages_);
    const std::vector<cascade::core::HostImage>& imgs = pluginImages_;
    // Textures are keyed by SLOT, and a rescan can put a different plugin in a
    // slot - or drop one entirely. A slot whose plugin changed keeps its GL
    // texture (allocating a new one per rescan would leak them) but has its
    // cached revision invalidated, because the new decoder's revision counter
    // starts at 0 again and a stale cache would leave the previous plugin's
    // picture on screen under the new plugin's name.
    while (imageTex_.size() > imgs.size()) {
        if (imageTex_.back() != 0u) { glDeleteTextures(1, &imageTex_.back()); }
        imageTex_.pop_back();
        imageTexRev_.pop_back();
        imageTexPlugin_.pop_back();
    }
    if (imageTex_.size() < imgs.size()) {
        imageTex_.resize(imgs.size(), 0u);
        imageTexRev_.resize(imgs.size(), 0ull);
        imageTexPlugin_.resize(imgs.size());
    }
    for (std::size_t i = 0; i < imgs.size(); ++i) {
        if (imageTexPlugin_[i] != imgs[i].plugin) {
            imageTexPlugin_[i] = imgs[i].plugin;
            imageTexRev_[i] = 0ull;
        }
    }
    for (std::size_t i = 0; i < imgs.size(); ++i) {
        const cascade::core::HostImage& im = imgs[i];
        // NOTHING RECEIVED YET, NO WINDOW. An image decoder is created the
        // moment its plugin loads, so every launch used to open a window
        // saying "waiting for the first image..." - for SSTV, which may not
        // hear a transmission all day. A window that appears when a picture
        // starts arriving, and not before, is the same rule the map already
        // follows: it opens itself the first time there is something to show.
        if (im.width == 0u || im.height == 0u) { continue; }
        telemetryNotePanel("image");
        // Its own operating system window, for the same reason as the map: a
        // received picture is something to put beside the radio, or on another
        // screen, not a panel inside it. Staggered per decoder so two plugins
        // producing pictures do not land exactly on top of each other.
        placeAsSeparateWindow(static_cast<int>(i) + 1);
        const std::string id = im.plugin + " image###image_" + im.plugin;
        if (ImGui::Begin(id.c_str())) {
            if (im.width == 0 || im.height == 0) {
                ImGui::TextDisabled("Waiting for the first image...");
            } else {
                // Uploaded only when the plugin says the pixels changed. A
                // slow-scan frame updates a few times a second at most, and
                // re-uploading a megapixel texture every frame to show the
                // same picture would cost more than the decoding does.
                if (imageTexRev_[i] != im.revision) {
                    if (imageTex_[i] == 0u) { glGenTextures(1, &imageTex_[i]); }
                    glBindTexture(GL_TEXTURE_2D, imageTex_[i]);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    // Rows are tightly packed by PluginUi, which is NOT the
                    // 4-byte default OpenGL assumes; without this a greyscale
                    // image whose width is not a multiple of four shears.
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    const GLenum fmt =
                        (im.format == CASCADE_IMAGE_RGB24) ? GL_RGB : GL_LUMINANCE;
                    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(fmt),
                                 static_cast<GLsizei>(im.width),
                                 static_cast<GLsizei>(im.height), 0, fmt, GL_UNSIGNED_BYTE,
                                 im.pixels.data());
                    imageTexRev_[i] = im.revision;
                }

                ImGui::Text("%ux%u  %s  frame %llu", im.width, im.height,
                            im.complete ? "complete" : "receiving...",
                            static_cast<unsigned long long>(im.sequence));
                ImGui::SameLine();
                if (ImGui::SmallButton("Save as BMP")) {
                    // Named by plugin, sequence and wall-clock time, next to
                    // the recordings rather than in the install directory.
                    std::error_code ec;
                    const std::filesystem::path dir =
                        std::filesystem::path(recordDir_.empty() ? "." : recordDir_);
                    std::filesystem::create_directories(dir, ec);
                    char stamp[32];
                    const std::time_t t = std::time(nullptr);
                    std::tm tmv{};
#if defined(_WIN32)
                    localtime_s(&tmv, &t);
#else
                    localtime_r(&t, &tmv);
#endif
                    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
                    std::string safe = im.plugin;
                    for (char& c : safe) {
                        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                        (c >= '0' && c <= '9') || c == '-' || c == '_';
                        if (!ok) { c = '_'; }
                    }
                    const std::filesystem::path out =
                        dir / (safe + "-" + stamp + ".bmp");
                    std::string err;
                    if (cascade::core::writeBmp24(im, out.string(), err)) {
                        imageSaveNote_ = "Saved " + out.string();
                    } else {
                        imageSaveNote_ = "Save failed: " + err;
                    }
                }

                // Fit to the window, preserving aspect: a weather image
                // stretched to the pane is a misread image.
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const float sx = avail.x / static_cast<float>(im.width);
                const float sy = avail.y / static_cast<float>(im.height);
                const float s = (sx < sy ? sx : sy);
                if (imageTex_[i] != 0u && s > 0.0f) {
                    ImGui::Image(static_cast<ImTextureID>(
                                     static_cast<std::uintptr_t>(imageTex_[i])),
                                 ImVec2(static_cast<float>(im.width) * s,
                                        static_cast<float>(im.height) * s));
                }
            }
            if (!imageSaveNote_.empty()) { ImGui::TextDisabled("%s", imageSaveNote_.c_str()); }
        }
        ImGui::End();
    }

    drawDecoderWindow();

    // Plugin-declared windows. Each gets its own, titled by the plugin, so two
    // plugins cannot collide in one panel.
    for (const cascade::core::HostPanel& p : pluginUi_.panels()) {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 300.0f), ImGuiCond_FirstUseEver);
        // The plugin NAME is part of the ImGui id, not just the title: two
        // plugins may legitimately call their window the same thing, and
        // colliding ids would merge them into one window.
        const std::string id = p.title + "###panel_" + p.plugin;
        if (ImGui::Begin(id.c_str())) {
            const int cols = static_cast<int>(p.headings.size());
            if (cols > 0 &&
                ImGui::BeginTable("##rows", cols,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
                for (const std::string& h : p.headings) {
                    ImGui::TableSetupColumn(h.c_str());
                }
                ImGui::TableHeadersRow();
                for (const CascadePanelRow& r : p.rows) {
                    if (r.kind == CASCADE_ROW_SEPARATOR) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Separator();
                        continue;
                    }
                    ImGui::TableNextRow();
                    if (r.kind == CASCADE_ROW_HEADING) {
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%.*s", CASCADE_PANEL_CELL_CHARS, r.cells[0]);
                        continue;
                    }
                    for (int c = 0; c < cols; ++c) {
                        ImGui::TableNextColumn();
                        // Bounded print: the ABI says the cells are
                        // NUL-terminated, but a plugin that fills every byte
                        // must not walk the host off the end of the array.
                        const char* cell = r.cells[c];
                        if ((r.flags & CASCADE_ROW_FLAG_WARN) != 0u) {
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%.*s",
                                               CASCADE_PANEL_CELL_CHARS, cell);
                        } else if ((r.flags & CASCADE_ROW_FLAG_GOOD) != 0u) {
                            ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f), "%.*s",
                                               CASCADE_PANEL_CELL_CHARS, cell);
                        } else if ((r.flags & CASCADE_ROW_FLAG_MUTED) != 0u) {
                            ImGui::TextDisabled("%.*s", CASCADE_PANEL_CELL_CHARS, cell);
                        } else {
                            ImGui::Text("%.*s", CASCADE_PANEL_CELL_CHARS, cell);
                        }
                    }
                }
                ImGui::EndTable();
            }
            if (p.rows.empty()) { ImGui::TextDisabled("Nothing to show yet."); }
        }
        ImGui::End();
    }
}

void AppWindow::drawTrackList() {
    const std::vector<cascade::core::HostTrack>& tracks = pluginUi_.tracks();
    // THE SAME RULE THE MAP USES, applied here too. A target the map has
    // dropped must not still be occupying a row: the list is what makes the
    // window need a scrollbar, and a decoder that never evicts - the shipped
    // ADS-B one does not - grows it without limit over a session.
    const std::size_t shown = cascade::core::visibleTrackCount(tracks);
    if (shown == 0u) {
        ImGui::TextDisabled("No targets.");
        ImGui::TextDisabled("Decoded aircraft, ships and stations appear here.");
        return;
    }

    ImGui::Text("%d target%s", static_cast<int>(shown), shown == 1 ? "" : "s");
    // FOLLOW is a toggle rather than a mode buried in a menu, because its
    // effect - the map moving on its own - is confusing if you cannot see at a
    // glance that you asked for it.
    const bool following = !map_->followedId().empty();
    if (following) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
        ImGui::TextWrapped("following %s", map_->followedId().c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Stop following")) { map_->clearFollow(); }
    }
    ImGui::Separator();

    // --- build the rows ------------------------------------------------------
    // Reduced to numbers FIRST, sorted second, drawn third. The host's track
    // vector is the plugins' output and is rebuilt on every poll, so sorting it
    // in place would be undone by the next frame; and a comparator that read
    // through to the plugin data would have to recompute a great circle per
    // comparison instead of once per target.
    std::vector<TrackRow> rows;
    rows.reserve(shown);
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const cascade::core::HostTrack& ht = tracks[i];
        if (!cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) { continue; }
        TrackRow r;
        // The LABEL is the callsign where one has been decoded and the id
        // otherwise. An aircraft's ICAO address is known from its first frame
        // but its callsign only arrives in a separate message type, so a list
        // that showed callsigns alone would leave rows blank for aircraft it
        // is tracking perfectly well.
        r.label = (ht.t.label[0] != '\0') ? ht.t.label : ht.t.id;
        r.id = ht.t.id;
        r.altM = ht.t.altM;
        r.speedMps = ht.t.speedMps;
        r.courseDeg = ht.t.courseDeg;
        // NaN WITHOUT A RECEIVER POSITION, which is what makes the two columns
        // print "no RX" instead of a distance from the Gulf of Guinea. It also
        // sorts them to the bottom, which is the right place for a column that
        // has no values at all.
        if (rxSet_) {
            r.distanceKm = greatCircleKm(rxLat_, rxLon_, ht.t.latDeg, ht.t.lonDeg);
            r.bearingDeg = initialBearingDeg(rxLat_, rxLon_, ht.t.latDeg, ht.t.lonDeg);
        }
        r.ageMs = ht.t.ageMs;
        r.source = i;
        rows.push_back(std::move(r));
    }

    // --- how it is ordered ---------------------------------------------------
    // ABOVE THE TABLE, and spelled out. The eight sort keys used to be eight
    // column headings, and in the width this list gets they were truncated to
    // the point of uselessness. One named key at a time is readable at any
    // width the list can be given, and none of the eight has been dropped.
    drawTrackSortControl();

    // --- the table -----------------------------------------------------------
    // Same idiom as the plugin panel tables above (Borders | RowBg | ScrollY),
    // but NOT Sortable. The sort lives in the control above: ImGui has no
    // public call to set a table's sort arrow, so a menu and clickable headings
    // could not be kept in agreement, and a heading arrow reading "Callsign"
    // over rows ordered by distance is the same unreadable-UI failure in a new
    // form.
    //
    // AND NOT RESIZABLE, WHICH IS DELIBERATE AND COSTS SOMETHING. A resizable
    // ImGui table puts an invisible drag handle over every inner border,
    // TABLE_RESIZE_SEPARATOR_HALF_THICKNESS (4 px) either side of it, and that
    // handle is submitted before the rows are - so it takes the click and the
    // row-spanning Selectable underneath never sees it. The result was an
    // eight-pixel band in every row, right beside the details button, where
    // clicking a row to fly the map to that aircraft did nothing at all.
    // Silent misses on the list's primary gesture are worse than not being able
    // to drag the callsign/id split, especially now the split is computed from
    // what the text actually measures rather than from two guessed weights.
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    // THE HEIGHT IS RESERVED BEFORE THE TABLE IS DRAWN, because a ScrollY table
    // with the default outer_size takes every pixel the pane has left and
    // anything emitted after it lands below the fold. The "no receiver
    // position" note under the table did exactly that: it was drawn, and it
    // took ten wheel notches to see. One line is held back for it, and only
    // when there is a line to hold back.
    const float tableH = tableHeightReservingLines(ImGui::GetContentRegionAvail().y,
                                                   ImGui::GetTextLineHeightWithSpacing(),
                                                   rxSet_ ? 0 : 1);

    // --- how wide the three columns come out ---------------------------------
    // THE HEADINGS ARE MEASURED, NOT GUESSED. Two weights picked by eye (2.0
    // and 1.6) were what left "Callsign" rendering as "Callsi..." in a 620 px
    // map window - a three-pixel shortfall, and the same unreadable heading the
    // eight-column table was replaced for. cascade::gui::trackListFit takes the
    // real font measurements and answers with the widths and with whether they
    // fit; the arithmetic is pure and tested against those exact pixel figures.
    //
    // TIGHTER CELL PADDING IS PART OF THE FIX. ImGui's default four pixels per
    // side spends twenty-four pixels of a two-hundred-pixel list on empty
    // margins, which is most of what was missing. Two is still a clear gap
    // between text and border.
    const float cellPadX = 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                        ImVec2(cellPadX, ImGui::GetStyle().CellPadding.y));

    // The width the columns will actually share. The scrollbar and the four
    // borders are ImGui's and are not part of it; this is an estimate and is
    // only ever used to CHOOSE A LABEL, never to set a width, so being a pixel
    // out picks the compact button a pixel early and nothing worse.
    const float availW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize -
                         4.0f;
    // The widest thing each column must show without truncating. For the
    // callsign that is the heading itself - callsigns are shorter than the word
    // - and for the id it is a six-character ICAO address, which is longer than
    // the heading "ID".
    const float callsignTextW = ImGui::CalcTextSize("Callsign").x;
    const float idTextW =
        (std::max)(ImGui::CalcTextSize("ID").x, ImGui::CalcTextSize("000000").x);
    const float framePadX2 = ImGui::GetStyle().FramePadding.x * 2.0f;

    // A NARROWER BUTTON IS WHERE THE LAST PIXELS COME FROM. Below about 600 px
    // of map window the two headings cannot both fit beside a "Details" button;
    // they can beside an "Info" one. The label is the only thing that changes -
    // same button, same row, same window - and it is chosen from the tested
    // fit rather than from a width threshold somebody picked.
    const char* detailsLabel = "Details";
    cascade::gui::TrackListFit fit = cascade::gui::trackListFit(
        availW, callsignTextW, idTextW, ImGui::CalcTextSize(detailsLabel).x + framePadX2,
        cellPadX);
    if (!fit.headingsFit) {
        const char* compact = "Info";
        const cascade::gui::TrackListFit compactFit = cascade::gui::trackListFit(
            availW, callsignTextW, idTextW, ImGui::CalcTextSize(compact).x + framePadX2,
            cellPadX);
        if (compactFit.headingsFit) {
            detailsLabel = compact;
            fit = compactFit;
        }
    }

    if (!ImGui::BeginTable("##tracktable", 3, flags, ImVec2(0.0f, tableH))) {
        ImGui::PopStyleVar();
        return;
    }

    // THREE COLUMNS, AND THE THIRD IS A BUTTON. The details column's heading is
    // deliberately blank: "Details" over a column of buttons that all say
    // "Details" is a word printed twice.
    //
    // The two text columns are stretch columns weighted by the widths
    // trackListFit returned. Weights are used rather than the widths
    // themselves because ImGui knows the true available width and this code
    // only estimates it - the RATIO is what the fit decided, and stretch
    // applies it to whatever room there really is. The button column is
    // WidthFixed, so it cannot be squeezed to nothing however narrow the pane
    // gets: a button too small to press is worse than a truncated word.
    ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthStretch, fit.callsignW);
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch, fit.idW);
    ImGui::TableSetupColumn("##details", ImGuiTableColumnFlags_WidthFixed |
                                             ImGuiTableColumnFlags_NoResize,
                            fit.detailsW);
    ImGui::TableSetupScrollFreeze(0, 1);  // headings stay put while the list scrolls
    ImGui::TableHeadersRow();

    sortTrackRows(rows, trackSortKey_, trackSortAscending_);

    for (const TrackRow& r : rows) {
        const cascade::core::HostTrack& ht = tracks[r.source];
        const cascade::core::TrackPresentation pres =
            cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind);
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(r.source));
        // Going quiet is visible in the list as well as on the map, and by the
        // same measure: a row that has faded is a target the map is about to
        // drop, which is the warning that makes the disappearance make sense.
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * pres.alpha);

        ImGui::TableNextColumn();
        const bool selected = (map_->selectedId() == r.id);
        // SPANNING ALL COLUMNS so the whole row is the click target. Clicking a
        // row does exactly what clicking a row has always done - the sort
        // reordered the rows, and r.source is what keeps each one pointing at
        // its own target through that.
        //
        // ALLOWOVERLAP is what keeps the details button from being swallowed by
        // the row underneath it. Without it the Selectable, submitted first and
        // covering the whole row, takes the click and the button never fires -
        // and with the button taking the click but the row not knowing, a press
        // on it would ALSO fly the map somewhere. Exactly one of the two must
        // react to any given press, and this is the flag that arranges it.
        if (ImGui::Selectable(r.label.c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap)) {
            // CLICK = GO TO. One click centres the map on it; the map only
            // ever tightens the zoom, so clicking a flight while already
            // zoomed in does not throw the view back out.
            map_->setSelected(ht.t.id);
            map_->goTo(ht.t.latDeg, ht.t.lonDeg);
            mapOpen_ = true;
        }
        // Double click starts following, which is the natural escalation of
        // "take me there" and needs no extra control.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            map_->setFollowed(ht.t.id);
        }
        // THE WHOLE BLOCK ON HOVER, the same one the map shows and the same one
        // the details button opens - so the six values the columns used to
        // carry are one hover away, not one click. It replaces a tooltip that
        // showed only the registration and type code and only when a
        // track-info plugin was installed; this one always has something to
        // say, because the position, altitude, speed, course, range, bearing
        // and age come from the track itself. Asking here is also what queues
        // the registry lookup for every listed target.
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            cascade::gui::drawTrackDetail(ht, &trackInfo_, rxSet_, rxLat_, rxLon_);
            ImGui::EndTooltip();
        }

        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", r.id.c_str());

        // --- the details button ----------------------------------------------
        // Everything the six deleted columns said, on demand, in a window that
        // has room to spell it out. The button is drawn AFTER the row-spanning
        // Selectable and the Selectable allows overlap, so a press here opens
        // the details and does NOT also fly the map: clicking a row to follow
        // an aircraft and getting a dialog instead would be the regression this
        // change most has to avoid.
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(detailsLabel)) {
            detailsTrackId_ = r.id;
            detailsOpen_ = true;
        }

        ImGui::PopStyleVar();
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();  // CellPadding

    // The one thing the list cannot say, said once under the table rather than
    // in a cell per row. It matters MORE now than when there were distance and
    // bearing columns to sit blank: sorting by distance with no receiver
    // position produces an order that looks arbitrary, and this is the line
    // that explains it.
    if (!rxSet_) {
        ImGui::TextDisabled("Distance and bearing need the RX position above.");
    }
}

void AppWindow::drawTrackSortControl() {
    // ONE KEY, NAMED IN FULL. This is what replaced eight sortable headings:
    // the headings had to share the list's width between them and were
    // truncated to "Cal.. ID A.. S.. C.. D.. B.. A..", while a combo shows one
    // key at a time and can spell out both the quantity and its unit.
    ImGui::TextDisabled("Sort");
    ImGui::SameLine();

    // The direction button is a fixed square; the combo takes what is left, so
    // the control fits whatever width the list is dragged to instead of
    // overflowing it.
    const float dirW = ImGui::GetFrameHeight();
    float comboW = ImGui::GetContentRegionAvail().x - dirW - ImGui::GetStyle().ItemSpacing.x;
    if (comboW < 60.0f) { comboW = 60.0f; }
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::BeginCombo("##tracksort", trackSortKeyName(trackSortKey_))) {
        // EVERY KEY THE COLUMNS COULD SORT BY, still here. kTrackSortKeyCount
        // is what stops this loop quietly listing fewer of them than exist:
        // dropping one from the menu is dropping the ability to ask its
        // question, and "what is nearest me" is the question the distance key
        // was added for.
        for (int i = 0; i < cascade::gui::kTrackSortKeyCount; ++i) {
            const TrackSortKey k = trackSortKeyForMenuIndex(i);
            const bool sel = (k == trackSortKey_);
            if (ImGui::Selectable(trackSortKeyName(k), sel)) { trackSortKey_ = k; }
            if (sel) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Order the list. Distance and bearing are measured from the RX position;\n"
            "the value itself is in the target's details and on the map's hover.");
    }
    ImGui::SameLine();
    // An ARROW rather than a caret character, because the arrow is drawn by
    // ImGui and cannot come out as a missing glyph in a font that lacks it.
    if (ImGui::ArrowButton("##tracksortdir",
                           trackSortAscending_ ? ImGuiDir_Up : ImGuiDir_Down)) {
        trackSortAscending_ = !trackSortAscending_;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(trackSortAscending_ ? "Smallest first - click to reverse"
                                              : "Largest first - click to reverse");
    }
}

void AppWindow::drawTargetDetailsWindow() {
    if (!detailsOpen_ || detailsTrackId_.empty()) { return; }

    // A WINDOW, NOT A POPUP AND NOT A PANEL UNDER THE LIST.
    //
    // A popup would be dismissed by the next click anywhere else - including
    // the click on the map to pan it, and the click on the row to follow the
    // aircraft - so the one thing a user does immediately after reading the
    // details would close them. A panel under the list would take the list's
    // own height inside a map window that is often small, which is trading the
    // complaint that started this for a different one.
    //
    // A window can be moved off the map entirely (this application runs ImGui
    // with viewports, so it becomes a real operating-system window), it stays
    // put while the map is used, and it carries a title bar with a close box -
    // plus the Close button below, because a control that is obvious is worth
    // one line.
    // IT RESIZES TO EVERY TARGET, NOT TO THE FIRST ONE. A one-off
    // SetNextWindowSize with a height of zero auto-fits on first use and then
    // never again, so the window kept whatever height the first aircraft
    // needed: open the details for a target the info plugin has no entry for
    // (seven lines), then for one with a registration, type, operator and
    // country, and the extra four lines went behind a scrollbar - taking the
    // age line and all three buttons below the fold. AlwaysAutoResize refits
    // every frame, which is what a block whose line count depends on the target
    // requires; the minimum width stops the "no longer being heard" state,
    // which is two short lines, from collapsing to a sliver.
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Target details", &detailsOpen_, ImGuiWindowFlags_AlwaysAutoResize)) {
        // FOUND BY ID EVERY FRAME. The host's track vector is rebuilt on every
        // poll, so a stored index or pointer would be describing a different
        // aircraft - or freed memory - within a frame or two.
        const std::vector<cascade::core::HostTrack>& tracks = pluginUi_.tracks();
        const cascade::core::HostTrack* found = nullptr;
        for (const cascade::core::HostTrack& ht : tracks) {
            if (!cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) {
                continue;
            }
            if (detailsTrackId_ == ht.t.id) {
                found = &ht;
                break;
            }
        }

        if (found == nullptr) {
            // SAID, NOT SILENTLY CLOSED. A target goes quiet and is dropped by
            // the same staleness rule the map and the list use; a window that
            // shut itself at that moment would look like a crash, and one that
            // kept showing the last values would be lying about a live aircraft.
            ImGui::TextUnformatted(detailsTrackId_.c_str());
            ImGui::Separator();
            ImGui::TextDisabled("No longer being heard.");
        } else {
            cascade::gui::drawTrackDetail(*found, &trackInfo_, rxSet_, rxLat_, rxLon_);
            ImGui::Separator();
            // The same two gestures the row offers, as named buttons: the
            // details window is reachable from a row, and a user who got here
            // should not have to go back to the row to act on what they read.
            if (ImGui::Button("Go to on map")) {
                map_->setSelected(found->t.id);
                map_->goTo(found->t.latDeg, found->t.lonDeg);
                mapOpen_ = true;
            }
            ImGui::SameLine();
            const bool following = (map_->followedId() == found->t.id);
            if (ImGui::Button(following ? "Stop following" : "Follow")) {
                if (following) {
                    map_->clearFollow();
                } else {
                    map_->setSelected(found->t.id);
                    map_->setFollowed(found->t.id);
                    mapOpen_ = true;
                }
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Close")) { detailsOpen_ = false; }
    }
    ImGui::End();
    // Cleared only once the window is actually shut, so the id survives being
    // closed by the title bar's own box as well as by the button.
    if (!detailsOpen_) { detailsTrackId_.clear(); }
}

void AppWindow::drawPluginPresets(const cascade::core::LoadedPlugin& p) {
    // ONE CLICK: go where this decoder listens, in the mode it needs, and show
    // its windows. A decoder knows its own frequency and the user usually does
    // not; making them find out that ADS-B is at 1090 MHz and wants 2 MS/s of
    // raw band is the difference between a plugin that works when you click it
    // and one that appears to do nothing.
    if (p.preset == nullptr) { return; }
    uint32_t n = p.preset->count();
    if (n == 0u) { return; }
    // A plugin is third-party code. A list this long is not a menu, and
    // without a cap a buggy plugin could put thousands of buttons on the
    // panel.
    if (n > kMaxPresetsPerPlugin) { n = kMaxPresetsPerPlugin; }

    for (uint32_t i = 0; i < n; ++i) {
        CascadePreset ps{};
        ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
        if (p.preset->get(i, &ps) != 1) { continue; }
        // Third-party numbers about to command a radio. A frequency that is
        // not a frequency is refused here rather than handed to a driver;
        // written as a positive test because the negation would accept NaN.
        if (!(ps.frequencyHz > 0.0 && ps.frequencyHz < 1e12)) { continue; }

        // Bounded print: the ABI says the label is NUL-terminated, but a
        // plugin that fills every byte must not walk the host off the end.
        char label[CASCADE_PRESET_LABEL_CHARS + 32];
        std::snprintf(label, sizeof(label), "%.*s##preset%u", CASCADE_PRESET_LABEL_CHARS,
                      ps.label[0] != '\0' ? ps.label : p.name.c_str(), i);
        if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) { applyPluginPreset(p, ps); }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Tune to %.4f MHz and open this plugin's windows",
                              ps.frequencyHz / 1.0e6);
        }
    }
}

void AppWindow::applyPluginPreset(const cascade::core::LoadedPlugin& p,
                                  const CascadePreset& ps) {
    // A PRESET ON A STOPPED PLUGIN STARTS IT. Pressing "ADS-B 1090 MHz" is an
    // unambiguous "I want this plugin now", and the alternative is the worst
    // outcome this feature can produce: the radio dutifully retunes to 1090 MHz
    // in the mode the plugin asked for, and then nothing decodes, because the
    // plugin the user just pressed a button on is switched off. Only recorded
    // here — the rebuild at the end of this function is the one that creates
    // the instances, and it has to happen after the receiver has moved anyway.
    //
    // The row keeps its Stop button, so this is undone with one click.
    recordPluginStopped(cascade::core::pluginKey(p), false);

    // MODE FIRST, because the mode's default bandwidth would otherwise
    // overwrite the one the preset asked for.
    if (ps.demodMode != CASCADE_DEMOD_UNCHANGED && ps.demodMode <= CASCADE_DEMOD_RAW) {
        // The ABI's numbering is deliberately its own rather than an alias of
        // this table's order: a plugin compiled today must not change meaning
        // because the host reordered its buttons.
        static const int kAbiToIndex[9] = {
            -1,  // UNCHANGED, handled above
            0,   // NFM
            1,   // WFM
            2,   // AM
            3,   // DSB
            4,   // USB
            5,   // CW
            6,   // LSB
            7    // RAW
        };
        const int idx = kAbiToIndex[ps.demodMode];
        if (idx >= 0) {
            modeIndex_ = idx;
            pipeline_.setDemodMode(kModeMap[idx]);
            bandwidthIndex_ = kModeDefaultBw[idx];
            vfoBandwidthHz_ = kBwHz[bandwidthIndex_];
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
        }
    }

    // The device rate, before the tune, because changing it re-plans the whole
    // chain. Advisory: a source that refuses simply keeps the rate it had, and
    // the decoder will say so itself rather than the host guessing.
    if (ps.sampleRateHz > 0.0 && soapy_ != nullptr &&
        pipeline_.activeSource().sampleRateHz() != ps.sampleRateHz) {
        if (soapy_->setSampleRateHz(ps.sampleRateHz)) {
            // The combo follows, or it would keep showing the rate the user
            // last picked while the radio ran at another.
            soapyRateIndex_ = nearestIndex(kSoapyRateHz, kSoapyRateCount,
                                           pipeline_.activeSource().sampleRateHz());
            followInputRate();
        }
    }

    if (ps.bandwidthHz > 0.0) {
        const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
        vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(ps.bandwidthHz, bwHi));
        pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
        bandwidthIndex_ = nearestIndex(kBwHz, 6, vfoBandwidthHz_);
    }

    // WHERE the frequency goes differs by decoder kind, and getting it wrong
    // half-works in a way that is hard to diagnose. An I/Q decoder is handed
    // the whole raw device band and tunes inside it, so the BAND must contain
    // its signal - hence the device centre. An audio decoder wants its signal
    // in the tuned channel, so the VFO goes there and the offset is preserved.
    if ((ps.flags & CASCADE_PRESET_DEVICE_CENTRE) != 0u) {
        retuneSourceHz(ps.frequencyHz);
    } else {
        tuneAbsoluteHz(ps.frequencyHz);
    }

    // Rebuild the decoders against the receiver they are now pointed at: the
    // rate and centre are passed to create() and cannot be changed on a live
    // instance, so without this the plugin the user just clicked would still
    // be configured for wherever the radio used to be.
    refreshPluginRunner();

    // ...and show what it produces. A tune with nothing on screen is the same
    // "did that work?" the button exists to answer. The map opens only for a
    // plugin that actually puts things on it; panels and image windows are
    // opened by their own capabilities being present.
    if (p.trackSource != nullptr) { mapOpen_ = true; }

    char note[192];
    std::snprintf(note, sizeof(note), "Tuned to %.4f MHz for %s", ps.frequencyHz / 1.0e6,
                  p.name.c_str());
    presetNote_ = note;
}

void AppWindow::applyPluginTuneGrants() {
    // Every entry here is a PluginUi::tuneKey() - a module file name. A config
    // written by an older build holds DISPLAY NAMES instead; those match no
    // module, so such a grant reverts to its default of OFF and shows up as a
    // revocable row rather than quietly granting anything.
    for (const std::string& k : pluginTuneAllowed_) { pluginUi_.setTuneAllowed(k, true); }
}

void AppWindow::setPluginTuneAllowed(const std::string& pluginKey, bool allowed) {
    pluginUi_.setTuneAllowed(pluginKey, allowed);
    const auto it =
        std::find(pluginTuneAllowed_.begin(), pluginTuneAllowed_.end(), pluginKey);
    if (allowed && it == pluginTuneAllowed_.end()) {
        pluginTuneAllowed_.push_back(pluginKey);
    } else if (!allowed && it != pluginTuneAllowed_.end()) {
        pluginTuneAllowed_.erase(it);
    }
}

bool AppWindow::pluginIsStopped(const std::string& pluginKey) const {
    // An empty key is what a record with no path produces; it must never
    // match, or one stray entry would stop every path-less plugin at once.
    if (pluginKey.empty()) { return false; }
    return std::find(pluginsStopped_.begin(), pluginsStopped_.end(), pluginKey) !=
           pluginsStopped_.end();
}

void AppWindow::recordPluginStopped(const std::string& pluginKey, bool stopped) {
    if (pluginKey.empty()) { return; }
    const auto it = std::find(pluginsStopped_.begin(), pluginsStopped_.end(), pluginKey);
    if (stopped && it == pluginsStopped_.end()) {
        pluginsStopped_.push_back(pluginKey);
    } else if (!stopped && it != pluginsStopped_.end()) {
        pluginsStopped_.erase(it);
    }
    // Down into both halves immediately. refreshPluginRunner pushes them again
    // before every rebuild, but a caller that only records (the preset path
    // does) still leaves the live objects agreeing with the durable list.
    pluginRunner_.setStopped(pluginsStopped_);
    pluginUi_.setStopped(pluginsStopped_);
    // The mute snapshot holds the same running state and is read every frame,
    // so it has to follow here too, not only at the next rebuild.
    rebuildMuteStates();
}

void AppWindow::setPluginStopped(const std::string& pluginKey, bool stopped) {
    // THE SAME LIFECYCLE PATH AS EVERYTHING ELSE, deliberately. Stopping could
    // have destroyed one plugin's instances in place, and that is precisely
    // the second lifecycle this avoids: the retune grant, the basemap, the
    // track-info client and the panel windows are all wired up in
    // refreshPluginRunner, so a bespoke teardown would have to repeat every one
    // of them and would drift from the original the first time one changed.
    // Rebuilding costs the other plugins one create()/destroy() pair on a user
    // action that happens seconds apart at worst.
    recordPluginStopped(pluginKey, stopped);
    refreshPluginRunner();
}

// --- Audio mute while a data decoder is running -------------------------------

bool AppWindow::pluginMutes(const cascade::core::LoadedPlugin& p) const {
    const bool def = cascade::core::muteDefaultForCaps(p.capabilities);
    const std::string key = cascade::core::pluginKey(p);
    if (key.empty()) { return def; }
    const bool overridden = std::find(pluginMuteOverride_.begin(),
                                      pluginMuteOverride_.end(),
                                      key) != pluginMuteOverride_.end();
    return overridden ? !def : def;
}

void AppWindow::setPluginMutes(const cascade::core::LoadedPlugin& p, bool mutes) {
    const std::string key = cascade::core::pluginKey(p);
    if (key.empty()) { return; }
    // Stored as a DIFFERENCE from the capability default (see the AppConfig
    // note): choosing the default removes the entry, so a config never carries
    // a redundant override that a later improvement to the default rule could
    // not reach.
    const bool wantOverride = (mutes != cascade::core::muteDefaultForCaps(p.capabilities));
    const auto it = std::find(pluginMuteOverride_.begin(), pluginMuteOverride_.end(), key);
    if (wantOverride && it == pluginMuteOverride_.end()) {
        pluginMuteOverride_.push_back(key);
    } else if (!wantOverride && it != pluginMuteOverride_.end()) {
        pluginMuteOverride_.erase(it);
    }
    rebuildMuteStates();
}

void AppWindow::rebuildMuteStates() {
    muteStates_.clear();
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        cascade::core::MutePlugin m;
        m.key = cascade::core::pluginKey(p);
        m.name = p.name;
        // RUNNING MEANS ACTUALLY DECODING, and both halves are needed.
        //
        // isFeeding alone would be right but LATE: the stop list is pushed
        // down before the runner is rebuilt (see recordPluginStopped, which
        // records without rebuilding at all on the preset path), so a plugin
        // stopped a moment ago still has its instances and would keep the
        // audio muted until the next rebuild - the exact opposite of what
        // stopping it was for.
        //
        // pluginIsStopped alone is what the audio mute used to ask, and it
        // cannot tell an idle plugin from a working one: with the generator at
        // 2 MS/s and the receiver on 162.000 MHz the Sinks panel read "Muted by
        // AIS" while the Plugins panel read that AIS needs 192 kHz raw I/Q and
        // was not being fed. Silence on behalf of a decoder the application
        // itself says is doing nothing is silence for a reason that is not a
        // reason.
        m.running = !pluginIsStopped(m.key) && pluginRunner_.isFeeding(m.key);
        m.mutes = pluginMutes(p);
        if (p.preset != nullptr) {
            uint32_t n = p.preset->count();
            // The same cap the preset BUTTONS get, for the same reason: this
            // walks third-party code and a plugin claiming thousands of
            // presets must not turn a frequency comparison into an unbounded
            // loop.
            if (n > kMaxPresetsPerPlugin) { n = kMaxPresetsPerPlugin; }
            for (uint32_t i = 0; i < n; ++i) {
                CascadePreset ps{};
                ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
                if (p.preset->get(i, &ps) != 1) { continue; }
                // The same sanity test applyPluginPreset applies before
                // commanding a radio, and written the same way (positive, so
                // NaN is refused rather than accepted by a negation).
                if (!(ps.frequencyHz > 0.0 && ps.frequencyHz < 1e12)) { continue; }
                cascade::core::MutePreset mp;
                mp.frequencyHz = ps.frequencyHz;
                mp.bandwidthHz = (ps.bandwidthHz > 0.0 && ps.bandwidthHz < 1e12)
                                     ? ps.bandwidthHz
                                     : 0.0;
                mp.deviceCentre = (ps.flags & CASCADE_PRESET_DEVICE_CENTRE) != 0u;
                m.presets.push_back(mp);
            }
        }
        muteStates_.push_back(std::move(m));
    }
}

std::string AppWindow::muteNameList(const std::vector<std::string>& names) {
    if (names.empty()) { return std::string(); }
    if (names.size() == 1) { return names[0]; }
    // "A and B" for two, "A, B and C" beyond. Several data decoders running at
    // once is an ordinary thing to do - ADS-B and AIS share no band but a user
    // watching both has both running - and a message that named only the first
    // would send them to stop a plugin that was not the whole reason.
    std::string s;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) { s += (i + 1 == names.size()) ? " and " : ", "; }
        s += names[i];
    }
    return s;
}

std::string AppWindow::muteSubjectText() const {
    return muteNameList(mutedBy_);
}

void AppWindow::updateAudioMute() {
    cascade::core::TunePoint tune;
    tune.deviceCentreHz = pipeline_.activeSource().centerFrequencyHz();
    tune.tunedHz = tune.deviceCentreHz + pipeline_.vfoOffsetHz();

    const cascade::core::MuteDecision d =
        cascade::core::muteActive(muteStates_, tune);

    // ARE THE PLUGINS THAT WERE MUTING STILL RUNNING? This is what separates
    // "the user tuned away" from "the user stopped the plugin" - both end the
    // mute, and only the first is worth a dialog. Asked about the specific
    // plugins named in mutedByKeys_ rather than about muting plugins in
    // general; see the note on cascade::core::anyStillRunning for the case
    // that proved the difference on the real application.
    const bool stillRunning =
        cascade::core::anyStillRunning(muteStates_, mutedByKeys_);

    // THE EDGE, evaluated before anything is changed.
    const bool edge = cascade::core::tuneAwayEdge(mutePrevOnPreset_, d.active);
    if (edge && stillRunning) {
        muteKeptRunning_ = true;  // the audio stays down until they answer
    }
    // WHAT THE DIALOG IS ABOUT is decided here, once, from the names that were
    // muting on the frame the user left the preset - and is withdrawn here too
    // when arriving on a preset makes the question moot, or when the plugins it
    // named stop. mutedBy_/mutedByKeys_ still hold the latched names at this
    // point (they are only recomputed below, and only while ON a preset), so
    // the edge captures the plugins the user actually tuned away from.
    const bool wasOpen = mutePopup_.open;
    mutePopup_ = cascade::core::advanceMutePopup(mutePopup_, d.active, edge,
                                                 stillRunning, mutedBy_, mutedByKeys_);
    if (mutePopup_.open && !wasOpen) {
        mutePopupQueued_ = true;
    } else if (!mutePopup_.open) {
        // A withdrawal has to cancel a queue as well as an open window: the
        // queue is one frame long, and opening a dialog the frame after the
        // radio arrived on a preset would ask the question in the one place it
        // must never be asked.
        mutePopupQueued_ = false;
    }
    mutePrevOnPreset_ = d.active;

    // Coming BACK to a preset clears the latch, which is what re-arms the
    // popup: the user has to leave again to be asked again. So does the last
    // of those plugins stopping - there is then nothing to keep quiet for, and
    // a banner offering to stop a plugin that is not running would be a button
    // that does nothing.
    if (d.active || !stillRunning) { muteKeptRunning_ = false; }

    const bool muted = d.active || muteKeptRunning_;
    if (d.active) {
        mutedBy_ = d.names;
        mutedByKeys_ = d.keys;
    } else if (!muted) {
        mutedBy_.clear();
        mutedByKeys_.clear();
    }
    // ...and when the latch is holding, mutedBy_ deliberately KEEPS the names
    // from the frame the user tuned away on. They are the plugins the banner
    // is about, and recomputing them off-preset would empty the list and leave
    // a banner that could not say what was muting anything.

    pipeline_.setAudioMuted(muted);
}

void AppWindow::stopMutingPlugins(const std::vector<std::string>& keys) {
    // EXACTLY THE PLUGINS THE MESSAGE NAMED, which is why the keys are an
    // ARGUMENT and not "every running plugin that mutes". Measured on the
    // running application: the wider rule also stopped AIS, which was running
    // 900 MHz away from its own preset and therefore muting nothing, from a
    // dialog whose sentence said "ADS-B". A button must do what the words above
    // it say - so the popup passes what it captured when it opened, and the
    // banner passes what it is displaying.
    //
    // COPIED FIRST because every caller's vector is one of the members cleared
    // below; iterating the caller's own storage while emptying it would be a
    // use-after-clear on the second plugin.
    //
    // All of them in one pass, with a SINGLE rebuild at the end: stopping one
    // at a time would rebuild every other plugin's instances once per plugin
    // stopped, and would leave the audio still muted after the first of what
    // the user read as one decision.
    const std::vector<std::string> copy = keys;
    for (const std::string& k : copy) { recordPluginStopped(k, true); }
    muteKeptRunning_ = false;
    mutedBy_.clear();
    mutedByKeys_.clear();
    mutePopup_ = cascade::core::MutePopupSubject{};
    mutePopupQueued_ = false;
    mutePrevOnPreset_ = false;
    refreshPluginRunner();
}

void AppWindow::drawMutePopup() {
    // Opened here rather than at the edge: ImGui wants OpenPopup and
    // BeginPopupModal in the same ID stack, and the edge is detected before
    // this frame's windows exist.
    if (mutePopupQueued_) {
        ImGui::OpenPopup("Sound is muted##mute_popup");
        mutePopupQueued_ = false;
    }
    // Centred on the main window, because it is asking about something the
    // user just did to the whole radio and a dialog in the corner of a
    // 5120-wide desktop would be missed.
    //
    // ALWAYS, not Appearing, and NoMove with it. Appearing places the window
    // on the one frame where an auto-resizing popup does not yet know its own
    // size, so the pivot has nothing to subtract and the dialog lands high and
    // off centre - measured on the running application, top edge 375 px above
    // where it belonged. Re-centring every frame costs nothing for a
    // two-button dialog and makes the placement the same every time; NoMove is
    // the honest consequence, since a window that re-centres itself would drag
    // straight back anyway.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Sound is muted##mute_popup", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoMove)) {
        return;
    }
    // THE CAPTURED NAMES, not the live decision. A dialog that re-read
    // muteSubjectText() every frame re-labelled itself mid-question when the
    // receiver moved on to a second decoder's preset - see advanceMutePopup for
    // the measurement.
    const std::string who = muteNameList(mutePopup_.names);
    // Withdrawn while the window was open - the radio arrived on a preset, or
    // the plugins it named were stopped from somewhere else. Closing beats
    // asking a question whose answer no longer applies.
    if (!mutePopup_.open || who.empty()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextWrapped("%s is still running and is muting the audio.", who.c_str());
    ImGui::TextWrapped("Stop it so sound resumes?");
    ImGui::Spacing();
    char stopLabel[256];
    std::snprintf(stopLabel, sizeof(stopLabel), "Stop %s and resume sound", who.c_str());
    if (ImGui::Button(stopLabel)) {
        stopMutingPlugins(mutePopup_.keys);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep it running")) {
        // KEEPS THE MUTE, deliberately, because that is the model the sentence
        // above offered: sound resumes when the plugin stops. Releasing the
        // audio here would make "keep it running" mean "and also undo the
        // muting", which is a different answer to a question nobody asked.
        // The banner is what stops that being a silent state.
        muteKeptRunning_ = true;
        // The question has been answered, so it is no longer pending: clearing
        // the subject is what makes the NEXT one a fresh capture rather than a
        // second showing of this one.
        mutePopup_ = cascade::core::MutePopupSubject{};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AppWindow::drawMuteBanner() {
    // Only while the LATCH is holding: on a preset the mute is explained by
    // where the radio is pointed, and the Sinks panel says so. Off the preset
    // it is explained by nothing at all unless this is here, which is the
    // whole of the "silent with no reason" failure this product's idle
    // reasons already exist to prevent.
    if (!muteKeptRunning_) { return; }
    const std::string who = muteSubjectText();
    if (who.empty()) { return; }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
    ImGui::Text("Sound muted by %s", who.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop plugin##mute_banner")) {
        stopMutingPlugins(mutedByKeys_);
    }
}

void AppWindow::drawPluginTuneControls() {
    // WHO GETS A ROW. Only a plugin that declares the host-client capability
    // can ever call request_tune, so only those are offerable — listing the
    // rest would suggest the receiver is at risk from decoders that cannot
    // touch it. A plugin that is NOT currently loaded but still holds a grant
    // gets a row too: it is the only way to revoke a permission given to
    // something since removed or quarantined, and a grant the user cannot see
    // is exactly the kind that should not exist.
    //
    // A row is identified by its MODULE FILE, which is what the grant is keyed
    // on; the display name only labels it, because a plugin picks its own name
    // and two of them may print the same one.
    struct TuneRow {
        std::string key;    // PluginUi::tuneKey()
        std::string label;
    };
    std::vector<TuneRow> rows;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded || p.hostClient == nullptr) { continue; }
        const std::string key = cascade::core::PluginUi::tuneKey(p);
        rows.push_back({key, p.name + " (" + key + ")"});
    }
    const std::size_t loadedRows = rows.size();
    for (const std::string& g : pluginTuneAllowed_) {
        const auto same = [&g](const TuneRow& r) { return r.key == g; };
        if (std::find_if(rows.begin(), rows.end(), same) == rows.end()) {
            rows.push_back({g, g});
        }
    }
    if (rows.empty()) { return; }

    ImGui::SeparatorText("Receiver control");

    // THE DENIAL NOTICE. A tracker that is refused just sits there looking
    // broken; this is the line that turns "why is my plugin doing nothing" into
    // a visible one-click decision, which is why PluginUi records refusals even
    // though it rejects them.
    const std::string& denied = pluginUi_.lastDeniedPlugin();
    if (!denied.empty() && !pluginUi_.tuneAllowed(denied)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
        ImGui::TextWrapped("\"%s\" asked to tune the receiver and was refused.",
                           denied.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::TextWrapped(
        "A ticked plugin may move the receiver's centre frequency — that is how a "
        "satellite tracker follows Doppler. Off by default.");

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const TuneRow& row = rows[i];
        ImGui::PushID(static_cast<int>(i));
        bool allowed = pluginUi_.tuneAllowed(row.key);
        if (ImGui::Checkbox(row.label.c_str(), &allowed)) {
            setPluginTuneAllowed(row.key, allowed);
        }
        // What the plugin has actually DONE with the permission, which is the
        // part that tells the user whether the row matters.
        const std::vector<std::string>& asked = pluginUi_.tuneRequesters();
        if (i >= loadedRows) {
            ImGui::TextDisabled("not currently installed — grant remembered");
        } else if (std::find(asked.begin(), asked.end(), row.key) != asked.end()) {
            ImGui::TextDisabled("has asked to tune this session");
        } else {
            ImGui::TextDisabled("has not asked to tune");
        }
        ImGui::PopID();
    }
}

void AppWindow::pumpDecoderOutput() {
    // Called from drawUi every frame, NOT from the panel. The runner's queue
    // is bounded and drops silently by design (the DSP thread must never
    // block on the GUI), so draining only while the Plugins section happened
    // to be expanded would quietly lose decodes the user never knew existed.
    for (cascade::core::DecodedLine& l : pluginRunner_.drainText()) {
        decoderLog_.push_back(std::move(l));
    }
    // The track-info plugin's status ("cannot reach the registry") lands in
    // the same log: it is the only place a user looks when a plugin is quiet.
    for (std::string& s : trackInfo_.drainText()) {
        cascade::core::DecodedLine l;
        l.plugin = "Aircraft info";
        l.text = std::move(s);
        decoderLog_.push_back(std::move(l));
    }
    while (decoderLog_.size() > kDecoderLogMax) { decoderLog_.pop_front(); }
}

void AppWindow::drawDecoderStatusRows() {
    const std::vector<cascade::core::DecoderStatus> st = pluginRunner_.status();
    if (st.empty()) { return; }

    ImGui::SeparatorText("Decoders");

    // Why a decoder is silent stays HERE rather than moving to the output
    // window, because it is a fact about the installation: "it is installed
    // and shows nothing" is otherwise indistinguishable from "there is
    // nothing on frequency", and only one of those is worth acting on.
    for (const cascade::core::DecoderStatus& s : st) {
        if (s.reason == cascade::core::DecoderIdleReason::Running) { continue; }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", s.detail.c_str());
        ImGui::PopStyleColor();
    }

    if (pluginRunner_.activeCount() == 0) {
        ImGui::TextDisabled("No decoder is running.");
        return;
    }

    if (ImGui::Button(decoderWindowOpen_ ? "Hide decoder output"
                                         : "Show decoder output",
                      ImVec2(-1.0f, 0.0f))) {
        decoderWindowOpen_ = !decoderWindowOpen_;
    }
    if (!decoderLog_.empty()) {
        ImGui::TextDisabled("%d line%s decoded", static_cast<int>(decoderLog_.size()),
                            decoderLog_.size() == 1 ? "" : "s");
    }
}

void AppWindow::drawDecoderWindow() {
    // ITS OWN OPERATING SYSTEM WINDOW, for the same reason the map and the
    // decoded images have one: a decoder's output is the reason the plugin
    // exists, and continuous text - CW, RTTY, APRS - is something to put
    // beside the radio or on a second screen, not to read through a slot in a
    // side panel.
    //
    // Opens itself the first time a decoder actually says something, which is
    // the rule the map already follows. A window that appears before there is
    // anything in it is just another empty box to close.
    if (!decoderLog_.empty() && !decoderWindowEverOpened_) {
        decoderWindowOpen_ = true;
        decoderWindowEverOpened_ = true;
    }
    if (!decoderWindowOpen_) { return; }
    telemetryNotePanel("decoded");

    placeAsSeparateWindow(9);
    if (ImGui::Begin("Decoder output###decoderout", &decoderWindowOpen_)) {
        ImGui::Checkbox("Follow", &decoderAutoScroll_);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##declog")) { decoderLog_.clear(); }
        ImGui::SameLine();
        ImGui::TextDisabled("%d line%s", static_cast<int>(decoderLog_.size()),
                            decoderLog_.size() == 1 ? "" : "s");

        ImGui::BeginChild("##decoderlog", ImVec2(0.0f, 0.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
    if (decoderLog_.empty()) {
        ImGui::TextDisabled("Listening...");
    }
    for (const cascade::core::DecodedLine& l : decoderLog_) {
        // The plugin name is dimmed and the text is not: with two decoders
        // running the tag is how you tell them apart, but the message is what
        // you are reading.
        ImGui::TextDisabled("%s", l.plugin.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(l.text.c_str());
    }
    // Only stick to the bottom when already there, so scrolling back to read
    // something is not yanked away by the next decode.
    if (decoderAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
        ImGui::EndChild();
    }
    ImGui::End();
}

void AppWindow::removeBlockedPlugin(const std::string& fileName) {
    installError_.clear();
    installReport_.clear();

    // No unloadAll() is needed for the delete itself — a blocked plugin was
    // never loaded, which is the entire point of blocking it — but rescan
    // below reloads everything anyway, and unloading first keeps this path
    // identical in shape to removeInstalledPlugin rather than subtly different.
    pluginHost_.unloadAll();
    std::string err;
    if (pluginRepo_.removeQuarantined(pluginDir_, fileName, pluginQuarantineSuffix(), err)) {
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

void AppWindow::reportPluginStatus() {
    // Bounded-run diagnostic only (CASCADE_PLUGIN_STATUS). It exists because
    // "the plugin is disabled" is a claim about what is MAPPED in this
    // process, and a screenshot of a red row proves nothing about that. Every
    // number here is read from the live objects the GUI draws from.
    const std::vector<cascade::core::LoadedPlugin>& list = pluginHost_.plugins();
    const std::size_t blocked = cascade::core::PluginRepo::blockedCount(
        pluginInventory_.plugins, pluginInventory_.policies);
    std::printf("plugin status: dir=%s candidates=%d loaded=%d blocked=%d managed=%d "
                "unmanaged=%d\n",
                pluginDir_.c_str(), static_cast<int>(list.size()),
                static_cast<int>(pluginHost_.loadedCount()), static_cast<int>(blocked),
                static_cast<int>(pluginInventory_.plugins.size()),
                static_cast<int>(pluginInventory_.unmanaged.size()));
    if (!pluginEnforceError_.empty()) {
        std::printf("plugin enforce: %s\n", pluginEnforceError_.c_str());
    }
    for (const cascade::core::LoadedPlugin& p : list) {
        const std::string file = std::filesystem::path(p.path).filename().string();
        std::printf("plugin host: file=%s loaded=%d name=%s version=%s error=%s\n",
                    file.c_str(), p.loaded ? 1 : 0, p.name.c_str(), p.version.c_str(),
                    p.error.empty() ? "-" : p.error.c_str());
    }

    // The SOURCE, because a decoder starved of samples is indistinguishable
    // from a decoder that decoded nothing, and sourceError_ is otherwise only
    // ever shown in the GUI - invisible to exactly the headless runs used to
    // verify decoding.
    // The device rate is printed to six decimals ON PURPOSE. A hardware rate
    // readback is a double the tuner computed from a master clock divider, and
    // it is very often NOT the round number that was asked for. The chain
    // requires an integer channel rate, so 2400000.000001 is refused while
    // "2400000" appears in every log - which reads as a nonsensical refusal of
    // a rate that is obviously fine.
    std::printf("source status: kind=%s name=%s deviceRate=%.6f chainRate=%.6f "
                "centre=%.0f running=%d error=%s\n",
                sourceKind_.c_str(), pipeline_.activeSourceName(),
                pipeline_.activeSource().sampleRateHz(), pipeline_.inputRateHz(),
                pipeline_.activeSource().centerFrequencyHz(),
                pipeline_.running() ? 1 : 0,
                sourceError_.empty() ? "-" : sourceError_.c_str());

    // The RUNNER, reported separately from the host, because "loaded" and
    // "being fed real audio" are different claims and the whole point of this
    // subsystem is the second one. A screenshot of a loaded plugin proves
    // nothing about whether a single sample ever reached it.
    // The UI capabilities, reported like the decoders: "the plugin is loaded"
    // and "the plugin put something on the map" are different claims.
    // tuneGranted is printed beside tuneRequests because the pair is the whole
    // permission story: a request with no grant is a refusal the user can undo,
    // and a grant with no request is a permission nothing is using.
    std::printf("plugin ui: tracks=%d paths=%d panels=%d tuneRequests=%d tuneGranted=%d\n",
                static_cast<int>(pluginUi_.tracks().size()),
                static_cast<int>(pluginUi_.paths().size()),
                static_cast<int>(pluginUi_.panels().size()),
                static_cast<int>(pluginUi_.tuneRequesters().size()),
                static_cast<int>(pluginTuneAllowed_.size()));
    for (const cascade::core::HostPanel& p : pluginUi_.panels()) {
        std::printf("plugin panel: name=%s title=\"%s\" columns=%d rows=%d\n",
                    p.plugin.c_str(), p.title.c_str(), static_cast<int>(p.headings.size()),
                    static_cast<int>(p.rows.size()));
    }
    for (const cascade::core::HostImage& im : pluginImages_) {
        std::printf("plugin image: from=%s %ux%u fmt=%u complete=%d seq=%llu bytes=%zu\n",
                    im.plugin.c_str(), im.width, im.height, im.format, im.complete ? 1 : 0,
                    static_cast<unsigned long long>(im.sequence), im.pixels.size());
    }
    for (const cascade::core::HostTrack& t : pluginUi_.tracks()) {
        std::printf("plugin track: from=%s id=%s label=%s pos=%.4f,%.4f kind=%u\n",
                    t.plugin.c_str(), t.t.id, t.t.label, t.t.latDeg, t.t.lonDeg, t.t.kind);
    }

    // WHICH PLUGINS ARE STOPPED, printed even when the list is empty (as
    // "stopped=0"), because "no line" and "nothing stopped" have to be
    // distinguishable in a bounded run's output - the same rule the counts
    // above follow.
    std::printf("plugin stopped: count=%d\n", static_cast<int>(pluginsStopped_.size()));
    for (const std::string& k : pluginsStopped_) {
        std::printf("plugin stopped: file=%s\n", k.c_str());
    }
    // THE SNAPSHOT THE MUTE DECISION IS TAKEN FROM, one line per loaded
    // plugin. `mutes` is the setting the checkbox shows - the capability
    // default with the user's override applied - and `running` is whether the
    // plugin has any claim on the audio at all, which is where "not stopped"
    // was once wrongly good enough. Printed because the alternative way to
    // find out which plugins mute by default is to read five checkboxes off a
    // screenshot, and a README written from a screenshot is how the shipped
    // documentation came to name two plugins that do not consume I/Q.
    for (const cascade::core::MutePlugin& m : muteStates_) {
        std::printf("plugin mute: file=%s name=%s running=%d mutes=%d presets=%d\n",
                    m.key.c_str(), m.name.c_str(), m.running ? 1 : 0, m.mutes ? 1 : 0,
                    static_cast<int>(m.presets.size()));
    }
    // THE MUTE, printed every run including "not muted", for the same reason
    // the counts above are: in a bounded run "no line" and "nothing is muting"
    // must not look the same.
    std::printf("audio mute: muted=%d by=%s\n",
                pipeline_.audioMuted() ? 1 : 0, muteSubjectText().c_str());
    std::printf("plugin runner: active=%d lines=%d audioFed=%llu iqFed=%llu\n",
                static_cast<int>(pluginRunner_.activeCount()),
                static_cast<int>(decoderLog_.size()),
                static_cast<unsigned long long>(pluginRunner_.audioFramesFed()),
                static_cast<unsigned long long>(pluginRunner_.iqFramesFed()));
    for (const cascade::core::DecoderStatus& s : pluginRunner_.status()) {
        std::printf("plugin decode: name=%s state=%s%s\n", s.plugin.c_str(),
                    s.reason == cascade::core::DecoderIdleReason::Running ? "running"
                                                                          : "idle",
                    s.reason == cascade::core::DecoderIdleReason::Running
                        ? ""
                        : (" reason=" + s.detail).c_str());
    }
    // The last few decoded lines: the only evidence that the chain from
    // antenna to plugin text actually closed.
    std::size_t shown = 0;
    for (auto it = decoderLog_.rbegin(); it != decoderLog_.rend() && shown < 5; ++it, ++shown) {
        std::printf("plugin text: [%s] %s\n", it->plugin.c_str(), it->text.c_str());
    }
    for (const cascade::core::BlockedPlugin& b : pluginBlocked_) {
        // mapped=1 would mean enforcement failed: the host would have a record
        // of a file it was supposed never to see.
        int mapped = 0;
        for (const cascade::core::LoadedPlugin& p : list) {
            const std::string file = std::filesystem::path(p.path).filename().string();
            if (equalsFileNameAscii(file, b.installed.file)) { mapped = 1; }
        }
        const char* reason =
            b.reason == cascade::core::PluginBlockReason::AbiMismatch ? "abi-mismatch"
                                                                      : "below-minimum-version";
        std::printf("plugin blocked: id=%s file=%s version=%s floor=%s catalogue=%s "
                    "reason=%s mapped=%d message=\"%s\"\n",
                    b.installed.id.c_str(), b.installed.file.c_str(),
                    b.installed.version.c_str(),
                    b.policy.minSupportedVersion.empty() ? "-"
                                                         : b.policy.minSupportedVersion.c_str(),
                    b.policy.catalogueVersion.empty() ? "-" : b.policy.catalogueVersion.c_str(),
                    reason, mapped, b.message.c_str());
    }
    for (const cascade::core::PluginUpdate& u : plannedPluginUpdates()) {
        std::printf("plugin update: id=%s from=%s to=%s\n", u.id.c_str(),
                    u.fromVersion.c_str(), u.toVersion.c_str());
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
    telemetryNotePanel("recorder");

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
        // THE TAKE IS SILENT, and this is the only place that can say so while
        // it still matters. The mute sits above the mono downmix the recorder
        // is fed from (Pipeline::processAudioBlock), deliberately - a recording
        // of the output should contain what the output contains - but the
        // counters above climb exactly as they do for a real capture, so
        // without this line a user recording a station while a decoder happens
        // to sit on its preset gets a WAV of digital silence and no hint until
        // they play it back. Measured on the running application: 240673
        // samples, every one of them zero, and the panel said nothing.
        //
        // Only for the AUDIO take. The IQ recorder is fed from the raw tap,
        // above everything the mute touches, so an IQ take is unaffected and
        // saying otherwise here would send someone looking for a fault in a
        // file that is fine.
        const std::string mutedWho = muteSubjectText();
        if (pipeline_.audioMuted() && !mutedWho.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
            ImGui::TextWrapped("Muted by %s - this take is silent",
                               mutedWho.c_str());
            ImGui::PopStyleColor();
        }
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
    telemetryNotePanel("bookmarks");

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
    // I/Q decoders are told for the same reason: they work on the raw band and
    // several of them (ADS-B, AIS) report when the receiver is nowhere near
    // the frequency they need. Reading back from the source rather than
    // trusting the requested value, because a device may land on a nearby
    // tuning step and the decoder should be told where it actually is.
    pluginRunner_.retune(src.centerFrequencyHz());
}

double AppWindow::currentAbsoluteHz() {
    return pipeline_.activeSource().centerFrequencyHz() + pipeline_.vfoOffsetHz();
}

// --- Scanner (P6) ----------------------------------------------------------------

void AppWindow::drawScannerSection() {
    if (!ImGui::CollapsingHeader("Scanner")) { return; }
    telemetryNotePanel("scanner");

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

// --- Web server mode (P11) ---------------------------------------------------

void AppWindow::publishWebAudio() {
    const std::uint64_t produced = pipeline_.audioSamplesProduced();
    if (!webServer_.running()) {
        // Keep the mark current so switching the server on later starts from
        // live rather than trying to publish everything since launch.
        webAudioLastProduced_ = produced;
        return;
    }
    if (produced <= webAudioLastProduced_) {
        // Equal is the ordinary "no new audio" case; less can only mean the
        // counter restarted, and the honest response to both is to re-mark.
        webAudioLastProduced_ = produced;
        return;
    }

    std::uint64_t fresh = produced - webAudioLastProduced_;
    // Pipeline::audioTap holds 4096 frames; anything older than that is gone
    // whatever we do here.
    constexpr std::uint64_t kTapFrames = 4096;
    if (fresh > kTapFrames) {
        fresh = kTapFrames;
    }
    webAudioBuf_.resize(static_cast<std::size_t>(fresh));
    const std::size_t got = pipeline_.audioTap(webAudioBuf_.data(), webAudioBuf_.size());
    webServer_.pushAudio(webAudioBuf_.data(), got);
    webAudioLastProduced_ = produced;
}

void AppWindow::publishWebImages() {
    if (!webServer_.running()) {
        return;
    }
    // Nothing to do unless a picture changed OR the set itself did (a rescan
    // can add or drop a decoder).
    bool dirty = (webImageRevs_.size() != pluginImages_.size());
    for (std::size_t i = 0; !dirty && i < pluginImages_.size(); ++i) {
        dirty = (webImageRevs_[i] != pluginImages_[i].revision);
    }
    if (!dirty) {
        return;
    }

    std::vector<cascade::net::WebImage> out;
    out.reserve(pluginImages_.size());
    webImageRevs_.assign(pluginImages_.size(), 0);
    for (std::size_t i = 0; i < pluginImages_.size(); ++i) {
        const cascade::core::HostImage& src = pluginImages_[i];
        webImageRevs_[i] = src.revision;
        cascade::net::WebImage w;
        w.plugin = src.plugin;
        w.width = src.width;
        w.height = src.height;
        w.complete = src.complete;
        w.revision = src.revision;
        std::string err;
        // A decoder that has produced no pixels yet encodes to nothing; the
        // slot is still published so the browser can show that it exists and
        // is waiting, rather than the picture appearing from nowhere later.
        if (!cascade::core::encodeBmp24(src, w.bmp, err)) {
            w.bmp.clear();
        }
        out.push_back(std::move(w));
    }
    webServer_.setImages(std::move(out));
}

void AppWindow::pumpWebTiles() {
    const bool active = basemap_.active();
    // A plugin change mid-session must not leave the old source's tiles being
    // served under the new one's attribution — or at all.
    if (active != webTilesActive_ ||
        (active && basemap_.attribution() != webTileAttribution_)) {
        webServer_.clearTiles();
        webTilesActive_ = active;
        webTileAttribution_ = active ? basemap_.attribution() : std::string();
    }
    if (!active || !webServer_.running()) {
        return;
    }
    const std::vector<cascade::net::WebServer::TileRequest> wants =
        webServer_.takePendingTileRequests();
    // Bounded per frame: each READY tile is a 192 KB copy plus a BMP encode,
    // and this runs on the frame path. Whatever is not served here the browser
    // simply asks for again.
    constexpr std::size_t kMaxTilesPerFrame = 16;
    std::size_t served = 0;
    for (const cascade::net::WebServer::TileRequest& r : wants) {
        if (served >= kMaxTilesPerFrame) {
            break;
        }
        std::vector<std::uint8_t> rgb;
        const std::int32_t got = basemap_.rawTile(r.z, r.x, r.y, rgb);
        if (got == CASCADE_TILE_PENDING) {
            // Asking is what started the plugin fetching; the browser's retry
            // will land after it has had time to finish.
            continue;
        }
        std::vector<std::uint8_t> bmp;
        if (got == CASCADE_TILE_READY) {
            cascade::core::HostImage img;
            img.width = basemap_.tileSize();
            img.height = basemap_.tileSize();
            img.format = CASCADE_IMAGE_RGB24;
            img.pixels = std::move(rgb);
            std::string err;
            if (!cascade::core::encodeBmp24(img, bmp, err)) {
                bmp.clear();  // published as missing: an unencodable tile has no
                              // better answer than "stop asking"
            }
        }
        webServer_.setTile(r.z, r.x, r.y, std::move(bmp));
        ++served;
    }
}

void AppWindow::publishWebSnapshot() {
    // Everything read here is read on the GUI thread, which is the contract
    // activeSource() and its readbacks require. The server's providers only
    // ever copy what this leaves behind.
    cascade::source::IqSource& src = pipeline_.activeSource();
    cascade::net::RadioStatus s;
    s.running = pipeline_.running();
    s.faulted = pipeline_.faulted();
    s.faultMessage = pipeline_.faultMessage();
    s.centerHz = src.centerFrequencyHz();
    s.sampleRateHz = src.sampleRateHz();
    s.sourceName = src.name();  // copied into a std::string here, deliberately
    s.vfoOffsetHz = pipeline_.vfoOffsetHz();
    s.bandwidthHz = vfoBandwidthHz_;
    s.mode = kModeNames[modeIndex_];
    s.signalDb = pipeline_.signalPowerDb();
    s.stereoActive = pipeline_.stereoActive();
    s.squelchDb = squelchDb_;
    s.volume = volume_;
    s.dbMin = dbMin_;
    s.dbMax = dbMax_;
    s.deemphasisIndex = deemphIndex_;
    s.nrEnabled = nrEnabled_;
    s.nrStrength = nrStrength_;
    s.notchEnabled = notchEnabled_;
    s.notchFreqHz = static_cast<double>(notchFreqHz_);
    s.notchQ = static_cast<double>(notchQ_);
    s.autoNotch = autoNotch_;
    s.autoNotchEngaged = pipeline_.autoNotchEngaged();
    s.autoNotchFreqHz = pipeline_.autoNotchFrequencyHz();
    s.stereoEnabled = stereoEnabled_;
    s.pilotLocked = pipeline_.pilotLocked();
    s.sourceKind = sourceKind_;
    s.soapyArgs = soapyArgs_;
    s.antenna = soapyAntenna_;
    s.antennas = soapyAntennas_;
    s.agcSupported = soapyAgcSupported_;
    s.agc = soapyAgc_;
    s.sourceBusy = soapyScanPending_ || soapyOpenPending_;
    s.sourceError = sourceError_;
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        s.devices.push_back({d.label, d.args});
    }
    // THE DEVICE THAT IS ALREADY OPEN MUST APPEAR IN THE LIST even when no
    // scan has run this session. soapyDevices_ is filled only by an explicit
    // scan (enumeration walks the USB bus, so it is never done automatically),
    // but a device restored from the config is open and receiving — and a
    // source list that omitted it showed only "Signal generator" while the
    // radio was plainly working, with no way to select it back after switching
    // away.
    if (sourceKind_ == "soapy" && !soapyArgs_.empty()) {
        bool listed = false;
        for (const cascade::net::RadioStatus::SoapyDevice& d : s.devices) {
            if (d.args == soapyArgs_) { listed = true; break; }
        }
        if (!listed) {
            s.devices.insert(s.devices.begin(), {s.sourceName, soapyArgs_});
        }
    }
    for (std::size_t i = 0; i < soapyGainNames_.size(); ++i) {
        const double db = (i < soapyGainsDb_.size())
                              ? static_cast<double>(soapyGainsDb_[i])
                              : 0.0;
        s.gains.push_back({soapyGainNames_[i], db});
    }

    s.iqRecording = iqRecorder_.recording();
    // The same names the Sinks panel shows, from the same source, so the two
    // clients cannot disagree about why the radio is quiet.
    s.audioMutedBy = muteSubjectText();
    s.audioRecording = audioRecorder_.recording();
    s.iqBytes = iqRecorder_.bytesWritten();
    s.audioBytes = audioRecorder_.bytesWritten();
    s.recordDir = recordDir_;
    s.recordError = recordError_;

    for (const cascade::core::Bookmark& b : freqMgr_.list()) {
        s.bookmarks.push_back({b.name, b.freqHz, b.mode, b.bandwidthHz});
    }

    s.scannerActive = scanner_.active();
    switch (scanner_.state()) {
        case cascade::core::Scanner::State::Idle: s.scannerState = "idle"; break;
        case cascade::core::Scanner::State::Scanning: s.scannerState = "scanning"; break;
        case cascade::core::Scanner::State::Paused: s.scannerState = "paused"; break;
        case cascade::core::Scanner::State::Holding: s.scannerState = "holding"; break;
    }
    s.scanStartHz = scanStartMhz_ * 1.0e6;
    s.scanStopHz = scanStopMhz_ * 1.0e6;
    s.scanStepHz = scanStepKhz_ * 1.0e3;

    for (const cascade::core::HostTrack& t : pluginUi_.tracks()) {
        // THE SAME STALENESS RULE THE DESKTOP MAP APPLIES, so the two views do
        // not disagree about what is still flying. A dropped target simply
        // stops appearing in the snapshot; ageMs is still published for the
        // ones that remain, so the web page is free to fade them on its own
        // terms - that presentation is the web UI's decision, not this one's.
        if (!cascade::core::trackPresentation(t.t.ageMs, t.t.kind).visible) { continue; }
        cascade::net::RadioStatus::Track w;
        w.id = t.t.id;
        w.label = t.t.label;
        w.plugin = t.plugin;
        w.latDeg = t.t.latDeg;
        w.lonDeg = t.t.lonDeg;
        w.altM = t.t.altM;
        w.courseDeg = t.t.courseDeg;
        w.speedMps = t.t.speedMps;
        w.ageMs = t.t.ageMs;
        w.kind = t.t.kind;
        w.flags = t.t.flags;
        // Enrichment from the track-info plugin. Running every frame for
        // every live target, this loop is ALSO what drives the lookups: the
        // ask is non-blocking, PENDING costs nothing, and answers are cached.
        if (trackInfo_.active()) {
            const cascade::gui::TrackInfoCache::Info* d = trackInfo_.get(w.id, w.kind);
            if (d != nullptr) {
                w.infoState = d->known ? 1u : 2u;
                if (d->known) {
                    w.registration = d->registration;
                    w.acType = !d->typeName.empty() ? d->typeName : d->typeCode;
                    w.acOperator = d->operatorName;
                    w.acCountry = d->country;
                }
            }
        }
        s.tracks.push_back(std::move(w));
    }

    for (const cascade::core::HostImage& im : pluginImages_) {
        s.images.push_back({im.plugin, im.width, im.height, im.complete, im.revision});
    }

    s.basemap.active = basemap_.active();
    s.basemap.attribution = basemap_.attribution();
    s.basemap.minZoom = basemap_.minZoom();
    s.basemap.maxZoom = basemap_.maxZoom();
    s.basemap.tileSize = basemap_.tileSize();

    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        cascade::net::RadioStatus::Plugin w;
        w.name = p.name;
        w.version = p.version;
        w.licence = p.licence;
        w.loaded = p.loaded;
        w.error = p.error;
        // Also the tune-grant key, and taken from the one place that computes
        // it so the two can never drift: the browser echoes this string back
        // to toggle the permission.
        w.fileName = cascade::core::PluginUi::tuneKey(p);
        // Keyed on the same file name, and read from the same durable list the
        // desktop row reads, so the browser cannot claim a stopped plugin is
        // running.
        w.stopped = pluginIsStopped(w.fileName);
        w.canRequestTune = (p.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u;
        // Keyed on the module file (w.fileName), never on the display name:
        // the browser sends that same key back to toggle the grant, and a name
        // is the plugin's own to choose.
        w.tuneAllowed = std::find(pluginTuneAllowed_.begin(), pluginTuneAllowed_.end(),
                                  cascade::core::PluginUi::tuneKey(p)) !=
                        pluginTuneAllowed_.end();
        // Declared presets, filtered by the SAME rules drawPluginPresets uses:
        // capped, because a plugin is third-party code and a list this long is
        // not a menu, and each frequency positively tested so a value that is
        // not a frequency (NaN included — which is why this is written as a
        // positive test rather than a negation) never reaches the browser or,
        // through it, a driver.
        if (p.preset != nullptr) {
            std::uint32_t n = p.preset->count();
            if (n > kMaxPresetsPerPlugin) { n = kMaxPresetsPerPlugin; }
            for (std::uint32_t i = 0; i < n; ++i) {
                CascadePreset ps{};
                ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
                if (p.preset->get(i, &ps) != 1) { continue; }
                if (!(ps.frequencyHz > 0.0 && ps.frequencyHz < 1e12)) { continue; }
                cascade::net::RadioStatus::Plugin::Preset wp;
                // The ABI says the label is NUL-terminated, but a plugin that
                // fills every byte must not walk us off the end.
                wp.label.assign(ps.label,
                                strnlen(ps.label, CASCADE_PRESET_LABEL_CHARS));
                if (wp.label.empty()) { wp.label = p.name; }
                wp.frequencyHz = ps.frequencyHz;
                wp.bandwidthHz = ps.bandwidthHz;
                wp.sampleRateHz = ps.sampleRateHz;
                w.presets.push_back(std::move(wp));
            }
        }
        s.plugins.push_back(std::move(w));
    }

    s.catalogueStatus = catalogStatus_;
    s.catalogueError = catalogError_;
    s.catalogueBusy = catalogPending_ || installPending_;
    s.installReport = installReport_;
    s.installError = installError_;
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        const cascade::core::PluginCatalogEntry& e = catalog_[static_cast<std::size_t>(i)];
        cascade::net::RadioStatus::CatalogEntry c;
        c.id = e.id;
        c.name = e.name;
        c.version = e.version;
        c.licence = e.licence;
        c.summary = e.summary;
        c.legalNotice = e.legalNotice;
        c.installed = catalogEntryInstalled(e);
        // THE SAME predicate the desktop's Install button consults, asked as
        // "would this be installable if the notice were acknowledged" — so the
        // browser and the window can never disagree about what may be
        // installed, and the notice itself is still a separate, explicit act.
        c.blockedReason = pluginInstallBlockedReason(i, /*acknowledged=*/true);
        s.catalogue.push_back(std::move(c));
    }

    // The tail of the decoder log. Bounded here rather than sending the whole
    // deque: this is a live readout, and the panel the desktop shows is a tail
    // too.
    {
        constexpr std::size_t kMaxWebDecoded = 60;
        const std::size_t total = decoderLog_.size();
        const std::size_t from = (total > kMaxWebDecoded) ? total - kMaxWebDecoded : 0;
        for (std::size_t i = from; i < total; ++i) {
            s.decoded.push_back({decoderLog_[i].plugin, decoderLog_[i].text});
        }
    }
    {
        const cascade::core::RdsSnapshot rds = pipeline_.rdsSnapshot();
        s.rdsSynced = rds.synced;
        s.rdsPiValid = rds.state.piValid;
        s.rdsPi = rds.state.pi;
        s.rdsPsValid = rds.state.psValid;
        s.rdsPs = rds.state.ps;
        s.rdsRadioText = rds.state.radioText;
        s.rdsPty = rds.state.pty;
        s.rdsTp = rds.state.tp;
        s.rdsTa = rds.state.ta;
        s.rdsGroups = rds.state.groupsDecoded;
        s.rdsErrors = rds.state.blockErrors;
    }

    const double centerHz = s.centerHz;
    const double spanHz = pipeline_.inputRateHz();

    std::lock_guard<std::mutex> lock(webMutex_);
    webStatus_ = std::move(s);
    // Copy the bins only when the frame actually advanced. lastFrame_ is what
    // the spectrum panel just drew, so the browser and the window are showing
    // the same data by construction.
    if (lastFrame_.seq != webSeq_) {
        webSeq_ = lastFrame_.seq;
        webBins_ = lastFrame_.dbBins;
        webSnapCenterHz_ = centerHz;
        webSnapSpanHz_ = spanHz;
    }
}

void AppWindow::applyWebControls() {
    std::vector<cascade::net::ControlRequest> requests =
        webServer_.takePendingControls();
    // CAT requests are the SAME ControlRequest type, so they are appended here
    // and applied by the identical code below rather than by a second copy of
    // it. That is the whole reason CAT emits control requests instead of
    // touching the radio: a frequency set over CAT and one set from the
    // browser must do exactly the same thing, including the parts that are
    // easy to forget — telling the RDS decoder to drop the old station,
    // clamping the offset against the live sample rate, and moving the
    // bandwidth to the new mode's default.
    if (catServer_.running()) {
        std::vector<cascade::net::ControlRequest> fromCat =
            catServer_.takePendingControls();
        requests.insert(requests.end(), std::make_move_iterator(fromCat.begin()),
                        std::make_move_iterator(fromCat.end()));
    }
    for (const cascade::net::ControlRequest& r : requests) {
        if (r.running.has_value()) {
            if (*r.running) {
                pipeline_.start();
            } else {
                pipeline_.stop();
            }
        }
        if (r.centerHz.has_value()) {
            // The GUI's own absolute-tune path, so the RDS/stereo decoders are
            // told to forget the old station exactly as they are for a tune
            // made from this window.
            retuneSourceHz(*r.centerHz);
        }
        if (r.mode.has_value()) {
            // Mapped by NAME, never by index: the button table's order and the
            // DemodMode enum's order deliberately differ.
            const std::string want = cascade::dsp::modeName(*r.mode);
            for (int i = 0; i < 8; ++i) {
                if (want == kModeNames[i]) {
                    modeIndex_ = i;
                    pipeline_.setDemodMode(kModeMap[i]);
                    // A mode button here also moves the bandwidth to that
                    // mode's default, so a browser mode change behaves the
                    // same way. An explicit bandwidthHz in the SAME request
                    // still wins, because it is applied below.
                    bandwidthIndex_ = kModeDefaultBw[i];
                    vfoBandwidthHz_ = kBwHz[bandwidthIndex_];
                    pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
                    break;
                }
            }
        }
        // Bandwidth before offset: the offset's limit depends on it.
        if (r.bandwidthHz.has_value()) {
            const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
            vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(*r.bandwidthHz, bwHi));
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
            bandwidthIndex_ = nearestIndex(kBwHz, 6, vfoBandwidthHz_);
        }
        if (r.vfoOffsetHz.has_value()) {
            // Clamped against the LIVE rate, the same rule the config restore
            // uses — web_control's range check is a sanity bound, not this.
            const double lim = 0.5 * pipeline_.inputRateHz() - 0.5 * vfoBandwidthHz_;
            const double off =
                (lim > 0.0) ? std::clamp(*r.vfoOffsetHz, -lim, lim) : 0.0;
            pipeline_.setVfoOffsetHz(off);
            vfoOffsetKhz_ = static_cast<float>(off / 1000.0);
        }
        if (r.squelchDb.has_value()) {
            squelchDb_ = static_cast<float>(*r.squelchDb);
            pipeline_.setSquelchDb(squelchDb_);
        }
        if (r.volume.has_value()) {
            volume_ = static_cast<float>(*r.volume);
            pipeline_.audio().setVolume(volume_);
        }
        // Display range. The same minimum-span rule the desktop sliders
        // enforce, applied against whichever end the request did not supply —
        // a degenerate or inverted span is a divide-by-zero where dB is mapped
        // to pixels.
        if (r.dbMin.has_value() || r.dbMax.has_value()) {
            float lo = r.dbMin.has_value() ? static_cast<float>(*r.dbMin) : dbMin_;
            float hi = r.dbMax.has_value() ? static_cast<float>(*r.dbMax) : dbMax_;
            if (lo > hi - kMinDbSpan) {
                // Push back the end the caller actually moved, so the other
                // does not shift under them.
                if (r.dbMin.has_value()) {
                    lo = hi - kMinDbSpan;
                } else {
                    hi = lo + kMinDbSpan;
                }
            }
            dbMin_ = lo;
            dbMax_ = hi;
            spectrum_->setRange(dbMin_, dbMax_);
        }
        if (r.deemphasisIndex.has_value()) {
            deemphIndex_ = *r.deemphasisIndex;
            pipeline_.setDeemphasisUs(kDeemphUs[deemphIndex_]);
        }
        if (r.stereoEnabled.has_value()) {
            stereoEnabled_ = *r.stereoEnabled;
            pipeline_.setStereoEnabled(stereoEnabled_);
        }
        if (r.nrEnabled.has_value()) {
            nrEnabled_ = *r.nrEnabled;
            pipeline_.setNoiseReductionEnabled(nrEnabled_);
        }
        if (r.nrStrength.has_value()) {
            nrStrength_ = static_cast<float>(*r.nrStrength);
            pipeline_.setNoiseReductionStrength(nrStrength_);
        }
        if (r.notchEnabled.has_value()) {
            notchEnabled_ = *r.notchEnabled;
            pipeline_.setNotchEnabled(notchEnabled_);
        }
        if (r.notchFreqHz.has_value()) {
            notchFreqHz_ = static_cast<float>(*r.notchFreqHz);
            pipeline_.setNotchFrequencyHz(static_cast<double>(notchFreqHz_));
        }
        if (r.notchQ.has_value()) {
            notchQ_ = static_cast<float>(*r.notchQ);
            pipeline_.setNotchQ(static_cast<double>(notchQ_));
        }
        if (r.autoNotch.has_value()) {
            autoNotch_ = *r.autoNotch;
            pipeline_.setAutoNotchEnabled(autoNotch_);
        }

        // --- Source ---------------------------------------------------------
        // All of this runs on the GUI thread by construction (applyWebControls
        // is called from drawUi), which is what makes it safe to touch the
        // source at all.
        if (r.scanDevices.value_or(false)) {
            scanSoapy();
        }
        if (r.sourceKind.has_value()) {
            if (*r.sourceKind == "siggen") {
                selectSource(0);
            } else if (*r.sourceKind == "soapy" && r.soapyArgs.has_value()) {
                // Matched against the SCANNED list rather than passed to the
                // driver verbatim: a browser must not be able to hand
                // arbitrary kwargs to a vendor module, and an unknown string
                // is simply not a device this receiver has seen.
                bool found = false;
                for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
                    if (soapyDevices_[i].args == *r.soapyArgs) {
                        selectSource(static_cast<int>(i) + 2);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    sourceError_ = "no scanned device matches those arguments; "
                                   "rescan and try again";
                }
            }
        }
        // The remaining source settings only mean anything with a device open.
        if (soapy_ != nullptr) {
            if (r.antenna.has_value()) {
                if (soapy_->setAntenna(*r.antenna)) {
                    soapyAntenna_ = soapy_->antenna();  // readback, not the request
                } else {
                    sourceError_ = soapy_->lastError();
                }
            }
            if (r.sampleRateHz.has_value()) {
                if (soapy_->setSampleRateHz(*r.sampleRateHz)) {
                    followInputRate();
                } else {
                    sourceError_ = soapy_->lastError();
                }
            }
            if (r.gainName.has_value() && r.gainDb.has_value()) {
                if (soapy_->setGainDb(*r.gainName, *r.gainDb)) {
                    for (std::size_t i = 0; i < soapyGainNames_.size(); ++i) {
                        if (soapyGainNames_[i] == *r.gainName &&
                            i < soapyGainsDb_.size()) {
                            soapyGainsDb_[i] = static_cast<float>(*r.gainDb);
                        }
                    }
                } else {
                    sourceError_ = soapy_->lastError();
                }
            }
            if (r.agc.has_value() && soapyAgcSupported_) {
                if (soapy_->setAutoGain(*r.agc)) {
                    soapyAgc_ = *r.agc;
                } else {
                    sourceError_ = soapy_->lastError();
                }
            }
        }

        // --- Recorder --------------------------------------------------------
        // The install/teardown ORDER is the Recorder contract's, not a choice:
        // start() then set*Recorder for a new take, set*Recorder(nullptr) then
        // stop() to end one. The stop* helpers already do the second.
        if (r.recordIq.has_value()) {
            if (*r.recordIq && !iqRecorder_.recording()) {
                const double rate = pipeline_.inputRateHz();
                std::string err;
                if (iqRecorder_.start(cascade::core::RecordKind::BasebandIq,
                                      recordDir_, rate, err)) {
                    iqRecordRateHz_ = rate;
                    iqRecordStartS_ = ImGui::GetTime();
                    pipeline_.setIqRecorder(&iqRecorder_);
                    recordError_.clear();
                } else {
                    recordError_ = err;
                }
            } else if (!*r.recordIq) {
                stopIqRecording();
            }
        }
        if (r.recordAudio.has_value()) {
            if (*r.recordAudio && !audioRecorder_.recording()) {
                std::string err;
                if (audioRecorder_.start(cascade::core::RecordKind::Audio, recordDir_,
                                         cascade::core::Pipeline::kAudioRateHz, err)) {
                    audioRecordStartS_ = ImGui::GetTime();
                    pipeline_.setAudioRecorder(&audioRecorder_);
                    recordError_.clear();
                } else {
                    recordError_ = err;
                }
            } else if (!*r.recordAudio) {
                stopAudioRecording();
            }
        }

        // --- Bookmarks -------------------------------------------------------
        // Indexes are re-checked against the LIVE list: the browser's copy can
        // be a poll out of date, and acting on a stale index would tune to, or
        // delete, the wrong entry.
        if (r.bookmarkAdd.has_value()) {
            cascade::core::Bookmark b;
            b.name = *r.bookmarkAdd;
            b.freqHz = currentAbsoluteHz();
            b.mode = kModeNames[modeIndex_];
            b.bandwidthHz = vfoBandwidthHz_;
            freqMgr_.add(b);
            saveBookmarks();
        }
        if (r.bookmarkTune.has_value()) {
            const std::size_t i = static_cast<std::size_t>(*r.bookmarkTune);
            if (i < freqMgr_.list().size()) {
                const cascade::core::Bookmark b = freqMgr_.list()[i];
                for (int m = 0; m < 8; ++m) {
                    if (b.mode == kModeNames[m]) {
                        modeIndex_ = m;
                        pipeline_.setDemodMode(kModeMap[m]);
                        break;
                    }
                }
                const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
                vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(b.bandwidthHz, bwHi));
                pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
                bandwidthIndex_ = nearestIndex(kBwHz, 6, vfoBandwidthHz_);
                tuneAbsoluteHz(b.freqHz);
            }
        }
        if (r.bookmarkRemove.has_value()) {
            if (freqMgr_.removeAt(static_cast<std::size_t>(*r.bookmarkRemove))) {
                saveBookmarks();
            }
        }

        // --- Scanner ---------------------------------------------------------
        if (r.scanStartHz.has_value()) { scanStartMhz_ = *r.scanStartHz / 1.0e6; }
        if (r.scanStopHz.has_value()) { scanStopMhz_ = *r.scanStopHz / 1.0e6; }
        if (r.scanStepHz.has_value()) { scanStepKhz_ = *r.scanStepHz / 1.0e3; }
        // --- Plugins ---------------------------------------------------------
        if (r.pluginFetch.value_or(false)) {
            startCatalogFetch();
        }
        if (r.pluginInstall.has_value()) {
            // The legal notice must be acknowledged explicitly, exactly as the
            // desktop requires a ticked box — and the gate is the SAME
            // predicate, so a plugin the window refuses to install is refused
            // here too, for the same stated reason.
            const bool ack = r.acknowledgeNotice.value_or(false);
            bool done = false;
            for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
                if (catalog_[static_cast<std::size_t>(i)].id != *r.pluginInstall) {
                    continue;
                }
                const std::string blocked = pluginInstallBlockedReason(i, ack);
                if (!blocked.empty()) {
                    installError_ = blocked;
                } else {
                    startInstall(catalog_[static_cast<std::size_t>(i)]);
                }
                done = true;
                break;
            }
            if (!done) {
                installError_ = "no catalogue entry with that id; fetch the "
                                "catalogue and try again";
            }
        }
        if (r.pluginRemove.has_value()) {
            removeInstalledPlugin(*r.pluginRemove);
        }
        if (r.pluginTuneName.has_value() && r.pluginTuneAllowed.has_value()) {
            setPluginTuneAllowed(*r.pluginTuneName, *r.pluginTuneAllowed);
        }
        if (r.pluginPresetName.has_value() && r.pluginPresetIndex.has_value()) {
            // Re-read the preset from the PLUGIN rather than trusting anything
            // the browser echoed back, and re-apply the same validity test, so
            // the only numbers that reach the receiver are ones the plugin
            // itself just produced. Then applyPluginPreset — the identical
            // path the desktop button takes, which also sets the mode,
            // bandwidth and device rate and opens the plugin's windows.
            for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
                if (lp.name != *r.pluginPresetName || lp.preset == nullptr) { continue; }
                const auto idx = static_cast<std::uint32_t>(*r.pluginPresetIndex);
                if (idx >= lp.preset->count() || idx >= kMaxPresetsPerPlugin) { break; }
                CascadePreset ps{};
                ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
                if (lp.preset->get(idx, &ps) != 1) { break; }
                if (!(ps.frequencyHz > 0.0 && ps.frequencyHz < 1e12)) { break; }
                applyPluginPreset(lp, ps);
                break;
            }
        }

        if (r.scannerActive.has_value()) {
            if (*r.scannerActive) {
                cascade::core::Scanner::Params p;
                p.startHz = scanStartMhz_ * 1.0e6;
                p.stopHz = scanStopMhz_ * 1.0e6;
                p.stepHz = scanStepKhz_ * 1.0e3;
                p.dwellMs = scanDwellMs_;
                p.holdMs = scanHoldMs_;
                p.resumeMs = scanResumeMs_;
                // configure() sanitizes (swaps a reversed range, floors the
                // step), so the browser's values get the same treatment the
                // panel's do.
                scanner_.configure(p);
                scanner_.start(ImGui::GetTime() * 1000.0);
                scannerHasExpected_ = false;
            } else {
                scanner_.stop();
            }
        }
    }
}

void AppWindow::refreshCatServer() {
    catStatus_.clear();
    if (!catEnabled_) {
        catServer_.stop();
        return;
    }
    if (catServer_.running()) {
        return;  // already listening on the configured port
    }
    std::string error;
    if (!catServer_.start(static_cast<std::uint16_t>(catPortMirror_), catBindAll_,
                          error)) {
        // Left enabled in the config on purpose: the usual cause is a port
        // held by something else, which the user fixes and retries. Silently
        // turning the setting off would hide that.
        catStatus_ = error;
        cascade::core::diagWarnf("cat: listener refused on port %d (%s)", catPortMirror_,
                                 error.c_str());
        return;
    }
    // The 120 s freeze that started this whole feature was a CAT client being
    // shut down. Whether one was connected at all is the first question a hang
    // report from that path has to answer.
    cascade::core::diagLogf("cat: listening on port %d (%s)", catPortMirror_,
                            catBindAll_ ? "all interfaces" : "loopback");
}

void AppWindow::applyWebSettings() {
    webError_.clear();
    webNote_.clear();
    webDirty_ = false;

    if (!webCfg_.enabled) {
        webServer_.stop();
        return;
    }

    std::string error;
    if (!webServer_.start(webCfg_, error)) {
        // The policy's refusal text is written to be shown verbatim; a bind
        // failure's is too. Which of the two it was is available from
        // decision(), but the user only needs the sentence.
        webError_ = error;
        cascade::core::diagWarnf("web: server refused to start (%s)", error.c_str());
        return;
    }
    // The PORT and the SCOPE, never the password record and never the bind
    // address's provenance: this line goes in a file the user may send us.
    cascade::core::diagLogf("web: serving on port %d", webServer_.boundPort());

    const cascade::net::BindDecision d = webServer_.decision();
    const int port = webServer_.boundPort();
    if (d.reachableOffMachine) {
        webNote_ = "serving on port " + std::to_string(port) +
                   " to every machine on your network";
    } else {
        webNote_ = "serving at http://127.0.0.1:" + std::to_string(port);
    }
}

void AppWindow::setWebPassword(const std::string& password) {
    webError_.clear();
    if (password.empty()) {
        // Clearing is allowed here; the POLICY decides whether the resulting
        // configuration may still listen, and it refuses an off-machine
        // binding with no password. That refusal is the right place for it —
        // this dialog should not be a second copy of the rule.
        webCfg_.passwordRecord.clear();
        webServer_.revokeAllSessions();
        applyWebSettings();
        return;
    }
    cascade::net::PasswordRecord rec;
    std::string error;
    if (!cascade::net::hashPassword(password, rec, error)) {
        webError_ = error;  // "at least 8 characters", or a CNG failure
        return;
    }
    webCfg_.passwordRecord = rec.serialize();
    // Anyone signed in under the old password should have to prove themselves
    // against the new one.
    webServer_.revokeAllSessions();
    applyWebSettings();
}

void AppWindow::drawCatSection() {
    if (!ImGui::CollapsingHeader("CAT control (rigctld)")) { return; }
    telemetryNotePanel("cat");

    ImGui::TextWrapped(
        "Lets logging and digital-mode software drive this receiver over the "
        "protocol Hamlib's rigctld speaks. Point the client at this machine's "
        "address on the port below and choose a Hamlib \"NET rigctl\" radio.");

    if (ImGui::Checkbox("Accept CAT connections", &catEnabled_)) {
        catServer_.stop();  // a port or scope change needs a fresh listener
        refreshCatServer();
    }

    // "Port##cat": the web section draws its own "Port" field, and neither
    // section pushes an ID (CollapsingHeader, unlike TreeNode, does not open
    // an ID scope) — so both live in the menu column's one scope. Identical
    // labels are identical IDs there, which makes ImGui treat the two fields
    // as ONE widget: typing in either fights the other's value. The "##"
    // suffix is not shown to the user and is how the rest of this file
    // separates same-named widgets.
    if (ImGui::InputInt("Port##cat", &catPortMirror_)) {
        catPortMirror_ = std::max(1024, std::min(catPortMirror_, 65535));
        catServer_.stop();
        refreshCatServer();
    }

    if (ImGui::Checkbox("Reachable from other machines", &catBindAll_)) {
        catServer_.stop();
        refreshCatServer();
    }
    if (catBindAll_) {
        // Said plainly, because it is a blunter exposure than the web server's:
        // there is no password to add.
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                           "This protocol has no password. Anything that can "
                           "reach the port can retune the receiver.");
    }

    if (!catStatus_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                           catStatus_.c_str());
    } else if (catServer_.running()) {
        ImGui::Text("Listening on %s:%d — %d client(s), %llu command(s)",
                    catBindAll_ ? "0.0.0.0" : "127.0.0.1", catPortMirror_,
                    catServer_.clientCount(),
                    static_cast<unsigned long long>(catServer_.commandCount()));
    } else {
        ImGui::TextDisabled("Not listening.");
    }
}

void AppWindow::drawWebSection() {
    if (!ImGui::CollapsingHeader("Web access")) { return; }
    telemetryNotePanel("web");

    if (!webAddressesScanned_) {
        webLocalAddresses_ = cascade::net::localInterfaceAddresses();
        webAddressesScanned_ = true;
    }

    bool enabled = webCfg_.enabled;
    if (ImGui::Checkbox("Serve a browser page", &enabled)) {
        webCfg_.enabled = enabled;
        webDirty_ = true;
    }

    // "A specific address" is offered only when the config already holds one,
    // so the common case is a two-way choice rather than a text field the user
    // has to get right.
    const char* kWhere[] = {"This machine only", "Every network interface",
                            "A specific address"};
    const int whereCount = (webBindChoice_ == 2) ? 3 : 2;
    if (ImGui::Combo("Reachable from", &webBindChoice_, kWhere, whereCount)) {
        if (webBindChoice_ == 0) {
            webCfg_.bindAddress = "127.0.0.1";
        } else if (webBindChoice_ == 1) {
            webCfg_.bindAddress = "0.0.0.0";
        }
        webDirty_ = true;
    }
    if (webBindChoice_ == 2) {
        ImGui::TextDisabled("address: %s", webCfg_.bindAddress.c_str());
    }

    // "Port##web": see the matching note in drawCatSection — the two sections
    // share one ID scope, so both fields need a distinct suffix.
    if (ImGui::InputInt("Port##web", &webPortMirror_)) {
        webPortMirror_ = std::clamp(webPortMirror_, cascade::net::kMinPort,
                                    cascade::net::kMaxPort);
        webCfg_.port = webPortMirror_;
        webDirty_ = true;
    }

    if (ImGui::InputText("User name", webUserBuf_, sizeof(webUserBuf_))) {
        webCfg_.username = webUserBuf_;
        webDirty_ = true;
    }

    // --- Password ------------------------------------------------------------
    const bool hasPassword = !webCfg_.passwordRecord.empty();
    ImGui::TextDisabled(hasPassword ? "a password is set" : "no password set");

    ImGui::InputText("Password", webPassBuf_, sizeof(webPassBuf_),
                     ImGuiInputTextFlags_Password);
    ImGui::InputText("Confirm", webPassConfirmBuf_, sizeof(webPassConfirmBuf_),
                     ImGuiInputTextFlags_Password);
    if (ImGui::Button("Set password")) {
        const std::string a(webPassBuf_);
        const std::string b(webPassConfirmBuf_);
        if (a != b) {
            webError_ = "the two passwords do not match";
        } else {
            setWebPassword(a);
        }
        // Do not leave the plaintext sitting in a buffer once it has been
        // hashed (or rejected).
        std::memset(webPassBuf_, 0, sizeof(webPassBuf_));
        std::memset(webPassConfirmBuf_, 0, sizeof(webPassConfirmBuf_));
    }
    if (hasPassword) {
        ImGui::SameLine();
        if (ImGui::Button("Clear password")) {
            setWebPassword(std::string());
        }
    }

    // --- Apply ---------------------------------------------------------------
    ImGui::Separator();
    ImGui::BeginDisabled(!webDirty_);
    if (ImGui::Button(webDirty_ ? "Apply" : "Applied")) {
        applyWebSettings();
    }
    ImGui::EndDisabled();

    // --- Outcome -------------------------------------------------------------
    if (!webError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
        ImGui::TextWrapped("%s", webError_.c_str());
        ImGui::PopStyleColor();
    } else if (webServer_.running()) {
        ImGui::TextWrapped("%s", webNote_.c_str());
        const cascade::net::BindDecision d = webServer_.decision();
        if (d.reachableOffMachine) {
            // The one thing worth shouting about on this panel.
            ImGui::TextColored(kErrorRed, "reachable from your network");
            for (const std::string& addr : webLocalAddresses_) {
                ImGui::TextDisabled("http://%s:%d", addr.c_str(),
                                    webServer_.boundPort());
            }
            ImGui::TextWrapped(
                "This link is plain HTTP: the password is sent unencrypted, so "
                "use it on a network you trust. To reach it from the internet, "
                "put it behind something that terminates TLS rather than "
                "forwarding this port.");
        }
        if (d.authRequired) {
            ImGui::TextDisabled("%d browser session(s)",
                                static_cast<int>(webServer_.sessionCount()));
        }
        if (webServer_.audioListeners() > 0) {
            ImGui::TextDisabled("%d listening to audio",
                                static_cast<int>(webServer_.audioListeners()));
        }
    } else if (webCfg_.enabled) {
        ImGui::TextDisabled("not serving");
    }
}

void AppWindow::drawUpdatesSection() {
    if (!ImGui::CollapsingHeader("Updates")) { return; }
    telemetryNotePanel("updates");

    if (ImGui::Checkbox("Check for updates at startup", &updateCheckEnabled_)) {
        // Off means off immediately: a check already in flight is not waited
        // for, and none is started again this launch.
        if (!updateCheckEnabled_) {
            update_ = cascade::core::UpdateInfo{};
            updateError_.clear();
        } else if (!updateStarted_) {
            startUpdateCheck();
        }
    }
    ImGui::TextWrapped(
        "Asks foxsdr.com once per launch whether a newer version exists, and sends the version "
        "you are running and nothing else - no identifier, no cookie. Nothing is downloaded or "
        "installed unless you press the button. This is not the usage report below; the two "
        "share nothing.");

    ImGui::TextDisabled("this build: %s", cascade::versionString());
    if (!updateCheckEnabled_) {
        ImGui::TextDisabled("checking is off, so you will not be told about a new version");
    } else if (updatePending_) {
        ImGui::TextDisabled("checking...");
    } else if (update_.newer) {
        ImGui::TextColored(update_.critical ? kErrorRed : ImVec4(1.0f, 0.8f, 0.35f, 1.0f),
                           "%s is available", update_.version.c_str());
        if (updateDismissed_ && ImGui::SmallButton("Show it again")) { updateDismissed_ = false; }
    } else if (updateStarted_ && updateError_.empty()) {
        ImGui::TextDisabled("up to date");
    } else if (!updateError_.empty()) {
        // Only here, never as a banner: a failed check is not the user's
        // problem and must not interrupt them.
        ImGui::TextDisabled("last check did not complete: %s", updateError_.c_str());
    }
}

void AppWindow::drawUsageReportingSection() {
    if (!ImGui::CollapsingHeader("Usage reporting")) { return; }
    telemetryNotePanel("usage reporting");

    // No endpoint compiled in means the feature cannot work, so it is shown as
    // unavailable rather than offering a switch that would collect into
    // nowhere. A source build gets this by default: a fork must not start
    // reporting to us because somebody rebuilt it.
    const std::string endpoint = cascade::core::telemetryEndpoint();
    if (endpoint.empty()) {
        ImGui::TextDisabled("Not available in this build.");
        return;
    }

    ImGui::TextWrapped(
        "Anonymous counts only: version, operating system, how long sessions "
        "run, which modes and plugins get used, and which radio model. Never "
        "frequencies, never anything decoded, never your location, and no IP "
        "address is recorded.");
    ImGui::Spacing();

    bool on = telemetryEnabled_;
    if (ImGui::Checkbox("Send anonymous usage reports", &on)) {
        if (on && !telemetryEnabled_) {
            // The id is created at the moment of consent, never before - so a
            // machine that never opts in has no identifier at all, not even an
            // unused one sitting in its config.
            telemetryInstallId_ = cascade::core::newInstallId();
            telemetryEnabled_ = !telemetryInstallId_.empty();
        } else if (!on) {
            // Off DELETES the identifier, so a later opt-in gets a new one
            // that cannot be tied to the old. Any report still waiting to be
            // sent goes with it.
            telemetryEnabled_ = false;
            telemetryInstallId_.clear();
        }

    }

    if (telemetryEnabled_) {
        ImGui::TextDisabled("Install id: %s", telemetryInstallId_.c_str());
        ImGui::TextDisabled("Sent once at start-up, describing the previous session.");
    } else {
        ImGui::TextDisabled("Off. Nothing is transmitted.");
    }
    if (ImGui::SmallButton("What exactly is sent?")) {
        privacyNoticeOpen_ = !privacyNoticeOpen_;
    }
    if (privacyNoticeOpen_) {
        telemetryNotePanel("privacy notice");
        ImGui::Indent();
        ImGui::TextDisabled(
            "id (random)  version  os  arch  launches  crashes\n"
            "session seconds  sdr model  modes used  panels  plugins\n"
            "See PRIVACY.md for the complete list and what is excluded.");
        ImGui::Unindent();
    }
}

void AppWindow::applyDiagnosticsEnabled(bool on) {
    // THE WHOLE SWITCH, IN ONE PLACE, because it was not. The checkbox used to
    // arm and disarm the crash handler and the log inline and never mention
    // the watchdog, so a session that started with diagnostics on and had the
    // box unticked kept a live watchdog writing hang reports - against a
    // promise made in PRIVACY.md, README.md and docs/DIAGNOSTICS.md - and a
    // session switched on mid-flight got everything except the component that
    // catches the fault this product actually ships. Both directions are now
    // one call, and tests/test_diag_hang.cpp drives the real application
    // through THIS function in both of them.
    diagnosticsEnabled_ = on;
    cascade::core::setCrashCaptureEnabled(diagnosticsEnabled_, diagnosticsMinidump_);
    // Guarded on diagCrashDir_ for the same reason as in run(): a run that was
    // never allowed to write (a bounded CI run) must not start writing because
    // a switch was flipped.
    if (!diagCrashDir_.empty()) {
        cascade::core::DiagLog::instance().configure(cascade::core::diagLogDir(),
                                                     diagnosticsEnabled_);
    }
    watchdog_.setReportDir(diagnosticsEnabled_ ? diagCrashDir_ : std::string());
}

void AppWindow::drawDiagnosticsSection() {
    if (!ImGui::CollapsingHeader("Diagnostics")) { return; }
    telemetryNotePanel("diagnostics");

    ImGui::TextWrapped(
        "If FoxSDR crashes or freezes, it writes a report on THIS machine. With "
        "this switch on, that report is also sent to us the NEXT time you open "
        "FoxSDR - never from inside the crash itself, because a program that has "
        "just failed cannot safely use the network.");
    ImGui::Spacing();

    bool on = diagnosticsEnabled_;
    if (ImGui::Checkbox("Record crashes and freezes, and send the report next time", &on)) {
        applyDiagnosticsEnabled(on);
    }
    // WHAT IS SENT AND WHAT IS NOT, at the switch itself. A privacy notice
    // somebody has to go and find is not a disclosure; this is the moment the
    // decision is being made.
    ImGui::TextDisabled(
        "Sent: the version and build, what failed and where (as a file name and\n"
        "an offset), every thread's stack in the same form, the recent log lines,\n"
        "and what the receiver was doing - mode, source, sample rate, radio model\n"
        "with the serial removed, and the plugins that were loaded.\n"
        "NEVER sent: a frequency you tuned to, anything decoded, your position,\n"
        "your name, your machine's name, or the name of any file you opened.\n"
        "At most five a day, and the same fault only once a day.\n"
        "Off means off: no report is written and nothing is sent.");
    ImGui::Spacing();

    bool dump = diagnosticsMinidump_;
    if (ImGui::Checkbox("Also write a full memory dump beside a crash report", &dump)) {
        diagnosticsMinidump_ = dump;
        cascade::core::setCrashCaptureEnabled(diagnosticsEnabled_, diagnosticsMinidump_);
    }
    ImGui::TextDisabled(
        "A memory dump can contain file names, window titles and received signal\n"
        "data. It is written LOCALLY ONLY and is NEVER uploaded under any\n"
        "setting - if it is ever useful you will be asked for it, and you decide.");

    ImGui::Spacing();
    const std::string logPath = cascade::core::DiagLog::instance().filePath();
    if (logPath.empty()) {
        ImGui::TextDisabled("Log: off (nothing is being written).");
    } else {
        ImGui::TextDisabled("Log: %s", logPath.c_str());
    }
    const std::string crashDir = cascade::core::diagCrashDir();
    ImGui::TextDisabled("Reports: %s", crashDir.empty() ? "(unavailable)" : crashDir.c_str());

    ImGui::Spacing();
    if (ImGui::Button("Copy diagnostics")) { copyDiagnosticsBundle(); }
    ImGui::SameLine();
    if (ImGui::Button("Open reports folder")) {
#if defined(_WIN32)
        if (!crashDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(crashDir), ec);
            ::ShellExecuteA(nullptr, "open", crashDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
#endif
    }
    if (!diagBundleStatus_.empty()) { ImGui::TextDisabled("%s", diagBundleStatus_.c_str()); }

    if (ImGui::SmallButton("What exactly is in a report?")) {
        privacyNoticeOpen_ = !privacyNoticeOpen_;
    }
    if (privacyNoticeOpen_) {
        ImGui::Indent();
        ImGui::TextDisabled(
            "version  commit  os  arch  mode  source  sample rate\n"
            "device open  sdr model (serial stripped)  loaded plugins\n"
            "faulting stack as module+offset, and every thread on a freeze\n"
            "the loaded module list with build ids, and the last 256 log lines\n"
            "NEVER a frequency, never anything decoded, never your position.\n"
            "See PRIVACY.md for the complete list and what is excluded.");
        ImGui::Unindent();
    }
}

void AppWindow::drawDiagnosticsOffer() {
    if (!diagOfferOpen_) { return; }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("FoxSDR did not close normally last time", &diagOfferOpen_,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        telemetryNotePanel("diagnostics offer");
        // WHY THE SENDING IS DESCRIBED CONDITIONALLY. The sweep running on this
        // start does not always send. It declines a signature already sent
        // inside the dedup window, a machine that has used up its per-window
        // allowance, a server backoff left by an earlier 429, a report that
        // will not parse, and one still too large after trimming. Every one of
        // those is ordinary on a machine that has just failed twice in a row -
        // which is precisely the machine this dialog appears on - so a sentence
        // asserting that the text of the report is on its way right now would
        // be untrue most of the times it is read. The old wording erred in the
        // safe direction (it promised more transmission than happens, never
        // less), but it was still a claim the product could not keep.
        //
        // THE LIVE OUTCOME IS DELIBERATELY NOT CONSULTED. crashUploader_
        // .outcome() is written by the sweep thread and is only safe to read
        // after stop(), which happens at shutdown; reading it from the frame
        // loop to make this sentence more specific would trade a copy defect
        // for a data race.
        // SO THE SENTENCE STATES THE ATTEMPT, NOT THE OUTCOME. "is sent
        // unless X or Y" still asserts, by construction, that it IS sent when
        // neither exception applies - and the commonest reason of all is
        // missing from that list: the machine cannot reach the site. A laptop
        // that crashed on a train is the ordinary case, not the exotic one.
        ImGui::TextWrapped(
            "The previous session ended without shutting down. If a report was "
            "written, it is on this machine. On this start we TRY to send its "
            "text - the version, what failed and where, the thread stacks and "
            "the recent log lines. It is not sent if it repeats one already "
            "sent, if this machine is over its send limit, or if foxsdr.com "
            "cannot be reached. Either way the report stays in the reports "
            "folder below. No memory dump is sent, ever. Settings > "
            "Diagnostics lists exactly what goes and turns it off.");
        ImGui::Spacing();
        if (ImGui::Button("Copy diagnostics")) { copyDiagnosticsBundle(); }
        ImGui::SameLine();
        if (ImGui::Button("Open reports folder")) {
#if defined(_WIN32)
            const std::string dir = cascade::core::diagCrashDir();
            if (!dir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(dir), ec);
                ::ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
#endif
        }
        ImGui::SameLine();
        if (ImGui::Button("Not now")) { diagOfferOpen_ = false; }
        if (!diagBundleStatus_.empty()) { ImGui::TextDisabled("%s", diagBundleStatus_.c_str()); }
    }
    ImGui::End();
}

void AppWindow::copyDiagnosticsBundle() {
    // EVERY FIELD HERE ALREADY EXISTS somewhere in this window. A bundle that
    // re-derived the version, the plugin list or the radio model would be a
    // second source of truth for exactly the facts a support conversation
    // turns on, and the two would eventually disagree.
    refreshDiagContext();

    cascade::core::DiagBundleInput in;
    in.context.version = cascade::versionString();
    in.context.commit = cascade::gitCommit();
    in.context.os = cascade::core::osDescription();
    in.context.arch = cascade::core::archDescription();
    in.context.mode = kModeNames[modeIndex_];
    in.context.sourceKind = sourceKind_;
    in.context.sampleRateHz = pipeline_.activeSource().sampleRateHz();
    in.context.deviceOpen = (sourceKind_ == "soapy");
    in.context.sdrModel = cascade::core::sanitiseDevice(soapyArgs_);
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded && in.context.plugins.size() < 32) {
            in.context.plugins.push_back(p.name + " " + p.version);
        }
    }
    in.logLines = cascade::core::DiagLog::instance().ringSnapshot();
    in.logLinesTotal = cascade::core::DiagLog::instance().linesWritten();
    in.logPath = cascade::core::DiagLog::instance().filePath();
    in.crashDir = cascade::core::diagCrashDir();
    in.lastRunUnclean = lastRunUnclean_;
    in.launches = telemetryLaunches_;
    in.crashes = telemetryCrashes_;

    const std::string bundle = cascade::core::buildDiagnosticsBundle(in);
    ImGui::SetClipboardText(bundle.c_str());

    // ...and on disk as well as on the clipboard, because a clipboard does not
    // survive the next copy and a support thread can take days.
    diagBundleStatus_ = "Copied to the clipboard.";
    if (!in.crashDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(in.crashDir), ec);
        const std::string path = in.crashDir + "/diagnostics.txt";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
            out << bundle;
            out.close();
            diagBundleStatus_ = "Copied to the clipboard, and saved as " + path;
        }
    }
    cascade::core::diagLogf("diagnostics bundle produced (%zu bytes)", bundle.size());
}

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

    // The map window's rectangle from the last session. Seeded here rather
    // than read at draw time so the very first Begin("Map") already has it —
    // ImGui's FirstUseEver only fires once, and a value that arrived a frame
    // late would be ignored for the whole session. All four are zero when
    // nothing was saved or the sanitizer rejected it, which is what selects
    // the monitor-derived default in drawPluginWindows.
    mapWinW_ = cfg.mapWindowWidth;
    mapWinH_ = cfg.mapWindowHeight;
    mapWinX_ = cfg.mapWindowX;
    mapWinY_ = cfg.mapWindowY;

    // The receiver's own position from the last session, PUSHED INTO THE MAP
    // here rather than waiting for the user to press the button again. It is
    // what the range rings, the hover readout and the track table's distance
    // and bearing columns are measured from, and a position that had to be
    // re-typed every launch is a position those columns could not rely on.
    // The sanitizer has already discarded anything out of range, so an unset
    // flag here means genuinely unset.
    rxSet_ = cfg.rxPositionSet;
    rxLat_ = cfg.rxLatDeg;
    rxLon_ = cfg.rxLonDeg;
    rxLatInput_ = rxLat_;
    rxLonInput_ = rxLon_;
    if (rxSet_ && map_) {
        map_->setHome(rxLat_, rxLon_);
    }

    // Plugin browser. Restoring the URL and the open/closed state does NOT
    // start a fetch — see AppConfig::pluginCatalogueUrl. The user still has
    // to press Browse, on this launch as on every other.
    pluginCatalogueUrl_ = cfg.pluginCatalogueUrl;
    std::snprintf(pluginUrlBuf_, sizeof(pluginUrlBuf_), "%s", pluginCatalogueUrl_.c_str());
    pluginBrowseOpen_ = cfg.pluginBrowserOpen;
    // Restored purely so it can be saved back unchanged when the user never
    // browses this session. Nothing reads it to decide whether to fetch.
    pluginLastUpdateCheck_ = cfg.pluginLastUpdateCheck;
    // Tune grants are pushed into PluginUi immediately, not just stored: the
    // plugin scan already ran in the constructor, so a tracker created during
    // it may call request_tune on its very first poll — before any rebuild
    // would have re-applied them.
    pluginTuneAllowed_ = cfg.pluginTuneAllowed;
    applyPluginTuneGrants();
    // STOPS ARE APPLIED, not merely stored, for a stronger version of the same
    // reason: the scan in the constructor has already built every plugin's
    // instances against the default source, so a plugin the user stopped last
    // session is running right now. refreshPluginRunner tears that set down and
    // rebuilds it without the stopped ones, which is also what the restored
    // source needs; doing it here means a stopped plugin never survives a
    // launch even for a frame.
    pluginsStopped_ = cfg.pluginsStopped;
    // BEFORE the rebuild, because refreshPluginRunner is what rebuilds the
    // mute snapshot the overrides are baked into. Restored after the stops for
    // the same reason they are restored at all: a decoder the user silenced
    // last session must not come back audible for the seconds it takes them to
    // find the checkbox again.
    pluginMuteOverride_ = cfg.pluginMuteOverride;
    refreshPluginRunner();

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
            // No open can be in flight during the startup restore, but the
            // counter's contract is "every install bumps it" — an invariant
            // with an exception in it is one nobody can rely on later.
            ++sourceGen_;
            pipeline_.setSource(std::move(file));
            sourceKind_ = "file";
            sourceSel_ = 1;
            followInputRate();
            cascade::core::diagLogf("source: restored an I/Q file at %.0f S/s",
                                    pipeline_.activeSource().sampleRateHz());
        } else {
            sourceError_ = file->lastError();
            // THE FACT, NEVER THE PATH - and the reason is five lines up in the
            // Open handler: a file name is the user's own data. The source's
            // error string is "cannot open file: <full absolute path>", and
            // this line goes to the ring, which means it goes into every crash
            // report, every hang report and the Copy diagnostics bundle. The
            // full text stays in sourceError_, which is shown on screen to the
            // person who already knows what they opened.
            cascade::core::diagWarnf("source: the saved I/Q file did not reopen");
        }
    } else if (cfg.sourceKind == "soapy" && !cfg.soapyArgs.empty()) {
        // Seeded BEFORE the open, because openSoapy applies it as part of
        // bringing the device up - the port has to be right from the first
        // sample, not corrected afterwards.
        soapyAntenna_ = cfg.soapyAntenna;
        auto dev = openSoapy(cfg.soapyArgs, cfg.sampleRateHz);
        if (dev) {
            dev->setCenterFrequencyHz(cfg.centerHz);
            soapy_ = dev.get();
            soapyArgs_ = cfg.soapyArgs;
            ++sourceGen_;  // same invariant as the file branch above
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
            cascade::core::diagLogf("source: restored %s at %.0f S/s",
                                    cascade::core::sanitiseDevice(soapyArgs_).c_str(),
                                    pipeline_.activeSource().sampleRateHz());
        } else {
            // openSoapy already set sourceError_. A radio that was there last
            // session and is not there now is the single most common support
            // question this product gets - but the driver's own message quotes
            // the device ARGUMENTS back, and those carry the serial number.
            // Same rule as the line above and as every other place these
            // strings are recorded: the sanitised model, never the raw args.
            cascade::core::diagWarnf("source: the saved radio (%s) did not reopen",
                                     cascade::core::sanitiseDevice(cfg.soapyArgs).c_str());
        }
    }
    if (sourceKind_ == "siggen") {
        // Generator kept (or fallen back to): carry the saved center so the
        // readout matches the last session. Nominal-center set cannot fail.
        pipeline_.activeSource().setCenterFrequencyHz(cfg.centerHz);
    }

    // Web server. Applied LAST in the restore so the snapshot the providers
    // publish is assembled from a fully restored radio; applyWebSettings is
    // what decides whether anything actually listens, and it refuses on its
    // own terms (an off-machine binding with no password never starts).
    webCfg_.enabled = cfg.webEnabled;
    webCfg_.bindAddress = cfg.webBindAddress;
    webCfg_.port = cfg.webPort;
    webCfg_.username = cfg.webUsername;
    webCfg_.passwordRecord = cfg.webPasswordRecord;
    webPortMirror_ = webCfg_.port;
    webBindChoice_ = cascade::net::isLoopbackAddress(webCfg_.bindAddress) ? 0
                     : cascade::net::isWildcardAddress(webCfg_.bindAddress) ? 1
                                                                            : 2;
    std::snprintf(webUserBuf_, sizeof(webUserBuf_), "%s", webCfg_.username.c_str());
    applyWebSettings();

    catEnabled_ = cfg.catEnabled;
    catBindAll_ = cfg.catBindAll;
    catPortMirror_ = cfg.catPort;
    refreshCatServer();

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

void AppWindow::telemetryAccrueMode() {
    // Seconds are banked against the mode that was ACTUALLY running, once per
    // frame. Accumulating against the mode selected at shutdown would credit
    // the whole session to whatever happened to be last.
    //
    // The accrual carries the sub-second remainder between calls. It has to:
    // a frame is ~17 ms, so every individual delta truncates to zero seconds
    // and nothing would ever be banked.
    const std::uint64_t secs = telemetryModeAccrual_.advance(glfwGetTime());
    if (secs > 0) { telemetryModeSeconds_[kModeNames[modeIndex_]] += secs; }
}

void AppWindow::telemetryNotePanel(const char* name) {
    if (name == nullptr || name[0] == '\0') { return; }
    const std::string n(name);
    if (telemetryPanels_.size() >= 16) { return; }
    if (std::find(telemetryPanels_.begin(), telemetryPanels_.end(), n) ==
        telemetryPanels_.end()) {
        telemetryPanels_.push_back(n);
    }
}

void AppWindow::telemetryStartup(const cascade::core::AppConfig& cfg) {
    telemetryEnabled_ = cfg.telemetryEnabled;
    telemetryInstallId_ = cfg.telemetryInstallId;
    // Reporting is on by default, so a first run arrives here enabled with no
    // identifier. Mint one now. If the CSPRNG fails there is no id, and
    // reporting stays off rather than falling back to anything guessable.
    if (telemetryEnabled_ && telemetryInstallId_.empty()) {
        telemetryInstallId_ = cascade::core::newInstallId();
        telemetryEnabled_ = !telemetryInstallId_.empty();
    }
    telemetryLaunches_ = cfg.telemetryLaunches + 1;
    telemetryCrashes_ = cfg.telemetryCrashes;
    // The previous run never wrote its clean-exit marker, so it did not end
    // normally. This is the whole crash-counting mechanism: no crash handler,
    // no minidump, nothing uploaded from the failure itself - just the
    // observation that last time the marker was never set.
    if (!cfg.telemetryCleanExit) { ++telemetryCrashes_; }
    // THE SAME MARKER, REUSED AS THE TRIGGER TO OFFER A REPORT. It already
    // detects exactly "the last run did not end normally", which is precisely
    // the moment to ask - a crash handler can write a report but it cannot ask
    // the user anything, because by then there is no user interface left.
    lastRunUnclean_ = !cfg.telemetryCleanExit;
    diagOfferOpen_ = lastRunUnclean_ && cfg.diagnosticsEnabled;
    // The crash-loop limiter's memory, VALIDATED rather than trusted: it is
    // user-editable text on disk, and the one thing it controls is how much
    // this machine is allowed to send.
    crashUploadState_ = cascade::core::decodePolicyState(
        cfg.crashUploadRecent, cfg.crashUploadWindowStart, cfg.crashUploadWindowCount,
        cfg.crashUploadBlockedUntil);
    telemetrySessionStart_ = glfwGetTime();
    telemetryModeAccrual_.reset(telemetrySessionStart_);
    // Last session's report goes now, on a thread, while the window is coming
    // up. Nothing waits for it and nothing reports if it fails.
    if (telemetryEnabled_ && !telemetryInstallId_.empty() &&
        !cfg.telemetryPending.empty()) {
        telemetryReporter_.send(cascade::core::telemetryEndpoint(), cfg.telemetryPending);
    }
}

void AppWindow::crashUploadStart() {
    // OFF MEANS OFF FOR UPLOADING TOO. Both halves are required: the user's
    // switch, and "may this run write to the machine at all" - a bounded CI run
    // has an empty diagCrashDir_ and must not start sending whatever the config
    // says.
    if (!diagnosticsEnabled_ || diagCrashDir_.empty()) { return; }

    cascade::core::SweepParams p;
    p.crashDir = diagCrashDir_;
    p.url = cascade::core::crashUploadEndpoint();
    // The SAME anonymous id the usage report uses, and empty when usage
    // reporting is off - because then it does not exist. A crash report must
    // not be what mints an identifier the user switched off. See PRIVACY.md.
    p.installId = telemetryEnabled_ ? telemetryInstallId_ : std::string();
    p.enabled = true;
    p.state = crashUploadState_;
    crashUploader_.start(p);
    crashUploadSwept_ = true;
}

void AppWindow::crashUploadFinish() {
    // Cancels first, then joins. The cancel closes the live request handle, so
    // a transfer blocked on a server that never answers returns immediately
    // instead of sitting out its receive timeout.
    crashUploader_.stop();
    if (!crashUploadSwept_) { return; }
    const cascade::core::SweepOutcome out = crashUploader_.outcome();
    crashUploadState_ = out.state;
    if (out.considered > 0) {
        cascade::core::diagLogf(
            "crash upload: %d considered, %d sent, %d duplicate, %d held, %d failed, "
            "%d abandoned, %d refused",
            out.considered, out.sent, out.duplicate, out.limited, out.failed, out.abandoned,
            out.refused);
        for (const std::string& n : out.notes) {
            cascade::core::diagLogf("crash upload: %s", n.c_str());
        }
    }
}

void AppWindow::telemetryJournal(cascade::core::AppConfig& cfg) {
    // The update setting rides along here because this is the one place the
    // GUI's copy of the config is written back before the file is saved. It
    // has nothing to do with telemetry and shares no state with it.
    cfg.updateCheckEnabled = updateCheckEnabled_;

    // Same rationale: this is the one place the GUI's copy of the config is
    // written back, so the diagnostics switches ride along here too.
    cfg.diagnosticsEnabled = diagnosticsEnabled_;
    cfg.diagnosticsMinidump = diagnosticsMinidump_;
    cfg.crashUploadRecent = cascade::core::encodePolicyRecent(crashUploadState_);
    cfg.crashUploadWindowStart = crashUploadState_.windowStart;
    cfg.crashUploadWindowCount = crashUploadState_.windowCount;
    cfg.crashUploadBlockedUntil = crashUploadState_.blockedUntil;

    cfg.telemetryEnabled = telemetryEnabled_;
    cfg.telemetryInstallId = telemetryInstallId_;
    cfg.telemetryLaunches = telemetryLaunches_;
    cfg.telemetryCrashes = telemetryCrashes_;
    cfg.telemetryPending.clear();
    if (!telemetryEnabled_ || telemetryInstallId_.empty()) {
        return;  // opted out: nothing is written, so nothing can later be sent
    }
    telemetryAccrueMode();

    cascade::core::TelemetryReport r;
    r.installId = telemetryInstallId_;
    r.appVersion = cascade::versionString();
    r.os = cascade::core::osDescription();
    r.arch = cascade::core::archDescription();
    r.launches = telemetryLaunches_;
    r.crashes = telemetryCrashes_;
    const double now = glfwGetTime();
    r.session.seconds = static_cast<std::uint64_t>(
        now > telemetrySessionStart_ ? now - telemetrySessionStart_ : 0.0);
    r.session.modeSeconds = telemetryModeSeconds_;
    r.session.panels = telemetryPanels_;
    // MODEL ONLY - sanitiseDevice strips the serial, which the raw args carry
    // twice (once alone, once inside the label).
    r.session.sdrModel = cascade::core::sanitiseDevice(soapyArgs_);
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded && r.session.plugins.size() < 20) {
            r.session.plugins.push_back(p.name + " " + p.version);
        }
    }
    cfg.telemetryPending = r.toJson();
}

cascade::core::AppConfig AppWindow::currentConfig() {
    cascade::core::AppConfig cfg;
    cfg.sourceKind = sourceKind_;
    cfg.soapyAntenna = soapyAntenna_;
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
    cfg.mapWindowWidth = mapWinW_;
    cfg.mapWindowHeight = mapWinH_;
    cfg.mapWindowX = mapWinX_;
    cfg.mapWindowY = mapWinY_;
    cfg.rxPositionSet = rxSet_;
    cfg.rxLatDeg = rxLat_;
    cfg.rxLonDeg = rxLon_;
    cfg.pluginCatalogueUrl = pluginCatalogueUrl_;
    cfg.pluginBrowserOpen = pluginBrowseOpen_;
    cfg.pluginLastUpdateCheck = pluginLastUpdateCheck_;
    cfg.pluginTuneAllowed = pluginTuneAllowed_;
    cfg.pluginsStopped = pluginsStopped_;
    cfg.pluginMuteOverride = pluginMuteOverride_;
    cfg.catEnabled = catEnabled_;
    cfg.catBindAll = catBindAll_;
    cfg.catPort = catPortMirror_;
    cfg.webEnabled = webCfg_.enabled;
    cfg.webBindAddress = webCfg_.bindAddress;
    cfg.webPort = webCfg_.port;
    cfg.webUsername = webCfg_.username;
    cfg.webPasswordRecord = webCfg_.passwordRecord;
    telemetryJournal(cfg);
    // FALSE while running, so a start-up that reads it back knows the previous
    // session never got as far as writing true. Set only on the clean exit
    // path, which is what makes an absent marker mean "crashed".
    cfg.telemetryCleanExit = telemetryCleanExit_;
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
