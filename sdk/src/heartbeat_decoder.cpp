// FoxSDR plugin SDK template: a decoder that says "heartbeat N" once for every second of audio it is fed.
//
// It decodes nothing, on purpose. It is the smallest complete CASCADE_CAP_DECODER: a static descriptor, one exported
// symbol, no exception across the boundary, no allocation and no I/O in the real-time callbacks, and an honest licence
// string. Copy this file, rename the plugin below, and replace decoderProcess() with your decoder.
//
// What the host does with it (docs/PLUGIN-API.md and src/core/plugin_abi.h are the contract; where they disagree with this
// file, they are right):
//   * the host feeds mono 32-bit float audio at requiredRateHz (it resamples to that rate) from decoderProcess's thread,
//     which is a REAL-TIME thread: no blocking, no allocation, no I/O, no settings call there;
//   * the host collects text by calling decoderPollText from another thread; whatever you write there is shown in the
//     shared Decoder output window and is never sent anywhere by the host;
//   * the "second" here is counted in SAMPLES, not by a clock: a stream whose sample rate is a little off still gets one
//     line per `rate` samples, which is how every sample-counting decoder behaves.
//
// SPDX-License-Identifier: MIT
#include "plugin_abi.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace {

// The audio rate this decoder asks the host to deliver. 0 would mean "any rate"; a fixed rate lets the host resample once
// for you. It must be 0 or inside [CASCADE_AUDIO_RATE_MIN_HZ, CASCADE_AUDIO_RATE_MAX_HZ].
constexpr uint32_t kRateHz = 8000u;

// One decoder instance. Fixed-size storage only: decoderProcess() and decoderPollText() must never allocate.
struct Instance {
    uint32_t rate;          // samples per heartbeat
    uint32_t counted;       // samples seen in the current second
    uint32_t seconds;       // seconds completed since create(); the number in the line
    char pending[64];       // one line, produced but not yet collected
    size_t pendingLen;
    size_t pendingRead;
};

void* decoderCreate(uint32_t rateHz) {
    // nothrow: create() returns NULL on failure, and an exception here would cross the plugin boundary.
    Instance* s = new (std::nothrow) Instance();
    if (s == nullptr) {
        return nullptr;
    }
    s->rate = rateHz != 0u ? rateHz : kRateHz;
    s->counted = 0u;
    s->seconds = 0u;
    s->pendingLen = 0u;
    s->pendingRead = 0u;
    return s;
}

// The audio itself is not looked at: a real decoder would run its demodulator here.
void decoderProcess(void* handle, const float* samples, size_t count) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || (samples == nullptr && count != 0u)) {
        return;  // a NULL handle can only be a host bug, but faulting in someone else's process is never the answer
    }
    (void)samples;
    size_t remaining = count;
    while (remaining != 0u) {
        const size_t room = static_cast<size_t>(s->rate - s->counted);
        const size_t step = remaining < room ? remaining : room;
        s->counted += static_cast<uint32_t>(step);
        remaining -= step;
        if (s->counted < s->rate) {
            break;
        }
        s->counted = 0u;
        ++s->seconds;
        // Emit only if the host has collected the previous line. Dropping output is the right back-pressure for a
        // real-time producer: blocking or growing a buffer are both worse. The number in the next line shows the gap.
        if (s->pendingRead == s->pendingLen) {
            const int n = std::snprintf(s->pending, sizeof(s->pending), "heartbeat %u\n", s->seconds);
            s->pendingLen = (n > 0) ? static_cast<size_t>(n) : 0u;
            if (s->pendingLen >= sizeof(s->pending)) {
                s->pendingLen = sizeof(s->pending) - 1u;
            }
            s->pendingRead = 0u;
        }
    }
}

// Copies up to `cap` bytes of pending text into `buf` and returns how many; 0 means nothing to say. The rest is kept for
// the next call, so a small buffer loses nothing.
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
        avail = cap;
    }
    std::memcpy(buf, s->pending + s->pendingRead, avail);
    s->pendingRead += avail;
    return static_cast<int32_t>(avail);
}

void decoderDestroy(void* handle) { delete static_cast<Instance*>(handle); }

// Static, immutable, and alive for as long as the module is mapped: the host keeps pointers into these.
const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)),
    kRateHz,
    &decoderCreate,
    &decoderProcess,
    &decoderPollText,
    &decoderDestroy,
};

// ABI 3 declares capabilities through a counted table, one entry per bit set in `capabilities`. tableSize is the size of
// the table AS THIS PLUGIN COMPILED IT, never a literal: it is what lets a host that knows a different version of the
// table skip just this capability instead of refusing the plugin.
const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Heartbeat Decoder",  // shown to the user; make it yours
    "1.0.0",              // keep equal to PROJECT_VERSION in CMakeLists.txt, which is also in the file name
    "Your name here",
    "MIT",                // declared honestly; the host displays it verbatim
    CASCADE_CAP_DECODER,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

// The one exported symbol. Returning NULL is the polite refusal when the host speaks a different ABI.
extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) {
        return nullptr;
    }
    return &kDesc;
}
