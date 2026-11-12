// fcd_control.hpp - the FUNcube Dongle Pro / Pro+ tuner control over HID, and FuncubeSource, the source that
// pairs it with the dongle's sound-device I/Q. Protocol and licence: fcd_protocol.hpp.
//
// HidLink is the seam: the production link (fcd_hid_win.cpp) uses the Windows HID API (hid.dll / SetupAPI, both
// part of Windows - no library is added, nothing downloaded, no driver installed); tests use a stand-in dongle.
// Receive only. Off until the user picks it; no dialog; an absent dongle is reported plainly by open().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/device_source.hpp"
#include "source/fcd_protocol.hpp"
#include "source/soundcard_source.hpp"

namespace cascade::source {

// A HID device by its reports. write() takes the 64 bytes AFTER the report id (fcd::report()); the transport
// prepends the id (0). read() returns the bytes of one input report without its id: >0 bytes, 0 none within the
// wait (0 ms = poll), <0 failed.
class HidLink {
public:
    virtual ~HidLink() = default;
    virtual bool write(const std::uint8_t* report64, std::string& error) = 0;
    virtual int read(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) = 0;
    virtual void close() = 0;
};

struct FcdDeviceInfo {
    std::string path;     // HID device path (the transport's handle; never shown, never logged)
    bool isProPlus = true;
};

// Every FUNcube Dongle Pro / Pro+ HID interface Windows lists (matched by the vendor and product id in the
// device path; no other HID device is opened). Empty off Windows.
std::vector<FcdDeviceInfo> enumerateFcdHid();
std::unique_ptr<HidLink> openFcdHid(const std::string& path, std::string& error);

class FcdControl {
public:
    FcdControl(std::unique_ptr<HidLink> link, bool isProPlus) : link_(std::move(link)), plus_(isProPlus) {}
    ~FcdControl();
    bool isProPlus() const { return plus_; }

    // The device's identity string (printable ASCII, at most 40 characters).
    bool query(std::string& text, std::string& error);
    // Tunes (whole Hz, after the ppm trim). Refuses frequencies outside the two bands.
    bool setFrequencyHz(double hz, double ppm, std::string& error);
    bool frequencyHz(std::uint32_t& hz, std::string& error);
    // Gains by name: "LNA", "Mixer", "IF" (dB). Pro+: LNA 0 or 10, mixer 0 or 20, IF 0..59 whole dB. Pro: LNA to the
    // nearest valid register, mixer 4 or 12, IF 3..57 whole dB. The value the device reports back is in `readBack`.
    bool setGainDb(const std::string& name, double db, std::string& error);
    bool readGainDb(const std::string& name, double& db, std::string& error);
    bool setBiasTee(bool on, std::string& error);  // Pro+ only
    bool biasTee(bool& on, std::string& error);

private:
    bool exchange(std::uint8_t command, const std::vector<std::uint8_t>& params, bool needReply, std::uint8_t* replyOut,
                  std::string& error);
    std::unique_ptr<HidLink> link_;
    bool plus_;
    std::mutex mu_;
};

class FuncubeSource final : public DeviceSource {
public:
    using HidEnumFn = std::function<std::vector<FcdDeviceInfo>()>;
    using HidOpenFn = std::function<std::unique_ptr<HidLink>(const std::string& path, std::string& error)>;

    explicit FuncubeSource(SoundCardSource::BackendFactory audio = {});
    ~FuncubeSource() override;
    // TESTS ONLY: what enumerates and opens the HID side.
    void setHidForTest(HidEnumFn enumerate, HidOpenFn opener);

    const char* driverKey() const override { return "funcube"; }
    // args: index=<n> (which dongle, default 0), audio=<sound input name> (default: the one input whose name contains
    // "funcube"), api=<host API name>, swapiq=0|1, ppm=<oscillator trim>.
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool isOpen() const override { return open_; }

    bool start() override;
    void stop() override;
    bool running() const override;
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;
    bool hasFrequencyCorrection() const override { return true; }
    bool setFrequencyCorrectionPpm(double ppm) override;
    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override;
    const char* name() const override;
    const char* lastError() const override;

    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override;
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override;
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double& lo, double& hi) const override;
    bool deviceDead() const override { return faulted(); }
    std::string faultedWhile() const override;

    bool isProPlus() const { return plus_; }

private:
    void setError(const std::string& s);
    SoundCardSource audio_;
    SoundCardSource::BackendFactory factory_;
    HidEnumFn enumFn_;
    HidOpenFn openFn_;
    std::unique_ptr<FcdControl> ctl_;
    mutable std::mutex mu_;
    bool open_ = false, plus_ = true, bias_ = false;
    double freqHz_ = 100000000.0, ppm_ = 0.0;
    double lna_ = 0.0, mixer_ = 0.0, if_ = 0.0;
    std::string err_, name_ = "FUNcube Dongle";
};

}  // namespace cascade::source
