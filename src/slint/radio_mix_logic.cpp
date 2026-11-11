// The strip, the audio mix, the status chips and the Add-a-radio card - see radio_mix_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radio_mix_logic.hpp"

#include "core/engine_vocab.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "host_logic.hpp"

#include <algorithm>
#include <cstdio>

namespace foxsdr::slinthost {

namespace {
// every sentence is a literal tr() argument at its call (the catalogue test finds the key there)
std::string fmt1(const char* translated, int a) { return cascade::core::formatText(translated, a); }
}  // namespace

RadioFacts readRadioFacts(const EngineReads& engine, int radio) {
    RadioFacts f;
    if (!engine.fact || radio < 1) { return f; }
    const std::string pre = "fact.radio." + std::to_string(radio) + ".";
    std::string v;
    if (engine.fact((pre + "state").c_str(), v) < 0) { return f; }
    f.exists = true;
    f.state = v;
    v.clear(); if (engine.fact((pre + "device").c_str(), v) >= 0) { f.device = v; }
    v.clear(); if (engine.fact((pre + "label").c_str(), v) >= 0) { f.label = v; }
    v.clear(); if (engine.fact((pre + "audio").c_str(), v) >= 0 && !v.empty()) { f.audio = v; }
    v.clear(); if (engine.fact((pre + "refusal").c_str(), v) >= 0) { f.refusal = v; }
    return f;
}

int routeOfWord(const std::string& word) {
    if (word == "left") { return kRouteLeft; }
    if (word == "right") { return kRouteRight; }
    if (word == "none") { return kRouteNone; }
    return kRouteBoth;
}
const char* routeWordOf(int route) {
    switch (route) {
        case kRouteLeft: return "left";
        case kRouteRight: return "right";
        case kRouteNone: return "none";
        default: return "both";
    }
}
std::string routeText(int route) {
    switch (route) {
        case kRouteLeft: return cascade::i18n::tr("Left");
        case kRouteRight: return cascade::i18n::tr("Right");
        case kRouteNone: return cascade::i18n::tr("None");
        default: return cascade::i18n::tr("Both");
    }
}
int nextRoute(int route) { return (std::clamp(route, 0, kRouteCount - 1) + 1) % kRouteCount; }

MasterState masterStateOf(const RadioFacts& f, const FoxReceiverState& st) {
    if (f.state == "faulted" || (st.flags & FOXAPI_RX_FAULTED) != 0u) { return MasterState::Failed; }
    if (f.state == "running") { return MasterState::On; }
    return MasterState::Off;
}

namespace {
std::string stateWord(const RadioFacts& f, MasterState m) {
    if (f.state == "lent") { return cascade::i18n::tr("LENT TO PATCH"); }
    if (f.state == "opening") { return cascade::i18n::tr("OPENING"); }
    if (m == MasterState::Failed) { return cascade::i18n::tr("FAIL"); }
    if (m == MasterState::On) { return cascade::i18n::tr("RUNNING"); }
    return cascade::i18n::tr("STOPPED");
}
}  // namespace

std::vector<MasterLampData> masterLamps(std::uint32_t mask, int selectedSlot, const RadioFacts* facts, const FoxReceiverState* states) {
    std::vector<MasterLampData> out;
    for (int s = 0; s < kMaxRadios; ++s) {
        if (((mask >> s) & 1u) == 0u) { continue; }
        MasterLampData m;
        m.radio = s + 1;
        m.caption = std::to_string(s + 1);
        m.name = fmt1(cascade::i18n::tr("Radio %d master"), s + 1);
        m.state = masterStateOf(facts[s], states[s]);
        m.tip = m.name + ": " + stateWord(facts[s], m.state);
        m.selected = s == selectedSlot;
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<int> selectorRadios(int shownLayout, int page, int selectedSlot, std::uint32_t mask) {
    // the first radio of the page shown: One shows the selected radio, a multi-pane layout the page's first pane
    const int first = shownLayout == kLayoutOne ? selectedSlot : page * pageSizeOf(shownLayout);
    const int block = std::clamp(first, 0, kMaxRadios - 1) / 4;
    std::vector<int> out;
    for (int s = block * 4; s < block * 4 + 4 && s < kMaxRadios; ++s) { if (((mask >> s) & 1u) != 0u) { out.push_back(s + 1); } }
    return out;
}

StripWords stripWords(int selectedRadio, bool selectedRunning, bool anyRunning) {
    StripWords w;
    w.stopSelected = fmt1(cascade::i18n::tr("STOP R%d"), selectedRadio);
    w.stopSelectedTip = fmt1(cascade::i18n::tr("Stop Radio %d"), selectedRadio);
    w.selectedRunning = selectedRunning;
    w.anyRunning = anyRunning;
    if (anyRunning) {
        w.stopAll = cascade::i18n::tr("STOP ALL");
        w.stopAllTip = cascade::i18n::tr("Stop every open radio");
    } else {
        w.stopAll = cascade::i18n::tr("START ALL");
        w.stopAllTip = cascade::i18n::tr("Start every open radio");
    }
    return w;
}

MixRowData mixRow(int radio, const RadioFacts& f, const FoxReceiverState& st, int soloRadio) {
    MixRowData r;
    r.radio = radio;
    r.name = fmt1(cascade::i18n::tr("Radio %d"), radio);
    r.route = routeOfWord(f.audio);
    r.routeText = routeText(r.route);
    r.routeLabel = fmt1(cascade::i18n::tr("Output of Radio %d"), radio);
    r.routeTip = r.routeLabel + ": " + r.routeText;
    r.muted = (st.flags & FOXAPI_RX_MUTED) != 0u;
    r.muteLabel = fmt1(cascade::i18n::tr("Mute Radio %d"), radio);
    r.solo = soloRadio == radio;
    r.soloLabel = fmt1(cascade::i18n::tr("Solo Radio %d"), radio);
    r.soloTip = cascade::i18n::tr("Hear only the radios marked SOLO");
    r.volume = static_cast<float>(std::clamp(st.volume, 0.0, 1.0));
    r.volumeLabel = r.name + ": " + cascade::i18n::tr("VOLUME");
    cascade::core::formatUtf8(r.volumeText, "%.0f%%", st.volume * 100.0);
    r.running = (st.flags & FOXAPI_RX_RUNNING) != 0u;
    return r;
}

RadioChipData radioChip(int radio, const RadioFacts& f, const FoxReceiverState& st, bool haveState) {
    RadioChipData c;
    c.radio = radio;
    c.key = "R" + std::to_string(radio);
    const MasterState m = masterStateOf(f, st);
    c.value = stateWord(f, m);
    // a radio's figures come from its own state; a state the host has not read yet shows "--" and never another radio's
    c.detail = haveState ? fmtMHz(st.tunedHz) + " " + modeName(st.demodMode) : std::string("--");
    c.tone = m == MasterState::Failed ? 3 : m == MasterState::On ? 1 : (f.state == "opening" || f.state == "lent") ? 2 : 0;
    c.lampLit = m != MasterState::Off;
    c.lampRed = m == MasterState::Failed;
    return c;
}

std::string deviceKeyOf(const DeviceRow& row) {
    if (row.index == 0) { return std::string(); }
    const std::size_t colon = row.detail.find(':');
    if (colon == std::string::npos) { return std::string(); }
    return row.detail.substr(0, colon) + "|" + row.detail.substr(colon + 1);
}

std::vector<DeviceChoice> addChoices(const std::vector<DeviceRow>& rows, const std::vector<std::string>& held) {
    std::vector<DeviceChoice> out;
    for (const DeviceRow& r : rows) {
        DeviceChoice c;
        if (r.index == 0) {
            c.caption = cascade::i18n::tr("Signal generator");
            c.id = "generator";
        } else if (r.index == 1 || r.index == cascade::core::kSoundCardRow) {
            continue;                                     // the I/Q file and the sound card: a second radio cannot open them
        } else {
            if (r.detail.rfind("pluto:", 0) == 0 || r.detail.rfind("rtltcp:", 0) == 0) { continue; }
            const std::size_t colon = r.detail.find(':');
            if (colon == std::string::npos || colon == 0) { continue; }
            const std::string key = deviceKeyOf(r);
            if (std::find(held.begin(), held.end(), key) != held.end()) { continue; }   // an open radio holds it
            const bool soapy = r.detail.rfind("soapy:", 0) == 0;
            c.caption = soapy ? r.name : cascade::core::formatText(cascade::i18n::tr("%s (native)"), r.name.c_str());
            c.id = r.detail;
        }
        c.accessible = std::string(cascade::i18n::tr("Add")) + " " + c.caption;
        out.push_back(std::move(c));
    }
    return out;
}

AddOutcome addOutcome(std::int32_t status, const char* message) {
    AddOutcome o;
    o.accepted = status == FOXAPI_OK || status == FOXAPI_NO_CHANGE;
    if (!o.accepted) { o.note = message != nullptr ? message : ""; }
    return o;
}

std::string addCardTitle() { return cascade::i18n::tr("Add a radio"); }
std::string addCardHint() { return cascade::i18n::tr("Choose a radio to open in this pane."); }
std::string addCardRescan() { return cascade::i18n::tr("Look for radios again"); }
std::string addCardNone() { return "No other radio found."; }   // English only, needs translation: not in the prepared set
std::string addCardSlotName(int slot) { return fmt1(cascade::i18n::tr("Add a radio to pane %d"), slot); }

bool removeNeedsConfirm(const FoxReceiverState& st) {
    return (st.flags & (FOXAPI_RX_RUNNING | FOXAPI_RX_RECORDING_IQ | FOXAPI_RX_RECORDING_AUDIO)) != 0u;
}
std::string removeQuestion(int radio) { return "Remove Radio " + std::to_string(radio) + "?"; }   // English only, needs translation
std::string removeExplain(const FoxReceiverState& st) {   // English only, needs translation
    const bool rec = (st.flags & (FOXAPI_RX_RECORDING_IQ | FOXAPI_RX_RECORDING_AUDIO)) != 0u;
    if (rec) { return "This radio is recording. Removing it ends the recording and lets go of its device."; }
    return "This radio is running. Removing it stops it and lets go of its device.";
}

}  // namespace foxsdr::slinthost
