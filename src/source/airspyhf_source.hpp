// airspyhf_source.hpp - FoxSDR's own Airspy HF+ driver: a DeviceSource that
// speaks the vendor protocol in src/source/airspyhf_protocol.hpp over the
// WinUSB transport in src/usb/usb_device.hpp, with no libairspyhf, no libusb
// and no SoapySDR module anywhere in the path.
//
// WHY A NATIVE DRIVER AT ALL is argued in usb_device.hpp: every crash this
// product has received from a USB radio landed inside somebody else's libusb,
// on a thread we did not create, behind a vendor module we could not fix. The
// three rules that header states are what this file is built on -
// enumeration never opens a device, WE own every thread, every wait is
// bounded - and the shape below is the consequence of the third one. It is
// deliberately the same shape as HackRfSource: same transport, same reader
// ownership, same bounded join with abandonment, same stream-health line. A
// reader who has understood one of these files has understood both, and that
// is worth more than any per-radio cleverness.
//
// THE THREAD, AND WHY IT OUTLIVES THIS OBJECT. One reader thread pulls
// completed bulk transfers, converts them and writes them into a ring; read()
// drains the ring on the pipeline's source thread. Stopping it is signal,
// join, then endBulkStream() - in that order, because the transport's ring is
// freed by endBulkStream and a readBulk still in flight would be reading
// memory it has just released. The join is BOUNDED (kReaderJoinWait): a
// reader that has not come back by then is abandoned rather than waited for,
// exactly as SoapySource abandons a wedged vendor call, because a hang on the
// GUI thread is worse than a leak. An abandoned reader must still have
// somewhere valid to run, so everything it touches - the run flag, the device
// pointer, the ring, the error slot, the health tally AND the whole DSP state
// below - lives in a ReaderLink behind a shared_ptr that the thread captures
// BY VALUE, and the abandoning path deliberately leaks the UsbDevice rather
// than destroying it under a thread that is still inside it.
//
// WHAT THIS DRIVER DOES THAT THE OTHERS DO NOT: THREE CORRECTIONS ON THE HOST.
// An Airspy HF+ hands over int16 I/Q and nothing else. The filter gain for the
// current rate, the fine-tuning rotation that undoes the deliberate 5 kHz
// zero-IF offset and the whole-kHz LO grid, and the adaptive IQ balancer that
// rejects the zero-IF image all run HERE, on the reader thread, exactly as
// libairspyhf runs them on its own consumer thread. airspyhf_protocol.hpp
// carries the arithmetic and the argument for each; what matters in this file
// is that they are all state the reader owns.
//
// WHICH IS WHY THE RETUNE PATH LOOKS ODD. The GUI thread computes a tune and
// publishes the residual and the filter gain as ATOMICS the reader picks up on
// its next block; it never touches the balancer, which is a large mutable
// object the reader is inside microseconds at a time. A reset is asked for
// with a flag and performed BY the reader. That keeps a retune at one control
// transfer with no lock between the two threads at all - the same property
// that makes the HackRF's retune cheap, bought differently.
//
// THE LOCKS. devMutex_ serialises control transfers against each other and
// against start/stop/open/close. The reader thread does NOT take it: it only
// ever calls readBulk on the bulk pipe, which the transport's rule 2 is
// written to allow concurrently with synchronous control transfers. The error
// slot and the health tally keep their own small mutexes inside the link,
// because the reader writes them while the GUI reads them.
//
// WHAT IS NOT HERE. CONFIG_WRITE and the flash-calibration path: this driver
// READS the device's stored calibration at open and will change it in memory,
// but it will not write anybody's receiver flash. SET_USER_OUTPUT, the four
// GPIO pins on the dual-port board, for the same reason. The bias-tee name
// request, because a name is only for a menu we do not have yet.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/spsc_ring.hpp"
#include "source/airspyhf_protocol.hpp"
#include "source/device_source.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source {

// --- the bounded waits ----------------------------------------------------
//
// Every one of these is named rather than written at its call site because
// tests/test_shutdown_budget.cpp discovers `constexpr std::chrono` constants
// under src/ and refuses to go green until each is classified in its
// kKnownWaits table. That is not bureaucracy: the file exists because a wait
// added in one change-set was invisible to the budget written in the same
// change-set, and a guard that ignores what it has not been introduced to is
// not a guard.
//
// THEY LIVE IN THE PROTOCOL'S NAMESPACE, not in cascade::source, and that is
// not a style choice: hackrf_source.hpp already declares kBulkReadWait,
// kReadWait, kReaderJoinWait and kStreamHealthWindow at cascade::source
// scope, and src/gui/app_window.hpp includes both drivers. Two headers
// declaring the same name in the same namespace is a redefinition the moment
// anything wants both radios in one translation unit, which is every build of
// the Source section. The RTL-SDR driver solves the same problem by making
// its window a class-scope static; this one qualifies by radio, which keeps
// each constant readable as "the Airspy HF+'s bulk read wait" at its call
// site.
namespace airspyhf {

// How long readBulk blocks for a completed transfer before answering "nothing
// yet". At 768 kS/s a 16384-byte transfer is 5.3 ms of signal, so this expires
// only when the radio has genuinely stopped delivering - and it is what bounds
// how long the reader thread takes to notice it has been asked to stop.
constexpr std::chrono::milliseconds kBulkReadWait{100};

// How long read() waits for the reader to put something in the ring before
// returning the IqSource contract's "nothing yet, retry" zero. The pipeline's
// self-paced loop then backs off a millisecond and asks again, so this only
// has to be short enough not to delay a stop.
constexpr std::chrono::milliseconds kReadWait{20};

// The bound on joining the reader thread (see the file header). Comfortably
// more than kBulkReadWait, because the ordinary exit costs at most one of
// those; anything past it is a reader that is not coming back.
constexpr std::chrono::milliseconds kReaderJoinWait{1000};

// The stream-health window, matching SoapySource::kStreamHealthWindow. Not a
// wait: nothing sleeps or blocks on it. It is how much streaming the reader
// tallies before it writes one "source: stream health ..." line.
constexpr std::chrono::milliseconds kStreamHealthWindow{60000};

}  // namespace airspyhf

// --- enumeration ----------------------------------------------------------

// Every Airspy HF+ bound to WinUSB, as the Source section wants them. NEVER
// OPENS A DEVICE (usb_device.hpp rule 1): the label and the args are built
// from what SetupAPI already knows.
//
// On a non-Windows build this is empty and says so in the log: the transport
// is WinUSB, and a Linux HF+ is reached through SoapySDR until a libusb
// backend exists behind the same interface.
std::vector<NativeDeviceInfo> enumerateAirspyHf();

// The pure half of the above: which of a list of USB devices are HF+ boards,
// and what each one's label and args string is. Separated out because it is
// the only part that can be proven without a radio on the bench, and because
// the args string it produces is the string open() has to take back.
//
// THE LABEL COMES OFF THE BUS, NOT OFF THE BOARD, and that is a decision
// rather than a shortcut: an HF+ Dual and an HF+ Discovery share one VID/PID,
// the reference library declares a board-id enum it never reads, and the only
// way to ask the hardware which it is would be to OPEN it - which rule 1
// forbids and which would be wrong anyway while another application is
// streaming from it. So the model name is whatever the device's own product
// string says, which Windows already has, and "Airspy HF+" when it says
// nothing useful.
std::vector<NativeDeviceInfo> airspyHfDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices);

// The VID/PID list enumerateAirspyHf() asks the transport for.
std::vector<cascade::usb::UsbId> airspyHfUsbIds();

// The sixteen hex digits out of an "AIRSPYHF SN:0123456789ABCDEF" serial
// string, lower-cased; anything that does not carry the prefix comes back
// lower-cased and otherwise untouched. Exposed because the Source section
// compares a saved SoapySDR args string against a native row, and both sides
// have to be normalised the same way or the same radio reads as two.
std::string normalisedSerial(const std::string& raw);

// --- the driver -----------------------------------------------------------

class AirspyHfSource : public DeviceSource {
public:
    AirspyHfSource() = default;
    ~AirspyHfSource() override;

    // Owns a device handle and a thread; copying either would be a
    // double-close or a double-join.
    AirspyHfSource(const AirspyHfSource&) = delete;
    AirspyHfSource& operator=(const AirspyHfSource&) = delete;

    // TESTS ONLY, and per instance rather than per process so two tests can
    // never see each other's transport. `devices` is what this object's
    // open() will enumerate instead of asking WinUSB, and `opener` is what it
    // will call instead of openWinUsb(). There is no Airspy HF+ on the bench
    // this driver was written on: the fake that implements
    // cascade::usb::UsbDevice and answers byte-for-byte as the firmware does
    // IS the proof, so the seam that admits it is part of the design and not
    // a back door bolted on.
    using UsbOpenFn =
        std::function<std::unique_ptr<cascade::usb::UsbDevice>(const std::string& path,
                                                               std::string& error)>;
    void setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices, UsbOpenFn opener);

    // --- DeviceSource ----------------------------------------------------

    const char* driverKey() const override { return "airspyhf"; }

    // Takes a NativeDeviceInfo::args string: "serial=<hex>" picks a device by
    // the serial SetupAPI reported (case-insensitive, normalised through
    // normalisedSerial, and a suffix match so the short form a user reads off
    // another tool's listing still finds it), "index=N" picks the Nth in
    // enumeration order. An empty string takes the first.
    //
    // A successful open has ASKED THE DEVICE WHAT IT IS AND WHAT IT CAN DO -
    // part id and serial, firmware version, the list of sample rates, which of
    // those rates are low-IF, the attenuator's steps, whether it has a bias
    // tee, and the crystal calibration stored in its own flash - and then put
    // it in a KNOWN STATE: the first rate in its list, 10 MHz, no attenuation,
    // preamp off, AGC off, bias tee off. Reading the rates is not optional
    // (a Discovery's list differs from a Dual's, and differs again with
    // firmware), and the known state is not cosmetic: this radio remembers
    // what the last application left it at. Nothing is streamed until start().
    bool open(const std::string& args) override;

    // Stops the stream, releases the device. Idempotent, safe on a
    // never-opened instance, and safe to call from the destructor.
    // lastError() survives it, so a failure reason outlives the cleanup.
    void closeDevice() override;

    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // TWO GAINS, AND ONE OF THEM COUNTS THE WRONG WAY ROUND UNLESS IT IS
    // TURNED OVER FIRST.
    //
    // The hardware has an ATTENUATOR (SET_ATT, index 0..8, six decibels a
    // step) and a PREAMP (SET_LNA, off or on, +6 dB "compensated in digital"
    // per airspyhf.h:170). The attenuator's native units run the opposite way
    // from every other control in this application: a bigger number means
    // LESS signal. Exposing it that way would put one slider in the Source
    // panel where right is quieter while every other slider's right is
    // louder, which is the kind of inconsistency that gets blamed on the
    // radio.
    //
    // So "ATT" IS PRESENTED AS A NEGATIVE GAIN: the range is -48..0 dB in
    // steps of 6, zero means no attenuation, -48 means all of it, and
    // gainDb("ATT") reports the negative of the step actually programmed. The
    // conversion to the index the firmware wants happens in one place
    // (airspyhf::attIndexFor) and the steps themselves are read off the
    // device, because a firmware old enough not to list them is assumed to
    // have the Discovery's nine (airspyhf.c:1049-1058).
    //
    // Out-of-range values are CLAMPED and rounded to a step the hardware has,
    // and gainDb() reports what was actually programmed - the DeviceSource
    // contract, and what stops a panel showing 30 dB of attenuation the radio
    // never had.
    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;

    // The HF+ HAS a hardware AGC, unlike every other native driver here:
    // SET_AGC turns it on and SET_AGC_THRESHOLD picks how hard it works.
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return autoGain_.load(std::memory_order_relaxed); }

    // airspyhf.h:168 - with the AGC on, the threshold is "0 = low, 1 = high".
    // Not part of DeviceSource, so the Source panel reaches it through the
    // concrete type when it grows a control for it.
    bool setAgcThresholdHigh(bool high);
    bool agcThresholdHigh() const { return agcHigh_.load(std::memory_order_relaxed); }

    // One antenna on a Discovery; the dual-port board has two physical
    // sockets but switches between them BY FREQUENCY, in the firmware, with
    // no request to choose one - so there is nothing here to select and
    // pretending otherwise would be a control that does nothing.
    std::vector<std::string> antennas() const override { return {"RX"}; }
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override { return "RX"; }

    // The rates the DEVICE listed at open, ascending. Unlike the HackRF's,
    // this is a real constraint and not a menu: the firmware takes an INDEX
    // into its own list, so a rate that is not in it cannot be programmed at
    // all. setSampleRateHz coerces to the nearest.
    std::vector<double> supportedSampleRatesHz() const override;

    // Which architecture the CURRENT rate uses. Zero-IF rates need the image
    // rejection and the 5 kHz offset; low-IF rates need neither, and the
    // reference switches both off for them (airspyhf.c:334-340, :1360-1363).
    bool isLowIf() const { return lowIf_.load(std::memory_order_relaxed); }

    // The envelope of the two bands this radio covers, because the interface
    // has room for one span. setCenterFrequencyHz enforces the real pair and
    // refuses the gap between them.
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource --------------------------------------------------------

    // RECEIVER_MODE off, the bulk pipe cleared, the ring queued, RECEIVER_MODE
    // on, then the reader thread - libairspyhf's own order (airspyhf.c:1308).
    // The off-then-on is not superstition: it is how a device left streaming
    // by an application that died is brought back to a known state.
    // Idempotent while running; false with lastError() when there is no
    // device.
    bool start() override;

    // RECEIVER_MODE off, reader joined (bounded, see the file header), bulk
    // ring torn down. Idempotent, safe before open.
    void stop() override;

    bool running() const override { return running_.load(std::memory_order_relaxed); }

    // The radio paces this source: the pipeline must not clock it.
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // Coerced to the nearest rate the DEVICE listed, and then programmed by
    // its index - the firmware has no other way to be told.
    //
    // ON A RUNNING STREAM the change is made with the radio QUIET: receiver
    // off, reader stopped, bulk ring torn down, the new rate programmed, then
    // the ring, the receiver and the reader again. running() reads true
    // throughout on the success path. That is the shape 0.89.0 had to give
    // the Soapy path after a live rate change killed the process on the
    // driver's own reader thread; there is no reason to learn it twice.
    //
    // A rate change re-tunes, because the LO floor and the IF offset both
    // depend on the architecture of the new rate - that is what
    // airspyhf_set_samplerate's trailing airspyhf_set_freq_double call is
    // for, and leaving it out moves the signal by 5 kHz when a rate change
    // crosses between zero-IF and low-IF.
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }

    // Live: one SET_FREQ, one GET_FREQ_DELTA, no stream interruption - and
    // the residual the reader rotates out is republished in the same breath.
    // Refused (with a reason) outside the two bands rather than clamped.
    bool setCenterFrequencyHz(double hz) override;

    // Drains the reader's ring. Blocks at most kReadWait when it is empty and
    // then returns 0 - the self-paced contract's "nothing yet, retry", which
    // the pipeline's source loop already handles with its own backoff.
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    // True once a transfer has failed or the device has gone. The pipeline's
    // source loop polls it and stops with lastError(), which is what turns an
    // unplugged radio into "Device stopped: ..." instead of a frozen display.
    bool faulted() const override;

    const char* name() const override;
    const char* lastError() const override;

    // --- the bias tee ----------------------------------------------------
    //
    // Present only on some boards, which is why it is ASKED FOR at open
    // (GET_BIAS_TEE_COUNT) rather than assumed. Deliberately NOT in
    // antennas(): it is not a choice of where to listen, it is power on a
    // connector, and a control that can damage a receiver connected to the
    // wrong thing should never be reachable by something iterating a list of
    // port names.
    bool biasTeeSupported() const { return biasTeeCount_.load(std::memory_order_relaxed) > 0; }
    bool setBiasT(bool on);
    bool biasT() const { return biasT_.load(std::memory_order_relaxed); }

    // --- the crystal calibration -----------------------------------------
    //
    // Parts per billion, read out of the DEVICE's flash at open when the page
    // carries the reference's magic word (airspyhf.c:1076-1099). Every tune
    // is scaled by it. The setter changes it for this session and retunes;
    // NOTHING HERE WRITES FLASH - a driver that can permanently alter a
    // stranger's receiver on a misclick is a driver that will.
    std::int32_t calibrationPpb() const { return calibrationPpb_.load(std::memory_order_relaxed); }
    bool setCalibrationPpb(std::int32_t ppb);

    // --- what open() read off the device ---------------------------------
    // Empty / zero before a successful open. Kept because "which board is
    // this and what firmware is on it" is the first question any problem
    // report needs answered, and asking the radio later means a control
    // transfer in the middle of a stream.
    std::uint32_t partId() const;
    std::string serialNo() const;  // sixteen hex digits, as every HF+ tool prints it
    std::string firmwareVersion() const;
    // The architecture flag per rate, in the same order as
    // supportedSampleRatesHz(): 0 zero-IF, 1 low-IF.
    std::vector<std::uint8_t> rateArchitectures() const;
    // The attenuator steps in dB the device listed (or the assumed nine).
    std::vector<float> attenuatorStepsDb() const;
    // What the LAST tune's GET_FREQ_DELTA answered, and the residual the
    // reader is rotating out. Both exposed because they are the half of a
    // tune that never reaches a register, so nothing else can show them.
    double frequencyDeltaHz() const { return freqDeltaHz_.load(std::memory_order_relaxed); }
    double frequencyShiftHz() const;

    // --- stream health ---------------------------------------------------
    //
    // Same tally and the same one-line format SoapySource writes (see its
    // header for the field crash that produced it): a line always for the
    // first window after a start, so a healthy radio leaves one proving it,
    // and after that only for a window with something to report.
    struct StreamHealth {
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

    // The line for the window so far, and the window starts again. Empty when
    // nothing has been read since the last line.
    std::string streamHealthLine();

    // Tests only: shorten the window so the reader's own reporting can be
    // seen without waiting a minute for it.
    void setStreamHealthWindowForTest(std::chrono::milliseconds w);

    // Transfers the reader has had to drop because the ring was full - the
    // host fell behind, not the radio. Counted rather than merely logged
    // because a timing assertion cannot tell a drop from a slow run.
    std::uint64_t droppedTransfers() const;

    // Reader threads this PROCESS has abandoned because they did not come
    // back within kReaderJoinWait. 0 on every healthy path; the delta is what
    // a test asserts, because elapsed time alone still passes when the bound
    // is deleted.
    static unsigned long long readersAbandoned();

    // Tests only: switch the host DSP off, which is airspyhf_set_lib_dsp(0)
    // (airspyhf.c:1592-1596). With it off the samples are the device's own,
    // scaled by the filter gain and nothing else - which is how a test proves
    // what the balancer is worth by measuring the same signal both ways.
    // Changing it also changes the tuning arithmetic, exactly as the
    // reference's flag does, so it retunes.
    bool setHostDspEnabled(bool on);
    bool hostDspEnabled() const { return dspEnabled_.load(std::memory_order_relaxed); }

private:
    // Everything the reader thread touches, in one object behind a shared_ptr
    // it captures BY VALUE - see the file header. An abandoned reader outlives
    // the AirspyHfSource that started it, and a thread reading freed members
    // would be a worse defect than the hang the bound exists to prevent.
    struct ReaderLink {
        explicit ReaderLink(std::size_t ringCapacity) : ring(ringCapacity) {}

        std::atomic<bool> run{false};

        // NOT owned here. The AirspyHfSource owns the handle, except on the
        // abandoning path, which deliberately leaks it so a stranded reader
        // still has a live object to be inside.
        cascade::usb::UsbDevice* dev = nullptr;

        cascade::dsp::SpscRing<std::complex<float>> ring;

        // read() parks here when the ring is empty; the reader signals after
        // every transfer it writes. `exited` is the reader's LAST act and the
        // thing the bounded join waits for: a thread that has not set it has
        // not left, and "the flag is still clear" is evidence a timing
        // measurement on its own cannot produce.
        std::mutex waitMutex;
        std::condition_variable waitCv;
        bool exited = false;

        // The error slot: written by the reader, read by the GUI and by the
        // pipeline's source loop. A std::string written on one thread and
        // read on another is UB, not a stale value.
        mutable std::mutex errorMutex;
        std::string lastError;
        bool faulted = false;
        bool deviceDead = false;
        std::string deadWhat;

        mutable std::mutex healthMutex;
        StreamHealth health;
        bool healthEverWritten = false;
        std::chrono::milliseconds healthWindow = airspyhf::kStreamHealthWindow;
        std::atomic<std::uint64_t> dropped{0};

        // --- the DSP the reader applies, and how the GUI changes it -------
        //
        // Published by the control thread, consumed by the reader on its next
        // block. Individually atomic rather than under a lock because the
        // reader must never wait on the GUI and the GUI must never wait on a
        // transfer: the worst a torn pair can cost is one block of 4096
        // samples rotated at the old frequency's residual, five milliseconds
        // of a retune nobody can hear.
        std::atomic<float> filterGain{1.0f};
        std::atomic<double> freqShiftHz{0.0};
        std::atomic<double> sampleRateHz{0.0};
        std::atomic<bool> lowIf{false};
        std::atomic<bool> dspEnabled{true};

        // A retune asks the balancer to start again (the reference calls
        // iq_balancer_set_optimal_point from set_freq for exactly this). The
        // READER performs it, because the balancer is its own and a control
        // thread reaching into it would be the data race this whole structure
        // exists to avoid.
        std::atomic<bool> balancerResetWanted{false};
        std::atomic<float> optimalPoint{0.0f};

        // Reader-only from here down. Constructed with the source so a
        // restart does not throw away an estimate that is still true.
        airspyhf::IqBalancer balancer;
        airspyhf::Rotator rotator;
    };

    // NEVER REASSIGNED, hence const: an abandoned reader holds its own copy of
    // this pointer, so a source that swapped in a fresh link could have a new
    // device behind a thread still pumping the old one.
    const std::shared_ptr<ReaderLink> link_ = std::make_shared<ReaderLink>(kRingCapacitySamples());

    // The ring holds every queued transfer several times over. At the
    // Discovery's 912 kS/s that is 287 ms of signal, against a pipeline that
    // asks for 10 ms chunks - generous, because this radio's buffers are
    // small and a late host should never lose one.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 18; }

    // The *Locked helpers assume devMutex_ is held. Public entry points take
    // it; privates never do, so open() can run a whole opening sequence under
    // one lock without recursing into it.
    bool controlOutLocked(airspyhf::VendorRequest r, std::uint16_t value, std::uint16_t index,
                          const std::uint8_t* data, std::size_t len, const char* what);
    // `moved`, when given, receives the byte count the device actually
    // answered with. GET_VERSION_STRING is as long as the firmware's string
    // and no longer, so a caller that turns the answer into text needs the
    // count rather than trusting a terminator the device never wrote.
    bool controlInLocked(airspyhf::VendorRequest r, std::uint16_t value, std::uint16_t index,
                         std::uint8_t* data, std::size_t len, const char* what,
                         std::size_t* moved = nullptr);

    bool setReceiverModeLocked(airspyhf::ReceiverMode mode, const char* what);
    // Reads the rate list and the architecture list. False only when the
    // device FAILED; a firmware with no architecture request is not a
    // failure, it is a device where every rate is zero-IF
    // (airspyhf.c:1002-1008).
    bool readRatesLocked();
    bool readAttStepsLocked();
    bool readCalibrationLocked();
    bool readBiasTeeCountLocked();
    // SET_SAMPLERATE by index, with the LO pre-tune, the filter-gain read and
    // the retune the reference performs around it.
    bool programRateIndexLocked(std::size_t index, const char* what);
    // The tune itself: SET_FREQ when the kHz has moved, then GET_FREQ_DELTA,
    // then publish the residual to the reader.
    bool programFrequencyLocked(double hz, const char* what);
    bool programAttLocked(double gainDb);
    bool programLnaLocked(bool on);
    bool programAgcLocked(bool on);
    bool programAgcThresholdLocked(bool high);
    bool programBiasTLocked(bool on);
    // Recomputes and republishes the residual without sending anything -
    // what a calibration or DSP-flag change needs.
    void republishTuningLocked();

    // Queue the bulk ring, RECEIVER_MODE on, spawn the reader. Assumes
    // devMutex_ held and the device open and not already running.
    bool startStreamingLocked();
    // RECEIVER_MODE off, join the reader (bounded), tear the bulk ring down.
    // Idempotent; assumes devMutex_ held.
    void stopStreamingLocked();

    // THE READER'S OWN HELPERS ARE STATIC AND TAKE THE LINK, not `this`. An
    // abandoned reader outlives the AirspyHfSource; a member function
    // reaching for a member of a destroyed object is precisely the defect the
    // link was introduced to prevent, and making these static is what stops
    // the compiler from letting one be written by accident.
    static void readerThreadBody(std::shared_ptr<ReaderLink> link);
    static void noteRead(ReaderLink& link, int ret, std::size_t samples, bool dropped);
    static std::string healthLineLocked(ReaderLink& link);  // link.healthMutex held

    // The only writers of the error slot, so no failure path can forget the
    // lock. noteTransportFault also raises faulted() and deviceDead(): a
    // transfer that FAILED on a device that was open is a device that has
    // gone, which is what an unplugged radio looks like from here.
    static void setErrorOn(ReaderLink& link, std::string msg);
    static void noteTransportFaultOn(ReaderLink& link, const char* what,
                                     const std::string& detail);
    void setError(std::string msg);
    void clearError();
    void noteTransportFault(const char* what, const std::string& detail);

    void setName(std::string n);

    // Resolves an args string against the transport's device list.
    bool resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                       std::string& error);

    // Serialises control transfers against each other and against
    // start/stop/open/close. The reader thread does not take it (file header).
    mutable std::mutex devMutex_;

    // Owned, except after an abandonment - see the file header.
    std::unique_ptr<cascade::usb::UsbDevice> dev_;
    std::thread reader_;

    // Lock-free mirrors, so per-frame GUI readouts never wait behind a
    // control transfer in flight.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<double> attDb_{0.0};
    std::atomic<double> lnaDb_{0.0};
    std::atomic<bool> autoGain_{false};
    std::atomic<bool> agcHigh_{false};
    std::atomic<bool> biasT_{false};
    std::atomic<int> biasTeeCount_{0};
    std::atomic<bool> lowIf_{false};
    std::atomic<bool> dspEnabled_{true};
    std::atomic<std::int32_t> calibrationPpb_{0};
    std::atomic<double> freqDeltaHz_{0.0};

    mutable std::mutex nameMutex_;
    std::string name_ = "Airspy HF+: (no device)";

    // Identity and capability, read once at open under devMutex_.
    std::uint32_t partId_ = 0;
    std::string serialNo_;
    std::string firmwareVersion_;
    std::vector<std::uint32_t> rates_;
    std::vector<std::uint8_t> architectures_;
    std::vector<float> attSteps_;
    std::size_t rateIndex_ = 0;
    // The kHz the device is currently tuned to, so a tune that does not move
    // it sends nothing - the reference's own `if (device->freq_khz !=
    // freq_khz)` guard (airspyhf.c:1372).
    std::uint32_t loKhz_ = 0;

    // The test seam (see setTransportForTest). Empty opener means the real
    // WinUSB transport.
    std::vector<cascade::usb::UsbDeviceInfo> fakeDevices_;
    UsbOpenFn fakeOpener_;
    bool useFakeTransport_ = false;
};

}  // namespace cascade::source
