// Tests for the plugin decoders a patch runs - core/patch_runner.hpp's
// buildDecoders(), and the feeding, polling and destroying around it.
//
// THE PLUGINS HERE ARE FAKES WRITTEN TO THE REAL C ABI. Each one is a
// CascadeIqDecoderApi or CascadeDecoderApi table of plain functions, exactly as
// a DLL would export it, so everything the runner does to a plugin is done to
// these - create() with a rate and a centre, process() with interleaved I/Q,
// poll_text() with a capped buffer, destroy() exactly once. What they add is
// memory: every call is recorded, and the I/Q a fake receives is MEASURED, so
// a test can say "this decoder was handed its own channel, tuned, at the rate
// it asked for" rather than "process() was called".
//
// The lifetime rule under test is the ABI's: destroy() on the control thread,
// after the last process() and poll_text(). The patch keeps it by construction
// - a decoder dies with its set, and a set only dies on the GUI thread - and
// [P8] checks that with a real DSP thread running.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_runner.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/plugin_abi.h"
#include "test_check.hpp"

using cascade::core::patch::buildStripSet;
using cascade::core::patch::compile;
using cascade::core::patch::Connect;
using cascade::core::patch::DecoderInfo;
using cascade::core::patch::Graph;
using cascade::core::patch::kNoNode;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::PatchLine;
using cascade::core::patch::Plan;
using cascade::core::patch::PluginApis;
using cascade::core::patch::PortType;
using cascade::core::patch::Runner;
using cascade::core::patch::StripSet;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 2400000.0;
constexpr double kCentre = 100000000.0;

// --- the fakes' memory ---------------------------------------------------------

struct Fake {
    double rate = 0.0;
    double centre = 0.0;
    std::uint64_t frames = 0;
    // Mean rotation of what arrived: the sum of each sample times the
    // conjugate of the one before it. Its angle is the phase step per sample.
    std::complex<double> rot{0.0, 0.0};
    std::complex<double> prev{0.0, 0.0};
    bool havePrev = false;
    // Audio: correlation against a 1 kHz reference at the created rate.
    double sc = 0.0, ss = 0.0, sv = 0.0;
    std::uint64_t audioIndex = 0;
    // What poll_text hands out next.
    std::string pending;
    bool failNow = false;
    int polls = 0;
};

struct Ledger {
    std::mutex m;
    int creates = 0;
    int destroys = 0;
    std::vector<std::thread::id> destroyThreads;
    std::vector<Fake*> live;
};
Ledger g_ledger;

void resetLedger() {
    std::lock_guard<std::mutex> lock(g_ledger.m);
    g_ledger.creates = 0;
    g_ledger.destroys = 0;
    g_ledger.destroyThreads.clear();
    g_ledger.live.clear();
}

Fake* track(Fake* f) {
    std::lock_guard<std::mutex> lock(g_ledger.m);
    ++g_ledger.creates;
    g_ledger.live.push_back(f);
    return f;
}

void untrack(void* h) {
    std::lock_guard<std::mutex> lock(g_ledger.m);
    ++g_ledger.destroys;
    g_ledger.destroyThreads.push_back(std::this_thread::get_id());
    for (auto it = g_ledger.live.begin(); it != g_ledger.live.end(); ++it) {
        if (*it == h) {
            g_ledger.live.erase(it);
            break;
        }
    }
    delete static_cast<Fake*>(h);
}

// The last instance created - the tests create one at a time where they read it.
Fake* lastFake() {
    std::lock_guard<std::mutex> lock(g_ledger.m);
    return g_ledger.live.empty() ? nullptr : g_ledger.live.back();
}

std::int32_t pollFake(void* h, char* buf, std::size_t cap) {
    Fake* f = static_cast<Fake*>(h);
    ++f->polls;
    if (f->failNow) { return -1; }
    if (f->pending.empty()) { return 0; }
    const std::size_t n = (f->pending.size() < cap) ? f->pending.size() : cap;
    std::memcpy(buf, f->pending.data(), n);
    f->pending.erase(0, n);
    return static_cast<std::int32_t>(n);
}

// --- an I/Q fake ---------------------------------------------------------------

void* iqCreate(double rateHz, double centerHz) {
    Fake* f = new Fake;
    f->rate = rateHz;
    f->centre = centerHz;
    return track(f);
}

void iqProcess(void* h, const float* iq, std::size_t frames) {
    Fake* f = static_cast<Fake*>(h);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::complex<double> z(iq[2 * i], iq[2 * i + 1]);
        if (f->havePrev) { f->rot += z * std::conj(f->prev); }
        f->prev = z;
        f->havePrev = true;
    }
    f->frames += frames;
}

CascadeIqDecoderApi makeIqApi(double requiredRateHz) {
    CascadeIqDecoderApi a{};
    a.structSize = sizeof(CascadeIqDecoderApi);
    a.requiredRateHz = requiredRateHz;
    a.preferredRateHz = 0.0;
    a.create = iqCreate;
    a.process = iqProcess;
    a.retune = nullptr;
    a.poll_text = pollFake;
    a.destroy = untrack;
    return a;
}

// --- an audio fake -------------------------------------------------------------

void* audioCreate(std::uint32_t rateHz) {
    Fake* f = new Fake;
    f->rate = static_cast<double>(rateHz);
    return track(f);
}

void audioProcess(void* h, const float* s, std::size_t count) {
    Fake* f = static_cast<Fake*>(h);
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(f->audioIndex++) / f->rate;
        const double ref = std::sin(2.0 * kPi * 1000.0 * t);
        const double c = std::cos(2.0 * kPi * 1000.0 * t);
        // Quadrature, so an arbitrary filter delay does not hide the tone.
        f->sc += static_cast<double>(s[i]) * ref;
        f->ss += static_cast<double>(s[i]) * c;
        f->sv += static_cast<double>(s[i]) * s[i];
    }
    f->frames += count;
}

CascadeDecoderApi makeAudioApi(std::uint32_t requiredRateHz) {
    CascadeDecoderApi a{};
    a.structSize = sizeof(CascadeDecoderApi);
    a.requiredRateHz = requiredRateHz;
    a.create = audioCreate;
    a.process = audioProcess;
    a.poll_text = pollFake;
    a.destroy = untrack;
    return a;
}

// A plugin whose create() refuses.
void* refuseCreate(double, double) { return nullptr; }

// --- signals -------------------------------------------------------------------

// A carrier at `offsetHz` from the radio centre, amplitude-modulated 50% by a
// 1 kHz tone, so both an I/Q decoder (rotation) and an AM demodulator (the
// tone) have something to find.
std::vector<std::complex<float>> signal(double offsetHz, std::size_t n, std::size_t start = 0) {
    std::vector<std::complex<float>> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(start + i) / kRate;
        const double env = 1.0 + 0.5 * std::sin(2.0 * kPi * 1000.0 * t);
        const double ph = 2.0 * kPi * offsetHz * t;
        out[i] = std::complex<float>(static_cast<float>(env * std::cos(ph)),
                                     static_cast<float>(env * std::sin(ph)));
    }
    return out;
}

double rotationHz(const Fake& f) {
    return std::arg(f.rot) * f.rate / (2.0 * kPi);
}

// Runs `blocks` blocks of the signal through the runner, continuous in time.
void run(Runner& r, double offsetHz, int blocks, std::size_t blockLen = 24000) {
    for (int b = 0; b < blocks; ++b) {
        const auto sig = signal(offsetHz, blockLen, static_cast<std::size_t>(b) * blockLen);
        r.process(sig.data(), sig.size());
    }
}

// Radio -> Channel -> Decoder(I/Q) -> Text, with the channel at `chanHz`.
struct IqOnChannel {
    Graph g;
    NodeId radio, chan, dec, text;
    IqOnChannel(double chanHz, const std::string& plugin) {
        radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        chan = g.addNode(NodeKind::Channel, "Tower", PortType::Iq);
        dec = g.addNode(NodeKind::Decoder, "My decoder", PortType::Iq);
        text = g.addNode(NodeKind::Sink, "Text", PortType::Text);
        g.mutableNode(chan)->freqHz = chanHz;
        g.mutableNode(dec)->plugin = plugin;
        g.connect(radio, 0, chan, 0);
        g.connect(chan, 0, dec, 0);
        g.connect(dec, 0, text, 0);
    }
};

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("patch decoder tests\n");

    const CascadeIqDecoderApi anyIq = makeIqApi(0.0);
    const CascadeIqDecoderApi iq192 = makeIqApi(192000.0);
    const CascadeDecoderApi pager = makeAudioApi(22050);
    CascadeIqDecoderApi refusing = makeIqApi(0.0);
    refusing.create = refuseCreate;

    const std::vector<DecoderInfo> cat = {
        {"any.dll", "Any I/Q", PortType::Iq, 0.0},
        {"ais.dll", "AIS", PortType::Iq, 192000.0},
        {"pager.dll", "Pager", PortType::Audio, 22050.0},
        {"refuse.dll", "Refuser", PortType::Iq, 0.0},
    };
    std::vector<PluginApis> apis(4);
    apis[0].iq = &anyIq;
    apis[1].iq = &iq192;
    apis[2].audio = &pager;
    apis[3].iq = &refusing;

    const double chanHz = kCentre + 200000.0;
    const double beat = 3000.0;   // the carrier sits 3 kHz above the channel

    // [P1] AN I/Q DECODER ON A CHANNEL IS HANDED THAT CHANNEL, TUNED. Created
    // with the channel's rate and the CHANNEL's frequency, and what it
    // receives rotates at +3 kHz - the carrier's distance from the channel,
    // not from the radio (which would be 203 kHz, far outside the channel).
    {
        resetLedger();
        IqOnChannel p(chanHz, "any.dll");
        const Plan plan = compile(p.g, kRate, kCentre, &cat);
        CHECK(plan.decoders.size() == 1u);
        Runner r;
        r.publish(buildStripSet(plan, p.g, kRate, kNoNode, 48000.0, &cat, &apis));
        run(r, 200000.0 + beat, 10);
        const Fake* f = lastFake();
        CHECK(f != nullptr);
        if (f != nullptr) {
            CHECK(f->centre == chanHz);
            CHECK(f->rate == 48000.0);
            CHECK(std::fabs(rotationHz(*f) - beat) < 30.0);
            // 10 blocks of 10 ms at 48 kHz: 4800 frames, give or take the
            // filter's start-up.
            CHECK(f->frames >= 4700u && f->frames <= 4800u);
        }
        r.flushNow();
        CHECK(g_ledger.creates == 1);
        CHECK(g_ledger.destroys == 1);
    }

    // [P2] A 192 kHz PLUGIN GETS 192 kHz. The channel runs at 200 kHz for it
    // and a resampler makes up the difference - and the rotation it measures
    // at 192 kHz is still the carrier's +3 kHz, so the resampler kept I and Q
    // in phase and the rate it was told is the rate it got.
    {
        resetLedger();
        IqOnChannel p(chanHz, "ais.dll");
        const Plan plan = compile(p.g, kRate, kCentre, &cat);
        Runner r;
        r.publish(buildStripSet(plan, p.g, kRate, kNoNode, 48000.0, &cat, &apis));
        run(r, 200000.0 + beat, 10);
        const Fake* f = lastFake();
        CHECK(f != nullptr);
        if (f != nullptr) {
            CHECK(f->rate == 192000.0);
            CHECK(f->centre == chanHz);
            CHECK(std::fabs(rotationHz(*f) - beat) < 30.0);
            // 100 ms at 192 kHz, within the resampler's start-up.
            CHECK(f->frames >= 19000u && f->frames <= 19200u);
        }
        r.flushNow();
    }

    // [P3] An I/Q decoder on the RADIO gets the whole capture at the radio's
    // centre - every sample of every block, untouched.
    {
        resetLedger();
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId dec = g.addNode(NodeKind::Decoder, "Wide", PortType::Iq);
        g.mutableNode(dec)->plugin = "any.dll";
        CHECK(g.connect(radio, 0, dec, 0) == Connect::Ok);
        const Plan plan = compile(g, kRate, kCentre, &cat);
        Runner r;
        r.publish(buildStripSet(plan, g, kRate, kNoNode, 48000.0, &cat, &apis));
        run(r, 200000.0, 3);
        const Fake* f = lastFake();
        CHECK(f != nullptr);
        if (f != nullptr) {
            CHECK(f->rate == kRate);
            CHECK(f->centre == kCentre);
            CHECK(f->frames == 3u * 24000u);
            CHECK(std::fabs(rotationHz(*f) - 200000.0) < 50.0);   // untuned: the full offset
        }
        r.flushNow();
    }

    // [P4] AN AUDIO DECODER BEHIND A DEMODULATOR hears that channel's audio,
    // at the rate it asked for: the 1 kHz modulation is in what it gets.
    {
        resetLedger();
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId chan = g.addNode(NodeKind::Channel, "Tower", PortType::Iq);
        const NodeId dm = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
        const NodeId dec = g.addNode(NodeKind::Decoder, "Pager", PortType::Audio);
        g.mutableNode(chan)->freqHz = chanHz;
        g.mutableNode(dm)->mode = 2;   // AM
        g.mutableNode(dec)->plugin = "pager.dll";
        CHECK(g.connect(radio, 0, chan, 0) == Connect::Ok);
        CHECK(g.connect(chan, 0, dm, 0) == Connect::Ok);
        CHECK(g.connect(dm, 0, dec, 0) == Connect::Ok);
        const Plan plan = compile(g, kRate, kCentre, &cat);
        CHECK(plan.decoders.size() == 1u);
        Runner r;
        r.publish(buildStripSet(plan, g, kRate, kNoNode, 48000.0, &cat, &apis));
        run(r, 200000.0, 20);
        const Fake* f = lastFake();
        CHECK(f != nullptr);
        if (f != nullptr) {
            CHECK(f->rate == 22050.0);
            // 200 ms at 22.05 kHz.
            CHECK(f->frames >= 4300u && f->frames <= 4420u);
            const double corr = (f->sv > 0.0)
                                    ? std::sqrt(f->sc * f->sc + f->ss * f->ss) /
                                          std::sqrt(f->sv * static_cast<double>(f->frames) * 0.5)
                                    : 0.0;
            CHECK(corr > 0.5);   // the tone, not noise or silence
        }
        r.flushNow();
    }

    // [P5] TEXT: lines split across polls are joined, a carriage return is
    // dropped, and each line arrives tagged with the NODE's name and id.
    {
        resetLedger();
        IqOnChannel p(chanHz, "any.dll");
        const Plan plan = compile(p.g, kRate, kCentre, &cat);
        Runner r;
        r.publish(buildStripSet(plan, p.g, kRate, kNoNode, 48000.0, &cat, &apis));
        run(r, 200000.0, 1);
        Fake* f = lastFake();
        CHECK(f != nullptr);
        if (f != nullptr) { f->pending = "first line\r\nsecond "; }
        run(r, 200000.0, 1);
        if (f != nullptr) { f->pending = "half\n"; }
        run(r, 200000.0, 1);
        const std::vector<PatchLine> lines = r.drainText();
        CHECK(lines.size() == 2u);
        if (lines.size() == 2u) {
            CHECK(lines[0].text == "first line");
            CHECK(lines[1].text == "second half");
            CHECK(lines[0].source == "My decoder");
            CHECK(lines[0].node == p.dec);
        }
        CHECK(r.drainText().empty());   // drained means drained
        r.flushNow();
    }

    // [P6] A DECODER THAT FAILS PERMANENTLY is fed and polled no further -
    // and is still destroyed exactly once, when its set dies.
    {
        resetLedger();
        IqOnChannel p(chanHz, "any.dll");
        const Plan plan = compile(p.g, kRate, kCentre, &cat);
        Runner r;
        r.publish(buildStripSet(plan, p.g, kRate, kNoNode, 48000.0, &cat, &apis));
        run(r, 200000.0, 2);
        Fake* f = lastFake();
        CHECK(f != nullptr);
        if (f != nullptr) {
            f->failNow = true;
            run(r, 200000.0, 1);             // the poll that reports it
            const std::uint64_t frozenFrames = f->frames;
            const int frozenPolls = f->polls;
            run(r, 200000.0, 5);
            CHECK(f->frames == frozenFrames);
            CHECK(f->polls == frozenPolls);
        }
        r.flushNow();
        CHECK(g_ledger.creates == 1);
        CHECK(g_ledger.destroys == 1);
    }

    // [P7] A PLUGIN THAT REFUSES TO START is recorded against its node, never
    // destroyed (it has no handle), and does not stop its neighbour running.
    {
        resetLedger();
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId bad = g.addNode(NodeKind::Decoder, "Refuser", PortType::Iq);
        const NodeId good = g.addNode(NodeKind::Decoder, "Fine", PortType::Iq);
        g.mutableNode(bad)->plugin = "refuse.dll";
        g.mutableNode(good)->plugin = "any.dll";
        CHECK(g.connect(radio, 0, bad, 0) == Connect::Ok);
        CHECK(g.connect(radio, 0, good, 0) == Connect::Ok);
        const Plan plan = compile(g, kRate, kCentre, &cat);
        auto set = buildStripSet(plan, g, kRate, kNoNode, 48000.0, &cat, &apis);
        CHECK(set->refused.size() == 1u);
        CHECK(!set->refused.empty() && set->refused[0] == bad);
        CHECK(set->decoders.size() == 1u);
        CHECK(g_ledger.creates == 1);   // the refuser's create returned no instance
        Runner r;
        // MOVED, as the app hands it over: a copy kept here would keep the
        // set - and its plugin instance - alive past flushNow(), which is
        // this test holding a reference, not the runner leaking one.
        r.publish(std::move(set));
        run(r, 200000.0, 2);
        const Fake* f = lastFake();
        CHECK(f != nullptr && f->frames == 2u * 24000u);
        r.flushNow();
        CHECK(g_ledger.destroys == 1);
    }

    // [P8] THE LIFETIME RULE WITH A REAL DSP THREAD. The GUI side publishes a
    // fresh set of decoders again and again, reaping as it goes, while the DSP
    // side runs flat out. Every instance created is destroyed exactly once,
    // and EVERY destroy runs on the GUI thread - never on the audio thread.
    {
        resetLedger();
        IqOnChannel p(chanHz, "any.dll");
        const Plan plan = compile(p.g, kRate, kCentre, &cat);
        Runner r;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> blocks{0};
        std::thread::id dspId;
        std::thread dsp([&] {
            const auto sig = signal(200000.0, 2400);
            while (!stop.load(std::memory_order_relaxed)) {
                r.process(sig.data(), sig.size());
                blocks.fetch_add(1, std::memory_order_relaxed);
            }
        });
        dspId = dsp.get_id();
        for (int i = 0; i < 150; ++i) {
            r.publish(buildStripSet(plan, p.g, kRate, kNoNode, 48000.0, &cat, &apis));
            if (i % 5 == 0) { r.clear(); }
            std::this_thread::yield();
            r.reap();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        r.flushNow();
        stop.store(true, std::memory_order_relaxed);
        dsp.join();

        CHECK(blocks.load() > 0u);
        CHECK(g_ledger.creates == 150);
        CHECK(g_ledger.destroys == 150);
        bool anyOnDsp = false;
        for (const std::thread::id t : g_ledger.destroyThreads) {
            if (t == dspId) { anyOnDsp = true; }
        }
        CHECK(!anyOnDsp);
        CHECK(g_ledger.live.empty());
    }

    // [P9] THE TEXT QUEUE IS BOUNDED. A decoder chattering to a GUI that is
    // not draining loses lines and COUNTS them, rather than growing a queue on
    // the audio thread forever. And an endless line is capped, not grown.
    {
        resetLedger();
        IqOnChannel p(chanHz, "any.dll");
        const Plan plan = compile(p.g, kRate, kCentre, &cat);
        Runner r;
        r.publish(buildStripSet(plan, p.g, kRate, kNoNode, 48000.0, &cat, &apis));
        run(r, 200000.0, 1, 2400);
        Fake* f = lastFake();
        CHECK(f != nullptr);
        if (f != nullptr) {
            for (int b = 0; b < 300; ++b) {
                f->pending = "a\nb\nc\nd\ne\n";   // five lines a block
                run(r, 200000.0, 1, 2400);
            }
            const std::vector<PatchLine> lines = r.drainText();
            CHECK(lines.size() == Runner::kMaxPendingLines);
            CHECK(r.droppedLines() == 1500u - Runner::kMaxPendingLines);

            f->pending.assign(20000, 'x');         // no newline, ever
            for (int b = 0; b < 30; ++b) { run(r, 200000.0, 1, 2400); }
            f->pending = "\n";
            run(r, 200000.0, 1, 2400);
            const std::vector<PatchLine> longOne = r.drainText();
            CHECK(longOne.size() == 1u);
            CHECK(!longOne.empty() &&
                  longOne[0].text.size() == cascade::core::patch::kMaxLineBytes);
        }
        r.flushNow();
    }

    // [P10] flushNow() with decoders alive leaves nothing alive - the promise
    // the plugin host's unload depends on.
    {
        resetLedger();
        IqOnChannel a(chanHz, "any.dll");
        IqOnChannel b(chanHz, "ais.dll");
        Runner r;
        r.publish(buildStripSet(compile(a.g, kRate, kCentre, &cat), a.g, kRate, kNoNode,
                                48000.0, &cat, &apis));
        run(r, 200000.0, 1);
        r.publish(buildStripSet(compile(b.g, kRate, kCentre, &cat), b.g, kRate, kNoNode,
                                48000.0, &cat, &apis));
        run(r, 200000.0, 1);                         // retires a's set
        r.publish(buildStripSet(compile(a.g, kRate, kCentre, &cat), a.g, kRate, kNoNode,
                                48000.0, &cat, &apis));   // pending, never adopted
        CHECK(g_ledger.creates == 3);
        CHECK(g_ledger.live.size() == 3u);           // running + retired + pending
        r.flushNow();
        CHECK(g_ledger.live.empty());
        CHECK(g_ledger.destroys == 3);
    }

    // [P11] WHAT REPUBLISHES AND WHAT DOES NOT. Moving, resizing or renaming a
    // CHANNEL changes nothing the engine reads, so the signature holds and no
    // plugin is restarted mid-decode. Everything the engine does read moves
    // it: a frequency, a mode, a plugin, a wire, the decoder's own name (it
    // tags the text), and the plugin's table address after a rescan.
    {
        using cascade::core::patch::dspSignature;
        IqOnChannel p(chanHz, "any.dll");
        const auto sig = [&](const Graph& g, const std::vector<PluginApis>& a) {
            return dspSignature(compile(g, kRate, kCentre, &cat), g, kRate, kNoNode, 48000.0,
                                &cat, &a);
        };
        const std::string base = sig(p.g, apis);

        Graph moved = p.g;
        moved.mutableNode(p.chan)->x += 300.0f;
        moved.mutableNode(p.dec)->y += 120.0f;
        moved.mutableNode(p.dec)->w = 500.0f;
        moved.mutableNode(p.chan)->name = "Renamed channel";
        CHECK(sig(moved, apis) == base);

        Graph freq = p.g;
        freq.mutableNode(p.chan)->freqHz += 12500.0;
        CHECK(sig(freq, apis) != base);

        Graph plugin = p.g;
        plugin.mutableNode(p.dec)->plugin = "ais.dll";
        CHECK(sig(plugin, apis) != base);

        Graph renamed = p.g;
        renamed.mutableNode(p.dec)->name = "Another tag";
        CHECK(sig(renamed, apis) != base);

        Graph unwired = p.g;
        unwired.disconnect(cascade::core::patch::Wire{p.chan, 0, p.dec, 0});
        CHECK(sig(unwired, apis) != base);

        // A demodulator's mode on a channel.
        Graph withDemod = p.g;
        const NodeId dm = withDemod.addNode(NodeKind::Demod, "FM", PortType::Iq);
        withDemod.mutableNode(dm)->mode = 0;
        CHECK(withDemod.connect(p.chan, 0, dm, 0) == Connect::Ok);
        const std::string fm = sig(withDemod, apis);
        withDemod.mutableNode(dm)->mode = 2;
        CHECK(sig(withDemod, apis) != fm);

        // The same plugin reloaded at a different address.
        const CascadeIqDecoderApi reloaded = makeIqApi(0.0);
        std::vector<PluginApis> apis2 = apis;
        apis2[0].iq = &reloaded;
        CHECK(sig(p.g, apis2) != base);
    }

    return testSummary("test_patch_decoders");
}
