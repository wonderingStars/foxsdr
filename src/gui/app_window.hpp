// GLFW + Dear ImGui application shell.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <thread>
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
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Forward-declared rather than including GLFW here: this header is included
// by the tests, and pulling a windowing library into them would make a
// headless build depend on one.
struct GLFWwindow;

#include "engine/engine.hpp"
#include "engine/receiver_tables.hpp"
#include "core/app_commands.hpp"
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
#include "core/patch_recordings.hpp"
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
#include "core/engine_measure.hpp"
#include "gui/frame_log.hpp"
#include "core/scanner.hpp"
#include "core/transmitter.hpp"
#include "gui/basemap_cache.hpp"
// The floor a torn-off page cannot be dragged under, and the reset generation
// that puts an already-wrong one back. ImGui-free for the same reason as the
// headers below it - the tests include it without a graphics context.
#include "gui/page_geometry.hpp"
#include "gui/store_first_open.hpp"
#include "gui/rail_banks.hpp"
#include "engine/running_view.hpp"
#include "gui/bench_rail.hpp"
#include "engine/audio_open.hpp"
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
#include "engine/tune_control.hpp"
#include "source/soundcard_source.hpp"
#include "engine/device_scan_plan.hpp"
#include "engine/source_fallback.hpp"
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
// The bias tee checkbox's state and rules (no ImGui in it).
#include "engine/bias_tee.hpp"

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
class AppWindow : private cascade::engine::EngineHost {
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
    // THE TEST SEAM for the paths that carry a frequency from one radio to the
    // next (tests/test_converter_app_paths.cpp): a device switch, the startup
    // restore, the reopen after a driver fault and the patch page's take-over
    // and hand-back. Each is AppWindow code that no pure function can stand in
    // for, so the test drives the real members through this friend, with the
    // two hooks below standing in for the hardware. Both are null in every
    // build of the application - nothing ever sets them outside that test -
    // and each is read at exactly one place.
    friend struct AppWindowTestAccess;
    // THE ENGINE (engine extraction stage 3a, docs/engine-stage3.md): the
    // receiver's state and machinery, owned by an Engine this window holds
    // and asks. Declared FIRST, so it is built before and destroyed after
    // everything the window keeps.
    std::unique_ptr<cascade::engine::Engine> engineHolder_;
    cascade::engine::Engine& engine_;

    // --- WHAT THE ENGINE ASKS OF THIS WINDOW (engine/engine_host.hpp) ---------
    // Each answers exactly as the line the moved code used to run here did,
    // on the same (GUI) thread at the same moment; the definitions are at the
    // end of app_window.cpp. openPluginWindowsFor, declared further down, is
    // the one this window already had under the same name.
    // THE FRAME CLOCK, THREAD-AWARE (engine/stage3b-pre, docs/engine-stage3.md
    // OPEN 4): on the GUI thread both read ImGui as they always did and leave
    // the answer in an atomic; from any other thread they return that answer
    // and never touch ImGui's context. drawUi reads them once a frame, so the
    // answer another thread gets is at most a frame old.
    bool frameClockRunning() const override;
    double frameTimeS() const override;
    std::thread::id guiThread_ = std::this_thread::get_id();
    mutable std::atomic<double> frameTimeCache_{0.0};
    mutable std::atomic<bool> frameClockCache_{false};
    double wallTimeS() const override;
    void pauseWatchdog() override;
    void resumeWatchdog() override;
    void onDisplayRange(float dbMin, float dbMax) override;
    void onBookmarksChanged() override;
    void onReceiverPositionApplied(double latDeg, double lonDeg) override;
    void onConverterChanged(bool clearLoError) override;
    void onPatchGraphChanged() override;
    void onPatchPicture(cascade::core::patch::NodeId node, cascade::core::HostImage&& img) override;
    void onGpsFixApplied() override;
    void onCatalogueFetchStarting() override;
    void onCatalogueResult() override;
    void onAddAllFinished() override;
    AddAllChoice planAddAll(bool noticesAcknowledged) override;
    void beforePluginRescan() override;
    void onPluginsUnloading() override;
    void attachBasemap(const CascadeBasemapApi* api) override;
    void attachTrackInfo(const CascadeTrackInfoApi* api) override;
    void showDemonstrationInstrument(const cascade::core::HostInstrument& in) override;
    std::vector<std::string> drainTrackInfoText() override;
    bool patchPageOpen() const override;
    // The three publish-time facts (engine/stage3b-pre OPEN 4): no longer
    // EngineHost hooks the engine calls while publishing, but this window's
    // answers, HANDED OVER once a frame (handFrontEndFacts).
    bool webListening() const;
    std::string tunerDisplayStyle() const;
    BasemapFacts basemapFacts() const;
    void handFrontEndFacts();
    void enrichWebTrack(cascade::net::RadioStatus::Track& w) override;
    void fillWebImages(cascade::net::RadioStatus& s) override;
    // THE WINDOW'S HALVES OF THREE ENGINE FUNCTIONS THAT WERE ONE. The engine
    // publishes the receiver snapshot and hands back the centre it published;
    // the browser's spectrum frame (a stream, not state) is copied here,
    // straight after, as it was. The usage report is the engine's; the crash
    // report offer, the crash-loop limiter's memory and the update and
    // diagnostics switches that were read and written beside it are this
    // window's, and run beside the engine's half exactly where it runs.
    void publishWebSpectrum(double centerHz);
    void diagnosticsStartup(const cascade::core::AppConfig& cfg);
    void diagnosticsJournal(cascade::core::AppConfig& cfg);
    // The engine's types and constants under the names this window's code
    // has always used them by.
    using LocalCommand = cascade::engine::Engine::LocalCommand;
    using DeviceOpenResult = cascade::engine::Engine::DeviceOpenResult;
    using SoundCardOpenResult = cascade::engine::Engine::SoundCardOpenResult;
    using ConverterHeldAir = cascade::engine::Engine::ConverterHeldAir;
    using PatchDecoderFace = cascade::engine::Engine::PatchDecoderFace;
    using PatchRadioOpen = cascade::engine::Engine::PatchRadioOpen;
    using PatchSpectrum = cascade::engine::Engine::PatchSpectrum;
    using PatchMainKeep = cascade::engine::Engine::PatchMainKeep;
    using PatchDeviceChoice = cascade::engine::Engine::PatchDeviceChoice;
    using PluginNotice = cascade::engine::Engine::PluginNotice;
    using PendingUserPresetEdit = cascade::engine::Engine::PendingUserPresetEdit;
    using CatalogFetchResult = cascade::engine::Engine::CatalogFetchResult;
    using PluginInstallResult = cascade::engine::Engine::PluginInstallResult;
    using AddAllRun = cascade::engine::Engine::AddAllRun;
    static constexpr auto kSoundCardRow = cascade::engine::Engine::kSoundCardRow;
    static constexpr auto kNativeRowBase = cascade::engine::Engine::kNativeRowBase;
    static constexpr auto kAudioOpenByUser = cascade::engine::Engine::kAudioOpenByUser;
    static constexpr auto kAudioOpenByWatchdog = cascade::engine::Engine::kAudioOpenByWatchdog;
    static constexpr auto kDecoderLogMax = cascade::engine::Engine::kDecoderLogMax;
    static constexpr auto kMaxPresetsPerPlugin = cascade::engine::Engine::kMaxPresetsPerPlugin;

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

    void drawPluginPresets(const cascade::core::LoadedPlugin& p);
    // The window half of a preset press: the plugin's map page, picture,
    // panels and instruments, or Decoder output for a text decoder.
    void openPluginWindowsFor(const cascade::core::LoadedPlugin& p) override;
    // The REMNANT of the receiver-control rows, and it is the half the fitted
    // window cannot draw: the refusal notice PluginUi records, and the grants
    // held by modules that are NOT installed any more. The fitted window
    // carries the grant key for every module it lists, so those rows are not
    // repeated here - but it lists only what the host loaded, and a permission
    // the user can neither see nor revoke is exactly the kind that must not
    // exist. Draws nothing when there is neither a refusal nor a stale grant.
    void drawPluginTuneControls();

    // The SELECT_SOURCE id of a row of the Source list (the inverse of
    // selectSourceById for every row a click can name).
    std::string sourceIdForRow(int row) const;
    // The plugins' spectrum and waterfall marks, over the panel at (x0, y0).
    void drawPluginMarkers(float x0, float y0, float width, float height, bool waterfall);

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
    // plugin publishes no valid preset. A key's own press only SUBMITS a
    // command (PLUGIN_PRESET / APP_USER_PRESET_APPLY), applied by the drain at
    // the top of the next frame - never here, mid-iteration of the very lists
    // a preset's own apply rebuilds (the crash 0.96.1 fixed in
    // gui/list_pick.hpp).
    void drawPluginPresetBar(const std::string& displayName);
    // THE SHARED DRAWING OF ONE PLUGIN'S ROW OF KEYS, used by
    // drawPluginPresetBar (one plugin, its own bar) and drawDecoderPresetBars
    // (several text decoders, one bar each, grouped under the shared Decoder
    // output window). Wraps within the available width via
    // cascade::gui::presetBarRows; every press submits a command naming
    // `pluginKey`, never applies inline.
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

    // The modal that offers to stop the plugins holding the audio down, and
    // the banner that stays when the user declines.
    void drawMutePopup();
    // What the banner names, or "" when there is no banner to draw. The test
    // seam FOXSDR_FORCE_MUTE_BANNER=<name> draws it naming <name> whatever
    // the audio is doing, so the theme census can place and measure it.
    std::string muteBannerSubject() const;
    // Draws the banner where layoutMuteBanner put it (bar-relative, from the
    // bar's top-left `barTL`): the words - whole, or shortened with the full
    // sentence in a tooltip - and the "Stop plugin" key, always whole.
    void drawMuteBanner(const cascade::gui::MuteBannerLayout& mb, const ImVec2& barTL,
                        const std::string& words);
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

    // --- the patch page ------------------------------------------------------
    // Every frame, shown or not: retires the receiver's patch sets, and when
    // the patch view has just been left does what closing the page always
    // did - stops the patch and gives the receiver its radio back. Draws
    // nothing since 0.99.40; the canvas is drawPatchView.
    void drawPatchPage();
    // THE PATCH VIEW (0.99.40): the canvas, its transport, parts bin and
    // inspector, drawn as the main window's face in place of the spectrum,
    // the waterfall and the status column - into the child it is called in,
    // so it is exactly as large as the window leaves it.
    void drawPatchView();
    // Which face the main window shows: the patch (true) or the receiver.
    // Only that - it asks for no device list (see patchListsWanted_).
    void setMainViewPatch(bool patch);
    // The two view keys, RECEIVER and PATCH, under the rail's bank keys:
    // at the column's left colX and width colW, laid from `top`. Returns the
    // y below them.
    float drawViewKeys(float colX, float colW, float top);
    // The starter patch (one Radio node on the receiver's radio, at its air
    // centre) when the page opens with none; a no-op once seeded.
    void seedPatchIfNeeded();
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

    // The Source section's lamp for a saved source the generator is standing
    // in for (0.99.36): lit while one is remembered and not open.
    bool radioNotOpenLit() const;

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

    // I/Q centre as typed, in MHz (the box's own unit).
    double soundCardCentreMhz_ = 0.0;

    // The Source section's controls for the row.
    void drawSoundCardControls();
    // "Receives X to Y." under the Source section's controls, from the
    // settings shown: the AIR range, through the converter stored for the
    // section's card (gui::soundCardAirSpan).
    std::string soundCardReceivesText() const;

    char iqPath_[512] = "";     // InputText buffer for the IQ file path
    // A LOCAL DRAFT (engine/stage3b-pre fields-to-commands round 2): the
    // Pluto address box's edit buffer, seeded from engine_.plutoUri_ once in
    // applyConfig - typing here never writes the engine field (Open commits
    // it, through Engine::openPlutoAt).
    char plutoUriDraft_[192] = "ip:192.168.2.1";
    // The Transmit page's address box: same shape. Both boxes are re-seeded
    // from the engine field on every frame the box is NOT being typed in (so
    // a change made elsewhere - the web remote's open-pluto, a TX_OPEN from
    // another client, a config reload - shows at once), and every edit is
    // committed as it is typed (FOXAPP_OP_SET_PLUTO_URI /
    // FOXAPP_OP_SET_TRANSMIT_ARGS), so an address typed but never opened is
    // still what config.json keeps, as it was before the drafts existed.
    char transmitArgsDraft_[128] = {0};
    // The scanner form's own draft (engine/stage3b-pre fields-to-commands
    // round 2, fixed in round 3): one per field, in drawScannerSection's
    // order - start MHz, stop MHz, step kHz, dwell, hold, resume, listen ms.
    // Defaults are Scanner::Params{}, the engine fields' own, so a launch
    // whose config never loaded shows (and commits) real values, never 0.
    // Re-seeded from the engine field whenever that field is neither being
    // typed in nor holding an edit not yet committed (scanDraftPending_), so
    // the form always shows what Start would scan - see drawScannerSection.
    double scanDraft_[7] = {
        cascade::core::Scanner::Params{}.startHz / 1.0e6, cascade::core::Scanner::Params{}.stopHz / 1.0e6,
        cascade::core::Scanner::Params{}.stepHz / 1.0e3,  cascade::core::Scanner::Params{}.dwellMs,
        cascade::core::Scanner::Params{}.holdMs,          cascade::core::Scanner::Params{}.resumeMs,
        cascade::core::Scanner::Params{}.listenMs,
    };
    // Bit i: scanDraft_[i] was edited while a scan was RUNNING and is not
    // committed yet (a running scan takes a field only once it is finished
    // with - never a half-typed value). flushScannerDraft commits it if the
    // field stops being edited without the section seeing it deactivate.
    unsigned scanDraftPending_ = 0;
    // The ImGui id each scanner field had when last drawn (0 = never drawn),
    // so flushScannerDraft can tell "still being typed in" without drawing.
    std::uint32_t scanDraftIds_[7] = {};  // ImGuiID, without imgui.h here
    // Commits the fields in `bits` from scanDraft_: the timing half through
    // FOXAPI_OP_SCANNER_CONFIG, then the range half through
    // FOXAPP_OP_SCANNER_RANGE (only the edited range fields' bits, plus the
    // reconfigure bit when `reconfigure` and a scan is running).
    void commitScannerDraft(unsigned bits, bool reconfigure);
    // Once a frame from drawUi, whether or not the Scanner section is drawn:
    // commits a pending running-scan edit whose field is no longer active
    // (the section was collapsed or its bank switched away mid-edit).
    void flushScannerDraft();
    // THE AIRSPY R2 / MINI's OWN CONTROLS (0.99.41, app_window_airspy.cpp):
    // one gain mode at a time - Sensitive, Linear or Free, the reference
    // Airspy application's three - with only that mode's sliders, Free mode's
    // two AGC switches, and the software decimation. Draws them and answers
    // true when the open radio is an Airspy, in which case the generic Auto
    // gain switch and gain sliders are not drawn; false and draws nothing for
    // every other radio. The state it reads (deviceGainNames_ etc.) and the
    // choosing (chooseAirspyDecimation/GainMode/Agc) are Engine's - receiver
    // state like every other gain/AGC/rate mirror - called directly as a
    // reviewed exception (kControlMayCall), the same pattern as scanSoundCards.
    bool drawAirspyControls();

    // --- THE DECK'S BIAS TEE KEY (2026-09-25, app_window_bias_key.cpp) --------
    //
    // A second control over biasTeePanel_, on the deck: drawn only while the
    // open radio has a bias tee, lit exactly when `shown` is, and switching
    // through switchBiasTee - the checkbox's own path - so the two cannot
    // disagree. Off is immediate; on asks first, once per radio per session
    // (engine/bias_tee.hpp, BiasKeyGate, says why a dialog and not a hold).
    cascade::gui::BiasKeyGate biasKeyGate_;
    // Raised by a press that has to ask; the dialog is opened from the top
    // level (drawBiasKeyConfirm), because the press happens inside the deck's
    // child and ImGui wants OpenPopup in the same ID stack as the modal.
    bool biasKeyAskQueued_ = false;
    // The panel the KEY shows: biasTeePanel_, or - only while no real radio
    // with a bias tee is open - the census/capture stand-in below.
    cascade::gui::BiasTeePanel biasKeyPanel() const;
    // The key was pressed / the dialog was answered (true: "Turn it on").
    void biasKeyPressed();
    void biasKeyAnswered(bool turnOn);
    // The confirmation dialog, drawn every frame from drawUi beside the mute
    // popup; it closes itself when its question stops applying.
    void drawBiasKeyConfirm();
    // FOXSDR_FORCE_BASEMAP in a bounded run (gui/basemap_stand_in.hpp): the
    // stand-in basemap is attached whenever no plugin supplies one.
    bool basemapStandIn_ = false;

    // --- THE CONVERTER IN FRONT OF THE RADIO (0.99.36, app_window_converter.cpp)
    //
    // An up- or down-converter between the antenna and the radio, set in the
    // Source section and REMEMBERED PER RADIO (AppConfig::converters, keyed by
    // core::converterRadioKey). The arithmetic is core/freq_converter.hpp's;
    // the translation is the pipeline's (activeSource() speaks air - see
    // Pipeline::setConverter), so nothing in this window converts anything
    // itself. What this window does is choose WHICH setting applies: after
    // every source install (setSource puts the pipeline back to OFF), the
    // installed radio's own.
    //
    // Every install site calls applyConverterForSource() straight after its
    // setSource and before any tune, so a carried-across frequency is sent
    // through the NEW radio's converter. radioHzForSource() is for the two
    // places that tune a source BEFORE it is installed (the restore at start
    // and the IQ file's Open).
    void drawConverterControls();
    // The modes the Converter combo offers for the source installed, in the
    // combo's order; EMPTY means the Converter controls are not drawn at all.
    std::vector<cascade::core::ConverterMode> converterModesOffered() const;
    // A sentence under the Converter controls about a setting that is stored
    // for this source but cannot apply to it; "" when there is none.
    std::string converterUnusableNote() const;
    // The status column's line while a converter is on, "" otherwise;
    // shortForm names the converter by its LO only, for a narrow column.
    std::string converterStatusLine(bool shortForm = false);
    // The one line the Source section shows while such an alias is in force
    // and the converter is on, or while a converter was NOT carried and none
    // is set here; "" otherwise.
    std::string converterAliasNote();
    char converterLoBuf_[40] = {};
    std::string converterLoSeededFor_;  // the radio key + LO the field was seeded from
    bool converterLoBad_ = false;

    // --- Frequency scale + view interaction state (P5) -----------------------
    // ONE scale owns the x <-> Hz <-> bin mapping for both center panels, fed
    // every frame from the active source's center readback and the pipeline's
    // DSP input rate, so spectrum, waterfall, axis strip and VFO overlay can
    // never disagree about what frequency a pixel column shows.
    FreqScale scale_;
    enum class VfoDrag { None, Center, EdgeLow, EdgeHigh };
    VfoDrag vfoDrag_ = VfoDrag::None;
    // mouseHz - band center at grab time, so a center drag never makes the
    // band jump to put its center under the cursor.
    double vfoGrabDeltaHz_ = 0.0;
    bool wfPanning_ = false;  // horizontal waterfall click-drag in progress

    // --- Config persistence state (P5) ----------------------------------------
    std::string configPath_;       // empty = persistence disabled (hermetic)
    bool configAnnounce_ = false;  // print "config applied: ..." (test hook)

    // The source combo was open on the previous frame. The native
    // enumeration and the lazy Soapy scan run on the frame it OPENS, not on
    // every frame it stays open - the combo asks sixty times a second
    // otherwise, and one of those two answers costs a SetupAPI walk.
    bool sourceComboWasOpen_ = false;
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
    std::vector<std::string> bookmarkGroups_;
    std::vector<std::uint32_t> bookmarkView_;
    unsigned bookmarkViewVersion_ = ~0u;
    std::string bookmarkViewKey_;
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
    // (The browser's bookmark rows map to Bookmark::ids through the ids
    // published WITH them: ReceiverSnapshot::Full::bookmarkIds, engine
    // stage 2 - never to list indices, which move.)

    bool bandPlanOverlay_ = true;

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

    // --- THE INTERFACE THEME (2026-09-25; gui/theme.hpp) ----------------------
    // The look chosen in Display, by its foxsdr-ui/1 key (AppConfig::uiTheme).
    // Applied only between frames, by applyPendingTheme() - the palette, the
    // ImGui style and the typeface pair all change together, never half way
    // through a frame. Starts pending so the first frame applies whatever
    // applyConfig restored.
    std::string uiThemeKey_ = "today";
    bool themeApplyPending_ = true;
    void applyPendingTheme();
    // Pick a theme from the Display picker: the palette AND the counter's and
    // readings' preset sizes (Bench Classic XL is the 2x counter without its
    // switches), which the user may then change on their own.
    void pickTheme(const std::string& key);
    // The counter's own settings (AppConfig::counterScale / counterSwitches)
    // and every other live figure's size (readingsScale). Read every frame.
    int counterScale_ = 1;
    bool counterSwitches_ = true;
    float readingsScale_ = 1.0f;
    // The status column's height at which its cards last failed to fit at the
    // enlarged reading size (-1: never). See drawStatusColumn's card().
    float statusEnlargeFailedRoom_ = -1.0f;
    // The counter's right-click menu: Enlarge figures / Normal size / Show
    // tuner switches / Counter face / Enlarge every reading.
    void drawCounterMenu();

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
    // The VOLUME meter's needle, carried between frames so it can fall
    // gently rather than follow every syllable - see gui/volume_meter.hpp.
    float volumeNeedle_ = 0.0f;
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
    bool radarScopeOpenedByEnv_ = false;   // FOXSDR_OPEN_RADAR_SCOPE, bounded runs
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

    double rxLatInput_ = 0.0;
    double rxLonInput_ = 0.0;
    std::string gpsPort_;
    int gpsBaud_ = cascade::core::kDefaultGpsBaud;
    char gpsPortInput_[cascade::core::kMaxSerialPortNameChars + 1] = "";
    std::vector<std::string> gpsPorts_;
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
    //
    // patchOpen_ IS "THE PATCH VIEW IS SHOWING" since 0.99.40, when the page
    // became the main window's face (the owner: "display the patch panel as
    // the main"). TRUE until the config says otherwise, so a fresh install -
    // and every hermetic --frames run - opens on it; applyConfig sets it from
    // AppConfig::mainView and currentConfig writes it back.
    bool patchOpen_ = true;
    // The engine's status lines as it last handed them over (Engine::
    // statusText, OPEN 3): refreshed once a frame, after the frame's publish;
    // every panel letters from this, never from the engine's fields.
    cascade::engine::Engine::StatusText statusLines_;
    cascade::gui::patch::Interaction patchUi_;
    // THE PAGE'S DRAFT OF THE GRAPH (engine/stage3b-pre, docs/engine-stage3.md
    // OPEN 6, Design A). The graph is the Engine's (patchGraph_); the canvas,
    // the faces and the inspector draw from and edit this copy, and
    // commitPatchDraft() hands the whole of it to the Engine as one
    // FOXAPP_OP_PATCH_SET_GRAPH - after the parts bin, after the canvas and
    // faces, and after the inspector, so every edit reaches the engine in the
    // frame it was made, before patchPublishSets builds from it.
    //
    // THE THREE RULES the round-3 review's scanner findings make for any
    // draft (docs/review-harness/engine-round3-review.md):
    //   - it FOLLOWS THE ENGINE. syncPatchDraft() copies the engine's graph
    //     whenever it differs from patchDraftBase_ - the graph this draft was
    //     last in step with (core::patch::graphsEqual, no text written) - so a config load, START/ALL OFF or a centre
    //     the running radio reported shows at once; never mid-drag, resize or
    //     wire (patchInteracting()), when the drag's own node would jump;
    //   - an edit made while the engine moved on is put ON TOP of the
    //     engine's graph (core::patch::rebaseDraft), never the stale copy
    //     sent back over it;
    //   - it is NEVER A DEFAULT: until it has been copied from the engine
    //     (patchDraftInStep_) nothing is ever committed from it.
    // drawUi commits once a frame as well, so an edit is never left behind
    // by a view that stopped drawing.
    cascade::core::patch::Graph patchDraft_;
    cascade::core::patch::Graph patchDraftBase_;
    std::uint64_t patchDraftEpoch_ = 0;
    bool patchDraftInStep_ = false;
    // True while the button is held on a drag, a resize or a wire; a state
    // left behind by a release the canvas never saw is cleared here.
    bool patchInteracting();
    void syncPatchDraft();
    void adoptPatchGraph();
    void commitPatchDraft();
    // A NEW DOCUMENT for the engine (the config's patch, a patch file): sent
    // flagged FOXAPP_PATCH_GRAPH_DOCUMENT, and the draft starts again from
    // what the engine then holds.
    bool loadPatchDocument(const cascade::core::patch::Graph& g);
    bool patchSeeded_ = false;
    bool patchOpenedByEnv_ = false;
    bool patchStartedByEnv_ = false;
    bool sourceDeviceByEnv_ = false;   // FOXSDR_SOURCE_DEVICE, bounded runs only
    bool patchFileLoaded_ = false;
    // The patch as core/patch_io.hpp writes it, rebuilt only when the
    // canvas says it changed. currentConfig() runs every frame and must
    // not serialise a document on each one.
    std::string patchText_;
    // What each planned channel is hearing this frame. Rebuilt per frame
    // rather than kept, because a stale level is worse than none: it reads
    // as a live measurement of a channel that may no longer exist.
    std::vector<cascade::gui::patch::NodeReading> patchReadings_;
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
    // How many parts have been dropped from the bin, used only to
    // stagger the next one so a run of clicks does not stack every
    // node on the same spot.
    int nodesPlaced_ = 0;

    // Why a centre typed on a Radio node was refused (setPatchRadioCentre);
    // cleared by the next one taken.
    std::map<cascade::core::patch::NodeId, std::string> patchCentreNote_;
    // The device a newly added Radio starts on: the receiver's own radio if
    // no other Radio has it, else the first free device listed, else the
    // generator.
    std::string patchDefaultDeviceKey() const;
    // A Radio PART dropped from the bin (engine/stage3b-pre B1, 2026-09-28):
    // starts on a free native radio, so it needs the native list first, at
    // once - the node is created and given a device on the same call, and a
    // QUEUED scan would still be sitting in the command queue when
    // patchDefaultDeviceKey() below reads it, saving the generator instead.
    cascade::core::patch::NodeId patchAddRadioPart(const std::string& label,
                                                    cascade::core::patch::PortType feed, float x,
                                                    float y);
    // A centre typed on a Radio node (its face or the panel): taken when the
    // radio behind the node's converter would be told something above 0 Hz
    // (core::radioCentreTakeable), refused with a sentence otherwise
    // (patchCentreNote_, drawn by drawPatchCentreNote). True when taken.
    bool setPatchRadioCentre(cascade::core::patch::Node& n, double airHz);
    void drawPatchCentreNote(const cascade::core::patch::Node& n);
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

    bool transmitPttHeld_ = false; // this frame's key request, rebuilt each frame
    // Every LATCH press ever made, COUNTED - never a per-frame flag, so the
    // engine's control side neither loses nor doubles one however its pumps
    // fall against these frames (gui::TxPageRequest) - and this window's frame
    // number for the request it hands over.
    std::uint32_t transmitLatchPresses_ = 0;
    std::uint64_t transmitKeyFrame_ = 0;
    bool transmitPageLive_ = false;  // page drawn with its controls THIS frame (not rolled up)
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

    // (A preset-bar key press used to be recorded into a pendingPresetRequest_
    // member and applied at a mid-frame safe point, because applyPluginPreset
    // rebuilds the lists drawPluginWindows walks. Since stage 1 a press
    // submits a command, and the command drain at the TOP of the next frame -
    // before any list is walked - is that safe point for every control.)

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

    // The store's one automatic catalogue read per session (gui/store_first_open.hpp).
    cascade::gui::StoreFirstOpen storeFirstOpen_;

    // ONE PLACE BUILDS THE STORE'S MODEL, and it has to be one place now that
    // something other than the draw needs it: startAddAll re-plans from live
    // state, and a second transcription of catalogue row into StoreModule
    // would be a second set of rules about what "fitted" and "blocked" mean.
    void buildPluginStoreModel(PluginStoreModel& model);
    // What the store window is told about the run in flight. Empty when none
    // is.
    std::string addAllProgressLine() const;

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
    // --- measurement (tools/measure_engine.ps1) ------------------------------
    // All three null in every ordinary run; each is created only by its debug
    // switch. FOXSDR_MEASURE (measure_) is honoured in a bounded --frames run
    // ONLY. FOXSDR_FRAME_LOG (frameLog_), FOXSDR_FRAME_CAP_HZ (frameCap_) and
    // FOXSDR_VSYNC_OFF (the swap interval, read where the window is made) are
    // read in EVERY run, interactive ones included - an environment switch,
    // never a setting, and there is no other way on. See gui/frame_log.hpp
    // and core/engine_measure.hpp.
    std::unique_ptr<FrameLog> frameLog_;
    std::unique_ptr<FrameCap> frameCap_;  // FOXSDR_FRAME_CAP_HZ
    std::unique_ptr<cascade::core::EngineMeasure> measure_;
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
    // THE SAME APPLIES TO EVERY OTHER MEMBER THE WORKER TOUCHES, which is why
    // the two result slots below moved up here from after the future. The
    // atomics are only what the worker READS; these are what it WRITES.
    // downloadUpdate() is handed updateResultPath_ and updateResultError_ by
    // reference and assigns to them as it goes - so declared after the future
    // they were std::string destructors running while a worker thread was
    // mid-assignment into them, a use-after-free reached through a dangling
    // `this` rather than a data race the flags could stop. The blocking future
    // destructor is also the only thing that makes this ordering enough: it
    // guarantees the worker has returned before anything declared above the
    // future is destroyed.
    //
    // THE CHECK IS NOT IN THAT ARRANGEMENT ANY MORE. It used to write
    // updateResult_ the same way, which is what forced ~AppWindow to wait for
    // it - and a check stalled in WinHTTP has no flag to poll. updateCheck_
    // returns its outcome by value and captures nothing of this window, so
    // ~AppWindow can abandon it (core::UpdateCheckTask).
    std::atomic<float> updateProgress_{0.0f};
    std::atomic<bool> updateCancel_{false};
    std::string updateResultError_;
    std::string updateResultPath_;
    cascade::core::UpdateCheckTask updateCheck_;
    std::future<bool> updateDownloadFuture_;

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
    // thread publishes one consistent snapshot per frame (receiverSnapshot_,
    // engine stage 2), and the providers do nothing but read it - without a
    // lock the GUI thread ever waits for.
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
    // What the web server's and CAT's providers answer, from the snapshot:
    // on their own threads, never touching the pipeline or this window's
    // members. The web one takes the whole block (state and lists of one
    // publish); CAT reads only figures, lock-free.
    cascade::net::RadioStatus webStatusNow() const;
    cascade::net::RadioStatus catStatusNow() const;
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

    // The spectrum frame for the browser (the status is receiverSnapshot_).
    mutable std::mutex webMutex_;
    std::vector<float> webBins_;
    std::uint64_t webSeq_ = 0;
    double webSnapCenterHz_ = 0.0;
    double webSnapSpanHz_ = 0.0;

    // The pointer ledger (see the frame loop), switched on from the
    // Diagnostics section so a tester can read it without setting an
    // environment variable. Not saved: it is a measurement, not a setting.
    bool inputLedger_ = false;
};

}  // namespace cascade::gui
