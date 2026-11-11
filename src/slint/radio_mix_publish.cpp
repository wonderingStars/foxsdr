// The strip, audio mix, card and remove question published into the window - see radio_mix_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radio_mix_publish.hpp"

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "link_logic.hpp"

#include <algorithm>
#include <cstdio>

namespace foxsdr::slintworkspace {

using namespace foxsdr::slinthost;

namespace {
slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
template <class T>
std::shared_ptr<slint::Model<T>> modelOf(std::vector<T> rows) { return std::make_shared<slint::VectorModel<T>>(std::move(rows)); }
}  // namespace

std::vector<RadioChipData> DeckPublisher::chips(const DeckInputs& in) const {
    std::vector<RadioChipData> out;
    for (int s = 0; s < kMaxRadios; ++s) {
        if (((in.mask >> s) & 1u) == 0u) { continue; }
        out.push_back(radioChip(s + 1, in.facts[static_cast<std::size_t>(s)], in.states[static_cast<std::size_t>(s)], in.haveState[static_cast<std::size_t>(s)]));
    }
    return out;
}

void DeckPublisher::publish(const DeckInputs& in) {
    auto& g = ui_.global<WorkspaceModel>();
    int count = 0;
    for (int s = 0; s < kMaxRadios; ++s) { count += ((in.mask >> s) & 1u) ? 1 : 0; }
    const bool several = count > 1;
    const int sel = std::clamp(in.selectedSlot, 0, kMaxRadios - 1);
    const FoxReceiverState& selSt = in.states[static_cast<std::size_t>(sel)];
    bool anyRunning = false;
    for (int s = 0; s < kMaxRadios; ++s) {
        if (((in.mask >> s) & 1u) != 0u && (in.states[static_cast<std::size_t>(s)].flags & FOXAPI_RX_RUNNING) != 0u) { anyRunning = true; }
    }
    const bool selRunning = (selSt.flags & FOXAPI_RX_RUNNING) != 0u;

    // ---- the strip: the selector, STOP Rn, STOP ALL / START ALL ----
    {
        std::vector<int> radios = several ? selectorRadios(in.shownLayout, in.page, in.selectedSlot, in.mask) : std::vector<int>();
        std::string sig;
        for (const int r : radios) { sig += std::to_string(r) + (r == sel + 1 ? "*" : "") + ";"; }
        if (sig != selectorSig_) {
            selectorSig_ = sig;
            std::vector<StripRadio> rows;
            for (const int r : radios) {
                StripRadio row{};
                row.radio = r;
                row.label = ss("R" + std::to_string(r));
                row.name = ss(std::string(cascade::i18n::tr("Controls follow")) + " " + cascade::core::formatText(cascade::i18n::tr("Radio %d"), r));
                row.tip = ss(cascade::core::formatText(cascade::i18n::tr("Select Radio %d"), r));
                row.on = r == sel + 1;
                rows.push_back(row);
            }
            g.set_selector(modelOf(std::move(rows)));
        }
        const StripWords w = stripWords(sel + 1, selRunning, anyRunning);
        const std::string sig2 = w.stopSelected + "|" + w.stopSelectedTip + "|" + (selRunning ? "r" : "-") + w.stopAll + "|" + w.stopAllTip;
        if (sig2 != stripSig_) {
            stripSig_ = sig2;
            g.set_stop_selected_text(ss(w.stopSelected));
            g.set_stop_selected_tip(ss(w.stopSelectedTip));
            g.set_selected_running(selRunning);
            g.set_stop_all_text(ss(w.stopAll));
            g.set_stop_all_tip(ss(w.stopAllTip));
        }
    }
    // ---- the MASTER lamps ----
    {
        std::vector<MasterLampData> lamps;
        if (several) {
            std::array<RadioFacts, kMaxRadios> f = in.facts;
            lamps = masterLamps(in.mask, in.selectedSlot, f.data(), in.states.data());
        }
        std::string sig;
        for (const auto& m : lamps) { sig += std::to_string(m.radio) + ":" + std::to_string(static_cast<int>(m.state)) + (m.selected ? "*" : "") + m.name + m.tip + ";"; }
        if (sig != masterSig_) {
            masterSig_ = sig;
            std::vector<MasterLamp> rows;
            for (const auto& m : lamps) {
                MasterLamp row{};
                row.radio = m.radio; row.caption = ss(m.caption); row.name = ss(m.name); row.tip = ss(m.tip);
                row.state = static_cast<int>(m.state); row.on = m.selected;
                rows.push_back(row);
            }
            g.set_masters(modelOf(std::move(rows)));
        }
    }
    // ---- the Audio mix rows ----
    {
        std::vector<MixRowData> rows;
        if (several) {
            for (int s = 0; s < kMaxRadios; ++s) {
                if (((in.mask >> s) & 1u) == 0u) { continue; }
                rows.push_back(mixRow(s + 1, in.facts[static_cast<std::size_t>(s)], in.states[static_cast<std::size_t>(s)], in.soloRadio));
            }
        }
        std::string sig;
        char b[64];
        for (const auto& r : rows) {
            std::snprintf(b, sizeof(b), "%d:%d%d%d%.4f", r.radio, r.route, r.muted ? 1 : 0, r.solo ? 1 : 0, static_cast<double>(r.volume));
            sig += b + r.routeText + r.name + ";";
        }
        if (sig != mixSig_) {
            mixSig_ = sig;
            std::vector<MixRow> out;
            for (const auto& r : rows) {
                MixRow row{};
                row.radio = r.radio; row.name = ss(r.name); row.route = r.route; row.route_text = ss(r.routeText); row.route_label = ss(r.routeLabel); row.route_tip = ss(r.routeTip);
                row.muted = r.muted; row.mute_label = ss(r.muteLabel); row.solo = r.solo; row.solo_label = ss(r.soloLabel); row.solo_tip = ss(r.soloTip);
                row.volume = r.volume; row.volume_label = ss(r.volumeLabel); row.volume_text = ss(r.volumeText); row.running = r.running;
                out.push_back(row);
            }
            g.set_mix(modelOf(std::move(out)));
        }
    }
    // ---- the card ----
    {
        const bool canAdd = in.freeSlots > 0;
        std::string sig = std::string(canAdd ? "c" : "-") + (in.addOpen ? "o" : "-") + "|" + in.addNote;
        for (const auto& c : in.choices) { sig += "|" + c.caption + "/" + c.accessible; }
        if (sig != addSig_) {
            addSig_ = sig;
            g.set_can_add(canAdd);
            g.set_add_open(in.addOpen && canAdd);
            g.set_add_note(ss(in.addNote));
            std::vector<AddChoice> rows;
            for (const auto& c : in.choices) { AddChoice row{}; row.caption = ss(c.caption); row.accessible = ss(c.accessible); rows.push_back(row); }
            g.set_add_choices(modelOf(std::move(rows)));
            g.set_add_title(ss(addCardTitle()));
            g.set_add_hint(ss(addCardHint()));
            g.set_add_none(ss(addCardNone()));
            g.set_add_rescan(ss(addCardRescan()));
            g.set_add_key(ss(addCardTitle()));
            g.set_add_key_tip(ss(addCardTitle()));
        }
    }
    // ---- the question before a running radio is removed ----
    {
        const std::string sig = std::to_string(in.removeConfirm);
        if (sig != removeSig_) {
            removeSig_ = sig;
            g.set_remove_confirm(in.removeConfirm);
            const int r = in.removeConfirm;
            const FoxReceiverState& st = in.states[static_cast<std::size_t>(std::clamp(r - 1, 0, kMaxRadios - 1))];
            g.set_remove_question(ss(r > 0 ? removeQuestion(r) : std::string()));
            g.set_remove_explain(ss(r > 0 ? removeExplain(st) : std::string()));
            g.set_remove_yes(ss(cascade::i18n::tr("Remove")));
            g.set_remove_no(ss(cascade::i18n::tr("Cancel")));
        }
    }
    // ---- STAGE S5: the bar's Link tuning and Start together ----
    {
        bool anyStopped = false;
        for (int s = 0; s < kMaxRadios; ++s) {
            if (((in.mask >> s) & 1u) != 0u && (in.states[static_cast<std::size_t>(s)].flags & FOXAPI_RX_RUNNING) == 0u) { anyStopped = true; }
        }
        const std::string sig = std::string(in.linkOn ? "L" : "-") + (anyStopped ? "s" : "-");
        if (sig != linkSig_) {
            linkSig_ = sig;
            g.set_link_on(in.linkOn);
            g.set_link_text(ss(linkKeyLabel()));
            g.set_link_tip(ss(linkKeyTip()));
            g.set_together_text(ss(startTogetherLabel()));
            g.set_together_tip(ss(startTogetherTip()));
            g.set_together_enabled(anyStopped);
        }
    }
    // ---- the sections panel's tag ----
    {
        const std::string name = cascade::core::formatText(cascade::i18n::tr("Radio %d"), sel + 1);
        if (name != tagSig_) { tagSig_ = name; g.set_selected_name(ss(name)); }
    }
}

}  // namespace foxsdr::slintworkspace
