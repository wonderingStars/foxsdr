// AAudio (Android) implementation of sink/audio_out.hpp's AudioOut.
//
// DESIGN DECISION. AudioOut is a concrete class over PortAudio today. Two
// shapes were on the table for a second backend:
//
//   (a) Keep AudioOut's public surface AS the interface, and select the
//       implementation by platform.
//   (b) Extract an abstract IAudioOut and give it two concrete
//       implementations (AudioOutPortAudio, AudioOutAAudio).
//
// (b) was rejected. Every caller of this class — Pipeline (audioOpener(),
// publishAudioChannels(), the mono/stereo write in processAudioBlock()),
// gui::AudioOpen's Opener, and every existing test — takes a concrete
// AudioOut by value, reference or std::shared_ptr<AudioOut>, none of them
// through a pointer to an interface. Turning that into a real interface
// would touch pipeline.hpp/.cpp, audio_open.hpp and every test that
// constructs an AudioOut, for a Windows/Linux build that gains nothing from
// it — there is exactly one AudioOut per Pipeline, chosen once at compile
// time by which .cpp file the platform links, never at runtime. A runtime-
// polymorphic interface is the right tool for "many implementations,
// selected while running"; this is "one implementation, selected while
// building", which is what (a) already is.
//
// (a) itself had two ways to write it: one file with `#if defined(__ANDROID__)`
// blocks inside audio_out.cpp, or two files. Two files won, because the task
// this exists for is explicit about it and the reason holds up: it is the
// only shape that leaves audio_out.cpp — the file every existing Windows and
// Linux test already exercises — completely unedited. Every line PortAudio's
// backend runs today still lives in exactly the bytes it lived in before
// this change; nothing here can regress it, because nothing here shares a
// translation unit with it. The cost is duplicating a few dozen lines of
// ring/priming/underrun bookkeeping (write, writeStereo, setVolume,
// pullBlock, ringFrames, ringCapacityFrames, openedDeviceName, the
// constructor and destructor) between the two files rather than sharing one
// copy — accepted deliberately, per the task's own guidance, in preference
// to editing audio_out.cpp to extract them. Nothing here diverges from
// audio_out.cpp's versions of those methods; if the two ever need to differ,
// that is the day to reconsider.
//
// WHY THIS FILE STILL COMPILES ON A HOST WITH NO ANDROID NDK. The methods
// above — the shared ring/priming/underrun core — touch no AAudio type at
// all, so they are defined UNCONDITIONALLY in this file, exactly as they
// read in audio_out.cpp. Only the methods that must call an AAudio entry
// point (open, close's real body, streamAlive, listOutputDevices, and the
// two callback trampolines AAudio calls directly) sit behind
// `#if defined(__ANDROID__)` and pull in <aaudio/AAudio.h>. That split is
// what makes tests/test_audio_out_aaudio.cpp possible at all: it compiles
// this exact file on Linux, with no NDK on the include path, and drives the
// ring/priming/underrun logic — and both callback CORES, since
// AudioOut::pullBlock and AudioOut::androidErrorCallback are themselves in
// the unconditional half — headlessly, the same way tests/test_audio_out.cpp
// drives PortAudio's pullBlock with no PaStream. What it cannot reach from
// Linux is open()/close()/streamAlive()/listOutputDevices() themselves (they
// do not exist in this translation unit without __ANDROID__) or the two
// trampoline FUNCTIONS AAudio calls with its own C signature — those are
// two-line casts with nothing left to unit-test once
// androidStreamStateIsAlive() (audio_out_aaudio.hpp) and pullBlock/
// androidErrorCallback are proven; see that header for the state-decision
// seam and this file's arm64 build for the actual AAudio wiring.
//
// THE THREE AAUDIO BEHAVIOURS THE TASK CALLS FOR, and how each is met:
//
//   1. THE DATA CALLBACK drains the SAME ring the DSP thread writes into,
//      with underrun counting identical to PortAudio's, because it does not
//      reimplement that logic — trampolineData() below calls
//      AudioOut::pullBlock() directly, the very code this file also defines
//      for a host build to test.
//
//   2. THE ERROR CALLBACK marks the stream dead rather than reopening from
//      inside the callback — AAudio's own documented contract for
//      AAudioStream_errorCallback (this NDK's AAudio.h, verified below)
//      FORBIDS calling requestStop(), requestPause(), close() or
//      waitForStateChange() from it:
//
//        "The following may NOT be called from the error callback:
//         AAudioStream_requestStop(), AAudioStream_requestPause(),
//         AAudioStream_close(), AAudioStream_waitForStateChange(), ...
//         In response, this function should signal or create another thread
//         to stop and close this stream. Do not stop or close the stream,
//         or reopen the new stream, directly from this callback."
//
//      So androidErrorCallback() does the one thing left: set a flag.
//      streamAlive() reads it; the actual reopen happens later, off the
//      AAudio callback thread, exactly the shape the PortAudio backend
//      already uses (see audio_out.hpp's own comment on streamAlive() for
//      why nothing recovers on its own and something has to ask).
//
//   3. CLOSE/TEARDOWN stops and closes with a BOUNDED wait. AAudio documents
//      requestStart()/requestPause()/requestFlush()/requestStop() as
//      "asynchronous" in this header — none of them block. AAudioStream_close()
//      carries no such note and no timeout parameter, and independent
//      accounts of AAudio's own implementation (Oboe's AudioStreamAAudio,
//      and the Legacy-stream path this header's own close() doc admits is
//      "not... fully implemented for MMAP streams... some callbacks may
//      still be in process after this call") describe it joining an
//      internal callback thread — so it is treated the same way this
//      product already treats a vendor call with no cancellation of its own
//      (see src/source/sdrplay_source.hpp's kCallbackDrainWait): the BOUND
//      is on how long closeLocked() waits for the async stop to land before
//      it asks AAudio to free the stream, not on close() itself, because
//      AAudio offers nothing to bound close() with. UNVERIFIED: this desk
//      has no Android device or emulator (no KVM under WSL), so how long a
//      real close() actually takes has never been measured — only compiled
//      and reasoned from the header and public accounts of the
//      implementation.
//
// WHAT DELIBERATELY IS NOT HERE. recoveryDeviceIndex()/clampDeviceRow()
// (audio_out.hpp) are free functions the desktop Sinks combo uses to pick
// which device a watchdog reopen should target and which row a combo should
// show — GUI concepts with no Android consumer in this slice (there is no
// Android app shell yet, and Pipeline itself never calls either function).
// AAudio's single-default-device model needs no equivalent: a recovery
// reopen is just open(-1, rate, channels) again. If an Android shell is
// ever built and wants the desktop's exact recovery shape, that is the day
// to add it — not preemptively here, against a caller that does not exist.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/audio_out.hpp"
#include "sink/audio_out_aaudio.hpp"

#include <chrono>
#include <cstring>

namespace cascade::sink {

// Same three guarantees audio_out.cpp asserts, restated here because this is
// a SEPARATE translation unit and the realtime callback path (pullBlock,
// reached from trampolineData on AAudio's own real-time callback thread) may
// not lock any more on Android than it may on Windows or Linux.
static_assert(std::atomic<float>::is_always_lock_free,
              "volume atomic must be lock-free for the audio callback");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "underrun counter must be lock-free for the audio callback");
static_assert(std::atomic<bool>::is_always_lock_free,
              "primed latch must be lock-free for the audio callback");

// ---------------------------------------------------------------------------
// THE SHARED RING/PRIMING/UNDERRUN CORE — unconditional (no AAudio type
// appears below this line until the __ANDROID__ block), and identical in
// behaviour to audio_out.cpp's versions. See the file header for why this is
// duplicated rather than shared, and tests/test_audio_out_aaudio.cpp for the
// host-side proof that it behaves exactly like the PortAudio backend's copy.
// ---------------------------------------------------------------------------

AudioOut::AudioOut() : ring_(kRingCapacity) {}

AudioOut::~AudioOut() { close(); }

void AudioOut::close() {
    std::lock_guard<std::mutex> lk(apiMutex_);
    closeLocked();
}

std::size_t AudioOut::write(const float* samples, std::size_t n) {
    return ring_.write(samples, n);
}

std::size_t AudioOut::writeStereo(const float* interleaved, std::size_t frames) {
    const std::size_t roomFrames = ring_.freeSpace() / 2;
    if (frames > roomFrames) { frames = roomFrames; }
    if (frames == 0) { return 0; }
    return ring_.write(interleaved, frames * 2) / 2;
}

void AudioOut::setVolume(float v01) {
    if (!(v01 >= 0.0f)) { v01 = 0.0f; }
    if (v01 > 1.0f) { v01 = 1.0f; }
    volume_.store(v01, std::memory_order_relaxed);
}

std::size_t AudioOut::pullBlock(void* self, float* dst, std::size_t frames) {
    auto* ao = static_cast<AudioOut*>(self);
    const std::size_t chan =
        static_cast<std::size_t>(ao->channels_.load(std::memory_order_relaxed) == 2 ? 2 : 1);
    const std::size_t n = frames * chan;

    if (!ao->primed_.load(std::memory_order_relaxed)) {
        ao->primingCallbacks_.fetch_add(1, std::memory_order_relaxed);
        if (ao->ring_.size() < kPrimeFrames * chan) {
            std::memset(dst, 0, n * sizeof(float));
            return 0;
        }
        ao->primed_.store(true, std::memory_order_relaxed);
    }

    const std::size_t got = ao->ring_.read(dst, n);
    const float vol = ao->volume_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < got; ++i) { dst[i] *= vol; }
    if (got < n) {
        std::memset(dst + got, 0, (n - got) * sizeof(float));
        ao->underruns_.fetch_add(1, std::memory_order_relaxed);
        ao->primed_.store(false, std::memory_order_relaxed);
    }
    return got;
}

std::size_t AudioOut::ringFrames() const {
    const std::size_t chan = static_cast<std::size_t>(channels() == 2 ? 2 : 1);
    return ring_.size() / chan;
}

std::size_t AudioOut::ringCapacityFrames() const {
    const std::size_t chan = static_cast<std::size_t>(channels() == 2 ? 2 : 1);
    return ring_.capacity() / chan;
}

std::string AudioOut::openedDeviceName() const {
    std::unique_lock<std::mutex> lk(apiMutex_, std::try_to_lock);
    if (!lk.owns_lock()) { return std::string(); }
    return openedName_;
}

// The one thing AAudio's error callback is allowed to do. See the file
// header's item 2 for the contract this keeps and audio_out.hpp for why this
// is a static method on the shared class rather than a free function: it
// needs androidStreamError_'s private access, the same way pullBlock needs
// the ring's.
void AudioOut::androidErrorCallback(void* self) {
    if (self == nullptr) { return; }
    static_cast<AudioOut*>(self)->androidStreamError_.store(true, std::memory_order_relaxed);
}

// See audio_out_aaudio.hpp for the full contract and the exact literals'
// provenance.
bool androidStreamStateIsAlive(std::int32_t rawState) {
    constexpr std::int32_t kStarting = 3;  // AAUDIO_STREAM_STATE_STARTING
    constexpr std::int32_t kStarted = 4;   // AAUDIO_STREAM_STATE_STARTED
    return rawState == kStarting || rawState == kStarted;
}

}  // namespace cascade::sink

// ---------------------------------------------------------------------------
// THE AAUDIO BACKEND ITSELF — Android only. Everything past this line
// touches <aaudio/AAudio.h> and libaaudio, neither of which exists on
// Windows, Linux or the host side of this WSL box; it is excluded from the
// desktop build by the CASCADE_SINK filter in the top-level CMakeLists.txt
// and built separately for arm64-v8a (see the commit message for the exact
// NDK clang++ invocation, since no CASCADE_ANDROID CMake target exists on
// this branch yet — that lands on branch and-core).
// ---------------------------------------------------------------------------
#if defined(__ANDROID__)

#include <aaudio/AAudio.h>

namespace cascade::sink {

namespace {

// AAudio has exactly one enumerable output device from pure C++ (see
// listOutputDevices() below), so it needs exactly one stable index. 0 rather
// than -1: -1 is open()'s OWN sentinel for "the system default" — the
// argument spelling every existing caller (Pipeline::audioOpener()) already
// uses — and listOutputDevices() should never hand back a value that means
// something different when passed back into open().
constexpr int kAndroidDefaultDeviceIndex = 0;

// How long closeLocked() waits for AAudioStream_requestStop()'s asynchronous
// stop to be reflected in the stream's state before calling
// AAudioStream_close() — see the file header's item 3 for why this bounds
// the WAIT and not close() itself, and why the bound cannot be verified on
// this desk. 500 ms: enough to drain the ring's whole prime-plus-a-callback
// lead (kPrimeFrames is 120 ms at 48 kHz; this is roughly four times that)
// without holding up whatever Android teardown path eventually calls
// close() should the device simply not answer in time — close() still runs
// either way, exactly as PortAudio's Pa_AbortStream() does not wait for a
// device to drain before this backend's closeLocked() moves on.
constexpr std::chrono::milliseconds kCloseWaitForStop{500};

aaudio_data_callback_result_t trampolineData(AAudioStream* /*stream*/, void* userData,
                                              void* audioData, std::int32_t numFrames) {
    // AAudio's own contract for this callback (AAudio.h, just above
    // AAudioStreamBuilder_setDataCallback) forbids allocation, locking,
    // sleeping and file/network I/O — pullBlock already promises exactly
    // that (see audio_out.hpp), which is what makes it safe to call
    // straight from here with nothing in between.
    if (numFrames <= 0) { return AAUDIO_CALLBACK_RESULT_CONTINUE; }
    AudioOut::pullBlock(userData, static_cast<float*>(audioData),
                        static_cast<std::size_t>(numFrames));
    // Always CONTINUE, win or starve: a starvation is recorded as an
    // underrun (streamAlive()/the caller's buffer-health reading is how it
    // becomes visible), never treated as a reason to stop the stream — the
    // same choice PortAudio's trampoline makes by always returning
    // paContinue.
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void trampolineError(AAudioStream* /*stream*/, void* userData, aaudio_result_t /*error*/) {
    AudioOut::androidErrorCallback(userData);
}

}  // namespace

std::vector<AudioDevice> AudioOut::listOutputDevices() {
    // AAudio itself has no output-device enumeration; that lives on
    // android.media.AudioManager (getDevices()/AudioDeviceInfo), reachable
    // only through JNI from Java/Kotlin. This file is pure C++ built by the
    // NDK toolchain with no JNI glue, so there is exactly one row: whatever
    // AAudio's shared-mode output stream resolves to when no device id is
    // requested — which is also the only thing open()'s deviceIndex can mean
    // here (see its own comment). A future Android shell that wants a real
    // device picker needs to route that Java list down through JNI and
    // extend this; out of scope for this sink.
    return {AudioDevice{kAndroidDefaultDeviceIndex, "AAudio default", true}};
}

bool AudioOut::open(int deviceIndex, double sampleRateHz, int channels) {
    if (!(sampleRateHz > 0.0)) { return false; }
    if (channels != 1 && channels != 2) { return false; }
    // Only one device can ever exist here (see listOutputDevices()); refuse
    // anything else exactly as PortAudio's open() refuses an out-of-range
    // index, rather than silently opening the default under a name the
    // caller did not ask for.
    if (deviceIndex != -1 && deviceIndex != kAndroidDefaultDeviceIndex) { return false; }

    // Held for the whole open, exactly as the PortAudio backend's comment on
    // apiMutex_ describes — AAudioStreamBuilder_openStream() and
    // AAudioStream_requestStart() are both synchronous AAudio calls with no
    // documented bound of their own, so a caller asking streamAlive() or
    // listOutputDevices() mid-open gets "busy" (via try_lock) rather than
    // racing this thread for the stream.
    std::lock_guard<std::mutex> lk(apiMutex_);
    closeLocked();

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) {
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSampleRate(builder, static_cast<std::int32_t>(sampleRateHz + 0.5));
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setDataCallback(builder, &trampolineData, this);
    AAudioStreamBuilder_setErrorCallback(builder, &trampolineError, this);

    AAudioStream* stream = nullptr;
    const aaudio_result_t openResult = AAudioStreamBuilder_openStream(builder, &stream);
    // The builder is a config holder independent of the stream it produced —
    // AAudio's own examples delete it immediately after openStream() whether
    // or not that call succeeded.
    AAudioStreamBuilder_delete(builder);
    if (openResult != AAUDIO_OK || stream == nullptr) { return false; }

    // Drain anything left from a previous session, same reasoning as
    // PortAudio's open() (audio_out.cpp): requestStart() has not been called
    // yet, so no callback can be running and this thread is the ring's only
    // reader.
    float scratch[256];
    while (ring_.read(scratch, sizeof(scratch) / sizeof(scratch[0])) != 0) {}

    // A stale error from whatever stream this AudioOut had open before (if
    // any) must not immediately mark the NEW one dead; see
    // androidStreamErrorFlagged()'s comment in audio_out.hpp.
    androidStreamError_.store(false, std::memory_order_relaxed);

    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        AAudioStream_close(stream);
        return false;
    }

    stream_ = stream;
    running_ = true;
    everOpened_ = true;
    openedRequested_ = deviceIndex;
    openedName_ = "AAudio default";
    channels_ = channels;
    return true;
}

void AudioOut::closeLocked() {
    if (stream_ == nullptr) { return; }  // idempotent / close-without-open
    AAudioStream* s = static_cast<AAudioStream*>(stream_);
    // Asynchronous per AAudio.h's own doc comment on requestStop(): this
    // call does not block. See the file header's item 3 for why the wait
    // that follows bounds itself, not AAudioStream_close().
    AAudioStream_requestStop(s);
    aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNKNOWN;
    AAudioStream_waitForStateChange(
        s, AAUDIO_STREAM_STATE_STOPPING, &next,
        static_cast<std::int64_t>(kCloseWaitForStop.count()) * 1000000LL);
    AAudioStream_close(s);
    stream_ = nullptr;
    running_ = false;
}

bool AudioOut::streamAlive() const {
    // AN OPEN IN PROGRESS IS NOT A DEAD STREAM — identical contract and
    // identical reasoning to the PortAudio backend's streamAlive(): a sink
    // busy being opened must report alive, or the audio watchdog opens it a
    // second time concurrently with the first.
    std::unique_lock<std::mutex> lk(apiMutex_, std::try_to_lock);
    if (!lk.owns_lock()) { return true; }
    if (stream_ == nullptr) { return false; }
    // The error callback's flag: see androidErrorCallback()'s comment for
    // why this exists at all rather than reopening straight from it.
    if (androidStreamErrorFlagged()) { return false; }
    static_assert(AAUDIO_STREAM_STATE_STARTING == 3 && AAUDIO_STREAM_STATE_STARTED == 4,
                  "androidStreamStateIsAlive()'s literals no longer match AAudio.h's "
                  "aaudio_stream_state_t enum - update audio_out_aaudio.hpp");
    const aaudio_stream_state_t state =
        AAudioStream_getState(static_cast<AAudioStream*>(stream_));
    return androidStreamStateIsAlive(static_cast<std::int32_t>(state));
}

}  // namespace cascade::sink

#else  // !defined(__ANDROID__)

namespace cascade::sink {

// HOST-MODE STUB. Reached only by tests/test_audio_out_aaudio.cpp (this file
// compiled with no __ANDROID__, no <aaudio/AAudio.h> on the include path at
// all). open()/streamAlive()/listOutputDevices() are not defined in this
// branch and that test never calls them, so their absence is not a link
// error — only closeLocked() is needed here, because ~AudioOut() calls
// close() calls closeLocked() and every AudioOut instance ODR-uses its own
// destructor. stream_ can never be non-null in this mode: the only thing
// that would set it, open(), does not exist here.
void AudioOut::closeLocked() {
    stream_ = nullptr;
    running_ = false;
}

}  // namespace cascade::sink

#endif  // defined(__ANDROID__)
