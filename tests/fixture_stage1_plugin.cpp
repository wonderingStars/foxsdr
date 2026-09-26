// fixture_stage1_plugin.cpp - the smallest REAL plugin that lets
// tests/test_apply_command.cpp apply the plugin ops to a loaded module rather
// than to an empty list (engine extraction stage 1).
//
// A module, built as a shared library beside the test binaries and copied by
// the test into its own scratch plugin directory (never into the directory
// every AppWindow-building test scans). It declares:
//
//   * CASCADE_CAP_DECODER - an audio decoder that consumes and says nothing,
//     so the host counts it as a decoder (start, stop, mute, stop-all);
//   * CASCADE_CAP_PRESET - two presets at frequencies the signal generator
//     can be tuned to, so PLUGIN_PRESET and the user's own presets have
//     something to press;
//   * CASCADE_CAP_HOST_CLIENT - it adds one command key (id 7, "PING") when
//     the host attaches it, so PLUGIN_COMMAND has a key to press.
//
// It is not a test (the tests/ glob builds only test_*.cpp as tests) and it
// ships nowhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_abi.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace {

struct Instance {
    uint32_t rate;
};

void* decoderCreate(uint32_t rateHz) {
    Instance* s = new (std::nothrow) Instance();
    if (s != nullptr) { s->rate = rateHz; }
    return s;
}

void decoderProcess(void*, const float*, size_t) {}

int32_t decoderPollText(void*, char*, size_t) { return 0; }

void decoderDestroy(void* handle) { delete static_cast<Instance*>(handle); }

const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)),
    48000u,
    &decoderCreate,
    &decoderProcess,
    &decoderPollText,
    &decoderDestroy,
};

// Two presets inside the signal generator's reach: 100.1 MHz WFM at 150 kHz,
// and 145.5 MHz NFM at 12.5 kHz.
uint32_t presetCount() { return 2u; }

int32_t presetGet(uint32_t index, CascadePreset* out) {
    if (out == nullptr || index >= 2u) { return 0; }
    const uint32_t size = out->structSize;
    std::memset(out, 0, sizeof(CascadePreset));
    out->structSize = size;
    if (index == 0u) {
        std::snprintf(out->label, sizeof(out->label), "%s", "Fixture WFM");
        out->frequencyHz = 100.1e6;
        out->demodMode = CASCADE_DEMOD_WFM;
        out->bandwidthHz = 150000.0;
    } else {
        std::snprintf(out->label, sizeof(out->label), "%s", "Fixture NFM");
        out->frequencyHz = 145.5e6;
        out->demodMode = CASCADE_DEMOD_NFM;
        out->bandwidthHz = 12500.0;
    }
    return 1;
}

const CascadePresetApi kPresets = {
    static_cast<uint32_t>(sizeof(CascadePresetApi)),
    &presetCount,
    &presetGet,
};

void attach(const CascadeHostApi* host) {
    if (host == nullptr) { return; }
    if (CASCADE_HOST_HAS(host, add_command)) { (void)host->add_command(host->ctx, 7u, "PING"); }
}

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &attach,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
    {CASCADE_CAP_PRESET, static_cast<uint32_t>(sizeof(CascadePresetApi)), &kPresets},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
};

// The descriptor name. The host runs one plugin per NAME (resolveDuplicate-
// Plugins), so test_snapshot_app builds this file three more times under
// three names to have three decoders loaded at once.
#ifndef FIXTURE_NAME
#define FIXTURE_NAME "Stage One Fixture"
#endif

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    FIXTURE_NAME,
    "1.0.0",
    "FoxSDR tests",
    "MIT",
    CASCADE_CAP_DECODER | CASCADE_CAP_PRESET | CASCADE_CAP_HOST_CLIENT,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
