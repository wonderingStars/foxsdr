// PluginRunner: the class that actually drives decoder plugins with audio.
//
// No DLL is loaded here. LoadedPlugin carries plain pointers to API tables, so
// a fake decoder is a static table plus a counter - which lets every case below
// assert on what the runner DID to the decoder (how many samples, how many
// instances, in what order it destroyed them) rather than on side effects
// observed through a real plugin.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/plugin_runner.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::DecodedLine;
using cascade::core::DecoderIdleReason;
using cascade::core::DecoderStatus;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;

// --- A fake decoder whose behaviour each test can steer -------------------

struct FakeState {
    int created = 0;
    int destroyed = 0;
    uint32_t lastRate = 0;
    std::size_t samples = 0;
    std::string queued;    // handed out by poll_text, a chunk at a time
    std::size_t chunk = 0; // 0 = all at once
    bool failCreate = false;
    bool failPoll = false; // poll_text returns < 0
};

FakeState g_fake;

void* fakeCreate(uint32_t rateHz) {
    if (g_fake.failCreate) { return nullptr; }
    ++g_fake.created;
    g_fake.lastRate = rateHz;
    return &g_fake;  // any non-null handle
}

void fakeProcess(void*, const float*, size_t count) { g_fake.samples += count; }

int32_t fakePoll(void*, char* buf, size_t cap) {
    if (g_fake.failPoll) { return -1; }
    if (g_fake.queued.empty()) { return 0; }
    std::size_t n = g_fake.queued.size();
    if (g_fake.chunk != 0 && g_fake.chunk < n) { n = g_fake.chunk; }
    if (n > cap) { n = cap; }
    std::memcpy(buf, g_fake.queued.data(), n);
    g_fake.queued.erase(0, n);
    return static_cast<int32_t>(n);
}

void fakeDestroy(void*) { ++g_fake.destroyed; }

CascadeDecoderApi makeApi(uint32_t rateHz) {
    CascadeDecoderApi a{};
    a.structSize = static_cast<uint32_t>(sizeof(CascadeDecoderApi));
    a.requiredRateHz = rateHz;
    a.create = &fakeCreate;
    a.process = &fakeProcess;
    a.poll_text = &fakePoll;
    a.destroy = &fakeDestroy;
    return a;
}

LoadedPlugin makePlugin(const char* name, const CascadeDecoderApi* api) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.decoder = api;
    return p;
}

void resetFake() { g_fake = FakeState{}; }

// The raw device band the I/Q cases are fed, and where the receiver is tuned.
// Chosen to look like a real ADS-B setup so a rate mismatch below reads as the
// mistake it would be in the product.
constexpr double kIqRate = 2400000.0;
constexpr double kCentre = 1090000000.0;

// --- A fake I/Q decoder, same trick as the audio one ----------------------

struct FakeIq {
    int created = 0;
    int destroyed = 0;
    double lastRate = 0.0;
    double lastCentre = 0.0;
    int retunes = 0;
    std::size_t frames = 0;
    std::string queued;
    bool failCreate = false;
};

FakeIq g_iq;

void* iqCreate(double rateHz, double centreHz) {
    if (g_iq.failCreate) { return nullptr; }
    ++g_iq.created;
    g_iq.lastRate = rateHz;
    g_iq.lastCentre = centreHz;
    return &g_iq;
}

void iqProcess(void*, const float*, size_t frames) { g_iq.frames += frames; }
void iqRetune(void*, double centreHz) { ++g_iq.retunes; g_iq.lastCentre = centreHz; }

int32_t iqPoll(void*, char* buf, size_t cap) {
    if (g_iq.queued.empty()) { return 0; }
    std::size_t n = g_iq.queued.size();
    if (n > cap) { n = cap; }
    std::memcpy(buf, g_iq.queued.data(), n);
    g_iq.queued.erase(0, n);
    return static_cast<int32_t>(n);
}

void iqDestroy(void*) { ++g_iq.destroyed; }

CascadeIqDecoderApi makeIqApi(double rateHz, bool withRetune) {
    CascadeIqDecoderApi a{};
    a.structSize = static_cast<uint32_t>(sizeof(CascadeIqDecoderApi));
    a.requiredRateHz = rateHz;
    a.preferredRateHz = 0.0;
    a.create = &iqCreate;
    a.process = &iqProcess;
    a.retune = withRetune ? &iqRetune : nullptr;
    a.poll_text = &iqPoll;
    a.destroy = &iqDestroy;
    return a;
}

LoadedPlugin makeIqPlugin(const char* name, const CascadeIqDecoderApi* api) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.iqDecoder = api;
    return p;
}

void resetIq() { g_iq = FakeIq{}; }

// --- A fake IMAGE decoder -------------------------------------------------
//
// The one that matters most here: until this was wired, an image decoder was
// created and polled but never handed a sample, so `frames` below could only
// ever be 0 in the product.

struct FakeImg {
    int created = 0;
    int destroyed = 0;
    double lastRate = 0.0;
    double lastCentre = -1.0;   // -1 = never called, to tell it from a real 0
    int retunes = 0;
    std::size_t frames = 0;
    int polls = 0;
    int releases = 0;
    bool failCreate = false;
    // What poll_image hands out. offer = 0 -> "nothing pending".
    int offer = 0;
    uint32_t w = 0, h = 0, fmt = CASCADE_IMAGE_GRAY8, stride = 0;
    uint64_t seq = 0;
    std::vector<uint8_t> pixels;
    std::string queued;  // status text
};

FakeImg g_img;

void* imgCreate(double rateHz, double centreHz) {
    if (g_img.failCreate) { return nullptr; }
    ++g_img.created;
    g_img.lastRate = rateHz;
    g_img.lastCentre = centreHz;
    return &g_img;
}

void imgProcess(void*, const float*, size_t frames) { g_img.frames += frames; }
void imgRetune(void*, double centreHz) { ++g_img.retunes; g_img.lastCentre = centreHz; }

int32_t imgPollImage(void*, CascadeImage* out) {
    ++g_img.polls;
    if (g_img.offer == 0) { return 0; }
    // The host is required to fill structSize before the call; a plugin that
    // could not see it would have no way to tell what it is filling.
    if (out->structSize != static_cast<uint32_t>(sizeof(CascadeImage))) { return -1; }
    out->width = g_img.w;
    out->height = g_img.h;
    out->format = g_img.fmt;
    out->stride = g_img.stride;
    out->complete = 1;
    out->sequence = g_img.seq;
    out->pixels = g_img.pixels.data();
    return 1;
}

void imgRelease(void*, const CascadeImage*) { ++g_img.releases; }

int32_t imgPollText(void*, char* buf, size_t cap) {
    if (g_img.queued.empty()) { return 0; }
    std::size_t n = g_img.queued.size();
    if (n > cap) { n = cap; }
    std::memcpy(buf, g_img.queued.data(), n);
    g_img.queued.erase(0, n);
    return static_cast<int32_t>(n);
}

void imgDestroy(void*) { ++g_img.destroyed; }

CascadeImageDecoderApi makeImgApi(uint32_t inputKind, double rateHz, bool withRetune) {
    CascadeImageDecoderApi a{};
    a.structSize = static_cast<uint32_t>(sizeof(CascadeImageDecoderApi));
    a.inputKind = inputKind;
    a.requiredRateHz = rateHz;
    a.preferredRateHz = 0.0;
    a.create = &imgCreate;
    a.process = &imgProcess;
    a.retune = withRetune ? &imgRetune : nullptr;
    a.poll_image = &imgPollImage;
    a.release_image = &imgRelease;
    a.poll_text = &imgPollText;
    a.destroy = &imgDestroy;
    return a;
}

LoadedPlugin makeImgPlugin(const char* name, const CascadeImageDecoderApi* api) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.imageDecoder = api;
    return p;
}

void resetImg() { g_img = FakeImg{}; }

// Bounds-safe element access for the assertions below. CHECK records a failure
// and CARRIES ON, so a plain v[0] guarded only by a preceding size CHECK is an
// out-of-bounds read in exactly the run that has something to report - the test
// would crash instead of naming the broken expectation. Returning a
// default-constructed element instead makes the follow-up checks fail honestly
// rather than being skipped.
template <typename T>
const T& at(const std::vector<T>& v, std::size_t i) {
    static const T kNone{};
    return i < v.size() ? v[i] : kNone;
}

// Offers a 2x2 GRAY8 image whose rows are PADDED to `stride` bytes, so the
// host's row-by-row copy is exercised rather than a flat memcpy that would
// happen to work when stride == width.
void offerGray2x2(uint32_t stride, uint64_t seq) {
    g_img.offer = 1;
    g_img.w = 2;
    g_img.h = 2;
    g_img.fmt = CASCADE_IMAGE_GRAY8;
    g_img.stride = stride;
    g_img.seq = seq;
    g_img.pixels.assign(static_cast<std::size_t>(stride) * 2u, 0xEE);  // padding filler
    g_img.pixels[0] = 1;
    g_img.pixels[1] = 2;
    g_img.pixels[stride + 0] = 3;
    g_img.pixels[stride + 1] = 4;
}

}  // namespace

int main() {
    // --- Rate matching --------------------------------------------------
    {
        resetFake();
        const CascadeDecoderApi any = makeApi(0u);  // "any rate"
        std::vector<LoadedPlugin> ps{makePlugin("Any", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 1u);
        CHECK(g_fake.created == 1);
        // A rate-0 decoder is told the rate it will actually receive, not 0 -
        // otherwise it cannot size a filter.
        CHECK(g_fake.lastRate == 48000u);
    }
    {
        resetFake();
        const CascadeDecoderApi exact = makeApi(48000u);
        std::vector<LoadedPlugin> ps{makePlugin("Exact", &exact)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 1u);
        CHECK(g_fake.lastRate == 48000u);
    }
    {
        // The case that must NOT silently do nothing: a fixed-rate decoder the
        // pipeline cannot currently feed. It is not created, and the reason is
        // reported in words a user can act on.
        resetFake();
        const CascadeDecoderApi wrong = makeApi(8000u);
        std::vector<LoadedPlugin> ps{makePlugin("Fixed8k", &wrong)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 0u);
        CHECK(g_fake.created == 0);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(st[0].reason == DecoderIdleReason::RateMismatch);
        CHECK(st[0].wantRateHz == 8000.0);
        CHECK(st[0].detail.find("8000") != std::string::npos);
        CHECK(st[0].detail.find("48000") != std::string::npos);
    }

    // --- Samples actually reach the decoder ------------------------------
    {
        resetFake();
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Any", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        std::vector<float> block(512, 0.25f);
        r.processAudio(block.data(), block.size());
        r.processAudio(block.data(), block.size());
        CHECK(g_fake.samples == 1024u);
        // Degenerate inputs are no-ops, not crashes.
        r.processAudio(nullptr, 10);
        r.processAudio(block.data(), 0);
        CHECK(g_fake.samples == 1024u);
    }

    // --- Line assembly ----------------------------------------------------
    {
        resetFake();
        g_fake.queued = "alpha\nbeta\n";
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Tag", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        const std::vector<DecodedLine> out = r.drainText();
        CHECK(out.size() == 2u);
        CHECK(out[0].text == "alpha");
        CHECK(out[1].text == "beta");
        // Tagged, so two decoders running together are distinguishable.
        CHECK(out[0].plugin == "Tag");
        // Draining twice does not repeat.
        CHECK(r.drainText().empty());
    }
    {
        // A line SPLIT ACROSS POLLS must arrive once and whole. The ABI
        // explicitly permits a partial line at the end of a poll, so this is
        // ordinary behaviour rather than an edge case, and emitting the
        // fragment would show the user a line that then changed.
        resetFake();
        g_fake.queued = "hello world\n";
        g_fake.chunk = 3;  // 3 bytes per poll
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Split", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        const std::vector<DecodedLine> out = r.drainText();
        CHECK(out.size() == 1u);
        CHECK(out[0].text == "hello world");
    }
    {
        // Text with no trailing newline stays buffered rather than being
        // emitted early...
        resetFake();
        g_fake.queued = "partial";
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Partial", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        CHECK(r.drainText().empty());
        // ...and completes when the newline arrives.
        g_fake.queued = " line\n";
        r.processAudio(block.data(), block.size());
        const std::vector<DecodedLine> out = r.drainText();
        CHECK(out.size() == 1u);
        CHECK(out[0].text == "partial line");
    }

    // --- Failure paths ----------------------------------------------------
    {
        resetFake();
        g_fake.failCreate = true;
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Broken", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 0u);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(st[0].reason == DecoderIdleReason::CreateFailed);
    }
    {
        // A decoder that declares no audio table is listed with a reason, not
        // dropped: "my plugin is installed and nothing happens" is the
        // question this answers.
        resetFake();
        LoadedPlugin iqOnly;
        iqOnly.loaded = true;
        iqOnly.name = "IqThing";
        iqOnly.decoder = nullptr;
        std::vector<LoadedPlugin> ps{iqOnly};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 0u);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(st[0].reason == DecoderIdleReason::NoAudioTable);
    }

    // --- Lifetime: every instance is destroyed exactly once ---------------
    {
        // The rule that matters most. A handle outliving its module is a crash
        // inside someone else's DLL, so rebuild() must destroy the old set
        // before creating a new one, and clear() must destroy the rest.
        resetFake();
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("A", &any), makePlugin("B", &any)};
        {
            PluginRunner r;
            r.rebuild(ps, 48000.0, kIqRate, kCentre);
            CHECK(g_fake.created == 2);
            CHECK(g_fake.destroyed == 0);
            r.rebuild(ps, 48000.0, kIqRate, kCentre);  // a rescan
            CHECK(g_fake.destroyed == 2);
            CHECK(g_fake.created == 4);
            r.clear();
            CHECK(g_fake.destroyed == 4);
            CHECK(r.activeCount() == 0u);
            r.clear();  // idempotent
            CHECK(g_fake.destroyed == 4);
        }
        // ...and the destructor does not double-destroy what clear() took.
        CHECK(g_fake.created == g_fake.destroyed);
    }
    {
        // Destruction via the destructor alone, with instances still live.
        resetFake();
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("A", &any)};
        {
            PluginRunner r;
            r.rebuild(ps, 48000.0, kIqRate, kCentre);
            CHECK(g_fake.created == 1);
        }
        CHECK(g_fake.destroyed == 1);
    }

    // --- I/Q decoders -----------------------------------------------------
    {
        // An "any rate" I/Q decoder is created with the RAW device rate and
        // the receiver's centre, not the channel rate: it works on the whole
        // band and must know where that band is.
        resetIq();
        const CascadeIqDecoderApi api = makeIqApi(0.0, true);
        std::vector<LoadedPlugin> ps{makeIqPlugin("ADS-B", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 1u);
        CHECK(g_iq.created == 1);
        CHECK(g_iq.lastRate == kIqRate);
        CHECK(g_iq.lastCentre == kCentre);
    }
    {
        // FRAMES, not floats. The ABI passes 2*frames floats interleaved, and
        // an off-by-two here would halve or double every decoder's idea of
        // time - which is the kind of bug that shows up as "it decodes at
        // 2.4 MS/s but not 1.2".
        resetIq();
        const CascadeIqDecoderApi api = makeIqApi(0.0, true);
        std::vector<LoadedPlugin> ps{makeIqPlugin("ADS-B", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        std::vector<float> iq(2 * 1000, 0.0f);  // 1000 complex samples
        r.processIq(iq.data(), 1000);
        CHECK(g_iq.frames == 1000u);
        r.processIq(nullptr, 10);
        r.processIq(iq.data(), 0);
        CHECK(g_iq.frames == 1000u);
    }
    {
        // Audio must not be delivered to an I/Q decoder, nor I/Q to an audio
        // one. They are different streams with different meanings, and
        // crossing them would feed a decoder noise it cannot recognise while
        // looking, from the outside, exactly like a decoder that is working.
        resetFake();
        resetIq();
        const CascadeDecoderApi aApi = makeApi(0u);
        const CascadeIqDecoderApi iApi = makeIqApi(0.0, true);
        std::vector<LoadedPlugin> ps{makePlugin("Aud", &aApi), makeIqPlugin("Iq", &iApi)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 2u);
        std::vector<float> buf(256, 0.0f);
        r.processAudio(buf.data(), 128);
        CHECK(g_fake.samples == 128u);
        CHECK(g_iq.frames == 0u);
        r.processIq(buf.data(), 128);
        CHECK(g_fake.samples == 128u);
        CHECK(g_iq.frames == 128u);
    }
    {
        // A fixed-rate I/Q decoder the device cannot supply idles with a
        // reason naming the I/Q stream, so it is not confused with the audio
        // rate mismatch above.
        resetIq();
        const CascadeIqDecoderApi api = makeIqApi(192000.0, true);  // AIS-like
        std::vector<LoadedPlugin> ps{makeIqPlugin("AIS", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 0u);
        CHECK(g_iq.created == 0);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(st[0].reason == DecoderIdleReason::RateMismatch);
        CHECK(st[0].stream == cascade::core::DecoderStream::Iq);
        CHECK(st[0].detail.find("I/Q") != std::string::npos);
    }
    {
        // retune reaches the decoder, is not sent when nothing moved, and is
        // safe when the plugin left the optional pointer NULL.
        resetIq();
        const CascadeIqDecoderApi withRt = makeIqApi(0.0, true);
        std::vector<LoadedPlugin> ps{makeIqPlugin("Rt", &withRt)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        r.retune(kCentre);  // unchanged
        CHECK(g_iq.retunes == 0);
        r.retune(1090500000.0);
        CHECK(g_iq.retunes == 1);
        CHECK(g_iq.lastCentre == 1090500000.0);
        r.retune(1090500000.0);  // still unchanged
        CHECK(g_iq.retunes == 1);
    }
    {
        resetIq();
        const CascadeIqDecoderApi noRt = makeIqApi(0.0, false);  // retune == NULL
        std::vector<LoadedPlugin> ps{makeIqPlugin("NoRt", &noRt)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        r.retune(1091000000.0);  // must not dereference the null pointer
        CHECK(g_iq.retunes == 0);
        CHECK(r.activeCount() == 1u);
    }
    {
        // I/Q text is tagged and line-split by the same path as audio.
        resetIq();
        g_iq.queued = "ADSB 406135 ident=EZY595R\n";
        const CascadeIqDecoderApi api = makeIqApi(0.0, true);
        std::vector<LoadedPlugin> ps{makeIqPlugin("ADS-B", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        std::vector<float> iq(64, 0.0f);
        r.processIq(iq.data(), 32);
        const std::vector<DecodedLine> out = r.drainText();
        CHECK(out.size() == 1u);
        CHECK(out[0].plugin == "ADS-B");
        CHECK(out[0].text == "ADSB 406135 ident=EZY595R");
    }
    {
        // I/Q instances are destroyed exactly once, like audio ones.
        resetIq();
        const CascadeIqDecoderApi api = makeIqApi(0.0, true);
        std::vector<LoadedPlugin> ps{makeIqPlugin("A", &api), makeIqPlugin("B", &api)};
        {
            PluginRunner r;
            r.rebuild(ps, 48000.0, kIqRate, kCentre);
            CHECK(g_iq.created == 2);
            r.rebuild(ps, 48000.0, kIqRate, kCentre);
            CHECK(g_iq.destroyed == 2);
        }
        CHECK(g_iq.created == g_iq.destroyed);
    }

    // --- IMAGE decoders: the wire that did not exist ----------------------
    {
        // THE BUG THIS FILE EXISTS TO CATCH. An image decoder was created and
        // polled for pictures but never handed a single sample, so it could not
        // produce anything at all. An audio-input one must receive the audio
        // stream, and be told the rate it will get.
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_AUDIO, 0.0, false);
        std::vector<LoadedPlugin> ps{makeImgPlugin("SSTV", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(g_img.created == 1);
        CHECK(g_img.lastRate == 48000.0);
        // The ABI says centerHz is 0 for an audio-input decoder: the audio has
        // already been tuned and demodulated, so an RF frequency would be a
        // number it could only misuse.
        CHECK(g_img.lastCentre == 0.0);
        CHECK(r.activeCount() == 1u);
        std::vector<float> block(512, 0.25f);
        r.processAudio(block.data(), block.size());
        CHECK(g_img.frames == 512u);
        // ...and it is NOT fed the raw I/Q as well.
        r.processIq(block.data(), 128);
        CHECK(g_img.frames == 512u);
    }
    {
        // An I/Q-input image decoder (LRPT) gets the raw device band and the
        // receiver's centre, exactly like an I/Q text decoder - and none of the
        // audio.
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_IQ, 0.0, true);
        std::vector<LoadedPlugin> ps{makeImgPlugin("LRPT", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(g_img.created == 1);
        CHECK(g_img.lastRate == kIqRate);
        CHECK(g_img.lastCentre == kCentre);
        std::vector<float> iq(2 * 300, 0.0f);
        r.processIq(iq.data(), 300);
        CHECK(g_img.frames == 300u);
        r.processAudio(iq.data(), 64);
        CHECK(g_img.frames == 300u);
        // The receiver moving reaches it, because where the band sits is what
        // an I/Q decoder tunes within.
        r.retune(137100000.0);
        CHECK(g_img.retunes == 1);
        CHECK(g_img.lastCentre == 137100000.0);
    }
    {
        // An AUDIO-input image decoder must NOT be retuned: it was created with
        // centerHz 0 by the ABI's own rule, so handing it a real RF frequency
        // later would contradict what it was told.
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_AUDIO, 0.0, true);
        std::vector<LoadedPlugin> ps{makeImgPlugin("SSTV", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        r.retune(137100000.0);
        CHECK(g_img.retunes == 0);
        CHECK(g_img.lastCentre == 0.0);
    }
    {
        // A fixed-rate image decoder the stream cannot supply idles with a
        // reason, and is matched against the rate of the stream IT asked for -
        // checking an I/Q decoder against the audio rate would refuse every
        // correct plugin.
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_IQ, 140000.0, false);
        std::vector<LoadedPlugin> ps{makeImgPlugin("LRPT", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(g_img.created == 0);
        CHECK(r.activeCount() == 0u);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(at(st, 0).reason == DecoderIdleReason::RateMismatch);
        CHECK(at(st, 0).output == cascade::core::DecoderOutput::Image);
        CHECK(at(st, 0).stream == cascade::core::DecoderStream::Iq);
        CHECK(at(st, 0).wantRateHz == 140000.0);
    }
    {
        // A rate an image decoder CAN have: 48 kHz audio, which is what the
        // pipeline delivers. Running, and reported as an image producer so the
        // panel does not describe a picture as silent text.
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_AUDIO, 48000.0, false);
        std::vector<LoadedPlugin> ps{makeImgPlugin("SSTV", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 1u);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(at(st, 0).reason == DecoderIdleReason::Running);
        CHECK(at(st, 0).output == cascade::core::DecoderOutput::Image);
    }

    // --- Images come back out, copied and released ------------------------
    {
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_AUDIO, 0.0, false);
        std::vector<LoadedPlugin> ps{makeImgPlugin("SSTV", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);

        std::vector<cascade::core::HostImage> imgs;
        // Nothing offered yet: one entry, empty, tagged with the plugin.
        r.pollImages(imgs);
        CHECK(imgs.size() == 1u);
        CHECK(at(imgs, 0).plugin == "SSTV");
        CHECK(at(imgs, 0).width == 0u);
        CHECK(at(imgs, 0).revision == 0u);
        CHECK(g_img.releases == 0);  // nothing borrowed, nothing to release

        // A picture with PADDED rows (stride 5 for a 2-pixel row): the host
        // copy must be tightly packed, or every consumer downstream - the
        // texture upload, the BMP writer - sees a sheared image.
        offerGray2x2(/*stride=*/5, /*seq=*/7);
        r.pollImages(imgs);
        CHECK(at(imgs, 0).width == 2u && at(imgs, 0).height == 2u);
        CHECK(at(imgs, 0).format == CASCADE_IMAGE_GRAY8);
        CHECK(at(imgs, 0).complete);
        CHECK(at(imgs, 0).sequence == 7u);
        // The packed pixels are compared as a WHOLE VECTOR: indexing them
        // element by element after a size check has the same out-of-bounds
        // hazard as indexing the image list itself.
        CHECK(at(imgs, 0).pixels == std::vector<std::uint8_t>({1, 2, 3, 4}));
        CHECK(at(imgs, 0).revision == 1u);
        // The borrow is returned immediately, once per successful poll.
        CHECK(g_img.releases == 1);

        // Nothing new offered: the picture STAYS, and is not re-copied. A GUI
        // that had to be handed the pixels again every frame would re-upload a
        // megapixel texture 60 times a second to show the same image.
        g_img.offer = 0;
        const int pollsBefore = g_img.polls;
        r.pollImages(imgs);
        CHECK(at(imgs, 0).width == 2u);
        CHECK(at(imgs, 0).revision == 1u);
        CHECK(g_img.releases == 1);
        CHECK(g_img.polls == pollsBefore + 1);  // asked, told nothing pending
    }
    {
        // Nonsense geometry is refused, not allocated. The width is a
        // third-party number about to size a buffer and bound a read; a stride
        // shorter than a row would walk the host off the end of the plugin's
        // buffer on the last line.
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_AUDIO, 0.0, false);
        std::vector<LoadedPlugin> ps{makeImgPlugin("Bad", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        std::vector<cascade::core::HostImage> imgs;

        offerGray2x2(2, 1);
        g_img.stride = 1;  // shorter than the 2-byte row
        r.pollImages(imgs);
        CHECK(at(imgs, 0).width == 0u);
        CHECK(at(imgs, 0).revision == 0u);
        CHECK(g_img.releases == 1);  // refused, but still released

        offerGray2x2(2, 2);
        g_img.w = CASCADE_IMAGE_MAX_DIM + 1u;
        r.pollImages(imgs);
        CHECK(at(imgs, 0).width == 0u);

        offerGray2x2(2, 3);
        g_img.fmt = 99u;  // not GRAY8 or RGB24
        r.pollImages(imgs);
        CHECK(at(imgs, 0).width == 0u);
        CHECK(at(imgs, 0).revision == 0u);
        CHECK(g_img.releases == 3);
    }
    {
        // A rebuild that puts a DIFFERENT plugin in the same slot must re-seed
        // the caller's buffer. Keeping the old entry would label the new
        // decoder's picture with the previous plugin's name - and the count
        // alone cannot detect the swap.
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_AUDIO, 0.0, false);
        PluginRunner r;
        std::vector<cascade::core::HostImage> imgs;
        r.rebuild({makeImgPlugin("First", &api)}, 48000.0, kIqRate, kCentre);
        offerGray2x2(2, 1);
        r.pollImages(imgs);
        CHECK(imgs.size() == 1u);
        CHECK(at(imgs, 0).plugin == "First");
        CHECK(at(imgs, 0).width == 2u);

        g_img.offer = 0;
        r.rebuild({makeImgPlugin("Second", &api)}, 48000.0, kIqRate, kCentre);
        r.pollImages(imgs);
        CHECK(imgs.size() == 1u);
        CHECK(at(imgs, 0).plugin == "Second");
        CHECK(at(imgs, 0).width == 0u);      // the old plugin's picture is gone
        CHECK(at(imgs, 0).revision == 0u);
    }
    {
        // Status text from an image decoder joins the same log as everything
        // else: it is where a slow-scan decoder says what it is receiving, and
        // a second log for it would only hide it.
        resetImg();
        g_img.queued = "SSTV Martin M1 sync at 1200 Hz\n";
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_AUDIO, 0.0, false);
        std::vector<LoadedPlugin> ps{makeImgPlugin("SSTV", &api)};
        PluginRunner r;
        r.rebuild(ps, 48000.0, kIqRate, kCentre);
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        const std::vector<DecodedLine> out = r.drainText();
        CHECK(out.size() == 1u);
        CHECK(at(out, 0).plugin == "SSTV");
        CHECK(at(out, 0).text == "SSTV Martin M1 sync at 1200 Hz");
    }
    {
        // Lifetime, the rule that matters most: a handle outliving its module
        // is a crash inside someone else's DLL.
        resetImg();
        const CascadeImageDecoderApi api = makeImgApi(CASCADE_INPUT_AUDIO, 0.0, false);
        std::vector<LoadedPlugin> ps{makeImgPlugin("A", &api), makeImgPlugin("B", &api)};
        {
            PluginRunner r;
            r.rebuild(ps, 48000.0, kIqRate, kCentre);
            CHECK(g_img.created == 2);
            r.rebuild(ps, 48000.0, kIqRate, kCentre);  // a rescan
            CHECK(g_img.destroyed == 2);
            r.clear();
            CHECK(g_img.destroyed == 4);
            r.clear();  // idempotent
            CHECK(g_img.destroyed == 4);
        }
        CHECK(g_img.created == g_img.destroyed);
    }
    {
        // A plugin declaring BOTH a text decoder and an image decoder gets one
        // instance of each, and both are fed.
        resetFake();
        resetImg();
        const CascadeDecoderApi tApi = makeApi(0u);
        const CascadeImageDecoderApi iApi = makeImgApi(CASCADE_INPUT_AUDIO, 0.0, false);
        LoadedPlugin p = makePlugin("Both", &tApi);
        p.imageDecoder = &iApi;
        PluginRunner r;
        r.rebuild({p}, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 2u);
        std::vector<float> block(128, 0.0f);
        r.processAudio(block.data(), block.size());
        CHECK(g_fake.samples == 128u);
        CHECK(g_img.frames == 128u);
    }
    {
        // Polling images with nothing loaded is safe, and empties the caller's
        // buffer rather than leaving a picture from a plugin that is gone.
        resetImg();
        PluginRunner r;
        std::vector<cascade::core::HostImage> imgs;
        r.pollImages(imgs);
        CHECK(imgs.empty());
    }

    // --- Feeding with nothing loaded is safe ------------------------------
    {
        resetFake();
        PluginRunner r;
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        CHECK(r.drainText().empty());
        CHECK(r.activeCount() == 0u);
    }

    return testSummary("test_plugin_runner");
}
