// radio_refusal_data.hpp - the engine's half of "a refused radio choice says why" (core/radio_refusal.hpp): who holds the radio, as the two numbers a refusal carries.
//
// applied[0] = the holder's kind (core::refusal::kHolder*), applied[1] = the number the sentence names. WHO MAY HOLD A RADIO is the ledger's rule (device_ledger.hpp) and is
// not decided here: this only describes the holder the ledger already named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_RADIO_REFUSAL_DATA_HPP
#define CASCADE_ENGINE_RADIO_REFUSAL_DATA_HPP

#include "core/radio_refusal.hpp"
#include "engine/device_ledger.hpp"

namespace cascade::engine {

struct RefusalHolder {
    int kind = 0;
    int number = 0;
};

inline RefusalHolder refusalHolderOf(const DeviceHolder& h) {
    namespace rf = cascade::core::refusal;
    switch (h.kind) {
        case DeviceHolder::Kind::Receiver: return {rf::kHolderReceiver, static_cast<int>(h.id) + 1};
        case DeviceHolder::Kind::PatchNode: return {rf::kHolderPatchPart, static_cast<int>(h.id)};
        case DeviceHolder::Kind::Transmitter: return {rf::kHolderTransmitter, 0};
        case DeviceHolder::Kind::None: break;
    }
    return {};
}

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_RADIO_REFUSAL_DATA_HPP
