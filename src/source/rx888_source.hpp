// rx888_source.hpp - FoxSDR's own RX888 mk2 driver: a DeviceSource that
// speaks the SDDC FX3 protocol in src/source/rx888_protocol.hpp over the
// WinUSB transport in src/usb/usb_device.hpp, with no CyAPI, no libusb and no
// SoapySDR module anywhere in the path.
//
// WHY A NATIVE DRIVER AT ALL is argued in usb_device.hpp. The RX888 adds two
// reasons of its own:
//
//  1. THE REFERENCE HOST DRIVER IS NOT A DRIVER WE COULD USE. ExtIO_sddc
//     reaches this hardware through Cypress's CyAPI/CyUSB3 kernel driver -
//     a closed, unsigned-for-modern-Windows stack the user has to install
//     from a vendor SDK. FoxSDR already asks its users for exactly one
//     driver install, Zadig's WinUSB, for every other native radio. Asking
//     for a second, different one for this radio would be the worst of both.
//  2. AN RX888 WITH NO FIRMWARE IS INVISIBLE, and that is the state every one
//     of them is in after a power cycle. There is no flash on the board: the
//     FX3 comes up as a Cypress bootloader with no bulk endpoint at all, and
//     a host that does not carry the image cannot make it into a receiver.
//     So this driver carries it (src/source/rx888_firmware.cpp) and LISTS a
//     bootloader device, saying what it is, instead of leaving a plugged-in
//     radio out of the list with no explanation - which is precisely the
//     failure the unbound-device listing in usb_device.hpp exists to stop.
//
// THE TWO IDENTITIES, WHICH IS THE THING TO KNOW BEFORE READING open(). A
// bootloader is 04B4:00F3 and a running SDDC device is 04B4:00F1; they are
// the same physical radio a second apart, and BOTH have to be bound to
// WinUSB for any of this to work. open() on a bootloader uploads the image,
// waits for the device to come back under the other identity (bounded, see
// kFirmwareReenumerateBudget) and opens that.
//
// THE THREAD, AND WHY IT OUTLIVES THIS OBJECT - the same argument, word for
// word, as hackrf_source.hpp's, because it is the same transport and the
// same obligation. One reader thread pulls completed bulk transfers,
// converts them and writes them into a ring; read() drains the ring on the
// pipeline's source thread. Stopping it is signal, join, then
// endBulkStream(), in that order, and the join is BOUNDED
// (kReaderJoinWait): a reader that has not come back by then is abandoned,
// because a hang on the GUI thread is worse than a leak. Everything the
// reader touches lives in a ReaderLink behind a shared_ptr it captures BY
// VALUE, and the abandoning path deliberately leaks the UsbDevice rather
// than destroying it under a thread that is still inside it.
//
// WHAT THE READER DOES THAT THE OTHER DRIVERS' DO NOT. This radio's samples
// are 16-bit REAL at 64 MS/s - 128 MB/s of them - and the pipeline wants
// complex baseband. The conversion (rx888::RealToIq) runs on the reader
// thread, between readBulk and the ring, and it is by far the most
// expensive thing any FoxSDR driver does per sample. It is guarded by its
// own small mutex rather than devMutex_, so a retune costs one NCO frequency
// change and does not wait for a USB transfer, and a USB transfer does not
// wait for a retune.
//
// WHAT IS NOT HERE. The ADC clock is fixed at 64 MHz: the firmware will
// drive the Si5351 from 50 to 140 MHz, but every published mk2 figure and
// every rate in the table below is derived from 64, and a clock the user can
// move is a clock that silently changes what every one of those rates means.
// The LEDs, the preselector (an mk3/RX999 control), raw I2C and the
// firmware's debug console are all absent - a request this driver cannot
// name is a request it cannot send by mistake.
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
#include "source/rx888_protocol.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source {

// --- enumeration ----------------------------------------------------------

// Every SDDC device bound to WinUSB - running firmware or still in the
// bootloader - as the Source section wants them. NEVER OPENS A DEVICE
// (usb_device.hpp rule 1).
//
// On a non-Windows build this is empty and says so in the log: the transport
// is WinUSB, and a Linux RX888 is reached through SoapySDR until a libusb
// backend exists behind the same interface.
std::vector<NativeDeviceInfo> enumerateRx888();

// The pure half of the above: which of a list of USB devices are SDDC
// devices, what each one's label and args string is, and which of them are
// bootloaders that will need the firmware on open. Separated out because it
// is the only part that can be proven without a radio on the bench.
std::vector<NativeDeviceInfo> rx888DevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices);

// The VID/PID list enumerateRx888() asks the transport for: both identities.
std::vector<cascade::usb::UsbId> rx888UsbIds();

// --- the driver -----------------------------------------------------------

class Rx888Source : public DeviceSource {
public:
    Rx888Source() = default;
    ~Rx888Source() override;

    // Owns a device handle and a thread; copying either would be a
    // double-close or a double-join.
    Rx888Source(const Rx888Source&) = delete;
    Rx888Source& operator=(const Rx888Source&) = delete;

    // --- the bounded waits ------------------------------------------------
    //
    // Every one of these is named rather than written at its call site
    // because tests/test_shutdown_budget.cpp discovers `constexpr
    // std::chrono` constants under src/ and refuses to go green until each is
    // classified in its kKnownWaits table. They are CLASS MEMBERS rather than
    // namespace constants for the reason airspy_source.hpp gives: four of
    // these names already exist at cascade::source scope in
    // hackrf_source.hpp, and gui/app_window.hpp includes both headers.

    // How long readBulk blocks for a completed transfer before answering
    // "nothing yet". At 64 MS/s a 131072-byte transfer is 1.02 ms of signal,
    // so this expires only when the radio has genuinely stopped delivering -
    // and it is what bounds how long the reader thread takes to notice it has
    // been asked to stop.
    static constexpr std::chrono::milliseconds kBulkReadWait{100};

    // How long read() waits for the reader to put something in the ring
    // before returning the IqSource contract's "nothing yet, retry".
    static constexpr std::chrono::milliseconds kReadWait{20};

    // The bound on joining the reader thread (see the file header).
    // Comfortably more than kBulkReadWait plus the conversion of one
    // transfer, because the ordinary exit costs at most one of each.
    static constexpr std::chrono::milliseconds kReaderJoinWait{1000};

    // The stream-health window, matching SoapySource::kStreamHealthWindow.
    // Not a wait: nothing sleeps or blocks on it.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};

    // HOW LONG open() WILL WAIT FOR A RADIO TO COME BACK AFTER A FIRMWARE
    // UPLOAD, and how often it looks. The FX3 drops off the bus at the jump
    // and re-enumerates under its other identity; Windows then has to bind
    // WinUSB to that identity and publish the interface. The reference sleeps
    // a flat 800 ms (Core/arch/win32/FX3handler.cpp:75) or 500 ms
    // (Core/arch/linux/usb_device.c:266) and hopes; this polls, because a
    // fixed sleep is either too short on a busy machine or wasted on a fast
    // one, and reports what it saw when the budget runs out.
    //
    // Neither is on the teardown path: firmware is uploaded at open.
    static constexpr std::chrono::milliseconds kFirmwareReenumerateBudget{5000};
    static constexpr std::chrono::milliseconds kFirmwarePollInterval{100};

    // TESTS ONLY, and per instance rather than per process so two tests can
    // never see each other's transport. `devices` is what this object's
    // open() will enumerate instead of asking WinUSB, and `opener` is what it
    // will call instead of openWinUsb(). There is no RX888 on the bench this
    // driver was written on: the fake that implements cascade::usb::UsbDevice
    // and answers byte-for-byte as the firmware does IS the proof, so the
    // seam that admits it is part of the design and not a back door.
    //
    // The device LIST is taken by a callable rather than by value, unlike the
    // HackRF's, because this driver enumerates TWICE in one open(): once to
    // find the radio and once, repeatedly, to watch it come back under its
    // other identity after the firmware upload. A fixed list could not
    // express the one behaviour the firmware path exists to have.
    using UsbOpenFn =
        std::function<std::unique_ptr<cascade::usb::UsbDevice>(const std::string& path,
                                                               std::string& error)>;
    using UsbListFn = std::function<std::vector<cascade::usb::UsbDeviceInfo>()>;
    void setTransportForTest(UsbListFn lister, UsbOpenFn opener);

    // --- DeviceSource ----------------------------------------------------

    const char* driverKey() const override { return "rx888"; }

    // Takes a NativeDeviceInfo::args string: "serial=<text>" picks a device by
    // the serial SetupAPI reported (case-insensitive, suffix match), "index=N"
    // picks the Nth in enumeration order. An empty string takes the first.
    //
    // A successful open has done up to six things: found the device, uploaded
    // the firmware and waited for it to come back if it was a bootloader,
    // read the model and firmware version off it with TESTFX3, refused
    // anything that is not an RX888 mk2, started the ADC clock, and put the
    // radio into a KNOWN STATE - HF mode, 8 MS/s, 10 MHz, no attenuation,
    // IF gain at 0 dB, dither and randomiser off, both bias tees OFF.
    // That last one is not cosmetic: an SDDC device remembers its GPIO word
    // across a re-open, bias tee included, and a driver that inherits 5 V on
    // somebody's antenna port silently cannot report what the radio is doing.
    bool open(const std::string& args) override;

    // Stops the stream, puts the front end to sleep, releases the device.
    // Idempotent, safe on a never-opened instance, and safe to call from the
    // destructor. lastError() survives it.
    void closeDevice() override;

    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // FOUR GAINS, OF WHICH TWO ARE LIVE AT A TIME, because this radio has two
    // entirely separate front ends and which one is in circuit is decided by
    // where it is tuned (rx888::kHfCeilingHz). They are all four always
    // listed, with fixed names, rather than the list changing under a panel
    // that is iterating it:
    //
    //   "HF ATT"  the DAT-31 step attenuator, -31.5 to 0 dB in 0.5 dB steps
    //   "HF IF"   the AD8340 IF amplifier, -24.6 to +33.1 dB, uneven steps
    //   "VHF RF"  the R828D's LNA/mixer chain, 0 to 49.6 dB, uneven steps
    //   "VHF IF"  the R828D's VGA, -4.7 to +40.8 dB, uneven steps
    //
    // A gain belonging to the inactive front end is REMEMBERED and programmed
    // when that front end comes into circuit, not sent to a chip that is
    // powered down. Out-of-range values are clamped and uneven scales are
    // snapped to the nearest step the hardware has, with gainDb() reporting
    // what was actually programmed - the DeviceSource contract.
    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;

    // There is no AGC anywhere in this radio: not in the LTC2208, not in the
    // AD8340, and the R828D's is not reachable through the SDDC firmware's
    // argument list. So there is nothing to expose and setAutoGain refuses.
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return false; }

    // THE ANTENNA IS THE FRONT END, and it follows the frequency rather than
    // the other way round: below the ADC's Nyquist the HF port is sampled
    // directly, above it the VHF port goes through the tuner
    // (RX888R2Radio.cpp:53-62). Both are listed so a panel can show which one
    // is live; setAntenna accepts the one the current frequency implies and
    // refuses the other WITH THE REASON, rather than silently retuning the
    // radio somewhere the user did not ask for.
    std::vector<std::string> antennas() const override { return {"HF", "VHF"}; }
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override;

    // adcRate/2 down to adcRate/32: 32, 16, 8, 4 and 2 MS/s at the 64 MHz
    // clock this driver uses. Unlike the HackRF's, this IS a constraint -
    // every one of them is a power-of-two decimation of the ADC clock and
    // there is nothing in between - so setSampleRateHz snaps to it.
    std::vector<double> supportedSampleRatesHz() const override;

    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource --------------------------------------------------------

    // Queues the bulk ring, sends STARTFX3 and starts the reader thread, in
    // that order - a GPIF producer told to run with nothing queued fills the
    // FX3's DMA buffers and overruns before the host's first read. Idempotent
    // while running; false with lastError() when there is no device.
    bool start() override;

    // STOPFX3, reader joined (bounded, see the file header), bulk ring torn
    // down. Idempotent, safe before open.
    void stop() override;

    bool running() const override { return running_.load(std::memory_order_relaxed); }

    // The radio paces this source: the pipeline must not clock it.
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // Snapped to the nearest supported rate, and in VHF mode capped at
    // rx888::kMaxVhfRateHz - a complex band wider than twice the tuner's
    // 4.57 MHz IF would reach below 0 Hz and fold its own mirror into itself.
    //
    // ON A RUNNING STREAM the change is made with the radio QUIET: STOPFX3,
    // reader stopped, bulk ring torn down, the conversion reconfigured, then
    // the ring, STARTFX3 and the reader again. running() reads true
    // throughout on the success path. That is the shape 0.89.0 had to give
    // the Soapy path after a live rate change killed the process on the
    // driver's own reader thread; there is no reason to learn it twice.
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }

    // Live within one front end: an HF retune is an NCO frequency and no USB
    // traffic at all, and a VHF retune is one TUNERTUNE. Crossing between
    // them switches the front end, which is several control transfers and
    // reprograms both of that side's gains.
    //
    // Refused (with a reason) outside 10 kHz - 1750 MHz, and refused in HF
    // when the requested band would not fit inside the ADC's 0 - 32 MHz -
    // a tune that silently lands somewhere else is worse than one that does
    // not happen.
    bool setCenterFrequencyHz(double hz) override;

    // Drains the reader's ring. Blocks at most kReadWait when it is empty and
    // then returns 0 - the self-paced contract's "nothing yet, retry".
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    bool faulted() const override;

    const char* name() const override;
    const char* lastError() const override;

    // --- the controls DeviceSource has no notion of ----------------------
    //
    // All four are GPIO bits in one word (rx888_protocol.hpp's Gpio), so each
    // one costs a read of our own mirror and a write of all 32 bits, exactly
    // as the reference does it (Core/RadioHardware.cpp:3-15).

    // The ADC's dither generator: a small pseudo-random signal added ahead of
    // the converter to break up its quantisation spurs at the cost of a
    // slightly higher noise floor (RadioHandler.cpp:301-309).
    bool setDither(bool on);
    bool dither() const { return dither_.load(std::memory_order_relaxed); }

    // The ADC's output randomiser: the LTC2208 inverts bits 15..1 of any
    // sample whose bit 0 is set, which spreads the energy that would
    // otherwise couple back into the analogue side as a tone. The host has to
    // undo it, and does (rx888::derandomise) - which is why this is one
    // switch and not two: turning it on at the chip without telling the
    // converter turns the whole band into noise.
    bool setRandomiser(bool on);
    bool randomiser() const { return randomiser_.load(std::memory_order_relaxed); }

    // The HF front end's PGA (RadioHandler.cpp:311-319).
    bool setPga(bool on);
    bool pga() const { return pga_.load(std::memory_order_relaxed); }

    // THE BIAS TEES, one per front end, each putting DC on its antenna
    // connector for a powered amplifier at the mast. Deliberately NOT in
    // antennas(): a control that can damage whatever is connected should
    // never be reachable by something iterating a list of port names.
    bool setBiasT(bool on);      // the HF port
    bool biasT() const { return biasHf_.load(std::memory_order_relaxed); }
    bool setVhfBiasT(bool on);   // the VHF port
    bool vhfBiasT() const { return biasVhf_.load(std::memory_order_relaxed); }

    // --- what open() read off the device ---------------------------------
    // Empty / zero before a successful open. Kept because "which board is
    // this and what firmware is on it" is the first question any RX888
    // problem report needs answered, and asking the radio later means a
    // control transfer in the middle of a 128 MB/s stream.
    rx888::Model model() const;
    std::uint16_t firmwareVersion() const;
    // True when this instance uploaded the firmware itself during open().
    bool firmwareWasUploaded() const;
    std::uint32_t adcRateHz() const { return adcRateHz_.load(std::memory_order_relaxed); }
    // "HF" or "VHF" - which front end the current tuning put in circuit.
    bool vhfMode() const { return vhf_.load(std::memory_order_relaxed); }

    // --- stream health ---------------------------------------------------
    //
    // Same tally and the same one-line format SoapySource writes: a line
    // always for the first window after a start, so a healthy radio leaves
    // one proving it, and after that only for a window with something to
    // report.
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
    // because a timing assertion cannot tell a drop from a slow run, and on
    // THIS radio it is the number that matters most: 128 MB/s in, and a
    // conversion in the middle of it.
    std::uint64_t droppedTransfers() const;

    // Reader threads this PROCESS has abandoned because they did not come
    // back within kReaderJoinWait. 0 on every healthy path; the delta is what
    // a test asserts, because elapsed time alone still passes when the bound
    // is deleted.
    static unsigned long long readersAbandoned();

private:
    // Everything the reader thread touches, in one object behind a shared_ptr
    // it captures BY VALUE - see the file header.
    struct ReaderLink {
        explicit ReaderLink(std::size_t ringCapacity) : ring(ringCapacity) {}

        std::atomic<bool> run{false};

        // NOT owned here. The Rx888Source owns the handle, except on the
        // abandoning path, which deliberately leaks it so a stranded reader
        // still has a live object to be inside.
        cascade::usb::UsbDevice* dev = nullptr;

        cascade::dsp::SpscRing<std::complex<float>> ring;

        // The real-to-IQ conversion, and the ONLY lock the reader takes.
        // Held for the length of one transfer's conversion by the reader and
        // for one NCO frequency write by whoever retunes.
        std::mutex convMutex;
        rx888::RealToIq conv;

        std::mutex waitMutex;
        std::condition_variable waitCv;
        bool exited = false;

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
    // this pointer.
    const std::shared_ptr<ReaderLink> link_ = std::make_shared<ReaderLink>(kRingCapacitySamples());

    // 1 Mi complex samples - 8 MB - which is 33 ms at the widest rate this
    // driver offers and 500 ms at the narrowest. Deeper than the HackRF's
    // 512 Ki because this radio delivers four times the bandwidth into it and
    // the reader has a real-to-IQ conversion to do between the wire and the
    // ring: a scheduling hiccup that costs a HackRF nothing costs this one
    // samples. Rounded up to the power of two SpscRing requires.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 20; }

    // The *Locked helpers assume devMutex_ is held. Public entry points take
    // it; privates never do, so open() can run a whole opening sequence under
    // one lock without recursing into it.
    bool controlOutLocked(rx888::Command c, std::uint16_t value, std::uint16_t index,
                          const std::uint8_t* data, std::size_t len, const char* what,
                          unsigned timeoutMs = rx888::kControlTimeoutMs);
    bool controlInLocked(rx888::Command c, std::uint16_t value, std::uint16_t index,
                         std::uint8_t* data, std::size_t len, const char* what,
                         std::size_t* moved = nullptr);

    // The three shapes every SDDC command takes (rx888_protocol.hpp's
    // encoding note): one zero byte, a little-endian uint32, a uint64.
    bool commandLocked(rx888::Command c, const char* what);
    bool commandU32Locked(rx888::Command c, std::uint32_t v, const char* what,
                          unsigned timeoutMs = rx888::kControlTimeoutMs);
    bool commandU64Locked(rx888::Command c, std::uint64_t v, const char* what);
    bool setArgLocked(rx888::Argument a, std::uint16_t value, const char* what);

    // The GPIO word is written whole; these keep the mirror and send it.
    bool gpioSetLocked(std::uint32_t mask, const char* what);
    bool gpioClearLocked(std::uint32_t mask, const char* what);
    bool gpioWriteLocked(std::uint32_t word, const char* what);

    bool programAdcClockLocked(std::uint32_t hz);
    // Switches the front end and programs that side's two gains. Idempotent
    // when already in the requested mode unless `force` (which open() uses,
    // because the radio's own state is unknown until we have set it).
    bool programModeLocked(bool vhf, bool force);
    bool programHfAttLocked(double db);
    bool programHfIfLocked(double db);
    bool programVhfRfLocked(double db);
    bool programVhfIfLocked(double db);
    bool programTunerLocked(double hz);

    // Reconfigures the conversion for a rate/mode change, under convMutex.
    void reconfigureConversion(double rateHz, double centerHz, bool vhf);
    void retuneConversion(double centerHz);

    // The firmware path. uploadFirmwareLocked writes the embedded image to a
    // bootloader and jumps to it; awaitStreamer polls the transport's list
    // until the device comes back, bounded by kFirmwareReenumerateBudget.
    bool uploadFirmwareLocked(cascade::usb::UsbDevice& boot);
    bool awaitStreamer(cascade::usb::UsbDeviceInfo& out, std::string& error);

    std::vector<cascade::usb::UsbDeviceInfo> listDevices();

    bool startStreamingLocked();
    void stopStreamingLocked();

    // THE READER'S OWN HELPERS ARE STATIC AND TAKE THE LINK, not `this`. An
    // abandoned reader outlives the Rx888Source; a member function reaching
    // for a member of a destroyed object is precisely the defect the link was
    // introduced to prevent.
    static void readerThreadBody(std::shared_ptr<ReaderLink> link);
    static void noteRead(ReaderLink& link, int ret, std::size_t samples, bool dropped);
    static std::string healthLineLocked(ReaderLink& link);  // link.healthMutex held

    static void setErrorOn(ReaderLink& link, std::string msg);
    static void noteTransportFaultOn(ReaderLink& link, const char* what,
                                     const std::string& detail);
    void setError(std::string msg);
    void clearError();
    void noteTransportFault(const char* what, const std::string& detail);

    void setName(std::string n);

    bool resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                       std::string& error);

    mutable std::mutex devMutex_;

    std::unique_ptr<cascade::usb::UsbDevice> dev_;
    std::thread reader_;

    // Lock-free mirrors, so per-frame GUI readouts never wait behind a
    // control transfer in flight.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<std::uint32_t> adcRateHz_{rx888::kDefaultAdcRateHz};
    std::atomic<bool> vhf_{false};
    std::atomic<double> hfAttDb_{0.0};
    std::atomic<double> hfIfDb_{0.0};
    std::atomic<double> vhfRfDb_{0.0};
    std::atomic<double> vhfIfDb_{0.0};
    std::atomic<bool> dither_{false};
    std::atomic<bool> randomiser_{false};
    std::atomic<bool> pga_{false};
    std::atomic<bool> biasHf_{false};
    std::atomic<bool> biasVhf_{false};

    // Our copy of the GPIO word, because the FX3 has no way to read it back.
    std::uint32_t gpio_ = 0;

    mutable std::mutex nameMutex_;
    std::string name_ = "RX888: (no device)";

    // Identity, read once at open under devMutex_.
    rx888::Model model_ = rx888::Model::None;
    std::uint16_t firmwareVersion_ = 0;
    bool firmwareUploaded_ = false;

    // The test seam (see setTransportForTest). An empty opener means the real
    // WinUSB transport.
    UsbListFn fakeLister_;
    UsbOpenFn fakeOpener_;
    bool useFakeTransport_ = false;
};

}  // namespace cascade::source
