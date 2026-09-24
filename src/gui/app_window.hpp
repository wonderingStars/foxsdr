// GLFW + Dear ImGui application shell.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Forward-declared rather than including GLFW here: this header is included
// by the tests, and pulling a windowing library into them would make a
// headless build depend on one.
struct GLFWwindow;

#include "core/band_plan.hpp"
#include "core/config.hpp"
#include "core/freq_manager.hpp"
#include "core/gps_reader.hpp"
#include "core/i18n.hpp"
#include "core/pipeline.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_radio.hpp"
#include "core/patch_runner.hpp"
#include "gui/patch_view_math.hpp"
#include "gui/patch_scope_math.hpp"
#include "gui/input_script.hpp"
#include "core/plugin_ui.hpp"
#include "core/plugin_repo.hpp"
#include "core/updater.hpp"
#include "core/utf8_text.hpp"
#include "core/recorder.hpp"
#include "core/retune_coalescer.hpp"
#include "core/scanner.hpp"
#include "core/transmitter.hpp"
#include "gui/basemap_cache.hpp"
// The floor a torn-off page cannot be dragged under, and the reset generation
// that puts an already-wrong one back. ImGui-free for the same reason as the
// headers below it - the tests include it without a graphics context.
#include "gui/page_geometry.hpp"
#include "gui/store_first_open.hpp"
#include "gui/rail_banks.hpp"
#include "gui/bench_rail.hpp"
#include "gui/audio_open.hpp"
#include "gui/config_writer.hpp"
#include "gui/shell_open.hpp"
// The keyboard, as a table. ImGui-free by construction (it declares ImGuiKey
// opaquely rather than including imgui.h - see its own note), so a KeyBindings
// can be held by value here without breaking the rule this header states above
// about GLFW and ImGui never reaching the tests.
#include "gui/key_bindings.hpp"
// The ADS-B radar scope. ImGui-free like track_metrics.hpp below, so it can be
// held by value here without breaking the rule that main() - and the tests -
// never see a GUI header.
#include "gui/scope_view.hpp"
// The DEMOD SCOPE's arithmetic and its settings struct, held by value below.
// ImGui-free by construction for the same reason scope_view.hpp is - the
// DRAWING half of that scope lives in gui/demod_scope_face.hpp, which this
// header deliberately does not reach.
#include "gui/demod_scope.hpp"
#include "gui/scope_memory.hpp"
#include "gui/transmit_page.hpp"
// The FFT the scope's spectrum position uses, held here by unique_ptr and so
// needing to be a complete type. Arrives transitively through pipeline.hpp
// anyway; named explicitly because a member's type should not depend on
// somebody else's include order.
#include "dsp/fft.hpp"
#include "gui/track_info_cache.hpp"
// RememberedSource, held by value below: the pure source decisions, ImGui-free
// like every other gui header included here.
#include "gui/readout_hold.hpp"
#include "gui/tune_control.hpp"
#include "gui/device_scan_plan.hpp"
#include "gui/viewport_policy.hpp"
// CoverageMap, TrackSortKey: the pure arithmetic behind the map's three
// receiver-relative features. Header-only and ImGui-free, so including it here
// keeps app_window.hpp usable from the tests (see the note below).
#include "gui/track_metrics.hpp"
// SatelliteDeck, and with it the MapView declaration this header used to
// forward-declare. Included rather than forward-declared because the deck is
// held BY VALUE in a MapPage below: it is the satellites window's settings,
// and map_view.hpp is explicit that the CALLER owns them. Safe to include for
// the same reason scope_view.hpp and track_metrics.hpp are - it declares no
// ImGui type and includes no ImGui header, so the rule that main() and the
// tests never see a graphics header still holds.
#include "gui/map_view.hpp"
#include "core/telemetry.hpp"
#include "core/crash_upload.hpp"
#include "core/feature_request.hpp"
#include "core/problem_report.hpp"
#include "core/hang_watchdog.hpp"
#include "gui/freq_scale.hpp"
// Pulls in the bind policy and the credential types too, but NOT httplib —
// web_server.hpp forward-declares it.
#include "net/cat_server.hpp"
#include "net/web_server.hpp"
// For SoapyDeviceInfo and the non-owning SoapySource* below; the header
// forward-declares the Soapy API types, so this pulls in no Soapy headers.
#include "source/soapy_source.hpp"
// The eight NATIVE drivers the Source section can open without any SoapySDR
// module at all, and the transport six of them enumerate through. Headers
// only - each one names a class and a free function; nothing here pulls in
// WinUSB, the SDRplay API or a socket.
//
// "Without a vendor module" is exact for seven of them and needs one word of
// care for the eighth: an SDRplay RSP is reached through sdrplay_api.dll, the
// user's own install, because SDRplay publish no device protocol at all - but
// that is the VENDOR's API called directly, not a SoapySDR module wrapping
// it, which is what every crash in this product's first month came out of.
#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/pluto_source.hpp"
#include "source/pluto_tx.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "source/sdrplay_source.hpp"
#include "usb/usb_device.hpp"

namespace cascade::gui {

// Forward declarations keep ImGui types out of this header (waterfall_view.hpp
// includes imgui.h), preserving the rule that main() never sees GUI headers.
class SpectrumView;
class WaterfallView;
// What the demod scope's page hands its tube. Forward-declared for the same
// reason: gui/demod_scope_face.hpp includes imgui.h, and gatherDemodScope
// below only takes a reference to one.
struct DemodScopeFeed;
// The two plugin windows' view objects and their decks, for exactly the same
// reason: gui/plugin_store_view.hpp and gui/plugins_view.hpp both include
// imgui.h, and this header is compiled into the tests. They are held by
// unique_ptr below and created in the constructor, where those headers are
// included; the decks are owned here rather than by the views because both
// view headers say outright that the CALLER owns them - they outlive a frame
// and this is the object that outlives frames.
class PluginStoreView;
struct PluginStoreDeck;
struct PluginStoreModel;
struct FittedModulesDeck;

// Whether a device open that finished on a worker thread should still be
// applied to the pipeline.
//
// Opening a SoapySDR device takes seconds, and the user is not frozen while it
// happens: they can pick the built-in generator, open an IQ file, or drive
// either from the web UI in the meantime. Without this check the open simply
// landed when it landed and replaced whatever they had chosen — the radio
// changed itself, several seconds after they told it not to.
//
// The rule is a sequence number rather than a "cancelled" flag because the
// question is not "was it cancelled" but "is the answer still about the source
// the user is looking at": every install of a source bumps the counter, the
// request records the value it was made at, and a result whose value has moved
// on is about a source selection that no longer exists.
inline bool asyncOpenStillWanted(std::uint64_t requestedAtGen,
                                 std::uint64_t currentGen) {
    return requestedAtGen == currentGen;
}

// --- THE FUNCTION SELECT RAIL'S ROW GEOMETRY ---------------------------------
//
// WHY IT IS OUT HERE AND NOT IN app_window.cpp's ANONYMOUS NAMESPACE. These
// numbers decide where a rail row's own name has to STOP, and Dear ImGui text
// does not wrap - it clips, silently, so a row that has quietly lost the end
// of its label looks like a design decision rather than a fault. The rail was
// laid out to a set of literals measured against a smaller typeface (the sizes
// in gui/fonts.hpp were raised by two points on a report that captions were
// hard to read), and a literal that happened to fit is not a measurement of
// anything. tests/test_app_rail.cpp pins every function below against the real
// typefaces at the sizes fonts.hpp is currently set to - and pins
// railChipReserve against drawRailChip's OWN DRAWN OUTPUT rather than against a
// transcription of its constants, so the two cannot drift apart in silence.
//
// PURE ARITHMETIC, NO ImGui TYPES, AND EVERY SIZE ARRIVES AS A PARAMETER. This
// header is compiled into the tests and must not pull in gui/fonts.hpp, which
// includes imgui.h - see the forward declarations further down.

// The height of one rail row. The reference draws a 28 px deck and that is the
// FLOOR, not the answer: a row carries a label at fonts::kUiSize, so once the
// type is larger than the row can hold at a sane padding the ROW grows and the
// label is not squeezed into it. At kUiSize 18 this is still exactly 28.
inline constexpr float kRailRowMinH = 28.0f;
// The smallest gap above and below the label inside a row. Anything less and
// the word touches the plate's bevel.
inline constexpr float kRailRowPadY = 5.0f;
inline float railRowHeight(float labelPx) {
    const float fromType = labelPx + 2.0f * kRailRowPadY;
    return fromType > kRailRowMinH ? fromType : kRailRowMinH;
}

// --- the bandwidth the combo offers, and the bandwidth it is actually running
//
// OUT HERE SO SOMETHING CAN CHECK THEM. These two were file-private in
// app_window.cpp, which is why the fault they now close shipped: nothing in
// the suite can construct an AppWindow, so anything private to that file is
// reachable only by running the application and looking at it. The pair below
// is pure arithmetic over a table, so it costs nothing to put where a test can
// see it - and this is the same reason railRowHeight and railChipReserve are
// here rather than there.
//
// The steps the Bandwidth combo offers, widest first. app_window.cpp letters
// them from kBwLabels, which must stay in step with this.
inline constexpr double kBwHz[6] = {200000.0, 150000.0, 12500.0, 10000.0, 6000.0, 3000.0};
inline constexpr int kBwCount = static_cast<int>(sizeof(kBwHz) / sizeof(kBwHz[0]));

// WHICH OF THE OFFERED STEPS A BANDWIDTH IS, OR -1 FOR NONE OF THEM.
//
// A "nearest step" answer was the fault. A plugin preset may ask for a
// bandwidth this table does not carry - the NOAA APT preset asks for 40 kHz,
// because an APT signal is about 34 kHz wide and 12.5 kHz slices its video
// sidebands off - and the nearest step to 40 kHz is 12.5 kHz. Pointing the
// combo there made it letter a bandwidth the receiver was not running, and
// because every control that writes this index back writes kBwHz[index] with
// it, the next touch of any of them really did narrow the receiver to the
// figure the combo had been wrongly showing.
//
// An eighth of a hertz of tolerance because both sides are exact table values,
// or a config round-trip of one, never a computed quantity.
inline int bandwidthStepIndex(double hz) {
    for (int i = 0; i < kBwCount; ++i) {
        const double d = kBwHz[i] - hz;
        if ((d < 0.0 ? -d : d) < 0.125) { return i; }
    }
    return -1;
}

// The bandwidth as the combo letters it, in kBwLabels' own shorthand, for a
// value that may be none of them: "200k", "40k", "12.5k", "3k". Three decimals
// of a kilohertz is one hertz, and the trailing zeros come off, so a table
// value reads exactly as its own label does and a preset's own figure reads as
// itself rather than as the nearest word in the table. Nothing below 3 kHz can
// arrive - kVfoBwMinHz is the floor every writer clamps to.
inline void formatBandwidth(double hz, char* out, std::size_t n) {
    int len = std::snprintf(out, n, "%.3f", hz / 1000.0);
    if (len < 0 || static_cast<std::size_t>(len) >= n) { return; }
    while (len > 0 && out[len - 1] == '0') { out[--len] = '\0'; }
    if (len > 0 && out[len - 1] == '.') { out[--len] = '\0'; }
    std::snprintf(out + len, n - static_cast<std::size_t>(len), "k");
}

// THE "SERIAL PORTS" ROW'S CHIP: how many the machine has right now, in the
// rail's own idiom of naming what a count IS rather than leaving a bare
// number ("12 TGT", "2000 ROW") - a bare "0" here would read as "off",
// which this row never is; it always reports, it just sometimes reports
// nothing found. Singular/plural rather than always "PORTS" because "1
// PORTS" is the kind of thing a person notices and a test can pin without
// a registry in the room.
// Drawn and nothing else, so it is written in the interface language.
// A string rather than a caller's buffer: the chip is a translated word, and
// a buffer sized for the English one cuts it.
inline std::string formatSerialPortsChip(std::size_t count) {
    if (count == 0) { return cascade::i18n::tr("NONE"); }
    if (count == 1) { return cascade::i18n::tr("1 PORT"); }
    return cascade::core::formatText(cascade::i18n::tr("%zu PORTS"), count);
}

// THE SOURCE ROW'S CHIP: the device in use, shortened to what fits. A driver's
// device name is cut to its last ": " part ("SoapySDR: B200" is "B200") and
// then to ten CHARACTERS - never a byte count, which would split an accented
// letter. That cut is for third-party names only.
//
// THE BUILT-IN GENERATOR IS OURS, AND IS TRANSLATED - so it is not cut. Its
// chip was the translation of "Signal generator" cut to ten characters, which
// is where English "Signal gen" came from, and in Dutch, Swedish and Danish it
// made "Signaalgen" / "Signalgene": a word hard-clipped with nothing to say so
// (34-language review). It is now its own key, "Signal gen", whose translation
// is a short form a translator chose, shown whole.
inline std::string sourceChipText(const char* sourceName) {
    if (sourceName != nullptr && std::strcmp(sourceName, "Signal generator") == 0) {
        return cascade::i18n::tr("Signal gen");
    }
    // Through tr() as before: "IQ file" is ours too, and a device name is in
    // no catalogue and comes back unchanged.
    std::string chip = sourceName != nullptr ? cascade::i18n::tr(sourceName) : "";
    const std::size_t colon = chip.rfind(": ");
    if (colon != std::string::npos) { chip = chip.substr(colon + 2); }
    const char* end = chip.c_str();
    for (int i = 0; i < 10 && *end != '\0'; ++i) { end = cascade::core::utf8Next(end); }
    chip.resize(static_cast<std::size_t>(end - chip.c_str()));
    return chip;
}

// The square key at the left of the row, and the plate that starts after it.
// kRailKeyMax is the reference's own key; below it the key follows the row so a
// short row does not carry an oversized one.
inline constexpr float kRailKeyInset = 3.0f;    // row's left edge to the key
inline constexpr float kRailKeyGap = 6.0f;      // key to the plate
inline constexpr float kRailLabelPadX = 8.0f;   // plate's edge to the word
inline constexpr float kRailKeyMin = 9.0f;
inline constexpr float kRailKeyMax = 18.0f;
inline float railKeySize(float rowH) {
    const float s = rowH - 10.0f;
    if (s < kRailKeyMin) { return kRailKeyMin; }
    if (s > kRailKeyMax) { return kRailKeyMax; }
    return s;
}
inline float railPlateLeft(float rowLeft, float rowH) {
    return rowLeft + kRailKeyInset + railKeySize(rowH) + kRailKeyGap;
}
inline float railLabelLeft(float rowLeft, float rowH) {
    return railPlateLeft(rowLeft, rowH) + kRailLabelPadX;
}

// WHAT THE STATE CHIP AND ITS LAMP TAKE OFF THE RIGHT END OF THE PLATE.
//
// This mirrors scope_face.hpp's drawRailChip, which lands the lamp hard against
// the plate's right edge and the chip just inboard of it. It is a mirror and
// not a shared constant because the chip is drawn by the scope's own face
// library and the label is drawn here; the test closes that gap by MEASURING
// what drawRailChip actually emits and refusing to pass if this disagrees.
inline constexpr float kRailLampRadiusMin = 3.0f;
inline constexpr float kRailLampRadiusShare = 0.20f;
inline constexpr float kRailLampEdgeGap = 6.0f;   // plate's right edge to lamp
inline constexpr float kRailChipLampGap = 7.0f;   // chip's right edge to lamp
inline constexpr float kRailChipPadX = 5.0f;      // inside the chip, each side
// Clear air between the end of the label and the start of the chip, so the two
// read as separate things rather than as one run-on line.
inline constexpr float kRailLabelChipGap = 4.0f;

inline float railLampRadius(float rowH) {
    const float r = rowH * kRailLampRadiusShare;
    return r > kRailLampRadiusMin ? r : kRailLampRadiusMin;
}
// `chipTextWidth` < 0 means the row carries a lamp but no chip; the row's
// callers here always pass one, and drawRailChip draws the lamp either way.
inline float railChipReserve(float rowH, float chipTextWidth) {
    const float lampR = railLampRadius(rowH);
    const float toLampLeft = 2.0f * lampR + kRailLampEdgeGap;
    if (!(chipTextWidth >= 0.0f)) { return toLampLeft; }
    return 2.0f * lampR + kRailChipLampGap + chipTextWidth + 2.0f * kRailChipPadX +
           kRailLampEdgeGap;
}

// The x a row's label must not cross. `chipTextWidth` < 0 for a row with no
// chip at all, in which case only the plate's own padding is kept back.
inline float railLabelRight(float rowRight, float rowH, float chipTextWidth) {
    const float plateEdge = rowRight - kRailLabelPadX;
    const float beforeChip =
        rowRight - railChipReserve(rowH, chipTextWidth) - kRailLabelChipGap;
    return beforeChip < plateEdge ? beforeChip : plateEdge;
}

// The width of the left column, and the pad that insets the rail's plate
// inside it. Out here with the rest of the rail's geometry because the test
// has to know how much room a row actually gets before it can say whether a
// label fits in one.
//
// 260 per the parity spec until 0.79.0, when every face grew three pixels
// (fonts.hpp) and "Plugins (12 disabled)" needed 96 px of a 93-px row. Eight
// more pixels of column keeps every shipped word whole beside the widest
// chip, with room to spare; test_app_rail.cpp measures it against the real
// typefaces, so a word that stops fitting fails a test rather than clipping.
//
// 384 SINCE 0.84.0, WHEN THE BENCH WENT OVER TO GEORGIA. A serif set at the
// same sizes runs about 1.6 times the width of Saira Condensed: the same
// "Plugins (12 disabled)" needed 170 px of the 59 the 268-px column left
// beside the widest chip, ten of the shipped labels overflowed with it, and
// the five bank keys could not hold their bold capitals either, until the
// sizes came down (fonts.hpp): at 17/15 everything fits in 384 with room.
// The column grew rather than the words being cut, and the spectrum gave up
// the difference; the size reduction that followed was the user's own call
// on seeing the serif at 21 px, not a fitting trick. test_app_rail still
// measures every label against the real face.
inline constexpr float kMenuWidth = 384.0f;   // left column
inline constexpr float kRailPlatePad = 8.0f;  // plate inset inside that column

// THE FIVE BANK KEYS (drawRailBankKeys): the column's width shared five ways
// after an 8 px inset each side and 4 px between keys, the word lettered at
// fonts::kTinySize with 3 px of brass kept clear each side, and drawn smaller
// down to kBankKeyWordFloorPx before anything is cut. That floor is the bench's
// absolute nine pixels (text_fit.hpp's kFitFloorPx), not the seven tenths a
// wider key stops at: at seven tenths "JÄRJESTELMÄ" (fi) lost its last letter
// and "РАСШИРЕНИЯ" (ru) its last two (34-language review). test_app_rail
// letters every catalogue's five words at this floor in this key.
inline constexpr float kBankKeyInset = 8.0f;
inline constexpr float kBankKeyGap = 4.0f;
inline constexpr float kBankKeyWordPadX = 3.0f;
inline constexpr float kBankKeyWordPadMinX = 1.0f;
inline constexpr float kBankKeyWordFloorPx = 9.0f;
inline float bankKeyWidth(float colW, int count) {
    return (colW - 2.0f * kBankKeyInset - kBankKeyGap * static_cast<float>(count - 1)) /
           static_cast<float>(count);
}
// The brass kept clear each side of the word: kBankKeyWordPadX, or - only for
// a word that does not fit even at the floor with that much - one pixel.
// "РАСШИРЕНИЯ" (ru) and "РОЗШИРЕННЯ" (uk) are 66.8 px at nine pixels in a
// 64.4 px room; two more pixels of metal each side hold them whole, and a word
// a pixel from the bevel is a tight key where a cut one is a broken key.
// `wordWAtFloor` is the word's width at kBankKeyWordFloorPx.
inline float bankKeyWordPadX(float wordWAtFloor, float keyW) {
    return wordWAtFloor + 2.0f * kBankKeyWordPadX <= keyW + 0.5f ? kBankKeyWordPadX
                                                                  : kBankKeyWordPadMinX;
}

// How wide ONE ROW ends up: the column, less the plate's inset on both sides,
// less the scrolling child's own padding, less the scrollbar that child has
// whenever the rail is longer than the window - which is every real session.
// The ImGui style values arrive as parameters for the same reason the font
// sizes do.
inline float railRowWidth(float menuWidth, float platePad, float childPadX,
                          float scrollbarW) {
    return menuWidth - 2.0f * platePad - 2.0f * childPadX - scrollbarW;
}

// One monitor's usable area, in the virtual-desktop coordinates ImGui and the
// window manager both speak. Its own type rather than an ImGui one so this
// stays testable without a platform backend.
struct ScreenRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// How much of the map window has to be reachable for a SAVED geometry to be
// worth restoring: a title bar's worth. An ImGui window is dragged by its
// title bar and by nothing else, so a rectangle whose title bar is off the
// screen cannot be moved back on by any means the user has - the resize grip
// in the far corner can only resize. 120x30 is a bar wide enough to grab and
// tall enough to hit.
constexpr float kMapReachableW = 120.0f;
constexpr float kMapReachableH = 30.0f;

// Whether a map-window rectangle saved on some PREVIOUS run can still be
// reached on THIS machine's monitors.
//
// The config sanitizer only checks the numbers are sane in isolation
// (AppConfig::kMapWindowMinPx/kMapWindowMaxPx); it has no idea what displays
// exist. A geometry saved on a second monitor that has since been unplugged is
// a perfectly legal rectangle in a place that no longer exists, and restoring
// it hands the user a window they cannot see or move. That is a NEW failure
// mode: before the map window's size was persisted it always opened beside the
// main window, where it could not be lost.
//
// The test is on the TITLE BAR STRIP, not on the window as a whole, because
// overlap somewhere is not the same as being usable - see kMapReachableH.
// WHICH monitor the title-bar strip landed on, as an index into `workAreas`,
// or -1 for none. The index is what the size clamp below needs: "it is on
// screen somewhere" does not say which screen's dimensions the window has to
// fit inside.
inline int mapReachableMonitor(int x, int y, int w, int h,
                               const std::vector<ScreenRect>& workAreas) {
    if (w <= 0 || h <= 0) { return -1; }
    const float left = static_cast<float>(x);
    const float top = static_cast<float>(y);
    const float right = left + static_cast<float>(w);
    // The strip is the window's width but only a title bar's height, and it
    // cannot be taller than the window itself.
    const float strip = (static_cast<float>(h) < kMapReachableH)
                            ? static_cast<float>(h)
                            : kMapReachableH;
    const float bottom = top + strip;
    for (std::size_t i = 0; i < workAreas.size(); ++i) {
        const ScreenRect& r = workAreas[i];
        const float ix0 = (left > r.x) ? left : r.x;
        const float ix1 = (right < r.x + r.w) ? right : r.x + r.w;
        const float iy0 = (top > r.y) ? top : r.y;
        const float iy1 = (bottom < r.y + r.h) ? bottom : r.y + r.h;
        if (ix1 - ix0 >= kMapReachableW && iy1 - iy0 >= kMapReachableH) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline bool mapGeometryOnScreen(int x, int y, int w, int h,
                                const std::vector<ScreenRect>& workAreas) {
    return mapReachableMonitor(x, y, w, h, workAreas) >= 0;
}

// FIELD-WISE AppConfig COMPARISON - the whole of the save debounce's decision
// about whether anything changed. Declared here, rather than left file-local
// in app_window.cpp, for the reason the pure decisions above are: a field
// MISSING from this comparison is not a compile error and not a visible bug -
// it is a setting that reaches the file only when something else happens to
// change in the same session, which is the subtlest way a preference can be
// lost. tests/test_config.cpp asks it directly, one field at a time.
//
// Exact float compares are correct: both sides come from the same
// currentConfig() code path, so any difference is a real user-visible change,
// never noise.
bool configsEqual(const cascade::core::AppConfig& a, const cascade::core::AppConfig& b);

// The share of a monitor's work area the map window OPENS at when nothing was
// saved. A DEFAULT ONLY: a window that exactly fills the work area looks
// maximised, and the user then cannot see what it is covering - which is a
// judgement about a size the host picks, not a limit on one the user picked.
// Applying it to a restored rectangle shrank a perfectly usable window on
// every launch (measured: a saved 1120x1300 at 600,60 - bottom 1360, well
// inside this machine's 1392 px work area - came back 1120x1183, and the 1183
// was written to the config, so the chosen height degraded once per restart
// until it reached the share).
constexpr float kMapWorkAreaShare = 0.85f;

// A RESTORED rectangle is clamped to WHAT ACTUALLY FITS WHERE IT SITS, and to
// nothing else.
//
// Only the position was being checked at first. A geometry saved on a taller
// or wider display - a 1440p monitor at the office, a laptop panel at home -
// is a perfectly reachable rectangle whose BOTTOM, and with it the resize
// grip, is off the screen it reopens on: the user can drag the window but
// cannot make it smaller, and the geometry is then saved back oversized every
// frame. The harm is the grip being off the screen, so the test is the screen
// edge - the work area's right and bottom measured from the window's own
// origin - and not a share of the monitor.
//
// Position is deliberately left alone. mapReachableMonitor has already said
// the title bar can be grabbed, and moving a window the user placed is a
// bigger liberty than shrinking one that cannot fit.
inline void mapClampRestoredSize(int x, int y, int& w, int& h,
                                 const std::vector<ScreenRect>& workAreas) {
    const int idx = mapReachableMonitor(x, y, w, h, workAreas);
    if (idx < 0) { return; }  // not restorable at all; the caller falls back
    const ScreenRect& r = workAreas[static_cast<std::size_t>(idx)];
    // Both are positive by construction: the title-bar strip overlapped this
    // work area by at least kMapReachableW x kMapReachableH, so the window's
    // own origin is left of its right edge and above its bottom one.
    int fitW = static_cast<int>(r.x + r.w) - x;
    int fitH = static_cast<int>(r.y + r.h) - y;
    // ...but never shrunk to something that is no longer a window. A rectangle
    // parked with only its title bar showing at the bottom of the screen would
    // otherwise be restored as a 30 px sliver AND saved that way; a little
    // overhang is the lesser harm, and the sanitizer's own minimum is the
    // smallest rectangle this product accepts anywhere.
    if (fitW < cascade::core::AppConfig::kMapWindowMinPx) {
        fitW = cascade::core::AppConfig::kMapWindowMinPx;
    }
    if (fitH < cascade::core::AppConfig::kMapWindowMinPx) {
        fitH = cascade::core::AppConfig::kMapWindowMinPx;
    }
    if (w > fitW) { w = fitW; }
    if (h > fitH) { h = fitH; }
}

// Where the map window OPENS when there is no saved geometry: the proposed
// rectangle, MOVED so all of it - the resize grip in the bottom-right corner
// included - is on the screen it lands on.
//
// The default size is capped at kMapWorkAreaShare, but the default POSITION
// was never checked against anything, and the two only make a usable window
// together. placeAsSeparateWindow anchors the map at the main window's top
// right plus 60 px, so once the default height grew to use the screen the
// monitor offers, the bottom fell off the work area whenever the main window
// sat low: measured on this 5120x1440 desktop with the main window at y=400,
// the map opened at (1248,491) 1120x1168 - a bottom of 1659 against a work
// area of 1392 - and that rectangle was then persisted and restored verbatim,
// because it is not oversized, only misplaced.
//
// MOVED rather than shrunk, because the size is the part that was chosen for
// this monitor on purpose; shrinking it would hand back the scrollbar the
// derived height exists to avoid. Only a rectangle larger than the whole work
// area is shrunk, which the share makes impossible unless the small-screen
// floor in mapDefaultSize is what set it.
inline void mapPlaceDefaultRect(float& x, float& y, float& w, float& h,
                                const std::vector<ScreenRect>& workAreas) {
    if (workAreas.empty()) { return; }
    // The monitor the proposed rectangle's title bar lands on. A default that
    // lands nowhere at all means the main window is itself somewhere odd, and
    // the first work area is a better answer than leaving the map off-screen.
    int idx = mapReachableMonitor(static_cast<int>(x), static_cast<int>(y),
                                  static_cast<int>(w), static_cast<int>(h), workAreas);
    if (idx < 0) { idx = 0; }
    const ScreenRect& r = workAreas[static_cast<std::size_t>(idx)];
    if (w > r.w) { w = r.w; }
    if (h > r.h) { h = r.h; }
    if (x + w > r.x + r.w) { x = r.x + r.w - w; }
    if (y + h > r.y + r.h) { y = r.y + r.h - h; }
    // Bottom-right first, then top-left: a rectangle as large as the work area
    // has to end up at its origin, not pushed off the top by the nudge above.
    if (x < r.x) { x = r.x; }
    if (y < r.y) { y = r.y; }
}

// Owns the GLFW window, the ImGui context and the top-level panel layout.
// All GLFW/ImGui usage stays behind this interface so main() (and any future
// headless harness) never needs GUI headers.
class AppWindow {
public:
    // Constructs the render pipeline with the demo SigGen signal already
    // configured, so the first Play click shows spectrum content immediately.
    //
    // configPath: where the persistent AppConfig is loaded from at
    // construction and saved to (debounced during run(), and on clean exit).
    // An EMPTY path disables persistence entirely — nothing is read or
    // written — which is the hermetic mode the --frames/--selftest CI
    // contract requires, and the default so a bare AppWindow can never touch
    // the user's real config by accident. announceConfig prints the one-line
    // "config applied: ..." diagnostic to stdout after the startup load (the
    // CASCADE_CONFIG_TEST hook; normal runs keep stdout byte-identical).
    explicit AppWindow(std::string configPath = {}, bool announceConfig = false);

    // Out-of-line: the unique_ptr members delete forward-declared types.
    // Also finalizes any recording still in flight (uninstall the pipeline
    // taps, then Recorder::stop) BEFORE the recorder members — which are
    // declared after pipeline_ and therefore destroyed first — can dangle
    // under a still-running DSP thread. run()'s teardown already does this
    // on the normal exit path; the destructor is the safety net.
    ~AppWindow();

    // Runs the shell until the window is closed, or — when `frames` >= 0 —
    // for exactly that many rendered frames. The bounded mode is the
    // `--frames N` self-test contract that the app_smoke ctest entry relies
    // on: render N frames, shut down cleanly, exit 0.
    //
    // Returns the process exit code: 0 on clean shutdown, 1 when GLFW or the
    // ImGui backends fail to initialize (the reason is printed to stderr).
    int run(int frames = -1);

    // Where hang reports go, and whether the frame loop should stage a
    // deliberate stall. Both are decided in main(), because only main() knows
    // whether this is a real session or a bounded CI run - and a bounded run
    // must leave nothing on the machine it ran on.
    void setDiagnosticsDir(std::string crashDir);
    void setDiagStallMs(int ms);
    // --diag-toggle on|off: +1, -1, or 0 for "leave it alone". Flips the
    // Settings > Diagnostics switch on frame 30, through the same function the
    // checkbox calls, so the mid-session behaviour of that switch can be
    // proved against the real application.
    void setDiagToggle(int mode);

private:
    // One plugin's map page — declared ahead of the drawing methods that take
    // it, defined in full beside the map state below.
    struct MapPage;

    void drawUi();
    void drawToolbar();
    // The right-hand column: what the receiver is doing, gathered from where it
    // was already scattered. Drawn only when the window is wide enough that
    // taking 230 px from the spectrum is not a bad trade.
    void drawStatusColumn();
    // The tuned-frequency counter, drawn into the geometry the top bar hands
    // it: the screen position of the well's top-left corner, and the scale the
    // bar is drawn at. Plain floats rather than an ImVec2 because this header
    // is deliberately free of ImGui types - see the forward declarations
    // above.
    void drawFrequencyReadout(float wellX, float wellY, float scale);
    void drawMenuColumn();
    // The update banner, and the work behind it. Drawn at the top of the menu
    // column because a build that cannot see the user's radio is the most
    // useful thing this application can say to them, and it is worth more than
    // whatever they opened the panel for.
    void drawUpdatesSection();
    void drawUpdateBanner();
    void startUpdateCheck();
    void startUpdateDownload();
    void pollUpdateAsync();
    // Once-a-second check that the output stream is still alive, reopening it
    // if it is not. See AudioOut::streamAlive() for what kills one; the short
    // version is that a dead sink is invisible from inside the app, so the
    // only fix is to keep asking.
    void pollAudioHealth();
    // Collects a finished asynchronous audio-device open. Called once per
    // frame, BEFORE pollAudioHealth: the watchdog must not judge a sink that
    // an open has just handed back.
    void pollAudioOpen();
    // Asks for an output device through audioOpen_, and applies the result
    // immediately when the device answered inside the bound. `recovery` marks
    // a request the audio watchdog made rather than the user, so only those
    // are counted as recoveries. Returns true when the open completed here.
    bool requestAudioOpen(int deviceIndex, bool recovery);
    // The GUI-thread half of an open, wherever it completed: republishes the
    // channel layout for the DSP thread, re-enumerates, and puts the Sinks
    // combo back on the device that is actually playing.
    void applyAudioOpenResult();
    // Takes every live plugin handle off the pipeline and the UI, in the one
    // order that is safe, then unmaps the modules. The ONLY way any code here
    // may call PluginHost::unloadAll() — see the note in its body.
    void detachAndUnloadPlugins();

    // Starts the downloaded installer and asks the run loop to exit. Separate
    // because an installer cannot replace a binary that is still running, so
    // "install" necessarily means "and close this".
    bool launchInstaller(const std::string& path);
    void requestClose() { closeRequested_ = true; }

    // THE WATCHDOG BRACKET EVERY SHELL CALL GOES THROUGH. ShellExecute blocks
    // the GUI thread for as long as the shell takes, which for an elevation or
    // SmartScreen prompt is as long as the USER takes - and the watchdog filed
    // a hang against exactly that in 0.96.2 ("hang ntdll.dll @
    // cascade::gui::AppWindow::launchInstaller"). See gui/shell_open.hpp for
    // why this is a pause and not a helper thread.
    cascade::gui::ShellPauseHooks watchdogShellHooks();
    // Hands a folder or a URL to the shell under that bracket. Every
    // ShellExecute in this file goes through here or through launchInstaller;
    // there is no third spelling.
    bool shellOpen(const std::string& target);

    void drawSourceSection();
    // The three sections that used to be written inline in drawMenuColumn,
    // and the DECODE bank's list. See gui/rail_banks.hpp for why the rail is
    // five banks and drawMenuColumn for the dispatch.
    void drawRadioSection();
    void drawSinksSection();
    void drawDisplaySection();
    // The trail width slider (0.99.26), in Display and in the map deck alike.
    void drawTrailWidthControl(const char* label);

    // --- LANGUAGE & COUNTRY (app_window_language.cpp) ------------------------
    //
    // The first section of the SYSTEM bank. The language is applied only by
    // applyPendingLanguage(), called before each frame begins - never in the
    // middle of one, where half the frame would be drawn in each language and
    // widget labels would change under ImGui mid-submission. It starts
    // pending, so the first frame applies whatever applyConfig restored (or
    // FOXSDR_LANGUAGE, which overrides the saved setting for that first
    // application only and is never written back).
    void drawLanguageSection();
    void applyPendingLanguage();
    std::string languageSetting_ = "auto";  // AppConfig::language, as chosen
    std::string countrySetting_;            // AppConfig::country, "" = not set
    bool languageApplyPending_ = true;
    bool languageEnvConsumed_ = false;
    std::string countryFilter_;  // the Country combo's type-to-narrow text
    // The country list in the order the active language reads it, rebuilt
    // when the language changes rather than sorted every frame.
    std::vector<std::size_t> countryOrder_;
    std::string countryOrderLanguage_;
    void drawDecodeBank();
    // FOXSDR_OPEN_DECODERS: puts the rail on DECODE for a self-capture.
    void selectDecodeBankForCapture();
    // The five bank keys under the rail's title, at the column's top-left
    // (colX, colY) and width colW, laid from bodyTop. Returns the y the
    // sections start at. Reads the keyboard too: F1..F5, one per key.
    float drawRailBankKeys(float colX, float colY, float colW, float bodyTop);
    // The fade a newly selected bank comes up with, drawn over the sections
    // child from inside it, so it covers hand-drawn plates and widgets alike.
    void drawRailBankCurtain();
    // Runs one SoapySDR enumeration into soapyDevices_ and re-points the
    // combo selection at the active device by args (labels can repeat; a
    // device that vanished from the scan leaves sourceSel_ = -1 and the
    // preview falls back to the live source name). Called from the combo's
    // first open, from Refresh and from the web interface's scanDevices —
    // deliberately never from the constructor (see soapyDevices_ below for
    // why).
    //
    // NEVER WHILE A RADIO IS OPEN (0.90.1). The scan's child-process probe
    // opens and resets every dongle it finds - the streaming one included -
    // and the 0.90.0 field report (NESDR SMArt v5, 2026-09-09) is our next
    // control call dying twelve seconds after exactly that. While a device is
    // open the scan is deferred instead: the list stays as it is, the open
    // device is given a row if it has none, one diag line says why, and
    // soapyScanned_ is left false so the next draw after the radio closes
    // scans as before. The decision itself is gui::deviceScanAllowed.
    void scanSoapy();
    // True while scanSoapy() is refusing to run (see deviceScanAllowed): the
    // Refresh key is disabled with the reason as its caption. Read every
    // frame the Source section draws, so it is never stale.
    bool soapyScanGated() const;
    // The name a deferral names the open radio by - the sanitised model of
    // deviceArgs_, the label of an open in flight, or a radio this session
    // could not release.
    std::string soapyScanGateDevice() const;
    // What scanSoapy() would do right now (2026-09-23): a whole scan, one that
    // leaves the open radios' drivers out, or nothing - built from the
    // receiver's radio and every patch radio. See gui/device_scan_plan.hpp.
    cascade::gui::SoapyScanPlan soapyScanPlan() const;
    // Combo-row click handler: 0 = generator, 1 = IQ file (panel only — the
    // pipeline switches on a successful Open), 2+i = soapyDevices_[i]
    // (opens immediately; on failure the combo selection is left unchanged).
    void selectSource(int idx);
    void drawCenterPanels();
    // THE SLIM TICK STRIP BETWEEN THE PANELS IS GONE, and this is where it was
    // declared. SpectrumView now letters the frequency axis along the foot of
    // its own well from the ticks drawCenterPanels hands it in
    // SpectrumView::Chrome, which is where the reference face puts it; the
    // strip would have been the same scale drawn a second time, eighteen
    // pixels below the first, disagreeing with it the moment either formatter
    // changed.

    // --- Recorder / Bookmarks / Scanner (P6) ----------------------------------
    void drawRecorderSection();
    void drawBookmarksSection();
    void drawScannerSection();

    // --- Stereo / RDS / audio filters / band plan / plugins (P7) --------------
    // Drawn inside the Radio section, and only while WFM is the active mode:
    // the pilot indicator, the force-mono toggle, and the RDS readout are
    // meaningless for every other demodulator.
    void drawStereoRdsControls();
    // "Audio filters": noise reduction + manual/auto notch, in the order the
    // pipeline applies them (notch -> auto-notch -> NR; see Pipeline).
    void drawAudioFilterSection();
    // "Plugin store": the rail KEY that opens the plugin store window, and
    // nothing else. The catalogue itself - fetching, the update plans, the
    // reach panel and the fit gate - is the window's, exactly as every
    // satellite control is the satellites window's: a function that gets its
    // own window gets a shape, and the rail row becomes the key that opens it
    // rather than a lid over a drawer.
    //
    // The RETIRED rows still hang under this key (drawBlockedPluginRows). They
    // are the catalogue version policy's doing and their remedy is an update,
    // so they belong to the store side; the store WINDOW cannot carry them
    // because PluginStoreModel has no field for a module the host never saw.
    void drawPluginStoreSection();
    // "Plugins": the rail KEY that opens the fitted-modules window. What is
    // installed on this machine, whether it is running, and why it is not, all
    // live in that window; this row is the switch and the chip that reports
    // fed-of-fitted without opening it.
    void drawPluginsSection();
    // The red rows: every plugin the cached catalogue policy retires, each
    // carrying PluginRepo::pluginBlockMessage() verbatim. Drawn under the
    // store's key because a disabled plugin is news about the catalogue.
    void drawBlockedPluginRows();
    // "Decoders": what neither new window carries and what would otherwise
    // have been lost when the two section bodies moved into them - the decoder
    // output window's switch, the radar scope's switch, each loaded plugin's
    // PRESETS and its mute-while-running override, and the receiver-control
    // grants that belong to plugins no longer installed. Every one of these is
    // a control that exists today; none of them is a fitted-module fact, and
    // the fitted window's own action set (plugins_view.hpp) has no room for
    // them.
    void drawDecodersSection();
    // The PLUGIN STORE window: cabinet, corner screws, the title plate as
    // content, and PluginStoreView filling the rest. Its own operating-system
    // window with the native frame, like the satellites map.
    void drawPluginStoreWindow();
    // The FITTED MODULES window, the same treatment. Drawn AFTER the store in
    // the same frame - see pluginBrowserDrawnThisFrame_ for the one ordering
    // constraint that survived both bodies becoming windows.
    void drawFittedModulesWindow();
    // "Receiver control": the checkbox that grants one plugin permission to
    // tune the radio. Without it the permission PluginUi enforces could never
    // be given — every request_tune was refused and the user had no way to say
    // yes — which made a satellite tracker's Doppler correction unreachable.
    // The one-click rows under an installed plugin: "ADS-B 1090 MHz" and the
    // like, declared by the plugin itself through CASCADE_CAP_PRESET.
    // The list of decoded targets down the side of the Map window: callsign,
    // id and a details button per row, with click-to-go-to and double-click to
    // follow. A map answers "where is everything"; this answers "what am I
    // hearing", and clicking answers "take me to that one".
    //
    // THREE THINGS, NOT EIGHT. It was an eight-column sortable table, and in
    // the width the list actually gets every heading was truncated - a table
    // sorted by columns nobody can read is worse than the plain list it
    // replaced. The other six values moved into the details window below, and
    // the eight sort keys into one labelled menu above the table, so nothing
    // that could be asked before has stopped being answerable.
    //
    // Per page now: `tracks` is the page's plugin's tracks only, already
    // filtered by the caller, and every selection/follow gesture lands on that
    // page's own MapView.
    void drawTrackList(MapPage& page,
                       const std::vector<cascade::core::HostTrack>& tracks);
    // The compact sort control above the list: which key, and which way. It is
    // the ONLY way the list is ordered - the table itself is no longer
    // ImGui-sortable, because ImGui offers no public way to write the header's
    // sort arrow back, so a menu and a clickable header would sooner or later
    // have shown a "Callsign" arrow over rows ordered by distance.
    void drawTrackSortControl();
    // "Target details": the full block for the one target whose row button was
    // pressed, in a window of its own. See the implementation for why it is a
    // window rather than a popup or a panel under the list.
    void drawTargetDetailsWindow();
    // The same block as a section in the MAIN window's menu column, so the
    // craft being watched is readable without the Map window arrangement —
    // the map is often on another monitor, or closed. Shows the target whose
    // Details button was pressed, else the followed one, else the selected
    // one.
    void drawTargetDetailsSection();
    // The by-id lookup both of those share: the host's track vector is rebuilt
    // every poll, so a stored pointer or index would go stale within a frame —
    // find it fresh, and only among tracks the staleness rule still shows.
    const cascade::core::HostTrack* findVisibleTrack(const std::string& id) const;

    // --- the ADS-B radar scope (see gui/scope_view.hpp) ---------------------
    //
    // THE WHOLE MAIN WINDOW, drawn instead of the spectrum, the waterfall and
    // the menu column. That is what the mode IS: a beta tester's dedicated
    // ADS-B receiver shows a scope and nothing else, and a scope squeezed into
    // a panel beside a waterfall would be a smaller version of the map rather
    // than the instrument he asked for.
    //
    // IT ALWAYS DRAWS ITS OWN WAY OUT, in a bar above the face, before
    // anything that can fail to have room. A mode with no exit is the
    // map-page latch again in a larger form: there, every reopen affordance
    // lived inside the window the user had just closed.
    void drawScopeMode();
    // The switch that turns it on, in the rail's "Turn on and off plugins"
    // section (its widget id is still ###decoders).
    void drawRadarSection();
    void drawScopeModeControl();
    // The receiver position entry - two coordinate fields and "Set RX here" -
    // drawn by BOTH the map pages and the scope's no-position state. One copy,
    // because the button has consequences beyond the two numbers (every map
    // page's home moves, the coverage accumulator is discarded because every
    // wedge in it was measured from somewhere else) and a second copy would
    // sooner or later do only some of them.
    void drawRxPositionEntry();
    // The one-click ways to a receiver position, drawn wherever the entry is
    // asked for and there is none yet: the middle of the aircraft being heard,
    // and the centre of a map page the user has been looking at. See the
    // implementation for why each is offered and how it is labelled.
    void drawReceiverPositionOffers();
    // THE GPS ROW (0.86.0): a port name, a baud, and "Read position from
    // GPS", as ONE copy drawn from three places: drawReceiverPositionOffers
    // (the rail's Radar section and the scope's empty state, while there is
    // no position), the rail's "Receiver position" fold (once there is one -
    // opened for the user on the frame a GPS fix is applied, so the "position
    // set" line is seen), and every map page's bar, always. The port list is
    // read from the machine only when its drop-down is opened, never per
    // frame. See the definition for what the row promises.
    void drawGpsPositionControl();
    // ONCE A FRAME, and once more after the last frame: takes the fix the
    // reader accepted, if there is one, and hands it to applyReceiverPosition
    // - the only door a position enters by. The single-shot takeFix() is
    // what keeps a 60 Hz poll from re-applying it (and resetting the
    // coverage map) sixty times a second.
    void pollGpsReader();
    // WHAT "SET RX HERE" ACTUALLY DOES, as a function, because there are now
    // THREE ways to say where the antenna is: the toolbar's fields, the
    // satellites window's coordinate cells, and a click on that window's map
    // while SET FROM MAP CLICK is armed. Every one of them has to move every
    // page's home, tell the scope, discard the coverage accumulated from the
    // old origin and put the toolbar's fields back in step - and a second
    // hand-written copy would sooner or later do only some of that.
    //
    // The pair is REFUSED, not clamped, outside -90..90 / -180..180: a typo
    // must not be able to install a receiver at the pole and quietly make
    // every distance on the window wrong. Answers whether it applied.
    bool applyReceiverPosition(double latDeg, double lonDeg);

    void drawPluginPresets(const cascade::core::LoadedPlugin& p);
    // Tunes to a preset, sets the mode/bandwidth/device rate it asks for,
    // rebuilds the decoders against the new receiver state, and opens what
    // that plugin contributes. The ONLY callers are a button and the deferred
    // preset-bar/web-remote apply paths, each of which has already re-read
    // and re-validated `ps` from the plugin itself: a preset is a plugin
    // publishing where it listens, never a plugin retuning the radio — that
    // still needs the separate per-plugin permission.
    void applyPluginPreset(const cascade::core::LoadedPlugin& p, const CascadePreset& ps);
    // The window half of a preset press: the plugin's map page, picture,
    // panels and instruments, or Decoder output for a text decoder.
    void openPluginWindowsFor(const cascade::core::LoadedPlugin& p);
    // THE ONE ENUMERATION, used by drawPluginPresets, maybeAutoPreset,
    // rebuildMuteStates and the web status snapshot: walks `p`'s preset table
    // (capped at kMaxPresetsPerPlugin, exactly as every consumer has always
    // capped it) and keeps only what cascade::gui::presetIsValid accepts,
    // paired with the RAW index get() was called with — the index a deferred
    // apply must record, because it is not the same number as this preset's
    // position in the returned (filtered) vector. Empty when p.preset is
    // null. This is a real call into third-party code and must only be
    // called on a plugin-set change, never once per frame per window — see
    // muteStates_ and the long comment on rebuildMuteStates for why.
    std::vector<cascade::gui::IndexedPreset> validatedPresets(
        const cascade::core::LoadedPlugin& p) const;
    // The REMNANT of the receiver-control rows, and it is the half the fitted
    // window cannot draw: the refusal notice PluginUi records, and the grants
    // held by modules that are NOT installed any more. The fitted window
    // carries the grant key for every module it lists, so those rows are not
    // repeated here - but it lists only what the host loaded, and a permission
    // the user can neither see nor revoke is exactly the kind that must not
    // exist. Draws nothing when there is neither a refusal nor a stale grant.
    void drawPluginTuneControls();
    // Grants or revokes one plugin, updating both the live PluginUi and the
    // persisted list. One function so the two can never disagree: a grant that
    // took effect but was not saved would come back revoked next launch.
    // `pluginKey` is a PluginUi::tuneKey() — the module file name, never the
    // display name, which the plugin itself chooses.
    void setPluginTuneAllowed(const std::string& pluginKey, bool allowed);
    // Pushes pluginTuneAllowed_ into pluginUi_. Called after every
    // PluginUi::rebuild, because rebuild follows a clear() that drops the
    // grants along with the instances — without this a rescan silently revoked
    // every permission the user had given.
    void applyPluginTuneGrants();

    // --- HOST API LEVEL 1 (0.99.31) - see core/plugin_api.hpp ---------------
    //
    // The SETTINGS grant, the level-1 twin of the tune grant: same keying,
    // same persistence, same re-application after every rebuild.
    void setPluginSettingsAllowed(const std::string& pluginKey, bool allowed);
    void applyPluginSettingsGrants();
    // ONCE A FRAME, straight after applyWebControls: applies what plugins
    // asked of the receiver (through applyControlRequest, the code a click or
    // a browser goes through), turns their log lines into decoder-output lines
    // and plate notices, folds their settings into the config, and publishes
    // the receiver snapshot they read. GUI thread - the only thread that may
    // touch the receiver, which is the whole reason plugin requests are queued.
    void applyPluginApi();
    // The snapshot half of the above, on its own so a control applied this
    // frame is visible to a plugin in the same frame's snapshot.
    void publishPluginApiState();
    // ONE control request, applied. Extracted from applyWebControls unchanged,
    // so the browser, CAT and a plugin all go through literally the same code.
    void applyControlRequest(const cascade::net::ControlRequest& r);
    // The plugins' spectrum and waterfall marks, over the panel at (x0, y0).
    void drawPluginMarkers(float x0, float y0, float width, float height, bool waterfall);
    // HOW MANY LOADED MODULES ARE DECODERS AT ALL - the denominator under the
    // word DECODERS, and under the rail's fed-of-fitted chip.
    //
    // It is the RUNNER'S OWN TEST, not a new opinion: PluginRunner creates an
    // instance for a module that supplies a decoder, an I/Q decoder or an
    // image decoder table, and for nothing else (see its rebuild). Counting
    // every loaded module instead put a basemap and a track-info provider -
    // neither of which can ever be fed a signal - permanently in the
    // denominator of "of N installed, M not fed", so a perfectly healthy
    // receiver read as two decoders broken.
    std::size_t loadedDecoderCount() const;
    // HOW MANY OF THOSE ARE BEING FED - the numerator over the same
    // population, counted the same way: per MODULE, from
    // PluginRunner::isFeeding, which answers from the very status list the
    // fitted window's rows are drawn from.
    //
    // NOT PluginRunner::activeCount, which counts INSTANCES. A module may
    // declare both an audio decoder and an I/Q one and get an instance for
    // each, so an instance count over a module count is two populations in one
    // chip - and can read 3/2. It also does NOT test the receiver's run state:
    // "matched to the rate the pipeline is configured for" is a different
    // question from "the DSP threads are turning", and the callers that need
    // both check pipeline_.running() beside this, exactly as they always did.
    std::size_t fedDecoderCount() const;
    // Whether the user has stopped this plugin. `pluginKey` is a module file
    // name (cascade::core::pluginKey), the same identity the tune grant uses.
    bool pluginIsStopped(const std::string& pluginKey) const;
    // Records a stop or a start WITHOUT rebuilding: updates the durable list
    // and pushes it into the runner and the UI half, so the next rebuild sees
    // it. Split from the button's action below because applyPluginPreset has
    // to start a plugin and then rebuild ONCE, having also moved the receiver.
    void recordPluginStopped(const std::string& pluginKey, bool stopped);
    // The Stop/Start button's action: record it, then rebuild through the one
    // lifecycle path everything else uses, so a stop tears the plugin's
    // instances down and a start builds them against the CURRENT receiver.
    void setPluginStopped(const std::string& pluginKey, bool stopped);
    // "WE WANT THE USER TO HAVE TO DO NOTHING" (the owner's words). Called
    // from setPluginStopped's own START branch only: looks the plugin back up
    // by key, and if it carries presets and the receiver is not already
    // sitting inside one of them (gui/tune_control.hpp's
    // autoPresetIndexOnStart), applies the first exactly as if its own button
    // had been pressed. A no-op for a plugin with no preset table, and never
    // called on a stop or from config load — see the call site in
    // setPluginStopped and core::startupState for why neither reaches here.
    void maybeAutoPresetOnStart(const std::string& pluginKey);
    // THE SAME RULE, for the gesture the DECODE rail actually offers: opening
    // a plugin's own window. Since 0.79.1 a window is shown ONLY by a row's
    // click (never restored at start-up, never self-opened - PluginWindows
    // starts empty every launch and MapPage::open is cleared by
    // core::startupState), so that click is exactly as deliberate an "I want
    // this plugin now" as pressing START. Call ONLY when
    // cascade::gui::autoPresetTriggersOnWindowClick says this frame's click
    // just turned a window from hidden to shown - never on a click that hides
    // one. Shares its decision and apply path with maybeAutoPresetOnStart
    // through the private maybeAutoPreset() below; only the log line's verb
    // differs ("window opened" here, "started" there).
    void maybeAutoPresetOnShow(const std::string& pluginKey);
    // The body both of the above call: find the plugin by key, decide via
    // autoPresetIndexOnStart, apply through applyPluginPreset, and log with
    // `verb` standing in for what just happened ("started" / "window
    // opened"). `verb` is a string literal from the two call sites, never
    // plugin-supplied text.
    void maybeAutoPreset(const std::string& pluginKey, const char* verb);
    // THE REVERSE OF cascade::core::pluginKey(): HostImage/HostPanel/
    // HostInstrument/MapPage all carry a plugin's DISPLAY name (LoadedPlugin::
    // name), the same identity drawPluginWindowRows and drawMapPageSections
    // build their window ids from - never the module FILE NAME
    // maybeAutoPresetOnShow needs to look the plugin back up by, the same
    // key setPluginStopped/recordPluginStopped use. Empty when no loaded
    // plugin answers to `displayName` (an unloaded or since-removed module),
    // which the two callers below treat as "nothing to auto-preset".
    std::string pluginKeyForDisplayName(const std::string& displayName) const;

    // --- Preset bars: a plugin's own window offers its own presets (0.99.0) --
    //
    // Looks up `displayName` (a HostTrack/HostImage/HostPanel/HostInstrument
    // /MapPage plugin tag — the same identity pluginKeyForDisplayName
    // resolves) in muteStates_, which is the ONE cache of a plugin's
    // validated presets that already exists and is already rebuilt on every
    // plugin-set change (see rebuildMuteStates) — never a fresh count()/get()
    // walk, which is what the header comment above rebuildMuteStates refuses
    // to let the frame path do. Null when no cached entry answers to that
    // name (an unloaded plugin, or one with no presets at all — see the
    // caller, which treats null the same as "nothing to draw").
    const cascade::core::MutePlugin* muteStateForDisplayName(const std::string& displayName) const;
    // THE BAR FOR ONE WINDOW: a map page, an image window, a panel or an
    // instrument window, each of which owns exactly one plugin and knows its
    // own display name. Draws nothing and costs no vertical space when that
    // plugin publishes no valid preset. A key's own press only RECORDS a
    // request into pendingPresetRequest_ — see the long comment beside that
    // member for why the apply cannot happen here, mid-iteration of the very
    // lists a preset's own apply rebuilds.
    void drawPluginPresetBar(const std::string& displayName);
    // THE SHARED DRAWING OF ONE PLUGIN'S ROW OF KEYS, used by
    // drawPluginPresetBar (one plugin, its own bar) and drawDecoderPresetBars
    // (several text decoders, one bar each, grouped under the shared Decoder
    // output window). Wraps within the available width via
    // cascade::gui::presetBarRows; every press records into
    // pendingPresetRequest_ under `pluginKey`, never applies inline.
    void drawPresetKeys(const std::string& pluginKey, const std::string& pluginName,
                        const std::vector<cascade::core::MutePreset>& presets);
    // THE GROUPED BAR IN THE SHARED DECODER OUTPUT WINDOW — "if I'm watching
    // ADS-B I can click a button on POCSAG" for exactly the plugins that have
    // no window of their own (POCSAG, FLEX, CW, RTTY, APRS, ...): every
    // loaded, NOT-stopped text decoder (p.decoder != nullptr) that publishes
    // at least one valid preset gets its own dimmed name followed by its own
    // keys, one plugin per line. A stopped decoder is skipped — a preset key
    // for a plugin the user just switched off would be confusing, and the
    // rail's own "Start"/preset buttons are already the way back in.
    void drawDecoderPresetBars();
    // THE SAFE POINT: called once a frame, from drawUi, AFTER drawPluginWindows
    // has finished every one of its loops — never from inside one. Consumes
    // pendingPresetRequest_ (at most one; see its own comment) and, if there
    // is one, re-resolves it against the CURRENT plugin list and preset
    // table (cascade::gui::presetRequestStillValid) rather than trusting
    // anything carried from the frame the key was pressed on. A plugin
    // unloaded in between, or an index that no longer names a valid preset,
    // is a silent no-op — exactly the contract a stale request must have.
    void consumePendingPresetRequest();
    // The user's own presets for one loaded plugin, in saved order (see
    // core/user_presets.hpp). Keyed by the version-stripped module id, so a
    // plugin update keeps them.
    std::vector<cascade::core::UserPreset> userPresetsForPlugin(
        const cascade::core::LoadedPlugin& p) const;
    // Applies pendingUserPresetEdit_ if there is one: a Save stores the
    // receiver's CURRENT tuning (absolute frequency, mode, channel bandwidth)
    // against that plugin, a Forget removes one. Reports what happened in
    // presetNote_. Called only from the safe point.
    void consumePendingUserPresetEdit();

    // --- Audio mute while a data decoder is running (see plugin_ui.hpp) -------
    // The EFFECTIVE "mute audio while running" setting for one plugin: the
    // default its capabilities imply, flipped if the user has overridden it.
    bool pluginMutes(const cascade::core::LoadedPlugin& p) const;
    // Records the user's choice as an override of the capability default, so
    // ticking the box back to the default REMOVES the entry rather than
    // recording a second kind of "yes". Rebuilds the mute snapshot.
    void setPluginMutes(const cascade::core::LoadedPlugin& p, bool mutes);
    // Rebuilds muteStates_ from the loaded plugins: identity, running state,
    // effective setting, and the plugin's presets.
    //
    // A SNAPSHOT REBUILT ON CHANGE, not read per frame, because reading the
    // presets means CALLING the plugin - count() and get() are its own code -
    // and doing that once per plugin per frame to decide whether to be quiet
    // would put third-party code on the frame path for no gain. Presets are a
    // property of the plugin and not of a running instance (the ABI says so),
    // so they cannot change without a rescan.
    void rebuildMuteStates();
    // Once per frame: evaluate the policy against where the receiver actually
    // is, push the result into the pipeline, and arm the popup on the edge.
    void updateAudioMute();
    // The modal that offers to stop the plugins holding the audio down, and
    // the banner that stays when the user declines.
    void drawMutePopup();
    void drawMuteBanner();
    // Stops exactly the plugins named by `keys`, through the ordinary stop
    // path, in one rebuild. The caller passes the keys its own message named -
    // the banner passes mutedByKeys_, the popup passes what it captured - so a
    // button can never stop something other than what the words above it said.
    void stopMutingPlugins(const std::vector<std::string>& keys);
    // "ADS-B decoder", or "ADS-B decoder and AIS decoder", or a comma list.
    // One place, because the popup, the banner and the Sinks panel all have to
    // name the same plugins the same way.
    static std::string muteNameList(const std::vector<std::string>& names);
    std::string muteSubjectText() const;
    // Decoder OUTPUT: what the loaded plugins are actually decoding, plus a
    // line per plugin that is loaded but not being fed and why. Drained from
    // PluginRunner every frame, because the runner's buffer is bounded and a
    // GUI that stops reading would silently drop the newest lines.
    // The button that opens the output window, and the count of what has been
    // decoded into it. The per-decoder IDLE REASONS are no longer printed
    // here: the fitted-modules window quotes the runner's own sentence against
    // the module it belongs to, which is where a user goes to ask why a
    // decoder is silent, and one sentence in two places is how two surfaces
    // come to describe one idle decoder two ways.
    void drawDecoderStatusRows();
    // The decoded text, in its own operating system window.
    void drawDecoderWindow();
    // --- the transmitter (0.95.0) --------------------------------------------
    //
    // WHY THE SWITCH IS IN SIGNAL PATH AND NOT IN VIEW, which is where the
    // demod scope's went. The scope is a way of LOOKING at what the receiver
    // produced; the transmitter is a STAGE, the outbound one, and it belongs
    // with the source, the radio and the sinks it is the counterpart of. A
    // panel whose "what the samples pass through" bank had everything except
    // the direction they go out in would be hiding the transmitter from the
    // one person looking for it.
    void drawTransmitSection();
    // The page: the frequency, the mode, the power, the input, the key.
    void drawTransmitPage();
    // Opens or closes the transmit radio. Bounded, and on the GUI thread -
    // opening a Pluto is a TCP connect with iiod::kConnectWait on it, not a
    // USB enumeration, so it is fast enough not to need a worker.
    void openTransmitRadio();
    void closeTransmitRadio();
    // Pushes the receiver's centre into the transmitter when SPLIT is off,
    // and nothing when it is on. Called once a frame.
    void followTransmitFrequency();

    // --- the patch page ------------------------------------------------------
    // The canvas: radios, channels, decoders and displays wired together.
    void drawPatchPage();
    // The key that opens it, FIRST in the SIGNAL PATH bank. It goes there
    // rather than in VIEW by the same test that put the recorder and the
    // transmitter in that bank: a patch is not a way of LOOKING at the signal
    // path, it IS the signal path, and it decides what the path consists of.
    void drawPatchSection();

    // --- the demod scope -----------------------------------------------------
    // The page itself: the tube, its keys and its readouts.
    void drawDemodScopePage();
    // The switch that opens it, in the VIEW bank beside the radar scope's.
    void drawDemodScopeSection();
    // Reads whichever tap the selected signal needs into the member buffers,
    // finds the trigger, and (in the spectrum position) transforms. Split out
    // because it is the half of the page that has nothing to do with drawing
    // and everything to do with what is being drawn.
    void gatherDemodScope(cascade::gui::DemodScopeFeed& feed);
    // Is the receiver demodulating wideband FM right now? The one question
    // the multiplex position turns on, asked in one place so the key row, the
    // feed and the saved-position fallback cannot disagree.
    bool pipelineIsWfm() const;
    // The saved scope position, against today's mode: MPX falls back to the
    // audio spectrum outside WFM WITHOUT rewriting the setting, so returning
    // to FM finds the scope where it was left.
    cascade::gui::ScopeSignal scopeSignalNow() const;
    // Moves decoded lines out of the runner into decoderLog_. Called from
    // drawUi unconditionally, because the runner's buffer is bounded and
    // draining only when the panel is visible would drop output silently.
    void pumpDecoderOutput();
    // Rebuilds every decoder instance against the CURRENT source rate and
    // centre frequency. Called after any source change, because both are
    // passed to a decoder's create() and cannot be changed afterwards.
    void refreshPluginRunner();
    // The map pages and every plugin-declared panel window. Drawn as their
    // own top-level windows rather than inside the menu column: a map squeezed
    // into a 300 px sidebar is not a map, and a plugin's window should be
    // movable and resizable like any other.
    void drawPluginWindows();
    // The page for a plugin, by the display name every HostTrack carries.
    // find returns null for a plugin with no page yet; ensure creates one
    // lazily — its own MapView, the receiver position applied, and geometry
    // seeded from the saved entry or from the legacy single-window rectangle
    // staggered by page index.
    MapPage* findMapPage(const std::string& plugin);
    MapPage& ensureMapPage(const std::string& plugin);
    // THE WHOLE CONTENT OF A SATELLITES MAP WINDOW: the cabinet, its four
    // screws and the SATELLITES MAP plate drawn as CONTENT inside the
    // operating system's own frame, with MapView::drawSatellitePanel filling
    // the plate. The frame, its minimise/maximise/close and its resize edges
    // stay Windows' own - torn-off windows here are real OS windows on
    // purpose, after a user could not find the resize edges on an undecorated
    // one.
    void drawSatelliteMapBody(MapPage& page);
    // ONE ROW ON THE FUNCTION SELECT RAIL PER MAP PAGE, in DECODE. A SWITCH,
    // not a section: it opens and closes that page's window and reports what
    // the window holds, and for the satellites page it is the whole of the
    // satellite presence in the main window - every satellite control lives in
    // the window itself, which is the point of the window.
    //
    // IT USED TO SKIP EVERY PAGE THAT WAS NOT THE SATELLITES ONE, and since
    // 0.79.1 - when nothing opens a page by itself any more - that left the
    // ADS-B, AIS and APRS maps reachable only through a preset, which also
    // retunes the radio. A map is not a thing a user should have to move the
    // receiver to look at.
    void drawMapPageSections();
    // One row per window a plugin publishes (a picture, a panel): the only
    // way such a window reaches the screen since 0.79.1.
    void drawPluginWindowRows();
    // Folds every live page's rectangle and open flag into mapPagesSaved_,
    // which is what currentConfig() writes out. Entries for plugins with no
    // page this session ride through untouched, so a geometry saved for a
    // plugin that is temporarily uninstalled is not erased by unrelated saves.
    void syncMapPagesToSaved();
    // Starting position and size for a window that should be its OWN operating
    // system window rather than a panel inside the main one. `slot` staggers
    // several of them. See the definition for why the position is what decides
    // this — ImGui has no flag for it.
    void placeAsSeparateWindow(int slot);
    // The size it opens at. Named rather than written twice because the same
    // pair is what beginPage is handed as the page's default size, and a
    // "reset window sizes" that put a window back to a DIFFERENT rectangle
    // than the one it opens at would be a third size nobody asked for.
    static constexpr float kSeparatePageW = 720.0f;
    static constexpr float kSeparatePageH = 520.0f;
    // The same, at a size the caller asks for and MOVED so all of it - the
    // resize grip in the bottom-right corner included - lands on the monitor
    // it opens on (mapPlaceDefaultRect). placeAsSeparateWindow's fixed
    // 720 x 520 is too small for a catalogue and its data plate, and an
    // anchor that knows nothing about the monitor is how the map window came
    // to open with its bottom off the work area. FirstUseEver throughout, so
    // this is where a window OPENS and never fights a later drag.
    void placeFeatureWindow(int slot, float wantW, float wantH);
    // The same, but preferring a rectangle SAVED from a previous session.
    // The saved one is used only if it is still reachable on the monitors
    // this machine has now, and only after being clamped to what fits where
    // it lands - the two rules the map pages learned the hard way, applied
    // through the same mapGeometryOnScreen/mapClampRestoredSize pair rather
    // than through a second copy of them. Anything else falls back to
    // placeFeatureWindow's default placement for `slot`. x/y/w/h are the
    // caller's saved rectangle and are updated in place by the clamp, so what
    // is persisted is what the window was actually asked to be.
    void placeSavedFeatureWindow(int slot, int& x, int& y, int& w, int& h, float wantW,
                                 float wantH);
    // The same anchor as a VALUE rather than as a side effect. The map needs
    // it before it is used: its default rectangle has to be checked against
    // the monitor (mapPlaceDefaultRect), and a function that only calls
    // SetNextWindowPos cannot answer where the window would have gone.
    static void separateWindowAnchor(int slot, float& x, float& y);
    // Where the map window should open when the config has no saved geometry:
    // a size derived from the MONITOR's work area, not a constant. See the
    // definition for why a constant was the "map screen isn't large enough"
    // report.
    static void mapDefaultSize(float& widthPx, float& heightPx);
    // Translucent service-band rectangles over the spectrum panel, plus the
    // labels that fit. `pos` is the panel's screen-space top-left as recorded
    // before the spectrum was drawn.
    void drawBandPlanOverlay(float x0, float y0, float width, float height);
    // Band plan (optional program data) and plugins (optional user
    // installs) — both silently absent when their directory does not exist.
    void loadBandPlan();
    void rescanPlugins();
    // Every tune that moves the SOURCE centre has to tell the pipeline, which
    // cannot see it: the RDS/stereo decoders must forget the old station.
    //
    // For a hardware (Soapy) source this is a REQUEST, paced through
    // retuneCoalescer_: bursts (one wheel notch per frame is 60-144 tunes a
    // second) collapse to at most one device call per ~50 ms, latest value
    // winning — the gesture that produced the most frequent 0.62.0 field
    // crash. A single tune still applies immediately. The generator and IQ
    // file sources apply immediately always (no USB to pace).
    //
    // isPluginPreset: true only from applyPluginPreset, and only so a
    // mismatch this retune produces (see noteTuneMismatch) can say the
    // PRESET needs a receiver that covers that band, rather than leaving an
    // unexplained tune. Every other caller takes the default.
    void retuneSourceHz(double centerHz, bool isPluginPreset = false);
    // The unpaced apply: setCenterFrequencyHz + decoder resets + readback.
    // Call directly only where the readback must be valid on return (the
    // carry-across on a fresh device open); everything else goes through
    // retuneSourceHz.
    void applyRetuneNow(double centerHz, bool isPluginPreset = false);
    // Frame-loop poll releasing a held retune once its interval has passed.
    void pollPendingRetune();
    // Compares what applyRetuneNow asked for against what the source actually
    // landed on (SoapySDR devices coerce; the generator and IQ file never
    // do) and, past kTuneMismatchToleranceHz, sets tuneMismatchNote_ (shown
    // in the Source section) and logs once per distinct request. Pulled out
    // of applyRetuneNow only so its one non-trivial decision — the wording,
    // in gui/tune_control.hpp's tuneMismatchMessage — stays testable without
    // a device.
    void noteTuneMismatch(double requestHz, double answeredHz, bool isPluginPreset);
    // The refusal's counterpart: a tune the source would not make at all,
    // reported only when the request lies outside the range the radio itself
    // publishes (gui/tune_control.hpp, tuneRefusedMessage). Same note line,
    // logged once per distinct request.
    void noteTuneRefused(double requestHz, bool isPluginPreset);
    double lastRefusedRequestHz_ = -1.0;

    // Uninstalls the matching pipeline tap, THEN stops the recorder — the
    // order the Recorder contract requires (see Pipeline::set*Recorder).
    // Both are harmless no-ops when nothing is recording, so the toolbar
    // Stop path calls them unconditionally.
    void stopIqRecording();
    void stopAudioRecording();
    // The Recorder section's own "Record audio" path, lifted out of the button
    // so the keyboard presses the SAME button rather than a second copy of it
    // that could drift from the one on screen. Returns whether a take started;
    // the error, when it did not, is in recordError_ exactly as before.
    bool startAudioRecording();
    // The Radio section's own mode-button path, lifted out for the same
    // reason: a mode key must set the demodulator, its default bandwidth and
    // the log line identically to a click on the button beside it.
    void setModeIndex(int index);

    // --- The keyboard ---------------------------------------------------------
    // ONE PLACE IN THE FRAME where a pressed chord becomes an action, and one
    // function that performs it by calling the same handlers the mouse calls.
    // See gui/key_bindings.hpp for the table and why it looks like HDSDR's
    // without being identical to it.
    void dispatchKeyBindings();
    void applyKeyAction(cascade::gui::KeyAction action);
    // The capture box's own frame, run from the dispatcher so it resolves even
    // if the settings row that started it has gone off screen.
    void pollKeyCapture();
    // Selecting a bank is TWO things - the bank and the fade that brings it up
    // (see drawRailBankCurtain) - so the keys and the pushbuttons share one
    // call rather than each remembering to reset the fade.
    void setRailBank(int index);

    cascade::gui::KeyBindings keyBindings_ = cascade::gui::defaultKeyBindings();
    // WHICH ROW IS LISTENING FOR A KEY, as an index into the action table, or
    // -1 for none. While a row is capturing, dispatchKeyBindings performs
    // NOTHING: the next chord belongs to the box, not to the radio, or
    // pressing Ctrl+F2 to rebind it would stop the receiver on the way past.
    int keyCaptureAction_ = -1;
    // THE THREE THINGS A KEY ASKS FOR THAT ONLY A LATER PART OF THE FRAME CAN
    // DO. Raised by applyKeyAction and consumed - and cleared - where the work
    // belongs: the frequency editor is opened inside drawFrequencyReadout (it
    // needs the plate's own geometry and the figure on the tubes), the
    // screenshot is taken in the render loop after the frame is drawn, and the
    // key-binding row is opened when the SYSTEM bank next draws it.
    bool freqEditRequest_ = false;
    bool shotRequest_ = false;
    bool openKeyBindingsRow_ = false;
    // THE USER'S OWN MUTE, kept apart from the plugin mute (mutedBy_ and the
    // rest). They are different things with different lifetimes: a plugin's
    // mute is recomputed from the tuning every frame in updateAudioMute, and
    // one that also cleared a user's mute would make the Mute key stop working
    // the moment a decoder was running. The pipeline is told the OR of the two.
    bool userMuted_ = false;

    // ONE absolute-tune path shared by bookmark click-to-tune and scanner
    // retunes: commands the SOURCE center to (absHz - VFO offset) through
    // activeSource().setCenterFrequencyHz — the same setter + readback path
    // the toolbar digit wheel uses — so the VFO band (whose offset is
    // preserved) lands on absHz and the display follows the readback.
    void tuneAbsoluteHz(double absHz, bool isPluginPreset = false);
    // The tuned station: source center readback + VFO offset (what the VFO
    // band marks on the spectrum). This is what a bookmark captures and what
    // the scanner's user-tune detection compares.
    double currentAbsoluteHz();

    // Once-per-GUI-frame scanner driver (called at the end of drawUi):
    // detects manual tunes (user wins -> stop), feeds tick() with ImGui's
    // clock and the squelch-open state, applies returned retunes.
    void scannerFrame();

    // Persists the bookmark list after a mutation; failures land in
    // bookmarkError_ (red text). No-op in hermetic mode (empty path).
    // DEBOUNCED since 0.99.19: it marks the list dirty and the write happens
    // about a second after the last change (flushBookmarkSave, every frame and
    // at exit) - a 33 000-entry list takes ~40 ms to write, and a hitch on
    // every star clicked is exactly the slowdown an imported list must not add.
    void saveBookmarks();
    void flushBookmarkSave(bool force);
    // Imports an SDR# frequencies.xml or a CSV into the bookmarks.
    void importBookmarkFile(const std::string& path);
    // The filtered, cached view the Bookmarks list draws from.
    void rebuildBookmarkView();
    // Bookmarks inside the visible span, as marks on the spectrum.
    void drawBookmarkMarkers(float x0, float y0, float width, float height);

    // --- Config persistence (P5) ---------------------------------------------
    // Pushes every AppConfig field into the pipeline/panel mirrors; source
    // restore failures (file gone, device unplugged) fall back to the
    // generator silently except for lastError surfaced via sourceError_.
    void applyConfig(const cascade::core::AppConfig& cfg);
    cascade::core::AppConfig currentConfig();  // snapshot of the live state
    void maybeSaveConfig(double nowS);  // debounced: ~2 s after the LAST change
    void saveConfigNow();               // clean-exit save (unconditional)

    // THE ASYNC SAVE (0.97.2). Field report "hang ntdll.dll @
    // cascade::core::ConfigStore::save" (0.96.3): both functions above used
    // to write the file directly, on the GUI thread, inside whichever of
    // them called them - see gui/config_writer.hpp for the whole argument.
    // requestConfigSave() is now the ONE place either of them hands bytes to
    // configWriter_, so pollConfigWriter() has one place to apply the result
    // to savedCfg_ from. `cfg` is remembered as lastRequestedConfig_
    // unconditionally: because configWriter_ coalesces bursts to the LAST
    // content asked for, whichever write is on disk once configWriter_ has
    // fully drained (poll()/finishOrAbandon() returned with nothing left
    // in flight or queued) is guaranteed to be this content - so savedCfg_
    // never needs to be paired with a specific request, only compared
    // against "drained and ok".
    void requestConfigSave(const cascade::core::AppConfig& cfg);
    // Collects a finished configWriter_ write, applies it to savedCfg_ on
    // success (see requestConfigSave above), and logs a failure exactly as
    // the old synchronous save() call sites did. Called once a frame; a
    // failed save is retried automatically because savedCfg_ stays stale,
    // so the next frame's maybeSaveConfig() sees "still different" and
    // restarts the debounce window on its own.
    void pollConfigWriter();

    // Opens a radio of `kind` ("soapy" or one of the eight native driver keys)
    // by its args on
    // THIS thread, pushes the requested rate and the default gains, and fills
    // the panel mirrors. Null (with sourceError_ set) when the open fails.
    // Used by the config restore, which happens before there is a frame to
    // draw and therefore has nothing to keep responsive; the dropdown's own
    // path goes through launchDeviceOpen onto a worker.
    std::unique_ptr<cascade::source::DeviceSource> openDeviceSync(const std::string& kind,
                                                                  const std::string& args,
                                                                  double requestRateHz);

    // Constructs an unopened driver of `kind`; null for a kind this build
    // does not know. One place decides what a kind name means, so the
    // worker, the synchronous restore and any future caller cannot disagree.
    static std::unique_ptr<cascade::source::DeviceSource> makeDeviceSource(
        const std::string& kind);

    // Re-reads nativeDevices_ and nativeUnbound_ from the transport. Cheap,
    // ungated and safe at any time - see nativeDevices_ for why a native
    // enumeration is nothing like a Soapy scan.
    void scanNative();

    // WHERE EACH FAMILY'S ROWS START IN THE SOURCE COMBO. Row 0 is the
    // generator and row 1 the IQ file; the native radios come next, and the
    // SoapySDR devices after them. Named rather than written as "2" at the
    // dozen sites that index this list, because one of those sites forgetting
    // that the native block exists is an off-by-N that opens the wrong radio.
    static constexpr int kNativeRowBase = 2;
    int soapyRowBase() const {
        return kNativeRowBase + static_cast<int>(nativeDevices_.size());
    }

    // The label of the native row whose args are `args`, or the args
    // themselves when no row matches (a device that has since been
    // unplugged). Used for the model string a log line names the radio by.
    std::string nativeLabelFor(const std::string& args) const;

    // Fills every panel mirror (rates, gains and their ranges, AGC, antenna)
    // from an open DeviceSource, priming the hardware where the panel has to
    // push a value to agree with it. Shared by finishDeviceOpen and
    // openDeviceSync so the async and synchronous opens cannot drift apart -
    // they had two copies of this before, and they had already drifted.
    void adoptDeviceMirrors(cascade::source::DeviceSource& dev, const std::string& kind,
                            double requestRateHz);

    // Makes the DSP chain follow activeSource().sampleRateHz() (rate-follow).
    // A pipeline refusal — fractional channel rate — keeps the old chain and
    // surfaces the reason in sourceError_.
    void followInputRate();

    // DSP pipeline plus the two live display widgets it feeds. The views are
    // held by unique_ptr for two reasons: the forward declarations above, and
    // the waterfall's GL texture, whose deletion needs the creating GL context
    // current — run() tears the view down explicitly before destroying the
    // context, because AppWindow itself outlives it (destroyed in main()).
    cascade::core::Pipeline pipeline_;
    std::unique_ptr<SpectrumView> spectrum_;
    std::unique_ptr<WaterfallView> waterfall_;

    // Newest frame received from the pipeline. Cached here (not just handed
    // to the views) so the spectrum keeps drawing the last data after Stop —
    // SpectrumView::draw takes bins per call and holds no history of its own.
    cascade::core::SpectrumFrame lastFrame_;

    // --- WHAT THE TWO PANELS ARE ALLOWED TO SAY ABOUT THEMSELVES -------------
    //
    // SpectrumView::Chrome and WaterfallView::Chrome both refuse to invent a
    // figure: every annotation they draw needs an input, and an absent input
    // removes the element rather than defaulting it. These four members are
    // where those inputs are MEASURED, and each one is measured rather than
    // assumed for a reason stated at the site that maintains it.
    //
    // Time is glfw/ImGui time (seconds since start), not a wall clock: every
    // consumer is an ELAPSED figure, and a wall clock would drag time-zone and
    // step changes into an age.
    //
    // When the GUI took delivery of the newest spectrum frame. Negative until
    // the first one arrives, which is what makes "no reading" distinguishable
    // from "zero seconds old".
    double lastFrameSeenS_ = -1.0;
    // Rolling one-second window behind the waterfall's scroll rate. It counts
    // LINES THIS GUI ACTUALLY PUSHED INTO THE RING, which is not the same as
    // frames the DSP published: getLatestFrame hands over at most one frame per
    // call, so at 2 MS/s the pipeline publishes ~1950 frames a second and the
    // waterfall takes one of them per GUI frame. Measured from the sequence
    // instead, the strip claimed 1938 line/s over a picture scrolling at about
    // sixty, and reported its whole visible history as "0 s". A stalled
    // pipeline still closes a window on zero, because no poll succeeds and no
    // line is pushed - which is the property that mattered about not counting
    // GUI frames.
    double frameRateWindowS_ = -1.0;
    std::uint64_t waterfallLines_ = 0;
    std::uint64_t waterfallLinesAtWindow_ = 0;
    float framesPerSecond_ = 0.0f;

    // Display range for both the spectrum axis and the waterfall colormap.
    float dbMin_ = -110.0f;
    float dbMax_ = 0.0f;

    // Radio/Sinks control state. The pipeline owns the live DSP values; these
    // mirrors exist because ImGui widgets edit by pointer. Defaults match the
    // pipeline's own defaults (WFM, 150 kHz bandwidth, -50 dB squelch) except
    // the VFO offset, which the constructor pushes to +300 kHz so the demo
    // tone 0 sits on the VFO — near-silent in WFM (an unmodulated carrier
    // demodulates to DC), a clean 700 Hz sidetone in CW.
    float volume_ = 0.5f;
    int modeIndex_ = 1;                              // WFM
    float vfoOffsetKhz_ = 300.0f;
    // WHICH OF THE OFFERED BANDWIDTH STEPS THE VFO IS ON, or -1 for none of
    // them: a plugin preset may ask for a width the list does not carry (the
    // NOAA APT one asks for 40 kHz), and the combo then letters the real
    // figure and ticks nothing. It used to be the NEAREST step, which made the
    // control show 12.5k over a 40 kHz VFO and apply that 12.5k on the next
    // click. Never used to index kBwHz without a literal step beside it.
    int bandwidthIndex_ = 1;                         // 150k
    float squelchDb_ = -50.0f;
    // Output devices, enumerated once at construction (a hot-plug refresh can
    // come with the settings work in P5); index into devices_, -1 when empty.
    std::vector<cascade::sink::AudioDevice> devices_;
    int deviceIndex_ = -1;
    // Output-stream watchdog (see pollAudioHealth). The note is shown in the
    // Sinks panel: a stream that had to be restarted is something the user
    // should be told about, because the alternative reading of the same
    // event — audio that stopped and came back on its own — is indistinguish-
    // able from a fault in their radio.
    double lastAudioProbeSec_ = 0.0;
    int audioRecoveries_ = 0;
    std::string audioHealthNote_;
    // THE DEVICE OPEN, OFF THIS THREAD. Field report "hang ntdll.dll @
    // InitializeWaveHandles" (0.96.4): picking an output device put the GUI
    // thread inside waveOutOpen for 57 seconds. See gui/audio_open.hpp for the
    // whole argument; what matters here is that nothing on this thread may
    // query the sink while inFlight() is true.
    cascade::gui::AudioOpen audioOpen_;
    // The AudioOpen::Result tag that says which of the two asked: the audio
    // watchdog reopening a dead stream (counted as a recovery, and the only
    // one that writes the health note) or the user picking a device. Carried
    // by the request rather than kept in a member here, because a click queued
    // behind a reopen would otherwise relabel the open already in flight.
    static constexpr int kAudioOpenByUser = 0;
    static constexpr int kAudioOpenByWatchdog = 1;
    // Once-a-minute starvation digest (see pollAudioHealth). Sampled every
    // frame — not gated behind the 1 Hz watchdog above — because a ring can
    // dip and recover well inside a second at 48 kHz, and a low-water mark
    // read only once a second would miss most of the dips it exists to
    // report. SIZE_MAX so the very first frame's real reading always beats
    // the sentinel instead of needing a separate "have we sampled yet" flag.
    double lastAudioLogSec_ = 0.0;
    std::size_t audioRingLowWaterFrames_ = SIZE_MAX;
    std::uint64_t audioUnderrunsAtLogStart_ = 0;
    std::uint64_t audioPrimingAtLogStart_ = 0;
    float splitRatio_ = 0.4f;  // spectrum's share of the center area

    // --- Source menu state (P4) ---------------------------------------------
    // The frequency readout no longer keeps a mirror: it always displays
    // pipeline_.activeSource().centerFrequencyHz() readback (nominal for the
    // generator/file, real device readback for Soapy). The generator's 100 MHz
    // default preserves the parity-spec startup display.
    //
    // Enumerated SoapySDR devices behind combo rows 2..N+1 (rows 0/1 are the
    // generator and the IQ file). Filled LAZILY by scanSoapy() — first
    // dropdown open, or Refresh — never at construction: enumeration loads
    // vendor modules (SoapyUHD -> uhd.dll -> libusb) whose USB discovery
    // crashed in-process in ~2% of measured runs (libusb-1.0.dll AV during
    // uhd::device::find, P6a investigation 2026-08-15). Deferring the scan
    // keeps generator/file sessions — and every bounded --frames CI run —
    // from ever executing that code.
    // Direct frequency entry: double-clicking the readout swaps the digit
    // strip for a text field (SDR++-style typing). Wheeling digits alone
    // cannot get you from 100 MHz to 433 MHz in any reasonable number of
    // notches, which is what made tuning feel broken.
    // Waterfall press tracking: a press that ends without crossing the drag
    // threshold is a CLICK (tune here); one that crosses it is a PAN. Both
    // gestures share the left button, so they can only be told apart on
    // release.
    float wfPressX_ = 0.0f;
    bool wfMoved_ = false;

    // Moves the VFO so the tuned frequency lands on wantAbsHz, snapping to the
    // mode's raster unless the caller says otherwise, and clamping the band
    // inside the baseband span. Shared by click-to-tune and the drag path.
    void setVfoToAbsoluteHz(double wantAbsHz, bool snap);

    int deemphIndex_ = 0;  // index into kDeemphUs; 0 = 50 us (global default)

    bool freqEditing_ = false;
    bool freqEditFocus_ = false;   // request keyboard focus on the first frame
    bool freqEditWasActive_ = false;  // field has held focus at least once
    char freqEditBuf_[32] = {0};
    // WHICH WAY EACH DIGIT'S TOGGLE SWITCH WAS LAST FLICKED. The lever under
    // a tube points up after a flick up and down after a flick down and
    // stays there - it is a memory of the last direction, not a momentary
    // spring return (the design reference's own rule). All down at start.
    // Cosmetic only: the tuned frequency lives in the pipeline, not here.
    bool freqLeverUp_[10] = {false, false, false, false, false,
                             false, false, false, false, false};

    std::vector<cascade::source::SoapyDeviceInfo> soapyDevices_;
    bool soapyScanned_ = false;  // one lazy scan done (scanSoapy())
    // A deferral has been logged for the radio currently open. The combo's
    // lazy scan asks on every frame the dropdown is open, so without this the
    // one diag line would be written sixty times a second; cleared the moment
    // the gate opens again (drawSourceSection), so the next radio gets its
    // own line.
    bool soapyScanDeferredLogged_ = false;
    // THE SCAN BESIDE AN OPEN RADIO (2026-09-23; gui/device_scan_plan.hpp).
    // soapyScanSkip_ is the drivers the scan in flight left out - the open
    // radios' own families - and pollSourceAsync keeps the rows of those
    // drivers from the old list, since that scan could not have seen them.
    // soapyScanPartial_ says the last scan was one of those, so the next
    // chance with no radio open does a whole one.
    std::vector<std::string> soapyScanSkip_;
    bool soapyScanPartial_ = false;
    // The patch page asked for a SoapySDR scan when it opened and has not had
    // one yet (the plan was deferring - a radio still opening). See patchReconcile.
    bool patchScanWanted_ = false;

    // --- Reopening after an absorbed driver fault (0.90.1) -----------------
    // When the automatic reopen was last attempted, in ImGui::GetTime()
    // seconds; negative = never. gui::autoReopenDue holds the next attempt
    // off for kSoapyReopenHoldoffSec after this, so a radio that is really
    // gone is tried once, not in a loop.
    double soapyReopenAttemptSec_ = -1.0;

    // --- Off-thread SoapySDR discovery and open --------------------------
    // SoapySDR::Device::enumerate()/make() do USB bus discovery and, for a
    // B200, an FPGA/firmware load: seconds of blocking work. Run inline they
    // froze the GUI for ~3 s on every source click. Both now run on a worker
    // thread; the GUI polls each frame and applies the result. The device
    // itself is only ever touched by the GUI thread once the future resolves,
    // so no locking is needed beyond the future's own synchronization.
    struct DeviceOpenResult {
        std::unique_ptr<cascade::source::DeviceSource> dev;  // null on failure
        // WHICH DRIVER THE WORKER SHOULD CONSTRUCT, and afterwards which one
        // it did: "soapy", or one of the eight native driver keys - the
        // same spellings
        // AppConfig::sourceKind uses. The kind has to travel with the request
        // because the worker is what decides the concrete type, and it has to
        // come back with the answer because sourceKind_ is set from it.
        std::string kind = "soapy";
        std::string args;
        // THE SOAPY ARGS THE PREFER-NATIVE DECISION WAS MADE FROM, carried
        // along so the worker can fall back to them. Empty for an open the
        // user asked for directly. See launchDeviceOpen: a dongle whose tuner
        // the native driver does not support (E4000, FC0012/13) must still
        // open the way it always did, and by the time that is known the
        // worker is the only thing still holding the request.
        std::string fallbackSoapyArgs;
        std::string error;
        int row = -1;
        double requestRateHz = 0.0;
    // The frequency the user was listening to when they changed device.
    //
    // A newly opened radio sits wherever its driver defaults to - an RTL-SDR
    // comes up at 100 MHz - so without carrying this across, changing device
    // silently retunes the receiver and the audio stops. Captured before the
    // switch because by the time the open finishes, the old source is gone.
    double keepCenterHz = 0.0;
    // WHAT AN AUTOMATIC REOPEN HAS TO PUT BACK (pollSoapyRecovery, 0.90.1).
    // The ordinary open primes every gain to its default and leaves AGC off;
    // a reopen after a driver fault is not a new radio to the user, so the
    // gains, the gain mode and - if the receiver was running when the driver
    // faulted - the running state are restored once the device is up. The
    // antenna needs nothing here: deviceAntenna_ is applied by every open.
    // Carried INSIDE the result rather than in a member so an answer the
    // user has moved on from (asyncOpenStillWanted) drops it with the rest.
    bool recovery = false;
    std::vector<std::string> recoveryGainNames;
    std::vector<float> recoveryGainsDb;
    bool recoveryAgc = false;
    bool recoveryRestart = false;
    };
    std::future<std::vector<cascade::source::SoapyDeviceInfo>> soapyScanFuture_;
    std::future<DeviceOpenResult> deviceOpenFuture_;
    bool soapyScanPending_ = false;
    bool deviceOpenPending_ = false;
    std::string deviceBusyLabel_;  // device name shown while an open is in flight

    // Paces hardware retunes — see retuneSourceHz. 50 ms: invisible against
    // the wheel gesture, one apply per notch burst instead of one per frame.
    cascade::core::RetuneCoalescer retuneCoalescer_{50.0};
    // The isPluginPreset a retune was requested with, carried across the
    // coalescer alongside its frequency — see retuneSourceHz. Overwritten on
    // every request, so a deferred apply reads the LATEST caller's context,
    // never a stale one from an earlier request the coalescer already
    // superseded.
    bool pendingRetuneIsPreset_ = false;

    // Set by noteTuneMismatch when a retune's readback disagreed with what it
    // asked for by more than kTuneMismatchToleranceHz; drawn in warning
    // colour under the Source controls, "" = the last retune landed where it
    // was asked. NaN so the very first mismatch this session sees is always
    // logged (NaN != NaN), never suppressed by an uninitialised zero that
    // happens to equal a real request.
    std::string tuneMismatchNote_;
    double lastMismatchRequestHz_ = std::numeric_limits<double>::quiet_NaN();
    double lastMismatchAnswerHz_ = std::numeric_limits<double>::quiet_NaN();

    // Source-selection sequence number, incremented by EVERY install of a
    // source into the pipeline (generator, IQ file, or a resolved device).
    // deviceOpenReqGen_ records the value an in-flight open was requested at;
    // asyncOpenStillWanted() compares the two when it resolves. See the
    // predicate's comment above for why a counter and not a flag.
    std::uint64_t sourceGen_ = 0;
    std::uint64_t deviceOpenReqGen_ = 0;

    // Drains a pending device open OFF the GUI thread at shutdown. See the
    // definition for the semantics chosen and what they cost.
    void reapPendingDeviceOpen();
    // The same for a pending device SCAN. Separate because the futures are
    // separate and either may be in flight alone; the definition explains why
    // this reaper has nothing to release where the open reaper has a handle.
    void reapPendingSoapyScan();

    // Consumes finished scan/open futures; called once per frame.
    void pollSourceAsync();
    // ONE AUTOMATIC REOPEN AFTER AN ABSORBED DRIVER FAULT (0.90.1); called
    // once per frame after pollSourceAsync. The 0.90.0 field report (NESDR
    // SMArt v5, 2026-09-09): a rate change faulted inside rtlsdr.dll, the
    // guard absorbed it, the device was condemned, and the radio stayed dead
    // - deck reading FAIL - until FoxSDR was restarted, though the fault was
    // on our own call frame and every thread of ours was out of the module.
    // When the open device is dead by such a fault (SoapySource::deadReason
    // == VendorFault - never Abandoned, whose driver still has a thread of
    // ours parked inside it), nothing is in flight, and no attempt was made
    // in the last kSoapyReopenHoldoffSec (gui::autoReopenDue), this closes
    // the dead source exactly as selectSource does and reopens the same args
    // at the same rate through launchDeviceOpen, with the state to restore in
    // the result. A reopen that fails leaves the ordinary failed-open state
    // and message, and nothing tries again.
    void pollSoapyRecovery();
    // The worker-thread open shared by selectSource and pollSoapyRecovery:
    // closes nothing (the caller has), stamps the request with sourceGen_,
    // and sets deviceOpenPending_/deviceBusyLabel_. `r` carries the args, the
    // row, the rate, the centre to carry across and any recovery payload.
    void launchDeviceOpen(DeviceOpenResult r, const std::string& busyLabel);
    // Applies a resolved open on the GUI thread (panel mirrors, gain priming,
    // pipeline install). Takes ownership of r.dev.
    void finishDeviceOpen(DeviceOpenResult r);
    // Combo selection. -1 means "active device no longer in the list" (a
    // Refresh dropped it); the preview then falls back to the active source
    // name. Distinct from the ACTIVE source: selecting "IQ file" only shows
    // the path controls — the pipeline keeps its source until Open succeeds.
    int sourceSel_ = 0;
    char iqPath_[512] = "";     // InputText buffer for the IQ file path
    std::string sourceError_;   // red text under the Source controls; "" = none
    // THE RADIO THE PANEL DRIVES, whatever kind it is. Non-owning view of the
    // DeviceSource installed in the pipeline (the pipeline owns it via
    // setSource); null whenever the active source is the generator or a file,
    // and must be nulled BEFORE any setSource that destroys the object.
    //
    // This was a SoapySource* until 0.91.0 and everything the Source section
    // did went through the concrete class. It is the interface now because
    // there are two more kinds of radio behind it - RtlSdrSource and
    // HackRfSource, which speak WinUSB and need no vendor module at all - and
    // a panel written against one of the three would have had to be written
    // three times. Rate, gains, AGC, antenna, tuning range, dead/faulted: all
    // of it is DeviceSource now, and a Soapy device answers exactly as it did.
    cascade::source::DeviceSource* device_ = nullptr;

    // ...AND THE SAME OBJECT AS A SoapySource WHEN IT IS ONE, for the four
    // things that are genuinely Soapy-specific and have no meaning for a
    // native driver: the child-process scan gate (a native enumeration opens
    // nothing and needs no gate), the module/vendor diagnostics under "no
    // radio hardware found", the "close the radio to look for other devices"
    // caption, and the automatic reopen after an absorbed vendor fault
    // (deadReason() distinguishes a faulted driver from a wedged one, which
    // only SoapySource has). Null whenever device_ is not a SoapySource -
    // including when it is a native radio - and set and cleared with it.
    cascade::source::SoapySource* soapyView_ = nullptr;

    std::string deviceArgs_;     // args of the open device (re-find on Refresh)
    // WHICH ROW OF supportedSampleRatesHz() the Rate combo is on. The list
    // used to be the fixed 1/2/4/8 MS/s table for every radio on every
    // driver; it is now the DEVICE's own list - the RTL-SDR's twelve standard
    // rates, the HackRF's 2..20 MS/s menu, whatever a Soapy driver reports -
    // so the index only means anything alongside deviceRatesHz_.
    int deviceRateIndex_ = 1;
    std::vector<double> deviceRatesHz_;         // supportedSampleRatesHz() at open
    std::vector<std::string> deviceRateLabels_;  // "2.400 MS/s", one per rate
    std::vector<std::string> deviceGainNames_;  // gains() at open, names only
    // The RANGE each of those gains will accept, parallel to the names. The
    // sliders were drawn 0..60 dB for every stage of every radio before this,
    // which is right for none of them: a B200's PGA goes to 76 dB and an
    // RTL-SDR's VGA starts at -4.7, and SoapySDR clamps silently so nothing
    // ever said so.
    std::vector<cascade::source::GainInfo> deviceGainRanges_;
    std::vector<float> deviceGainsDb_;          // slider mirrors, one per name

    // WHETHER GAIN i IS DECIBELS OR THE HARDWARE'S OWN STEPS, for the four
    // places that letter a gain (the sliders, the RECEIVER card, the scope
    // deck's knob, the browser status). Decibels for anything the driver did
    // not describe - an out-of-range index, or a mirror that outlived the
    // ranges it was filled beside - because that is what every gain in
    // FoxSDR was before the native Airspy and what every other driver still
    // reports.
    cascade::source::GainUnit gainUnitAt(std::size_t i) const {
        return i < deviceGainRanges_.size() ? deviceGainRanges_[i].unit
                                            : cascade::source::GainUnit::Decibels;
    }
    cascade::source::GainUnit firstGainUnit() const { return gainUnitAt(0); }
    bool deviceAgcSupported_ = false;
    bool deviceAgc_ = false;

    // THE BIAS TEE. Present only when the OPEN device is one of the native
    // drivers that has one and can say so (see withBiasTee in app_window.cpp
    // for which, and for why this is not a DeviceSource method).
    // deviceBiasT_ is the persisted setting as well as the checkbox's mirror:
    // it is seeded from AppConfig::nativeBiasT at restore, applied to the
    // radio by adoptDeviceMirrors after every open, and read BACK from the
    // driver afterwards so the box can never claim power the hardware did not
    // switch on.
    bool deviceBiasTPresent_ = false;
    bool deviceBiasT_ = false;

    // THE SWITCHES THAT BELONG TO ONE RADIO EACH, and are NOT persisted.
    //
    // The bias tee above is saved because leaving it off silently costs a
    // user an evening of a dead band. None of these does: an RSP's notches
    // and HDR mode and an RX888's dither and output randomiser change what is
    // heard, are visible in the spectrum the moment they move, and each
    // driver deliberately puts the radio into a known state at open. So these
    // mirror the DRIVER'S READBACK for the session and nothing more - which
    // also means there is no stale saved value to reconcile against a
    // driver's open-time policy, the exact reconciliation the RTL-SDR's bias
    // tee is kept out of withBiasTee to avoid.
    //
    // "Present" is asked of the CONCRETE TYPE once per open, because these
    // are per-model even within one driver: an RSP1A has no HDR mode, an
    // RSPdx has no DAB notch, and a panel that offered either would be
    // offering a control the API answers with an error.
    bool deviceRfNotchPresent_ = false;
    bool deviceRfNotch_ = false;
    bool deviceDabNotchPresent_ = false;
    bool deviceDabNotch_ = false;
    bool deviceHdrPresent_ = false;
    bool deviceHdr_ = false;
    bool deviceAdcSwitchesPresent_ = false;  // the RX888's pair, together
    bool deviceDither_ = false;
    bool deviceRandomiser_ = false;

    // --- Native radios ----------------------------------------------------
    // Every radio one of our own drivers can open, from the eight enumerate*
    // functions. UNGATED and refreshed freely, unlike soapyDevices_: a native
    // enumeration reads SetupAPI properties and NEVER OPENS A DEVICE
    // (src/usb/usb_device.hpp rule 1), which is the exact rule the vendor
    // probe breaks and the whole reason scanSoapy() has a gate. It is cheap
    // enough to run on the GUI thread.
    //
    // TWO OF THE EIGHT ARE NOT USB AND ARE STILL IN HERE. enumerateSdrPlay()
    // asks the SDRplay service for its list, which is safe at any time for
    // the same reason rule 1 exists - it does not touch the bus. The Pluto's
    // row is not a discovery at all: a network cannot be walked, so
    // scanNative appends ONE row for it unconditionally, at the end, and that
    // row opens nothing until the user presses Open on an address. See
    // plutoUri_ and kPlutoDriverKey.
    std::vector<cascade::source::NativeDeviceInfo> nativeDevices_;
    // Their combo captions, composed once by scanNative(): the row label with
    // " (native)" appended. Stored rather than built per frame because the
    // combo hands ImGui a const char* that has to outlive the call.
    std::vector<std::string> nativeRowLabels_;
    // Dongles that are PRESENT but not bound to WinUSB - the DVB-T driver, or
    // no driver at all (problem code 28). They cannot be opened by anything,
    // so they are not offered as rows; the Source section says so in one
    // sentence instead, because "my dongle is not in the list" with no
    // explanation was the single worst thing this panel used to do.
    std::vector<cascade::usb::UsbDeviceInfo> nativeUnbound_;

    // WHERE THE PLUTO IS. An InputText buffer rather than a std::string
    // because that is what ImGui edits, seeded from AppConfig::plutoUri at
    // restore and written back by currentConfig().
    //
    // It is an ADDRESS AND NOT A ROW, which is the whole difference between
    // this radio and the other seven: nothing is contacted until the user
    // presses Open, so choosing the Pluto row costs no network traffic, no
    // timeout and no wait - a user who has never owned one can select it,
    // read what it wants, and select something else.
    char plutoUri_[192] = "ip:192.168.2.1";

    // WHAT THE SOURCE SECTION SAYS ABOUT THE SDRPLAY API WHEN THERE IS NO RSP
    // ROW TO SHOW. Composed by scanNative() from the driver's own pure
    // sdrPlayPanelAdvice(), so the sentence the user is given is the one a
    // test pins; empty when the API is installed and new enough, which is when
    // there is nothing to say. It covers BOTH ways an RSP goes missing - no
    // API and an API too old - which took the enumeration's recorded reason,
    // because the table's own version field is written only by a session that
    // got past the version gate. sdrPlayApiDetail_ is the loader's own account
    // of where it looked, shown dimmed underneath for whoever is helping.
    std::string sdrPlayAdvice_;
    std::string sdrPlayApiDetail_;
    bool sdrPlayRowsFound_ = false;

    // --- Frequency scale + view interaction state (P5) -----------------------
    // ONE scale owns the x <-> Hz <-> bin mapping for both center panels, fed
    // every frame from the active source's center readback and the pipeline's
    // DSP input rate, so spectrum, waterfall, axis strip and VFO overlay can
    // never disagree about what frequency a pixel column shows.
    FreqScale scale_;
    // Last REQUESTED VFO bandwidth (Hz): combo presets and band-edge drags
    // both land here, and this is what the overlay and the config store use.
    // (The combo keeps showing its last preset after an edge drag — the combo
    // is a preset picker, not a readback; the overlay is the truth.)
    double vfoBandwidthHz_ = 150000.0;
    enum class VfoDrag { None, Center, EdgeLow, EdgeHigh };
    VfoDrag vfoDrag_ = VfoDrag::None;
    // mouseHz - band center at grab time, so a center drag never makes the
    // band jump to put its center under the cursor.
    double vfoGrabDeltaHz_ = 0.0;
    bool wfPanning_ = false;  // horizontal waterfall click-drag in progress

    // --- Config persistence state (P5) ----------------------------------------
    std::string configPath_;       // empty = persistence disabled (hermetic)
    bool configAnnounce_ = false;  // print "config applied: ..." (test hook)
    // The ACTIVE source's kind as the config store spells it. Tracked at each
    // successful switch because the pipeline does not expose source identity.
    // "siggen"|"file"|"soapy"|"rtlsdr"|"hackrf"|"airspy"|"airspyhf"|
    // "sdrplay"|"mirisdr"|"rx888"|"pluto"
    std::string sourceKind_ = "siggen";

    // WHAT THE CONFIG REMEMBERS, ONE SLOT PER FAMILY, and they are separate
    // on purpose. deviceArgs_ above is the LIVE device's args; these two are
    // the last Soapy device's kwargs and the last native device's args, kept
    // apart because the prefer-native rule needs both at once: it reads the
    // SAVED SOAPY args to learn "driver=rtlsdr, serial=00000001" and decide
    // whether a native row is the same dongle, and it must still have those
    // Soapy args to fall back to when the native open refuses the tuner. One
    // shared field would be overwritten by whichever family opened last, and
    // the fallback would then have nothing to fall back to.
    std::string cfgSoapyArgs_;
    std::string cfgNativeArgs_;

    // THE SAVED RADIO A RESTORE COULD NOT OPEN, held so the exit save can
    // write it back instead of the generator that stood in for it. Set only
    // by the startup restore's failure path and cleared the moment the user
    // deliberately chooses any other source; see
    // gui::rememberedSourceAfterFailedOpen for why one session with the
    // dongle unplugged used to lose the radio permanently. The label is what
    // the Source section shows in the combo while this is set - a radio that
    // is saved but not open, rather than a generator presented as if it had
    // been chosen.
    cascade::gui::RememberedSource restoreKeep_;
    std::string restoreKeepLabel_;

    // The open radio's MODEL, with no serial in it - what every diagnostic
    // line, the crash context and the scan-gate caption name it by. Kept as a
    // member because it cannot be derived from the args for a native device:
    // core::sanitiseDevice's allow list is driver/product/type, and a native
    // row's args are nothing but a serial, so sanitising them yields "".
    std::string deviceModel_;

    // The source combo was open on the previous frame. The native
    // enumeration and the lazy Soapy scan run on the frame it OPENS, not on
    // every frame it stays open - the combo asks sixty times a second
    // otherwise, and one of those two answers costs a SetupAPI walk.
    bool sourceComboWasOpen_ = false;
    // RX antenna ports the open device offers, and the one selected. Empty
    // until a Soapy device is opened. Persisted, because which port carries
    // the antenna is a property of the user's cabling, not of a session.
    std::vector<std::string> deviceAntennas_;
    std::string deviceAntenna_;
    std::string iqOpenPath_;  // last successfully opened IQ file (persisted;
                              // iqPath_ is just the edit buffer)
    cascade::core::AppConfig savedCfg_;    // what the config file holds now
    cascade::core::AppConfig pendingCfg_;  // debounce comparator
    double lastChangeTimeS_ = -1.0;  // glfwGetTime() of the last observed
                                     // change; < 0 = nothing pending

    // THE WRITE, OFF THIS THREAD. Field report "hang ntdll.dll @
    // cascade::core::ConfigStore::save" (0.96.3): the debounced save above
    // put the GUI thread inside the file write itself. See
    // gui/config_writer.hpp for the whole argument; requestConfigSave() and
    // pollConfigWriter() are the only callers.
    cascade::gui::ConfigWriter configWriter_;
    // The config content behind whatever configWriter_ is currently holding
    // (in flight or coalesced-and-queued) - see requestConfigSave()'s
    // comment on the header for why one variable is enough.
    cascade::core::AppConfig lastRequestedConfig_;

    // --- Recorder state (P6) --------------------------------------------------
    // Two independent Recorder instances so IQ and audio takes can run
    // simultaneously (each records ONE kind at a time by its contract). The
    // pipeline holds non-owning pointers to them only while a take is live;
    // stop*Recording clears the pointer before stopping the recorder.
    cascade::core::Recorder iqRecorder_;
    cascade::core::Recorder audioRecorder_;
    std::string recordDir_;    // %USERPROFILE%/Documents/SDR-recordings
    std::string recordError_;  // red text in the Recorder section; "" = none
    double iqRecordStartS_ = 0.0;     // ImGui::GetTime() at take start, for
    double audioRecordStartS_ = 0.0;  // the elapsed-wall-time readout
    // Input rate the live IQ take's WAV header was written for. A rate-follow
    // change (source switch, Soapy rate change) finalizes the take: a WAV
    // whose header rate disagrees with its samples would replay detuned.
    double iqRecordRateHz_ = 0.0;

    // --- Bookmarks state (P6) --------------------------------------------------
    // Loaded at startup from FreqManager::defaultPath() and saved after every
    // mutation — but ONLY when config persistence is enabled: hermetic runs
    // (empty configPath_, i.e. every --frames/--selftest CI run) leave
    // bookmarkPath_ empty and never read or write the user's bookmark file.
    cascade::core::FreqManager freqMgr_;
    std::string bookmarkPath_;   // empty = bookmark persistence disabled
    std::string bookmarkError_;  // red text in the Bookmarks section
    char bookmarkName_[128] = "";  // editable name for the next "Add current"
    // --- A large imported list (0.99.19) ----------------------------------------
    // The list can be tens of thousands of entries, so nothing here walks it
    // per frame: the view is a cached index rebuilt only when the list or the
    // filter changes, the list is drawn through a clipper, and the spectrum
    // marks come from a binary search of the visible span.
    char bookmarkFilter_[96] = "";
    int bookmarkGroupSel_ = 0;           // 0 = every group
    bool bookmarkFavOnly_ = false;
    bool bookmarkMarkers_ = true;        // draw bookmarks on the spectrum
    char bookmarkImportPath_[512] = "";
    std::string bookmarkImportNote_;     // what the last import did
    std::vector<std::string> bookmarkGroups_;
    std::vector<std::uint32_t> bookmarkView_;
    unsigned bookmarkViewVersion_ = ~0u;
    std::string bookmarkViewKey_;
    bool bookmarkSaveDirty_ = false;
    double bookmarkSaveDueS_ = 0.0;
    // A file dropped on the window, picked up by the next frame.
    std::string pendingDropPath_;
    // FOXSDR_BOOKMARK_IMPORT (bounded runs): opens VIEW > Bookmarks with the
    // path in the import box, for a scripted press of Import.
    bool bookmarkImportByEnv_ = false;
    bool bookmarkOpenByEnv_ = false;
    bool bookmarkScrollByEnv_ = false;
    // FOXSDR_PRESS_PRESET (bounded runs): the named plugin's first preset is
    // pressed once the plugins have loaded - the user's own key, for a
    // capture of that plugin at work.
    bool pressPresetByEnvDone_ = false;
    // The browser gets at most a few hundred bookmarks (favourites and the
    // ones nearest the tuned frequency); this maps its row numbers back.
    std::vector<std::size_t> webBookmarkIndex_;

    // --- Scanner state (P6) -----------------------------------------------------
    // The Scanner itself is a pure state machine (core/scanner.hpp); these
    // mirrors exist because ImGui edits by pointer. Defaults come from
    // Scanner::Params's own member initializers so the two can never drift.
    // --- P7 feature state -----------------------------------------------------
    // Panel mirrors for the pipeline's stereo / NR / notch settings (ImGui
    // edits by pointer). Defaults match AppConfig's, which match the
    // pipeline's own construction defaults, so the three can never disagree
    // before the first user click.
    bool stereoEnabled_ = true;
    bool nrEnabled_ = false;
    float nrStrength_ = 0.5f;
    bool notchEnabled_ = false;
    float notchFreqHz_ = 1000.0f;
    float notchQ_ = 30.0f;
    bool autoNotch_ = false;
    bool bandPlanOverlay_ = true;

    // Band plan: OPTIONAL display data merged from
    // BandPlan::defaultDir() at construction. A missing directory is the
    // normal case for a run-from-build-tree session and is silent — there is
    // simply no overlay. A directory that EXISTS but fails to parse keeps its
    // reason here and shows it in the Display section, because that one is a
    // user-visible mistake worth reporting.
    cascade::core::BandPlan bandPlan_;
    std::string bandPlanError_;
    // The active plan's id and the menu of installed plans. The list is built
    // ONCE in loadBandPlan() rather than per frame: available() opens and
    // parses every file in the directory, which is fine at startup and at a
    // deliberate re-scan, and is not something to do sixty times a second
    // inside a combo box.
    std::string bandPlanSelection_ = "world";
    std::vector<cascade::core::PlanInfo> bandPlanChoices_;
    // Ribbon size / segment-colour picker (issue #1). Int mirrors for the
    // two Combo boxes, the same pattern deemphIndex_ uses beside kDeemphUs —
    // see kBandPlanSizeKeys/kBandPlanPaletteKeys in app_window.cpp for what
    // each index means and currentConfig()/applyConfig() for the string
    // AppConfig fields these round-trip through.
    int bandPlanSizeIndex_ = 0;
    int bandPlanPaletteIndex_ = 0;

    // WHICH FACE THE FREQUENCY COUNTER WEARS (GitHub issue #1). Held as the
    // STYLE, not as the name: the name is the config file's vocabulary and
    // the enum is what the painter switches on, so the one conversion happens
    // where the config arrives and nowhere else. Defaults to the plate the
    // deck has always had.
    cascade::gui::TunerStyle tunerStyle_ = cascade::gui::TunerStyle::Nixie;

    // Plugin host: scanned once at construction and on Rescan. Owns the
    // loaded modules, so it must outlive nothing in particular here — but it
    // is declared before the pipeline-dependent members so it unloads last.
    cascade::core::PluginHost pluginHost_;
    // GUI-side plugin capabilities: map targets, plugin windows, and THE HOST
    // SERVICES a plugin calls back through.
    //
    // DECLARED BEFORE pluginRunner_, so it is destroyed AFTER it, and the
    // three are torn down host-services-last: runner, then UI, then host. That
    // is the order detachAndUnloadPlugins() has always performed and the order
    // ~AppWindow now performs explicitly; this declaration is the net under
    // that, because reverse declaration order is what the destructor falls
    // back on and it used to have these two the wrong way round. A decoder's
    // destroy() may ask the host for the time (Survey Engine 0.1.0 does, to
    // timestamp the dwell it is finishing), and with pluginUi_ destroyed first
    // that call reached a dead host: an access violation on Windows and an
    // abort inside libc++ on Android, both reported from the field on
    // 2026-09-16 from the same plugin at shutdown.
    cascade::core::PluginUi pluginUi_;
    // Drives the loaded decoders with real audio. Declared AFTER pluginHost_
    // and pluginUi_ so it is destroyed BEFORE both: the runner's destructor
    // calls each plugin's destroy(), which is code inside a module the host
    // unmaps and which may call a host service on its way out.
    cascade::core::PluginRunner pluginRunner_;

    // --- Per-plugin map pages ---------------------------------------------
    // ONE MAP PAGE PER PLUGIN THAT HAS A TRACK INSTANCE, replacing the single
    // "Map" window that drew every plugin's targets merged — which is why
    // switching from the Satellites plugin to ADS-B still showed "the
    // satellite map". Capability decides the page set, not content: an ADS-B
    // page with no aircraft decoded yet still exists, because "the page is
    // there but empty" answers the user's question and "the page is missing"
    // does not.
    //
    // Each page owns its own MapView, so view centre, zoom, follow and
    // selection are all per-page — that separation is the point of the
    // feature. Pages are created lazily the first frame their plugin appears
    // and never destroyed while the app runs; rescanPlugins() leaves them in
    // place (its comment measures the cost of clearing them), so a plugin
    // that reappears finds its page where it was.
    struct MapPage {
        std::string plugin;  // display name, as HostTrack::plugin carries it
        std::unique_ptr<MapView> view;
        // Opened by the user's own hand - the rail row, a preset, a details
        // window's Go-to - and by nothing else: not restored from the config
        // at start-up, and never self-opened on an arriving target (0.79.1,
        // the application starts on the main screen alone). Until then the
        // first visible target opened the page as an edge, which on a
        // propagating tracker - a full sky on the first frame of every launch
        // - meant a window at every start whether wanted or not.
        bool open = false;
        // The window's rectangle: seeded at creation from the saved entry (or
        // the legacy single-window rectangle), read back from ImGui every
        // frame the window is drawn, and written to AppConfig::mapPages.
        // Zero width/height means nothing saved — default placement.
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        // THIS PAGE IS THE SATELLITE INSTRUMENT, and it is decided from what
        // the plugin PUBLISHES rather than from what it is called: a page is
        // the satellites window once every track it has reported carries
        // CASCADE_TRACK_SATELLITE. A plugin name is third-party text and
        // matching on it would be a guess; the track kind is the ABI's own
        // answer to "what is this".
        //
        // STICKY FOR THE SESSION, because the alternative is a window that
        // rebuilds its entire layout the moment a propagator has nothing to
        // report for a frame. It is never persisted: the kinds arrive again
        // on the first poll of the next launch, and a saved flag could only
        // ever be a stale opinion about a plugin that has since changed.
        bool satellite = false;
        // How many of this page's targets the staleness rule shows, recorded
        // where the page's tracks are already filtered so the rail row and the
        // window cannot report two different numbers. Updated for CLOSED pages
        // too - that is exactly when the rail is the only thing saying it.
        std::size_t visibleCount = 0;
        // The satellites window's own controls. Held here rather than in the
        // MapView because map_view.hpp says outright that the caller owns
        // them: four of the eight fields are AppConfig settings shared with
        // every other map page (copied in and out each frame, so there is one
        // copy of each and this is a view onto it), and the other four - the
        // sort key, its direction and the two coordinate cells while they are
        // being typed - belong to this window and are session-scoped.
        cascade::gui::SatelliteDeck deck;
    };
    // Creation order, which is the plugins' load order — that is what keeps
    // the default cascade of fresh pages stable across launches.
    std::vector<MapPage> mapPages_;
    // AppConfig::mapPages as loaded, re-synced from the live pages by
    // syncMapPagesToSaved(). Kept separately from mapPages_ so an entry for a
    // plugin that is not installed this session survives the session's saves.
    std::vector<cascade::core::AppConfig::MapPage> mapPagesSaved_;
    // Scratch for one page's filtered tracks and paths, reused across pages
    // so the per-frame filter allocates nothing in the steady state. The
    // per-plugin caps in PluginUi bound what lands here.
    std::vector<cascade::core::HostTrack> pageTracks_;
    std::vector<cascade::core::HostPath> pagePaths_;
    // --- Anonymous usage reporting (opt-in; see PRIVACY.md) ----------------
    // Counters for the session in progress, journalled to the config at exit
    // and sent at the NEXT start-up. Nothing here is transmitted unless the
    // user has turned reporting on.
    cascade::core::TelemetryReporter telemetryReporter_;
    // "Running now" beats: a minimal ping every five minutes while the app is
    // open, only while reporting is on. See HeartbeatSender in telemetry.hpp.
    cascade::core::HeartbeatSender telemetryHeartbeat_;
    bool telemetryEnabled_ = false;
    std::string telemetryInstallId_;
    std::uint64_t telemetryLaunches_ = 0;
    std::uint64_t telemetryCrashes_ = 0;
    // The VOLUME meter's needle, carried between frames so it can fall
    // gently rather than follow every syllable - see gui/volume_meter.hpp.
    float volumeNeedle_ = 0.0f;
    bool telemetryCleanExit_ = false;   // true only on the normal shutdown path
    double telemetrySessionStart_ = 0.0;                  // glfwGetTime at start
    // Time in the current mode. A plain "since" mark cannot be used here: the
    // accrual runs once per frame, and a per-frame delta truncated to whole
    // seconds is always zero, which is what kept modeSeconds empty.
    cascade::core::SecondAccrual telemetryModeAccrual_;
    std::map<std::string, std::uint64_t> telemetryModeSeconds_;
    std::vector<std::string> telemetryPanels_;
    // Called once per frame, so the seconds land against the mode that was
    // actually running rather than the last one selected.
    void telemetryAccrueMode();
    void telemetryNotePanel(const char* name);
    // Reads the previous session out of the config, counts a crash if it
    // never finished, and sends its report. Start-up only.
    void telemetryStartup(const cascade::core::AppConfig& cfg);
    // Builds this session's report into `cfg` for the next start-up to send.
    void telemetryJournal(cascade::core::AppConfig& cfg);
    // "Usage reporting" settings section: the opt-in switch and what it sends.
    void drawUsageReportingSection();
    bool privacyNoticeOpen_ = false;

    // -----------------------------------------------------------------------
    // Diagnostics (see core/crash_handler.hpp, core/hang_watchdog.hpp)
    // -----------------------------------------------------------------------
    //
    // The watchdog is fed once per rendered frame from run(). It is the only
    // thing in this application that can see a hang - and every fault this
    // product has actually shipped was a hang, not a crash.
    cascade::core::HangWatchdog watchdog_;
    // Where reports go, decided in main() so that a bounded CI run leaves
    // nothing on the machine. Empty means the watchdog still runs (a recovered
    // stall is still logged) but writes no file.
    std::string diagCrashDir_;
    // --diag-stall N: wedge the frame loop for N ms, once, on frame 60. The
    // only way to prove the shipped threshold fires against the real loop.
    int diagStallMs_ = 0;
    // --diag-toggle on|off: flip the diagnostics switch on frame 30. See
    // setDiagToggle().
    int diagToggle_ = 0;
    // The whole diagnostics switch - crash handler, log AND watchdog - in one
    // place. The checkbox, the arming path in run() and the test hook all go
    // through it; they used to disagree about the watchdog.
    void applyDiagnosticsEnabled(bool on);
    // Set for the ONE heartbeat after a deliberate stall, so a stall the test
    // asked for cannot become "the worst frame gap this build measured".
    bool diagSkipNextGap_ = false;
    // Rebuilds the report context out of state the application already has -
    // mode, source, rate, radio model, loaded plugins with versions. Nothing
    // here is re-derived.
    void refreshDiagContext();
    std::size_t diagPluginCount_ = static_cast<std::size_t>(-1);
    // The previous session did not reach its clean-exit save. Reuses
    // telemetryCleanExit, which already detects exactly this, and is the
    // trigger for offering a report on this start.
    bool lastRunUnclean_ = false;
    bool diagOfferOpen_ = false;

    // --- Sending a captured report (see core/crash_upload.hpp) -------------
    //
    // ON THE NEXT START, NEVER FROM THE FAULT HANDLER. The process that faulted
    // could not safely open a socket; this one can. The sweep runs on a
    // background thread the moment the frame loop is armed, and is cancelled
    // and joined before the clean-exit save - which is what keeps a server that
    // accepts and never answers from delaying shutdown by so much as a frame.
    cascade::core::CrashUploader crashUploader_;
    // Dedup and rate-limit memory, read from the config at start and written
    // back after the sweep. Persisted because a crash loop IS a sequence of
    // runs: a limit held only in memory would reset on every restart.
    cascade::core::UploadPolicyState crashUploadState_;
    // False unless a sweep was actually started on this run. Without it, a run
    // in which the sweep never ran (diagnostics off) would journal a DEFAULT
    // state back over the user's real one and quietly reset the crash-loop
    // limiter - so switching diagnostics off for one session would re-arm the
    // sender for the next.
    bool crashUploadSwept_ = false;
    void crashUploadStart();
    void crashUploadFinish();

    // --- REQUEST A FEATURE (see core/feature_request.hpp) -------------------
    //
    // A KEY ON THE MAIN SCREEN, not a rail row: this is not a mode of the
    // receiver and has no lamp to keep lit, so it lives on the STATUS column,
    // above REPORT A BUG / DISLIKE and the maker's plate - see
    // drawStatusColumn(). Never persisted and
    // never reopened at start-up (the same rule as demodScopeOpen_ above:
    // nothing here rides in AppConfig, so there is nothing FOR startupState()
    // to have to clear).
    bool featureRequestOpen_ = false;
    // FOXSDR_OPEN_FEATURE_REQUEST's one-shot latch - see the call site in
    // run() beside FOXSDR_OPEN_SERIAL_PORTS's sibling seams.
    bool featureRequestOpenedByEnv_ = false;
    // The same one-shot latch for FOXSDR_OPEN_DEMOD_SCOPE - see its use.
    bool demodScopeOpenedByEnv_ = false;
    // ...and for FOXSDR_OPEN_MAP.
    bool mapOpenedByEnv_ = false;
    // The typed text and the typed contact line - IN MEMORY ONLY, per
    // PRIVACY.md and the file header of feature_request.hpp: neither field
    // is ever read from or written to AppConfig, and neither reaches the
    // diagnostics log. `featureRequestContact_` survives a successful send
    // (the contract calls for keeping it so a person filing several requests
    // does not have to retype it); `featureRequestText_` is cleared.
    std::string featureRequestText_;
    std::string featureRequestContact_;
    cascade::core::FeatureRequestSender featureRequestSender_;
    // The state drawFeatureRequestPage() last logged against, and the
    // character count it logged with - so a send's outcome is logged exactly
    // once, on the frame the worker's Sending -> terminal transition is first
    // seen, and the log line can say how many characters without saying what
    // any of them were.
    cascade::core::FeatureRequestState featureRequestLastLoggedState_ =
        cascade::core::FeatureRequestState::Idle;
    std::size_t featureRequestSentChars_ = 0;
    // The height of everything under the page's text box on the last frame,
    // so the box can give way to it (gui::boxGivingWay). 0 until measured.
    float featureRequestBelowBoxH_ = 0.0f;
    void drawFeatureRequestPage();

    // --- REPORT A BUG / DISLIKE (see core/problem_report.hpp) ----------------
    //
    // The second key on the STATUS column, directly under REQUEST A FEATURE
    // and the same size, and the same rules to the letter: never persisted,
    // never reopened at start-up, nothing typed ever reaches AppConfig or the
    // diagnostics log. `problemReportKind_` is empty until the person picks
    // one of the two - the page has no default, so every report's kind was
    // chosen - and it and the contact line survive a successful send; the
    // text is cleared on the one frame the success is first seen.
    bool problemReportOpen_ = false;
    // FOXSDR_OPEN_PROBLEM_REPORT's one-shot latch, beside the feature
    // request's.
    bool problemReportOpenedByEnv_ = false;
    std::string problemReportKind_;
    std::string problemReportText_;
    std::string problemReportContact_;
    cascade::core::ProblemReportSender problemReportSender_;
    cascade::core::FeatureRequestState problemReportLastLoggedState_ =
        cascade::core::FeatureRequestState::Idle;
    std::size_t problemReportSentChars_ = 0;
    // The kind the send in flight carried, for its one log line.
    std::string problemReportSentKind_;
    // As featureRequestBelowBoxH_, for this page's box.
    float problemReportBelowBoxH_ = 0.0f;
    void drawProblemReportPage();
    // "Serial ports" settings section: the machine's ports as a table, and
    // the GPS row (drawGpsPositionControl) that used to be findable only
    // under the rail's Radar section. Drawn before Diagnostics, on the SYSTEM
    // bank.
    void drawSerialPortsSection();
    // "Key bindings" settings section: every action on one key-shaped label,
    // and the capture that rebinds it. Drawn on the SYSTEM bank beside the
    // other two settings rows.
    void drawKeyBindingsSection();
    // "Diagnostics" settings section: where the log is, what a report carries,
    // and the one-click bundle.
    void drawDiagnosticsSection();
    // Shown once, at the start of the run AFTER an unclean exit. A crash
    // handler can write a report but it cannot ask anything - by the time it
    // runs there is no user interface left to ask with. This is the ask.
    void drawDiagnosticsOffer();
    void copyDiagnosticsBundle();
    std::string diagBundleStatus_;
    bool diagnosticsEnabled_ = true;
    bool diagnosticsMinidump_ = false;

    // Who each map target actually is, answered by a track-info plugin
    // (registration, type, operator). Inactive when none is installed.
    TrackInfoCache trackInfo_;
    // Map imagery from a basemap plugin, as GL textures. Inactive - and the
    // map is the built-in coastline - unless such a plugin is installed, which
    // is the shipped configuration. Declared after pluginHost_ so it detaches
    // before the modules it borrows tiles from are unmapped.
    BasemapCache basemap_;
    // The LEGACY single-window rectangle, seeded from AppConfig at start-up
    // and never changed after: it is only the default rectangle for a page
    // with no saved mapPages entry of its own (staggered per page index so
    // pages do not stack exactly). Kept so an install upgrading from the
    // one-window map reopens its pages where that window used to sit. The
    // legacy config keys are read but no longer written — see AppConfig.
    // All zero means nothing was ever saved (or the config sanitizer rejected
    // what was), which is what selects the monitor-derived default instead.
    int mapWinW_ = 0;
    int mapWinH_ = 0;
    int mapWinX_ = 0;
    int mapWinY_ = 0;
    // The receiver's own position, seeded from AppConfig and written back to
    // it. This used to be two static locals beside the "Set RX here" button,
    // which meant it died with the process: range rings had to be re-entered
    // every launch, and a distance column built on that would have measured
    // every target from 0N 0E. See AppConfig::rxPositionSet for why an
    // explicit flag rather than a sentinel coordinate.
    //
    // The two INPUT fields are separate from the applied position on purpose:
    // typing a latitude must not move the range rings until the button is
    // pressed, or every intermediate keystroke would be a different receiver.
    void applyScopeWindowVisibility();
    // Scope mode hides the torn-off windows while it is on; these two carry
    // the edge that puts them back on the way out.
    bool scopeWasOn_ = false;
    bool scopeLeftThisFrame_ = false;

    GLFWwindow* mainWindow_ = nullptr;

    // WHETHER A TORN-OFF PAGE GETS AN OPERATING SYSTEM WINDOW, and why not
    // when it does not. Decided once at startup from FOXSDR_SINGLE_VIEWPORT
    // and a probe for a second shared GL context, and re-decided the moment
    // the backend reports that the driver refused one - see
    // gui/viewport_policy.hpp and the 0.95.0 "crash cascade.exe @
    // glfwGetWin32Window" report that made it necessary. Kept as state rather
    // than re-derived because the sentence has to be logged exactly once.
    cascade::gui::ViewportDecision viewportDecision_ = cascade::gui::ViewportDecision::Enabled;

    // --- THE WINDOW FRAMES, ON THE METAL (0.78.0) ------------------------------
    //
    // The operating system's title bars are gone from the main window and from
    // every page. The cabinet's own top rail carries the window's name and
    // three brass keys - minimise, maximise, close - and the rail is what the
    // window is dragged by. The main window's half of that is native
    // (gui/win_frame.hpp: the system still moves, resizes and snaps it); a
    // page's half is beginPage, which draws the cabinet round the page and
    // makes its rail an ImGui drag handle. A page inside the main window
    // "minimises" by rolling up to its rail and "maximises" to the main
    // window's area; a torn-off page does both to the desktop, as any window
    // does, and has a taskbar button to come back from.
    struct PageChrome {
        bool collapsed = false;
        bool maximised = false;
        float restoreX = 0.0f;
        float restoreY = 0.0f;
        float restoreW = 0.0f;
        float restoreH = 0.0f;
        bool pendingMaximise = false;
        bool pendingRestore = false;
        // The value of pageResetGen_ this page last acted on. Behind it means
        // "Reset window sizes" has been pressed since this page was last
        // drawn, so its next frame re-places it - see gui/page_geometry.hpp.
        std::uint32_t seenResetGen = 0;
        // The corner grip's drag: the page size and pointer position when it
        // began (gui/page_geometry.hpp pageGripResize).
        float gripW0 = 0.0f;
        float gripH0 = 0.0f;
        float gripMx = 0.0f;
        float gripMy = 0.0f;
    };
    std::map<std::string, PageChrome> pageChrome_;
    // Bumped by resetPageWindows(). One counter for the whole application
    // rather than a flag pushed at every window, because the windows that most
    // need putting back are often the ones that are not being drawn.
    std::uint32_t pageResetGen_ = 0;
    // "Reset window sizes" on the fitted-modules window: no page is collapsed
    // or maximised any more, and every page takes its opening rectangle again
    // on its next frame.
    void resetPageWindows();
    // Whether beginPage opened the well child this frame, so endPage closes it.
    bool pageBodyOpen_ = false;
    float pageInset_ = 0.0f;
    // Begin a page: the window, its cabinet, its rail and keys. Returns whether
    // the body should be drawn; ALWAYS pair with endPage(), as Begin with End.
    //
    // defaultW/defaultH are THE SIZE THIS PAGE OPENS AT - the same pair the
    // call site hands ImGui as its FirstUseEver size - and they are applied
    // again, once, by a reset. Left at 0 by a caller with no opening size of
    // its own (the auto-resizing target details), which is then left alone.
    // Plain floats rather than an ImVec2 for the reason the rest of this
    // header gives: it is compiled into the tests and must not need imgui.h.
    bool beginPage(const char* id, const char* title, bool* open, int flags = 0,
                   float defaultW = 0.0f, float defaultH = 0.0f);
    void endPage();
    // A page that is maximised or rolled up is showing a rectangle the key
    // chose, not the user: geometry read-backs skip it.
    bool pageGeometryTransient(const char* id) const;
    // The main window's rail: name, keys, and the drag region handed to the
    // native hit test - and, should a machine's hit test not take it, dragged
    // from this side through GLFW (drawCabinetRail explains the two routes).
    void drawCabinetRail(float x0, float y0, float x1, float y1, float margin);
    float mainDragCarryX_ = 0.0f;  // sub-pixel remainder of the GLFW-side drag
    float mainDragCarryY_ = 0.0f;
    bool mainDragSaid_ = false;    // the diagnostic line about it, once a run

    bool rxSet_ = false;
    double rxLat_ = 0.0;
    double rxLon_ = 0.0;
    double rxLatInput_ = 0.0;
    double rxLonInput_ = 0.0;
    // --- the GPS the position can be read from (0.86.0) ---------------------
    // The reader owns its thread and joins in its destructor; run() also
    // stops it at the top of the shutdown path so the join precedes the GL
    // teardown. gpsPort_ and gpsBaud_ are the persisted choice
    // (AppConfig::gpsPort / gpsBaud); gpsPortInput_ is the text field's own
    // buffer, one byte over the port layer's limit for the terminator, so
    // the field cannot hold a name the sanitiser would cut. gpsPorts_ is the
    // last enumeration, taken when the drop-down opened. gpsRefusal_ is the
    // one sentence shown when a fix the reader accepted was refused by
    // applyReceiverPosition - which cannot happen while both apply the same
    // predicate, and is shown rather than swallowed precisely so a day it
    // does happen is visible.
    cascade::core::GpsReader gpsReader_;
    std::string gpsPort_;
    int gpsBaud_ = cascade::core::kDefaultGpsBaud;
    char gpsPortInput_[cascade::core::kMaxSerialPortNameChars + 1] = "";
    std::vector<std::string> gpsPorts_;
    std::string gpsRefusal_;
    // Set by pollGpsReader when a fix is applied; the rail's "Receiver
    // position" fold opens itself once on it, so the Fixed status line is
    // seen rather than lost with the no-position block that held the row.
    bool gpsRowReveal_ = false;
    // --- SYSTEM > Serial ports: the machine's ports gathered in one place,
    // so a person is not sent hunting through Radar to find the GPS row.
    // A SEPARATE cache from gpsPorts_ above, on a DIFFERENT trigger: this one
    // is read when the SECTION opens (and on its own Refresh key), not when a
    // combo inside it is pressed - a table that re-read the registry every
    // frame it was open would be the same cost drawGpsPositionControl's own
    // comment warns against, paid by a row with no combo to hide it behind.
    std::vector<std::string> serialPortsList_;
    bool serialPortsListLoaded_ = false;
    // Last frame's open/closed state, so drawSerialPortsSection can tell
    // "just opened" (enumerate) from "still open" (use the cache) from one
    // benchSection() call that only ever reports the current state.
    bool serialPortsSectionWasOpen_ = false;
    // How far anything has been heard, per five-degree bearing bucket. Fed
    // once per frame from the visible tracks - which is every track source at
    // once, not just ADS-B - and drawn over the map when coverageShow_ is on.
    // Session-scoped by design; see CoverageMap.
    CoverageMap coverage_;
    bool coverageShow_ = false;
    // The two trail switches, pushed into every page's MapView each frame and
    // persisted (AppConfig::mapTrails, AppConfig::mapTrailAltitudeColours).
    // They live here rather than in a MapPage because they are a statement
    // about how this user wants trails drawn, not about one plugin's window:
    // two map pages disagreeing about whether a trail is coloured by altitude
    // would be two answers to one question.
    bool mapTrails_ = true;
    bool mapTrailAltColours_ = true;
    int mapTrailStyle_ = 0;  // 0 line, 1 ribbon - see AppConfig::mapTrailStyle
    // Pixels across, every map and scope - see AppConfig::aircraftIconPx.
    int aircraftIconPx_ = cascade::gui::kAircraftIconDefaultPx;
    // Trail width, pixels - see AppConfig::mapTrailWidthPx.
    int mapTrailWidthPx_ = cascade::gui::kTrailWidthDefaultPx;

    // --- the ADS-B radar scope ----------------------------------------------
    // The renderer, and the two pieces of state that outlive it. The RANGE
    // lives here rather than only in the view because it is persisted
    // (AppConfig::scopeRangeNm) and because two controls set it - the stepper
    // in the scope's own bar and the wheel over the face - so the value is
    // pushed in before each draw and read back after it.
    //
    // Held BY VALUE, unlike the map pages' views: there is exactly one scope
    // for the application (it is a mode of the main window, not a window per
    // plugin), and it owns nothing that needs a stable address.
    ScopeView scope_;
    bool scopeMode_ = false;
    int scopeRangeNm_ = kScopeDefaultRangeNm;

    // --- THE DEMOD SCOPE (0.94.0) --------------------------------------------
    //
    // The bench oscilloscope in the VIEW bank: the demodulated audio on a
    // graticule, its spectrum, and the channel I/Q as two traces and as a
    // Lissajous. A PAGE rather than a mode - unlike the radar scope above,
    // which takes over the whole window - because it is something you watch
    // BESIDE the spectrum while you tune, not instead of it.
    //
    // THE BUFFERS ARE MEMBERS so a frame of scope allocates nothing: the taps
    // are read into them, the I/Q is split into two parallel arrays for the
    // reductions, and the per-column envelope lands in scopeLo_/scopeHi_. At
    // the longest sweep the audio one holds half a second and the I/Q one
    // rather more, so these are the largest scratch buffers in the window and
    // resizing them once a frame would be the most expensive thing on it.
    // The patch, and where the user has scrolled it to. The GRAPH outlives the
    // page - it is the document, and the page is closed far more often than
    // the radios are - so it is owned here rather than by the canvas.
    bool patchOpen_ = false;
    cascade::core::patch::Graph patchGraph_;
    cascade::gui::patch::Interaction patchUi_;
    bool patchSeeded_ = false;
    bool patchOpenedByEnv_ = false;
    // THE PATCH'S OWN TRANSPORT (0.99.18). Opening the page no longer starts
    // anything: START on the page opens the radios that are switched on and
    // takes the receiver's radio; STOP or ALL OFF closes them and gives it
    // back. Session-only - a patch never starts itself at launch.
    bool patchRunning_ = false;
    bool patchWasRunning_ = false;
    bool patchStartedByEnv_ = false;
    bool sourceDeviceByEnv_ = false;   // FOXSDR_SOURCE_DEVICE, bounded runs only
    bool patchFileLoaded_ = false;
    // The patch as core/patch_io.hpp writes it, rebuilt only when the
    // canvas says it changed. currentConfig() runs every frame and must
    // not serialise a document on each one.
    std::string patchText_;
    // This frame's compile() of the patch: what would be built, and every
    // reason it could not be. Recomputed while the page is open.
    cascade::core::patch::Plan patchPlan_;
    // What each planned channel is hearing this frame. Rebuilt per frame
    // rather than kept, because a stale level is worse than none: it reads
    // as a live measurement of a channel that may no longer exist.
    std::vector<cascade::gui::patch::NodeReading> patchReadings_;
    // WHAT THE RUNNING SET WAS BUILT FROM, as core::patch::dspSignature()
    // spells it. A new set is published only when this changes. It used to
    // be the rate and centre plus "anything was edited", which rebuilt the
    // set on every frame of a node drag; now that a set holds plugin
    // instances, that would restart every decoder sixty times a second.
    std::string patchDspSig_;
    bool patchWasOpen_ = false;
    // The installed decoder plugins as the patch sees them, parallel lists.
    // Rebuilt from the plugin host each frame the page is open (a dozen
    // records), and EMPTIED in detachAndUnloadPlugins() before any module is
    // unmapped - the API pointers point into those modules.
    std::vector<cascade::core::patch::DecoderInfo> patchCatalogue_;
    std::vector<cascade::core::patch::PluginApis> patchApis_;
    // Decoder nodes whose plugin returned no instance from create() in the
    // last build, so the node can say so rather than look ready.
    std::vector<cascade::core::patch::NodeId> patchRefused_;
    // Decoder nodes whose FIRST line since the last build has been logged.
    // One line per decoder per build goes to the application log - enough to
    // prove from a user's log that a patch decoder was fed and produced
    // output, without copying a busy decoder's whole stream into it.
    std::set<cascade::core::patch::NodeId> patchFirstLineLogged_;
    // Each decoder node's latest line and running count since the last
    // build, for its face on the canvas. Kept whether or not the node is
    // wired to a Text out, because a decoder working with nowhere to send its
    // text is still working, and the face is where that is visible.
    struct PatchDecoderFace {
        std::string last;
        std::uint64_t lines = 0;
    };
    std::map<cascade::core::patch::NodeId, PatchDecoderFace> patchDecoderFaces_;
    // Each Text out node's recent lines, newest last, for its face - the
    // patch's own decoder log. Bounded per node.
    std::map<cascade::core::patch::NodeId, std::deque<std::string>> patchSinkLines_;
    // Each Spectrum node's waterfall memory, and the spectrum frame it last
    // took a row from - a row per published frame, not per GUI frame, so the
    // waterfall scrolls at the same pace as the main one.
    std::map<cascade::core::patch::NodeId, cascade::gui::patch::ScopeHistory> patchScopes_;
    std::map<cascade::core::patch::NodeId, std::uint64_t> patchScopeSeq_;
    // Each picture decoder node's newest picture and its GL texture, uploaded
    // only when the picture's revision moves. Textures are deleted when the
    // node goes and at shutdown, while the GL context is current.
    struct PatchPicture {
        cascade::core::HostImage img;
        unsigned int tex = 0;
        std::uint64_t texRev = 0;
    };
    std::map<cascade::core::patch::NodeId, PatchPicture> patchPictures_;
    void releasePatchPictureTextures();
    // Where the patch canvas was drawn last frame, in ImGui screen space:
    // what lets an input script aim at a node by its patch coordinates.
    float patchCanvasOriginX_ = 0.0f;
    float patchCanvasOriginY_ = 0.0f;

    // --- the scripted pointer (bounded runs only; see gui/input_script.hpp) ---
    std::vector<cascade::gui::ScriptStep> inputScript_;
    std::size_t inputScriptPos_ = 0;
    bool inputScriptActive_ = false;
    bool scriptMouseSet_ = false;
    float scriptMouseX_ = 0.0f;
    float scriptMouseY_ = 0.0f;
    // Feeds this frame's steps into ImGui's input queue. Called between the
    // platform backend's NewFrame and ImGui::NewFrame, so the script's events
    // are the last word on the frame.
    void applyInputScript(long frame);
    // The controls on every node's face, drawn over the canvas after it:
    // the node IS the instrument, so what it is set to is set on it.
    void drawPatchFaces(float originX, float originY, float width, float height);
    // GUI thread: rebuilds the two lists above from pluginHost_.
    void rebuildPatchCatalogue();
    // True when the node's Text output reaches a Text sink - the only way
    // its lines are shown in the Decoder output window.
    bool patchDecoderIsShown(cascade::core::patch::NodeId node) const;
    // How many parts have been dropped from the bin, used only to
    // stagger the next one so a run of clicks does not stack every
    // node on the same spot.
    int nodesPlaced_ = 0;

    // --- the patch's own radios (0.99.17, app_window_patch_radios.cpp) --------
    // UP TO FIVE, EACH ITS OWN DEVICE. Every Radio node with a device chosen
    // runs a core::patch::PatchRadio while the page is open: its own device
    // open, reader thread, runner and spectrum. Keyed by node.
    std::map<cascade::core::patch::NodeId, std::unique_ptr<cascade::core::patch::PatchRadio>>
        patchRadios_;
    // What each running radio was opened AS ("<device key>@<rate>"), so a
    // changed device or rate reopens it and nothing else does.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioOpenedAs_;
    // A hardware open in flight, per node. The open is a multi-second USB
    // walk, so it runs on a worker exactly as the receiver's own does.
    struct PatchRadioOpen {
        std::unique_ptr<cascade::source::IqSource> src;
        std::string label;
        std::string error;
    };
    std::map<cascade::core::patch::NodeId, std::future<PatchRadioOpen>> patchRadioPending_;
    std::map<cascade::core::patch::NodeId, std::string> patchRadioPendingAs_;
    // Why a radio is not running, when it tried and failed; drawn on its face
    // and turned into Problem::RadioFailed for the plan.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioError_;
    // The "<key>@<rate>" that failed, so a radio that would not open is not
    // retried every frame - only when its device or rate is changed.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioFailedAs_;
    // Decoder nodes that refused to start, per radio, merged into
    // patchRefused_ for the plan.
    std::map<cascade::core::patch::NodeId, std::vector<cascade::core::patch::NodeId>>
        patchRefusedBy_;
    // After the plan is compiled: make and drop speaker outputs, and publish
    // each running radio's set when it has changed.
    void patchPublishSets();
    // Per radio: what its running set was built from (radioSignature).
    std::map<cascade::core::patch::NodeId, std::string> patchRadioSig_;
    // Each speaker's output, and the output key it was made for.
    cascade::core::patch::DestTable patchDests_;
    std::map<cascade::core::patch::NodeId, std::string> patchDestMadeFor_;
    std::map<cascade::core::patch::NodeId, std::string> patchDestError_;
    // Each radio's newest spectrum, for its Spectrum parts and channel levels.
    struct PatchSpectrum {
        std::vector<float> db;
        std::uint64_t seq = 0;
    };
    std::map<cascade::core::patch::NodeId, PatchSpectrum> patchSpectra_;
    // THE RECEIVER'S RADIO, HANDED TO THE PATCH (owner: "when the patch panel
    // is running unpopulate the sdr from the main sdr software so it show
    // signal generator"). Opening the page switches the receiver to the
    // generator and remembers the radio here; closing the page opens it again.
    struct PatchMainKeep {
        bool valid = false;
        std::string kind;
        std::string args;
        std::string label;
        double rateHz = 0.0;
        double centreHz = 0.0;
    };
    PatchMainKeep patchMainKeep_;
    // One device a patch Radio can be set to, for the inspector's list.
    struct PatchDeviceChoice {
        std::string key;     // core/patch_devices.hpp
        std::string label;
    };
    std::vector<PatchDeviceChoice> patchDeviceChoices() const;
    std::string patchDeviceLabel(const std::string& key) const;
    // The device a newly added Radio starts on: the receiver's own radio if
    // no other Radio has it, else the first free device listed, else the
    // generator.
    std::string patchDefaultDeviceKey() const;
    // Per frame while the page is open: take the receiver's radio, open and
    // close radios to match the nodes, make and drop speaker outputs, and
    // publish each radio's set when it has changed.
    void patchReconcile();
    std::vector<cascade::core::patch::RadioInfo> patchRadioInfos() const;
    // Stops every patch radio and drops every output. `restoreMain` then hands
    // the receiver its radio back.
    void patchStopAll(bool restoreMain);
    // The patch transport (0.99.18): the START/STOP key, the red ALL OFF,
    // what a change of running does, and each radio's own switch.
    void patchPressStart();
    void patchAllOff();
    void patchApplyRunning();
    void drawPatchTransport();
    void drawPatchRadioSwitch(cascade::core::patch::Node& n);
    // THE MAP PARTS (0.99.18): each Map node's own view (its pan, zoom and
    // selection are its own, like a map page's), and this frame's targets for
    // it - the tracks of every decoder module wired into it.
    std::map<cascade::core::patch::NodeId, std::unique_ptr<MapView>> patchMapViews_;
    std::vector<cascade::core::HostTrack> patchMapTracks_;
    std::vector<cascade::core::HostPath> patchMapPaths_;
    // Fills the two lists above for one Map node.
    void patchCollectMapTargets(cascade::core::patch::NodeId map);
    void drawPatchMapInspector(cascade::core::patch::Node& n);
    // A demodulator's squelch: the switch, the threshold and the live level,
    // on its face and in the inspector alike (0.99.18). `width` is the control
    // width in pixels.
    void drawPatchSquelch(cascade::core::patch::Node& n, float width);
    // Every demodulator's squelch setting, handed to the runner of the radio
    // its channel is on - every frame, lock-free, so a drag is live.
    void patchPushSquelch();
    // The inspector's panels for a Radio and a speaker.
    void drawPatchRadioInspector(cascade::core::patch::Node& n);
    void drawPatchSinkInspector(cascade::core::patch::Node& n);

    bool demodScopeOpen_ = false;
    DemodScopeState demodScope_;
    std::vector<float> scopeAudioBuf_;
    std::vector<std::complex<float>> scopeIqBuf_;
    std::vector<float> scopeI_;
    std::vector<float> scopeQ_;
    std::vector<float> scopeLo_;
    std::vector<float> scopeHi_;
    // What the tube remembers for AVG and PERSIST (gui/scope_memory.hpp), the
    // mode it was filled under (a change of mode starts it afresh), and the
    // time of the last frame it saw - the blends and fades are timed.
    cascade::gui::ScopeMemory scopeMemory_;
    int scopeMemoryMode_ = -1;
    double scopeMemoryLastS_ = 0.0;
    // The audio spectrum, and the plan that makes it. Created on the first
    // frame the spectrum position is selected and kept afterwards: a pffft
    // setup is not free, and the page can be left on that position for hours.
    std::vector<float> scopeSpecDb_;
    std::vector<std::complex<float>> scopeFftIn_;
    std::vector<std::complex<float>> scopeFftOut_;
    std::vector<float> scopeFftWindow_;
    std::unique_ptr<cascade::dsp::ComplexFFT> scopeFft_;
    // WHETHER EACH TAP IS PRODUCING, measured on the clock rather than frame
    // to frame - see scopeTapLive in gui/demod_scope.hpp for why that
    // distinction is the difference between a working readout and one that
    // says "RECEIVER STOPPED" over a running receiver.
    //
    // TWO RECORDS, ONE PER TAP, and not one shared: the page reads whichever
    // tap the selected signal needs, and a record carried over from the other
    // one would answer for a tap nobody is looking at.
    cascade::gui::ScopeLiveness scopeAudioLive_;
    cascade::gui::ScopeLiveness scopeIqLive_;
    // The multiplex tap's own counter. A SEPARATE one because it stops the
    // instant the mode leaves WFM while the audio tap carries on - reading
    // the audio tap's liveness for the multiplex position would draw a
    // confident trace of a signal that is no longer being produced.
    cascade::gui::ScopeLiveness scopeMpxLive_;
    bool scopeLive_ = false;
    // The plugin currently playing through the host, if any. A member rather
    // than a local because the face is handed a const char* into it and the
    // string has to outlive the call.
    std::string scopeAudioFrom_;

    // --- THE TRANSMITTER (0.95.0) --------------------------------------------
    //
    // The TX thread, the modulator, the microphone and the radio all live
    // inside Transmitter; this is everything the PAGE needs and nothing more.
    //
    // NOTHING HERE CAN KEY IT. transmitPttHeld_ is written from the page's
    // own key and from the spacebar while the page has focus, and it is reset
    // to false at the top of every frame - so a page that stops being drawn
    // stops asking, which is the behaviour a window losing focus should have.
    // The LATCH is a deliberate switch and has its own failsafe inside
    // Transmitter. Neither is persisted; core/config.hpp says why at length.
    cascade::core::Transmitter transmitter_;
    bool transmitOpen_ = false;    // is the PAGE on screen
    bool transmitPttHeld_ = false; // this frame's key request, rebuilt each frame
    bool transmitLatched_ = false;
    bool transmitSplit_ = false;
    double transmitSplitHz_ = 145.5e6;
    int transmitModeIndex_ = 0;    // dsp::TxMode
    int transmitInputIndex_ = 1;   // core::TxInput; 1 is TONE, and deliberately
    double transmitPowerDb_ = -89.75;
    double transmitToneHz_ = cascade::dsp::kToneDefaultHz;
    // LISTEN WHILE TRANSMITTING. Off by default: a receiver left unmuted on
    // the frequency it is transmitting on is a howl, and on a full-duplex
    // board like the Pluto the receiver really is still running. On is what
    // somebody working split, or listening to their own signal through a
    // second radio, actually wants.
    bool transmitMonitor_ = false;
    // The address the transmit radio is opened at. Seeded from the Source
    // section's own args when that is a Pluto, because the overwhelmingly
    // common case is one board doing both.
    std::string transmitArgs_;
    std::string transmitError_;
    // The microphone's peak, decayed towards zero so the meter falls rather
    // than flickering - a bar redrawn from one 10 ms peak a frame is unreadable.
    float transmitPeak_ = 0.0f;

    // --- THE FUNCTION SELECT RAIL'S BANK -------------------------------------
    // Which of the five banks (gui/rail_banks.hpp) the rail is showing, as
    // 0..4; persisted as `railBank`. railBankFade_ is the progress of the
    // fade a newly selected bank comes up with, 1.0 once it is fully there,
    // so the swap reads as a lamp coming up rather than the column changing
    // in one frame.
    int railBank_ = 0;
    float railBankFade_ = 1.0f;

    // Whether ANYTHING asked the basemap for a tile this frame - the map pages
    // and, now, the scope. BasemapCache::endFrame() evicts every tile nothing
    // asked for, so it must run exactly once per frame and only AFTER every
    // surface that wants tiles has asked. It used to be called at the end of
    // the map-page loop, which was correct while that loop was the only asker;
    // with the scope drawing later in the same frame, an endFrame there would
    // have thrown away the scope's tiles and made it re-upload every one of
    // them, every frame.
    bool basemapUsedThisFrame_ = false;
    // Sort state for the track list, held here because the sort menu above the
    // table is the only thing that sets it and the table below has to be
    // ordered by it every frame. SEEDED FROM THE SAME CONSTANT the menu opens
    // on, so the opening order the code states is the one the screen shows -
    // written separately, the two disagreed and the constant here was dead.
    TrackSortKey trackSortKey_ = trackSortKeyForMenuIndex(kTrackSortDefaultIndex);
    bool trackSortAscending_ = true;
    // THE TARGET THE DETAILS WINDOW IS SHOWING, by id, empty when it is shut.
    // An id rather than a pointer or an index because the host's track vector
    // is rebuilt on every poll: an index would point at a different aircraft a
    // frame later, and a pointer would dangle. An id that is no longer in the
    // vector is a target that has gone, which the window says rather than
    // silently closing - a detail view that vanishes on its own looks like a
    // crash.
    std::string detailsTrackId_;
    bool detailsOpen_ = false;

    // WHICH PLUGIN WINDOWS ARE ON SCREEN - a decoder's picture, a plugin's own
    // panel - by ImGui window identity. Empty at every launch and never saved:
    // the application starts on the main screen alone (0.79.1), a plugin's
    // window opens from its row in DECODE (drawPluginWindowRows) and closes
    // from its own key. Until 0.79.1 this was the opposite memory - the
    // windows the user had CLOSED, persisted as AppConfig::closedWindows -
    // because every published window used to appear by itself. See
    // core::PluginWindows for the rule and its test.
    cascade::core::PluginWindows pluginWindows_;
    // What each INSTRUMENT window has shown: the plugin's event sequence when
    // the face last drew it, and when that happened. An event the window has
    // not drawn is NEW on the rail's chip; one it drew within the last few
    // seconds still lights the face's NEW lamp, because a lamp lit for one
    // frame is a lamp nobody saw. Keyed by the window id, never saved.
    struct InstrumentSeen {
        std::uint32_t seq = 0;
        double atSec = -1.0e9;
    };
    std::map<std::string, InstrumentSeen> instrumentSeen_;
    static constexpr double kInstrumentNewHoldSec = 4.0;
    // Decoded images, refreshed from PluginRunner once per frame. Owned HERE
    // rather than by the runner because it is written only when a decoder
    // produces a new picture: keeping the GUI's copy out of the runner is what
    // lets a megapixel frame be copied once per change instead of once per
    // rendered frame.
    std::vector<cascade::core::HostImage> pluginImages_;
    // GL textures for plugin images, one per image, keyed by index. Uploaded
    // only when the plugin says the pixels changed. imageTexPlugin_ records
    // whose picture each slot currently holds, so a rescan that puts a
    // different plugin in a slot cannot leave the old texture on screen.
    // The maker's badge on the scope face, uploaded ONCE from the RGBA icon
    // compiled into this binary (resources/icon/foxsdr_icon_rgba.hpp) - the
    // same pixels the window and taskbar use, so the badge cannot drift from
    // the application's own identity. 0 until the scope is first drawn.
    unsigned int scopeBadgeTex_ = 0;

    std::vector<unsigned int> imageTex_;
    std::vector<std::uint64_t> imageTexRev_;
    std::vector<std::string> imageTexPlugin_;
    // WHAT THE SAVE BUTTON SAID, WHOSE WINDOW SAID IT, AND WHEN.
    //
    // It was one AppWindow-wide string drawn inside EVERY image window by the
    // per-image loop, so saving an APT picture put "Saved ...bmp" underneath
    // the SSTV picture as well - a filename that has nothing to do with the
    // window it is lettered in - and nothing ever cleared it, so it sat there
    // for the rest of the session. The plugin name is what makes it belong to
    // one window; the timestamp is what makes it a message rather than a
    // permanent caption.
    std::string imageSaveNotePlugin_;
    std::string imageSaveNote_;
    double imageSaveNoteAtS_ = -1.0;
    // Long enough to read a full path off the window, short enough that it is
    // plainly about the save that just happened.
    static constexpr double kImageSaveNoteSeconds = 10.0;
    // Decoded output, newest last, bounded. The panel is a tail, not an
    // archive; the recorder is where a permanent copy belongs.
    std::deque<cascade::core::DecodedLine> decoderLog_;
    static constexpr std::size_t kDecoderLogMax = 500;
    // THE ONLY MESSAGE COUNT THIS APPLICATION HAS. No decoder plugin reports a
    // message tally over the ABI and the runner keeps none, so the one
    // countable thing a decoder produces is a LINE of output, counted here as
    // pumpDecoderOutput drains it. Cumulative and never reset, so the status
    // column can difference it over a window; decoderLog_ itself cannot serve
    // (it is a bounded tail and drops its oldest entries).
    std::uint64_t decoderLinesTotal_ = 0;
    // Rolling window behind the status column's decoder-line rate. Negative
    // rate means "not measured yet" - the card then prints no figure at all.
    std::uint64_t decoderLinesAtWindow_ = 0;
    double decoderRateWindowS_ = -1.0;
    float decoderLinesPerSec_ = -1.0f;
    // The span the figure above was actually divided by, so the card can
    // caption it with the mean it really is. It used to letter the nominal
    // window from a constant while dividing by however long the status column
    // had been off screen - and scope mode hides that column entirely, so a
    // spell in the scope produced a figure captioned "2 s mean" that was a
    // five-minute mean. The guard beside it throws such a window away; this is
    // what stops the two seconds either side of it from being a claim.
    double decoderRateSpanS_ = 0.0;
    // WHEN THE MUTE STARTED, as this GUI observed it: the mute subject the
    // status column last saw, and the time it changed to that. Nothing in the
    // pipeline timestamps a mute, so an unobserved one (the window was closed,
    // the application had not started) has no age and the card says nothing
    // about duration rather than guessing one.
    std::string muteSubjectSeen_;
    double muteSinceS_ = -1.0;
    bool decoderAutoScroll_ = true;
    // The output window opens from the DECODERS row's key and closes from its
    // own; it never opens itself (0.79.1 - it used to, on a decoder's first
    // line, which put it on screen at every launch).
    bool decoderWindowOpen_ = false;
    std::string pluginDir_;

    // --- Retirement enforcement (P11) -----------------------------------------
    //
    // WHY THERE IS A QUARANTINE STEP AND NOT JUST A RED ROW. A retired plugin
    // that is merely painted red is still mapped into this process: its
    // DllMain has run, its static initialisers are live, and its decoder
    // callbacks are one click away. The whole point of the feature is that the
    // stale CODE does not execute, so enforcement has to happen BEFORE
    // LoadLibrary, not after.
    //
    // PluginHost::scan() takes a DIRECTORY and loads every ".dll" in it (there
    // is no per-file load entry point, and plugin_host is fixed), so the only
    // way to keep one file out of a scan is to make it not look like a plugin
    // for the duration. rescanPlugins() therefore runs one ordered sequence:
    //
    //   unloadAll -> un-quarantine everything -> loadInventory (the disk is
    //   now complete, so reconciliation and planUpdates see the truth) ->
    //   blockedPlugins -> rename each blocked file to "<name>.dll.disabled"
    //   -> scan.
    //
    // The renamed file has no plugin extension, so PluginHost never sees it
    // and LoadLibrary is never called on it - not even once, not even at
    // startup. The rename is reversible and local: drop the floor (or update
    // the plugin) and the next rescan puts the name back.
    //
    // FAIL CLOSED on a failed rename. If a blocked file cannot be moved aside
    // (locked, read-only), the scan is SKIPPED entirely and the reason is
    // shown: loading every other plugin while the retired one loads with them
    // would be exactly the state this exists to prevent.
    static const char* pluginQuarantineSuffix();  // ".disabled"
    // Renames every "<name>.dll.disabled" back to "<name>.dll". A leftover
    // whose live name already exists (an update landed while it was aside) is
    // DELETED instead - it is a copy of a file this code renamed, and keeping
    // stale bytes around under a hidden name helps nobody.
    bool restoreQuarantinedPlugins(std::string& error);
    // Renames every currently blocked plugin file out of the scan's way.
    bool quarantineBlockedPlugins(std::string& error);
    // The manifest + cached policy, as of the last rescan and with the whole
    // directory present (see the ordering above). Everything downstream -
    // the red rows, the badge, planUpdates - reads THIS, not a fresh
    // loadInventory, so nothing ever plans against a quarantined file.
    cascade::core::PluginInventory pluginInventory_;
    std::vector<cascade::core::BlockedPlugin> pluginBlocked_;
    // Why enforcement could not be completed (red, above the list). Empty in
    // the normal case, which is every case where nothing is retired.
    std::string pluginEnforceError_;
    // What the catalogue currently offers over what is installed. Pure, cheap,
    // and empty until the user has fetched a catalogue this session - there is
    // no startup fetch, so an update can only ever be offered on request.
    std::vector<cascade::core::PluginUpdate> plannedPluginUpdates() const;
    // AppConfig::pluginLastUpdateCheck, stamped when a catalogue fetch
    // succeeds and persisted with the rest of the config.
    std::int64_t pluginLastUpdateCheck_ = 0;
    // AppConfig::pluginTuneAllowed. THIS is the durable copy of the grant, and
    // PluginUi holds the live one: PluginUi::clear() drops its set with the
    // instances on every rescan, so the permission has to survive somewhere
    // that a rescan does not touch.
    std::vector<std::string> pluginTuneAllowed_;
    // AppConfig::pluginSettingsAllowed - the durable copy of the level-1
    // SETTINGS grant, for exactly the reason pluginTuneAllowed_ is one.
    std::vector<std::string> pluginSettingsAllowed_;
    // AppConfig::pluginSettings - the durable copy of every plugin's own
    // settings. PluginApiCore holds the live one; applyPluginApi copies it
    // here whenever its generation moves, and currentConfig() saves this.
    std::map<std::string, std::map<std::string, std::string>> pluginSettings_;
    std::uint64_t pluginSettingsGen_ = 0;
    // The newest WARN or ERROR each plugin logged, by module file name, for
    // the notice on its plate. Session only, like the decoder output.
    struct PluginNotice {
        std::uint32_t level = 0;
        std::string text;
    };
    std::map<std::string, PluginNotice> pluginNotices_;
    // The plugins' marks, copied out of the core only when its sequence moves.
    std::vector<cascade::core::HostMarker> pluginMarkers_;
    std::uint64_t pluginMarkersSeq_ = 0;
    // AppConfig::pluginsStopped. The durable copy of "the user stopped this
    // plugin", by module file name, held here for the same reason the grants
    // are: PluginRunner and PluginUi are rebuilt on every source change and
    // cleared on every rescan, so the decision has to live somewhere neither
    // touches.
    std::vector<std::string> pluginsStopped_;
    // AppConfig::pluginMuteOverride. Held here for the same reason: the value
    // is a decision about a plugin, and the objects that act on it are torn
    // down and rebuilt underneath it.
    std::vector<std::string> pluginMuteOverride_;
    // What the mute policy is evaluated against, rebuilt whenever the plugin
    // set, a stop, or an override changes (see rebuildMuteStates).
    std::vector<cascade::core::MutePlugin> muteStates_;
    // Display names of the plugins currently holding the audio down. Empty
    // when nothing is. Read by the Sinks panel, the banner, the popup and the
    // web snapshot, so all four say the same thing.
    std::vector<std::string> mutedBy_;
    // Their module file names, in the same order. Kept beside the names rather
    // than looked up from them because the popup's Stop button must act on
    // EXACTLY the plugins the sentence above it named.
    //
    // Measured on the running application: stopping "every running plugin that
    // mutes" instead stopped AIS as well, which was running on a band 900 MHz
    // away and muting nothing - a dialog that said "ADS-B" and switched off
    // two decoders.
    std::vector<std::string> mutedByKeys_;
    // Was the receiver on a muting plugin's preset LAST frame. The popup fires
    // on the true -> false edge (cascade::core::tuneAwayEdge) and re-arms only
    // when this goes true again, which is what stops a modal reappearing on
    // every frame of a slow tune.
    bool mutePrevOnPreset_ = false;
    // The user tuned away and chose "Keep it running". The audio stays muted -
    // that is the model they were offered, sound comes back when the plugin
    // stops - and the banner stays up until it does, or until the tune returns
    // to a preset.
    bool muteKeptRunning_ = false;
    // WHAT THE POPUP IS ASKING ABOUT: the plugin names and keys captured when
    // the edge opened it, and never re-read from the live decision afterwards.
    // See cascade::core::advanceMutePopup for the measured failure that made
    // this a captured value rather than a call to muteSubjectText().
    cascade::core::MutePopupSubject mutePopup_;
    // Set when mutePopup_ goes open, consumed by the next drawMutePopup().
    // Deferred rather than calling ImGui::OpenPopup from the tune path because
    // the evaluation runs before the frame's windows exist, and a popup opened
    // against no ID stack is a popup that never appears.
    bool mutePopupQueued_ = false;
    // Bound on how many one-click presets a single plugin may put on the
    // panel. A plugin is third-party code and a list this long is not a menu.
    static constexpr std::uint32_t kMaxPresetsPerPlugin = 16u;
    // What the last preset click did, shown under the list — a receiver that
    // moved with no acknowledgement reads as a button that did nothing.
    std::string presetNote_;
    // A PRESET-BAR KEY PRESS, recorded and not yet applied. Every bar drawn
    // inside drawPluginWindows (the map/image/panel/instrument pages, and the
    // grouped bar in the Decoder output window) records into this rather than
    // calling applyPluginPreset directly — applyPluginPreset ends with
    // refreshPluginRunner(), which rebuilds the very panel/instrument/image
    // lists and map pages drawPluginWindows is iterating at that moment, and
    // rebuilding a list a for-loop is still walking is exactly the shape of
    // the crash 0.96.1 fixed in gui/list_pick.hpp. consumePendingPresetRequest
    // is the only reader, called once a frame from drawUi AFTER
    // drawPluginWindows returns — never from inside it.
    cascade::gui::PendingPresetRequest pendingPresetRequest_;

    // --- The user's own presets (0.99.4) ---------------------------------------
    // AppConfig::userPresets, for every plugin (see core/user_presets.hpp for
    // what they are and why). Held here, like the plugin lists above, because
    // everything that acts on them is rebuilt underneath them.
    std::vector<cascade::core::UserPreset> userPresets_;
    // A SAVE OR A FORGET, recorded and not yet applied. Both end in
    // rebuildMuteStates(), which replaces the muteStates_ entry a preset bar
    // is reading its keys from while it draws them - so, exactly like
    // pendingPresetRequest_, a press only records and the edit happens at the
    // same safe point (consumePendingPresetRequest). `pluginFileKey` is the
    // module file name (core::pluginKey); the version-stripped key the list
    // is stored under is derived from it when the edit is applied.
    struct PendingUserPresetEdit {
        enum class Op { None, Save, Forget };
        Op op = Op::None;
        std::string pluginFileKey;
        std::size_t ordinal = 0;  // Forget only: which of that plugin's presets
    };
    PendingUserPresetEdit pendingUserPresetEdit_;

    // --- Readouts held long enough to read (0.99.4) ---------------------------
    // See gui/readout_hold.hpp. FRAME TIME's text is the mean of the last half
    // second rather than this frame's delta; the AUDIO - UNDERRUNS card's
    // figures are re-read twice a second rather than every frame. The meter
    // needle stays live.
    cascade::gui::HeldMean frameTimeHold_;
    struct AudioReadout {
        unsigned long long underruns = 0;
        double ringMs = 0.0;
        double capMs = 0.0;
        unsigned long long gaps = 0;
        unsigned long long gapFrames = 0;
    };
    cascade::gui::HeldSample<AudioReadout> audioHold_;

    // --- Plugin browser (P9) --------------------------------------------------
    //
    // THE PRIVACY PROMISE. Nothing here contacts the catalogue origin until
    // the user presses Browse. Not at startup, not when the section is
    // expanded, not on a config restore that remembers the browser was open.
    // The published catalogue's README makes that promise to plugin authors
    // and users, and a paid product has to keep it, so the ONLY caller of
    // startCatalogFetch() is a button.
    //
    // THREADING. Identical in shape to the Soapy scan/open pair above and for
    // the same reason: a catalogue fetch is a TLS handshake plus an HTTP
    // round trip, and an install is a multi-megabyte download — seconds of
    // blocking work that would otherwise freeze the window and stall the
    // radio. Both run on a worker via std::async; the GUI polls the future
    // once per frame (pollPluginAsync) and applies the result on the GUI
    // thread. The worker touches pluginRepo_ and nothing else the GUI reads,
    // except progress()/cancel(), which are atomics for exactly this.
    cascade::core::PluginRepo pluginRepo_;

    // Result carriers. The catalogue entries are COPIED out of the repo on
    // the worker thread so the GUI never reads pluginRepo_.entries() while a
    // transfer could be rewriting it.
    struct CatalogFetchResult {
        bool ok = false;
        std::vector<cascade::core::PluginCatalogEntry> entries;
        std::string error;
        // cacheCataloguePolicies() runs on the SAME worker, immediately after
        // a successful fetch: that call is the only moment a retirement floor
        // is ever written to this machine, and deferring it to "some later
        // fetch" would mean a user who browses once and never again is never
        // protected. It is done off the GUI thread because it re-hashes every
        // installed plugin. A failure here is reported, never swallowed - the
        // catalogue still loaded, but the policy the user just saw was not
        // remembered.
        std::string policyError;
    };
    struct PluginInstallResult {
        bool ok = false;
        std::string name;           // display name, for the report
        std::string installedPath;  // set on success
        std::string error;          // verbatim from PluginRepo on failure
        bool isUpdate = false;      // applyUpdate (records itself) vs install
        // recordInstall's failure, for a PLAIN install only. The file is
        // installed and verified either way; what failed is the manifest
        // write, which leaves the plugin unmanaged (and therefore fail-open,
        // never retired) until an install or a catalogue fetch repairs it.
        std::string recordError;
    };
    std::future<CatalogFetchResult> catalogFuture_;
    std::future<PluginInstallResult> installFuture_;
    bool catalogPending_ = false;
    // The store's one automatic catalogue read per session (gui/store_first_open.hpp).
    cascade::gui::StoreFirstOpen storeFirstOpen_;
    bool installPending_ = false;
    std::string installBusyName_;  // shown in "Downloading <name>..."

    // Starts the catalogue fetch on a worker. https:// goes through
    // PluginRepo::fetchIndex; a path with no "://" scheme is read from disk
    // and parsed with the same parseIndex — see AppConfig::pluginCatalogueUrl
    // for why the local form exists and why it grants nothing extra.
    void startCatalogFetch();
    // Starts one install on a worker. Takes the entry BY VALUE: the worker
    // outlives the frame that spawned it, and catalog_ can be replaced by a
    // Refresh in the meantime.
    void startInstall(cascade::core::PluginCatalogEntry entry);
    // Starts ONE user-requested update on the same worker slot as an install
    // (PluginRepo has a single progress/cancel pair, so only one transfer runs
    // at a time). The plan's `entry` aliases catalog_, which a Refresh can
    // replace mid-transfer, so the worker takes its own COPY of the entry and
    // re-points the plan at it before calling applyUpdate.
    void startUpdate(const cascade::core::PluginUpdate& u);

    // --- ADD ALL PLUGINS (0.96.0) -------------------------------------------
    //
    // ONE RUN, MANY TRANSFERS, AND NOT ONE NEW CODE PATH FOR THE BYTES. The
    // store's key hands back a list of catalogue rows; this queue starts them
    // through startInstall / startUpdate one at a time, because PluginRepo has
    // a single progress/cancel pair and applies exactly one transfer at a
    // time. Everything a single FIT gets - the https rule, the byte cap, the
    // ABI test, the sha256, the manifest record, the rescan afterwards - a
    // module in this queue gets, because it IS a single FIT.
    //
    // IDENTIFIED BY CATALOGUE ID, NOT BY INDEX. A run outlives many frames and
    // a Refresh can replace catalog_ under it; an index would then name a
    // different module. An id that is no longer in the catalogue is recorded
    // as a failure with that reason rather than silently dropped.
    struct AddAllRun {
        bool active = false;
        std::vector<std::string> ids;
        std::vector<bool> isUpdate;
        std::size_t next = 0;  // the next id to start
        int installed = 0;
        int failed = 0;
        // {name, reason}, the reason verbatim and in English from whatever
        // refused it. Kept apart so the log gets the English and the panel a
        // translation of the same words (gui::trStoredReason).
        std::vector<std::pair<std::string, std::string>> failures;
        std::string currentName;  // what is moving right now
        std::size_t total = 0;
    };
    AddAllRun addAllRun_;
    // ONE PLACE BUILDS THE STORE'S MODEL, and it has to be one place now that
    // something other than the draw needs it: startAddAll re-plans from live
    // state, and a second transcription of catalogue row into StoreModule
    // would be a second set of rules about what "fitted" and "blocked" mean.
    void buildPluginStoreModel(PluginStoreModel& model);
    // Builds the queue from the store's own plan and starts it. Re-plans from
    // live state rather than trusting the plan the key was drawn from.
    void startAddAll(bool noticesAcknowledged);
    // Starts the next module in the queue when the slot is free, and writes
    // the summary when the queue empties. Called once per frame, after
    // pollPluginAsync has had its chance to clear installPending_.
    void pumpAddAll();
    // What the store window is told about the run in flight. Empty when none
    // is.
    std::string addAllProgressLine() const;
    // What a finished run left behind: "23 installed, 0 failed", or the names
    // that failed with the reason each gave. Kept until the next run starts.
    std::string addAllSummary_;
    bool addAllFailed_ = false;

    // Consumes finished catalogue/install futures; called once per frame from
    // drawUi, right beside pollSourceAsync.
    void pollPluginAsync();
    // True if the catalogue entry's file name already exists in the plugins
    // directory, comparing against every record PluginHost produced — loaded
    // AND refused — and against the manifest's own records. A refused DLL
    // still occupies the name, so treating it as "not installed" would offer
    // an install that could not replace it; and a RETIRED plugin has been
    // renamed out of the scan, so the host has no record of it at all — the
    // manifest is what keeps it from looking uninstalled and sending the user
    // down an Install path when Update is the remedy.
    bool catalogEntryInstalled(const cascade::core::PluginCatalogEntry& e) const;
    // The HOST RECORD for a catalogue entry, or null when the host has none.
    //
    // Narrower than catalogEntryInstalled on purpose, and the difference is
    // the point: this answers "did the loader see this file", which is what
    // lets the store's data plate report loaded, refused, running and the tune
    // grant from the same record the fitted window uses. A RETIRED module is
    // installed and has no host record - it was renamed out of the scan - so
    // this returns null for one while catalogEntryInstalled still says true,
    // and neither answer is wrong.
    //
    // The pointer is into PluginHost's own vector and is valid only until the
    // next rescan; every caller uses it inside the frame that asked.
    const cascade::core::LoadedPlugin* installedPluginRecord(
        const cascade::core::PluginCatalogEntry& e) const;

    // THE INSTALL GATE, as one named predicate so the button, the tooltip and
    // the --frames diagnostic can never disagree about it. Returns the reason
    // Install must stay disabled for catalog_[idx], or an EMPTY string when it
    // may be clicked. `acknowledged` is the state of the legal-notice
    // checkbox; it is a parameter rather than a member read so the same
    // function answers "would this be installable if the box were ticked",
    // which is what makes the gate observable in a headless run.
    std::string pluginInstallBlockedReason(int idx, bool acknowledged) const;

    // Deletes one installed plugin (see drawPluginsSection for the
    // unload-first rationale) and rescans.
    void removeInstalledPlugin(const std::string& fileName);

    // Deletes one BLOCKED (retired or ABI-mismatched) plugin. Separate from
    // removeInstalledPlugin because a blocked plugin is not on disk under its
    // own name: it has been renamed aside with pluginQuarantineSuffix(), so it
    // needs PluginRepo::removeQuarantined rather than remove().
    void removeBlockedPlugin(const std::string& fileName);

    // IS THE PLUGIN STORE WINDOW OPEN. Still AppConfig::pluginBrowserOpen,
    // which is exactly what that field has always meant - "the plugin browser
    // was open when you left" - now that the browser IS the window. Restoring
    // it opens the window and starts no fetch, the same promise the field
    // carried before: nothing in this application contacts the catalogue until
    // the user asks.
    //
    // NOTHING OPENS THIS WINDOW BUT ITS RAIL KEY. Not restored from the
    // config at start-up since 0.79.1 (the application starts on the main
    // screen alone), and nothing ever opened it by itself.
    bool pluginBrowseOpen_ = false;
    // IS THE FITTED MODULES WINDOW OPEN, and where it sat.
    //
    // PERSISTED, exactly as its sibling is. This was a plain session member
    // with no config key while the store beside it had one, so a window the
    // user left open closed on exit and was gone on the next launch - two
    // sibling keys in the same rail group behaving differently. It is now
    // AppConfig::fittedModulesOpen, and the rectangle is
    // AppConfig::fittedModulesX/Y/Width/Height, restored the way a map page's
    // is: checked against the monitors that exist NOW, clamped to what fits
    // where it lands, and read back every frame because ImGui's own .ini
    // persistence is switched off in this application.
    //
    // NOTHING OPENS IT BUT ITS RAIL KEY - the same promise pluginBrowseOpen_
    // above makes, and like it not restored from the config at start-up
    // since 0.79.1.
    bool fittedWindowOpen_ = false;
    // Zero width means "nothing saved" - the map pages' sentinel, and the
    // reason no companion flag is needed. Written back from the live window
    // every frame it is drawn (see drawFittedModulesWindow).
    int fittedWinX_ = 0;
    int fittedWinY_ = 0;
    int fittedWinW_ = 0;
    int fittedWinH_ = 0;
    // Whether the STORE WINDOW actually drew its content this frame.
    //
    // THE ORDERING CONSTRAINT SURVIVED BOTH BODIES BECOMING WINDOWS, it only
    // moved: it used to be "the store SECTION is drawn before the inventory
    // SECTION", because the rail drew them in that order and the second read
    // the flag the first had set. Both are windows now, drawn from
    // drawPluginWindows before the rail exists at all - so the rule is now
    // that drawPluginStoreWindow() is called before drawFittedModulesWindow(),
    // and it is enforced by their call order there and by this flag being
    // reset at the top of the store's own draw.
    //
    // What it is for is unchanged. installReport_/installError_ are written by
    // BOTH an install (the store) and a remove (the fitted window), so both
    // windows can show the same sentence; printing it twice reads as two
    // separate outcomes, and printing it in neither loses a failed remove
    // entirely. The store window shows it whenever it is drawn; the fitted
    // window shows it only when the store did not.
    bool pluginBrowserDrawnThisFrame_ = false;
    // The two windows' view objects and their persistent decks. Pointers
    // because both types live behind imgui.h; see the forward declarations at
    // the top of this header.
    std::unique_ptr<PluginStoreView> pluginStoreView_;
    std::unique_ptr<PluginStoreDeck> pluginStoreDeck_;
    std::unique_ptr<FittedModulesDeck> fittedDeck_;
    char pluginUrlBuf_[512] = "";    // edit buffer for the catalogue URL
    std::string pluginCatalogueUrl_;  // committed value (persisted)
    std::vector<cascade::core::PluginCatalogEntry> catalog_;
    std::string catalogError_;   // red: fetch/parse failure, verbatim
    std::string catalogStatus_;  // neutral: "N plugins in the catalogue"
    // --- update check --------------------------------------------------------
    bool closeRequested_ = false;     // set by the updater; the run loop honours it
    bool updateCheckEnabled_ = true;
    bool updateStarted_ = false;      // one check per launch, no retry storm
    bool updateDismissed_ = false;    // "not now" hides it until next launch
    bool updatePending_ = false;      // a check or a download is in flight
    bool updateDownloading_ = false;
    cascade::core::UpdateInfo update_;
    std::string updateError_;
    std::string updateReadyPath_;     // verified installer, waiting to be run
    // The app-update transfer's own progress and cancel, NOT pluginRepo_'s.
    // The banner used to read pluginRepo_.progress(), which nothing on this
    // path ever writes — the bar sat at 0 for the whole download — and
    // pluginRepo_.cancel() could not reach a transfer started through the
    // static fetch helper, so quitting mid-update blocked in the future's
    // destructor until the download finished. One pair per transfer keeps the
    // plugin browser's bar and this one from reporting each other's bytes.
    //
    // DECLARED BEFORE THE FUTURES, deliberately: members are destroyed in
    // reverse declaration order, and the download future's destructor BLOCKS
    // until its worker returns — a worker that is still polling this flag. A
    // pair declared after the future would be destroyed while that read is in
    // flight.
    //
    // THE SAME APPLIES TO EVERY OTHER MEMBER EITHER WORKER TOUCHES, which is
    // why the three result slots below moved up here from after the futures.
    // The atomics are only what the workers READ; these are what they WRITE.
    // downloadUpdate() is handed updateResultPath_ and updateResultError_ by
    // reference and assigns to them as it goes, and checkForUpdate() does the
    // same with updateResult_ and updateResultError_ — so declared after the
    // futures they were std::string and UpdateInfo destructors running while a
    // worker thread was mid-assignment into them, a use-after-free reached
    // through a dangling `this` rather than a data race the flags could stop.
    // The blocking future destructor that makes quit slow is also the only
    // thing that makes this ordering enough: it guarantees both workers have
    // returned before anything declared above the futures is destroyed.
    std::atomic<float> updateProgress_{0.0f};
    std::atomic<bool> updateCancel_{false};
    cascade::core::UpdateInfo updateResult_;
    std::string updateResultError_;
    std::string updateResultPath_;
    std::future<bool> updateCheckFuture_;
    std::future<bool> updateDownloadFuture_;

    // WHICH CATALOGUE ROW IS SELECTED, AND WHETHER ITS NOTICE WAS TICKED, both
    // live in PluginStoreDeck now (selected, legalAck) - the store window owns
    // its own selection and clears the tick whenever that selection moves, so
    // consent given for one plugin can never be carried to the next. The two
    // members that used to hold them are gone rather than kept in step with
    // the deck: two copies of a consent flag is one copy too many.
    std::string installReport_;  // green: last successful install/remove
    std::string installError_;   // red: last failed install/remove, verbatim
    // The REMOVE confirmation for an installed module is the fitted window's
    // (FittedModulesDeck::confirmRemove, keyed on the module file name because
    // a rescan reorders the list). Only the disabled list still keeps an index
    // here, and it is drawn from the rail.
    int blockedRemoveConfirmIdx_ = -1;  // disabled-list row awaiting confirmation

    // --- Bounded-run test hook (P9) --------------------------------------------
    // CASCADE_PLUGIN_TEST=<url-or-path>, honored ONLY by run(frames >= 0),
    // exactly like main()'s CASCADE_CONFIG_TEST: it points the browser at a
    // catalogue, opens the section, starts ONE fetch on the first frame and
    // prints a machine-readable summary of the catalogue and of every entry's
    // install-gate decision. No interactive session can be redirected by a
    // stray environment variable, and a normal --frames run prints nothing
    // extra, so the byte-identical-stdout contract is untouched.
    std::string pluginTestHook_;
    bool pluginTestStarted_ = false;
    // Frames rendered so far, published so the hook's diagnostic can name the
    // frame the fetch resolved on — which is what proves the window kept
    // rendering while the transfer was in flight.
    int frameCounter_ = 0;
    // Prints that summary from the LIVE member state (catalog_ /
    // catalogError_), not from the future's payload, so what it reports is
    // exactly what the UI is about to draw.
    void reportPluginTestResult();

    // CASCADE_PLUGIN_STATUS=1, honored ONLY by run(frames >= 0), like the hook
    // above. Prints what the ENFORCEMENT decided: how many candidates the host
    // actually mapped, what blockedCount() says, and one line per retired
    // plugin including whether the host has any record of it (mapped=0 is the
    // proof that the stale code is not in the process). It is printed at the
    // end of the run so it reflects any catalogue fetch the run performed.
    bool pluginStatusHook_ = false;
    void reportPluginStatus();

    // --- Web server mode (P11) ------------------------------------------------
    //
    // WHY THE SERVER IS FED FROM A PUBLISHED SNAPSHOT rather than reading the
    // pipeline directly. Its provider callbacks run on HTTP worker threads, and
    // two of the things a browser wants are documented GUI-THREAD-ONLY:
    // Pipeline::activeSourceName() returns a const char* valid only until the
    // next setSource, and the source's own centre-frequency readback has the
    // same contract. Calling either from a request handler would be a race that
    // shows up as a torn string or a dangling pointer under exactly the
    // conditions — a source swap — that are hardest to reproduce. So the GUI
    // thread assembles one consistent snapshot per frame under webMutex_, and
    // the providers do nothing but copy it out.
    void drawCatSection();
    void drawWebSection();
    // Copies audio produced since the last frame into the server's ring.
    //
    // The pipeline's tap is a rolling 4096-frame window with no per-reader
    // position, so "what is new" comes from audioSamplesProduced(), which is
    // monotonic and counts FRAMES in the same unit the tap returns. The
    // difference between two readings is exactly what to copy — that is what
    // makes the stream gap-free and repeat-free rather than "whatever the
    // window happened to hold". If the GUI ever stalls longer than the window
    // (85 ms at 48 kHz) the excess is unrecoverable and is dropped; the
    // listener hears a glitch, which beats hearing stale audio for ever after.
    void publishWebAudio();
    // Re-encodes decoded pictures for the browser, but ONLY when a decoder's
    // revision has actually moved: encoding a megapixel BMP every frame would
    // cost more than everything else the server does put together.
    void publishWebImages();
    // Revisions the last publish encoded, one per image slot.
    std::vector<std::uint64_t> webImageRevs_;
    // Serves the browser map's tile wants from the basemap plugin. The
    // browser's viewport drives WHICH tiles; this frame-time pump is what
    // keeps the plugin on the GUI thread, the only thread the ABI lets call
    // it: the server records requests it cannot answer, this drains a bounded
    // number per frame, fetches, encodes, publishes, and the browser retries.
    void pumpWebTiles();
    // What the last pump saw, so a plugin change or removal clears the
    // server's store instead of serving the old source's imagery as the new's.
    bool webTilesActive_ = false;
    std::string webTileAttribution_;
    // Copies the current radio state and newest spectrum frame into the
    // members below. Called once per frame from drawUi, unconditionally: the
    // panel being collapsed must not stop the browser being served.
    void publishWebSnapshot();
    // Drains the server's control queue and applies each request to the radio.
    // Called once per frame from drawUi, on the GUI thread, because that is
    // the only thread allowed to move the SOURCE — the HTTP handler validated
    // the request and queued it precisely so it would not have to.
    //
    // Applying here rather than in the handler also keeps the panel mirrors
    // (modeIndex_, vfoOffsetKhz_, squelchDb_ ...) in step, so a change made
    // from a browser shows up on the desktop window and in the debounced
    // config save exactly as though it had been clicked.
    // Drains BOTH servers' queued requests — the browser's and CAT's — and
    // applies them through one body of code, so the two can never drift.
    void applyWebControls();
    // Starts or stops the CAT server to match the current configuration, and
    // records why in catStatus_ when it will not start.
    void refreshCatServer();
    // Applies webCfg_ to the server: starts, restarts or stops it, and puts
    // the outcome in webError_ / webNote_. The ONE place that calls
    // WebServer::start, so the panel, the config restore and the password
    // dialog cannot drift apart.
    void applyWebSettings();
    // Hashes `password` and stores the record, then re-applies. An empty
    // string CLEARS the password, which the policy will refuse if the binding
    // is not loopback — deliberately, since that is the user removing the only
    // thing protecting an exposed receiver.
    void setWebPassword(const std::string& password);

    cascade::net::CatServer catServer_;
    // Shown in the panel: empty while things are as configured, otherwise the
    // reason the server is not listening (a port already held by a real
    // rigctld being much the most common).
    std::string catStatus_;
    // Panel mirrors, assembled into an AppConfig by currentConfig(). There is
    // no separate dirty flag anywhere in this window: the debounced save
    // compares the live state against the last saved one, so a setting is
    // persisted purely by appearing in currentConfig() and configsEqual().
    bool catEnabled_ = false;
    bool catBindAll_ = false;
    int catPortMirror_ = static_cast<int>(cascade::net::kDefaultCatPort);

    cascade::net::WebServer webServer_;
    cascade::net::WebServerConfig webCfg_;
    // Panel mirrors (ImGui edits by pointer). webBindChoice_: 0 = this machine
    // only, 1 = every network interface. A specific interface address loaded
    // from the config that matches neither shows as choice 2 and is left alone.
    int webBindChoice_ = 0;
    int webPortMirror_ = cascade::net::kDefaultWebPort;
    char webUserBuf_[64] = "admin";
    char webPassBuf_[128] = "";
    char webPassConfirmBuf_[128] = "";
    std::string webError_;   // red: the policy's refusal, or a bind failure
    std::string webNote_;    // neutral/green: "serving at http://..."
    // SWITCHED ON AND NOT LISTENING, WHICH IS NOT THE SAME AS SWITCHED OFF.
    // Set by applyWebSettings when a start is refused - a port already held by
    // something else is much the commonest cause - and cleared by the next
    // apply, successful or not. It exists because the status column and the
    // rail row both used to read "off" for a server the user had deliberately
    // turned on, with the refusal visible only to somebody who opened the Web
    // access section; a socket that could not be opened is news, not a
    // setting. webError_ cannot serve in its place: the password field writes
    // its own complaints there, and a mistyped password is not a listener that
    // refused to start.
    std::string webStartRefusal_;
    // Edits are staged and applied on a button rather than taking effect as
    // they are typed. Two reasons: restarting the listener on every keystroke
    // of the port field is nonsense, and — the real one — a setting that
    // decides who can reach the receiver should be reviewed before it takes
    // effect, not applied halfway through being typed.
    bool webDirty_ = false;
    // Addresses of this machine's own interfaces, for the "open this on your
    // phone" hint. Filled lazily the first time the section is drawn, because
    // enumerating adapters is a syscall nobody needs on a headless run.
    std::vector<std::string> webLocalAddresses_;
    bool webAddressesScanned_ = false;

    // audioSamplesProduced() at the last publish, and the scratch buffer the
    // tap is read into (a member so a steady stream never allocates).
    std::uint64_t webAudioLastProduced_ = 0;
    std::vector<float> webAudioBuf_;

    mutable std::mutex webMutex_;
    cascade::net::RadioStatus webStatus_;
    std::vector<float> webBins_;
    std::uint64_t webSeq_ = 0;
    double webSnapCenterHz_ = 0.0;
    double webSnapSpanHz_ = 0.0;

    cascade::core::Scanner scanner_;
    double scanStartMhz_ = cascade::core::Scanner::Params{}.startHz / 1.0e6;
    double scanStopMhz_ = cascade::core::Scanner::Params{}.stopHz / 1.0e6;
    double scanStepKhz_ = cascade::core::Scanner::Params{}.stepHz / 1.0e3;
    double scanDwellMs_ = cascade::core::Scanner::Params{}.dwellMs;
    double scanHoldMs_ = cascade::core::Scanner::Params{}.holdMs;
    double scanResumeMs_ = cascade::core::Scanner::Params{}.resumeMs;
    double scanListenMs_ = cascade::core::Scanner::Params{}.listenMs;
    // The pointer ledger (see the frame loop), switched on from the
    // Diagnostics section so a tester can read it without setting an
    // environment variable. Not saved: it is a measurement, not a setting.
    bool inputLedger_ = false;
    // Readback (center + offset) right after the last scanner-commanded
    // retune. Any later frame where the live readback differs is a tune the
    // scanner did not make — a manual tune, and the user wins (scan stops).
    double scannerExpectedAbsHz_ = 0.0;
    bool scannerHasExpected_ = false;  // false until the scan's first retune
};

}  // namespace cascade::gui
