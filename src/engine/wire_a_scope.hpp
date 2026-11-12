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
            return true;
        default:
            return false;
    }
}

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_WIRE_A_SCOPE_HPP
