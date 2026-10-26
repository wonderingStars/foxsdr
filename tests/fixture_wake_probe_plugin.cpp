// fixture_wake_probe_plugin.cpp - the module that shows WHICH BLOCK a woken plugin decoded first (stub campaign B2, PLUGIN-RUN;
// tests/test_engine_api_03.cpp group N).
//
// A TEXT DECODER on the audio stream that says one line for every block it is handed, naming the block by the value of its FIRST
// sample and its length: "wake-probe block first=<value> frames=<n>". It decodes nothing else and keeps no state between blocks, so
// what a test reads back is exactly which blocks reached the instance and in what order - the way to prove that a plugin woken by a
// window's signal is handed the very first block it should be (a plugin that missed it would name the SECOND), and that a dormant one is
// handed none (its line count does not move).
//
// It is not a test (the tests/ glob builds only test_*.cpp as tests) and it ships nowhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_abi.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

constexpr std::size_t kLineMax = 96;
constexpr std::size_t kQueueLines = 64;

struct Instance {
    // The lines made by process(), waiting for poll_text(): a ring of fixed lines, so process() allocates nothing.
    char lines[kQueueLines][kLineMax];
    std::size_t lens[kQueueLines];
    std::size_t head = 0;
    std::size_t count = 0;
};

void* decoderCreate(uint32_t) {
    return new (std::nothrow) Instance();
}

void decoderProcess(void* handle, const float* samples, size_t frames) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || samples == nullptr || frames == 0u) { return; }
    if (s->count == kQueueLines) { return; }  // (a test that does not poll loses the newest, never overruns)
    const std::size_t at = (s->head + s->count) % kQueueLines;
    const int n = std::snprintf(s->lines[at], kLineMax, "wake-probe block first=%.3f frames=%zu\n", static_cast<double>(samples[0]),
                                static_cast<std::size_t>(frames));
    s->lens[at] = n > 0 ? static_cast<std::size_t>(n) : 0u;
    ++s->count;
}

int32_t decoderPollText(void* handle, char* buf, size_t cap) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || s->count == 0u) { return 0; }
    const std::size_t len = s->lens[s->head];
    if (len > cap) { return 0; }
    std::memcpy(buf, s->lines[s->head], len);
    s->head = (s->head + 1u) % kQueueLines;
    --s->count;
    return static_cast<int32_t>(len);
}

void decoderDestroy(void* handle) {
    delete static_cast<Instance*>(handle);
}

const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)),
    48000u,
    &decoderCreate,
    &decoderProcess,
    &decoderPollText,
    &decoderDestroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Wake Probe",
    "1.0.0",
    "FoxSDR tests",
    "MIT",
    CASCADE_CAP_DECODER,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}

