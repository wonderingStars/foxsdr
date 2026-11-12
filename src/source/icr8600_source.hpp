// icr8600_source.hpp - the Icom IC-R8600's USB [I/Q OUT] port as a source, over the WinUSB transport in
// src/usb (no libusb, as the brief says). Protocol, sources and licences: icr8600_protocol.hpp.
//
// WHAT THE USER MUST SUPPLY. The port is a firmware-less Cypress FX2 (0C26:0022) until the maker's firmware is
// loaded into its RAM; it then streams as 0C26:0023. FoxSDR ships and fetches NO Icom firmware. open() accepts
//   firmware=<path>   the file Icom's installer leaves (IC-R8600_usb_iq.spt) or the JSON sequence made from it;
//                     used ONLY when the port is found still waiting at 0C26:0022 (and WinUSB is bound to it);
// or the user installs Icom's own "USB I/Q Package for HDSDR", whose driver loads the firmware so that the port
// already shows as 0C26:0023 (and WinUSB is bound to that). Either way no hub between the radio and the PC
// (Icom's advice per the ibelinp README).
//
// Receive only; off until the user picks it; no Windows dialog is raised by anything here; an absent or
// unprepared radio is reported plainly by open(). One reader thread (NetSampleSource's). The I/Q data is read
// from bulk 0x86 as a WinUSB ring; CI-V commands go out on 0x02 and their answers come back on 0x88 through
// UsbDevice::readBulkOnce (added to src/usb for this driver).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/icr8600_protocol.hpp"
#include "source/net_sample_source.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source {

// The VID/PID list the production enumeration asks for: the streaming personality first, then the loader.
std::vector<cascade::usb::UsbId> icr8600UsbIds();

class Icr8600Source : public NetSampleSource {
public:
    Icr8600Source() = default;
    ~Icr8600Source() override;

    using EnumFn = std::function<std::vector<cascade::usb::UsbDeviceInfo>()>;
    using UsbOpenFn = std::function<std::unique_ptr<cascade::usb::UsbDevice>(const std::string& path, std::string& error)>;
    // TESTS ONLY, per instance: what enumeration returns (called again while waiting for the radio to come back
    // after the firmware load) and what opens a path. There is no IC-R8600 on the bench.
    void setTransportForTest(EnumFn enumerate, UsbOpenFn opener);

    const char* driverKey() const override { return "icr8600"; }
    // args: firmware=<path> (optional, see above), timeout=<ms> (CI-V answers, 100..10000, default 1500).
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;

    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double v) override;
    double gainDb(const std::string& name) const override;
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override;
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override;
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double&, double&) const override { return false; }

    // Counters for the health row.
    std::uint64_t syncMarkers() const;
    std::uint64_t realignments() const;  // times the I/Q phase had to be re-locked (data was lost)
    std::uint64_t refusedOptional() const;  // optional CI-V commands the radio answered NG to

private:
    struct Conn;
    bool failOpen(const std::string& why);
    void teardown();
    // One CI-V command and its answer. True on OK. `optional`: an NG answer is counted and tolerated.
    bool civ(const std::vector<std::uint8_t>& frame, const char* what, bool optional);
    bool loadFirmware(const std::string& path, const cascade::usb::UsbDeviceInfo& loader, std::string& error);
    std::vector<cascade::usb::UsbDeviceInfo> enumerateAll() const;

    EnumFn enumFn_;
    UsbOpenFn openFn_;
    mutable std::mutex ctlMutex_;
    mutable std::mutex connMutex_;
    std::mutex civMutex_;
    std::shared_ptr<Conn> conn_;
    int ackMs_ = 1500;
    bool active_ = false;
    std::uint8_t rateCode_ = 0x03;  // 1.92 MS/s: a rate the USB 2.0 link carries comfortably
    std::uint64_t freqHz_ = 100000000;
    unsigned rfGain_ = 255;  // MAX: the README's rule - below it the stream may be all zeros
    unsigned attenuatorDb_ = 0;
    bool preamp_ = true;
    unsigned antennaIndex_ = 0;
    std::atomic<std::uint64_t> refusedOptional_{0};
};

}  // namespace cascade::source
