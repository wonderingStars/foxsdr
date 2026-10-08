// A REAL plugin module whose output is a PICTURE, and a picture that never arrives - for
// tests/test_decoder_window_app.cpp, which has to see the plugin's image window open INSIDE the
// main window (and not past its right edge, where it used to open with only its 19 px margin on
// the screen). A decoded-picture window is drawn for an image decoder the receiver is running,
// whether or not a picture has come, and says it is waiting: which is all this module gives it.
//
// It declares CASCADE_CAP_IMAGE_DECODER and nothing else, takes demodulated audio at any rate,
// decodes nothing and never offers an image. Built by tests/CMakeLists.txt as a MODULE and
// including ONLY plugin_abi.h, as every third-party plugin must.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <new>

#include "plugin_abi.h"

namespace {

struct Instance {
    int unused = 0;
};

void* create(double, double) { return new (std::nothrow) Instance(); }

void process(void*, const float*, size_t) {}

int32_t pollImage(void*, CascadeImage*) { return 0; }  // none pending

void releaseImage(void*, const CascadeImage*) {}

int32_t pollText(void*, char*, size_t) { return 0; }

void destroy(void* handle) { delete static_cast<Instance*>(handle); }

const CascadeImageDecoderApi kImageDecoder = {
    static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
    CASCADE_INPUT_AUDIO,
    0.0,  // requiredRateHz: any rate
    0.0,  // preferredRateHz
    &create,
    &process,
    nullptr,  // retune: this decoder does not care
    &pollImage,
    &releaseImage,
    &pollText,
    &destroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
     &kImageDecoder},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Image Probe",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_IMAGE_DECODER,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
