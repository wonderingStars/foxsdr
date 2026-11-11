// radio_mix_publish.hpp - THE STRIP, THE AUDIO MIX, THE ADD-A-RADIO CARD AND THE REMOVE QUESTION INTO THE WINDOW (docs/multi-radio-records/screen-s4.md): the words and rows of radio_mix_logic.hpp
// into the stage-S4 half of the WorkspaceModel global of ui/workspace.slint. A value is set only when it changed (a Slint model set replaces every repeater item behind it), so a quiet strip
// costs nothing, and with ONE radio the publisher writes the same empty models once and then nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <string>
#include <vector>

#include <slint.h>

#include "main.h"
#include "radio_mix_logic.hpp"

namespace foxsdr::slintworkspace {

// Everything one tick of the deck needs, gathered by the host.
struct DeckInputs {
    std::uint32_t mask = 1u;                                  // the radios that exist (bit i = slot i)
    int selectedSlot = 0;
    int shownLayout = 0, page = 0;
    std::array<foxsdr::slinthost::RadioFacts, foxsdr::slinthost::kMaxRadios> facts;
    std::array<FoxReceiverState, foxsdr::slinthost::kMaxRadios> states{};
    std::array<bool, foxsdr::slinthost::kMaxRadios> haveState{};
    int soloRadio = 0;                                        // 1-based, 0 = nobody (the host's own record: the engine publishes no read-back)
    std::vector<foxsdr::slinthost::DeviceChoice> choices;     // the devices the card offers
    int freeSlots = 0;                                        // radios that can still be added
    std::string addNote;                                      // the engine's refusal, unchanged
    bool addOpen = false;
    int removeConfirm = 0;                                    // 1-based radio awaiting a yes, 0 none
};

class DeckPublisher {
public:
    explicit DeckPublisher(MainWindow& ui) : ui_(ui) {}
    void publish(const DeckInputs& in);

    // the status bar's chips for the open radios, in slot order
    std::vector<foxsdr::slinthost::RadioChipData> chips(const DeckInputs& in) const;

private:
    MainWindow& ui_;
    std::string selectorSig_, stripSig_, masterSig_, mixSig_, addSig_, removeSig_, tagSig_;
};

}  // namespace foxsdr::slintworkspace
