// Direction-finding maths for N phase-coherent channels (N up to 8): sample
// covariance, Hermitian eigen-decomposition (cyclic Jacobi, written here),
// Bartlett and MUSIC bearing spectra for a uniform circular array (UCA) and a
// uniform linear array (ULA), peak picking with a confidence figure, and a
// per-channel phase and gain calibration from a capture of one common source.
//
// MATHS ONLY: no hardware, no capture, no I/O. A later lane wires it to a
// coherent multi-channel source.
//
// Sources (opened; no SDR program's source was read, krakensdr_doa, pyArgus,
// gr-doa and every other GPL/LGPL/AGPL project were deliberately avoided):
//  * MUSIC steps (covariance, eigen-decomposition, noise subspace, pseudo-
//    spectrum 1 / (a^H U_N U_N^H a), number of sources must be known): English
//    Wikipedia "MUSIC (algorithm)", which cites R.O. Schmidt, "Multiple Emitter
//    Location and Signal Parameter Estimation", IEEE Trans. Antennas Propag.
//    AP-34 (March 1986) pp. 276-280 (the paper itself I did not open).
//    https://en.wikipedia.org/wiki/MUSIC_(algorithm)
//  * UCA plane-wave model: element m at (x_m, y_m) sees phase
//    2 pi (x_m cos(theta) + y_m sin(theta)) / lambda for a far-field wave from
//    azimuth theta: L. Werner, M. Gardill, M. Hutter, "Direction Finding for
//    Software Defined Radios with Switched Uniform Circular Arrays",
//    arXiv:2502.08592v1 (Feb 2025), eq. (1) and the MUSIC spectrum eq. (16).
//    https://arxiv.org/abs/2502.08592
//  * Bartlett (conventional) spectrum: the output power of the matched
//    (delay-and-sum) beamformer w = a / (a^H a), the sigma^2 I special case of
//    the MVDR solution in S. Loyka, ELG5132 lecture 7, eqs (7.8)-(7.14),
//    https://www.site.uottawa.ca/~sloyka/elg5132/Lec_7_ELG5132.pdf
//  * Cyclic Jacobi for a Hermitian matrix: a phase rotation that makes the
//    pivot real followed by the classical real Jacobi rotation (standard
//    numerical linear algebra; derived and checked by the test here).
//  * Calibration model: x_n = c_n s + noise with c_n the unknown complex gain of
//    channel n and s the common source. R = E[x x^H] = P c c^H + noise, so the
//    dominant eigenvector of R is c up to one complex scalar; this is derived
//    here, no source claims it. A common-source phase/gain calibration is the
//    approach Werner et al. mention only in the abstract ("a central reference
//    antenna mitigates phase jitter") and I did not find a free detailed
//    description, so the method is stated as my own derivation.
//
// Conventions: bearing theta in radians, UCA elements at angles 2 pi m / N,
// element 0 on the +x axis, theta measured from +x counter-clockwise; ULA
// elements at x = m d along the axis, theta measured FROM THE AXIS (end-fire =
// 0), element m phase = k m d cos(theta). Steering vector entries are
// exp(+j k (x_m cos(theta) + y_m sin(theta))) (UCA) or exp(+j k m d cos theta)
// (ULA), k = 2 pi f / c.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace cascade::dsp::doa {

using cd = std::complex<double>;
constexpr int kMaxChannels = 8;
constexpr double kSpeedOfLight = 299792458.0;

// Row-major N x N complex matrix.
struct Matrix {
    int n = 0;
    std::vector<cd> a;
    Matrix() = default;
    explicit Matrix(int size) : n(size), a(static_cast<std::size_t>(size) * size) {}
    cd& at(int r, int c) { return a[static_cast<std::size_t>(r) * n + c]; }
    const cd& at(int r, int c) const { return a[static_cast<std::size_t>(r) * n + c]; }
};

// R = (1/T) sum_t x_t x_t^H, T = count, channels[n][t]. N = 1..8, else an empty
// matrix. Valid for coherent, equal-length, sample-aligned channels.
Matrix sampleCovariance(const std::complex<float>* const* channels, int numChannels, std::size_t count);

struct Eigen {
    std::vector<double> values;   // descending
    Matrix vectors;               // column k is the unit eigenvector of values[k]
    int sweeps = 0;
    bool converged = false;
};

// Hermitian eigen-decomposition by cyclic Jacobi. Only the Hermitian part of
// `h` is used ((h + h^H)/2). Any N from 1 to 8 (larger works, slower).
Eigen hermitianEigen(const Matrix& h);

enum class ArrayType { Circular, Linear };

struct Geometry {
    ArrayType type = ArrayType::Circular;
    int elements = 5;
    // Circular: radius in metres. Linear: element spacing in metres.
    double sizeMetres = 0.0;
    double frequencyHz = 0.0;
};

// Steering vector for bearing `theta` (radians), unit-modulus entries.
std::vector<cd> steering(const Geometry& g, double theta);

// Bearing grid: Circular covers [0, 2 pi), Linear covers [0, pi] (the other half
// is the mirror image and cannot be told apart; see the ambiguity note below).
std::vector<double> bearingGrid(const Geometry& g, double stepRadians);

// Bartlett spectrum, normalised so one unit-power plane wave gives 1 at its
// bearing: P = a^H R a / (a^H a)^2.
std::vector<double> bartlettSpectrum(const Matrix& r, const Geometry& g, const std::vector<double>& grid);

// MUSIC pseudo-spectrum for `sources` signals: 1 / (a^H U_N U_N^H a) with a
// normalised to unit length (so the value is >= 1). Eigenvectors are taken from
// `eig` (a decomposition of the sample covariance), sources in [1, N-1].
std::vector<double> musicSpectrum(const Eigen& eig, const Geometry& g, const std::vector<double>& grid, int sources);

struct Peak {
    double bearing = 0.0;        // radians, parabolic-refined
    double height = 0.0;
    double peakToMedianDb = 0.0; // 10 log10(height / median of the spectrum)
    // Dimensionless, 0..1, monotone in peakToMedianDb: 1 - 10^(-peakToMedianDb/20)
    // (6 dB -> 0.5, 20 dB -> 0.9). A HEURISTIC figure for display and gating, not
    // a probability; the test pins how it behaves against SNR and on noise.
    double confidence = 0.0;
};

// The `count` highest local maxima (wrap-around for a circular grid), highest
// first, with parabolic refinement of the bearing.
std::vector<Peak> pickPeaks(const std::vector<double>& spectrum, const std::vector<double>& grid,
                            bool wrap, int count);

// Simple source-count heuristic: number of eigenvalues above `factor` x the
// smallest eigenvalue (default 4 = 6 dB). A heuristic, NOT an information
// criterion; callers that know the source count should pass it.
int countAboveNoise(const Eigen& eig, double factor = 4.0);

// Phase and gain calibration from the covariance of a capture of ONE common
// source feeding every channel (a noise source through a splitter). Returns the
// complex factor k_n for each channel such that k_n x_n lines up with channel 0
// (k_0 = 1). Phase in degrees and gain in dB are also given.
struct Calibration {
    std::vector<cd> factor;
    std::vector<double> phaseDeg;
    std::vector<double> gainDb;
    double dominance = 0.0;  // lambda_1 / sum of the others: a high value means a clean common source
    bool ok = false;
};
Calibration calibrateFromCommonSource(const Matrix& commonCovariance);

// Applies factors to a covariance: R' = K R K^H.
Matrix applyCalibration(const Matrix& r, const std::vector<cd>& factor);

}  // namespace cascade::dsp::doa
