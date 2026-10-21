// Maidenhead grid locator conversion - the receiver-position half of
// CASCADE_CAP_RECEIVER_LOCATOR's receiverLocator (plugin_abi.h, 0.99.43).
//
// A pure function, deliberately with no dependency on the rest of the app, so
// it can be unit-tested directly against known reference points rather than
// only indirectly through PluginApiCore::getState.
#pragma once

#include <cstdio>

namespace cascade::core {

// Six-character Maidenhead grid (field + square + subsquare), e.g. "IO91wm"
// for London (51.5074, -0.1278). `out` must be at least 7 bytes; the result
// is NUL-terminated. Returns false (and leaves `out` empty) for a coordinate
// pair outside latDeg in [-90, 90] or lonDeg in [-180, 180], or non-finite -
// never a guess dressed as a grid.
//
// Field:  20 deg longitude x 10 deg latitude -> 'A'..'R'
// Square:  2 deg longitude x  1 deg latitude -> '0'..'9'
// Subsquare: 5 min longitude x 2.5 min latitude -> 'a'..'x' (lowercase, the
//   convention every ham radio tool - including PSK Reporter's own worked
//   example, "FN42hn" - uses for the third pair).
inline bool maidenheadGrid6(double latDeg, double lonDeg, char out[7]) {
    out[0] = '\0';
    if (!(latDeg >= -90.0 && latDeg <= 90.0)) { return false; }
    if (!(lonDeg >= -180.0 && lonDeg <= 180.0)) { return false; }

    // Clamp the pole/antimeridian edge cases into the last valid cell rather
    // than overflowing a field/square/subsquare index by one.
    double lonAdj = lonDeg + 180.0;
    double latAdj = latDeg + 90.0;
    if (lonAdj >= 360.0) { lonAdj = 359.999999; }
    if (latAdj >= 180.0) { latAdj = 179.999999; }
    if (lonAdj < 0.0) { lonAdj = 0.0; }
    if (latAdj < 0.0) { latAdj = 0.0; }

    const int fieldLon = static_cast<int>(lonAdj / 20.0);
    const int fieldLat = static_cast<int>(latAdj / 10.0);

    const double afterFieldLon = lonAdj - static_cast<double>(fieldLon) * 20.0;
    const double afterFieldLat = latAdj - static_cast<double>(fieldLat) * 10.0;

    const int squareLon = static_cast<int>(afterFieldLon / 2.0);
    const int squareLat = static_cast<int>(afterFieldLat / 1.0);

    const double afterSquareLon = afterFieldLon - static_cast<double>(squareLon) * 2.0;
    const double afterSquareLat = afterFieldLat - static_cast<double>(squareLat) * 1.0;

    int subLon = static_cast<int>(afterSquareLon / (2.0 / 24.0));
    int subLat = static_cast<int>(afterSquareLat / (1.0 / 24.0));
    if (subLon > 23) { subLon = 23; }
    if (subLon < 0) { subLon = 0; }
    if (subLat > 23) { subLat = 23; }
    if (subLat < 0) { subLat = 0; }

    out[0] = static_cast<char>('A' + fieldLon);
    out[1] = static_cast<char>('A' + fieldLat);
    out[2] = static_cast<char>('0' + squareLon);
    out[3] = static_cast<char>('0' + squareLat);
    out[4] = static_cast<char>('a' + subLon);
    out[5] = static_cast<char>('a' + subLat);
    out[6] = '\0';
    return true;
}

}  // namespace cascade::core
