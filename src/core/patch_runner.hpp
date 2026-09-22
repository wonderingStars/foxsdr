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
// A SET THE DSP THREAD NEVER ADOPTED is dropped when the next one replaces it,
// and its destructor runs on whichever thread happened to release the last
// reference. That is fine - it owns only memory - and it is why the set holds
// no handles to anything that must be closed on a particular thread.
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
                                               double deviceRateHz, NodeId listening) {
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
    return set;
}

class Runner {
public:
    // GUI THREAD. Hands a complete set over. Returns immediately; the DSP
    // thread picks it up at the top of its next block.
    void publish(std::shared_ptr<StripSet> set) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_ = std::move(set);
        hasPending_.store(true, std::memory_order_release);
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
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            locks_.fetch_add(1, std::memory_order_relaxed);
            taken = std::move(pending_);
            pending_.reset();
            // CLEARED HERE, unconditionally, including when the pointer
            // turns out to be empty. Leaving it set is externally
            // invisible - adopt() still answers false - and makes the DSP
            // thread take this mutex on every block from then on, which
            // is the one thing this file exists to avoid. A mutant that
            // removed this survived until lockCount() was asserted.
            hasPending_.store(false, std::memory_order_release);
        }
        if (!taken) { return false; }
        active_ = std::move(taken);
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
        }
    }

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

    // DSP THREAD. Forgets everything, so a stopped receiver does not leave a
    // patch holding buffers that will be stale when it starts again.
    void clear() {
        active_.reset();
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.reset();
        hasPending_.store(false, std::memory_order_release);
    }

private:
    // Written by the GUI thread under the mutex, read by the DSP thread.
    std::mutex pendingMutex_;
    std::shared_ptr<StripSet> pending_;
    std::atomic<bool> hasPending_{false};

    // DSP THREAD ONLY after adoption. No other thread may touch this.
    std::shared_ptr<StripSet> active_;
    std::vector<float> scratch_;

    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> locks_{0};
};

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_RUNNER_HPP
