// radio_mix_logic.hpp - THE STRIP, THE AUDIO MIX, THE STATUS CHIPS AND THE ADD-A-RADIO CARD (docs/multi-radio-records/screen-s4.md, stage S4): the host's arithmetic and words for the parts of the
// multi-radio screen that are not a pane. Pure logic over the engine's facts and states: it reads nothing and draws nothing, so the host tests drive it with a made-up engine and the publisher
// (radio_mix_publish.cpp) turns its answers into the WorkspaceModel of ui/workspace.slint.
//
// THE RULES (owner decisions of 2026-11-11 for this stage):
//  - MASTER lamps: one per OPEN radio (up to eight), captioned with the radio's number; lit green while it runs, red when it has failed, dark when it is stopped; a click selects that radio and turns
//    the page to it. Shown only when more than one radio is open.
//  - The strip's radio selector shows the radios of the visible page; with more than four radios it follows the page (a block of four: 1-4, 5-8).
//  - STOP Rn acts on the selected radio; STOP ALL / START ALL act on every open radio (START ALL shows while nothing runs).
//  - THE ADD-A-RADIO CARD lists the devices not already held and adds through RADIO_ADD; a refusal is shown as the engine worded it, unchanged.
//  - THE AUDIO MIX: per radio the route (both, left, right, none), solo, mute and volume. Solo is kept here: the engine publishes no read-back of it (a seam, screen-s4.md).
//  - A figure the engine does not give per radio shows "--", never radio 1's value.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "foxsdr_api.h"
#include "radio_view.hpp"
#include "workspace_logic.hpp"

namespace foxsdr::slinthost {

// ---- the engine's words for a radio (fact.radio.<n>.*) ----
struct RadioFacts {
    bool exists = false;
    std::string state;        // "opening", "running", "stopped", "faulted", "lent"; "" when the radio is not there
    std::string device;       // the key of the device it holds ("" on the generator or a file)
    std::string label;
    std::string audio = "both";   // both, left, right, none
    std::string refusal;      // why its device could not be had
};
RadioFacts readRadioFacts(const EngineReads& engine, int radio);   // radio is 1-based

// ---- the audio route: 0 both, 1 left, 2 right, 3 none (RADIO_AUDIO's ival[1]) ----
constexpr int kRouteBoth = 0, kRouteLeft = 1, kRouteRight = 2, kRouteNone = 3, kRouteCount = 4;
int routeOfWord(const std::string& word);          // unknown words read as both
const char* routeWordOf(int route);                // the engine's word
std::string routeText(int route);                  // the translated caption: Both, Left, Right, None
int nextRoute(int route);                          // the key cycles both -> left -> right -> none -> both

// ---- the MASTER lamp of a radio ----
enum class MasterState { Off = 0, On = 1, Failed = 2 };
MasterState masterStateOf(const RadioFacts& f, const FoxReceiverState& st);

struct MasterLampData {
    int radio = 0;
    std::string caption;      // "1" .. "8": the radio's number
    std::string name;         // "Radio 2 master": the accessible name
    std::string tip;          // what it says now, with the radio's state word
    MasterState state = MasterState::Off;
    bool selected = false;
};
// One per open radio, in order. `facts[i]` and `states[i]` are the radio in slot i (only read for an open one).
std::vector<MasterLampData> masterLamps(std::uint32_t mask, int selectedSlot, const RadioFacts* facts, const FoxReceiverState* states);

// ---- the strip's selector: the radios of the page shown, as radio numbers (1-based) ----
std::vector<int> selectorRadios(int shownLayout, int page, int selectedSlot, std::uint32_t mask);

struct StripWords {
    std::string stopSelected;       // "STOP R2"
    std::string stopSelectedTip;
    bool selectedRunning = false;
    std::string stopAll;            // "STOP ALL" while any radio runs, else "START ALL"
    std::string stopAllTip;
    bool anyRunning = false;
};
StripWords stripWords(int selectedRadio, bool selectedRunning, bool anyRunning);

// ---- the audio mix rows ----
struct MixRowData {
    int radio = 0;
    std::string name;           // "Radio 2"
    int route = 0;
    std::string routeText, routeLabel, routeTip;
    bool muted = false;
    std::string muteLabel;
    bool solo = false;
    std::string soloLabel, soloTip;
    float volume = 0;           // 0..1
    std::string volumeLabel, volumeText;
    bool running = false;
};
MixRowData mixRow(int radio, const RadioFacts& f, const FoxReceiverState& st, int soloRadio);

// ---- the status bar's chips: one per open radio ----
struct RadioChipData {
    std::string key;            // "R2"
    std::string value;          // the state word: RUNNING, STOPPED, FAIL, OPENING, LENT TO PATCH
    std::string detail;         // the frequency and mode, "--" where the radio has none yet
    int tone = 0;               // 0 neutral, 1 good, 2 warn, 3 err
    bool lampLit = false, lampRed = false;
    int radio = 0;
};
RadioChipData radioChip(int radio, const RadioFacts& f, const FoxReceiverState& st, bool haveState);

// ---- the Add-a-radio card ----
// One row of the engine's device list, as the card needs it.
struct DeviceRow {
    int index = 0;              // the row in FOXAPI_LIST_DEVICES: 0 the signal generator, 1 the I/Q file, 2 the sound card, 3 and up the scanned radios
    std::string name;
    std::string detail;         // "soapy:driver=rtlsdr", "rtlsdr:0" ...; "pluto:" and "rtltcp:" rows are not scanned devices
};
struct DeviceChoice {
    std::string caption;        // what the key says
    std::string accessible;     // "Add Signal generator"
    std::string id;             // RADIO_ADD's text: "generator" or "<kind>:<args>"
};
// The devices a second radio can open that no open radio already holds (`held` = the device keys of the open radios): the signal generator (each radio has its own), and the scanned radios.
// NOT offered, and why: the I/Q file, the sound card, a Pluto and an rtl_tcp address - RADIO_ADD refuses them (UNSUPPORTED, engine stage 4: "a second radio opens a scanned device or the signal generator").
std::vector<DeviceChoice> addChoices(const std::vector<DeviceRow>& rows, const std::vector<std::string>& held);
// The key the engine's ledger would hold for a row ("" for the generator), spelled as fact.radio.<n>.device is.
std::string deviceKeyOf(const DeviceRow& row);

// The engine's answer to a RADIO_ADD from the card: taken (OK, or NO_CHANGE for a radio that is open already) or refused. A refusal's note is the engine's message, byte for byte: it names every
// holder ("in use by Radio 1", "in use by patch radio 2") and states the limit of eight; nothing here rewords it.
struct AddOutcome {
    bool accepted = false;
    std::string note;           // "" when accepted
};
AddOutcome addOutcome(std::int32_t status, const char* message);

// The words of the card
std::string addCardTitle();
std::string addCardHint();
std::string addCardRescan();
std::string addCardNone();                 // "No other radio found." (English only)
std::string addCardSlotName(int slot);     // "Add a radio to pane %d"

// ---- removing a radio ----
// A stopped radio goes at once; a running one, or one that is recording, asks first.
bool removeNeedsConfirm(const FoxReceiverState& st);
std::string removeQuestion(int radio);     // "Remove Radio 2?" (English only)
std::string removeExplain(const FoxReceiverState& st);   // what removal does to a running or recording radio (English only)

}  // namespace foxsdr::slinthost
