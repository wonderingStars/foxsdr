// Link tuning - see link_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "link_logic.hpp"

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

#include <algorithm>
#include <cmath>

namespace foxsdr::slinthost {

namespace {
bool openBit(std::uint32_t open, int slot) { return slot >= 0 && slot < kMaxRadios && ((open >> slot) & 1u) != 0u; }
}  // namespace

void LinkGroup::setOn(bool on, std::uint32_t open) {
    on_ = on;
    if (on) { excluded_ = 0; }
    for (Slot& s : slot_) { s = Slot{}; }
    knownOpen_ = open;
}

void LinkGroup::restore(bool on, std::uint32_t excluded) {
    on_ = on;
    excluded_ = excluded & ((1u << kMaxRadios) - 1u);
    for (Slot& s : slot_) { s = Slot{}; }
    knownOpen_ = 0;
}

bool LinkGroup::setLinked(int slot, bool linked, std::uint32_t open) {
    if (!on_ || !openBit(open, slot)) { return false; }
    const bool was = (excluded_ & (1u << slot)) == 0u;
    if (was == linked) { return false; }
    if (linked) { excluded_ &= ~(1u << slot); } else { excluded_ |= 1u << slot; }
    slot_[static_cast<std::size_t>(slot)] = Slot{};   // a radio that joins takes its gap to the others as it is now (a baseline at the next step)
    return true;
}

bool LinkGroup::linked(int slot, std::uint32_t open) const { return on_ && openBit(open, slot) && (excluded_ & (1u << slot)) == 0u; }

void LinkGroup::forget(int slot) {
    if (slot < 0 || slot >= kMaxRadios) { return; }
    excluded_ &= ~(1u << slot);
    slot_[static_cast<std::size_t>(slot)] = Slot{};
}

LinkState LinkGroup::stateOf(int slot, std::uint32_t open) const {
    if (!linked(slot, open)) { return LinkState::Off; }
    return slot_[static_cast<std::size_t>(slot)].couldNot ? LinkState::CouldNotFollow : LinkState::Linked;
}

std::vector<LinkMove> LinkGroup::step(const std::array<LinkReading, kMaxRadios>& readings, std::uint32_t open, const std::function<LinkRange(int)>& range) {
    std::vector<LinkMove> out;
    knownOpen_ = open;
    struct Mover { int slot; double d; };
    std::vector<Mover> movers;
    std::vector<int> group;     // the linked radios that have a reading
    for (int s = 0; s < kMaxRadios; ++s) {
        Slot& sl = slot_[static_cast<std::size_t>(s)];
        if (!openBit(open, s)) { sl = Slot{}; continue; }          // a radio that has gone is forgotten; one that comes back takes a new baseline
        if (!linked(s, open) || !readings[static_cast<std::size_t>(s)].valid) {
            if (!linked(s, open)) { sl = Slot{}; }
            continue;
        }
        const double obs = readings[static_cast<std::size_t>(s)].tunedHz;
        if (!sl.tracked) {
            sl = Slot{};
            sl.tracked = true;
            base_[static_cast<std::size_t>(s)] = intended_[static_cast<std::size_t>(s)] = obs;
            continue;
        }
        group.push_back(s);
        if (sl.waiting) {
            ++sl.age;
            if (std::fabs(obs - sl.wantHz) <= kLinkSettleTolHz || sl.age >= kLinkGiveUpTicks) {
                sl.waiting = false;
                base_[static_cast<std::size_t>(s)] = obs;
                sl.couldNot = std::fabs(obs - intended_[static_cast<std::size_t>(s)]) > kLinkSettleTolHz;
            }
            continue;
        }
        const double d = obs - base_[static_cast<std::size_t>(s)];
        if (std::fabs(d) > kLinkMoveTolHz) { movers.push_back({s, d}); }
    }
    if (movers.empty()) { return out; }
    // THE GROUP MOVES BY THE LARGEST MOVE MADE THIS TICK; a radio that moved itself is where it is now (what the user did to it is its new place), the others shift their intended frequency by the same amount
    const Mover* lead = &movers.front();
    for (const Mover& m : movers) { if (std::fabs(m.d) > std::fabs(lead->d)) { lead = &m; } }
    const double delta = lead->d;
    for (const Mover& m : movers) {
        const double obs = readings[static_cast<std::size_t>(m.slot)].tunedHz;
        base_[static_cast<std::size_t>(m.slot)] = intended_[static_cast<std::size_t>(m.slot)] = obs;
        slot_[static_cast<std::size_t>(m.slot)].couldNot = false;
    }
    for (const int s : group) {
        if (std::any_of(movers.begin(), movers.end(), [s](const Mover& m) { return m.slot == s; })) { continue; }
        Slot& sl = slot_[static_cast<std::size_t>(s)];
        double& want = intended_[static_cast<std::size_t>(s)];
        want += delta;
        double target = want;
        if (range) {
            const LinkRange r = range(s);
            if (r.known) { target = std::clamp(target, r.loHz, r.hiHz); }
        }
        const double obs = readings[static_cast<std::size_t>(s)].tunedHz;
        if (std::fabs(target - obs) > kLinkMoveTolHz) {
            sl.waiting = true;
            sl.wantHz = target;
            sl.age = 0;
            out.push_back({s, target});
        } else {
            sl.waiting = false;
            base_[static_cast<std::size_t>(s)] = obs;
        }
        sl.couldNot = std::fabs(target - want) > kLinkSettleTolHz;   // a known limit: the radio will be short of the group (an unknown one is found out when the radio settles)
        if (!sl.waiting) { sl.couldNot = sl.couldNot || std::fabs(obs - want) > kLinkSettleTolHz; }
    }
    return out;
}

std::string linkKeyLabel() { return cascade::i18n::tr("Link tuning"); }
std::string linkKeyTip() { return cascade::i18n::tr("Linked radios move together by the same amount and keep the gaps between them."); }
std::string paneLinkLabel(int radio) { return cascade::core::formatText(cascade::i18n::tr("Link Radio %d"), radio); }
std::string paneLinkTip(bool linked) {
    return linked ? std::string(cascade::i18n::tr("Linked: moves with the other linked radios")) : std::string(cascade::i18n::tr("Not linked: Link tuning never moves this radio"));
}
std::string couldNotFollowTip(int radio, const std::string& whereText) {
    return cascade::core::formatText(cascade::i18n::tr("Radio %d could not follow the link and stays at %s."), radio, whereText.c_str());
}
std::string startTogetherLabel() { return cascade::i18n::tr("Start together"); }
std::string startTogetherTip() { return cascade::i18n::tr("Start every open radio"); }

}  // namespace foxsdr::slinthost
