// Reference cascade plugin: a minimal CASCADE_CAP_IQ_DECODER - the complex
// baseband capability added in plugin ABI 2 - that measures each block of raw
// IQ and reports it as a line of UTF-8 text.
//
// Per window of kWindowFrames complex samples it emits the RMS level, the
// peak instantaneous magnitude, and a coarse estimate of the strongest tone's
// offset from the tuned centre. The tone estimate is there on purpose: it is
// the cheapest measurement that is WRONG unless the interleaving is read
// correctly, so if this example ever prints a plausible frequency, the I/Q
// convention it demonstrates is genuinely being honoured.
//
// It is deliberately useless as a decoder and deliberately complete as an
// example: static descriptor, one exported symbol, no exception crossing the
// boundary, no allocation and no I/O in the real-time callbacks, an honest
// licence string, and an optional retune() that is actually implemented.
//
// WHAT A REAL IQ DECODER LOOKS LIKE. ADS-B is the mode this capability was
// added for: 1090 MHz, >= 2 MS/s, preamble detection and pulse-position
// demodulation on the magnitude envelope (I*I + Q*Q - no square root needed),
// then CRC-24 over the 56/112-bit frame. Everything it needs is in the same
// process() signature used below; the only differences are the size of the
// state and the fact that the answer is worth reading.
//
// SPDX-License-Identifier: MIT
//
// ===========================================================================
// HOW TO BUILD THIS BY HAND  (it is NOT part of the cascade CMake build, on
// purpose: plugins are separate downloads built by third parties, and adding
// it to the product build would defeat the point of the plugin host)
// ===========================================================================
//
// A plugin needs exactly ONE file from cascade: src/core/plugin_abi.h. Copy
// it next to this .cpp, or point the compiler at it with /I as shown below.
// A plugin never links against cascade and never includes anything else
// from its tree.
//
// Windows (from an "x64 Native Tools Command Prompt for VS 2022", in this
// directory - one line):
//
//   cl /nologo /LD /O2 /EHsc /W4 /std:c++17 /DCASCADE_PLUGIN_BUILD
//      /I "..\..\src\core" example_iq_plugin.cpp /Fe:example_iq_decoder.dll
//
// Then copy example_iq_decoder.dll into <cascade install dir>\plugins\ .
//
// Linux / macOS:
//
//   g++ -shared -fPIC -O2 -Wall -Wextra -std=c++17 -DCASCADE_PLUGIN_BUILD \
//       -I ../../src/core example_iq_plugin.cpp -o example_iq_decoder.so
//
// The source is C-compatible apart from the anonymous-namespace linkage, so
// it can equally be built with a C compiler after renaming to .c and
// replacing `namespace { }` with `static`.
// ===========================================================================

#include "plugin_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

// The ABI version this build claims. Overridable ONLY so a deliberately
// mismatched module can be produced for the host's refusal test; a real
// plugin just uses CASCADE_PLUGIN_ABI_VERSION.
#if defined(CASCADE_EXAMPLE_FORCE_ABI)
const uint32_t kClaimedAbi = CASCADE_EXAMPLE_FORCE_ABI;
#else
const uint32_t kClaimedAbi = CASCADE_PLUGIN_ABI_VERSION;
#endif

// Complex samples per report. Fixed rather than rate-derived so the output
// cadence is predictable at any input rate, and small enough that a test
// feeding a few thousand frames sees a line immediately.
const size_t kWindowFrames = 4096u;

// This decoder works at ANY rate (requiredRateHz 0) but would rather have
// 2.4 MS/s, the usual ADS-B capture rate - which is exactly the situation
// preferredRateHz exists for. The host may ignore it entirely, so create()
// must honour the rate it is actually given, which is what happens below.
const double kPreferredRateHz = 2400000.0;

// One decoder instance. Fixed-size storage throughout: process() runs on the
// host's real-time DSP thread, where an allocation is a latency spike and a
// bad_alloc would be an exception crossing the boundary.
struct Instance {
    double rate;        // as create() was told, Hz
    double centerHz;    // updated by retune()
    size_t frames;      // complex samples accumulated into this window
    double sumPower;    // sum of I*I + Q*Q
    double peakPower;   // max of I*I + Q*Q
    // Accumulated z[n] * conj(z[n-1]); its argument is the mean phase
    // advance per sample, i.e. the dominant tone's offset from DC.
    double lagRe;
    double lagIm;
    float prevI;
    float prevQ;
    bool havePrev;
    char pending[256];  // text produced but not yet collected by the host
    size_t pendingLen;
    size_t pendingRead;
};

void resetWindow(Instance* s) {
    s->frames = 0u;
    s->sumPower = 0.0;
    s->peakPower = 0.0;
    s->lagRe = 0.0;
    s->lagIm = 0.0;
    // prevI/prevQ deliberately survive: the phase-difference estimator is a
    // streaming measurement and dropping the carry-over sample once per
    // window would put one bogus difference into every report.
}

void* iqCreate(double rateHz, double centerHz) {
    // nothrow: the ABI says create() returns NULL on failure, and a throwing
    // allocation here would be undefined behaviour in the host's frame.
    Instance* s = new (std::nothrow) Instance();
    if (s == nullptr) {
        return nullptr;
    }
    // Honour what the host actually gave us. requiredRateHz is 0, so the host
    // is free to hand over any rate it has, and a plugin that quietly used
    // its preferred rate instead would mis-scale every frequency it reports.
    s->rate = (rateHz > 0.0) ? rateHz : kPreferredRateHz;
    s->centerHz = centerHz;
    s->prevI = 0.0f;
    s->prevQ = 0.0f;
    s->havePrev = false;
    s->pendingLen = 0u;
    s->pendingRead = 0u;
    resetWindow(s);
    return s;
}

void iqProcess(void* handle, const float* interleavedIq, size_t frames) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || (interleavedIq == nullptr && frames != 0u)) {
        return;  // defensive: a NULL handle can only mean a host bug, but
                 // faulting in someone else's process is never the answer
    }
    for (size_t n = 0; n < frames; ++n) {
        // THE INTERLEAVING RULE, in code: sample n is the PAIR at 2*n and
        // 2*n+1, and the buffer holds 2*frames floats. Iterating to `frames`
        // over interleavedIq[i] would read only the I components of the first
        // half of the block, and would look almost right on a noise floor.
        const float i = interleavedIq[2u * n];
        const float q = interleavedIq[2u * n + 1u];

        const double power = static_cast<double>(i) * i + static_cast<double>(q) * q;
        s->sumPower += power;
        if (power > s->peakPower) {
            s->peakPower = power;
        }
        if (s->havePrev) {
            // z[n] * conj(z[n-1]) = (i + jq)(prevI - j prevQ)
            s->lagRe += static_cast<double>(i) * s->prevI + static_cast<double>(q) * s->prevQ;
            s->lagIm += static_cast<double>(q) * s->prevI - static_cast<double>(i) * s->prevQ;
        }
        s->prevI = i;
        s->prevQ = q;
        s->havePrev = true;
        ++s->frames;

        if (s->frames < kWindowFrames) {
            continue;
        }
        // One window complete: emit a line, but only if the host has drained
        // the previous one. Dropping output is the correct back-pressure
        // behaviour for a real-time producer - blocking or growing a buffer
        // are both worse.
        if (s->pendingRead == s->pendingLen) {
            const double rms = std::sqrt(s->sumPower / static_cast<double>(s->frames));
            const double peak = std::sqrt(s->peakPower);
            // atan2 of the accumulated lag product = mean radians per sample;
            // scale by rate / 2pi for Hz. Unambiguous over +/- rate/2, which
            // is the whole baseband span, so no unwrapping is needed.
            const double radPerSample = std::atan2(s->lagIm, s->lagRe);
            const double toneHz = radPerSample * s->rate / (2.0 * 3.14159265358979323846);
            const int n2 = std::snprintf(s->pending, sizeof(s->pending),
                                         "example-iq: %u frames @ %.6g Hz, centre %.6g Hz, "
                                         "rms %.4f, peak %.4f, tone %+.0f Hz\n",
                                         static_cast<unsigned>(s->frames), s->rate, s->centerHz,
                                         rms, peak, toneHz);
            s->pendingLen = (n2 > 0) ? static_cast<size_t>(n2) : 0u;
            if (s->pendingLen >= sizeof(s->pending)) {
                s->pendingLen = sizeof(s->pending) - 1u;  // snprintf truncated
            }
            s->pendingRead = 0u;
        }
        resetWindow(s);
    }
}

// Optional in the ABI - implemented here to show the shape. The host
// null-checks this pointer, so a decoder that does not care where the
// receiver is tuned may leave it NULL in the table below.
void iqRetune(void* handle, double centerHz) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr) {
        return;
    }
    s->centerHz = centerHz;
}

int32_t iqPollText(void* handle, char* buf, size_t cap) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || buf == nullptr || cap == 0u) {
        return 0;
    }
    size_t avail = s->pendingLen - s->pendingRead;
    if (avail == 0u) {
        return 0;
    }
    if (avail > cap) {
        avail = cap;  // the host will collect the remainder next poll
    }
    std::memcpy(buf, s->pending + s->pendingRead, avail);
    s->pendingRead += avail;
    return static_cast<int32_t>(avail);
}

void iqDestroy(void* handle) { delete static_cast<Instance*>(handle); }

// Static, immutable, and outlives every call - as the ABI requires.
const CascadeIqDecoderApi kIqDecoder = {
    static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)),
    0.0,                 // requiredRateHz: 0 = whatever the device is running
    kPreferredRateHz,    // preferredRateHz: a hint, nothing more
    &iqCreate,
    &iqProcess,
    &iqRetune,           // may be NULL; implemented here to demonstrate it
    &iqPollText,
    &iqDestroy,
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    kClaimedAbi,
    "Example IQ Meter",
    "1.0.0",
    "cascade project",
    "MIT",  // declared honestly; the host displays this verbatim
    CASCADE_CAP_IQ_DECODER,
    0u,
    nullptr,      // no audio decoder: CASCADE_CAP_DECODER is not declared
    &kIqDecoder,
};

}  // namespace

// The one exported symbol. Returning NULL is the polite refusal when the host
// speaks a different ABI; the host also checks kDesc.abiVersion itself.
extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
#if !defined(CASCADE_EXAMPLE_FORCE_ABI)
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) {
        return nullptr;
    }
#else
    (void)hostAbiVersion;  // the mismatch build answers anyway, so that the
                           // host's own descriptor check is what refuses it
#endif
    return &kDesc;
}
