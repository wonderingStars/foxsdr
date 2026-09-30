// THE CRYSTAL CORRECTION ("PPM frequency correction", 0.99.56) below the
// application: core/ppm_correction.hpp's arithmetic and rules, the source view
// that applies the retune fallback (source/converter_view.hpp) through both of
// its owners (Pipeline::activeSource and core::patch::PatchRadio), and the two
// drivers that correct their own crystal - the native RTL-SDR (against
// usb::FakeUsbDevice, the register it writes) and SoapySource (against fake
// SoapySDR drivers registered in-process, with and without
// hasFrequencyCorrection).
//
// test_ppm_app proves the APPLICATION hands the right value to these on every
// open and every change; this proves each of them does the right thing with it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Version.hpp>

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/freq_converter.hpp"
#include "core/patch_radio.hpp"
#include "core/pipeline.hpp"
#include "core/ppm_correction.hpp"
#include "source/converter_view.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/soapy_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

namespace cc = cascade::core;
using cc::PpmMemo;
using cc::PpmMethod;

namespace {

// --- A radio stand-in ------------------------------------------------------------

// Stores what it is told (optionally rounded to a tuning step, or clamped to a
// range the way a driver answers "somewhere else"), and records every tune.
// `corrects` makes it a radio with a correction of its own.
class FakeRadio final : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return false; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 2.4e6; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return centre; }
    bool setCenterFrequencyHz(double hz) override {
        told.push_back(hz);
        if (refuse) { return false; }
        double f = hz;
        if (stepHz > 0.0) { f = std::round(f / stepHz) * stepHz; }
        if (f > topHz) { f = topHz; }
        centre = f;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {0.0f, 0.0f}; }
        return n;
    }
    const char* name() const override { return "Fake radio"; }
    const char* lastError() const override { return ""; }
    bool hasFrequencyCorrection() const override { return corrects; }
    bool setFrequencyCorrectionPpm(double ppm) override {
        corrections.push_back(ppm);
        return corrects;
    }

    double centre = 100.0e6;
    double stepHz = 0.0;
    double topHz = 1.0e12;
    bool refuse = false;
    bool corrects = false;
    std::vector<double> told;
    std::vector<double> corrections;
};

// --- the arithmetic ------------------------------------------------------------------

void testConversion() {
    std::printf("  the conversion, both directions\n");
    // 0 ppm IS THE IDENTITY, bit for bit - not a multiply by 1.0, not a
    // rounding: a fractional frequency passes through untouched.
    for (double f : {100.0e6, 100000000.3, 17200.0, 1766.0e6, 0.0, -283600.0}) {
        CHECK(cc::ppmRequestHz(f, 0.0) == f);
        CHECK(cc::ppmTrueHz(f, 0.0) == f);
        CHECK(cc::ppmReadbackHz(f, 0.0, PpmMemo{true, 1.0, f}) == f);  // memo ignored at 0
    }

    // THE SIGN. +50 ppm: the crystal runs fast, so the radio is asked for
    // LESS than the true frequency and lands on it. 100 MHz / 1.00005 =
    // 99 995 000.25 Hz, told as whole hertz.
    CHECK(cc::ppmRequestHz(100.0e6, 50.0) == 99995000.0);
    CHECK(cc::ppmRequestHz(100.0e6, 50.0) < 100.0e6);
    // -50 ppm: slow crystal, asked for MORE. 100 MHz / 0.99995 = 100 005 000.25.
    CHECK(cc::ppmRequestHz(100.0e6, -50.0) == 100005000.0);
    CHECK(cc::ppmRequestHz(100.0e6, -50.0) > 100.0e6);
    // Back the other way: where a radio +50 ppm out that reports 99 995 000 Hz
    // really is - within the half hertz the request rounded away.
    CHECK_NEAR(cc::ppmTrueHz(99995000.0, 50.0), 100.0e6, 0.5001);
    CHECK_NEAR(cc::ppmTrueHz(100005000.0, -50.0), 100.0e6, 0.5001);
    CHECK(cc::ppmTrueHz(99995000.0, 50.0) > 99995000.0);

    // A LARGE VALUE at the top of an RTL-SDR's range: 200 ppm at 1.7 GHz is
    // 340 kHz, and the round trip still lands within a hertz.
    const double top = 1.7e9;
    const double asked = cc::ppmRequestHz(top, 200.0);
    CHECK_NEAR(top - asked, 339932.0, 1.0);
    CHECK_NEAR(cc::ppmTrueHz(asked, 200.0), top, 1.0);
    CHECK(asked == std::round(asked));  // whole hertz

    // THE READBACK. A radio that still reports exactly what it was told is
    // exactly where the user asked - not a fraction of a hertz off it.
    const PpmMemo memo{true, 100.0e6, 99995000.0};
    CHECK(cc::ppmReadbackHz(99995000.0, 50.0, memo) == 100.0e6);
    // A radio that answered elsewhere (an edge clamp) is read through the
    // conversion, so the move shows.
    CHECK(cc::ppmReadbackHz(99000000.0, 50.0, memo) == cc::ppmTrueHz(99000000.0, 50.0));
    // No memo: the conversion.
    CHECK(cc::ppmReadbackHz(99995000.0, 50.0, PpmMemo{}) == cc::ppmTrueHz(99995000.0, 50.0));
}

void testValuesAndKeys() {
    std::printf("  values, keys, rounding and the method\n");
    CHECK(cc::sanitisePpm(std::nan("")) == 0.0);
    CHECK(cc::sanitisePpm(std::numeric_limits<double>::infinity()) == 0.0);
    CHECK(cc::sanitisePpm(250.0) == 200.0);
    CHECK(cc::sanitisePpm(-1.0e9) == -200.0);
    CHECK(cc::sanitisePpm(1.26) == 1.3);
    CHECK(cc::sanitisePpm(-12.34) == -12.3);
    CHECK(!std::signbit(cc::sanitisePpm(-0.04)));  // no "-0.0"
    CHECK(cc::ppmText(1.5) == "+1.5");
    CHECK(cc::ppmText(-12.0) == "-12.0");
    CHECK(cc::ppmText(-0.01) == "+0.0");

    // The native RTL-SDR register's whole ppm: nearest, halves away from zero.
    CHECK(cc::ppmWholeForRadio(2.4) == 2);
    CHECK(cc::ppmWholeForRadio(2.5) == 3);
    CHECK(cc::ppmWholeForRadio(2.6) == 3);
    CHECK(cc::ppmWholeForRadio(-2.5) == -3);
    CHECK(cc::ppmWholeForRadio(-0.4) == 0);
    CHECK(cc::ppmRadioTakesWholePpm("rtlsdr"));
    CHECK(!cc::ppmRadioTakesWholePpm("soapy"));

    // ONE CRYSTAL, ONE KEY: the native driver's args and SoapySDR's for the
    // same dongle name the same radio.
    CHECK(cc::ppmRadioKey("rtlsdr", "serial=00000001") == "rtlsdr|serial=00000001");
    CHECK(cc::ppmRadioKey("soapy", "driver=rtlsdr, serial=00000001") == "rtlsdr|serial=00000001");
    CHECK(cc::ppmRadioKey("soapy", "driver=RTLSDR,serial=00000001") == "rtlsdr|serial=00000001");
    CHECK(cc::ppmRadioKey("hackrf", "serial=abc") == "hackrf|serial=abc");
    CHECK(cc::ppmRadioKey("soapy", "driver=uhd,serial=31E0000") == "uhd|serial=31E0000");
    // No serial: kind and the whole args.
    CHECK(cc::ppmRadioKey("rtlsdr", "index=0") == "rtlsdr|index=0");
    CHECK(cc::ppmRadioKey("pluto", "uri=ip:192.168.2.1") == "pluto|uri=ip:192.168.2.1");
    // No crystal to correct: no key.
    for (const char* k : {"siggen", "file", "iqfile", "soundcard", ""}) {
        CHECK(cc::ppmRadioKey(k, "serial=1").empty());
        CHECK(!cc::ppmKindApplies(k));
    }

    // THE METHOD: the radio's own where it has one, retuning otherwise, and
    // nothing for a source with no crystal - whatever it claims.
    CHECK(cc::ppmMethodFor("rtlsdr", true) == PpmMethod::InRadio);
    CHECK(cc::ppmMethodFor("soapy", true) == PpmMethod::InRadio);
    CHECK(cc::ppmMethodFor("soapy", false) == PpmMethod::Retune);
    CHECK(cc::ppmMethodFor("hackrf", false) == PpmMethod::Retune);
    CHECK(cc::ppmMethodFor("sdrplay", false) == PpmMethod::Retune);
    CHECK(cc::ppmMethodFor("airspy", false) == PpmMethod::Retune);
    for (const char* k : {"siggen", "file", "iqfile", "soundcard"}) {
        CHECK(cc::ppmMethodFor(k, true) == PpmMethod::NotApplicable);
        CHECK(cc::ppmMethodFor(k, false) == PpmMethod::NotApplicable);
    }

    // The value in force: only while the switch is on, only for its radio.
    const std::map<std::string, double> v = {{"rtlsdr|serial=1", 2.5}, {"hackrf|serial=x", -7.0}};
    CHECK(cc::ppmFor(false, v, "rtlsdr|serial=1") == 0.0);
    CHECK(cc::ppmFor(true, v, "rtlsdr|serial=1") == 2.5);
    CHECK(cc::ppmFor(true, v, "rtlsdr|serial=2") == 0.0);
    CHECK(cc::ppmFor(true, v, "") == 0.0);

    // The remembered map made safe.
    std::map<std::string, double> in = {{"", 3.0}, {"a|serial=1", 0.0}, {"b|serial=1", 999.0},
                                        {"c|serial=1", 1.26}};
    for (int i = 0; i < 100; ++i) { in["z|serial=" + std::to_string(i)] = 1.0; }
    const std::map<std::string, double> out = cc::sanitisePpmValues(in);
    CHECK(out.count("") == 0);
    CHECK(out.count("a|serial=1") == 0);  // 0 is "no entry"
    CHECK(out.at("b|serial=1") == 200.0);
    CHECK(out.at("c|serial=1") == 1.3);
    CHECK(out.size() == cc::kMaxPpmRadios);
}

// --- the view, and its two owners ------------------------------------------------------

void testView() {
    std::printf("  the source view applies the retune fallback, and nothing at 0 ppm\n");
    FakeRadio radio;
    cascade::source::ConverterView view;
    view.bind(&radio);

    // OFF: exactly what the view always did - the request reaches the radio
    // untouched, fractions and all, and the readback is the radio's.
    CHECK(view.setCenterFrequencyHz(100000000.3));
    CHECK(radio.told.back() == 100000000.3);
    CHECK(view.centerFrequencyHz() == 100000000.3);

    // +50 ppm: the radio is told the corrected frequency and the view reads
    // back EXACTLY the true one.
    view.setPpm(50.0);
    CHECK(view.setCenterFrequencyHz(100.0e6));
    CHECK(radio.told.back() == cc::ppmRequestHz(100.0e6, 50.0));
    CHECK(radio.centre == 99995000.0);
    CHECK(view.centerFrequencyHz() == 100.0e6);

    // WITH A CONVERTER: the correction sits between the converter and the
    // radio. 17.2 kHz through a 125 MHz up-converter, 10 ppm fast.
    view.setConverter({cc::ConverterMode::Up, 125.0e6, false});
    view.setPpm(10.0);
    CHECK(view.setCenterFrequencyHz(17200.0));
    CHECK(radio.told.back() == cc::ppmRequestHz(125017200.0, 10.0));
    CHECK(view.centerFrequencyHz() == 17200.0);
    view.setConverter(cc::ConverterSetting{});

    // A RADIO ON ITS OWN TUNING STEP (1 kHz): it lands near, not on, what it
    // was told, and the readback says so - within the tune-mismatch
    // tolerance, so no false "answered somewhere else".
    radio.stepHz = 1000.0;
    view.setPpm(50.0);
    CHECK(view.setCenterFrequencyHz(100.0e6));
    CHECK(radio.centre == 99995000.0);  // 99 995 000 is on the step
    CHECK(view.setCenterFrequencyHz(100.0003e6));
    CHECK(view.centerFrequencyHz() != 100.0003e6);
    CHECK_NEAR(view.centerFrequencyHz(), 100.0003e6, 500.0);
    radio.stepHz = 0.0;

    // A RADIO THAT ANSWERED SOMEWHERE ELSE (clamped at its top) still reads as
    // having moved, correction or not.
    radio.topHz = 1766.0e6;
    CHECK(view.setCenterFrequencyHz(1800.0e6));
    CHECK(std::fabs(view.centerFrequencyHz() - 1800.0e6) > 30.0e6);
    radio.topHz = 1.0e12;

    // A REFUSED tune leaves the last one's exact readback in place.
    CHECK(view.setCenterFrequencyHz(144.8e6));
    radio.refuse = true;
    CHECK(!view.setCenterFrequencyHz(145.0e6));
    CHECK(view.centerFrequencyHz() == 144.8e6);
    radio.refuse = false;

    // A new value re-reads the radio through it (no retune of its own): the
    // caller retunes to keep the station.
    const std::size_t tunes = radio.told.size();
    view.setPpm(-20.0);
    CHECK(radio.told.size() == tunes);
    CHECK(view.centerFrequencyHz() == cc::ppmTrueHz(radio.centre, -20.0));
    // Back to 0: the radio's own figure again.
    view.setPpm(0.0);
    CHECK(view.centerFrequencyHz() == radio.centre);

    // A memo handed over (the patch worker's first tune) reads back exactly.
    radio.centre = 99995000.0;
    view.setPpm(50.0, PpmMemo{true, 100.0e6, 99995000.0});
    CHECK(view.centerFrequencyHz() == 100.0e6);
    // A rebind forgets it.
    view.bind(&radio);
    CHECK(view.centerFrequencyHz() == cc::ppmTrueHz(99995000.0, 50.0));

    // The radio's own correction passes through the view.
    CHECK(!view.hasFrequencyCorrection());
    radio.corrects = true;
    CHECK(view.hasFrequencyCorrection());
    CHECK(view.setFrequencyCorrectionPpm(1.5));
    CHECK(radio.corrections.back() == 1.5);
}

cc::Pipeline::Config testConfig() {
    cc::Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;
    return cfg;
}

void testPipeline() {
    std::printf("  the pipeline: activeSource() corrects, and a swap resets it\n");
    cc::Pipeline p(testConfig());
    auto fake = std::make_unique<FakeRadio>();
    FakeRadio* radio = fake.get();
    p.setSource(std::move(fake));
    CHECK(p.softwarePpm() == 0.0);
    p.setSoftwarePpm(-12.5);
    CHECK(p.softwarePpm() == -12.5);
    CHECK(p.activeSource().setCenterFrequencyHz(433.92e6));
    CHECK(radio->told.back() == cc::ppmRequestHz(433.92e6, -12.5));
    CHECK(p.rawSource().centerFrequencyHz() == cc::ppmRequestHz(433.92e6, -12.5));
    CHECK(p.activeSource().centerFrequencyHz() == 433.92e6);

    // A SWAP DOES NOT CARRY ONE RADIO'S CRYSTAL ONTO THE NEXT.
    auto next = std::make_unique<FakeRadio>();
    FakeRadio* radio2 = next.get();
    p.setSource(std::move(next));
    CHECK(p.softwarePpm() == 0.0);
    CHECK(p.activeSource().setCenterFrequencyHz(433.92e6));
    CHECK(radio2->told.back() == 433.92e6);
}

void testPatchRadio() {
    std::printf("  a patch radio: both methods\n");
    {
        auto fake = std::make_unique<FakeRadio>();
        FakeRadio* radio = fake.get();
        cc::patch::PatchRadio r(1, std::move(fake), "Fake");
        CHECK(!r.radioCorrects());
        CHECK(r.softwarePpm() == 0.0);
        CHECK(r.setCentreHz(145.0e6));
        CHECK(radio->told.back() == 145.0e6);  // off: untouched
        r.setSoftwarePpm(30.0);
        CHECK(r.setCentreHz(145.0e6));
        CHECK(radio->told.back() == cc::ppmRequestHz(145.0e6, 30.0));
        CHECK(r.centreHz() == 145.0e6);
        CHECK(radio->corrections.empty());
    }
    {
        auto fake = std::make_unique<FakeRadio>();
        fake->corrects = true;
        FakeRadio* radio = fake.get();
        cc::patch::PatchRadio r(2, std::move(fake), "Fake");
        CHECK(r.radioCorrects());
        CHECK(r.radioPpm() == 0.0);
        CHECK(r.setRadioPpm(-4.2));
        CHECK(r.radioPpm() == -4.2);
        CHECK(radio->corrections.back() == -4.2);
        // In the radio: the view corrects nothing.
        CHECK(r.setCentreHz(145.0e6));
        CHECK(radio->told.back() == 145.0e6);
        r.noteRadioPpm(1.0);
        CHECK(r.radioPpm() == 1.0);
    }
}

// --- the native RTL-SDR: the register the correction goes into --------------------------

struct DemodWrite {
    std::uint8_t page;
    std::uint8_t addr;
    std::uint8_t value;
};
std::vector<DemodWrite> demodWrites(const std::vector<cascade::usb::FakeControl>& v) {
    std::vector<DemodWrite> out;
    for (const cascade::usb::FakeControl& c : v) {
        if (!(c.out && c.index < 0x100) || c.data.empty()) { continue; }
        out.push_back({static_cast<std::uint8_t>(c.index & 0x0f),
                       static_cast<std::uint8_t>(c.value >> 8), c.data[0]});
    }
    return out;
}

// The resampler trim register pair (page 1, 0x3f low byte and 0x3e top six
// bits) as the last write left it, or -1 when it was never written.
int lastTrim(const std::vector<cascade::usb::FakeControl>& v, std::uint8_t addr) {
    int last = -1;
    for (const DemodWrite& d : demodWrites(v)) {
        if (d.page == 1 && d.addr == addr) { last = d.value; }
    }
    return last;
}

void testRtlSdrNative() {
    std::printf("  the native RTL-SDR corrects in the radio, in whole ppm\n");
    auto fake = std::make_unique<cascade::usb::FakeUsbDevice>();
    // A plain R820T dongle (tests/test_rtlsdr_source.cpp: dressAsR820T).
    fake->answerIn(0, 0x0034, 0x0600, {0x69, 0x00, 0x02, 0x00, 0xA4});
    fake->answerIn(0, 0x00a0, 0x0600, {0x00});
    cascade::usb::FakeUsbDevice* live = fake.get();
    cascade::source::RtlSdrSource src;
    CHECK(src.openWithTransport(std::move(fake), "fake R820T dongle"));
    CHECK(src.hasFrequencyCorrection());
    CHECK(src.freqCorrectionPpm() == 0);
    const double centre = src.centerFrequencyHz();

    // 2.6 ppm -> 3 whole ppm. The trim is -ppm * 2^24 / 1e6 = -50.33 -> -50
    // = 0xFFCE: 0x3f takes 0xCE, 0x3e the top six bits 0x3f.
    live->clear();
    cascade::source::IqSource& asSource = src;
    CHECK(asSource.setFrequencyCorrectionPpm(2.6));
    CHECK(src.freqCorrectionPpm() == 3);
    CHECK(lastTrim(live->writes(), 0x3f) == 0xCE);
    CHECK(lastTrim(live->writes(), 0x3e) == 0x3f);
    // The correction retuned the dongle where it already was, and it still
    // reports the frequency it was on - the correction is inside the radio.
    CHECK(src.centerFrequencyHz() == centre);

    // -2.5 -> -3: +50 = 0x0032, so 0x3f = 0x32 and 0x3e = 0x00.
    live->clear();
    CHECK(asSource.setFrequencyCorrectionPpm(-2.5));
    CHECK(src.freqCorrectionPpm() == -3);
    CHECK(lastTrim(live->writes(), 0x3f) == 0x32);
    CHECK(lastTrim(live->writes(), 0x3e) == 0x00);

    // Off again: 0 ppm, a zero trim.
    CHECK(asSource.setFrequencyCorrectionPpm(0.0));
    CHECK(src.freqCorrectionPpm() == 0);
    CHECK(lastTrim(live->writes(), 0x3f) == 0x00);
    src.closeDevice();
}

// --- SoapySDR: hasFrequencyCorrection decides ---------------------------------------------

struct SoapyRecord {
    std::vector<double> corrections;
    std::vector<double> tunes;
};
SoapyRecord g_soapy;

class PpmSoapyDevice : public SoapySDR::Device {
public:
    explicit PpmSoapyDevice(bool corrects) : corrects_(corrects) {}
    std::string getDriverKey() const override { return corrects_ ? "fakeppmyes" : "fakeppmno"; }
    std::string getHardwareKey() const override { return "fake ppm radio"; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&, const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long, const size_t) override {
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override { return 0; }
    double getSampleRate(const int, const size_t) const override { return 2.4e6; }
    void setFrequency(const int, const size_t, const double f, const SoapySDR::Kwargs&) override {
        g_soapy.tunes.push_back(f);
        freq_ = f;
    }
    double getFrequency(const int, const size_t) const override { return freq_; }
    bool hasFrequencyCorrection(const int, const size_t) const override { return corrects_; }
    void setFrequencyCorrection(const int, const size_t, const double value) override {
        g_soapy.corrections.push_back(value);
    }
    int readStream(SoapySDR::Stream*, void* const*, const size_t, int&, long long&,
                   const long) override {
        return SOAPY_SDR_TIMEOUT;
    }

private:
    bool corrects_;
    double freq_ = 100.0e6;
};

SoapySDR::KwargsList findYes(const SoapySDR::Kwargs& args) {
    const auto d = args.find("driver");
    if (d != args.end() && d->second != "fakeppmyes") { return {}; }
    return SoapySDR::KwargsList{{{"driver", "fakeppmyes"}, {"label", "fake ppm radio"}}};
}
SoapySDR::KwargsList findNo(const SoapySDR::Kwargs& args) {
    const auto d = args.find("driver");
    if (d != args.end() && d->second != "fakeppmno") { return {}; }
    return SoapySDR::KwargsList{{{"driver", "fakeppmno"}, {"label", "fake ppm radio"}}};
}
SoapySDR::Device* makeYes(const SoapySDR::Kwargs&) { return new PpmSoapyDevice(true); }
SoapySDR::Device* makeNo(const SoapySDR::Kwargs&) { return new PpmSoapyDevice(false); }

void testSoapy() {
    std::printf("  SoapySDR: in the radio when the driver reports a correction, else retune\n");
    SoapySDR::Registry yes("fakeppmyes", &findYes, &makeYes, SOAPY_SDR_ABI_VERSION);
    SoapySDR::Registry no("fakeppmno", &findNo, &makeNo, SOAPY_SDR_ABI_VERSION);
    {
        cascade::source::SoapySource src;
        CHECK(!src.hasFrequencyCorrection());  // nothing open
        CHECK(!src.setFrequencyCorrectionPpm(1.0));
        CHECK(src.open("driver=fakeppmyes"));
        CHECK(src.hasFrequencyCorrection());
        CHECK(cc::ppmMethodFor("soapy", src.hasFrequencyCorrection()) == PpmMethod::InRadio);
        CHECK(src.setCenterFrequencyHz(433.92e6));
        g_soapy = SoapyRecord{};
        CHECK(src.setFrequencyCorrectionPpm(1.5));
        CHECK(g_soapy.corrections.size() == 1u && g_soapy.corrections.back() == 1.5);
        // RETUNED to where it already was, so the correction takes effect now.
        CHECK(g_soapy.tunes.size() == 1u && g_soapy.tunes.back() == 433.92e6);
        CHECK(src.centerFrequencyHz() == 433.92e6);
        src.closeDevice();
        CHECK(!src.hasFrequencyCorrection());  // forgotten with the device
    }
    {
        cascade::source::SoapySource src;
        CHECK(src.open("driver=fakeppmno"));
        CHECK(!src.hasFrequencyCorrection());
        CHECK(cc::ppmMethodFor("soapy", src.hasFrequencyCorrection()) == PpmMethod::Retune);
        g_soapy = SoapyRecord{};
        CHECK(!src.setFrequencyCorrectionPpm(1.5));
        CHECK(std::strlen(src.lastError()) > 0);
        CHECK(g_soapy.corrections.empty());  // never sent to a driver without one
        src.closeDevice();
    }
}

}  // namespace

int main() {
    std::printf("test_ppm_correction\n");
    testConversion();
    testValuesAndKeys();
    testView();
    testPipeline();
    testPatchRadio();
    testRtlSdrNative();
    testSoapy();
    return testSummary("test_ppm_correction");
}
