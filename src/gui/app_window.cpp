// GLFW + Dear ImGui application shell — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <imgui.h>
// For StartMouseMovingWindow and the window's viewport ownership: a page's
// rail has to drag the page through the same machinery a title bar uses, or a
// page could no longer be torn out of the main window and merged back into it.
#include <imgui_internal.h>
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
#include "gui/scope_face.hpp"
#include "gui/fonts.hpp"
#include "gui/theme.hpp"
#include "gui/map_view.hpp"
// WHY EVERY "nothing here" SENTENCE IN THIS FILE COMES FROM ONE PLACE. An empty
// list from PluginUi or PluginRunner is not evidence about the disk - both skip
// a refused module and a stopped one before they write anything - so the words
// a surface prints when it is empty are decided by the census in this header
// and by nothing else. No ImGui in it; it is tested without a graphics context.
#include "gui/module_census.hpp"
// The two windows that replaced the plugin store and plugin inventory rail
// sections. Included here rather than in app_window.hpp because both include
// imgui.h and that header is compiled into the tests; the members are held by
// unique_ptr behind forward declarations for exactly that reason.
#include "gui/plugin_store_view.hpp"
#include "gui/plugins_view.hpp"
#include "gui/spectrum_view.hpp"
#include "gui/track_detail_view.hpp"
#include "gui/waterfall_view.hpp"
#include "gui/win_frame.hpp"
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

// kMenuWidth now lives in app_window.hpp, beside the rail-row geometry it is
// part of: the test that asks whether a rail label still fits its plate has to
// know how wide a row is, and a column width kept private here is a column
// width nothing can check against.

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
// THE ONE RED. Defined from the theme rather than beside it: there used to be
// a second, near-identical red written inline elsewhere in this file, which is
// how a product ends up with two failure colours that are not quite the same.
const ImVec4 kErrorRed = cascade::gui::theme::bad();

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

// Panel-furniture colors. The vertical tick gridlines are fainter than the
// spectrum's 10 dB grid (alpha 18 vs 26) so the two grids stay visually
// separable; the waterfall marker reuses the spectrum overlay's warm
// center-line color. (The axis strip's own three colours went with it when
// SpectrumView took over lettering the frequency scale.)
constexpr ImU32 kTickGridColor = IM_COL32(255, 255, 255, 18);
constexpr ImU32 kWfMarkerColor = IM_COL32(255, 170, 60, 200);

// The frequency readout's 10 digit places, most significant first
// (digit i steps by kPlaceHz[i] on a wheel tick over it).
constexpr double kPlaceHz[10] = {1e9, 1e8, 1e7, 1e6, 1e5, 1e4, 1e3, 1e2, 1e1, 1e0};

// Largest value the fixed 10-digit field can show; the display clamps here
// (a device readback cannot exceed it in practice — 9.99 GHz).
constexpr double kMaxDisplayHz = 9999999999.0;

// --- THE NARROWEST THE MAIN WINDOW MAY BE ------------------------------------
//
// THIS IS WHAT MAKES THE TOP BAR'S SCALE FLOOR TRUE. The bar shrinks with the
// window and stops shrinking at kBarMinScale (see drawToolbar), which keeps
// the whole fixed cluster - transport, master lamps, counter, VOLUME DIAL -
// drawn at a legible size. What the floor cannot do on its own is keep that
// cluster INSIDE the window: below some width the bar is simply wider than the
// space it is drawn in, its child clips the overflow, and the dial - the only
// volume control in the application - is silently not there and cannot be
// operated. Refusing the width is the honest fix; scaling further down does
// not work either, because drawBrassVolumeKnob draws nothing under a 6 px
// radius and the dial would vanish just the same.
//
// The number is checked against the bar's own geometry by a static_assert
// beside those constants, so neither can drift away from the other.
// Both numbers are the CLIENT area, which is what glfwSetWindowSizeLimits
// takes - GLFW adds the frame itself.
constexpr int kMinWindowW = 560;
// A minimum height is not needed by the bar and is given anyway: GLFW's Win32
// backend applies its minimum only when BOTH dimensions are set, so a width
// limit on its own is no limit at all. This is the height at which the bar and
// the head of the function rail still sit on screen together.
constexpr int kMinWindowH = 400;
// The cabinet takes at most 24 px a side (drawCabinet clamps its margin there)
// and the body clears the well's bevel by 3 more, so the bar is handed at
// least kMinWindowW minus this much.
constexpr float kCabinetInsetMaxPx = 27.0f * 2.0f;

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
           a.mapTrails == b.mapTrails &&
           a.mapTrailAltitudeColours == b.mapTrailAltitudeColours &&
           a.mapTrailStyle == b.mapTrailStyle &&
           // The scope's mode and range. Both are user switches that change
           // only on a click or a wheel notch, so they belong here: without
           // them, leaving the application in scope mode - or on a range other
           // than the one it opened with - would survive a restart only if
           // something else happened to trigger a save.
           a.scopeMode == b.scopeMode && a.scopeRangeNm == b.scopeRangeNm &&
           // Map page geometry takes part, which is what makes a resize save
           // at all. The debounce restarts on every change, so a drag writes
           // once when it stops rather than once per frame while it is
           // happening. The legacy single-window fields ride along too: they
           // are constant after start-up, so comparing them costs nothing and
           // keeps the very first frame from reporting a phantom change.
           a.mapPages == b.mapPages &&
           a.mapWindowWidth == b.mapWindowWidth &&
           a.mapWindowHeight == b.mapWindowHeight &&
           a.mapWindowX == b.mapWindowX && a.mapWindowY == b.mapWindowY &&
           a.rxPositionSet == b.rxPositionSet && a.rxLatDeg == b.rxLatDeg &&
           a.rxLonDeg == b.rxLonDeg &&
           a.pluginCatalogueUrl == b.pluginCatalogueUrl &&
           a.pluginBrowserOpen == b.pluginBrowserOpen &&
           // The fitted modules window, open flag AND rectangle, because
           // this comparison is what decides
           // whether the file is written at all, so a field missing from it
           // persists only when something else happens to change in the same
           // session. Without the rectangle a resize would never be saved.
           a.fittedModulesOpen == b.fittedModulesOpen &&
           a.fittedModulesX == b.fittedModulesX &&
           a.fittedModulesY == b.fittedModulesY &&
           a.fittedModulesWidth == b.fittedModulesWidth &&
           a.fittedModulesHeight == b.fittedModulesHeight &&
           a.pluginLastUpdateCheck == b.pluginLastUpdateCheck &&
           a.pluginTuneAllowed == b.pluginTuneAllowed &&
           a.pluginsStopped == b.pluginsStopped &&
           a.pluginMuteOverride == b.pluginMuteOverride &&
           // Without this a close is remembered in memory and never written:
           // the comparison is what decides whether the file is saved at all,
           // so a field missing from it persists only when something else
           // happens to change in the same session.
           a.closedWindows == b.closedWindows &&
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
           // save the fields below trigger, and on the two unconditional
           // saves at exit — the pre-join one that carries the finished
           // report, and the post-join one that writes the clean-exit marker
           // only once the pipeline has actually shut down.
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
      waterfall_(std::make_unique<WaterfallView>(static_cast<int>(kFftSize), kWaterfallHistory)),
      // The plugin store's view and both windows' decks. Created here, where
      // their headers are visible, and never rebuilt: a deck holds the search
      // text, the SHOW rockers, the sort key and the selection, and every one
      // of those is a thing the user set and expects to still be set after a
      // rescan replaces the catalogue underneath it.
      pluginStoreView_(std::make_unique<PluginStoreView>()),
      pluginStoreDeck_(std::make_unique<PluginStoreDeck>()),
      fittedDeck_(std::make_unique<FittedModulesDeck>()) {
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
    // Setting it here is NOT a fetch: nothing contacts the origin until CHECK
    // NOW is pressed in the plugin store window.
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
    mainWindow_ = window;
    if (window == nullptr) {
        std::fprintf(stderr, "cascade: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    // A FLOOR ON THE WIDTH, because the top bar has one and could not keep it
    // alone: narrower than this and the volume dial is drawn outside the bar's
    // own child and clipped away, leaving no volume control at all.
    //
    // BOTH MINIMA HAVE TO BE GIVEN OR NEITHER IS APPLIED. GLFW's Win32 backend
    // fills ptMinTrackSize only when minwidth AND minheight are both set
    // (win32_window.c, WM_GETMINMAXINFO), so passing GLFW_DONT_CARE for the
    // height silently threw the width limit away too - which is exactly what
    // the first version of this line did, and it read as a working fix. The
    // height chosen is the modest one that keeps the bar and the head of the
    // rail on screen together; nothing on this face disappears below it, it
    // only gets less room to scroll in.
    glfwSetWindowSizeLimits(window, kMinWindowW, kMinWindowH, GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
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
    // THE TYPEFACES, before anything measures a string.
    //
    // Every hard-coded width in this layout was chosen against these faces, so
    // they have to be in the atlas before the first frame rather than swapped
    // in later. A failure here is survivable and deliberately not fatal: the
    // application wears ImGui's own bitmap font, which is ugly and correct,
    // and says so on the console rather than refusing to start over a font.
    if (!cascade::gui::fonts::load()) {
        std::fprintf(stderr,
                     "cascade: could not load the bundled typefaces; "
                     "falling back to the built-in font\n");
    }
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
    // TORN-OFF WINDOWS GET NO FRAME FROM THE OPERATING SYSTEM (0.78.0). From
    // 0.66.0 they had one - a native title bar with the desktop's own
    // minimise, maximise and close, added after a user could not find a way
    // to resize the map - and the user then asked for every such bar to go
    // and its three controls to be "on the metal". So a page is a cabinet
    // now: beginPage draws the brass, the screws and a rail with the page's
    // name and the three keys, the rail drags the window, and ImGui's own
    // edge zones resize it. The keys do what the desktop's did, on the
    // desktop's terms - a torn-off page minimises to the taskbar, which is
    // why it must HAVE a taskbar button: a minimised window with no button
    // is a window that has vanished, the lesson the radar unit taught.
    ImGui::GetIO().ConfigViewportsNoDecoration = true;
    ImGui::GetIO().ConfigViewportsNoTaskBarIcon = false;
    // THE BENCH, applied once, before the first frame.
    //
    // This replaces ImGui::StyleColorsDark() outright rather than tinting it:
    // the two are different instruments, and half a dark theme showing through
    // brass is worse than either on its own. Everything the style can reach -
    // every button, field, header, popup, tooltip, scrollbar and tab in the
    // application - follows from gui/theme.hpp from here on.
    //
    // The two overrides that used to live here in full are now INSIDE
    // applyTheme, with the reasoning that made them load-bearing: a torn-off
    // window is a real operating system window in this application, so its
    // background must be opaque and its corners square or the OS frame and the
    // ImGui corner disagree.
    cascade::gui::theme::applyTheme();

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

    // THE MAIN WINDOW'S OWN TITLE BAR GOES TOO, once the ImGui backend has
    // hooked the window procedure, so this hook sits above it and sees every
    // hit test first - see gui/win_frame.hpp. Where it cannot go (every
    // platform but Windows) the window keeps the frame the desktop gave it
    // and drawCabinetRail draws no keys.
    cascade::gui::frame::install(window);

    // A previous run() tore the waterfall down with its GL context (see the
    // teardown below); re-create it against the new context so run() stays
    // callable more than once.
    if (!waterfall_) {
        waterfall_ = std::make_unique<WaterfallView>(static_cast<int>(kFftSize),
                                                     kWaterfallHistory);
    }
    // No map view is created here any more: each plugin's map page creates
    // its own MapView lazily in ensureMapPage(), which also applies the
    // restored receiver position at that moment — so the ordering bug the old
    // single view had here (config applied before the view existed) cannot
    // recur by construction.

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
    // The deliberate SHUTDOWN wedge, and the only way to hold the shutdown
    // budget against the real teardown of the real binary. --diag-stall proves
    // the frame-loop half; there is no equivalent for the teardown, because
    // the teardown is not a place a test can reach — it has no frame counter,
    // no UI, and by then the window is going away. Bounded runs only, exactly
    // like the plugin hooks above: an interactive session can never be wedged
    // by a stray environment variable.
    int shutdownStallMs = 0;
    if (frames >= 0) {
        const char* hook = std::getenv("CASCADE_PLUGIN_TEST");
        if (hook != nullptr && *hook != '\0') { pluginTestHook_ = hook; }
        const char* shutdownStall = std::getenv("CASCADE_DIAG_SHUTDOWN_STALL_MS");
        if (shutdownStall != nullptr && *shutdownStall != '\0') {
            shutdownStallMs = std::atoi(shutdownStall);
            if (shutdownStallMs < 0) { shutdownStallMs = 0; }
        }
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
    if (frames < 0) {
        startUpdateCheck();
        // Heartbeats are interactive-only for the same reason - and this is
        // measured, not hypothetical: 0.65.0 armed them in telemetryStartup,
        // so every app-level ctest run minted a fresh isolated config (whose
        // default is reporting on), got a brand-new install id, and fired its
        // first-poll beat at the LIVE endpoint. One machine running one copy
        // read as four "running right now". configure() still refuses an
        // empty id, so an opted-out interactive run arms nothing here either.
        telemetryHeartbeat_.configure(cascade::core::telemetryEndpoint(),
                                      telemetryInstallId_, cascade::versionString());
    }

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

        // SCOPE MODE IS SUPPOSED TO COVER THE WHOLE APPLICATION, NOT JUST
        // THIS WINDOW. Switching what this window draws alone left a user's
        // map page and decoder output sitting on the desktop beside a scope
        // that is supposed to BE the interface while it is on.
        //
        // Applied every frame rather than once, because the torn-off windows
        // are ImGui viewports: UpdatePlatformWindows above re-shows any it
        // decides should be visible, so a single hide would be undone on the
        // next frame. Hiding here, immediately after it, is what makes the
        // decision stick without fighting the backend for ownership of the
        // window - and the geometry read-back that persists their rectangles
        // is unaffected, because a hidden window keeps its position.
        applyScopeWindowVisibility();

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

    // THE TEARDOWN GETS ITS OWN BUDGET, AND STAYS WATCHED.
    //
    // Everything below runs with no heartbeat on purpose, so a shutdown that
    // wedges is reported like any other hang — the worst freeze this product
    // ever shipped was 120 s inside a CAT client shutdown. What it cannot do
    // is run against the FRAME threshold: pipeline_.stop() alone may
    // deliberately spend 1.5 s on the driver lock, 1.5 s on the abandonable
    // vendor call inside it, and 3 s on the source thread — 6 s against the
    // 5 s the frame loop is judged by, and the newest field report is the
    // watchdog filing a hang against its own clean shutdown 478 ms after the
    // second of those guards returned.
    //
    // DO NOT MAINTAIN THAT SUM BY HAND. It was 4.5 s across two waits until
    // the vendor call was bounded, and the test written to catch exactly this
    // drift was itself blinded by it, because it scanned for two constants by
    // name. tests/test_shutdown_budget.cpp now DISCOVERS every
    // constexpr chrono wait under src/ and fails on one it has never been
    // told the cost of, so the next wait added here cannot pass quietly.
    //
    // beginShutdown() restarts the stall clock — the teardown is measured
    // from here, not from whatever was left of the last frame — and raises
    // the threshold to one sized against those bounded waits. It pauses
    // nothing and disarms nothing: a shutdown that genuinely wedges still
    // crosses the budget and is still captured with every thread's stack.
    watchdog_.beginShutdown();
    const auto teardownStart = std::chrono::steady_clock::now();

    // Closing the window mid-take finalizes both recordings cleanly (same
    // contract as the toolbar Stop): taps out, then headers patched — before
    // the pipeline teardown below ends the sample flow they were taping.
    stopIqRecording();
    stopAudioRecording();

    // The enforcement diagnostic, printed at the END so it reports the state
    // the run actually finished in — including any retirement that a catalogue
    // fetch during the run brought into force.
    if (pluginStatusHook_) { reportPluginStatus(); }

    // Final-state save, unconditional: cheap, and it guarantees the on-disk
    // config matches the final session state even when the debounce never
    // fired (e.g. a change made less than 2 s before closing the window).
    // Runs before pipeline_.stop() so the snapshot reads live state.
    //
    // telemetryCleanExit_ is deliberately still FALSE here. The clean-exit
    // marker is written by a second save AFTER pipeline_.stop() below —
    // because a death during the pipeline join is a death. Setting it before
    // the join, as this path did until 0.66.0, made the field's worst shipped
    // freeze class (a shutdown that wedges in that join) count as a clean
    // exit and offer no report.
    // BEFORE the save, so the sweep's dedup and rate-limit memory reaches the
    // file - and before anything else on this path, so a transfer in flight is
    // cancelled rather than waited out. This is the line that makes "a hanging
    // endpoint does not delay shutdown" true; it is measured against the real
    // binary in tests/test_crash_upload.cpp.
    crashUploadFinish();
    if (!configPath_.empty()) { saveConfigNow(); }
    cascade::core::diagLogf("frame loop ended after %d frames; shutting down", rendered);

    // The deliberate shutdown wedge, in the place the real one lives: the
    // stretch around pipeline_.stop(), where the bounded driver waits are
    // spent and where the 120 s CAT freeze happened. Bounded runs only (see
    // where shutdownStallMs is read). Printed, so a test can prove the hook
    // ran rather than inferring it from an absent report.
    if (shutdownStallMs > 0) {
        std::printf("cascade: --diag-shutdown-stall wedging the teardown for %d ms\n",
                    shutdownStallMs);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(shutdownStallMs));
    }

    // Closing the window while receiving must not leave DSP threads pacing a
    // dead display; stop before teardown so the join happens while the object
    // graph is still fully alive.
    pipeline_.stop();

    // THE CLEAN-EXIT MARKER, after the pipeline join. The join above — DSP
    // threads, the CAT server, the USB device stack — is where the worst
    // shipped shutdown freeze lived, so the marker must not be on disk before
    // it completes; a death in there now leaves telemetryCleanExit=false and
    // is counted, where until 0.66.0 it read as a clean exit.
    //
    // It rewrites THE SNAPSHOT THE SAVE ABOVE JUST WROTE with one field
    // changed, rather than calling saveConfigNow() again, and that shape is
    // the point. A second currentConfig() would re-derive every value at a
    // moment when the session is half torn down: it rebuilds the pending
    // usage report through telemetryJournal, whose clock is glfwGetTime()
    // (0.0 once GLFW is terminated — every session would then report zero
    // seconds), and it re-reads live source state through a pipeline that has
    // just been stopped. Neither can happen to a snapshot taken while
    // everything was still alive. Residual, stated plainly: a death in the
    // GL/GLFW teardown below still counts as a clean exit — the watchdog,
    // stopped last as ever, is what covers that stretch.
    telemetryCleanExit_ = true;
    if (!configPath_.empty()) {
        cascade::core::AppConfig marked = savedCfg_;
        marked.telemetryCleanExit = true;
        std::string err;
        if (cascade::core::ConfigStore::save(configPath_, marked, err)) {
            savedCfg_ = marked;
        } else {
            std::fprintf(stderr, "cascade: %s\n", err.c_str());
        }
    }

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
    // the teardown would make that exact bug invisible again. Everything above
    // is judged against the shutdown budget beginShutdown() set, so a report
    // from here means the close really did wedge - not that a bounded driver
    // wait spent what it is allowed to spend.
    watchdog_.stop();

    // HOW LONG THE TEARDOWN ACTUALLY TOOK, printed rather than assumed - the
    // same discipline as the worst frame gap above, and for the same reason.
    // The budget it is judged against was derived from constants in two other
    // files; this is the measurement that says what the real path costs, and
    // tests/test_shutdown_budget.cpp reads it back and requires a healthy
    // close to sit far under the budget. A teardown that grows into it goes
    // red here instead of arriving as a false hang report on a user's
    // machine, which is exactly how this defect reached the field.
    std::printf("cascade: teardown %.1f ms\n",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - teardownStart)
                    .count());

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

    // The module table only has to be rebuilt when something LOADED CODE.
    // Doing it every frame would mean walking the loader's module list 60
    // times a second for a table that changes a handful of times a session.
    // This covers the plugins; the OTHER thing that maps code into this
    // process after start-up is a device open, and that has its own refresh
    // where the open completes (pollSoapyAsync) - a device open changes no
    // plugin count, so this test cannot see it.
    if (loaded != diagPluginCount_) {
        diagPluginCount_ = loaded;
        cascade::core::refreshModuleTable();
    }
}

namespace {

// --- THE CABINET -------------------------------------------------------------
//
// THE WHOLE APPLICATION SITS IN A BOX, and until this existed it did not. The
// panels were drawn straight onto a flat ground that simply stopped at the
// window edge, which is why the face read as a screenshot of controls rather
// than as a machine: a real instrument has an EDGE, and the edge is what tells
// an eye where the machine ends and the desk begins.
//
// So: a rounded brass face with one hairline of light along the top and left
// and one of shadow along the bottom and right, a countersunk screw at each
// corner, and the panels sunk into a well cut in the middle of it. The screws
// are each turned to their own angle, because four identical screws read as
// printed wallpaper rather than as fasteners.
//
// NOTHING HERE IS MEASURED IN THE ARTBOARD'S PIXELS. The reference is a
// 1720 x 986 image whose margin is 22 pixels; that margin is carried here as a
// fraction of the window's smaller side, so the same cabinet is drawn at any
// size. Returns the margin it used, so the caller insets its content by a
// measurement rather than by a guess - the one number the two have to agree on
// is handed over instead of being written down twice.
//
// `minMargin` is the least the margin may be. Since 0.78.0 the margin is also
// the rail the window's name and its three keys sit on - the title bar's
// stand-in - and a small page (a 720 x 520 decoder output) came out with an
// eleven-pixel margin, which is no room for a key at all: a window with no
// close. The main window and every page ask for 22, the size of a legible
// key; a cabinet drawn as decoration inside a panel keeps the old ten.
// The least a rail may be when it carries the keys: room for one a finger can
// find. The main window and every page pass this.
constexpr float kRailMinMargin = 22.0f;

float drawCabinet(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, float minMargin = 10.0f) {
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    if (dl == nullptr || w < 80.0f || h < 80.0f) { return 0.0f; }
    const float m = std::clamp(std::min(w, h) * 0.022f, std::max(10.0f, minMargin), 24.0f);
    const float round = std::max(4.0f, m * 0.45f);

    // The brass, lit from above like every other surface on this face.
    // AddRectFilledMultiColor cannot round its corners, so the shape is laid
    // down flat first and the gradient inset by the radius - the same trick
    // addBenchPlate uses, and at this contrast the corners are
    // indistinguishable from the ramp continuing through them.
    dl->AddRectFilled(tl, br, cascade::gui::theme::kBrassShade, round);
    if (w > round * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(tl.x + round, tl.y), ImVec2(br.x - round, br.y),
                                    cascade::gui::theme::kBrassShade,
                                    cascade::gui::theme::kBrassShade,
                                    cascade::gui::theme::kBrassMid,
                                    cascade::gui::theme::kBrassMid);
    }
    cascade::gui::addBenchBevel(dl, tl, br, round, true);

    // The well the panels sit in: the same hairline pair run the other way
    // round, which is the entire difference between metal proud of the face
    // and metal cut into it.
    const ImVec2 iTL(tl.x + m, tl.y + m);
    const ImVec2 iBR(br.x - m, br.y - m);
    if (iBR.x > iTL.x + 8.0f && iBR.y > iTL.y + 8.0f) {
        dl->AddRectFilled(iTL, iBR, cascade::gui::theme::kEnamelDark,
                          cascade::gui::theme::kPanelRounding);
        cascade::gui::addBenchBevel(dl, iTL, iBR, cascade::gui::theme::kPanelRounding,
                                    false);
    }

    // FOUR SCREWS, FOUR ANGLES, sitting in the margin rather than on the well's
    // lip: the countersink is 1.3 radii across, so a head at 0.52 of the margin
    // with a radius of 0.30 clears both the outer bevel and the inner one.
    const float sr = std::max(3.5f, m * 0.30f);
    const float inset = m * 0.52f;
    const float slot[4] = {24.0f, -38.0f, 68.0f, 7.0f};
    const ImVec2 at[4] = {
        ImVec2(tl.x + inset, tl.y + inset),
        ImVec2(br.x - inset, tl.y + inset),
        ImVec2(tl.x + inset, br.y - inset),
        ImVec2(br.x - inset, br.y - inset),
    };
    for (int i = 0; i < 4; ++i) {
        cascade::gui::addCabinetScrew(dl, at[i], sr, slot[i]);
    }
    return m;
}

// --- A LETTERED BRASS KEY ----------------------------------------------------
//
// scope_face.hpp's drawBenchKey is the small SQUARE key on a rail row and
// carries no word; a key with a word on it is drawn by every hand-drawn panel
// in this application from its own local copy (map_view.cpp, plugins_view.cpp
// and plugin_store_view.cpp each have one). This is the copy the WINDOW OWNER
// needs: the satellites window's two view controls and its follow key live
// here, in the code that owns the MapView, and not inside the panel that draws
// the rest of that window.
//
// A REAL ImGui ITEM, like every other control on this bench: an
// InvisibleButton takes the hover, the focus and the keyboard, so the key is
// operable without a mouse and takes part in the same input arbitration as the
// widgets around it. `enabled` false draws a dead key that keeps its word -
// a control that vanishes when it cannot be used is a control the user cannot
// learn.
bool benchWordKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                  bool enabled, const char* id) {
    if (dl == nullptr || br.x - tl.x < 12.0f || br.y - tl.y < 8.0f) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##wordkey", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    ImGui::EndDisabled();
    ImGui::PopID();

    const float r = cascade::gui::theme::kKeyRounding;
    if (!enabled) {
        dl->AddRectFilled(tl, br, cascade::gui::theme::kWell, r);
        dl->AddRect(tl, br,
                    cascade::gui::theme::withAlpha(cascade::gui::theme::kBrassDark, 0.80f),
                    r, 0, cascade::gui::theme::kHairline);
    } else {
        // Proud metal casts a shadow and pressed metal does not, which is the
        // state indication before any colour is used.
        if (!held) {
            dl->AddRectFilled(
                ImVec2(tl.x + 1.0f, tl.y + 2.0f), ImVec2(br.x + 1.0f, br.y + 2.0f),
                cascade::gui::theme::withAlpha(cascade::gui::theme::kVoid, 0.45f), r);
        }
        const ImU32 top = held      ? cascade::gui::theme::kBrassMid
                          : hovered ? cascade::gui::theme::kIvory
                                    : cascade::gui::theme::kCream;
        const ImU32 bot =
            held ? cascade::gui::theme::kBrassDark : cascade::gui::theme::kBrassBright;
        dl->AddRectFilled(tl, br, bot, r);
        if (br.x - tl.x > r * 2.0f) {
            dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y), top,
                                        top, bot, bot);
        }
        cascade::gui::addBenchBevel(dl, tl, br, r, !held);
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    cascade::gui::theme::kBrassBright, r + 1.0f, 0,
                    cascade::gui::theme::kHairline);
    }
    // The word is CUT INTO the brass - ink on metal, which is this palette's
    // treatment for anything a hand operates. Never amber: amber is a reading.
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0.0f, label);
    dl->AddText(f, px,
                ImVec2((tl.x + br.x) * 0.5f - ts.x * 0.5f,
                       (tl.y + br.y) * 0.5f - ts.y * 0.5f + (held ? 1.0f : 0.0f)),
                enabled ? cascade::gui::theme::kEnamel : cascade::gui::theme::kInkFaint,
                label);
    return pressed;
}

// --- THE WORD ON A RAIL ROW'S PLATE, CLIPPED TO THE ROOM IT ACTUALLY HAS ------
//
// Both rail rows draw the same label the same way, so they draw it through one
// function: a second copy is how the switch row and the section row come to
// clip at two different places.
//
// IT USED TO BE UNCLIPPED, and that was safe only for as long as every label
// happened to be short. It is not a property of the map rows, whose name is a
// PLUGIN's own display name and therefore arbitrary third-party text; it was
// not a property anything in the build checked; and it stopped being comfortably
// true when fonts.hpp raised the type by two points. An overrunning word runs
// under the state chip, then past the end of the plate, and Dear ImGui does not
// wrap it - it clips it at the window, so what the user sees is a name with its
// tail missing and nothing to say that is a fault rather than a style.
//
// The limit comes from railLabelRight() and the chip's OWN MEASURED WIDTH, so
// the label and the chip are laid out from one piece of arithmetic instead of
// from two hopes. `chipText` null is a row with no chip, which keeps back only
// the plate's own padding.
void railPlateLabel(ImDrawList* dl, const ImVec2& rowTL, const ImVec2& rowBR,
                    float plateLeft, float labelPx, const char* shown,
                    const char* chipText, ImU32 ink) {
    if (dl == nullptr || shown == nullptr || shown[0] == '\0') { return; }
    ImFont* f = cascade::gui::fonts::ui();
    float chipW = -1.0f;
    if (chipText != nullptr && chipText[0] != '\0') {
        chipW = cascade::gui::fonts::legend()
                    ->CalcTextSizeA(cascade::gui::fonts::kTinySize, FLT_MAX, 0.0f,
                                    chipText)
                    .x;
    }
    const float right =
        cascade::gui::railLabelRight(rowBR.x, rowBR.y - rowTL.y, chipW);
    const ImVec2 ts = f->CalcTextSizeA(labelPx, FLT_MAX, 0.0f, shown);
    const ImVec2 at(plateLeft + cascade::gui::kRailLabelPadX,
                    (rowTL.y + rowBR.y) * 0.5f - ts.y * 0.5f);
    if (right <= at.x + 1.0f) { return; }
    // PER-GLYPH CLIPPING, not a wrap width: a wrap would push the tail of a
    // long name onto a second line the row has no height for, which is a worse
    // fault than the one being fixed. The dark pass under the word is the cut
    // the letters sit in, not a drop shadow, so it is clipped with them.
    const ImVec4 clip(at.x, rowTL.y, right, rowBR.y);
    dl->AddText(f, labelPx, ImVec2(at.x + 1.0f, at.y + 1.0f),
                cascade::gui::theme::withAlpha(cascade::gui::theme::kVoid, 0.55f), shown,
                nullptr, 0.0f, &clip);
    dl->AddText(f, labelPx, at, ink, shown, nullptr, 0.0f, &clip);
}

// --- THE DRAWERS' MOTION -----------------------------------------------------
//
// A section that pops from closed to open in one frame reads as the screen
// glitching; a real drawer unfolds. So a section's content is drawn inside a
// child whose height runs from nothing to the content's own height over
// kRailDrawerSeconds (gui/rail_banks.hpp), and back again when it closes -
// and while it is fully open it is drawn exactly as it always was, inline,
// with no child at all, so an open section costs nothing it did not before.
//
// THE HEIGHT COMES FROM A MEASUREMENT, NOT A GUESS: while a section is open
// and inline, the distance from its header to the next row is recorded, and
// that is the height the drawer runs to next time. A section that has never
// been measured (the first opening after a restart, of one that started
// closed) simply opens at once - a jump on one frame beats an unfold to a
// height invented for it.
//
// WHY THERE IS A FLUSH. benchSection returns before the content is drawn and
// has no way to run code after it, so the drawer child begun there is ended by
// the NEXT rail primitive (a row, a switch, a group caption) and by
// benchRailFlush() at the end of the column. That is the same linear order the
// rail has always been drawn in; nothing here can nest, and the GUI thread is
// the only thread that draws.
struct RailDrawer {
    float progress = 0.0f;  // 0 folded .. 1 unfolded
    float naturalH = 0.0f;  // the content's own height, once measured
    bool measured = false;
    bool seen = false;
};

std::unordered_map<ImGuiID, RailDrawer>& railDrawers() {
    static std::unordered_map<ImGuiID, RailDrawer> drawers;
    return drawers;
}

struct RailPending {
    bool active = false;
    bool inChild = false;  // a drawer child is open and must be ended
    ImGuiID id = 0;
    float startY = 0.0f;   // where the inline content began, for measuring
};
RailPending g_railPending;

void benchRailFlush() {
    if (!g_railPending.active) { return; }
    if (g_railPending.inChild) {
        ImGui::EndChild();
        ImGui::PopID();
    } else {
        RailDrawer& d = railDrawers()[g_railPending.id];
        const float h = ImGui::GetCursorPosY() - g_railPending.startY;
        if (h > 0.0f) {
            d.naturalH = h;
            d.measured = true;
        }
    }
    g_railPending = RailPending{};
}

// --- ONE ROW OF THE FUNCTION RAIL --------------------------------------------
//
// SAME CONTRACT AS ImGui::CollapsingHeader: it draws the row and returns
// whether the section is open. Every section in the left column calls this
// instead, so the rail is one object rather than nineteen bespoke rows - and a
// section added next year gets the bench by writing the same call the others
// write.
//
// IT IS A CollapsingHeader UNDERNEATH, and that is the whole design. The widget
// keeps the open/closed state, the ImGui id, the click target, the Tab stop and
// the space bar; all this adds is what the row looks like. Reimplementing the
// header to gain a look would have traded working behaviour for appearance,
// including the "Label###id" ids the plugin sections key their state on.
//
// THE KEY IS gui::drawBenchKey, THE SHARED PRIMITIVE, and it used to be a
// hand-rolled copy of it sitting right here - thirty lines drawing the same
// shadow, face, inner shadow and bevel, with a comment explaining that the
// real one could not be used because it is an ImGui item and would fight the
// header for the click. Two implementations of one part is how the rail and
// the scope face drift apart, so the copy is gone and the objection is
// answered instead:
//
//   - the header is submitted with ImGuiTreeNodeFlags_AllowOverlap, so the
//     key laid over it afterwards is allowed to take the hover and the click
//     rather than being refused by the header underneath;
//   - the key is submitted under ImGuiItemFlags_NoNav, so it adds no second
//     focus stop to a rail of sixteen rows - the header keeps the keyboard;
//   - pressing the key toggles the same section pressing the row toggles, so
//     it punches no hole in the header's click target. The toggle lands on the
//     next frame (the header for this frame has already been submitted), which
//     at any frame rate this application runs at is not visible.
//
// THE CHIP AND THE LAMP ARE ARGUMENTS RATHER THAN A SEPARATE CALL AFTER IT.
// They used to be railChip(), which decorated "the last item ImGui submitted"
// - true only while nothing else was submitted in between, which stopped being
// true the moment the key became a real item. Drawing them here also means the
// rail cannot grow a row that reports nothing by accident: a section with
// genuinely no state passes no chip, deliberately and visibly.
//
// `chipText` == nullptr draws neither chip nor lamp.
bool benchSection(const char* label, bool defaultOpen, const char* chipText = nullptr,
                  ImU32 lampColour = cascade::gui::theme::kPhosphor,
                  bool lampLit = false) {
    // The previous section's drawer ends where the next row begins.
    benchRailFlush();
    // The visible name stops at the id suffix: "Plugins###plugins" is a widget
    // called plugins that shows the word Plugins, and the rail must letter the
    // word rather than the plumbing.
    char shown[96];
    std::size_t n = 0;
    for (const char* p = label; *p != '\0' && n + 1 < sizeof(shown); ++p) {
        if (p[0] == '#' && p[1] == '#') { break; }
        shown[n++] = *p;
    }
    shown[n] = '\0';

    // A row is a fixed deck like the top bar, not a line of text with padding
    // round it: the reference's rows are all one height whatever is written on
    // them. The height is NO LONGER THE LITERAL 28 IT WAS MEASURED AT - see
    // railRowHeight in app_window.hpp. At the sizes fonts.hpp is set to now it
    // still works out to exactly 28; raised again, the row grows rather than
    // the label being squeezed into a deck that was measured for smaller type.
    const float labelPx = cascade::gui::fonts::kUiSize;
    const float kRowH = cascade::gui::railRowHeight(labelPx);

    // THE KEY'S PRESS, CARRIED ONE FRAME. The key is submitted after the
    // header, so a press cannot change the state the header has already
    // reported; it is remembered here and applied to the same id on the next
    // frame. One pair rather than a map because a mouse press lands on exactly
    // one key, and the GUI thread is the only thread that draws.
    static ImGuiID pendingId = 0;
    static bool pendingOpen = false;
    const ImGuiID rowId = ImGui::GetID(label);
    if (pendingId == rowId) {
        ImGui::SetNextItemOpen(pendingOpen);
        pendingId = 0;
    }

    // SUBMITTED INVISIBLY, THEN PAINTED. Header, HeaderHovered and HeaderActive
    // carry the widget's entire background and Text carries both its label and
    // its arrow, so four transparent colours leave an item that behaves exactly
    // as it did and draws nothing at all.
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x,
                               std::max(2.0f, (kRowH - labelPx) * 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_Header, clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, clear);
    ImGui::PushStyleColor(ImGuiCol_Text, clear);
    ImGui::PushFont(cascade::gui::fonts::ui(), labelPx);
    const bool open = ImGui::CollapsingHeader(
        label, ImGuiTreeNodeFlags_AllowOverlap |
                   (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None));
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    const ImVec2 tl = ImGui::GetItemRectMin();
    const ImVec2 br = ImGui::GetItemRectMax();
    const float h = br.y - tl.y;
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (dl == nullptr || h < 6.0f) { return open; }

    const float keySize = cascade::gui::railKeySize(h);
    const ImVec2 kTL(tl.x + cascade::gui::kRailKeyInset, tl.y + (h - keySize) * 0.5f);
    const ImVec2 kBR(kTL.x + keySize, kTL.y + keySize);

    // The label plate: a shade lighter than the ground it is screwed to, which
    // is what makes the rail read as a column of plates rather than as text on
    // a panel. It runs out to the header's right edge on purpose - railChip()
    // lands its chip and lamp on that end afterwards, and they have to sit ON
    // the plate rather than beside it.
    const ImVec2 pTL(kBR.x + cascade::gui::kRailKeyGap, tl.y + 1.0f);
    const ImVec2 pBR(br.x, br.y - 1.0f);
    if (pBR.x > pTL.x + 24.0f) {
        ImU32 plate = cascade::gui::theme::kBrassDark;
        if (held) {
            plate = cascade::gui::theme::kBrassShade;
        } else if (hovered) {
            plate = cascade::gui::theme::kBrassMid;
        }
        dl->AddRectFilled(pTL, pBR, plate, cascade::gui::theme::kKeyRounding);
        cascade::gui::addBenchBevel(dl, pTL, pBR, cascade::gui::theme::kKeyRounding, true);

        // IVORY ON METAL, which is the palette's rule for anything a hand
        // operates - never amber, which on this panel means a reading. Where
        // the word has to STOP is railPlateLabel's business: it is measured
        // against the chip that is about to be landed on the same plate.
        railPlateLabel(dl, tl, br, pTL.x, labelPx, shown, chipText,
                       open ? cascade::gui::theme::kIvory : cascade::gui::theme::kCream);
    }

    // THE STATE OF THE SECTION, READ WITHOUT OPENING IT: a chip naming what it
    // is doing and a lamp saying whether it is doing it, landed on the end of
    // the plate. The reference's rail carries both on every row; without them
    // the rail is a column of names.
    if (chipText != nullptr) {
        cascade::gui::drawRailChip(dl, tl, br, chipText, lampColour, lampLit);
    }

    // THE KEY, and it is the shared primitive. `open || held` presses it in
    // while the section is open OR while the row is being clicked anywhere
    // along its length, so it still reads as the row's own disclosure
    // indicator rather than as a separate control that happens to sit on it.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool keyPressed = cascade::gui::drawBenchKey(dl, kTL, keySize, open || held);
    ImGui::PopItemFlag();
    if (keyPressed) {
        pendingId = rowId;
        pendingOpen = !open;
    }
    // AND THE ROW IS PUT BACK THE SIZE IT WAS. The key's InvisibleButton is a
    // small item inset inside the row, so it leaves the cursor above where the
    // header left it and tells the parent the content only reaches as far as
    // the key - which on the last row of the rail is a scroll extent short by
    // most of a line, and which ImGui's own error recovery reports as
    // "SetCursorPos used to extend window boundaries". Re-declaring the row's
    // own rectangle restores both the cursor and the extent exactly, and a
    // Dummy carries no id, so nothing about the header's interaction changes.
    ImGui::SetCursorScreenPos(tl);
    ImGui::Dummy(ImVec2(br.x - tl.x, br.y - tl.y));

    // Keyboard focus has to be VISIBLE or reachability is a claim nobody can
    // act on - and the header's own highlight was one of the four colours
    // painted out above.
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 1.0f, tl.y - 1.0f), ImVec2(br.x + 1.0f, br.y + 1.0f),
                    cascade::gui::theme::kBrassBright,
                    cascade::gui::theme::kKeyRounding + 1.0f, 0,
                    cascade::gui::theme::kHairline);
    }

    // THE DRAWER - see "THE DRAWERS' MOTION" above. `open` is the target; the
    // section is drawn while any of it is showing, which includes the frames
    // it spends folding shut.
    RailDrawer& d = railDrawers()[rowId];
    if (!d.seen) {
        d.seen = true;
        d.progress = open ? 1.0f : 0.0f;  // no motion on the first frame ever
    }
    if (open && !d.measured && d.progress < 1.0f) { d.progress = 1.0f; }
    d.progress = cascade::gui::railDrawerAdvance(d.progress, open, ImGui::GetIO().DeltaTime);
    const bool moving = d.progress > 0.0f && d.progress < 1.0f;
    if (moving) {
        const float drawerH = std::max(1.0f, d.naturalH * cascade::gui::railEase(d.progress));
        ImGui::PushID(static_cast<int>(rowId));
        // Transparent, like the sections child itself: the plate's ground
        // carries the drawer, and a box around a half-open section would be a
        // box the reference does not have.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("##drawer", ImVec2(ImGui::GetContentRegionAvail().x, drawerH),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        g_railPending = RailPending{true, true, rowId, 0.0f};
        return true;
    }
    if (open) { g_railPending = RailPending{true, false, rowId, ImGui::GetCursorPosY()}; }
    return open;
}

// --- ONE SWITCH ON THE FUNCTION RAIL -----------------------------------------
//
// THE SAME ROW, WIRED TO A FLAG INSTEAD OF TO A DRAWER. benchSection above is
// a CollapsingHeader wearing the bench; this is the identical plate, key, chip
// and lamp driving a boolean the CALLER owns. The satellites map needs exactly
// that and nothing more: its rail row is a switch that puts the window on
// screen, because every satellite control lives inside that window and a
// second set of them out here would be the scattering the window exists to
// end.
//
// Returns true on the frame it is pressed and toggles nothing itself, so the
// row can operate a flag that lives somewhere else - a map page's `open`, in
// the one caller there is.
//
// THE KEY AND THE ROW ARE TWO ITEMS AND ONLY ONE OF THEM CAN BE PRESSED. The
// row is submitted first under SetNextItemAllowOverlap, so the key laid over
// it afterwards takes the hover and the click when the pointer is on the key,
// and the row takes it everywhere else - the same arbitration benchSection
// uses, and the reason this needs no deferred-press bookkeeping: whichever
// item reports the press, the answer is one press.
//
// `enabled` false is a BLOCKED control, not a hidden one. It keeps its plate,
// its chip and its lamp and refuses the click; the sentence saying what would
// unblock it is the caller's, drawn under the row, because only the caller
// knows which of several reasons applies.
bool benchSwitchRow(const char* label, bool on, const char* chipText,
                    ImU32 lampColour, bool lampLit, bool enabled,
                    const char* tooltip) {
    benchRailFlush();
    // Same rule as benchSection: the visible name stops at the id suffix, so
    // "Satellites map###satmap:X" letters the words and not the plumbing.
    char shown[96];
    std::size_t n = 0;
    for (const char* p = label; *p != '\0' && n + 1 < sizeof(shown); ++p) {
        if (p[0] == '#' && p[1] == '#') { break; }
        shown[n++] = *p;
    }
    shown[n] = '\0';

    // The same deck height benchSection's header works out to, from the same
    // function, so a switch and a section can never sit at two heights on one
    // rail. See railRowHeight in app_window.hpp for why it is no longer 28.
    const float labelPx = cascade::gui::fonts::kUiSize;
    const float kRowH = cascade::gui::railRowHeight(labelPx);
    const float w = ImGui::GetContentRegionAvail().x;
    if (w < 40.0f) { return false; }

    ImGui::PushID(label);
    const ImVec2 tl = ImGui::GetCursorScreenPos();
    const ImVec2 br(tl.x + w, tl.y + kRowH);

    ImGui::BeginDisabled(!enabled);
    ImGui::SetNextItemAllowOverlap();
    const bool rowPressed = ImGui::InvisibleButton("##switchrow", ImVec2(w, kRowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    if (tooltip != nullptr &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tooltip);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (dl == nullptr) {
        ImGui::EndDisabled();
        ImGui::PopID();
        return false;
    }

    const float keySize = cascade::gui::railKeySize(kRowH);
    const ImVec2 kTL(tl.x + cascade::gui::kRailKeyInset, tl.y + (kRowH - keySize) * 0.5f);
    const ImVec2 kBR(kTL.x + keySize, kTL.y + keySize);

    const ImVec2 pTL(kBR.x + cascade::gui::kRailKeyGap, tl.y + 1.0f);
    const ImVec2 pBR(br.x, br.y - 1.0f);
    if (pBR.x > pTL.x + 24.0f) {
        // A BLOCKED ROW IS DARK METAL, not greyed lettering on live brass: the
        // plate itself has to say the control is not available, because a
        // colour difference in the word alone is exactly what a user reads as
        // "this one is just less important".
        ImU32 plate = cascade::gui::theme::kEnamel;
        if (enabled) {
            plate = cascade::gui::theme::kBrassDark;
            if (held) {
                plate = cascade::gui::theme::kBrassShade;
            } else if (hovered) {
                plate = cascade::gui::theme::kBrassMid;
            }
        }
        dl->AddRectFilled(pTL, pBR, plate, cascade::gui::theme::kKeyRounding);
        cascade::gui::addBenchBevel(dl, pTL, pBR, cascade::gui::theme::kKeyRounding,
                                    enabled);

        // Ivory on metal, the palette's rule for anything a hand operates -
        // never amber, which on this panel means a reading. THE CLIP MATTERS
        // MOST HERE: a map row's label is "<plugin display name> map", and a
        // plugin names itself. Nothing bounds that string, so nothing but
        // railPlateLabel's measured limit keeps it off the chip.
        railPlateLabel(dl, tl, br, pTL.x, labelPx, shown, chipText,
                       !enabled ? cascade::gui::theme::kInkFaint
                                : (on ? cascade::gui::theme::kIvory
                                      : cascade::gui::theme::kCream));
    }

    if (chipText != nullptr) {
        cascade::gui::drawRailChip(dl, tl, br, chipText, lampColour, lampLit);
    }

    // NoNav for the same reason benchSection gives: the row already owns the
    // keyboard stop, and a second one per switch would double the length of
    // the rail's tab order to reach the same control twice.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool keyPressed = cascade::gui::drawBenchKey(dl, kTL, keySize, on || held);
    ImGui::PopItemFlag();
    ImGui::EndDisabled();

    // The key's small item left the cursor above where the row ended and told
    // the parent the content reaches only as far as the key - which on the
    // last row of a rail is a scroll extent short by most of a line, and which
    // ImGui reports in its error hook. Re-declaring the row's own rectangle
    // restores both; a Dummy carries no id, so no interaction changes.
    ImGui::SetCursorScreenPos(tl);
    ImGui::Dummy(ImVec2(w, kRowH));

    if (focused) {
        dl->AddRect(ImVec2(tl.x - 1.0f, tl.y - 1.0f), ImVec2(br.x + 1.0f, br.y + 1.0f),
                    cascade::gui::theme::kBrassBright,
                    cascade::gui::theme::kKeyRounding + 1.0f, 0,
                    cascade::gui::theme::kHairline);
    }
    ImGui::PopID();
    return enabled && (rowPressed || keyPressed);
}

// An engraved group caption across the rail - SIGNAL PATH, DECODE and the rest.
// Reserves its own row so the sections below it flow normally; without the
// Dummy the caption would be painted into space the next row then draws over.
void benchGroup(const char* caption) {
    benchRailFlush();
    ImGui::Spacing();
    const float px = cascade::gui::fonts::kTinySize;
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    cascade::gui::addBenchGroupCaption(ImGui::GetWindowDrawList(),
                                       ImVec2(at.x + 3.0f, at.y), w - 6.0f, caption);
    ImGui::Dummy(ImVec2(w, px + 4.0f));
}

}  // namespace

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

    // THE CABINET, FIRST AND UNDERNEATH. Painted into the root window's own
    // draw list before a single widget is submitted, which is what puts it
    // under everything: ImGui renders a parent's list ahead of its children's,
    // so the brass, the bevel, the four screws and the sunk well are laid down
    // and every panel below is drawn into them.
    //
    // THEN THE CONTENT IS INSET BY THE MARGIN THE CABINET REPORTS. A panel
    // painted over the bevel would rub out the one hairline that makes the edge
    // an edge, so the body lives in a child sized from drawCabinet's own answer
    // rather than from a number typed twice. Three pixels more than the margin:
    // the well's bevel is drawn ON the margin's inner boundary and the content
    // must clear it.
    const ImVec2 rootTL = ImGui::GetWindowPos();
    const ImVec2 rootSize = ImGui::GetWindowSize();
    const float cabinetM =
        drawCabinet(ImGui::GetWindowDrawList(), rootTL,
                    ImVec2(rootTL.x + rootSize.x, rootTL.y + rootSize.y), kRailMinMargin);
    // THE RAIL STANDS IN FOR THE TITLE BAR: the name engraved at its left, the
    // three keys at its right, and the rest of it the handle the window is
    // dragged by (0.78.0 - "put the minimise, maximise and close on the
    // metal").
    drawCabinetRail(rootTL.x, rootTL.y, rootTL.x + rootSize.x, rootTL.y + rootSize.y, cabinetM);
    // THE INPUT LEDGER, on request. FOXSDR_DEBUG_INPUT=1 prints, on the
    // bench, every number the pointer's position passes through on its way
    // from the operating system to a widget: where the OS says the cursor
    // is, where this window's client origin is, what GLFW reports for the
    // window and the framebuffer, and what ImGui believes. When a click
    // lands beside the control it was aimed at, the disagreeing pair is on
    // screen in one shot instead of being reasoned about.
    if (const char* dbg = std::getenv("FOXSDR_DEBUG_INPUT"); dbg != nullptr && dbg[0] != '\0') {
        const ImGuiIO& dio = ImGui::GetIO();
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        int wx = 0, wy = 0, ww = 0, wh = 0, fw = 0, fh = 0;
        double cx = 0.0, cy = 0.0;
        if (mainWindow_ != nullptr) {
            glfwGetWindowPos(mainWindow_, &wx, &wy);
            glfwGetWindowSize(mainWindow_, &ww, &wh);
            glfwGetFramebufferSize(mainWindow_, &fw, &fh);
            glfwGetCursorPos(mainWindow_, &cx, &cy);
        }
        char line[512];
        int n = std::snprintf(line, sizeof(line),
                              "imgui mouse %.0f,%.0f | viewport pos %.0f,%.0f size %.0fx%.0f | "
                              "display %.0fx%.0f | glfw win %d,%d %dx%d fb %dx%d cursor %.0f,%.0f",
                              dio.MousePos.x, dio.MousePos.y, mv->Pos.x, mv->Pos.y, mv->Size.x,
                              mv->Size.y, dio.DisplaySize.x, dio.DisplaySize.y, wx, wy, ww, wh, fw,
                              fh, cx, cy);
#ifdef _WIN32
        if (n > 0 && n < static_cast<int>(sizeof(line))) {
            POINT os{0, 0};
            GetCursorPos(&os);
            HWND hw = static_cast<HWND>(mv->PlatformHandleRaw);
            POINT origin{0, 0};
            RECT cr{0, 0, 0, 0};
            RECT wr{0, 0, 0, 0};
            if (hw != nullptr) {
                ClientToScreen(hw, &origin);
                GetClientRect(hw, &cr);
                GetWindowRect(hw, &wr);
            }
            std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n),
                          " | os cursor %ld,%ld client origin %ld,%ld client %ldx%ld window %ld,%ld %ldx%ld",
                          os.x, os.y, origin.x, origin.y, cr.right, cr.bottom, wr.left, wr.top,
                          wr.right - wr.left, wr.bottom - wr.top);
        }
#endif
        ImDrawList* ddl = ImGui::GetForegroundDrawList();
        const ImVec2 at(rootTL.x + 300.0f, rootTL.y + 4.0f);
        ddl->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 2.0f), ImVec2(at.x + 1200.0f, at.y + 16.0f),
                           IM_COL32(0, 0, 0, 200));
        ddl->AddText(at, IM_COL32(255, 255, 0, 255), line);
    }
    const float bodyInset = cabinetM + 3.0f;
    ImGui::SetCursorScreenPos(ImVec2(rootTL.x + bodyInset, rootTL.y + bodyInset));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##cabinet_face",
                      ImVec2(rootSize.x - bodyInset * 2.0f, rootSize.y - bodyInset * 2.0f),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // BOTH POPPED IMMEDIATELY. BeginChild paints the child's background inside
    // itself, so the transparent ChildBg has already done its work - and left
    // pushed it would strip the ground from every child NESTED in this one, the
    // status column and the centre included.
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // The bar owns its own height - a fixed deck, not a row of widgets whose
    // tallest item decides - and finishes itself with a bevelled rail. No
    // ImGui separator under it: a flat grey line under a machined one reads as
    // a mistake rather than as a second edge.
    drawToolbar();

    // SCOPE MODE REPLACES THE WHOLE LAYOUT, and that is the mode rather than a
    // side effect of it: the instrument the request came from shows a scope, a
    // detail panel and nothing else, and a scope sharing the window with a
    // waterfall and a settings rail would be a smaller map rather than the
    // thing that was asked for. The toolbar above stays - it carries Play/Stop
    // and the frequency, which are what keeps the aircraft arriving at all -
    // and drawScopeMode draws its own way back before anything else.
    if (scopeMode_) {
        // THE SPECTRUM IS STILL CONSUMED IN SCOPE MODE, even though nothing
        // here draws it. getLatestFrame is what advances lastFrame_, and
        // publishWebSnapshot copies its bins for the browser only when the
        // sequence moves - so skipping it left the web UI serving the last
        // spectrum captured before the mode was entered, underneath a status
        // line that kept reporting the CURRENT frequency and mode. A picture
        // of 88.5 MHz labelled 100.1 MHz is worse than no picture, and this
        // mode persists across restarts, so an install left in it would have
        // served that from its first frame. The waterfall is fed here too, or
        // leaving the mode would show a hard gap in the history.
        if (pipeline_.getLatestFrame(lastFrame_)) {
            waterfall_->addLine(lastFrame_.dbBins.data(),
                                static_cast<int>(lastFrame_.dbBins.size()), dbMin_, dbMax_);
            // Counted here too, so the tally and the ring cannot drift apart
            // across a spell in scope mode. drawCenterPanels restarts its
            // averaging window rather than dividing this by the time the scope
            // was up - see the note there.
            ++waterfallLines_;
        }
        drawScopeMode();
    } else {
        ImGui::BeginChild("##menu_column", ImVec2(kMenuWidth, 0.0f), ImGuiChildFlags_None);
        drawMenuColumn();
        ImGui::EndChild();

        ImGui::SameLine();

        // THE STATUS COLUMN IS OPTIONAL AND SIZED FIRST, so the centre takes
        // what is left rather than the column taking what the centre spared.
        // It is dropped entirely on a narrow window: the spectrum is what this
        // application is for, and squeezing it to keep a status card visible
        // has the priority backwards.
        constexpr float kStatusWidth = 230.0f;
        const bool showStatus =
            ImGui::GetContentRegionAvail().x > kStatusWidth + 520.0f;
        const float centreW = showStatus ? -(kStatusWidth + ImGui::GetStyle().ItemSpacing.x)
                                         : 0.0f;

        // The center area owns its scrolling (none): the spectrum/waterfall
        // pair always fills whatever space the splitter hands it.
        ImGui::BeginChild("##center", ImVec2(centreW, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawCenterPanels();
        ImGui::EndChild();

        if (showStatus) {
            ImGui::SameLine();
            ImGui::BeginChild("##status_column", ImVec2(kStatusWidth, 0.0f),
                              ImGuiChildFlags_None);
            drawStatusColumn();
            ImGui::EndChild();
        }
    }

    // The cabinet's inner face closes here. Its own style pushes were popped at
    // the top, so there is nothing left to unwind but the child itself.
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

    // ONE basemap eviction pass, AFTER every surface that wanted tiles has
    // asked for them. Two things ask now - the map pages, which draw inside
    // drawPluginWindows near the top of this function, and the scope, which
    // draws inside the root window below it - and endFrame() drops every tile
    // nothing asked for during the frame. Left where it was (at the end of the
    // page loop) it would have run BEFORE the scope's requests, evicting the
    // scope's whole tile set on every frame and re-uploading it on the next.
    // Skipped entirely when nothing asked, so an eviction pass with no map and
    // no scope open cannot age the tile set for nothing.
    if (basemapUsedThisFrame_) { basemap_.endFrame(); }
    basemapUsedThisFrame_ = false;

    // Scanner driver, AFTER all widgets: any manual tune the user made this
    // frame (digit wheel, VFO drag, bookmark click) is already applied, so
    // the user-wins detection inside sees this frame's state, not last
    // frame's. Inert while the scanner is Idle — including every hermetic
    // --frames run.
    scannerFrame();

    // Apply any finished SoapySDR scan/open. Last in the frame so the result
    // lands before the next draw reads the device list.
    pollSoapyAsync();
    // Release a wheel-burst retune the coalescer held back (~50 ms pacing).
    pollPendingRetune();
    // Same contract for the catalogue fetch / plugin download.
    pollPluginAsync();
    pollUpdateAsync();
    // And the sink: everything above this line can be working perfectly while
    // the user hears nothing.
    pollAudioHealth();
    // "Still running" beat, five-minute cadence. A no-op when reporting is
    // off, and never blocks - see HeartbeatSender::poll.
    telemetryHeartbeat_.poll(ImGui::GetTime());
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

namespace {

// The bench's quieter lettering, for captions and hints - the ink the
// reference engraves its plate legends in rather than a dimmed white.
//
// THE KEYS NEED NO HELPER ANY MORE. When this was written the application was
// still on ImGui's dark theme and each key had to be dressed at its own call
// site; gui/theme.hpp now sets Button, Header, Frame and the rest globally, so
// a pushBenchStyle() here would only be a second place for the same colours to
// drift apart. That is the whole reason the theme file exists.
// STAGE 3 - THE STATE OF A SECTION, READ WITHOUT OPENING IT.
//
// The reference's rail is a column of plates, each carrying a chip that says
// what that section is doing and a lamp that says whether it is doing it. The
// application's rail was a column of names, so answering "is the recorder on"
// meant opening the recorder. That work now lives in benchSection's own
// arguments - see the note there - because the railChip() that used to stand
// here decorated "whatever item ImGui submitted last", which stopped being the
// header the moment the disclosure key became a real item.

void benchHint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// --- WHY A SURFACE IS EMPTY: MOVED OUT OF THIS FILE ---------------------------
//
// The census of what this machine holds, and the sentence a surface prints when
// it is holding none of it, now live in gui/module_census.hpp. They decide what
// a user is TOLD, and while they sat in this anonymous namespace no test could
// reach one line of them. The census there is general over capability bits, so
// the track-source surfaces below and the decoder surfaces count one module the
// same way and their two sentences cannot come to disagree about it.

// --- the status column's lettering -------------------------------------------
//
// One sub-line of a status card: the words, and the ink they are cut in. A
// colour per line rather than one per card, because a card can carry a SETTING
// and a READING at once - the receiver's antenna port beside the sample rate it
// is actually delivering - and the palette's whole rule is that those two are
// not written in the same colour.
struct StatusLine {
    const char* text;
    ImU32 colour;
};

// Letter-spaced width, which Dear ImGui cannot measure for us because it has no
// tracking. ASCII only, deliberately: every caption on this column is a machine
// legend in capitals.
float statusTrackedWidth(ImFont* f, float px, const char* text, float track) {
    if (f == nullptr || text == nullptr) { return 0.0f; }
    float w = 0.0f;
    int glyphs = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const char one[2] = {*p, '\0'};
        w += f->CalcTextSizeA(px, FLT_MAX, 0.0f, one).x;
        ++glyphs;
    }
    if (glyphs > 1) { w += track * static_cast<float>(glyphs - 1); }
    return w;
}

void statusTrackedText(ImDrawList* dl, ImFont* f, float px, ImVec2 at, ImU32 col,
                       const char* text, float track) {
    if (dl == nullptr || f == nullptr || text == nullptr) { return; }
    float x = at.x;
    for (const char* p = text; *p != '\0'; ++p) {
        const char one[2] = {*p, '\0'};
        dl->AddText(f, px, ImVec2(x, at.y), col, one);
        x += f->CalcTextSizeA(px, FLT_MAX, 0.0f, one).x + track;
    }
}

// A card's caption. LIGHT INK WITH A SHADOW UNDER IT, not the dark cut a
// caption gets on brass: these cards are wells of dark enamel, where the
// engraved treatment would be a caption nobody could read. Same decision, and
// the same reason, as addBenchPlate's title.
void statusCaption(ImDrawList* dl, ImVec2 at, const char* text) {
    ImFont* f = cascade::gui::fonts::legend();
    const float px = cascade::gui::fonts::kTinySize;
    const float track = px * 0.22f;
    statusTrackedText(dl, f, px, ImVec2(at.x + 1.0f, at.y + 1.0f),
                      cascade::gui::theme::withAlpha(cascade::gui::theme::kVoid, 0.6f),
                      text, track);
    statusTrackedText(dl, f, px, at, cascade::gui::theme::kInkMuted, text, track);
}

// A legend CUT INTO BRASS, for the maker's plate at the foot of the column -
// the one surface down here that is metal rather than enamel, and therefore the
// one that takes the dark-into-metal treatment: the lit lower lip of the cut
// first, then the cut itself.
void statusEngrave(ImDrawList* dl, float centreX, float y, float px, const char* text) {
    ImFont* f = cascade::gui::fonts::legend();
    const float track = px * 0.24f;
    const float x = centreX - statusTrackedWidth(f, px, text, track) * 0.5f;
    statusTrackedText(dl, f, px, ImVec2(x, y + 1.0f),
                      cascade::gui::theme::withAlpha(cascade::gui::theme::kBrassTint, 0.55f),
                      text, track);
    statusTrackedText(dl, f, px, ImVec2(x, y), cascade::gui::theme::kEngraved, text, track);
}

// THE TYPEFACE RULE FROM fonts.hpp, APPLIED BY MEASUREMENT RATHER THAN BY
// MEMORY. Nova Mono is for figures: its capital M is three close stems in a
// monospaced cell, and below about 20 px they merge into a solid block - which
// is exactly how "MUTED" came out of a status card once already. So a value
// made only of digits and the punctuation a number carries takes the reading
// face, and anything containing a letter takes the UI face.
ImFont* statusValueFace(const char* text) {
    for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalpha(c) != 0) { return cascade::gui::fonts::ui(); }
    }
    return cascade::gui::fonts::reading();
}

// An elapsed duration as a bench clock reads one: m:ss under an hour, h:mm:ss
// over it. Only ever called with a duration this application timed itself.
void statusElapsed(char* out, std::size_t cap, double seconds) {
    if (out == nullptr || cap == 0) { return; }
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) {
        out[0] = '\0';
        return;
    }
    const long long total = static_cast<long long>(seconds);
    const long long h = total / 3600;
    const long long m = (total % 3600) / 60;
    const long long s = total % 60;
    if (h > 0) {
        std::snprintf(out, cap, "%lld:%02lld:%02lld", h, m, s);
    } else {
        std::snprintf(out, cap, "%lld:%02lld", m, s);
    }
}

}  // namespace

// THE STATUS COLUMN: what the receiver is doing, in one place.
//
// Everything here was already on screen somewhere - buried in a section the
// user had to open, or in the footer, or nowhere at all. Gathering it into one
// column is the point: an operator glances right and knows whether the radio,
// the decoders, the audio and the recorder are healthy without opening
// anything.
//
// EVERY CARD SAYS HOW OLD IT IS OR WHAT IT IS SHOWING. A figure with no
// context reads as current whether or not it is, and this product's design
// brief forbids exactly that.
void AppWindow::drawStatusColumn() {
    // THE COLUMN IS A PLATE, exactly as the function rail on the other side of
    // the face is. addBenchPlate lays the ground, the bevel, the engraved title
    // and the rule under it, and hands back the y beneath that rule - so the
    // cards start from a measurement rather than from a guess at how tall a
    // title is.
    constexpr float kPad = 8.0f;
    // How long the decoder-line rate is averaged over. Two seconds: long enough
    // that a burst decoder (ADS-B is silent between aircraft) does not flick
    // between 0 and 40, short enough that the figure still tracks a receiver
    // being tuned across a band.
    constexpr double kRateWindowS = 2.0;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 colTL = ImGui::GetWindowPos();
    const ImVec2 colSize = ImGui::GetWindowSize();
    const ImVec2 colBR(colTL.x + colSize.x, colTL.y + colSize.y);
    if (colSize.x < 40.0f || colSize.y < 60.0f) { return; }
    const float bodyTop = cascade::gui::addBenchPlate(dl, colTL, colBR, "STATUS");

    ImFont* legendF = cascade::gui::fonts::legend();
    ImFont* uiF = cascade::gui::fonts::ui();
    const float tinyPx = cascade::gui::fonts::kTinySize;
    const float valuePx = cascade::gui::fonts::kUiSize;
    const float tinyH = legendF->CalcTextSizeA(tinyPx, FLT_MAX, 0.0f, "X").y;
    const float valueH = uiF->CalcTextSizeA(valuePx, FLT_MAX, 0.0f, "X").y;

    // THE MAKER'S PLATE IS MEASURED FIRST AND DRAWN LAST, so the cards know
    // where they have to stop. A card laid over it would be dark lettering on
    // brass, and the plate is the one fixed thing in this column.
    const float plateH = tinyH * 2.0f + 13.0f;
    const ImVec2 plateTL(colTL.x + kPad, colBR.y - kPad - plateH);
    const ImVec2 plateBR(colBR.x - kPad, colBR.y - kPad);
    const float cardsBottom = plateTL.y - 8.0f;

    const float cardL = colTL.x + kPad;
    const float cardR = colBR.x - kPad;
    float y = bodyTop;

    const double nowS = ImGui::GetTime();
    const ImU32 kFaint = cascade::gui::theme::kInkFaint;
    const ImU32 kMuted = cascade::gui::theme::kInkMuted;

    // ONE CARD: a well cut into the plate, a caption, a value, and as many
    // sub-lines as that card has honest things to say. The height is computed
    // from the lines that actually go in it and never from a multiple of the
    // current line height - the caption and the value are set at two different
    // sizes, so "three and a bit lines" is not a measurement of anything, and
    // when it was, every card clipped its own sub-line to a sliver.
    //
    // A card that will not fit above the maker's plate is SKIPPED whole. Half a
    // card, or a card lettered across the plate, is worse than a card the user
    // has to make the window taller to see.
    const auto card = [&](const char* caption, ImU32 valueCol, const char* value,
                          const StatusLine* lines, int lineCount) {
        const float h = 6.0f + tinyH + 2.0f + valueH +
                        static_cast<float>(lineCount) * (tinyH + 1.0f) + 6.0f;
        if (y + h > cardsBottom) { return; }
        const ImVec2 tl(cardL, y);
        const ImVec2 br(cardR, y + h);
        dl->AddRectFilled(tl, br, cascade::gui::theme::kWell,
                          cascade::gui::theme::kKeyRounding);
        dl->AddRect(tl, br,
                    cascade::gui::theme::withAlpha(cascade::gui::theme::kBrassDark, 0.90f),
                    cascade::gui::theme::kKeyRounding, 0, cascade::gui::theme::kHairline);
        // raised=false: the hairline of light along the BOTTOM and right, which
        // is the whole difference between a card sitting on the plate and one
        // cut into it.
        cascade::gui::addBenchBevel(dl, tl, br, cascade::gui::theme::kKeyRounding, false);
        // CLIPPED TO ITS OWN WELL. A device name is whatever its driver calls
        // it, and one long enough to reach the next card would be lettering
        // across a card it is not about.
        dl->PushClipRect(ImVec2(tl.x + 1.0f, tl.y + 1.0f),
                         ImVec2(br.x - 1.0f, br.y - 1.0f), true);
        float ty = tl.y + 6.0f;
        statusCaption(dl, ImVec2(tl.x + 8.0f, ty), caption);
        ty += tinyH + 2.0f;
        dl->AddText(statusValueFace(value), valuePx, ImVec2(tl.x + 8.0f, ty), valueCol,
                    value);
        ty += valueH;
        for (int i = 0; i < lineCount; ++i) {
            ty += 1.0f;
            if (lines[i].text != nullptr && lines[i].text[0] != '\0') {
                dl->AddText(legendF, tinyPx, ImVec2(tl.x + 8.0f, ty), lines[i].colour,
                            lines[i].text);
            }
            ty += tinyH;
        }
        dl->PopClipRect();
        y = br.y + 6.0f;
    };

    char v[96];
    char l0[128];
    char l1[128];
    char l2[128];
    char l3[128];

    // --- AUDIO ---------------------------------------------------------------
    //
    // THE REFERENCE ARTBOARD LETTERS THIS CARD "AUDIO BUFFER" AND PRINTS "41
    // ms". NOTHING IN THIS APPLICATION MEASURES THAT. AudioOut keeps its ring
    // private and publishes no fill level, no depth and no latency; the single
    // number it does publish about the health of the sound path is a count of
    // STARVED CALLBACKS. So that is the figure, and the caption names it. A
    // millisecond number here would have had to be invented, and an invented
    // reading on an instrument face is the one fault this panel may not have.
    const unsigned long long under =
        static_cast<unsigned long long>(pipeline_.audio().underruns());
    std::snprintf(v, sizeof(v), "%llu", under);
    {
        const StatusLine lines[1] = {
            {under == 0 ? "no callback has starved yet" : "starved callbacks, since start",
             under == 0 ? kFaint : cascade::gui::theme::kAlarm}};
        card("AUDIO - UNDERRUNS", cascade::gui::theme::kAmber, v, lines, 1);
    }

    // --- MESSAGE RATE --------------------------------------------------------
    //
    // WHAT IS COUNTED IS SAID ON THE CARD. No decoder plugin reports a message
    // tally across the ABI and PluginRunner keeps none, so the countable thing a
    // decoder produces is a LINE of output - one per decoded message for every
    // text decoder shipped, and status text for the rest. It is DIFFERENCED over
    // a window rather than sampled: a cumulative counter is not a rate, and
    // printing one as though it were is the same lie in a shorter form.
    if (decoderRateWindowS_ < 0.0) {
        decoderRateWindowS_ = nowS;
        decoderLinesAtWindow_ = decoderLinesTotal_;
    } else if (nowS - decoderRateWindowS_ >= kRateWindowS) {
        decoderLinesPerSec_ = static_cast<float>(
            static_cast<double>(decoderLinesTotal_ - decoderLinesAtWindow_) /
            (nowS - decoderRateWindowS_));
        decoderRateWindowS_ = nowS;
        decoderLinesAtWindow_ = decoderLinesTotal_;
    }
    // BEING FED IS TWO CONDITIONS, NOT ONE. PluginRunner::activeCount says its
    // instances exist and are matched to the rate the pipeline is configured
    // for; it does NOT say the DSP threads are turning. A stopped receiver
    // hands them nothing, so the run state is checked alongside it everywhere
    // below - the first render of this column read "2 running" and "DECODING"
    // with the pipeline stopped and the picture frozen.
    const bool rxRunning = pipeline_.running();
    // PER MODULE, not per instance - the same population the "of N fitted"
    // denominator below counts. activeCount() counts decoder INSTANCES, and a
    // module that declares both an audio decoder and an I/Q one has two, so
    // the old pair could print "3 running of 2 fitted".
    const std::size_t matched = fedDecoderCount();
    const std::size_t feeding = rxRunning ? matched : 0;
    {
        StatusLine lines[1];
        if (feeding == 0) {
            lines[0] = {rxRunning ? "no decoder is running" : "the receiver is stopped",
                        kFaint};
            card("MESSAGE RATE", kMuted, "--", lines, 1);
        } else if (decoderLinesPerSec_ < 0.0f) {
            // The first window has not closed yet. "--" rather than 0.0 /s:
            // "we have not measured" and "we measured nothing" are different
            // statements and this panel is not allowed to confuse them.
            lines[0] = {"measuring", kFaint};
            card("MESSAGE RATE", kMuted, "--", lines, 1);
        } else {
            std::snprintf(v, sizeof(v), "%.1f /s",
                          static_cast<double>(decoderLinesPerSec_));
            std::snprintf(l0, sizeof(l0), "decoder output lines, %.0f s mean",
                          kRateWindowS);
            lines[0] = {l0, kFaint};
            card("MESSAGE RATE", cascade::gui::theme::kAmber, v, lines, 1);
        }
    }

    // --- DECODERS ------------------------------------------------------------
    //
    // BEING FED, NOT MERELY INSTALLED. PluginRunner::activeCount is the number
    // of decoder instances the runner is actually handing samples to - which is
    // a different question from "the user has not stopped it", and the runner's
    // own header spends a paragraph on why they must not be conflated. A plugin
    // that needs a rate the receiver is not producing is idle; it is counted on
    // the sub-line, so a small figure here is explained rather than mysterious.
    //
    // AND THE DENOMINATOR IS DECODERS, WHICH IS WHAT THE CAPTION SAYS. It used
    // to be every loaded module, so a basemap and a track-info provider - both
    // of which are fed no signal by design and never can be - sat permanently
    // in "of N installed, M not fed" under the word DECODERS. Two modules
    // doing exactly what they were fitted to do were reported as two decoders
    // that had stopped working. loadedDecoderCount() asks the runner's own
    // question instead: does this module supply a decoder table at all.
    const std::size_t installed = loadedDecoderCount();
    if (!rxRunning) {
        // READY, NOT RUNNING. The instances are built and matched; the receiver
        // that would feed them is stopped, so nothing is decoding and the card
        // must not imply that it is.
        std::snprintf(v, sizeof(v), "%zu ready", matched);
        std::snprintf(l0, sizeof(l0), "of %zu fitted - receiver stopped", installed);
    } else {
        std::snprintf(v, sizeof(v), "%zu running", feeding);
        if (installed > feeding) {
            std::snprintf(l0, sizeof(l0), "of %zu fitted, %zu not fed", installed,
                          installed - feeding);
        } else {
            std::snprintf(l0, sizeof(l0), "of %zu fitted", installed);
        }
    }
    {
        const StatusLine lines[1] = {{l0, kFaint}};
        card("DECODERS", feeding > 0 ? cascade::gui::theme::kPhosphor : kMuted, v, lines,
             1);
    }

    // --- SINK ----------------------------------------------------------------
    //
    // "WHY IS THERE NO SOUND", ANSWERED BEFORE IT IS ASKED - and the duration
    // beneath it is measured here or not written at all. Nothing in the pipeline
    // timestamps a mute, so what this column can honestly report is how long IT
    // has been watching this particular one: the subject is compared against
    // what it saw last frame and the clock starts when that changes. A mute that
    // began before the application did, or while this column was hidden, gets no
    // duration line rather than a fabricated one.
    const std::string muteBy = muteSubjectText();
    if (muteBy != muteSubjectSeen_) {
        muteSubjectSeen_ = muteBy;
        muteSinceS_ = muteBy.empty() ? -1.0 : nowS;
    }
    {
        StatusLine lines[2];
        int n = 0;
        if (!muteBy.empty()) {
            std::snprintf(l0, sizeof(l0), "by %s", muteBy.c_str());
            lines[n++] = {l0, kFaint};
            if (muteSinceS_ >= 0.0) {
                char elapsed[24];
                statusElapsed(elapsed, sizeof(elapsed), nowS - muteSinceS_);
                std::snprintf(l1, sizeof(l1), "for %s", elapsed);
                lines[n++] = {l1, kFaint};
            }
            card("SINK", cascade::gui::theme::kAmber, "MUTED", lines, n);
        } else if (!pipeline_.audio().everOpened()) {
            lines[n++] = {"no output device was opened", kFaint};
            card("SINK", kMuted, "NO DEVICE", lines, n);
        } else if (!pipeline_.audio().streamAlive()) {
            // The dead-stream case the audio watchdog exists for: everything
            // upstream stays healthy and the speakers go quiet, so it has to be
            // visible from the panel rather than inferred from silence.
            lines[n++] = {"the output stream stopped", cascade::gui::theme::kAlarm};
            card("SINK", cascade::gui::theme::kAlarm, "STOPPED", lines, n);
        } else {
            std::snprintf(l0, sizeof(l0), "%s",
                          pipeline_.audio().openedDeviceName().c_str());
            lines[n++] = {l0, kFaint};
            card("SINK", cascade::gui::theme::kPhosphor, "OPEN", lines, n);
        }
    }

    // --- RECORDER ------------------------------------------------------------
    //
    // Both figures come from the take itself: bytesWritten is what stdio
    // actually accepted (Recorder's header is explicit that its counters only
    // ever report accepted bytes), and the elapsed time runs from the start
    // stamp the recorder section already takes. ONLY A LIVE RECORDER'S BYTES ARE
    // ADDED - a stopped one keeps its final total by design, and including it
    // would inflate the take that is running with the size of one that ended.
    const bool iqTaping = iqRecorder_.recording();
    const bool audioTaping = audioRecorder_.recording();
    if (iqTaping || audioTaping) {
        const char* what =
            (iqTaping && audioTaping) ? "IQ + AUDIO" : (iqTaping ? "IQ" : "AUDIO");
        const double startS = iqTaping ? iqRecordStartS_ : audioRecordStartS_;
        char elapsed[24];
        statusElapsed(elapsed, sizeof(elapsed), nowS - startS);
        double bytes = 0.0;
        if (iqTaping) { bytes += static_cast<double>(iqRecorder_.bytesWritten()); }
        if (audioTaping) { bytes += static_cast<double>(audioRecorder_.bytesWritten()); }
        std::snprintf(l0, sizeof(l0), "%s elapsed", elapsed);
        std::snprintf(l1, sizeof(l1), "%.1f MB written", bytes / (1024.0 * 1024.0));
        // A DISK THAT STOPPED TAKING THE FILE IS SAID, NOT LEFT TO BE NOTICED.
        // The recorder latches writeFailed() when stdio refuses a write (full,
        // removed, gone read-only) and stops paying for a dead stream; from
        // out here that used to look like a take whose MB counter had frozen,
        // with the card still reading IQ in rust as if all were well.
        const bool diskFault = (iqTaping && iqRecorder_.writeFailed()) ||
                               (audioTaping && audioRecorder_.writeFailed());
        if (diskFault) {
            const StatusLine lines[2] = {{l1, kFaint},
                                         {"disk refused the file - nothing more is being kept",
                                          cascade::gui::theme::kAlarmHot}};
            card("RECORDER", cascade::gui::theme::kAlarmHot, "STALLED", lines, 2);
        } else {
            const StatusLine lines[2] = {{l0, kFaint}, {l1, kFaint}};
            card("RECORDER", cascade::gui::theme::kAlarm, what, lines, 2);
        }
    } else {
        const StatusLine lines[1] = {{"no file open", kFaint}};
        card("RECORDER", kMuted, "off", lines, 1);
    }

    // --- WEB ACCESS ----------------------------------------------------------
    //
    // NOT ON THE REFERENCE ARTBOARD, AND KEPT ANYWAY. It is a listening socket
    // on the user's own machine, and whether it is open is exactly the sort of
    // thing a status column exists to answer without opening a section. Dropping
    // it to match a mock would be losing a fact to gain a resemblance.
    if (webServer_.running()) {
        std::snprintf(v, sizeof(v), "port %d", webCfg_.port);
        std::snprintf(l0, sizeof(l0), "%s", webCfg_.bindAddress.c_str());
        const StatusLine lines[1] = {{l0, kFaint}};
        card("WEB ACCESS", cascade::gui::theme::kPhosphor, v, lines, 1);
    } else {
        const StatusLine lines[1] = {{"not listening", kFaint}};
        card("WEB ACCESS", kMuted, "off", lines, 1);
    }

    // --- RECEIVER, and it is the tall one ------------------------------------
    //
    // The reference card carries four lines: the device, the antenna port, the
    // gain and a frequency correction. THE FOURTH DOES NOT EXIST HERE. Nothing
    // in FoxSDR reads, sets or stores a PPM correction - not SoapySource, not
    // the config store, nowhere - so this card has no "0.5 PPM" line, because
    // the only way to draw one would be to make the number up.
    //
    // The three that do exist are lettered by what they ARE. The antenna is a
    // readback (SoapySource::antenna(), taken after the open), the gain is what
    // this application COMMANDED - the driver offers no per-element readback -
    // and both are settings, so both are cream. The sample rate is the device's
    // own readback of what it is delivering, so it is amber: the palette's rule
    // is that amber belongs to a measurement and nothing else.
    {
        const bool faulted = pipeline_.faulted();
        StatusLine lines[4];
        int n = 0;
        std::snprintf(l0, sizeof(l0), "%s", pipeline_.activeSource().name());
        lines[n++] = {l0, cascade::gui::theme::kCream};
        if (soapy_ != nullptr && !soapyAntenna_.empty()) {
            std::snprintf(l1, sizeof(l1), "ANTENNA %s", soapyAntenna_.c_str());
            lines[n++] = {l1, cascade::gui::theme::kCream};
        }
        l2[0] = '\0';
        if (soapy_ != nullptr) {
            if (soapyAgc_) {
                std::snprintf(l2, sizeof(l2), "GAIN AUTO - AGC ON");
            } else if (!soapyGainNames_.empty() &&
                       soapyGainsDb_.size() == soapyGainNames_.size()) {
                // The first element by name, and a count of the rest. Summing
                // them would print a total this application never commanded and
                // the driver never reported.
                if (soapyGainNames_.size() == 1) {
                    std::snprintf(l2, sizeof(l2), "%s %.0f dB", soapyGainNames_[0].c_str(),
                                  static_cast<double>(soapyGainsDb_[0]));
                } else {
                    std::snprintf(l2, sizeof(l2), "%s %.0f dB +%zu more",
                                  soapyGainNames_[0].c_str(),
                                  static_cast<double>(soapyGainsDb_[0]),
                                  soapyGainNames_.size() - 1);
                }
            }
            if (l2[0] != '\0') { lines[n++] = {l2, cascade::gui::theme::kCream}; }
        }
        const double rate = pipeline_.activeSource().sampleRateHz();
        if (std::isfinite(rate) && rate > 0.0) {
            std::snprintf(l3, sizeof(l3), "%.3f MS/s", rate / 1.0e6);
            lines[n++] = {l3, cascade::gui::theme::kAmber};
        }
        card("RECEIVER",
             faulted ? cascade::gui::theme::kAlarm
                     : (rxRunning ? cascade::gui::theme::kPhosphor : kMuted),
             faulted ? "FAULT" : (rxRunning ? "RUNNING" : "STOPPED"), lines, n);
    }

    // --- the maker's plate ---------------------------------------------------
    //
    // The only brass in this column, so the only thing in it that takes the
    // engraved treatment - dark cut into metal with the lit lower lip of the cut
    // beneath. Two lines, because a maker's plate carries the maker and the
    // type. Drawn last so nothing can be laid over it, and skipped entirely on a
    // column too short to hold it above the title rule.
    if (plateTL.y > bodyTop && plateBR.x > plateTL.x + 16.0f) {
        const float round = cascade::gui::theme::kKeyRounding;
        dl->AddRectFilled(plateTL, plateBR, cascade::gui::theme::kBrassShade, round);
        if (plateBR.x - plateTL.x > round * 2.0f) {
            dl->AddRectFilledMultiColor(ImVec2(plateTL.x + round, plateTL.y),
                                        ImVec2(plateBR.x - round, plateBR.y),
                                        cascade::gui::theme::kBrassShade,
                                        cascade::gui::theme::kBrassShade,
                                        cascade::gui::theme::kBrassMid,
                                        cascade::gui::theme::kBrassMid);
        }
        cascade::gui::addBenchBevel(dl, plateTL, plateBR, round, true);
        const float midX = (plateTL.x + plateBR.x) * 0.5f;
        statusEngrave(dl, midX, plateTL.y + 5.0f, tinyPx, "FOX & SCHIRMER");
        statusEngrave(dl, midX, plateTL.y + 5.0f + tinyH + 1.0f, tinyPx,
                      "TYPE 71 - MK II");
    }
}

namespace {

// --- THE TOP BAR'S GEOMETRY --------------------------------------------------
//
// Measured off the reference face and kept in ITS units - a 1720 x 986 artboard
// whose top bar is 157 tall - but laid out from the bar's own top-left corner
// and never from that width. The cluster from the transport button to the
// volume dial is a fixed piece of hardware, so it keeps its size as the window
// grows; only the two meters are pinned to the right edge.
//
// WHY EXPLICIT GEOMETRY AND NOT SameLine. The reference positions things
// vertically as well as horizontally: a caption at 45, the dial it names at 82,
// the value it reads at 126, all three in one column. ImGui's cursor flow can
// express a row of widgets and nothing else, and a row of widgets is exactly
// what the old bar looked like.
constexpr float kBarH = 160.0f;   // the bar's height, in reference units
constexpr float kCoreW = 800.0f;  // transport button through volume dial

// THE VOLUME DIAL'S OWN GEOMETRY, HOISTED OUT OF drawToolbar, because the
// bar's scale floor is a promise about this one control and a promise checked
// against a number typed somewhere else is not checked at all.
constexpr float kVolumeCx = 745.0f;  // the dial's centre, in reference units
constexpr float kVolumeR = 26.0f;    // ...and its radius
constexpr float kVolumeEdgePad = 6.0f;

// The bar only ever shrinks, and it stops here: below this the counter's
// digits stop being figures and the dial stops being a control (its primitive
// draws nothing under a 6 px radius, which at this scale is a long way off).
constexpr float kBarMinScale = 0.62f;

// AND THE FLOOR IS ONLY HONEST IF THE WINDOW CANNOT GO NARROWER THAN IT NEEDS.
// kMinWindowW is the width limit run() puts on the OS window; the cabinet eats
// at most kCabinetInsetMaxPx of it before the bar is drawn. If someone lowers
// that limit, or moves the dial right, this stops compiling rather than
// quietly clipping the only volume control out of the application.
static_assert((kVolumeCx + kVolumeR + kVolumeEdgePad) * kBarMinScale <=
                  static_cast<float>(kMinWindowW) - kCabinetInsetMaxPx,
              "the top bar's floor scale must leave the volume dial inside the "
              "narrowest window run() allows");

// THE COUNTER, CELL BY CELL, AND SHARED WITH drawFrequencyReadout. The bar
// sizes its middle section from the same numbers the readout draws with,
// because two copies of this arithmetic is how a well and the digits inside it
// come to disagree about where they are.
constexpr int kFreqCells = 10;
constexpr float kFreqCellW = 30.0f;
constexpr float kFreqCellH = 44.0f;
constexpr float kFreqGap = 3.0f;        // between apertures
constexpr float kFreqGroupGap = 10.0f;  // ...and at a thousands break
constexpr float kFreqWellPadX = 12.0f;
constexpr float kFreqWellPadY = 6.0f;

// A wider gap BEFORE these cells: after the first digit, and after the fourth
// and the seventh, which is where a mechanical counter's thousands breaks
// fall. The reference calls out the break after the seventh; the other two are
// the same rule carried up the scale, and dropping them would make ten
// undifferentiated digits harder to read rather than easier.
constexpr bool freqGroupBreak(int i) { return i == 1 || i == 4 || i == 7; }

constexpr float freqRowWidth() {
    float w = 0.0f;
    for (int i = 0; i < kFreqCells; ++i) {
        if (i > 0) { w += freqGroupBreak(i) ? kFreqGroupGap : kFreqGap; }
        w += kFreqCellW;
    }
    return w;
}
constexpr float kFreqWellW = freqRowWidth() + kFreqWellPadX * 2.0f;
constexpr float kFreqWellH = kFreqCellH + kFreqWellPadY * 2.0f;

// LETTER-SPACING, WHICH DEAR IMGUI HAS NOT, and which is most of what
// separates an engraved legend from a word in a label. ASCII only and
// deliberately: every caption on this bar is a machine legend in capitals.
float barTrackedWidth(ImFont* f, float px, const char* text, float track) {
    if (f == nullptr || text == nullptr) { return 0.0f; }
    float w = 0.0f;
    int glyphs = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const char one[2] = {*p, '\0'};
        w += f->CalcTextSizeA(px, FLT_MAX, 0.0f, one).x;
        ++glyphs;
    }
    if (glyphs > 1) { w += track * static_cast<float>(glyphs - 1); }
    return w;
}

// A caption CUT INTO THE BRASS: the lit lower lip of the cut first, then the
// cut itself. Dark-into-metal is this design's treatment for a caption, and
// for a caption only - a figure goes on glass, which is why every reading on
// this bar is amber in a well or ivory on a meter's face and none of them is
// lettered like this.
//
// THE CUT IS DEEPER THAN IT WAS, AND THAT IS THE OTHER HALF OF "HARD TO SEE".
// theme.hpp says so in its own header: dark-into-brass is about 2.3:1, "fine
// for a label at rest and not acceptable for a number somebody is trying to
// read at arm's length". Measured, kEngraved (#3B3529) on this deck's own
// gradient is 2.11:1 against kBrassMid at the bottom of the ramp and 2.62:1
// against kBrassShade at the top - the worst contrast anywhere on this window,
// and it is carrying MASTER, TUNED - HERTZ and VOLUME, which are the three
// words that say what the three instruments on the deck ARE. Those are labels
// a user has to read at least once; the lamp words under MASTER, the digits in
// the counter and the figure under the dial are all already on glass.
//
// So the cut is FILLED rather than repainted: the void laid into it at 0.90,
// which is what a paint-filled engraving on a 1960s brass panel actually is,
// and which measures 3.19:1 against kBrassMid and 3.93:1 against kBrassShade.
// The lit lip is raised with it, from 0.55 to 0.75, because on a real cut it is
// the highlight along the lower edge that makes the letterform, not the shadow.
// Nothing else about the treatment changes and no other surface is touched: the
// maker's plate at the foot of the status column keeps the shallow cut, because
// a maker's plate is decoration and is not read for information.
void barEngrave(ImDrawList* dl, ImVec2 at, float px, const char* text, bool centred) {
    if (dl == nullptr || text == nullptr || text[0] == '\0') { return; }
    ImFont* f = cascade::gui::fonts::legend();
    const float track = px * 0.24f;
    if (centred) { at.x -= barTrackedWidth(f, px, text, track) * 0.5f; }
    const ImU32 lip =
        cascade::gui::theme::withAlpha(cascade::gui::theme::kBrassTint, 0.75f);
    const ImU32 cut = cascade::gui::theme::withAlpha(cascade::gui::theme::kVoid, 0.90f);
    float x = at.x;
    for (const char* p = text; *p != '\0'; ++p) {
        const char one[2] = {*p, '\0'};
        dl->AddText(f, px, ImVec2(x, at.y + 1.0f), lip, one);
        dl->AddText(f, px, ImVec2(x, at.y), cut, one);
        x += f->CalcTextSizeA(px, FLT_MAX, 0.0f, one).x + track;
    }
}

}  // namespace

void AppWindow::drawToolbar() {
    // THE BAR IS A PANEL, NOT A ROW OF CONTROLS: a brass deck carrying the
    // transport, the master lamps, the counter, the volume dial and the two
    // meters, with a bevelled rail across its foot dividing it from the body
    // below. Everything in it is placed at a measured position.
    const float availW = ImGui::GetContentRegionAvail().x;

    // ONE SCALE FOR THE WHOLE BAR, AND IT ONLY EVER SHRINKS. A window narrower
    // than the fixed cluster gets the same panel drawn smaller rather than a
    // panel with pieces missing - the volume dial is the ONLY volume control
    // in the application, so dropping it on a narrow window would be a
    // regression wearing the clothes of a layout decision.
    //
    // THE FLOOR IS KEPT BY THE WINDOW, NOT BY THIS LINE. Stopping the scale at
    // kBarMinScale is what keeps the cluster legible; it is run()'s width limit
    // on the OS window that keeps the cluster INSIDE the bar, and the
    // static_assert beside kBarMinScale is what ties the two together. Before
    // that limit existed this floor was a claim the code could not keep: at
    // 300 px of width the dial was drawn 160 px past the bar's right edge, the
    // child clipped it, and the application had no volume control at all.
    float scale = 1.0f;
    if (availW > 0.0f && availW < kCoreW) {
        scale = std::max(kBarMinScale, availW / kCoreW);
    }
    const float barH = kBarH * scale;

    // DRAWN IN A CHILD, so the bar clips itself. Explicit geometry means an
    // item can be asked for at a position a narrow window cannot hold, and a
    // child cuts it off at the bar's edge instead of letting it paint across
    // the spectrum below.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool barOpen =
        ImGui::BeginChild("##bench_topbar", ImVec2(0.0f, barH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // A CULLED CHILD DRAWS NOTHING AT ALL. Everything below paints straight
    // into a draw list, which does not consult ImGui's SkipItems the way a
    // widget does, so the bar has to check for itself rather than emit a
    // panel's worth of geometry into a window that is not being rendered.
    if (!barOpen) {
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 barTL = ImGui::GetWindowPos();
    const float barW = ImGui::GetWindowSize().x;
    const ImVec2 barBR(barTL.x + barW, barTL.y + barH);
    // Reference units to screen pixels, in one place: X and Y for a position,
    // S for a length.
    const auto X = [&](float u) { return barTL.x + u * scale; };
    const auto Y = [&](float u) { return barTL.y + u * scale; };
    const auto S = [&](float u) { return u * scale; };
    // The engraving never goes below nine pixels: a caption too small to read
    // is not a smaller caption, it is dirt on the panel.
    const float capPx = std::max(9.0f, cascade::gui::fonts::kTinySize * scale);

    // The brass the deck is machined from, lit from above like every other
    // surface on this face.
    dl->AddRectFilledMultiColor(barTL, barBR, cascade::gui::theme::kBrassShade,
                                cascade::gui::theme::kBrassShade,
                                cascade::gui::theme::kBrassMid,
                                cascade::gui::theme::kBrassMid);

    // --- the transport ------------------------------------------------------
    // The label reads the pipeline, not a local flag, so the button can never
    // disagree with the actual thread state - drawBenchStopButton letters
    // itself STOP or START from the same bool.
    const bool running = pipeline_.running();
    if (cascade::gui::drawBenchStopButton(dl, ImVec2(X(74.0f), Y(85.0f)), S(46.0f),
                                          running)) {
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(running ? "Stop the receiver" : "Start the receiver");
    }
    // THE MASTER CLUSTER. Four lamps under an engraved caption, each with its
    // word beneath it, because a state carried by colour alone fails the
    // design's own black-and-white rule - and MUTE amber against FAIL rust is
    // a hue distinction that about one man in twelve cannot make.
    barEngrave(dl, ImVec2(X(150.0f), Y(52.0f)), capPx, "MASTER", false);
    {
        // BEING FED, WHICH IS THE STRONGEST THING THIS APPLICATION CAN
        // HONESTLY SAY, and the same predicate the DECODERS card in the status
        // column prints a few hundred pixels away. That card reads "2 running,
        // of 4 installed, 2 not fed"; this lamp used to count every plugin the
        // user had not stopped, so a rig with four installed and none being
        // handed samples lit DEC while the card said nothing was running.
        //
        // PluginRunner::activeCount is the number of decoder instances the
        // runner is actually giving samples to - not "loaded", and not "the
        // user has not stopped it" - and the receiver has to be running for
        // those samples to exist at all, which is why pipeline_.running() is
        // part of it: the runner keeps its matched instances across a stop.
        //
        // It is a PROXY and the word is meant: being fed is not proof that a
        // frame was decoded, only that the samples are arriving. Nothing in
        // the plugin ABI reports a successful decode, so this is the honest
        // ceiling, and MESSAGE RATE in the status column is where the traffic
        // itself is reported.
        const bool decoding = running && pluginRunner_.activeCount() > 0;
        const bool muted = !muteSubjectText().empty();
        struct MasterLamp {
            ImU32 colour;
            bool lit;
            const char* word;
        };
        const MasterLamp lamps[4] = {
            {cascade::gui::theme::kPhosphor, running, "RUN"},
            {cascade::gui::theme::kPhosphor, decoding, "DEC"},
            {cascade::gui::theme::kAmber, muted, "MUTE"},
            {cascade::gui::theme::kAlarm, pipeline_.faulted(), "FAIL"},
        };
        // drawBenchLamp letters its caption in whatever face is bound, and the
        // UI face at its normal size runs MUTE straight into FAIL at this
        // pitch, so these take the engraving face - AT THE DECK'S OWN CAPTION
        // SIZE, which is what they should have been all along.
        //
        // ELEVEN WAS A GUESS THAT SURVIVED A TYPE CHANGE. It was chosen to fit
        // the reference's 28-unit lamp spacing and never re-checked; when
        // fonts.hpp went up two points these four words stayed put and became
        // the smallest lettering anywhere in the application - four state
        // indicators, set smaller than the caption above them and smaller than
        // every legend around them, which is precisely the complaint this
        // change set answers. Measured at the engraving face: the tightest
        // adjacent pair is MUTE/FAIL, which needs 15.74 px of the 28-unit
        // pitch at capPx = 14, leaving over twelve pixels of clear metal
        // between them. capPx keeps its own nine-pixel floor, so a bar shrunk
        // to kBarMinScale still letters them rather than smudging them.
        ImGui::PushFont(cascade::gui::fonts::legend(), capPx);
        for (int i = 0; i < 4; ++i) {
            cascade::gui::drawBenchLamp(
                dl, ImVec2(X(158.0f + 28.0f * static_cast<float>(i)), Y(86.0f)), S(7.0f),
                lamps[i].colour, lamps[i].lit, lamps[i].word);
        }
        ImGui::PopFont();
    }

    // --- the counter --------------------------------------------------------
    // A groove between the master cluster and the tuned figure, then the
    // engraved caption over the well the digits are recessed into.
    cascade::gui::addBenchDivider(dl, X(272.0f), Y(30.0f), Y(135.0f));
    barEngrave(dl, ImVec2(X(304.0f), Y(42.0f)), capPx, "TUNED - HERTZ", false);
    drawFrequencyReadout(X(292.0f), Y(62.0f), scale);
    cascade::gui::addBenchDivider(dl, X(684.0f), Y(30.0f), Y(135.0f));
    // THE VOLUME IS A DIAL, in the handoff's 1960s brass. A slider is a
    // perfectly good control and completely wrong on a bench receiver; this
    // one turns, carries its own tick arc, and answers the wheel as well as
    // the hand so it is still usable without a drag.
    {
        const float cx = X(kVolumeCx);
        barEngrave(dl, ImVec2(cx, Y(42.0f)), capPx, "VOLUME", true);
        const float moved = cascade::gui::drawBrassVolumeKnob(
            dl, ImVec2(cx, Y(82.0f)), S(kVolumeR), volume_);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Volume - drag the dial, or scroll over it");
        }
        if (moved >= 0.0f) {
            volume_ = moved;
            pipeline_.audio().setVolume(volume_);
        }
        // THE SETTING, IN CREAM, AND DELIBERATELY NOT IN AMBER. This is where
        // the hand has put the control, not something the radio measured, and
        // the palette reserves amber for a reading. Figures, so the monospaced
        // face: the number stops shuffling sideways as the dial turns.
        char vtxt[16];
        std::snprintf(vtxt, sizeof(vtxt), "%.2f", static_cast<double>(volume_));
        ImFont* vf = cascade::gui::fonts::reading();
        const float vpx = std::max(11.0f, cascade::gui::fonts::kReadingSize * scale);
        const ImVec2 vs = vf->CalcTextSizeA(vpx, FLT_MAX, 0.0f, vtxt);
        dl->AddText(vf, vpx, ImVec2(cx - vs.x * 0.5f, Y(118.0f)),
                    cascade::gui::theme::kCream, vtxt);
    }

    // THE TWO METERS, and BOTH ARE DRIVEN BY THE FIGURE PRINTED UNDER THEM.
    //
    // The reference's own meters are decoration - its PROCESSOR needle animates
    // through 44-62% of arc while the text beside it reads 22%, and its SAMPLE
    // RATE needle is hand-placed with no full scale stated anywhere. Copying
    // either would put a moving needle next to a number it disagrees with,
    // which is the single most damaging thing a restyle can do: an instrument
    // that lies is worse than no instrument.
    //
    // So both scales are chosen here, stated here, and the needle comes from
    // the same value as the text. There is deliberately NO "processor load"
    // meter, because this application measures no such thing and inventing one
    // would be the same lie in a different place; the second meter reports the
    // frame time it actually has, and says so.
    //
    // THE REFERENCE CAPTIONS THE SECOND METER "PROCESSOR", AND IT IS NOT
    // RENAMED TO MATCH. Nothing in this application measures processor load -
    // there is no such figure anywhere in the pipeline, the plugin host or the
    // watchdog - so a meter under that caption would have to be fed by
    // something invented, which is the artboard's own fault repeated in our
    // code. It keeps the name of the thing it actually measures.
    const float lineH = ImGui::GetTextLineHeight();
    constexpr float kMeterW = 126.0f;
    constexpr float kMeterFaceH = 66.0f;
    // drawBenchMeter spends one text line above the face on the caption and
    // one below it on the value, so the height asked for is the face the
    // reference measures plus both of them.
    const float meterH = kMeterFaceH + lineH * 2.0f + 8.0f;
    const float meter2X = barTL.x + barW - 34.0f - kMeterW;
    const float meter1X = meter2X - 16.0f - kMeterW;
    // DROPPED ENTIRELY ON A NARROW WINDOW rather than allowed to slide left
    // into the volume dial. They are the least load-bearing things on this bar
    // - both figures are also in the status column - and the alternative is
    // two instruments overlapping a control, or two drawn off the right edge
    // where they render perfectly and are never seen.
    const bool showMeters = (barW >= kCoreW + kMeterW * 2.0f + 110.0f);
    if (showMeters) {
        const float my = barTL.y + 28.0f;

        // SAMPLE RATE: full scale 10 MS/s, which covers every device this
        // application has been run against without compressing the common
        // 2 MS/s case into the first tenth of the arc.
        const double rate = pipeline_.activeSource().sampleRateHz();
        const bool haveRate = std::isfinite(rate) && rate > 0.0;
        char rateTxt[32];
        std::snprintf(rateTxt, sizeof(rateTxt), haveRate ? "%.3f MS/s" : "--",
                      rate / 1.0e6);
        cascade::gui::drawBenchMeter(dl, ImVec2(meter1X, my), kMeterW, meterH,
                                     "SAMPLE RATE", static_cast<float>(rate / 10.0e6),
                                     haveRate, rateTxt, "MS/s");

        // FRAME TIME: the GUI's own, from ImGui's delta, against a 16.7 ms
        // budget - so full scale is "one frame's worth of 60 Hz". It is a
        // real measurement of a real thing, and it is NOT called PROCESSOR
        // because it is not the DSP load and must not be read as one.
        const float dt = ImGui::GetIO().DeltaTime;
        const bool haveDt = dt > 0.0f && dt < 1.0f;
        char dtTxt[32];
        std::snprintf(dtTxt, sizeof(dtTxt), haveDt ? "%.0f %% - %.1f ms" : "--",
                      static_cast<double>(dt) * 1000.0 / 16.7 * 100.0,
                      static_cast<double>(dt) * 1000.0);
        cascade::gui::drawBenchMeter(dl, ImVec2(meter2X, my), kMeterW, meterH,
                                     "FRAME TIME", dt * 1000.0f / 16.7f, haveDt, dtTxt,
                                     "ms");
    }

    // Beside the frequency, because the banner is ABOUT the frequency: it
    // appears when the user has tuned away from a decoder's preset and kept
    // the decoder running, and the two things it has to be read together with
    // are the readout that just changed and the volume control that is not
    // the reason there is no sound.
    //
    // It takes the bar's open middle where there is one, and the strip under
    // the counter where there is not. Both are inside the bar, and neither can
    // reach the dial or the meters - a warning that overlaps a control is a
    // warning the user cannot act on.
    {
        const float from = X(kCoreW) + 12.0f;
        const float to = showMeters ? meter1X - 16.0f : barTL.x + barW - 12.0f;
        ImVec2 at(from, Y(62.0f));
        if (to - from < 220.0f) { at = ImVec2(X(300.0f), Y(124.0f)); }
        ImGui::SetCursorScreenPos(at);
        // A zero-sized item so the SameLine drawMuteBanner opens with has a
        // line to resume: the banner lays itself out and this is the only way
        // to tell it where.
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        drawMuteBanner();
    }

    // THE RAIL ACROSS THE FOOT. A light hairline directly above a dark one,
    // and the whole of what divides this deck from the body below it - which
    // is why the root layout no longer draws an ImGui separator under the bar.
    cascade::gui::addBenchRail(dl, barTL.x + S(12.0f), barTL.x + barW - S(12.0f),
                               barTL.y + barH - 3.0f);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void AppWindow::drawFrequencyReadout(float wellX, float wellY, float scale) {
    // Fixed 10-digit field grouped in thousands ("0.100.000.000" at 100 MHz).
    // The field width is constant so digits never shift as the tuned
    // frequency changes; the zeros (and separators) ahead of the first
    // significant digit are dimmed so the eye reads only the live value.
    //
    // WHAT THE COUNTER SHOWS IS THE TUNED STATION - the source centre readback
    // PLUS the VFO offset - and the bar letters it "TUNED - HERTZ". Those two
    // used to disagree: the counter drew the bare centre, so with the VFO
    // parked 5 kHz down a live capture read 1090.000000 MHz here while the
    // waterfall's own footer read 1089.9950 MHz for the same instant. One of
    // them had to give, and it is the value that gives, for three reasons.
    //
    // FIRST, EVERYTHING ELSE IN THE APPLICATION ALREADY MEANS TUNED. The band
    // plan lookup, the bookmark this counter's neighbour saves, the scanner's
    // user-tune baseline and the status footer all read currentAbsoluteHz();
    // the counter was the only surface reading the centre, and it is the one
    // with the caption on it.
    //
    // SECOND, THE TYPED EDITOR DID NOT ROUND-TRIP. It seeds itself from the
    // figure on the drums and commits through tuneAbsoluteHz(), so with any
    // VFO offset at all, opening the editor and pressing Enter without typing
    // anything moved the radio by that offset. Seeding from the same quantity
    // the commit path applies is what makes an unchanged edit a no-op.
    //
    // THIRD, THE READBACK ARGUMENT SURVIVES THE CHANGE. The comment this
    // replaces defended the centre on the grounds that it is a readback -
    // nominal for the generator and the IQ file, the device's own answer for
    // Soapy - so the display can never disagree with the hardware. That still
    // holds: the offset is an exact number this window set itself, added to
    // that same readback, so the counter still cannot drift from what the
    // tuner actually did. What it now also cannot do is disagree with the
    // rest of the window.
    //
    // Tuning: the mouse wheel over a digit steps the TUNED frequency by that
    // digit's place value through tuneAbsoluteHz(), which commands the source
    // centre and leaves the offset where the user put it, so the digit under
    // the cursor is the digit that moves. activeSource() is a GUI/control-
    // thread call per the IqSource contract, and this IS that thread — the
    // same one that performs source swaps.
    const double hz = std::max(0.0, currentAbsoluteHz());
    // A tune may never ask the source for a negative centre, so the lowest
    // TUNED frequency the wheel can reach is the offset itself when that
    // offset is positive.
    const double minTunedHz = std::max(0.0, pipeline_.vfoOffsetHz());

    // THE WELL FIRST, because everything else in the counter sits inside it -
    // the ten apertures when the figure is being shown, the typed field when
    // it is being set. Its size comes from the cells it holds, so the bar that
    // placed it and the readout that fills it cannot disagree about where the
    // counter ends.
    ImDrawList* fdl = ImGui::GetWindowDrawList();
    const ImVec2 wtl(wellX, wellY);
    const ImVec2 wbr(wellX + kFreqWellW * scale, wellY + kFreqWellH * scale);
    cascade::gui::drawFreqDrumWell(fdl, wtl, wbr);

    // --- Typed entry (click the readout) ------------------------------------
    // Enter commits, Escape or clicking away cancels. The field is seeded in
    // MHz because that is how frequencies are spoken; parseFrequencyHz still
    // accepts Hz, kHz and GHz with an explicit suffix.
    if (freqEditing_) {
        // IN THE WELL THE DIGITS CAME OUT OF. The editor used to open wherever
        // the cursor happened to be, which on a bar laid out by position is
        // nowhere in particular; typing a frequency belongs in the counter's
        // own aperture, at the size the aperture has room for.
        ImGui::PushFont(cascade::gui::fonts::ui(),
                        std::max(14.0f, kFreqCellH * scale * 0.60f));
        const float inputH = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(
            ImVec2(wtl.x + 6.0f, wtl.y + (wbr.y - wtl.y - inputH) * 0.5f));
        ImGui::SetNextItemWidth(wbr.x - wtl.x - 12.0f);
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

    // THE COUNTER IS A ROW OF DRUMS, in the handoff's 1960s bench: amber
    // digits in machined apertures, recessed into a dark well, grouped in
    // thousands by a wider gap rather than by a printed separator - which is
    // what a mechanical counter actually does.
    //
    // The behaviour underneath is unchanged and must stay that way: the wheel
    // over a digit still steps that digit's place value, and a click still
    // opens the typed editor. Those are the reasons this readout is worth
    // having at all, and a restyle that lost them would be a downgrade
    // wearing better clothes.
    //
    // THE CELLS ARE PLACED, NOT FLOWED. Each aperture's left edge is computed
    // from the well's, so the row cannot drift with ImGui's item spacing and
    // the bar can put the whole counter wherever its own geometry says.
    //
    // WHICH DIGITS ARE DIM IS A MEASUREMENT, NOT A STYLE. The zeros ahead of
    // the first significant figure are the dark ones, because they carry no
    // value; the reference dims its three trailing digits instead, which would
    // say the Hz are somehow less real than the MHz.
    const float cellW = kFreqCellW * scale;
    const float cellH = kFreqCellH * scale;
    const float fontPx = cellH * 0.66f;
    const float cellY = wtl.y + kFreqWellPadY * scale;
    const auto cellLeft = [&](int i) {
        float x = wtl.x + kFreqWellPadX * scale;
        for (int k = 1; k <= i; ++k) {
            x += (kFreqCellW + (freqGroupBreak(k) ? kFreqGroupGap : kFreqGap)) * scale;
        }
        return x;
    };
    bool significant = false;
    bool hoveredDigit = false;
    for (int i = 0; i < kFreqCells; ++i) {
        if (digits[i] != '0') { significant = true; }
        const ImVec2 ctl(cellLeft(i), cellY);
        ImGui::SetCursorScreenPos(ctl);
        ImGui::InvisibleButton(("##fd" + std::to_string(i)).c_str(),
                               ImVec2(cellW, cellH));
        cascade::gui::drawFreqDrumCell(fdl, ctl, ImVec2(ctl.x + cellW, ctl.y + cellH),
                                       digits[i], significant, fontPx);

        // Per-digit wheel tuning. A trailing separator belongs to the digit
        // cell it follows, so hovering it tunes that digit — the natural
        // reading. Fractional wheel deltas (touchpads) below one notch still
        // step once, in the delta's direction.
        if (ImGui::IsItemHovered()) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                double ticks = static_cast<double>(static_cast<long long>(wheel));
                if (ticks == 0.0) { ticks = (wheel > 0.0f) ? 1.0 : -1.0; }
                const double next = std::max(minTunedHz, hz + ticks * kPlaceHz[i]);
                // THE SAME QUANTITY THE DRUMS SHOW. tuneAbsoluteHz commands
                // the source centre at (next - VFO offset), so the tuned
                // figure moves by exactly this digit's place value and the
                // offset the user set is left alone. Failure (a tune the
                // driver refuses) needs no handling here: the display follows
                // the readback, which won't move.
                tuneAbsoluteHz(next);
            }
            hoveredDigit = true;  // tooltip is drawn after PopFont (see below)
            // SINGLE click opens the editor. Double-click was tried first and
            // is a trap here: the digits are Text items, and a synthetic or
            // fast double-click can collapse into one registered click, so it
            // silently did nothing. Single click also matches what SDR++
            // does, and wheel tuning is unaffected because that needs only
            // hover, never a click.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // Seeded from the TUNED figure the drums are showing, which is
                // the quantity the commit above applies - so opening the
                // editor and pressing Enter unchanged tunes nowhere.
                std::snprintf(freqEditBuf_, sizeof(freqEditBuf_), "%.6f", hz / 1.0e6);
                freqEditing_ = true;
                freqEditFocus_ = true;
                freqEditWasActive_ = false;
            }
        }
    }

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
    // THE RAIL IS A PLATE, not a column of headers. addBenchPlate lays the
    // ground, the bevel, the engraved title and the rule under it, and hands
    // back the y beneath that rule - so the sections start from a measurement
    // rather than from a guess at how tall a title is.
    constexpr float kPlatePad = 8.0f;
    ImDrawList* colDl = ImGui::GetWindowDrawList();
    const ImVec2 colTL = ImGui::GetWindowPos();
    const ImVec2 colSize = ImGui::GetWindowSize();
    float bodyTop = cascade::gui::addBenchPlate(
        colDl, colTL, ImVec2(colTL.x + colSize.x, colTL.y + colSize.y), "FUNCTION SELECT");
    // THE FIVE BANK KEYS, under the title and above the sections - the
    // function selector a 1960s bench actually has: a row of pushbuttons, one
    // lit. See gui/rail_banks.hpp for why the rail stopped being one list.
    bodyTop = drawRailBankKeys(colTL.x, colTL.y, colSize.x, bodyTop);

    // The sections scroll inside an inner child sized to leave room for the
    // status footer, so the footer stays pinned to the bottom of the column
    // regardless of how many sections are open.
    const float footerHeight = 2.0f * ImGui::GetTextLineHeightWithSpacing() +
                               ImGui::GetStyle().ItemSpacing.y + 4.0f;
    ImGui::SetCursorScreenPos(ImVec2(colTL.x + kPlatePad, bodyTop));
    // Transparent, so the plate's own ground carries the whole rail: the
    // theme's ChildBg is a 55% well tint, and a second one laid inside the
    // plate would draw a box around the sections that the reference does not
    // have.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##menu_sections",
                      ImVec2(colSize.x - kPlatePad * 2.0f, -(footerHeight + kPlatePad)),
                      ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    drawUpdateBanner();

    // ONE BANK AT A TIME. The five captions the rail always carried are now
    // the five keys drawn above this child (see drawRailBankKeys and
    // gui/rail_banks.hpp for why), and the column shows the sections of the
    // selected bank and nothing else. Every section keeps its own
    // open/closed state across a bank change - the state is ImGui's, keyed
    // on the row's id, and the id does not change with the bank.
    switch (cascade::gui::railBankFromIndex(railBank_)) {
        case cascade::gui::RailBank::SignalPath:
            // --- SIGNAL PATH: what the samples pass through, in the order
            // they do. The recorder is here rather than under DECODE because
            // it captures the signal path's own product - raw I/Q or the
            // demodulated audio - and has nothing to do with decoding.
            benchGroup("SIGNAL PATH");
            drawSourceSection();
            drawRadioSection();
            drawAudioFilterSection();
            drawSinksSection();
            drawRecorderSection();
            break;
        case cascade::gui::RailBank::Decode:
            drawDecodeBank();
            break;
        case cascade::gui::RailBank::View:
            // --- VIEW: how what was decoded is shown. The radar scope is a
            // way of LOOKING at the traffic the decoders produce, which is
            // why it sits beside Display rather than among the decoders.
            benchGroup("VIEW");
            drawDisplaySection();
            drawRadarSection();
            drawBookmarksSection();
            drawScannerSection();
            break;
        case cascade::gui::RailBank::Extend:
            benchGroup("EXTEND");
            drawWebSection();
            drawCatSection();
            break;
        case cascade::gui::RailBank::System:
            benchGroup("SYSTEM");
            drawUpdatesSection();
            drawDiagnosticsSection();
            drawUsageReportingSection();
            break;
    }
    // The last section's drawer, if it is mid-motion, is closed here rather
    // than by a next row that does not exist.
    benchRailFlush();
    drawRailBankCurtain();
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

// The Radio section of the rail: mode keys, VFO, bandwidth, squelch,
// de-emphasis, the WFM stereo/RDS controls and the S-meter. Moved out of
// drawMenuColumn unchanged when the rail became five banks (see
// gui/rail_banks.hpp): the column dispatches a bank, and a bank is a list
// of section calls, so an inline section would have been the one thing
// in the list that was not a call.
void AppWindow::drawRadioSection() {
    // kModeNames is the one table the mode buttons and this chip both read,
    // so the rail cannot claim a mode the buttons do not show.
    const bool radioOpen = benchSection("Radio", true, kModeNames[modeIndex_],
                                        cascade::gui::theme::kPhosphor,
                                        pipeline_.running());
    if (radioOpen) {
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
}

// The Sinks section: the output device and why there is no sound. Moved out
// of drawMenuColumn unchanged, for the reason drawRadioSection gives.
void AppWindow::drawSinksSection() {
    // MUTED is the state worth seeing from the rail without opening it -
        // "why is there no sound" is the question this chip answers - but it
        // is not the only way for the sound to stop, and the lamp beside it
        // used to be the literal `true`. A lamp wired on reports nothing; this
        // one now reports what the SINK card in the status column reports,
        // from the same two predicates, so the rail and the card cannot
        // contradict each other:
        //
        //   everOpened()   an output device was opened at all
        //   streamAlive()  Pa_IsStreamActive - the case the audio watchdog
        //                  exists for, where everything upstream stays healthy
        //                  and the speakers go quiet because the host API took
        //                  the stream away (0.57.0's "the radio keeps going
        //                  silent")
        //
        // THE LAMP'S COLOUR SAYS WHICH STATE AND ITS LIT-NESS SAYS THERE IS
        // ONE, which is the convention the rest of the rail already keeps
        // (rust and lit while the recorder is taping, phosphor and lit while
        // the web server listens). So: phosphor while audio is leaving the
        // application, amber while a plugin holds the mute, rust while the
        // stream is dead, and dark only when no output device has ever been
        // opened - which is the one case where there is nothing to report.
    const bool sinkMuted = !muteSubjectText().empty();
    const bool sinkOpened = pipeline_.audio().everOpened();
    const bool sinkAlive = sinkOpened && pipeline_.audio().streamAlive();
    const char* sinkChip = "ON";
    ImU32 sinkLamp = cascade::gui::theme::kPhosphor;
    if (!sinkOpened) {
        sinkChip = "NO DEV";
        sinkLamp = cascade::gui::theme::kInkFaint;
    } else if (!sinkAlive) {
        // Rust, the same as the card: this is a fault, not a setting.
        sinkChip = "DEAD";
        sinkLamp = cascade::gui::theme::kAlarm;
    } else if (sinkMuted) {
        sinkChip = "MUTED";
        sinkLamp = cascade::gui::theme::kAmber;
    }
    const bool sinksOpen = benchSection("Sinks", true, sinkChip, sinkLamp, sinkOpened);
    if (sinksOpen) {
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
            ImGui::TextColored(cascade::gui::theme::warning(), "%s",
                               audioHealthNote_.c_str());
        }
        // WHY THERE IS NO SOUND, said where the sound settings are. This is the
        // panel a user goes to when the speakers are silent, and finding a
        // working device, a healthy watchdog and a volume that is not zero
        // there - with no explanation - is exactly the dead end the 0.58.0
        // audio investigation started from. Wrapped, because the menu column
        // is narrow and a plugin name is the plugin's to choose.
        if (!mutedBy_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
            ImGui::TextWrapped("Muted by %s", muteSubjectText().c_str());
            ImGui::PopStyleColor();
            // Said separately rather than folded into the line above, because
            // the two halves answer different questions: what is doing it, and
            // what makes it stop. The volume is untouched throughout, which is
            // the one thing a user staring at this panel will assume first.
            ImGui::TextDisabled("sound returns when the plugin stops");
        }
    }
}

// The Display section: the shared dB window and the band-plan overlay.
// Moved out of drawMenuColumn unchanged, for the reason drawRadioSection
// gives.
void AppWindow::drawDisplaySection() {
    // THE BAND PLAN OVERLAY IS THE ONLY THING IN THIS SECTION THAT IS EITHER
    // ON OR OFF, so it is what the chip reports and the comment says so rather
    // than the chip implying it speaks for the whole of Display. The dB window
    // beside it is a range, not a state, and a two-ended range does not fit a
    // chip without being rounded into something that is no longer the setting.
    //
    // The lamp is the overlay ACTUALLY DRAWING - asked for, no load error, and
    // a plan with bands in it - so "PLAN" with the lamp out is the honest
    // reading of "you switched it on and there is nothing installed".
    const bool planDrawing = bandPlanOverlay_ && bandPlanError_.empty() &&
                             !bandPlan_.entries().empty();
    if (benchSection("Display", true, bandPlanOverlay_ ? "PLAN" : "PLAIN",
                     cascade::gui::theme::kPhosphor, planDrawing)) {
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
}

// The DECODE bank of the rail, in the reading order its comment explains.
// The radar scope and the recorder used to be listed here too; they moved
// to VIEW and SIGNAL PATH when the rail became five banks, because a bank
// is chosen by what a control IS, and a scope is a view while a recorder
// is part of the signal path.
void AppWindow::drawDecodeBank() {
    // --- DECODE: what is made of the samples, and what is kept of it --------
    //
    // THE ORDER STILL MATTERS, AND ONE OF ITS TWO REASONS HAS MOVED. The store
    // and the inventory are WINDOWS now, drawn from drawPluginWindows before
    // this rail exists at all - so the "store before inventory" rule that
    // pluginBrowserDrawnThisFrame_ needs is enforced there, by the order of
    // those two calls, and no longer by the order of these two rows. What
    // remains here is the reading order of a rail: the two keys that open
    // those windows, then the decoder controls neither window carries, then
    // TARGET DETAILS, which is drawn after them because the targets it
    // describes come from the modules above it.
    benchGroup("DECODE");
    drawPluginStoreSection();
    drawPluginsSection();
    drawDecodersSection();
    drawTargetDetailsSection();
    // THE SATELLITES MAP'S ONLY PRESENCE OUT HERE. A switch, not a section:
    // the window it opens carries every satellite control there is, and the
    // design's own note in the corner of the mock says as much - it "reopens
    // from the Windows menu and from its own rocker in DECODE". There is no
    // Windows menu in this application, so this rocker is the whole of it, and
    // it is drawn after the plugin inventory because the pages it switches are
    // created from what that inventory loaded.
    drawSatelliteMapSection();
    // THE PLUGIN WINDOWS' ONLY PRESENCE OUT HERE, and their only way onto the
    // screen: one row per picture or panel a plugin publishes (0.79.1).
    drawPluginWindowRows();
}

// --- THE BANK KEYS -----------------------------------------------------------
//
// One pushbutton of the function selector: a lettered brass key that stays
// PRESSED while its bank is showing, with a phosphor strip lit under it - the
// lamp a 1960s selector puts beside the button that is in. benchWordKey draws
// the momentary version of the same part; this one has a latched state, and
// the two are kept as two functions because a latched key that pops back up
// under the hand is the one thing a selector must never do.
//
// A real ImGui item, so the keys take part in the same input arbitration as
// every other control and can be reached with Tab; the F-keys are handled by
// the caller, once for the row, because they are not a property of a key.
static bool benchBankKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                         const char* label, bool on, const char* tooltip, int index) {
    if (dl == nullptr || br.x - tl.x < 12.0f || br.y - tl.y < 8.0f) { return false; }
    ImGui::PushID(index);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##bankkey", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    if (hovered && tooltip != nullptr) { ImGui::SetTooltip("%s  (F%d)", tooltip, index + 1); }
    ImGui::PopID();

    const float r = cascade::gui::theme::kKeyRounding;
    const bool down = on || held;
    // Proud metal casts a shadow and pressed metal does not, which is the
    // state indication before any colour is used - the same rule as
    // benchWordKey, so the two parts read as one family.
    if (!down) {
        dl->AddRectFilled(ImVec2(tl.x + 1.0f, tl.y + 2.0f), ImVec2(br.x + 1.0f, br.y + 2.0f),
                          cascade::gui::theme::withAlpha(cascade::gui::theme::kVoid, 0.45f), r);
    }
    const ImU32 top = down      ? cascade::gui::theme::kBrassMid
                      : hovered ? cascade::gui::theme::kIvory
                                : cascade::gui::theme::kCream;
    const ImU32 bot = down ? cascade::gui::theme::kBrassDark : cascade::gui::theme::kBrassBright;
    dl->AddRectFilled(tl, br, bot, r);
    if (br.x - tl.x > r * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y), top, top,
                                    bot, bot);
    }
    cascade::gui::addBenchBevel(dl, tl, br, r, !down);
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    cascade::gui::theme::kBrassBright, r + 1.0f, 0,
                    cascade::gui::theme::kHairline);
    }
    // THE LAMP STRIP: lit phosphor under the key that is in, dark glass under
    // the others - so which bank is showing can be read from across the room,
    // and read in the display's own colour, because "this is what is on" is
    // closer to a reading than to a control.
    {
        const ImVec2 sTL(tl.x + 3.0f, br.y + 3.0f);
        const ImVec2 sBR(br.x - 3.0f, br.y + 6.0f);
        dl->AddRectFilled(sTL, sBR, cascade::gui::theme::kWell, 1.0f);
        if (on) {
            dl->AddRectFilled(ImVec2(sTL.x - 1.0f, sTL.y - 1.0f),
                              ImVec2(sBR.x + 1.0f, sBR.y + 1.0f),
                              cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphor, 0.25f),
                              2.0f);
            dl->AddRectFilled(sTL, sBR, cascade::gui::theme::kPhosphor, 1.0f);
        }
    }
    // The word, cut into the brass: ink on metal, never amber.
    ImFont* f = cascade::gui::fonts::legend();
    const float px = cascade::gui::fonts::kTinySize;
    const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0.0f, label);
    dl->AddText(f, px,
                ImVec2((tl.x + br.x) * 0.5f - ts.x * 0.5f,
                       (tl.y + br.y) * 0.5f - ts.y * 0.5f + (down ? 1.0f : 0.0f)),
                on ? cascade::gui::theme::kIvory : cascade::gui::theme::kEnamel, label);
    return pressed;
}

float AppWindow::drawRailBankKeys(float colX, float colY, float colW, float bodyTop) {
    (void)colY;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float kPad = 8.0f;    // the plate's own inset, as the sections use
    constexpr float kGap = 4.0f;
    constexpr float kStrip = 7.0f;  // the lamp strip and its gap, below the key
    const float keyH = std::max(22.0f, cascade::gui::fonts::kTinySize + 9.0f);
    const float x0 = colX + kPad;
    const float x1 = colX + colW - kPad;
    const float keyW = (x1 - x0 - kGap * static_cast<float>(cascade::gui::kRailBankCount - 1)) /
                       static_cast<float>(cascade::gui::kRailBankCount);
    if (keyW < 24.0f || dl == nullptr) { return bodyTop; }

    // THE KEYBOARD'S ROW OF FUNCTION KEYS IS THE SAME ROW, F1 to F5 left to
    // right - not while a field is being typed in, where a function key may
    // mean something to the field.
    int selected = -1;
    if (!ImGui::GetIO().WantTextInput) {
        for (int i = 0; i < cascade::gui::kRailBankCount; ++i) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1 + i), false)) {
                selected = cascade::gui::railBankForFunctionKey(i);
            }
        }
    }
    for (int i = 0; i < cascade::gui::kRailBankCount; ++i) {
        const cascade::gui::RailBank b = cascade::gui::railBankFromIndex(i);
        const ImVec2 tl(x0 + static_cast<float>(i) * (keyW + kGap), bodyTop);
        const ImVec2 br(tl.x + keyW, bodyTop + keyH);
        if (benchBankKey(dl, tl, br, cascade::gui::railBankLabel(b), railBank_ == i,
                         cascade::gui::railBankCaption(b), i)) {
            selected = i;
        }
    }
    if (selected >= 0 && selected != railBank_) {
        railBank_ = selected;
        // The new bank comes up rather than appearing - see drawRailBankCurtain.
        railBankFade_ = 0.0f;
    }
    // The cursor is left where the sections start, and the caller lays them
    // from the y handed back.
    const float below = bodyTop + keyH + kStrip + 6.0f;
    ImGui::SetCursorScreenPos(ImVec2(x0, below));
    return below;
}

void AppWindow::drawRailBankCurtain() {
    // Drawn INSIDE the sections child, at its end, so it lies over everything
    // the bank drew - the hand-drawn plates as much as the widgets - and is
    // clipped to the child. It is the plate's own ground (the same enamel
    // gradient addBenchPlate lays), fading out over kRailBankFadeSeconds: a
    // bank that comes up like a lamp rather than a column that changes in one
    // frame. Nothing is drawn once the fade is done, which is nearly always.
    if (railBankFade_ >= 1.0f) { return; }
    railBankFade_ = cascade::gui::railDrawerAdvance(railBankFade_, true, ImGui::GetIO().DeltaTime,
                                                    cascade::gui::kRailBankFadeSeconds);
    const float a = 1.0f - cascade::gui::railEase(railBankFade_);
    if (a <= 0.0f) { return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tl = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    const ImVec2 br(tl.x + sz.x, tl.y + sz.y);
    dl->AddRectFilledMultiColor(tl, br,
                                cascade::gui::theme::withAlpha(cascade::gui::theme::kEnamel, a),
                                cascade::gui::theme::withAlpha(cascade::gui::theme::kEnamel, a),
                                cascade::gui::theme::withAlpha(cascade::gui::theme::kEnamelDark, a),
                                cascade::gui::theme::withAlpha(cascade::gui::theme::kEnamelDark, a));
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
    const ImVec4 accent = update_.critical ? kErrorRed : cascade::gui::theme::warning();
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
        ImGui::TextColored(cascade::gui::theme::warning(), "Downloading...");
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
    // The device in use, shortened to what fits: "SoapySDR: B200" is the
    // full name and "B200" is the part that identifies it.
    std::string sourceChip = pipeline_.activeSource().name();
    const std::size_t colon = sourceChip.rfind(": ");
    if (colon != std::string::npos) { sourceChip = sourceChip.substr(colon + 2); }
    if (sourceChip.size() > 10) { sourceChip = sourceChip.substr(0, 10); }
    // Rust and lit while the pipeline is faulted, phosphor and lit while it
    // runs: the colour says which state and the lamp says there is one, which
    // is the rule the whole rail keeps.
    const bool sourceFaulted = pipeline_.faulted();
    const bool sourceOpen =
        benchSection("Source", true, sourceChip.c_str(),
                     sourceFaulted ? cascade::gui::theme::kAlarm
                                   : cascade::gui::theme::kPhosphor,
                     sourceFaulted || pipeline_.running());
    if (!sourceOpen) { return; }

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
        ImGui::TextColored(cascade::gui::theme::warning(), "%s",
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
        ImGui::TextColored(cascade::gui::theme::warning(), "No radio hardware found");
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
        // THE MODULE TABLE, REBUILT BECAUSE THE OPEN LOADED CODE.
        //
        // SoapySDR::Device::make() maps the vendor module and everything it
        // pulls in - rtlsdrSupport.dll, rtlsdr.dll, libusb - into THIS
        // process, on the worker that has just finished. The table a report
        // resolves addresses against was last built at start-up, so until now
        // every one of those modules resolved to a bare address: a field
        // report whose radio was opened 102 s after launch named the fault in
        // hex, and the frame that identified it had to be recovered by hand.
        // diag_report.hpp has always said "after a device is opened"; this is
        // the call that makes that true.
        //
        // BEFORE finishSoapyOpen, not after, because that function returns
        // early on a failed open and on an open the user has moved on from -
        // and both of those loaded the vendor module just the same. Rebuilt
        // once per open, which is a user action, not per frame.
        cascade::core::refreshModuleTable();
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

    // Failure: the reason lands in red under the control. The combo settles
    // on what is actually installed — since the close-first ordering in
    // selectSource, a failed device open leaves the GENERATOR running (the
    // old radio was closed before the attempt), so a selection still pointing
    // at a device row would be a readout disagreeing with the hardware.
    if (!r.dev) {
        sourceError_ = r.error.empty() ? "device open failed" : r.error;
        if (soapy_ == nullptr && sourceKind_ == "siggen" && sourceSel_ != 1) {
            sourceSel_ = 0;
        }
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
        // A tune the coalescer was still holding was aimed at the OLD source;
        // the carry-across below supersedes it. Applied unpaced, because the
        // readback two lines down must be valid on return.
        retuneCoalescer_.clearPending();
        applyRetuneNow(r.keepCenterHz);
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

    // BUSY CHECK ON EVERY ROW, not just the device rows (adjudicated fix #4
    // for the 0.62.0 field crashes: selectSource(0) and the web route lacked
    // the check the idx>=2 path had). While a device open is resolving on its
    // worker, no source switch of any kind is accepted — switching to the
    // generator mid-open used to strand the resolving device for a stale-drop
    // teardown, and the combo's busy label already tells the user why the
    // click did nothing.
    if (soapyOpenPending_) { return; }
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
    if (soapyScanPending_) { return; }  // one at a time (open is checked above)

    // The open runs on a worker: Device::make() is the multi-second, USB-bus
    // -walking call that used to freeze the GUI here. sourceSel_ is left
    // alone until it resolves; on failure finishSoapyOpen settles the combo
    // on whatever is actually installed.
    const std::string args = soapyDevices_[d].args;
    const double rate = kSoapyRateHz[kSoapyRateDefaultIndex];
    // Read the tuned frequency NOW: the old source is closed below, and a
    // device that has never been opened reports 0, which finishSoapyOpen
    // treats as "nothing to carry".
    const double keepCenterHz = pipeline_.activeSource().centerFrequencyHz();

    // CLOSE THE OLD RADIO BEFORE OPENING THE NEW ONE (adjudicated fix #3 for
    // the 0.62.0 field crashes). The previous flow opened the new device on
    // the worker while the old one was still open and streaming, then unmade
    // the old one afterwards — two device lifetimes overlapping in one
    // process's libusb, which is the lifecycle overlap fingerprinted as the
    // corrupting event. Now: quiesce and destroy the old device HERE, on this
    // thread, with the source thread joined (setSource does both), and only
    // then let the worker call Device::make. The cost is honest: if the new
    // device fails to open, the receiver is on the generator with the reason
    // shown, not silently back on a radio it had to close to try.
    if (soapy_ != nullptr) {
        cascade::core::diagLogf(
            "source: closing %s before opening another device",
            cascade::core::sanitiseDevice(soapyArgs_).c_str());
        soapy_ = nullptr;
        soapyArgs_.clear();
        ++sourceGen_;
        pipeline_.setSource(nullptr);
        sourceKind_ = "siggen";
        // The DSP chain must follow the source that is actually installed —
        // if the open below fails, the generator would otherwise keep running
        // at the closed radio's rate.
        followInputRate();
    }
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
    const double nowS = ImGui::GetTime();
    if (pipeline_.getLatestFrame(lastFrame_)) {
        // One waterfall line per *new* frame (not per GUI frame): duplicate
        // lines would fake scroll speed while the pipeline is stalled.
        waterfall_->addLine(lastFrame_.dbBins.data(),
                            static_cast<int>(lastFrame_.dbBins.size()), dbMin_, dbMax_);
        ++waterfallLines_;
        // WHEN THIS FIGURE WAS TAKEN, which is what the spectrum's "HEARD n s
        // AGO" line reports. SpectrumFrame carries a sequence number and no
        // timestamp, so the closest honest measurement available is the moment
        // the GUI took DELIVERY of the frame - later than the DSP published it
        // by at most one GUI frame (~16 ms at 60 Hz), and the line is printed
        // to a tenth of a second. Named for what it measures accordingly.
        lastFrameSeenS_ = nowS;
    }

    // THE WATERFALL'S SCROLL RATE, MEASURED RATHER THAN ASSUMED, AND MEASURED
    // ON THE RIGHT QUANTITY. It counts the lines actually pushed into the ring
    // by the loop above - not the frames the DSP published, which is a much
    // larger and completely different number: getLatestFrame hands over at most
    // one frame per call, so at 2 MS/s with a 1024-point FFT the pipeline
    // publishes about 1950 frames a second and this poll takes one of them per
    // GUI frame. Measured from the sequence, the strip captioned a picture
    // scrolling at sixty lines a second "SCROLL 1938 line/s" and called its
    // whole visible history "0s VISIBLE".
    //
    // It is still not a count of GUI frames: a window in which the pipeline
    // published nothing sees every poll fail, pushes no line, and closes on a
    // difference of zero - which WaterfallView::Chrome reads as "not measured"
    // and answers by removing the elapsed-time strip and the scroll line
    // altogether, rather than scrolling labels over a picture that has stopped.
    //
    // A window LONGER than two seconds is thrown away rather than averaged: it
    // means this panel was not being drawn at all (scope mode owns the whole
    // window while it is on, and the pipeline keeps publishing underneath it),
    // so dividing the lines pushed in that time by a stretch nothing here
    // watched would answer a question that was not asked.
    if (frameRateWindowS_ < 0.0 || nowS - frameRateWindowS_ > 2.0) {
        frameRateWindowS_ = nowS;
        waterfallLinesAtWindow_ = waterfallLines_;
    } else if (nowS - frameRateWindowS_ >= 1.0) {
        const std::uint64_t pushed = waterfallLines_ - waterfallLinesAtWindow_;
        framesPerSecond_ = static_cast<float>(static_cast<double>(pushed) /
                                              (nowS - frameRateWindowS_));
        frameRateWindowS_ = nowS;
        waterfallLinesAtWindow_ = waterfallLines_;
    }

    // The frequency scale follows the tuned center (active source readback)
    // and the DSP input rate every frame; setSpan preserves the user's zoom
    // window whenever it still fits the new baseband, so a small retune or a
    // rate change does not silently throw the view away.
    cascade::source::IqSource& src = pipeline_.activeSource();
    scale_.setSpan(src.centerFrequencyHz(), pipeline_.inputRateHz());

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterThickness = 6.0f;
    // NO TICK STRIP TO RESERVE ANY MORE: the frequency axis is lettered inside
    // the spectrum's own well (SpectrumView::Chrome::freqTicks), so the two
    // panels and the splitter own the whole region between them.
    const float usable = avail.y - splitterThickness;
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

    // THE AXIS IS COMPUTED BEFORE THE PANEL IS DRAWN, because the panel now
    // letters it: SpectrumView draws the frequency scale along the foot of its
    // own well from the ticks handed to it in Chrome, and the same ticks drive
    // the vertical gridlines painted over the trace afterwards. One table, so
    // a gridline and the number under it cannot disagree.
    double tickHz[kMaxTicks];
    char tickLabels[kMaxTicks][16];
    const int tickCount = scale_.ticks(static_cast<double>(width), tickHz,
                                       tickLabels, kMaxTicks);
    SpectrumView::AxisTick axisTicks[kMaxTicks];
    for (int i = 0; i < tickCount; ++i) {
        axisTicks[i].xFrac = static_cast<float>(scale_.hzToX(tickHz[i]));
        axisTicks[i].label = tickLabels[i];
    }

    // --- VFO band: the drag first, then the panel, then the overlay ---------
    // Band edges in absolute Hz -> panel-width fractions via the shared
    // scale. vfoBandwidthHz_ is the REQUESTED bandwidth (the Vfo clamps its
    // filter internally; the overlay shows what the user asked for).
    //
    // AN IN-PROGRESS DRAG IS SETTLED BEFORE THE SPECTRUM IS DRAWN, and that
    // ordering is load-bearing rather than tidy. The panel's "PEAK IN
    // PASSBAND" figure is a maximum over exactly the bins this band covers, so
    // the band handed to drawBinRange has to be the same one drawVfoOverlay
    // paints a few lines below; measuring over the pre-drag band and drawing
    // the post-drag one would put a figure on the panel that belongs to a
    // passband nothing on screen shows. Only the drag's CONTINUATION moves
    // here - starting one needs the item's hover state, which does not exist
    // until the panel has been submitted, and it stays below.
    double bandCenterAbs = src.centerFrequencyHz() + pipeline_.vfoOffsetHz();
    SpectrumView::VfoBand band;
    band.x0Frac = scale_.hzToX(bandCenterAbs - 0.5 * vfoBandwidthHz_);
    band.x1Frac = scale_.hzToX(bandCenterAbs + 0.5 * vfoBandwidthHz_);
    band.dragging = (vfoDrag_ != VfoDrag::None);

    const double mouseFrac =
        static_cast<double>(io.MousePos.x - specPos.x) / static_cast<double>(width);
    if (vfoDrag_ != VfoDrag::None) {
        if (!ImGui::IsMouseDown(0)) {
            vfoDrag_ = VfoDrag::None;
            band.dragging = false;
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

    // WHAT THE PANEL IS ALLOWED TO SAY ABOUT ITSELF, and every field of it
    // comes from something this application measures or configures:
    //
    //   emaAlpha    kAveragingAlpha - the SAME constant the constructor hands
    //               Pipeline::Config, so the caveat under the header describes
    //               the estimator that actually drew the trace. NOT the
    //               reference artboard's "AVG 4": no boxcar depth exists here,
    //               and the two usual conversions from an EMA weight disagree.
    //   passband    the band settled immediately above and painted by
    //               drawVfoOverlay immediately below.
    //   dataAgeSec  how long ago the GUI took delivery of this frame; negative
    //               (the initial value) before any frame has arrived, which is
    //               what suppresses the age line rather than printing "0.0 s".
    //   freqTicks   FreqScale's own ticks and its own labels - this file never
    //               formats a second frequency string of its own.
    //   spanHz      the view window's width, from the same scale.
    //
    // There is deliberately no title override: the widget's default names it
    // SPECTRUM and appends the bin count it was actually handed.
    SpectrumView::Chrome chrome;
    chrome.emaAlpha = kAveragingAlpha;
    chrome.passband = &band;
    chrome.dataAgeSec = (lastFrameSeenS_ >= 0.0) ? (nowS - lastFrameSeenS_) : -1.0;
    chrome.freqTicks = (tickCount > 0) ? axisTicks : nullptr;
    chrome.freqTickCount = tickCount;
    chrome.spanHz = scale_.viewHighHz() - scale_.viewLowHz();

    // Before the first frame lastFrame_.dbBins is empty; SpectrumView renders
    // the background + grid for null bins, which is the wanted idle look.
    const float* bins = lastFrame_.dbBins.empty() ? nullptr : lastFrame_.dbBins.data();
    spectrum_->drawBinRange(bins, static_cast<int>(lastFrame_.dbBins.size()),
                            firstBin, lastBin, width, spectrumHeight, &chrome);
    const bool specHovered = ImGui::IsItemHovered();

    // Band plan behind the trace (see kBandFillAlphaScale for why "behind"
    // is achieved with a translucent fill painted after it). Before the
    // gridlines and the VFO overlay so those stay the topmost furniture.
    if (bandPlanOverlay_) {
        drawBandPlanOverlay(specPos.x, specPos.y, width, spectrumHeight);
    }

    for (int i = 0; i < tickCount; ++i) {
        // ticks() only returns in-view frequencies, so x stays in-panel.
        const float x = specPos.x + axisTicks[i].xFrac * width;
        drawList->AddLine(ImVec2(x, specPos.y), ImVec2(x, specPos.y + spectrumHeight),
                          kTickGridColor);
    }

    // STARTING a drag, which is the half that needs the hover state and so
    // could not move above the panel with the continuation.
    if (vfoDrag_ == VfoDrag::None && specHovered) {
        const auto hit = SpectrumView::hitTest(
            mouseFrac, band, static_cast<double>(kVfoEdgeTolPx) / width);
        if (hit == SpectrumView::VfoHit::EdgeLow ||
            hit == SpectrumView::VfoHit::EdgeHigh) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else if (hit == SpectrumView::VfoHit::Center) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        // THE GESTURES, SAID ONCE THE HAND HAS RESTED. Every way this panel is
        // tuned - drag the band, drag its edge, click, wheel, double-click -
        // was discoverable only by accident; the digit wheel three lines up
        // the window has had a tooltip from the start. Shown after ImGui's
        // normal hover delay and only while nothing is being dragged, so it
        // never sits under a hand that is already working.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay) &&
            !ImGui::IsMouseDown(0)) {
            if (hit == SpectrumView::VfoHit::Center) {
                ImGui::SetTooltip("Drag to move the tuned band | drag an edge to widen it\n"
                                  "Shift-drag: free, unsnapped | wheel: zoom | double-click: unzoom");
            } else if (hit == SpectrumView::VfoHit::EdgeLow ||
                       hit == SpectrumView::VfoHit::EdgeHigh) {
                ImGui::SetTooltip("Drag to widen or narrow the tuned band");
            } else {
                ImGui::SetTooltip("Click to tune here | wheel: zoom about the pointer\n"
                                  "double-click: unzoom | drag the shaded band to move it");
            }
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
    spectrum_->drawVfoOverlay(band, width, spectrumHeight);

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

    // WHAT THE PICTURE CANNOT KNOW ABOUT ITSELF, supplied from the two things
    // that do: the measured publish rate above, and the receiver's own state.
    //
    // The foot line is assembled ONLY from readbacks. The frequency is the
    // same one the counter shows (source centre + VFO offset); the decoder
    // named is one the runner reports as actually RUNNING, which is a
    // different question from "installed" and from "not stopped" - a plugin
    // whose rate the receiver is not producing is idle, and naming it here
    // would caption the picture with a decode that is not happening.
    //
    // AND A STOPPED RECEIVER IS NOT DECODING WHATEVER THE RUNNER SAYS. Its
    // instances exist and are matched to the rate - which is what the runner
    // reports - but the DSP threads are not turning, so nothing is being handed
    // to them. The first render of this line read "100.3000 MHz ADS-B +1 -
    // DECODING" under a picture that had not moved since Stop; the run state is
    // therefore checked first and wins. With no tuned frequency at all (an
    // inert scale, before any source has opened) the line is left empty, which
    // WaterfallView reads as "draw no line".
    WaterfallView::Chrome wfChrome;
    wfChrome.linesPerSecond = framesPerSecond_;
    char decodeLine[192] = "";
    if (std::isfinite(bandCenterAbs) && bandCenterAbs > 0.0) {
        std::string firstDecoder;
        int runningDecoders = 0;
        if (pipeline_.running()) {
            for (const cascade::core::DecoderStatus& s : pluginRunner_.status()) {
                if (s.reason != cascade::core::DecoderIdleReason::Running) { continue; }
                if (runningDecoders == 0) { firstDecoder = s.plugin; }
                ++runningDecoders;
            }
        }
        char freqTxt[32];
        std::snprintf(freqTxt, sizeof(freqTxt), "%.4f MHz", bandCenterAbs / 1.0e6);
        if (runningDecoders == 1) {
            std::snprintf(decodeLine, sizeof(decodeLine), "%s %s - DECODING", freqTxt,
                          firstDecoder.c_str());
        } else if (runningDecoders > 1) {
            std::snprintf(decodeLine, sizeof(decodeLine), "%s %s +%d - DECODING", freqTxt,
                          firstDecoder.c_str(), runningDecoders - 1);
        } else {
            std::snprintf(decodeLine, sizeof(decodeLine), "%s %s - %s", freqTxt,
                          kModeNames[modeIndex_],
                          pipeline_.running() ? "RECEIVING" : "STOPPED");
        }
    }
    wfChrome.decoding = (decodeLine[0] != '\0') ? decodeLine : nullptr;
    waterfall_->draw(width, waterfallHeight, u0, u1, wfChrome);
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

// AppWindow::drawFreqAxis USED TO LIVE HERE - an eighteen-pixel strip of its
// own between the spectrum and the waterfall, carrying the frequency scale.
// SpectrumView now letters that scale inside its own well, from the ticks
// drawCenterPanels hands it, which is where the reference face draws it. The
// strip is not kept as well: two renderings of one scale, eighteen pixels
// apart, is a disagreement waiting for whichever of them is edited next.

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
        ImGui::TextColored(cascade::gui::theme::good(), "ST");
    } else if (locked) {
        ImGui::TextColored(cascade::gui::theme::warning(), "ST (forced mono)");
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
    // HOW MANY OF THE THREE ARE IN THE CHAIN, which is the only thing this
    // section has that is either on or off. The three switches below are what
    // the count is taken from, so the chip cannot claim a filter the panel
    // does not show engaged.
    const int filtersOn = (nrEnabled_ ? 1 : 0) + (notchEnabled_ ? 1 : 0) +
                          (autoNotch_ ? 1 : 0);
    char filterChip[16];
    if (filtersOn == 0) {
        std::snprintf(filterChip, sizeof(filterChip), "OFF");
    } else {
        std::snprintf(filterChip, sizeof(filterChip), "%d ON", filtersOn);
    }
    if (!benchSection("Audio filters", false, filterChip,
                      cascade::gui::theme::kPhosphor, filtersOn > 0)) {
        return;
    }
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
            ImGui::TextColored(cascade::gui::theme::good(), "on %.0f Hz",
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

    // The plugin windows the user has OPEN ride through the rescan: their
    // identities are the plugin's name and its window's title, so a plugin
    // that survives the rescan keeps its window on screen and one that does
    // not simply has no window to show. (Until 0.79.1 this cleared the set
    // of CLOSED windows, which was then the only way to get one back; a
    // window now opens from its own row on the rail instead.)

    // The map pages are NOT cleared. The first version cleared them here,
    // and an adversarial review measured the cost: every MapView died, so a
    // rescan — which also fires from the plugin store's async catalogue fetch
    // and from installs, seconds after an unrelated click — silently reset
    // zoom, centre, selection and an active follow. Geometry is folded into
    // the saved store as a belt (a page whose plugin vanishes is pruned in
    // drawPluginWindows, and that prune re-syncs first), and the page
    // objects and their views all ride through the rescan untouched. A
    // plugin that reappears finds its page exactly
    // where it was.
    syncMapPagesToSaved();

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
    // catalog_ is only ever filled by CHECK NOW in the plugin store window.
    return cascade::core::PluginRepo::planUpdates(catalog_, pluginInventory_.plugins);
}

void AppWindow::drawPluginStoreSection() {
    // A KEY, NOT A DRAWER. The catalogue is a window of its own now, for the
    // reason the satellites map is: a function that gets its own window gets a
    // shape, and what was a section body five levels deep in a 300 px rail is
    // a panel the user can drag as large as their screen. This row is the
    // switch that puts it there and the chip that reports it without opening
    // it - the same arrangement, and the same primitive, as the satellite row
    // below.
    //
    // "###pluginstore" IS KEPT so no open/closed state resets: the id is what
    // ImGui hashes, and a row that changed identity would forget its state on
    // the launch that shipped this change.
    //
    // WHAT THE STORE CAN HONESTLY SAY FROM THE RAIL, and what it must not.
    // catalog_ is empty until the user asks for a catalogue, so "0 updates"
    // before that would be the clean-zero this product has been bitten by:
    // "nothing to update" and "we have not looked" are different statements.
    // IDLE is the second one.
    const bool haveCatalog = !catalog_.empty();
    const std::size_t pendingUpdates =
        haveCatalog ? plannedPluginUpdates().size() : 0u;
    const bool storeBusy = catalogPending_ || installPending_;
    char storeChip[16];
    if (storeBusy) {
        // A TRANSFER IS THE MOST IMPORTANT THING THIS ROW CAN SAY, and it
        // outranks the update count: something is moving over the network on
        // the user's behalf and the window that can cancel it is behind this
        // key.
        std::snprintf(storeChip, sizeof(storeChip), "BUSY");
    } else if (!haveCatalog) {
        std::snprintf(storeChip, sizeof(storeChip), "IDLE");
    } else if (pendingUpdates > 0) {
        std::snprintf(storeChip, sizeof(storeChip), "%zu UPD", pendingUpdates);
    } else {
        std::snprintf(storeChip, sizeof(storeChip), "OK");
    }
    if (benchSwitchRow("Plugin store###pluginstore", pluginBrowseOpen_, storeChip,
                       storeBusy      ? cascade::gui::theme::kPhosphor
                       : pendingUpdates > 0 ? cascade::gui::theme::kAmber
                                            : cascade::gui::theme::kPhosphor,
                       storeBusy || pendingUpdates > 0, true,
                       "Opens the plugin store: the catalogue, what each module "
                       "reaches for,\nwhat it costs to fit, and the updates the "
                       "catalogue offers.\nNothing is fetched until you ask inside "
                       "that window.")) {
        pluginBrowseOpen_ = !pluginBrowseOpen_;
    }

    // WHERE THE CATALOGUE IS READ FROM, and it stays EDITABLE. The store
    // window shows the source as a fact - which is right, because on that
    // window it is the provenance of everything on the panel - and it offers
    // no way to change it. This field is the one that used to sit at the top
    // of the browser, kept because taking it away would take a capability with
    // it: an enterprise deployment points this at its own index, and a local
    // index.json path is how the whole store is exercised offline. It is a
    // deployment setting rather than a browse control, which is why the rail
    // is a reasonable home for it.
    //
    // Committed on deactivate-after-edit rather than per keystroke, so a
    // half-typed host is never what a fetch would use.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##catalogue_url", "catalogue index.json URL",
                             pluginUrlBuf_, sizeof(pluginUrlBuf_));
    if (ImGui::IsItemDeactivatedAfterEdit()) { pluginCatalogueUrl_ = pluginUrlBuf_; }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("An https:// catalogue, or a path to a local index.json.\n"
                          "Nothing is fetched until you press CHECK NOW in the plugin\n"
                          "store window. Plugin downloads are always https and always\n"
                          "sha256-verified.");
    }

    // THE RETIRED MODULES HANG UNDER THE STORE'S KEY, and they are the one
    // thing about plugins that belongs to neither window. A retired module is
    // a fact about what the cached CATALOGUE POLICY now says - the host never
    // loaded it, so the fitted window cannot list it, and PluginStoreModel
    // carries no field for a module that is installed and quarantined. Its
    // remedy is an update, which is the store's business, so its rows and its
    // two keys stay here where the store's own key is.
    drawBlockedPluginRows();
}

void AppWindow::drawDecodersSection() {
    // WHAT NEITHER NEW WINDOW CARRIES. When the store and the inventory became
    // windows, most of what those two rail sections held went with them - but
    // not all of it, and the part that did not is not a leftover: it is four
    // controls that exist today and would simply have been deleted.
    //
    //   - the decoder OUTPUT window's switch, which is about traffic and not
    //     about installation;
    //   - the RADAR SCOPE's switch, which is a way of looking at what the
    //     receiver hears and only ever lived here because this is where a user
    //     goes looking for what their ADS-B plugin can do;
    //   - each loaded module's PRESETS - "where this decoder listens", one
    //     click - which the fitted window has no key for;
    //   - each loaded module's MUTE WHILE RUNNING override, which it has no
    //     key for either.
    //
    // The receiver-control remnant is here too: the refusal notice and any
    // grant held by a module that is no longer installed. Everything else
    // about a fitted module - start, stop, remove, its grant, why it is silent
    // - is in the fitted window and is deliberately not repeated.
    const std::size_t fed = pipeline_.running() ? fedDecoderCount() : 0u;
    char decChip[16];
    std::snprintf(decChip, sizeof(decChip), "%zu FED", fed);
    if (!benchSection("Decoders###decoders", false, decChip,
                      cascade::gui::theme::kPhosphor, fed > 0)) {
        return;
    }
    telemetryNotePanel("decoders");

    drawDecoderStatusRows();

    // --- presets and mute, per loaded module ---------------------------------
    // Deferred past the loop, exactly as the old installed list deferred it:
    // setPluginMutes rebuilds the mute snapshot, which walks the very vector
    // being iterated, and applyPluginPreset rebuilds every decoder instance.
    const std::vector<cascade::core::LoadedPlugin>& list = pluginHost_.plugins();
    int toggleMuteIdx = -1;
    bool toggleMuteTo = false;
    bool anyRow = false;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const cascade::core::LoadedPlugin& p = list[i];
        // A REFUSED CANDIDATE HAS NOTHING TO SET. Its reason is printed in the
        // fitted window, against the module it belongs to, and a mute
        // checkbox on a module that never loaded would imply the silence is
        // something the user chose.
        if (!p.loaded) { continue; }
        const bool hasPresets = p.preset != nullptr && p.preset->count() > 0u;
        const bool isDecoder =
            p.decoder != nullptr || p.iqDecoder != nullptr || p.imageDecoder != nullptr;
        if (!hasPresets && !isDecoder) { continue; }
        anyRow = true;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SeparatorText(p.name.c_str());
        drawPluginPresets(p);
        if (isDecoder) {
            // MUTE AUDIO WHILE RUNNING, per plugin, defaulted from what the
            // plugin consumes rather than from a global preference. An I/Q
            // decoder leaves behind a demodulated channel that is not the
            // signal being decoded - hiss, at whatever the volume is set to -
            // so it defaults ON; an audio decoder is fed the very audio the
            // speakers get, and SSTV's warble is how people tune it by ear, so
            // it defaults OFF. Neither default is right for everybody, which
            // is the entire reason this is a control.
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
        ImGui::PopID();
    }
    if (toggleMuteIdx >= 0 && static_cast<std::size_t>(toggleMuteIdx) < list.size()) {
        setPluginMutes(list[static_cast<std::size_t>(toggleMuteIdx)], toggleMuteTo);
    }
    if (!anyRow) {
        ImGui::TextDisabled("No fitted module publishes a preset or is fed a signal.");
    }
    if (!presetNote_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::good());
        ImGui::TextWrapped("%s", presetNote_.c_str());
        ImGui::PopStyleColor();
    }

    drawPluginTuneControls();
}

void AppWindow::drawPluginsSection() {
    // A KEY, NOT A DRAWER - the twin of the store row above it, and the same
    // primitive the satellites map uses. What is installed, whether it is
    // running, why it is not, and what to do about it are all in the fitted
    // modules window; this row opens it and reports the one thing worth
    // knowing without opening it.
    //
    // "###plugins" IS KEPT, so the row's open/closed state does not reset on
    // the launch that ships this change - and so does the disabled count in
    // the visible half of the label, which is the news a user who never opens
    // the window still has to learn.
    const std::size_t blocked = cascade::core::PluginRepo::blockedCount(
        pluginInventory_.plugins, pluginInventory_.policies);
    char header[64];
    if (blocked == 0u) {
        std::snprintf(header, sizeof(header), "Plugins###plugins");
    } else {
        std::snprintf(header, sizeof(header), "Plugins (%d disabled)###plugins",
                      static_cast<int>(blocked));
    }
    // FED, OF DECODERS FITTED - the same two numbers the DECODERS card in the
    // status column prints, from the same two places, so the rail and the card
    // cannot say different things about the same modules.
    //
    // AND THE DENOMINATOR IS DECODERS. It was every loaded module, which put a
    // basemap and a track-info provider - neither of which is ever fed a
    // signal - permanently in the bottom half of a chip whose top half counts
    // decoders being fed, so a healthy receiver reported 1/3.
    const std::size_t decoders = loadedDecoderCount();
    const std::size_t fedPlugins = pipeline_.running() ? fedDecoderCount() : 0u;
    char pluginChip[16];
    std::snprintf(pluginChip, sizeof(pluginChip), "%zu/%zu", fedPlugins, decoders);
    // The lamp is decoding, and only that. A blocked module is already
    // lettered into this row's own label, and a rust lamp for it would put
    // trouble on a rail row that is at that moment decoding perfectly well.
    if (benchSwitchRow(header, fittedWindowOpen_, pluginChip,
                       cascade::gui::theme::kPhosphor, fedPlugins > 0, true,
                       "Opens the fitted modules window: what is installed, which\n"
                       "modules are being fed, which were refused and why, and the\n"
                       "keys that start, stop and remove them.")) {
        fittedWindowOpen_ = !fittedWindowOpen_;
    }

    // ONE LINE, AND ONLY WHEN THERE IS SOMETHING TO SAY. A pointer, not a
    // copy: the fitted window quotes the runner's own sentence against the
    // module it belongs to, and repeating those sentences here would be the
    // same idle decoder described twice, in two places, by two pieces of code.
    // What this row owes a user who has not opened the window is the fact that
    // there is something in it to read.
    int refused = 0;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { ++refused; }
    }
    const bool notFed = pipeline_.running() && decoders > fedPlugins;
    if (refused > 0 || notFed) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              refused > 0 ? kErrorRed : cascade::gui::theme::warning());
        if (refused > 0) {
            ImGui::TextWrapped("%d module%s found and refused. The window says why.",
                               refused, refused == 1 ? " was" : "s were");
        } else {
            ImGui::TextWrapped("%zu fitted decoder%s not being fed. The window says why.",
                               decoders - fedPlugins,
                               (decoders - fedPlugins) == 1u ? " is" : "s are");
        }
        ImGui::PopStyleColor();
    }
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
                // user asks. Say where the key lives rather than showing a
                // dead one - and it now lives in a DIFFERENT WINDOW, so the
                // directions have to name that window or they send the user
                // hunting along this rail for a key that left it.
                ImGui::TextDisabled("Open the plugin store with the key above, then "
                                    "press CHECK NOW, to fetch the update.");
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

const cascade::core::LoadedPlugin* AppWindow::installedPluginRecord(
    const cascade::core::PluginCatalogEntry& e) const {
    const cascade::core::PluginPlatform* p = e.thisPlatform();
    if (p == nullptr) { return nullptr; }
    // The SAME sanitised-name comparison catalogEntryInstalled makes, and for
    // the same reason: the name install() would write is the name to look for,
    // not the one the catalogue happens to spell.
    std::string want;
    std::string err;
    if (!cascade::core::PluginRepo::sanitiseFileName(p->file, want, err)) {
        return nullptr;
    }
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        const std::string have = std::filesystem::path(lp.path).filename().string();
        if (equalsFileNameAscii(have, want)) { return &lp; }
    }
    // No manifest fallback here, deliberately - see the header. A retired
    // module is installed and has no record; answering with something else's
    // record would be worse than answering "the host never saw it".
    return nullptr;
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
    // AND THE SELECTION AND THE CONSENT GO WITH IT. The store window clamps
    // its own selection every frame and clears its tick whenever the selection
    // moves, but the tick is consent for ONE plugin and a fetch that replaces
    // the whole catalogue can leave the same index pointing at a different
    // module - so it is cleared here, at the moment the ground moves, rather
    // than left to a rule that is about a different event.
    if (pluginStoreDeck_) {
        pluginStoreDeck_->selected = -1;
        pluginStoreDeck_->legalAck = false;
    }
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

// --- THE RAIL ACROSS THE TOP OF A CABINET -------------------------------------
//
// The operating system's title bar used to sit above the main window and above
// every torn-off page, in the desktop's own style; it is gone (0.78.0, at the
// user's request: "remove all the top bars and put the minimise, maximise and
// close on the metal"), and this is what stands in for it. The window's name
// is engraved at the left of the cabinet's top rail, between the screws, and
// three brass keys sit at its right: minimise, maximise (restore, when the
// window is already full) and close. The rest of the rail is the handle the
// window is dragged by.
//
// ONE DRAWING FOR BOTH KINDS OF WINDOW. The main window's rail is hit-tested
// by the operating system (gui/win_frame.hpp) and its keys act on the GLFW
// window; a page's rail is an ImGui item and its keys act on the page. The
// keys are ImGui buttons in both cases, so hover, press and keyboard focus are
// the bench's own, and the glyphs are DRAWN rather than lettered because the
// bundled faces carry no box and no multiplication sign.
namespace {

struct RailPress {
    bool minimise = false;
    bool maximise = false;
    bool close = false;
};

// Where the keys sit on a rail `m` tall: right-aligned, clear of the top-right
// screw (whose countersink reaches 0.91 of the margin in from the edge), and
// inset from the rail's top and bottom.
struct RailKeyLayout {
    float keyW = 0.0f;
    float keyH = 0.0f;
    float gap = 0.0f;
    float top = 0.0f;
    float left = 0.0f;   // the left edge of the leftmost key
    float right = 0.0f;  // the right edge of the rightmost
    bool fits = false;
};

RailKeyLayout railKeyLayout(const ImVec2& tl, const ImVec2& br, float m, int keys) {
    RailKeyLayout k;
    if (m < 12.0f || keys <= 0) { return k; }
    k.keyH = std::max(10.0f, m - 6.0f);
    k.keyW = std::floor(k.keyH * 1.45f);
    k.gap = 4.0f;
    k.top = tl.y + (m - k.keyH) * 0.5f;
    k.right = br.x - m * 1.05f;
    k.left = k.right - static_cast<float>(keys) * k.keyW -
             static_cast<float>(keys - 1) * k.gap;
    // Room for the name as well, or the rail is too short to carry keys at all.
    k.fits = k.left > tl.x + m * 1.05f + 40.0f;
    return k;
}

RailPress drawRailChrome(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, float m,
                         const char* title, bool maximised, bool showMinMax,
                         const char* idPrefix, cascade::gui::frame::Rect* keysOut) {
    RailPress out;
    if (keysOut != nullptr) { *keysOut = cascade::gui::frame::Rect{}; }
    if (dl == nullptr || m < 12.0f) { return out; }
    const int keyCount = showMinMax ? 3 : 1;
    const RailKeyLayout k = railKeyLayout(tl, br, m, keyCount);

    // THE NAME, engraved into the brass between the screw and the keys: the
    // cut in the dark ink with a hairline of light under it, which is how
    // every caption on this bench is lettered.
    if (title != nullptr && title[0] != '\0') {
        ImFont* f = cascade::gui::fonts::legend();
        const float px = std::clamp(m * 0.62f, 10.0f, cascade::gui::fonts::kLegendSize);
        const float x = tl.x + m * 1.15f;
        const float y = tl.y + (m - px) * 0.5f;
        const float maxX = (k.fits ? k.left : br.x - m) - 8.0f;
        if (maxX > x + 8.0f) {
            const ImVec4 clip(x, tl.y, maxX, tl.y + m);
            dl->AddText(f, px, ImVec2(x, y + 1.0f),
                        cascade::gui::theme::withAlpha(cascade::gui::theme::kBrassTint, 0.55f),
                        title, nullptr, 0.0f, &clip);
            dl->AddText(f, px, ImVec2(x, y), cascade::gui::theme::kEngraved, title, nullptr,
                        0.0f, &clip);
        }
    }
    if (!k.fits) { return out; }

    const ImU32 ink = cascade::gui::theme::kIvory;
    // Half the size of a glyph, so the three read as one family.
    const float g = std::max(3.0f, std::floor(k.keyH * 0.22f));
    float x = k.left;
    const auto keyAt = [&](const char* id) {
        const ImVec2 a(x, k.top);
        const ImVec2 b(x + k.keyW, k.top + k.keyH);
        const bool pressed = benchWordKey(dl, a, b, "", true, id);
        x += k.keyW + k.gap;
        return std::pair<bool, ImVec2>(pressed, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f));
    };
    ImGui::PushID(idPrefix);
    if (showMinMax) {
        // MINIMISE: a bar low in the key, the desktop's own sign for it.
        const std::pair<bool, ImVec2> mn = keyAt("min");
        dl->AddLine(ImVec2(mn.second.x - g, mn.second.y + g * 0.6f),
                    ImVec2(mn.second.x + g, mn.second.y + g * 0.6f), ink, 1.5f);
        out.minimise = mn.first;
        // MAXIMISE: one pane; RESTORE: two, the back one offset.
        const std::pair<bool, ImVec2> mx = keyAt("max");
        if (maximised) {
            dl->AddRect(ImVec2(mx.second.x - g + 2.0f, mx.second.y - g - 1.0f),
                        ImVec2(mx.second.x + g + 1.0f, mx.second.y + g - 2.0f), ink, 0.0f, 0,
                        1.2f);
            dl->AddRectFilled(ImVec2(mx.second.x - g - 1.0f, mx.second.y - g + 2.0f),
                              ImVec2(mx.second.x + g - 2.0f, mx.second.y + g + 1.0f),
                              cascade::gui::theme::kBrassMid);
            dl->AddRect(ImVec2(mx.second.x - g - 1.0f, mx.second.y - g + 2.0f),
                        ImVec2(mx.second.x + g - 2.0f, mx.second.y + g + 1.0f), ink, 0.0f, 0,
                        1.2f);
        } else {
            dl->AddRect(ImVec2(mx.second.x - g, mx.second.y - g),
                        ImVec2(mx.second.x + g, mx.second.y + g), ink, 0.0f, 0, 1.2f);
        }
        out.maximise = mx.first;
    }
    // CLOSE: the cross.
    const std::pair<bool, ImVec2> cl = keyAt("close");
    dl->AddLine(ImVec2(cl.second.x - g, cl.second.y - g), ImVec2(cl.second.x + g, cl.second.y + g),
                ink, 1.5f);
    dl->AddLine(ImVec2(cl.second.x - g, cl.second.y + g), ImVec2(cl.second.x + g, cl.second.y - g),
                ink, 1.5f);
    out.close = cl.first;
    ImGui::PopID();
    if (keysOut != nullptr) {
        *keysOut = cascade::gui::frame::Rect{k.left - 2.0f, k.top - 2.0f, k.right + 2.0f,
                                             k.top + k.keyH + 2.0f};
    }
    return out;
}

}  // namespace

// The main window's rail: the application's name, engraved, and the three keys
// that stand in for the title bar's - each acting on the GLFW window, so the
// desktop's own minimise, maximise and close happen rather than imitations of
// them. The rail's height and the keys' rectangle are handed to the native hit
// test every frame (gui/win_frame.hpp), which is what makes the rail a handle
// and the keys not.
void AppWindow::drawCabinetRail(float x0, float y0, float x1, float y1, float margin) {
    if (!cascade::gui::frame::installed() || mainWindow_ == nullptr) { return; }
    const ImVec2 tl(x0, y0);
    const ImVec2 br(x1, y1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool maximised = glfwGetWindowAttrib(mainWindow_, GLFW_MAXIMIZED) != 0;
    const std::string title =
        std::string(cascade::appName()) + " " + cascade::versionString();
    cascade::gui::frame::Rect keys;
    const RailPress press =
        drawRailChrome(dl, tl, br, margin, title.c_str(), maximised, true, "##mainrail", &keys);
    if (press.minimise) { glfwIconifyWindow(mainWindow_); }
    if (press.maximise) {
        if (maximised) {
            glfwRestoreWindow(mainWindow_);
        } else {
            glfwMaximizeWindow(mainWindow_);
        }
    }
    // The same path the desktop's close button took: the run loop sees the
    // window asked to close and shuts the receiver down cleanly.
    if (press.close) { glfwSetWindowShouldClose(mainWindow_, GLFW_TRUE); }

    // THE RAIL DRAGS THE WINDOW BY TWO ROUTES, and a machine takes whichever
    // it offers. On the desk this was built at, WM_NCHITTEST calls the rail
    // the caption (win_frame.cpp), so a press on it never reaches ImGui: the
    // operating system moves the window itself, with snap, shake and the
    // system menu. On one user's laptop, 0.78.1 could not be dragged at all,
    // and from here the reason cannot be seen - whatever answers that
    // machine's hit test is not answering CAPTION over the rail. So the press
    // is caught on THIS side too: an invisible item over the rail, left of the
    // keys, and while it is held the window follows the pointer through GLFW.
    // Where the native route works the item never sees a press, so the two
    // cannot fight; where it does not, this one moves the window - without
    // snap, but moved. A double-click here fills the screen as it does on the
    // native route. The route a machine took is written to the diagnostic log
    // the first time this one engages, so the next report says which.
    if (margin >= 12.0f) {
        const float railRight = keys.empty() ? br.x - margin : keys.x0;
        const float railW = railRight - tl.x;
        if (railW > 1.0f) {
            const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(tl);
            ImGui::InvisibleButton("##maindrag", ImVec2(railW, margin));
            if (ImGui::IsItemActivated()) {
                mainDragCarryX_ = 0.0f;
                mainDragCarryY_ = 0.0f;
                if (!mainDragSaid_) {
                    mainDragSaid_ = true;
                    cascade::core::diagLogf("frame: rail press reached the client (hit test did "
                                            "not call it the caption); moving the window through "
                                            "GLFW instead");
                }
            }
            if (ImGui::IsItemActive() && !maximised &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                // Whole pixels to the window, the fraction carried to the next
                // frame, so a slow drag does not lose its sub-pixel remainder
                // every frame and lag the pointer.
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                mainDragCarryX_ += d.x;
                mainDragCarryY_ += d.y;
                const int dx = static_cast<int>(mainDragCarryX_);
                const int dy = static_cast<int>(mainDragCarryY_);
                if (dx != 0 || dy != 0) {
                    mainDragCarryX_ -= static_cast<float>(dx);
                    mainDragCarryY_ -= static_cast<float>(dy);
                    int wx = 0;
                    int wy = 0;
                    glfwGetWindowPos(mainWindow_, &wx, &wy);
                    glfwSetWindowPos(mainWindow_, wx + dx, wy + dy);
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (maximised) {
                    glfwRestoreWindow(mainWindow_);
                } else {
                    glfwMaximizeWindow(mainWindow_);
                }
            }
            ImGui::SetCursorScreenPos(savedCursor);
        }
    }
    // In client pixels. With viewports on, the main viewport's origin IS the
    // window's client origin on screen.
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    cascade::gui::frame::CaptionLayout layout;
    layout.railHeight = margin;
    layout.keys = cascade::gui::frame::Rect{keys.x0 - origin.x, keys.y0 - origin.y,
                                            keys.x1 - origin.x, keys.y1 - origin.y};
    cascade::gui::frame::setCaptionLayout(layout);
}

bool AppWindow::pageGeometryTransient(const char* id) const {
    const auto it = pageChrome_.find(id);
    return it != pageChrome_.end() && (it->second.collapsed || it->second.maximised);
}

// A PAGE IS A CABINET. The window is begun without ImGui's title bar; the
// brass, the screws and the well are drawn over its whole rectangle, the rail
// carries its name and keys, and the body is laid in a child inset by the
// margin the cabinet reported. Pages that used to draw a cabinet of their own
// (the plugin store, the fitted modules, the satellites map) now draw into the
// well instead, so every page is one object - and the main window is the same
// object, drawn by the same drawCabinet.
bool AppWindow::beginPage(const char* id, const char* title, bool* open, int flags) {
    PageChrome& pc = pageChrome_[id];
    constexpr float kStripH = 30.0f;

    if (pc.pendingMaximise) {
        // The work area of whichever monitor the page is on, or the main
        // window's own area for a page still inside it.
        const ImGuiViewport* main = ImGui::GetMainViewport();
        ImVec2 pos = main->WorkPos;
        ImVec2 size = main->WorkSize;
        if (ImGuiWindow* w = ImGui::FindWindowByName(id)) {
            if (w->ViewportOwned) {
                const ImVec2 c(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f);
                const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
                for (const ImGuiPlatformMonitor& mon : pio.Monitors) {
                    if (c.x >= mon.MainPos.x && c.x < mon.MainPos.x + mon.MainSize.x &&
                        c.y >= mon.MainPos.y && c.y < mon.MainPos.y + mon.MainSize.y) {
                        pos = mon.WorkPos;
                        size = mon.WorkSize;
                        break;
                    }
                }
            }
        }
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        pc.pendingMaximise = false;
    }
    if (pc.pendingRestore) {
        ImGui::SetNextWindowPos(ImVec2(pc.restoreX, pc.restoreY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(pc.restoreW, pc.restoreH), ImGuiCond_Always);
        pc.pendingRestore = false;
    }

    ImGuiWindowFlags f = static_cast<ImGuiWindowFlags>(flags) | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse;
    if (pc.collapsed) {
        // Rolled up to its rail: a fixed strip, as wide as the page was.
        f |= ImGuiWindowFlags_NoResize;
        f &= ~ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::SetNextWindowSize(ImVec2(std::max(pc.restoreW, 240.0f), kStripH),
                                 ImGuiCond_Always);
    }
    // NO PADDING AND NO BORDER: the brass reaches the window's edge, which on a
    // torn-off page is the edge of the operating system's window. The resize
    // grip is made invisible, not removed - the edge zones still resize the
    // page - because a grey triangle over the bottom-right screw is not a
    // fastener.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, cascade::gui::theme::kBrassShade);
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, IM_COL32(0, 0, 0, 0));
    const bool visible = ImGui::Begin(id, open, f);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    pageBodyOpen_ = false;
    pageInset_ = 0.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tl = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 br(tl.x + size.x, tl.y + size.y);
    float m = 0.0f;
    if (pc.collapsed || size.y < 80.0f || size.x < 80.0f) {
        // Too small for a cabinet, or rolled up on purpose: a brass strip.
        dl->AddRectFilled(tl, br, cascade::gui::theme::kBrassShade, 3.0f);
        cascade::gui::addBenchBevel(dl, tl, br, 3.0f, true);
        m = std::min(kStripH, size.y);
    } else {
        m = drawCabinet(dl, tl, br, kRailMinMargin);
    }
    ImGuiWindow* self = ImGui::GetCurrentWindow();
    const bool ownWindow = self != nullptr && self->ViewportOwned;
    cascade::gui::frame::Rect keys;
    const RailPress press =
        drawRailChrome(dl, tl, br, m, title, pc.maximised, true, "##pagerail", &keys);

    // THE RAIL DRAGS THE PAGE, through the same machinery a title bar uses, so
    // a page dragged out of the main window becomes its own window and one
    // dragged back merges into it again. A double-click on it fills the
    // screen, as on a title bar.
    bool toggleMax = false;
    if (m > 0.0f) {
        const float railW = std::max(1.0f, (keys.empty() ? br.x - m : keys.x0) - tl.x);
        ImGui::SetCursorScreenPos(tl);
        ImGui::InvisibleButton("##pagedrag", ImVec2(railW, m));
        if (ImGui::IsItemActivated() && self != nullptr) { ImGui::StartMouseMovingWindow(self); }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            toggleMax = true;
        }
    }
    if (press.close && open != nullptr) { *open = false; }
    if (press.minimise) {
        if (ownWindow) {
            // To the taskbar, which it has a button on (ConfigViewportsNoTaskBarIcon
            // is off for exactly this).
            GLFWwindow* w = static_cast<GLFWwindow*>(self->Viewport->PlatformHandle);
            if (w != nullptr) { glfwIconifyWindow(w); }
        } else if (pc.collapsed) {
            pc.collapsed = false;
            pc.pendingRestore = true;
            pc.restoreX = tl.x;
            pc.restoreY = tl.y;
        } else {
            pc.collapsed = true;
            pc.restoreW = size.x;
            pc.restoreH = size.y;
        }
    }
    if (press.maximise || toggleMax) {
        if (pc.maximised) {
            pc.maximised = false;
            pc.pendingRestore = true;
        } else {
            pc.restoreX = tl.x;
            pc.restoreY = tl.y;
            pc.restoreW = size.x;
            pc.restoreH = size.y;
            pc.maximised = true;
            pc.pendingMaximise = true;
        }
    }
    if (!visible || pc.collapsed) { return false; }

    // THE WELL: the page's content, inset by the margin the cabinet reported
    // plus the bevel it drew on the margin's inner boundary. An auto-sizing
    // page (the target details) gets an auto-sizing well, so the window still
    // fits its content and the cabinet fits the window.
    const float inset = m + 3.0f;
    const ImVec2 wellSize(size.x - inset * 2.0f, size.y - inset * 2.0f);
    const bool autoSize = (static_cast<ImGuiWindowFlags>(flags) & ImGuiWindowFlags_AlwaysAutoResize) != 0;
    if (!autoSize && (wellSize.x < 8.0f || wellSize.y < 8.0f)) { return false; }
    ImGui::SetCursorScreenPos(ImVec2(tl.x + inset, tl.y + inset));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGuiChildFlags cf = ImGuiChildFlags_None;
    ImVec2 childSize = wellSize;
    if (autoSize) {
        cf = ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY |
             ImGuiChildFlags_AlwaysAutoResize;
        childSize = ImVec2(0.0f, 0.0f);
    }
    ImGui::BeginChild("##pagewell", childSize, cf, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();
    pageBodyOpen_ = true;
    pageInset_ = inset;
    return true;
}

void AppWindow::endPage() {
    if (pageBodyOpen_) {
        ImGui::EndChild();
        // The bottom and right margins count towards the window's content,
        // which is what lets an auto-sizing page size itself to the cabinet
        // rather than to the well alone.
        const ImVec2 wellBR = ImGui::GetItemRectMax();
        ImGui::SetCursorScreenPos(ImVec2(wellBR.x + pageInset_, wellBR.y + pageInset_));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        pageBodyOpen_ = false;
    }
    ImGui::End();
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

AppWindow::MapPage* AppWindow::findMapPage(const std::string& plugin) {
    for (MapPage& pg : mapPages_) {
        if (pg.plugin == plugin) { return &pg; }
    }
    return nullptr;
}

AppWindow::MapPage& AppWindow::ensureMapPage(const std::string& plugin) {
    if (MapPage* existing = findMapPage(plugin)) { return *existing; }

    MapPage page;
    page.plugin = plugin;
    page.view = std::make_unique<MapView>();
    // Receiver-wide facts reach every page at birth. Applying the position
    // here — at the one place a view is created — is what makes the old
    // "config applied before the view existed" ordering bug impossible now.
    if (rxSet_) { page.view->setHome(rxLat_, rxLon_); }
    // WHAT ALTITUDE THIS AIRCRAFT HAD WHERE ITS TRAIL RUNS, answered by the
    // observations PluginUi records as it polls (see altitudeNear). Installed
    // ONCE, here, for the same reason the receiver position is: this is the
    // one place a view is created, and a std::function rebound every frame
    // would be a per-frame allocation to say something that never changes.
    // The capture is `this`, which outlives every page - the pages are members
    // and are cleared, never detached.
    page.view->setAltitudeLookup([this](const std::string& plugin, const std::string& id,
                                        double latDeg, double lonDeg, double& outAltM) {
        return pluginUi_.altitudeNear(plugin, id, latDeg, lonDeg, outAltM);
    });

    // Geometry and open state: the page's own saved entry wins; failing that,
    // the LEGACY single-window rectangle (read from the old config keys, no
    // longer written) staggered by page index, so an upgrading install's
    // pages open where its one map used to sit without stacking exactly. The
    // 34 px step matches separateWindowAnchor's stagger. No legacy rectangle
    // either leaves all four zero, which selects the monitor-derived default
    // at draw time.
    const auto it = std::find_if(
        mapPagesSaved_.begin(), mapPagesSaved_.end(),
        [&plugin](const cascade::core::AppConfig::MapPage& e) {
            return e.plugin == plugin;
        });
    if (it != mapPagesSaved_.end()) {
        page.x = it->x;
        page.y = it->y;
        page.w = it->width;
        page.h = it->height;
        // Not the saved entry's open flag. A page is created closed and opened
        // by the user's own hand - its rail row, a preset, a details window's
        // Go-to - whatever the file says it was doing at the last exit.
    } else if (mapWinW_ > 0 && mapWinH_ > 0) {
        const int stagger = 34 * static_cast<int>(mapPages_.size());
        page.x = mapWinX_ + stagger;
        page.y = mapWinY_ + stagger;
        page.w = mapWinW_;
        page.h = mapWinH_;
    }

    mapPages_.push_back(std::move(page));
    return mapPages_.back();
}

void AppWindow::syncMapPagesToSaved() {
    for (const MapPage& pg : mapPages_) {
        cascade::core::AppConfig::MapPage e;
        e.plugin = pg.plugin;
        e.x = pg.x;
        e.y = pg.y;
        e.width = pg.w;
        e.height = pg.h;
        e.open = pg.open;
        const auto it = std::find_if(
            mapPagesSaved_.begin(), mapPagesSaved_.end(),
            [&pg](const cascade::core::AppConfig::MapPage& s) {
                return s.plugin == pg.plugin;
            });
        if (it != mapPagesSaved_.end()) {
            *it = std::move(e);
        } else if (mapPagesSaved_.size() < cascade::core::AppConfig::kMaxMapPages) {
            // Appended, so saved order stays stable and a diff of the config
            // file across sessions reads as growth, not reshuffling. The cap
            // matches the loader's: entries past it would be dropped on the
            // next load anyway, so writing them would only feign persistence.
            mapPagesSaved_.push_back(std::move(e));
        }
    }
}

bool AppWindow::applyReceiverPosition(double latDeg, double lonDeg) {
    // REFUSED RATHER THAN CLAMPED, and by the same positive range test the
    // config sanitizer uses, so a typo cannot silently install a receiver at
    // the pole and quietly make every distance wrong. Every caller - the
    // toolbar's button, the satellites window's coordinate cells, a click on
    // that window's map - gets the same refusal, because a check written once
    // per entry point is a check that will eventually be missing from one.
    // The predicate also refuses 0,0 (receiverPositionAcceptable says why).
    if (!cascade::gui::receiverPositionAcceptable(latDeg, lonDeg)) { return false; }
    rxLat_ = latDeg;
    rxLon_ = lonDeg;
    rxSet_ = true;
    // THE TYPED FIELDS FOLLOW THE APPLIED POSITION. They are the toolbar's
    // draft of this same number, and a position set from the satellites
    // window would otherwise leave them showing whatever was last typed
    // somewhere else - two controls claiming to be the receiver's latitude
    // and disagreeing about it.
    rxLatInput_ = latDeg;
    rxLonInput_ = lonDeg;
    // EVERY page, not just the one whose control was used: the receiver's
    // position is a fact about the antenna, and two pages disagreeing about
    // where home is would put the range rings in two different places at
    // once. The scope is told for the same reason - it is a third view of
    // the same antenna.
    for (MapPage& other : mapPages_) {
        other.view->setHome(rxLat_, rxLon_);
    }
    scope_.setReceiver(rxLat_, rxLon_);
    // A NEW ORIGIN INVALIDATES THE OLD COVERAGE. Every wedge in it was
    // measured from somewhere else, and keeping them would draw one antenna's
    // pattern around another antenna's position.
    coverage_.reset();
    return true;
}

void AppWindow::drawRxPositionEntry() {
    // The receiver's own position is asked for, never guessed. It is what
    // range and bearing are measured from - on the map, in the track table and
    // now for every mark on the radar scope - and inferring it from a decoded
    // target would be confidently wrong the moment the first aircraft appears.
    //
    // DOUBLE, NOT FLOAT, and persisted. A float carries about seven significant
    // digits, which runs out inside the decimal part of a latitude and moves
    // the origin of every range ring by metres for no reason; and the pair used
    // to be static LOCALS, so the position died with the process. Both are now
    // AppConfig fields - see AppConfig::rxPositionSet.
    //
    // ONE COPY, drawn by the map pages and by the scope's no-position state.
    // The button does more than assign two numbers, and a second hand-written
    // copy would sooner or later do only some of it.
    //
    // SIZED TO THE COLUMN IT IS DRAWN IN. On a map page or the scope's empty
    // state there is room for two 96-px cells and the key; on the rail's
    // Radar section, which draws the same row above its "Radar scope" key,
    // there is not, and the fixed widths pushed the key past the edge so it
    // read "Set RX" with the rest cut off. The key keeps its full label and
    // the cells give way, never below what five decimals need to be legible.
    const ImGuiStyle& rxStyle = ImGui::GetStyle();
    const float rxKeyW = ImGui::CalcTextSize("Set RX here").x + rxStyle.FramePadding.x * 2.0f;
    const float rxAvail = ImGui::GetContentRegionAvail().x;
    const float rxCellW = std::clamp((rxAvail - rxKeyW - rxStyle.ItemSpacing.x * 2.0f) * 0.5f, 60.0f, 96.0f);
    ImGui::SetNextItemWidth(rxCellW);
    ImGui::InputDouble("##homelat", &rxLatInput_, 0.0, 0.0, "%.5f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Receiver latitude, degrees north (-90..90)");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(rxCellW);
    ImGui::InputDouble("##homelon", &rxLonInput_, 0.0, 0.0, "%.5f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Receiver longitude, degrees east (-180..180)");
    }
    ImGui::SameLine();
    // REFUSED RATHER THAN CLAMPED, and by the same positive range test the
    // config sanitizer uses, so a typo cannot silently install a receiver at
    // the pole and quietly make every distance wrong. The button simply does
    // not accept the pair.
    // AND NOT 0,0 - see receiverPositionAcceptable: the pair an untouched
    // entry holds is the pair a user's scope was found measuring from.
    const bool rxInputOk = cascade::gui::receiverPositionAcceptable(rxLatInput_, rxLonInput_);
    ImGui::BeginDisabled(!rxInputOk);
    if (ImGui::SmallButton("Set RX here")) {
        applyReceiverPosition(rxLatInput_, rxLonInput_);
    }
    ImGui::EndDisabled();
    if (!rxInputOk && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (rxLatInput_ == 0.0 && rxLonInput_ == 0.0) {
            ImGui::SetTooltip("Type the receiver's position first: 0,0 is a point in the\n"
                              "Gulf of Guinea, not a receiver.");
        } else {
            ImGui::SetTooltip("Latitude must be -90..90 and longitude -180..180");
        }
    }
}

// THE POSITION IN ONE CLICK, where before it was two numbers to look up.
//
// The radar scope refuses to draw until it knows where the antenna is, and it
// is right to: every mark on it is a range and a bearing from that one place.
// But a fresh install answered that with two zeroed fields and a sentence, on
// an otherwise empty panel, and the person who pressed "Radar scope" saw a
// scope that did nothing. Reported as exactly that - "the radar scope doesn't
// work" - by a user whose config had never carried a position and whose
// ADS-B map had been open beside it the whole time.
//
// So the two things the application already knows are OFFERED, each as a key
// that says what it will do and where that would put the receiver:
//
//   - THE MIDDLE OF THE TRAFFIC. Reception is roughly a disc about the
//     antenna; the mean position of the aircraft being heard is near its
//     centre. Offered once three aircraft with positions are on the books.
//   - THE MAP'S CENTRE. A user who has been panning a map around their own
//     town has been saying where they are the whole time.
//
// Both are starting points and are said to be. A position set from either
// goes through applyReceiverPosition like a typed one, so the map pages, the
// coverage accumulator and the scope all learn it together, and the typed
// fields below follow it so the exact figure can be corrected afterwards -
// or set precisely from a map click, which the map pages carry.
void AppWindow::drawReceiverPositionOffers() {
    double tLat = 0.0;
    double tLon = 0.0;
    const int heard = cascade::gui::scopeTrafficCentre(pluginUi_.tracks(), tLat, tLon);
    const bool trafficReady = heard >= cascade::gui::kScopeTrafficCentreMinAircraft &&
                              cascade::gui::receiverPositionAcceptable(tLat, tLon);
    char label[96];
    if (trafficReady) {
        std::snprintf(label, sizeof(label), "Use the middle of the %d aircraft heard  (%.2f%c %.2f%c)",
                      heard, std::fabs(tLat), tLat >= 0.0 ? 'N' : 'S', std::fabs(tLon),
                      tLon >= 0.0 ? 'E' : 'W');
    } else if (heard > 0) {
        std::snprintf(label, sizeof(label), "Use the middle of the traffic  (%d heard, need %d)",
                      heard, cascade::gui::kScopeTrafficCentreMinAircraft);
    } else {
        std::snprintf(label, sizeof(label), "Use the middle of the traffic  (none heard yet)");
    }
    ImGui::BeginDisabled(!trafficReady);
    if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) { applyReceiverPosition(tLat, tLon); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("The mean position of the aircraft with a reported position.\n"
                          "Reception is roughly a disc about the antenna, so its middle is\n"
                          "near the receiver - a starting point, not a survey.");
    }

    // The map's centre: the open page first, because that is the one being
    // looked at, else any page that has a view.
    const MapPage* mapPage = nullptr;
    for (const MapPage& page : mapPages_) {
        if (page.view == nullptr) { continue; }
        if (mapPage == nullptr || (page.open && !mapPage->open)) { mapPage = &page; }
    }
    // NOT OFFERED FROM THE ORIGIN. A map backed out to the whole world sits on
    // 0,0, and a map never yet looked at can too; offering that as "the map's
    // centre" is how a receiver ends up in the Gulf of Guinea with the view
    // dragged to Liverpool - see receiverPositionAcceptable.
    if (mapPage != nullptr &&
        cascade::gui::receiverPositionAcceptable(mapPage->view->viewCentreLatDeg(),
                                                 mapPage->view->viewCentreLonDeg())) {
        const double mLat = mapPage->view->viewCentreLatDeg();
        const double mLon = mapPage->view->viewCentreLonDeg();
        std::snprintf(label, sizeof(label), "Use the %s map's centre  (%.2f%c %.2f%c)",
                      mapPage->plugin.c_str(), std::fabs(mLat), mLat >= 0.0 ? 'N' : 'S',
                      std::fabs(mLon), mLon >= 0.0 ? 'E' : 'W');
        if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) { applyReceiverPosition(mLat, mLon); }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Where that map is looking right now. If you have been panning\n"
                              "it around your own area, that is close to where you are.");
        }
    }
    benchHint("or type it, or click SET FROM MAP CLICK on a map page:");
    drawRxPositionEntry();
}

// THE RADAR SCOPE, ON THE RAIL.
//
// Its own key rather than a line inside Decoders: a mode that replaces the
// entire window is not the same kind of control as a checkbox next to a
// decoder, and burying it inside that section is what got it reported
// missing from the rail twice, which is twice more than a feature should
// have to be.
void AppWindow::drawRadarSection() {
    const char* chip = scopeMode_ ? "SCOPE" : "OFF";
    if (!benchSection("Radar", false, chip, cascade::gui::theme::kPhosphor,
                      scopeMode_)) {
        return;
    }
    telemetryNotePanel("radar");

    drawScopeModeControl();
}

void AppWindow::drawScopeModeControl() {
    // THE POSITION COMES FIRST, HERE ON THE RAIL, when there is none. The
    // scope cannot draw a range or a bearing from nowhere, and dropping the
    // user into it to find that out is what "the radar scope doesn't work"
    // meant. The offers are the same ones the scope's own empty state makes;
    // once a position is set this block disappears and the key is the whole
    // section again.
    if (!rxSet_) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("The scope needs the receiver's position first.");
        ImGui::PopStyleColor();
        drawReceiverPositionOffers();
        ImGui::Spacing();
    }
    // THE SWITCH. It lives in the Radar section of the rail - the place a user
    // goes looking for a radar - and is a button rather than a checkbox on
    // purpose: turning it on replaces the entire window, which is a place you
    // go rather than a setting you tick, and a checkbox that silently
    // swallowed the spectrum would read as a fault.
    if (ImGui::Button(scopeMode_ ? "Leave radar scope" : "Radar scope",
                      ImVec2(-1.0f, 0.0f))) {
        scopeMode_ = !scopeMode_;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "A plan-position indicator for aircraft: range rings and bearing\n"
            "ticks around the receiver, with a detail panel for the one you\n"
            "select. Takes the whole window; there is a way back on its own bar.");
    }
    // SAID HERE RATHER THAN DISCOVERED IN AN EMPTY SCOPE. The scope draws
    // whatever aircraft the host has, which with nothing publishing tracks is
    // none at all - and an empty scope looks identical to a broken one.
    //
    // AND IT SAYS WHICH KIND OF NOTHING. trackPluginNames() is empty for a
    // machine with no tracker, for one whose tracker the user stopped and for
    // one whose tracker the host refused; this line used to read "no aircraft
    // source installed" in all three, which is a lie in two of them and sends
    // the user to buy what they already own. See gui/module_census.hpp.
    if (pluginUi_.trackPluginNames().empty()) {
        const cascade::gui::ModuleCensus census = cascade::gui::censusModules(
            pluginHost_.plugins(), CASCADE_CAP_TRACK_SOURCE,
            [this](const std::string& key) { return pluginIsStopped(key); });
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        // WRAPPED, not TextDisabled: this rail is narrow and a user may drag it
        // narrower, and a sentence that runs off the edge of the column says
        // nothing at all.
        ImGui::TextWrapped(
            "The scope will be empty. %s",
            cascade::gui::trackSourceAbsenceNote(
                census, "aircraft positions",
                "An ADS-B decoder is what fills this face; the plugin store is where "
                "one is fitted from.")
                .c_str());
        ImGui::PopStyleColor();
    }
}

void AppWindow::drawScopeMode() {
    telemetryNotePanel("scope");

    // --- the bar, drawn FIRST and unconditionally ---------------------------
    // The way out comes before anything that can run out of room. Everything
    // below this bar is sized from what is left, and every one of those things
    // is allowed to decide there is not enough space and draw nothing; the
    // exit is not, because a mode you cannot leave is the map-page latch again
    // in a larger form.
    const bool leaveScope = ImGui::Button("EXIT SCOPE");
    if (leaveScope) { scopeMode_ = false; }
    // A SECOND WAY OUT, and it is not belt and braces for its own sake: the
    // button above is a DRAWN thing, and a drawn thing can be below the fold of
    // a window somebody has dragged very short - this root window has no
    // scrollbar by design. Escape is what every full-screen mode is already
    // expected to answer, and it costs one line.
    //
    // NOT WHILE A FIELD IS BEING EDITED and not while a popup is up: Escape
    // means "cancel this edit" and "close this dialog" everywhere else in the
    // application, and a mode that stole it would make the coordinate boxes on
    // this very bar behave differently from every other box in the product.
    const bool popupUp = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!popupUp && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        scopeMode_ = false;
    }
    ImGui::SameLine();
    benchHint("|");
    ImGui::SameLine();
    benchHint("RANGE");
    ImGui::SameLine();
    // A STEPPER, NOT A SLIDER OR A FREE FIELD. The range is a ladder of six
    // values (see kScopeRangesNm) and the two buttons are the ladder: there is
    // no state either of them can put the view into that it has no rings for,
    // and each is disabled at its own end so the control says where the travel
    // stops rather than silently doing nothing.
    const int downNm = scopeRangeStepped(scopeRangeNm_, -1);
    ImGui::BeginDisabled(downNm == scopeRangeNm_);
    const bool stepDown = ImGui::Button("-##scoperange");
    ImGui::EndDisabled();
    if (stepDown) { scopeRangeNm_ = downNm; }
    ImGui::SameLine();
    // Fixed width so the buttons either side do not shuffle as the number's
    // width changes between "10 NM" and "400 NM".
    {
        const std::string txt = scopeRangeReadout(scopeRangeNm_);
        // "400 NM" is the widest entry on the ladder in every font this
        // application is likely to be given, but the pad is floored anyway: a
        // font where some other digit is wider would otherwise hand ImGui a
        // negative-width item, and guessing about a font is exactly what the
        // map's altitude legend was rewritten to stop doing.
        const float w = ImGui::CalcTextSize("400 NM").x;
        const float have = ImGui::CalcTextSize(txt.c_str()).x;
        ImGui::Dummy(ImVec2(std::max(0.0f, w - have), 0.0f));
        ImGui::SameLine(0.0f, 0.0f);
        // A READING, not a legend: the bench letters its controls in ivory and
        // its numbers in amber, and this is a number.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(240.0f / 255.0f, 168.0f / 255.0f, 64.0f / 255.0f, 1.0f));
        ImGui::TextUnformatted(txt.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    const int upNm = scopeRangeStepped(scopeRangeNm_, +1);
    ImGui::BeginDisabled(upNm == scopeRangeNm_);
    const bool stepUp = ImGui::Button("+##scoperange");
    ImGui::EndDisabled();
    if (stepUp) { scopeRangeNm_ = upNm; }
    ImGui::SameLine();
    benchHint("the mouse wheel over the scope steps it too");
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // --- no receiver position is an HONEST EMPTY STATE ----------------------
    //
    // NOT A BROKEN SCOPE. Every mark on this face is a distance and a direction
    // from one place; without that place there is nothing to centre it on. The
    // one thing it must not do is default to 0N 0E, which is not a neutral
    // value - it is a real place in the Gulf of Guinea, and a scope centred
    // there would draw a plausible-looking picture in which every range and
    // bearing was wrong by the distance from there to the antenna.
    if (!rxSet_) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextUnformatted("The scope needs the receiver's position.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Everything on a radar scope is a range and a bearing measured from "
            "the antenna, so there is nothing to draw until this receiver knows "
            "where it is. It is the same position the map uses, and setting it "
            "here sets it there. Either key below puts the receiver at a place "
            "the application already knows; the scope draws the moment one is "
            "pressed, and the exact figure can be corrected afterwards.");
        ImGui::Spacing();
        // Kept to the width of the rail's own controls: a key the full width
        // of a 1600 px window is a bar, not a key.
        ImGui::BeginChild("##scopeoffers", ImVec2(std::min(520.0f, avail.x), 0.0f),
                          ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
        drawReceiverPositionOffers();
        ImGui::EndChild();
        return;
    }

    // The position may have been set from a map page (or restored from the
    // config) since the last time this ran, so it is pushed in every frame
    // rather than only on the button - the same rule the trail switches follow,
    // and what makes a change on any surface reach every other one immediately.
    scope_.setReceiver(rxLat_, rxLon_);
    // The range likewise travels in both directions: the bar's buttons write
    // scopeRangeNm_, the wheel over the face writes the view's own copy, and
    // the read-back below is what carries the wheel's change into the config.
    scope_.setRangeNm(scopeRangeNm_);

    // --- the instrument face ------------------------------------------------
    //
    // The layout of the photographed receiver: gauges outboard, the round scope
    // and its data panel inboard, two real controls on a plinth beneath. It is
    // drawn only when there is genuinely room for it; below that the scope
    // takes the whole area and the face is dropped entirely, because a bezel
    // that has squeezed the actual instrument down to nothing has the priority
    // exactly backwards.
    // THE CABINET IS A FIXED 1080 x 822 SLAB, scaled uniformly and centred in
    // whatever room the window has - not a set of parts stretched to fill it.
    //
    // That single decision is what makes the face read as one machined object:
    // the reference design fixes every position inside that slab, and a layout
    // that rescales each part independently reproduces the components while
    // losing the thing they add up to. Measured against the reference, the
    // fill-the-box version had the tube half again too large relative to its
    // panel and the plinth bays at three different widths.
    constexpr float kCabH = 822.0f;
    // THE SCALE COMES FROM THE HEIGHT, and the cabinet then FILLS the window.
    //
    // Scaling uniformly on min(width/1080, height/822) and centring the result
    // kept the reference's proportions exactly and letterboxed the rest -
    // which on a wide window is a broad band of dead case down each side and
    // under the plinth. Reported as exactly that.
    //
    // The reference's own grid already says what to do with slack: its upper
    // deck is 62 | 1fr | 330 | 62, and the 1fr is the scope bay. So the FIXED
    // parts - the two gauge bays, the LCD, the deck heights, every gap and pad
    // - keep their designed size at this scale, and every spare pixel goes to
    // the tube, which is the thing worth making bigger anyway.
    const float scale = std::clamp(avail.y / kCabH, 0.42f, 2.2f);
    // Below this, or too narrow to hold the fixed columns with a usable tube
    // between them, the cabinet is dropped and the tube takes everything: a
    // bezel that has squeezed the instrument to nothing has it backwards.
    const bool roomForFace = scale >= 0.42f && avail.x >= 900.0f;

    if (!roomForFace) {
        scope_.draw(avail.x, avail.y, pluginUi_.tracks(), &basemap_, &trackInfo_);
        scopeRangeNm_ = scope_.rangeNm();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winTL = ImGui::GetCursorScreenPos();
    const float kGaugeW = 62.0f * scale;

    // THE CABINET IS AS WIDE AS ITS CONTENTS, AND NO WIDER.
    //
    // Stretching it to the window put the deck's contents in the middle of a
    // slab with a band of empty case down each side - the dead space this was
    // reported as twice. The unit is an OBJECT: it should be the size of the
    // instrument in it, and what is left over is the desk it sits on, not more
    // of the unit.
    //
    // The width follows from the tube, and the tube from the deck's height,
    // so the order below is forced: work out how tall the deck can be, that
    // gives the tube its size, and the row of bays either side of it gives the
    // cabinet its width. If that comes out wider than the window, the tube -
    // the only flexible part - gives back the difference.
    const float cabPad = 16.0f * scale;
    const float innerGap = 10.0f * scale;
    const float deckPadY = 12.0f * scale;
    const float lowerH = 165.0f * scale;
    const float cabH = std::floor(avail.y);
    const float deckH =
        cabH - cabPad * 2.0f - deckPadY * 3.0f - lowerH;
    const float panelW = std::min(340.0f * scale, avail.x * 0.30f);
    float tubeSide = std::max(120.0f, deckH);
    float groupW = kGaugeW * 2.0f + innerGap * 2.0f + tubeSide + 14.0f + panelW;
    const float maxGroupW = avail.x - cabPad * 2.0f;
    if (groupW > maxGroupW) {
        tubeSide = std::max(120.0f, tubeSide - (groupW - maxGroupW));
        groupW = kGaugeW * 2.0f + innerGap * 2.0f + tubeSide + 14.0f + panelW;
    }
    const float cabW = std::min(avail.x, groupW + cabPad * 2.0f);
    const ImVec2 faceTL(std::floor(winTL.x + (avail.x - cabW) * 0.5f),
                        std::floor(winTL.y));
    const ImVec2 faceBR(faceTL.x + cabW, faceTL.y + cabH);

    // THE CASE, in the reference design's own colours rather than colours
    // chosen here: #26271f down to #14150f at 55% and #0f100b at the foot,
    // bordered #2e2f26, with an inner rule inset 12 px. The grey-blue case
    // this replaced was a perfectly reasonable dark panel and completely wrong
    // beside the thing it was meant to match.
    {
        constexpr float kRound = 12.0f;
        const float midY = faceTL.y + (faceBR.y - faceTL.y) * 0.55f;
        dl->AddRectFilled(faceTL, faceBR, IM_COL32(20, 21, 15, 255), kRound);
        if (faceBR.x - faceTL.x > kRound * 2.0f) {
            dl->AddRectFilledMultiColor(
                ImVec2(faceTL.x + kRound, faceTL.y), ImVec2(faceBR.x - kRound, midY),
                IM_COL32(38, 39, 31, 255), IM_COL32(38, 39, 31, 255),
                IM_COL32(20, 21, 15, 255), IM_COL32(20, 21, 15, 255));
            dl->AddRectFilledMultiColor(
                ImVec2(faceTL.x + kRound, midY), ImVec2(faceBR.x - kRound, faceBR.y),
                IM_COL32(20, 21, 15, 255), IM_COL32(20, 21, 15, 255),
                IM_COL32(15, 16, 11, 255), IM_COL32(15, 16, 11, 255));
        }
        dl->AddRect(faceTL, faceBR, IM_COL32(46, 47, 38, 255), kRound, 0, 1.0f);
        dl->AddLine(ImVec2(faceTL.x + kRound, faceTL.y + 1.0f),
                    ImVec2(faceBR.x - kRound, faceTL.y + 1.0f),
                    IM_COL32(255, 255, 255, 15), 1.0f);
        dl->AddRect(ImVec2(faceTL.x + 12.0f, faceTL.y + 12.0f),
                    ImVec2(faceBR.x - 12.0f, faceBR.y - 12.0f),
                    IM_COL32(255, 255, 255, 10), kRound, 0, 1.0f);
    }
    // The four corner fixings, each a machined disc with a slot at its own
    // angle - four identical screws read as a repeated sprite, and a real
    // panel's never line up.
    {
        const float screwR = 15.0f;
        const float screwIn = 26.0f;
        const ImVec2 screws[4] = {
            ImVec2(faceTL.x + screwIn, faceTL.y + screwIn),
            ImVec2(faceBR.x - screwIn, faceTL.y + screwIn),
            ImVec2(faceTL.x + screwIn, faceBR.y - screwIn),
            ImVec2(faceBR.x - screwIn, faceBR.y - screwIn)};
        const float slotDeg[4] = {28.0f, -14.0f, -40.0f, 12.0f};
        for (int i = 0; i < 4; ++i) {
            const ImVec2 c = screws[i];
            // An eccentric highlight rather than a flat disc: the design lights
            // every round part from 35%/30%, and a centred one reads as a hole.
            dl->AddCircleFilled(c, screwR, IM_COL32(27, 28, 22, 255), 24);
            dl->AddCircleFilled(ImVec2(c.x - screwR * 0.18f, c.y - screwR * 0.22f),
                                screwR * 0.72f, IM_COL32(58, 59, 50, 255), 24);
            dl->AddCircleFilled(ImVec2(c.x - screwR * 0.26f, c.y - screwR * 0.30f),
                                screwR * 0.40f, IM_COL32(74, 75, 64, 255), 24);
            dl->AddCircle(c, screwR, IM_COL32(13, 14, 10, 255), 24, 2.0f);
            const float a = slotDeg[i] * 3.14159265f / 180.0f;
            const float sx = std::cos(a) * 7.0f;
            const float sy = std::sin(a) * 7.0f;
            dl->AddLine(ImVec2(c.x - sx, c.y - sy), ImVec2(c.x + sx, c.y + sy),
                        IM_COL32(12, 13, 9, 255), 4.0f);
        }
    }

    // THE DESIGN'S GRID, in its own coordinates. Content box 27..1053; a 34 px
    // deck gap; the upper deck 61..566 as columns 62 | 530 | 330 | 62 with 14
    // px gaps; the lower deck 584..769 as three equal 331.33 px bays with 16 px
    // gaps. These are measurements, not choices, which is why they are written
    // out rather than derived.
    // The reference's own metrics for everything that is FIXED - 27 px of
    // cabinet padding, a 34 px deck gap, an 18 px gap between the decks, a 185
    // px lower deck and 26 px beneath it - with the upper deck taking whatever
    // is left. Measurements, not choices, which is why they are written out.
    // TIGHTENED FROM THE REFERENCE'S OWN SPACING, deliberately and only here.
    //
    // The reference sets 27 px of cabinet padding, a 34 px gap above the upper
    // deck, 18 px between the decks and 26 px below the plinth. Those are
    // right on its fixed 1080x822 slab; on a window half as tall again they
    // are four bands of empty case stacked around the one thing anybody is
    // looking at. Every one of them is cut to what still reads as machining,
    // and the height that frees goes to the upper deck - which is to say, to
    // the tube.
    const float instLeft = faceTL.x + cabPad;
    const float instRight = faceBR.x - cabPad;
    const float plinthBotY = faceBR.y - cabPad - deckPadY;
    const float plinthTopY = plinthBotY - lowerH;
    const float instTop = faceTL.y + cabPad + deckPadY;
    const float instBot = plinthTopY - deckPadY;
    const float bayGap = 16.0f * scale;
    // The plinth's three bays span the same width as the deck above them, so
    // the unit has one edge down each side rather than two.
    const float bayW = (instRight - instLeft - bayGap * 2.0f) / 3.0f;

    // The deck fills the cabinet exactly, because the cabinet was sized to it.
    const float deckL = instLeft;
    const float innerL = deckL + kGaugeW + innerGap;
    const float innerR = instRight - kGaugeW - innerGap;

    // Outboard gauges. Both are fed by a real measurement and say which; the
    // left one has a reading only when the pipeline is running, and draws its
    // scale with no bar when it does not.
    const bool running = pipeline_.running();
    const double sigDb = pipeline_.signalPowerDb();
    const bool haveSig = running && std::isfinite(sigDb);
    // -100..0 dBFS across the scale, which is the range the rest of this
    // application's meters already work in.
    const double sigFrac = haveSig ? (sigDb + 100.0) / 100.0 : 0.0;
    char sigTxt[16];
    std::snprintf(sigTxt, sizeof(sigTxt), "%.0f", sigDb);
    cascade::gui::drawScopeGauge(dl, ImVec2(deckL, instTop),
                                 ImVec2(deckL + kGaugeW, instBot), "SIG",
                                 sigFrac, haveSig, sigTxt);

    // THE RIGHT GAUGE IS THE SELECTED AIRCRAFT'S ALTITUDE.
    //
    // It began as a track count, which duplicated the number already in the
    // scope's own corner; it then became the HIGHEST contact, which was a
    // reading nobody had asked for and - the point - was indistinguishable on
    // the face from the altitude of whatever the user had just clicked. Asked
    // for directly: it should show the selected craft's altitude.
    //
    // WITH NOTHING SELECTED IT READS NOTHING. A gauge that silently switched
    // to "the highest in the sky" when no target was picked would be answering
    // a different question in the same needle, and there would be no way to
    // tell which question from looking at it. The count of what is being heard,
    // and how high the traffic is, live on the HOME screen where they can be
    // labelled.
    std::size_t aircraft = 0;
    for (const cascade::core::HostTrack& ht : pluginUi_.tracks()) {
        if (ht.t.kind == CASCADE_TRACK_AIRCRAFT) { ++aircraft; }
    }

    double topAltM = 0.0;
    bool haveAlt = false;
    const std::string& selId = scope_.selectedId();
    if (!selId.empty()) {
        for (const cascade::core::HostTrack& ht : pluginUi_.tracks()) {
            if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
            if (selId != ht.t.id) { continue; }
            if (std::isfinite(ht.t.altM)) {
                topAltM = ht.t.altM;
                haveAlt = true;
            }
            break;
        }
    }
    char altTxt[16];
    if (haveAlt) {
        std::snprintf(altTxt, sizeof(altTxt), "FL%03d",
                      static_cast<int>(topAltM * 3.28084 / 100.0));
    } else if (!selId.empty()) {
        // Selected, but the aircraft has not reported an altitude - which is a
        // different statement from nothing being selected, and reads as one.
        std::snprintf(altTxt, sizeof(altTxt), "NO ALT");
    } else {
        std::snprintf(altTxt, sizeof(altTxt), "--");
    }
    // 13,000 m of full scale - a little above the ceiling of everything with a
    // transponder, so an airliner at cruise sits high on the bar without ever
    // pinning it.
    const float deckR = instRight;
    cascade::gui::drawScopeGauge(dl, ImVec2(deckR - kGaugeW, instTop),
                                 ImVec2(deckR, instBot), "ALT",
                                 haveAlt ? topAltM / 13000.0 : 0.0, haveAlt, altTxt);

    // The scope and its panel fill everything between the gauges.

    ImGui::SetCursorScreenPos(ImVec2(innerL, instTop));
    scope_.draw(innerR - innerL, instBot - instTop, pluginUi_.tracks(), &basemap_,
                &trackInfo_);

    // --- the plinth ---------------------------------------------------------
    // THREE EQUAL BAYS, which is what the reference has and what the previous
    // "a third of the width each, minus a pad" arithmetic did not produce: the
    // plate ended up wider than the knob bay and the power bay wider again.
    const float plinthTop = plinthTopY;
    const float plinthMid = (plinthTopY + plinthBotY) * 0.5f;

    // The maker's plate, from the icon compiled into this binary - the same
    // pixels Windows uses for the taskbar, so the badge cannot drift from the
    // application's own identity.
    {
        const ImVec2 pTL(instLeft, plinthTop);
        const ImVec2 pBR(instLeft + bayW, plinthBotY);
        cascade::gui::addScopeBay(dl, pTL, pBR, true);
        // Uploaded on first use rather than at start-up: a user who never
        // opens the scope never pays for it, and here the GL context is
        // certainly current.
        if (scopeBadgeTex_ == 0u) {
            glGenTextures(1, &scopeBadgeTex_);
            glBindTexture(GL_TEXTURE_2D, scopeBadgeTex_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cascade::gui::icon::kSize48,
                         cascade::gui::icon::kSize48, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         cascade::gui::icon::kPixels48);
        }
        // TOP-LEFT, not centred: the reference sets the wordmark against the
        // bay's top-left corner with the counters along the foot, and a
        // nameplate floating in the middle of its panel reads as a label
        // rather than as something stamped into the case.
        const float padL = 18.0f * scale;
        const float padT = 16.0f * scale;
        if (scopeBadgeTex_ != 0u) {
            const float side = std::max(20.0f, 34.0f * scale);
            dl->AddImage(static_cast<ImTextureID>(scopeBadgeTex_),
                         ImVec2(pTL.x + padL, pTL.y + padT),
                         ImVec2(pTL.x + padL + side, pTL.y + padT + side));
            // The wordmark, set as large as the bay allows and baseline-aligned
            // with the mark beside it.
            const float wordPx = std::max(16.0f, 30.0f * scale);
            dl->AddText(ImGui::GetFont(), wordPx,
                        ImVec2(pTL.x + padL + side + 10.0f * scale,
                               pTL.y + padT + side * 0.5f - wordPx * 0.62f),
                        IM_COL32(223, 226, 205, 255), "FOX");
            // The diamond the reference sets after the wordmark - a rotated
            // square, in the brand's own green.
            const float dSide = std::max(6.0f, 11.0f * scale);
            const float dcx = pTL.x + padL + side + 10.0f * scale +
                              ImGui::GetFont()->CalcTextSizeA(wordPx, FLT_MAX, 0.0f,
                                                              "FOX").x +
                              10.0f * scale + dSide;
            const float dcy = pTL.y + padT + side * 0.5f;
            const ImVec2 dia[4] = {ImVec2(dcx, dcy - dSide), ImVec2(dcx + dSide, dcy),
                                   ImVec2(dcx, dcy + dSide), ImVec2(dcx - dSide, dcy)};
            dl->AddConvexPolyFilled(dia, 4, IM_COL32(122, 179, 58, 255));
            // The sub-line, widely tracked as an engraved plate is.
            const float subPx = std::max(9.0f, 11.0f * scale);
            dl->AddText(ImGui::GetFont(), subPx,
                        ImVec2(pTL.x + padL, pTL.y + padT + side + 8.0f * scale),
                        IM_COL32(127, 134, 108, 255), "& SCHIRMER INDUSTRIES");
        }

        // THE ODOMETER COUNTERS. The range the face is set to and the number of
        // aircraft on it, as mechanical drums - the reference design's, and the
        // one part of the plate that is an instrument rather than a nameplate.
        // Both repeat a number that is already in the scope's corners, which is
        // deliberate: the corners are inside the tube and are read while
        // looking AT the picture, and these are on the panel and are read while
        // reaching for the knob.
        const float drumH = std::max(20.0f, 38.0f * scale);
        const float drumW = drumH * 0.68f;
        const float capH = ImGui::GetTextLineHeight() + 4.0f;
        const float drumsY = pBR.y - 16.0f * scale - drumH - capH;
        if (drumsY > pTL.y + 4.0f && (pBR.x - pTL.x) > drumW * 8.5f) {
            // THE COUNTER READS THE RANGE TO THE SELECTED AIRCRAFT, measured
            // from the antenna - the one distance an operator actually wants
            // off a panel, and the number the ALT gauge beside it is already
            // reporting the other half of.
            //
            // WITH NOTHING SELECTED IT SHOWS THE SCOPE'S OWN RANGE SETTING,
            // and the caption says WHICH of the two it is. Two different
            // distances in the same four digits with one label would be
            // unreadable: "100" could be an aircraft a hundred miles out or a
            // face set to a hundred-mile sweep, and nothing on the panel would
            // separate them.
            int drumNm = scopeRangeNm_;
            const char* drumCap = "SET RANGE NM";
            const std::string& selId = scope_.selectedId();
            if (!selId.empty() && rxSet_) {
                for (const cascade::core::HostTrack& ht : pluginUi_.tracks()) {
                    if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
                    if (selId != ht.t.id) { continue; }
                    const cascade::gui::ScopePolar p = cascade::gui::scopeRelative(
                        rxLat_, rxLon_, ht.t.latDeg, ht.t.lonDeg);
                    if (std::isfinite(p.rangeNm)) {
                        drumNm = static_cast<int>(p.rangeNm + 0.5);
                        drumCap = "TGT RANGE NM";
                    }
                    break;
                }
            }
            const float x0 = pTL.x + 18.0f * scale;
            // FOUR DRUMS FOR THE RANGE since the ladder reached 1600 NM; three
            // would have shown "600" for the longest scale the face offers.
            cascade::gui::drawScopeDrums(dl, ImVec2(x0, drumsY), drumW, drumH, 4,
                                         drumNm, drumCap);
            cascade::gui::drawScopeDrums(
                dl, ImVec2(x0 + (drumW + 3.0f) * 4.0f + 26.0f * scale, drumsY), drumW,
                drumH, 3, static_cast<int>(aircraft), "TRACKS");
        }
    }

    // RANGE: the knob turns the same ladder the buttons on the bar do, and the
    // buttons stay because a knob cannot be reached from a keyboard.
    {
        const ImVec2 kTL(instLeft + bayW + bayGap, plinthTop);
        const ImVec2 kBR(kTL.x + bayW, plinthBotY);
        cascade::gui::addScopeBay(dl, kTL, kBR, true);

        // THE KNOB IS THE GAIN.
        //
        // Range already has three ways to set it - the wheel over the face, the
        // two stepper buttons on the bar, and the ladder they both walk - while
        // the gain, which is the ONLY control that changes how far this
        // receiver actually hears, had none at all once scope mode hid the left
        // rail. So the one physical control on the plinth drives the thing that
        // could not be reached, not the thing that could be reached three ways.
        //
        // It drives the FIRST gain stage the device reports, which on every
        // radio this has been run against is the one that matters - PGA on a
        // B200, TUNER on an RTL dongle - and prints that stage's name beneath
        // itself so it is never a mystery which one moved.
        //
        // AND IT REFUSES TO PRETEND. Under hardware AGC a manual gain would
        // silently fight the device, so the knob is dead and reads AUTO; with
        // no SoapySDR device open there is nothing to set and it reads N/A.
        // Neither is a knob that turns and does nothing.
        const float knobR = std::max(20.0f, 30.0f * scale);
        const ImVec2 knobC((kTL.x + kBR.x) * 0.5f, plinthMid + 8.0f * scale);
        const bool haveGain = soapy_ != nullptr && !soapyGainNames_.empty();
        const bool gainLive = haveGain && !soapyAgc_;

        // The scale, in whole decibels across the device's own travel.
        {
            int ticks[5];
            for (int i = 0; i < 5; ++i) {
                ticks[i] = static_cast<int>(
                    kSoapyGainMinDb +
                    (kSoapyGainMaxDb - kSoapyGainMinDb) * static_cast<float>(i) / 4.0f);
            }
            int sel = -1;
            if (gainLive) {
                const float span = kSoapyGainMaxDb - kSoapyGainMinDb;
                if (span > 0.0f) {
                    sel = static_cast<int>(
                        (soapyGainsDb_[0] - kSoapyGainMinDb) / span * 4.0f + 0.5f);
                    sel = std::clamp(sel, 0, 4);
                }
            }
            cascade::gui::drawScopeKnobTicks(dl, knobC, knobR, ticks, 5, sel);
        }

        char gainTxt[24];
        if (!haveGain) {
            std::snprintf(gainTxt, sizeof(gainTxt), "N/A");
        } else if (soapyAgc_) {
            std::snprintf(gainTxt, sizeof(gainTxt), "AUTO");
        } else {
            std::snprintf(gainTxt, sizeof(gainTxt), "%.0f dB",
                          static_cast<double>(soapyGainsDb_[0]));
        }
        // Where the gain sits on its own travel, so the pointer shows it.
        float gainFrac = 0.5f;
        if (haveGain && kSoapyGainMaxDb > kSoapyGainMinDb) {
            gainFrac = (soapyGainsDb_[0] - kSoapyGainMinDb) /
                       (kSoapyGainMaxDb - kSoapyGainMinDb);
        }
        const int gainSteps = cascade::gui::drawScopeKnob(dl, knobC, knobR, "GAIN",
                                                          gainTxt, gainLive, gainFrac);
        if (gainSteps != 0 && gainLive) {
            // Two decibels a detent: fine enough to find the knee between more
            // aircraft and more noise, coarse enough to cross the whole travel
            // in one comfortable sweep.
            float db = soapyGainsDb_[0] + static_cast<float>(gainSteps) * 2.0f;
            db = std::clamp(db, kSoapyGainMinDb, kSoapyGainMaxDb);
            if (db != soapyGainsDb_[0]) {
                soapyGainsDb_[0] = db;
                if (!soapy_->setGainDb(soapyGainNames_[0], static_cast<double>(db))) {
                    sourceError_ = soapy_->lastError();
                }
            }
        }
        // Which stage moved, or why nothing will.
        {
            const char* note = !haveGain   ? "no device"
                               : soapyAgc_ ? "auto gain is on"
                                           : soapyGainNames_[0].c_str();
            const ImVec2 nsz = ImGui::CalcTextSize(note);
            // BELOW THE READOUT, and measured off the same radius the readout
            // is, or the two land on each other - "30 dB" and "PGA" were
            // printing through one another at the default knob size.
            dl->AddText(ImVec2(knobC.x - nsz.x * 0.5f,
                               knobC.y + knobR * 1.30f + ImGui::GetTextLineHeight() +
                                   10.0f),
                        IM_COL32(127, 134, 108, 255), note);
        }
    }

    // POWER: the RECEIVER's run state. Not the application's, and not the way
    // out of this mode - both of those would be a button lying about what it
    // controls, and the way out is the bar above and Escape.
    {
        const ImVec2 wTL(instLeft + (bayW + bayGap) * 2.0f, plinthTop);
        const ImVec2 wBR(wTL.x + bayW, plinthBotY);
        cascade::gui::addScopeBay(dl, wTL, wBR, true);
        const ImVec2 c((wTL.x + wBR.x) * 0.5f, plinthMid + 8.0f * scale);
        // 48 px of lamp in a 96 px well, as the reference has it, so the button
        // is the size of a thing a hand reaches for rather than a marker.
        const float lampR = std::max(18.0f, 30.0f * scale);
        if (cascade::gui::drawScopePowerButton(dl, c, lampR, running)) {
            if (running) {
                pipeline_.stop();
            } else {
                pipeline_.start();
            }
        }
        const char* lbl = "POWER";
        const ImVec2 lsz = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(c.x - lsz.x * 0.5f,
                           c.y - lampR - ImGui::GetTextLineHeight() - 8.0f),
                    IM_COL32(207, 211, 188, 255), lbl);
        // What the lamp means, spelled out. A lit lamp with no word beside it
        // is a state carried by colour alone, which this design forbids.
        const char* state = running ? "ON LINE" : "STANDBY";
        const ImVec2 ssz = ImGui::CalcTextSize(state);
        dl->AddText(ImVec2(c.x - ssz.x * 0.5f, c.y + lampR + 8.0f),
                    IM_COL32(127, 134, 108, 255), state);
    }

    scopeRangeNm_ = scope_.rangeNm();
    // Same eviction discipline as the map pages: the scope records that it
    // asked, and the ONE endFrame() runs at the end of drawUi.
    if (scope_.askedForTiles()) { basemapUsedThisFrame_ = true; }
}

void AppWindow::placeFeatureWindow(int slot, float wantW, float wantH) {
    float x = 0.0f;
    float y = 0.0f;
    separateWindowAnchor(slot, x, y);
    // THE MONITORS, in the virtual-desktop coordinates ImGui and the window
    // manager both speak. Empty is a headless --frames run, where the main
    // viewport's own work area is the only honest answer - the same fallback
    // the map pages use.
    std::vector<ScreenRect> workAreas;
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    for (int i = 0; i < pio.Monitors.Size; ++i) {
        const ImGuiPlatformMonitor& m = pio.Monitors[i];
        workAreas.push_back(ScreenRect{m.WorkPos.x, m.WorkPos.y, m.WorkSize.x, m.WorkSize.y});
    }
    if (workAreas.empty()) {
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        workAreas.push_back(
            ScreenRect{mv->WorkPos.x, mv->WorkPos.y, mv->WorkSize.x, mv->WorkSize.y});
    }
    float w = wantW;
    float h = wantH;
    mapPlaceDefaultRect(x, y, w, h, workAreas);
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
}

void AppWindow::placeSavedFeatureWindow(int slot, int& x, int& y, int& w, int& h,
                                        float wantW, float wantH) {
    // A SAVED RECTANGLE ONLY COUNTS IF IT IS STILL SOMEWHERE. The config
    // sanitizer checks the four numbers are sane; it cannot know what monitors
    // exist, and a rectangle saved on a display that has since been unplugged
    // restores off-screen where ImGui's own clamp leaves a 19 px sliver of
    // title bar - a window that cannot be dragged back and whose unusable
    // geometry is then re-saved every frame. The map pages were bitten by
    // exactly this; the same two functions answer it here.
    std::vector<ScreenRect> workAreas;
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    for (int i = 0; i < pio.Monitors.Size; ++i) {
        const ImGuiPlatformMonitor& m = pio.Monitors[i];
        workAreas.push_back(ScreenRect{m.WorkPos.x, m.WorkPos.y, m.WorkSize.x, m.WorkSize.y});
    }
    if (workAreas.empty()) {
        // No platform monitor list: a headless --frames run. The main
        // viewport's own work area is the only honest answer.
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        workAreas.push_back(
            ScreenRect{mv->WorkPos.x, mv->WorkPos.y, mv->WorkSize.x, mv->WorkSize.y});
    }
    if (w > 0 && h > 0 && mapGeometryOnScreen(x, y, w, h, workAreas)) {
        // ...AND NO BIGGER THAN WHAT FITS WHERE IT SITS, so the resize grip
        // cannot end up off the bottom of a shorter screen. Only that overhang
        // is taken off it; a rectangle that fits is the user's own and is
        // restored untouched.
        mapClampRestoredSize(x, y, w, h, workAreas);
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(x), static_cast<float>(y)),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), static_cast<float>(h)),
                                 ImGuiCond_FirstUseEver);
        return;
    }
    placeFeatureWindow(slot, wantW, wantH);
}

void AppWindow::drawPluginStoreWindow() {
    // RESET FIRST, AND UNCONDITIONALLY. The fitted window reads this flag to
    // decide whether IT has to print the install/remove outcome, and a store
    // window that is closed prints nothing - so the reset has to happen on
    // every frame, before the open test, exactly as it used to happen before
    // the section's own header test.
    pluginBrowserDrawnThisFrame_ = false;
    if (!pluginBrowseOpen_) { return; }
    telemetryNotePanel("plugin store");

    // A REAL OPERATING SYSTEM WINDOW, by the same two devices the map pages
    // use: an opening rectangle that overhangs the main window, and
    // NoAutoMerge to say outright what the overhang only implies - a window
    // that FITS inside the main viewport is otherwise merged into it, and a
    // store dragged small would silently stop being its own window.
    ImGuiWindowClass storeClass;
    storeClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&storeClass);
    // Wide enough for the deck, the module list and the data plate side by
    // side - PluginStoreView refuses to lay out under 560 px and says so - and
    // clamped to the monitor it opens on.
    placeFeatureWindow(6, 1180.0f, 780.0f);
    // NO WINDOW PADDING, because the content is a CABINET: the brass has to
    // reach the frame the operating system drew, and four pixels of ImGui's
    // window ground all the way round it would read as a gap between the
    // instrument and its case.
    bool open = true;
    // The visible half of the title is the plate's own word; the ### half is
    // the stable identity, and it is NOT "###pluginstore" - that id belongs to
    // the rail row, and two widgets sharing one id is how a window's drag
    // state and a rail row's press end up in the same hash bucket. beginPage
    // draws the cabinet this window is, with its name and keys on the rail.
    const bool drawn = beginPage("Plugin store###pluginstorewindow", "PLUGIN STORE", &open);
    // The frame's close button is a real close: it puts the key on the rail
    // back to off, and that is the only state either of them reads.
    if (!open) { pluginBrowseOpen_ = false; }
    if (drawn) {
        // --- what the window is told, from where it is measured -------------
        cascade::gui::PluginStoreModel model;
        model.sourceUrl = pluginCatalogueUrl_;
        // NOT "the catalogue is empty". Nothing here contacts the origin until
        // the user asks, so before that every count would be a claim about
        // something nobody has looked at; the window's banner says IDLE
        // instead of printing a clean zero.
        model.haveCatalogue = !catalog_.empty();
        model.sourceStatus = catalogStatus_;
        model.sourceError = catalogError_;
        model.busy = catalogPending_ || installPending_;
        model.progress = pluginRepo_.progress();
        model.busyLabel = installPending_ ? ("downloading " + installBusyName_)
                          : catalogPending_ ? std::string("fetching the catalogue")
                                            : std::string();
        model.resultReport = installReport_;
        model.resultError = installError_;

        // The update plans, once for the whole list rather than once per row:
        // planUpdates walks the catalogue against the manifest, and asking it
        // per module would be that walk squared for no new information.
        const std::vector<cascade::core::PluginUpdate> updates = plannedPluginUpdates();

        model.modules.reserve(catalog_.size());
        for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
            const cascade::core::PluginCatalogEntry& e =
                catalog_[static_cast<std::size_t>(i)];
            cascade::gui::StoreModule sm;
            sm.id = e.id;
            sm.plate.name = e.name;
            sm.plate.version = e.version;
            sm.plate.maker = e.author;
            sm.plate.licence = e.licence;
            sm.plate.blurb = e.description.empty() ? e.summary : e.description;
            sm.plate.homepage = e.homepage;
            sm.plate.legalNotice = e.legalNotice;
            // THE ABI IS KNOWN FOR A CATALOGUE ROW, and both halves of the
            // comparison are stated so the plate can letter the mismatch
            // rather than the verdict.
            sm.plate.haveAbi = true;
            sm.plate.abiVersion = e.abiVersion;
            sm.plate.hostAbiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
            sm.plate.retirementFloor = e.minSupportedVersion;
            // "windows/x64, linux/x64" - the builds the catalogue publishes.
            for (const cascade::core::PluginPlatform& pf : e.platforms) {
                if (!sm.plate.platforms.empty()) { sm.plate.platforms += ", "; }
                sm.plate.platforms += pf.os + "/" + pf.arch;
            }
            const cascade::core::PluginPlatform* plat = e.thisPlatform();
            if (plat != nullptr && plat->sizeBytes > 0u) {
                // ADVISORY, AND ONLY WHEN STATED. A catalogue that publishes
                // no size gets haveSizeBytes false and the plate says it was
                // never told - never a clean zero, which is the opposite
                // claim.
                sm.plate.haveSizeBytes = true;
                sm.plate.sizeBytes = plat->sizeBytes;
            }
            // IS THERE A BUILD THIS MACHINE COULD RUN. A stable fact about the
            // entry - the exact-ABI test the loader uses, and an os/arch build
            // existing - deliberately not blockedReason, which also carries
            // transient states such as a transfer already in flight.
            sm.installableHere = e.compatible && plat != nullptr;
            // FITTED, by the SAME test the desktop has always used: the
            // sanitised file name against the host's records AND the manifest,
            // so a retired module still counts as fitted.
            sm.plate.fitted = catalogEntryInstalled(e);
            // ...and if it is fitted, what the host actually made of it. This
            // is the only place the catalogue row and the loaded record meet,
            // and it is what lets the store's plate report loaded/running/
            // refused instead of guessing from "the file is there".
            if (const cascade::core::LoadedPlugin* lp = installedPluginRecord(e)) {
                sm.plate.fileName =
                    std::filesystem::path(lp->path).filename().string();
                sm.plate.loaded = lp->loaded;
                sm.plate.refusalReason = lp->error;
                sm.plate.haveCapabilities = lp->loaded;
                sm.plate.capabilities = lp->capabilities;
                const std::string key = cascade::core::pluginKey(*lp);
                sm.plate.running = lp->loaded && !pluginIsStopped(key);
                if (lp->hostClient != nullptr) {
                    sm.plate.haveTuneGrant = true;
                    sm.plate.tuneGranted =
                        pluginUi_.tuneAllowed(cascade::core::PluginUi::tuneKey(*lp));
                }
            }
            // WHY FIT MAY NOT BE PRESSED, from the SAME predicate the key
            // itself is tested against when the press is applied - so the
            // sentence under the key and the key can never disagree.
            //
            // THE TICK BELONGS TO ONE MODULE, and the predicate is told so.
            // deck.legalAck is the acknowledgement for the SELECTED row and
            // for no other; passing it to every row would let a tick given to
            // the plugin the user just read about unblock the FIT key on a
            // different plugin's row, which is consent nobody gave. Every
            // other row is asked as unacknowledged, so a module with a notice
            // stays blocked until it is the one on the plate.
            const bool acked =
                (i == pluginStoreDeck_->selected) && pluginStoreDeck_->legalAck;
            sm.blockedReason = pluginInstallBlockedReason(i, acked);
            for (const cascade::core::PluginUpdate& u : updates) {
                if (u.id != e.id) { continue; }
                sm.updateToVersion = u.toVersion;
                sm.updateReason = u.reason;
                break;
            }
            model.modules.push_back(std::move(sm));
        }

        // --- the cabinet, and the plate as content --------------------------
        // Whether PluginStoreView::draw actually ran; see the request block
        // below for why that is not the same question as "the window drew".
        bool viewDrawn = false;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 tl = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (dl != nullptr && avail.x > 8.0f && avail.y > 8.0f) {
            const ImVec2 br(tl.x + avail.x, tl.y + avail.y);
            // THE CABINET IS THE PAGE'S OWN NOW - beginPage drew it, screws,
            // rail and keys - so the plate goes straight into the well.
            const float margin = 0.0f;
            const ImVec2 pTL(tl.x + margin, tl.y + margin);
            const ImVec2 pBR(br.x - margin, br.y - margin);
            float bodyTop = pTL.y;
            if (pBR.x > pTL.x + 32.0f && pBR.y > pTL.y + 32.0f) {
                // THE PLATE STAYS AS CONTENT, which is the design's own
                // arrangement - and the mock's minimise/maximise/close buttons
                // beside it do not, because this is a real operating system
                // window with the frame the operating system drew.
                bodyTop = cascade::gui::addBenchPlate(dl, pTL, pBR, "PLUGIN STORE");
            }
            const float pad = 8.0f;
            const float faceW = pBR.x - pTL.x - pad * 2.0f;
            const float faceH = pBR.y - pad - bodyTop;
            if (faceW > 40.0f && faceH > 40.0f) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::SetCursorScreenPos(ImVec2(pTL.x + pad, bodyTop));
                ImGui::BeginChild("##storeface", ImVec2(faceW, faceH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                pluginStoreView_->draw(faceW, faceH, model, *pluginStoreDeck_);
                // THE CURSOR THE VIEW LEAVES BEHIND HAS TO BE DECLARED. Its
                // last statement moves the cursor to the foot of what it drew
                // so a caller can carry on below it, and a cursor moved past a
                // window's content extent with no item after it is exactly
                // what ImGui's own check reports: "Code uses SetCursorPos() to
                // extend window/parent boundaries. Please submit an item e.g.
                // Dummy() afterwards." It printed once per frame into the
                // bounded run's output, which is the contract those runs have
                // to keep clean. A zero-size Dummy is that item.
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ImGui::EndChild();
                viewDrawn = true;
            } else {
                ImGui::SetCursorScreenPos(ImVec2(pTL.x + pad, bodyTop + pad));
                ImGui::TextDisabled("Too small - drag the window larger.");
            }
        }

        // --- what the window ASKED for --------------------------------------
        // THE VIEW ASKS, THIS APPLIES, and every one of these re-tests its own
        // gate: a request is raised inside a draw and answered after it, and
        // the state it was raised against can have moved on in between.
        //
        // ONLY IF THE VIEW ACTUALLY DREW THIS FRAME. Its four requests are
        // cleared at the top of ITS draw and nowhere else, so a frame that
        // skipped the draw - the window dragged too small, which is a state a
        // user can hold with the mouse button down - would still be holding
        // last frame's answers and would apply them again, once per frame. A
        // second install of the module the user fitted a moment ago is not a
        // theoretical fault; the transfer gate would refuse most of them and
        // the first gap between two transfers would let one through.
        //
        // ...AND THE SAME TEST ANSWERS THE OTHER QUESTION. "Drawn" means the
        // PANEL drew, not merely that the window did: the fitted window reads
        // this flag to decide whether IT prints the install/remove outcome,
        // and a store window dragged too small to lay out has printed nothing.
        // It must not be the one claiming to have shown it, or a failed remove
        // would appear on neither window.
        pluginBrowserDrawnThisFrame_ = viewDrawn;
        if (!viewDrawn) {
            endPage();
            return;
        }
        if (pluginStoreView_->checkNowRequested()) { startCatalogFetch(); }
        if (pluginStoreView_->cancelRequested()) { pluginRepo_.cancel(); }
        const int fitIdx = pluginStoreView_->fitRequested();
        if (fitIdx >= 0 && fitIdx < static_cast<int>(catalog_.size())) {
            // RE-TESTED, NOT TRUSTED. The view only offers the key where the
            // reason was empty, but the predicate covers a transfer in flight
            // and an acknowledgement that may have been cleared since the key
            // was drawn - and this is the single gate the web control path
            // goes through as well. The acknowledgement is read the same way
            // the model built it: it belongs to the SELECTED module and to no
            // other, so a fit asked for on any other row is asked for
            // unacknowledged.
            const std::string blocked = pluginInstallBlockedReason(
                fitIdx, fitIdx == pluginStoreDeck_->selected && pluginStoreDeck_->legalAck);
            if (blocked.empty()) {
                startInstall(catalog_[static_cast<std::size_t>(fitIdx)]);
            } else {
                installError_ = blocked;
            }
        }
        const int updIdx = pluginStoreView_->updateRequested();
        if (updIdx >= 0 && updIdx < static_cast<int>(catalog_.size())) {
            // The plan is looked up again rather than captured with the model:
            // planUpdates' entries point INTO catalog_, and the frame that
            // built the model is not the frame that acts on it.
            const std::string& id = catalog_[static_cast<std::size_t>(updIdx)].id;
            const std::vector<cascade::core::PluginUpdate> plans = plannedPluginUpdates();
            for (const cascade::core::PluginUpdate& u : plans) {
                if (u.id == id) {
                    startUpdate(u);
                    break;
                }
            }
        }
    }
    endPage();
}

void AppWindow::drawFittedModulesWindow() {
    if (!fittedWindowOpen_) { return; }
    telemetryNotePanel("fitted modules");

    ImGuiWindowClass fittedClass;
    fittedClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&fittedClass);
    // WHERE THE USER LEFT IT, and only if that place still exists on this
    // machine; otherwise a different slot from the store's, so opening both
    // for the first time does not stack one exactly on the other.
    placeSavedFeatureWindow(7, fittedWinX_, fittedWinY_, fittedWinW_, fittedWinH_,
                            1060.0f, 720.0f);
    bool open = true;
    const bool drawn =
        beginPage("Fitted modules###fittedmoduleswindow", "FITTED MODULES", &open);
    if (!open) { fittedWindowOpen_ = false; }
    // READ BACK EVERY FRAME, which is the whole of the persistence: ImGui's
    // own .ini is switched off in this application, so unless the rectangle is
    // copied out here and into AppConfig it exists only until the process
    // ends. The POSITION is read even when Begin answered false - a collapsed
    // window is not gone and can still be dragged, and freezing its position
    // would lose a move made while it was rolled up - but the SIZE is not,
    // because a collapsed window reports its title-bar-only height and
    // persisting that would corrupt the real one.
    // ...and NOT while the window is maximised or rolled up by its own keys:
    // that rectangle is the key's, and persisting it would reopen the window
    // filling a monitor it may no longer be on.
    if (!pageGeometryTransient("Fitted modules###fittedmoduleswindow")) {
        const ImVec2 wpos = ImGui::GetWindowPos();
        fittedWinX_ = static_cast<int>(wpos.x);
        fittedWinY_ = static_cast<int>(wpos.y);
        if (drawn) {
            const ImVec2 wsize = ImGui::GetWindowSize();
            fittedWinW_ = static_cast<int>(wsize.x);
            fittedWinH_ = static_cast<int>(wsize.y);
        }
    }
    if (drawn) {
        // --- what the window is told ----------------------------------------
        cascade::gui::FittedModulesModel model;
        model.directory = pluginDir_;
        // GATES FED FOR EVERY MODULE AT ONCE, which is why it is stated on the
        // panel rather than left to be inferred from four idle rows.
        model.receiverRunning = pipeline_.running();
        const std::vector<cascade::core::DecoderStatus> status = pluginRunner_.status();
        const std::vector<cascade::core::LoadedPlugin>& list = pluginHost_.plugins();
        model.modules.reserve(list.size());
        for (const cascade::core::LoadedPlugin& p : list) {
            const std::string file = cascade::core::pluginKey(p);
            // THE RUNNER'S OWN SENTENCE, quoted rather than rewritten, and
            // matched by KEY rather than by display name - two installed
            // modules may legitimately print the same name.
            std::string idleDetail;
            for (const cascade::core::DecoderStatus& s : status) {
                if (s.key != file) { continue; }
                if (s.reason == cascade::core::DecoderIdleReason::Running) { continue; }
                idleDetail = s.detail;
                break;
            }
            cascade::gui::FittedModule m = cascade::gui::makeFittedModule(
                p, pluginIsStopped(file), pluginRunner_.isFeeding(file),
                std::move(idleDetail),
                pluginUi_.tuneAllowed(cascade::core::PluginUi::tuneKey(p)));
            // THE ONLY SIZE THERE IS. No descriptor carries one, so it can
            // only come from stat-ing the file; a failure leaves it at 0,
            // which the shared plate reads as "not measured" and never draws
            // as a clean zero.
            std::error_code sizeEc;
            const std::uintmax_t bytes = std::filesystem::file_size(p.path, sizeEc);
            if (!sizeEc) { m.sizeBytes = static_cast<std::uint64_t>(bytes); }
            model.modules.push_back(std::move(m));
        }
        // ONLY WHEN THE STORE DID NOT PRINT IT. installReport_/installError_
        // are written by an install AND by a remove, so both windows can hold
        // the same sentence; printing it in both reads as two separate
        // outcomes, and printing it in neither loses a failed remove entirely.
        if (!pluginBrowserDrawnThisFrame_) {
            model.report = installReport_;
            model.error = installError_;
        }

        // --- the cabinet ----------------------------------------------------
        // NO TITLE PLATE HERE: drawFittedModulesPanel draws its own FITTED
        // MODULES plate as the first thing in its layout, and a second plate
        // above it would be the window's name twice.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 tl = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        cascade::gui::FittedModulesAction act;
        if (dl != nullptr && avail.x > 8.0f && avail.y > 8.0f) {
            const ImVec2 br(tl.x + avail.x, tl.y + avail.y);
            // THE CABINET IS THE PAGE'S OWN NOW - beginPage drew it, screws,
            // rail and keys - so the face goes straight into the well.
            const float margin = 0.0f;
            const ImVec2 pTL(tl.x + margin, tl.y + margin);
            const ImVec2 pBR(br.x - margin, br.y - margin);
            const float pad = 8.0f;
            const float faceW = pBR.x - pTL.x - pad * 2.0f;
            const float faceH = pBR.y - pTL.y - pad * 2.0f;
            if (faceW > 40.0f && faceH > 40.0f) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::SetCursorScreenPos(ImVec2(pTL.x + pad, pTL.y + pad));
                ImGui::BeginChild("##fittedface", ImVec2(faceW, faceH),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                act = cascade::gui::drawFittedModulesPanel(*fittedDeck_, model);
                ImGui::EndChild();
            } else {
                ImGui::SetCursorScreenPos(ImVec2(pTL.x + pad, pTL.y + pad));
                ImGui::TextDisabled("Too small - drag the window larger.");
            }
        }

        // --- what the frame's keys asked for --------------------------------
        // APPLIED AFTER THE PANEL, never inside it: every one of these
        // rebuilds the plugin set - rescan unloads and re-loads every module,
        // a stop or a start rebuilds every instance, a remove deletes a file
        // and rescans - and the vector the panel was drawn from is the vector
        // they replace.
        switch (act.kind) {
            case cascade::gui::FittedModulesAction::Kind::Rescan:
                rescanPlugins();
                break;
            case cascade::gui::FittedModulesAction::Kind::Start:
                setPluginStopped(act.file, false);
                break;
            case cascade::gui::FittedModulesAction::Kind::Stop:
                setPluginStopped(act.file, true);
                break;
            case cascade::gui::FittedModulesAction::Kind::Remove:
                removeInstalledPlugin(act.file);
                break;
            case cascade::gui::FittedModulesAction::Kind::SetTune:
                setPluginTuneAllowed(act.file, act.flag);
                break;
            case cascade::gui::FittedModulesAction::Kind::None:
                break;
        }
    }
    endPage();
}

void AppWindow::drawPluginWindows() {
    // THE STORE FIRST, THEN THE INVENTORY, and the order is load-bearing: the
    // store resets pluginBrowserDrawnThisFrame_ and sets it if it draws, and
    // the fitted window reads it to decide whether IT has to print the
    // install/remove outcome. That rule used to live in the rail's DECODE
    // group, where the two SECTIONS were drawn in this order; both are windows
    // now, so the rule moved here with them and is these two lines.
    //
    // Neither window opens by itself, and since 0.79.1 neither is restored
    // from the config either: the only thing that puts one on screen is its
    // rail key. (Until 0.79.1 the store and this window came back if they
    // were open at the last exit; the user asked for the application to
    // start on the main screen alone.)
    drawPluginStoreWindow();
    drawFittedModulesWindow();

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

    // --- the map pages ------------------------------------------------------
    // ONE PAGE PER PLUGIN THAT HAS A TRACK INSTANCE. The single "Map" window
    // this replaces drew every plugin's targets merged, so switching from the
    // Satellites plugin to ADS-B still showed "the satellite map". The page
    // set is decided by CAPABILITY, not content: an ADS-B page with no
    // aircraft decoded yet still exists, because "the page is there but
    // empty" answers the user's question and "the page is missing" does not.
    // Pages are created lazily here and never destroyed while the app runs;
    // rescanPlugins() leaves them in place.
    for (const std::string& name : pluginUi_.trackPluginNames()) {
        ensureMapPage(name);
    }
    // A page whose plugin is GONE (removed, or a rescan came back without it)
    // is pruned — geometry folded into the saved store first, so it comes
    // back where it was if the plugin returns. Erasing dead pages here rather
    // than in rescanPlugins is what lets live pages survive a rescan with
    // their view state intact (see there).
    {
        const std::vector<std::string>& live = pluginUi_.trackPluginNames();
        bool anyDead = false;
        for (const MapPage& pg : mapPages_) {
            if (std::find(live.begin(), live.end(), pg.plugin) == live.end()) {
                anyDead = true;
                break;
            }
        }
        if (anyDead) {
            syncMapPagesToSaved();
            mapPages_.erase(
                std::remove_if(mapPages_.begin(), mapPages_.end(),
                               [&live](const MapPage& pg) {
                                   return std::find(live.begin(), live.end(),
                                                    pg.plugin) == live.end();
                               }),
                mapPages_.end());
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < mapPages_.size(); ++pageIndex) {
        MapPage& page = mapPages_[pageIndex];


        // THIS PAGE'S TRACKS AND PATHS ONLY, filtered by the plugin tag each
        // HostTrack carries. Copies into reused scratch, bounded by the
        // per-plugin caps in PluginUi — a few dozen structs per frame, which
        // is why nothing here is measured or cached across frames.
        pageTracks_.clear();
        pagePaths_.clear();
        for (const cascade::core::HostTrack& ht : pluginUi_.tracks()) {
            if (ht.plugin == page.plugin) { pageTracks_.push_back(ht); }
        }
        for (const cascade::core::HostPath& hp : pluginUi_.paths()) {
            if (hp.plugin == page.plugin) { pagePaths_.push_back(hp); }
        }

        // IS THIS THE SATELLITE INSTRUMENT? Asked of the TRACK KIND, which is
        // the ABI's own answer to "what is this", and never of the plugin's
        // display name - that is third-party text and matching on it would be
        // a guess dressed as a rule. Every track this page holds has to be a
        // satellite: a source reporting satellites AND aircraft is not the
        // window the design describes, and drawing it as one would put an
        // orbital altitude ladder under an aeroplane.
        //
        // Sticky (see MapPage::satellite): a propagator with nothing to report
        // for one frame must not throw the window's whole layout away and
        // rebuild it on the next.
        if (!page.satellite && !pageTracks_.empty()) {
            bool everySatellite = true;
            for (const cascade::core::HostTrack& ht : pageTracks_) {
                if (ht.t.kind != CASCADE_TRACK_SATELLITE) {
                    everySatellite = false;
                    break;
                }
            }
            page.satellite = everySatellite;
        }
        // COUNTED HERE, WHERE THE FILTER ALREADY RAN, and counted for closed
        // pages too - a closed page's count is exactly what its rail row is
        // for. One count, so the rail chip and the window's own TARGETS
        // heading cannot disagree.
        page.visibleCount = cascade::core::visibleTrackCount(pageTracks_);

        // A PAGE NEVER OPENS ITSELF. Until 0.79.1 it did, as an edge - nothing
        // -> something opened it, a close held until the air went quiet - and
        // the user asked for the application to show nothing but the main
        // screen unless they open it. The count above still runs for a closed
        // page: its rail row carries it, and that row is the invitation.
        if (!page.open) { continue; }
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
        if (page.w > 0 && page.h > 0 &&
            mapGeometryOnScreen(page.x, page.y, page.w, page.h, workAreas)) {
            // ...AND NO BIGGER THAN WHAT FITS WHERE IT SITS. A rectangle saved
            // on a taller display is reachable by its title bar and still too
            // tall here, which puts the resize grip off the bottom: the window
            // can be dragged but not shrunk, and the oversized geometry is then
            // written back to the config every frame. Only that overhang is
            // taken off it - a rectangle that fits is the user's own and is
            // restored untouched.
            mapClampRestoredSize(page.x, page.y, page.w, page.h, workAreas);
            // The page's own rectangle, from the config (or the staggered
            // legacy seed ensureMapPage gave it). FirstUseEver, so it is where
            // the window OPENS and never fights a later drag.
            ImGui::SetNextWindowPos(
                ImVec2(static_cast<float>(page.x), static_cast<float>(page.y)),
                ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(
                ImVec2(static_cast<float>(page.w), static_cast<float>(page.h)),
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
            // The page's index is the anchor slot, so several fresh pages
            // cascade instead of stacking exactly on top of one another.
            separateWindowAnchor(static_cast<int>(pageIndex), dx, dy);
            float dw = 0.0f;
            float dh = 0.0f;
            mapDefaultSize(dw, dh);
            mapPlaceDefaultRect(dx, dy, dw, dh, workAreas);
            ImGui::SetNextWindowPos(ImVec2(dx, dy), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(dw, dh), ImGuiCond_FirstUseEver);
        }
        // The ### suffix is the STABLE ImGui identity: the visible half of the
        // title carries the plugin's display name, and a future rename of that
        // display text would not orphan the window's drag or dock state.
        // The identity half must not contain '#': ImGui hashes the text
        // after the LAST "###", so a plugin display name carrying "###"
        // (third-party text — validated only as non-empty) could collide two
        // pages onto one window id. '#' becomes '_' in both halves; every
        // sane name is unchanged.
        std::string pageIdent = page.plugin;
        for (char& idc : pageIdent) {
            if (idc == '#') { idc = '_'; }
        }
        const std::string pageTitle = pageIdent + " Map###mapPage:" + pageIdent;
        // NO WINDOW PADDING ON THE SATELLITE PAGE, because its content is a
        // CABINET: the brass has to reach the frame the operating system drew,
        // and a four-pixel border of ImGui's window background all the way
        // round it would read as a gap between the instrument and its case.
        // Pushed before Begin, which is what reads it, and popped straight
        // after so nothing nested inherits it.
        // The page's name on its rail is the plugin's, in the bench's capitals.
        std::string railName = pageIdent + " MAP";
        for (char& rc : railName) {
            rc = static_cast<char>(std::toupper(static_cast<unsigned char>(rc)));
        }
        const bool pageDrawn = beginPage(pageTitle.c_str(), railName.c_str(), &page.open);
        if (!pageDrawn) {
            // COLLAPSED, NOT GONE. Begin() answers false for a collapsed
            // window, but the window still exists and can still be DRAGGED —
            // and this read-back is the application's only geometry
            // persistence (ImGui's .ini is off). Freezing it here meant a
            // move made while collapsed was lost if the app exited before the
            // window was ever expanded again. Position is read regardless;
            // the SIZE is not, because a collapsed window reports its
            // title-bar-only height and persisting that would corrupt the
            // real one.
            const ImVec2 wpos = ImGui::GetWindowPos();
            page.x = static_cast<int>(wpos.x);
            page.y = static_cast<int>(wpos.y);
        }
        if (pageDrawn) {
            // READ BACK EVERY FRAME, which is the whole of the persistence:
            // ImGui's own .ini is switched off in this application, so unless
            // the size is copied out here and into AppConfig it exists only
            // until the process ends. Rounded to whole pixels because that is
            // what a window manager deals in and what the config stores.
            // ...and NOT while maximised or rolled up by the page's own keys:
            // that rectangle is the key's, not the user's.
            if (!pageGeometryTransient(pageTitle.c_str())) {
                const ImVec2 wpos = ImGui::GetWindowPos();
                const ImVec2 wsize = ImGui::GetWindowSize();
                page.x = static_cast<int>(wpos.x);
                page.y = static_cast<int>(wpos.y);
                page.w = static_cast<int>(wsize.x);
                page.h = static_cast<int>(wsize.y);
            }
            // TWO KINDS OF MAP PAGE, AND THE SATELLITE ONE IS A WHOLE
            // INSTRUMENT. Every control below - fit, the receiver position,
            // the coverage and trail switches, the target list - exists on the
            // satellites window too, inside it, captioned and with its blocked
            // states explained. Drawing this bar above that panel would be the
            // same controls twice, in two idioms, disagreeing about which is
            // the real one.
            if (page.satellite) {
                drawSatelliteMapBody(page);
            } else {
                if (ImGui::SmallButton("Fit")) { page.view->requestFitToTracks(); }
                ImGui::SameLine();
                drawRxPositionEntry();
                ImGui::SameLine();
                // COUNTED THE WAY THEY ARE DRAWN, and counted for THIS page: the
                // number beside an ADS-B map must be its aircraft, not the whole
                // receiver's targets. A plugin that never evicts keeps reporting
                // targets the staleness rule has dropped, and a count of
                // everything reported over a map showing only what is live is a
                // number that contradicts the picture beside it.
                const std::size_t shown =
                    cascade::core::visibleTrackCount(pageTracks_);
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
                // THE TRAIL SWITCHES, on this row because they answer the same
                // kind of question the coverage one does - what else the map draws
                // besides the targets themselves - and because a control the user
                // has to open a menu to find is a control they will not find.
                // Applied to EVERY page below, not just this one: how a trail is
                // drawn is a preference, and two pages disagreeing about it would
                // be two answers to one question.
                ImGui::Checkbox("Trails", &mapTrails_);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Draw the lines plugins publish: flight trails, predicted\n"
                        "ground tracks and footprints. Off draws targets only.");
                }
                ImGui::SameLine();
                // DISABLED WHEN THERE ARE NO TRAILS, because a colour control for
                // a hidden thing is a lie: ticking it would change nothing on
                // screen and the user would be left wondering which of the two
                // settings was broken. The value itself is untouched, so turning
                // trails back on restores the colouring choice that was made.
                ImGui::BeginDisabled(!mapTrails_);
                ImGui::Checkbox("Altitude colours", &mapTrailAltColours_);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        mapTrails_
                            ? "Colour each part of a trail by the altitude observed\n"
                              "there, using the same bands as the markers. Off draws\n"
                              "each trail in its target's single colour."
                            : "Turn Trails on to colour them by altitude.");
                }
                ImGui::SameLine();
                // A LIST, NOT MORE CHECKBOXES. The styles are alternatives, and a
                // control that cannot express "line and ribbon at once" is the
                // one that cannot be put into a state the renderer has no meaning
                // for. It also makes a third style a one-line change here.
                ImGui::BeginDisabled(!mapTrails_);
                ImGui::SetNextItemWidth(110.0f);
                const char* kTrailStyles[] = {"Line", "Ribbon"};
                if (mapTrailStyle_ < 0 || mapTrailStyle_ > 1) { mapTrailStyle_ = 0; }
                ImGui::Combo("##trailstyle", &mapTrailStyle_, kTrailStyles, 2);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "Line draws a thin trail. Ribbon draws a wider translucent\n"
                        "band, easier to follow over a detailed map, at the cost of\n"
                        "covering more of what is underneath.");
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
                const float listWidth =
                    std::min(300.0f, std::max(190.0f, mapAvail.x * 0.36f));
                ImGui::BeginChild("##tracklist", ImVec2(listWidth, 0.0f), true);
                drawTrackList(page, pageTracks_);
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
                page.view->setCoverage(coverageShow_ ? &coverage_ : nullptr);
                // Pushed every frame, like the coverage pointer beside it, so a
                // checkbox on ANY page reaches every page's map on the next one.
                page.view->setTrailOptions(mapTrails_, mapTrailAltColours_);
                page.view->setTrailStyle(mapTrailStyle_);
                page.view->draw(avail.x, avail.y, pageTracks_, pagePaths_,
                                &basemap_, &trackInfo_);
                if (credit) {
                    ImGui::TextDisabled("%s", basemap_.attribution().c_str());
                }
                // endFrame() is NOT called here: its eviction drops every tile
                // nothing asked for this frame, so calling it inside one page's
                // draw would let it evict a second open page's tiles mid-frame.
                // The one call per frame happens at the END OF drawUi, not at the
                // end of this loop, because the scope draws after it and asks for
                // tiles of its own.
                basemapUsedThisFrame_ = true;
            }

            // THE FOLLOW-INTERRUPT ASK. A drag on the map while a target is
            // followed no longer pans (MapView latches the attempt instead —
            // see the drag handler's comment for why panning there was a tug
            // of war the user always lost); it opens this prompt. Modal on
            // purpose: the answer changes what the very next drag does, so
            // nothing else should happen until it is given.
            if (page.view->followInterruptRequested()) {
                page.view->clearFollowInterruptRequest();
                // Opened in THIS page's ID stack, so two pages each carry
                // their own copy of the question about their own follow.
                ImGui::OpenPopup("Stop following?");
            }
            if (ImGui::BeginPopupModal("Stop following?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                if (page.view->followedId().empty()) {
                    // The follow ended some other way (list button, details
                    // window) while the prompt was up: the question no longer
                    // exists, so it must not linger waiting for an answer.
                    ImGui::CloseCurrentPopup();
                } else {
                    ImGui::Text("The map is following %s.",
                                page.view->followedId().c_str());
                    ImGui::TextUnformatted(
                        "Stop following it so the map can be moved freely?");
                    ImGui::Separator();
                    if (ImGui::Button("Stop following")) {
                        page.view->clearFollow();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Keep following")) { ImGui::CloseCurrentPopup(); }
                }
                ImGui::EndPopup();
            }
        }
        endPage();
    }

    // OUTSIDE the map pages on purpose, so it is a sibling of them rather
    // than something drawn inside one: a map window can be small, and a detail
    // view that ate the list would have traded one complaint for another.
    // Drawn whether or not any map page is open - a details window the user
    // has dragged to a second screen must not vanish because a map was shut.
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
        // THE WINDOW APPEARS WHEN THE USER OPENS IT FROM ITS ROW IN DECODE, and
        // not before - not when the decoder loads, not when a picture starts
        // arriving (0.79.1: the application starts on the main screen alone
        // and opens nothing by itself; until then a picture window appeared
        // on its own the moment a picture began, and a close was remembered
        // across launches). Opened before a picture exists it says it is
        // waiting, which is the truth and what the user asked to see.
        const std::string id = im.plugin + " image###image_" + im.plugin;
        if (!pluginWindows_.shown(id)) { continue; }
        telemetryNotePanel("image");
        // Its own operating system window, for the same reason as the map: a
        // received picture is something to put beside the radio, or on another
        // screen, not a panel inside it. Staggered per decoder so two plugins
        // producing pictures do not land exactly on top of each other.
        placeAsSeparateWindow(static_cast<int>(i) + 1);
        // A REAL p_open, so the frame's close key is not a lie: closing takes
        // the window out of pluginWindows_ and its row's lamp goes out.
        bool imageOpen = true;
        std::string railName = im.plugin + " IMAGE";
        for (char& rc : railName) {
            rc = static_cast<char>(std::toupper(static_cast<unsigned char>(rc)));
        }
        if (beginPage(id.c_str(), railName.c_str(), &imageOpen)) {
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
        endPage();
        if (!imageOpen) { pluginWindows_.hide(id); }
    }

    drawDecoderWindow();

    // Plugin-declared windows. Each gets its own, titled by the plugin, so two
    // plugins cannot collide in one panel.
    for (const cascade::core::HostPanel& p : pluginUi_.panels()) {
        // The plugin NAME is part of the ImGui id, not just the title: two
        // plugins may legitimately call their window the same thing, and
        // colliding ids would merge them into one window.
        const std::string id = p.title + "###panel_" + p.plugin;
        // Shown only once the user has opened it from its row in DECODE
        // (0.79.1). The size hint is given AFTER that decision: a hint left
        // behind by a skipped window would land on whatever Begin came next.
        if (!pluginWindows_.shown(id)) { continue; }
        ImGui::SetNextWindowSize(ImVec2(520.0f, 300.0f), ImGuiCond_FirstUseEver);
        bool panelOpen = true;
        std::string railName = p.title;
        for (char& rc : railName) {
            rc = static_cast<char>(std::toupper(static_cast<unsigned char>(rc)));
        }
        if (beginPage(id.c_str(), railName.c_str(), &panelOpen)) {
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
                            ImGui::TextColored(cascade::gui::theme::warning(), "%.*s",
                                               CASCADE_PANEL_CELL_CHARS, cell);
                        } else if ((r.flags & CASCADE_ROW_FLAG_GOOD) != 0u) {
                            ImGui::TextColored(cascade::gui::theme::good(), "%.*s",
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
        endPage();
        if (!panelOpen) { pluginWindows_.hide(id); }
    }
}

// ---------------------------------------------------------------------------
// The SATELLITES MAP window
//
// EVERYTHING FOR SATELLITES IS IN ONE WINDOW, and that is the whole design.
// The receiver's position, the overlay switches, the trail style, the coverage
// ring, the target register, the selected target's figures and the map itself
// all live in here; the main window's rail keeps a single switch that opens
// it, and nothing else. MapView::drawSatellitePanel draws the instrument, and
// this draws the case it is bolted into.
//
// THE CASE IS CONTENT; THE FRAME IS WINDOWS'. Torn-off windows in this
// application carry the operating system's own frame, deliberately - an
// earlier undecorated version left users hunting for the resize edges - so the
// minimise, maximise and close buttons and the drag bar are the platform's,
// and the design's brass shell, its four screws and its SATELLITES MAP plate
// are drawn inside them.
// ---------------------------------------------------------------------------

void AppWindow::drawSatelliteMapBody(MapPage& page) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tl = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (dl == nullptr || avail.x < 8.0f || avail.y < 8.0f) { return; }
    const ImVec2 br(tl.x + avail.x, tl.y + avail.y);

    // THE CABINET, FIRST AND UNDERNEATH, painted into the window's own draw
    // list before a single widget is submitted - ImGui renders a parent's list
    // ahead of its children's, so the brass, the bevel, the four screws and
    // the sunk well go down and every panel below is drawn into them. The same
    // call the main window's root makes, so the two cases are one object and
    // cannot drift apart.
    // THE CABINET IS THE PAGE'S OWN NOW - beginPage drew it, screws, rail and
    // keys, the same drawCabinet this used to call - so the plate goes
    // straight into the well.
    const float margin = 0.0f;
    const ImVec2 pTL(tl.x + margin, tl.y + margin);
    const ImVec2 pBR(br.x - margin, br.y - margin);

    // THE PLATE THE WINDOW IS NAMED BY, as content rather than as a title bar:
    // the same addBenchPlate the FUNCTION SELECT rail and the STATUS column
    // wear, so the window is lettered in the application's own engraving
    // rather than in a second style invented for it. It hands back the y below
    // its rule, which is where the instrument starts - a measurement rather
    // than a guess at how tall a title is.
    float bodyTop = pTL.y;
    if (pBR.x > pTL.x + 32.0f && pBR.y > pTL.y + 32.0f) {
        bodyTop = cascade::gui::addBenchPlate(dl, pTL, pBR, "SATELLITES MAP");
    }

    // --- THE VIEW STRIP: what the satellite panel does not carry -------------
    //
    // TWO CONTROLS THIS WINDOW LOST WHEN IT BECAME AN INSTRUMENT, and both
    // were reported. drawSatellitePanel draws the deck, the register and the
    // map; it has no fit key and no follow indicator, and the only place those
    // existed was the ELSE arm of this branch - the ordinary map page's little
    // toolbar - so the satellites window had neither.
    //
    //   FIT and WHOLE WORLD. A satellites window that fits to a single
    //   propagated target lands on a 24-degree patch of the planet with one
    //   marker in it (map_view.hpp's setProjection note names the same case),
    //   and until now there was no way back out: the whole-globe view is asked
    //   for ONCE, on the frame the page first pins its projection, and a wheel
    //   is not a control anybody finds. Two keys, because "show me everything
    //   plotted" and "show me the planet" are different requests and on this
    //   window the second is the one that rescues the first.
    //
    //   FOLLOWING, AND HOW TO STOP. A satellite can be followed from the
    //   target details window ("Follow" reaches the owning page's view), and
    //   the map then re-centres on it every frame and refuses to be dragged -
    //   with nothing on this window saying so and no way to undo it. The
    //   indicator is the followed id, in gold because it is a state the user
    //   should notice, and the key beside it is the way out.
    {
        constexpr float kStripPad = 8.0f;
        constexpr float kStripKeyH = 26.0f;
        constexpr float kFitW = 66.0f;
        constexpr float kWorldW = 108.0f;
        constexpr float kStopW = 126.0f;
        const float stripY = bodyTop + 4.0f;
        if (pBR.x > pTL.x + kStripPad * 2.0f + kFitW + kWorldW + 8.0f &&
            pBR.y > stripY + kStripKeyH + 24.0f) {
            dl->PushClipRect(ImVec2(pTL.x + 2.0f, stripY),
                             ImVec2(pBR.x - 2.0f, stripY + kStripKeyH), true);
            float x = pTL.x + kStripPad;
            // FIT IS DEAD WITH NOTHING PLOTTED, and says so by staying dead
            // rather than by disappearing: requestFitToTracks is ignored by a
            // draw with no tracks (see MapView::draw), so an enabled key would
            // be one that visibly does nothing.
            const std::size_t plotted = cascade::core::visibleTrackCount(pageTracks_);
            if (benchWordKey(dl, ImVec2(x, stripY), ImVec2(x + kFitW, stripY + kStripKeyH),
                             "FIT", plotted > 0u, "satfit")) {
                page.view->requestFitToTracks();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(plotted > 0u
                                      ? "Centre and zoom on everything plotted."
                                      : "Nothing is plotted, so there is nothing to fit.");
            }
            x += kFitW + 8.0f;
            if (benchWordKey(dl, ImVec2(x, stripY), ImVec2(x + kWorldW, stripY + kStripKeyH),
                             "WHOLE WORLD", true, "satworld")) {
                page.view->requestWholeWorld();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Back out until the whole planet is in the window.");
            }
            x += kWorldW + 12.0f;

            ImFont* sf = cascade::gui::fonts::ui();
            const float spx = cascade::gui::fonts::kTinySize;
            const std::string followed = page.view->followedId();
            if (!followed.empty()) {
                // THE KEY IS OFFERED WHEREVER IT FITS, AND THE INDICATOR IS
                // DRAWN WHETHER IT DOES OR NOT. A window dragged narrow is
                // exactly the case where the two must not fail together: the
                // whole fault being fixed here is a user who is following a
                // target and cannot see that they are, and a strip that
                // silently drew neither below some width would reintroduce it
                // at a width the user chooses. The key goes if there is no
                // room for it - the map's own drag prompt is still a way out -
                // and the sentence stays, clipped by the strip's clip rect
                // rather than dropped.
                float textRight = pBR.x - kStripPad;
                if (pBR.x - kStripPad - kStopW > x) {
                    const float stopX = pBR.x - kStripPad - kStopW;
                    textRight = stopX - 8.0f;
                    if (benchWordKey(dl, ImVec2(stopX, stripY),
                                     ImVec2(stopX + kStopW, stripY + kStripKeyH),
                                     "STOP FOLLOWING", true, "satunfollow")) {
                        page.view->clearFollow();
                    }
                }
                char line[96];
                std::snprintf(line, sizeof line, "FOLLOWING %s", followed.c_str());
                const ImVec2 ts = sf->CalcTextSizeA(spx, FLT_MAX, 0.0f, line);
                dl->AddText(sf, spx, ImVec2(x, stripY + (kStripKeyH - ts.y) * 0.5f),
                            cascade::gui::theme::kGold, line, nullptr,
                            (textRight > x) ? (textRight - x) : 1.0f);
            } else {
                dl->AddText(sf, spx,
                            ImVec2(x, stripY + (kStripKeyH - cascade::gui::fonts::kTinySize) *
                                                  0.5f),
                            cascade::gui::theme::kInkFaint,
                            "The map moves on its own only while a target is followed.",
                            nullptr, pBR.x - kStripPad - x);
            }
            dl->PopClipRect();
            bodyTop = stripY + kStripKeyH + 6.0f;
        }
    }

    // --- the deck's four shared settings, borrowed for the frame ------------
    // COPIED IN AND OUT RATHER THAN DUPLICATED. Coverage, trails, altitude
    // colours and trail style are AppConfig fields every map page reads, and
    // map_view.hpp is explicit that the caller owns them: two pages
    // disagreeing about how a trail is drawn would be two answers to one
    // question. The panel edits the deck in place; what it edited is written
    // straight back to the one copy that persists.
    page.deck.coverage = coverageShow_;
    page.deck.groundTracks = mapTrails_;
    page.deck.altitudeColours = mapTrailAltColours_;
    page.deck.trailStyle = mapTrailStyle_;

    const float pad = 8.0f;
    const float faceW = pBR.x - pTL.x - pad * 2.0f;
    const float faceH = pBR.y - pad - bodyTop;
    if (faceW > 40.0f && faceH > 40.0f) {
        // A CHILD SO THE PANEL'S CONTENT REGION IS EXACTLY THE PLATE'S INSIDE.
        // drawSatellitePanel lays itself out from GetContentRegionAvail, and
        // measured against the whole window it would run its map straight over
        // the cabinet's lower bevel and its screws. Transparent, because the
        // plate's own ground is already down; no scrollbar, because the panel
        // scrolls its register internally and an outer bar would be a second
        // one for the same content.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::SetCursorScreenPos(ImVec2(pTL.x + pad, bodyTop));
        ImGui::BeginChild("##satface", ImVec2(faceW, faceH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        // THE ACCUMULATOR IS PASSED WHETHER OR NOT THE OVERLAY IS ON. The
        // deck's own note reports what has been heard from this position even
        // while the ring is hidden, because "nothing heard yet" and "hidden"
        // are different facts and the window says which.
        page.view->drawSatellitePanel(page.deck, pageTracks_, pagePaths_, &coverage_,
                                      &trackInfo_);
        ImGui::EndChild();
    } else {
        // TOO SMALL TO DRAW HONESTLY, AND SAYING SO. A control deck squeezed
        // into forty pixels is a row of clipped words, which reads as a
        // broken window rather than as a small one.
        ImGui::SetCursorScreenPos(ImVec2(pTL.x + pad, bodyTop + pad));
        ImGui::TextDisabled("Too small - drag the window larger.");
    }

    coverageShow_ = page.deck.coverage;
    mapTrails_ = page.deck.groundTracks;
    mapTrailAltColours_ = page.deck.altitudeColours;
    mapTrailStyle_ = page.deck.trailStyle;

    // --- what the panel ASKED for -------------------------------------------
    // THE PANEL ASKS, THIS APPLIES. A receiver position reaches every map
    // page, the radar scope and the coverage accumulator, and the coverage
    // ring belongs to this window rather than to a view - knowledge the map
    // deliberately does not have. Both requests are latched rather than
    // edge-triggered inside draw(), so neither can be lost by a frame that
    // happened to skip; both are cleared here, after acting.
    if (page.view->receiverPositionRequested()) {
        applyReceiverPosition(page.view->requestedReceiverLatDeg(),
                              page.view->requestedReceiverLonDeg());
        page.view->clearReceiverPositionRequest();
    }
    if (page.view->coverageResetRequested()) {
        coverage_.reset();
        page.view->clearCoverageResetRequest();
    }
}

void AppWindow::drawPluginWindowRows() {
    // ONE ROW PER WINDOW A PLUGIN PUBLISHES - a decoder's picture, a plugin's
    // own panel - and these rows are the only way such a window reaches the
    // screen. Since 0.79.1 nothing opens by itself: the application starts on
    // the main screen alone, and a window is opened by pressing its row and
    // closed by its own key, which puts the row's lamp out. The chip says
    // what the window would show right now, so a row can be judged without
    // opening it: for a picture WAIT until the first one arrives, RX while
    // one is being received, IMG once one is complete; for a panel, how many
    // rows it holds.
    //
    // '#' BECOMES '_' IN THE ID HALF, as the map rows do it and for the same
    // reasons: a plugin's name and a panel's title are third-party text, ImGui
    // hashes what follows the LAST "###", and benchSwitchRow letters what
    // precedes the FIRST "##".
    auto ident = [](const std::string& s) {
        std::string out = s;
        for (char& c : out) {
            if (c == '#') { c = '_'; }
        }
        return out;
    };
    for (const cascade::core::HostImage& im : pluginImages_) {
        const std::string id = im.plugin + " image###image_" + im.plugin;
        const bool on = pluginWindows_.shown(id);
        const char* chip =
            (im.width == 0u || im.height == 0u) ? "WAIT" : (im.complete ? "IMG" : "RX");
        const std::string row = ident(im.plugin) + " image###imgrow:" + ident(im.plugin);
        if (benchSwitchRow(row.c_str(), on, chip, cascade::gui::theme::kPhosphor, on, true,
                           "Opens this decoder's picture window. WAIT until the first\n"
                           "picture arrives, RX while one is coming in, IMG when it is\n"
                           "complete. Nothing opens this window for you.")) {
            pluginWindows_.toggle(id);
        }
    }
    for (const cascade::core::HostPanel& p : pluginUi_.panels()) {
        const std::string id = p.title + "###panel_" + p.plugin;
        const bool on = pluginWindows_.shown(id);
        char chip[24];
        std::snprintf(chip, sizeof chip, "%d ROW", static_cast<int>(p.rows.size()));
        const std::string row =
            ident(p.title) + "###panelrow:" + ident(p.plugin) + ":" + ident(p.title);
        if (benchSwitchRow(row.c_str(), on, chip, cascade::gui::theme::kPhosphor, on, true,
                           "Opens this plugin's own window. Nothing opens it for you;\n"
                           "close it from its key and it stays closed.")) {
            pluginWindows_.toggle(id);
        }
    }
}

void AppWindow::drawSatelliteMapSection() {
    // THE WHOLE OF THE SATELLITE PRESENCE IN THE MAIN WINDOW: one row per
    // satellite page, and no satellite controls anywhere else on the rail.
    // The user asked for the instrument to be self-contained, so this is a
    // switch that puts the window on screen and reports what is in it - never
    // a second, smaller copy of the controls that window carries.
    bool any = false;
    for (MapPage& pg : mapPages_) {
        if (!pg.satellite) { continue; }
        any = true;
        // THE CHIP IS THE COUNT THE WINDOW ITSELF SHOWS - MapPage::visibleCount
        // is written where the page's tracks are filtered, so the rail and the
        // window's TARGETS heading cannot report two different numbers. The
        // LAMP is whether the window is open, which is the other half of what
        // a switch has to say about itself.
        char chip[24];
        std::snprintf(chip, sizeof chip, "%d TGT", static_cast<int>(pg.visibleCount));
        // The plugin's own display name, which is what its window is titled
        // with: two satellite sources installed would otherwise give two rows
        // reading the same word.
        //
        // '#' BECOMES '_' IN THE ID HALF, exactly as the window title does it
        // and for both of that rule's reasons: a display name is third-party
        // text, ImGui hashes what follows the LAST "###", and benchSwitchRow
        // letters what precedes the FIRST "##" - so a name carrying either
        // could collide two rows onto one id or truncate its own word.
        std::string ident = pg.plugin;
        for (char& idc : ident) {
            if (idc == '#') { idc = '_'; }
        }
        const std::string row = ident + " map###satmap:" + ident;
        if (benchSwitchRow(row.c_str(), pg.open, chip, cascade::gui::theme::kPhosphor,
                           pg.open, true,
                           "Opens the satellites window: receiver position, overlays,\n"
                           "trail style, coverage, the target register and the map.\n"
                           "Everything for satellites is in that one window.")) {
            pg.open = !pg.open;
        }
    }
    if (any) { return; }

    // --- blocked, and saying what would unblock it --------------------------
    // SEVERAL DIFFERENT REASONS THE ROW IS DEAD, and they must not read the
    // same. One is answered by waiting for the tracker already running to
    // report a satellite; the rest are answered by starting a module, by
    // reading why the host refused one, or - and only then - by installing
    // one. A single "unavailable" would send most of those users to the plugin
    // store for nothing.
    //
    // WHAT trackPluginNames() ACTUALLY ANSWERS. It is the list of track
    // sources PluginUi has INSTANTIATED, and rebuild skips an unloaded module
    // and a stopped one before it reaches that list - so an empty list is
    // equally true of a machine with no tracker and of one whose tracker the
    // user stopped a moment ago. This row said "No track source installed" for
    // both, while the Fitted modules window two keys away lettered that same
    // file STOPPED BY YOU. See gui/module_census.hpp.
    const bool anyTrackSource = !pluginUi_.trackPluginNames().empty();
    benchSwitchRow("Satellites map###satmapnone", false, "NONE",
                   cascade::gui::theme::kPhosphor, false, false, nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (anyTrackSource) {
        ImGui::TextWrapped(
            "No satellite targets reported yet. A tracker that publishes satellite "
            "positions gets its own window, and this key opens it.");
    } else {
        const cascade::gui::ModuleCensus census = cascade::gui::censusModules(
            pluginHost_.plugins(), CASCADE_CAP_TRACK_SOURCE,
            [this](const std::string& key) { return pluginIsStopped(key); });
        ImGui::TextWrapped(
            "%s", cascade::gui::trackSourceAbsenceNote(
                      census, "satellite positions",
                      "A satellite tracker plugin reports the positions this window "
                      "draws - install one from the plugin store above.")
                      .c_str());
    }
    ImGui::PopStyleColor();
}

void AppWindow::drawTrackList(MapPage& page,
                              const std::vector<cascade::core::HostTrack>& tracks) {
    // `tracks` is this page's plugin's tracks only — the caller filtered them
    // by the tag each HostTrack carries — so every row here belongs to the
    // page it is drawn in, and a click can only ever land on this page's map.
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
    const bool following = !page.view->followedId().empty();
    if (following) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("following %s", page.view->followedId().c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Stop following")) { page.view->clearFollow(); }
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
        const bool selected = (page.view->selectedId() == r.id);
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
            // CLICK = GO TO, on THIS page's map — the row and the map it
            // commands live in the same window, which is already open or this
            // code would not be running. One click centres the map on it; the
            // map only ever tightens the zoom, so clicking a flight while
            // already zoomed in does not throw the view back out.
            page.view->setSelected(ht.t.id);
            page.view->goTo(ht.t.latDeg, ht.t.lonDeg);
        }
        // Double click starts following, which is the natural escalation of
        // "take me there" and needs no extra control.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            page.view->setFollowed(ht.t.id);
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

const cascade::core::HostTrack* AppWindow::findVisibleTrack(const std::string& id) const {
    // FOUND BY ID EVERY FRAME. The host's track vector is rebuilt on every
    // poll, so a stored index or pointer would be describing a different
    // aircraft - or freed memory - within a frame or two. Only tracks the
    // staleness rule still shows count: a dropped target must read as "no
    // longer being heard", not as its last stale values.
    for (const cascade::core::HostTrack& ht : pluginUi_.tracks()) {
        if (!cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) {
            continue;
        }
        if (id == ht.t.id) { return &ht; }
    }
    return nullptr;
}

void AppWindow::drawTargetDetailsSection() {
    // WHICH TARGET: the one whose Details button was pressed wins (it is the
    // explicit ask), then the one being followed (the map is already glued to
    // it, so it is what the user is watching), then the last one clicked in
    // the list. The precedence means this section tracks whatever the user
    // most recently singled out, with no control of its own to manage.
    //
    // With one map page per plugin there can be several follows and
    // selections alive at once; the first page in load order that has one
    // answers, which is at least a stable choice the user can predict.
    std::string id = detailsTrackId_;
    // OPEN pages only, in both loops. A follow or selection living in a page
    // the user has CLOSED is a leftover, not an intent — an adversarial
    // review showed a stale follow on a closed ADS-B page outranking the AIS
    // vessel the user had just clicked, with the closed page's state
    // unreachable and uncancellable. What the user can see is what may speak
    // here.
    if (id.empty()) {
        for (const MapPage& pg : mapPages_) {
            if (pg.open && !pg.view->followedId().empty()) {
                id = pg.view->followedId();
                break;
            }
        }
    }
    if (id.empty()) {
        for (const MapPage& pg : mapPages_) {
            if (pg.open && !pg.view->selectedId().empty()) {
                id = pg.view->selectedId();
                break;
            }
        }
    }
    // THE CHIP IS THE CHOSEN TARGET'S OWN ID, cut to what a chip can hold -
    // an ICAO address or an MMSI is what identifies it and what the rest of
    // this section is about to describe. The lamp is lit only while that
    // target is STILL BEING HEARD (findVisibleTrack applies the staleness
    // rule), so a chip with the lamp out is the honest reading of "this is
    // what you singled out, and it has stopped transmitting".
    //
    // The precedence above is walked before the header rather than after it
    // because the chip has to be handed over as the row is drawn; it reads no
    // ImGui state and reorders nothing.
    const std::string detailChip = id.substr(0, 10);
    // Looked up ONCE and reused by the body below: the host's track vector is
    // rebuilt every poll and walking it twice a frame to answer the same
    // question would be paying for the chip twice.
    const cascade::core::HostTrack* found =
        id.empty() ? nullptr : findVisibleTrack(id);
    if (!benchSection("Target details", false, id.empty() ? "NONE" : detailChip.c_str(),
                      cascade::gui::theme::kPhosphor, found != nullptr)) {
        return;
    }

    if (id.empty()) {
        // Wrapped, not line-broken by hand: the menu column is narrower than a
        // comfortable sentence and hard-broken lines clipped at its edge.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(
            "No target chosen. Click a target in the map's list, press its "
            "Details button, or follow one - it will be shown here.");
        ImGui::PopStyleColor();
        return;
    }

    if (found == nullptr) {
        // Same honesty as the details window: said, not silently blanked.
        ImGui::TextUnformatted(id.c_str());
        ImGui::TextDisabled("No longer being heard.");
        return;
    }

    cascade::gui::drawTrackDetail(*found, &trackInfo_, rxSet_, rxLat_, rxLon_);
    ImGui::Separator();
    // The same two gestures the details window offers, so acting on what was
    // just read never requires finding the row it came from. Both land on the
    // page of the plugin that decoded THIS target — the track carries its
    // plugin's name — and open that page, because "go to on map" from here
    // is the one route to a page whose window is currently shut.
    if (ImGui::SmallButton("Go to on map")) {
        MapPage& pg = ensureMapPage(found->plugin);
        pg.view->setSelected(found->t.id);
        pg.view->goTo(found->t.latDeg, found->t.lonDeg);
        pg.open = true;
    }
    ImGui::SameLine();
    MapPage* owner = findMapPage(found->plugin);
    const bool following =
        owner != nullptr && owner->view->followedId() == found->t.id;
    if (ImGui::SmallButton(following ? "Stop following" : "Follow")) {
        if (following) {
            owner->view->clearFollow();
        } else {
            MapPage& pg = ensureMapPage(found->plugin);
            pg.view->setSelected(found->t.id);
            pg.view->setFollowed(found->t.id);
            pg.open = true;
        }
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
    if (beginPage("Target details", "TARGET DETAILS", &detailsOpen_,
                  ImGuiWindowFlags_AlwaysAutoResize)) {
        const cascade::core::HostTrack* found = findVisibleTrack(detailsTrackId_);

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
            // Routed to the page of the plugin that decoded this target, and
            // that page is opened — the details window outlives its map page
            // being closed, so the gesture must be able to bring it back.
            if (ImGui::Button("Go to on map")) {
                MapPage& pg = ensureMapPage(found->plugin);
                pg.view->setSelected(found->t.id);
                pg.view->goTo(found->t.latDeg, found->t.lonDeg);
                pg.open = true;
            }
            ImGui::SameLine();
            MapPage* owner = findMapPage(found->plugin);
            const bool following =
                owner != nullptr && owner->view->followedId() == found->t.id;
            if (ImGui::Button(following ? "Stop following" : "Follow")) {
                if (following) {
                    owner->view->clearFollow();
                } else {
                    MapPage& pg = ensureMapPage(found->plugin);
                    pg.view->setSelected(found->t.id);
                    pg.view->setFollowed(found->t.id);
                    pg.open = true;
                }
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Close")) { detailsOpen_ = false; }
    }
    endPage();
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

    // THE PRESET BUTTON OPENS THE MAP PAGE, EXPLICITLY. Pressing it is the
    // one unambiguous "show me this plugin" gesture the product offers (its
    // tooltip promises as much), and since 0.79.1 a page opens by such a
    // gesture and by nothing else - no page opens itself on its first
    // target any more, so this line is one of the ways a page is reached.
    // Guarded to track-capable plugins so a preset on a plain decoder does
    // not conjure an empty map window it never asked for.
    {
        const std::vector<std::string>& trackNames = pluginUi_.trackPluginNames();
        if (std::find(trackNames.begin(), trackNames.end(), p.name) != trackNames.end()) {
            MapPage& pg = ensureMapPage(p.name);
            pg.open = true;
        }
    }

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

std::size_t AppWindow::loadedDecoderCount() const {
    // THE RUNNER'S OWN TEST, and deliberately not a capability-bit test of its
    // own: PluginRunner::rebuild creates an instance when a module supplies a
    // decoder, an iqDecoder or an imageDecoder TABLE, and the host only fills
    // those pointers when the module declared the matching capability AND
    // supplied the table behind it (a declared capability with no table is a
    // refused load, PluginRejection::MissingDecoderApi). Asking the same
    // question the same way is what keeps this denominator and the runner's
    // activeCount numerator counting the same population.
    std::size_t n = 0;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        if (p.decoder != nullptr || p.iqDecoder != nullptr || p.imageDecoder != nullptr) {
            ++n;
        }
    }
    return n;
}

std::size_t AppWindow::fedDecoderCount() const {
    std::size_t n = 0;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        if (p.decoder == nullptr && p.iqDecoder == nullptr && p.imageDecoder == nullptr) {
            continue;
        }
        if (pluginRunner_.isFeeding(cascade::core::pluginKey(p))) { ++n; }
    }
    return n;
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
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
    ImGui::Text("Sound muted by %s", who.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop plugin##mute_banner")) {
        stopMutingPlugins(mutedByKeys_);
    }
}

void AppWindow::drawPluginTuneControls() {
    // THE HALF OF THE RECEIVER-CONTROL ROWS THE FITTED WINDOW CANNOT DRAW.
    //
    // That window carries a GRANT key on every module it lists, so a grant for
    // an installed module is set there and is deliberately not repeated here -
    // two controls on one permission is how two surfaces come to disagree
    // about who has it. What that window cannot show is a grant held by a
    // module it does not list: it lists what the host LOADED, and a module
    // that was removed or quarantined is not in that list while its permission
    // very much survives in the config. A grant the user can neither see nor
    // revoke is exactly the kind that must not exist, so those rows are here.
    //
    // The refusal notice is here for the same reason: it names a plugin by the
    // key PluginUi recorded, which may be one no longer installed, and it is
    // the line that turns "why is my tracker doing nothing" into a visible
    // one-click decision.
    //
    // A row is identified by its MODULE FILE, which is what the grant is keyed
    // on; a display name is the plugin's own to choose and two may share one.
    //
    // TWO REASONS A GRANT HAS NO KEY IN THAT WINDOW, AND THEY ARE NOT ONE
    // FACT. The window draws the grant key only on a module that LOADED
    // (plugins_view.cpp:1019), so a grant lands here either because no file of
    // that name is fitted at all - removed, or renamed aside by the version
    // policy - or because the file IS in the plugin folder and the host
    // REFUSED it, which that window letters REFUSED and prints the host's own
    // reason for. This surface called both "not currently installed", which of
    // the second is simply untrue: it sends the user to fetch a file that is
    // already on their disk, and it is a PERMISSION being described, which is
    // the worst kind of thing to be wrong about.
    struct StaleGrant {
        std::string file;
        // A record for this file exists and did not load. Empty capabilities
        // or full, the host has a reason recorded either way.
        bool refused = false;
    };
    std::vector<StaleGrant> stale;
    for (const std::string& g : pluginTuneAllowed_) {
        bool loadedHere = false;
        bool presentUnloaded = false;
        for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
            if (cascade::core::PluginUi::tuneKey(p) != g) { continue; }
            if (p.loaded) {
                loadedHere = true;
                break;
            }
            // NOT a break: two files can share a key only if they share a
            // name, which they cannot in one directory - but the loaded record
            // is the one that decides, so the scan runs on rather than
            // settling on the first match it happens to meet.
            presentUnloaded = true;
        }
        if (!loadedHere) { stale.push_back(StaleGrant{g, presentUnloaded}); }
    }
    const std::string& denied = pluginUi_.lastDeniedPlugin();
    const bool showDenied = !denied.empty() && !pluginUi_.tuneAllowed(denied);
    if (stale.empty() && !showDenied) { return; }

    ImGui::SeparatorText("Receiver control");

    if (showDenied) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("\"%s\" asked to tune the receiver and was refused.",
                           denied.c_str());
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "A module may move the receiver's centre frequency only if it is granted "
            "that - which is how a satellite tracker follows Doppler. The grant is a "
            "key on the module's own plate in the Plugins window, and it is off until "
            "you give it.");
    }

    if (stale.empty()) { return; }
    ImGui::TextWrapped(
        "These grants belong to modules the Fitted modules window cannot put a key on, "
        "because it draws one only on a module the host loaded. They are remembered, so "
        "a module that comes back finds its permission as it was - and they are shown "
        "here because a permission nobody can see is one nobody can take back. Each row "
        "says which of the two it is.");
    for (std::size_t i = 0; i < stale.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool allowed = pluginUi_.tuneAllowed(stale[i].file);
        if (ImGui::Checkbox(stale[i].file.c_str(), &allowed)) {
            setPluginTuneAllowed(stale[i].file, allowed);
        }
        // WHAT THE PREDICATE ABOVE ACTUALLY PROVED, and nothing more. One of
        // these files is on the disk and one is not, and the remedy is a
        // different one in each case.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (stale[i].refused) {
            ImGui::TextWrapped(
                "this file is in the plugin folder and the host refused it - the Fitted "
                "modules window prints why. Grant kept.");
        } else {
            ImGui::TextWrapped("no module of this name is fitted here - grant kept");
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void AppWindow::pumpDecoderOutput() {
    // Called from drawUi every frame, NOT from the panel. The runner's queue
    // is bounded and drops silently by design (the DSP thread must never
    // block on the GUI), so draining only while the Plugins section happened
    // to be expanded would quietly lose decodes the user never knew existed.
    for (cascade::core::DecodedLine& l : pluginRunner_.drainText()) {
        // COUNTED HERE BECAUSE THIS IS THE ONLY PLACE THAT SEES THEM ALL.
        // decoderLog_ is a bounded tail and drops its oldest entries, so its
        // size is not a count of anything; the status column's rate card
        // differences this cumulative figure over a window instead.
        ++decoderLinesTotal_;
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

    // THE OUTPUT AND THE SCOPE ARE DRAWN WHETHER OR NOT A DECODER IS LOADED.
    // The radar scope is a way of LOOKING at what a receiver hears, it is the
    // whole main window while it is on, and a switch that appeared and
    // vanished with the plugin list is a switch nobody would find twice. The
    // early returns are branches for the same reason.
    //
    // WHY A DECODER IS SILENT IS NO LONGER PRINTED HERE. It is printed in the
    // fitted modules window, against the module it is about, from the same
    // DecoderStatus::detail this loop used to quote - which is where a user
    // goes to ask the question, and which is one place rather than two. The
    // rail's Plugins key carries a one-line pointer when there is something in
    // there to read.
    //
    // AND AN EMPTY STATUS LIST IS NOT AN EMPTY PLUGIN FOLDER. THE FIFTH
    // INSTANCE OF THE FAMILY gui/module_census.hpp EXISTS FOR: PluginRunner::
    // rebuild skips a module that did not load and one the user stopped BEFORE
    // it writes a status line (plugin_runner.cpp:50-57), so status().empty() is
    // equally true of a machine with nothing in the folder, one whose decoder
    // the host REFUSED, and one whose decoder loaded perfectly and the user
    // STOPPED. This line read "No decoder plugin is installed." in all three,
    // which is a lie in two of them - and in both of those the Fitted modules
    // window one key away letters that same file REFUSED or STOPPED BY YOU.
    //
    // THE CENSUS IS TAKEN ONCE, for both branches, because the second one has
    // the same fault in a quieter form: with only a basemap fitted the runner
    // has a status line (the "provides no decoder this build can drive" one)
    // and no instances, and "No decoder is running" would put an idle decoder
    // on a machine that has none.
    const cascade::gui::ModuleCensus decoders = cascade::gui::censusModules(
        pluginHost_.plugins(), cascade::gui::kDecoderCaps,
        [this](const std::string& key) { return pluginIsStopped(key); });
    const bool anyDecoderFitted =
        decoders.live + decoders.stopped + decoders.refused > 0;
    if (st.empty() || (pluginRunner_.activeCount() == 0 && !anyDecoderFitted)) {
        // WRAPPED, not TextDisabled: this rail is narrow and a user may drag it
        // narrower, and a sentence that runs off the edge says nothing at all.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", cascade::gui::decoderAbsenceNote(decoders).c_str());
        ImGui::PopStyleColor();
    } else if (pluginRunner_.activeCount() == 0) {
        // A decoder IS fitted and none is running: the runner's own reason is
        // against the module in the Fitted modules window, which is the one
        // place this product answers "why is it silent".
        ImGui::TextDisabled("No decoder is running.");
    } else {
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
}

void AppWindow::drawDecoderWindow() {
    // ITS OWN OPERATING SYSTEM WINDOW, for the same reason the map and the
    // decoded images have one: a decoder's output is the reason the plugin
    // exists, and continuous text - CW, RTTY, APRS - is something to put
    // beside the radio or on a second screen, not to read through a slot in a
    // side panel.
    //
    // IT NEVER OPENS ITSELF. Until 0.79.1 it did, on a decoder's first line -
    // and the NOAA APT decoder says "listening" the moment it is fed, so the
    // window appeared at every launch with that plugin fitted. The user asked
    // for the application to start on the main screen alone; this window is
    // the DECODERS row's own key away ("Show decoder output").
    if (!decoderWindowOpen_) { return; }
    telemetryNotePanel("decoded");

    placeAsSeparateWindow(9);
    if (beginPage("Decoder output###decoderout", "DECODER OUTPUT", &decoderWindowOpen_)) {
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
    endPage();
}

void AppWindow::removeBlockedPlugin(const std::string& fileName) {
    installError_.clear();
    installReport_.clear();

    // THROUGH detachAndUnloadPlugins(), NEVER unloadAll() DIRECTLY. This used
    // to call unloadAll() on its own, on the reasoning that a blocked plugin
    // was never loaded so there was nothing to detach - which is true of the
    // blocked plugin and false of every OTHER plugin, all of which unloadAll()
    // unmaps too, with their track-source, panel and decoder instances still
    // alive. A live handle is memory inside a module that has just been
    // unmapped, and a plugin whose static state owns a thread (Satellites
    // 1.0.1) reached std::terminate in its own CRT the moment that happened:
    // an abort() the crash handler never sees. The ordered sequence exists so
    // there is exactly one way to take a module down; this was the one caller
    // that did not use it.
    detachAndUnloadPlugins();
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
    const bool taping = iqRecorder_.recording() || audioRecorder_.recording();
    const bool recorderOpen =
        benchSection("Recorder", false, taping ? "REC" : "OFF",
                     taping ? cascade::gui::theme::kAlarm : cascade::gui::theme::kPhosphor,
                     taping);
    if (!recorderOpen) { return; }
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
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
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
    // HOW MANY ARE SAVED, which is what this section holds and the only thing
    // about it worth reading from the rail. The lamp is lit while there is at
    // least one, so an empty list reads as empty rather than as a count the
    // user has to squint at.
    const std::size_t bookmarkCount = freqMgr_.list().size();
    char bookmarkChip[16];
    std::snprintf(bookmarkChip, sizeof(bookmarkChip), "%zu", bookmarkCount);
    if (!benchSection("Bookmarks", false, bookmarkChip, cascade::gui::theme::kPhosphor,
                      bookmarkCount > 0)) {
        return;
    }
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

namespace {
// Monotonic milliseconds for the retune coalescer — steady_clock, because a
// wall-clock step (NTP, DST) must never stall or flood the tune pacing.
double steadyNowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

void AppWindow::retuneSourceHz(double centerHz) {
    // Hardware tunes are PACED (see the header declaration): a burst becomes
    // one device call per interval with the latest value. Everything without
    // a USB control path underneath applies immediately.
    //
    // The SCANNER also bypasses the pacing: tickScanner() reads the frequency
    // back synchronously after every commanded tune to set its user-wins
    // baseline, and a tune held by the coalescer would make that readback
    // stale — the scanner would then see its own deferred retune land a frame
    // later and stop itself, misreading it as the user's hand. The scanner is
    // a control loop paced by its own dwell, not a human gesture burst.
    if (soapy_ == nullptr || scanner_.active()) {
        applyRetuneNow(centerHz);
        return;
    }
    if (retuneCoalescer_.request(centerHz, steadyNowMs())) {
        applyRetuneNow(centerHz);
    }
}

void AppWindow::pollPendingRetune() {
    if (const std::optional<double> hz = retuneCoalescer_.due(steadyNowMs())) {
        applyRetuneNow(*hz);
    }
}

void AppWindow::applyRetuneNow(double centerHz) {
    // ONE place where the source centre moves. The pipeline cannot observe a
    // device retune (the source owns the tuner), so the RDS/stereo decoders
    // have to be told explicitly — otherwise the previous station's PS name
    // stays on screen over the new one, which is a wrong readout, not a
    // cosmetic lag. No-op when the tune does not actually move anything, so
    // a repeated command cannot keep the decoders permanently reset.
    cascade::source::IqSource& src = pipeline_.activeSource();
    if (src.centerFrequencyHz() == centerHz) { return; }
    src.setCenterFrequencyHz(centerHz);
    // Out-of-band applies (a device open's carry-across) pace the next burst
    // off this moment too, so the coalescer's clock never lags an apply.
    retuneCoalescer_.noteApplied(steadyNowMs());
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
    const bool scannerOpen =
        benchSection("Scanner", false, scanner_.active() ? "SCAN" : "IDLE",
                     cascade::gui::theme::kPhosphor, scanner_.active());
    if (!scannerOpen) { return; }
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
    s.rxPositionSet = rxSet_;
    s.rxLatDeg = rxLat_;
    s.rxLonDeg = rxLon_;
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

// Hide or show every torn-off window the application owns, following scope
// mode.
//
// This runs each frame and covers the ImGui viewports, which are created and
// re-shown by the backend and so cannot be settled once.
void AppWindow::applyScopeWindowVisibility() {
    // Leaving scope mode is an EDGE, and the frame it happens on is the one
    // that has to show the windows again - after that ImGui owns their
    // visibility once more, and forcing them visible every frame would stop
    // the user ever closing one.
    scopeLeftThisFrame_ = scopeWasOn_ && !scopeMode_;
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) {
        scopeWasOn_ = scopeMode_;
        return;
    }
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    for (int i = 0; i < pio.Viewports.Size; ++i) {
        ImGuiViewport* vp = pio.Viewports[i];
        if (vp == nullptr) { continue; }
        // The main viewport is the main window: scope mode draws inside it
        // rather than hiding it, so it is skipped here along with anything
        // else that is not a torn-off window.
        if ((vp->Flags & ImGuiViewportFlags_IsPlatformWindow) == 0) { continue; }
        GLFWwindow* w = static_cast<GLFWwindow*>(vp->PlatformHandle);
        if (w == nullptr || w == mainWindow_) { continue; }
        // SCOPE MODE HIDES THEM. It is a full-screen instrument - the user
        // asked for the panel with as little around it as possible - and a
        // torn-off decoder window floating over the middle of the tube is the
        // opposite of that. Seen the first time the compact cabinet was
        // drawn: the scope was correct and completely covered.
        if (scopeMode_) {
            glfwHideWindow(w);
        } else if (scopeLeftThisFrame_) {
            glfwShowWindow(w);
        }
    }
    // Showing is a one-shot: after the frame that restores them, ImGui owns
    // their visibility again, and forcing them visible every frame would stop
    // the user ever closing one.
    scopeLeftThisFrame_ = false;
    scopeWasOn_ = scopeMode_;
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
    // LISTENING, AND HOW MANY CLIENTS ARE ON IT. A CAT port with something
    // connected to it can retune this receiver, which is exactly the sort of
    // thing the rail exists to show without opening the section. catStatus_ is
    // the listener's own failure text, so a refused port reads as trouble
    // rather than as OFF.
    char catChip[16];
    ImU32 catLamp = cascade::gui::theme::kPhosphor;
    if (!catStatus_.empty()) {
        std::snprintf(catChip, sizeof(catChip), "FAIL");
        catLamp = cascade::gui::theme::kAlarm;
    } else if (!catServer_.running()) {
        std::snprintf(catChip, sizeof(catChip), "OFF");
    } else if (catServer_.clientCount() > 0) {
        std::snprintf(catChip, sizeof(catChip), "%d CLI", catServer_.clientCount());
    } else {
        std::snprintf(catChip, sizeof(catChip), "ON");
    }
    if (!benchSection("CAT control (rigctld)", false, catChip, catLamp,
                      !catStatus_.empty() || catServer_.running())) {
        return;
    }
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
        ImGui::TextColored(cascade::gui::theme::warning(),
                           "This protocol has no password. Anything that can "
                           "reach the port can retune the receiver.");
    }

    if (!catStatus_.empty()) {
        ImGui::TextColored(cascade::gui::theme::bad(), "%s",
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
    const bool webOpen =
        benchSection("Web access", false, webServer_.running() ? "ON" : "OFF",
                     cascade::gui::theme::kPhosphor, webServer_.running());
    if (!webOpen) { return; }
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
    // THE FOUR STATES THIS SECTION ACTUALLY HAS, and no fifth invented one.
    // "OK" is only said once a check has completed without error - before
    // that it is IDLE, because "up to date" and "we have not asked" are
    // different statements and the second one must not wear the first one's
    // clothes.
    const char* updateChip = "OFF";
    ImU32 updateLamp = cascade::gui::theme::kPhosphor;
    bool updateLit = false;
    if (updateCheckEnabled_) {
        if (update_.newer) {
            updateChip = update_.critical ? "IMPT" : "NEW";
            updateLamp = update_.critical ? cascade::gui::theme::kAlarm
                                          : cascade::gui::theme::kAmber;
            updateLit = true;
        } else if (updatePending_) {
            updateChip = "CHECK";
        } else if (updateStarted_ && updateError_.empty()) {
            updateChip = "OK";
        } else {
            updateChip = "IDLE";
        }
    }
    if (!benchSection("Updates", false, updateChip, updateLamp, updateLit)) { return; }
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
        ImGui::TextColored(update_.critical ? kErrorRed : cascade::gui::theme::warning(),
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
    // ON, OFF, OR NOT BUILT IN. A build with no endpoint compiled in cannot
    // report at all, and saying OFF there would describe a switch this build
    // does not have - the same distinction the body below draws when it
    // refuses to offer the checkbox.
    const bool usageAvailable = !cascade::core::telemetryEndpoint().empty();
    if (!benchSection("Usage reporting", false,
                      usageAvailable ? (telemetryEnabled_ ? "ON" : "OFF") : "N/A",
                      usageAvailable ? cascade::gui::theme::kPhosphor
                                     : cascade::gui::theme::kInkFaint,
                      usageAvailable && telemetryEnabled_)) {
        return;
    }
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
        // The heartbeat follows the switch in the same click: off disarms it
        // (configure refuses the now-empty id), on arms it with the new id.
        telemetryHeartbeat_.configure(cascade::core::telemetryEndpoint(),
                                      telemetryInstallId_, cascade::versionString());
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
    // WHETHER ANYTHING IS BEING RECORDED, which is the whole promise this
    // section makes: off means no report is written and nothing is sent. The
    // memory-dump switch is reported too, because it is the one setting here
    // that changes what lands on the user's own disk.
    if (!benchSection("Diagnostics", false,
                      diagnosticsEnabled_ ? (diagnosticsMinidump_ ? "ON+DMP" : "ON")
                                          : "OFF",
                      cascade::gui::theme::kPhosphor, diagnosticsEnabled_)) {
        return;
    }
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

void AppWindow::applyConfig(const cascade::core::AppConfig& saved) {
    // WHAT THE WINDOW STARTS FROM IS NOT THE FILE AS SAVED. The file records
    // what was showing at the last exit - the scope, the store, the fitted
    // modules window, each map page - and the application starts on the
    // bench alone whatever that was (the user's instruction, 0.79.1). So the
    // saved config passes through core::startupState first, which clears
    // every open flag and the scope mode and touches nothing else: every
    // rectangle, every setting, every position survives. Everything below
    // reads `cfg`, the start-up state, never `saved`.
    const cascade::core::AppConfig cfg = cascade::core::startupState(saved);
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
    // The trail switches. Not pushed into any MapView here: a page may not
    // exist yet (they are created as track-capable plugins appear), and the
    // page loop hands both to every view it draws anyway - which is also what
    // makes a mid-session change take effect on the very next frame.
    mapTrails_ = cfg.mapTrails;
    mapTrailAltColours_ = cfg.mapTrailAltitudeColours;
    mapTrailStyle_ = cfg.mapTrailStyle;

    // The radar scope. The MODE arrives off - startupState cleared it, because
    // the application starts on the bench whatever was showing at the last
    // exit. The RANGE is restored, so the scope comes up at the scale it was
    // left at when the user switches it on; it is snapped again here rather
    // than trusted from the config: the loader already clamps it, and the
    // view clamps it once more inside setRangeNm, so there is no path by
    // which an unrepresentable scale reaches the rings - which is the point
    // of a ladder.
    scopeMode_ = cfg.scopeMode;
    scopeRangeNm_ = clampScopeRangeNm(cfg.scopeRangeNm);
    // The rail opens on the bank it was left on. Clamped again here even
    // though load() already did: this is the value a widget indexes with.
    railBank_ = static_cast<int>(cascade::gui::railBankFromIndex(cfg.railBank));
    scope_.setRangeNm(scopeRangeNm_);

    // The map pages' rectangles from the last session, seeded here rather
    // than read at draw time so the very first Begin of each page already has
    // them — ImGui's FirstUseEver only fires once, and a value that arrived a
    // frame late would be ignored for the whole session. ensureMapPage
    // consumes these as pages appear.
    mapPagesSaved_ = cfg.mapPages;
    // The LEGACY single-window rectangle, kept only as the default for a page
    // with no saved entry of its own — an install upgrading from the
    // one-window map reopens its pages where that window used to sit. Read
    // here, never written back; see AppConfig for the migration note.
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
    if (rxSet_) {
        // Every existing page; a page created later gets the position in
        // ensureMapPage. At construction this loop is empty and harmless.
        for (MapPage& pg : mapPages_) {
            pg.view->setHome(rxLat_, rxLon_);
        }
    }

    // The plugin store. Restoring the URL does NOT start a fetch - see
    // AppConfig::pluginCatalogueUrl. The user still has to press CHECK NOW,
    // on this launch as on every other. The window's open flag arrives
    // cleared (startupState): the store opens from its rail key, never by
    // itself.
    pluginCatalogueUrl_ = cfg.pluginCatalogueUrl;
    std::snprintf(pluginUrlBuf_, sizeof(pluginUrlBuf_), "%s", pluginCatalogueUrl_.c_str());
    pluginBrowseOpen_ = cfg.pluginBrowserOpen;
    // The fitted modules window: its rectangle is restored, its open flag
    // arrives cleared for the same reason. Opening it later starts no scan,
    // no fetch and no network call of any kind.
    fittedWindowOpen_ = cfg.fittedModulesOpen;
    fittedWinX_ = cfg.fittedModulesX;
    fittedWinY_ = cfg.fittedModulesY;
    fittedWinW_ = cfg.fittedModulesWidth;
    fittedWinH_ = cfg.fittedModulesHeight;
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
    // AppConfig::closedWindows is no longer applied. Until 0.79.1 every plugin
    // window appeared by itself and this list kept the ones the user had shut
    // from coming back; now no plugin window appears until the user opens it
    // from its row, so there is nothing for the list to hold back. It is
    // still read and written so older builds and this one agree on the file.
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
    // Heartbeats are NOT armed here: this runs for bounded --frames runs too,
    // and arming them for those put ctest's throwaway install ids on the live
    // endpoint. run() arms them, interactive runs only, beside the update
    // check that follows the same rule.
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
    cfg.mapTrails = mapTrails_;
    cfg.mapTrailAltitudeColours = mapTrailAltColours_;
    cfg.mapTrailStyle = mapTrailStyle_;
    cfg.scopeMode = scopeMode_;
    cfg.scopeRangeNm = scopeRangeNm_;
    cfg.railBank = railBank_;
    // The pages' rectangles and open flags, via the saved store so an entry
    // for a plugin with no page this session rides through untouched. The
    // legacy fields are copied back purely so the first configsEqual against
    // the loaded config stays quiet — save() never writes them.
    syncMapPagesToSaved();
    cfg.mapPages = mapPagesSaved_;
    cfg.mapWindowWidth = mapWinW_;
    cfg.mapWindowHeight = mapWinH_;
    cfg.mapWindowX = mapWinX_;
    cfg.mapWindowY = mapWinY_;
    cfg.rxPositionSet = rxSet_;
    cfg.rxLatDeg = rxLat_;
    cfg.rxLonDeg = rxLon_;
    cfg.pluginCatalogueUrl = pluginCatalogueUrl_;
    cfg.pluginBrowserOpen = pluginBrowseOpen_;
    cfg.fittedModulesOpen = fittedWindowOpen_;
    cfg.fittedModulesX = fittedWinX_;
    cfg.fittedModulesY = fittedWinY_;
    cfg.fittedModulesWidth = fittedWinW_;
    cfg.fittedModulesHeight = fittedWinH_;
    cfg.pluginLastUpdateCheck = pluginLastUpdateCheck_;
    cfg.pluginTuneAllowed = pluginTuneAllowed_;
    cfg.pluginsStopped = pluginsStopped_;
    // closedWindows is written empty: nothing reads it since 0.79.1 (see
    // applyConfig), and an empty list is what an older build would take to
    // mean "no window was shut" - the nearest true statement it can make
    // about a session in which windows only ever open by hand.
    cfg.closedWindows.clear();
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
