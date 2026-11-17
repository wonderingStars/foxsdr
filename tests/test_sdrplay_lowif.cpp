// test_sdrplay_lowif.cpp - THE SDRPLAY LOW-IF SETTING (screen lane S5b, row G1-13), OFF BY DEFAULT, over the fake SDRplay API of tests/sdrplay_fake_api.hpp. No SDRplay API, service or radio is touched.
//
// WHY THIS ROW IS DANGEROUS: low-IF was RETIRED in 0.99.44 after two real RSP2 logs showed the ADC rate (6 MS/s) arriving where the plan expected 2 MS/s. There is no SDRplay on the desk, so low-IF is unproven on a real radio.
// It is therefore a setting that is OFF by default, with a first-stream rate check that falls back to zero-IF on a mismatch and says so, and a default path proved unchanged.
//   A  THE DEFAULT PATH IS UNCHANGED: the complete list of API calls of open, start, rate changes, tune, gain and stop, recorded from the fake API, equals the list recorded from the tree BEFORE the setting existed (kGolden,
//      captured by running this section against the unmodified source); a source that never has the setting touched also reads back zero-IF, no note, and offers exactly sdrPlaySupportedRatesHz()
//   B  the setting on: the rate menu is the low-IF one; a rate of it writes the listed (fs, BW, IF) combination in ONE Update; zero-IF again restores today's plan
//   C  the first-stream check: a stream that delivers the planned rate keeps low-IF (and says nothing); a stream that delivers the ADC rate (the 0.99.43 field behaviour) falls back to zero-IF, writes the zero-IF plan to the
//      radio and carries the sentence; a stream that delivers some other rate falls back too
//   D  a radio that refuses the low-IF Update leaves the radio where it was and the setting off
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/radio_events.hpp"
#include "core/radio_reporter.hpp"
#include "sdrplay_fake_api.hpp"
#include "source/sdrplay_if_mode.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

using cascade::source::SdrPlaySource;
using fakesdrplay::FakeSdrPlayApi;
namespace abi = cascade::source::sdrplay_abi;
namespace src_ns = cascade::source;
namespace rr = cascade::core::radioreport;
namespace fs = std::filesystem;

namespace {

// The radio reporter over a stand-in transport and a temporary folder (error-path third reading): the fall-back below is a radio's setting that did not take, and is said to the owner.
struct ReportRig {
    fs::path dir;
    std::mutex mu;
    std::vector<std::string> bodies;
    ReportRig() {
        dir = fs::temp_directory_path() / ("lowif-" + rr::randomHex(8));
        fs::create_directories(dir);
        rr::Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.liveSend = true;
        o.transport = [this](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(body);
            return rr::PostResult{true, 204, 0, ""};
        };
        o.identity.installId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.launches = 3;
        rr::global().configure(o);
    }
    ~ReportRig() {
        rr::global().shutdown();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // how many radio_settings messages the reporter sends now whose reason says the low-IF mode fell back
    int fallBackReports() {
        {
            std::lock_guard<std::mutex> lk(mu);
            bodies.clear();
        }
        rr::global().runBatchNow();
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (const std::string& b : bodies) {
            const nlohmann::json m = nlohmann::json::parse(b);
            if (m.value("kind", std::string()) == "radio_settings" && m["body"].value("reason", std::string()).find("low-IF fell back to zero-IF") != std::string::npos) { ++n; }
        }
        return n;
    }
};

bool openOn(SdrPlaySource& src, FakeSdrPlayApi& fake) {
    src.setApiForTest(&fake.table);
    return src.open(std::string());
}

// THE LIST RECORDED FROM THE TREE BEFORE THE SETTING EXISTED (this section was run against the unmodified source of 57520bd8 and its output pasted here):
const std::vector<std::string> kGolden = {
    "Open",
    "ApiVersion",
    "LockDeviceApi",
    "GetDevices",
    "SelectDevice(tuner=1,mode=0)",
    "UnlockDeviceApi",
    "DebugEnable",
    "GetDeviceParams",
    "Init",
    "Update(0x00040001,0x00000000)",
    "Update(0x00840001,0x00000000)",
    "Update(0x00840001,0x00000000)",
    "Update(0x00008000,0x00000000)",
    "Uninit",
    "ReleaseDevice",
    "Close"};

// The list of API calls (every call, in order) of the default path: open, start, a run of rate changes, a tune, two gains, stop, close.
std::vector<std::string> defaultPathCalls(bool touchTheSetting) {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    if (touchTheSetting) { CHECK(src.setLowIf(false)); }   // asking for what it already is changes nothing
    CHECK(src.setSampleRateHz(8000000.0));
    CHECK(src.setSampleRateHz(384000.0));
    CHECK(src.setSampleRateHz(2000000.0));
    CHECK(src.setCenterFrequencyHz(100000000.0));
    CHECK(src.setGainDb("IF", -40.0));
    CHECK(src.setGainDb("LNA", 2.0));
    src.stop();
    src.closeDevice();
    return fake.calls;
}

}  // namespace

// Pushes `rate` samples a second for `seconds` through the fake service, reading them out as the pipeline does.
void deliver(FakeSdrPlayApi& fake, SdrPlaySource& src, double rate, double seconds) {
    const unsigned int kBlockN = 2016;
    std::vector<short> xi(kBlockN, 100);
    std::vector<short> xq(kBlockN, -100);
    std::vector<std::complex<float>> buf(1u << 16);
    const auto t0 = std::chrono::steady_clock::now();
    double sent = 0.0;
    for (;;) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed > seconds) { break; }
        while (sent < elapsed * rate) {
            fake.pushSamples(xi.data(), xq.data(), kBlockN);
            sent += kBlockN;
            while (src.read(buf.data(), buf.size()) == buf.size()) {}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    while (src.read(buf.data(), buf.size()) > 0) {}
}

// An output rate of the low-IF table whose plan decimates, so the ADC rate and the planned rate differ.
double decimatedLowIfRate(src_ns::SdrPlayRatePlan& plan) {
    for (const double r : src_ns::sdrPlaySupportedRatesForMode(src_ns::SdrPlayIfMode::LowIf)) {
        if (src_ns::sdrPlayRatePlanForMode(r, src_ns::SdrPlayIfMode::LowIf, plan) && plan.decM > 1) { return r; }
    }
    return 0.0;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_sdrplay_lowif\n");

    // A: THE DEFAULT PATH IS UNCHANGED
    {
        const std::vector<std::string> now = defaultPathCalls(false);
        CHECK(now == kGolden);
        if (now != kGolden) {
            std::printf("  the default path's calls differ from the recorded ones (%zu now, %zu recorded)\n", now.size(), kGolden.size());
            for (std::size_t i = 0; i < std::max(now.size(), kGolden.size()); ++i) {
                std::printf("    %zu: now [%s] recorded [%s]\n", i, i < now.size() ? now[i].c_str() : "-", i < kGolden.size() ? kGolden[i].c_str() : "-");
            }
        }
        CHECK(defaultPathCalls(true) == kGolden);   // touching the setting to say "off" changes nothing either
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(!src.lowIfChosen() && src.lowIfNote().empty());
        CHECK(src.supportedSampleRatesHz() == src_ns::sdrPlaySupportedRatesHz());
        CHECK(src.start());
        // a stream that delivers the wrong rate with the setting never touched is not a low-IF matter: no fallback, no note, no extra call
        const std::size_t before = fake.callCount();
        deliver(fake, src, 2000000.0, 1.3);
        CHECK(fake.callCount() == before && src.lowIfNote().empty() && !src.lowIfChosen());
        src.stop();
        src.closeDevice();
    }

    // B: the setting on: the plan is a listed low-IF combination, in ONE Update; off again restores today's plan
    {
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.setSampleRateHz(2000000.0));
        fake.calls.clear();
        CHECK(src.setLowIf(true));
        CHECK(src.lowIfChosen());
        CHECK(fake.callCount() <= 1);   // an open radio that is not streaming has its parameter block written and nothing sent; a streaming one gets ONE Update (section D and C)
        CHECK(fake.chA.tunerParams.ifType != abi::IF_Zero);
        src_ns::SdrPlayRatePlan want;
        CHECK(src_ns::sdrPlayRatePlanForMode(src.sampleRateHz(), src_ns::SdrPlayIfMode::LowIf, want));
        CHECK_NEAR(fake.devParams.fsFreq.fsHz, want.fsHz, 1.0);
        CHECK(fake.chA.tunerParams.ifType == want.ifType && fake.chA.tunerParams.bwType == want.bwType);
        // a rate set while on follows the low-IF table
        CHECK(src.setSampleRateHz(4000000.0));
        CHECK(fake.chA.tunerParams.ifType != abi::IF_Zero);
        CHECK_NEAR(src.sampleRateHz(), 4000000.0, 1.0);
        // off again: today's zero-IF plan, written back
        CHECK(src.setLowIf(false));
        CHECK(!src.lowIfChosen());
        CHECK(fake.chA.tunerParams.ifType == abi::IF_Zero);
        src_ns::SdrPlayRatePlan zero;
        CHECK(src_ns::sdrPlayRatePlan(src.sampleRateHz(), zero));
        CHECK_NEAR(fake.devParams.fsFreq.fsHz, zero.fsHz, 1.0);
        // no radio open: the choice is refused with a sentence
        src.closeDevice();
        CHECK(!src.setLowIf(true) && !src.lowIfChosen() && *src.lastError() != 0);
    }

    // C: the first-stream check
    {
        src_ns::SdrPlayRatePlan plan;
        const double rate = decimatedLowIfRate(plan);
        CHECK(rate > 0.0 && plan.decM > 1);
        std::printf("  low-IF test rate %.0f S/s from fs %.0f (decimation %u)\n", rate, plan.fsHz, plan.decM);
        // C1: the radio delivers the planned rate: low-IF stays, nothing is said
        {
            FakeSdrPlayApi fake;
            fake.addDevice("1811003EFB", abi::kRsp1A);
            SdrPlaySource src;
            src.setLowIfCheckSecondsForTest(0.6);
            CHECK(openOn(src, fake));
            CHECK(src.setLowIf(true));
            CHECK(src.setSampleRateHz(rate));
            CHECK(src.start());
            deliver(fake, src, rate, 1.4);
            CHECK(src.lowIfChosen() && src.lowIfNote().empty());
            CHECK(fake.chA.tunerParams.ifType != abi::IF_Zero);
            src.stop();
            src.closeDevice();
        }
        // C2: the radio delivers the ADC rate (the 0.99.43 field behaviour): back to zero-IF, written to the radio, said
        {
            FakeSdrPlayApi fake;
            fake.addDevice("1811003EFB", abi::kRsp1A);
            SdrPlaySource src;
            src.setLowIfCheckSecondsForTest(0.6);
            CHECK(openOn(src, fake));
            CHECK(src.setLowIf(true));
            CHECK(src.setSampleRateHz(rate));
            CHECK(src.start());
            fake.calls.clear();
            deliver(fake, src, plan.fsHz, 1.4);
            const std::string note = src.lowIfNote();
            std::printf("  fallback: %s\n", note.c_str());
            CHECK(!src.lowIfChosen());
            CHECK(note.find("Low-IF is switched off") != std::string::npos && note.find("zero-IF") != std::string::npos);
            CHECK(fake.chA.tunerParams.ifType == abi::IF_Zero);
            src_ns::SdrPlayRatePlan zero;
            CHECK(src_ns::sdrPlayRatePlan(src.sampleRateHz(), zero));
            CHECK_NEAR(fake.devParams.fsFreq.fsHz, zero.fsHz, 1.0);
            CHECK(fake.indexStarting("Update(") >= 0);   // the zero-IF plan was written by an Update
            // and the rate menu is today's again
            CHECK(src.supportedSampleRatesHz() == src_ns::sdrPlaySupportedRatesHz());
            src.stop();
            src.closeDevice();
        }
        // C2b: the same fall-back is a REPORT: the person switched low-IF on and the radio did not deliver it. Said once, once the stream has been checked, and not at all when low-IF holds.
        {
            ReportRig rig;
            FakeSdrPlayApi fake;
            fake.addDevice("1811003EFB", abi::kRsp1A);
            SdrPlaySource src;
            src.setLowIfCheckSecondsForTest(0.6);
            CHECK(openOn(src, fake));
            rr::registerSource(&src, rr::identify("sdrplay", "serial=1811003EFB"));
            CHECK(src.setLowIf(true));
            CHECK(src.setSampleRateHz(rate));
            CHECK(src.start());
            deliver(fake, src, rate, 1.4);   // low-IF holds: nothing said (the check measures the stream's own pace, so a machine too loaded to deliver it is a fall-back and IS said: the invariant is one report exactly when it fell back)
            CHECK(rig.fallBackReports() == (src.lowIfChosen() ? 0 : 1));
            src.stop();
            src.closeDevice();
            rr::forgetRadio(rr::identify("sdrplay", "serial=1811003EFB").key);
        }
        {
            ReportRig rig;
            FakeSdrPlayApi fake;
            fake.addDevice("1811003EFB", abi::kRsp1A);
            SdrPlaySource src;
            src.setLowIfCheckSecondsForTest(0.6);
            CHECK(openOn(src, fake));
            rr::registerSource(&src, rr::identify("sdrplay", "serial=1811003EFC"));
            CHECK(src.setLowIf(true));
            CHECK(src.setSampleRateHz(rate));
            CHECK(src.start());
            deliver(fake, src, plan.fsHz, 1.4);   // the ADC rate arrives: back to zero-IF
            CHECK(!src.lowIfChosen());
            CHECK(rig.fallBackReports() == 1);
            src.stop();
            src.closeDevice();
            rr::forgetRadio(rr::identify("sdrplay", "serial=1811003EFC").key);
        }
        // C3: some other rate also falls back
        {
            FakeSdrPlayApi fake;
            fake.addDevice("1811003EFB", abi::kRsp1A);
            SdrPlaySource src;
            src.setLowIfCheckSecondsForTest(0.6);
            CHECK(openOn(src, fake));
            CHECK(src.setLowIf(true));
            CHECK(src.setSampleRateHz(rate));
            CHECK(src.start());
            deliver(fake, src, rate * 0.5, 1.4);
            CHECK(!src.lowIfChosen() && !src.lowIfNote().empty());
            CHECK(fake.chA.tunerParams.ifType == abi::IF_Zero);
            src.stop();
            src.closeDevice();
        }
    }

    // D: a radio that refuses the low-IF Update stays where it was and the setting stays off
    {
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        const double fsBefore = fake.devParams.fsFreq.fsHz;
        const int ifBefore = static_cast<int>(fake.chA.tunerParams.ifType);
        fake.refuseUpdateAfter = 0;
        CHECK(!src.setLowIf(true));
        CHECK(!src.lowIfChosen());
        CHECK(fake.devParams.fsFreq.fsHz == fsBefore && static_cast<int>(fake.chA.tunerParams.ifType) == ifBefore);
        CHECK(!src.lowIfNote().empty());
        src.stop();
        src.closeDevice();
    }

    return testSummary("test_sdrplay_lowif");
}
