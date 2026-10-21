// tx_input.hpp - where a transmission's audio comes from, as data (engine extraction stage 4, the fence).
//
// Moved out of core/transmitter.hpp, unchanged: the Transmit page's input combo names these and the transmitter is the engine's, which the
// interface may not include. The functions are still defined in transmitter.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TX_INPUT_HPP
#define CASCADE_CORE_TX_INPUT_HPP

namespace cascade::core {

// Where the audio comes from. MICROPHONE is what a transmitter is for; TONE
// is what answers "is anything coming out at all" without needing a
// microphone, a quiet room, or a second receiver to listen on.
enum class TxInput : int { Microphone = 0, Tone, Count };

inline constexpr int kTxInputCount = static_cast<int>(TxInput::Count);

const char* txInputName(TxInput in);
TxInput txInputFromIndex(int index);
bool txInputFromName(const char* name, TxInput& out);

}  // namespace cascade::core

#endif  // CASCADE_CORE_TX_INPUT_HPP
