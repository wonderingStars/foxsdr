// A DAB-shaped plugin: I/Q decoder, its own preset, and decoded PCM output.
// The real AppWindow must let this programme survive its default carrier mute.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "plugin_abi.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

namespace {
std::atomic<bool> g_play{true};
struct Instance { std::uint64_t frame = 0; };
void* create(double, double) { return new (std::nothrow) Instance(); }
void process(void*, const float*, size_t) {}
int32_t text(void*, char*, size_t) { return 0; }
void destroy(void* h) { delete static_cast<Instance*>(h); }
int32_t active(void*) { return g_play.load() ? 1 : 0; }
int32_t pull(void* h, float* pcm, size_t frames) {
    auto* s = static_cast<Instance*>(h);
    for (size_t i = 0; i < frames; ++i) {
        pcm[i] = 0.5f * static_cast<float>(std::sin(6.2831853071795864769 * 1000.0 *
                                                 static_cast<double>(s->frame++) / 48000.0));
    }
    return static_cast<int32_t>(frames);
}
uint32_t presetCount() { return 1u; }
int32_t presetGet(uint32_t i, CascadePreset* p) {
    if (i != 0u || p == nullptr) { return 0; }
    std::memset(p, 0, sizeof(*p));
    p->structSize = static_cast<uint32_t>(sizeof(*p));
    std::strcpy(p->label, "Programme 12B");
    p->frequencyHz = 225648000.0;
    p->demodMode = CASCADE_DEMOD_RAW;
    p->sampleRateHz = 1000000.0;
    p->flags = CASCADE_PRESET_DEVICE_CENTRE;
    return 1;
}
const CascadeIqDecoderApi kIq = {
    sizeof(CascadeIqDecoderApi), 0.0, 0.0, &create, &process, nullptr, &text, &destroy};
const CascadeAudioOutApi kAudio = {sizeof(CascadeAudioOutApi), 48000u, 1u, &pull, &active};
const CascadePresetApi kPreset = {sizeof(CascadePresetApi), &presetCount, &presetGet};
const CascadeCapabilityEntry kCaps[] = {
    {CASCADE_CAP_IQ_DECODER, sizeof(kIq), &kIq},
    {CASCADE_CAP_AUDIO_OUT, sizeof(kAudio), &kAudio},
    {CASCADE_CAP_PRESET, sizeof(kPreset), &kPreset},
};
const CascadePluginDesc kDesc = {
    sizeof(CascadePluginDesc), CASCADE_PLUGIN_ABI_VERSION, "Programme Audio Fixture", "1.0.0",
    "FoxSDR tests", "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_IQ_DECODER | CASCADE_CAP_AUDIO_OUT | CASCADE_CAP_PRESET, 3u, kCaps};
}
extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t abi) {
    return abi == CASCADE_PLUGIN_ABI_VERSION ? &kDesc : nullptr;
}
extern "C" CASCADE_PLUGIN_EXPORT void programme_fixture_play(int on) { g_play.store(on != 0); }
