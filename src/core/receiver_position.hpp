// receiver_position.hpp - the one rule for whether a latitude/longitude pair
// can be the receiver's own position.
//
// WHY IT LIVES HERE. It is applied at every door a position comes in by: the
// typed entry and the one-click offers (the window), the config file on load
// (core/config.cpp) and a GPS fix (core/gps_reader.cpp). Until the engine was
// split from the window (engine extraction, step 1) it sat in
// gui/scope_view.hpp and the two engine-side doors reached into gui/ for it.
// It moved here VERBATIM; gui/scope_view.hpp brings it back into cascade::gui
// with a using-declaration, so the window's callers read exactly as before
// and every door still applies the same one rule, not a copy of it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RECEIVER_POSITION_HPP
#define CASCADE_CORE_RECEIVER_POSITION_HPP

#include <cmath>

namespace cascade::core {

// WHETHER A PAIR CAN BE THE RECEIVER. Finite and on the globe, and NOT the
// exact origin: 0 N 0 E is a point in the Gulf of Guinea that nobody who
// types, clicks or presses a key on this application is at, and it is the
// value every empty field, every unset map and every zeroed struct holds. A
// user's scope was found measuring from there, with the view dragged three
// thousand miles to the coast it should have been on; the position had been
// set from a control whose inputs still read 0.00000. The application
// refuses it at every door - the typed entry, the one-click offers, the
// config file - so that "unset" and "set to nothing" can never be the same
// picture.
inline bool receiverPositionAcceptable(double latDeg, double lonDeg) {
    if (!std::isfinite(latDeg) || !std::isfinite(lonDeg)) { return false; }
    if (latDeg < -90.0 || latDeg > 90.0 || lonDeg < -180.0 || lonDeg > 180.0) { return false; }
    if (latDeg == 0.0 && lonDeg == 0.0) { return false; }
    return true;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_RECEIVER_POSITION_HPP
