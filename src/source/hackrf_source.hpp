// hackrf_source.hpp - FoxSDR's own HackRF One driver: a DeviceSource that
// speaks the vendor protocol in src/source/hackrf_protocol.hpp over the
// WinUSB transport in src/usb/usb_device.hpp, with no libhackrf, no libusb
// and no SoapySDR module anywhere in the path.
//
// WHY A NATIVE DRIVER AT ALL is argued in usb_device.hpp: every crash this
// product has received from a USB radio landed inside somebody else's libusb,
// on a thread we did not create, behind a vendor module we could not fix. The
// three rules that header states are what this file is built on -
// enumeration never opens a device, WE own every thread, every wait is
// bounded - and the shape below is the consequence of the third one.
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
// pointer, the ring, the error slot, the health tally - lives in a ReaderLink
// behind a shared_ptr that the thread captures BY VALUE, and the abandoning
// path deliberately leaks the UsbDevice rather than destroying it under a
// thread that is still inside it.
//
// THE LOCKS. devMutex_ serialises control transfers against each other and
// against start/stop/open/close. The reader thread does NOT take it: it only
// ever calls readBulk on the bulk pipe, which the transport's rule 2 is
// written to allow concurrently with synchronous control transfers. So a
// retune costs one control transfer and does not wait for a transfer to
// complete, which is the whole reason for having our own transport. The
// error slot and the health tally keep their own small mutexes inside the
// link, because the reader writes them while the GUI reads them.
//
// WHAT IS NOT HERE. Transmit, in any form: no request number for it is even
// named in hackrf_protocol.hpp. Sweep mode, Opera Cake, CPLD and SPI-flash
// access, the M0 state, the clock in/out controls - none of it. This is a
// receiver driver, and a request it cannot name is a request it cannot send.
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
#include "source/hackrf_protocol.hpp"
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

// How long readBulk blocks for a completed transfer before answering "nothing
// yet". At 20 MS/s a 262144-byte transfer is 6.6 ms of signal, so this expires
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

// --- enumeration ----------------------------------------------------------

// Every HackRF-family device bound to WinUSB, as the Source section wants
// them. NEVER OPENS A DEVICE (usb_device.hpp rule 1): the label and the args
// are built from what SetupAPI already knows.
//
// On a non-Windows build this is empty and says so in the log: the transport
// is WinUSB, and a Linux HackRF is reached through SoapySDR until a libusb
// backend exists behind the same interface.
std::vector<NativeDeviceInfo> enumerateHackRf();

// The pure half of the above: which of a list of USB devices are HackRFs, and
// what each one's label and args string is. Separated out because it is the
// only part that can be proven without a radio on the bench, and because the
// args string it produces is the string open() has to take back.
std::vector<NativeDeviceInfo> hackRfDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices);

// The VID/PID list enumerateHackRf() asks the transport for.
std::vector<cascade::usb::UsbId> hackRfUsbIds();

// --- the driver -----------------------------------------------------------

class HackRfSource : public DeviceSource {
public:
    HackRfSource() = default;
    ~HackRfSource() override;

    // Owns a device handle and a thread; copying either would be a
    // double-close or a double-join.
    HackRfSource(const HackRfSource&) = delete;
    HackRfSource& operator=(const HackRfSource&) = delete;

    // TESTS ONLY, and per instance rather than per process so two tests can
    // never see each other's transport. `devices` is what this object's
    // open() will enumerate instead of asking WinUSB, and `opener` is what it
    // will call instead of openWinUsb(). There is no HackRF on the bench this
    // driver was written on: the fake that implements cascade::usb::UsbDevice
    // and answers byte-for-byte as the firmware does IS the proof, so the seam
    // that admits it is part of the design and not a back door bolted on.
    using UsbOpenFn =
        std::function<std::unique_ptr<cascade::usb::UsbDevice>(const std::string& path,
                                                               std::string& error)>;
    void setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices, UsbOpenFn opener);

    // --- DeviceSource ----------------------------------------------------

    const char* driverKey() const override { return "hackrf"; }

    // Takes a NativeDeviceInfo::args string: "serial=<hex>" picks a device by
    // the serial SetupAPI reported (case-insensitive, and a suffix match, so
    // the short form a user reads off the case still finds it), "index=N"
    // picks the Nth in enumeration order. An empty string takes the first.
    //
    // A successful open has done four things: opened the pipe, read the board
    // id, firmware version and part-id/serial back off the device, and put the
    // radio into a KNOWN STATE - 10 MS/s with the filter that goes with it,
    // 100 MHz, LNA 16 dB, VGA 16 dB, amplifier off, bias-T off. The last of
    // those is not cosmetic: a HackRF remembers what the last application left
    // it at, including a bias-T feeding 3.3 V into somebody's antenna, and a
    // driver that inherits that state silently cannot report what the radio is
    // doing. Nothing is streamed until start().
    bool open(const std::string& args) override;

    // Stops the stream, releases the device. Idempotent, safe on a
    // never-opened instance, and safe to call from the destructor.
    // lastError() survives it, so a failure reason outlives the cleanup.
    void closeDevice() override;

    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // LNA (0-40 dB, 8 dB steps), VGA (0-62 dB, 2 dB steps) and AMP (the
    // front-end amplifier, presented as a gain of 0 or 14 dB). Out-of-range
    // values are CLAMPED and then rounded down to the hardware's step, and
    // gainDb() reports what was actually programmed - the DeviceSource
    // contract, and what stops a panel showing 20 dB of LNA the radio never
    // had.
    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;

    // The HackRF has no AGC of any kind: there is no register for it, so
    // there is nothing to expose and setAutoGain always refuses.
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return false; }

    // One RX port. The bias-T is NOT an antenna: it is 3.3 V on the same
    // connector, and putting it in this list would make it selectable by
    // something that thinks it is choosing where to listen. See setBiasT.
    std::vector<std::string> antennas() const override { return {"RX"}; }
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override { return "RX"; }

    // The rates the Source panel offers. The hardware accepts ANY rate
    // between 2 and 20 MS/s - it is a fractional divider, not a table - so
    // this is a menu rather than a constraint, and setSampleRateHz does not
    // snap to it. It exists because a panel needs something to show.
    std::vector<double> supportedSampleRatesHz() const override;

    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource --------------------------------------------------------

    // libhackrf's hackrf_start_rx order (hackrf.c:2339-2352): the transceiver
    // into RECEIVE FIRST, then the bulk ring, then the reader thread. The ring
    // must NOT go first: firmware 2018.01.1 and earlier disables bulk endpoint
    // 0x81 on every mode change and enables it only for RECEIVE
    // (usb_api_transceiver.c set_transceiver_mode), so reads queued before
    // RECEIVE are reads against a disabled endpoint - the Airspy firmware,
    // which shares that code, fails them with Windows error 31 on real
    // hardware. Firmware from v2021.03.1 on only flushes, and does not care.
    // Idempotent while running; false with lastError() when there is no
    // device.
    bool start() override;

    // libhackrf's hackrf_stop_rx order (hackrf.c:2369-2378): the reader told
    // to stop and joined (bounded, see the file header), the bulk ring torn
    // down, and only THEN the transceiver OFF - so no read is left on the pipe
    // for an old firmware's endpoint disable to fail and have mistaken for a
    // dead radio. Idempotent, safe before open.
    void stop() override;

    bool running() const override { return running_.load(std::memory_order_relaxed); }

    // The radio paces this source: the pipeline must not clock it.
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // Coerced to what the hardware accepts: above 20 MS/s is clamped DOWN to
    // 20, and below 2 MS/s is REFUSED with a reason rather than quietly
    // raised, because a caller asking for 1 MS/s wants narrowband behaviour
    // and silently giving it 2 would be a lie the spectrum would not reveal.
    //
    // The baseband filter follows the rate automatically, to the widest
    // MAX2837 setting no more than 75% of it - libhackrf does the same inside
    // hackrf_set_sample_rate_manual, and a rate change that left the old
    // filter behind would alias or throw away half the span.
    //
    // ON A RUNNING STREAM the change is made with the radio QUIET: reader
    // stopped, bulk ring torn down, transceiver OFF, the new rate and filter
    // programmed, then RECEIVE, the ring and the reader again. running()
    // reads true throughout on the success path. That is the shape 0.89.0 had
    // to give the Soapy path after a live rate change killed the process on
    // the driver's own reader thread; there is no reason to learn it twice.
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }

    // Live: one control transfer, no stream interruption. Refused (with a
    // reason) outside 1 MHz - 6 GHz rather than clamped, because a tune that
    // silently lands somewhere else is worse than one that does not happen.
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
    // ANTENNA_ENABLE puts 3.3 V at up to 50 mA on the antenna port, for a
    // powered LNA at the mast. It is deliberately NOT in antennas(): it is
    // not a choice of where to listen, it is power on a connector, and a
    // control that can damage a receiver connected to the wrong thing should
    // never be reachable by something iterating a list of port names.
    //
    // Not part of DeviceSource - the interface has no notion of it yet - so
    // the Source panel reaches these two through the concrete type when it
    // grows a control for it.
    bool setBiasT(bool on);
    bool biasT() const { return biasT_.load(std::memory_order_relaxed); }

    // --- what open() read off the device ---------------------------------
    // Empty / zero before a successful open. Kept because "which board is
    // this and what firmware is on it" is the first question any HackRF
    // problem report needs answered, and asking the radio later means a
    // control transfer in the middle of a stream.
    std::uint8_t boardId() const;
    std::string firmwareVersion() const;
    std::string partIdSerialNo() const;

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

    // Tests only: does the reader's link still point at a device?
    //
    // THERE IS NO OTHER WAY TO SEE THE STATE THIS EXISTS FOR. After a reader
    // is abandoned the driver leaks the UsbDevice on purpose and leaves
    // link_->dev pointing at it, because the stranded thread dereferences
    // that pointer at the top of every loop; closeDevice() clearing it anyway
    // is a data race with that read, and on the iteration where the zombie
    // has just passed its `run` check it is a null dereference. The zombie
    // never calls back into the fake once `run` is false, so no transport
    // fake can observe it - only the driver can be asked. Read-only, taken
    // under the device mutex, and called from nowhere in the product.
    bool linkHoldsDeviceForTest() const;

private:
    // Everything the reader thread touches, in one object behind a shared_ptr
    // it captures BY VALUE - see the file header. An abandoned reader outlives
    // the HackRfSource that started it, and a thread reading freed members
    // would be a worse defect than the hang the bound exists to prevent.
    struct ReaderLink {
        explicit ReaderLink(std::size_t ringCapacity) : ring(ringCapacity) {}

        std::atomic<bool> run{false};

        // NOT owned here. The HackRfSource owns the handle, except on the
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
        std::chrono::milliseconds healthWindow = kStreamHealthWindow;
        std::atomic<std::uint64_t> dropped{0};
    };

    // NEVER REASSIGNED, hence const: an abandoned reader holds its own copy of
    // this pointer, so a source that swapped in a fresh link could have a new
    // device behind a thread still pumping the old one.
    const std::shared_ptr<ReaderLink> link_ = std::make_shared<ReaderLink>(kRingCapacitySamples());

    // The ring holds kTransferCount whole transfers plus headroom, rounded up
    // to the power of two SpscRing requires. At 20 MS/s that is 26 ms of
    // signal, against a pipeline that asks for 10 ms chunks.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 19; }

    // The *Locked helpers assume devMutex_ is held. Public entry points take
    // it; privates never do, so open() can run a whole opening sequence under
    // one lock without recursing into it.
    bool controlOutLocked(hackrf::VendorRequest r, std::uint16_t value, std::uint16_t index,
                          const std::uint8_t* data, std::size_t len, const char* what);
    // `moved`, when given, receives the byte count the device actually
    // answered with. VERSION_STRING_READ is as long as the firmware's string
    // and no longer, so a caller that turns the answer into text needs the
    // count rather than trusting a terminator the device never wrote.
    bool controlInLocked(hackrf::VendorRequest r, std::uint16_t value, std::uint16_t index,
                         std::uint8_t* data, std::size_t len, const char* what,
                         std::size_t* moved = nullptr);

    bool setTransceiverModeLocked(hackrf::TransceiverMode mode, const char* what);
    bool programRateLocked(const hackrf::RateSetting& r, const char* what);
    bool programFrequencyLocked(double hz, const char* what);
    bool programLnaLocked(double db);
    bool programVgaLocked(double db);
    bool programAmpLocked(bool on);
    bool programBiasTLocked(bool on);

    // RECEIVE, queue the bulk ring, spawn the reader - libhackrf's order (see
    // start()). Assumes devMutex_ held and the device open and not already
    // running.
    bool startStreamingLocked();
    // Lower the reader's run flag, join the reader (bounded), tear the bulk
    // ring down, THEN transceiver off - libhackrf's order (see stop()).
    // Idempotent; assumes devMutex_ held.
    void stopStreamingLocked();

    // THE READER'S OWN HELPERS ARE STATIC AND TAKE THE LINK, not `this`. An
    // abandoned reader outlives the HackRfSource; a member function reaching
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
    std::atomic<double> lnaDb_{0.0};
    std::atomic<double> vgaDb_{0.0};
    std::atomic<double> ampDb_{0.0};
    std::atomic<bool> biasT_{false};

    mutable std::mutex nameMutex_;
    std::string name_ = "HackRF: (no device)";

    // Identity, read once at open under devMutex_.
    std::uint8_t boardId_ = 0;
    std::string firmwareVersion_;
    std::string partIdSerialNo_;

    // The test seam (see setTransportForTest). Empty opener means the real
    // WinUSB transport.
    std::vector<cascade::usb::UsbDeviceInfo> fakeDevices_;
    UsbOpenFn fakeOpener_;
    bool useFakeTransport_ = false;
};

}  // namespace cascade::source
