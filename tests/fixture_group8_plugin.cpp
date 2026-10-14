// fixture_group8_plugin.cpp - a REAL plugin for the tests of window port group 8 (the decoders,
// the presets, the marks on the spectrum, the plugin-mute cluster and the tune grants;
// docs/api-0.3-additions.md section 8.7).
//
// A module, built as a shared library beside the test binaries and copied by the test into its own
// scratch plugin directory (never into the directory every AppWindow-building test scans). It
// declares what tests/fixture_stage1_plugin.cpp does not:
//
//   * CASCADE_CAP_IQ_DECODER - a decoder fed raw I/Q, which the "mute audio while running" setting
//     DEFAULTS to ON for (the stage-1 fixture is an audio decoder, which defaults to off), so the
//     two fixtures together show both defaults;
//   * CASCADE_CAP_PRESET - three entries: raw index 0, a DEVICE-CENTRE preset; raw index 1, whose
//     frequency is not a frequency (the host drops it, so the raw indexes seen by an interface skip
//     it); raw index 2, a channel preset with no label of its own and a bandwidth;
//   * CASCADE_CAP_HOST_CLIENT - two marks on the spectrum set when the host attaches it: a point
//     with the host's own colour, and a dashed, spectrum-only span with a colour of its own.
//
// FOR THE BOUNDED VISUAL CHECK ONLY: with FOXSDR_FIXTURE_G8_LINES=<n> in its environment the decoder
// answers its first poll with n short lines at once ("G8 decoded line 001" ...), once a process, so the
// Decoder output window has text to draw that is the same in every run. Without it, it says nothing.
//
// It is not a test (the tests/ glob builds only test_*.cpp as tests) and it ships nowhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

struct Instance {
    double rate;
};

void* iqCreate(double rateHz, double) {
    Instance* s = new (std::nothrow) Instance();
    if (s != nullptr) { s->rate = rateHz; }
    return s;
}

void iqProcess(void*, const float*, size_t) {}

int32_t iqPollText(void*, char* buf, size_t cap) {
    static int remaining = -1;  // lines still to say; read from the environment the first time
    static int said = 0;
    if (remaining < 0) {
        const char* v = std::getenv("FOXSDR_FIXTURE_G8_LINES");
        remaining = v != nullptr ? std::atoi(v) : 0;
    }
    if (remaining <= 0 || buf == nullptr) { return 0; }
    size_t n = 0;
    while (remaining > 0 && n + 24u <= cap) {
        ++said;
        const int wrote = std::snprintf(buf + n, cap - n, "G8 decoded line %03d\n", said);
        if (wrote <= 0) { break; }
        n += static_cast<size_t>(wrote);
        --remaining;
    }
    return static_cast<int32_t>(n);
}

void iqDestroy(void* handle) { delete static_cast<Instance*>(handle); }

const CascadeIqDecoderApi kIq = {
    static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)),
    0.0,  // any rate
    0.0,
    &iqCreate,
    &iqProcess,
    nullptr,  // no retune
    &iqPollText,
    &iqDestroy,
};

uint32_t presetCount() { return 3u; }

int32_t presetGet(uint32_t index, CascadePreset* out) {
    if (out == nullptr || index >= 3u) { return 0; }
    const uint32_t size = out->structSize;
    std::memset(out, 0, sizeof(CascadePreset));
    out->structSize = size;
    if (index == 0u) {
        std::snprintf(out->label, sizeof(out->label), "%s", "G8 centre");
        out->frequencyHz = 1090.0e6;
        out->demodMode = CASCADE_DEMOD_UNCHANGED;
        out->flags = CASCADE_PRESET_DEVICE_CENTRE;
    } else if (index == 1u) {
        std::snprintf(out->label, sizeof(out->label), "%s", "G8 not a frequency");
        out->frequencyHz = -5.0;  // the host drops it
    } else {
        // No label: the plugin's own name stands in for it.
        out->frequencyHz = 162.4e6;
        out->demodMode = CASCADE_DEMOD_NFM;
        out->bandwidthHz = 25000.0;
    }
    return 1;
}

const CascadePresetApi kPresets = {
    static_cast<uint32_t>(sizeof(CascadePresetApi)),
    &presetCount,
    &presetGet,
};

void attach(const CascadeHostApi* host) {
    if (host == nullptr || !CASCADE_HOST_HAS(host, set_marker)) { return; }
    CascadeMarker point{};
    point.structSize = static_cast<uint32_t>(sizeof(point));
    point.id = 6u;
    point.kind = CASCADE_MARKER_POINT;
    point.freqHz = 100.1e6;
    std::snprintf(point.label, sizeof(point.label), "%s", "G8 mark");
    (void)host->set_marker(host->ctx, &point);
    CascadeMarker span{};
    span.structSize = static_cast<uint32_t>(sizeof(span));
    span.id = 5u;
    span.kind = CASCADE_MARKER_SPAN;
    span.flags = CASCADE_MARKER_FLAG_DASHED | CASCADE_MARKER_FLAG_SPECTRUM_ONLY;
    span.freqHz = 162.4e6;
    span.widthHz = 25000.0;
    span.colourRgba = 0x11223344u;
    std::snprintf(span.label, sizeof(span.label), "%s", "G8 span");
    (void)host->set_marker(host->ctx, &span);
}

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &attach,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_IQ_DECODER, static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)), &kIq},
    {CASCADE_CAP_PRESET, static_cast<uint32_t>(sizeof(CascadePresetApi)), &kPresets},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Group Eight Fixture",
    "1.0.0",
    "FoxSDR tests",
    "MIT",
    CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PRESET | CASCADE_CAP_HOST_CLIENT,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
