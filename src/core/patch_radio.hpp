// patch_radio.hpp - one of a patch's radios, running on its own (0.99.17).
//
// UP TO FIVE AT ONCE (owner, 2026-09-23). Before this the patch had one radio:
// the receiver's, fed from the receiver's DSP thread. Now each Radio node opens
// its OWN device and this object runs it - the source, a reader thread, the
// patch Runner holding that radio's channels, decoders and speaker taps, and a
// spectrum of what it is receiving for the node's display.
//
// THE READER THREAD is the whole of the DSP for this radio: read a block, hand
// it to the runner (which demodulates, decodes and writes each speaker's
// output), and every ~50 ms fold a block into the spectrum. It is the "DSP
// thread" of patch_runner.hpp's rules: the GUI builds sets and publishes them,
// this thread adopts them, and retired sets die on the GUI thread through
// runner().reap().
//
// PACING follows the receiver's own rule (Pipeline::sourceThreadBody): a device
// paces itself - read() blocks until samples arrive - while the generator is
// free-running and is paced here by the wall clock, or it would produce a
// minute of signal a second.
//
// STOPPING is bounded. stop() asks the source to stop (which is what lets a
// device's blocking read return) and waits up to kStopWaitMs for the thread. A
// driver that does not honour the abort is ABANDONED rather than waited on
// forever: the thread keeps a reference to everything it touches, so it can
// finish whenever the driver lets it go without reading freed memory - the
// same policy the receiver applies to a wedged vendor stack. Its DECODERS are
// not abandoned with it: stop() flushes the runner first, so every plugin
// handle is destroyed on the calling thread before stop() returns, never
// later on the abandoned one (by which time a rescan may have unmapped the
// plugin's module).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/patch_runner.hpp"
#include "source/iq_source.hpp"

namespace cascade::core::patch {

class PatchRadio {
public:
    // Takes an already OPEN source (a device opened on a worker, or the
    // generator). The label is what the node's face names it by.
    PatchRadio(NodeId node, std::unique_ptr<cascade::source::IqSource> source,
               std::string label);
    ~PatchRadio();   // stop()
    PatchRadio(const PatchRadio&) = delete;
    PatchRadio& operator=(const PatchRadio&) = delete;

    // Starts the source and the reader thread. False with a reason.
    bool start(std::string& error);

    // Stops and joins (or, past kStopWaitMs, abandons) the reader. Idempotent.
    // After it returns no further process() runs on this radio's runner, and
    // on the abandon path every decoder handle it held is already destroyed.
    void stop();

    static constexpr int kStopWaitMs = 3000;

    NodeId node() const { return node_; }
    const std::string& label() const { return label_; }
    bool running() const;

    // The device's own readback. GUI thread.
    double rateHz() const;
    double centreHz() const;
    // Retunes the device. GUI thread; sources accept a retune while their
    // reader is inside read(), as the receiver's own retune relies on.
    bool setCentreHz(double hz);

    // The runner holding this radio's strips. publish() and reap() from the
    // GUI thread; the reader thread is its DSP thread.
    Runner& runner();

    // The newest spectrum, dB per bin in frequency order (fftshifted), across
    // the whole capture. Returns false when nothing newer than `seq` exists.
    bool spectrum(std::vector<float>& db, std::uint64_t& seq) const;

    // Non-empty once the source has faulted or stopped on its own.
    std::string fault() const;

    // Blocks read from the source since start - a liveness figure for tests
    // and the log.
    std::uint64_t blocksRead() const;

    // The state the reader thread shares with this object. Opaque: defined in
    // patch_radio.cpp, named here only so the reader can hold a reference.
    struct Shared;

private:
    NodeId node_;
    std::string label_;
    std::shared_ptr<Shared> sh_;
    std::thread thread_;
    bool started_ = false;
};

}  // namespace cascade::core::patch
