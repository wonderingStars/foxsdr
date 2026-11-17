// cat_scope.hpp - where the CAT server may be reached from (owner decision 19, 2026-11-16: CAT emulation is for THIS computer only).
//
// Both windows used to draw a "Reachable from other machines" tick for the CAT server. The engine now refuses to start a CAT server beyond this computer without a typed
// list of allowed client addresses, and neither window has a field for that list, so the tick could only produce a refusal: it is not offered. A saved "bind all" value
// from an older version stays in the settings file (nothing rewrites it); the windows show the one sentence below and never offer a way to change it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::core {

// Whether a window offers the CAT "reachable from other machines" tick. A rule a test holds in both windows.
inline constexpr bool kCatReachableOffered = false;

}  // namespace cascade::core
