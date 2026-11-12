// test_sdrplay_ifmode_diag.cpp - R13 (SDRplay IF-mode choice), W4a (diagnostic report model) and
// W4c (service restart decision logic over an injected Service Control Manager). Nothing here
// touches the real SDRplay API or the real service: the ScmInterface is a fake and the real
// WindowsScm is only constructed (it makes no Windows call until open()).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "source/sdrplay_diag_report.hpp"
#include "source/sdrplay_if_mode.hpp"
#include "source/sdrplay_scm_restart.hpp"
#include "test_check.hpp"

using namespace cascade::source;
namespace abi = sdrplay_abi;

namespace {

// ---- R13 ---------------------------------------------------------------

void testZeroIfIsToday() {
    // The default must be today's behaviour: every supported rate, identical plan.
    for (const double r : sdrPlaySupportedRatesHz()) {
        SdrPlayRatePlan a, b;
        CHECK(sdrPlayRatePlan(r, a));
        CHECK(sdrPlayRatePlanForMode(r, SdrPlayIfMode::ZeroIf, b));
        CHECK(a.fsHz == b.fsHz && a.decM == b.decM && a.ifType == b.ifType && a.bwType == b.bwType &&
              a.decEnable == b.decEnable && a.wideBandSignal == b.wideBandSignal);
        CHECK(b.ifType == abi::IF_Zero);
    }
    SdrPlayRatePlan p;
    CHECK(!sdrPlayRatePlanForMode(12345.0, SdrPlayIfMode::ZeroIf, p));
    CHECK(sdrPlaySupportedRatesForMode(SdrPlayIfMode::ZeroIf) == sdrPlaySupportedRatesHz());
}

void testLowIfPlans() {
    const std::vector<double> rates = sdrPlaySupportedRatesForMode(SdrPlayIfMode::LowIf);
    CHECK(rates.size() == 9);
    for (std::size_t i = 1; i < rates.size(); ++i) { CHECK(rates[i - 1] < rates[i]); }
    for (const double r : rates) {
        SdrPlayRatePlan p;
        CHECK(sdrPlayRatePlanForMode(r, SdrPlayIfMode::LowIf, p));
        // The one arithmetic rule: output = fs / decimation, decimation enabled iff > 1.
        CHECK_NEAR(p.fsHz / p.decM, r, 0.5);
        CHECK((p.decM > 1) == (p.decEnable == 1));
        CHECK(p.ifType != abi::IF_Zero);
        // Only listed (fs, BW, IF) combinations.
        const bool listed =
            (p.fsHz == 2000000.0 && p.ifType == abi::IF_0_450 &&
             (p.bwType == abi::BW_0_200 || p.bwType == abi::BW_0_600)) ||
            (p.fsHz == 6000000.0 && p.ifType == abi::IF_1_620 && p.bwType == abi::BW_1_536) ||
            ((p.fsHz == 8000000.0 || p.fsHz == 8192000.0) && p.ifType == abi::IF_2_048 &&
             (p.bwType == abi::BW_1_536 || p.bwType == abi::BW_5_000));
        CHECK(listed);
    }
    SdrPlayRatePlan p;
    CHECK(!sdrPlayRatePlanForMode(5000000.0, SdrPlayIfMode::LowIf, p));  // no listed combination
    CHECK(p.fsHz == 0.0);
    CHECK(!sdrPlayRatePlanForMode(0.0, SdrPlayIfMode::LowIf, p));
    CHECK(!sdrPlayRatePlanForMode(-1.0, SdrPlayIfMode::LowIf, p));
    CHECK(!sdrPlayRatePlanForMode(96000.0, SdrPlayIfMode::LowIf, p));
}

void testVerdict() {
    SdrPlayRatePlan p;
    CHECK(sdrPlayRatePlanForMode(2000000.0, SdrPlayIfMode::LowIf, p));
    p.fsHz = 6000000.0;
    p.decM = 3;  // the retired 0.99.43 plan: 6 MHz ADC, expected 2 MS/s
    CHECK(sdrPlayLowIfRateVerdict(p, 2000000.0) == LowIfRateVerdict::AsPlanned);
    CHECK(sdrPlayLowIfRateVerdict(p, 2030000.0) == LowIfRateVerdict::AsPlanned);
    // The field log: 359978976 samples in a minute = 5.9996 MS/s.
    CHECK(sdrPlayLowIfRateVerdict(p, 359978976.0 / 60.0) == LowIfRateVerdict::DeliversAdcRate);
    CHECK(sdrPlayLowIfRateVerdict(p, 3000000.0) == LowIfRateVerdict::Other);
    CHECK(sdrPlayLowIfRateVerdict(p, 0.0) == LowIfRateVerdict::Other);
    CHECK(sdrPlayLowIfRateVerdict(p, -5.0) == LowIfRateVerdict::Other);
    // decM == 1: the ADC rate IS the planned rate, never "delivers ADC rate".
    SdrPlayRatePlan q = p;
    q.decM = 1;
    CHECK(sdrPlayLowIfRateVerdict(q, 6000000.0) == LowIfRateVerdict::AsPlanned);
    SdrPlayRatePlan z;  // default plan, fsHz 0
    CHECK(sdrPlayLowIfRateVerdict(z, 1.0e6) == LowIfRateVerdict::Other);
    CHECK(std::string(sdrPlayLowIfRateVerdictSentence(LowIfRateVerdict::DeliversAdcRate)).find("not usable") !=
          std::string::npos);
}

void testParseMode() {
    SdrPlayIfMode m = SdrPlayIfMode::ZeroIf;
    CHECK(parseSdrPlayIfMode("Low-IF", m) && m == SdrPlayIfMode::LowIf);
    CHECK(parseSdrPlayIfMode("ZERO", m) && m == SdrPlayIfMode::ZeroIf);
    m = SdrPlayIfMode::LowIf;
    CHECK(!parseSdrPlayIfMode("", m) && m == SdrPlayIfMode::LowIf);
    CHECK(!parseSdrPlayIfMode("medium", m) && m == SdrPlayIfMode::LowIf);
    CHECK(std::string(sdrPlayIfModeName(SdrPlayIfMode::ZeroIf)) == "zero-IF");
    // 10,000 pseudo-random strings, fixed seed: no crash, and only the four names parse.
    std::uint32_t s = 12345u;
    int parsed = 0;
    for (int i = 0; i < 10000; ++i) {
        std::string t;
        s = s * 1664525u + 1013904223u;
        const int len = static_cast<int>((s >> 24) % 12);
        for (int k = 0; k < len; ++k) {
            s = s * 1664525u + 1013904223u;
            t += static_cast<char>(s >> 24);
        }
        SdrPlayIfMode mm = SdrPlayIfMode::ZeroIf;
        if (parseSdrPlayIfMode(t, mm)) { ++parsed; }
        SdrPlayRatePlan p;
        sdrPlayRatePlanForMode(static_cast<double>(s % 20000000u), SdrPlayIfMode::LowIf, p);
    }
    CHECK(parsed < 50);
}

// ---- W4a ---------------------------------------------------------------

void testReport() {
    SdrPlayDiagReport r;
    CHECK(formatSdrPlayDiagReport(r) ==
          "SDRplay diagnostic report\nAPI: not loaded\nDevices: 0\nLast error: none\n");
    r.apiLoaded = true;
    r.apiVersion = 3.15f;
    r.apiPath = "C:\\Program Files\\SDRplay\\API\\x64\\sdrplay_api.dll";
    r.serviceSummary = "running";
    SdrPlayDiagDevice d;
    d.serial = "1234567890";
    d.hwVer = abi::kRspDuo;
    d.tuner = "Tuner A";
    d.mode = "Single tuner mode";
    d.selected = true;
    r.devices.push_back(d);
    SdrPlayDiagDevice e;
    e.serial = "12";
    e.hwVer = 99;
    r.devices.push_back(e);
    r.errorText = "sdrplay_api_Fail";
    r.lastStep = "Init";
    const std::string t = formatSdrPlayDiagReport(r);
    CHECK(t.find("API: loaded, version 3.15 (C:\\Program Files") != std::string::npos);
    CHECK(t.find("Service: running\n") != std::string::npos);
    CHECK(t.find("1. RSPduo  serial ...7890  [Tuner A] [Single tuner mode] (in use)\n") != std::string::npos);
    CHECK(t.find("2. unknown model (99)  serial ...\n") != std::string::npos);
    CHECK(t.find("1234567890") == std::string::npos);  // the serial is never printed whole
    CHECK(t.find("Last error: sdrplay_api_Fail (while: Init)\n") != std::string::npos);
    CHECK(sdrPlayDiagMaskSerial("") == "(none)");
    CHECK(sdrPlayDiagMaskSerial("abcd") == "...abcd");
    CHECK(sdrPlayDiagModelName(abi::kRsp1A) == "RSP1A");
    CHECK(sdrPlayDiagModelName(abi::kRspDxR2) == "RSPdx-R2");
}

// ---- W4c ---------------------------------------------------------------

struct FakeScm : ScmInterface {
    unsigned long openErr = 0, stopErr = 0, startErr = 0, queryErr = 0;
    ScmState state = ScmState::Running;
    int stopSettlesAfter = 1;   // queries before Stopped shows (-1: never)
    int startSettlesAfter = 1;  // queries before Running shows (-1: never)
    bool stopped = false, started = false;
    int queries = 0, opens = 0, closes = 0, stops = 0, starts = 0;
    std::chrono::milliseconds slept{0};
    unsigned long open(const std::string&) override { ++opens; return openErr; }
    unsigned long queryState(ScmState& s) override {
        ++queries;
        if (queryErr != 0) { return queryErr; }
        if (stopped && !started) {
            if (stopSettlesAfter >= 0 && --stopSettlesAfter < 0) { state = ScmState::Stopped; }
        } else if (started) {
            if (startSettlesAfter >= 0 && --startSettlesAfter < 0) { state = ScmState::Running; }
        }
        s = state;
        return 0;
    }
    unsigned long requestStop() override {
        ++stops;
        if (stopErr == 0) { stopped = true; state = ScmState::StopPending; }
        return stopErr;
    }
    unsigned long requestStart() override {
        ++starts;
        if (startErr == 0) { started = true; state = ScmState::StartPending; }
        return startErr;
    }
    void sleepFor(std::chrono::milliseconds ms) override { slept += ms; }
    void close() override { ++closes; }
};

void testRestart() {
    {  // happy path
        FakeScm f;
        const SdrPlayScmOutcome o = restartSdrPlayServiceVia(f, "SDRplayAPIService");
        CHECK(o.result == SdrPlayScmResult::Restarted);
        CHECK(f.stops == 1 && f.starts == 1 && f.closes >= 1);
    }
    {  // already stopped: no stop request
        FakeScm f;
        f.state = ScmState::Stopped;
        const SdrPlayScmOutcome o = restartSdrPlayServiceVia(f, "x");
        CHECK(o.result == SdrPlayScmResult::Restarted && f.stops == 0 && f.starts == 1);
    }
    {  // rights missing: a clear result, nothing else attempted
        FakeScm f;
        f.openErr = kScmErrorAccessDenied;
        const SdrPlayScmOutcome o = restartSdrPlayServiceVia(f, "x");
        CHECK(o.result == SdrPlayScmResult::AccessDenied && o.error == 5);
        CHECK(o.sentence.find("administrator") != std::string::npos);
        CHECK(f.stops == 0 && f.starts == 0);
    }
    {  // rights missing at the stop call
        FakeScm f;
        f.stopErr = kScmErrorAccessDenied;
        const SdrPlayScmOutcome o = restartSdrPlayServiceVia(f, "x");
        CHECK(o.result == SdrPlayScmResult::AccessDenied && f.starts == 0 && f.closes >= 1);
    }
    {  // not installed
        FakeScm f;
        f.openErr = kScmErrorServiceDoesNotExist;
        CHECK(restartSdrPlayServiceVia(f, "x").result == SdrPlayScmResult::NotInstalled);
    }
    {  // never stops: bounded, no real sleeping
        FakeScm f;
        f.stopSettlesAfter = -1;
        SdrPlayScmLimits lim;
        lim.stopWait = std::chrono::milliseconds(1000);
        const SdrPlayScmOutcome o = restartSdrPlayServiceVia(f, "x", lim);
        CHECK(o.result == SdrPlayScmResult::StopTimedOut && f.starts == 0);
        CHECK(f.slept.count() == 1000 && f.closes >= 1);
    }
    {  // never starts
        FakeScm f;
        f.startSettlesAfter = -1;
        SdrPlayScmLimits lim;
        lim.startWait = std::chrono::milliseconds(500);
        CHECK(restartSdrPlayServiceVia(f, "x", lim).result == SdrPlayScmResult::StartTimedOut);
    }
    {  // other errors carry the number
        FakeScm f;
        f.startErr = 1056;
        const SdrPlayScmOutcome o = restartSdrPlayServiceVia(f, "x");
        CHECK(o.result == SdrPlayScmResult::OtherError && o.error == 1056);
        CHECK(o.sentence.find("1056") != std::string::npos);
        FakeScm g;
        g.queryErr = 6;
        CHECK(restartSdrPlayServiceVia(g, "x").error == 6);
    }
    {  // the real interface is constructible without touching Windows
        WindowsScm w;
        w.close();
    }
}

}  // namespace

int main() {
    testZeroIfIsToday();
    testLowIfPlans();
    testVerdict();
    testParseMode();
    testReport();
    testRestart();
    return testSummary("test_sdrplay_ifmode_diag");
}
