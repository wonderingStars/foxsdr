// GLFW + Dear ImGui application shell.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <functional>
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

#include "core/engine_vocab.hpp"
#include "core/pass_part.hpp"
#include "core/plugin_ui_port.hpp"
#include "core/receiver_tables.hpp"
#include "core/status_text.hpp"
#include "core/app_commands.hpp"
#include "core/band_plan.hpp"
#include "core/config.hpp"
#include "core/diag_history.hpp"
#include "core/freq_import.hpp"
#include "core/freq_markers.hpp"
#include "core/file_pick.hpp"
#include "core/radio_setup.hpp"
#include "core/bookmark.hpp"
#include "core/i18n.hpp"
#include "core/plugin_host_data.hpp"
#include "core/patch_blueprints.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_presets.hpp"
#include "core/scanner_params.hpp"
#include "core/serial_port.hpp"
#include "core/tx_input.hpp"
#include "dsp/modulator.hpp"
#include "core/spectrum_frame.hpp"
#include "core/freq_converter.hpp"
#include "core/patch_device_label.hpp"
#include "core/patch_plan.hpp"
#include "gui/meter_styles_math.hpp"
#include "gui/patch_view_math.hpp"
#include "gui/patch_scope_math.hpp"
#include "gui/input_script.hpp"
#include "core/plugin_ui_data.hpp"
#include "core/plugin_ui_port.hpp"
#include "core/plugin_repo_data.hpp"
#include "core/updater.hpp"
#include "core/store_update.hpp"
#include "core/pasted_report.hpp"
#include "core/utf8_text.hpp"
#include "core/bounded_hooks.hpp"
#include "gui/frame_log.hpp"
#include "gui/airband_window_state.hpp"
#include "gui/deck_parts.hpp"
#include "gui/basemap_cache.hpp"
// The floor a torn-off page cannot be dragged under, and the reset generation
// that puts an already-wrong one back. ImGui-free for the same reason as the
// headers below it - the tests include it without a graphics context.
#include "gui/page_geometry.hpp"
#include "gui/store_first_open.hpp"
#include "gui/rail_banks.hpp"
#include "gui/rail_reads.hpp"
#include "gui/section_reads.hpp"
#include "gui/tuning_reads.hpp"
#include "gui/decoder_reads.hpp"
#include "gui/plugin_settings_form.hpp"
#include "gui/scope_gain.hpp"
#include "gui/tune_local.hpp"
#include "core/running_view.hpp"
#include "gui/bench_rail.hpp"
#include "core/audio_open.hpp"
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
#include "core/tune_control.hpp"
#include "source/soundcard_source.hpp"
// THE SDRPLAY API SERVICE CARD and THE SDRPLAY DIAGNOSTIC (stub campaign B18): the service's state and its restart worker, the driver's trouble record, and the probe's child process.
#include "core/frame_timing.hpp"
#include "source/sdrplay_probe.hpp"
#include "source/sdrplay_service.hpp"
#include "source/sdrplay_source.hpp"
#include "core/device_scan_plan.hpp"
#include "core/source_fallback.hpp"
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
#include "core/tester_usage.hpp"
#include "core/tester_link.hpp"
#include "gui/link_request_poll.hpp"
#include "core/feature_request.hpp"
#include "core/problem_report.hpp"
#include "core/hang_watchdog.hpp"
#include "gui/freq_scale.hpp"
// THE SPECTRUM'S TRACE MODES, THE DISPLAY RANGE AND THE TIP THAT SAYS THEY ARE THERE (stub campaign B11): all three header-only and free of ImGui, so held
// by value below like the scale is.
#include "core/display_range.hpp"
#include "core/trace_hold.hpp"
#include "gui/hover_dwell.hpp"
// The settings panels' port range, default ports and the two address classifiers - as data (stage 5b): the interface includes nothing of
// net/ for them any more (the servers are the engine's).
#include "core/server_ports.hpp"
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
#include "core/bias_tee.hpp"

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
struct FittedListCache;
struct StoreListCache;
struct ImportJobs;   // the import lookup's worker (app_window_import_dialog.cpp)
struct DiskWorkState;   // the window's own disk workers: a picture's save and the screenshot's files (app_window_disk_work.cpp)

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
// `s` is the interface-scale factor (gui/ui_scale.hpp), defaulted to 1.0f so
// every existing call — including every test that pins this against the base
// font sizes — is unchanged. A real draw call site passes labelPx already at
// its scaled size (fonts::uiPx() and friends) AND the same s, so the padding
// around the label grows with the label rather than staying a fixed few
// pixels beside increasingly large type.
inline float railRowHeight(float labelPx, float s = 1.0f) {
    const float fromType = labelPx + 2.0f * kRailRowPadY * s;
    const float floorH = kRailRowMinH * s;
    return fromType > floorH ? fromType : floorH;
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
// `scale` is the interface-scale factor; defaulted to 1.0f so every call
// unaware of it - including every existing test - is unchanged. The local
// `sized` shadows the parameter name `rowH - 10.0f` used to hold so the
// keyMin/keyMax comparison below reads exactly as it always did, just against
// the scaled floor and ceiling.
inline float railKeySize(float rowH, float scale = 1.0f) {
    const float sized = rowH - 10.0f * scale;
    const float lo = kRailKeyMin * scale;
    const float hi = kRailKeyMax * scale;
    if (sized < lo) { return lo; }
    if (sized > hi) { return hi; }
    return sized;
}
inline float railPlateLeft(float rowLeft, float rowH, float scale = 1.0f) {
    return rowLeft + kRailKeyInset * scale + railKeySize(rowH, scale) + kRailKeyGap * scale;
}
inline float railLabelLeft(float rowLeft, float rowH, float scale = 1.0f) {
    return railPlateLeft(rowLeft, rowH, scale) + kRailLabelPadX * scale;
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

inline float railLampRadius(float rowH, float scale = 1.0f) {
    const float r = rowH * kRailLampRadiusShare;
    const float floorR = kRailLampRadiusMin * scale;
    return r > floorR ? r : floorR;
}
// `chipTextWidth` < 0 means the row carries a lamp but no chip; the row's
// callers here always pass one, and drawRailChip draws the lamp either way.
// `chipTextWidth` itself is a measurement (already at the drawn, scaled font
// size at a real call site) and is never multiplied by `scale` here.
inline float railChipReserve(float rowH, float chipTextWidth, float scale = 1.0f) {
    const float lampR = railLampRadius(rowH, scale);
    const float toLampLeft = 2.0f * lampR + kRailLampEdgeGap * scale;
    if (!(chipTextWidth >= 0.0f)) { return toLampLeft; }
    return 2.0f * lampR + kRailChipLampGap * scale + chipTextWidth + 2.0f * kRailChipPadX * scale +
           kRailLampEdgeGap * scale;
}

// The x a row's label must not cross. `chipTextWidth` < 0 for a row with no
// chip at all, in which case only the plate's own padding is kept back.
inline float railLabelRight(float rowRight, float rowH, float chipTextWidth, float scale = 1.0f) {
    const float plateEdge = rowRight - kRailLabelPadX * scale;
    const float beforeChip =
        rowRight - railChipReserve(rowH, chipTextWidth, scale) - kRailLabelChipGap * scale;
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

// --- THE RAIL FOLDS AWAY (0.99.49 beta feedback) -----------------------------
//
// On a small laptop the FUNCTION SELECT column is 384 of 1366 pixels, and
// the patch page and the spectrum beside it are what the user is working in.
// A "<<" key at the right of the plate's title row folds the whole column to
// a strip holding a ">>" key that opens it again; the main area - the patch
// page or the receiver's spectrum - takes the width that frees. Remembered in
// the config (AppConfig::railCollapsed). Every size here is logical pixels,
// scaled by the caller with the interface size, like kMenuWidth itself.
inline constexpr float kRailFoldedW = 34.0f;   // the strip the column folds to
inline constexpr float kRailFoldKeyW = 26.0f;
inline constexpr float kRailFoldKeyH = 20.0f;
inline constexpr float kRailFoldKeyTop = 6.0f;  // from the plate's top edge

// The left column's width, open or folded.
inline float railColumnWidth(bool collapsed) { return collapsed ? kRailFoldedW : kMenuWidth; }

// Where the fold key sits, in screen pixels, for a column at colX, colY
// colW wide at interface size s: open, at the right end of the title row,
// inside the plate's inset and above the rule under the title; folded,
// centred on the strip at the same height.
struct RailFoldKeyRect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};
inline RailFoldKeyRect railFoldKeyRect(float colX, float colY, float colW, bool collapsed, float s) {
    const float w = kRailFoldKeyW * s;
    const float h = kRailFoldKeyH * s;
    const float y0 = colY + kRailFoldKeyTop * s;
    const float x0 = collapsed ? colX + (colW - w) * 0.5f : colX + colW - kRailPlatePad * s - w;
    return RailFoldKeyRect{x0, y0, x0 + w, y0 + h};
}

// --- THE STATUS COLUMN FOLDS AWAY TOO (0.99.74 beta feedback) -----------------
//
// The tester, 2026-10-12: "Just like you added the << and >> to expand and
// collapse the Function Select panel on the left side, can you do this for
// the right side Status panel? I am just looking at it from the perspective
// if that information is useful to the end user while they are looking at the
// spectrum or whatever." The same key, mirrored: ">>" at the LEFT end of the
// STATUS plate's title row (the end nearest the spectrum, as the rail's "<<"
// sits at its right end) pushes the column away to a strip; "<<" on the strip
// brings it back. Remembered in the config (AppConfig::statusCollapsed).
// Logical pixels, scaled by the caller with the interface size.
inline constexpr float kStatusW = 230.0f;        // the open column (was a local in drawUi)
inline constexpr float kStatusFoldedW = 34.0f;   // the strip it folds to, the rail's width

// The right column's width, open or folded.
inline float statusColumnWidth(bool folded) { return folded ? kStatusFoldedW : kStatusW; }

// Where the status fold key sits, in screen pixels, for a column at colX, colY
// colW wide at interface size s: the rail's key (same size, same height, same
// inset), mirrored - open, at the LEFT end of the title row inside the plate's
// inset; folded, centred on the strip.
inline RailFoldKeyRect statusFoldKeyRect(float colX, float colY, float colW, bool folded, float s) {
    const float w = kRailFoldKeyW * s;
    const float h = kRailFoldKeyH * s;
    const float y0 = colY + kRailFoldKeyTop * s;
    const float x0 = folded ? colX + (colW - w) * 0.5f : colX + kRailPlatePad * s;
    return RailFoldKeyRect{x0, y0, x0 + w, y0 + h};
}

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
// `colW` arrives already scaled (a real call site passes uiscale::px(kMenuWidth));
// `scale` is what widens the inset and gap between the five keys with it, so
// the keys do not crowd together as the column they share grows.
inline float bankKeyWidth(float colW, int count, float scale = 1.0f) {
    return (colW - 2.0f * kBankKeyInset * scale - kBankKeyGap * scale * static_cast<float>(count - 1)) /
           static_cast<float>(count);
}
// The brass kept clear each side of the word: kBankKeyWordPadX, or - only for
// a word that does not fit even at the floor with that much - one pixel.
// "РАСШИРЕНИЯ" (ru) and "РОЗШИРЕННЯ" (uk) are 66.8 px at nine pixels in a
// 64.4 px room; two more pixels of metal each side hold them whole, and a word
// a pixel from the bevel is a tight key where a cut one is a broken key.
// `wordWAtFloor` is the word's width at kBankKeyWordFloorPx * scale.
inline float bankKeyWordPadX(float wordWAtFloor, float keyW, float scale = 1.0f) {
    return wordWAtFloor + 2.0f * kBankKeyWordPadX * scale <= keyW + 0.5f
              ? kBankKeyWordPadX * scale
              : kBankKeyWordPadMinX * scale;
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
// together. separateWindowAnchor anchors the map at the main window's top
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

// WHAT THE HOST PROCESS HANDS THE WINDOW (shell retirement R7, 2026-10-28; docs/engine-stage4.md 6.25). The window no longer builds the engine: the
// host - main.cpp, or the test host - builds an EngineService, takes the settings lock, restores the settings into the engine, and constructs the
// window on the restored engine. The window is a client of it from its first line: it opens its own session on the table the host passes and
// reads its half of the settings back from the table (fact.config.interface).
struct WindowHost {
    // THE TABLE AND THE HANDLE of the engine the window's LOCAL session opens on (engineApi(...) and Engine::apiHandle(), named by the host: the
    // window cannot include either).
    const FoxEngineApi* api = nullptr;
    FoxEngine* handle = nullptr;
    // THE BOUNDED RUN'S TWO HOOKS (shell retirement R9; core/bounded_hooks.hpp): the stepping of the engine's pass and the end of each frame, installed
    // by the host in a `--frames` run and left EMPTY in a product run (a free-running pass has nothing to step, and the window then does nothing there).
    // Built by engine/host_measure.hpp's makeBoundedHooks, which the window cannot include.
    cascade::core::BoundedHooks bounded;
    // THE SETTINGS FILE the host restored from; empty is hermetic - nothing read, written or kept. The window still owns the KEEPER until R8 (it
    // saves to this path, debounced, and at the end), so it needs the path and is told how the restore went.
    std::string configPath;
    bool configLoaded = false;       // the file was read (a missing file is a first run: loaded); false - damaged, or hermetic: nothing to apply
    bool lastRunUnclean = false;     // the file said the last run never wrote its clean-exit marker (the window's crash-report offer)
    std::string sourceKind;          // the saved source, as the file named it: the I/Q path box starts from `iqFilePath` when this is "file"
    std::string iqFilePath;
};

// Owns the GLFW window, the ImGui context and the top-level panel layout.
// All GLFW/ImGui usage stays behind this interface so main() (and any future
// headless harness) never needs GUI headers.
class AppWindow {
public:
    // The window's start-up on an engine the host has built and restored: its API session, the plugin UI's port (a late attach - the scan has
    // run), its half of the settings from the table, and the bounded run's two hooks when the host installed them. Nothing here builds,
    // starts or restores the engine - that is EngineService's (engine/engine_service.hpp) - and nothing reads the settings file. An EMPTY
    // `configPath` is the hermetic mode the --frames/--selftest CI contract requires: nothing is read or written. The CASCADE_CONFIG_TEST line
    // ("config applied: ...") is the host's to print (EngineService::announce).
    explicit AppWindow(const WindowHost& host);

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
    //
    // SINCE SHELL RETIREMENT R8 THE ENGINE IS NOT THIS WINDOW'S TO START OR STOP: the host (main.cpp, or the test host's runWindow) starts the
    // engine's service before run() and stops it after, then calls closeWindow(). run() is the window's own life: the GL window and the frame
    // loop, and - when the loop ends - the window's part of the shutdown, which has to be done while the engine is still running: the crash-report
    // sweep and the senders stopped, the last half of the settings handed to the engine and acknowledged (so the engine's final save holds it),
    // and the plugin UI released and disconnected on this thread with the GL context current. Then it returns.
    int run(int frames = -1);

    // The second half of the window's end, after the host has STOPPED the engine: the GL textures, ImGui and GLFW torn down, the hang watchdog
    // stopped LAST (so a wedge anywhere in the engine's stop is reported against the shutdown budget), and the lines a clean end prints ("teardown N
    // ms", "rendered N frames", ...). A no-op when run() never made a window (it failed, or handed the run to the software renderer). Idempotent.
    void closeWindow();

    // True when run() returned because the graphics driver could not be used
    // and this run is to be handed to a second process on the software
    // renderer (gui/software_gl.hpp): main() does that once this object is
    // gone, and ends with the second process's exit code.
    bool relaunchWithSoftwareGl() const { return relaunchWithSoftwareGl_; }

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
    // --link-tester (main.cpp, via the process boot's parse): reveals SYSTEM > Beta tester's paste box for this session even with no app token held
    // yet. See the private testerLinkReveal_ field's own comment (stub campaign B15: the flow is the window's again).
    void setLinkTesterReveal(bool reveal) { testerLinkReveal_ = reveal; }

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
    // THE TEST HOST'S KEY (shell retirement R1; tests/test_host.hpp): the window-side seams a test needs and a table call cannot give - the plugin UI port
    // (ui_), the late attach, and the window's half of a start-up state. Defined in the tests only; no src/ file names it. (It reached the Engine behind the
    // window until shell retirement R10, 2026-10-29; the window holds no engine of any kind now, and th::engine(window) finds the Host's.)
    friend struct TestHostAccess;
    // THE ENGINE IS NOT HERE. It is the HOST's (shell retirement R7: an EngineService, built by main.cpp or the test host and restored before this
    // window exists); the window is a client of it through the API table and nothing else (the port and the two bounded hooks below are the two
    // bounded exceptions), and the interface target cannot include engine.hpp at all. (The window held a non-owning VIEW of it, an EngineShell, until
    // shell retirement R10, 2026-10-29.)
    // THE PLUGIN UI IS THIS WINDOW'S (the pump move's step P3): the track sources, panels and instruments polled in the frame
    // (hostPollPlugins) and drawn all frame long, built over the API core the ENGINE owns and held behind the PLUGIN UI PORT
    // (core/plugin_ui_port.hpp - the window names neither the PluginUi nor the directive link), which it reaches through the table's one
    // in-process extension, get_local_extension(FOXAPI_EXT_PLUGIN_UI) on its LOCAL session (attachPluginUi; shell retirement R4). Only this
    // thread touches it. The engine does not rebuild or clear it: it posts a directive and this window applies the newest at the head of its
    // frame (applyPluginDirective), which is also its acknowledgement of an unload - see Engine::detachAndUnloadPlugins. Declared right after
    // the engine so the engine is built first and outlives it: the port's destructor ends the engine's use of this window's applier.
    std::unique_ptr<cascade::core::PluginUiPort> ui_;
    // The attach (the constructor's, and a test's: a window that arrives after the engine's first scan is replayed the plugin UI's state).
    void attachPluginUi();
    // The frame's hand-over (handFrontEndFacts): one object, refilled each frame and passed by reference - the engine's side copies what it keeps.
    cascade::core::PluginUiFrontEnd frontEnd_;

    // --- WHAT THE ENGINE ONCE ASKED OF THIS WINDOW (the shell's ShellHooks, until shell retirement R9) ---------
    // Nothing: the engine asks its front end for no answer. What is left is the window's own clock, the usage report's session timer. (The pump move's
    // step P2 took seven hooks out of this list - onDisplayRange, onBookmarksChanged,
    // onConverterChanged, drainTrackInfoText, enrichWebTrack, fillWebImages and the window's onCatalogueResult - and turned
    // openPluginWindowsFor into the window's own answer to an event: each wrote or read state the window owns from the engine's
    // pass, which a pass on a thread of its own must not.)
    // (The pump move's step P4 took the frame clock out: frameClockRunning and frameTimeS, and the caches that made them answerable from
    // the pass's thread. The engine runs on its own steady clock, Engine::clockS().)
    // The wall-clock the window's own usage report (the beta tester's) measures a session with (glfwGetTime()).
    double wallTimeS() const;
    // THE BOUNDED RUN'S STEP (shell retirement R9): the host's hook run under the window's own WatchdogPause - the frame is BLOCKED in the wait for the
    // engine's pass part, not hung, and a part that really hangs is the control thread's own watchdog's to report (it was Engine::setFrontEndWaitBeat,
    // the window's heartbeat, until R9). Does nothing in a product run, whose pass free-runs. The three calls of drawUi's look are these.
    void stepPass(cascade::core::PassPart part);
    // THE END OF A FRAME in a bounded run: the host's hook (the measurement's step, the frame the catalogue report names). It is not under a pause of its
    // own (it runs every frame); the host brackets the one wait it makes with boundedClient_.blocked. False when the measurement has finished and the
    // window is to close. True, and nothing done, when the host installed none.
    bool endOfFrameHook(int nextFrame);
    // The hooks the host installed (empty in a product run), and what only this window can do for a measurement, built once when there are some.
    cascade::core::BoundedHooks bounded_;
    cascade::core::BoundedClient boundedClient_;
    // (The pump move's step P3 took the last five hooks out - beforePluginRescan, onPluginsUnloading, attachBasemap, attachTrackInfo
    // and showDemonstrationInstrument: the plugin UI is this window's, so the engine POSTS a directive and this window applies it.)
    // Applies the newest plugin directive, if one is waiting, on this thread: at the head of the frame and at the head of
    // hostPollPlugins (both before any reference into the plugin UI is taken); the engine runs it too, inline, where it is
    // running on this thread (the feed installs the applier).
    void applyPluginDirective();
    // What one directive does HERE, in the two steps the feed runs around the plugin UI's own rebuild: BEFORE it, a release folds the map
    // pages' geometry into the saved list while the instances still stand; AFTER it, the basemap and the track-info client are attached or
    // let go (every GL delete this causes is on this thread, with the context current), and a demonstration instrument's window shown.
    void applyDirectiveBefore(const cascade::core::PluginDirectiveFacts& d);
    void applyDirectiveAfter(const cascade::core::PluginDirectiveFacts& d);
    // The scope `plugins-reload` for as long as the window is applying a directive that rebuilds or releases the plugin UI - open in applyDirectiveBefore, closed when
    // applyDirectiveAfter leaves (stub campaign B18; core/frame_timing.hpp). The engine's own part of a reload is its guard's (OwnedFrameScope).
    std::optional<cascade::core::OwnedFrameScope> reloadScope_;
    // The publish-time facts (engine/stage3b-pre OPEN 4): no longer
    // EngineHost hooks the engine calls while publishing, but this window's
    // answers, HANDED OVER once a frame (handFrontEndFacts) - and, since the
    // pump move's step P2, the track-info plugin's answers for the web status
    // and the track list (fillTrackFeed; the receiver plugins' pictures are the engine's own since stage 5e), the bounded run's report switch
    // and the frame count it names. (Whether the web server listens is not one of them any more: the server is the engine's, and the
    // state this window reads carries the bit - webListening.)
    bool webListening() const;
    std::string tunerDisplayStyle() const;
    cascade::core::BasemapFacts basemapFacts() const;
    // `withPluginStatus`: the one hand-over at the end of a bounded run that asked for CASCADE_PLUGIN_STATUS also carries what the plugin poll and the
    // picture buffer hold, for the engine to print (PluginUiFrontEnd::pluginStatusReport).
    void handFrontEndFacts(bool withPluginStatus = false);
    // The track list the web page shows, into `out` (cleared first); false when there is none to show.
    bool fillTrackFeed(cascade::core::TrackFeed& out);
    // The track-info plugin's status lines, polled on THIS thread (the cache is GUI-thread state) and sent to the engine, one
    // FOXAPP_OP_TRACK_INFO_LINE each - at the head of the frame, so they land in the pass that frame's drain applies.
    void sendTrackInfoText();
    // THE WINDOW'S HALVES OF TWO ENGINE FUNCTIONS THAT WERE ONE. The usage report is the engine's; the crash
    // report offer, the crash-loop limiter's memory and the update and
    // diagnostics switches that were read and written beside it are this
    // window's, and run beside the engine's half exactly where it runs. (The browser's spectrum frame was copied here once a frame until
    // stage 5e; the page reads it from the table on its own session now, so a window that is not framing no longer freezes it.)
    void diagnosticsStartup(const cascade::core::AppConfig& cfg);
    void diagnosticsJournal(cascade::core::AppConfig& cfg, bool bookkeeping = true);
    // The engine's constants under the names this window's code has always used them by. (The engine's nested types the window once
    // aliased here - LocalCommand, DeviceOpenResult, SoundCardOpenResult, ConverterHeldAir, PatchRadioOpen, PluginNotice,
    // PendingUserPresetEdit, CatalogFetchResult, PluginInstallResult, AddAllRun - had no user left in src/gui; the fence cannot see
    // them and the aliases are gone with it.)
    static constexpr auto kSoundCardRow = cascade::core::kSoundCardRow;
    static constexpr auto kNativeRowBase = cascade::core::kNativeRowBase;
    static constexpr auto kDecoderLogMax = cascade::core::kDecoderLogMax;

    // One plugin's map page — declared ahead of the drawing methods that take
    // it, defined in full beside the map state below.
    struct MapPage;

    void drawUi();
    void drawToolbar();
    // The right-hand column: what the receiver is doing, gathered from where it
    // was already scattered. Drawn only when the window is wide enough that
    // taking 230 px from the spectrum is not a bad trade.
    void drawStatusColumn();
    // The folded status column: a plate the width of kStatusFoldedW with the
    // "<<" key that opens it, and nothing else (0.99.74). In
    // app_window_chrome.cpp, with the rest of the window's fold and size chrome.
    void drawStatusStrip();
    // The ">>" / "<<" key (statusFoldKeyRect), for the open column and the
    // strip.
    void drawStatusFoldKey(float colX, float colY, float colW);
    // What the key does, as one function so a test can press the real thing.
    void toggleStatusFold();
    // The status column is folded to its strip (AppConfig::statusCollapsed).
    bool statusCollapsed_ = false;
    // The tuned-frequency counter, drawn into the geometry the top bar hands
    // it: the screen position of the well's top-left corner, and the scale the
    // bar is drawn at. Plain floats rather than an ImVec2 because this header
    // is deliberately free of ImGui types - see the forward declarations
    // above.
    void drawFrequencyReadout(float wellX, float wellY, float scale);
    void drawMenuColumn();
    // The folded rail: a plate the width of kRailFoldedW with the ">>" key
    // that opens it, and nothing else (0.99.49). In app_window_chrome.cpp.
    void drawRailStrip();
    // The "<<" / ">>" key (railFoldKeyRect), for the open rail and the strip.
    void drawRailFoldKey(float colX, float colY, float colW);
    // The rail is folded to its strip (AppConfig::railCollapsed).
    bool railCollapsed_ = false;
    // The Display section (first in the SYSTEM bank) is open
    // (AppConfig::displayOpen). Folded on a fresh install; drawDisplaySection
    // starts the header from it and writes the header's answer back.
    bool displayOpen_ = false;
    // The update banner, and the work behind it. Drawn at the top of the menu
    // column because a build that cannot see the user's radio is the most
    // useful thing this application can say to them, and it is worth more than
    // whatever they opened the panel for.
    void drawUpdatesSection();
    void drawUpdateBanner();
    // A MESSAGE FROM THE TEAM (0.99.79; stub campaign B14, src/gui/app_window_updates.cpp): a card above the update banner for each message the
    // update service carries that this copy has not marked Seen - whether or not there is an update. Not drawn when the Updates tick is off, or for a
    // Microsoft Store copy (which asks the Store, not foxsdr.com, and so never hears one).
    void drawTeamMessages();
    // Has the Seen key been pressed for the message with this id (messagesSeen_).
    bool teamMessageSeen(const std::string& id) const;
    // A PLUGIN LEFT OUT OF THIS START (plugin containment P1, docs/engine-stage4.md 6.28; src/gui/app_window_updates.cpp): a card above the team's
    // messages for each plugin the crash-loop breaker held off at this start - the last run ended with it in play - with the one key that turns it
    // back on (RE-ENABLE: FOXAPP_OP_PLUGIN_REENABLE, a local op) and a Seen key that puts the card away for this session. Read from
    // fact.plugins.fault.notices when the lists move; not persisted (the engine says it once, at the start it acted on).
    void drawPluginFaultNotices();
    struct FaultNotice {
        std::string id;        // the plugin's id (what RE-ENABLE takes)
        std::string version;
        unsigned strikes = 0;
        bool permanent = false;  // strike two: off until re-enabled
        std::string phase;
    };
    std::vector<FaultNotice> faultNotices_;
    std::uint64_t faultNoticesSeq_ = 0;
    bool faultNoticesRead_ = false;
    std::vector<std::string> faultNoticesSeen_;
    void startUpdateCheck();
    void startUpdateDownload();
    void pollUpdateAsync();
    // A copy from the Microsoft Store (0.99.53, restored in stub campaign B14): the same banner and section, asking the Store instead of foxsdr.com -
    // see core/store_update.hpp.
    void drawStoreUpdateBanner();
    void startStoreInstall();

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
    // When this window first saw each take's start still opening its file (ImGui's clock; -1 none out): the section's "Waiting for the disk" line counts from it
    // (stub campaign B18). The engine's start has its own clock, which is not on the table; a second either way is what the line is for.
    double recordStartSeenS_[2] = {-1.0, -1.0};
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
    //
    // `heard` (0.99.75) is the same plugin's targets HEARD WITHOUT A POSITION
    // (PluginUi::unplacedTracks, filtered the same way): listed after every placed
    // row, dimmed, with their altitude and the words "no position yet", and
    // counted in the heading. They are never drawn on the map and never fly it
    // anywhere - there is nowhere to fly to. (app_window_map.cpp.)
    void drawTrackList(MapPage& page, const std::vector<cascade::core::HostTrack>& tracks,
                       const std::vector<cascade::core::HostTrack>& heard);
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
    const cascade::core::HostTrack* findVisibleTrack(const std::string& id);

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
    //
    // FROM THE WINDOW'S ENGINE API SESSION ONLY (window port group 12, API 0.3 section 8.11), the aircraft apart:
    // the receiver's position (the state's FOXAPI_RX_POSITION_SET, rxLatDeg and rxLonDeg), the SIG gauge and the
    // POWER lamp (signalDb and FOXAPI_RX_RUNNING), the GAIN knob (the first GAINS entry through sourceLists(), the
    // device-open and AGC flags, scopeGainLocal_ for the figure it has sent and the engine has not yet shown), the
    // module census behind "the scope will be empty" (pluginLists()), and POWER and the knob's detents as
    // FOXAPI_OP_RUN and FOXAPP_OP_SET_GAIN_NO_READBACK through uiSubmit. The aircraft are the HOST PROCESS's:
    // the plugin windows' own poll fills them from the frame, as the plugin ABI says (hostTracks()).
    void drawScopeMode();
    // The track source's output, the one thing the radar scope reads of the host process: what the loaded
    // track-source modules published the last time drawPluginWindows polled them, in this frame. NOT ported and
    // not meant to be (plugin_abi.h: a track source is polled from the GUI thread at frame rate), and the one
    // place the radar scope names the engine: the guard counts it as a read and does not list it.
    const std::vector<cascade::core::HostTrack>& hostTracks();
    // ...and the targets the same modules HEAR AND CANNOT PLACE YET (0.99.75), the feed's ninth read (stub campaign B16; END stage 7h, with
    // the feed). Kept apart from hostTracks() on purpose: every reader of that one draws or measures from a position, and these have none
    // (NaN). Read by drawPluginWindows' per-page filter alone, for the target list and its heading; the map, the satellite panel, the fit,
    // the coverage and the web page never see them.
    const std::vector<cascade::core::HostTrack>& hostUnplacedTracks();
    // ...and the paths (a track's trail) the same modules published: the Patch page's Map part draws both (window port
    // group 13). HOST-PROCESS for the same reason hostTracks() is, and the other place the guard counts as a read and
    // does not list.
    const std::vector<cascade::core::HostPath>& hostPaths();
    // ...and the rest of what the map pages and the plugin rows take of the plugin UI (window port group 14, part a): the
    // altitude a track had where its trail runs (the map view's lookup), the panels and the instruments the rail's rows
    // list. HOST-PROCESS like the two above - the plugin UI is polled from this thread at frame rate - and counted by the
    // guard as one read each, none of them listed as ported.
    bool hostAltitudeNear(const std::string& plugin, const std::string& id, double latDeg, double lonDeg,
                          double& outAltM);
    const std::vector<cascade::core::HostPanel>& hostPanels();
    const std::vector<cascade::core::HostInstrument>& hostInstruments();
    // ...and the three the plugin windows themselves take (window port group 14, part d): the names of the modules that hold a
    // track-source instance (the map pages are one per name), the frame's one poll of the plugin UI (the plugins are asked once
    // and the answer is shared by the map, the panels and the instruments), and the receiver plugins' own pictures (the runner
    // takes its DSP lock for the copy; the patch decoders' pictures are the engine's and read_image's, these are not).
    // HOST-PROCESS, counted by the guard as one read each, none listed as ported: with hostTracks(), hostPaths(), hostPanels(),
    // hostInstruments() and hostAltitudeNear() they are the whole of what the plugin windows take of the engine, and the
    // method set of the one feed interface the fence will put in their place (docs/engine-stage4.md, group 14 part d).
    const std::vector<std::string>& hostTrackPluginNames();
    void hostPollPlugins();
    // WHAT THE WINDOW SHOWS OF EACH PLUGIN, told to the engine (stub campaign B2, 0.99.73; app_window_plugin_use.cpp): the plugins whose
    // windows are open, whose map page is open, the radar scope when it shows, the Decoder output window - the signals only a window
    // can see, which wake a plugin set to AUTO. The whole table is sent when it CHANGES (FOXAPP_OP_PLUGIN_USE), never every frame.
    void reportPluginUse();
    std::string pluginUseText_;  // this frame's table (kept for its room)
    std::string pluginUseSent_;  // the last one sent
    void hostPollImages(std::vector<cascade::core::HostImage>& out);
    // Does the receiver have a position? The state's FOXAPI_RX_POSITION_SET (rxLatDeg and rxLonDeg are valid with it).
    bool receiverPositionKnown() const { return (uiState_.flags & FOXAPI_RX_POSITION_SET) != 0u; }
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

    void drawPluginPresets(const cascade::gui::PluginRow& p);
    // The window half of a preset press: the plugin's map page, picture,
    // panels and instruments, or Decoder output for a text decoder. The engine no longer calls it (the pump move's step P2): the
    // press is an EVENT, FOXAPI_EVENT_PRESET_APPLIED (the module's file name), that pumpUiEvents takes in the look and answers
    // with this, from the PLUGINS row; run()'s bounded-run seam calls it directly with what the loaded plugin declares.
    // `presetFlags`: the preset's CASCADE_PRESET_* word (the event's value[0]); 0 from the capture seam, whose press is of a plugin with none.
    void openPluginWindowsFor(const std::string& name, std::uint32_t caps, std::uint32_t presetFlags = 0u);
    // THE ONE WINDOW A DOUBLE-CLICK ON A FITTED MODULES ROW OPENS (0.99.77; stub campaign B5, app_window_fitted.cpp): the panel, else the picture
    // window, else the instrument, else the map page, of the plugin the module file names, marked shown. A STOPPED plugin has no instances, so
    // nothing is shown for it (false). Marking it shown is the whole of the work: the window reports it (reportPluginUse) and the engine's
    // lifecycle wakes a plugin that was asleep.
    bool openPluginOwnWindow(const std::string& pluginKey);
    // THE TRACK SOURCES THE TABLE DECLARES, by display name: loaded, a track source by its capability word, not stopped (master 0.99.73). The map
    // pages are made and pruned from this, and not from the plugin UI's instances: a tracker set to AUTO that nothing is using is asleep and has
    // none, and its page's row on the rail - and opening it - is how it is woken.
    std::vector<std::string> declaredTrackPluginNames();
    // The REMNANT of the receiver-control rows, and it is the half the fitted
    // window cannot draw: the refusal notice PluginUi records, and the grants
    // held by modules that are NOT installed any more. The fitted window
    // carries the grant key for every module it lists, so those rows are not
    // repeated here - but it lists only what the host loaded, and a permission
    // the user can neither see nor revoke is exactly the kind that must not
    // exist. Draws nothing when there is neither a refusal nor a stale grant.
    void drawPluginTuneControls();

    // The plugins' spectrum and waterfall marks, over the panel at (x0, y0).
    void drawPluginMarkers(float x0, float y0, float width, float height, bool waterfall);

    // --- Preset bars: a plugin's own window offers its own presets (0.99.0) --
    //
    // Looks up `displayName` (a HostTrack/HostImage/HostPanel/HostInstrument
    // /MapPage plugin tag — the same identity pluginKeyForDisplayName
    // resolves) among the LOADED plugins of the window's plugin cache
    // (pluginLists), which holds each one's validated presets as the table
    // lists them (PRESETS, USER_PRESETS) - never a fresh count()/get() walk
    // of the plugin's own code. Since window port group 8 the answer is a
    // row of that cache and not the engine's muteStates_. Null when no loaded
    // plugin answers to that name (see the caller, which treats null the same
    // as "nothing to draw"); a loaded plugin with no presets answers with a
    // row whose presets are empty.
    const cascade::gui::PluginRow* muteStateForDisplayName(const std::string& displayName);
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
    std::string muteBannerSubject();
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
    // (drawPatchPage left with window port group 13: the close rule, the radios,
    // the plan and the sets are the engine's pass's now - Engine::pumpPatch -
    // and the window tells it whether the view is showing: setMainViewPatch.)
    // THE PATCH VIEW (0.99.40): the canvas, its transport, parts bin and
    // inspector, drawn as the main window's face in place of the spectrum,
    // the waterfall and the status column - into the child it is called in,
    // so it is exactly as large as the window leaves it.
    void drawPatchView();
    // Which face the main window shows: the patch (true) or the receiver.
    // Only that - it asks for no device list (see patchListsWanted_) - and the
    // engine is told (FOXAPP_OP_SET_PATCH_PAGE_OPEN), whose pass runs the patch
    // while it is showing and stops it when it is not.
    void setMainViewPatch(bool patch);
    // Tells the engine whether the view is showing, when that differs from what
    // it was last told (the engine starts "showing", as the window does); every
    // frame from drawUi, so a command the session could not take is sent again.
    void syncPatchPageOpen();
    bool patchPageOpenSent_ = true;
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
    // Starting position and size for a page that opens INSIDE THE MAIN WINDOW,
    // centred and stepped down and right by `slot` so two that open together
    // do not hide each other (gui/page_geometry.hpp, pageOpenInsideStaggered).
    // `wantW` x `wantH` is the size it would like; `outW` x `outH` is what it
    // gets once held to the main window, and is what the caller hands
    // beginPage as the page's default so a RESET WINDOW SIZES puts it back to
    // the rectangle it opened at - position as well as size, because the
    // placement is queued FirstUseEver every frame and beginPage's reset
    // re-arms that. Used by the Decoder output window (slot 0) and the plugin
    // picture windows (slot index + 1), which used to hang off the main
    // window's right edge so as to be a separate operating system window and
    // opened with only their 19 px margin on screen whenever the main window
    // reached the edge of the screen (report df5aff7da93dc88c). A page dragged
    // out of the main window still becomes its own window. (app_window_map.cpp.)
    void placeInsideMainWindow(int slot, float wantW, float wantH, float& outW, float& outH);
    // The size such a page opens at. Named rather than written twice because
    // the same pair is what beginPage is handed as the page's default size,
    // and a "reset window sizes" that put a window back to a DIFFERENT
    // rectangle than the one it opens at would be a third size nobody asked
    // for.
    static constexpr float kSeparatePageW = 720.0f;
    static constexpr float kSeparatePageH = 520.0f;
    // The same, at a size the caller asks for and MOVED so all of it - the
    // resize grip in the bottom-right corner included - lands on the monitor
    // it opens on (mapPlaceDefaultRect). kSeparatePageW x kSeparatePageH's fixed
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

    // --- THE WATERFALL'S MARKERS (stub campaign B10, 0.99.57; app_window_markers.cpp has the why) ----------------------------------------
    // A user's request (2026-09-30): drop a marker on the waterfall at a frequency of interest WITHOUT tuning there, list them, copy them, clear them. The
    // list is the ENGINE's (Engine::freqMarkers_, markers.json, saved on a worker; core/freq_markers.hpp has the rules - the numbers M1, M2, ... are never
    // reused); the window reads it as FREQ_MARKERS, again only when fact.markers.version moves, and works it only by FOXAPP_OP_MARKER_*.
    struct MarkerListCache {
        std::uint64_t listSeq = ~std::uint64_t{0};
        std::string version;                           // fact.markers.version the rows were read at; "" never
        std::vector<cascade::core::FreqMarker> rows;   // in the order dropped (ascending number); the note whole (fact.markers.note.<n>)
        std::string error;                             // fact.markers.error: a damaged or unsaved markers.json, "" none
        std::string factText;                          // the fact as just read (kept for its room)
        std::string factKey;
        std::uint32_t copies = 0;                      // times the rows were copied (a test reads it)
    };
    MarkerListCache markerCache_;
    const MarkerListCache& markerLists();
    // The number of the marker nearest `hz` within `tolHz`, or 0 (FreqMarkers::nearest's rule, over the window's copy).
    int markerNearHz(double hz, double tolHz);
    // Draws the markers inside [loHz, hiHz] over a waterfall at x0, y0 of `width` x `height`: a dashed line each and a tab ("M3 145.502") along the foot,
    // stacked upward where tabs would touch. `scale` is the part's zoom (1 on the receiver); `avoid`, when given, is the waterfall's own foot plate (x0, y0,
    // x1, y1) whose extent counts as taken. Draw calls only - no widget - so it can never take a click meant for the waterfall.
    void drawFreqMarkers(double loHz, double hiHz, float x0, float y0, float width, float height, float scale, const ImVec4* avoid = nullptr);
    // The same markers as faint lines through the SPECTRUM TRACE over [x0, x0+width] x [y0, y0+height], so a noted signal's peak can be found there too.
    void drawFreqMarkerLines(double loHz, double hiHz, float x0, float y0, float width, float height, float scale);
    // Asks for the marker menu at the pointer for frequency `hz` on a view of `hzPerPx` hertz a pixel. The menu itself is drawn by drawMarkerMenu, at the
    // top level, because the receiver and every patch Display part open the SAME one. Opening it changes NOTHING.
    void requestMarkerMenu(double hz, double hzPerPx);
    void drawMarkerMenu();
    void drawMarkerWindow();
    bool markerWindowOpen_ = false;
    bool markerMenuPending_ = false;    // open the menu at the top level this frame
    double markerMenuHz_ = 0.0;         // where the right-click landed, rounded
    double markerMenuStepHz_ = 1.0;     // that view's pixel, as a 1-2-5 step
    int markerMenuNear_ = 0;            // a marker under the pointer, or 0
    // A NOTE BEING TYPED: the note goes to the engine a keystroke at a time (the engine saves half a second after the last), and the box shows what was
    // typed until the engine's list has it back - never the older text a frame later, which would eat the characters typed in between.
    struct MarkerNoteHold {
        int number = 0;
        std::string text;
        int frames = 0;
    };
    MarkerNoteHold markerNoteHold_;

    // --- THE SPECTRUM'S TRACE, THE DISPLAY RANGE AND THE PASSBAND'S SLIDE (stub campaign B11, 0.99.57 - 0.99.83; app_window_display.cpp has the why) ------
    // All window state, over the table's existing words: nothing here is a new op, list or fact (the engine holds the dB window IN FORCE as dbMin/dbMax and
    // takes it by FOXAPI_OP_SET_DISPLAY_RANGE; which of Automatic and Manual that is, and the pair Manual returns to, is the person's choice about what they
    // see and is kept - and saved - here, as the trace mode is).
    //
    // THE TRACE MODE (a right-click on the spectrum, or on a patch Spectrum part's trace): Normal, Peak hold or Average over a length. One setting for every
    // spectrum; the held trace is drawn in bold over the live one. spectrumHold_ is the receiver's, patchHolds_ one per Spectrum part (its axis is its own columns).
    cascade::core::TraceMode spectrumTraceMode_ = cascade::core::TraceMode::Normal;
    double spectrumAverageMs_ = cascade::core::kTraceAverageDefaultMs;
    cascade::core::TraceHold spectrumHold_;                       // the receiver's
    std::map<cascade::core::patch::NodeId, cascade::core::TraceHold> patchHolds_;
    std::uint64_t spectrumHoldSeq_ = ~std::uint64_t{0};            // the frame the receiver's hold last took
    bool traceMenuPending_ = false;                                // open the menu at the top level this frame
    void setSpectrumTraceMode(cascade::core::TraceMode m);
    void setSpectrumAverageMs(double ms);
    void resetSpectrumTraces();
    void feedSpectrumHold();                                       // once a frame, from readSpectrum: a NEW frame moves the receiver's hold
    unsigned int traceColour() const;
    void drawTraceModeLabel(float x0, float y0, float scale);
    void drawTraceModeMenu();
    // THE DISPLAY RANGE (a right-click on the spectrum or the waterfall): Range: Automatic or Manual, Floor and Ceiling, and Reset. displayRange_ is the
    // person's choice; the window in force goes to the engine by SET_DISPLAY_RANGE, and the picture is drawn from the state's dbMin/dbMax as ever.
    cascade::core::DisplayRange displayRange_;
    // What the window asked the engine for and is waiting to see in the state (a command lands at the engine's next pass), so that a state that has not caught
    // up is not mistaken for somebody else's change; and the last pair the state showed, so a CHANGE of it that nobody here asked for (the web remote's
    // range) is taken as the person's choice of Manual, as master's onDisplayRange did.
    struct DisplaySent {
        bool pending = false;
        cascade::core::DbPair pair;
        double atS = 0.0;
    };
    DisplaySent displaySent_;
    bool displaySeenValid_ = false;
    cascade::core::DbPair displaySeen_;
    void sendDisplayRange();                                       // the window in force, to the engine
    void followDisplayRange();                                     // once a frame after refreshUiState
    void restoreDisplayRange(const cascade::core::AppConfig& cfg); // applyConfig's half
    void setRangeMode(cascade::core::RangeMode m);
    void setManualRange(float floorDb, float ceilingDb, bool floorMoved);
    void resetDisplay();
    void drawDisplayMenuBlock();                                   // the block that ends the spectrum's menu and the waterfall's
    // THE TIP: "Right-click to change the range and display", after four seconds of rest on the spectrum, the waterfall or a patch Spectrum part
    // (gui/hover_dwell.hpp). displayTipShown_ is whether it was up this frame (the census reads it).
    cascade::gui::HoverDwell displayTip_;
    bool displayTipShown_ = false;
    void showDisplayTip(std::uint64_t region, bool over, const char* name);
    // THE PASSBAND'S SLIDE (Shift + wheel over the spectrum or the waterfall, 0.99.82): the tuned frequency moves by the mode's raster a notch; the picture
    // follows it for kPassbandFollowS (passbandFollowUntilS_, ImGui's clock), and ends at once under the hand. slideNotches_/slideAtS_ gather a touchpad's
    // fractions into whole notches; slideAsk_ is what the last slide asked the engine for while the state has not shown it yet (the retune is paced by the
    // engine), so a run of notches in a few frames counts from where the last one is going and not from where the receiver still is.
    // THE PICTURE FOLLOWS A SLID PASSBAND (0.99.82): for this long after the last Shift + wheel the view is panned to keep the passband in it, with this share of the
    // view's width to spare at the edge it was leaving, so the band is not left against the frame. A second and a half outlasts the retune pacing (50 ms) and the source's
    // readback, and is short enough that a view the user arranges afterwards is never fought; the user's own pan, drag or zoom ends it at once.
    static constexpr double kPassbandFollowS = 1.5;
    static constexpr double kPassbandFollowMargin = 0.08;
    // The regions the 4-second tip is asked about (gui/hover_dwell.hpp). A patch part's regions are (node id << 2) | 1 for its trace and | 2 for its waterfall, which for any
    // node (ids start at 1) is 5 or more, so these two cannot meet one.
    static constexpr std::uint64_t kTipRegionSpectrum = 1;
    static constexpr std::uint64_t kTipRegionWaterfall = 2;
    void slidePassband(double notches);
    double slideNotches_ = 0.0;
    double slideAtS_ = -1.0e9;
    struct SlideAsk {
        bool pending = false;
        double centreHz = 0.0;     // the source's centre the last slide asked for
        double offsetHz = 0.0;     // the passband's offset from it
        double atS = 0.0;
    };
    SlideAsk slideAsk_;
    double passbandFollowUntilS_ = 0.0;
    // THE TYPED FREQUENCY'S MODE (0.99.83): typing a frequency in another band adopts that band's conventional mode (core/band_mode.hpp has the rule).
    // Returns the 1-based FOXAPI_DEMOD_* the mode should become, or 0 for "leave it".
    int modeForTypedFrequency(double typedHz);

    // --- THE AIRBAND SECTION (stub campaign B7, 0.99.60 - 0.99.71; app_window_airband.cpp has the why) -----------------------------------
    // The monitor is the engine's (src/engine/engine_airband.cpp): the window sends FOXAPP_OP_AIRBAND, FOXAPP_OP_BOOKMARK_TICK, _ADD_ROWS and
    // FOXAPI_OP_BOOKMARK_REMOVE_GROUP, reads fact.airband.* when listSeq moves, and reads the live list AIRBAND_CHANNELS once a frame - never
    // on listSeq. What the window keeps is airband_ (gui/airband_window_state.hpp).
    void drawAirbandSection();
    void airbandFrame();   // once a frame, with the polls: disk jobs, the view switch, the monitor's sentences, the census
    // On the spectrum: one mark per channel of the block on the air, from the live list (called from drawCenterPanels), and the caption.
    void drawAirbandMarkers(float x0, float y0, float width, float height);
    void drawAirbandCaption(float x0, float y0, float width, float yBottom);
    // The monitor's facts (read when listSeq moves) and its channels (read at most once an ImGui frame, never on listSeq).
    const cascade::gui::AirbandFacts& airbandFacts();
    void airbandReadLive();
    void airbandRebuildView();
    bool airbandStatusText(std::string& block, std::string& hearing);
    std::string airbandPresetName() const;
    void airbandLookup(const std::string& code);
    void airbandAddAirport(const cascade::core::Airport& a);
    void airbandAddManual();
    void airbandShowGroup(const std::string& group);
    void airbandImportPreset();                       // the Import key: the typed path's rows into the preset (the empty box: openImportDialog)
    void airbandImportFile(const std::string& path, const cascade::core::ImportInto& into);   // the preset (and tick) the rows join
    void airbandPollJobs();
    bool airbandImportPending();   // the file for a preset is being read on its worker
    void airbandPollExport();   // the preset's Export CSV the engine has finished writing (fact.bookmarks.export.*)
    void airbandSendChunks();
    void airbandFinishImport();
    void airbandRemovePreset();
    void airbandExportPreset();
    void airbandListen();
    // A command whose answer the section waits for (its ticket is kept: airbandOnAnswer), and the answers; actOnUiAnswers calls the second
    // for every result.
    std::uint64_t airbandSend(const cascade::core::cmd::QueuedCommand& q, cascade::gui::AirbandAsk::Kind kind, const std::string& group,
                              std::size_t total, std::size_t ticked);
    void airbandOnAnswer(const FoxCommandResult& r);
    cascade::gui::AirbandJobs& airbandJobs();
    cascade::gui::AirbandWindowState airband_;

    // --- THE "Import a frequency list" WINDOW and the import path (0.99.80 - 0.99.82; stub campaign B8; app_window_import_dialog.cpp) ----------
    //
    // A centred modal with a drop zone drawn in words, a BROWSE key (only where there is a file dialog) and Cancel. It opens when Import is
    // pressed with its box empty (the Bookmarks section's, or the Airband preset's, whose rows then join the preset) and when an import comes
    // back with nothing; it closes when one comes back with rows. THE IMPORT ITSELF is the engine's (FOXAPP_OP_BOOKMARK_IMPORT_FILE): the
    // window cleans what was typed, pasted or dropped into a path (cleanImportPath: quotes, the Enter of a paste, a file:// address) BEFORE the
    // op; a path with a root goes to the engine as it stands; any other - a bare name, lists/marine.csv - is looked for in the working directory
    // and then in the recordings folder on a worker (the read stays off the pass), and the op names the place found (or the last place tried, so
    // the failure says where it looked). The engine's answer (FAILED when it read nothing) is what opens the window.
    void importBookmarkFile(const std::string& path, const cascade::core::ImportInto& into = cascade::core::ImportInto{});
    void importSendOp(const std::string& path, const cascade::core::ImportInto& into);   // the op, its answer awaited
    void importLookupPoll();                        // once a frame: a finished lookup is sent on to the engine
    void importOnAnswer(const FoxCommandResult& r); // the engine's answer to an import this window sent
    bool bookmarkImportPending();                   // a lookup is out on its worker, or the engine's read of the file is on its (fact.bookmarks.importing)
    double bookmarkImportElapsedS();
    void importOutcomePoll();                       // once a frame: an import the engine has finished reading (fact.bookmarks.import.*) opens or closes the window
    void drawImportDialog();
    // Opens it (the state only: the keys that ask are drawn inside the rail, before the root level exists, and ImGui wants OpenPopup and
    // BeginPopupModal in one ID stack - the deferral drawMutePopup() uses). `into` is where the rows go (a preset's name, or nothing: as the file
    // says), `reason` the sentence of the failure that opened it.
    void openImportDialog(const cascade::core::ImportInto& into, const std::string& reason);
    bool importDialogOpen_ = false;
    bool importDialogQueued_ = false;
    cascade::core::ImportInto importDialogInto_;
    std::string importDialogReason_;
    struct ImportAsk {
        std::uint64_t ticket = 0;
        cascade::core::ImportInto into;
        std::string path;                           // what was sent: the engine's outcome names the path it read
    };
    std::vector<ImportAsk> importAsks_;             // the imports sent whose answer has not come
    std::vector<ImportAsk> importOut_;              // answered OK: the engine is reading the file; its outcome is fact.bookmarks.import.*
    std::uint64_t importSeqSeen_ = 0;               // fact.bookmarks.import.seq as last taken over
    double importOutSinceS_ = -1.0;                 // when the engine's read was first seen out (the "waiting for the disk" line's clock)
    std::shared_ptr<cascade::gui::ImportJobs> importJobs_;   // made on first use; defined in app_window_import_dialog.cpp
    // THE BROWSE KEY'S FILE DIALOG (core/file_pick.hpp; 0.99.74): a blocking native dialog under the watchdog's user-paced pause
    // (runShellOpen(watchdogShellHooks(), ...)), or - in a bounded run - the seam FOXSDR_FILE_PICK: the variable's text is the file "chosen",
    // an empty one is the dialog cancelled. Defined in app_window.cpp, where the pause's call sites are counted (tests/test_excuse_cap.cpp).
    bool pickFile(const cascade::core::FilePickRequest& request, std::string& path);
    bool filePickOffered_ = false;                  // there is a dialog to open (the system's, or the seam)
    std::optional<std::string> filePickSeam_;       // FOXSDR_FILE_PICK, read by run() in a bounded run only

    // --- RADIO SETUP (core/radio_setup.hpp, app_window_radio_setup.cpp; 0.99.69, the INSTALL DRIVER key 0.99.88; stub campaign B8) ----------
    //
    // THE FIRST-RUN PAGE: what is plugged in, what is installed for it, and the one thing to install when the two do not add up. It opens BY
    // ITSELF once a launch, when the start-up probe has finished, the application's own enumeration has listed no radio, nothing the probe found
    // is usable and the person has not ticked "Don't show this again"; and by a key in the Source section whenever they want it. The open flag is
    // in no config (the rule of every torn-off page); the one thing saved is the tick, as AppConfig::radioSetupDontShow. The probe is the
    // window's own (host glue: the USB bus and the vendors' installs of THIS computer, on a worker of its own); what the application's lists hold
    // comes from the table (gui/radio_setup_inventory.hpp) - the Source rows, the patch's radios, the state's flags, fact.source.uhdNoImages.
    bool radioSetupOpen_ = false;
    bool radioSetupDontShow_ = false;
    cascade::core::radiosetup::Probe radioSetupProbe_;     // started on the first frame and again by CHECK AGAIN
    bool radioSetupStarted_ = false;
    bool radioSetupDecided_ = false;                       // the once-a-launch decision to raise the page has been made, whichever way
    // FOXSDR_FAKE_RADIO_SETUP, read in bounded runs only: a synthetic machine that replaces the probe AND the application's lists, so a test can
    // make the page appear with a chosen finding on any desk.
    std::string radioSetupFake_;
    // This is a bounded (--frames) run: it never has the page raise itself on the real bus (a CI runner has no radio and would get the page over
    // every scripted click of every other test); it still probes and logs, and raises the page only for a synthetic machine.
    bool radioSetupBounded_ = false;
    cascade::core::radiosetup::Inventory radioSetupInventory();
    void radioSetupStartProbe();
    void radioSetupFrame();
    void radioSetupCheckAgain();
    void drawRadioSetupPage();
    // THE ONE-CLICK DRIVER INSTALL (0.99.88, core/winusb_install.hpp): the elevated helper started by INSTALL DRIVER (`cascade --install-winusb`,
    // a process_boot helper, kind-blind), waited for off the GUI thread. Null when none has been started; ONE at a time.
    struct DriverInstallJob;
    std::shared_ptr<DriverInstallJob> driverInstall_;
    std::string driverInstallStatus_;                      // the last install's sentence for the page, already translated
    // FOXSDR_FAKE_DRIVER_INSTALL=<outcome>[,<milliseconds>] (bounded runs with a synthetic machine only): the key answers with that outcome
    // after that long, starting nothing and showing no prompt - so a test can hold an install "in flight" and see the key refuse a second one.
    std::string driverInstallFake_;
    void radioSetupInstallDriver(const std::string& instanceId);
    void radioSetupPollDriverInstall();

    // --- Config persistence (P5) ---------------------------------------------
    // THE WINDOW'S HALF OF THE START-UP STATE into its own members (shell retirement R7; was applyConfig, which also applied the engine's half - that is
    // the host's restore now). `startup` is a config with the start-up state's (core::startupState) fields of the ENGINE's half that this window reads;
    // the interface's half is taken from the table (withInterfaceHalfFromTable). Returns the config it applied, interface half and all. A test host
    // applies the engine's half itself and then calls this (tests/test_host.hpp: th::applyConfig).
    cascade::core::AppConfig applyWindowHalf(const cascade::core::AppConfig& startup);
    // THE INTERFACE'S HALF OF THE SETTINGS FILE (shell retirement R5; core/config_halves.hpp, docs/api-0.3-additions.md 9.16). This window owns part of the
    // file - its view state, the key bindings, the patch text, the update and diagnostics switches, the beta tester's record - and hands that part to the
    // engine as a document (FOXAPP_OP_INTERFACE_CONFIG) whenever it changes, so the engine's snapshot is the whole file; it reads the part it STARTS from
    // from the engine (fact.config.interface) instead of from the file it loaded. buildInterfaceHalf: the live state into `cfg` (`bookkeeping` false
    // leaves the beta tester's running report out, which is built only for a document that is about to be sent). withInterfaceHalfFromTable: `fromFile`
    // with its interface half replaced by the table's - the file's own when the table cannot be read. sendInterfaceHalf: once a look, after the frame -
    // sends when the half differs from the one last sent (a compare that leaves the self-moving fields out), at most one document per
    // kInterfaceSendGapS, except that a change of the Diagnostics switch is sent at once ("off means off" must not wait); a document the session would
    // not take (BUSY) is asked for again at the next look that is allowed to. True when a document went.
    void buildInterfaceHalf(cascade::core::AppConfig& cfg, bool bookkeeping);
    cascade::core::AppConfig withInterfaceHalfFromTable(const cascade::core::AppConfig& fromFile);
    bool sendInterfaceHalf(double nowS);
    static constexpr double kInterfaceSendGapS = 0.25;
    // THE LAST HALF, handed over at the end of the frame loop (shell retirement R8): the engine's final-state save is a snapshot of what the engine
    // holds, so what this window changed in its last second - which no look has sent yet, the self-moving fields included - has to be IN the engine
    // before the host stops it. Sent whole whatever the coalescing gap says. It does not wait for the answer: the frames are over, so in a stepped run
    // nothing would run the pass that takes it - the service's stop makes one whole pass before it joins the control thread (ServiceOptions::finalPass,
    // set for the window application), which applies everything this window submitted last; a free-running run's submit wakes the pass at once.
    // True when the session took it; false (said in the log) when it did not - the engine then saves the half it last had, which is what a crash would
    // have left.
    bool flushInterfaceHalf();
    // A CHANGE THE FRAME WANTS IN THE FILE AT ONCE (stub campaign B14: the Seen key of a message from the team): since shell retirement R8 the window saves nothing - the
    // engine's service keeps the file from the engine's snapshot - so this only lets the next look hand the interface's half over without waiting out the
    // coalescing gap (sendInterfaceHalf), and the engine's keeper writes it a debounce later.
    void saveConfigFromFrame();
    // The Source section's lamp for a saved source the generator is standing
    // in for (0.99.36): lit while one is remembered and not open. From the
    // window's engine API session (window port group 11): fact.source.keep,
    // the open flags, the generator's DEVICES entry and FOXAPI_SRC_CARD_OPENING.
    bool radioNotOpenLit();

    std::unique_ptr<SpectrumView> spectrum_;
    std::unique_ptr<WaterfallView> waterfall_;

    // Newest frame the window has taken from the engine's spectrum stream
    // (read_spectrum, once a frame, in refreshUiState - window port, group 6).
    // Cached here (not just handed to the views) so the spectrum keeps drawing
    // the last data after Stop - SpectrumView::draw takes bins per call and holds
    // no history of its own. `seq` is the stream's own sequence, the cursor the
    // next read asks "anything newer?" with; `dbBins` is sized to the engine's
    // bin count, once (a different count is the only thing that resizes it), so
    // a frame costs a copy and no allocation. The three places that used to poll
    // the pipeline for it (scope mode, the patch view, the centre panels) read
    // this.
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
    // When the press began (ImGui time): a press held as long as a long press is a HOLD and tunes nothing (gui/press_gesture.hpp, 0.99.57).
    double wfPressT_ = 0.0;
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

    // The Source section's controls for the row. From the window's engine API
    // session only (window port group 11, API 0.3 section 8.10): the cards,
    // their rates and the form are the table's SOUND_CARDS, SOUND_CARD_RATES
    // and SOUND_CARD_FORM, the listing's state is the sourceFlags word, and
    // what a control changes goes by FOXAPP_OP_SOUND_CARD_PICK / _FORM (with
    // its field mask) / SOUNDCARD_IQ_CENTRE and SELECT_SOURCE through uiSubmit.
    void drawSoundCardControls();
    // "Receives X to Y." under the Source section's controls, from the
    // settings shown: the AIR range, through the converter stored for the
    // section's card - fact.soundcard.span, which the engine computes with
    // gui::soundCardAirSpan.
    std::string soundCardReceivesText();

    // --- THE SOURCE SECTION AND THE SOUND CARD (window port group 11, API 0.3 section 8.10) ------
    //
    // WHAT THEY TAKE FROM THE LISTS AND FACTS, read when listSeq moves and kept (the rule of
    // uiFact): the combo's rows (DEVICES: an entry's index is its row) with the caption each
    // is lettered with - "(native)" composed HERE, in the language in force, so a language
    // switch (which the engine's fingerprint hashes) rebuilds it - the combo's own row and the
    // kept source's kind (fact.source.*), the open radio's rates, antennas, gains and options,
    // the dongles that cannot be opened, where the SoapySDR runtime looked, the sound card
    // inputs, their rates and the form, and the sentences' inputs. The per-frame flags (the
    // scan's states, a card opening) are NOT here: they are sourceFlags of uiState_. A frame
    // in which nothing moved reads and allocates nothing; strings and vectors keep their
    // room. listSeq 0 is "never read".
    struct SourceListCache {
        std::uint64_t listSeq = 0;
        std::uint32_t refreshes = 0;         // times everything was read again (a test reads it)
        struct Row {
            std::string name;                // DEVICES name (cut at 95 bytes): the radio's own label
            std::string caption;             // what the combo letters: "<name> (native)", the Pluto's own, a Soapy label
            std::uint64_t id = 0;            // the entry's id: what fact.source.id.<n> is keyed by
            bool soapy = false;              // the id begins "soapy:"
            bool pluto = false;              // the id begins "pluto:"
            bool rtltcp = false;             // the id begins "rtltcp:" (0.99.70: the Pluto's kind of row)
            std::string driver;              // the radio's driver key, for the RADIO SETUP page: a native row's own ("rtlsdr"), a SoapySDR row's "driver=" ("uhd"); "" for the three fixed rows
        };
        std::vector<Row> rows;               // DEVICES, in the combo's order
        bool anySoapyRow = false;            // some entry is a SoapySDR row (its id begins "soapy:")
        int soapyRows = 0;                   // how many entries are SoapySDR rows
        int nativeRadioRows = 0;             // native rows that are a radio found: not the three fixed rows, not the Pluto's or the rtl_tcp server's (rows that ask for an address)
        bool generatorActive = false;        // entry 0 is ACTIVE: the source in use is the generator
        int row = -1;                        // fact.source.row: the combo's own row, -1 none
        std::string keepKind;                // fact.source.keep
        std::string opening;                 // fact.source.opening
        std::string scanGate;                // fact.source.scanGate
        std::string scanSkip;                // fact.source.scanSkip
        std::string faultedDrivers;          // fact.source.faultedDrivers
        bool uhdNoImages = false;            // fact.source.uhdNoImages: UHD itself said it has no firmware images (0.99.85)
        bool usrpOnUsb = false;              // fact.source.usrpOnUsb: the last scan's USB listing held a USRP
        std::string plutoUri;                // fact.source.plutoUri
        std::string rtlTcpAddr;              // fact.source.rtlTcpAddr
        struct Rate {
            double hz = 0.0;                 // what SET_SAMPLE_RATE takes
            std::string label;               // the Rate combo's caption
        };
        std::vector<Rate> rates;
        std::vector<std::string> antennas;
        std::string antenna;                 // the ANTENNAS entry that is ACTIVE; "" none
        struct Gain {
            std::string name;                // what SET_GAIN takes
            float lo = 0.0f, hi = 0.0f;      // the slider's span (the list's min and max)
            float db = 0.0f;                 // the radio's readback
            cascade::source::GainUnit unit = cascade::source::GainUnit::Decibels;
        };
        std::vector<Gain> gains;
        // DEVICE_OPTIONS: present = listed, on = ACTIVE.
        bool rfNotchPresent = false, rfNotch = false;
        bool dabNotchPresent = false, dabNotch = false;
        bool hdrPresent = false, hdr = false;
        bool adcPresent = false, dither = false, randomiser = false;
        struct Unbound {
            std::string name;                // the bus description; "" when it reports none
            std::string family;              // "rtl", "rx888" or ""
        };
        std::vector<Unbound> unbound;
        struct Evidence {
            int kind = 0;                    // 1 a vendor install, 2 a search path, 3 a module
            std::string name;
            std::string detail;
        };
        std::vector<Evidence> evidence;
        int modulesLoaded = 0;               // entries of kind 3
        // The sound card.
        struct Card {
            std::string name;                // "device (host API)"
            std::uint64_t id = 0;            // what FOXAPP_OP_SOUND_CARD_PICK takes
            bool active = false;             // the entry the form's card is
        };
        std::vector<Card> cards;
        int cardAt = -1;                     // the ACTIVE card's index; -1 none
        struct CardRate {
            double hz = 0.0;
            bool exclusive = false;
            bool active = false;             // the form's rate
        };
        std::vector<CardRate> cardRates;
        struct Form {
            std::string device;              // fact.soundcard.device, whole
            std::string hostApi;             // fact.soundcard.hostApi, whole
            double rateHz = 0.0;
            double centreHz = 0.0;
            int channel = 0;                 // 0 left, 1 right
            bool iqStereo = false;
            bool swapIq = false;
            bool picked = false;
        };
        Form form;
        std::string cardLabel;               // "device (host API)", "" with no device named
        double spanLoHz = 0.0;               // fact.soundcard.span: what the form's card receives on the air
        double spanHiHz = 0.0;
        std::string factText;                // the fact as just read (kept for its room)
    };
    SourceListCache sourceCache_;
    const SourceListCache& sourceLists();
    // WHAT THE SOURCE ROW SAYS ABOUT RADIOS (stub campaign B18, src/gui/app_window_source_row.cpp; gui/source_row_status.hpp holds the rules): whether "No radio hardware found" is
    // shown (a completed scan, and neither list holds a radio), the radios a running patch holds, and the chip and the combo's preview that name them.
    bool noRadioHardwareShown();
    const std::vector<std::string>& patchHeldRadioNames();
    std::string sourceRowChip();
    const char* sourceRowLabel(int idx);
    bool sourceKeepPreview();
    const std::string& sourceComboPreview();
    // The held radios' names, kept for the frames in which nothing moved (rebuilt when the lists did or the patch started or stopped), and the preview's buffer.
    struct HeldRadios {
        bool valid = false;
        std::uint64_t listSeq = 0;
        bool running = false;
        std::vector<std::string> names;
    };
    HeldRadios heldRadios_;
    std::string sourcePreview_;
    // A DRAFT FOR EACH BOX THAT IS TYPED IN (the scanner form's shape, window port group 7): what an edit sent
    // is not in the table until the engine's next pass, so the box keeps its own text until the engine has
    // caught up with it (the fact equals the draft) or a moment has passed (the engine took something else).
    // The Pluto box sends every edit as it is typed (FOXAPP_OP_SET_PLUTO_URI), so config.json keeps an address
    // that was never opened; the sound card's centre box sends it when Return is pressed.
    bool plutoUriPending_ = false;
    double plutoUriEditedAt_ = 0.0;           // ImGui::GetTime() of the last edit
    // The rtl_tcp box (0.99.70): the Pluto box's twin, with its own draft (FOXAPP_OP_SET_RTLTCP_ADDR per edit).
    bool rtlTcpAddrPending_ = false;
    double rtlTcpAddrEditedAt_ = 0.0;
    char rtlTcpAddrDraft_[192] = "127.0.0.1:1234";
    bool soundCardCentrePending_ = false;
    double soundCardCentreSentHz_ = 0.0;      // the centre the last commit sent
    double soundCardCentreEditedAt_ = 0.0;

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
        cascade::core::ScannerParams{}.startHz / 1.0e6, cascade::core::ScannerParams{}.stopHz / 1.0e6,
        cascade::core::ScannerParams{}.stepHz / 1.0e3,  cascade::core::ScannerParams{}.dwellMs,
        cascade::core::ScannerParams{}.holdMs,          cascade::core::ScannerParams{}.resumeMs,
        cascade::core::ScannerParams{}.listenMs,
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
    // every other radio. It draws from the window's engine API session only
    // (window port, group 4): the gain modes, the decimation factors and the
    // gain stages are the table's lists - GAIN_MODES, DECIMATIONS, GAINS, whose
    // stage its own AGC is driving carries FOXAPI_ITEM_AGC (API 0.3 section
    // 8.3) - and the choosing (chooseAirspyDecimation/GainMode/Agc) is by
    // command (FOXAPP_OP_AIRSPY_*, FOXAPI_OP_SET_GAIN through uiSubmit): no
    // member of the engine is called from these controls, so nothing of this
    // panel is a reviewed exception in kControlMayCall any more.
    bool drawAirspyControls();
    // THE PANEL'S READS OF THOSE LISTS, taken when listSeq moves (the rule of
    // uiFact) and kept: a frame in which nothing changed reads nothing and
    // allocates nothing. listSeq 0 is "never read".
    struct AirspyListCache {
        std::uint64_t listSeq = 0;
        bool open = false;             // GAIN_MODES has entries: an Airspy is open
        int gainMode = 2;              // the ACTIVE mode's code (AirspySource::GainMode)
        unsigned decimation = 1;       // the ACTIVE factor
        double hardwareRateHz = 0.0;   // that entry's rate times its factor: the board's own
        std::vector<unsigned> choices; // the factors, in the combo's order
        struct Stage {
            std::string name;          // what FOXAPI_OP_SET_GAIN takes
            float maxDb = 0.0f;        // the top of its slider
            float db = 0.0f;           // the radio's readback
            bool agc = false;          // FOXAPI_ITEM_AGC: its own AGC is driving it
        };
        std::vector<Stage> stages;
        bool lnaAgc = false;           // the LNA / MIXER stage's AGC (Free mode's two switches)
        bool mixerAgc = false;
    };
    AirspyListCache airspyCache_;
    const AirspyListCache& airspyLists();

    // --- THE DECK'S BIAS TEE KEY (2026-09-25, app_window_bias_key.cpp) --------
    //
    // A second control over biasTeePanel_, on the deck: drawn only while the
    // open radio has a bias tee, lit exactly when `shown` is, and switching
    // through switchBiasTee - the checkbox's own path - so the two cannot
    // disagree. Off is immediate; on asks first, once per radio per session
    // (core/bias_tee.hpp, BiasKeyGate, says why a dialog and not a hold).
    cascade::gui::BiasKeyGate biasKeyGate_;
    // Raised by a press that has to ask; the dialog is opened from the top
    // level (drawBiasKeyConfirm), because the press happens inside the deck's
    // child and ImGui wants OpenPopup in the same ID stack as the modal.
    bool biasKeyAskQueued_ = false;
    // The panel the KEY shows (window port, group 4): drawn while
    // fact.biasTee.reachable says "1", lit from FOXAPI_RX_BIAS_TEE_ON - the
    // engine's, whether that is a radio's readback or the census/capture
    // stand-in's (the window knows no stand-in). Not const: it reads facts
    // through the session's cache. The panel is the window's own member, set
    // here and handed back by reference: a BiasTeePanel holds a map (one heap
    // node on this standard library even when empty), and the deck draws the
    // key every frame.
    const cascade::gui::BiasTeePanel& biasKeyPanel();
    cascade::gui::BiasTeePanel biasKeyView_;
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
    // THE CRYSTAL CORRECTION'S CONTROLS (0.99.56, app_window_ppm.cpp): the engine owns the correction (engine/engine_ppm.cpp) and
    // publishes fact.ppm.*; these draw it and send FOXAPP_OP_SET_PPM. The cache is re-read only when listSeq moves.
    struct PpmCache {
        std::uint64_t listSeq = 0;
        bool on = false;
        std::string method;       // "na" | "radio" | "retune"
        double stored = 0.0;      // this radio's value
        bool wholeRadio = false;
        int wholeValue = 0;
        std::string error, card, model, serial, key;
    };
    PpmCache ppmCache_;
    const PpmCache& ppmLists();
    std::string ppmCardLine();
    void drawPpmControls();
    void drawPpmSection();
    void drawPpmBody();
    double ppmEdit_ = 0.0;          // the field's value while it is edited
    std::string ppmEditSeededFor_;  // the radio key + value the field was seeded from
    // The modes the Converter combo offers for the source installed, in the
    // combo's order; EMPTY means the Converter controls are not drawn at all.
    // (fact.converter.modes, kept in converterCache_; not const, like the
    // other members that read the session.)
    const std::vector<cascade::core::ConverterMode>& converterModesOffered();
    // A sentence under the Converter controls about a setting that is stored
    // for this source but cannot apply to it; "" when there is none.
    std::string converterUnusableNote();
    // WHAT THE CONVERTER ROWS READ OF THE LISTS AND FACTS, taken when listSeq
    // moves and kept (the rule of uiFact): the modes offered, the converter in
    // force (fact.converter.live), the open radio's stored setting (the
    // CONVERTERS entry that is ACTIVE) and where that entry is - the radio's
    // own centre is its value[3], read live when wanted. listSeq 0: never read.
    struct ConverterListCache {
        std::uint64_t listSeq = 0;
        std::vector<cascade::core::ConverterMode> offered;
        cascade::core::ConverterSetting live;
        cascade::core::ConverterSetting stored;
        int activeIndex = -1;
    };
    ConverterListCache converterCache_;
    const ConverterListCache& converterLists();
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

    // The source combo was open on the previous frame. The native
    // enumeration and the lazy Soapy scan run on the frame it OPENS, not on
    // every frame it stays open - the combo asks sixty times a second
    // otherwise, and one of those two answers costs a SetupAPI walk.
    bool sourceComboWasOpen_ = false;
                              // iqPath_ is just the edit buffer)
    // THE INTERFACE'S HALF AS THE ENGINE LAST HAD IT FROM THIS WINDOW (sendInterfaceHalf): the baseline is taken at the end of the constructor, so
    // nothing is sent until something differs from what the window started with. interfaceSentAtS_ is the clock of the last ATTEMPT (a refused one
    // too: it is what spaces the retries); interfaceSends_ counts the documents the session took.
    cascade::core::AppConfig interfaceSent_;
    bool interfaceSentKnown_ = false;
    double interfaceSentAtS_ = -1.0;
    std::uint64_t interfaceSends_ = 0;
    bool interfaceSendWarned_ = false;

    char bookmarkName_[128] = "";  // editable name for the next "Add current"
    // THE GROUP THE NEXT "Add current" JOINS (2026-10, a tester's request: an airport's tower, ground and approach kept together under its name).
    // Typed once and left - the field is not cleared by Add, so a run of frequencies lands in one group; empty means ungrouped, as every bookmark
    // was before.
    char bookmarkGroup_[96] = "";
    // "Rename this group": the inline field is open, and what it holds.
    bool bookmarkRenameOpen_ = false;
    char bookmarkRenameName_[96] = "";
    // A row's right-click menu: the "new group" field it offers.
    char bookmarkRowGroup_[96] = "";
    // AFTER A RENAME the group filter follows the group to its new name (master did it in the same frame, from its own list; the window's copy of
    // BOOKMARKS moves a publish later, so the choice is remembered here and made when the renamed group appears in the list; it lapses after
    // bookmarkRenameWantUntil_ frames so a refused rename cannot hold a later group of that name).
    std::string bookmarkRenameWant_;
    int bookmarkRenameWantUntil_ = 0;
    // --- A large imported list (0.99.19) ----------------------------------------
    // The list can be tens of thousands of entries, so nothing here walks it
    // per frame: the view is a cached index rebuilt only when the list or the
    // filter changes, the list is drawn through a clipper, and the spectrum
    // marks come from a binary search of the visible span.
    char bookmarkFilter_[96] = "";
    int bookmarkGroupSel_ = 0;           // 0 = every group
    bool bookmarkFavOnly_ = false;
    bool bookmarkMarkers_ = true;        // draw bookmarks on the spectrum
    bool bookmarkStackNames_ = true;     // a name that would overlap goes on the row below, up to three (0.99.54; AppConfig::bookmarkStackNames)
    char bookmarkImportPath_[512] = "";
    std::vector<std::string> bookmarkGroups_;
    std::vector<std::uint32_t> bookmarkView_;
    // What the view was built from (window port, group 7): the copy's counter (tuningCache_.markerCopies,
    // which moves exactly when fact.bookmarks.version did), the group chosen, favourites-only and the
    // filter text. Compared a frame at a time; nothing is built unless one moved.
    std::uint32_t bookmarkViewCopies_ = ~0u;
    int bookmarkViewGroupSel_ = -1;
    bool bookmarkViewFavOnly_ = false;
    std::string bookmarkViewFilter_;
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

    // WHICH FACE EACH BENCH METER WEARS (0.99.48; an Italian user on 0.99.42: "is it possible to customise the VU meter, choosing between 3
    // or 4 different VU meters?"), picked by right-clicking the meter itself (drawMeterStyleMenu). Each meter keeps its OWN style - the two
    // are never linked - and each carries its own peak-hold catch, which only the LED ladder and Peak styles read
    // (gui/meter_styles_math.hpp). Defaults to the face every meter has always worn. Window-only state: AppConfig carries the names.
    cascade::gui::MeterStyle meterStyleVolume_ = cascade::gui::MeterStyle::Classic;
    cascade::gui::MeterStyle meterStyleRate_ = cascade::gui::MeterStyle::Classic;
    cascade::gui::MeterPeakHold meterPeakHoldVolume_;
    cascade::gui::MeterPeakHold meterPeakHoldRate_;
    // A bench meter's right-click menu: the four styles, ticking the one `style` currently holds. Shared by both meters - each calls it with
    // its OWN style field, so the two menus can never write into each other's setting.
    void drawMeterStyleMenu(cascade::gui::MeterStyle& style);
    // The gesture that opens it, and the tooltip that says it exists: right-click anywhere in [tl, br) (the meter's own census rectangle)
    // opens `popupId`, which draws drawMeterStyleMenu(style). One function so the rate meter and the volume meter cannot answer the same
    // gesture two different ways.
    void drawMeterContextMenu(const ImVec2& tl, const ImVec2& br, const char* popupId, cascade::gui::MeterStyle& style);
    // THE TOP BAR'S KNOB ROW (stub campaign B12; app_window_deck.cpp, master 0.99.74): GAIN, SQUELCH and VOLUME, each its own dial, with a latched AUTO key under the first
    // two. Called from drawToolbar with the bar's own measurements. It draws from the state (uiState_), the first gain stage (refreshStatusLists) and two facts (squelch.auto,
    // airband.*), and changes things only through uiSubmit: SET_GAIN (paced, the last value the hand ended on), SET_DEVICE_AGC, SET_SQUELCH (which turns AUTOMATIC off in the
    // engine: a hand is a manual choice), FOXAPP_OP_AIRBAND 4 while the monitor listens, FOXAPP_OP_SET_SQUELCH_AUTO and SET_VOLUME. deckOnAnswer judges the SET_GAIN's answer.
    void drawDeckKnobs(ImDrawList* dl, const ImVec2& barTL, const ImVec2& barBR, float scale, float capPx, const cascade::gui::CounterLayout& layout);
    void deckOnAnswer(const FoxCommandResult& r);
    void deckAskGain();
    cascade::gui::DeckKnobState deck_;

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

    // --- THE INTERFACE SIZE (2026-09-28; gui/ui_scale.hpp) --------------------
    // "auto" (the default, follows the monitor's own Windows scaling) or one
    // of uiscale::kSteps as decimal text - AppConfig::interfaceScale. Applied
    // between frames like the theme: a change from the Display combo or from
    // Ctrl+=/Ctrl+-/Ctrl+0 calls uiscale::setChoice(), and applyPendingUiScale
    // - called every frame, cheap when nothing moved - rebuilds the ImGui
    // style from its saved un-scaled snapshot when uiscale::factor() changes.
    // Fonts need no such step: every real draw call site asks fonts::uiPx()
    // and friends for the CURRENT scaled size on every frame already (Dear
    // ImGui 1.92 draws any requested size from one loaded face at no atlas
    // cost), so there is nothing to rebuild there.
    // The un-scaled ImGuiStyle snapshot applyPendingUiScale rebuilds from
    // lives as a file-local in app_window_chrome.cpp, not here: this header is
    // compiled into the tests and must not need imgui.h (see the forward
    // declarations note above), and nothing outside that one function needs
    // to see it.
    std::string interfaceScale_ = "auto";
    void applyPendingUiScale();
    // Ctrl+=/Ctrl+-/Ctrl+0: step through uiscale::kSteps, or back to "auto".
    // Never armed while a text field wants the keyboard (WantTextInput) -
    // the same guard drawRailBankKeys uses for F1-F5.
    void handleInterfaceScaleKeys();
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
    // The same plugin's targets heard without a position (0.99.75): listed, never drawn - see drawTrackList. Filled beside pageTracks_ and
    // cleared with it.
    std::vector<cascade::core::HostTrack> pageUnplaced_;
    std::vector<cascade::core::HostPath> pagePaths_;
    // The VOLUME meter's needle, carried between frames so it can fall
    // gently rather than follow every syllable - see gui/volume_meter.hpp.
    float volumeNeedle_ = 0.0f;
    // "Usage reporting" settings section: the opt-in switch and what it sends.
    void drawUsageReportingSection();
    bool privacyNoticeOpen_ = false;
    // THE SWITCH'S CHECKBOX SHOWS WHAT WAS JUST ASKED FOR until the engine's fact catches up (window port group 14, part c;
    // fact.telemetry.enabled): the consent goes through the session and lands at the next pass, so a checkbox that read the
    // fact alone would snap back for a frame or two after a click. Released when the fact says what was asked, or after half
    // a second if it never does (a command that was refused). The chip, the lamp and the install id are the fact's own: they
    // show the change a pass after the click, where the old direct call showed it on the same frame.
    struct TelemetryPending {
        bool valid = false;
        bool on = false;
        double at = 0.0;
    };
    TelemetryPending telemetryPending_;

    // --- Beta tester usage (opt-in by pasting a code; see PRIVACY.md, -------
    // core/tester_usage.hpp) --------------------------------------------------
    // INDEPENDENT of telemetryEnabled_ above: a different switch, a different
    // credential, a different purpose. Nothing here collects or sends while
    // testerToken_ is empty - testerUsage_->armed() is exactly that condition.
    //
    // THE RECORDER IS THE WINDOW'S (stub campaign B15; stage-7 survey section 8, owner decision O8): a headless engine must never be able to send a tester
    // report, so the recorder, the sender, the queue and the token live here, in host glue, fed from facts the table already publishes (testerNoteFacts) and
    // from the window's own events (the noteFeature marks beside each control). It is shared, not a member by value, because the two listeners call it
    // from threads of their own and a server's last request may come after this window's members are gone: the closures keep it alive for as long as a
    // server holds them. (Until shell retirement R3 the two listeners called it - "browser", "cat" - from the closures installTesterUsageHooks handed the
    // engine; now the window reads the servers' two counters through the table, testerNoteListeners, and nothing calls the recorder from another thread. The
    // shared_ptr is left as it was: no behaviour reason to change it in this batch.)
    std::shared_ptr<cascade::core::TesterUsageRecorder> testerUsage_ = std::make_shared<cascade::core::TesterUsageRecorder>();
    // The recorder's two listener marks ("browser", "cat"), noted when the table's fact.web.served / fact.cat.accepted counters have moved since the last read
    // (once a frame in testerNoteFacts, and at every save of the session's report). Disarmed, the baseline follows the counters, so a code pasted mid-session does
    // not credit the requests served before it.
    void testerNoteListeners();
    std::uint64_t testerWebServed_ = 0;      // fact.web.served as last read
    std::uint64_t testerCatAccepted_ = 0;    // fact.cat.accepted as last read
    // The marks that need no control of their own because the table says them: a take in progress, the scanner, RF going out, the radio kind in use. Once
    // a frame, after the state is read; a no-op while no code is set.
    void testerNoteFacts();
    std::uint64_t testerFactsSeq_ = ~std::uint64_t{0};  // the listSeq the radio kind was last read at
    std::string testerSourceKind_;                      // the kind of the DEVICES entry that is the source in use ("siggen", "rtlsdr", "soapy", "soundcard", "file"; "" none)
    // The module files being fed now (the receiver's decoders, and the patch's while it runs): what testerUsageAccrue credits a second to.
    void testerFedPlugins(std::vector<std::string>& files, bool& patchDecoder);
    void testerNoteRadio(const std::string& kind);
    std::string testerToken_;
    bool testerTokenInvalid_ = false;
    // The retry queue carried from config, plus whatever this session's own
    // finished report adds to it - see testerUsageStartup/testerUsageJournal.
    // Kept free of any report not carrying THIS token - see
    // core::TesterUsageQueue::dropOthers, called wherever testerToken_
    // changes.
    cascade::core::TesterUsageQueue testerUsageQueue_;
    // ONE sender: the startup flush and the periodic in-session retry share
    // it (never more than one send in flight at a time - busy() refuses a
    // second). There is deliberately no exit-time sender any more: an
    // earlier version tried a bounded attempt at clean exit and measured up
    // to ~3.9s of added shutdown time against an unresponsive server (WinHTTP's
    // own floor), which is not what "never delays exit" can mean. The
    // finished session's report is saved to disk at exit exactly like
    // telemetry's own, and sent at the NEXT launch - see testerUsageJournal
    // and testerUsageStartup. TesterUsageSender's destructor never blocks
    // (see its header comment), so a send still in flight when the window
    // closes costs nothing at all.
    cascade::core::TesterUsageSender testerUsageSender_;
    double testerSessionStart_ = 0.0;  // glfwGetTime at start, for the duration
    std::time_t testerSessionStartWall_ = 0;  // wall clock, for the RFC3339 stamp
    cascade::core::SecondAccrual testerUsageAccrual_;
    // Set once a launch's queue flush hits Invalid or Retry, so a black-holed
    // endpoint or a bad code cannot turn testerUsagePoll() into a per-frame
    // retry loop against it for the rest of the session.
    bool testerUsageQueueTriedThisSession_ = false;
    bool testerShowPreview_ = false;
    bool testerPreviewOpenedByEnv_ = false;
    std::string testerCodeError_;
    // The SYSTEM bank's text field. A separate buffer from testerToken_,
    // sized for a whole pasted portal link, so a half-typed paste never
    // becomes "the token" until the Use button commits it
    // (drawTesterUsageSection), and so the masked display can show something
    // different from what is being typed. Rendered with
    // ImGuiInputTextFlags_Password like the web server's own password field,
    // so the credential is not shown in full while it is being typed either.
    char testerCodeBuf_[256] = "";
    // Sets testerToken_ to `token`, arms/disarms the recorder to match, and
    // drops every queued report that does not carry this token (a fresh
    // opt-in, a replaced code, and a removal all funnel through here) - the
    // one place that decision is made, so paste and replace cannot disagree
    // with it. `startFreshSession` is true only for a brand-new opt-in (the
    // token was empty before): a session already being recorded under an
    // old, now-replaced code keeps what it has, but one starting from
    // nothing gets a clean slate.
    void setTesterToken(const std::string& token, bool startFreshSession);
    void testerUsageStartup(const cascade::core::AppConfig& cfg);
    void testerUsageJournal(cascade::core::AppConfig& cfg, bool bookkeeping = true);
    // Called from within testerUsageJournal, the same cadence
    // telemetryAccrueMode runs at: banks running time against whichever
    // plugins are currently being fed.
    void testerUsageAccrue();
    cascade::core::TesterUsageReport buildTesterUsageReport(double sessionSeconds) const;
    // Drains a finished send's outcome on the GUI thread and acts on it via
    // core::applyTesterUsageOutcome (marks the token invalid, drops the head
    // of the queue, or leaves it for next time) - called once a frame.
    void testerUsagePoll();
    void drawTesterUsageSection();

    // --- Beta tester APP TOKEN / portal link (core/tester_link.hpp) --------
    //
    // A SECOND, DISTINCT identity from testerToken_ above (32 hex): the
    // 40-hex credential minted for this ONE install via the portal's "Link
    // FoxSDR" button or --link-tester, confirmed by the tester pressing Link
    // after seeing their own name (PORTAL-LINK-VERDICT.md finding 1 - never
    // stored silently). Non-empty means SYSTEM > Beta tester is visible even
    // with no portal token at all - see testerSectionVisible().
    std::string testerAppToken_;
    std::string testerAppTokenName_;

    // Confirm-by-name (job D): resolves a token's owner off the GUI thread
    // before anything is stored.
    cascade::core::BetaLinkNameSender testerLinkNameSender_;
    // The app token currently being resolved by testerLinkNameSender_, if
    // any - send() only takes the token, so this is what lets the result
    // (name only) be turned back into a full TesterLinkPending once it
    // arrives.
    std::string testerLinkResolvingToken_;
    // Migration (job E): exchanges an existing portal token for an app token
    // once, in the background, at start-up.
    cascade::core::BetaMigrationSender testerMigrationSender_;
    bool testerMigrationTried_ = false;

    // The link-request file (core/tester_link.hpp) is polled at ~1 Hz, not
    // every frame - there is no reason to ask the disk 60 times a second for a
    // file that, in the overwhelming majority of frames, does not exist.
    // Starts far in the past so the VERY FIRST frame always polls once - a
    // fresh launch that just wrote its own link-request file (main.cpp's
    // primary-instance path) must not wait a second to notice it.
    double testerLinkPollLast_ = -1.0e9;

    // WHERE THE POLL'S FILESYSTEM CALL RUNS - A WORKER, NEVER THIS THREAD. The
    // poll was a synchronous std::filesystem::exists() in a directory
    // (%APPDATA%\foxsdr) that can be redirected, synced or scanned, and it
    // froze the window (field report "hang ntdll.dll @ __std_fs_get_stats",
    // 0.99.59). gui/link_request_poll.hpp carries the report and the argument;
    // testerLinkPoll() only ever asks it and collects from it, and neither
    // can block.
    cascade::gui::LinkRequestPoll linkRequestPoll_;

    // A token claimed from the link-request file while a confirm-by-name
    // lookup was already in flight or a prompt was already awaiting the
    // tester's decision - claimLinkRequestFile() has already deleted the
    // file by the time this is known, so a request that could not be started
    // immediately used to simply vanish. Held here instead and started the
    // moment the sender/prompt frees up; a second arrival while one is
    // already queued overwrites it - LATEST WINS, since only the most recent
    // link click is what the tester actually meant.
    std::string testerLinkQueuedToken_;

    // --link-tester (main.cpp): reveals the paste box with no app token held,
    // for this session only - never written to config (see main.cpp's own
    // comment on why a bare flag, never --link-tester=<token>).
    bool testerLinkReveal_ = false;

    // The app token a migration exchange (job E) resolved to, THIS SESSION,
    // for whatever portal token it was tried against - kept even after
    // testerLinkPoll() has already acted on it (set testerAppToken_ and
    // cleared testerToken_), so a LATER manual link confirmation over a
    // still-held legacy portal token (the migration attempt failed with
    // NetworkError, or was never tried) can ask "is this the same tester the
    // migration already found?" - see TesterLinkPending::keepsLegacyQueue.
    std::optional<std::string> testerMigrationResolvedAppToken_;

    // Awaiting the tester's own Link/Not now decision - populated once
    // testerLinkNameSender_ resolves a token's name, drained by
    // drawTesterLinkPrompt(). Nothing is written to testerAppToken_ until
    // the tester presses Link.
    struct TesterLinkPending {
        std::string token;
        std::string name;
        bool replacing = false;  // an app token OR a legacy portal token was already held
        // Which identity `replacing` refers to - governs the prompt's
        // wording (drawTesterLinkPrompt): an app-token replace names the old
        // tester by their confirmed name, a legacy-token replace has no name
        // to offer for the thing being replaced (a portal token is never
        // resolved to a name on its own).
        bool replacingLegacyPortalToken = false;
        // Only meaningful when replacingLegacyPortalToken: true when this
        // session's own migration exchange (testerMigrationResolvedAppToken_)
        // already proved the legacy token and this new link resolve to the
        // SAME tester, so the reports queued under the legacy token are kept
        // (rewritten onto the new identity) rather than dropped. False,
        // including "we simply do not know", is the safe default and is said
        // plainly rather than assumed.
        bool keepsLegacyQueue = false;
    };
    // Pulled out of testerLinkPoll() as its own pure function, callable with
    // synthetic inputs, so the replacing/replacingLegacyPortalToken/
    // keepsLegacyQueue decision is testable directly (tests/test_tester_link_app.cpp)
    // rather than only reachable through a real confirm-by-name network
    // round trip.
    static TesterLinkPending computeLinkPending(
        const std::string& token, const std::string& name, bool appTokenHeld,
        bool portalTokenHeld, const std::optional<std::string>& migrationResolvedAppToken);
    std::optional<TesterLinkPending> testerLinkPending_;
    std::string testerLinkError_;  // "that link is not valid", shown once

    // testerAppToken_ when held, else testerToken_ - the identity a usage
    // report is actually sent under (core/tester_usage.hpp's contract
    // accepts either during the migration transition).
    std::string activeTesterToken() const;
    // Stores a CONFIRMED app token (the tester has already pressed Link),
    // clears the portal token and its queue (finding 7: the old identity
    // must not survive beside the new one), arms the recorder, and starts a
    // fresh session record exactly like a fresh portal-token opt-in does.
    void setTesterAppToken(const std::string& token, const std::string& name);
    // Clears the app token and its queued reports - used on revocation (a
    // 404 while sending under it) and by "Unlink". Unlike an invalid PORTAL
    // token, which is kept on screen for the tester to fix
    // (testerTokenInvalid_), a revoked app token is cleared outright: there
    // is nothing to fix in place, only a fresh portal link produces a new
    // one (PORTAL-LINK-VERDICT.md §8).
    void clearTesterAppToken();
    // True iff SYSTEM > Beta tester should be drawn at all: a confirmed app
    // token OR a legacy portal token is held (activeTesterToken()), a
    // migration exchange is in flight, or --link-tester was given this
    // session.
    bool testerSectionVisible() const;
    // Called once a frame: 1 Hz link-request file poll, migration sender
    // drain, and confirm-by-name sender drain.
    void testerLinkPoll();
    // The "Link this FoxSDR to beta tester NAME?" prompt, top-level like
    // drawDiagnosticsOffer - see that function's own comment for why a
    // confirmation belongs outside the borderless root window's ID stack.
    void drawTesterLinkPrompt();


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
    // CASCADE_FOREIGN_MODULE_TEST=<path of a DLL> (0.99.69, bounded runs only, Windows only): the frame loop maps that DLL with
    // LoadLibrary on frame 90, from the window's own thread, once - what another program's injected hook looks like to the loader.
    // Empty once it has been done, and in every run that is not a bounded one.
    std::string foreignModuleTestPath_;
    void loadForeignModuleForTest(int rendered);
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
    // THE SWEEP STARTS ON THE FIRST FRAME THAT CAN SAY WHOSE IT IS (window port group 14, part e). run() used to start it before
    // the frame loop, reading the engine's usage switch and install id directly; the id is now read as the engine's facts
    // (fact.telemetry.enabled, .installId) like everything else the window knows of it, and a fact answers nothing before the
    // engine's first publish (a read there would have started the sweep with an EMPTY id: "reporting is off"). run() arms
    // this; the frame's look (drawUi, right after refreshUiState) calls crashUploadStart until it answers true - decided:
    // started, or nothing to start - and the first look after the first publish is the first that can.
    bool crashUploadStartPending_ = false;
    bool crashUploadStart();
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
    // "Attach the diagnostics log": true by default for a bug, false for a dislike (cascade::core::problemReportDefaultAttachDiagnostics), reset to that
    // default each time the person (re)chooses a kind, but otherwise theirs to tick or untick. Never persisted, like everything else on this page.
    bool problemReportAttachDiag_ = false;
    // "Show what will be sent": a read-only preview of the exact scrubbed, capped text that would be attached - drawn only while this is true and the
    // checkbox above it is ticked.
    bool problemReportShowDiag_ = false;
    // THE ATTACHMENT'S CACHE. Rebuilding it means a ring snapshot under a mutex, the plugin-list walk and the whole-bundle scrub pass - real work that this page
    // (unlike Copy diagnostics, a one-off button press) would otherwise repeat every single frame the checkbox is left ticked. Rebuilt only when the log has
    // actually grown (problemReportDiagCacheLines_ disagrees with DiagLog's own count) or at most once per second (problemReportDiagCacheEpoch_ disagrees with
    // the page's own nowEpoch, itself whole seconds) - never invalidated just for being unticked. And once more, soon, when what it holds was built while the
    // reports folder was still being read (problemReportDiagCachePending_, retried at problemReportDiagRetryAt_): an attachment that said "still being read"
    // must not be the one that goes (B14).
    std::string problemReportDiagCache_;
    std::uint64_t problemReportDiagCacheLines_ = 0;
    std::uint64_t problemReportDiagCacheEpoch_ = 0;
    bool problemReportDiagCacheValid_ = false;
    bool problemReportDiagCachePending_ = false;
    double problemReportDiagRetryAt_ = 0.0;
    // THE REPORT RECOGNISED IN THE TEXT BOX (0.99.69, core/pasted_report.hpp), looked for again only when the text is not the text it was last looked for in: the
    // box holds up to 100000 characters and the page is drawn every frame, so the scan is not repeated for a text that has not changed. Empty when none was found.
    std::optional<cascade::core::PastedReport> problemReportPasted_;
    std::string problemReportPastedFor_;
    bool problemReportPastedValid_ = false;
    // The SDRplay diagnostic's text once it has finished (drawSdrPlayProbeDialog writes it, stub campaign B18; until then empty, and the page's "Attach the SDRplay diagnostic too" row
    // is never drawn) and whether the page attaches it.
    std::string sdrplayProbeReport_;
    bool problemReportAttachProbe_ = false;
    cascade::core::ProblemReportSendFlow problemReportSender_;
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
    // and the one-click bundle - and the SDRplay diagnostic's key (stub campaign B18).
    void drawDiagnosticsSection();
    // --- THE SDRPLAY API SERVICE CARD'S RESTART KEY and THE SDRPLAY DIAGNOSTIC'S DIALOG (stub campaign B18, src/gui/app_window_sdrplay.cpp) -------------------------------
    // sdrPlayService_ is what Windows last said the SDRplay API Service was doing (asked once per change of the driver's trouble, and when a restart ends); sdrPlayTroubleSeen_ is the trouble it
    // was last asked for (the driver's own record in the process's API table). The restart itself runs on sdrPlayRestart_'s worker, never this thread; sdrPlayRestartNote_ is the key's outcome,
    // already translated. pollSdrPlayService runs once a frame (the Polls scope); drawSdrPlayServiceKey is drawn in the Source section under the SDRplay API's advice.
    cascade::source::SdrPlayServiceStatus sdrPlayService_;
    cascade::source::SdrPlayServiceTrouble sdrPlayTroubleSeen_ = cascade::source::SdrPlayServiceTrouble::None;
    cascade::source::SdrPlayServiceRestart sdrPlayRestart_;
    std::string sdrPlayRestartNote_;
    void pollSdrPlayService();
    void drawSdrPlayServiceKey();
    void finishSdrPlayServiceRestart(const cascade::source::SdrPlayRestartOutcome& outcome);
    // The diagnostic's dialog (and, every frame whether or not it is showing, the poll of the child), and the start the dialog's Run makes. THE BIAS TEE, TWO STEPS: ticking the box only ASKS
    // (sdrplayProbeBiasAsk_); sdrplayProbeBiasOn_ becomes true only on "Yes, switch it on", and only it reaches the child. sdrplayProbeReport_ (above, with the bug page's attach tick) is the
    // text of the last finished diagnostic.
    void drawSdrPlayProbeDialog();
    void startSdrPlayProbe();
    bool sdrplayProbeConfirmOpen_ = false;
    bool sdrplayProbeBiasAsk_ = false;
    bool sdrplayProbeBiasOn_ = false;
    std::unique_ptr<cascade::source::SdrPlayProbeChild> sdrplayProbeChild_;
    std::string sdrplayProbePath_;    // the file being written, or last written
    std::string sdrplayProbeStatus_;  // the line under the key
    // Shown once, at the start of the run AFTER an unclean exit. A crash
    // handler can write a report but it cannot ask anything - by the time it
    // runs there is no user interface left to ask with. This is the ask.
    void drawDiagnosticsOffer();
    void copyDiagnosticsBundle();
    // The reports folder in the shell (made first, under a watchdog pause): the diagnostics offer's and the Diagnostics section's key.
    void openReportsFolder();
    // THE WHOLE BUNDLE (what Copy diagnostics puts on the clipboard and what a problem report attaches): the engine's rendered context
    // block, the log, the counters, the SDRplay service's state, the previous session's log and the reports on this machine, and this
    // session's slow-frame table. `freshHistory` asks for the report list to be read again and waited for (boundedly, on a worker) -
    // a person's own click; the problem-report page's once-a-second rebuild leaves it false and takes what has been read.
    std::string currentDiagnosticsBundle(bool freshHistory = false);
    // THE SESSIONS BEFORE THIS ONE for the bundle (core/diag_history.hpp): the end of the previous session's log and the reports on this
    // machine. Read on a worker, never on the frame; with diagnostics off - or a run that may not touch the disk - it returns an answer with
    // `included` false and starts NOTHING. startDiagHistoryRead() is the early read (applyDiagnosticsEnabled: run() start, and the moment
    // diagnostics are switched on), so the previous session is read before this session's log can rotate it away.
    cascade::core::DiagHistory diagHistoryForBundle(bool freshHistory);
    void startDiagHistoryRead();
    cascade::core::DiagHistoryCache diagHistory_;
    // applyDiagnosticsEnabled's first call is the start of the session; whether the switch was on then is whether the log file holds
    // this session's start line (see diagHistoryForBundle).
    bool diagStartApplied_ = false;
    bool diagLogHasSessionStart_ = false;
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
    // run() handed this run to a second process on the software renderer.
    bool relaunchWithSoftwareGl_ = false;
    // THE TWO HALVES OF THE WINDOW'S END (shell retirement R8): run() brings the window up and leaves the GL context, ImGui and GLFW standing when its
    // loop ends - the engine is stopped by the host while they still are - and closeWindow() takes them down. `glUp_` is whether there is anything for
    // closeWindow to take down (set once the backends are up; cleared by closeWindow, which is then idempotent); `renderedFrames_` and
    // `teardownStart_` are what run() knows and closeWindow prints (the frames the loop drew, and the moment the shutdown began, so "teardown N ms"
    // measures the whole of it - the engine's stop included - as it always has).
    bool glUp_ = false;
    int renderedFrames_ = 0;
    std::chrono::steady_clock::time_point teardownStart_{};
    // The window's own release of its end of the plugin UI port: disconnect, once (the port's own disconnect is idempotent). run()'s prelude calls it
    // after the port's release; ~AppWindow calls it for every path that never reached the prelude (a window that could not be created, a
    // constructor that threw, a test window that never ran).
    void leavePluginUi();

    // WHETHER A TORN-OFF PAGE GETS AN OPERATING SYSTEM WINDOW, and why not
    // when it does not. Decided once at startup from FOXSDR_SINGLE_VIEWPORT
    // and a probe for a second shared GL context, and re-decided the moment
    // the backend reports that the driver refused one - see
    // gui/viewport_policy.hpp and the 0.95.0 "crash cascade.exe @
    // glfwGetWin32Window" report that made it necessary. Kept as state rather
    // than re-derived because the sentence has to be logged exactly once.
    cascade::gui::ViewportDecision viewportDecision_ = cascade::gui::ViewportDecision::Enabled;

    // PAGES BROUGHT INSIDE WHEN SEPARATE WINDOWS ARE OFF (gui/page_geometry.hpp, pageFitInside). With separate windows off, every top-level
    // page that is not wholly inside the main window is moved and shrunk into it: all of them for a few frames after the driver refuses a
    // window mid-session (ImGui moves a page out of its own viewport over a frame or two, and the pass must see where it finally lands),
    // and any page on the frame it appears for the rest of the session. Called after drawUi, before ImGui::Render. pagesFitFrames_ is how
    // many more frames check every page.
    void fitPagesIntoMainWindow();
    int pagesFitFrames_ = 0;

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
    // THE RECEIVER'S POSITION, FOLLOWED (window port group 14, part a). The engine used to call the window from its
    // own pass when a position was applied (EngineHost::onReceiverPositionApplied: the typed fields, every map page's
    // home, the scope, the coverage map); the window now looks at the position the state carries once a frame
    // (refreshUiState -> followReceiverPosition) and does the same when it MOVED. positionSeen_ is what the last look
    // saw: set, latitude, longitude.
    struct PositionSeen {
        bool set = false;
        double latDeg = 0.0;
        double lonDeg = 0.0;
    };
    PositionSeen positionSeen_;
    void followReceiverPosition();
    // THE ONE DOOR THE WINDOW'S OWN PRESSES SEND A POSITION BY ("Set RX here", the two offers, a map click): the
    // FOXAPI_OP_SET_POSITION through the session, and - for a pair the rule accepts - the note that the coverage map
    // starts again at the next look EVEN WHEN THE NUMBERS ARE THE ONES IN FORCE. The state shows no change for a re-apply
    // of the same pair, and the old hook cleared the wedges on every apply; this keeps that, as the window's own effect.
    void submitReceiverPosition(double latDeg, double lonDeg);
    bool coverageResetPending_ = false;
    std::string gpsPort_;
    int gpsBaud_ = cascade::core::kDefaultGpsBaud;
    char gpsPortInput_[cascade::core::kMaxSerialPortNameChars + 1] = "";
    // THE GPS READER AS THE POSITION ROW DRAWS IT (window port group 14, part b; API 0.3 section 8.13): the GPS list's one
    // entry, read at the call once a look (readGpsRow, in refreshUiState) because the reader's seconds and sentence move every
    // second it listens and the entry is NOT part of the block listSeq counts. The reader itself is the engine's.
    struct GpsRow {
        int state = 0;               // 0 idle, 1 listening, 2 fixed, 3 failed, 4 timed out (the entry's value[0])
        bool listening = false;      // ACTIVE: what disables the port and baud fields and flips the key to Stop
        bool trouble = false;        // FAULT: failed or timed out - the warning ink
        double elapsedS = 0.0;
        int satellites = -1;
        int baud = 0;
        std::string port;            // as given to the last start ("" idle; the entry is blank to a session that is not LOCAL)
        std::string line;            // the engine's sentence (core::gpsStatusLine); "" idle: the row draws nothing
    };
    GpsRow gpsRow_;
    void readGpsRow();
    const GpsRow& gpsRow() const { return gpsRow_; }
    // A port or baud edit forgets the last result and the refusal: two FOXAPP_OP_CLEAR_STATUS through the session.
    void clearGpsResult();
    // Set when the GPS entry goes to Fixed (the cue the engine's onGpsFixApplied hook used to give); the rail's "Receiver
    // position" fold opens itself once on it, so the Fixed status line is seen rather than lost with the no-position block
    // that held the row.
    bool gpsRowReveal_ = false;
    // --- THE MACHINE'S SERIAL PORTS, the engine's (FOXAPI_OP_SCAN_SERIAL_PORTS, the SERIAL_PORTS list; window port group 14
    // part b). The window used to walk the registry itself in four places; it now ASKS - when the drop-down's arrow is
    // pressed, when SYSTEM > Serial ports opens or is refreshed, and once to seed the row's chip - and draws the list the
    // engine's pass answers with (a pass later: nothing is drawn of it, and no "none found", until the answer is in).
    // serialPorts() is the cache, read again only when listSeq moves.
    struct SerialPortsCache {
        std::uint64_t listSeq = 0;
        std::uint32_t refreshes = 0;       // times the list was read (a test reads it)
        std::vector<std::string> ports;
    };
    SerialPortsCache serialPortsCache_;
    const std::vector<std::string>& serialPorts();
    void askForSerialPorts();
    // True from the ask until the lists move (the scan's answer) or two seconds pass (a command that was never applied).
    bool serialPortsWaiting() const;
    bool serialPortsAsked_ = false;
    std::uint64_t serialPortsAskedSeq_ = 0;
    double serialPortsAskedAt_ = 0.0;
    bool serialPortsListLoaded_ = false;
    // Last frame's open/closed state, so drawSerialPortsSection can tell
    // "just opened" (ask) from "still open" (use the cache) from one
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
    // The target list's "On screen only" switch: list only the aircraft inside
    // the map as last drawn (MapView::isOnScreen) instead of every one heard.
    // Persisted (AppConfig::mapListOnScreenOnly) and app-wide for the same
    // reason the trail switches are: it is how this user wants lists to read,
    // not a statement about one plugin's window. (The window's half of the
    // config: applyConfig and currentConfig carry it, stub campaign B16.)
    bool mapListOnScreenOnly_ = false;
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
    // The engine's status lines as the last look read them (the table's
    // fact.status.<line>; before shell retirement R3 Engine::statusText's copy,
    // OPEN 3): refreshed once a frame by refreshStatusLines, after the frame's
    // publish and before the state is read; every panel letters from this,
    // never from the engine's fields.
    cascade::core::EngineStatusText statusLines_;
    // The look's status read: the 22 sentences from fact.status.*, the ADD ALL
    // run's level from the STATUS_LINES list, and - once, at the first look that
    // finds fact.store.url - the store's URL box.
    void refreshStatusLines();
    cascade::gui::patch::Interaction patchUi_;
    // THE PAGE'S DRAFT OF THE GRAPH (engine/stage3b-pre, docs/engine-stage3.md
    // OPEN 6, Design A; on the window's session since window port group 13, API
    // 0.3 8.12). The graph is the Engine's; the canvas, the faces and the
    // inspector draw from and edit this copy, and commitPatchDraft() hands the
    // whole of it to the engine as one FOXAPP_OP_PATCH_SET_GRAPH through the
    // session, naming the graph VERSION the draft was last in step with
    // (ival[1]) - the engine MERGES an edit whose version is not the current
    // one, so nothing it did meanwhile (START's switches, a learnt centre,
    // another session) is overwritten.
    //
    // THE RULES for any draft (docs/review-harness/engine-round3-review.md),
    // and what an asynchronous commit adds to them:
    //   - it FOLLOWS THE ENGINE. syncPatchDraft() reads the graph, its version
    //     and its epoch from ONE publish (patchLook) and, when the version is
    //     not the draft's base, takes the engine's graph - never mid-drag,
    //     resize or wire (patchInteracting()), when the drag's own node would
    //     jump;
    //   - an edit made while the engine moved on is put ON TOP of the
    //     engine's graph (core::patch::rebaseDraft), never the stale copy sent
    //     back over it;
    //   - it is NEVER A DEFAULT: until a publish has been read and adopted
    //     (patchDraftInStep_) nothing is ever committed from it, and a draft
    //     is never made "in step" with a graph that has not been published
    //     (frame 1 of an application whose patch is still queued);
    //   - A COMMIT IS NOT AN ADOPT. After a commit the published graph is the
    //     OLD one until the pass has applied it, so the draft keeps its edit
    //     and nothing is adopted until the command's own answer names the new
    //     version (patchSend_): a node dragged across several frames never
    //     snaps back. One commit is in flight at a time; edits made meanwhile
    //     stay in the draft and go with the next, from the new base.
    // drawUi commits once a frame as well, so an edit is never left behind by
    // a view that stopped drawing.
    cascade::core::patch::Graph patchDraft_;
    cascade::core::patch::Graph patchDraftBase_;
    std::uint64_t patchDraftVersion_ = 0;   // the version patchDraftBase_ is (0: none known)
    std::uint64_t patchDraftEpoch_ = 0;
    bool patchDraftInStep_ = false;
    // THE LOOK: fact.patch.graph, .version and .epoch read together, when
    // listSeq has moved. The graph is parsed again only when its version or
    // epoch changed. `valid` is false before the first publish and for a read
    // that straddled one (the version is read again after the graph).
    struct PatchLook {
        std::uint64_t listSeq = ~std::uint64_t{0};
        bool valid = false;
        std::uint64_t version = 0;
        std::uint64_t epoch = 0;
        cascade::core::patch::Graph graph;
        std::string text;
        std::string scratch;
    };
    PatchLook patchLook_;
    const PatchLook& patchLook();
    // THE PATCH PAGE'S READS OF THE TABLE (window port group 13, part b; API 0.3 8.12), taken when listSeq has moved:
    // the parts bin (PATCH_PARTS, as the DecoderInfo list the core's pure functions take), the plan the canvas marks and
    // the inspector words are drawn from (PATCH_PLAN, PATCH_PROBLEMS and fact.patch.runnable, as the Plan the core's
    // pure functions take), the radios (PATCH_RADIOS), the nodes the engine refused to run (PATCH_NODES), the devices a
    // Radio can be set to (PATCH_DEVICES) with the receiver's own radio and each Radio node's converter. What the pass
    // changed arrives a pass after it did; the page's own edits are in the draft, not here.
    struct PatchListCache {
        std::uint64_t listSeq = ~std::uint64_t{0};
        std::vector<cascade::core::patch::DecoderInfo> parts;
        cascade::core::patch::Plan plan;
        struct Radio {
            std::string device;              // the label of the device it is on
            std::string note;                // the line its face prints (why it did not open, or the note of a running one)
            double rateHz = 0.0;             // a running radio's own rate
            double centreHz = 0.0;           // ...and its AIR centre
            bool open = false;
            bool fault = false;
            bool pending = false;            // its open is still on the worker
        };
        std::map<cascade::core::patch::NodeId, Radio> radios;
        std::size_t radiosOpen = 0;
        std::vector<cascade::core::patch::NodeId> refused;   // decoder nodes whose module refused to start
        struct Device {
            std::string label;
            std::string key;
            double rateHz = 0.0;             // a recording's own rate
        };
        std::vector<Device> devices;         // in the Device combo's order
        std::string receiverRadio;           // fact.patch.receiverRadio
        bool receiverTuned = false;          // fact.patch.receiverCentre exists (its VALUE is the state's centreHz)
        std::map<cascade::core::patch::NodeId, cascade::core::ConverterSetting> conv;   // fact.patch.conv.<node>
        std::string factText;                // the fact as just read (kept for its room)
    };
    PatchListCache patchCache_;
    const PatchListCache& patchLists();
    const PatchListCache::Radio* patchRadio(cascade::core::patch::NodeId node);
    // WHAT THE RUNNING NODES ARE DOING NOW (PATCH_LIVE): read once a frame by the page that draws it (the list moves no
    // listSeq), into rows that keep their room.
    struct PatchLiveRow {
        std::uint64_t node = 0;
        int kind = 0;                        // 1 squelch, 2 speaker, 3 channel level, 4 decoder face, 5 speaker error
        double v0 = 0.0, v1 = 0.0;
        bool open = false;
        std::string name, detail;
    };
    std::vector<PatchLiveRow> patchLive_;
    std::size_t patchLiveCount_ = 0;
    void readPatchLive();
    const PatchLiveRow* patchLiveRow(int kind, cascade::core::patch::NodeId node) const;
    // A radio's spectrum, read once a frame at the most (read_node_spectrum, the cursor the newest sequence held);
    // null with no radio running there.
    struct PatchSpectrumCache {
        std::vector<float> db;
        std::uint64_t seq = 0;
        std::uint64_t readFrame = 0;
        bool have = false;
    };
    std::map<cascade::core::patch::NodeId, PatchSpectrumCache> patchSpectrumCache_;
    std::vector<float> patchSpectrumScratch_;
    std::uint64_t patchFrame_ = 0;           // bumped by readPatchLive
    const PatchSpectrumCache* patchSpectrum(cascade::core::patch::NodeId radio);
    // The newest lines each Text out has been shown (200 at most, "<source>: <text>"), routed from the window's copy of
    // DECODER_LINES by the draft's wires: a decoder shows on every Text out it is wired to.
    std::map<cascade::core::patch::NodeId, std::deque<std::string>> patchTextOutLines_;
    std::uint64_t patchSinkRoutedId_ = 0;
    void routePatchSinkLines();
    // THE COMMIT IN FLIGHT (stop-and-wait). Sent: submitted, not answered;
    // Applied: answered OK with the new version, waiting for the publish that
    // shows it to be read. `sent` is the draft as it was sent, the base of the
    // rebase that puts the edits made since on top of what the engine now holds.
    struct PatchSend {
        enum class State { Idle, Sent, Applied };
        State state = State::Idle;
        std::uint64_t ticket = 0;
        std::uint64_t newVersion = 0;
        bool document = false;
        cascade::core::patch::Graph sent;
    };
    PatchSend patchSend_;
    // The commit was refused (or its base had moved on): the next valid look
    // is adopted whole.
    bool patchAdoptWanted_ = false;
    // The answer to the graph command, from pumpTransmitKey's drain.
    void onPatchGraphResult(const FoxCommandResult& r);
    void settlePatchSend();
    void adoptPatchLook(const PatchLook& l, bool documentChanged = true);
    void forgetPatchInteraction();
    // True while the button is held on a drag, a resize or a wire; a state
    // left behind by a release the canvas never saw is cleared here.
    bool patchInteracting();
    void syncPatchDraft();
    void commitPatchDraft();
    // A NEW DOCUMENT for the engine (the config's patch, a patch file): sent
    // flagged FOXAPP_PATCH_GRAPH_DOCUMENT, and the draft is the document at once
    // and then what the engine holds when it has answered.
    bool loadPatchDocument(const cascade::core::patch::Graph& g);
    // A Radio part waiting for the native scan its press asked for: the node is
    // in the draft with no device, and gets one when the scan's answer arrives
    // (finishPatchRadioParts, from syncPatchDraft).
    struct PatchRadioWait {
        cascade::core::patch::NodeId node = cascade::core::patch::kNoNode;
        std::uint64_t scanTicket = 0;
    };
    std::vector<PatchRadioWait> patchRadioWaits_;
    std::uint64_t patchScanDone_ = 0;   // the newest SCAN_NATIVE_ONLY ticket answered
    void finishPatchRadioParts();
    bool patchSeeded_ = false;
    bool patchOpenedByEnv_ = false;
    bool patchStartedByEnv_ = false;
    // FOXSDR_SOURCE_DEVICE (bounded runs only), sent on the first frame as TWO COMMANDS in one drain (shell retirement R2, docs/engine-stage4.md 6.25):
    // FOXAPP_OP_SCAN_NATIVE_ONLY (scanNative() and nothing else, as the old door seam's scan was) and then FOXAPI_OP_SELECT_SOURCE with the id the key
    // names ("<driver>:<args>", the key's first '|' turned into ':'; Engine::sourceIdForRow spells a native row's id so). The pass applies them in
    // that order, so the second finds the row the first listed - which is what the seam did on the pass. (The survey's multi-frame machine - scan, wait
    // for the answer, read the DEVICES list, then select - was built first and put the deck in "no radio" for the frames the waiting took, which
    // tests/test_deck_knobs_app holds never to happen: the seam's selection was applied by the FIRST frame's pass.)
    bool sourceDeviceByEnv_ = false;
    // FOXSDR_PRESS_PRESET: the named plugin's first preset by its key's own command, or - for a plugin with none - the plugin pinned to run and its windows
    // opened. Read from the PLUGINS and PRESETS lists (pluginLists()); the commands go on the window's session.
    void pressPresetByEnv(const char* want);
    bool patchFileLoaded_ = false;
    // The patch as core/patch_io.hpp writes it, rebuilt only when the
    // canvas says it changed. currentConfig() runs every frame and must
    // not serialise a document on each one.
    std::string patchText_;
    // THE PATCH PRESETS (stub campaign B17; core/patch_presets.hpp). The named presets and the previous-patch slot are the WINDOW'S half of
    // the config: from AppConfig at applyConfig, back into it at currentConfig, changed only by the PRESETS panel (drawPatchPresetsPage).
    // Saving stores the patch on the canvas as patch_io writes it NOW - the page's own draft, which holds every edit, not patchText_, which
    // catches up only at the end of the page's frame. Loading (loadPatchPreset) goes through replacePatch: a running patch is stopped
    // through its own STOP (FOXAPI_OP_PATCH_RUN 0, applied by the pass ahead of the new document) and the new patch is left STOPPED - the
    // user presses START - and, once the new patch is in, the one it replaced is kept in the "(previous patch)" slot, so a load is always one
    // load away from being undone. `text` is a preset's text or the slot's own; loading the slot swaps it with the canvas. False, and
    // nothing changed, when the text is not a patch. app_window_patch_presets.cpp.
    cascade::core::PatchPresetStore patchPresets_;
    cascade::core::PatchPresetStatus savePatchPreset(const std::string& name, bool overwrite);
    bool loadPatchPreset(std::string text);
    std::string serialisePatchNow() const;
    // A NEW PATCH ON THE USER'S SAY (a preset): parsed first - a text that is not a patch changes nothing at all, not a running patch, not a
    // face - then STOP through the session, the document through loadPatchDocument (the engine takes it as a new epoch, ids from 1 again),
    // and every per-node memory of the window dropped with the graph it described: node ids start again, so anything remembered by id would
    // attach to whichever new node took the number.
    bool replacePatch(const std::string& text);
    // THE PRESETS PANEL: a page of its own, opened by the PRESETS key in the patch page's control row (drawPatchTransport) and drawn only
    // while the patch face is showing. The list (click to select, double-click or LOAD to load), SAVE AS... with a name field, RENAME and
    // DELETE; an overwrite and a delete each ask first.
    void drawPatchPresetsPage();
    void drawPatchPresetsBody();
    // The sentence the panel shows for what a save, rename, delete or load answered; `name` fills the sentences that name the preset.
    std::string patchPresetSentence(cascade::core::PatchPresetStatus st, const std::string& name) const;
    // Loads the selected row - a preset or the previous-patch slot - and says what happened.
    void loadSelectedPatchPreset();
    bool patchPresetsOpen_ = false;
    // The selected row: a preset by its name, or the previous-patch slot.
    std::string patchPresetSel_;
    bool patchPresetSelPrevious_ = false;
    // The name field SAVE AS... and RENAME open, and which of them it is for.
    enum class PatchPresetEdit { None, SaveAs, Rename };
    PatchPresetEdit patchPresetEdit_ = PatchPresetEdit::None;
    char patchPresetName_[cascade::core::kMaxPatchPresetNameBytes + 1] = {};
    bool patchPresetFocusName_ = false;
    // A question waiting for its answer: replace the preset named patchPresetPending_, or delete it.
    bool patchPresetAskOverwrite_ = false;
    bool patchPresetAskDelete_ = false;
    std::string patchPresetPending_;
    // What the last action did, in a sentence under the list.
    std::string patchPresetNote_;
    // What each planned channel is hearing this frame. Rebuilt per frame
    // rather than kept, because a stale level is worse than none: it reads
    // as a live measurement of a channel that may no longer exist.
    std::vector<cascade::gui::patch::NodeReading> patchReadings_;
    // Each Spectrum node's waterfall memory, and the spectrum frame it last
    // took a row from - a row per published frame, not per GUI frame, so the
    // waterfall scrolls at the same pace as the main one.
    std::map<cascade::core::patch::NodeId, cascade::gui::patch::ScopeHistory> patchScopes_;
    std::map<cascade::core::patch::NodeId, std::uint64_t> patchScopeSeq_;
    // Each picture decoder node's newest picture, as a GL texture. The PICTURE is the engine's (API 0.3 8.12, part c):
    // PATCH_LIVE's picture row (kind 6) names the newest picture's id, and when the id moves the face reads that picture
    // with read_image into patchPictureScratch_ and uploads it - on this thread, where the GL context is current - then
    // lets the pixels go; only the texture and the figures it was made from are kept. Textures are deleted when the node
    // goes (or its row does) and at shutdown, while the context is current.
    struct PatchPicture {
        unsigned int tex = 0;
        std::uint64_t imageId = 0;   // the picture the texture shows (0: none yet)
        std::uint32_t width = 0, height = 0;
    };
    std::map<cascade::core::patch::NodeId, PatchPicture> patchPictures_;
    std::vector<std::uint8_t> patchPictureScratch_;
    void releasePatchPictureTextures();
    // Reads the picture `live` names and uploads it (patchPictureGl_.upload); false, and the old texture stays, when the
    // picture has moved on already (NOT_FOUND: the next frame's row names the newer one) or is not a picture.
    bool takePatchPicture(PatchPicture& pic, const PatchLiveRow& live);
    // The GL half, a pair of function pointers so a test that has no GL context can stand in for it (the scope badge's
    // precedent): make or refill the texture and return its id; delete one.
    struct PatchPictureGl {
        unsigned int (*upload)(unsigned int tex, std::uint32_t width, std::uint32_t height, std::uint32_t format,
                               const std::uint8_t* pixels) = nullptr;
        void (*destroy)(unsigned int tex) = nullptr;
    };
    static unsigned int uploadPatchPictureGl(unsigned int tex, std::uint32_t width, std::uint32_t height,
                                             std::uint32_t format, const std::uint8_t* pixels);
    static void destroyPatchPictureGl(unsigned int tex);
    PatchPictureGl patchPictureGl_{&AppWindow::uploadPatchPictureGl, &AppWindow::destroyPatchPictureGl};
    // Where the patch canvas was drawn last frame, in ImGui screen space:
    // what lets an input script aim at a node by its patch coordinates.
    float patchCanvasOriginX_ = 0.0f;
    float patchCanvasOriginY_ = 0.0f;

    // --- the scripted pointer (bounded runs only; see gui/input_script.hpp) ---
    std::vector<cascade::gui::ScriptStep> inputScript_;
    std::size_t inputScriptPos_ = 0;
    bool inputScriptActive_ = false;
    // FOXSDR_SCRIPT_TRACE=<path>: one line a frame while a script runs - the pointer, both buttons, the OS focus, and what ImGui made of them
    // (the hovered window, the hovered and active item) - so a scripted click that did not take leaves its evidence behind. Empty = off.
    std::string scriptTracePath_;
    void traceScriptFrame(long frame);
    bool scriptMouseSet_ = false;
    float scriptMouseX_ = 0.0f;
    float scriptMouseY_ = 0.0f;
    // A scripted "ctrlwheel" step spans the frame it was read on plus the two that follow, the way a real held Ctrl key does - see
    // applyInputScript's CtrlWheel case. -1 means nothing is pending. "shiftwheel" (0.99.82) takes the same three frames with Shift in
    // Ctrl's place; pendingWheelMod_ is the modifier the pending step is holding.
    long pendingCtrlWheelFrame_ = -1;
    float pendingCtrlWheelY_ = 0.0f;
    bool pendingCtrlWheelSent_ = false;
    int pendingWheelMod_ = 0;  // an ImGuiKey: ImGuiMod_Ctrl or ImGuiMod_Shift
    // THE SCRIPT IS THE ONLY POINTER AND KEYBOARD (2026-10-01): ImGui's input event id from which events in the queue were NOT put there by
    // the script, and how many such events the last frame discarded (traced). See the top of applyInputScript.
    unsigned int scriptNextEventId_ = 0;
    int scriptDroppedEvents_ = 0;
    // Feeds this frame's steps into ImGui's input queue. Called between the
    // platform backend's NewFrame and ImGui::NewFrame, so the script's events
    // are the last word on the frame.
    void applyInputScript(long frame);
    // The controls on every node's face, drawn over the canvas after it:
    // the node IS the instrument, so what it is set to is set on it.
    void drawPatchFaces(float originX, float originY, float width, float height);
    // THE CANVAS'S OWN "+", "-" AND "Fit" KEYS (0.99.49 beta feedback: a laptop touchpad has no middle button to pan with, and nothing on the
    // page said how to zoom). Bottom-right of the canvas, over it. Stub campaign B17; app_window_patch.cpp.
    void drawPatchViewKeys(float originX, float originY, float width, float height);
    // THE PATCH PAGE'S LAYOUT (0.99.49 beta feedback; gui::patch::patchInfoPaneWidth and friends). The information pane - the patch's
    // state, the selection's problems, the help - sits in the top band's right-hand part at a fixed height with its own scroll; the selected
    // node's controls are in a drawer at the canvas's right that folds to a strip. patchBandRight_ is the screen x the band's rows keep short
    // of. The pane reads the engine's plan (patchLists().plan) and this frame's readings; it changes nothing.
    void drawPatchInfoPane(float x, float y, float w, float h);
    // THE BLUEPRINTS LIST (owner, 2026-10-26: "select in the list adsb and it places a full work flow for that app on the patch panel"; master 0.99.94
    // and 0.99.96, app_window_patch_blueprints.cpp). One pick places a whole wired workflow for one plugin, tuned by the plugin's own preset;
    // core/patch_blueprints.hpp decides what is built, this draws the list on the parts band and says what happened. ON THE TABLE: the parts are the
    // PATCH_PARTS copy (patchLists().parts), the presets are PRESETS (value[3], the preset's sample rate, is the 0.99.97 merge's addition), the
    // towers are BOOKMARKS' groups and the receiver's position, and the workflow is placed on the page's own draft and committed as every edit is.
    void drawPatchBlueprints();
    // Every blueprint the parts bin's catalogue makes, with each plugin's presets read out of the table now - asked only while the list is open.
    std::vector<cascade::core::patch::Blueprint> patchBlueprints();
    // A pick is REMEMBERED HERE and placed once the canvas has a size, as a press of a part is, so the view can be moved to show what landed.
    void placePendingPatchBlueprint(float canvasW, float canvasH);
    std::optional<cascade::core::patch::Blueprint> patchBlueprintPending_;
    // What the last pick did, in a sentence beside the list.
    std::string patchBlueprintNote_;
    void drawPatchInspectorFoldKey();
    float patchBandRight_ = 0.0f;
    bool patchInspectorFolded_ = false;
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
    std::string patchDefaultDeviceKey();
    // A Radio PART dropped from the bin (engine/stage3b-pre B1, 2026-09-28):
    // starts on a free native radio, so it needs the native list first. THE
    // SCAN AND THE READ ARE TWO PASSES since window port group 13: the node is
    // created at once with no device (so the press shows), the scan is asked
    // for through the session, and finishPatchRadioParts gives the node its
    // device when the scan's answer arrives - by which time the engine has the
    // list patchDefaultDeviceKey() reads. (The queued scan read on the next
    // line, B1's bug, is the case the wait exists for.)
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
    // The radar scope's gain knob: the figure its last detent sent and the engine's readback has not yet shown
    // (gui/scope_gain.hpp: the next detent computes from it, so two detents before a pass publishes are two).
    cascade::gui::ScopeGainLocal scopeGainLocal_;
    // ...and what the knob last lettered (gui/scope_gain.hpp: ScopeKnobView), which a test reads.
    cascade::gui::ScopeKnobView scopeKnob_;

    bool transmitPttHeld_ = false; // this frame's key request, rebuilt each frame
    bool transmitPageLive_ = false;  // page drawn with its controls THIS frame (not rolled up)
    // THE KEY THROUGH THE WINDOW'S OWN ENGINE API SESSION (engine stage 4,
    // API 0.2's THE KEY - docs/engine-stage4.md decision 1). The page no
    // longer hands the engine a per-frame request: it SUBMITS its key
    // (pumpTransmitKey, once a frame) on a LOCAL session the window opened on
    // the engine at start-up, and heartbeats that session once a frame -
    // which is what keeps any key the window holds:
    //   - PTT 1 every frame while held (a 2 s hold, re-asserted), PTT 0 once
    //     when let go;
    //   - the LATCH key is EDGE-triggered: a press on a key shown released
    //     sends LATCH 0 then LATCH 1 in one batch, a press on a key shown
    //     latched sends LATCH 0;
    //   - "Allow remote transmit" sends TX_REMOTE_ARM 1/0 - the local
    //     operator's consent the web remote's PTT now needs;
    //   - the page going away (closed, rolled up) lets go of all three.
    const FoxEngineApi* engineApi_ = nullptr;
    FoxSession* uiSession_ = nullptr;
    // The engine the session is on (the handle the table's engine-level calls take: get_local_extension). Set where the session is opened.
    FoxEngine* uiEngine_ = nullptr;
    FoxReceiverState uiState_{};     // read once a frame, after the publish
    bool transmitLatchPressed_ = false;  // this frame
    int transmitArmRequest_ = -1;        // this frame: -1 none, 0 withdraw, 1 consent
    cascade::gui::TxKeyMemory transmitKey_;  // between frames (gui/transmit_page.hpp)
    std::string transmitKeyNote_;        // the last refusal of a key command, for the page
    // A PLUGIN MAY KEY THE TRANSMITTER, THE WINDOW'S HALF (stub campaign B13; app_window_transmit.cpp). The grant, the arming window and the key are the ENGINE's (engine/
    // engine_tx_plugin.cpp): what is here is the page that asks for them and the frame that tells the engine the page is still on screen. The window holds NO state that could
    // key a radio: it draws fact.tx.plugin.* and sends FOXAPP_OP_TX_PLUGIN_ARM / _PLUGIN_TX_GRANT / _TX_DATA_FLAT, all LOCAL ops that are not on the web profile.
    int transmitPluginArmRequest_ = -2;            // this frame: -2 none, 0 OFF pressed, 5 / 15 / 60 minutes (rebuilt from nothing every frame)
    cascade::gui::TxPluginArmMemory transmitPluginArm_;   // between frames: when an ARM was last sent (gui/transmit_page.hpp)
    bool transmitOpenedByEnv_ = false;             // FOXSDR_OPEN_TRANSMIT, bounded runs
    void drawTransmitPluginBlock();                // the page's ARM FOR PLUGINS keys, the arming line and DATA (flat)
    void submitTransmitPluginArming();             // once a frame, after the key's own submit
    void openUiSession();
    void closeUiSession();
    void refreshUiState();
    // THE KEY'S SUBMIT AND THE ANSWERS' DRAIN, which pumpTransmitKey did together at the END of the frame (the pump move's step P4;
    // the survey's 1.6 B). The rule for the look is: the session's results and events FIRST, then the state, the spectrum and the lists -
    // the table promises a result is never readable before the snapshot that shows it, which only helps a reader that reads the result
    // before the state - and the answers are ACTED ON after the state is read, so each one is handled against a state at least as new as
    // the snapshot that raised it (an event handler that reads the lists the event announces must read them after the state that moved
    // their sequence). pollUiAnswers takes them into a fixed buffer, actOnUiAnswers handles that buffer; drawUi runs the first before the
    // look and the second after it, and submitTransmitKey at the end of the frame. pumpTransmitKey stays as the three in turn for a
    // caller (a test frame) that has no look of its own to put them around.
    void submitTransmitKey();
    void pollUiAnswers();
    // `stateRead`: for a caller that has no look of its own (pumpTransmitKey) - act on the buffer whatever the last state read did. A frame
    // (drawUi) passes nothing: the answers are acted on only against a state this frame's look READ (uiStateFresh_), else they wait.
    void actOnUiAnswers(bool stateRead = false);
    void pumpTransmitKey();
    // The frame takes a file dropped on the window (pendingDropPath_) as an import command on the window's session (the pump move's step P4).
    void submitPendingDrop();
    struct UiAnswers {
        static constexpr int kMaxResults = 64;
        static constexpr int kMaxEvents = 16;
        FoxCommandResult results[kMaxResults];
        FoxEvent events[kMaxEvents];
        int nResults = 0;
        int nEvents = 0;
    };
    UiAnswers uiAnswers_;
    // DID THIS LOOK'S STATE READ SUCCEED (refreshUiState)? A read that keeps overlapping a publish answers BUSY and the last good state
    // stands - older than the snapshot that raised the answers the frame has just polled. Acting on them against that state breaks the
    // rule the look's order exists for (found by the P6 soak of test_look_order: 4 runs in 50 saw, in about 1 answer in 100 000, a state older
    // than the figure the answer names - a BUSY read, the publishing thread preempted mid-write). So a frame whose read failed LEAVES
    // THE ANSWERS in their buffer (pollUiAnswers keeps what is not yet acted on) and acts on them at the next look that did read.
    bool uiStateFresh_ = true;
    std::uint64_t uiStateBusyReads_ = 0;   // reads that answered BUSY (the answers they would have been acted on against were held back)
    // A test's tap on the answers the window acts on (null: none): called for each result with the state the window had read when it
    // acted - which tests/test_look_order.cpp holds to be at least as new as the snapshot that raised the result.
    std::function<void(const FoxCommandResult&)>* uiAnswerTap_ = nullptr;
    // THE WINDOW'S COMMANDS ON ITS SESSION (the engine-stage4 window port):
    // one command, submitted for the engine's next pass - its long text, when
    // it has one, through submit_text (API 0.3). The answers are drained once
    // a frame with the key's (pumpTransmitKey).
    void uiSubmit(const cascade::core::cmd::QueuedCommand& q);
    // uiSubmit that answers the ticket the engine gave the command (0 when the
    // session is not there, or would not take it): what a caller that waits
    // for the command's answer needs.
    std::uint64_t uiSubmitTicket(const cascade::core::cmd::QueuedCommand& q);
    void uiSubmit(const FoxCommand& c) { uiSubmit(cascade::core::cmd::QueuedCommand{c, {}}); }
    // TEST SEAM, null in every build of the application and read at exactly one place
    // (uiSubmit): every command uiSubmit takes is appended here, in order, before it is
    // sent. It lets a test hold the window's commands against the ones its members always
    // sent without an engine pass applying them - a key that starts the receiver, or a tune.
    std::vector<cascade::core::cmd::QueuedCommand>* uiSubmitTap_ = nullptr;
    // Commands uiSubmit could not hand over because the session was full (BUSY): counted, the first said in the log (uiSubmitTicket).
    std::uint64_t uiBusyDrops_ = 0;
    // THE NEWEST FIGURE OF A SETTER WINS when the session is full (the pump move's step P5, found by test_window_threaded_look G: the engine's
    // queue merges only the commands that make the key SAFER, so a drag that sends a figure every frame fills the 256 slots in 256 frames
    // while a pass is stalled - 0.2 s on a display that does not pace the frame - and the figures that would have mattered, the LAST ones,
    // are the ones refused). A setter of this kind (latestWins) the session refuses with BUSY is HELD instead - one per op, a newer figure
    // replacing an older - and offered again once a frame (pollUiAnswers) until the session has room. Everything else is refused as ever
    // (counted, the first said in the log).
    std::map<std::uint32_t, cascade::core::cmd::QueuedCommand> uiHeld_;
    static bool latestWins(std::uint32_t op);
    void flushHeldSetters();
    // THE USAGE REPORT'S PANEL NAMES (window port, group 3): a page calls
    // this every frame it is open, and the name goes to the engine ONCE a run
    // as FOXAPP_OP_NOTE_PANEL. uiNotedPanels_ is the names already sent, kept
    // sorted: a repeat is one binary search with no allocation (the window has
    // 21 page names, so it never holds more).
    void uiNotePanel(const char* name);
    std::vector<std::string> uiNotedPanels_;
    // A FACT (API 0.3, get_setting("fact.<key>")), cached until the lists'
    // publish moves listSeq - read on the first ask after that, so a page
    // drawn every frame costs nothing between changes. "" when the engine
    // has none.
    const std::string& uiFact(const char* key);
    // (std::less<>: a lookup by C string builds no temporary std::string, so
    // a fact read every frame - the deck's bias tee key reads one - allocates
    // nothing.)
    std::map<std::string, std::string, std::less<>> uiFacts_;
    std::uint64_t uiFactsListSeq_ = 0;
    // WHAT THE STATUS COLUMN TAKES FROM THE LISTS (window port, group 2), read
    // when listSeq moves and kept here, the way uiFacts_ is: the RECEIVER
    // card's first gain stage (its name, its value, its unit token read back
    // as a GainUnit), the antenna in use, and where the two recorders' takes
    // sit in the RECORDINGS list (kind 1 I/Q, 2 audio; -1 none) - whose LIVE
    // figures the column reads each frame, one read_list apiece. Everything
    // else on the column is the state, read once a frame.
    void refreshStatusLists();
    std::uint64_t statusListsSeq_ = 0;   // listSeq the members below were read at; 0: never
    bool statusGainHave_ = false;        // the GAINS list has a stage 0
    std::string statusGain0Name_;
    double statusGain0Db_ = 0.0;
    double statusGain0Lo_ = 0.0, statusGain0Hi_ = 0.0, statusGain0Step_ = 0.0;   // ...its travel and its step (the top bar's GAIN dial turns over them)
    cascade::source::GainUnit statusGain0Unit_ = cascade::source::GainUnit::Decibels;
    std::string statusAntenna_;          // the ANTENNAS entry that is ACTIVE; "" none
    int statusTakeIq_ = -1;
    int statusTakeAudio_ = -1;
    // A TAKE, READ LIVE (the status column's RECORDER card and the Recorder section, window port
    // group 7): the RECORDINGS entry at `index` if it is still the recorder of `kind` (1 I/Q, 2
    // audio) - its flags, its bytes and its seconds, as of this read. All zero for a take that
    // never started.
    struct TakeNow {
        bool active = false;
        bool stalled = false;
        bool full = false;
        double bytes = 0.0;
        double seconds = 0.0;
    };
    TakeNow readTake(int index, double kind);
    // WHAT THE RECEIVER RAIL AND THE DISPLAY TAKE FROM THE LISTS AND FACTS
    // (window port, group 5, API 0.3 section 8.4), read when listSeq moves and
    // kept: the plans SET_BAND_PLAN takes (BAND_PLANS) and the one in force, the
    // audio outputs (AUDIO_DEVICES) and the one in use, the loaded plan's name
    // (fact.bandPlan.name), and the two status lines the rail letters
    // (fact.status.bandPlanError, fact.status.audioHealth) - read into strings
    // that keep their room, so a refresh allocates only when something grew.
    //
    // THE LOADED PLAN'S ROWS (BAND_PLAN: hundreds of them, walked every frame by
    // the overlay and the footer) are copied ONCE: listSeq also moves when a
    // decoder prints a line, so it is not the plan's own signal. They are read
    // again only when the plan changed - the number of rows, the plan in force
    // or its name, which are what the engine's own fingerprint hashes for it.
    // railLists() is the one call a member makes; listSeq 0 is "never read".
    struct RailListCache {
        std::uint64_t listSeq = 0;
        std::uint32_t refreshes = 0;       // times everything but the rows was read again (a test reads it)
        struct Plan {
            std::string id;                // what SET_BAND_PLAN takes
            std::string name;              // the plan's own name, untranslated
        };
        std::vector<Plan> plans;
        int activePlan = -1;               // the entry that is ACTIVE; -1: none ("all installed")
        struct Sink {
            std::string name;              // cut at the list item's size (95 bytes)
            int index = 0;                 // what FOXAPI_OP_AUDIO_DEVICE takes
        };
        std::vector<Sink> sinks;
        int activeSink = -1;
        std::string bandPlanName;          // fact.bandPlan.name: the loaded plan, a chain joined with " + "
        std::string bandPlanError;         // fact.status.bandPlanError
        std::string audioHealth;           // fact.status.audioHealth
        int audioBufferMs = 0;             // fact.audio.bufferMs: the chosen buffer, 0 AUTOMATIC (0.99.73)
        int audioLeadMs = 0;               // fact.audio.leadMs: the lead in force (the AUTOMATIC preview's number)
        std::string audioLeadNote;         // fact.audio.leadNote: "raised|restored|lowered <ms> <count>", "" nothing
        std::vector<cascade::gui::BandRow> bands;
        int bandsCount = -1;               // what the rows were read for: their number...
        std::string bandsPlanId;           // ...the plan in force...
        std::string bandsPlanName;         // ...and its name
        std::uint32_t bandReads = 0;       // times the rows were copied (a test reads it)
    };
    RailListCache railCache_;
    const RailListCache& railLists();
    // The Sinks rail's note about what AUTOMATIC last did to the buffer (translated), from fact.audio.leadNote; "" when nothing.
    std::string audioLeadNotice() const;
    // A fact's text into `out` (cleared first, "" when the engine has none), whole:
    // what uiFact caches, for a reader that keeps its own string. True when the
    // engine HAS the fact (even an empty one), false when it answers NOT_FOUND
    // (before its first publish) or DENIED.
    bool readFactText(const char* key, std::string& out);
    // THE LANGUAGE SECTION'S NOTE ABOUT THE BAND PLAN (gui/rail_reads.hpp): what
    // a country picked there asked the engine for, and when.
    cascade::gui::BandPlanAsk bandPlanAsk_;
    // THE TUNING SURFACE (window port, group 6: drawToolbar, drawFrequencyReadout,
    // applyKeyAction, drawCenterPanels and the bookmark markers' reader; API 0.3
    // section 8.5, gui/tuning_reads.hpp).
    //
    // WHAT IT TAKES FROM THE LISTS AND FACTS, read when listSeq moves and kept (the
    // rail's rule): the DSP chain's input rate (fact.input.rateHz - the spectrum's
    // axis and the VFO's reach are measured against it, and it is not the source's),
    // and how many decoders are being fed and the first one's name (DECODERS, for the
    // waterfall's caption).
    //
    // THE BOOKMARK MARKERS are copied ONLY when the store changes: listSeq also moves
    // every time a decoder prints a line, so it is not the store's signal - the
    // store's own version (fact.bookmarks.version) is. A 33 000-entry list is copied
    // once per change and not once a pass. tuningLists() is the one call a member
    // makes; listSeq 0 is "never read".
    struct TuningListCache {
        std::uint64_t listSeq = 0;
        std::uint32_t refreshes = 0;       // times everything but the rows was read again (a test reads it)
        double inputRateHz = 0.0;          // fact.input.rateHz; 0 until read
        cascade::gui::FedDecoders fed;     // DECODERS entries that are FED, and the first of them
        std::vector<cascade::gui::MarkerRow> markers;  // BOOKMARKS, in its order: by frequency
        std::string markersVersion;        // fact.bookmarks.version the rows were read at; "" never
        std::string factText;              // the fact as just read (kept for its room)
        std::uint32_t markerCopies = 0;    // times the rows were copied (a test reads it)
    };
    TuningListCache tuningCache_;
    const TuningListCache& tuningLists();
    // THE DECODERS, THE PRESETS, THE MARKS, THE MUTE CLUSTER AND THE GRANTS (window port
    // group 8: drawDecodersSection, drawDecoderStatusRows, drawPluginPresets, drawPresetKeys,
    // drawDecoderPresetBars, drawPluginMarkers, drawPluginTuneControls, drawMutePopup,
    // muteBannerSubject, drawMuteBanner and the lookup of a plugin's presets; API 0.3 section
    // 8.7, gui/decoder_reads.hpp). What they read of the table is kept here and read again ONLY
    // when listSeq moves (and, since D3, a decoder line does not move it): PLUGINS merged with
    // its DECODERS, PRESETS and USER_PRESETS entries into one row a plugin
    // (cascade::gui::PluginRow), MARKERS (as the plugin ABI's CascadeMarker rows
    // pluginMarkerGeometry takes), MUTE_CAUSES, STALE_GRANTS, and the three facts
    // (fact.mute.subject, fact.mute.keptRunning, fact.plugin.lastDenied) into strings that keep
    // their room - so a refresh that finds nothing bigger allocates only the table's own
    // get_setting text. pluginLists() is the one call a member makes; listSeq 0 is "never read".
    struct PluginListCache {
        std::uint64_t listSeq = 0;
        std::uint32_t refreshes = 0;       // times the lists were read again (a test reads it)
        std::vector<cascade::gui::PluginRow> plugins;
        cascade::gui::ModuleCensus decoderCensus;  // gui::kDecoderCaps over the rows (module_census.hpp's rule)
        std::vector<CascadeMarker> markers;
        std::vector<cascade::gui::StaleGrantRow> staleGrants;
        std::vector<cascade::gui::MuteCauseRow> muteCauses;
        bool muteKeptRunning = false;      // fact.mute.keptRunning
        std::string muteSubject;           // fact.mute.subject
        std::string lastDenied;            // fact.plugin.lastDenied
        std::string factText;              // the fact as just read (kept for its room)
        std::string factKey;               // the key of a per-file fact being read (kept for its room)
    };
    PluginListCache pluginCache_;
    const PluginListCache& pluginLists();
    // THE DECODER OUTPUT (the Decoder output window and the rail's line count): the window's copy of
    // DECODER_LINES. The ring is read ONLY when the state's decoderLinesTotal has moved (a decoder
    // line moves nothing else, D3), and only the entries whose id is above the highest it holds are
    // copied - never the ring again. Clear empties the copy and the ids at or below the highest it
    // held are never taken again. Bounded at kDecoderLogMax, the newest kept, as the engine's tail was.
    struct DecoderLineRow {
        std::uint64_t id = 0;              // the line's number
        std::uint64_t node = 0;            // the patch decoder node that said it (value[0]); 0 = the receiver's own
        std::string plugin;                // cut at the list item's 95 bytes
        std::string text;                  // cut at its 159
    };
    struct DecoderLineCache {
        std::deque<DecoderLineRow> lines;
        std::uint64_t seenTotal = 0;       // decoderLinesTotal the ring was last read at
        std::uint64_t lastId = 0;          // the highest id copied (or cleared)
        std::uint32_t reads = 0;           // times the ring was read (a test reads it)
        std::uint64_t copied = 0;          // lines copied, in all (a test reads it)
        std::vector<DecoderLineRow> fresh; // scratch for one read, newest first
    };
    DecoderLineCache decoderLineCache_;
    const DecoderLineCache& decoderLines();
    void clearDecoderLines();
    // THE TUNE-AWAY QUESTION (API 0.3 8.7, FOXAPI_EVENT_MUTE_TUNE_AWAY): the session subscribes to the
    // event; pumpTransmitKey drains the session's events once a frame (pumpUiEvents) and, on one,
    // askAboutMutedPlugins captures the names and the module keys from MUTE_CAUSES - the dialog is about
    // THOSE plugins, however the lists move on - and asks drawMutePopup to open. The dialog closes when
    // fact.mute.keptRunning reads 0 again (the engine's own withdrawal) or the operator answers.
    void pumpUiEvents();
    void handleUiEvent(const FoxEvent& ev);
    void askAboutMutedPlugins();
    bool mutePopupOpen_ = false;           // the question is up (or about to be)
    bool mutePopupOpenRequest_ = false;    // ImGui::OpenPopup is due on the next drawMutePopup
    std::vector<std::string> mutePopupNames_;
    std::vector<std::string> mutePopupKeys_;
    // THE SPECTRUM, refreshUiState's second read: ONE read_spectrum a frame into
    // lastFrame_, and the waterfall fed from it - one row for each NEW sequence and none
    // for a NO_CHANGE. spectrumArrived_ says a frame was taken since drawUi last
    // stamped lastFrameSeenS_ (ImGui is not available to refreshUiState: tests call it
    // with no context).
    void readSpectrum();
    bool spectrumArrived_ = false;
    // WHAT A COMMAND SENT THIS FRAME WILL HAVE DONE, until the state shows it: the VFO
    // and bandwidth drags settle the band BEFORE the spectrum is drawn, because the
    // passband the panel measures PEAK IN PASSBAND over and paints must be the one the
    // drag just set - and a command lands at the engine's next pass. So the drag keeps
    // the numbers it sent (clamped by the engine's own limits, which the engine will
    // apply to the same figures) until the state shows them: refreshUiState drops one when
    // the readback has come to EQUAL it or a moment (kTuneLocalHoldS) has gone by - not "when
    // the next state is read", which was right only while the frame was the pass (the pump
    // move's step P4; gui/tune_local.hpp).
    struct TuneLocal {
        cascade::gui::TuneLocalFigure offset;
        cascade::gui::TuneLocalFigure bandwidth;
    };
    TuneLocal tuneLocal_;
    double tunedVfoOffsetHz() const { return tuneLocal_.offset.valid ? tuneLocal_.offset.hz : uiState_.vfoOffsetHz; }
    double tunedBandwidthHz() const { return tuneLocal_.bandwidth.valid ? tuneLocal_.bandwidth.hz : uiState_.bandwidthHz; }
    // THE WINDOW'S CLOCK for the kept figures: seconds on a steady clock. A member so a test can drive it (ImGui's own is not
    // available to refreshUiState, which tests call with no context).
    static double steadyNowS();
    double (*uiClockS_)() = &AppWindow::steadyNowS;
    // The two drag steps (drawCenterPanels): send the command and keep what it will have
    // done. The centre drag sends FOXAPI_OP_SET_VFO_OFFSET with the offset the engine's
    // own limit (vfoOffsetInsideSpan, over the chain's input rate and the bandwidth) allows
    // for the absolute frequency wanted; the edge drag sends FOXAPP_OP_SET_BANDWIDTH_DRAG
    // with the width the pointer asks for, and keeps the width the engine's bounds
    // (kVfoBwMinHz, kVfoBwMaxChanFrac of the channel rate) will leave it.
    void dragVfoCentre(double wantAbsHz);
    void dragVfoEdge(double mouseHz, double bandCentreAbsHz);
    // The frequency entry's Enter: the text parsed and checked (readable, and a figure the
    // table takes), the old red line cleared, then the tune sent - or one sentence on the
    // red line saying it could not be read.
    void commitTypedFrequency(const char* text);
    // THE TYPED FREQUENCY'S REFUSAL: the message of the last FOXAPI_OP_SET_FREQUENCY the
    // table refused or could not apply (pumpTransmitKey drains every answer once a
    // frame, with the key's), kept for the readout to say on the red line next frame -
    // the window's own presentation state, with the one-pass lag that comes with it.
    std::string freqEntryRefusal_;
    // THE TRANSMIT PAGE IS OPEN - the window's own page, so the window's own
    // flag: restored from the config it hands the engine (applyConfig), moved
    // by its own clicks, each told to the engine
    // (FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN) for the web status and the save.
    bool transmitPageOpen_ = false;
    void setTransmitPageOpen(bool open);
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
    // Uploads image slot i's pixels into imageTex_[i] when its revision moved.
    // Shared by the picture window and the fax recorder's paper (which prints
    // the chart, field report 932444a354dee32f), so the upload happens whenever
    // EITHER consumer is on screen; the caller checks the image has pixels.
    void ensureImageTexture(std::size_t i, const cascade::core::HostImage& im);
    // True while a SHOWN fax instrument belongs to `plugin` - the question that
    // keeps its picture uploaded while the picture window itself is closed.
    bool faxInstrumentWantsChart(const std::string& plugin);
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
    // --- THE WINDOW'S OWN DISK WORK, on workers (stub campaign B19; app_window_disk_work.cpp has the why and what is NOT here) ----------------
    // A plugin's picture saved as a BMP and F12's screenshot: the GUI thread does what only it can (the GL read-back, ImGui's window list, the file's
    // name, a copy of the picture's pixels) and hands finished inputs to a worker that makes the folder and writes the file. One worker per feature at
    // a time. The state is behind a shared_ptr the header cannot see into, made on first use (the object-file limit, C1128: every DiskJob<T>
    // instantiation lives in app_window_disk_work.cpp).
    void saveImageBmp(const cascade::core::HostImage& im);
    bool imageSavePending() const;
    void shotAddPicture(std::string path, cascade::core::HostImage image);
    void shotAddText(std::string path, std::string text);
    void shotFlush();
    void pollDiskJobs();   // once a frame: a saved picture's note; a finished screenshot frees the next
    // WHAT THE ENGINE'S WORKERS ARE DOING, as it publishes it (the keys that start them are disabled while they run, and a long wait says so):
    bool bookmarkExportPending();    // fact.bookmarks.exporting: a frequency list is being written
    bool iqOpenPending();            // fact.source.iqOpening: an I/Q file's header is being read
    double iqOpenElapsedS();         // ...for how long (this window's clock from when it first saw it)
    double iqOpenSinceS_ = -1.0;
    cascade::gui::DiskWorkState& diskWork();
    std::shared_ptr<cascade::gui::DiskWorkState> diskWork_;
    // THE SLOW-DISK SEAM: called by each of the window's workers immediately before it touches the disk. A test binds one that sleeps - a slow disk
    // as far as the frame loop can tell, with the real files and the real BMP encoder behind it. (The engine's workers have their own,
    // Engine::setDiskHookForTest.)
    std::function<void()> diskHookForTest_;
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

    // THE STORE'S MODEL IS THE TABLE'S (window port, group 10): it is built from the window's engine API session
    // when listSeq moves (storeLists, below) and nowhere else - what ADD ALL plans against is the engine's own
    // catalogue (Engine::startAddAll), not a model the window builds.

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
    // WHERE THE PLUGIN STORE WINDOW SAT (stub campaign B4; master 0.99.72): AppConfig::pluginStoreX/Y/Width/Height, restored the way the
    // fitted window's rectangle is - checked against the monitors that exist NOW, clamped to what fits where it lands - and read back from
    // the live window every frame it is drawn, because ImGui's own .ini persistence is switched off in this application. Zero width means
    // "nothing saved" (the map pages' sentinel). Members in app_window_store.cpp.
    int storeWinX_ = 0;
    int storeWinY_ = 0;
    int storeWinW_ = 0;
    int storeWinH_ = 0;
    // The listSeq at which the store window last asked the engine for the open page's pictures that nobody had asked for (once a move, so a
    // refusal is not repeated every frame): see drawPluginStoreWindow.
    std::uint64_t storePicturesWantSeq_ = 0;
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
    // WHAT THE FITTED MODULES WINDOW TAKES FROM THE TABLE (window port, group 9; API 0.3 section 8.8,
    // gui/plugins_view.hpp): its whole model - one record a plugin file, from PLUGINS, its facts and
    // PLUGIN_COMMANDS - built when listSeq moves and kept, and only while the window is drawn
    // (fittedLists() is called from nowhere else). A pointer for the reason the decks are: the type
    // lives behind imgui.h.
    std::unique_ptr<FittedListCache> fittedCache_;
    FittedListCache& fittedLists();
    // A PLUGIN'S SETTINGS FORM (CASCADE_CAP_SETTINGS_UI, 0.99.43; stub campaign B9, app_window_plugin_settings.cpp). The form is the WINDOW's (its boxes, the
    // text being typed, the edits sent and not yet answered); what each module declares is the table's list PLUGIN_SETTINGS, read when listSeq moves
    // and only by a surface that draws one (settingsLists); what each field shows is fact.plugin.setting.<id>; an edit is FOXAPP_OP_PLUGIN_SETTING_EDIT
    // and its answer (settingsOnAnswer, from the once-a-frame drain) is what a refused box snaps back on. The window adds no door: a list, a fact and an op.
    cascade::gui::PluginSettingsFormState pluginSettingsForms_;
    cascade::gui::PluginSettingsLists pluginSettingsLists_;
    const cascade::gui::PluginSettingsLists& settingsLists();
    bool pluginHasSettings(const std::string& file);
    void drawPluginSettingsFields(const std::string& file, const char* scope);   // one module's fields, where the caller stands
    void drawPluginSettingsSection(const std::string& displayName);              // a plugin's own window: a folded "Plugin settings" line
    void drawPluginSettingsRailNode(const std::string& file);                    // the DECODE bank's folded line
    void drawPluginSettingsPatchNode(const std::string& nodePlugin);             // the patch inspector's form
    void settingsFormsBeginFrame();                                              // the frame's first word...
    void settingsFormsFlush();                                                   // ...and its last: a box whose surface vanished is committed
    void settingsOnAnswer(const FoxCommandResult& r);
    friend class PluginSettingsSink;
    // THE ROWS OF A PLUGIN'S PANEL THAT ARE KEYS (0.99.77): the commands the modules registered (PLUGIN_COMMANDS), by module file, read when listSeq
    // moves and only when a table has a row that asks to be a key; the rows drawn, a press sent as FOXAPI_OP_PLUGIN_COMMAND.
    struct PluginCommandRegistry {
        std::uint64_t listSeq = ~std::uint64_t{0};
        std::map<std::string, std::vector<cascade::core::HostCommand>> byFile;
    };
    PluginCommandRegistry pluginCommandRegistry_;
    const std::vector<cascade::core::HostCommand>& registeredCommandsOf(const std::string& file);
    void drawPluginRows(const std::string& plugin, const char* censusSuffix, const std::vector<std::string>& headings,
                        const std::vector<CascadePanelRow>& rows);
    // The listSeq at which the open module page's pictures were last asked for (the store window's storePicturesWantSeq_ for this window): a page
    // asks on the edge of opening, and later at most once for a move of listSeq and only for a picture the table lists nobody asked for.
    std::uint64_t fittedPicturesWantSeq_ = 0;
    // WHAT THE PLUGIN STORE TAKES FROM THE TABLE (window port, group 10; API 0.3 section 8.9,
    // gui/plugin_store_view.hpp): two tiers, both read when listSeq moves. The RAIL tier - the store key's chip, the
    // retired rows under it, the plugins key's refused count, and the edges the deck's consents are cleared on
    // (a fetch began, an ADD ALL run ended) - is read by every member that draws on the rail and by the window
    // every frame, open or shut; the FULL tier, the model, only while the window is drawn (`full`). What moves
    // with no listSeq - the transfer's progress - is read once a frame by a full call, and the status lines come
    // from statusLines_. Called from the window's own draw only.
    std::unique_ptr<StoreListCache> storeCache_;
    StoreListCache& storeLists(bool full);
    char pluginUrlBuf_[512] = "";    // edit buffer for the catalogue URL
    // Whether pluginUrlBuf_ has been seeded from fact.store.url (the first look that finds the fact; applyConfig clears it, the plugin test hook sets it). The
    // seed's text is read into pluginUrlSeed_, a string that keeps its room.
    bool pluginUrlSeeded_ = false;
    std::string pluginUrlSeed_;
    // --- measurement (tools/measure_engine.ps1) ------------------------------
    // Both null in every ordinary run; each is created only by its debug
    // switch. FOXSDR_MEASURE is the bounded run's HOST's since shell retirement R9
    // (engine/host_measure.hpp; honoured in a bounded --frames run ONLY, driven by
    // the end-of-frame hook above). FOXSDR_FRAME_LOG (frameLog_), FOXSDR_FRAME_CAP_HZ (frameCap_) and
    // FOXSDR_VSYNC_OFF (the swap interval, read where the window is made) are
    // read in EVERY run, interactive ones included - an environment switch,
    // never a setting, and there is no other way on. See gui/frame_log.hpp
    // and core/engine_measure.hpp.
    std::unique_ptr<FrameLog> frameLog_;
    std::unique_ptr<FrameCap> frameCap_;  // FOXSDR_FRAME_CAP_HZ
    // --- update check --------------------------------------------------------
    bool closeRequested_ = false;     // set by the updater; the run loop honours it
    bool updateCheckEnabled_ = true;
    // AppConfig::updateChannel (0.99.88): "stable" or "beta".
    std::string updateChannel_ = "stable";
    // The ids of the team messages whose Seen key was pressed, oldest first (AppConfig::messagesSeen, 0.99.79). Local only; pruned to the ids the last
    // successful check still carried (pollUpdateAsync). "Not now" on the update banner (updateDismissed_) does not touch it, and Seen does not touch
    // updateDismissed_.
    std::vector<std::string> messagesSeen_;
    bool updateStarted_ = false;      // one check per launch, no retry storm
    bool updateDismissed_ = false;    // "not now" hides it until next launch
    bool updatePending_ = false;      // a check or a download is in flight
    bool updateDownloading_ = false;
    cascade::core::UpdateInfo update_;
    std::string updateError_;
    std::string updateReadyPath_;     // verified installer, waiting to be run
    // --- the Microsoft Store's answer, for a packaged copy (0.99.53) --------
    // updateStarted_ / updatePending_ / updateError_ / updateDismissed_ above mean the same things for a Store copy as for an ordinary one; these hold what
    // only the Store can say. Both workers return BY VALUE and capture nothing of this window, so ~AppWindow abandons them rather than waiting
    // (core::AbandonableTask) - which matters for the install request, whose worker sits inside Windows' own dialogs for as long as the user takes.
    cascade::core::StoreUpdateCheck storeUpdate_;        // the last answer
    bool storeInstallPending_ = false;                   // Windows is on it
    bool storeInstallDone_ = false;                      // an outcome to show
    cascade::core::StoreInstallOutcome storeInstallOutcome_;
    cascade::core::AbandonableTask<cascade::core::StoreUpdateCheck> storeCheck_;
    cascade::core::AbandonableTask<cascade::core::StoreInstallOutcome> storeInstall_;
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
    // (FittedModulesDeck::confirmRemove); the disabled list's is here, drawn
    // from the rail. BOTH ARE KEYED ON THE MODULE FILE NAME, not a position:
    // every fetch, install and removal rescans, and a position would arm the
    // Delete of whichever module the rescan put there (window port, group 10).
    std::string blockedRemoveConfirmFile_;  // disabled-list row awaiting confirmation ("": none)

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
    // THE SUMMARY ITSELF IS THE ENGINE'S (Engine::reportPluginTestResult, window port group 14 part e), printed from the engine's
    // LIVE catalogue and not from the future's payload, so what it reports is exactly what the UI is about to draw: the engine
    // calls this window's onCatalogueResult from inside its poll and the hook, with the switch above set, names the frame.
    //
    // CASCADE_PLUGIN_STATUS=1, honored ONLY by run(frames >= 0), like the hook above, is a local of run(): Engine::
    // reportPluginStatus prints what the ENFORCEMENT decided (how many candidates the host actually mapped, what blockedCount()
    // says, one line per retired plugin including whether the host has any record of it - mapped=0 is the proof that the stale
    // code is not in the process) at the end of the run, so it reflects any catalogue fetch the run performed. The receiver
    // plugins' pictures are the one thing it takes from this window (pluginImages_, the window's own copy of the engine's accumulator).

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
    // Serves the browser map's tile wants from the basemap plugin. The
    // browser's viewport drives WHICH tiles; this frame-time pump is what
    // keeps the plugin on the GUI thread, the only thread the ABI lets call
    // it: the server records requests it cannot answer, this drains a bounded
    // number per frame, fetches, encodes, publishes, and the browser retries.
    // It is the ONE feed the window still gives the page (stage 5e: the spectrum and the audio are the table's, the pictures the engine's).
    void pumpWebTiles();
    // What the last pump saw, so a plugin change or removal clears the
    // server's store instead of serving the old source's imagery as the new's.
    bool webTilesActive_ = false;
    std::string webTileAttribution_;
    // THE WEB AND CAT SERVERS ARE THE ENGINE'S (stage 5b, docs/engine-stage4.md 6.10). This window no longer owns a socket, a start, a stop, a
    // password hash or a copy of their configuration: the panels configure them with FOXAPI_OP_SERVER_CONFIG, and draw what the table says of
    // them - the SERVERS list (49) for whether each listens, on which port, with what refusal and how many clients, and the facts fact.web.* /
    // fact.cat.* for the configuration and what the bind policy makes of it. What follows is the VIEW's: the panel mirrors ImGui edits by
    // pointer, the staged web edits an Apply sends, and the last read of the table.
    struct ServerPanel {
        std::uint64_t listSeq = 0;      // the list part below was read at this listSeq; 0: never
        std::uint64_t factsSeq = 0;     // ...and the facts part at this one
        // the SERVERS list (value[0] is the port listened on, else the configured one)
        bool webRunning = false;
        bool webFault = false;          // enabled, not listening, and a refusal to show
        int webPort = 0;
        std::string webRefusal;
        bool catRunning = false;
        bool catFault = false;
        int catPort = 0;
        std::string catRefusal;
        // the facts (LOCAL): the configuration, and what the policy makes of it
        bool webEnabled = false;
        std::string webBindAddress;
        int webConfiguredPort = 0;
        std::string webUsername;
        bool webHasPassword = false;
        bool webReachable = false;
        bool webAuthRequired = false;
        std::vector<std::string> webAddresses;
        bool catEnabled = false;
        bool catBindAll = false;
        int catConfiguredPort = 0;
        std::string factText;
    };
    // Read when listSeq has moved (the rail's rule); the facts only when a panel asks for them (`withFacts`): the status column and the
    // rail rows need the list alone and one fact (uiFact), the two sections need all of it.
    const ServerPanel& serverPanel(bool withFacts);
    ServerPanel serverPanel_;
    // The live figures of one server (the list entry's value[1..3]), read now: browser sessions / CAT clients, browsers listening to the
    // audio, commands received. All zero when the entry is not there.
    struct ServerLive {
        int clients = 0;
        int audioListeners = 0;
        unsigned long long commands = 0;
    };
    ServerLive readServerLive(bool cat);
    // The refusal sentence WHOLE: a list item's detail is cut at 159 bytes and the policy's longest sentence is a little over, so the fact
    // that carries the policy's sentence in full stands in when the list's is its beginning.
    std::string serverRefusalText(const std::string& listed, const char* policyFact);
    // SERVER_CONFIG on the window's session; false when the session did not take it. The answers it is owed are counted, because the
    // panel mirrors below are taken from the table only when nothing this window asked is still on its way (a mirror re-read in between
    // would flicker back to the old value).
    bool sendServerConfig(const std::string& json);
    int serverAsksPending_ = 0;
    std::uint64_t serverMirrorSeq_ = 0;
    void syncServerMirrors(const ServerPanel& sp);
    // Applies the staged web edits (and `extra`, a password member, when the password button asked) in one SERVER_CONFIG.
    void applyWebSettings(const std::string& extraWebMembers = std::string());
    // The password button: a plaintext password is sent for the ENGINE to hash on a worker; an empty one CLEARS the password, which the
    // policy will refuse if the binding is not loopback - deliberately, since that is the user removing the only thing protecting an
    // exposed receiver.
    void setWebPassword(const std::string& password);

    // Panel mirrors (ImGui edits by pointer).
    bool catEnabled_ = false;
    bool catBindAll_ = false;
    int catPortMirror_ = static_cast<int>(cascade::net::kDefaultCatPort);

    // The web section's staged edits (ImGui edits by pointer). webBindChoice_: 0 = this machine only, 1 = every network interface. A
    // specific interface address loaded from the config that matches neither shows as choice 2 and is left alone.
    bool webStageEnabled_ = false;
    std::string webStageBind_ = "127.0.0.1";
    int webBindChoice_ = 0;
    int webPortMirror_ = cascade::net::kDefaultWebPort;
    char webUserBuf_[64] = "admin";
    char webPassBuf_[128] = "";
    char webPassConfirmBuf_[128] = "";
    std::string webError_;   // red: a refusal of the last SERVER_CONFIG, or the password field's own complaint
    std::string webNote_;    // neutral/green: "serving at http://..."
    // Edits are staged and applied on a button rather than taking effect as
    // they are typed. Two reasons: restarting the listener on every keystroke
    // of the port field is nonsense, and — the real one — a setting that
    // decides who can reach the receiver should be reviewed before it takes
    // effect, not applied halfway through being typed.
    bool webDirty_ = false;

    // The pointer ledger (see the frame loop), switched on from the
    // Diagnostics section so a tester can read it without setting an
    // environment variable. Not saved: it is a measurement, not a setting.
    bool inputLedger_ = false;
};

}  // namespace cascade::gui
