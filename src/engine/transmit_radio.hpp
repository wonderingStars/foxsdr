// transmit_radio.hpp - WHICH RADIO THE TRANSMITTER FOLLOWS (multi-radio engine, stage 6; docs/multi-radio-records/engine-stages-5-6.md).
//
// The transmitter is one device. Its frequency follows a radio's: the radio whose device is the transmitter's own (a Pluto is both a receiver and a transmitter), else the first
// radio. Pure, so that the rule can be held without a Pluto: the engine hands it what each radio is on and what the transmitter was opened on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cascade::engine {

// One radio, in slot order: the driver kind it is on ("pluto" for a Pluto), the arguments it was opened with, and whether a device is open at all.
struct TransmitCandidate {
    std::string kind;
    std::string args;
    bool deviceOpen = false;
};

// The same, by reference, for a caller that must not copy strings (the engine asks every pass): null kind/args = no radio in that slot.
struct TransmitView {
    const std::string* kind = nullptr;
    const std::string* args = nullptr;
    bool deviceOpen = false;
};

// The slot (index) of the radio the transmitter follows: the first radio whose open device is a Pluto at the transmitter's own address; slot 0 when no transmitter is open, none of
// them is, or the transmitter's address is empty. `view(slot)` answers a TransmitView for slot 0..count-1.
template <class View>
std::size_t transmitRadioForViews(bool transmitterOpen, const std::string& transmitArgs, std::size_t count, View&& view) {
    if (!transmitterOpen || transmitArgs.empty()) { return 0u; }
    for (std::size_t slot = 0; slot < count; ++slot) {
        const TransmitView r = view(slot);
        if (r.deviceOpen && r.kind != nullptr && r.args != nullptr && *r.kind == "pluto" && *r.args == transmitArgs) { return slot; }
    }
    return 0u;
}

inline std::size_t transmitRadioFor(bool transmitterOpen, const std::string& transmitArgs, const std::vector<TransmitCandidate>& radios) {
    return transmitRadioForViews(transmitterOpen, transmitArgs, radios.size(), [&](std::size_t slot) { return TransmitView{&radios[slot].kind, &radios[slot].args, radios[slot].deviceOpen}; });
}

}  // namespace cascade::engine
