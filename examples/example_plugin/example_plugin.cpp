// Reference cascade plugin: a minimal CASCADE_CAP_DECODER that reports the
// RMS level of the demodulated audio once per second as a line of UTF-8 text.
//
// It is deliberately useless as a decoder and deliberately complete as an
// example: it shows every rule of the ABI being obeyed - static descriptor,
// single exported symbol, no exception crossing the boundary, no allocation
// and no I/O in the real-time callbacks, an honest licence string.
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
//      /I "..\..\src\core" example_plugin.cpp /Fe:example_decoder.dll
//
// Then copy example_decoder.dll into <cascade install dir>\plugins\ .
//
// To produce the deliberately-incompatible build used to prove that the host
// refuses a mismatched ABI, add /DCASCADE_EXAMPLE_FORCE_ABI=999 :
//
//   cl /nologo /LD /O2 /EHsc /W4 /std:c++17 /DCASCADE_PLUGIN_BUILD
//      /DCASCADE_EXAMPLE_FORCE_ABI=999 /I "..\..\src\core"
//      example_plugin.cpp /Fe:bad_abi_decoder.dll
//
// Linux / macOS:
//
//   g++ -shared -fPIC -O2 -Wall -Wextra -std=c++17 -DCASCADE_PLUGIN_BUILD \
//       -I ../../src/core example_plugin.cpp -o example_decoder.so
//
// The source is C-compatible apart from the anonymous-namespace linkage, so
// it can equally be built with a C compiler after renaming to .c and
// replacing `namespace { }` with `static`.
// ===========================================================================

#include "plugin_abi.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <new>

namespace {

// The ABI version this build claims. Overridable ONLY so the second build
// above can produce a module the host must refuse; a real plugin just uses
// CASCADE_PLUGIN_ABI_VERSION.
#if defined(CASCADE_EXAMPLE_FORCE_ABI)
const uint32_t kClaimedAbi = CASCADE_EXAMPLE_FORCE_ABI;
#else
const uint32_t kClaimedAbi = CASCADE_PLUGIN_ABI_VERSION;
#endif

// Audio rate this decoder wants; the host resamples to it before create().
const uint32_t kRateHz = 8000u;

// One decoder instance. Fixed-size storage throughout: process() and
// poll_text() run on the host's real-time thread, where an allocation is a
// latency spike and a bad_alloc would be an exception crossing the boundary.
struct Instance {
    uint32_t rate;
    uint32_t counted;   // samples accumulated into the current window
    double sumSquares;
    char pending[256];  // text produced but not yet collected by the host
    size_t pendingLen;
    size_t pendingRead;
};

void* decoderCreate(uint32_t rateHz) {
    // nothrow: the ABI says create() returns NULL on failure, and a throwing
    // allocation here would be undefined behaviour in the host's frame.
    Instance* s = new (std::nothrow) Instance();
    if (s == nullptr) {
        return nullptr;
    }
    s->rate = rateHz != 0u ? rateHz : kRateHz;
    s->counted = 0u;
    s->sumSquares = 0.0;
    s->pendingLen = 0u;
    s->pendingRead = 0u;
    return s;
}

void decoderProcess(void* handle, const float* samples, size_t count) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || (samples == nullptr && count != 0u)) {
        return;  // defensive: a NULL handle can only mean a host bug, but
                 // faulting in someone else's process is never the answer
    }
    for (size_t i = 0; i < count; ++i) {
        const double v = static_cast<double>(samples[i]);
        s->sumSquares += v * v;
        ++s->counted;
        if (s->counted < s->rate) {
            continue;
        }
        // One window complete: emit a line, but only if the host has drained
        // the previous one. Dropping output is the correct back-pressure
        // behaviour for a real-time producer - blocking or growing a buffer
        // are both worse.
        if (s->pendingRead == s->pendingLen) {
            const double rms = std::sqrt(s->sumSquares / static_cast<double>(s->counted));
            const int n = std::snprintf(s->pending, sizeof(s->pending),
                                        "example: %u samples, rms %.4f\n", s->counted, rms);
            s->pendingLen = (n > 0) ? static_cast<size_t>(n) : 0u;
            if (s->pendingLen >= sizeof(s->pending)) {
                s->pendingLen = sizeof(s->pending) - 1u;  // snprintf truncated
            }
            s->pendingRead = 0u;
        }
        s->counted = 0u;
        s->sumSquares = 0.0;
    }
}

int32_t decoderPollText(void* handle, char* buf, size_t cap) {
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

void decoderDestroy(void* handle) { delete static_cast<Instance*>(handle); }

// Static, immutable, and outlives every call - as the ABI requires.
const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)),
    kRateHz,
    &decoderCreate,
    &decoderProcess,
    &decoderPollText,
    &decoderDestroy,
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    kClaimedAbi,
    "Example RMS Reporter",
    "1.0.0",
    "cascade project",
    "MIT",  // declared honestly; the host displays this verbatim
    CASCADE_CAP_DECODER,
    0u,
    &kDecoder,
};

}  // namespace

// The one exported symbol. Returning NULL is the polite refusal when the host
// speaks a different ABI; the host also checks kDesc.abiVersion itself, which
// is what the /DCASCADE_EXAMPLE_FORCE_ABI build exercises.
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
