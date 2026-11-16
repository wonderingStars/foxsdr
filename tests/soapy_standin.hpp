// soapy_standin.hpp - STAND-IN SOAPYSDR DRIVERS for the S5b tests (screen lane S5b, rows G1-15a/b): three in-process SoapySDR::Device classes a test registers with SoapySDR::Registry, so SoapySource::open("driver=...")
// reaches them through the real registry, the real device lock and the real vendor guard. Nothing here touches a real module or a real radio. Everything the "driver" is handed is logged in g_log.
//   standinbare     neither a bandwidth range nor a setting
//   standinrich     a bandwidth range (200 kHz - 1.5 MHz and 5 - 8 MHz), settings of every type, one the driver refuses and one it rounds
//   standinhostile  200 settings with control characters and over-long text, a setting that cannot be read, ranges that are NaN, infinite or negative
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Version.hpp>

#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace soapystandin {

// What the stand-in driver was handed, in order (the proof is read from here).
struct Log {
    std::mutex m;
    std::vector<std::string> calls;
    void add(const std::string& s) { std::lock_guard<std::mutex> lk(m); calls.push_back(s); }
    std::size_t count(const std::string& prefix) {
        std::lock_guard<std::mutex> lk(m);
        std::size_t n = 0;
        for (const std::string& c : calls) { if (c.rfind(prefix, 0) == 0) { ++n; } }
        return n;
    }
    std::string last(const std::string& prefix) {
        std::lock_guard<std::mutex> lk(m);
        for (auto it = calls.rbegin(); it != calls.rend(); ++it) { if (it->rfind(prefix, 0) == 0) { return *it; } }
        return std::string();
    }
    void clear() { std::lock_guard<std::mutex> lk(m); calls.clear(); }
};
Log g_log;

// The stream half every driver below shares: nothing flows, but open() can set one up.
class StreamBase : public SoapySDR::Device {
public:
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&, const std::vector<size_t>&, const SoapySDR::Kwargs&) override { return reinterpret_cast<SoapySDR::Stream*>(this); }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long, const size_t) override { return 0; }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override { return 0; }
    double getSampleRate(const int, const size_t) const override { return 2.0e6; }
    double getFrequency(const int, const size_t) const override { return 100.0e6; }
    int readStream(SoapySDR::Stream*, void* const*, const size_t, int&, long long&, const long) override { return SOAPY_SDR_TIMEOUT; }
};

// A driver that has nothing of either kind (the Soapy base class answers empty lists).
class BareDevice : public StreamBase {
public:
    std::string getDriverKey() const override { return "standinbare"; }
    std::string getHardwareKey() const override { return "stand-in with no settings"; }
    size_t getNumChannels(const int) const override { return 1; }
    double getSampleRate(const int, const size_t) const override { return 2.0e6; }
    double getFrequency(const int, const size_t) const override { return 100.0e6; }
    void setBandwidth(const int, const size_t, const double bw) override { g_log.add("setBandwidth " + std::to_string(bw)); }
    void writeSetting(const std::string& k, const std::string& v) override { g_log.add("writeSetting " + k + "=" + v); }
};

// A driver with a bandwidth range and settings of every type.
class RichDevice : public StreamBase {
public:
    std::string getDriverKey() const override { return "standinrich"; }
    std::string getHardwareKey() const override { return "stand-in with settings"; }
    size_t getNumChannels(const int) const override { return 1; }
    double getSampleRate(const int, const size_t) const override { return 2.0e6; }
    double getFrequency(const int, const size_t) const override { return 100.0e6; }

    std::vector<SoapySDR::Range> getBandwidthRange(const int, const size_t) const override {
        return {SoapySDR::Range(200e3, 1.5e6), SoapySDR::Range(5e6, 8e6)};
    }
    double getBandwidth(const int, const size_t) const override { return bw_; }
    void setBandwidth(const int, const size_t, const double bw) override {
        g_log.add("setBandwidth " + std::to_string(static_cast<long long>(bw)));
        bw_ = bw < 300e3 ? 300e3 : bw;   // a driver that rounds a very small request up
    }

    SoapySDR::ArgInfoList getSettingInfo() const override {
        SoapySDR::ArgInfoList l;
        SoapySDR::ArgInfo b; b.key = "biastee"; b.name = "Bias tee"; b.description = "Power the antenna port"; b.type = SoapySDR::ArgInfo::BOOL; b.value = "false";
        l.push_back(b);
        SoapySDR::ArgInfo i; i.key = "offset"; i.name = "Tuner offset"; i.units = "kHz"; i.type = SoapySDR::ArgInfo::INT; i.value = "0"; i.range = SoapySDR::Range(-100, 100);
        l.push_back(i);
        SoapySDR::ArgInfo f; f.key = "trim"; f.name = "Crystal trim"; f.units = "ppm"; f.type = SoapySDR::ArgInfo::FLOAT; f.value = "0.0"; f.range = SoapySDR::Range(-50.0, 50.0);
        l.push_back(f);
        SoapySDR::ArgInfo s; s.key = "clock"; s.name = "Clock source"; s.type = SoapySDR::ArgInfo::STRING; s.value = "internal";
        s.options = {"internal", "external", "gpsdo"}; s.optionNames = {"Internal", "External 10 MHz", "GPS"};
        l.push_back(s);
        SoapySDR::ArgInfo t; t.key = "label"; t.name = "Free text"; t.type = SoapySDR::ArgInfo::STRING; t.value = "";
        l.push_back(t);
        SoapySDR::ArgInfo r; r.key = "refuses"; r.name = "A setting the driver refuses"; r.type = SoapySDR::ArgInfo::INT; r.value = "1";
        l.push_back(r);
        SoapySDR::ArgInfo c; c.key = "clamps"; c.name = "A setting the driver rounds"; c.type = SoapySDR::ArgInfo::INT; c.value = "5";
        l.push_back(c);
        return l;
    }
    std::string readSetting(const std::string& k) const override {
        const auto it = values_.find(k);
        if (it != values_.end()) { return it->second; }
        if (k == "biastee") { return "false"; }
        if (k == "offset") { return "0"; }
        if (k == "trim") { return "0.0"; }
        if (k == "clock") { return "internal"; }
        if (k == "clamps") { return "5"; }
        if (k == "refuses") { return "1"; }
        return "";
    }
    void writeSetting(const std::string& k, const std::string& v) override {
        g_log.add("writeSetting " + k + "=" + v);
        if (k == "refuses") { throw std::runtime_error("the driver will not change that while a stream is running"); }
        if (k == "clamps") { values_[k] = "10"; return; }   // asked for 99, kept 10
        values_[k] = v;
    }

private:
    double bw_ = 1.0e6;
    std::map<std::string, std::string> values_;
};

// A driver that lies in every direction.
class HostileDevice : public StreamBase {
public:
    std::string getDriverKey() const override { return "standinhostile"; }
    std::string getHardwareKey() const override { return "hostile stand-in"; }
    size_t getNumChannels(const int) const override { return 1; }
    double getSampleRate(const int, const size_t) const override { return 2.0e6; }
    double getFrequency(const int, const size_t) const override { return 100.0e6; }
    std::vector<SoapySDR::Range> getBandwidthRange(const int, const size_t) const override {
        return {SoapySDR::Range(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()), SoapySDR::Range(-5.0, -1.0)};
    }
    SoapySDR::ArgInfoList getSettingInfo() const override {
        SoapySDR::ArgInfoList l;
        for (int n = 0; n < 200; ++n) {
            SoapySDR::ArgInfo a;
            a.key = "k" + std::to_string(n);
            a.name = n == 0 ? std::string("line one\nline two\x01 and a long tail ") + std::string(500, 'x') : "n" + std::to_string(n);
            a.type = SoapySDR::ArgInfo::STRING;
            l.push_back(a);
        }
        SoapySDR::ArgInfo bad; bad.key = std::string(500, 'q'); l.push_back(bad);
        SoapySDR::ArgInfo empty; l.push_back(empty);
        return l;
    }
    std::string readSetting(const std::string& k) const override {
        if (k == "k1") { throw std::runtime_error("cannot be read"); }
        return std::string(1000, 'v');
    }
};

SoapySDR::KwargsList findFor(const SoapySDR::Kwargs& args, const char* key, const char* label) {
    const auto d = args.find("driver");
    if (d != args.end() && d->second != key) { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = key;
    k["label"] = label;
    return SoapySDR::KwargsList{k};
}
SoapySDR::KwargsList findBare(const SoapySDR::Kwargs& a) { return findFor(a, "standinbare", "bare"); }
SoapySDR::KwargsList findRich(const SoapySDR::Kwargs& a) { return findFor(a, "standinrich", "rich"); }
SoapySDR::KwargsList findHostile(const SoapySDR::Kwargs& a) { return findFor(a, "standinhostile", "hostile"); }
SoapySDR::Device* makeBare(const SoapySDR::Kwargs&) { return new BareDevice(); }
SoapySDR::Device* makeRich(const SoapySDR::Kwargs&) { return new RichDevice(); }
SoapySDR::Device* makeHostile(const SoapySDR::Kwargs&) { return new HostileDevice(); }

}  // namespace soapystandin
