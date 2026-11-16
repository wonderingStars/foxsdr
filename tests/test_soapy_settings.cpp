// test_soapy_settings.cpp - THE SOAPY DRIVER'S OWN BANDWIDTH AND SETTINGS, AGAINST A STAND-IN MODULE (screen lane S5b, rows G1-15a and G1-15b).
//
// The stand-in is a SoapySDR::Device registered in-process (as test_soapy_read_bound.cpp does), so SoapySource::open("driver=...") reaches it through the real SoapySDR registry and every call below goes through
// the source's device lock and vendor guard exactly as a real module's would. Everything asserted is what the STAND-IN received or answered, never what the source stored.
//   A  a driver with neither bandwidth nor settings: bandwidthInfo() says unsupported, listSettings() is empty, both without an error; setBandwidthHz and writeSetting refuse with a sentence
//   B  a driver with both: the list carries every setting the driver reports (key, name, type, range, options, current value, units) and nothing else; the bandwidth range and current value come from the driver
//   C  setBandwidthHz writes the driver's setBandwidth with the Hz asked and reads the driver's own answer back; a value outside the driver's range, 0, negative, NaN and infinity never reach the driver
//   D  writeSetting delivers key and value to the driver's writeSetting and returns its readSetting; a driver that clamps shows the clamped value; a driver that throws is shown as refused with the driver's words;
//      a value of the wrong type, outside the range or not among the options is refused before the driver is called; an unknown key is refused
//   E  a hostile list (200 settings, control characters, over-long text, a setting that cannot be read) is bounded and cleaned
//   F  nothing is called with no device open
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Version.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "soapy_standin.hpp"
#include "test_check.hpp"

namespace {

using cascade::source::SoapySource;

using namespace soapystandin;

const SoapySource::SettingInfo* find(const std::vector<SoapySource::SettingInfo>& v, const std::string& key) {
    for (const auto& s : v) { if (s.key == key) { return &s; } }
    return nullptr;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_soapy_settings\n");
    SoapySDR::Registry regBare("standinbare", &findBare, &makeBare, SOAPY_SDR_ABI_VERSION);
    SoapySDR::Registry regRich("standinrich", &findRich, &makeRich, SOAPY_SDR_ABI_VERSION);
    SoapySDR::Registry regHostile("standinhostile", &findHostile, &makeHostile, SOAPY_SDR_ABI_VERSION);

    // F: nothing with no device open
    {
        SoapySource src;
        CHECK(!src.bandwidthInfo().supported && src.listSettings().empty());
        CHECK(!src.setBandwidthHz(1.0e6) && *src.lastError() != 0);
        CHECK(!src.writeSetting("biastee", "true") && *src.lastError() != 0);
    }

    // A: a driver with neither
    {
        SoapySource src;
        CHECK(src.open("driver=standinbare"));
        const SoapySource::BandwidthInfo bi = src.bandwidthInfo();
        CHECK(!bi.supported);
        CHECK(src.listSettings().empty());
        g_log.clear();
        CHECK(!src.setBandwidthHz(1.0e6));
        CHECK(std::string(src.lastError()).find("no bandwidth setting") != std::string::npos);
        CHECK(!src.writeSetting("biastee", "true"));
        CHECK(std::string(src.lastError()).find("no setting called") != std::string::npos);
        CHECK(g_log.count("setBandwidth") == 0 && g_log.count("writeSetting") == 0);   // the driver was never asked to change anything
        src.closeDevice();
    }

    // B, C, D: a driver with both
    {
        SoapySource src;
        CHECK(src.open("driver=standinrich"));
        const SoapySource::BandwidthInfo bi = src.bandwidthInfo();
        CHECK(bi.supported && bi.currentHz == 1.0e6 && bi.loHz == 200e3 && bi.hiHz == 8e6);   // the driver's own range and value

        const std::vector<SoapySource::SettingInfo> l = src.listSettings();
        CHECK(l.size() == 7);
        const auto* bias = find(l, "biastee");
        const auto* off = find(l, "offset");
        const auto* trim = find(l, "trim");
        const auto* clock = find(l, "clock");
        CHECK(bias && bias->type == SoapySource::SettingType::Bool && bias->value == "false" && bias->name == "Bias tee" && !bias->hasRange);
        CHECK(off && off->type == SoapySource::SettingType::Int && off->hasRange && off->lo == -100 && off->hi == 100 && off->units == "kHz");
        CHECK(trim && trim->type == SoapySource::SettingType::Float && trim->hasRange && trim->hi == 50.0 && trim->units == "ppm");
        CHECK(clock && clock->type == SoapySource::SettingType::String && clock->options.size() == 3 && clock->optionNames[1] == "External 10 MHz" && clock->value == "internal");
        CHECK(find(l, "nothing") == nullptr);

        // C: bandwidth
        g_log.clear();
        double applied = 0.0;
        CHECK(src.setBandwidthHz(500e3, &applied));
        CHECK(g_log.last("setBandwidth") == "setBandwidth 500000" && applied == 500e3);
        CHECK(src.bandwidthInfo().currentHz == 500e3);                            // read back from the driver, not a copy
        CHECK(src.setBandwidthHz(250e3, &applied));
        CHECK(applied == 300e3);                                                  // the driver rounded it up: the readback is what is reported
        g_log.clear();
        for (const double bad : {0.0, -1.0, 100e3, 3e6, 9e6, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            CHECK(!src.setBandwidthHz(bad));
            CHECK(*src.lastError() != 0);
        }
        CHECK(g_log.count("setBandwidth") == 0);                                  // nothing outside the driver's own range reached it
        CHECK(std::string(src.lastError()).find("accepts bandwidths from 200000 to 8000000 Hz") != std::string::npos || std::string(src.lastError()).find("positive") != std::string::npos);
        CHECK(src.setBandwidthHz(6e6) && g_log.last("setBandwidth") == "setBandwidth 6000000");   // inside the second range

        // D: settings
        g_log.clear();
        std::string rb;
        CHECK(src.writeSetting("biastee", "true", &rb) && rb == "true");
        CHECK(g_log.last("writeSetting") == "writeSetting biastee=true");
        CHECK(src.writeSetting("offset", "-40", &rb) && rb == "-40");
        CHECK(src.writeSetting("trim", "1.25", &rb) && rb == "1.25");
        CHECK(src.writeSetting("clock", "external", &rb) && rb == "external");
        CHECK(src.writeSetting("label", "my antenna", &rb) && rb == "my antenna");
        CHECK(find(src.listSettings(), "offset")->value == "-40");                 // the list reads the driver again: the value after a write
        // a driver that rounds: the readback is what is reported
        CHECK(src.writeSetting("clamps", "99", &rb) && rb == "10");
        // a driver that refuses: shown as refused with its words
        g_log.clear();
        CHECK(!src.writeSetting("refuses", "2", &rb));
        CHECK(std::string(src.lastError()).find("will not change that while a stream is running") != std::string::npos);
        CHECK(g_log.count("writeSetting") == 1);                                   // it was asked once
        // wrong type, outside the range, not among the options, unknown key: refused before the driver is called
        g_log.clear();
        CHECK(!src.writeSetting("biastee", "maybe") && std::string(src.lastError()).find("on or off") != std::string::npos);
        CHECK(!src.writeSetting("offset", "abc") && std::string(src.lastError()).find("number") != std::string::npos);
        CHECK(!src.writeSetting("offset", "1.5") && std::string(src.lastError()).find("whole number") != std::string::npos);
        CHECK(!src.writeSetting("offset", "500") && std::string(src.lastError()).find("accepts -100 to 100") != std::string::npos);
        CHECK(!src.writeSetting("trim", "nan") && !src.writeSetting("trim", "inf") && !src.writeSetting("trim", ""));
        CHECK(!src.writeSetting("trim", "51") && std::string(src.lastError()).find("accepts -50 to 50") != std::string::npos);
        CHECK(!src.writeSetting("clock", "atomic") && std::string(src.lastError()).find("not one of the values") != std::string::npos);
        CHECK(!src.writeSetting("nokey", "1") && std::string(src.lastError()).find("no setting called") != std::string::npos);
        CHECK(g_log.count("writeSetting") == 0);
        src.closeDevice();
        CHECK(!src.writeSetting("biastee", "true"));
    }

    // E: a hostile driver
    {
        SoapySource src;
        CHECK(src.open("driver=standinhostile"));
        const std::vector<SoapySource::SettingInfo> l = src.listSettings();
        CHECK(l.size() == 64);                                                    // bounded: the first 64 of 202
        CHECK(!l.empty() && l[0].name.size() <= 100 && l[0].name.find('\n') == std::string::npos && l[0].name.find('\x01') == std::string::npos);
        for (const auto& s : l) { CHECK(s.value.size() <= 100 && s.key.size() <= 100 && !s.key.empty()); }
        CHECK(find(l, "k1") != nullptr);                                           // an unreadable setting is listed with the driver's own default, not dropped or thrown
        const SoapySource::BandwidthInfo bi = src.bandwidthInfo();
        CHECK(!bi.supported);                                                     // NaN, infinite and negative ranges are not believed
        CHECK(!src.setBandwidthHz(1e6));
        src.closeDevice();
    }

    return testSummary("test_soapy_settings");
}
