// mirisdr_source.hpp - FoxSDR's own driver for the Mirics MSi2500 + MSi001
// pair: a DeviceSource that speaks the vendor protocol in
// src/source/msi2500.hpp and programs the tuner through
// src/source/tuner_msi001.hpp, over the WinUSB transport in
// src/usb/usb_device.hpp. No libmirisdr, no libusb and no SoapySDR module
// anywhere in the path.
//
// THE LICENCE POSTURE IS THE RTL-SDR DRIVER'S, and it is why this file exists
// at all in this shape. libmirisdr-4 is GPL-2.0 and this product never links
// copyleft code, so nothing could be reused even if reuse were wanted: what
// was taken is the description of how the silicon behaves - which is a fact
// about hardware - and everything here is written from it in this codebase's
// own style. msi2500.hpp states it in full and
// installer/THIRD-PARTY-LICENSES.txt carries it for the product.
//
// WHY A NATIVE DRIVER AT ALL is argued in usb_device.hpp: every crash this
// product has received from a USB radio landed inside somebody else's libusb,
// on a thread we did not create, behind a vendor module we could not fix. The
// three rules that header states - enumeration never opens a device, WE own
// every thread, every wait is bounded - are what the shape below is built on.
//
// THE THREAD, AND WHY IT OUTLIVES THIS OBJECT. One reader thread pulls
// completed bulk transfers, unpacks them and writes them into a ring; read()
// drains the ring on the pipeline's source thread. Stopping it is signal,
// join, then endBulkStream() - in that order, because the transport's ring is
// freed by endBulkStream and a readBulk still in flight would be reading
// memory it has just released. The join is BOUNDED (kReaderJoinWait): a reader
// that has not come back by then is abandoned rather than waited for, because
// a hang on the GUI thread is worse than a leak. An abandoned reader must
// still have somewhere valid to run, so everything it touches lives in a
// ReaderLink behind a shared_ptr the thread captures BY VALUE, and the
// abandoning path deliberately leaks the UsbDevice rather than destroying it
// under a thread that is still inside it.
//
// ONE THING THIS RADIO DOES THAT THE OTHERS DO NOT, and it shapes three
// methods below. The tuner's register 0 carries the BASEBAND FILTER WIDTH
// alongside the band and the IF mode, and the gain register's meaning depends
// on WHICH BAND the tuner ended up in. So a rate change has to re-send the
// tune words (the filter follows the rate), and a tune has to re-send the gain
// words (the band may have changed under them). Neither is an optimisation
// that can be skipped: a rate change that left the filter behind would alias,
// and a tune into an AM row that left the gain behind would program a
// low-noise amplifier that is not in circuit.
//
// THERE IS NO MIRICS DEVICE ON THIS BENCH. tests/test_mirisdr_source.cpp is
// the proof, against a fake that records every control transfer, and every
// expectation in it was produced by compiling the reference's own arithmetic
// and printing the answer rather than by reading it off by eye.
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
#include "source/device_source.hpp"
#include "source/msi2500.hpp"
#include "source/tuner_msi001.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source {

// --- enumeration ----------------------------------------------------------

// Every MSi2500-based device bound to WinUSB, as the Source section wants
// them. NEVER OPENS A DEVICE (usb_device.hpp rule 1): the label and the args
// are built from what SetupAPI already knows.
//
// On a non-Windows build this is empty and says so in the log: the transport
// is WinUSB, and a Linux Mirics device is reached through SoapySDR until a
// libusb backend exists behind the same interface.
std::vector<NativeDeviceInfo> enumerateMiriSdr();

// The pure half of the above: which of a list of USB devices are ours, and
// what each one's label and args string is. Separated out because it is the
// only part that can be proven without hardware, and because the args string
// it produces is the string open() has to take back.
std::vector<NativeDeviceInfo> miriSdrDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices);

// --- the driver -----------------------------------------------------------

class MiriSdrSource : public DeviceSource {
public:
    // --- the bounded waits ------------------------------------------------
    //
    // Every one of these is named rather than written at its call site because
    // tests/test_shutdown_budget.cpp discovers `constexpr std::chrono`
    // constants under src/ and refuses to go green until each is classified in
    // its kKnownWaits table. A guard that ignores what it has not been
    // introduced to is not a guard.
    //
    // THEY ARE CLASS MEMBERS RATHER THAN NAMESPACE CONSTANTS, for the reason
    // airspy_source.hpp gives: hackrf_source.hpp already declares these four
    // names at cascade::source scope and gui/app_window.hpp includes every
    // driver header at once. The budget's scan finds a class-static perfectly
    // well.

    // How long readBulk blocks for a completed transfer before answering
    // "nothing yet". One transfer is 2.67 ms of signal at EVERY rate this
    // radio runs (see msi2500::kTransferBytes), so this expires only when the
    // device has genuinely stopped delivering - and it is what bounds how long
    // the reader thread takes to notice it has been asked to stop.
    static constexpr std::chrono::milliseconds kBulkReadWait{100};

    // How long read() waits for the reader to put something in the ring before
    // returning the IqSource contract's "nothing yet, retry" zero. The
    // pipeline's self-paced loop then backs off a millisecond and asks again.
    static constexpr std::chrono::milliseconds kReadWait{20};

    // The bound on joining the reader thread (see the file header). Comfortably
    // more than kBulkReadWait, because the ordinary exit costs at most one of
    // those; anything past it is a reader that is not coming back.
    static constexpr std::chrono::milliseconds kReaderJoinWait{1000};

    // The stream-health window, matching SoapySource::kStreamHealthWindow. Not
    // a wait: nothing sleeps or blocks on it. It is how much streaming the
    // reader tallies before it writes one "source: stream health ..." line.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};

    MiriSdrSource() = default;
    ~MiriSdrSource() override;

    // Owns a device handle and a thread; copying either would be a
    // double-close or a double-join.
    MiriSdrSource(const MiriSdrSource&) = delete;
    MiriSdrSource& operator=(const MiriSdrSource&) = delete;

    // TESTS ONLY, and per instance rather than per process so two tests can
    // never see each other's transport. `devices` is what this object's open()
    // will enumerate instead of asking WinUSB, and `opener` is what it will
    // call instead of openWinUsb(). There is no Mirics device on the bench this
    // driver was written on: the fake that implements cascade::usb::UsbDevice
    // and records every transfer IS the proof, so the seam that admits it is
    // part of the design and not a back door bolted on.
    using UsbOpenFn =
        std::function<std::unique_ptr<cascade::usb::UsbDevice>(const std::string& path,
                                                               std::string& error)>;
    void setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices, UsbOpenFn opener);

    // --- DeviceSource ----------------------------------------------------

    const char* driverKey() const override { return "mirisdr"; }

    // Takes a NativeDeviceInfo::args string: "serial=<text>" picks a device by
    // the serial SetupAPI reported (case-insensitive, and a suffix match),
    // "index=N" picks the Nth in enumeration order. An empty string takes the
    // first.
    //
    // A successful open has QUIETENED the device and then put it into a known
    // state: streaming stopped and the ADC asleep first, because a device left
    // running by whatever had it before us would deliver somebody else's
    // transfer as our first one; then the chip's initialisation sequence, then
    // 2 MS/s, 100 MHz, the low-noise amplifier and mixer both in circuit and
    // the baseband amplifier at 30 dB of its 59. Nothing is streamed until
    // start().
    bool open(const std::string& args) override;

    // Stops the stream, releases the device. Idempotent, safe on a
    // never-opened instance, and safe to call from the destructor.
    // lastError() survives it, so a failure reason outlives the cleanup.
    void closeDevice() override;

    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // Four stages, and one of them is deliberately not in decibels:
    //
    //   LNA        0 or 24 dB - a switch, and not in circuit below 50 MHz
    //   MIXER      0 or 19 dB - a switch
    //   BASEBAND   0 to 59 dB in 1 dB steps - the continuous one
    //   AM BUFFER  0 to 3 STEPS - what stands in for the LNA on the AM inputs
    //
    // The buffer is steps because what a step is worth depends on which AM
    // port the band plan selected (0/6/12/18 dB on port 1, 0 or 24 dB on port
    // 2), and a decibel figure that changes meaning with the band is a figure
    // no panel can letter honestly. GainUnit::Steps exists for exactly this.
    //
    // Out-of-range values are CLAMPED and then rounded to what the hardware
    // can hold, and gainDb() reports what was actually programmed - the
    // DeviceSource contract, and what stops a panel showing 24 dB of LNA on a
    // medium-wave frequency where there is no LNA at all.
    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;

    // The reference has an automatic gain mode whose two register writes are
    // COMMENTED OUT and whose function body does nothing at all. There is
    // therefore no verified sequence to send, and a switch that claims to do
    // something and does not is worse than one that refuses.
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return false; }

    // One receive port. The AM input the tuner uses is chosen by the BAND
    // PLAN, not by the operator - crossing 50 MHz switches it - so offering it
    // as an antenna would be offering a choice that the next tune overrides.
    std::vector<std::string> antennas() const override { return {"RX"}; }
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override { return "RX"; }

    // The rates the Source panel offers. The hardware accepts anything between
    // 1.3 and 15 MS/s - it is a divider and a fraction, not a table - so this
    // is a menu rather than a constraint, and setSampleRateHz does not snap to
    // it. IT DELIBERATELY SKIPS 8.064 to 9.216 MS/s: that window is the one
    // packing whose bit layout could not be derived exactly from the reference
    // (see msi2500.hpp), and a menu entry is an invitation.
    std::vector<double> supportedSampleRatesHz() const override;

    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource --------------------------------------------------------

    // Queues the bulk ring, starts the device streaming and spawns the reader,
    // in that order - a device told to stream with nothing queued fills its own
    // buffer and overruns before the host's first read. Idempotent while
    // running; false with lastError() when there is no device.
    bool start() override;

    // Streaming off, reader joined (bounded, see the file header), bulk ring
    // torn down. Idempotent, safe before open.
    void stop() override;

    bool running() const override { return running_.load(std::memory_order_relaxed); }

    // The radio paces this source: the pipeline must not clock it.
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // Coerced to what the hardware accepts: above 15 MS/s is clamped DOWN to
    // 15, and below 1.3 MS/s is REFUSED with a reason rather than quietly
    // raised, because a caller asking for 1 MS/s wants narrowband behaviour
    // and silently giving it 1.3 would be a lie the spectrum would not reveal.
    //
    // The baseband filter follows the rate, and BECAUSE THE FILTER LIVES IN
    // THE TUNER the tune words are re-sent afterwards - see the file header.
    //
    // ON A RUNNING STREAM the change is made with the radio QUIET: streaming
    // off, reader stopped, bulk ring torn down, the ADC put to sleep, the new
    // rate programmed, then the ring, streaming and the reader again.
    // running() reads true throughout on the success path. That is the shape
    // 0.89.0 had to give the Soapy path after a live rate change killed the
    // process on the driver's own reader thread.
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }

    // Live: six control transfers for the tune and two for the gain that
    // follows it, no stream interruption. Refused (with a reason) outside
    // 150 kHz - 2 GHz rather than clamped.
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

    // --- the bias-T ------------------------------------------------------
    //
    // A bit in the same register as the band switch, putting power on the
    // antenna port for an amplifier at the mast. It is deliberately NOT an
    // antenna and not a gain: it is power on a connector, and a control that
    // can damage a receiver connected to the wrong thing should never be
    // reachable by something iterating a list of port names.
    //
    // Not part of DeviceSource - the interface has no notion of it yet - so
    // the Source panel reaches these two through the concrete type when it
    // grows a control for it.
    bool setBiasT(bool on);
    bool biasT() const { return biasT_.load(std::memory_order_relaxed); }

    // --- what the tuner is actually doing ---------------------------------
    // Which band the last tune landed in, and which filter width followed the
    // rate. Kept because "which band was it in" is the first question any
    // Mirics problem report needs answered, and the answer is not derivable
    // from the frequency alone once a device takes the other band plan.
    msi001::Band band() const;
    double bandwidthHz() const;
    msi2500::Format sampleFormat() const;

    // --- stream health ---------------------------------------------------
    //
    // Same tally and the same one-line format SoapySource writes: a line always
    // for the first window after a start, so a healthy radio leaves one
    // proving it, and after that only for a window with something to report.
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

    // Tests only: shorten the window so the reader's own reporting can be seen
    // without waiting a minute for it.
    void setStreamHealthWindowForTest(std::chrono::milliseconds w);

    // Transfers the reader has had to drop because the ring was full - the
    // host fell behind, not the radio. Counted rather than merely logged
    // because a timing assertion cannot tell a drop from a slow run.
    std::uint64_t droppedTransfers() const;

    // Reader threads this PROCESS has abandoned because they did not come back
    // within kReaderJoinWait. 0 on every healthy path; the delta is what a
    // test asserts, because elapsed time alone still passes when the bound is
    // deleted.
    static unsigned long long readersAbandoned();

private:
    // Everything the reader thread touches, in one object behind a shared_ptr
    // it captures BY VALUE - see the file header. An abandoned reader outlives
    // the MiriSdrSource that started it, and a thread reading freed members
    // would be a worse defect than the hang the bound exists to prevent.
    struct ReaderLink {
        explicit ReaderLink(std::size_t ringCapacity) : ring(ringCapacity) {}

        std::atomic<bool> run{false};

        // NOT owned here. The MiriSdrSource owns the handle, except on the
        // abandoning path, which deliberately leaks it so a stranded reader
        // still has a live object to be inside.
        cascade::usb::UsbDevice* dev = nullptr;

        // Which packing the reader is unpacking. Written under devMutex_ while
        // the reader is stopped - every path that changes the format stops the
        // stream first - and read on the reader's own thread, so it is an
        // atomic rather than a plain member even though the ordering makes a
        // race impossible today.
        std::atomic<int> format{static_cast<int>(msi2500::Format::Bits16)};

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
        // pipeline's source loop. A std::string written on one thread and read
        // on another is UB, not a stale value.
        mutable std::mutex errorMutex;
        std::string lastError;
        bool faulted = false;
        bool deviceDead = false;
        std::string deadWhat;

        mutable std::mutex healthMutex;
        StreamHealth health;
        bool healthEverWritten = false;
        std::chrono::milliseconds healthWindow = kStreamHealthWindow;
        std::atomic<std::uint64_t> dropped{0};
    };

    // NEVER REASSIGNED, hence const: an abandoned reader holds its own copy of
    // this pointer, so a source that swapped in a fresh link could have a new
    // device behind a thread still pumping the old one.
    const std::shared_ptr<ReaderLink> link_ = std::make_shared<ReaderLink>(kRingCapacitySamples());

    // The ring holds every transfer that can be in flight at the widest
    // packing, twice over: 8 transfers of 64 blocks of 504 pairs is 258048
    // samples, and the next power of two above twice that is 2^19.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 19; }

    // The *Locked helpers assume devMutex_ is held. Public entry points take
    // it; privates never do, so open() can run a whole opening sequence under
    // one lock without recursing into it.
    bool writeRegisterLocked(std::uint8_t reg, std::uint32_t val, const char* what);
    // `timeoutMs` is the ordinary control bound everywhere but the teardown -
    // see msi2500::kTeardownControlTimeout for why that one is shorter.
    bool commandLocked(msi2500::VendorRequest r, const char* what,
                       unsigned timeoutMs = msi2500::kControlTimeoutMs);

    bool programRateLocked(const msi2500::RateSetting& r, const char* what);
    bool programTuneLocked(double hz, const char* what);
    bool programGainLocked(const char* what);
    bool programBandSwitchLocked(const char* what);

    // Queue the bulk ring, start the device streaming, spawn the reader.
    // Assumes devMutex_ held and the device open and not already running.
    bool startStreamingLocked();
    // Streaming off, join the reader (bounded), tear the bulk ring down.
    // Idempotent; assumes devMutex_ held.
    void stopStreamingLocked();

    // THE READER'S OWN HELPERS ARE STATIC AND TAKE THE LINK, not `this`. An
    // abandoned reader outlives the MiriSdrSource; a member function reaching
    // for a member of a destroyed object is precisely the defect the link was
    // introduced to prevent, and making these static is what stops the
    // compiler from letting one be written by accident.
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
    // start/stop/open/close. The reader thread does not take it: it only ever
    // calls readBulk on the bulk pipe, which the transport's rule 2 allows
    // concurrently with synchronous control transfers - so a retune does not
    // wait for a transfer to complete.
    mutable std::mutex devMutex_;

    // Owned, except after an abandonment - see the file header.
    std::unique_ptr<cascade::usb::UsbDevice> dev_;
    std::thread reader_;

    // Lock-free mirrors, so per-frame GUI readouts never wait behind a control
    // transfer in flight.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<bool> biasT_{false};

    // The tuner's own state, all under devMutex_. `stages_` is what the four
    // gain controls hold, `band_` is where the last tune landed, and
    // `bandSelectWord_` is what register 8 carries - kept because the bias-T
    // shares that register and must not clear it.
    msi001::GainStages stages_;
    msi001::Band band_ = msi001::Band::Vhf;
    msi001::Plan plan_ = msi001::Plan::Default;
    msi001::Bandwidth bandwidth_ = msi001::Bandwidth::Mhz8;
    msi001::IfMode ifMode_ = msi001::IfMode::Zero;
    std::uint32_t bandSelectWord_ = 0;
    msi2500::Format format_ = msi2500::Format::Bits16;

    mutable std::mutex nameMutex_;
    std::string name_ = "Mirics: (no device)";

    // The test seam (see setTransportForTest). Empty opener means the real
    // WinUSB transport.
    std::vector<cascade::usb::UsbDeviceInfo> fakeDevices_;
    UsbOpenFn fakeOpener_;
    bool useFakeTransport_ = false;
};

}  // namespace cascade::source
