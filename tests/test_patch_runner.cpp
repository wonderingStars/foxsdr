// Tests for core/patch_runner.hpp - swapping a patch while it is running.
//
// The DSP is already tested in test_patch_strip. What is tested here is the
// hand-off: the GUI thread replaces the set of strips while the DSP thread is
// running them, and the DSP thread must never see a half-built set, never
// block on the GUI thread, and never miss the change.
//
// A SINGLE-THREADED TEST CANNOT PROVE THAT, so the last test really does run
// two threads: one calling process() as fast as it can while the other
// publishes hundreds of sets. It cannot prove the absence of a race - nothing
// can - but it fails loudly under a thread sanitiser or a debug allocator, and
// it would catch the obvious version of getting this wrong, which is adopting
// a pointer that the publisher is still writing through.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_runner.hpp"

#include <atomic>
#include <cmath>
#include <complex>
#include <thread>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "test_check.hpp"

using cascade::core::patch::buildStripSet;
using cascade::core::patch::compile;
using cascade::core::patch::Connect;
using cascade::core::patch::Demod;
using cascade::core::patch::Graph;
using cascade::core::patch::kMaxBlockAudio;
using cascade::core::patch::kNoNode;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::Plan;
using cascade::core::patch::PortType;
using cascade::core::patch::Runner;
using cascade::core::patch::StripSet;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 2400000.0;
constexpr double kCentre = 100000000.0;

std::vector<std::complex<float>> tone(double offsetHz, std::size_t n) {
    std::vector<std::complex<float>> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double env = 1.0 + 0.5 * std::sin(2.0 * kPi * 1000.0 * t);
        const double ph = 2.0 * kPi * offsetHz * t;
        out[i] = std::complex<float>(static_cast<float>(env * std::cos(ph)),
                                     static_cast<float>(env * std::sin(ph)));
    }
    return out;
}

// A radio feeding `count` channels, each on its own frequency, all into one
// text sink so the plan is runnable.
struct Patch {
    Graph g;
    NodeId radio = kNoNode;
    std::vector<NodeId> channels;

    explicit Patch(int count) {
        radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId sink = g.addNode(NodeKind::Sink, "Text", PortType::Text);
        for (int i = 0; i < count; ++i) {
            const NodeId ch = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
            const NodeId dec = g.addNode(NodeKind::Decoder, "d", PortType::Iq);
            g.mutableNode(ch)->freqHz = kCentre + 100000.0 * (i + 1);
            g.connect(radio, 0, ch, 0);
            g.connect(ch, 0, dec, 0);
            g.connect(dec, 0, sink, 0);
            channels.push_back(ch);
        }
    }

    std::shared_ptr<StripSet> build(NodeId listening = kNoNode) const {
        return buildStripSet(compile(g, kRate, kCentre), g, kRate, listening);
    }
};

double followsTone(const std::vector<float>& v, double toneHz, double rate) {
    if (v.size() < 8) { return 0.0; }
    double sc = 0.0;
    double ss = 0.0;
    double sv = 0.0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double ref = std::sin(2.0 * kPi * toneHz * t);
        sc += static_cast<double>(v[i]) * ref;
        ss += ref * ref;
        sv += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    }
    return (ss > 0.0 && sv > 0.0) ? std::fabs(sc) / std::sqrt(ss * sv) : 0.0;
}

double rms(const float* p, std::size_t n) {
    if (p == nullptr || n == 0) { return 0.0; }
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) { s += static_cast<double>(p[i]) * p[i]; }
    return std::sqrt(s / static_cast<double>(n));
}

}  // namespace

int main() {
    // [1] A runner with nothing published runs nothing, and says so rather
    // than pretending.
    {
        Runner r;
        CHECK(!r.running());
        CHECK(r.channelCount() == 0u);
        CHECK(r.generation() == 0u);

        const auto sig = tone(100000.0, 4800);
        r.process(sig.data(), sig.size());   // must not crash
        std::size_t n = 999;
        CHECK(r.listeningAudio(n) == nullptr);
        CHECK(n == 0u);
    }

    // [2] Publishing does NOT take effect until the DSP thread adopts. This is
    // the whole contract: the GUI thread hands over and walks away.
    {
        Patch p(2);
        Runner r;
        r.publish(p.build());
        CHECK(!r.running());          // nothing adopted yet
        CHECK(r.generation() == 0u);

        CHECK(r.adopt());
        CHECK(r.running());
        CHECK(r.channelCount() == 2u);
        CHECK(r.generation() == 1u);

        // Adopting again with nothing pending is a no-op, not a re-adopt.
        CHECK(!r.adopt());
        CHECK(r.generation() == 1u);

        // AND IT COSTS NOTHING. The promise this file is built on is that
        // the DSP thread does not take the publish mutex unless a patch
        // actually changed - otherwise it can be made to wait on a GUI
        // thread that is allocating. One publish, one lock, however many
        // blocks follow.
        const std::uint64_t locksAfterOne = r.lockCount();
        CHECK(locksAfterOne == 1u);
        const auto quiet = tone(100000.0, 480);
        for (int i = 0; i < 200; ++i) { r.process(quiet.data(), quiet.size()); }
        CHECK(r.lockCount() == locksAfterOne);
        CHECK(r.generation() == 1u);
    }

    // [3] process() adopts for you, so a patch published between blocks takes
    // effect at a block boundary and never halfway through one.
    {
        Patch p(1);
        Runner r;
        r.publish(p.build());
        const auto sig = tone(100000.0, 4800);
        r.process(sig.data(), sig.size());
        CHECK(r.running());
        CHECK(r.generation() == 1u);
    }

    // [4] The channel the patch is listening to produces audio, and the one it
    // is not produces none - at the speaker, that is.
    {
        Patch p(2);
        Runner r;
        r.publish(p.build(p.channels[0]));

        // The signal is on channel 0's frequency.
        const auto sig = tone(100000.0, 48000);
        r.process(sig.data(), sig.size());

        std::size_t n = 0;
        const float* audio = r.listeningAudio(n);
        CHECK(audio != nullptr);
        CHECK(n > 0u);
        CHECK(rms(audio, n) > 0.1);

        // Listening to the OTHER channel, the same input is near silent -
        // which is the filter doing its job, seen through the runner.
        Runner other;
        other.publish(p.build(p.channels[1]));
        other.process(sig.data(), sig.size());
        std::size_t m = 0;
        const float* quiet = other.listeningAudio(m);
        CHECK(quiet != nullptr);
        CHECK(m > 0u);
        CHECK(rms(quiet, m) < rms(audio, n) / 20.0);
    }

    // [5] Listening to nothing is a legal state - a patch of displays with no
    // speaker - and must not be mistaken for a fault.
    {
        Patch p(1);
        Runner r;
        r.publish(p.build(kNoNode));
        const auto sig = tone(100000.0, 4800);
        r.process(sig.data(), sig.size());
        CHECK(r.running());
        std::size_t n = 999;
        CHECK(r.listeningAudio(n) == nullptr);
        CHECK(n == 0u);
    }

    // [6] Listening to a node that is not in the set answers nothing rather
    // than the first channel it finds. A stale id is exactly what a rewire
    // leaves behind for one block.
    {
        Patch p(2);
        Runner r;
        r.publish(p.build(9999u));
        const auto sig = tone(100000.0, 4800);
        r.process(sig.data(), sig.size());
        std::size_t n = 999;
        CHECK(r.listeningAudio(n) == nullptr);
        CHECK(n == 0u);
    }

    // [7] A block bigger than the per-channel buffer loses samples rather than
    // growing a vector inside the callback or writing past the end.
    {
        Patch p(1);
        Runner r;
        r.publish(p.build(p.channels[0]));

        // At decimation 50 this is well over kMaxBlockAudio outputs.
        const std::size_t huge = (kMaxBlockAudio + 1000) * 50;
        const auto sig = tone(100000.0, huge);
        r.process(sig.data(), sig.size());

        std::size_t n = 0;
        CHECK(r.listeningAudio(n) != nullptr);
        CHECK(n == kMaxBlockAudio);
    }

    // [8] Replacing a running patch keeps running, with the NEW channels.
    {
        Patch small(1);
        Patch big(3);
        Runner r;
        const auto sig = tone(100000.0, 4800);

        r.publish(small.build());
        r.process(sig.data(), sig.size());
        CHECK(r.channelCount() == 1u);
        CHECK(r.generation() == 1u);

        r.publish(big.build());
        r.process(sig.data(), sig.size());
        CHECK(r.channelCount() == 3u);
        CHECK(r.generation() == 2u);
    }

    // [9] Publishing twice before a single adopt takes the LAST one. A user
    // dragging a wire publishes on every frame and the DSP thread must not
    // work through a queue of stale patches to catch up.
    {
        Patch one(1);
        Patch four(4);
        Runner r;
        r.publish(one.build());
        r.publish(four.build());
        CHECK(r.adopt());
        CHECK(r.channelCount() == 4u);
        CHECK(r.generation() == 1u);   // ONE adoption, not two
        CHECK(!r.adopt());
    }

    // [10] clear() forgets everything, including anything pending. A stopped
    // receiver must not leave a patch holding buffers that go stale.
    {
        Patch p(2);
        Runner r;
        r.publish(p.build());
        CHECK(r.adopt());
        r.publish(p.build());          // something pending as well
        r.clear();
        CHECK(!r.running());
        CHECK(r.channelCount() == 0u);
        CHECK(!r.adopt());             // the pending one went too
    }

    // [11] THE HAND-OFF, WITH TWO REAL THREADS. One runs process() as fast as
    // it can while the other publishes hundreds of sets of differing size.
    //
    // This cannot prove the absence of a race, and it is not claimed to. What
    // it does is exercise the path under a real scheduler, so the obvious way
    // of getting this wrong - adopting a pointer the publisher is still
    // writing through - fails here loudly rather than once a month on a user's
    // machine. The assertions at the end are the honest ones: every publish
    // was eventually seen, and the runner is left in a usable state.
    {
        Patch a(1);
        Patch b(5);
        Runner r;
        const auto sig = tone(100000.0, 2400);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> blocks{0};

        std::thread dsp([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                r.process(sig.data(), sig.size());
                blocks.fetch_add(1, std::memory_order_relaxed);
                std::size_t n = 0;
                const float* audio = r.listeningAudio(n);
                // Reading it is the point: a torn set shows up here.
                if (audio != nullptr && n > 0) { volatile float sink = audio[n - 1]; (void)sink; }
            }
        });

        constexpr int kPublishes = 400;
        for (int i = 0; i < kPublishes; ++i) {
            r.publish((i % 2 == 0) ? a.build(a.channels[0]) : b.build(b.channels[2]));
            std::this_thread::yield();
        }
        // Let the DSP thread catch up with the last one.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true, std::memory_order_relaxed);
        dsp.join();

        CHECK(blocks.load() > 0u);
        CHECK(r.running());
        // Every publish was either adopted or superseded by a later one, so
        // the count is at least one and never more than the number published.
        CHECK(r.generation() >= 1u);
        CHECK(r.generation() <= static_cast<std::uint64_t>(kPublishes));
        // And it still works afterwards.
        r.process(sig.data(), sig.size());
        CHECK(r.channelCount() == 1u || r.channelCount() == 5u);
    }

    // [12] The demodulator each channel gets comes from the Demod node it
    // feeds - and a channel feeding no demodulator takes AM rather than
    // guessing, because a wrong demodulator is silence, not a worse version
    // of the right one.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId spk = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
        const NodeId ch = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
        const NodeId dm = g.addNode(NodeKind::Demod, "NFM", PortType::Iq);
        g.mutableNode(ch)->freqHz = kCentre + 100000.0;
        g.mutableNode(dm)->mode = 0;     // NFM, in the host's mode order
        CHECK(g.connect(radio, 0, ch, 0) == Connect::Ok);
        CHECK(g.connect(ch, 0, dm, 0) == Connect::Ok);
        CHECK(g.connect(dm, 0, spk, 0) == Connect::Ok);

        const auto set = buildStripSet(compile(g, kRate, kCentre), g, kRate, ch);
        CHECK(set->channels.size() == 1u);
        CHECK(set->channels[0].mode == Demod::Fm);

        // The same channel with an AM demodulator downstream.
        g.mutableNode(dm)->mode = 2;     // AM
        const auto am = buildStripSet(compile(g, kRate, kCentre), g, kRate, ch);
        CHECK(am->channels[0].mode == Demod::Am);

        // And with no demodulator at all.
        Graph bare;
        const NodeId r2 = bare.addNode(NodeKind::Radio, "R", PortType::Iq);
        const NodeId c2 = bare.addNode(NodeKind::Channel, "c", PortType::Iq);
        const NodeId d2 = bare.addNode(NodeKind::Display, "Spectrum", PortType::Iq);
        bare.mutableNode(c2)->freqHz = kCentre;
        CHECK(bare.connect(r2, 0, c2, 0) == Connect::Ok);
        CHECK(bare.connect(c2, 0, d2, 0) == Connect::Ok);
        const auto none = buildStripSet(compile(bare, kRate, kCentre), bare, kRate, kNoNode);
        CHECK(none->channels.size() == 1u);
        CHECK(none->channels[0].mode == Demod::Am);
    }

    // [13] pullAudio answers FALSE when no patch is listening, and leaves the
    // buffers alone. That is the contract the pipeline depends on: false means
    // "keep your demodulated audio", so writing anything here would silence a
    // receiver that is working perfectly.
    {
        Patch p(1);
        Runner r;
        r.publish(p.build(kNoNode));           // running, but listening to nothing
        const auto sig = tone(100000.0, 4800);
        r.process(sig.data(), sig.size());

        float l[64];
        float rr[64];
        for (int i = 0; i < 64; ++i) { l[i] = 1234.0f; rr[i] = 5678.0f; }
        CHECK(!r.pullAudio(l, rr, 64));
        CHECK(l[0] == 1234.0f);                // untouched
        CHECK(rr[63] == 5678.0f);

        // ...and likewise with nothing published at all.
        Runner empty;
        CHECK(!empty.pullAudio(l, rr, 64));
        CHECK(l[0] == 1234.0f);
    }

    // [14] Listening to a channel gives audio at the SINK's rate, carrying the
    // tone that was transmitted.
    //
    // The rates really do differ: at 2.4 MS/s a channel runs at 48000 exactly,
    // so this test uses a device rate that does NOT divide to it, and the
    // resampler is what makes the difference invisible.
    {
        Patch p(1);
        Runner r;
        r.publish(p.build(p.channels[0]));
        CHECK(r.adopt());
        // The strip is not at the sink's rate, which is the point.
        CHECK(r.bufferedFrames() == 0u);

        const auto sig = tone(100000.0, 240000);   // 0.1 s at 2.4 MS/s
        r.process(sig.data(), sig.size());
        CHECK(r.bufferedFrames() > 0u);

        std::vector<float> l(2048, 0.0f);
        std::vector<float> rr(2048, 0.0f);
        CHECK(r.pullAudio(l.data(), rr.data(), l.size()));
        CHECK(rms(l.data(), l.size()) > 0.05);
        // Mono on both channels, identically.
        bool sameBoth = true;
        for (std::size_t i = 0; i < l.size(); ++i) {
            if (l[i] != rr[i]) { sameBoth = false; }
        }
        CHECK(sameBoth);

        // It is the 1 kHz tone the signal was modulated with, judged at the
        // SINK's rate - which is the one thing that proves the resampler is
        // doing its job rather than merely producing samples.
        CHECK(followsTone(l, 1000.0, 48000.0) > 0.7);
    }

    // [15] A patch with nothing ready hands out SILENCE and still says true.
    // Handing back to the demodulator instead would make a momentarily starved
    // patch chatter between two sources, which is worse than a gap and much
    // harder to diagnose.
    {
        Patch p(1);
        Runner r;
        r.publish(p.build(p.channels[0]));
        CHECK(r.adopt());
        CHECK(r.starvedFrames() == 0u);

        std::vector<float> l(512, 9.0f);
        std::vector<float> rr(512, 9.0f);
        CHECK(r.pullAudio(l.data(), rr.data(), l.size()));   // nothing produced yet
        CHECK(rms(l.data(), l.size()) == 0.0);               // silence, not 9
        CHECK(r.starvedFrames() == 512u);
    }

    // [16] A rewire throws the buffered audio away. What is in the ring is at
    // the old channel's rate and from the old channel's frequency; playing it
    // after the patch changed is playing the patch the user just replaced.
    {
        Patch a(1);
        Patch b(2);
        Runner r;
        r.publish(a.build(a.channels[0]));
        const auto sig = tone(100000.0, 240000);
        r.process(sig.data(), sig.size());
        CHECK(r.bufferedFrames() > 0u);

        r.publish(b.build(b.channels[1]));
        CHECK(r.adopt());
        CHECK(r.bufferedFrames() == 0u);
    }

    // [17] The ring drops the OLDEST when it overflows. A full ring means the
    // sink is behind, and the samples worth keeping are the ones about to be
    // played rather than the ones that went stale a second ago.
    {
        Patch p(1);
        Runner r;
        r.publish(p.build(p.channels[0]));
        CHECK(r.adopt());

        // Well over a second of audio without ever pulling any.
        const auto sig = tone(100000.0, 2400000);   // 1 s at 2.4 MS/s
        for (int i = 0; i < 3; ++i) { r.process(sig.data(), sig.size()); }

        // Bounded, not grown: the ring is a second at the sink's rate.
        CHECK(r.bufferedFrames() <= 48000u);
        CHECK(r.bufferedFrames() > 1000u);

        // And what comes out is still audio rather than a torn join.
        std::vector<float> l(4096, 0.0f);
        std::vector<float> rr(4096, 0.0f);
        CHECK(r.pullAudio(l.data(), rr.data(), l.size()));
        CHECK(rms(l.data(), l.size()) > 0.01);
    }

    // [18] clear() empties the ring too, so a stopped receiver does not resume
    // by playing a second of what it heard before it stopped.
    {
        Patch p(1);
        Runner r;
        r.publish(p.build(p.channels[0]));
        const auto sig = tone(100000.0, 240000);
        r.process(sig.data(), sig.size());
        CHECK(r.bufferedFrames() > 0u);
        r.clear();
        CHECK(r.bufferedFrames() == 0u);

        float l[16];
        float rr[16];
        CHECK(!r.pullAudio(l, rr, 16));   // nothing running any more
    }

    return testSummary("test_patch_runner");
}
