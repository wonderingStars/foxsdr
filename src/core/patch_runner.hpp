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
#include <memory>
#include <mutex>
#include <vector>

#include "core/patch_graph.hpp"
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
};

// Everything the DSP thread needs to run a patch. Built complete on the GUI
// thread; touched by exactly one thread at a time thereafter.
struct StripSet {
    std::vector<RunningChannel> channels;

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
inline std::shared_ptr<StripSet> buildStripSet(const Plan& plan, const Graph& g,
                                               double deviceRateHz, NodeId listening,
                                               double audioRateHz = 48000.0) {
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

    // DSP THREAD. Runs every channel over the block. Adopts first, so a patch
    // published between blocks takes effect at a block boundary and never
    // halfway through one.
    void process(const std::complex<float>* in, std::size_t n) {
        adopt();
        if (!active_ || in == nullptr) { return; }
        for (RunningChannel& rc : active_->channels) {
            rc.produced = 0;
            scratch_.clear();
            rc.strip.process(in, n, rc.mode, scratch_);
            const std::size_t take = (scratch_.size() < rc.audio.size()) ? scratch_.size()
                                                                         : rc.audio.size();
            for (std::size_t i = 0; i < take; ++i) { rc.audio[i] = scratch_[i]; }
            rc.produced = take;

            // The listening channel also goes to the sink's rate and into
            // the ring the audio stage draws from.
            if (rc.node == active_->listening && active_->toAudio && take > 0) {
                const std::size_t got = active_->toAudio->process(
                    rc.audio.data(), take, active_->resampled.data(),
                    active_->resampled.size());
                pushRing(active_->resampled.data(), got);
            }
        }
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
    bool pullAudio(float* left, float* right, std::size_t frames) {
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

    // Frames of silence handed out because the patch had nothing ready.
    // The honest health number for this path, and the one a dropout shows
    // up in - a patch that is never ready is a patch producing nothing.
    std::uint64_t starvedFrames() const { return starved_; }

    std::size_t bufferedFrames() const { return ringCount_; }

    // DSP THREAD. The audio of the channel the patch is listening to, or
    // nothing when it is not running one.
    const float* listeningAudio(std::size_t& count) const {
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
