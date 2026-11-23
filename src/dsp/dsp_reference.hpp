// dsp_reference.hpp - the switch that puts the optimised receive kernels back on their REFERENCE paths (2026-11-23, the demod performance pass).
//
// The FIR kernel (dsp/fir.cpp), the receive oscillator (dsp/nco_fast.hpp) and the stereo decoder's carrier keep the loops they had before the pass as
// their reference paths, selected here. NOTHING in the application ever sets it: it exists so a test or the benchmark can run the whole receive chain
// twice in one process, old kernels and new, on the same samples, and compare what comes out (tests/test_dsp_equivalence.cpp,
// tools/bench_demod_chain). It is read once per call of a kernel (one relaxed atomic load), never per sample.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>

namespace cascade::dsp {

// One bit per optimised kernel, so a test can switch back exactly the one it is proving (the others keep their fast paths).
enum RefKernel : unsigned {
    kRefFir = 1u,            // FirDecimator: the scalar loop
    kRefNco = 2u,            // FastNco: the library cos and sin of every sample
    kRefStereoCarrier = 4u,  // StereoFm: the 38 kHz carrier is computed even while the gate is shut
    kRefAll = 7u,
};

namespace detail {
inline std::atomic<unsigned> g_referenceKernels{0u};
}  // namespace detail

// The set of kernels that run their reference paths (a mask of RefKernel). Process-wide; test and benchmark use only.
inline void setReferenceKernels(unsigned mask) { detail::g_referenceKernels.store(mask, std::memory_order_relaxed); }
inline bool referenceKernel(RefKernel k) { return (detail::g_referenceKernels.load(std::memory_order_relaxed) & static_cast<unsigned>(k)) != 0u; }

}  // namespace cascade::dsp
