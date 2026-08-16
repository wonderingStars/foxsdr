// Reference cascade plugin: a minimal CASCADE_CAP_IMAGE_DECODER - the image
// capability added in plugin ABI 3 - that turns demodulated audio into a
// scrolling grey-scale "level ribbon".
//
// It is deliberately useless as a decoder and deliberately complete as an
// example of the image contract, which is the part of the ABI with the most
// ways to be subtly wrong:
//
//   - the pixels are OWNED BY THE PLUGIN and only BORROWED by the host, so
//     there are two buffers here: one being built from incoming audio and one
//     published. Nothing rewrites the published buffer while the host holds it;
//   - `stride` is deliberately LARGER than width, which is legal and is what
//     padding exists for. A host that flat-memcpy'd width*height bytes would
//     shear this image visibly, so if it renders straight, the row-by-row copy
//     is genuinely being done;
//   - `complete` is 0 while the frame is still filling. A slow-scan picture
//     takes tens of seconds and showing it build is the difference between
//     "working" and "frozen", so partial frames are normal, not an error;
//   - `sequence` increments per frame so the host can tell a new picture from
//     an update to the one it is already displaying.
//
// WHAT A REAL IMAGE DECODER LOOKS LIKE. SSTV is the mode this capability was
// added for: a 1500-2300 Hz subcarrier whose instantaneous frequency is the
// luminance of one pixel, with a 1200 Hz sync pulse starting each line and a
// VIS header naming the mode. Everything it needs is in the same process()
// signature used below; the differences are the size of the state and the fact
// that the picture means something.
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
//      /I "..\..\src\core" example_image_plugin.cpp /Fe:example_image_decoder.dll
//
// Then copy example_image_decoder.dll into <cascade install dir>\plugins\ .
//
// Linux / macOS:
//
//   g++ -shared -fPIC -O2 -Wall -Wextra -std=c++17 -DCASCADE_PLUGIN_BUILD \
//       -I ../../src/core example_image_plugin.cpp -o example_image_decoder.so
// ===========================================================================

#include "plugin_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

const uint32_t kClaimedAbi = CASCADE_PLUGIN_ABI_VERSION;

// Small enough that a bounded test run completes several frames, and not a
// multiple of four, so a host that assumes four-byte row alignment anywhere is
// caught.
const uint32_t kWidth = 126u;
const uint32_t kHeight = 64u;

// PADDING ON PURPOSE. A real decoder pads for alignment; this one pads so the
// host's row-by-row copy is exercised by a real module rather than only by a
// fake table in the host's own tests.
const uint32_t kStride = 132u;

// Audio samples per output row. At 48 kHz a full frame is 126*64 samples,
// about 0.17 s - fast enough to see the picture build in a short run.
const uint32_t kSamplesPerPixel = 1u;

const double kPreferredRateHz = 48000.0;

struct Instance {
    double rate;

    // TWO buffers, which is the whole point of the borrow contract: `back` is
    // written by process() as audio arrives, `front` is what the host may be
    // looking at. They are never the same memory.
    unsigned char back[kStride * kHeight];
    unsigned char front[kStride * kHeight];

    uint32_t col;       // next pixel column to write in `back`
    uint32_t row;       // next row
    uint32_t acc;       // samples accumulated into the current pixel
    double peak;        // running peak |sample| for the current pixel

    bool dirty;         // `back` has changed since the last publish
    bool borrowed;      // the host is holding `front` right now
    bool frameDone;     // the last published frame was a complete picture
    uint64_t sequence;

    char pending[192];
    size_t pendingLen;
    size_t pendingRead;
};

void startFrame(Instance* s) {
    std::memset(s->back, 0, sizeof(s->back));
    s->col = 0u;
    s->row = 0u;
    s->acc = 0u;
    s->peak = 0.0;
}

void* imgCreate(double rateHz, double centerHz) {
    // centerHz is 0 for an audio-input decoder by the ABI's own rule: the audio
    // has already been tuned and demodulated, so there is no RF frequency this
    // decoder could correctly use.
    (void)centerHz;
    Instance* s = new (std::nothrow) Instance();
    if (s == nullptr) {
        return nullptr;
    }
    s->rate = (rateHz > 0.0) ? rateHz : kPreferredRateHz;
    s->dirty = false;
    s->borrowed = false;
    s->frameDone = false;
    s->sequence = 0u;
    s->pendingLen = 0u;
    s->pendingRead = 0u;
    std::memset(s->front, 0, sizeof(s->front));
    startFrame(s);
    return s;
}

void emit(Instance* s, const char* what) {
    // Dropped rather than queued when the host has not drained the last line:
    // back-pressure by discarding is the correct behaviour for a real-time
    // producer, and this runs on the host's DSP thread.
    if (s->pendingRead != s->pendingLen) {
        return;
    }
    const int n = std::snprintf(s->pending, sizeof(s->pending),
                                "example-image: %s frame %llu, %ux%u @ %.6g Hz\n", what,
                                static_cast<unsigned long long>(s->sequence), kWidth, kHeight,
                                s->rate);
    s->pendingLen = (n > 0) ? static_cast<size_t>(n) : 0u;
    if (s->pendingLen >= sizeof(s->pending)) {
        s->pendingLen = sizeof(s->pending) - 1u;
    }
    s->pendingRead = 0u;
}

void imgProcess(void* handle, const float* samples, size_t frames) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || (samples == nullptr && frames != 0u)) {
        return;  // a NULL handle can only be a host bug, but faulting in
                 // someone else's process is never the answer
    }
    for (size_t n = 0; n < frames; ++n) {
        const double a = std::fabs(static_cast<double>(samples[n]));
        if (a > s->peak) {
            s->peak = a;
        }
        if (++s->acc < kSamplesPerPixel) {
            continue;
        }

        // One pixel: peak level mapped to 0..255 with a mild curve, so a quiet
        // signal is visible rather than a black picture.
        double v = std::sqrt(s->peak) * 255.0;
        if (v > 255.0) {
            v = 255.0;
        }
        // NOTE the stride, not the width: the row start is kStride bytes apart
        // and the bytes past kWidth in each row are padding the host must skip.
        s->back[s->row * kStride + s->col] = static_cast<unsigned char>(v);
        s->acc = 0u;
        s->peak = 0.0;
        s->dirty = true;

        if (++s->col < kWidth) {
            continue;
        }
        s->col = 0u;
        if (++s->row < kHeight) {
            continue;
        }
        // A whole picture. It is published on the next poll like any other
        // change; what makes it different is `complete`.
        s->frameDone = true;
        s->row = 0u;
    }
}

int32_t imgPollImage(void* handle, CascadeImage* out) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || out == nullptr) {
        return 0;
    }
    // The host fills structSize before the call; a plugin that finds a size it
    // does not recognise must not write the struct.
    if (out->structSize != static_cast<uint32_t>(sizeof(CascadeImage))) {
        return -1;
    }
    if (s->borrowed || !s->dirty) {
        return 0;  // must be cheap: the host polls this every rendered frame
    }

    // Publish: copy the work buffer into the one the host is about to borrow.
    // This is the only moment the two buffers touch, and it cannot happen while
    // the host holds `front`, which is what the borrow contract requires.
    std::memcpy(s->front, s->back, sizeof(s->front));
    s->dirty = false;
    const bool complete = s->frameDone;
    if (complete) {
        s->frameDone = false;
        ++s->sequence;
        emit(s, "completed");
        startFrame(s);
    }

    out->width = kWidth;
    out->height = kHeight;
    out->format = CASCADE_IMAGE_GRAY8;
    out->stride = kStride;
    out->complete = complete ? 1 : 0;
    out->sequence = s->sequence;
    out->pixels = s->front;
    s->borrowed = true;
    return 1;
}

void imgReleaseImage(void* handle, const CascadeImage* img) {
    Instance* s = static_cast<Instance*>(handle);
    (void)img;
    if (s == nullptr) {
        return;
    }
    s->borrowed = false;
}

int32_t imgPollText(void* handle, char* buf, size_t cap) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || buf == nullptr || cap == 0u) {
        return 0;
    }
    size_t avail = s->pendingLen - s->pendingRead;
    if (avail == 0u) {
        return 0;
    }
    if (avail > cap) {
        avail = cap;  // the host collects the remainder next poll
    }
    std::memcpy(buf, s->pending + s->pendingRead, avail);
    s->pendingRead += avail;
    return static_cast<int32_t>(avail);
}

void imgDestroy(void* handle) { delete static_cast<Instance*>(handle); }

// Static, immutable, and outliving every call - as the ABI requires.
const CascadeImageDecoderApi kImageDecoder = {
    static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
    CASCADE_INPUT_AUDIO,  // demodulated audio, like SSTV; LRPT would say IQ
    0.0,                  // requiredRateHz: 0 = whatever the host delivers
    kPreferredRateHz,     // preferredRateHz: a hint, nothing more
    &imgCreate,
    &imgProcess,
    nullptr,              // retune: an audio-input decoder has no use for it
    &imgPollImage,
    &imgReleaseImage,
    &imgPollText,
    &imgDestroy,
};

// ABI 3 carries capabilities in a counted array rather than as trailing
// members of the descriptor, which is what keeps the descriptor a fixed size
// as capabilities are added.
const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
     &kImageDecoder},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    kClaimedAbi,
    "Example Image",
    "1.0.0",
    "cascade project",
    "MIT",  // declared honestly; the host displays this verbatim
    CASCADE_CAP_IMAGE_DECODER,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

// The one exported symbol. Returning NULL is the polite refusal when the host
// speaks a different ABI; the host also checks kDesc.abiVersion itself.
extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) {
        return nullptr;
    }
    return &kDesc;
}
