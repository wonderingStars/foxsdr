// fixture_group13_plugin.cpp - a REAL picture decoder for the tests of the patch page's pictures (window port group 13,
// part c; API 0.3 section 8.12.7): the one module in the tests that produces a picture the way a weather-satellite or
// SSTV decoder does, through the plugin ABI's CASCADE_CAP_IMAGE_DECODER, so a patch that runs it on the built-in signal
// generator hands the engine a real picture with no hardware, no network and no file.
//
// It declares one audio-input picture decoder. It takes no interest in the audio beyond COUNTING it: each time the host
// polls and more samples have arrived since the last picture, it offers the next one. The pictures are chosen so that
// every property a reader can check is a function of the picture's sequence number N (1, 2, 3, ...):
//
//   * the format alternates: an odd N is RGB24, 8 x 6 (144 bytes); an even N is GRAY8, 5 x 3 (15 bytes - a width that is
//     not a multiple of four, which is what shears a texture uploaded with GL's default row alignment);
//   * byte i of picture N is (N * 13 + i * 7) & 0xFF, so a picture that is half one N and half the next - the tear a
//     reader that is not careful would show - is a picture whose bytes do not follow that formula;
//   * it is `complete` when N is a multiple of 3, and its sequence is N.
//
// A second library built from this file under another name (-DFIXTURE_NAME) is not needed; the descriptor name is fixed.
//
// It is not a test (the tests/ glob builds only test_*.cpp as tests) and it ships nowhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_abi.h"

#include <cstdint>
#include <cstring>
#include <new>

namespace {

struct Instance {
    uint64_t frames = 0;     // samples consumed
    uint64_t offered = 0;    // the frames count at the last picture offered
    uint64_t sequence = 0;   // the last picture's number
    uint8_t pixels[8 * 6 * 3] = {};
};

void* create(double, double) { return new (std::nothrow) Instance(); }

void process(void* handle, const float*, size_t frames) {
    if (handle != nullptr) { static_cast<Instance*>(handle)->frames += frames; }
}

int32_t pollImage(void* handle, CascadeImage* out) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || out == nullptr || s->frames == s->offered) { return 0; }
    s->offered = s->frames;
    const uint64_t n = ++s->sequence;
    const bool rgb = (n % 2u) == 1u;
    const uint32_t w = rgb ? 8u : 5u;
    const uint32_t h = rgb ? 6u : 3u;
    const uint32_t bpp = rgb ? 3u : 1u;
    for (uint32_t i = 0; i < w * h * bpp; ++i) { s->pixels[i] = static_cast<uint8_t>((n * 13u + i * 7u) & 0xFFu); }
    out->width = w;
    out->height = h;
    out->format = rgb ? CASCADE_IMAGE_RGB24 : CASCADE_IMAGE_GRAY8;
    out->stride = w * bpp;
    out->complete = (n % 3u) == 0u ? 1 : 0;
    out->sequence = n;
    out->pixels = s->pixels;
    return 1;
}

void releaseImage(void*, const CascadeImage*) {}

int32_t pollText(void*, char*, size_t) { return 0; }

void destroy(void* handle) { delete static_cast<Instance*>(handle); }

const CascadeImageDecoderApi kImage = {
    static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
    CASCADE_INPUT_AUDIO,
    0.0,
    0.0,
    &create,
    &process,
    nullptr,
    &pollImage,
    &releaseImage,
    &pollText,
    &destroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)), &kImage},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Group Thirteen Fixture",
    "1.0.0",
    "FoxSDR tests",
    "MIT",
    CASCADE_CAP_IMAGE_DECODER,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
