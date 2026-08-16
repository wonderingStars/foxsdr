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
