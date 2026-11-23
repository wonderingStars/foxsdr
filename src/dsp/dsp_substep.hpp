// DSP_SUBSTEP: scoped timers for the demod benchmark (tools/bench_demod_chain), compiled ONLY when CASCADE_BENCH_SUBSTEPS is defined, which
// only that tool does (it compiles the handful of dsp sources it times itself, with the definition). In every other build the macro is
// `((void)0)` and the shipped objects are what they were.
//
//   { DSP_SUBSTEP("vfo.mix"); ...the step... }
//
// The span is read from the cycle counter at the scope's two ends, once per step per BLOCK (never per sample), and added to a per-name total
// the benchmark prints. Names are string literals; the table is keyed by the literal's address and merged by text when printed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#if defined(CASCADE_BENCH_SUBSTEPS)

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace cascade::dsp::bench {

struct SubstepSlot {
    const char* name = nullptr;
    std::uint64_t cycles = 0;
    std::uint64_t calls = 0;
};
constexpr unsigned kMaxSubsteps = 64;
inline SubstepSlot g_substeps[kMaxSubsteps];

inline SubstepSlot& substepSlot(const char* name) {
    for (unsigned i = 0; i < kMaxSubsteps; ++i) {
        if (g_substeps[i].name == name) { return g_substeps[i]; }
        if (g_substeps[i].name == nullptr) {
            g_substeps[i].name = name;
            return g_substeps[i];
        }
    }
    return g_substeps[kMaxSubsteps - 1];
}

class ScopedStep {
public:
    explicit ScopedStep(const char* name) : slot_(substepSlot(name)), t0_(__rdtsc()) {}
    ~ScopedStep() { stop(); }
    // Ends the span early (DSP_STEP_END), for a step that is not a scope of its own; the destructor then adds nothing.
    void stop() {
        if (open_) {
            slot_.cycles += __rdtsc() - t0_;
            ++slot_.calls;
            open_ = false;
        }
    }
    ScopedStep(const ScopedStep&) = delete;
    ScopedStep& operator=(const ScopedStep&) = delete;

private:
    SubstepSlot& slot_;
    std::uint64_t t0_;
    bool open_ = true;
};

}  // namespace cascade::dsp::bench

#define DSP_SUBSTEP_JOIN2(a, b) a##b
#define DSP_SUBSTEP_JOIN(a, b) DSP_SUBSTEP_JOIN2(a, b)
#define DSP_SUBSTEP(name) ::cascade::dsp::bench::ScopedStep DSP_SUBSTEP_JOIN(dspSubstep_, __LINE__)(name)
// A step that cannot be wrapped in a scope without re-indenting the code around it: DSP_STEP_BEGIN(id, "name") ... DSP_STEP_END(id).
#define DSP_STEP_BEGIN(id, name) ::cascade::dsp::bench::ScopedStep id(name)
#define DSP_STEP_END(id) id.stop()

#else

#define DSP_SUBSTEP(name) ((void)0)
#define DSP_STEP_BEGIN(id, name) ((void)0)
#define DSP_STEP_END(id) ((void)0)

#endif
