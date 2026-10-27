// app_window_source_row.cpp - what the Source row says about radios (0.99.49 beta feedback, stub campaign B18, ledger SOURCE-ROWS): the "No radio hardware found" gate, the
// radios a running patch holds, and the chip and the combo's preview that name them. AppWindow members, kept out of app_window.cpp (which is at the object-file limit,
// src/gui_pending/app_window_disk_state.hpp).
//
// THE REPORT BEHIND IT. A tester on a small laptop with a NooElec NESDR SMArt v5 had a patch running that radio perfectly, while the Function Select panel said "No radio hardware
// found" and the Source row's chip said "Signal gen". Two faults, both reading as "FoxSDR cannot see my radio": the warning was gated on the SoapySDR list alone, so a dongle the
// NATIVE driver had found - listed in the combo right above the warning - did not count; and while a patch runs it has the radios and the receiver is moved to the signal generator,
// so the row named only the generator. The rules are gui/source_row_status.hpp's (pure, tests/test_source_row_status.cpp); this file asks them of what the table says.
//
// ON THE TABLE (docs/engine-stage4.md 6.5): the rows are the DEVICES list as sourceLists() caches it, the scan's states are the state's sourceFlags, and the radios a patch holds are
// PATCH_RADIOS (their labels) joined to the patch graph (fact.patch.graph) by node - the graph says what each Radio node is set to, which is what tells a radio from the generator and
// from a recording; the list's label alone cannot. Nothing here reads an engine field.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <string>
#include <utility>
#include <vector>

#include "core/i18n.hpp"
#include "core/patch_graph.hpp"
#include "core/utf8_text.hpp"
#include "gui/source_row_status.hpp"

namespace cascade::gui {

using cascade::i18n::tr;

bool AppWindow::noRadioHardwareShown() {
    // EITHER LIST (0.99.49 beta feedback). This was the SoapySDR list alone, so a dongle the native driver had found - listed in the combo right above the warning - still read "No
    // radio hardware found" whenever no SoapySDR module claimed it. The ADALM-Pluto and rtl_tcp rows are rows that ask for an address, not radios that were found, and do not
    // count (nativeRadioRows). See gui/source_row_status.hpp.
    const SourceListCache& c = sourceLists();
    const std::uint32_t src = uiState_.sourceFlags;
    const bool busy = (src & FOXAPI_SRC_SCAN_PENDING) != 0u || (uiState_.flags & FOXAPI_RX_DEVICE_OPENING) != 0u;
    return cascade::gui::noRadioHardwareFound((src & FOXAPI_SRC_SCANNED) != 0u, (src & FOXAPI_SRC_SCAN_PARTIAL) != 0u, busy, c.soapyRows, c.nativeRadioRows);
}

const std::vector<std::string>& AppWindow::patchHeldRadioNames() {
    // What each running patch radio was opened AS says whether it is hardware: the generator and a recording are not radios the receiver could have had. The graph's node says
    // which a Radio is set to; the list's label is the name it opened under. Rebuilt only when the lists moved or the patch started or stopped (the strings keep their room).
    const bool running = (uiState_.flags & FOXAPI_RX_PATCH_RUNNING) != 0u;
    HeldRadios& h = heldRadios_;
    if (h.valid && h.listSeq == uiState_.listSeq && h.running == running) { return h.names; }
    h.valid = true;
    h.listSeq = uiState_.listSeq;
    h.running = running;
    h.names.clear();
    if (!running) { return h.names; }
    const PatchListCache& lists = patchLists();
    const PatchLook& look = patchLook();
    if (!look.valid) {
        h.valid = false;  // the graph is not read yet (the first frame of a window): ask again next frame
        return h.names;
    }
    std::vector<cascade::gui::PatchHeldRadio> held;
    for (const auto& [id, radio] : lists.radios) {
        if (!radio.open) { continue; }
        const cascade::core::patch::Node* node = look.graph.find(id);
        if (node == nullptr) { continue; }
        held.push_back({node->device, radio.device});
    }
    h.names = cascade::gui::patchHardwareNames(true, held);
    return h.names;
}

std::string AppWindow::sourceRowChip() {
    // The device in use, shortened to what fits: "SoapySDR: B200" is the full name and "B200" is the part that identifies it. Translated when it is the built-in generator, and
    // shortened by CHARACTERS: a byte cut would split an accented letter in half. The built-in generator's chip is its own short key, never cut (app_window.hpp, sourceChipText).
    // ...and while the patch has the radio, the chip says so rather than naming only the generator the receiver was moved to.
    return cascade::gui::sourceChipWithPatch(cascade::gui::sourceChipText(uiState_.deviceName), patchHeldRadioNames());
}

const char* AppWindow::sourceRowLabel(int idx) {
    // Row label for a combo index; -1 (active device dropped by a Refresh) falls back to the live source name so the preview is never a lie.
    //
    // NATIVE ROWS COME FIRST, immediately under the generator and the IQ file, and Soapy's rows follow them. Not a cosmetic ordering: with both present the native driver is the
    // one this product can be held responsible for, and the list is read top-down. (An entry's index in DEVICES IS its row; the cache keeps each radio's caption, "(native)"
    // composed.)
    const SourceListCache& c = sourceLists();
    if (idx == 0) { return tr("Signal generator"); }
    if (idx == 1) { return tr("IQ file"); }
    if (idx == kSoundCardRow) { return tr("Sound card"); }
    if (idx >= kNativeRowBase && idx < static_cast<int>(c.rows.size())) { return c.rows[static_cast<std::size_t>(idx)].caption.c_str(); }
    return tr(uiState_.deviceName);
}

bool AppWindow::sourceKeepPreview() {
    // The saved radio could not be opened and the generator is running in its place (the radio, or the I/Q file, is remembered for the config): the combo says so. Not while a
    // sound card is opening either: the card is the one being brought back, and "(saved, not open)" would be the lamp's false alarm in words (gui::radioNotOpenLamp).
    const SourceListCache& c = sourceLists();
    return !c.keepKind.empty() && (uiState_.flags & FOXAPI_RX_DEVICE_OPEN) == 0u && c.generatorActive && !statusLines_.restoreKeepLabel.empty() &&
           (uiState_.sourceFlags & FOXAPI_SRC_CARD_OPENING) == 0u;
}

const std::string& AppWindow::sourceComboPreview() {
    // THE SAVED SOURCE IS WHAT THE COMBO SAYS WHILE IT IS STILL THE SAVED ONE. A restore that could not open it leaves the generator running and the radio - or the I/Q file -
    // remembered for the config (see gui::sourceToSave); showing "Signal generator" here would say the user had chosen that, and would disagree with the file this session is
    // going to write. No row is ticked in the list below, because none of them is what is installed. "(not open)" in the preview itself, because the combo is the one place a user
    // looks to find out what the receiver is on, and the name alone there would claim the radio was running.
    if (sourceKeepPreview()) {
        cascade::core::formatUtf8(sourcePreview_, tr("%s (saved, not open)"), statusLines_.restoreKeepLabel.c_str());
        return sourcePreview_;
    }
    // THE PATCH HAS THE RADIO, said where the user looks for it: the receiver was moved to the generator when the patch started, and "Signal generator" alone read as "there is no
    // radio". (assigned into a string that keeps its room: a frame that changes nothing allocates nothing.)
    const std::vector<std::string>& held = patchHeldRadioNames();
    const char* row = sourceRowLabel(sourceLists().row);
    if (held.empty()) {
        sourcePreview_.assign(row);
    } else {
        sourcePreview_ = cascade::gui::sourceRowText(row, held);
    }
    return sourcePreview_;
}

}  // namespace cascade::gui
