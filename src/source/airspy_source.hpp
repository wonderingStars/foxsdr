// airspy_source.hpp - FoxSDR's own Airspy R2 / Mini driver: a DeviceSource
// that speaks the vendor protocol in src/source/airspy_protocol.hpp over the
// WinUSB transport in src/usb/usb_device.hpp, with no libairspy, no libusb and
// no SoapySDR module anywhere in the path.
//
// WHY A NATIVE DRIVER AT ALL is argued in usb_device.hpp, and the three rules
// that header states are what this file is built on - enumeration never opens
// a device, WE own every thread, every wait is bounded. The shape below is the
// HackRF driver's, deliberately and almost line for line: same reader-thread
// ownership, same ReaderLink behind a shared_ptr, same bounded join with
// abandonment, same stream-health line. Two drivers that solve the same
// problem differently are two drivers to reason about separately, and the
// argument was already paid for once.
//
// THE THREAD, AND WHY IT OUTLIVES THIS OBJECT. One reader thread pulls
// completed bulk transfers, unpacks and converts them and writes them into a
// ring; read() drains the ring on the pipeline's source thread. Stopping it is
// signal, join, then endBulkStream() - in that order, because the transport's
// ring is freed by endBulkStream and a readBulk still in flight would be
// reading memory it has just released. The join is BOUNDED
// (kReaderJoinWait): a reader that has not come back by then is abandoned
// rather than waited for, because a hang on the GUI thread is worse than a
// leak. An abandoned reader must still have somewhere valid to run, so
// everything it touches lives in a ReaderLink behind a shared_ptr that the
// thread captures BY VALUE, and the abandoning path deliberately leaks the
// UsbDevice rather than destroying it under a thread that is still inside it.
//
// WHAT IS DIFFERENT FROM THE HACKRF, and all of it is the radio's doing:
//
//  1. THE AIRSPY'S SAMPLES ARE REAL, NOT I/Q. A 12-bit real ADC at twice the
//     advertised rate; the host translates by fs/4, half-band filters and
//     decimates by two. The conversion is in airspy_protocol.hpp with its
//     derivation; what matters here is that the rate this source reports is
//     the COMPLEX rate and it is half the ADC's.
//  2. THE RATES ARE A LIST THE DEVICE READS OUT, not a formula. open() asks
//     the firmware (GET_SAMPLERATES) and supportedSampleRatesHz() answers with
//     exactly what it said; setSampleRateHz coerces to the nearest of them and
//     programs it BY INDEX. An R2 answers 10 and 2.5 MS/s, a Mini answers its
//     own set; this driver does not know which in advance and does not need to.
//  3. THE SAMPLES ARRIVE PACKED. Three bytes per two samples, so 12 bits of
//     ADC cost 12 bits of USB rather than 16. Enabled at open.
//  4. THERE ARE FIVE GAINS, three of them registers (LNA, MIXER, VGA) and two
//     of them libairspy's curated walks up all three at once (LINEARITY,
//     SENSITIVITY). And unlike the HackRF there IS an AGC - two of them, one
//     per stage - so autoGainSupported() is true here.
//
// WHAT IS NOT HERE. The SPI flash, the Si5351C, direct R820T register access,
// the GPIO direction registers, the Microsoft OS descriptor request: a
// receiver driver has no business writing a radio's firmware storage or
// driving its tuner behind the firmware's back. The one GPIO this file does
// touch is the bias tee, because that is where libairspy puts it.
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
#include "source/airspy_protocol.hpp"
#include "source/device_source.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source {

// --- enumeration ----------------------------------------------------------

// Every Airspy bound to WinUSB, as the Source section wants them. NEVER OPENS
// A DEVICE (usb_device.hpp rule 1): the label and the args are built from what
// SetupAPI already knows.
//
// On a non-Windows build this is empty and says so in the log: the transport
// is WinUSB, and a Linux Airspy is reached through SoapySDR until a libusb
// backend exists behind the same interface.
std::vector<NativeDeviceInfo> enumerateAirspy();

// The pure half of the above: which of a list of USB devices are Airspys, and
// what each one's label and args string is. Separated out because it is the
// only part that can be proven without a radio on the bench, and because the
// args string it produces is the string open() has to take back.
std::vector<NativeDeviceInfo> airspyDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices);

// The VID/PID list enumerateAirspy() asks the transport for. One entry: the
// whole family shares 0x1D50:0x60A1.
std::vector<cascade::usb::UsbId> airspyUsbIds();

// WHICH AIRSPY THIS IS, from a string, and why it is not read off the board id.
//
// The brief this driver was written to said the model comes from the board id.
// IT DOES NOT, and the reference says so plainly: airspy.h:78-82 has exactly
// two board ids, AIRSPY_BOARD_ID_PROTO_AIRSPY = 0 and INVALID = 0xFF, and
// airspy.c:1994-2007 maps the first to the single name "AIRSPY". An R2 and a
// Mini both answer 0, and they share a product id as well, so nothing that can
// be read before or after opening the device NAMES the model except the
// strings it carries: the USB product description SetupAPI already has
// (enumeration, no device opened) and the firmware version string open() reads
// back. Both spell "MINI" on a Mini.
//
// So this is a substring test over whichever of those two strings is
// available, and it answers "Airspy" when neither says. Inventing a board-id
// table that the reference does not have would have been a plausible-sounding
// wrong answer of exactly the kind that costs a debugging session.
std::string airspyModelFrom(const std::string& text);

// --- the driver -----------------------------------------------------------

class AirspySource : public DeviceSource {
public:
    // --- the bounded waits ------------------------------------------------
    //
    // Every one of these is named rather than written at its call site because
    // tests/test_shutdown_budget.cpp discovers `constexpr std::chrono`
    // constants under src/ and refuses to go green until each is classified in
    // its kKnownWaits table. A guard that ignores what it has not been
    // introduced to is not a guard.
    //
    // THEY ARE CLASS MEMBERS RATHER THAN NAMESPACE CONSTANTS, which is the one
    // place this file departs from hackrf_source.hpp's layout, and it is not a
    // preference: that header declares kBulkReadWait, kReadWait,
    // kReaderJoinWait and kStreamHealthWindow at cascade::source scope, and
    // gui/app_window.hpp includes every driver header at once. Four identically
    // named constants in one namespace is a redefinition, so these live here -
    // where rtlsdr_source.hpp already put its own kStreamHealthWindow for the
    // same reason. The budget's scan finds a class-static perfectly well.

    // How long readBulk blocks for a completed transfer before answering
    // "nothing yet". At 20 MS/s of real samples a 147456-byte packed transfer
    // is 4.9 ms of signal, so this expires only when the radio has genuinely
    // stopped delivering - and it is what bounds how long the reader thread
    // takes to notice it has been asked to stop.
    static constexpr std::chrono::milliseconds kBulkReadWait{100};

    // How long read() waits for the reader to put something in the ring before
    // returning the IqSource contract's "nothing yet, retry" zero. The
    // pipeline's self-paced loop then backs off a millisecond and asks again.
    static constexpr std::chrono::milliseconds kReadWait{20};

    // The bound on joining the reader thread (see the file header).
    // Comfortably more than kBulkReadWait, because the ordinary exit costs at
    // most one of those; anything past it is a reader that is not coming back.
    static constexpr std::chrono::milliseconds kReaderJoinWait{1000};

    // The stream-health window, matching SoapySource::kStreamHealthWindow. Not
    // a wait: nothing sleeps or blocks on it. It is how much streaming the
    // reader tallies before it writes one "source: stream health ..." line.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};

    // How many times in a row the reader will RE-ARM the stream after a failed
    // bulk read before it calls the radio dead. A re-arm is libairspy's own
    // start sequence again (receiver off, clear the halt on 0x81, receiver
    // on, queue the ring), and it is what clears a halted pipe. libairspy
    // itself stops streaming on the first failed transfer and leaves the
    // application to notice; this driver did the same and put "unplug it and
    // plug it back in" on screen for a radio that was working (field reports,
    // 0.99.27/0.99.28). Three, because a pipe that is still failing after the
    // third clean restart is not a transient, and every attempt is a warning
    // line in the log. The count starts again after kRearmForgiveAfter of
    // clean streaming, so one halt an hour does not use up the session.
    static constexpr int kMaxStreamRearms = 3;
    // Not a wait - nothing sleeps on it; it is how long samples must flow
    // after a re-arm before the budget above is refilled.
    static constexpr std::chrono::milliseconds kRearmForgiveAfter{2000};

    AirspySource() = default;
    ~AirspySource() override;

    // Owns a device handle and a thread; copying either would be a
    // double-close or a double-join.
    AirspySource(const AirspySource&) = delete;
    AirspySource& operator=(const AirspySource&) = delete;

    // TESTS ONLY, and per instance rather than per process so two tests can
    // never see each other's transport. `devices` is what this object's open()
    // will enumerate instead of asking WinUSB, and `opener` is what it will
    // call instead of openWinUsb(). There is no Airspy on the bench this
    // driver was written on: the fake that implements cascade::usb::UsbDevice
    // and answers byte-for-byte as the firmware does IS the proof, so the seam
    // that admits it is part of the design and not a back door bolted on.
    using UsbOpenFn =
        std::function<std::unique_ptr<cascade::usb::UsbDevice>(const std::string& path,
                                                               std::string& error)>;
    void setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices, UsbOpenFn opener);

    // --- DeviceSource ----------------------------------------------------

    const char* driverKey() const override { return "airspy"; }

    // Takes a NativeDeviceInfo::args string: "serial=<hex>" picks a device by
    // the serial SetupAPI reported (case-insensitive, and a suffix match, so
    // the short form a user reads off another tool's listing still finds it),
    // "index=N" picks the Nth in enumeration order. An empty string takes the
    // first.
    //
    // A successful open has done five things: opened the pipe, read the board
    // id, firmware version and part-id/serial back off the device, READ THE
    // RATE LIST OUT OF THE FIRMWARE (there is no other way to know what this
    // model can do), turned packing on, and put the radio into a KNOWN STATE -
    // the first rate the firmware listed, 100 MHz, both AGCs off, LNA/MIXER/VGA
    // at 8, bias tee off. The last of those is not cosmetic: an Airspy
    // remembers what the last application left it at, including a bias tee
    // feeding 4.5 V into somebody's antenna, and a driver that inherits that
    // state silently cannot report what the radio is doing. Nothing is
    // streamed until start().
    bool open(const std::string& args) override;

    // Stops the stream, releases the device. Idempotent, safe on a
    // never-opened instance, and safe to call from the destructor.
    // lastError() survives it, so a failure reason outlives the cleanup.
    void closeDevice() override;

    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // FIVE GAINS, AND THE UNITS ARE THE HARDWARE'S OWN STEPS, NOT DECIBELS.
    // libairspy takes an index for every one of them (airspy.h:182-206) and
    // publishes no mapping to dB; GainInfo's fields are named minDb/maxDb
    // because the first radio behind them had a dB control, and putting an
    // invented decibel scale here so the field name reads true would be a
    // number on screen that nothing in the world produced.
    //
    //   LNA          0..14   R820T low-noise amplifier    (airspy.c:1691)
    //   MIXER        0..15   R820T mixer                  (airspy.c:1721)
    //   VGA          0..15   R820T IF amplifier           (airspy.c:1751)
    //   LINEARITY    0..21   all three at once, headroom  (airspy.c:1829)
    //   SENSITIVITY  0..21   all three at once, weak signals (airspy.c:1863)
    //
    // The last two are libairspy's own tables and each of them REWRITES the
    // first three, so gainDb("LNA") after a LINEARITY change reports what the
    // table actually programmed rather than what was there before.
    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;

    // Unlike the HackRF, the Airspy HAS automatic gain control - two of them,
    // SET_LNA_AGC and SET_MIXER_AGC (airspy.c:1775, :1802). setAutoGain drives
    // both, because half an AGC is a configuration nobody asked for.
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return autoGain_.load(std::memory_order_relaxed); }

    // One RX port. The bias tee is NOT an antenna: it is power on the same
    // connector, and putting it in this list would make it selectable by
    // something that thinks it is choosing where to listen. See setBiasT.
    std::vector<std::string> antennas() const override { return {"RX"}; }
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override { return "RX"; }

    // WHAT THE DEVICE SAID, ascending, in Hz - and these are COMPLEX rates
    // (airspy_protocol.hpp has the three places in the reference that prove
    // it). Empty before a successful open, because until the firmware has been
    // asked there is no honest answer. The reference's own fallback list when
    // the request fails is {10 MS/s, 2.5 MS/s} (airspy.c:902-906) and this
    // driver uses the same one for the same reason.
    std::vector<double> supportedSampleRatesHz() const override;

    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource --------------------------------------------------------

    // libairspy's airspy_start_rx, step for step (airspy.c:1192-1218):
    // RECEIVER_MODE off, clear the halt on the RX pipe, RECEIVER_MODE RX, and
    // only THEN queue the bulk ring (create_io_threads -> prepare_transfers,
    // :549-559) and start the reader. The ring must NOT go first: the firmware
    // disables bulk endpoint 0x81 on every receiver-mode change and enables it
    // only on RX (airspyone_firmware airspy_m0/airspy_rx.c set_receiver_mode),
    // so reads queued before RX are reads against a disabled endpoint - and on
    // real R2s they came back as Windows error 31 on the very first read.
    // Idempotent while running; false with lastError() when there is no
    // device.
    bool start() override;

    // The reader is told to stop FIRST, then RECEIVER_MODE off, then the
    // reader is joined (bounded, see the file header) and the ring torn down.
    // In that order because OFF disables the endpoint under the reads still
    // queued on it and they fail - which is expected, and must not be taken
    // for a dead radio (libairspy's airspy_stop_rx raises stop_requested
    // before it sends OFF for the same reason, airspy.c:1220-1235).
    // Idempotent, safe before open.
    void stop() override;

    bool running() const override { return running_.load(std::memory_order_relaxed); }

    // The radio paces this source: the pipeline must not clock it.
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // COERCED TO THE NEAREST RATE THE DEVICE LISTED, never refused and never
    // approximated: the hardware has a menu, not a divider, so there is no
    // such thing as an unsupported-but-close rate to fail on. The readback
    // then reports what was actually programmed, which is the DeviceSource
    // contract and what stops a panel showing 8 MS/s a radio never had.
    //
    // ON A RUNNING STREAM the change is made with the radio QUIET: receiver
    // off, reader stopped, bulk ring torn down, the pipe reset (libairspy
    // clears the halt itself at airspy.c:1147 before every SET_SAMPLERATE),
    // the new index programmed, then the ring, RX and the reader again.
    // running() reads true throughout on the success path. That is the shape
    // 0.89.0 had to give the Soapy path after a live rate change killed the
    // process on the driver's own reader thread; there is no reason to learn
    // it twice.
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }

    // Live: one control transfer, no stream interruption. Refused (with a
    // reason) outside 24 MHz - 1.75 GHz rather than clamped, because a tune
    // that silently lands somewhere else is worse than one that does not
    // happen.
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
    // Puts about 4.5 V on the antenna port for a powered LNA at the mast.
    // Deliberately NOT in antennas(): it is not a choice of where to listen,
    // it is power on a connector, and a control that can damage a receiver
    // connected to the wrong thing should never be reachable by something
    // iterating a list of port names.
    //
    // It is a GPIO write, not the request whose name says bias - see
    // airspy_protocol.hpp's kBiasTPortPin for the reference lines.
    bool setBiasT(bool on);
    bool biasT() const { return biasT_.load(std::memory_order_relaxed); }

    // --- what open() read off the device ---------------------------------
    // Empty / zero before a successful open. Kept because "which board is this
    // and what firmware is on it" is the first question any Airspy problem
    // report needs answered, and asking the radio later means a control
    // transfer in the middle of a stream.
    std::uint8_t boardId() const;
    std::string firmwareVersion() const;
    std::string partIdSerialNo() const;
    // "Airspy R2", "Airspy Mini" or plain "Airspy" - see airspyModelFrom.
    std::string model() const;
    // True once open() has turned 12-bit packing on.
    bool packingEnabled() const { return packing_.load(std::memory_order_relaxed); }

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

    // Tests only: does the reader's link still point at a device? The same
    // accessor, for the same reason, as HackRfSource::linkHoldsDeviceForTest:
    // after an abandonment the stranded reader dereferences link_->dev, so
    // closeDevice() must leave it alone, and the zombie never calls back into
    // the fake once `run` is false - only the driver can be asked. Read-only,
    // taken under the device mutex, and called from nowhere in the product.
    bool linkHoldsDeviceForTest() const;

private:
    // Everything the reader thread touches, in one object behind a shared_ptr
    // it captures BY VALUE - see the file header. An abandoned reader outlives
    // the AirspySource that started it, and a thread reading freed members
    // would be a worse defect than the hang the bound exists to prevent.
    struct ReaderLink {
        explicit ReaderLink(std::size_t ringCapacity) : ring(ringCapacity) {}

        std::atomic<bool> run{false};

        // Held by stopStreamingLocked while it lowers `run`, and by the
        // reader across the "is run still up? then RECEIVER_MODE RX and queue
        // the ring" step of a re-arm. Without it a re-arm racing a stop could
        // put the receiver back into RX just after stop() had switched it
        // off, and leave a radio streaming into a host that has gone.
        std::mutex rearmMutex;

        // NOT owned here. The AirspySource owns the handle, except on the
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

    // The ring holds several whole transfers plus headroom, rounded to the
    // power of two SpscRing requires. One packed transfer is 49152 complex
    // samples, so this is ten of them - 52 ms at 10 MS/s, against a pipeline
    // that asks for 10 ms chunks.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 19; }

    // The *Locked helpers assume devMutex_ is held. Public entry points take
    // it; privates never do, so open() can run a whole opening sequence under
    // one lock without recursing into it.
    bool controlOutLocked(airspy::VendorRequest r, std::uint16_t value, std::uint16_t index,
                          const std::uint8_t* data, std::size_t len, const char* what);
    // `moved`, when given, receives the byte count the device actually
    // answered with. VERSION_STRING_READ is as long as the firmware's string
    // and no longer, so a caller that turns the answer into text needs the
    // count rather than trusting a terminator the device never wrote.
    bool controlInLocked(airspy::VendorRequest r, std::uint16_t value, std::uint16_t index,
                         std::uint8_t* data, std::size_t len, const char* what,
                         std::size_t* moved = nullptr);

    bool setReceiverModeLocked(airspy::ReceiverMode mode, const char* what);
    bool readSampleRatesLocked();
    bool programRateIndexLocked(std::size_t index, const char* what);
    bool programFrequencyLocked(double hz, const char* what);
    bool programPackingLocked(bool on);
    bool programLnaLocked(int index);
    bool programMixerLocked(int index);
    bool programVgaLocked(int index);
    bool programLnaAgcLocked(bool on);
    bool programMixerAgcLocked(bool on);
    bool programCombinedLocked(int index, bool linearity);
    bool programBiasTLocked(bool on);

    // Receiver off, clear the halt, RX, queue the bulk ring, spawn the reader
    // - libairspy's order (see start()). Assumes devMutex_ held and the
    // device open and not already running.
    bool startStreamingLocked();
    // Lower the reader's run flag, RX off, join the reader (bounded), tear the
    // bulk ring down. Idempotent; assumes devMutex_ held.
    void stopStreamingLocked();

    // The reader's recovery from a failed bulk read: the start sequence again,
    // on the reader thread and WITHOUT devMutex_ (the GUI thread may hold it
    // while it waits for this very thread to stop). Done - the ring is queued
    // again; Stopped - stop() got there first and nothing was restarted;
    // Failed - a step failed and `why` says which.
    enum class Rearm { Done, Stopped, Failed };
    static Rearm rearmStreamOn(ReaderLink& link, std::string& why);

    // THE READER'S OWN HELPERS ARE STATIC AND TAKE THE LINK, not `this`. An
    // abandoned reader outlives the AirspySource; a member function reaching
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

    // Lock-free mirrors, so per-frame GUI readouts never wait behind a control
    // transfer in flight.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<int> lnaIndex_{0};
    std::atomic<int> mixerIndex_{0};
    std::atomic<int> vgaIndex_{0};
    std::atomic<int> linearityIndex_{-1};    // -1: never set through the table
    std::atomic<int> sensitivityIndex_{-1};
    std::atomic<bool> autoGain_{false};
    std::atomic<bool> biasT_{false};
    std::atomic<bool> packing_{false};

    mutable std::mutex nameMutex_;
    std::string name_ = "Airspy: (no device)";

    // Identity and the rate list, read once at open under devMutex_.
    std::uint8_t boardId_ = 0;
    std::string firmwareVersion_;
    std::string partIdSerialNo_;
    std::string model_;
    std::vector<double> rates_;      // ascending, as the panel wants them
    std::vector<std::size_t> rateIndex_;  // rates_[i] is the firmware's rateIndex_[i]

    // The test seam (see setTransportForTest). Empty opener means the real
    // WinUSB transport.
    std::vector<cascade::usb::UsbDeviceInfo> fakeDevices_;
    UsbOpenFn fakeOpener_;
    bool useFakeTransport_ = false;
};

}  // namespace cascade::source
