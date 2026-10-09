// PortAudio float32 audio output sink (mono or interleaved stereo).
//
// CHANNELS. The sink opens either one or two channels; the layout is fixed at
// open() and reported by channels(). Two channels are INTERLEAVED L,R,L,R…
// in the ring, exactly as PortAudio wants them in the callback buffer, so the
// realtime path stays a straight ring read with no de-interleaving. The
// producer side is what has to be careful: a partial frame accepted by the
// ring would shift every later sample by one and swap the channels for the
// rest of the session, so writeStereo() rounds its request down to whole
// frames against the ring's free space instead of letting write() split one.
//
// Threading model, and why the pieces are shaped the way they are:
//
//   DSP / feed thread            PortAudio callback thread
//   ─────────────────            ─────────────────────────
//   write(samples, n) ──SPSC──▶  pullBlock(this, out, frames)
//                      ring
//
// write() is the producer side of a cascade::dsp::SpscRing<float> and never
// blocks: it accepts what fits and reports the count, so a slow or stalled
// audio device can never back-pressure the DSP thread into missing IQ input.
// The PortAudio callback is the consumer side and does nothing except call
// pullBlock() — a ring read, a volume multiply, and a zero-fill when the ring
// is starved. No locks, no allocation, no I/O on that path: the callback runs
// on a realtime audio thread where a blocked mutex or a heap call is an
// audible dropout.
//
// pullBlock() is public and static (taking the object through void*) so tests
// can exercise the exact code the callback runs — starvation, partial fills,
// volume — headless, without opening a real device.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "core/health_events.hpp"
#include "dsp/spsc_ring.hpp"

namespace cascade::sink {

// One selectable output device, as shown in the Sinks panel.
struct AudioDevice {
    int index;       // PortAudio device index, valid for open()
    std::string name;
    bool isDefault;  // true for the host API's default output device
};

class AudioOut {
public:
    // Calls Pa_Initialize(). PortAudio itself refcounts paired
    // Initialize/Terminate calls, so multiple AudioOut instances are safe;
    // the guard here is remembering whether OUR Initialize succeeded so the
    // destructor never calls Pa_Terminate() unmatched (the PortAudio docs
    // forbid Terminate after a failed Initialize).
    AudioOut();
    ~AudioOut();  // close()s any open stream, then releases PortAudio

    // The object owns a PaStream and live atomics; copying one would either
    // double-close the stream or split the ring between two owners.
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Every device with at least one output channel, in PortAudio index
    // order. Empty if PortAudio failed to initialize.
    std::vector<AudioDevice> listOutputDevices();

    // Opens deviceIndex (-1 = system default output) as a float32 callback
    // stream at sampleRateHz with `channels` channels (1 = mono, 2 =
    // interleaved stereo; anything else is refused) and starts it. An
    // already-open stream is closed first, so open() doubles as "switch
    // device / rate / layout". Returns false on any PortAudio failure,
    // leaving the object closed.
    //
    // The default argument keeps the original two-argument mono call site
    // (and its tests) behaving exactly as before.
    bool open(int deviceIndex, double sampleRateHz, int channels = 1);

    // Stops and releases the stream. Idempotent: safe to call twice, or
    // without a successful open().
    void close();

    bool running() const { return running_.load(std::memory_order_relaxed); }

    // True while the callback is playing from the ring rather than building
    // its lead (see pullBlock's PRIMING note). The producer's DriftMatcher
    // reads it: only a playing ring's fill level says anything about the
    // clocks.
    bool primed() const { return primed_.load(std::memory_order_relaxed); }

    // True while a stream is open AND PortAudio still reports it running.
    //
    // NOT the same thing as running(), and the difference is the whole reason
    // this exists. running_ is our own bookkeeping: it says "we opened a
    // stream and have not closed it". The stream underneath can die without
    // anyone telling us — a USB audio device that re-enumerates takes its
    // stream with it, and re-enumeration is not rare (unplugging the device,
    // or binding a driver to ANY device on the same controller, which is
    // exactly what installing WinUSB for an SDR dongle does). PortAudio has
    // no callback for it: the audio callback simply stops being called.
    //
    // The visible symptom is total silence with a perfectly healthy-looking
    // receiver — spectrum, waterfall and squelch all live, because they sit
    // upstream of the sink — and a starvation counter frozen at whatever it
    // reached before the stream died. Nothing recovers it on its own, so
    // something has to ask; this is the question.
    bool streamAlive() const;

    // Whether ANY open has ever succeeded on this object. Recovery uses it to
    // tell "the stream died" from "there was never a device" — on a headless
    // box the second is normal and must not provoke endless reopen attempts.
    bool everOpened() const { return everOpened_.load(std::memory_order_relaxed); }

    // The deviceIndex argument of the last successful open, verbatim: -1 when
    // the caller asked for the system default. Kept UNRESOLVED on purpose —
    // "follow the default" is a different intent from "use this device", and
    // a recovery must preserve which one the user expressed.
    int openedDeviceRequested() const {
        return openedRequested_.load(std::memory_order_relaxed);
    }

    // Name of the device the last successful open actually landed on. Names
    // are the only stable handle across a device coming and going: PortAudio
    // indices are positions in a list that shifts when the set changes.
    // BY VALUE, not by reference: open() rewrites this string, and since
    // 0.96.5 open() may be running on a worker thread (see gui/audio_open.hpp)
    // while the GUI asks. A reference into a string another thread is
    // assigning is a dangling read; a copy taken under the api lock is not.
    // Empty while an open is in progress - see streamAlive() for why that is
    // answered rather than waited for.
    std::string openedDeviceName() const;

    // The host API the last successful open went through ("MME", "Windows
    // WASAPI", "Windows DirectSound"...), empty until one has. By value under
    // the same try_lock as openedDeviceName(), and empty while an open is in
    // progress for the same reason. A host API names a driver model, never a
    // person's device, so unlike the device's name it is safe to put in the
    // log and in the diagnostics bundle - and it is the first thing anyone
    // asks about a Windows audio fault.
    std::string openedHostApi() const;

    // Channel layout of the most recent SUCCESSFUL open (1 until one
    // succeeds). Deliberately retained across close(): it describes how the
    // ring's contents are laid out, and the ring outlives the stream.
    int channels() const { return channels_.load(std::memory_order_relaxed); }

    // Non-blocking producer push of raw ring samples. Returns how many
    // samples the ring accepted (< n when the ring is full — the caller
    // drops or retries; this thread is never made to wait on the audio
    // device). On a stereo sink `samples` must be interleaved and n a
    // multiple of 2 — prefer writeStereo(), which cannot split a frame.
    std::size_t write(const float* samples, std::size_t n);

    // Interleaved stereo push, in FRAMES. Never accepts a partial frame (see
    // the CHANNELS note in the file header): the request is capped at the
    // ring's whole-frame free space, so channel order can never shift.
    // Returns the number of frames accepted.
    std::size_t writeStereo(const float* interleaved, std::size_t frames);

    // Cumulative count of starved callbacks (one per pullBlock that could
    // not fully fill its buffer), monotonic over the object's lifetime. A
    // starved callback only counts once PRIMED playback has actually begun —
    // see kPrimeFrames below — so this is "audible stutters", not every
    // callback that ever touched an empty ring.
    std::uint64_t underruns() const {
        return underruns_.load(std::memory_order_relaxed);
    }

    // Cumulative count of callbacks served while NOT primed (silence, no
    // underrun charged). Exposed so the priming gap is visible on its own
    // terms rather than hiding inside a flat underrun count: a device that
    // reopens often shows a climbing priming count with underruns barely
    // moving, which is a different story from one that is actually
    // starving in steady state.
    std::uint64_t primingCallbacks() const {
        return primingCallbacks_.load(std::memory_order_relaxed);
    }

    // How many frames the ring currently holds / can hold, in FRAMES (a
    // stereo frame is one L+R pair). For the status card and the remote
    // JSON, which both want "ring X of Y ms" rather than a raw sample count
    // the caller would have to divide by channels() itself.
    std::size_t ringFrames() const;
    std::size_t ringCapacityFrames() const;

    // Output gain, clamped to [0, 1]. Stored in an atomic and applied inside
    // the callback, so the GUI thread can move a slider while audio runs
    // without a lock. One gain for the whole stream, applied by the same
    // per-sample multiply to every channel, so a stereo image can never be
    // tilted by the volume control.
    void setVolume(float v01);

    // The callback core. `frames` is what PortAudio hands the callback, so
    // the buffer it fills is frames * channels() floats.
    //
    // PRIMING. Playback starts on the first callback with an empty ring —
    // steady state then hovers near-empty, and ordinary producer jitter (the
    // DSP thread is not a hard-realtime source) starves a callback every
    // time it runs late by more than what's left in the ring. So pullBlock
    // keeps a "primed" latch: while not primed, every callback plays SILENCE
    // and charges NO underrun (there was never a promise of audio yet), and
    // primes the instant the ring holds leadFrames() or more (kPrimeFrames
    // until setLeadFrames() says otherwise) — at which point
    // THAT SAME callback plays real audio (the lead is already there to
    // support it). Once primed, a starved callback zero-fills the shortfall,
    // charges exactly one underrun as before, and drops back to unprimed:
    // rather than keep stuttering against a ring that is still catching up,
    // it takes one short silent gap to rebuild the lead before playback
    // resumes. See primingCallbacks() for how often the silent side of this
    // fires.
    //
    // Returns the number of SAMPLES that came from the ring (0 while
    // unprimed), which for a mono sink equals the frame count when the ring
    // kept up (so the original mono contract is unchanged in that case).
    // Static + void* so it is callable both from the C callback and from
    // tests; it must stay lock-free and allocation-free (see file header).
    static std::size_t pullBlock(void* self, float* dst, std::size_t frames);

    // 120 ms of frames at the sink rate (Pipeline::kAudioRateHz = 48 kHz):
    // the lead pullBlock demands before it starts playing, AS A NEW SINK
    // STARTS - since 0.99.73 it is the default of a runtime value (leadFrames()
    // below), not a constant the callback compares against. Chosen to clear
    // the ~64 ms worst-case producer gap measured against a USRP B200 with
    // margin, while staying well inside the ring (see kRingCapacity below).
    // Public so tests can prime a ring to the exact threshold instead of
    // guessing at it.
    static constexpr std::size_t kPrimeFrames = 5760;

    // THE LEAD IS A RUNTIME VALUE (0.99.73), held per instance. The 12CF report
    // (110 to 126 starved callbacks a minute on a slow machine) showed that a
    // fixed 120 ms is not enough everywhere, and that a deeper lead is useless
    // unless the DriftMatcher's target moves with it - a matcher still steering
    // the fill to 160 ms bleeds a 480 ms lead back down. So the AudioOut owns
    // both: the lead pullBlock demands before it plays, and targetFrames(), the
    // fill the producer's matcher steers to, which is always the lead plus
    // kTargetMarginFrames (today's 120 -> 160 ms relation kept).
    //
    // setLeadFrames() is for the GUI thread (never the callback). A raise
    // changes the matcher's target and takes effect at the next prime after an
    // underrun. A decrease also requests a callback-owned trim: on its next
    // invocation the callback drops the oldest samples above the new target
    // and re-primes from the retained sound. The setter never reads the ring.
    // Clamped to [kPrimeFrames, kMaxLeadFrames].
    static constexpr std::size_t kTargetMarginFrames = 1920;  // 40 ms
    // 960 ms: the ceiling. With the ring at 1 << 17 samples it leaves 400 ms of
    // headroom in the narrower, stereo case (see kRingCapacity).
    static constexpr std::size_t kMaxLeadFrames = 46080;
    void setLeadFrames(std::size_t frames);
    std::size_t leadFrames() const { return leadFrames_.load(std::memory_order_relaxed); }
    // The fill, in frames, the producer's DriftMatcher should steer to.
    std::size_t targetFrames() const { return leadFrames() + kTargetMarginFrames; }

private:
    // The body of close(), for the paths that already hold apiMutex_ (open()
    // closes the previous stream before it opens the next one).
    void closeLocked();

    // What an open() has to say about itself, composed under apiMutex_ and
    // WRITTEN after it is released: a log write is a disk write, and the
    // stream lifecycle lock is the one every other query try_locks on.
    struct OpenNote {
        std::string line;  // empty: nothing new to say (a repeated refusal)
        bool warn = false;
        // THE ANONYMOUS COUNT OF A REFUSAL (0.99.64, core/health_events.hpp):
        // set on EVERY refusal, including a repeated one whose log line is
        // suppressed - the ledger keeps one per session, the log keeps one a
        // minute. The class is read off PortAudio's error CODE, never its text.
        bool failed = false;
        cascade::core::health::SoundReason reason = cascade::core::health::SoundReason::Other;
        cascade::core::health::AudioApi api = cascade::core::health::AudioApi::None;
    };
    // The body of open() once apiMutex_ is held. Fills `note`.
    bool openLocked(int deviceIndex, double sampleRateHz, int channels, OpenNote& note);

    // 131072 samples: 131072 mono frames (2.73 s at 48 kHz) or 65536 STEREO
    // frames (1.37 s - a stereo frame is one L+R pair, so it costs two
    // samples). It was 1 << 15 (341 ms in stereo) until 0.99.73, when the lead
    // became adjustable: the deepest lead (kMaxLeadFrames, 960 ms) plus the
    // matcher's margin leaves 365 ms of headroom in the stereo case, so no
    // resize is ever needed while audio runs. 512 KiB per sink. Power of two as
    // SpscRing requires.
    static constexpr std::size_t kRingCapacity = std::size_t{1} << 17;

    dsp::SpscRing<float> ring_;
    std::atomic<float> volume_{1.0f};
    std::atomic<std::uint64_t> underruns_{0};
    // The lead pullBlock demands before it plays, in frames. Read by the
    // callback on every unprimed block, written by setLeadFrames(): it must be
    // lock-free (asserted in the .cpp).
    std::atomic<std::size_t> leadFrames_{kPrimeFrames};
    // Set by the control thread on a decrease, consumed by the callback. Only
    // the callback may discard from ring_, preserving its single reader.
    std::atomic<bool> trimPending_{false};
    // Starts false: playback is unprimed until the first callback observes
    // leadFrames_ or more in the ring, exactly as a fresh stream should be.
    std::atomic<bool> primed_{false};
    std::atomic<std::uint64_t> primingCallbacks_{0};
    // PaStream*, stored as void* so this public header does not force
    // portaudio.h onto every includer; audio_out.cpp casts at the API line.
    void* stream_ = nullptr;
    bool paOk_ = false;    // did OUR Pa_Initialize() succeed?
    std::atomic<bool> running_{false};
    // Layout of the last successful open (see channels()). ATOMIC because the
    // PortAudio callback reads it on every block while open() writes it.
    std::atomic<int> channels_{1};
    // THE STREAM LIFECYCLE LOCK. open() and close() hold it for their whole
    // duration - which on Windows means for the whole of waveOutOpen - and the
    // queries that touch PortAudio or the identity strings take it with
    // try_lock, so a caller that asks about the sink while another thread is
    // opening it gets an answer instead of a stall or a race. It is NEVER
    // taken by write()/writeStereo()/pullBlock(): the realtime path stays
    // lock-free, exactly as the file header promises.
    mutable std::mutex apiMutex_;
    // Identity of the last successful open, retained across close() because
    // that is precisely when recovery needs it (see streamAlive()).
    std::atomic<bool> everOpened_{false};
    std::atomic<int> openedRequested_{-1};
    std::string openedName_;     // guarded by apiMutex_
    std::string openedHostApi_;  // guarded by apiMutex_
    // THE LAST REFUSAL THAT WAS SAID, and when. A refusal that repeats - the
    // audio watchdog retries a dead stream once a second - is written once and
    // then at most once a minute: the log's ring holds 256 lines, and a line
    // per retry would push the whole session out of a report in four minutes.
    // A refusal for a different reason is a new fact and is always written; so
    // is the first one after a success. Guarded by apiMutex_.
    std::string lastRefusal_;
    std::chrono::steady_clock::time_point lastRefusalSaid_{};
};

// Which device a recovery reopen should target, given what the last
// successful open asked for and what is present now. Returns a PortAudio
// device index, or -1 for "the system default".
//
// Matching is BY NAME, not by index, because an index is a position in a list
// that renumbers whenever the set of devices changes. Reopening a stale index
// after a device disappeared does not merely fail — it can succeed against a
// different device that inherited the number, moving the user's audio
// somewhere they never chose, which is a worse outcome than the silence being
// recovered from.
//
// A last open that asked for -1 stays -1: "follow the system default" is an
// intent to preserve, not a device to pin. If the named device is gone, the
// default is the only honest fallback — sound somewhere beats sound nowhere,
// and the caller says so in the UI.
int recoveryDeviceIndex(int lastRequested, const std::string& lastName,
                        const std::vector<AudioDevice>& present);

// Makes a remembered ROW of a device list safe to subscript against the list
// as it is NOW. Returns -1 only when `present` is empty.
//
// Different question from recoveryDeviceIndex above, which answers "which
// DEVICE should be reopened" in PortAudio index space. This one is about the
// combo's selection, which is a POSITION in the vector - and the vector is
// re-enumerated every time the watchdog finds the stream dead, precisely
// because a device disappeared. A list that SHRINKS leaves the remembered
// position past the end, and the Sinks combo subscripts it unguarded on the
// very next frame (it only checks that the list is non-empty), so a stale row
// is an out-of-bounds read, not a cosmetic mismatch.
//
// A row that no longer exists falls back to the default device's row, or to
// the first row when nothing is flagged default: whatever is shown may be the
// wrong device, but the panel says so through the watchdog note, and a wrong
// name is survivable where reading past the end of the vector is not.
int clampDeviceRow(int row, const std::vector<AudioDevice>& present);

// ---------------------------------------------------------------------------
// THE POLICY THAT DEEPENS THE LEAD (0.99.73)
//
// The 12CF report: a slow machine starved 110 to 126 callbacks a minute at a
// 120 ms lead, and a bigger buffer is the one thing that can help once the
// producer cannot be made faster. The lead steps up through 120 -> 240 -> 480
// -> 960 ms when a MINUTE closes with kAudioLeadStarvedPerMinute or more
// starved callbacks, and never above 960 ms (which leaves 365 ms of ring
// headroom). A minute with fewer does nothing - in particular there is NO
// stepping down: a machine that fell behind once will again, and a lead that
// came down by itself would put the stutter back; the person can set it back
// in the Sinks rail. A fixed setting (`audioBufferMs` in the config, anything
// but 0) never steps.
//
// Pure, so every transition is a table test (tests/test_audio_lead.cpp). It is
// applied from the GUI thread's minute poll (AppWindow::closeAudioMinute), never
// from the audio callback.
// ---------------------------------------------------------------------------

inline constexpr int kAudioLeadStepsMs[4] = {120, 240, 480, 960};
inline constexpr int kAudioLeadStarvedPerMinute = 3;

// Milliseconds <-> frames at the sink's 48 kHz rate (Pipeline::kAudioRateHz).
constexpr std::size_t audioLeadFrames(int ms) { return static_cast<std::size_t>(ms) * 48u; }
constexpr int audioLeadMs(std::size_t frames) { return static_cast<int>(frames / 48u); }

// True for 0 (automatic) and for each of the four steps - the only values the
// `audioBufferMs` setting may hold.
bool validAudioBufferSetting(int ms);

// The lead, in milliseconds, for the minute that has just closed with
// `underrunsLastMinute` starved callbacks while the lead was `currentLeadMs`.
// `configuredMs` is the setting: 0 is automatic; anything else is a fixed value
// and the answer is `currentLeadMs`, unchanged. At the ceiling the answer is
// `currentLeadMs` unchanged as well, whatever it is.
int nextAudioLead(std::uint64_t underrunsLastMinute, int currentLeadMs, int configuredMs = 0);

}  // namespace cascade::sink
