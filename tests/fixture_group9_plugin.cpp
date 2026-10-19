// fixture_group9_plugin.cpp - a REAL plugin for the tests of window port group 9 (the fitted-modules
// window; docs/api-0.3-additions.md section 8.8).
//
// A module, built as a shared library beside the test binaries and copied by the test into its own
// scratch plugin directory (never into the directory every AppWindow-building test scans). It declares
// what the two earlier fixtures do not, which is a plugin that is NOT a decoder:
//
//   * CASCADE_CAP_AUDIO_PROCESSOR - an in-chain audio processor that leaves the audio as it is. The
//     runner writes it a Running status row, so it must read MATCHED (the runner would feed it) with
//     the receiver stopped and with no DECODERS entry to say so;
//   * CASCADE_CAP_HOST_CLIENT - two command keys added when the host attaches it (id 1 "RESET", id 2
//     "CALIBRATE"), so PLUGIN_COMMANDS has a module with more than one key.
//
// Its descriptor carries a version, an author and a licence of its own ("9.1.0", "FoxSDR tests (group
// nine)", "MIT"), so the per-file facts have text to read back.
//
// FOR THE TESTS ONLY: with FOXSDR_FIXTURE_G9_NO_CREATE set to a non-empty value in its environment, the
// processor's create() returns nothing, which the runner reports as "failed to start its audio
// processor" - the one idle sentence a module that is not a decoder can have with the receiver stopped.
//
// It is not a test (the tests/ glob builds only test_*.cpp as tests) and it ships nowhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_abi.h"

#include <cstdlib>
#include <new>

namespace {

struct Instance {
    uint32_t rate;
};

void* procCreate(uint32_t rateHz, uint32_t) {
    const char* refuse = std::getenv("FOXSDR_FIXTURE_G9_NO_CREATE");
    if (refuse != nullptr && refuse[0] != '\0') { return nullptr; }
    Instance* s = new (std::nothrow) Instance();
    if (s != nullptr) { s->rate = rateHz; }
    return s;
}

void procProcess(void*, float*, size_t) {}

void procDestroy(void* handle) { delete static_cast<Instance*>(handle); }

const CascadeAudioProcessorApi kProcessor = {
    static_cast<uint32_t>(sizeof(CascadeAudioProcessorApi)),
    0u,
    "Group Nine Passthrough",
    &procCreate,
    &procProcess,
    &procDestroy,
};

void attach(const CascadeHostApi* host) {
    if (host == nullptr) { return; }
    if (CASCADE_HOST_HAS(host, add_command)) {
        (void)host->add_command(host->ctx, 1u, "RESET");
        (void)host->add_command(host->ctx, 2u, "CALIBRATE");
    }
}

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &attach,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_AUDIO_PROCESSOR, static_cast<uint32_t>(sizeof(CascadeAudioProcessorApi)), &kProcessor},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Group Nine Fixture",
    "9.1.0",
    "FoxSDR tests (group nine)",
    "MIT",
    CASCADE_CAP_AUDIO_PROCESSOR | CASCADE_CAP_HOST_CLIENT,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
