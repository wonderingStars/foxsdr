// plugin_runner.hpp - drives loaded decoder plugins with real samples.
//
// The missing half of the plugin system. plugin_host loads and validates a
// plugin and hands back its API tables; nothing until this class ever CALLED
// them, so an installed decoder sat in the list and decoded nothing.
//
// THREADING, which is the whole difficulty here:
//
//   rebuild()/clear()   control thread. Creates and destroys instances.
//   processAudio()      DSP thread, real-time. Must not block or allocate.
//   drainText()         GUI thread, once per frame.
//
// A decoder instance is owned by exactly one thread at a time, as the ABI
// requires. That is enforced by never touching instances_ from the GUI thread
// and by taking the same lock in rebuild() that the DSP thread takes, so a
// rescan cannot destroy a handle that process() is inside.
//
// The lock on the audio path is deliberate and is the honest trade. A
// lock-free handoff would be nicer, but rebuild() happens on a user action
// (a rescan, an install) perhaps once a minute, holds the lock only to swap a
// vector, and never blocks on I/O; the DSP thread's wait is bounded by that.
// The alternative - a wait-free structure guarding pointers whose lifetime is
// controlled by another thread - is where use-after-free bugs live.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_CORE_PLUGIN_RUNNER_HPP
#define CASCADE_CORE_PLUGIN_RUNNER_HPP

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"

namespace cascade::core {

// One line of decoder output, tagged with which plugin said it. Without the
// tag a user running POCSAG and APRS together sees one undifferentiated stream
// and cannot tell which decoder is working.
struct DecodedLine {
    std::string plugin;  // the plugin's short descriptor name, e.g. "POCSAG"
    std::string text;    // one line, no trailing newline
};

// Why a loaded plugin is not currently being fed. Reported rather than
// silently ignored: "my decoder is installed and produces nothing" is the
// support question this exists to pre-empt.
enum class DecoderIdleReason {
    Running = 0,
    RateMismatch,   // wants a fixed rate the pipeline is not delivering
    CreateFailed,   // its create() returned NULL
    NoAudioTable,   // declares no capability this runner can drive
};

// Which stream a running decoder is being fed, so the panel can say so: a
// user who tuned the VFO and sees an I/Q decoder ignore it should be able to
// find out that it works on the whole band instead.
enum class DecoderStream {
    Audio = 0,
    Iq,
    None,
};

struct DecoderStatus {
    std::string plugin;
    DecoderIdleReason reason = DecoderIdleReason::Running;
    DecoderStream stream = DecoderStream::None;
    double wantRateHz = 0.0;  // meaningful for RateMismatch
    std::string detail;       // ready-to-display sentence
};

class PluginRunner {
public:
    PluginRunner() = default;
    ~PluginRunner();

    PluginRunner(const PluginRunner&) = delete;
    PluginRunner& operator=(const PluginRunner&) = delete;

    // Creates one instance per loaded plugin that declares a decoder this
    // runner can feed. Destroys whatever existed before. Safe to call while
    // the DSP thread is running.
    //
    // audioRateHz is the post-demodulation audio rate; iqRateHz is the raw
    // device rate and centreHz the RF frequency currently at DC, both of which
    // an I/Q decoder needs at create().
    void rebuild(const std::vector<LoadedPlugin>& plugins, double audioRateHz,
                 double iqRateHz, double centreHz);

    // Destroys every instance. Must be called BEFORE the plugin host unloads
    // the modules, or the handles would outlive the code that owns them.
    void clear();

    // DSP thread. `mono` is post-demod, post-AGC, post-squelch audio at the
    // rate passed to rebuild().
    void processAudio(const float* mono, std::size_t frames);

    // DSP thread. `interleaved` is 2*frames floats, I,Q,I,Q..., raw device
    // baseband at the iqRateHz passed to rebuild() - NOT the tuned, decimated
    // channel. An I/Q decoder does its own tuning and filtering; handing it
    // the VFO's narrow channel would throw away the band it needs (ADS-B alone
    // wants 2 MHz of it).
    void processIq(const float* interleaved, std::size_t frames);

    // DSP or control thread. The receiver moved; effective from the next
    // process call. Cheap and safe to call when nothing has changed.
    void retune(double centreHz);

    // GUI thread. Moves out whatever has been decoded since the last call.
    std::vector<DecodedLine> drainText();

    // GUI thread. What each loaded decoder is doing, or why it is not.
    std::vector<DecoderStatus> status() const;

    // Number of instances currently being fed.
    std::size_t activeCount() const;

    // How many samples have actually been handed to decoders since the last
    // rebuild. Diagnostic, but the important one: "the decoder produced
    // nothing" has two completely different causes - it was never fed, or it
    // was fed and found nothing - and without a count they are
    // indistinguishable from the outside.
    std::size_t audioFramesFed() const;
    std::size_t iqFramesFed() const;

private:
    struct Instance {
        const CascadeDecoderApi* api = nullptr;
        void* handle = nullptr;
        std::string name;
        // poll_text writes no NUL and splits only on code-point boundaries,
        // so a line can arrive across two polls. Carried here between calls.
        std::string partial;
    };

    struct IqInstance {
        const CascadeIqDecoderApi* api = nullptr;
        void* handle = nullptr;
        std::string name;
        std::string partial;
    };

    void destroyLocked();
    void pollLocked();
    void pollIqLocked();
    // Shared by both poll paths: appends `bytes` of decoder output to
    // `partial`, emits every complete line, keeps any tail. One
    // implementation, because two would eventually disagree about where a
    // line ends.
    void absorbLocked(const std::string& name, std::string& partial, const char* data,
                      std::size_t bytes);

    mutable std::mutex mutex_;
    std::vector<Instance> instances_;
    std::vector<IqInstance> iqInstances_;
    std::vector<DecoderStatus> status_;
    double audioRateHz_ = 0.0;
    double iqRateHz_ = 0.0;
    double centreHz_ = 0.0;
    std::size_t audioFed_ = 0;
    std::size_t iqFed_ = 0;

    // Bounded so a chatty decoder cannot grow this without limit when the GUI
    // is not draining (minimised, or a modal open). Oldest lines are dropped;
    // a decoder log is a tail, and stalling the DSP thread to preserve
    // history would be the wrong trade.
    static constexpr std::size_t kMaxPendingLines = 2000;
    std::deque<DecodedLine> pending_;

    // Scratch for poll_text, reused so the real-time path allocates nothing.
    std::vector<char> pollBuf_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_RUNNER_HPP
