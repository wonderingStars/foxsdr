// CASCADE_CAP_AUDIO_OUT: a plugin playing sound through the host.
//
// WHAT IS BEING PROVED, and why each half exists.
//
// The first half drives a fake audio-out table through the real PluginRunner.
// No DLL is loaded: LoadedPlugin carries plain pointers to API tables, so a
// fake plugin is a static table plus a counter, and every case can assert on
// what the HOST did to the plugin - when it pulled, when it did not, what it
// did with mono, what it did with a rate that is not the sink's - rather than
// on a side effect observed through a real module.
//
// The second half runs the whole thing: a real Pipeline, a real signal
// generator, a real demodulator, and the fake plugin taking the speakers away
// from it mid-stream. That is the only place the question the feature exists to
// answer can be asked at all - does the changeover click, and does it leave a
// hole - because both are properties of the finished audio and neither is
// visible from the runner alone.
//
// The reference tone here is synthesised in this file and measured back out of
// the chain by correlation at the frequency it was made at. Nothing is compared
// against a constant taken from the implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::LoadedPlugin;
using cascade::core::PluginRejection;
using cascade::core::PluginRunner;

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kSinkRateHz = cascade::core::Pipeline::kAudioRateHz;  // 48000

// --- The fake plugin -------------------------------------------------------
//
// One object per instance, reached through the handle its create() returned -
// which is also how the host is expected to address it, so a test with two
// plugins running at once is testing the real dispatch and not a global.

enum class PullMode {
    Tone,     // hands back a tone, up to `maxPerPull` frames at a time
    Nothing,  // returns 0: "nothing right now", a gap and not an end
    Dead,     // returns < 0: stopped producing for good
};

struct FakePlugin {
    // Decoder side.
    int created = 0;
    int destroyed = 0;
    std::size_t iqFrames = 0;

    // Audio side.
    int wantsSpeakers = 0;  // what active() answers
    int activeCalls = 0;
    int pulls = 0;
    std::size_t framesPulled = 0;
    PullMode mode = PullMode::Tone;
    std::size_t maxPerPull = 0;  // 0 = whatever was asked for

    // The tone it plays, on ITS OWN clock: the sample counter is continuous
    // across pulls, so a resampler downstream sees a real signal and not a
    // phase discontinuity every block.
    double toneHz = 1000.0;
    double rateHz = kSinkRateHz;
    float amp = 0.5f;
    std::uint32_t channels = 1;
    float rightGain = 1.0f;  // so a stereo test can tell the channels apart
    std::uint64_t n = 0;
};

FakePlugin g_a;
FakePlugin g_b;

FakePlugin& self(void* h) { return *static_cast<FakePlugin*>(h); }

void* createA(double, double) {
    ++g_a.created;
    return &g_a;
}
void* createB(double, double) {
    ++g_b.created;
    return &g_b;
}
void fakeProcess(void* h, const float*, std::size_t frames) { self(h).iqFrames += frames; }
std::int32_t fakePollText(void*, char*, std::size_t) { return 0; }
void fakeDestroy(void* h) { ++self(h).destroyed; }

std::int32_t fakeActive(void* h) {
    ++self(h).activeCalls;
    return self(h).wantsSpeakers;
}

std::int32_t fakePull(void* h, float* out, std::size_t frames) {
    FakePlugin& f = self(h);
    ++f.pulls;
    if (f.mode == PullMode::Dead) { return -1; }
    if (f.mode == PullMode::Nothing) { return 0; }
    std::size_t n = frames;
    if (f.maxPerPull != 0 && n > f.maxPerPull) { n = f.maxPerPull; }
    for (std::size_t i = 0; i < n; ++i) {
        const float s = f.amp * static_cast<float>(std::sin(
                                    kTwoPi * f.toneHz * static_cast<double>(f.n) / f.rateHz));
        if (f.channels == 2u) {
            out[2 * i] = s;
            out[2 * i + 1] = s * f.rightGain;
        } else {
            out[i] = s;
        }
        ++f.n;
    }
    f.framesPulled += n;
    return static_cast<std::int32_t>(n);
}

CascadeIqDecoderApi makeIqApi(void* (*create)(double, double)) {
    CascadeIqDecoderApi a{};
    a.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
    a.requiredRateHz = 0.0;  // any rate: this decoder is a stand-in
    a.preferredRateHz = 0.0;
    a.create = create;
    a.process = &fakeProcess;
    a.retune = nullptr;
    a.poll_text = &fakePollText;
    a.destroy = &fakeDestroy;
    return a;
}

CascadeAudioOutApi makeAudioApi(std::uint32_t rateHz, std::uint32_t channels) {
    CascadeAudioOutApi a{};
    a.structSize = static_cast<std::uint32_t>(sizeof(CascadeAudioOutApi));
    a.sampleRateHz = rateHz;
    a.channels = channels;
    a.pull = &fakePull;
    a.active = &fakeActive;
    return a;
}

LoadedPlugin makePlugin(const char* name, const char* path, const CascadeIqDecoderApi* iq,
                        const CascadeAudioOutApi* audio) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.path = path;
    p.version = "1.0.0";
    p.capabilities = CASCADE_CAP_IQ_DECODER | (audio != nullptr ? CASCADE_CAP_AUDIO_OUT : 0u);
    p.iqDecoder = iq;
    p.audioOut = audio;
    return p;
}

void resetFakes() {
    g_a = FakePlugin{};
    g_b = FakePlugin{};
}

// Amplitude of `hz` in `x`, by correlation. A pure tone of amplitude A reads
// back as A; anything else reads back near zero, so one number answers both
// "is it the right note" and "is it the right loudness".
double toneAmplitude(const float* x, std::size_t n, double hz, double rateHz) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = kTwoPi * hz * static_cast<double>(i) / rateHz;
        re += static_cast<double>(x[i]) * std::cos(t);
        im += static_cast<double>(x[i]) * std::sin(t);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

double rms(const float* x, std::size_t n) {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) { s += static_cast<double>(x[i]) * x[i]; }
    return std::sqrt(s / static_cast<double>(n));
}

// Longest run of samples whose magnitude is below `floorMag` - the measurement
// of "a gap", in frames. A tone crosses zero, so a run of one or two is the
// signal and not a hole; a run the length of a block is the hole.
std::size_t longestQuietRun(const float* x, std::size_t n, double floorMag) {
    std::size_t best = 0;
    std::size_t run = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::fabs(static_cast<double>(x[i])) < floorMag) {
            ++run;
            if (run > best) { best = run; }
        } else {
            run = 0;
        }
    }
    return best;
}

// How many frames the changeover TAKES, measured on the signal being replaced:
// from the last place it is still at nine tenths of its level to the first
// place it is under one tenth.
//
// THIS IS THE ANTI-CLICK MEASUREMENT, and it is here because the obvious one -
// the size of the step at the seam - cannot fail reliably. A hard cut between
// two tones steps by their difference at whatever phase the seam happens to
// land on, and half the time that difference is small; a test that only catches
// a defect on a coin toss is not a test. How LONG the changeover takes is a
// property of the crossfade itself and does not depend on phase at all: a cut
// finishes inside one audio block, a fade cannot finish in less than the fade.
//
// The sliding window smears the answer by its own length, which is why the
// window is short and the assertion has room either side. 96 frames at 48 kHz
// is exactly six cycles of the 3 kHz tone being measured AND exactly two cycles
// of the 1 kHz one replacing it, so the correlation is exact and the other tone
// cancels out of it completely instead of leaking in as a floor the fade can
// never get below.
std::size_t crossfadeWidth(const float* x, std::size_t n, double hz, double rateHz,
                           double full) {
    constexpr std::size_t kWindow = 96;
    constexpr std::size_t kStep = 4;
    if (n < kWindow + kStep) { return 0; }
    std::size_t lastStrong = 0;
    bool haveStrong = false;
    for (std::size_t i = 0; i + kWindow <= n; i += kStep) {
        if (toneAmplitude(x + i, kWindow, hz, rateHz) > 0.90 * full) {
            lastStrong = i;
            haveStrong = true;
        }
    }
    if (!haveStrong) { return 0; }
    for (std::size_t i = lastStrong + kStep; i + kWindow <= n; i += kStep) {
        if (toneAmplitude(x + i, kWindow, hz, rateHz) < 0.10 * full) { return i - lastStrong; }
    }
    return 0;
}

// The biggest sample-to-sample step in [from, to) - the measurement of "a
// click". A discontinuity between two unrelated signals shows up here as a step
// far larger than either signal can make on its own.
double maxStep(const float* x, std::size_t from, std::size_t to) {
    double best = 0.0;
    for (std::size_t i = from + 1; i < to; ++i) {
        const double d = std::fabs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1]));
        if (d > best) { best = d; }
    }
    return best;
}

template <typename Fn>
bool waitFor(Fn ready, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ready();
}

// ---------------------------------------------------------------------------
// 1. The ABI addition itself
// ---------------------------------------------------------------------------
void testAbiSurface() {
    static_assert((CASCADE_CAP_ALL_KNOWN & CASCADE_CAP_AUDIO_OUT) != 0u,
                  "CASCADE_CAP_ALL_KNOWN must carry the audio-out bit");
    static_assert(CASCADE_CAP_AUDIO_OUT == 0x00000400u, "the bit is part of the contract");
    // The ABI version did NOT move: a new capability is additive, which is the
    // whole claim ABI 3 makes about itself.
    static_assert(CASCADE_PLUGIN_ABI_VERSION == 3,
                  "a new capability bit must not cost an ABI bump");

    // The typed accessor reads the same descriptor everything else does.
    const CascadeAudioOutApi api = makeAudioApi(48000u, 2u);
    const CascadeIqDecoderApi iq = makeIqApi(&createA);
    const CascadeCapabilityEntry caps[] = {
        {CASCADE_CAP_IQ_DECODER, static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi)), &iq},
        {CASCADE_CAP_AUDIO_OUT, static_cast<std::uint32_t>(sizeof(CascadeAudioOutApi)), &api},
    };
    CascadePluginDesc desc{};
    desc.structSize = static_cast<std::uint32_t>(sizeof(CascadePluginDesc));
    desc.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
    desc.name = "Fake";
    desc.version = "1.0.0";
    desc.author = "";
    desc.licence = "MIT";
    desc.capabilities = CASCADE_CAP_IQ_DECODER | CASCADE_CAP_AUDIO_OUT;
    desc.capabilityCount = 2u;
    desc.capabilityTables = caps;
    CHECK(cascade_plugin_audio_out(&desc) == &api);
    CHECK(cascade::core::validatePluginDesc(&desc) == PluginRejection::None);

    // A rate of zero is refused HERE and nowhere else: every consuming table
    // reads 0 as "any rate", and a producer that says it would leave the host
    // resampling from a rate nobody stated.
    {
        CascadeAudioOutApi bad = api;
        bad.sampleRateHz = 0u;
        const CascadeCapabilityEntry c[] = {
            {CASCADE_CAP_IQ_DECODER, static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi)),
             &iq},
            {CASCADE_CAP_AUDIO_OUT, static_cast<std::uint32_t>(sizeof(CascadeAudioOutApi)),
             &bad},
        };
        CascadePluginDesc d = desc;
        d.capabilityTables = c;
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::AudioOutBadRate);
        const std::string said =
            cascade::core::describePluginRejection(PluginRejection::AudioOutBadRate, &d);
        CHECK(said.find("0 Hz") != std::string::npos);
    }
    {
        CascadeAudioOutApi bad = api;
        bad.channels = 3u;
        const CascadeCapabilityEntry c[] = {
            {CASCADE_CAP_IQ_DECODER, static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi)),
             &iq},
            {CASCADE_CAP_AUDIO_OUT, static_cast<std::uint32_t>(sizeof(CascadeAudioOutApi)),
             &bad},
        };
        CascadePluginDesc d = desc;
        d.capabilityTables = c;
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::AudioOutBadChannels);
    }
    {
        CascadeAudioOutApi bad = api;
        bad.pull = nullptr;
        const CascadeCapabilityEntry c[] = {
            {CASCADE_CAP_IQ_DECODER, static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi)),
             &iq},
            {CASCADE_CAP_AUDIO_OUT, static_cast<std::uint32_t>(sizeof(CascadeAudioOutApi)),
             &bad},
        };
        CascadePluginDesc d = desc;
        d.capabilityTables = c;
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::MissingAudioOutFunction);
    }
    // AUDIO_OUT ON ITS OWN IS NOT A PLUGIN. It has no create(), so there is no
    // instance for the host to pull from and it could never make a sound.
    {
        const CascadeCapabilityEntry c[] = {
            {CASCADE_CAP_AUDIO_OUT, static_cast<std::uint32_t>(sizeof(CascadeAudioOutApi)),
             &api},
        };
        CascadePluginDesc d = desc;
        d.capabilities = CASCADE_CAP_AUDIO_OUT;
        d.capabilityCount = 1u;
        d.capabilityTables = c;
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::NoUsableCapability);
    }
}

// ---------------------------------------------------------------------------
// 2. The runner: when it pulls, and when it does not
// ---------------------------------------------------------------------------
void testPullOnlyOnTheAudioPath() {
    resetFakes();
    const CascadeIqDecoderApi iq = makeIqApi(&createA);
    const CascadeAudioOutApi audio = makeAudioApi(48000u, 1u);
    PluginRunner runner;
    runner.rebuild({makePlugin("Fake DAB", "C:/plugins/fake.dll", &iq, &audio)}, kSinkRateHz,
                   2048000.0, 222.064e6);
    CHECK(g_a.created == 1);

    // Feeding the decoder its samples must not produce a single pull: the
    // decode and the playback are two different questions asked on the same
    // thread, and only one of them is asked by the audio path.
    std::vector<float> iqBlock(4096, 0.1f);
    for (int i = 0; i < 8; ++i) { runner.processIq(iqBlock.data(), 2048); }
    CHECK(g_a.iqFrames == 8u * 2048u);
    CHECK(g_a.pulls == 0);
    CHECK(g_a.activeCalls == 0);

    // Not active: no takeover, and the caller's buffers are left exactly as
    // they were - the demodulated audio is what plays.
    std::vector<float> l(256, 7.0f);
    std::vector<float> r(256, -7.0f);
    CHECK(runner.pullPluginAudio(l.data(), r.data(), 256) == false);
    CHECK(l[0] == 7.0f);
    CHECK(r[255] == -7.0f);
    CHECK(g_a.activeCalls == 1);
    CHECK(g_a.pulls == 0);
    CHECK(runner.playingPlugin().empty());

    // Active: the takeover happens on the very next block.
    g_a.wantsSpeakers = 1;
    CHECK(runner.pullPluginAudio(l.data(), r.data(), 256) == true);
    CHECK(g_a.pulls == 1);
    CHECK(runner.playingPlugin() == "Fake DAB");
    CHECK(runner.playingPluginKey() == "fake.dll");

    // MONO GOES TO BOTH EARS, at full level. The plugin wrote one channel; both
    // sides of the sink must carry it, or every mono service plays half as loud
    // out of one speaker.
    for (std::size_t i = 0; i < 256; ++i) { CHECK(l[i] == r[i]); }
    CHECK(toneAmplitude(l.data(), 256, 1000.0, kSinkRateHz) > 0.45);
    CHECK(toneAmplitude(l.data(), 256, 1000.0, kSinkRateHz) < 0.55);

    // ...and it gives them back when it stops asking.
    g_a.wantsSpeakers = 0;
    const int pullsAtRelease = g_a.pulls;
    CHECK(runner.pullPluginAudio(l.data(), r.data(), 256) == false);
    CHECK(g_a.pulls == pullsAtRelease);
    CHECK(runner.playingPlugin().empty());

    runner.clear();
    CHECK(g_a.destroyed == 1);
    CHECK(runner.playingPlugin().empty());
}

void testStereoPassesThroughUntouched() {
    resetFakes();
    g_a.channels = 2u;
    g_a.rightGain = 0.5f;  // a right channel that cannot be confused with left
    g_a.wantsSpeakers = 1;
    const CascadeIqDecoderApi iq = makeIqApi(&createA);
    const CascadeAudioOutApi audio = makeAudioApi(48000u, 2u);
    PluginRunner runner;
    runner.rebuild({makePlugin("Stereo", "C:/plugins/stereo.dll", &iq, &audio)}, kSinkRateHz,
                   2048000.0, 100.0e6);

    std::vector<float> l(512, 0.0f);
    std::vector<float> r(512, 0.0f);
    CHECK(runner.pullPluginAudio(l.data(), r.data(), 512) == true);
    // Rates agree, so this is a copy and the samples must be EXACT: any
    // filtering here would be the host resampling 48000 to 48000.
    for (std::size_t i = 0; i < 512; ++i) {
        const float s = 0.5f * static_cast<float>(
                                   std::sin(kTwoPi * 1000.0 * static_cast<double>(i) / 48000.0));
        CHECK_NEAR(l[i], s, 1e-6);
        CHECK_NEAR(r[i], 0.5f * s, 1e-6);
    }
}

void testResampledFrom44100() {
    resetFakes();
    g_a.rateHz = 44100.0;
    g_a.toneHz = 1000.0;
    g_a.amp = 0.5f;
    g_a.wantsSpeakers = 1;
    const CascadeIqDecoderApi iq = makeIqApi(&createA);
    const CascadeAudioOutApi audio = makeAudioApi(44100u, 1u);
    PluginRunner runner;
    runner.rebuild({makePlugin("CD rate", "C:/plugins/cd.dll", &iq, &audio)}, kSinkRateHz,
                   2048000.0, 100.0e6);

    // Several blocks: the first carries the resampler's startup transient, and
    // measuring a filter's first samples measures the filter waking up.
    std::vector<float> l(1024, 0.0f);
    std::vector<float> r(1024, 0.0f);
    for (int i = 0; i < 4; ++i) { CHECK(runner.pullPluginAudio(l.data(), r.data(), 1024)); }

    // A 1 kHz tone made at 44100 must still be a 1 kHz tone at 48000, at the
    // same level. Reading it at 44100 - the mistake of not resampling at all -
    // would put it at 1088 Hz.
    const double at1000 = toneAmplitude(l.data(), 1024, 1000.0, kSinkRateHz);
    const double at1088 = toneAmplitude(l.data(), 1024, 1088.4, kSinkRateHz);
    std::printf("44100->48000: amplitude at 1000 Hz %.4f, at 1088 Hz %.4f\n", at1000, at1088);
    CHECK(at1000 > 0.45);
    CHECK(at1000 < 0.55);
    CHECK(at1088 < 0.10);
    // And it kept up: a resampler asked for the wrong number of input frames
    // charges a gap on nearly every block.
    CHECK(runner.audioGaps() == 0u);
}

void testGapIsSilenceAndIsCounted() {
    resetFakes();
    g_a.wantsSpeakers = 1;
    const CascadeIqDecoderApi iq = makeIqApi(&createA);
    const CascadeAudioOutApi audio = makeAudioApi(48000u, 1u);
    PluginRunner runner;
    runner.rebuild({makePlugin("Stuttering", "C:/plugins/stutter.dll", &iq, &audio)},
                   kSinkRateHz, 2048000.0, 100.0e6);

    std::vector<float> l(256, 9.0f);
    std::vector<float> r(256, 9.0f);
    g_a.mode = PullMode::Nothing;
    CHECK(runner.pullPluginAudio(l.data(), r.data(), 256) == true);
    // STILL PLAYING. A decoder between superframes has not stopped being the
    // thing the user is listening to, so the host fills the hole rather than
    // handing the speakers back to the hiss.
    CHECK(runner.playingPlugin() == "Stuttering");
    for (std::size_t i = 0; i < 256; ++i) {
        CHECK(l[i] == 0.0f);
        CHECK(r[i] == 0.0f);
    }
    CHECK(runner.audioGaps() == 1u);
    CHECK(runner.audioGapFrames() == 256u);

    // A short pull is the same thing for the shortfall only.
    g_a.mode = PullMode::Tone;
    g_a.maxPerPull = 100;
    std::fill(l.begin(), l.end(), 9.0f);
    CHECK(runner.pullPluginAudio(l.data(), r.data(), 256) == true);
    CHECK(runner.audioGaps() == 2u);
    CHECK(runner.audioGapFrames() == 256u + 156u);
    CHECK(l[99] != 0.0f);
    CHECK(l[200] == 0.0f);
}

void testNegativePullEndsTheTakeover() {
    resetFakes();
    g_a.wantsSpeakers = 1;
    g_a.mode = PullMode::Dead;
    const CascadeIqDecoderApi iq = makeIqApi(&createA);
    const CascadeAudioOutApi audio = makeAudioApi(48000u, 1u);
    PluginRunner runner;
    runner.rebuild({makePlugin("Doomed", "C:/plugins/doomed.dll", &iq, &audio)}, kSinkRateHz,
                   2048000.0, 100.0e6);

    std::vector<float> l(256, 3.0f);
    std::vector<float> r(256, 3.0f);
    // The first block takes the speakers and the pull that follows gives them
    // straight back - there is nothing of this plugin's left to play out, so
    // the demodulated audio returns in the same block rather than after one of
    // silence.
    CHECK(runner.pullPluginAudio(l.data(), r.data(), 256) == true);
    CHECK(g_a.pulls == 1);
    CHECK(l[0] == 0.0f);  // the takeover block itself: nothing to play

    CHECK(runner.pullPluginAudio(l.data(), r.data(), 256) == false);
    CHECK(runner.playingPlugin().empty());
    // NEVER PULLED AGAIN. The ABI defines a negative return as permanent, and
    // a host that kept asking would be feeding a plugin that has given up.
    const int pullsAtDeath = g_a.pulls;
    g_a.mode = PullMode::Tone;  // even if it changes its mind
    for (int i = 0; i < 10; ++i) {
        CHECK(runner.pullPluginAudio(l.data(), r.data(), 256) == false);
    }
    CHECK(g_a.pulls == pullsAtDeath);
    CHECK(runner.playingPlugin().empty());
}

void testFirstActiveWins() {
    resetFakes();
    g_a.wantsSpeakers = 1;
    g_b.wantsSpeakers = 1;
    g_b.toneHz = 4000.0;
    const CascadeIqDecoderApi iqA = makeIqApi(&createA);
    const CascadeIqDecoderApi iqB = makeIqApi(&createB);
    const CascadeAudioOutApi audioA = makeAudioApi(48000u, 1u);
    const CascadeAudioOutApi audioB = makeAudioApi(48000u, 1u);
    PluginRunner runner;
    runner.rebuild({makePlugin("First", "C:/plugins/a.dll", &iqA, &audioA),
                    makePlugin("Second", "C:/plugins/b.dll", &iqB, &audioB)},
                   kSinkRateHz, 2048000.0, 100.0e6);

    std::vector<float> l(256, 0.0f);
    std::vector<float> r(256, 0.0f);
    for (int i = 0; i < 4; ++i) { CHECK(runner.pullPluginAudio(l.data(), r.data(), 256)); }
    CHECK(runner.playingPlugin() == "First");
    CHECK(g_a.pulls == 4);
    // The loser is ASKED every block - that is how it would get the speakers
    // when the winner lets go - and PULLED not once.
    CHECK(g_b.activeCalls == 4);
    CHECK(g_b.pulls == 0);
    CHECK(toneAmplitude(l.data(), 256, 1000.0, kSinkRateHz) > 0.45);
    CHECK(toneAmplitude(l.data(), 256, 4000.0, kSinkRateHz) < 0.05);

    // ...and that is exactly what happens when it does.
    g_a.wantsSpeakers = 0;
    CHECK(runner.pullPluginAudio(l.data(), r.data(), 256));
    CHECK(runner.playingPlugin() == "Second");
    CHECK(g_b.pulls == 1);
    CHECK(toneAmplitude(l.data(), 256, 4000.0, kSinkRateHz) > 0.45);
}

// ---------------------------------------------------------------------------
// 3. End to end: the changeover, in the real chain
// ---------------------------------------------------------------------------
void testPipelineTakeover() {
    resetFakes();
    g_a.toneHz = 1000.0;
    g_a.amp = 0.5f;
    g_a.channels = 2u;
    g_a.rightGain = 1.0f;

    const CascadeIqDecoderApi iq = makeIqApi(&createA);
    const CascadeAudioOutApi audio = makeAudioApi(48000u, 2u);

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // headless: the chain runs, no device is opened
    cascade::core::Pipeline p(cfg);
    // A carrier 3 kHz up, demodulated single-sideband, is a clean 3 kHz audio
    // tone - loud, steady, and impossible to confuse with the plugin's 1 kHz.
    p.setDemodMode(cascade::dsp::DemodMode::USB);
    p.setSquelchDb(-200.0f);
    p.sigGen().setTone(0, 3000.0, 0.0f);
    p.sigGen().setNoiseFloorDb(-300.0f);

    PluginRunner runner;
    runner.rebuild({makePlugin("Fake DAB", "C:/plugins/fake.dll", &iq, &audio)},
                   cascade::core::Pipeline::kAudioRateHz, cfg.sampleRateHz, 222.064e6);
    p.setPluginRunner(&runner);
    p.start();

    constexpr std::size_t kWin = 4096;
    std::vector<float> left(kWin, 0.0f);
    std::vector<float> right(kWin, 0.0f);

    // (a) The demodulated audio, before anything takes it away. Without this
    // every measurement below would be vacuous: a chain producing nothing at
    // all would pass a "the plugin is what is playing" test perfectly.
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 3u * kWin; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double demodRms = rms(left.data(), kWin);
    const double demodAt3k = toneAmplitude(left.data(), kWin, 3000.0, kSinkRateHz);
    std::printf("demod: rms=%.4f amplitude at 3 kHz=%.4f\n", demodRms, demodAt3k);
    CHECK(demodRms > 0.01);
    CHECK(demodAt3k > 0.01);
    CHECK(toneAmplitude(left.data(), kWin, 1000.0, kSinkRateHz) < 0.02);
    CHECK(g_a.pulls == 0);  // nothing pulled while the plugin is not asking
    const double demodStep = maxStep(left.data(), 0, kWin);

    // (b) The takeover, caught in the window it happens in. Half the tap is
    // filled after the flag is set, so the seam is somewhere in the middle of
    // what is read back.
    const std::uint64_t mark = p.audioSamplesProduced();
    g_a.wantsSpeakers = 1;
    CHECK(waitFor([&] { return p.audioSamplesProduced() - mark >= kWin / 2; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);

    // NO HOLE. A changeover that silences the audio for a block is a defect a
    // user reports as a click of its own. One audio block here is
    // fftSize/decimation scaled to 48 kHz - about 49 frames - so anything even
    // approaching that length is a gap, while the one or two samples a tone
    // spends crossing zero are the signal.
    const std::size_t quiet = longestQuietRun(left.data(), kWin, 1e-6);
    const double seamStep = maxStep(left.data(), 0, kWin);
    std::printf("takeover: longest quiet run=%zu frames, max step=%.4f (demod alone %.4f)\n",
                quiet, seamStep, demodStep);
    CHECK(quiet < 16u);

    // NO CLICK, and the length of the changeover is what proves it (see
    // crossfadeWidth). A hard cut finishes inside ~80 frames - the measuring
    // window's own smear and nothing else - while the 240-frame linear fade
    // takes 90% of the way down to 10% in 0.8 of its length, about 192.
    // Measured at 192 +/- 8; the bound sits between the two answers, not
    // beside one of them.
    const double demodPeak = demodAt3k;
    const std::size_t width =
        crossfadeWidth(left.data(), kWin, 3000.0, kSinkRateHz, demodPeak);
    std::printf("takeover: changeover width=%zu frames (fade is %zu)\n", width,
                cascade::core::Pipeline::kPluginFadeFrames);
    CHECK(width > 150u);
    CHECK(width < 3u * cascade::core::Pipeline::kPluginFadeFrames);

    // ...and the seam is inside what the signals themselves already do, which
    // is the residual the fade leaves. A secondary bound: it catches a cut only
    // when the phases are unlucky, where the width above catches one always.
    CHECK(seamStep < 1.2 * demodStep + 0.02);

    // (c) ...and by the end of the window the plugin is what is playing.
    CHECK(waitFor([&] { return p.audioSamplesProduced() - mark >= 3u * kWin; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double playAt1k = toneAmplitude(left.data(), kWin, 1000.0, kSinkRateHz);
    const double playAt3k = toneAmplitude(left.data(), kWin, 3000.0, kSinkRateHz);
    std::printf("playing: amplitude at 1 kHz=%.4f, at 3 kHz=%.4f, gaps=%llu\n", playAt1k,
                playAt3k, static_cast<unsigned long long>(runner.audioGaps()));
    CHECK(playAt1k > 0.45);
    CHECK(playAt1k < 0.55);
    // REPLACED, not mixed: the demodulated hiss a digital carrier makes must
    // not be playing under the programme.
    CHECK(playAt3k < 0.02);
    CHECK(runner.playingPlugin() == "Fake DAB");

    // (d) The mute is the user's, and it applies to a plugin exactly as it
    // applies to the receiver. A plugin that could play over a muted receiver
    // would be a plugin that took the mute lamp away from the user.
    p.setAudioMuted(true);
    const std::uint64_t muteMark = p.audioSamplesProduced();
    CHECK(waitFor([&] { return p.audioSamplesProduced() - muteMark >= 2u * kWin; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    std::size_t nonZero = 0;
    for (std::size_t i = 0; i < kWin; ++i) {
        if (left[i] != 0.0f || right[i] != 0.0f) { ++nonZero; }
    }
    std::printf("muted while a plugin plays: %zu non-zero of %zu\n", nonZero, kWin);
    CHECK(nonZero == 0u);
    p.setAudioMuted(false);

    // (e) Handing them back brings the demodulated audio with it.
    g_a.wantsSpeakers = 0;
    const std::uint64_t backMark = p.audioSamplesProduced();
    CHECK(waitFor([&] { return p.audioSamplesProduced() - backMark >= 3u * kWin; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double backAt3k = toneAmplitude(left.data(), kWin, 3000.0, kSinkRateHz);
    const double backAt1k = toneAmplitude(left.data(), kWin, 1000.0, kSinkRateHz);
    std::printf("returned: amplitude at 3 kHz=%.4f, at 1 kHz=%.4f\n", backAt3k, backAt1k);
    CHECK(backAt3k > 0.01);
    CHECK(backAt1k < 0.02);
    CHECK(runner.playingPlugin().empty());

    // The decoder was being fed its I/Q the whole time - the audio path is an
    // addition to the decode, not a replacement for it.
    CHECK(g_a.iqFrames > 0u);

    p.stop();
    p.setPluginRunner(nullptr);
    runner.clear();
    CHECK(g_a.destroyed == 1);
}

}  // namespace

int main() {
    testAbiSurface();
    testPullOnlyOnTheAudioPath();
    testStereoPassesThroughUntouched();
    testResampledFrom44100();
    testGapIsSilenceAndIsCounted();
    testNegativePullEndsTheTakeover();
    testFirstActiveWins();
    testPipelineTakeover();
    return testSummary("test_plugin_audio");
}
