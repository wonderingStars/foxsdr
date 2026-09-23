// patch_runner.hpp - the strips a patch is currently running, and the hand-off
// that lets them be replaced while audio is playing.
//
// THIS FILE IS ABOUT ONE PROBLEM AND IT IS NOT DSP. patch_strip.hpp already
// demodulates a channel correctly. What is left is that the user rewires the
// patch on the GUI thread while the DSP thread is halfway through a block, and
// the set of strips has to change without the DSP thread ever seeing a
// half-built one, taking a lock, or allocating.
//
// THE RULE, and everything here follows from it:
//
//   THE GUI THREAD BUILDS. THE DSP THREAD ADOPTS AND OWNS.
//
// publish() is called from the GUI thread with a set that is already complete -
// every vector sized, every filter designed, every allocation done. It stores
// the pointer and returns. The DSP thread calls adopt() at the top of its
// block; if something was published it takes it, and from that instant it is
// the only thread that touches it. The GUI thread never reads or writes a set
// it has published, so there is no sharing to synchronise beyond the pointer
// itself.
//
// WHY NOT A MUTEX. A lock held on the audio thread is a lock the GUI thread can
// make it wait on, and a GUI thread that is swapping a patch is doing
// allocation and file I/O. This product has already shipped one hang whose
// stack was the GUI thread blocked in a write; the audio thread must never be
// able to join that queue.
//
// WHY NOT REBUILD ON THE DSP THREAD. It is simpler to pass the plan across and
// build the strips where they are used - and it puts a filter design and half
// a dozen allocations inside the audio callback, which is the definition of a
// dropout. The build belongs where a pause is invisible.
//
// EVERY SET DIES ON THE GUI THREAD. A set the DSP thread stops running -
// because a newer one was adopted, or because clear() asked it to stop - is
// not destroyed there. It is handed back through a retired list, and reap(),
// called by the GUI thread every frame, destroys it. A set that was published
// and superseded before it was ever adopted is released inside publish() or
// clear(), which are GUI-thread calls anyway. So no set's destructor ever runs
// on the audio thread, which is what lets a set own things that must be closed
// on the control thread - a plugin's decoder handle, whose destroy() the ABI
// requires there and after the last process().
//
// clear() IS A REQUEST. Before 0.99.15 it reset the running set directly, and
// it is called from the GUI thread when the patch page closes - so closing the
// page with a patch playing freed the strips the DSP thread was inside. The
// runner test reproduced that as a segfault on every run.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_RUNNER_HPP
#define CASCADE_CORE_PATCH_RUNNER_HPP

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/plugin_abi.h"
#include "core/host_image.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_strip.hpp"
#include "dsp/resampler.hpp"

namespace cascade::core::patch {

// One running channel: the strip, what it is demodulating, and where its audio
// goes. `audio` is preallocated so process() never grows it on the DSP thread.
struct RunningChannel {
    NodeId node = kNoNode;
    Strip strip;
    Demod mode = Demod::Am;
    std::vector<float> audio;
    std::size_t produced = 0;   // samples written into `audio` this block
    // The channel itself, tuned and decimated, for an I/Q decoder hung off
    // it. Filled only when one is (`wantIq`), because copying baseband nobody
    // reads is work on the audio thread for nothing.
    bool wantIq = false;
    std::vector<std::complex<float>> iq;
};

// --- plugin decoders -----------------------------------------------------------
//
// What the builder needs to call one catalogue entry: its API table, exactly
// one of the two set. Parallel to the DecoderInfo list compile() was given -
// index i here is index i there. Pointers into a loaded module, so they are
// valid only until the plugin host unloads it, and Runner::flushNow() must run
// before it does.
struct PluginApis {
    const CascadeDecoderApi* audio = nullptr;
    const CascadeIqDecoderApi* iq = nullptr;
    const CascadeImageDecoderApi* image = nullptr;
};

// The largest block a decoder is handed in one call. A radio-fed decoder sees
// whole device blocks, which can be large; slicing them keeps every scratch
// buffer a fixed size allocated at build time.
inline constexpr std::size_t kDecoderSlice = 16384;

// One running plugin instance.
//
// ITS HANDLE IS DESTROYED IN ITS DESTRUCTOR, and that is safe only because of
// where the destructor runs: a RunningDecoder lives inside a StripSet, and a
// StripSet only ever dies on the GUI thread, after the DSP thread has stopped
// running it (see the note at the top of this file). That is precisely the
// ABI's rule - destroy() on the control thread, after the last process() and
// poll_text() - so the lifetime rules of the set ARE the plugin's lifetime
// rules, and nothing here has to remember to call destroy().
struct RunningDecoder {
    NodeId node = kNoNode;
    NodeId channel = kNoNode;
    DecoderSource source = DecoderSource::Radio;
    std::string name;                     // the node's name, which tags its text
    const CascadeDecoderApi* audioApi = nullptr;
    const CascadeIqDecoderApi* iqApi = nullptr;
    const CascadeImageDecoderApi* imageApi = nullptr;
    void* handle = nullptr;
    // PICTURES: polled on the DSP thread (poll_image must be serialised with
    // process() on the same instance, and this path takes no lock), no more
    // often than every kImagePollFrames of input, copied out and released at
    // once. `rateHz` sizes that interval; `lastImagePoll` is when it last ran.
    double rateHz = 0.0;
    std::uint64_t lastImagePoll = 0;
    std::uint64_t imageRevision = 0;
    bool failed = false;                  // poll_text said "permanently failed"

    // Resampling from the source's rate to the plugin's. Absent when they
    // match. An I/Q stream needs two - I and Q through identical filters, so
    // they stay in phase - and an audio stream one.
    std::unique_ptr<cascade::dsp::RationalResampler> rsI;
    std::unique_ptr<cascade::dsp::RationalResampler> rsQ;
    std::vector<float> inI, inQ, outI, outQ;   // one slice each, preallocated
    std::vector<float> interleaved;            // I,Q,I,Q for process()

    std::string partial;                  // a line poll_text split across polls
    std::uint64_t framesFed = 0;          // what process() has been handed
    // Index into StripSet::channels of the channel this is fed from, fixed at
    // build time so the audio thread does not search for it every block.
    std::size_t chanIndex = static_cast<std::size_t>(-1);

    RunningDecoder() = default;
    RunningDecoder(const RunningDecoder&) = delete;
    RunningDecoder& operator=(const RunningDecoder&) = delete;
    ~RunningDecoder() {
        if (handle == nullptr) { return; }
        if (iqApi != nullptr) {
            iqApi->destroy(handle);
        } else if (audioApi != nullptr) {
            audioApi->destroy(handle);
        } else if (imageApi != nullptr) {
            // No borrow can be outstanding: every successful poll_image is
            // released before pollImage() returns, as the ABI requires before
            // destroy().
            imageApi->destroy(handle);
        }
    }
};

// A decoded line on its way to the GUI, tagged with the node that said it.
struct PatchLine {
    NodeId node = kNoNode;
    std::string source;
    std::string text;
};

// Everything the DSP thread needs to run a patch. Built complete on the GUI
// thread; touched by exactly one thread at a time thereafter.
struct StripSet {
    std::vector<RunningChannel> channels;
    // Behind unique_ptr because a RunningDecoder owns a plugin handle and
    // must never be copied or moved-from in a way that could destroy it twice.
    std::vector<std::unique_ptr<RunningDecoder>> decoders;
    // Decoder nodes whose plugin was asked to create() an instance and
    // returned NULL. Kept so the canvas can say "the plugin refused to start"
    // rather than showing a node that looks ready and does nothing.
    std::vector<NodeId> refused;

    // THE LISTENING CHANNEL'S CONVERSION TO THE SINK'S RATE. A strip runs
    // at whatever whole division of the device rate lands nearest 48 kHz -
    // 62500 on a 2 MS/s generator, 47628 on an RTL-SDR at 2.048 - and the
    // sink wants exactly 48000, so this is not optional and it is not a
    // rounding. Built here, on the GUI thread, with everything else.
    std::unique_ptr<cascade::dsp::RationalResampler> toAudio;
    std::vector<float> resampled;   // preallocated scratch for one block
    // Which channel's audio reaches the speaker. Exactly one, because mixing
    // two demodulated channels is not defined here - the graph refuses
    // fan-in on samples for the same reason.
    NodeId listening = kNoNode;
};

// The most audio one block can produce per channel. process() writes no more
// than this and reports what it wrote, so a caller that hands over an
// unexpectedly large block loses samples rather than the DSP thread growing a
// vector inside the callback.
inline constexpr std::size_t kMaxBlockAudio = 65536;

// Builds the set a plan describes. GUI THREAD ONLY - it allocates.
// A plugin's text lines are capped at this length. A decoder that never sends a
// newline would otherwise grow `partial` without bound on the audio thread.
inline constexpr std::size_t kMaxLineBytes = 4096;

// Builds the decoder instances a plan describes into `set`. GUI THREAD ONLY:
// it calls create(), which the ABI requires on the control thread, and it
// allocates every buffer the DSP thread will use.
inline void buildDecoders(StripSet& set, const Plan& plan, const Graph& g,
                          const std::vector<DecoderInfo>& catalogue,
                          const std::vector<PluginApis>& apis) {
    for (const DecoderPlan& dp : plan.decoders) {
        if (dp.plugin >= apis.size() || dp.plugin >= catalogue.size()) { continue; }
        const PluginApis& a = apis[dp.plugin];
        const bool iqFeed = dp.source != DecoderSource::Audio;

        auto d = std::make_unique<RunningDecoder>();
        d->node = dp.node;
        d->channel = dp.channel;
        d->source = dp.source;
        const Node* n = g.find(dp.node);
        d->name = (n != nullptr && !n->name.empty()) ? n->name : catalogue[dp.plugin].name;

        if (dp.source != DecoderSource::Radio) {
            for (std::size_t i = 0; i < set.channels.size(); ++i) {
                if (set.channels[i].node == dp.channel) {
                    d->chanIndex = i;
                    break;
                }
            }
            if (d->chanIndex >= set.channels.size()) { continue; }
        }

        const bool resample = std::fabs(dp.rateHz - dp.inRateHz) > 1e-6 * dp.rateHz;
        const unsigned inR = resample ? resampleInputRate(dp.inRateHz, dp.rateHz) : 0u;
        if (resample && inR == 0u) {
            // The plan refuses these, so this is a guard for a caller that
            // built a plan by hand - never a resampler with a zero rate.
            set.refused.push_back(dp.node);
            continue;
        }

        // create() last among the things that can fail, so a node skipped
        // for any other reason never leaves a live handle behind.
        d->rateHz = dp.rateHz;
        if (catalogue[dp.plugin].image) {
            // A picture decoder takes either input through one table; an
            // audio-input one is told centre 0, as the ABI specifies.
            if (a.image == nullptr) {
                set.refused.push_back(dp.node);
                continue;
            }
            d->imageApi = a.image;
            d->handle = a.image->create(dp.rateHz, iqFeed ? dp.centreHz : 0.0);
        } else if (iqFeed) {
            if (a.iq == nullptr) {
                set.refused.push_back(dp.node);
                continue;
            }
            d->iqApi = a.iq;
            d->handle = a.iq->create(dp.rateHz, dp.centreHz);
        } else {
            if (a.audio == nullptr) {
                set.refused.push_back(dp.node);
                continue;
            }
            d->audioApi = a.audio;
            d->handle = a.audio->create(static_cast<std::uint32_t>(dp.rateHz + 0.5));
        }
        if (d->handle == nullptr) {
            // The ABI's "the plugin is unusable": nothing else may be called
            // on it, and with a null handle the destructor calls nothing.
            set.refused.push_back(dp.node);
            continue;
        }

        std::size_t outCap = kDecoderSlice;
        if (resample) {
            const auto outR = static_cast<unsigned>(dp.rateHz + 0.5);
            d->rsI = std::make_unique<cascade::dsp::RationalResampler>(outR, inR);
            if (iqFeed) { d->rsQ = std::make_unique<cascade::dsp::RationalResampler>(outR, inR); }
            outCap = d->rsI->maxOut(kDecoderSlice) + 8;
            d->outI.assign(outCap, 0.0f);
            if (iqFeed) { d->outQ.assign(outCap, 0.0f); }
        }
        d->inI.assign(kDecoderSlice, 0.0f);
        if (iqFeed) {
            d->inQ.assign(kDecoderSlice, 0.0f);
            d->interleaved.assign(2 * outCap, 0.0f);
        }
        d->partial.reserve(kMaxLineBytes);

        if (dp.source == DecoderSource::Channel) {
            RunningChannel& rc = set.channels[d->chanIndex];
            if (!rc.wantIq) {
                rc.wantIq = true;
                rc.iq.assign(kMaxBlockAudio, std::complex<float>(0.0f, 0.0f));
            }
        }
        set.decoders.push_back(std::move(d));
    }
}

// EVERYTHING buildStripSet() READS, AND NOTHING ELSE, as one comparable string.
//
// The GUI republishes a patch when this changes, and only then. Before plugin
// decoders it republished on any edit - including every frame of a node being
// dragged - which merely rebuilt some filters. With decoders in the set, every
// republish destroys and re-creates every plugin instance, so a drag across the
// canvas would restart every decoder sixty times a second and lose whatever
// each was half-way through decoding. Position, size, zoom and pan are not in
// here because the DSP does not read them.
//
// The decoder's NAME is, because it tags every line the decoder produces; the
// API pointers are, because a rescan can reload a module at a new address
// under the same key, and a set still holding the old pointers would call
// into an unmapped image.
inline std::string dspSignature(const Plan& plan, const Graph& g, double deviceRateHz,
                                NodeId listening, double audioRateHz,
                                const std::vector<DecoderInfo>* catalogue,
                                const std::vector<PluginApis>* apis) {
    std::string s;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "r%.17g l%u a%.17g|", deviceRateHz,
                  static_cast<unsigned>(listening), audioRateHz);
    s += buf;
    for (const ChannelPlan& c : plan.channels) {
        // The mode each channel is demodulated with, exactly as the builder
        // derives it: from the Demod node it feeds.
        int mode = -1;
        for (const Wire& w : g.wires()) {
            if (w.from != c.node) { continue; }
            const Node* dst = g.find(w.to);
            if (dst != nullptr && dst->kind == NodeKind::Demod) {
                mode = dst->mode;
                break;
            }
        }
        std::snprintf(buf, sizeof(buf), "c%u o%.17g d%u m%d|", static_cast<unsigned>(c.node),
                      c.offsetHz, c.decimation, mode);
        s += buf;
    }
    for (const DecoderPlan& d : plan.decoders) {
        std::snprintf(buf, sizeof(buf), "d%u s%u ch%u i%.17g o%.17g f%.17g|",
                      static_cast<unsigned>(d.node), static_cast<unsigned>(d.source),
                      static_cast<unsigned>(d.channel), d.inRateHz, d.rateHz, d.centreHz);
        s += buf;
        if (catalogue != nullptr && d.plugin < catalogue->size()) {
            s += (*catalogue)[d.plugin].key;
            s += '|';
        }
        if (apis != nullptr && d.plugin < apis->size()) {
            std::snprintf(buf, sizeof(buf), "%p %p %p|",
                          static_cast<const void*>((*apis)[d.plugin].iq),
                          static_cast<const void*>((*apis)[d.plugin].audio),
                          static_cast<const void*>((*apis)[d.plugin].image));
            s += buf;
        }
        if (const Node* n = g.find(d.node); n != nullptr) {
            s += n->name;
            s += '|';
        }
    }
    return s;
}

inline std::shared_ptr<StripSet> buildStripSet(const Plan& plan, const Graph& g,
                                               double deviceRateHz, NodeId listening,
                                               double audioRateHz = 48000.0,
                                               const std::vector<DecoderInfo>* catalogue = nullptr,
                                               const std::vector<PluginApis>* apis = nullptr) {
    auto set = std::make_shared<StripSet>();
    set->listening = listening;
    set->channels.reserve(plan.channels.size());

    for (const ChannelPlan& cp : plan.channels) {
        RunningChannel rc;
        rc.node = cp.node;
        rc.strip.configure(cp.offsetHz, deviceRateHz, cp.decimation);

        // What this channel is demodulated as comes from the Demod node it
        // feeds, if it feeds one. A channel wired straight to a display has
        // no demodulator and no opinion, so it takes AM - an envelope is the
        // more useful of the two to look at.
        rc.mode = Demod::Am;
        for (const Wire& w : g.wires()) {
            if (w.from != cp.node) { continue; }
            const Node* dst = g.find(w.to);
            if (dst != nullptr && dst->kind == NodeKind::Demod) {
                // Mode indices are the host's; odd ones here are the FM
                // family. Anything unrecognised stays AM rather than
                // guessing - a wrong demodulator is silence, not a worse
                // version of the right one.
                rc.mode = (dst->mode == 0 || dst->mode == 1) ? Demod::Fm : Demod::Am;
                break;
            }
        }

        rc.audio.assign(kMaxBlockAudio, 0.0f);
        set->channels.push_back(std::move(rc));
    }

    // Only the channel being listened to needs converting - the others are
    // measured and decoded, not played, and a resampler each would be work
    // nobody hears.
    for (const RunningChannel& rc : set->channels) {
        if (rc.node != listening) { continue; }
        const double inRate = rc.strip.outRateHz();
        if (inRate > 0.0 && audioRateHz > 0.0) {
            set->toAudio = std::make_unique<cascade::dsp::RationalResampler>(
                static_cast<unsigned>(audioRateHz + 0.5),
                static_cast<unsigned>(inRate + 0.5));
            set->resampled.assign(set->toAudio->maxOut(kMaxBlockAudio) + 8, 0.0f);
        }
        break;
    }

    if (catalogue != nullptr && apis != nullptr) {
        buildDecoders(*set, plan, g, *catalogue, *apis);
    }
    return set;
}

class Runner {
public:
    // GUI THREAD. Hands a complete set over. Returns immediately; the DSP
    // thread picks it up at the top of its next block.
    void publish(std::shared_ptr<StripSet> set) {
        // A set published and never adopted is released HERE, on this
        // thread, and after the lock - destroying it under the mutex would
        // hold the DSP thread out for however long its destructor takes.
        std::shared_ptr<StripSet> superseded;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            superseded = std::move(pending_);
            pending_ = std::move(set);
            stopPending_ = false;
            hasPending_.store(true, std::memory_order_release);
        }
    }

    // DSP THREAD. Takes any published set. Returns true when one was adopted,
    // which is what a test uses to prove the hand-off happened.
    //
    // THE ATOMIC IS CHECKED FIRST AND THE LOCK IS TAKEN ONLY WHEN THERE IS
    // SOMETHING TO TAKE. In the overwhelmingly common case - no rewire this
    // block - this is one relaxed load and no lock at all, so the audio thread
    // never waits on the GUI thread except in the instant a patch actually
    // changed, where the GUI thread holds the mutex for one pointer move.
    bool adopt() {
        const DspScope scope(*this);
        if (!scope.entered) { return false; }
        return adoptImpl();
    }

    // DSP THREAD. Runs every channel over the block. Adopts first, so a patch
    // published between blocks takes effect at a block boundary and never
    // halfway through one.
    void process(const std::complex<float>* in, std::size_t n) {
        const DspScope scope(*this);
        if (!scope.entered) { return; }   // a flush has the runner
        adoptImpl();
        processImpl(in, n);
    }

    // DSP THREAD. Fills `left` and `right` with `frames` of the patch's
    // audio and returns TRUE; returns FALSE when no patch is listening,
    // leaving both buffers untouched so the caller keeps its demodulated
    // audio.
    //
    // DELIBERATELY THE SAME SHAPE AS PluginRunner::pullPluginAudio, down to
    // the short-block rule: a patch that has not produced enough this block
    // is still the thing being listened to, so the shortfall is silence
    // rather than a handback to the demodulator. Handing back would make a
    // momentarily starved patch chatter between two sources, which is worse
    // than a gap and much harder to diagnose.
    //
    // During a flushNow() this answers FALSE - the patch is not playing for
    // those few blocks, so the receiver's own audio carries on.
    bool pullAudio(float* left, float* right, std::size_t frames) {
        const DspScope scope(*this);
        if (!scope.entered) { return false; }
        return pullAudioImpl(left, right, frames);
    }

    // DSP THREAD. The audio of the channel the patch is listening to, or
    // nothing when it is not running one. Test-facing: the pointer is only
    // good until the next process(), and nothing outside the tests reads it.
    const float* listeningAudio(std::size_t& count) const {
        count = 0;
        const DspScope scope(const_cast<Runner&>(*this));
        if (!scope.entered) { return nullptr; }
        return listeningAudioImpl(count);
    }

    // GUI THREAD, and SYNCHRONOUS. Stops the patch and destroys every set
    // the runner holds - running, pending and retired - before it returns.
    //
    // WHY THIS EXISTS BESIDE clear(). clear() is the everyday stop: it asks,
    // and the set dies at a later reap(). That is not good enough before the
    // plugin host unmaps its modules, because a set may own decoder handles
    // and a destroy() that runs after its DLL is gone is a crash in somebody
    // else's code with no useful stack. So this does it NOW.
    //
    // HOW, WITHOUT A LOCK ON THE AUDIO PATH. Every DSP entry point marks
    // itself inside (inDsp_) and then checks frozen_; this sets frozen_ and
    // then waits until inDsp_ is clear. Both sides use sequentially
    // consistent operations, so at least one of them sees the other: either
    // the DSP thread sees the freeze and leaves without touching anything,
    // or this sees the DSP thread inside and waits for it to finish its
    // block. The wait is one DSP block at most - process() never blocks on
    // anything this thread holds - and it happens only on a plugin rescan or
    // removal, which is a pause the user has asked for.
    void flushNow() {
        frozen_.store(true, std::memory_order_seq_cst);
        while (inDsp_.load(std::memory_order_seq_cst)) { std::this_thread::yield(); }

        // The DSP thread is out and stays out until frozen_ clears, so this
        // thread may touch what it owns.
        std::shared_ptr<StripSet> running = std::move(active_);
        std::shared_ptr<StripSet> pending;
        std::vector<std::shared_ptr<StripSet>> dead;
        dead.reserve(kRetiredReserve);
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pending = std::move(pending_);
            stopPending_ = false;
            hasPending_.store(false, std::memory_order_release);
            dead.swap(retired_);
        }
        resetRing();
        // The seq_cst store is what hands everything written above back to
        // the DSP thread: its next entry loads frozen_ and sees false only
        // after this, so it sees an empty runner.
        frozen_.store(false, std::memory_order_seq_cst);

        // Destroyed here, on this thread, with nothing held.
        running.reset();
        pending.reset();
        dead.clear();
    }

private:
    // Marks the calling DSP thread inside the runner for the length of one
    // call, unless a flush has frozen it. See flushNow().
    struct DspScope {
        Runner& r;
        bool entered = false;
        explicit DspScope(Runner& runner) : r(runner) {
            r.inDsp_.store(true, std::memory_order_seq_cst);
            entered = !r.frozen_.load(std::memory_order_seq_cst);
            if (!entered) { r.inDsp_.store(false, std::memory_order_seq_cst); }
        }
        ~DspScope() {
            if (entered) { r.inDsp_.store(false, std::memory_order_seq_cst); }
        }
        DspScope(const DspScope&) = delete;
        DspScope& operator=(const DspScope&) = delete;
    };

    std::atomic<bool> frozen_{false};
    std::atomic<bool> inDsp_{false};

    bool adoptImpl() {
        if (!hasPending_.load(std::memory_order_acquire)) { return false; }
        std::shared_ptr<StripSet> taken;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            locks_.fetch_add(1, std::memory_order_relaxed);
            taken = std::move(pending_);
            pending_.reset();
            stop = stopPending_;
            stopPending_ = false;
            // CLEARED HERE, unconditionally, including when the pointer
            // turns out to be empty. Leaving it set is externally
            // invisible - adopt() still answers false - and makes the DSP
            // thread take this mutex on every block from then on, which
            // is the one thing this file exists to avoid. A mutant that
            // removed this survived until lockCount() was asserted.
            hasPending_.store(false, std::memory_order_release);

            // THE OLD SET GOES BACK TO THE GUI THREAD, not to the
            // destructor. Retired under the lock this block already holds,
            // so reap() never races the push. The vector is reserved at
            // construction and emptied every GUI frame, so this does not
            // allocate on the audio thread in practice; if the GUI thread
            // ever stalls long enough to fill it, growing it here is still
            // the right trade - the alternative is destroying a plugin
            // handle on the wrong thread.
            if ((taken || stop) && active_) { retired_.push_back(std::move(active_)); }
        }
        if (!taken) {
            if (stop) { resetRing(); }
            return false;
        }
        active_ = std::move(taken);
        // THE RING GOES WITH THE OLD SET. Its contents are at the previous
        // channel's rate and from the previous channel's frequency; playing
        // them after a rewire is playing the patch the user just replaced.
        resetRing();
        generation_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // The body of process(), run inside a DspScope after adoptImpl().
    void processImpl(const std::complex<float>* in, std::size_t n) {
        if (!active_ || in == nullptr) { return; }
        for (RunningChannel& rc : active_->channels) {
            rc.produced = 0;
            scratch_.clear();
            iqScratch_.clear();
            rc.strip.process(in, n, rc.mode, scratch_, rc.wantIq ? &iqScratch_ : nullptr);
            const std::size_t take = (scratch_.size() < rc.audio.size()) ? scratch_.size()
                                                                         : rc.audio.size();
            for (std::size_t i = 0; i < take; ++i) { rc.audio[i] = scratch_[i]; }
            rc.produced = take;
            if (rc.wantIq) {
                // Same count as the audio by construction (one I/Q sample per
                // demodulated one), and capped by the same preallocation.
                for (std::size_t i = 0; i < take && i < iqScratch_.size(); ++i) {
                    rc.iq[i] = iqScratch_[i];
                }
            }

            // The listening channel also goes to the sink's rate and into
            // the ring the audio stage draws from.
            if (rc.node == active_->listening && active_->toAudio && take > 0) {
                const std::size_t got = active_->toAudio->process(
                    rc.audio.data(), take, active_->resampled.data(),
                    active_->resampled.size());
                pushRing(active_->resampled.data(), got);
            }
        }

        // THE DECODERS, after every channel has produced its block, so a
        // decoder always reads its channel's samples from THIS block.
        for (const std::unique_ptr<RunningDecoder>& up : active_->decoders) {
            RunningDecoder& d = *up;
            if (d.failed) { continue; }
            switch (d.source) {
                case DecoderSource::Radio:
                    feedIq(d, in, n);
                    break;
                case DecoderSource::Channel: {
                    const RunningChannel& rc = active_->channels[d.chanIndex];
                    feedIq(d, rc.iq.data(), rc.produced);
                    break;
                }
                case DecoderSource::Audio: {
                    const RunningChannel& rc = active_->channels[d.chanIndex];
                    feedAudio(d, rc.audio.data(), rc.produced);
                    break;
                }
            }
            pollText(d);
            if (d.imageApi != nullptr && !d.failed) { pollImage(d); }
        }
    }

    // THE NEWEST PICTURE, polled at most every kImagePollSeconds of input.
    // poll_image hands a BORROW of the plugin's pixels; they are copied into
    // a HostImage and released before this returns, so no borrow is ever
    // outstanding when the set - and with it destroy() - goes. The copy is an
    // allocation on the audio thread, which is why it is throttled: a picture
    // decoder builds a line or two a second, and a few copies a second of a
    // weather-satellite frame is noise beside the demodulation.
    static constexpr double kImagePollSeconds = 0.25;
    void pollImage(RunningDecoder& d) {
        const auto every = static_cast<std::uint64_t>(d.rateHz * kImagePollSeconds);
        if (d.lastImagePoll != 0 && d.framesFed - d.lastImagePoll < every) { return; }
        d.lastImagePoll = d.framesFed == 0 ? 1 : d.framesFed;
        CascadeImage img{};
        img.structSize = sizeof(CascadeImage);
        const std::int32_t r = d.imageApi->poll_image(d.handle, &img);
        if (r < 0) {
            d.failed = true;
            return;
        }
        if (r != 1) { return; }
        // Checked the way the plugin runner checks it: a corrupt size must not
        // make the host read or allocate absurd amounts.
        const std::uint32_t bpp = img.format == CASCADE_IMAGE_RGB24 ? 3u
                                  : img.format == CASCADE_IMAGE_GRAY8 ? 1u
                                                                     : 0u;
        const bool sane = bpp != 0u && img.pixels != nullptr && img.width >= 1u &&
                          img.height >= 1u && img.width <= CASCADE_IMAGE_MAX_DIM &&
                          img.height <= CASCADE_IMAGE_MAX_DIM &&
                          img.stride >= img.width * bpp;
        if (sane) {
            cascade::core::HostImage h;
            h.plugin = d.name;
            h.width = img.width;
            h.height = img.height;
            h.format = img.format;
            h.complete = img.complete != 0;
            h.sequence = img.sequence;
            h.revision = ++d.imageRevision;
            const std::size_t row = static_cast<std::size_t>(img.width) * bpp;
            h.pixels.resize(row * img.height);
            for (std::uint32_t y = 0; y < img.height; ++y) {
                const std::uint8_t* src = img.pixels + static_cast<std::size_t>(y) * img.stride;
                std::copy(src, src + row, h.pixels.begin() + static_cast<std::ptrdiff_t>(y * row));
            }
            std::lock_guard<std::mutex> lock(imagesMutex_);
            // The newest per node only: a GUI that is not looking loses
            // superseded frames, never grows a queue of them.
            bool replaced = false;
            for (auto& slot : images_) {
                if (slot.first == d.node) {
                    slot.second = std::move(h);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) { images_.emplace_back(d.node, std::move(h)); }
        }
        // Released whether or not it was sane: every successful poll_image
        // gets exactly one release_image, before destroy().
        d.imageApi->release_image(d.handle, &img);
    }

    // The one place each kind of table is called, so a picture decoder - which
    // takes either input through one process() - is fed by the same code.
    static void processIq(RunningDecoder& d, const float* interleaved, std::size_t frames) {
        if (d.iqApi != nullptr) {
            d.iqApi->process(d.handle, interleaved, frames);
        } else if (d.imageApi != nullptr) {
            d.imageApi->process(d.handle, interleaved, frames);
        }
    }
    static void processAudio(RunningDecoder& d, const float* samples, std::size_t frames) {
        if (d.audioApi != nullptr) {
            d.audioApi->process(d.handle, samples, frames);
        } else if (d.imageApi != nullptr) {
            d.imageApi->process(d.handle, samples, frames);
        }
    }

    // Hands `count` complex samples to an I/Q decoder, through its resampler
    // when it has one, a fixed-size slice at a time.
    static void feedIq(RunningDecoder& d, const std::complex<float>* x, std::size_t count) {
        for (std::size_t off = 0; off < count; off += kDecoderSlice) {
            const std::size_t m = (count - off < kDecoderSlice) ? (count - off) : kDecoderSlice;
            if (!d.rsI) {
                // std::complex<float> is layout-compatible with float[2], and
                // that is the ABI's I,Q,I,Q rule - a view, not a copy.
                processIq(d, reinterpret_cast<const float*>(x + off), m);
                d.framesFed += m;
                continue;
            }
            for (std::size_t i = 0; i < m; ++i) {
                d.inI[i] = x[off + i].real();
                d.inQ[i] = x[off + i].imag();
            }
            const std::size_t ki = d.rsI->process(d.inI.data(), m, d.outI.data(), d.outI.size());
            const std::size_t kq = d.rsQ->process(d.inQ.data(), m, d.outQ.data(), d.outQ.size());
            // Identical filters on identical input counts produce identical
            // output counts; the min is a guard, not an expectation.
            const std::size_t k = (ki < kq) ? ki : kq;
            for (std::size_t i = 0; i < k; ++i) {
                d.interleaved[2 * i] = d.outI[i];
                d.interleaved[2 * i + 1] = d.outQ[i];
            }
            if (k > 0) {
                processIq(d, d.interleaved.data(), k);
                d.framesFed += k;
            }
        }
    }

    static void feedAudio(RunningDecoder& d, const float* x, std::size_t count) {
        for (std::size_t off = 0; off < count; off += kDecoderSlice) {
            const std::size_t m = (count - off < kDecoderSlice) ? (count - off) : kDecoderSlice;
            if (!d.rsI) {
                processAudio(d, x + off, m);
                d.framesFed += m;
                continue;
            }
            const std::size_t k = d.rsI->process(x + off, m, d.outI.data(), d.outI.size());
            if (k > 0) {
                processAudio(d, d.outI.data(), k);
                d.framesFed += k;
            }
        }
    }

    // Reads whatever text the decoder has, splits it into lines, and queues
    // complete ones for the GUI. Bounded: a handful of polls per block, and a
    // line never longer than kMaxLineBytes, so a misbehaving plugin cannot
    // hold the audio thread or grow memory without limit.
    void pollText(RunningDecoder& d) {
        char buf[1024];
        for (int tries = 0; tries < 8; ++tries) {
            const std::int32_t r =
                (d.iqApi != nullptr)      ? d.iqApi->poll_text(d.handle, buf, sizeof(buf))
                : (d.audioApi != nullptr) ? d.audioApi->poll_text(d.handle, buf, sizeof(buf))
                                          : d.imageApi->poll_text(d.handle, buf, sizeof(buf));
            if (r < 0) {
                // "Failed permanently": fed and polled no further. The handle
                // is KEPT until the set dies, so destroy() still runs exactly
                // once, after the last call of anything else on it.
                d.failed = true;
                return;
            }
            if (r == 0) { return; }
            // Never trust a length past what was offered.
            const std::size_t len = (static_cast<std::size_t>(r) < sizeof(buf))
                                        ? static_cast<std::size_t>(r)
                                        : sizeof(buf);
            for (std::size_t i = 0; i < len; ++i) {
                const char c = buf[i];
                if (c == '\n') {
                    if (!d.partial.empty()) { emitLine(d); }
                    d.partial.clear();
                } else if (c != '\r' && d.partial.size() < kMaxLineBytes) {
                    d.partial.push_back(c);
                }
            }
        }
    }

    // THE ONE LOCK THIS FILE TAKES ON THE AUDIO THREAD OUTSIDE A HAND-OFF, and
    // only when a decoder has finished a line - never per block. The GUI side
    // holds it for a vector swap and nothing else, so the audio thread cannot
    // be made to wait behind allocation or I/O. The queue is bounded; a GUI
    // that stops draining loses the NEWEST lines and counts them, rather than
    // the audio thread growing it forever.
    void emitLine(const RunningDecoder& d) {
        std::lock_guard<std::mutex> lock(textMutex_);
        if (lines_.size() >= kMaxPendingLines) {
            ++droppedLines_;
            return;
        }
        lines_.push_back(PatchLine{d.node, d.name, d.partial});
    }

public:
    // GUI THREAD. Every line the patch's decoders have finished since the last
    // call, oldest first.
    std::vector<PatchLine> drainText() {
        std::vector<PatchLine> out;
        std::lock_guard<std::mutex> lock(textMutex_);
        out.swap(lines_);
        return out;
    }

    // GUI THREAD. The newest picture from each picture decoder since the last
    // call.
    std::vector<std::pair<NodeId, cascade::core::HostImage>> drainImages() {
        std::vector<std::pair<NodeId, cascade::core::HostImage>> out;
        std::lock_guard<std::mutex> lock(imagesMutex_);
        out.swap(images_);
        return out;
    }

    // Lines lost because nobody drained them in time.
    std::uint64_t droppedLines() const {
        std::lock_guard<std::mutex> lock(textMutex_);
        return droppedLines_;
    }

    static constexpr std::size_t kMaxPendingLines = 1000;

private:
    // The body of pullAudio(); see there for the contract.
    bool pullAudioImpl(float* left, float* right, std::size_t frames) {
        if (!active_ || active_->listening == kNoNode || !active_->toAudio) {
            return false;
        }
        if (left == nullptr || right == nullptr) { return false; }
        for (std::size_t i = 0; i < frames; ++i) {
            float v = 0.0f;
            if (ringCount_ > 0) {
                v = ring_[ringRead_];
                ringRead_ = (ringRead_ + 1 == ring_.size()) ? 0 : ringRead_ + 1;
                --ringCount_;
            } else {
                ++starved_;
            }
            left[i] = v;
            right[i] = v;   // mono, on both
        }
        return true;
    }

public:
    // Frames of silence handed out because the patch had nothing ready.
    // The honest health number for this path, and the one a dropout shows
    // up in - a patch that is never ready is a patch producing nothing.
    std::uint64_t starvedFrames() const { return starved_; }

    std::size_t bufferedFrames() const { return ringCount_; }

private:
    // The body of listeningAudio().
    const float* listeningAudioImpl(std::size_t& count) const {
        count = 0;
        // The kNoNode test is an early-out, not a guard: the loop below
        // compares every channel against listening, and no real node id is
        // kNoNode, so removing this changes no answer - only the length of
        // the scan. A break-it run correctly reports it as equivalent.
        if (!active_ || active_->listening == kNoNode) { return nullptr; }
        for (const RunningChannel& rc : active_->channels) {
            if (rc.node == active_->listening) {
                count = rc.produced;
                return rc.audio.data();
            }
        }
        return nullptr;
    }

public:
    // How many sets have been adopted. Test-facing, and cheap enough to leave
    // in: it is the only way to tell "the swap happened" from "the swap was
    // published and quietly dropped".
    std::uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }

    // How many times the DSP thread has taken the publish mutex. The
    // promise is that this moves only when a patch actually changed, so a
    // test can assert the audio thread is not queueing behind the GUI
    // thread on every block.
    std::uint64_t lockCount() const { return locks_.load(std::memory_order_relaxed); }

    std::size_t channelCount() const { return active_ ? active_->channels.size() : 0; }

    bool running() const { return static_cast<bool>(active_); }

    // GUI THREAD. Asks the DSP thread to stop running the patch at its next
    // block, and drops anything published but not yet adopted.
    //
    // A REQUEST, NOT AN ACT: see the note at the top of this file. The set
    // that is running is the DSP thread's until the DSP thread lets it go, and
    // it comes back through reap(). A receiver that is stopped runs no blocks,
    // so the request waits until it starts again - harmless, because a stopped
    // receiver feeds the set nothing in the meantime.
    void clear() {
        std::shared_ptr<StripSet> dropped;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            dropped = std::move(pending_);
            stopPending_ = true;
            hasPending_.store(true, std::memory_order_release);
        }
    }

    // GUI THREAD, every frame, whether or not the patch page is open - the
    // set a closing page stops is retired AFTER the page has gone. Destroys
    // every set the DSP thread has stopped running.
    void reap() {
        std::vector<std::shared_ptr<StripSet>> dead;
        dead.reserve(kRetiredReserve);
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            // Swapped, so retired_ comes away with the fresh reservation
            // and the DSP thread's next push does not allocate.
            dead.swap(retired_);
        }
        // `dead` is destroyed here, on this thread, outside the lock.
    }

private:
    static constexpr std::size_t kRetiredReserve = 32;

    void resetRing() {
        ringRead_ = 0;
        ringWrite_ = 0;
        ringCount_ = 0;
    }

    // Written by the GUI thread under the mutex, read by the DSP thread.
    std::mutex pendingMutex_;
    std::shared_ptr<StripSet> pending_;
    bool stopPending_ = false;
    std::atomic<bool> hasPending_{false};
    // Sets the DSP thread has stopped running, waiting for reap(). Pushed by
    // the DSP thread and swapped out by the GUI thread, both under the mutex.
    std::vector<std::shared_ptr<StripSet>> retired_ = [] {
        std::vector<std::shared_ptr<StripSet>> v;
        v.reserve(kRetiredReserve);
        return v;
    }();

    // DSP THREAD ONLY after adoption. No other thread may touch this.
    std::shared_ptr<StripSet> active_;
    std::vector<float> scratch_;
    std::vector<std::complex<float>> iqScratch_;

    // Decoded lines on their way to the GUI; see emitLine().
    mutable std::mutex textMutex_;
    std::vector<PatchLine> lines_;
    std::uint64_t droppedLines_ = 0;
    std::mutex imagesMutex_;
    std::vector<std::pair<NodeId, cascade::core::HostImage>> images_;

    // A second of audio at 48 kHz, allocated once. DSP thread only: it is
    // written by process() and read by pullAudio(), which the pipeline
    // calls from the same thread.
    //
    // OLDEST DROPPED WHEN IT OVERFLOWS, not newest. A full ring means the
    // sink is behind, and the samples worth keeping are the ones about to
    // be played, not the ones that went stale a second ago.
    std::vector<float> ring_ = std::vector<float>(48000, 0.0f);
    std::size_t ringRead_ = 0;
    std::size_t ringWrite_ = 0;
    std::size_t ringCount_ = 0;
    std::uint64_t starved_ = 0;

    void pushRing(const float* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            ring_[ringWrite_] = p[i];
            ringWrite_ = (ringWrite_ + 1 == ring_.size()) ? 0 : ringWrite_ + 1;
            if (ringCount_ < ring_.size()) {
                ++ringCount_;
            } else {
                // Full: the write just overwrote the oldest unread sample,
                // so the read cursor has to move with it.
                ringRead_ = ringWrite_;
            }
        }
    }

    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> locks_{0};
};

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_RUNNER_HPP
