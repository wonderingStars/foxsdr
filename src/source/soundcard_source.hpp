// soundcard_source.hpp - a SOUND CARD's input as a receiver source.
//
// WHY. A beta tester receives VLF - SAQ Grimeton on 17.2 kHz, and his own
// signal generator into a loop antenna - through an external USB sound card
// sampling at 192 kHz, which is what the program SAQrx does: below about
// 100 kHz a sound card IS the receiver, and nothing else FoxSDR drives goes
// that low. A stereo card is also the classic input of an I/Q receiver (a
// SoftRock, or any direct-conversion front end), so both uses are served:
//
//   REAL (mono)   one channel, left or right, of real audio 0 .. fs/2. It is
//                 turned into complex I/Q at fs/2 centred on fs/4 (see
//                 dsp/real_to_iq.hpp), so the display runs 0 .. fs/2 on the
//                 AIR frequency: at 192 kHz, 0 - 96 kHz. The centre is fixed
//                 by the arithmetic, and the application tunes inside the span
//                 by moving the VFO (gui::tuneWithFixedCentre) rather than asking this
//                 source to move a tuner it does not have.
//   I/Q (stereo)  left = I, right = Q (or swapped, because half the I/Q
//                 hardware ever built has them the other way round and the
//                 symptom - a mirrored spectrum - is otherwise baffling),
//                 at the card's own rate, centred on a frequency the user
//                 enters: what the external receiver is tuned to.
//
// THE DEVICE IS REMEMBERED BY NAME AND HOST API, NEVER BY INDEX, for the
// reason audio_out.hpp's recoveryDeviceIndex gives: a PortAudio index is a
// position in a list that renumbers, and PortAudio lists one physical card
// once per host API - so the NAME alone is not unique either. A saved pair
// that is not present is a missing device, reported as such; it is never
// quietly replaced by whichever card happens to answer, because a receiver
// that silently moved to the laptop's microphone would look like a dead band.
//
// THE SEAM. Everything PortAudio does is behind SoundCardBackend: listing the
// input devices and their rates, opening a stream that calls back with
// interleaved float frames, closing it, and saying whether it is still alive.
// The application uses the PortAudio backend; tests hand the source a fake
// that pushes scripted frames, so ctest never needs a sound card.
//
// THREADING. The IqSource contract, plus: the realtime callback is the
// PRODUCER of an SPSC ring and does nothing but a ring write (whole frames
// only - a split frame would swap the channels for the rest of the session,
// the lesson written into audio_out.hpp) and a counter bump; read() on the
// pipeline's source thread is the CONSUMER and does the conversion. open()
// and enumeration call into the audio stack and can block for as long as a
// host API likes, so the application only ever calls them on a worker.
//
// LIVENESS. A USB card pulled out mid-stream does not tell PortAudio anything:
// the callback simply stops being called. read() therefore watches for it -
// the backend's own alive() poll, and a stall of kStallMs with no samples at
// all - and latches faulted(), which the pipeline's source thread turns into
// its "Device stopped" state with lastError() as the reason. It is the output
// sink's streamAlive() watchdog, moved to where the samples are consumed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dsp/real_to_iq.hpp"
#include "dsp/spsc_ring.hpp"
#include "source/device_source.hpp"

namespace cascade::source {

enum class SoundCardFormat {
    RealMono,  // one channel of real audio, 0 .. rate/2 on the air
    IqStereo,  // left = I, right = Q, around a user-entered centre
};

// Everything that decides what a sound card source delivers. The application
// keeps one of these for the Source section and saves it in the config.
struct SoundCardSettings {
    std::string device;   // PortAudio device name, exactly as listed
    std::string hostApi;  // PortAudio host API name ("Windows WASAPI", "MME", "ALSA")
    double cardRateHz = 48000.0;  // the rate the CARD samples at
    SoundCardFormat format = SoundCardFormat::RealMono;
    int channel = 0;              // RealMono: 0 = left, 1 = right
    bool swapIq = false;          // IqStereo: right is I, left is Q
    double iqCentreHz = 0.0;      // IqStereo: what the external receiver is tuned to
};

// The complex rate a card at s.cardRateHz delivers in s.format (fs/2 for real,
// fs for I/Q), and the centre of what it covers (fs/4 for real, the entered
// centre for I/Q).
double soundCardIqRateHz(const SoundCardSettings& s);
double soundCardCentreHz(const SoundCardSettings& s);

// The settings as one args string, and back - the form DeviceSource::open()
// takes, which is how the patch page's radio nodes name a sound card. Device
// and host API names are free text (commas, equals signs and bars all occur
// in real Windows device names), so they are percent-encoded here and decoded
// by the parser. parseSoundCardArgs leaves any field the string does not name
// at its default, and answers false only for a value it cannot read.
std::string soundCardArgs(const SoundCardSettings& s);
bool parseSoundCardArgs(const std::string& args, SoundCardSettings& out);
// Just the card - "device=...,api=..." - the part of the args that names WHICH
// input it is, and all a patch radio's key carries (see soundCardArgs for the
// encoding).
std::string soundCardDeviceArgs(const std::string& device, const std::string& hostApi);

// One rate a card accepts. `exclusive`: accepted only when the stream is
// opened in WASAPI EXCLUSIVE mode - which is how a Windows card reaches the
// rates its own hardware runs at rather than the one the Windows mixer is set
// to. Always false off Windows and on every other host API.
struct SoundCardRate {
    double hz = 0.0;
    bool exclusive = false;
};

// One input device as the backend lists it.
struct SoundCardDevice {
    int index = -1;  // the backend's own handle; valid for this process only
    std::string name;
    std::string hostApi;
    int maxInputChannels = 0;
    double defaultRateHz = 0.0;
    bool isDefault = false;               // the system's default input
    std::vector<SoundCardRate> rates;     // ascending, for min(2, maxInputChannels) channels
};

// "name (host API)" - what the Source section's device combo shows.
std::string soundCardDeviceLabel(const SoundCardDevice& d);

// Which listed device the settings name: an exact NAME AND HOST API match,
// the first if a machine has two identical cards. -1 when there is none - the
// caller reports a missing device and never substitutes another one. An EMPTY
// device name means "the system default input" (a patch node that has not
// chosen one), and answers the default's row or -1.
int findSoundCard(const std::vector<SoundCardDevice>& list, const std::string& name,
                  const std::string& hostApi);

// The rates a device offers for a format, ascending and in Hz of the CARD.
// REAL mode needs an even rate of at least 16 kHz (its complex output is half
// the card rate, and the pipeline takes nothing under 8 kHz); I/Q needs two
// channels at all.
std::vector<SoundCardRate> soundCardRatesFor(const SoundCardDevice& d, SoundCardFormat f);

// THE BACKEND SEAM. See the file header.
class SoundCardBackend {
public:
    // Called on the backend's realtime thread with `frames` interleaved
    // frames of `channels` floats each (channels as passed to open()). Must
    // not block, lock or allocate.
    using PushFn = void (*)(void* user, const float* interleaved, std::size_t frames);

    virtual ~SoundCardBackend() = default;

    // Every device with at least one input channel, with the rates it
    // accepts. May take seconds (every rate is asked of the host API): call
    // it off the GUI thread.
    virtual std::vector<SoundCardDevice> listDevices() = 0;

    // Opens and starts an input stream. Blocking; false with `error` set on
    // any failure, leaving nothing open.
    virtual bool open(const SoundCardDevice& dev, int channels, double rateHz, bool exclusive,
                      PushFn push, void* user, std::string& error) = 0;

    // Stops and closes the stream. Idempotent.
    virtual void close() = 0;

    // True while a stream is open AND the host API still says it is running.
    // Called from the pipeline's source thread; must never block for long - a
    // backend that cannot answer right now answers true (unknown is not dead;
    // the stall timer is the other half of the watch).
    virtual bool alive() = 0;
};

// The real one. Each backend holds its own Pa_Initialize, so the list it
// enumerates is PortAudio's snapshot for as long as ANY PortAudio user in the
// process is alive - which, with the audio output always open, is the whole
// session: a card plugged in after launch is seen by the next launch.
std::shared_ptr<SoundCardBackend> makePortAudioSoundCardBackend();

class SoundCardSource final : public DeviceSource {
public:
    // Where the backend comes from. An empty factory means PortAudio. It is a
    // FACTORY rather than one backend because a card that died is closed on a
    // thread of its own (see closeDevice), which takes that backend with it -
    // a reopen of the same source object then needs a fresh one.
    using BackendFactory = std::function<std::shared_ptr<SoundCardBackend>()>;
    explicit SoundCardSource(BackendFactory factory = {});
    ~SoundCardSource() override;

    SoundCardSource(const SoundCardSource&) = delete;
    SoundCardSource& operator=(const SoundCardSource&) = delete;

    // How long read() tolerates no samples at all from an open stream before
    // it calls the card dead. A card at the lowest rate offered (16 kHz)
    // delivers a block every few tens of milliseconds; two seconds of nothing
    // is a stream that has stopped, not a slow one.
    static constexpr std::chrono::milliseconds kStallMs{2000};
    // How often, while waiting for samples, the backend is asked alive().
    static constexpr std::chrono::milliseconds kAlivePollMs{250};
    // How long one read() waits for samples before returning 0 (the IqSource
    // bound for self-paced sources is ~100 ms).
    static constexpr std::chrono::milliseconds kReadWaitMs{100};

    // Opens the card the settings name, with those settings. BLOCKING (it
    // enumerates, then opens): call it on a worker. A device the settings
    // name that is not present fails with lastError() saying so, and nothing
    // else is opened in its place. A rate the card does not offer is coerced
    // to the nearest one it does, and lastError() says so on a call that
    // still returns true.
    bool openWith(const SoundCardSettings& s);
    // The same, against a list the caller already enumerated (the Source
    // section keeps one) - no second enumeration.
    bool openWith(const SoundCardSettings& s, const std::vector<SoundCardDevice>& list);

    // What is open (settings() carries the rate actually used).
    const SoundCardSettings& settings() const { return settings_; }
    const std::string& deviceLabel() const { return label_; }

    // Callbacks whose frames did not all fit in the ring - the pipeline not
    // keeping up, or stopped. Monotonic.
    std::uint64_t overruns() const;

    // WHERE THE REALTIME CALLBACK WRITES, and why it is not a member of the
    // source itself: a card that died is closed on a thread of its own (a
    // host API asked to close a vanished device is not trusted to return
    // promptly), and that close can still be delivering a last callback after
    // the source is gone. So the ring lives in this block, shared between the
    // source and whoever is closing its stream, and outlives both.
    struct Capture {
        explicit Capture(std::size_t floats) : ring(floats) {}
        dsp::SpscRing<float> ring;
        std::atomic<int> channels{1};
        std::atomic<std::uint64_t> overruns{0};
    };

    // The realtime callback's work: `user` is the Capture. Whole frames only;
    // a block the ring cannot take in full keeps the frames that fit and
    // counts one overrun. Lock-free and allocation-free.
    static void pushFrames(void* user, const float* interleaved, std::size_t frames);

    // --- DeviceSource ---------------------------------------------------------
    const char* driverKey() const override { return "soundcard"; }
    bool open(const std::string& args) override;  // parseSoundCardArgs + openWith
    void closeDevice() override;
    bool isOpen() const override { return open_; }
    // A sound card has no gains, no automatic gain and one input: its level
    // is set in the operating system's own mixer, which this source does not
    // touch.
    std::vector<GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& /*name*/) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool on) override { return !on; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override;
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override;
    // COMPLEX rates, as IqSource::sampleRateHz reports them: half the card's
    // rates in REAL mode. setSampleRateHz takes one of these and reopens the
    // stream at the matching card rate (blocking - off the GUI thread).
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;
    bool deviceDead() const override { return faulted(); }
    std::string faultedWhile() const override;

    // --- IqSource -------------------------------------------------------------
    // start() empties the ring (what the card captured while the pipeline was
    // stopped is stale) and arms the stall timer; stop() makes an in-flight
    // read() return. Neither touches the stream: opening and closing the
    // audio device are blocking calls into the host API, and start()/stop()
    // run on the GUI thread (Pipeline::start/stop). The stream is closed by
    // closeDevice() and the destructor.
    bool start() override;
    void stop() override;
    bool running() const override { return running_.load(std::memory_order_relaxed); }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    // REAL mode: the centre is fixed at fs/4 by the conversion, so only that
    // value is accepted. I/Q mode: nominal - it records what the external
    // receiver is tuned to, and is always accepted.
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;
    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override { return faulted_.load(std::memory_order_acquire); }
    const char* name() const override { return name_.c_str(); }
    const char* lastError() const override;

private:
    bool openLocked(const SoundCardSettings& s, const std::vector<SoundCardDevice>& list);
    void latchFault(const std::string& why);

    // 2^18 floats: 680 ms of stereo at 192 kHz. The pipeline reads every
    // 10 ms; the depth is for a GUI or scheduler hiccup, not for latency.
    static constexpr std::size_t kRingFloats = std::size_t{1} << 18;

    BackendFactory factory_;
    std::shared_ptr<SoundCardBackend> backend_;
    std::shared_ptr<Capture> cap_;
    dsp::RealToIq realToIq_;
    SoundCardSettings settings_;
    SoundCardDevice device_;
    int channels_ = 1;            // interleaved floats per frame in the ring
    bool open_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> faulted_{false};
    // The source thread's own clock state for the liveness watch.
    std::chrono::steady_clock::time_point lastDataAt_{};
    std::chrono::steady_clock::time_point lastAlivePollAt_{};
    // Scratch for read(), grown once.
    std::vector<float> interleaved_;
    std::vector<float> mono_;
    std::string name_ = "Sound card";
    std::string label_;
    // Control-thread errors (open, setters) and the source thread's fault
    // message are kept apart: the fault is written once, by read(), before
    // faulted_ is released, and never again - so lastError() can hand out
    // either pointer without a lock, which the IqSource contract requires.
    std::string error_;
    std::string faultMsg_;
};

}  // namespace cascade::source
