// cat_scope.hpp - where the CAT server may be reached from (owner decision 19, 2026-11-16: CAT emulation is for THIS computer only).
//
// Both windows used to draw a "Reachable from other machines" tick for the CAT server. The engine now refuses to start a CAT server beyond this computer without a typed
// list of allowed client addresses, and neither window has a field for that list, so the tick could only produce a refusal: it is not offered. A saved "bind all" value
// from an older version stays in the settings file (nothing rewrites it); the windows show the one sentence below and never offer a way to change it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace cascade::core {

// Whether a window offers the CAT "reachable from other machines" tick. A rule a test holds in both windows.
inline constexpr bool kCatReachableOffered = false;

// Whether the CAT server really listens beyond this computer, given the saved "reachable" value and the typed list of computers. WHILE THE TICK IS NOT OFFERED a saved true with no list
// means nothing the user can act on (no window can type the list), so it is read as "this computer only": CAT starts on the loopback address and no fault is shown (engine merge 11, row
// SF2-cat-ui). A list that was typed on purpose (SERVER_CONFIG, or the headless --cat-allow) keeps the listener beyond this computer, for those computers only.
inline bool catReachesOtherComputers(bool savedBindAll, const std::string& allowList) {
    return savedBindAll && (kCatReachableOffered || !allowList.empty());
}

}  // namespace cascade::core
