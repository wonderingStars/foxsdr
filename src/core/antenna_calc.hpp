// antenna_calc.hpp - antenna calculators: wavelength, half-wave dipole, quarter-wave ground plane, Yagi element starting lengths,
// coax length from velocity factor, free-space path loss. Row Q7a of the gaps survey. STANDALONE and PURE; not wired.
//
// UNITS ARE IN THE NAMES: MHz in, metres or dB out. Every function returns std::nullopt for a non-finite, zero or negative input
// or a value outside the stated range; nothing is clamped silently.
//
// SOURCES (read 2026-11-11)
//  * Speed of light c = 299 792 458 m/s (exact, SI definition).
//  * Half-wave dipole: length about 143 / f metres, 468 / f feet (f in MHz): Wikipedia, "Dipole antenna",
//    https://en.wikipedia.org/wiki/Dipole_antenna ; the page gives the shortening factor k as 0.98 (thin wire) to 0.94 (thick), and
//    the free-space half wave as about 150 / f m. Here the end-effect factor is a parameter, default 0.954 (143 / 149.896).
//  * Quarter-wave monopole = half a dipole over its ground image (same page); radiator length is a quarter of the dipole formula.
//  * Free-space path loss: Wikipedia, "Free-space path loss", https://en.wikipedia.org/wiki/Free-space_path_loss : FSPL(dB) =
//    20 log10(d) + 20 log10(f) + 32.45 for d in km and f in MHz; the page's worked example 10 km at 2.4 GHz is 120.05 dB. Here the
//    constant is computed from c (20 log10(4 pi 1e9 / c) = 32.4478), not typed. Valid in the far field only (d much greater than the
//    wavelength): fsplFarField() reports whether d is at least 10 wavelengths.
//  * Velocity factor: Wikipedia, "Velocity factor", https://en.wikipedia.org/wiki/Velocity_factor : the ratio of wave speed in the
//    medium to c; coax about 0.66 solid polyethylene, 0.77 to 0.86 foam, about 0.93 air-spaced. Physical length = VF x fraction x c / f
//    (derived; the page gives the worked 100 MHz, 0.82, quarter-wave example, 0.62 m, which is the test).
//  * Yagi starting lengths: reflector 0.495 wavelength, driven element 0.473, directors 0.440 stepping down 0.005 per director, spacing
//    0.20 / 0.15 / 0.25 wavelength. These are RULES OF THUMB FROM MEMORY of amateur designs and are NOT from a source read this
//    session: UNVERIFIED. They are STARTING points to be tuned with an analyser or a model, and are labelled so in the result.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <optional>
#include <vector>

namespace cascade::core::antenna {

inline constexpr double kSpeedOfLight = 299792458.0;   // m/s
inline constexpr double kMinMHz = 0.01;                // 10 kHz
inline constexpr double kMaxMHz = 300000.0;            // 300 GHz
inline constexpr double kMetresPerFoot = 0.3048;

inline bool freqOk(double fMHz) { return std::isfinite(fMHz) && fMHz >= kMinMHz && fMHz <= kMaxMHz; }

inline std::optional<double> wavelengthM(double fMHz) {
    if (!freqOk(fMHz)) return std::nullopt;
    return kSpeedOfLight / (fMHz * 1e6);
}

// Half-wave dipole, cut length in metres. k: end-effect shortening factor, 0.90 to 1.0 (default 0.954 which equals 143 / f).
inline std::optional<double> dipoleLengthM(double fMHz, double k = 0.954) {
    const auto w = wavelengthM(fMHz);
    if (!w || !std::isfinite(k) || k < 0.90 || k > 1.0) return std::nullopt;
    return 0.5 * *w * k;
}

struct GroundPlane {
    double radiatorM = 0;
    double radialM = 0;    // each radial, 5 percent longer than the radiator (rule of thumb, UNVERIFIED)
};

inline std::optional<GroundPlane> groundPlaneM(double fMHz, double k = 0.954) {
    const auto d = dipoleLengthM(fMHz, k);
    if (!d) return std::nullopt;
    GroundPlane g;
    g.radiatorM = *d / 2.0;
    g.radialM = g.radiatorM * 1.05;
    return g;
}

struct YagiElement {
    const char* role;     // "reflector", "driven", "director"
    double lengthM;
    double positionM;     // along the boom from the reflector
};

// elements >= 2 (reflector + driven) and <= 20. Starting values only (UNVERIFIED rule of thumb, see the header).
inline std::optional<std::vector<YagiElement>> yagiStartingLengths(double fMHz, int elements) {
    const auto w = wavelengthM(fMHz);
    if (!w || elements < 2 || elements > 20) return std::nullopt;
    std::vector<YagiElement> v;
    v.push_back({"reflector", 0.495 * *w, 0.0});
    v.push_back({"driven", 0.473 * *w, 0.20 * *w});
    double pos = 0.20 * *w;
    for (int i = 0; i < elements - 2; ++i) {
        pos += (i == 0 ? 0.15 : 0.25) * *w;
        double len = (0.440 - 0.005 * i) * *w;
        v.push_back({"director", len, pos});
    }
    return v;
}

// Physical length in metres of `fraction` of a wavelength in a cable of velocity factor vf (0 < vf <= 1), fraction in (0, 100].
inline std::optional<double> coaxLengthM(double fMHz, double fraction, double vf) {
    const auto w = wavelengthM(fMHz);
    if (!w || !std::isfinite(fraction) || fraction <= 0.0 || fraction > 100.0 || !std::isfinite(vf) || vf <= 0.0 || vf > 1.0)
        return std::nullopt;
    return vf * fraction * *w;
}

// Free-space path loss in dB for a distance in km and a frequency in MHz.
inline std::optional<double> fsplDb(double distanceKm, double fMHz) {
    if (!freqOk(fMHz) || !std::isfinite(distanceKm) || distanceKm <= 0.0 || distanceKm > 1e9) return std::nullopt;
    const double constant = 20.0 * std::log10(4.0 * 3.14159265358979323846 * 1e9 / kSpeedOfLight);
    return 20.0 * std::log10(distanceKm) + 20.0 * std::log10(fMHz) + constant;
}

// True when the distance is at least 10 wavelengths (the formula assumes far field).
inline bool fsplFarField(double distanceKm, double fMHz) {
    const auto w = wavelengthM(fMHz);
    return w && std::isfinite(distanceKm) && distanceKm * 1000.0 >= 10.0 * *w;
}

inline double metresToFeet(double m) { return m / kMetresPerFoot; }

}  // namespace cascade::core::antenna
