// tx_input.hpp - where a transmission's audio comes from, as data (engine extraction stage 4, the fence).
//
// Moved out of core/transmitter.hpp (with master's 0.99.74 Plugin input, merged): the Transmit page's input combo names these and the transmitter is the engine's, which the
// interface may not include. The functions are still defined in transmitter.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TX_INPUT_HPP
#define CASCADE_CORE_TX_INPUT_HPP

namespace cascade::core {

// Where the audio comes from. MICROPHONE is what a transmitter is for; TONE
// is what answers "is anything coming out at all" without needing a
// microphone, a quiet room, or a second receiver to listen on.
// PLUGIN (0.99.74) is a data-mode plugin's audio (core/tx_audio.hpp): it is NEVER
// something a hand selects - the page offers the first two, the config can
// name only the first two, and setInput(Plugin) lands on the tone - it is what
// the transmitter reads from when, and only when, the plugin's key is the only
// key down (activeInput()).
enum class TxInput : int { Microphone = 0, Tone, Plugin, Count };

inline constexpr int kTxInputCount = static_cast<int>(TxInput::Count);
// The inputs a person can choose from the page or write in a config file.
inline constexpr int kTxHandInputCount = 2;

const char* txInputName(TxInput in);
TxInput txInputFromIndex(int index);
bool txInputFromName(const char* name, TxInput& out);

}  // namespace cascade::core

#endif  // CASCADE_CORE_TX_INPUT_HPP
