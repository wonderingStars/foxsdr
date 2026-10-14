// engine_api_fake_radio.hpp - the radio the engine-API conformance ports scan
// and open in place of the reference engine's "Mock RTL-SDR": no USB walk, no
// SoapySDR probe and no driver, through the Engine's own test hooks
// (Engine::testHooks_) - so no test ever touches the desk's radios. Its
// figures are the mock's: six rates, one gain stage "TUNER" of 0..49.6 dB in
// 0.1 dB steps. A rate it does not offer is coerced to the nearest it does,
// as a real driver does.
//
// Include this from inside a test that defines cascade::gui::
// AppWindowTestAccess (the Engine's friend) AFTER including it, and call
// installFakeRadioHooks() from there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "source/device_source.hpp"
#include "source/soapy_source.hpp"

namespace engine_api_test {

constexpr const char* kFakeRadioDriver = "rtlsdr";
constexpr const char* kFakeRadioArgs = "serial=00000001";
constexpr const char* kFakeRadioId = "rtlsdr:serial=00000001";

// A name a test may give the open radio (empty: the mock's own), to show that
// the engine notices a radio's NAME changing when nothing else about it does
// (tests/test_engine_api_03.cpp, the bias tee dialog's radio line). Only a test
// that drives the engine from its own thread sets it.
inline std::string& fakeRadioNameOverride() {
    static std::string name;
    return name;
}

class FakeRadio final : public cascade::source::DeviceSource {
public:
    explicit FakeRadio(std::string kind) : kind_(std::move(kind)) {}
    bool start() override {
        abort_ = false;
        return true;
    }
    void stop() override { abort_ = true; }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        const auto& r = rates();
        rate_ = *std::min_element(r.begin(), r.end(),
                                  [hz](double a, double b) { return std::fabs(a - hz) < std::fabs(b - hz); });
        return true;
    }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (abort_) { return 0; }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {0.0f, 0.0f}; }
        return n;
    }
    const char* name() const override {
        const std::string& given = fakeRadioNameOverride();
        return given.empty() ? "Mock RTL-SDR (serial 00000001)" : given.c_str();
    }
    const char* lastError() const override { return ""; }
    const char* driverKey() const override { return kind_.c_str(); }
    bool open(const std::string&) override {
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override {
        cascade::source::GainInfo g;
        g.name = "TUNER";
        g.minDb = 0.0;
        g.maxDb = 49.6;
        g.stepDb = 0.1;
        return {g};
    }
    bool setGainDb(const std::string& name, double db) override {
        if (name != "TUNER") { return false; }
        gain_ = std::clamp(std::round(db * 10.0) / 10.0, 0.0, 49.6);
        return true;
    }
    double gainDb(const std::string&) const override { return gain_; }
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override {
        agc_ = on;
        return true;
    }
    bool autoGain() const override { return agc_; }
    std::vector<std::string> antennas() const override { return {"RX"}; }
    bool setAntenna(const std::string& name) override { return name == "RX"; }
    std::string antenna() const override { return "RX"; }
    std::vector<double> supportedSampleRatesHz() const override { return rates(); }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = 1766.0e6;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    static const std::vector<double>& rates() {
        static const std::vector<double> r = {250000.0, 1024000.0, 1400000.0, 2048000.0, 2400000.0, 3200000.0};
        return r;
    }
    std::string kind_;
    std::atomic<bool> abort_{true};
    double rate_ = 2048000.0;
    double centre_ = 100.0e6;
    double gain_ = 29.7;
    bool agc_ = false;
    bool open_ = false;
};

inline std::unique_ptr<cascade::source::DeviceSource> makeFakeRadio(const std::string& kind) {
    return std::make_unique<FakeRadio>(kind);
}

inline std::vector<cascade::source::NativeDeviceInfo> fakeNativeScan() {
    return {{kFakeRadioDriver, "Mock RTL-SDR (serial 00000001)", kFakeRadioArgs}};
}

inline std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }

}  // namespace engine_api_test
