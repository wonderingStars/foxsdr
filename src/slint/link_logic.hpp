// link_logic.hpp - LINK TUNING (docs/multi-radio-records/screen-s5.md, stage S5): linked radios move together by the same amount and KEEP THEIR GAPS; a radio that is not linked is never moved.
// Pure logic over what the host reads from the engine (each open radio's tuned frequency, once a tick): it reads nothing, sends nothing and draws nothing, so the host tests drive it with
// made-up readings and the host (main.cpp) turns the moves it returns into SET_FREQUENCY commands aimed at the followers.
//
// THE OWNER'S RULES (2026-11-11): Link is an OPTION, off by default. When on, tuning one linked radio by +10 kHz - however it is done: a click in its pane, the strip's counter, the keyboard, a bookmark,
// another window - moves every other linked radio by +10 kHz, so the gaps between them stay what they were. A radio that would leave its device's tuning range:
//   - the host asks it for the frequency it should have (its INTENDED frequency) and the engine does what the device does: it clamps to the edge or refuses and the radio stays where it was;
//   - the radio is read back and is where it is (its pane says that it could not follow, and where it stays); the others move; nothing else is disturbed;
//   - the intended frequency is KEPT: moving the group back brings the radio back with the gap it had (the gaps are the intended offsets, not wherever a limit left the radios).
//   The product cannot ask a device for its range (the API has none): a range is known only to a test (a bounded run's seam), which then clamps the request itself, as a device that clamps would.
// A radio that leaves the group (its link key, removal) is not moved and does not move the others; a radio that joins takes the gap it has to the others at that moment. Radios added while Link is
// on join it. Tuning is observed, not intercepted: the host reads every open radio's frequency each tick, so every way of tuning is covered and the gap is the engine's own after its snapping.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "workspace_logic.hpp"

namespace foxsdr::slinthost {

// A tune smaller than this is noise, not the user's hand (the tuned frequency is centre + offset, read back from a driver that snaps to its own step).
constexpr double kLinkMoveTolHz = 2.0;
// A follower is settled when it is within this of what it was asked for (a real tuner snaps to its own step) or has stopped moving for kLinkStillTicks; it is given kLinkGiveUpTicks to get there.
constexpr double kLinkSettleTolHz = 500.0;
constexpr int kLinkStillTicks = 3;
constexpr int kLinkGiveUpTicks = 30;

struct LinkReading {
    bool valid = false;        // the radio is open and its state has been read
    double tunedHz = 0.0;
};
struct LinkRange {
    bool known = false;
    double loHz = 0.0, hiHz = 0.0;
};
struct LinkMove {
    int slot = 0;              // 0-based
    double hz = 0.0;           // the frequency to ask that radio for (SET_FREQUENCY)
};

enum class LinkState { Off = 0, Linked = 1, CouldNotFollow = 2 };   // per radio: not in the group (or Link off), in it, in it but not where the group is

class LinkGroup {
public:
    // ---- the option ----
    bool on() const { return on_; }
    // Switching Link on puts every open radio in the group (`open` = bit per slot); off leaves nothing linked. Either way the baseline is taken at the next step().
    void setOn(bool on, std::uint32_t open);
    // The pane's key: takes a radio out of the group or puts it back (only while Link is on and the radio is open). False when nothing changed.
    bool setLinked(int slot, bool linked, std::uint32_t open);
    bool linked(int slot, std::uint32_t open) const;           // on, open and not left out
    // What the saved layout holds: the option and the radios the user took out (bit per slot; kept for radios that are not open now).
    std::uint32_t excluded() const { return excluded_; }
    void restore(bool on, std::uint32_t excluded);
    // A radio the user removed: it is forgotten (its place in the group is not kept for the next radio of that number).
    void forget(int slot);

    // ---- one tick ----
    // `readings[i]` is slot i. Returns the commands to send (one SET_FREQUENCY per follower that must move); updates the baselines. `range(slot)` is the radio's tuning range when it is known.
    std::vector<LinkMove> step(const std::array<LinkReading, kMaxRadios>& readings, std::uint32_t open, const std::function<LinkRange(int)>& range = nullptr);

    // ---- what a pane says ----
    LinkState stateOf(int slot, std::uint32_t open) const;
    double intendedHz(int slot) const { return slot >= 0 && slot < kMaxRadios ? intended_[static_cast<std::size_t>(slot)] : 0.0; }
    double heldHz(int slot) const { return slot >= 0 && slot < kMaxRadios ? base_[static_cast<std::size_t>(slot)] : 0.0; }   // where the radio is: what it was last read at

private:
    struct Slot {
        bool tracked = false;      // a baseline has been taken
        bool waiting = false;      // a follow command is on its way
        double wantHz = 0.0;
        int age = 0;               // ticks since the command
        double lastObs = 0.0;
        int still = 0;
        bool couldNot = false;
    };
    bool on_ = false;
    std::uint32_t excluded_ = 0;
    std::uint32_t knownOpen_ = 0;
    std::array<double, kMaxRadios> base_{};       // where each radio was last settled
    std::array<double, kMaxRadios> intended_{};   // where the group wants it (base_ plus every move of the group since the baseline)
    std::array<Slot, kMaxRadios> slot_{};
};

// ---- the words ----
std::string linkKeyLabel();                         // "Link tuning"
std::string linkKeyTip();                           // "Linked radios move together by the same amount and keep the gaps between them."
std::string paneLinkLabel(int radio);               // "Link Radio 2"
std::string paneLinkTip(bool linked);               // "Linked: moves with the other linked radios" / "Not linked: Link tuning never moves this radio"
std::string couldNotFollowTip(int radio, const std::string& whereText);   // "Radio 2 could not follow the link and stays at 144.500000 MHz."

// ---- start together ----
std::string startTogetherLabel();                   // "Start together"
std::string startTogetherTip();                     // "Start every open radio"

}  // namespace foxsdr::slinthost
