// A NATIVE RTL-SDR: our transport, our reader thread, our enumeration.
//
// WHY THIS EXISTS AT ALL. Every crash report this product received from an
// RTL-SDR user in its first month landed inside libusb-1.0.dll, reached
// through a SoapySDR vendor module that came from somebody else's install: a
// reader thread we did not create, a device lock we could not see, and an
// enumeration probe that opened and reset the very dongle a stream was
// running on. None of that code was ours, so none of it could be fixed here -
// the only remedies available were "restart FoxSDR" and "do not enumerate
// while streaming", and both of them are apologies rather than fixes.
//
// This class owns all three. The USB traffic goes through src/usb (WinUSB,
// every wait bounded); the reader thread is spawned by start() and joined by
// stop(); enumeration reads SetupAPI properties and never opens anything. A
// radio that goes wrong is therefore something this code can be held
// responsible for.
//
// THE SHAPE OF IT, top to bottom:
//
//   RtlSdrSource   this file - DeviceSource, the reader thread, the ring
//     Rtl2832u     the demodulator: vendor requests, the resampler, the DDC
//       TunerR82xx the tuner, over the demodulator's I2C repeater
//     usb::UsbDevice  the transport (WinUSB, or the test fake)
//
// THREADING, and it is the SoapySource contract with one difference. Every
// entry into the hardware - open, start, stop, a retune, a gain change, and
// the reader thread's own bulk read - is taken under the link's mutex, so
// exactly one thread is ever inside the device. The difference is that here
// the bound the driver promises is a bound this code actually enforces: every
// control transfer and every bulk read carries a timeout into WinUSB, so
// there is no such thing as a call that never comes back.
//
// A SAMPLE-RATE CHANGE IS MADE ON A QUIET STREAM, for the same reason
// SoapySource does it: the RTL2832U's resampler is soft-reset as part of the
// change, and a bulk stream running across that reset delivers a buffer that
// is half one rate and half the other. Here it costs nothing extra - the rate
// change holds the device lock across endBulkStream / reprogram /
// resetBuffer / beginBulkStream while the reader thread waits its turn.
//
// THE LINK, and why it is a separate shared object rather than plain members.
// stop() waits kReaderJoinWait for the reader thread and, if that expires,
// ABANDONS it: detaches the thread and condemns the radio. An abandoned
// thread is still inside readBulk(), still holding a pointer to the ring it
// writes and the mutex it took - and the RtlSdrSource can be destroyed a
// millisecond later (the Source panel makes a fresh one per open). A thread
// that captured `this` would then be writing into freed memory, which is a
// worse defect than the stall it was abandoned to escape. So everything the
// reader touches lives in Link, the thread holds a COPY OF THE SHARED
// POINTER, and the link outlives whichever end finishes first. This is
// SoapySource::DeviceLink, one layer down and for the same reason.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/spsc_ring.hpp"
#include "source/device_source.hpp"
#include "source/rtl2832u.hpp"
#include "source/tuner_r82xx.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source {

// The RTL2832U dongles this driver will open, by USB id. Every one of them is
// the same silicon behind a different sticker; the list exists because
// Windows binds a driver per VID/PID, so a dongle whose id is not here is not
// enumerated even when it is plainly an RTL-SDR.
const std::vector<usb::UsbId>& rtlSdrUsbIds();

// Lists every RTL2832U bound to WinUSB, WITHOUT OPENING ONE. This is the rule
// the SoapySDR vendor probe broke and the reason a scan used to be able to
// kill a running capture. The label is built from what SetupAPI already
// knows - the bus-reported description and the serial - so a dongle that is
// already streaming in this process appears in the list unharmed.
std::vector<NativeDeviceInfo> enumerateRtlSdr();

// THE SENTENCES OF A RE-OPEN THAT ENDS IN A FAULT, one place so the driver, the tests and the reports agree on the words.
//   kReopenStoppedSentence  - a stop() landed while the radio was being reconnected: the radio is closed and latched faulted (never "unplugged").
//   kReopenWhichSentence    - the radio left the bus and what came back could not be proven to be the same radio (see locateRadio and the identity check).
//   kReopenBusySentence     - the driver lost the race for the device lock three times in a row.
inline constexpr const char* kReopenStoppedSentence = "the radio was being reconnected when it was stopped; pick the radio again";
inline constexpr const char* kReopenWhichSentence = "the radio dropped off USB and FoxSDR could not tell which of the connected radios it was; pick it again";
inline constexpr const char* kReopenBusySentence = "the radio stopped answering and reopening it did not help (the driver was busy)";
// What the re-open's finder answers (as the error text, with `present` true) when more than one listed radio could be this one and none is provably it.
inline constexpr const char* kRadioAmbiguousError = "more than one connected radio could be this one";
// HOW LONG ONE ATTEMPT OF THE RE-OPEN MAY TAKE to bring the chip up, put the settings back and restart the stream (decision S1 of the second review): a refused tuner
// transfer costs about 200 ms and a bench run measured a 5.0 s bring-up, so a bound below that turns a recoverable drop into a fault. Generous on purpose: the liveness
// beat keeps the stuck-reader check quiet whatever the length, and a stop() still ends the bring-up within the one transfer in flight.
inline constexpr int kReopenBringUpMs = 10000;

// WHICH PHYSICAL RADIO A SOURCE OPENED, remembered so a re-open never guesses (decision F4 of the review). `path` is the WinUSB interface path, `location` the
// hub-and-port of its USB node, `serial` the serial the device reported (empty when it reported none), `serialUnique` whether it was the only listed radio
// with that serial when it was opened (two dongles that both say "00000001" make the serial no identity at all).
struct RadioIdentity {
    std::string path;
    std::string location;
    std::string serial;
    bool serialUnique = false;
};
enum class RadioLocate { Found, Absent, Ambiguous };
// Finds the radio `id` among `found` (a bus enumeration), in this order and never by guessing: the exact interface path; else the same location; else the same
// serial when that serial was unique at open and is unique now. A listed device whose serial differs from a non-empty remembered one is never taken for it.
// Ambiguous when the serial points at one or more devices but is no proof (it was shared at open, or two share it now) and neither path nor location matched;
// nothing is opened and `foundPath` is set only for Found.
RadioLocate locateRadio(const std::vector<usb::UsbDeviceInfo>& found, const RadioIdentity& id, std::string& foundPath);
// The two lambdas open() arms a source with, over an enumeration and an opener that the caller supplies (the shipped ones are the bus walk and openWinUsb;
// the tests pass fakes).
using RadioListFn = std::function<std::vector<usb::UsbDeviceInfo>()>;
using RadioOpenFn = std::function<std::unique_ptr<usb::UsbDevice>(const std::string& path, std::string& error)>;
std::function<std::unique_ptr<usb::UsbDevice>(std::string& error, bool& present)> makeRadioReopenFn(RadioIdentity id, RadioListFn list, RadioOpenFn open);
std::function<int()> makeRadioListedFn(RadioIdentity id, RadioListFn list);

class RtlSdrSource final : public DeviceSource {
public:
    RtlSdrSource() = default;
    ~RtlSdrSource() override;

    RtlSdrSource(const RtlSdrSource&) = delete;
    RtlSdrSource& operator=(const RtlSdrSource&) = delete;

    const char* driverKey() const override { return "rtlsdr"; }

    // args is "serial=00000001" or "index=0", the .args of an
    // enumerateRtlSdr() entry. An empty string opens the first device found.
    bool open(const std::string& args) override;

    // TEST SEAM, and the only way the register-sequence tests can exist: open
    // against a transport the caller supplies (usb::FakeUsbDevice) instead of
    // one this class enumerates. Everything after the transport is the
    // shipping code path - the same init, the same tuner probe, the same
    // register writes - which is what makes a byte-exact assertion against it
    // mean something.
    bool openWithTransport(std::unique_ptr<usb::UsbDevice> transport, const std::string& label);

    void closeDevice() override;
    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override;

    // "RX" always, and on a Blog V4 also "HF" - which is a READOUT, not a
    // control: the V4's input switch follows the frequency, so the panel
    // shows which path is live rather than offering a way to get it wrong.
    std::vector<std::string> antennas() const override;
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override;

    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource ----------------------------------------------------------
    bool start() override;
    void stop() override;
    bool running() const override { return running_.load(std::memory_order_relaxed); }
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }
    bool setCenterFrequencyHz(double hz) override;

    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override;
    const char* name() const override;
    const char* lastError() const override;

    // --- things only an RTL-SDR has ----------------------------------------

    // The bias tee: 4.5 V up the coax for a mast-head amplifier. Off on every
    // open unless the dongle's EEPROM says it is wired permanently on,
    // because a user who does not know it is on can damage a receiver that is
    // not expecting it.
    //
    // SPELLED setBiasT/biasT, the same as every other native driver, so the
    // Source panel's one dispatch (core/bias_tee.hpp) reaches it with the same
    // call. It was setBiasTee/biasTee until the RTL-SDR joined that checkbox;
    // nothing outside this driver called the old setter, so no alias was
    // kept.
    //
    // biasT() is the READBACK: it moves only when the dongle accepted the
    // GPIO writes, so a refused switch leaves it where it was.
    bool setBiasT(bool on);
    // WHILE THE RADIO IS BEING REOPENED the answer is what the re-open will put on the wire (the user's latest wish, or off when the repeat rule withheld it),
    // not the old readback: the box on screen must show what the radio will do, and never an "off" over a tee that is about to come on.
    bool biasT() const;

    // WHAT THE EEPROM SAID AT OPEN, which the panel needs to decide whether a
    // remembered "on" may be put back (core/bias_tee.hpp).
    //   eepromValid()         - the configuration EEPROM carried the RTL2832U
    //                           header 0x28 0x32. False on a dongle with no
    //                           EEPROM at all, which reads as zeroes.
    //   biasTForcedByEeprom() - a valid EEPROM whose byte 7 bit 1 is clear:
    //                           the maker wired the bias tee permanently on,
    //                           and open() switched it on because of that.
    // Both false while closed.
    bool eepromValid() const { return eepromValid_.load(std::memory_order_relaxed); }
    bool biasTForcedByEeprom() const { return biasTForced_.load(std::memory_order_relaxed); }

    // The crystal trim, in parts per million. Every RTL2832U dongle is a few
    // ppm out and the error is proportional, so it is worth tens of kHz at
    // the top of the range.
    bool setFreqCorrectionPpm(int ppm);
    int freqCorrectionPpm() const { return ppm_.load(std::memory_order_relaxed); }

    // The application's crystal correction (0.99.56, IqSource): this driver
    // trims its own crystal, so the correction is applied IN THE RADIO -
    // sample rate included - through setFreqCorrectionPpm. That register takes
    // whole ppm, so the value is rounded to the nearest whole ppm, halves away
    // from zero (core::ppmWholeForRadio); the Source section says which
    // number went in.
    bool hasFrequencyCorrection() const override { return true; }
    bool setFrequencyCorrectionPpm(double ppm) override;

    // What the tuner is: "R820T", "R828D", "R828D (RTL-SDR Blog V4)".
    std::string tunerName() const;

    // DIRECT SAMPLING, THE USER'S CHOICE (engine serial lane A, row R11). 0 automatic (the old rule, unchanged: below 24 MHz a plain R82xx dongle's demodulator is switched to the Q branch; the tuner's
    // own range stops a tune that low first, see frequencyRangeHz), 1 never, 2 always on the I branch, 3 always on the Q branch (where the usual direct-sampling modification puts the antenna).
    // A forced choice (2 or 3) widens the range this radio accepts to 100 kHz to 14.4 MHz (half the 28.8 MHz ADC clock: the first Nyquist zone) and tunes by the demodulator's down-converter.
    // False, nothing changed, for a value outside 0..3 and for a radio that cannot (a Blog V4 has an upconverter and never leaves the tuner). Changing it retunes to where the radio is.
    static constexpr double kDirectLoHz = 100000.0;
    static constexpr double kDirectHiHz = 14400000.0;
    bool setDirectSamplingChoice(int choice);
    int directSamplingChoice() const { return directChoice_.load(std::memory_order_relaxed); }
    // THE ANALOGUE IF BANDWIDTH (screen lane S5b, row G1-14): the tuner's own filter in front of the ADC. 0 (the default) is automatic - the filter follows the sample rate, as it always has; otherwise 300 kHz to 8 MHz.
    // False, nothing changed, for a value outside that, a closed or faulted radio, or a tuner that refuses the register writes. A rate change keeps the chosen value.
    static constexpr int kIfBandwidthMinHz = 300000;
    static constexpr int kIfBandwidthMaxHz = 8000000;
    bool setIfBandwidthHz(int hz);
    int ifBandwidthHz() const { return ifBandwidthHz_.load(std::memory_order_relaxed); }
    bool directSamplingSupported() const;     // a radio with a tuner that is not a Blog V4
    // WHAT AN rtl_tcp CLIENT IS TOLD ABOUT THIS RADIO (the rtl_tcp server's header, hw-findings item 7): the rtl_tcp tuner code (5 = R820T, 6 = R828D; 0 = unknown when no radio is open) and
    // the number of gain steps of that tuner (the aggregate ladder this driver itself steps; 0 when unknown). Both are 0 while the radio is busy answering another call.
    std::uint32_t rtlTcpTunerCode() const;
    std::uint32_t tunerGainSteps() const;
    int directSamplingMode() const;           // what the demodulator is doing now: 0 off, 1 the I branch, 2 the Q branch

    // STREAM HEALTH, in the same words SoapySource writes, so one log reads
    // the same whichever way a radio was opened.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};
    std::string streamHealthLine();
    void setStreamHealthWindowForTest(std::chrono::milliseconds w);
    // Tests only: how long of silence re-arms the stream, and how long a reader stuck inside a USB call is tolerated by read() (the product's 3 s and 20 s).
    void setStallBoundsForTest(int silenceRearmAfterMs, int readerStuckAfterMs);

    // THE RE-OPEN (diagnosis 2026-11-23, signature 80913FA5). A radio whose handle has gone bad (the device re-enumerated, a port reset, the chip lost its
    // registers) cannot be repaired by a pipe restart: the reader thread closes the handle, waits for the same radio to come back on the bus, opens it again,
    // runs the whole chip bring-up, puts back the rate, frequency, gains, correction, direct sampling and bias tee, and restarts the stream into the same ring.
    // open() arms it; openWithTransport() (the tests' door) leaves it empty, so a source opened that way behaves exactly as it did before unless a test arms it
    // with setReopenForTest.
    //   fn(error, present): looks for the SAME radio (same serial, else same interface path) and opens it; `present` says whether it is listed on the bus.
    //   listed(): 1 listed, 0 not listed, -1 cannot tell; only for the one diagnostic line, so it may be empty.
    using ReopenFn = std::function<std::unique_ptr<usb::UsbDevice>(std::string& error, bool& present)>;
    void setReopenForTest(ReopenFn fn, std::function<int()> listed = {});
    // Tests only: how long the re-open waits for the radio to come back, and how often it looks (the product's 5000 ms and 250 ms).
    // `forgiveMs` (the product's 60000) is how long samples must flow after a re-open before the window's count of them is forgiven.
    // `bringUpMs` (the product's kReopenBringUpMs) is the overall bound of the bring-up that follows (the transfers of the whole of it, settings included).
    void setReopenBoundsForTest(int waitMs, int pollMs, int forgiveMs = 60000, int bringUpMs = kReopenBringUpMs);
    // The bound as it stands now (the product's value unless a test changed it).
    int reopenBringUpMsForTest() const { return link_->reopenBringUpMs.load(std::memory_order_relaxed); }
    // Tests only: called by a setter that found the radio being reopened, just before it hands its change over (decision S7's test holds a setter there until the
    // re-open has ended, which is the instant between its two looks). Empty in the product.
    void setBeforeDeferForTest(std::function<void()> fn);
    // Tests only: called on every try of retakeAfterReopen's wait for the device lock, with the number of control calls then announced in controlWaiters (decision S7's
    // test asserts the wait announces itself, so the reader stands aside). Empty in the product.
    void setRetakeProbeForTest(std::function<void(int)> fn);
    // Tests and the bench tool only: makes the transport's handle fail the way one does after a re-enumeration (every later call answers "invalid handle"),
    // without freeing anything a thread is inside. False when the transport cannot.
    bool invalidateTransportForTest();
    // Tests only: true while a re-open is taking settings (the radio is closed or being brought up and the setters hand their changes to it).
    bool reopeningForTest() const { return link_->reopening.load(std::memory_order_acquire); }
    // How many re-opens this open has done (tests, the bench tool).
    int reopenCount() const { return link_->reopensThisOpen.load(std::memory_order_relaxed); }

    // THE LINK. See the file header for why this is a separate object.
    // Public only because the reader thread's body is a free function in the
    // .cpp; nothing outside this class has any business touching one.
    struct Link {
        const void* owner = nullptr;   // the source object, for the radio reports (core/radio_events.hpp)
        struct Health {
            std::uint64_t reads = 0;
            std::uint64_t withSamples = 0;
            std::uint64_t samples = 0;
            std::uint64_t timeouts = 0;
            std::uint64_t overflows = 0;
            std::uint64_t errors = 0;
            std::int64_t longestGapMs = 0;
            bool windowOpen = false;
            std::chrono::steady_clock::time_point windowStart{};
            std::chrono::steady_clock::time_point lastSamples{};
        };

        // Serialises every entry into the device. Timed, so no acquisition
        // can freeze a caller.
        std::timed_mutex mutex;
        // Control calls waiting for `mutex`; while non-zero the reader stays
        // out of it (see controlLock in rtlsdr_source.cpp).
        std::atomic<int> controlWaiters{0};
        // THE READER'S HEARTBEAT (steady_clock nanoseconds since its epoch): set at the top of every pass of the
        // reader loop, and 0 while no reader runs. read() - on the pipeline's own thread - compares it with the clock,
        // so a reader that is stuck INSIDE a USB call for good is a fault the pipeline hears (field fault 3, 2026-11-19).
        std::atomic<std::int64_t> readerBeatNs{0};
        // How long of nothing re-arms the stream, and how long a stuck reader is tolerated: the product's numbers, settable so a test need not wait them out.
        std::atomic<int> silenceRearmAfterMs{3000};
        std::atomic<int> readerStuckAfterMs{20000};

        // THE RE-OPEN's state (see setReopenForTest). `reopening` is true from the moment the reader closes the handle until the bring-up has finished or
        // failed: while it is set every control call answers at once without touching the device (controlLock) and every setter stores its value in `wanted`.
        std::function<std::unique_ptr<usb::UsbDevice>(std::string& error, bool& present)> reopen;
        std::function<int()> listed;
        std::atomic<bool> reopening{false};
        std::atomic<int> reopensThisOpen{0};
        std::atomic<int> reopenWaitMs{5000};
        std::atomic<int> reopenPollMs{250};
        std::atomic<int> reopenForgiveAfterMs{60000};
        std::atomic<int> reopenBringUpMs{kReopenBringUpMs};
        std::function<void()> beforeDeferForTest;   // (see setBeforeDeferForTest; set before the reader runs, read by setters)
        std::function<void(int)> retakeProbeForTest;   // (see setRetakeProbeForTest)
        std::atomic<int> unlockedTunes{0};   // (moved here from the source: the re-open's retune counts into it too)
        std::atomic<bool> biasTee{false};    // (likewise: the re-open may leave the bias tee off, and the readout must say so)
        // What the user last asked of the radio, kept so a re-open can put it back. Written by each setter when it succeeds (or, while a re-open runs,
        // when it is asked), under `wantedMutex`.
        struct Wanted {
            double rateHz = 0.0;        // 0: never set, the open's default stands
            double freqHz = 0.0;
            int ifBandwidthHz = 0;
            int directChoice = 0;
            int ppm = 0;
            // "WAS SET" flags (decision S6): a value changed BACK to zero (no correction, automatic bandwidth, automatic direct sampling) is a change the re-open must put on,
            // which a test for non-zero cannot see. Set by the setters, never by the open.
            bool ppmSet = false;
            bool ifBandwidthSet = false;
            bool directChoiceSet = false;
            bool biasTee = false;
            bool autoGain = false;
            bool autoGainSet = false;   // the automatic-gain mode was asked for since the open
            bool gainSet = false;       // some manual gain was asked for since the open
            bool gainByStage = false;   // the last gain asked for was a single stage, not the aggregate
            int aggregateTenthDb = 0;
            int stageTenthDb[3] = {0, 0, 0};
            bool stageSet[3] = {false, false, false};
        };
        std::mutex wantedMutex;
        Wanted wanted;
        std::atomic<unsigned> wantedGen{0};   // bumped with every change of `wanted`, so a re-open can tell a setting arrived while it ran
        std::string tunerNameCached;          // (under wantedMutex) for the readouts that must answer while the tuner object is gone
        bool blogV4Cached = false;
        // WHICH RADIO THIS IS, as the open found it (under wantedMutex): where it sits, and what the bring-up read from it. A re-open compares what it finds
        // with this and refuses a radio that is not provably the same one.
        struct Identity {
            RadioIdentity radio;
            std::string tuner;
            std::string manufacturer;
            std::string product;
        };
        Identity identity;

        // Written and read ONLY under `mutex` - except by a reader thread
        // that has been abandoned, which is exactly the point after which
        // nothing may write them again.
        std::unique_ptr<usb::UsbDevice> transport;
        std::unique_ptr<Rtl2832u> rtl;
        std::unique_ptr<TunerR82xx> tuner;

        // The ring the reader fills and read() drains. Single producer; the
        // consumer side is read() and, once per rate change, the change
        // itself emptying it - which is why both hold `ringMutex` (see
        // rateEpoch).
        std::unique_ptr<dsp::SpscRing<std::complex<float>>> ring;

        // THE RATE'S GENERATION, so samples made at an old rate are never
        // delivered as the new one. A rate change, under `mutex` and on a
        // stopped stream, empties the ring and bumps this together under
        // `ringMutex`. The reader notes the generation under `mutex` with
        // every bulk read, and under `ringMutex` drops a buffer whose
        // generation has moved on instead of writing it. Uncontended except
        // during a rate change.
        std::atomic<std::uint64_t> rateEpoch{0};
        std::mutex ringMutex;

        // THIS GENERATION's dead-man switch. Cleared by stop(); an abandoned
        // thread wakes into a token that reads false forever, so it cannot
        // resume beside a later one.
        std::atomic<bool> readerRun{false};

        // Set once, by stop(), when a reader thread was abandoned. From then
        // on the device is condemned permanently and the handles are never
        // touched again - a stranded thread is still using them, and that
        // thread never stops reading them, so nothing may free or reprogram
        // what it is looking at.
        //
        // ATOMIC rather than plain, because it is written by stop() WITHOUT
        // the device mutex (the whole point is that the mutex may be held by
        // the thread being abandoned) and read by every later teardown.
        std::atomic<bool> abandoned{false};

        // The error slot keeps its own lock: the reader thread records
        // failures at the same time as the GUI reads lastError() and the
        // pipeline polls faulted(). A std::string written on one thread and
        // read on another is undefined behaviour, not a stale value.
        std::mutex errorMutex;
        std::string name = "RTL-SDR: (no device)";
        std::string lastError;
        std::string faultedWhile;
        bool faulted = false;
        bool deviceDead = false;

        std::mutex healthMutex;
        Health health;
        bool healthEverWritten = false;
        std::chrono::milliseconds healthWindow = kStreamHealthWindow;
    };

private:
    bool bringUpLocked();
    void teardownLocked() noexcept;
    bool startStreamLocked();
    void stopStreamLocked();
    bool retuneLocked(double hz);
    bool rangeLocked(double& loHz, double& hiHz) const;
    void setError(std::string msg);
    void clearError();

    // NEVER REASSIGNED, hence const: an abandoned reader holds its own copy
    // of this pointer, so a source that swapped in a fresh link could be
    // inside a new device while a stranded thread is still inside the old
    // one. One link per RtlSdrSource, for the life of the object.
    const std::shared_ptr<Link> link_ = std::make_shared<Link>();

    // Lock-free mirrors for the per-frame GUI readouts, exactly as
    // SoapySource keeps them: a readout must never block behind a transfer.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<int> ifBandwidthHz_{0};  // G1-14: 0 automatic (the sample rate), else the user's analogue IF bandwidth in Hz
    int ifBandwidthFor(double rateHz) const;
    std::atomic<int> directChoice_{0};   // R11: 0 automatic / 1 never / 2 I branch / 3 Q branch
    std::atomic<bool> eepromValid_{false};
    std::atomic<bool> biasTForced_{false};
    std::atomic<int> ppm_{0};

    // TUNES THAT DID NOT LOCK, counted rather than narrated. A PLL that will
    // not lock does not lock ONCE - it fails on every retune, and a warning
    // per retune buries the log it is trying to explain (the field report
    // that found this carried one line per frequency and nothing that said
    // how many). So: the first one in an open says everything worth saying,
    // the rest only increment, and close() states the total.
    // (unlockedTunes now lives in Link: the re-open's retune counts into it too.)

    std::thread reader_;
    // Satisfied by the reader thread as its last act, so stop() has a BOUNDED
    // join - std::thread offers none.
    std::future<void> readerExit_;
};

}  // namespace cascade::source
