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
//   pullPluginAudio()   DSP thread, real-time, the same thread and the same
//                       lock as processAudio - which is what lets the ABI
//                       promise a plugin that pull() never runs concurrently
//                       with process() on its handle.
//   drainText()         GUI thread, once per frame.
//   pollImages()        GUI thread, once per frame.
//
// IMAGE DECODERS LIVE HERE, not in PluginUi where they were first created.
// They consume samples like every other decoder - the only difference is that
// what comes back is a picture instead of a line of text - and a decoder that
// is never handed a sample cannot produce anything at all.
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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/host_image.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "dsp/resampler.hpp"

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
    // A NEGATIVE RETURN FROM poll_text OR poll_image, which the ABI defines as
    // "the decoder has failed permanently; the host stops polling it"
    // (plugin_abi.h, CascadeDecoderApi::poll_text and
    // CascadeImageDecoderApi::poll_image). The runner used to collapse that
    // into "nothing pending" at every one of the four poll sites: no status
    // line changed, nothing was written to the decoder log, and the runner
    // went on feeding samples to a decoder that had given up. An image row
    // therefore said WAIT for ever, which is the most misleading thing the
    // panel can say - a decoder that has failed looks exactly like one still
    // building a picture, and only one of the two is worth waiting for.
    PollFailed,
};

// What a running instance produces. Text and image decoders are fed
// identically and differ only in what comes back out, so the status panel says
// which - "installed and silent" reads very differently once you know the
// plugin's output is a picture that takes a minute to build.
enum class DecoderOutput {
    Text = 0,
    Image,
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
    // The module file name (pluginKey), because `plugin` is a DISPLAY name and
    // two installed modules may legitimately carry the same one - a plugin
    // upgraded in one directory while an older copy sits in another is the
    // ordinary case. Anything that has to act on the plugin this line is about
    // (isFeeding, and through it the audio mute) has to ask by identity.
    std::string key;
    DecoderIdleReason reason = DecoderIdleReason::Running;
    DecoderStream stream = DecoderStream::None;
    DecoderOutput output = DecoderOutput::Text;
    double wantRateHz = 0.0;  // meaningful for RateMismatch
    std::string detail;       // ready-to-display sentence
};

class PluginRunner {
public:
    PluginRunner() = default;
    ~PluginRunner();

    // Per-instance resampling state; public only so the file-local helpers
    // in plugin_runner.cpp can build and drive it. See the note above
    // Instance for why it exists.
    struct AudioResample {
        std::unique_ptr<cascade::dsp::RationalResampler> resampler;
        std::vector<float> out;
        double rateHz = 0.0;  // the decoder's rate; 0 when no resampling
    };

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

    // The plugins the user has STOPPED, by module file name. Applied by the
    // next rebuild(): a stopped plugin gets no instance, is fed no samples,
    // and produces no line of text or picture, while its module stays mapped
    // and its row stays on the panel.
    //
    // A SETTER RATHER THAN AN ARGUMENT to rebuild, and the set SURVIVES
    // clear(), because a stop is a decision about a plugin and not about one
    // set of instances. rebuild() is called on every source change, and
    // clear() runs before every rescan; if the set travelled as an argument or
    // were dropped by clear(), a plugin the user stopped would come back to
    // life the first time they changed the sample rate.
    // Takes the same lock rebuild() does: both run on the control thread
    // today, but a set that is read under a lock and written without one is a
    // race waiting for the first caller who forgets that.
    void setStopped(std::vector<std::string> keys);
    bool isStopped(const std::string& pluginKey) const;

    // Whether this plugin has at least one instance the runner is ACTUALLY
    // FEEDING - created, and matched to the rate the pipeline is delivering.
    //
    // WHY THIS IS NOT "IS IT STOPPED". They differ in exactly the case that
    // matters: a plugin the user never stopped, whose required rate the
    // receiver is not producing, is idle and says so in orange on its own row.
    // The audio mute used the stop list alone and therefore silenced the
    // speakers on behalf of such a plugin - reproduced on the running
    // application with the generator at 2 MS/s and the receiver on 162.000
    // MHz, where the Sinks panel read "Muted by AIS" while the Plugins panel
    // read "\"AIS\" needs 192000 Hz raw I/Q and the receiver is producing
    // 2000000 Hz, so it is not being fed". Two panels contradicting each other,
    // and the sound taken away for a decoder the application itself said was
    // doing nothing.
    //
    // Answered from the same status list the panel draws, so the two can never
    // disagree again: whatever the row says is idle, this says is not feeding.
    bool isFeeding(const std::string& pluginKey) const;

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

    // ----- The plugin audio path (CASCADE_CAP_AUDIO_OUT) --------------------
    //
    // DSP thread, real-time, and the SAME thread as processAudio/processIq -
    // which is the whole reason the ABI can promise a plugin that pull() is
    // never concurrent with process() on its handle. Serialised with them by
    // the same lock either way, so a rebuild still cannot destroy a handle a
    // pull is inside.
    //
    // Fills `left` and `right` with `frames` of the playing plugin's audio at
    // the audio rate rebuild() was given, resampled and channel-converted, and
    // returns TRUE. Returns FALSE when no plugin wants the speakers, leaving
    // both buffers untouched - the caller keeps its demodulated audio.
    //
    // A plugin that has nothing to hand over right now still counts as
    // playing: the shortfall is filled with silence and charged as a gap (see
    // audioGaps below), because a decoder between superframes has not stopped
    // being the thing the user is listening to.
    bool pullPluginAudio(float* left, float* right, std::size_t frames);

    // Which plugin currently holds the speakers, by descriptor name; empty
    // when the demodulated audio is playing. This is what the Sinks card and
    // /api/status read - any thread, cheap.
    std::string playingPlugin() const;
    // ...and its module file name, for anything that has to act on the plugin
    // rather than print it (the same identity every other per-plugin decision
    // is keyed on - see pluginKey).
    std::string playingPluginKey() const;

    // Blocks in which the playing plugin handed over less than a full block,
    // and how many frames of silence that cost. Cumulative since the last
    // rebuild. The sibling of AudioOut::underruns() for the plugin path: a
    // decoder that is playing but cannot keep up sounds exactly like a device
    // that is starving, and without these two numbers side by side there is no
    // way to tell which of the two is happening.
    std::uint64_t audioGaps() const;
    std::uint64_t audioGapFrames() const;

    // GUI thread. Writes the diagnostics the audio path recorded: one line per
    // takeover transition, and a once-a-minute gap digest when there is
    // anything to say.
    //
    // CALLED BY drainText(), which the GUI already calls every frame, so this
    // needs no new wiring to work - and is public so the status card can call
    // it explicitly if the frame loop ever stops draining text. The real-time
    // path only records fixed-size events; the formatting, the clock and the
    // log write all happen here, on a thread that is allowed to do them.
    void pollAudioDiagnostics();

    // GUI thread. Moves out whatever has been decoded since the last call.
    std::vector<DecodedLine> drainText();

    // GUI thread. Refreshes `out` from the image decoders: one entry per image
    // decoder instance, in rebuild order, each carrying the newest picture that
    // decoder has produced.
    //
    // `out` BELONGS TO THE CALLER and persists across calls, which is the whole
    // design. An entry is rewritten only when its decoder offers a new image,
    // so a megapixel weather frame is copied once per change rather than once
    // per rendered frame, and the GUI keeps drawing the last picture in between
    // without asking the plugin for it again.
    //
    // The plugin's poll_image/release_image pair runs under the same lock the
    // DSP thread takes, because the borrow must not overlap a process() call on
    // that instance. The copy therefore happens with the lock held; it is only
    // paid when an image actually changed.
    void pollImages(std::vector<HostImage>& out);

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
    // THE RATE A DECODER ASKED FOR IS THE RATE IT GETS. The ABI promises "the
    // host resamples to it", and until 0.83.0 the host did not: a decoder
    // asking for anything but the pipeline's own audio rate was idled with a
    // RateMismatch, which stranded every decoder built around a different
    // clock (a 16 kHz tone decoder, a 22.05 kHz pager decoder). Now each such
    // instance owns a rational resampler from the pipeline rate to its own,
    // fed in processAudio, and a mismatch is a thing the runner does rather
    // than a thing it reports. The scratch buffer is sized on the first block
    // and grows only if a larger block ever arrives, so the audio thread does
    // not allocate per call.
    struct Instance {
        const CascadeDecoderApi* api = nullptr;
        void* handle = nullptr;
        std::string name;
        AudioResample resample;
        // poll_text writes no NUL and splits only on code-point boundaries,
        // so a line can arrive across two polls. Carried here between calls.
        std::string partial;
        // WHICH status_ ROW THIS INSTANCE IS REPORTED THROUGH. Fixed when the
        // instance is created and valid for exactly as long as the instance
        // is: the only thing that empties status_ is destroyInstances(), which
        // empties these vectors in the same breath, so the index cannot come
        // to point at another plugin's row.
        std::size_t statusIndex = 0;
        // Set once, when a poll returns negative - the ABI's "failed
        // permanently". From then on the instance is handed no samples and
        // polled no further, but its handle is KEPT until the next
        // rebuild()/clear() so destroy() still runs exactly once, after the
        // last call of anything else on that handle, as the ABI requires.
        bool failed = false;
    };

    // Same two trailing fields as Instance above, for the same reasons.
    struct IqInstance {
        const CascadeIqDecoderApi* api = nullptr;
        void* handle = nullptr;
        std::string name;
        std::string partial;
        std::size_t statusIndex = 0;
        bool failed = false;
    };

    // An image decoder takes EITHER stream (SSTV wants demodulated audio, LRPT
    // wants complex baseband), so unlike the two above, the instance has to
    // carry which one it asked for.
    struct ImageInstance {
        const CascadeImageDecoderApi* api = nullptr;
        void* handle = nullptr;
        std::string name;
        std::string partial;
        std::uint32_t inputKind = CASCADE_INPUT_AUDIO;
        AudioResample resample;  // audio-input image decoders only
        std::size_t statusIndex = 0;
        // ONE FLAG FOR BOTH POLLS. A permanent failure is a property of the
        // instance and not of the call that reported it, so an image decoder
        // whose poll_text gives up stops being asked for pictures too.
        bool failed = false;
    };

    // ONE PLUGIN'S CLAIM ON THE SPEAKERS, riding on a decoder instance.
    //
    // It holds no handle of its own because CASCADE_CAP_AUDIO_OUT has no
    // create(): `handle` is borrowed from whichever decoder instance this
    // plugin produced, and destroyInstances() must therefore drop these BEFORE
    // it destroys the instances they point at.
    //
    // Everything else here exists so the real-time pull allocates nothing. The
    // plugin's rate is almost never the sink's, and a resampler emits a
    // variable number of samples per call while the sink needs an exact block,
    // so the converted audio lands in a small FIFO and the block is taken from
    // that. `fifoHead` is an index rather than an erase: at 48 kHz this runs
    // every few milliseconds for as long as the user is listening.
    struct AudioInstance {
        const CascadeAudioOutApi* api = nullptr;
        void* handle = nullptr;  // borrowed from a decoder instance
        std::string name;
        std::string key;
        std::uint32_t rateHz = 0;
        std::uint32_t channels = 1;
        // Set when pull() returns negative - the ABI's "stopped producing for
        // good". The instance is never pulled or asked active() again.
        bool stopped = false;
        // Said once, when this instance asked for the speakers while another
        // already had them. Cleared when it stops asking, so a plugin that
        // contends again after a real change of mind is heard from again.
        bool contentionLogged = false;
        // One resampler per output channel, from rateHz to the sink rate; null
        // when the two agree. Two of them for the same reason the pipeline has
        // two: a resampler is a state machine and one instance cannot carry
        // two signals.
        std::unique_ptr<cascade::dsp::RationalResampler> rsL;
        std::unique_ptr<cascade::dsp::RationalResampler> rsR;
        std::vector<float> pullBuf;  // interleaved, handed to the plugin
        std::vector<float> inL, inR;
        std::vector<float> fifoL, fifoR;
        std::size_t fifoHead = 0;
    };

    // Recorded by the real-time path, written out by pollAudioDiagnostics on
    // the GUI thread. FIXED STORAGE and a fixed-size array, because the thread
    // that fills one of these is not allowed to allocate - a std::string here
    // would put a heap call in the audio path for the sake of a log line.
    struct AudioEvent {
        enum class Kind { Started, Stopped, Contended } kind = Kind::Started;
        char name[64] = {0};
        // Whoever has the speakers, for a Contended event - a line that names
        // only the loser leaves the user hunting for which plugin to stop.
        char holder[64] = {0};
        std::uint32_t rateHz = 0;
        std::uint32_t channels = 0;
        std::uint32_t sinkRateHz = 0;
    };
    static constexpr std::size_t kMaxAudioEvents = 8;

    // Enough converted audio to absorb one large block plus the ragged
    // remainder a resampler leaves. Sized once, at rebuild.
    static constexpr std::size_t kAudioFifoFrames = 16384;

    // No plugin is playing. std::size_t, not -1 in an int, because it indexes
    // audioInstances_.
    static constexpr std::size_t kNoAudio = static_cast<std::size_t>(-1);

    // Fills one instance's FIFO and takes `frames` out of it. Called with the
    // lock held, from the real-time path only.
    void pullAudioLocked(AudioInstance& a, float* left, float* right, std::size_t frames);
    // Records a transition for pollAudioDiagnostics. Lock held, no allocation.
    // `holder` is the instance currently holding the speakers, for a Contended
    // event, and null otherwise.
    void noteAudioEventLocked(AudioEvent::Kind kind, const AudioInstance& a,
                              const AudioInstance* holder);

    // A plugin is third-party code: an absurd width/height must not make the
    // host try to allocate it. CASCADE_IMAGE_MAX_DIM bounds each side; this
    // bounds the product, which is what actually gets allocated.
    static constexpr std::size_t kMaxImagePixels = 64u * 1024u * 1024u;

    // DESTROYS EVERY INSTANCE WITH THE LOCK DROPPED, which is why it takes the
    // caller's lock rather than assuming one is held.
    //
    // A PLUGIN'S destroy() IS THIRD-PARTY CODE THAT MAY CALL THE HOST BACK.
    // Survey Engine 0.1.0 finishes the dwell in progress from destroy(), which
    // asks the host for the time and, when it is sweeping, asks it to retune;
    // the host's tune service ends in AppWindow::applyRetuneNow, which calls
    // PluginRunner::retune, which takes this same mutex_. Held across
    // destroy(), that is a non-recursive mutex re-locked on its own thread:
    // libc++ aborts (reported from Android on every exit of the app) and
    // MSVC's throws "resource deadlock would occur" out of the plugin's
    // destroy, unwinding through the plugin boundary into std::terminate.
    //
    // So the instances are MOVED OUT under the lock - after which the runner
    // holds nothing, and the DSP thread, should it take the lock meanwhile,
    // correctly finds nothing to feed - the lock is dropped, destroy() runs on
    // the moved-out copies, and the lock is retaken before returning, so a
    // caller sees exactly the state a function that never let go would leave.
    void destroyInstances(std::unique_lock<std::mutex>& lock);
    void pollLocked();
    void pollIqLocked();
    // Status text from the image decoders fed by `inputKind`, into the same
    // line queue as everything else: an image decoder reports its progress and
    // its failures as text, and a second log for it would only hide them.
    void pollImageTextLocked(std::uint32_t inputKind);
    // Shared by both poll paths: appends `bytes` of decoder output to
    // `partial`, emits every complete line, keeps any tail. One
    // implementation, because two would eventually disagree about where a
    // line ends.
    void absorbLocked(const std::string& name, std::string& partial, const char* data,
                      std::size_t bytes);
    // Records that one instance has failed permanently: the status row it was
    // built with becomes PollFailed with a sentence the panel already knows how
    // to draw, and one line goes into the decoder log so the failure is written
    // where everything else the decoder said is written. Called once per
    // instance, on the transition, by whichever poll saw the negative return.
    void failLocked(std::size_t statusIndex, const std::string& name);

    mutable std::mutex mutex_;
    // Read only inside rebuild(), under the same lock as everything else, so
    // a stop that arrives while the DSP thread is running cannot race the
    // instance vectors it decides the contents of.
    PluginStopSet stopped_;
    std::vector<Instance> instances_;
    std::vector<IqInstance> iqInstances_;
    std::vector<ImageInstance> imageInstances_;
    // How many image instances take each stream. Kept so the real-time path can
    // decide whether it has anything to feed without walking the vector.
    std::size_t audioImageCount_ = 0;
    std::size_t iqImageCount_ = 0;
    std::vector<AudioInstance> audioInstances_;
    // Index into audioInstances_ of the plugin holding the speakers, or
    // kNoAudio. FIRST WINS: the search runs in rebuild order and stops at the
    // first instance that says it is active, so two plugins asking at once is
    // decided by load order and not by which block the question was asked in.
    std::size_t playing_ = kNoAudio;
    std::array<AudioEvent, kMaxAudioEvents> audioEvents_{};
    std::size_t audioEventCount_ = 0;
    std::uint64_t audioGaps_ = 0;
    std::uint64_t audioGapFrames_ = 0;
    // What pollAudioDiagnostics last reported, so the once-a-minute digest
    // reports the minute rather than the session. GUI thread only.
    std::uint64_t audioGapsReported_ = 0;
    std::uint64_t audioGapFramesReported_ = 0;
    std::chrono::steady_clock::time_point audioDigestAt_{};
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
