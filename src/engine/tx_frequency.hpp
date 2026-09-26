// tx_frequency.hpp - which frequency the transmitter is on, the one piece of
// the TRANSMIT page's arithmetic the engine needs (engine extraction stage 3:
// the transmitter follows the receiver's dial from Engine::followTransmitFrequency).
// Moved verbatim from gui/transmit_page.hpp, which includes it, so every caller
// still finds it where it was.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_TX_FREQUENCY_HPP
#define CASCADE_ENGINE_TX_FREQUENCY_HPP

namespace cascade::gui {

// --- the frequency -----------------------------------------------------------

// Which frequency the transmitter should be on this frame.
//
// NOT SPLIT is the default and is the answer almost everybody wants: the
// transmitter follows the receiver, so tuning the dial moves both and a reply
// goes out where the call came in. SPLIT unlinks them, which is what a
// repeater, a DX pile-up and a satellite all need - and which is also the
// state where somebody transmits somewhere they are not listening, so the
// page lights the key while it is on.
inline double txFrequencyHz(bool split, double splitHz, double receiverHz) {
    return split ? splitHz : receiverHz;
}

}  // namespace cascade::gui

#endif  // CASCADE_ENGINE_TX_FREQUENCY_HPP
