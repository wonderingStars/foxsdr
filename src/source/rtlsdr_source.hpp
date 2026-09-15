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
    bool setBiasTee(bool on);
    bool biasTee() const { return biasTee_.load(std::memory_order_relaxed); }

    // The crystal trim, in parts per million. Every RTL2832U dongle is a few
    // ppm out and the error is proportional, so it is worth tens of kHz at
    // the top of the range.
    bool setFreqCorrectionPpm(int ppm);
    int freqCorrectionPpm() const { return ppm_.load(std::memory_order_relaxed); }

    // What the tuner is: "R820T", "R828D", "R828D (RTL-SDR Blog V4)".
    std::string tunerName() const;

    // STREAM HEALTH, in the same words SoapySource writes, so one log reads
    // the same whichever way a radio was opened.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};
    std::string streamHealthLine();
    void setStreamHealthWindowForTest(std::chrono::milliseconds w);

    // THE LINK. See the file header for why this is a separate object.
    // Public only because the reader thread's body is a free function in the
    // .cpp; nothing outside this class has any business touching one.
    struct Link {
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

        // Written and read ONLY under `mutex` - except by a reader thread
        // that has been abandoned, which is exactly the point after which
        // nothing may write them again.
        std::unique_ptr<usb::UsbDevice> transport;
        std::unique_ptr<Rtl2832u> rtl;
        std::unique_ptr<TunerR82xx> tuner;

        // The ring the reader fills and read() drains. Single producer,
        // single consumer, no lock.
        std::unique_ptr<dsp::SpscRing<std::complex<float>>> ring;

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
    std::atomic<bool> biasTee_{false};
    std::atomic<int> ppm_{0};

    std::thread reader_;
    // Satisfied by the reader thread as its last act, so stop() has a BOUNDED
    // join - std::thread offers none.
    std::future<void> readerExit_;
};

}  // namespace cascade::source
