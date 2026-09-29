// aor_source.hpp - FoxSDR's own driver for AOR digital-I/Q receivers: the
// AR5700D, and the AR2300, AR5001D and AR6000 fitted with the IQ5001.
//
// WRITTEN FROM AOR'S DOCUMENTATION AND NOTHING ELSE. The one source is AOR's
// "Digital I/Q USB Interface Developer Information", Revision 1.1, September
// 2026 - thanks to AOR, Ltd. for writing it down. It is a CLEAN-ROOM driver:
// SoapyAOR and AOR-GQRX-RPi (GPL-3) were not read, fetched or copied, and
// AOR's Windows driver AorAlpha.sys is not used - its IOCTL contract is
// unpublished, and this driver goes through WinUSB (Windows) or usbfs (Linux)
// like every other native radio here.
//
// NOBODY ON THE PROJECT HAS AN AOR RECEIVER. Every behaviour in this file is
// tested against a fake transport and a fake serial port
// (tests/test_aor_source.cpp) and NOT on hardware. AOR's document itself says
// only the AR5700D has been validated; the other models are marked unverified
// where they are told apart (aor_control.hpp's identifyVr).
//
// TWO USB CONNECTIONS, ONE RECEIVER:
//   I/Q      08D0:A001, an FX2 with volatile firmware: iso IN 0x86 carries
//            1.125 MS/s of packed 8-byte words, bulk OUT 0x02 takes START and
//            STOP (aor_protocol.hpp). Opened through src/usb.
//   control  an FTDI serial port (0403:6001) at 115200 8N1: EX, VR, @21, VFA
//            and RF to tune (aor_control.hpp). Found by asking VR, never by
//            VID/PID alone.
//
// THE THREAD is the same shape and the same obligation as every native
// driver here (airspyhf_source.hpp states it in full): one reader pulls
// completed iso transfers, aligns and decodes them into a ring; read()
// drains the ring; stopping is STOP, signal, BOUNDED join, then the iso ring
// torn down; a reader that will not come back is abandoned with its device
// deliberately leaked rather than destroyed under it.
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
#include "source/aor_control.hpp"
#include "source/aor_protocol.hpp"
#include "source/device_source.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source {

// --- enumeration ------------------------------------------------------------

// The I/Q interface's VID/PID - the same before and after its firmware runs.
std::vector<cascade::usb::UsbId> aorUsbIds();

// Every reachable AOR I/Q interface, as the Source section wants them. NEVER
// OPENS A DEVICE (usb_device.hpp rule 1) and never touches a serial port:
// pairing the control port happens at open.
std::vector<NativeDeviceInfo> enumerateAor();

// The pure half of the above.
std::vector<NativeDeviceInfo> aorDevicesFrom(const std::vector<cascade::usb::UsbDeviceInfo>& devices);

// Where FoxSDR looks for AOR's FX2 firmware: resources/firmware/aor/fx2fw.hex
// beside the executable. AOR's copyright; not in this repository until AOR
// supplies it with its redistribution notice (resources/firmware/aor/NOTICE).
std::string aorFirmwarePath();

// The Source section's sentence for an AOR I/Q interface that is present but
// not bound to WinUSB (typically because AOR's own AorAlpha.sys has it), in
// the language in force. `what` names the device.
std::string aorUnboundAdvice(const std::string& what);

// THE OPEN QUESTION, ISOLATED.
//
// TODO(AOR question 1): AOR's document addresses the FX2 as 08D0:A001 both
// BEFORE and AFTER the firmware is loaded, and does not say how a host tells
// an unprogrammed interface from a running one. This driver does not guess a
// rule (a descriptor string, a device release number, an endpoint's
// presence). It asks the device the one question whose answer cannot be
// wrong - does it stream? - and loads the firmware only when it does not:
//
//   Stream        START was accepted, the iso transfers were armed, and the
//                 aligner found strong alignment (four consecutive
//                 marker-valid words) within kProbeWindow. The firmware is
//                 running; use it.
//   LoadFirmware  anything else, the first time: the endpoint was not there,
//                 START was refused, or nothing aligned. Load fx2fw.hex, wait
//                 for the device to come back, and ask again.
//   GiveUp        anything else after the firmware has already been loaded
//                 in this open: say so in words; never load twice.
//
// When AOR answers, this function is the one place that changes.
enum class FirmwareStep { Stream, LoadFirmware, GiveUp };

struct StreamProbe {
    bool isoArmed = false;      // beginIsoStream() succeeded on 0x86
    bool startAccepted = false; // the START write went out whole
    bool aligned = false;       // the aligner locked within the probe window
    bool firmwareLoadedThisOpen = false;
};

FirmwareStep firmwareDecision(const StreamProbe& probe);

// --- the driver ---------------------------------------------------------------

class AorSource : public DeviceSource {
public:
    AorSource() = default;
    ~AorSource() override;

    AorSource(const AorSource&) = delete;
    AorSource& operator=(const AorSource&) = delete;

    // --- the bounded waits ------------------------------------------------
    // Named, and registered in tests/test_shutdown_budget.cpp, like every
    // other driver's.

    // How long readIso blocks for a completed transfer. A 128-packet
    // transfer is 16 ms of microframes, so this expires only when the
    // interface has stopped delivering - and it bounds how long the reader
    // takes to notice it has been asked to stop.
    static constexpr std::chrono::milliseconds kIsoReadWait{100};

    // How long read() waits for the reader before answering "nothing yet".
    static constexpr std::chrono::milliseconds kReadWait{20};

    // The bound on joining the reader thread.
    static constexpr std::chrono::milliseconds kReaderJoinWait{1000};

    // The bound on one START/STOP bulk write.
    static constexpr std::chrono::milliseconds kCommandWriteWait{500};

    // The stream-health window. Not a wait.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};

    // How long the open-time probe listens for aligned samples after START
    // (see firmwareDecision). At 9 MB/s a running interface aligns within
    // the first transfer or two; this is generous so a slow start is not
    // mistaken for missing firmware.
    static constexpr std::chrono::milliseconds kProbeWindow{750};

    // After the firmware is released: at least the AOR document's own
    // "sleep 1" between load and use, and at most this long for the device
    // to come back, looked for this often.
    static constexpr std::chrono::milliseconds kFirmwareSettle{1000};
    static constexpr std::chrono::milliseconds kFirmwareReenumerateBudget{5000};
    static constexpr std::chrono::milliseconds kFirmwarePollInterval{100};

    // --- the test seam ----------------------------------------------------
    //
    // There is no AOR receiver on the bench: the fakes that implement
    // UsbDevice and ControlLink ARE the proof, so the seam that admits them
    // is part of the design.
    struct TestTransport {
        std::function<std::vector<cascade::usb::UsbDeviceInfo>()> list;     // WinUSB-reachable
        std::function<std::vector<cascade::usb::UsbDeviceInfo>()> unbound;  // present, not WinUSB
        std::function<std::unique_ptr<cascade::usb::UsbDevice>(const std::string& path,
                                                               std::string& error)>
            open;
        std::function<std::vector<std::string>()> controlPorts;
        aor::LinkOpener openControl;
        std::string firmwarePath;
        std::function<void(std::chrono::milliseconds)> sleep;  // null: really sleep
    };
    void setTransportForTest(TestTransport t);

    // --- DeviceSource -----------------------------------------------------

    const char* driverKey() const override { return "aor"; }

    // Args: "serial=<text>" or "index=N" pick the I/Q interface; an optional
    // "control=<port>" names the control serial port instead of pairing by VR.
    //
    // A successful open has: found the I/Q interface (or said in one sentence
    // that it is not bound to WinUSB), paired the control port by VR, probed
    // the interface and - only if it did not stream - loaded AOR's firmware
    // and waited for it, run the documented control start-up, and tuned the
    // default frequency. A missing firmware file is said in plain words and
    // the device is not opened.
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // No gain control is documented for this interface: none is offered.
    std::vector<GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return false; }

    // Antenna selection is not documented for this interface: one path.
    std::vector<std::string> antennas() const override { return {"RX"}; }
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override { return "RX"; }

    // Fixed at 1.125 MS/s: the only rate offered, and any request coerces to it.
    std::vector<double> supportedSampleRatesHz() const override { return {aor::kSampleRateHz}; }

    // The AOR document gives no tuning range, so none is claimed: false.
    // setCenterFrequencyHz refuses only what the RF command cannot carry.
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource ---------------------------------------------------------

    // Arms the iso transfers, THEN sends START, then starts the reader - the
    // order this driver keeps (see the comment in the .cpp for how it relates
    // to the AOR document's reference order).
    bool start() override;
    // STOP first, then the reader joined (bounded), then the iso transfers
    // cancelled.
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

    // --- what open() found ------------------------------------------------
    aor::Identity identity() const;
    std::string controlPort() const;
    bool firmwareWasLoaded() const;

    // --- stream health ----------------------------------------------------
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
    std::string streamHealthLine();
    void setStreamHealthWindowForTest(std::chrono::milliseconds w);

    // Totals since start(), for the log and the tests.
    std::uint64_t transfersCompleted() const { return link_->transfers.load(); }
    std::uint64_t transfersDiscarded() const { return link_->discarded.load(); }
    std::uint64_t alignmentLosses() const { return link_->losses.load(); }
    std::uint64_t droppedTransfers() const { return link_->dropped.load(); }
    static unsigned long long readersAbandoned();

private:
    struct ReaderLink {
        explicit ReaderLink(std::size_t cap) : ring(cap) {}
        std::atomic<bool> run{false};
        cascade::usb::UsbDevice* dev = nullptr;  // NOT owned; see the file header
        cascade::dsp::SpscRing<std::complex<float>> ring;
        aor::Aligner aligner;  // the reader's alone while it runs

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

        std::atomic<std::uint64_t> transfers{0};
        std::atomic<std::uint64_t> discarded{0};
        std::atomic<std::uint64_t> losses{0};
        std::atomic<std::uint64_t> dropped{0};
    };

    // 512 Ki complex samples: 466 ms at 1.125 MS/s.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 19; }
    const std::shared_ptr<ReaderLink> link_ = std::make_shared<ReaderLink>(kRingCapacitySamples());

    bool resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                       std::string& error);
    std::vector<cascade::usb::UsbDeviceInfo> listDevices();
    std::unique_ptr<cascade::usb::UsbDevice> openUsb(const std::string& path, std::string& error);
    StreamProbe probeStreamLocked();
    bool loadFirmwareLocked(cascade::usb::UsbDeviceInfo& info, std::string& error);
    bool writeCommandLocked(const std::uint8_t* cmd, const char* what);
    bool startStreamingLocked();
    void stopStreamingLocked();

    static void readerThreadBody(std::shared_ptr<ReaderLink> link);
    static void noteRead(ReaderLink& link, bool completed, std::size_t samples, bool dropped,
                         bool failed);
    static std::string healthLineLocked(ReaderLink& link);
    static void setErrorOn(ReaderLink& link, std::string msg);
    static void noteTransportFaultOn(ReaderLink& link, const char* what, const std::string& detail);
    void setError(std::string msg);
    void clearError();
    void noteTransportFault(const char* what, const std::string& detail);
    void setName(std::string n);

    mutable std::mutex devMutex_;
    std::unique_ptr<cascade::usb::UsbDevice> dev_;
    std::unique_ptr<aor::ControlLink> control_;
    std::thread reader_;

    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};

    aor::Identity identity_;
    std::string controlPort_;
    bool firmwareLoaded_ = false;

    mutable std::mutex nameMutex_;
    std::string name_ = "AOR: (no device)";

    TestTransport fake_;
    bool useFake_ = false;
};

}  // namespace cascade::source
