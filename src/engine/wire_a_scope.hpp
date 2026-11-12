// wire_a_scope.hpp - which ops of the wire-a block address ONE radio (the target), for engine/radio_scope.hpp's isRadioScopedOp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_WIRE_A_SCOPE_HPP
#define CASCADE_ENGINE_WIRE_A_SCOPE_HPP

#include <cstdint>

#include "core/app_commands.hpp"

namespace cascade::engine {

inline bool isWireARadioScoped(std::uint32_t op) {
    switch (op) {
        case FOXAPP_OP_HISTORY_STEP:
        case FOXAPP_OP_CHANNEL:
        case FOXAPP_OP_BAND_MEMORY_NOTE:
        case FOXAPP_OP_RECORDINGS_OPEN:
        case FOXAPP_OP_WF_HISTORY:
        case FOXAPP_OP_KEY_ACTION:
        case FOXAPP_OP_SCAN_PLAN:
        case FOXAPP_OP_FAVOURITE_SAVE:
        case FOXAPP_OP_FAVOURITE_RECALL:
            return true;
        default:
            return false;
    }
}

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_WIRE_A_SCOPE_HPP
