// patch_logic.hpp - THE PATCH PAGE'S PURE LOGIC (docs/engine-stage4.md, the Slint port, phase 3, the patch allowance): what the PATCH view shows - the band (transport, the
// seven part keys, the decoder keys, the blueprints), the information pane, the canvas with its nodes, wires and faces, the inspector and the presets page - is computed from
// the C API table's lists and facts by these functions, and every control the operator touches becomes the exact edit of the DRAFT or the exact command the ImGui window's own
// control makes. No Slint type and no engine header appears here (foxsdr_api.h, core/app_commands.hpp and the core headers named below), so the file is tested without a window
// or an engine (src/slint/tests/test_slint_host_patch.cpp reads a stub table and the ImGui sources for every literal, as test_slint_host_extend.cpp does).
//
// THE CONTRACT is the UI family's (ui/patch_page.slint; the note slint-patch-contract.md): the models below are the Slint structs with the same fields in snake_case; the
// publisher (patch_publish.*, not this file) maps them 1:1 and wires each PatchEvents callback to the PatchPage method the table below names.
//
// THE SHAPE. One class, PatchPage, owns the state the ImGui window keeps in AppWindow (the draft and its commit machine, the canvas interaction and view, the per-node notes and
// queues, the blueprint note, the presets page) and is driven by three calls: tick() once per tick with the table and the receiver state, one method per callback, onAnswer() for
// each answer the session polls. It is built from the pieces below, each usable alone: readLists / readLive / readLook (the reads), PatchDraft (the draft machine),
// PatchCanvas (the pointer and the view), SinkLines (the Text out queues). The pieces that need a ticket back from the session (the commit, a document, the radio scan) take a
// Submitter; the ones that are one op return it as data (opRun, opAllOff, opDeviceOpened, opLook, opViewShown, opScanNativeOnly).
//
// WHERE THE RULES COME FROM (the ImGui window, read for behaviour and for every sentence, never for drawing): app_window.cpp drawPatchView 12753-13335, drawPatchFaces 11459-12117,
// the draft machine 12220-12251 and 12522-12751, patchLists 12298-12428, routePatchSinkLines 12502; app_window_patch.cpp (the information pane); app_window_patch_radios.cpp
// (transport, switch, squelch, the radio and sink inspectors, the centre); app_window_patch_blueprints.cpp; app_window_patch_presets.cpp; gui/patch_view.cpp (the pointer, the
// wires, the refusals). The sentences are those drawers' own, through cascade::i18n::tr() (the catalogues already hold them) except the two the contract names as new:
// "%zu line" / "%zu lines" (the window's literal "%llu line%s" is not translatable).
//
// CHOICES WHERE THE CONTRACT LEAVES A QUESTION (each is the rule the contract states, or the window's own when the contract is silent):
//  1. THE RADIO SWITCH shows OFF (0) whenever the node's own switch is off, even while its row is still open - the window's drawPatchRadioSwitch tests !n.on first, so a press
//     shows at once; the contract's one-line precedence (open first) would show an open radio as ON for a pass after it was switched off. Open (2) is only for a switched-on node.
//  2. THE SEED sets the centre only for a radio whose device was empty (the window's `if (n->device.empty())`); the seeded radio always is one.
//  3. RESTORE runs at the first tick after the first valid look whether or not the page is showing (the engine must hold the document before START); the SEED runs only while the
//     page is showing, as the window's drawPatchView does. A restore the session would not take is tried again next tick; a text that does not parse is given up (seeded stays false).
//  4. PRESETS: nothing about a preset is logged. A load outside the engine's reach (the STOP the session would not take) is "not a patch" in the note, as the window words it.
//  5. THE DISPLAY FACE (deferred) says "No spectrum yet - its radio is not running." when its feeder is a planned Channel or a running Radio and the node's radio row is not open
//     (the contract's rule); the window says it when no spectrum frame has arrived.
//  6. THE REFUSAL CLOCK is the tick counter (kRefusalTicks = 76 at the 33 ms tick = 2.5 s); the host's `now` is the number of tick() calls.
//  7. The seam to the settings file is PatchConfig (patch(), presets(), previous(), savePatch(), savePresets()): the publisher implements it over HostDuties::interfaceHalf() /
//     saveInterfaceHalf() (half.patch, half.patchPresets, half.patchPresetPrevious - all Interface-half fields, core/config_halves.hpp).
//  8. SINK TEXT FIT uses the window's arithmetic with the face's height in world units (the zoom cancels): fit = max(1, int((h - 40) / 13.1) - 1) where h is the node's height.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "core/freq_converter.hpp"
#include "core/patch_blueprints.hpp"
#include "core/patch_devices.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_io.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_presets.hpp"
#include "gui/patch_view_math.hpp"
#include "kit_logic.hpp"
#include "signal_logic.hpp"

namespace foxsdr::slintpatch {

namespace pc = cascade::core::patch;
namespace pg = cascade::gui::patch;
using foxsdr::slintkit::TableReader;
using foxsdr::slintsignal::Command;
using foxsdr::slintsignal::SelectItem;
using foxsdr::slintsignal::Tone;

// ---- the constants the ImGui window states ----
constexpr std::size_t kNameBytes = 63;                // the inspector's name buffer char[64]
constexpr int kRefusalTicks = 76;                     // 2.5 s at the 33 ms tick (patch_view.cpp kRefusalHoldSec)
constexpr float kWireGrabPx = 7.0f;                   // patch_view.cpp kWireGrabPx
constexpr float kGridWorld = 26.0f;                   // patch_view.cpp kGridWorld
constexpr float kGridMinStepPx = 6.0f;                // the grid is not drawn below this pitch
constexpr float kFacesMinZoom = 0.6f;                 // drawPatchFaces: below this the controls are left off
constexpr float kInspectorW = 190.0f;                 // the artboard's (the window's constant is 236: Open questions 2)
constexpr float kInspectorFoldedW = 24.0f;            // ...and the folded strip (the window's is 30)
constexpr float kKeyH = 24.0f, kKeyGap = 4.0f, kKeyInset = 10.0f, kStepKeyW = 28.0f, kFitKeyMinW = 44.0f;   // the three canvas keys (drawPatchViewKeys)
constexpr std::size_t kSinkLinesCap = 200;            // routePatchSinkLines
constexpr double kBlueprintDefaultRateHz = 2.0e6;     // app_window_patch_blueprints.cpp
constexpr std::size_t kTowerNearest = 6;              // the Airband section's own "Nearest airports" count
constexpr double kSquelchSpanDb = 120.0;
constexpr double kRates[11] = {1.0e6, 1.024e6, 2.0e6, 2.048e6, 2.4e6, 2.5e6, 3.0e6, 4.0e6, 6.0e6, 8.0e6, 10.0e6};   // kPatchRatesHz
constexpr int kRateCount = 11;

// ---- the page's widths (patch_view_math.hpp's own arithmetic, with the artboard's inspector widths) ----
inline float inspectorWidth(bool nodeSelected, bool folded) {
    if (!nodeSelected) { return 0.0f; }
    return folded ? kInspectorFoldedW : kInspectorW;
}
inline float canvasWidthFor(float pageW, float inspectorW) { return pg::patchCanvasWidth(pageW, inspectorW); }
inline float infoWidthFor(float pageW) { return pg::patchInfoPaneWidth(pageW, 1.0f); }

// ---- the models (ui/patch_page.slint's structs, snake_case) ----
// FaceLine.tone: 0 plain, 1 muted, 2 amber (a number or a note), 3 phosphor (what the radio received), 4 rust (a fault).
enum LineTone : int { kTonePlain = 0, kToneMuted = 1, kToneAmber = 2, kTonePhosphor = 3, kToneRust = 4 };
struct FaceLine {
    std::string text;
    int tone = 0;
};
struct PartKey {
    std::string label;
    int kind = 0;                          // 0..6 in the order of kParts (Radio Channel Demod Spectrum Speaker Text-out Map)
    bool enabled = true;
    std::string tooltip;
};
struct DecoderKey {
    std::string label;
    int index = 0;                         // the PATCH_PARTS row
    std::string tooltip;
};
struct ProblemLine {
    std::string text;
    bool advisory = false;
};
struct InfoModel {
    bool runnable = false;
    std::string verdict, level_line, plan_line;
    bool none_selected = true;
    std::vector<std::string> help;         // the three sentences while nothing is selected
    std::string selection;                 // "<KIND>  <name>"
    std::string folded_note;
    std::vector<ProblemLine> problems;
};
struct PatchModel {
    bool running = false;
    std::string run_label, run_tooltip, run_line;
    bool run_line_lit = false;
    bool presets_open = false;
    std::string tab_chip;
    Tone tab_chip_tone = Tone::Neutral;
    std::vector<PartKey> parts;
    std::vector<DecoderKey> decoders;
    std::string no_decoders;
    bool blueprints_shown = false;
    std::vector<SelectItem> blueprints;
    std::string blueprint_note;
    InfoModel info;
    bool inspector_shown = false;
    bool inspector_folded = false;
    // ---- host extras ----
    std::string all_off_tooltip, presets_tooltip;
    std::string blueprint_placeholder;     // "Pick a plugin to place its whole workflow"
};
struct PortDot {
    float x = 0.0f, y = 0.0f;              // canvas-local pixels, the dot's centre
    int tone = 0;                          // 0 I/Q, 1 audio, 2 text, 3 control, 4 track
};
struct FaceModel {
    int radio_switch = 0;                  // 0 off, 1 on and waiting, 2 open
    std::string radio_switch_label, radio_switch_tooltip, device_label, centre_text, centre_note;
    std::string freq_text;
    int mode_index = 0;
    bool squelch_on = false;
    float squelch_norm = 0.0f;
    std::string squelch_text;
    bool gate_shown = false;
    std::string gate_text, gate_word;
    bool gate_lit = false;
    bool level_shown = false;
    float level_norm = 0.0f;
    std::uint64_t picture_id = 0;          // the image id the picture is of (the publisher turns it into the slint image)
    bool have_picture = false;
    std::vector<FaceLine> lines, lines_after;
    // ---- host extras ----
    std::string centre_tooltip, freq_tooltip, squelch_tooltip;
};
struct CanvasNode {
    int id = 0;
    int kind = 0;                          // core NodeKind: 0 Radio 1 Channel 2 Demod 3 Decoder 4 Display 5 Sink 6 Map
    std::string kind_caption, name;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool selected = false, blocked = false, fault = false, active = false, hot_close = false, hot_grip = false;
    std::vector<PortDot> inputs, outputs;
    std::string reading;
    int reading_tone = 0;
    std::string reading2;
    FaceModel face;
};
struct CanvasWire {
    std::string path;                      // "M x y C x y x y x y" in canvas-local pixels
    int tone = 0;                          // the SOURCE output's port type
    bool selected = false;
};
struct Refusal {
    bool shown = false;
    std::string text;
    float x = 0.0f, y = 0.0f;              // the bubble's top-left, already pointer + (12, 10)
};
struct CanvasModel {
    float zoom = 1.0f;
    bool faces_live = true;
    float grid_step = 0.0f, grid_ox = 0.0f, grid_oy = 0.0f;
    std::vector<CanvasNode> nodes;
    std::vector<CanvasWire> wires;
    std::string ghost;
    Refusal refusal;
    bool keys_shown = false;
    // ---- host extras ----
    bool resize_cursor = false;            // the cursor hint: nwse-resize
    std::string zoom_out_tooltip, zoom_in_tooltip, fit_tooltip, fit_label;
};
struct InspectorRadio {
    std::vector<SelectItem> device_items;
    int device_index = -1;
    std::string device_label;
    bool looking = false, look_enabled = true;
    std::string centre_text, centre_note;
    std::vector<SelectItem> rate_items;
    int rate_index = -1;
    std::string rate_label;
    bool rate_fixed = false;
    std::string rate_fixed_text;
    std::vector<FaceLine> lines;
};
struct InspectorDecoder {
    std::vector<SelectItem> plugin_items;
    int plugin_index = -1;
    std::string plugin_label, feed_note;
    std::vector<std::string> fed;
    std::string shown_note;
    // The plugin's settings form is the kit's: the module key and its fields as the kit read them (the publisher maps them to the form's rows).
    std::string settings_module;
    std::vector<foxsdr::slintkit::SettingSpec> settings;
    bool has_settings = false;
};
struct InspectorMapSource {
    std::string count, name;
};
struct InspectorSink {
    std::vector<SelectItem> items;
    int index = -1;
    std::string label;
    std::vector<FaceLine> lines;
    std::string files_note;
};
struct InspectorModel {
    bool shown = false, folded = false;
    int node = 0;
    int kind = 0;
    std::string kind_caption, name;
    InspectorRadio radio;
    std::string freq_text;
    int mode_index = 0;
    bool squelch_on = false;
    float squelch_norm = 0.0f;
    std::string squelch_text;
    bool gate_shown = false;
    std::string gate_text, gate_word;
    bool gate_lit = false;
    InspectorDecoder decoder;
    std::string map_wired;
    std::vector<InspectorMapSource> map_sources;
    std::string map_help;
    InspectorSink sink;
    bool audio_sink = false;
    std::string help;
    // ---- host extras ----
    std::string fold_tooltip;
};
struct PresetRow {
    std::string text;
    bool previous = false, selected = false;
};
struct PatchPresetsModel {
    std::vector<PresetRow> rows;
    bool empty_shown = false;
    std::string empty_text;
    bool load_enabled = false, rename_enabled = false, delete_enabled = false;
    int edit = 0;                          // 0 none, 1 SAVE AS, 2 RENAME
    std::string name_draft, note;
    bool note_good = false;
    bool ask_replace = false;
    std::string replace_text;
    bool ask_delete = false;
    std::string delete_text;
    // ---- host extras ----
    bool focus_name = false;               // the host takes the keyboard focus into the field when the row opens
    std::string previous_tooltip;
};

// ---- the seams ----
// What a command's ticket costs: the session's submit (a ticket, 0 when the session refused it) and its offer (a repeatable one: a refusal is no drop).
class Submitter {
public:
    virtual ~Submitter() = default;
    virtual std::uint64_t submit(const Command& c) = 0;
    virtual std::uint64_t offer(const Command& c) { return submit(c); }
};
// The settings file's half of the page: the saved patch, the presets and the previous-patch slot (see the header note 7).
class PatchConfig {
public:
    virtual ~PatchConfig() = default;
    virtual std::string patch() const = 0;
    virtual std::vector<cascade::core::PatchPreset> presets() const = 0;
    virtual std::string previous() const = 0;
    virtual void savePatch(const std::string& text) = 0;
    virtual void savePresets(const std::vector<cascade::core::PatchPreset>& list, const std::string& previous) = 0;
};

// ---- the reads (a port of patchLists, patchLook and readPatchLive) ----
struct RadioRow {
    std::string device, note;              // PATCH_RADIOS name = the device, detail = the note its face prints
    double rate_hz = 0.0, centre_hz = 0.0;
    bool pending = false, open = false, fault = false;
};
struct DeviceRow {
    std::string label, key;
    double rate_hz = 0.0;
};
struct PatchLists {
    std::vector<pc::DecoderInfo> parts;
    pc::Plan plan;
    std::vector<pc::NodeId> refused;
    std::map<pc::NodeId, RadioRow> radios;
    int radios_open = 0;
    std::map<pc::NodeId, cascade::core::ConverterSetting> conv;
    std::vector<DeviceRow> devices;
    std::string receiver_radio;
    bool receiver_tuned = false;
    // ---- what the page also needs of the table ----
    std::vector<std::string> bookmark_groups;      // BOOKMARKS' groups, each once, first seen first (the tower blueprints)
    std::vector<std::string> audio_devices;        // AUDIO_DEVICES names, in order
    std::string recorder_dir;                      // fact.recorder.dir
    std::uint64_t list_seq = ~std::uint64_t{0};    // the listSeq the lists were read at
};
void readLists(TableReader& table, std::uint64_t listSeq, PatchLists& out);   // no-op while out.list_seq == listSeq

struct LiveRow {
    pc::NodeId node = pc::kNoNode;
    int kind = 0;                          // 1 gate, 2 speaker output, 3 channel level, 4 decoder face, 5 speaker error, 6 picture
    double v0 = 0.0, v1 = 0.0;
    bool open = false;
    std::string name, detail;
};
std::vector<LiveRow> readLive(TableReader& table);                                            // every tick; it moves no listSeq
const LiveRow* liveRow(const std::vector<LiveRow>& rows, int kind, pc::NodeId node);

struct PatchLook {
    bool valid = false;
    std::uint64_t version = 0, epoch = 0;
    pc::Graph graph;
    std::string text, scratch;
    std::uint64_t list_seq = ~std::uint64_t{0};
};
// The graph, its version and its epoch of ONE publish, read when listSeq has moved (the two-reads rule); a publish between the reads sets list_seq to ~0 so the next tick looks again.
const PatchLook& readLook(TableReader& table, std::uint64_t listSeq, PatchLook& cache);

// The device a new Radio takes: the receiver's own radio first, then every PATCH_DEVICES row that is neither the generator nor a recording, the first no Radio node holds; else the generator.
std::string defaultDeviceKey(const pc::Graph& draft, const PatchLists& lists);

// ---- the draft machine (patchInteracting / syncPatchDraft / commitPatchDraft / settlePatchSend / adoptPatchLook / loadPatchDocument / forgetPatchInteraction / onPatchGraphResult) ----
class PatchDraft {
public:
    struct Send {
        enum class State { Idle, Sent, Applied };
        State state = State::Idle;
        std::uint64_t ticket = 0;
        pc::Graph sent;
        std::uint64_t new_version = 0;
        bool merged = false;               // the engine merged the edit (applied[1] == 1)
        bool document = false;
    };
    struct RadioWait {
        pc::NodeId node = pc::kNoNode;
        std::uint64_t scan_ticket = 0;
    };

    pc::Graph& graph() { return draft_; }
    const pc::Graph& graph() const { return draft_; }
    const pc::Graph& base() const { return base_; }
    std::uint64_t version() const { return version_; }
    std::uint64_t epoch() const { return epoch_; }
    bool inStep() const { return in_step_; }
    bool adoptWanted() const { return adopt_wanted_; }
    const Send& send() const { return send_; }
    const std::vector<RadioWait>& waits() const { return waits_; }
    std::uint64_t scanDone() const { return scan_done_; }
    // Bumped by every adopt that drops the interaction (a new epoch) and by loadDocument: the canvas forgets its drag / resize / wire / selection when it sees it move.
    std::uint64_t forgetSerial() const { return forget_serial_; }
    bool dirty = false;                    // an edit, a pan, a zoom: the config's document follows

    // syncPatchDraft: `interacting` is a drag, a resize or a wire in the air with the button held.
    void sync(const PatchLook& look, const PatchLists& lists, bool interacting);
    // commitPatchDraft: one commit in flight; a ticket of 0 = not taken, try next tick.
    void commit(const PatchLook& look, Submitter& sub);
    // settlePatchSend: the answer's publish has been read.
    void settle(const PatchLook& look);
    // onPatchGraphResult: the engine's answer to the commit in flight.
    void onAnswer(const FoxCommandResult& r, const PatchLook& look);
    // The answer to a FOXAPP_OP_SCAN_NATIVE_ONLY: raises scanDone.
    void noteScanAnswer(std::uint64_t ticket) { scan_done_ = std::max(scan_done_, ticket); }
    // loadPatchDocument: false when the session would not take it (nothing changes then but the interaction).
    bool loadDocument(const pc::Graph& g, Submitter& sub);
    // A Radio part waits for its scan; the device is chosen when the answer arrives.
    void addWait(pc::NodeId node, std::uint64_t scanTicket) { waits_.push_back({node, scanTicket}); }
    void finishRadioParts(const PatchLists& lists);
    void adopt(const PatchLook& look, bool documentChanged = true);

private:
    pc::Graph draft_, base_;
    std::uint64_t version_ = 0, epoch_ = 0;
    bool in_step_ = false, adopt_wanted_ = false;
    Send send_;
    std::vector<RadioWait> waits_;
    std::uint64_t scan_done_ = 0;
    std::uint64_t forget_serial_ = 0;
};

// ---- the canvas: the pointer stream, the view, the model of nodes and wires ----
class PatchCanvas {
public:
    pg::Interaction ui;                    // ui.view is canvas-relative (pan within the canvas), the interface factor S is 1
    float canvas_w = 0.0f, canvas_h = 0.0f;

    void setSize(float w, float h) { canvas_w = w; canvas_h = h; }
    // press(x, y, button): button 0 left, 1 right, 2 middle; positions are canvas-local pixels.
    void press(PatchDraft& d, float x, float y, int button, std::int64_t now);
    void move(PatchDraft& d, float x, float y);
    void release(PatchDraft& d, float x, float y, int button, std::int64_t now);
    void left();
    void wheel(PatchDraft& d, float x, float y, float notchesX, float notchesY, bool ctrl, bool shift);
    void zoomStep(PatchDraft& d, bool in);
    void fit(PatchDraft& d);
    void deleteKey(PatchDraft& d);
    void setView(const pg::View& v) { ui.view = v; }
    // A drag, a resize or a wire in the air with the button held (patchInteracting).
    bool interacting() const;
    bool buttonDown() const { return down_; }
    // The canvas forgets what the draft's interaction was of when the draft says it is a new patch (forgetSerial moved) and anything of a node that vanished.
    void reconcile(const PatchDraft& d);
    void forget();
    bool pointerKnown() const { return pointer_known_; }
    pg::Vec2 pointerWorld() const;
    pg::View drawView() const { return pg::drawView(ui.view, 1.0f); }
    std::int64_t refusedTick() const { return refused_tick_; }

private:
    bool down_ = false;
    bool pointer_known_ = false;
    pg::Vec2 pointer_;                     // canvas-local pixels
    pg::Vec2 last_;                        // the last move's pixels (the pan delta)
    std::int64_t refused_tick_ = 0;
    std::uint64_t seen_serial_ = 0;
};

// ---- the Text out queues (routePatchSinkLines): each decoder line the engine says the decoder of a patch node said goes to every Text out that node is wired to ----
class SinkLines {
public:
    // Reads DECODER_LINES rows above the highest routed id; `graph` is the draft's wires.
    void read(TableReader& table, const pc::Graph& graph);
    const std::map<pc::NodeId, std::deque<std::string>>& queues() const { return queues_; }
    void forgetNode(pc::NodeId id) { queues_.erase(id); }
    void clear() { queues_.clear(); }
    void dropGone(const pc::Graph& graph);
    std::uint64_t routedId() const { return routed_id_; }
private:
    std::map<pc::NodeId, std::deque<std::string>> queues_;
    std::uint64_t routed_id_ = 0;
};

// ---- the small pure rules the models and the effects share ----
const char* kindCaption(pc::NodeKind k);                  // translated: RADIO CHANNEL DEMOD DECODER DISPLAY SINK MAP, "NODE"
const char* refusalText(pc::Connect why);                 // translated
int portTone(pc::PortType t);                             // 0 I/Q, 1 audio, 2 text, 3 control, 4 track
std::string partLabelEnglish(int kind);                   // "Radio" "Channel" "Demod" "Spectrum" "Speaker" "Text out" "Map" (stored in the patch as data)
std::string converterNameText(const cascade::core::ConverterSetting& s);
// A text typed in a MHz box: the number, false for a text that is not one (an InputDouble would not take it).
bool parseMhz(const std::string& text, double& mhz);
std::string cutBytes(const std::string& s, std::size_t bytes);   // cut on a character boundary

// The Interface half's document: serialise(draft, pan, zoom).
std::string documentOf(const pc::Graph& g, const pg::View& view);

// ---- the effects that are one op ----
Command opRun(std::uint32_t receiverFlags);                // FOXAPI_OP_PATCH_RUN 0x0902 makeInt(op, running ? 0 : 1)
Command opAllOff();                                        // FOXAPI_OP_PATCH_ALL_OFF 0x0903, no arguments
Command opScanNativeOnly();                                // FOXAPP_OP_SCAN_NATIVE_ONLY 0x8406
std::vector<Command> opDeviceOpened();                     // PATCH_RADIO_LIST_OPENED 0x8407 then PATCH_LIST_RECORDINGS 0x8422
std::vector<Command> opLook(std::uint32_t sourceFlags);    // nothing while a scan is pending; else LOOK_FOR_RADIOS 0x8413, SCAN_DEVICES 0x0301, LIST_RECORDINGS 0x8422
Command opViewShown(bool shown);                           // FOXAPP_OP_SET_PATCH_PAGE_OPEN 0x8421 ival[0] = shown ? 1 : 0 (OFFERED)

// ---- a picture a decoder node holds ----
struct PictureWant {
    pc::NodeId node = pc::kNoNode;
    std::uint64_t image_id = 0;
    std::uint64_t bytes = 0;
};
// The decoder nodes of the draft whose kind-6 row names an image other than the one held (held: node -> image id).
std::vector<PictureWant> picturesWanted(const pc::Graph& draft, const std::vector<LiveRow>& live, const std::map<pc::NodeId, std::uint64_t>& held);
// read_image's bytes are a whole picture when width * height * bytes-per-pixel == the bytes got (bpp 3 for CASCADE_IMAGE_RGB24, 1 for GRAY8).
bool pictureWhole(std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint64_t bytesGot);

// ---- what the publisher hands in beside the table ----
struct MapTrack {
    std::string plugin;                    // the module's display name
    std::uint32_t kind = 0;                // CASCADE_TRACK_AIRCRAFT / VESSEL / STATION
};

// ---- the page ----
class PatchPage {
public:
    PatchPage();

    // ---- wiring ----
    void setConfig(PatchConfig* config) { config_ = config; }
    void setKit(const foxsdr::slintkit::KitLists* kit) { kit_ = kit; }            // the kit's lists (read with ReadOptions::settings while the form shows)
    void setTracks(std::vector<MapTrack> tracks) { tracks_ = std::move(tracks); }
    void setPresetsOpen(bool open) { presets_open_ = open; }
    void setViewShown(bool shown) { want_shown_ = shown; }                        // main.cpp's on_view_selected
    void setCanvasSize(float w, float h);
    // The one tick: the view-shown offer, the reads, the draft sync, the restore and the seed, the draft's commit, the config's document. `showing` = view-index == 1.
    void tick(TableReader& table, const FoxReceiverState& state, Submitter& sub, bool showing);
    void onAnswer(const FoxCommandResult& r);

    // ---- the models (built from what the last tick read) ----
    PatchModel bandModel() const;
    InfoModel infoModel() const;
    CanvasModel canvasModel() const;
    InspectorModel inspectorModel() const;
    PatchPresetsModel presetsModel() const;

    // ---- the band ----
    Command runClicked() const { return opRun(flags_); }
    Command allOffClicked() const { return opAllOff(); }
    void partPressed(int kind, Submitter& sub);
    void decoderPressed(int index, Submitter& sub);
    void blueprintsOpened(TableReader& table);
    void blueprintPicked(int index, Submitter& sub);

    // ---- the canvas ----
    void canvasPressed(float x, float y, int button);
    void canvasMoved(float x, float y);
    void canvasReleased(float x, float y, int button);
    void canvasLeft();
    void canvasWheel(float x, float y, float notchesX, float notchesY, bool ctrl, bool shift);
    void zoomStep(bool in);
    void fit();
    void canvasDelete();

    // ---- the faces ----
    void faceSwitch(int node);
    void faceCentre(int node, const std::string& mhz);
    void faceFreq(int node, const std::string& mhz);
    void faceMode(int node, int index);
    void faceSquelchOn(int node, bool on);
    void faceSquelch(int node, float norm);

    // ---- the inspector ----
    void inspectorFold() { folded_ = !folded_; }
    void inspectorName(const std::string& text);
    std::vector<Command> inspectorDeviceOpened() const { return opDeviceOpened(); }
    void inspectorDevice(int index);
    std::vector<Command> inspectorLook() const { return opLook(source_flags_); }
    void inspectorCentre(const std::string& mhz);
    void inspectorRate(int index);
    void inspectorFreq(const std::string& mhz);
    void inspectorMode(int index);
    void inspectorSquelchOn(bool on);
    void inspectorSquelch(float norm);
    void inspectorPlugin(int index);
    void inspectorSink(int index);
    void inspectorSize(int step);                                                  // 0 Smaller, 1 Larger, 2 Default size
    void inspectorRemove();

    // ---- the presets page ----
    void presetsRow(int index);
    void presetsRowActivated(int index, Submitter& sub);
    void presetsLoad(Submitter& sub);
    void presetsSaveAs();
    void presetsRename();
    void presetsDelete();
    void presetsName(const std::string& text);
    void presetsGo();
    void presetsCancel();
    void presetsReplace(bool yes);
    void presetsDeleteConfirm(bool yes);

    // ---- what the publisher needs of the page ----
    PatchDraft& draft() { return draft_; }
    const PatchDraft& draft() const { return draft_; }
    PatchCanvas& canvas() { return canvas_; }
    const PatchCanvas& canvas() const { return canvas_; }
    const PatchLists& lists() const { return lists_; }
    const std::vector<LiveRow>& live() const { return live_; }
    const PatchLook& look() const { return look_; }
    const SinkLines& sinkLines() const { return sink_lines_; }
    pc::NodeId selected() const { return canvas_.ui.selected; }
    bool folded() const { return folded_; }
    float inspectorWidthNow() const { return inspectorWidth(selectedNode() != nullptr, folded_); }
    bool seeded() const { return seeded_; }
    std::vector<PictureWant> picturesToRead() const { return picturesWanted(draft_.graph(), live_, pictures_); }
    void pictureTaken(pc::NodeId node, std::uint64_t imageId) { pictures_[node] = imageId; }
    const std::string& blueprintNote() const { return blueprint_note_; }
    std::int64_t now() const { return now_; }

private:
    void restoreAndSeed(Submitter& sub, const FoxReceiverState& state, bool showing);
    void forgetGone(bool showing);
    bool setRadioCentre(pc::Node& n, double airHz);
    void persistPresets();
    bool replacePatch(const std::string& text, Submitter& sub);
    bool loadPreset(const std::string& text, Submitter& sub);
    void loadSelected(Submitter& sub);
    std::string presetSentence(cascade::core::PatchPresetStatus st, const std::string& name) const;
    pc::Node* selectedNode();
    const pc::Node* selectedNode() const;
    FaceModel faceOf(const pc::Node& n) const;
    std::vector<FaceLine> radioLines(const pc::Node& n) const;
    void readingOf(const pc::Node& n, std::string& reading, int& tone, std::string& reading2) const;
    std::vector<pc::Blueprint> buildBlueprints(TableReader& table) const;

    PatchLists lists_;
    std::vector<LiveRow> live_;
    PatchLook look_;
    PatchDraft draft_;
    PatchCanvas canvas_;
    SinkLines sink_lines_;
    PatchConfig* config_ = nullptr;
    const foxsdr::slintkit::KitLists* kit_ = nullptr;
    std::vector<MapTrack> tracks_;

    // the receiver state as the last tick read it
    std::uint32_t flags_ = 0, source_flags_ = 0;
    double centre_hz_ = 0.0, rx_lat_ = 0.0, rx_lon_ = 0.0;

    std::map<pc::NodeId, std::string> centre_notes_;
    std::map<pc::NodeId, std::uint64_t> pictures_;
    std::string blueprint_note_;
    std::vector<pc::Blueprint> blueprints_;
    bool folded_ = false, presets_open_ = false;
    int nodes_placed_ = 0;
    std::int64_t now_ = 0;
    bool restored_ = false, seeded_ = false, lists_taken_ = false;
    bool want_shown_ = false, shown_taken_ = true;
    std::uint64_t listseq_ = 0, forget_seen_ = 0;

    // the presets page
    cascade::core::PatchPresetStore store_;
    bool store_loaded_ = false;
    std::string preset_sel_;
    bool preset_sel_previous_ = false;
    int preset_edit_ = 0;
    std::string preset_name_, preset_pending_, preset_note_;
    bool preset_note_good_ = false, preset_ask_replace_ = false, preset_ask_delete_ = false, preset_focus_name_ = false;
};

}  // namespace foxsdr::slintpatch
