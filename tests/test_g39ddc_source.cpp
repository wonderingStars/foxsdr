// test_g39ddc_source.cpp - the WiNRADiO G39DDC run-time bridge (source/g39ddc_source), proven against a STAND-IN G39DDCAPI.dll that this project builds
// from the maker's published document (tests/fixtures/g39ddc_standin.cpp).
//
// WHAT THIS CAN AND CANNOT PROVE. It proves what the bridge does with a library that behaves as the document says - find it, judge it, load it,
// call it in the documented order on ONE thread, tune so the centre is the centre, convert 16- and 32-bit I/Q, restart across a rate change - and
// what it does with one that does not: no library, not a library, the wrong library, a record of the wrong size, no receiver, a refused call, a
// silent stream, a disconnect, bad callback arguments, a call that crashes, a call that never returns. It cannot prove the document was read
// rightly or anything about a real receiver (UNVERIFIED ON HARDWARE). Generated signals carry noise, a static frequency error and a sample-clock
// error of 1500 ppm; the tone is found where the clock error puts it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "eol_fixture.hpp"
#include "source/g39ddc_source.hpp"
#include "source/vendor_lib.hpp"
#include "test_check.hpp"

using cascade::source::G39DdcSource;
using cascade::source::G39Tune;
using cascade::source::g39PlanTune;
namespace vl = cascade::source::vendorlib;

namespace {

enum Key : int {
    kMisbehave = 1, kToneHz = 2, kSpeed = 3, kSilentAfter = 4, kPpmX1000 = 5, kFe = 10, kDdc1 = 11, kDdcType = 12, kPower = 13, kStarts = 14,
    kStops = 15, kOpens = 16, kCloses = 17, kThreads = 18, kOverlap = 19, kPreamp = 20, kCallbackSet = 21, kCalls = 22, kNoiseX1000 = 23,
};
enum : std::int64_t {
    mNoDevices = 1 << 0, mOpenFails = 1 << 1, mStartFails = 1 << 2, mStartHangs = 1 << 3, mStartCrashes = 1 << 4, mBigRecord = 1 << 5,
    mDisconnected = 1 << 6, mBadChannel = 1 << 7, mBadBits = 1 << 8, mBadCount = 1 << 9, mTuneFails = 1 << 10, mSetDdcFails = 1 << 11,
};

struct Fx {
    vl::Library lib;
    void (*set)(int, std::int64_t) = nullptr;
    std::int64_t (*get)(int) = nullptr;
    void (*reset)() = nullptr;
    std::string dir;
    bool load() {
        dir = eol::u8(eol::fixtureDir());
        if (!lib.open(dir + "\\G39DDCAPI.dll").ok()) { return false; }
        set = reinterpret_cast<void (*)(int, std::int64_t)>(lib.symbol("eol_set"));
        get = reinterpret_cast<std::int64_t (*)(int)>(lib.symbol("eol_get"));
        reset = reinterpret_cast<void (*)()>(lib.symbol("eol_reset"));
        return set != nullptr && get != nullptr && reset != nullptr;
    }
};
Fx g_fx;

std::string args(const std::string& extra = "") { return "lib=" + g_fx.dir + (extra.empty() ? "" : "," + extra); }

// n samples within timeoutMs (fewer if it times out).
std::vector<std::complex<float>> collect(G39DdcSource& s, std::size_t n, int timeoutMs = 8000) {
    std::vector<std::complex<float>> out;
    std::vector<std::complex<float>> tmp(8192);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t got = s.read(tmp.data(), std::min(tmp.size(), n - out.size()));
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

// The tone's frequency as the mean phase step between neighbouring samples, at the nominal sample rate the source reports.
double toneHz(const std::vector<std::complex<float>>& x, double rate) {
    std::complex<double> acc = 0;
    for (std::size_t i = 1; i < x.size(); ++i) { acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1])); }
    return std::arg(acc) * rate / (2.0 * 3.14159265358979323846);
}
double meanMag(const std::vector<std::complex<float>>& x) {
    double m = 0;
    for (const auto& v : x) { m += std::abs(v); }
    return x.empty() ? 0 : m / static_cast<double>(x.size());
}
// What the stand-in's tone should measure: (tone + 12.3 Hz static error - centre), seen through a sample clock that runs 1500 ppm fast.
double expectedHz(double toneAbs, double centre) { return (toneAbs + 12.3 - centre) / (1.0 + 1500e-6); }

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// -------------------------------------------------------------------------------------------------------------------------------------------------

void testPlanTune() {
    G39Tune t;
    std::string why;
    // The nearest multiple of the step; the rest on DDC1.
    CHECK(g39PlanTune(7013450.0, 100000, 30000000, 10000, 2000000, 150000, t, why));
    CHECK(t.frontEndHz == 7010000 && t.ddc1Hz == 3450);
    CHECK(g39PlanTune(7016000.0, 100000, 30000000, 10000, 2000000, 150000, t, why));
    CHECK(t.frontEndHz == 7020000 && t.ddc1Hz == -4000);
    // Exactly on the minimum (which need not be a multiple of the step), below it (within the window), and at the top.
    CHECK(g39PlanTune(100000.0, 100000, 30000000, 30000, 2000000, 150000, t, why));
    CHECK(t.frontEndHz == 100000 && t.ddc1Hz == 0);
    CHECK(g39PlanTune(100000.0, 100000, 30000000, 30000, 2000000, 150000, t, why));
    CHECK(g39PlanTune(50000.0, 100000, 30000000, 10000, 2000000, 150000, t, why));
    CHECK(t.frontEndHz == 100000 && t.ddc1Hz == -50000);
    CHECK(g39PlanTune(30000000.0, 100000, 30000000, 10000, 2000000, 150000, t, why));
    CHECK(t.frontEndHz == 30000000 && t.ddc1Hz == 0);
    CHECK(g39PlanTune(30500000.0, 100000, 30000000, 10000, 2000000, 150000, t, why));
    CHECK(t.frontEndHz == 30000000 && t.ddc1Hz == 500000);
    // Too far outside the range for the window to cover: refused, with the range in the sentence.
    CHECK(!g39PlanTune(50.0e6, 100000, 30000000, 10000, 2000000, 150000, t, why));
    CHECK(contains(why, "0.100 to 30.000 MHz"));
    CHECK(!g39PlanTune(-1.0, 100000, 30000000, 10000, 2000000, 150000, t, why));
    CHECK(!g39PlanTune(std::nan(""), 100000, 30000000, 10000, 2000000, 150000, t, why));
    CHECK(!g39PlanTune(INFINITY, 100000, 30000000, 10000, 2000000, 150000, t, why));
    // A step of zero is treated as one; a window of zero means "no limit stated".
    CHECK(g39PlanTune(7000123.0, 100000, 30000000, 0, 0, 0, t, why));
    CHECK(t.frontEndHz == 7000123 && t.ddc1Hz == 0);
    // A step so coarse that no multiple lies within the DDC's reach: refused rather than tuned to the wrong place.
    CHECK(!g39PlanTune(7500000.0, 100000, 30000000, 5000000, 2000000, 1900000, t, why));
    // Boundary: the offset may be exactly window/2 - bandwidth/2, not one more.
    CHECK(g39PlanTune(7000000.0 + 425000.0, 100000, 30000000, 1000000, 2000000, 150000, t, why));
    CHECK(t.ddc1Hz == 425000 || std::llabs(t.ddc1Hz) <= 925000);
    // Property: whatever is accepted reproduces the wish exactly.
    for (double f = 100000.0; f < 30.0e6; f += 123457.0) {
        if (g39PlanTune(f, 100000, 30000000, 10000, 2000000, 150000, t, why)) {
            CHECK(static_cast<double>(t.frontEndHz) + t.ddc1Hz == std::llround(f));
            CHECK(t.frontEndHz == 100000 || t.frontEndHz % 10000 == 0);
            CHECK(std::llabs(t.ddc1Hz) <= 1000000 - 75000);
        } else {
            CHECK(false);
        }
    }
}

void testNothingAtStart() {
    const std::uint64_t loads = vl::Library::silentLoadCount();
    {
        G39DdcSource s;  // constructing and destroying one loads nothing and starts nothing
        CHECK(!s.isOpen());
        CHECK(!s.running());
        CHECK(!s.deviceDead());
        CHECK(std::string(s.lastError()).empty());
        std::complex<float> x[16];
        CHECK(s.read(x, 16) == 0);
        CHECK(!s.start());
        CHECK(!s.setCenterFrequencyHz(7e6));
        CHECK(!s.setSampleRateHz(48000));
        s.stop();
        s.closeDevice();
    }
    CHECK(vl::Library::silentLoadCount() == loads);
}

void testOpenRefusals() {
    const auto dir = eol::scratch("g39ref");
    const auto fx = eol::fixtureDir();
    // No library in the folder.
    {
        G39DdcSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "not-found");
        CHECK(contains(s.lastError(), "G39DDCAPI.dll"));
        CHECK(!contains(s.lastError(), "eol-g39ref"));
        CHECK(!s.isOpen());
    }
    // A text file with the library's name.
    {
        eol::writeFile(dir / "G39DDCAPI.dll", std::vector<std::uint8_t>(3000, 'x'));
        G39DdcSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "not-an-image");
        CHECK(contains(s.lastError(), "not a Windows library"));
    }
    // A library that exports only one of the calls: the sentence names what is missing.
    {
        std::filesystem::copy_file(fx / "eol_partial.dll", dir / "G39DDCAPI.dll", std::filesystem::copy_options::overwrite_existing);
        G39DdcSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "symbol-missing");
        CHECK(contains(s.lastError(), "OpenDevice"));
        CHECK(contains(s.lastError(), "StartDDC1"));
        CHECK(!contains(s.lastError(), "GetDeviceList,"));  // the one it does export is not listed as missing
    }
    // A library whose own dependency is missing.
    {
        std::filesystem::copy_file(fx / "eol_needs_dep.dll", dir / "G39DDCAPI.dll", std::filesystem::copy_options::overwrite_existing);
        G39DdcSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "load-failed");
        CHECK(contains(s.lastError(), "126"));
    }
    // Settings that are not valid.
    for (const char* bad : {"channel=2", "channel=x", "rate=abc", "rate=5", "timeout=1", "silence=0", "timeout=99999999999"}) {
        G39DdcSource s;
        CHECK(!s.open(args(bad)));
        CHECK(contains(s.lastError(), "not valid"));
        CHECK(!s.isOpen());
    }
    // Refusals from the receiver side.
    struct Case {
        std::int64_t mis;
        const char* extra;
        const char* verdict;
        const char* sentence;
    };
    const Case cases[] = {
        {mNoDevices, "", "no-device", "no WiNRADiO G39DDC receiver is connected"},
        {mBigRecord, "", "no-device", ""},
        {mOpenFails, "", "open-failed", "could not be opened"},
        {mSetDdcFails, "", "open-failed", "refused the sample rate"},
        {mTuneFails, "", "tune-failed", "front-end frequency"},
        {0, "serial=NOPE", "no-device", "serial number you gave"},
    };
    for (const Case& c : cases) {
        g_fx.reset();
        g_fx.set(kMisbehave, c.mis);
        G39DdcSource s;
        const bool ok = s.open(args(c.extra));
        CHECK(!ok);
        CHECK(!s.isOpen());
        CHECK(!std::string(s.lastError()).empty());
        CHECK(s.openVerdictWord() == c.verdict);
        if (c.sentence[0] != '\0') { CHECK(contains(s.lastError(), c.sentence)); }
        if (c.mis == mBigRecord) {
            CHECK(contains(s.lastError(), "316 bytes"));
            CHECK(contains(s.lastError(), "315"));
            CHECK(contains(s.lastError(), "will not read it"));
        }
        // Whatever failed, no receiver handle is left open: opens and closes balance.
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
        CHECK(!s.deviceDead());
        // The source is reusable after a refusal.
        g_fx.set(kMisbehave, 0);
        CHECK(s.open(args()));
        CHECK(s.isOpen());
        s.closeDevice();
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    g_fx.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testHappyPath() {
    g_fx.reset();
    G39DdcSource s;
    CHECK(s.open(args()));
    CHECK(s.isOpen());
    CHECK(std::string(s.driverKey()) == "g39ddc");
    CHECK(s.selfPaced());
    // The DDC types the receiver offers: the one with rate 0 and the 24-bit one are not offered.
    const std::vector<double> rates = s.supportedSampleRatesHz();
    CHECK((rates == std::vector<double>{48000, 96000, 192000, 384000, 768000}));
    CHECK(s.sampleRateHz() == 192000.0);  // nearest to 192 kS/s by default
    double lo = 0, hi = 0;
    CHECK(s.frequencyRangeHz(lo, hi));
    CHECK(lo == 0.0 || lo < 100000.0);
    CHECK(hi > 30.0e6);
    CHECK(s.serialMasked() == "****0001");
    CHECK(!contains(s.lastError(), "ST000001"));
    CHECK(g_fx.get(kPower) == 1);
    CHECK(g_fx.get(kCallbackSet) == 1);
    CHECK(s.centerFrequencyHz() == 10.0e6);
    CHECK(g_fx.get(kFe) == 10000000);

    // Samples flow only after start().
    std::complex<float> junk[64];
    CHECK(s.read(junk, 64) == 0);
    CHECK(s.start());
    CHECK(s.running());
    CHECK(s.start());  // idempotent

    // Tuned 5 kHz below the tone: the tone appears at +5 kHz less the clock error.
    CHECK(s.setCenterFrequencyHz(7010000.0));
    CHECK(g_fx.get(kFe) == 7010000 && g_fx.get(kDdc1) == 0);
    {
        collect(s, 20000);  // let the retune pass through the stream
        const auto x = collect(s, 600000);
        CHECK(x.size() == 600000);
        CHECK_NEAR(toneHz(x, s.sampleRateHz()), expectedHz(7015000, 7010000), 2.5);
        CHECK_NEAR(meanMag(x), 0.5, 0.03);
        // The clock error is in the data: ignoring it would put the tone 7 Hz elsewhere.
        CHECK(std::fabs(toneHz(x, s.sampleRateHz()) - (7015000 + 12.3 - 7010000)) > 4.0);
    }
    // Retuned above the tone while running; an odd centre goes to the front-end step and the remainder to DDC1.
    CHECK(s.setCenterFrequencyHz(7013450.0));
    CHECK(g_fx.get(kFe) == 7010000 && g_fx.get(kDdc1) == 3450);
    {
        collect(s, 30000);
        const auto x = collect(s, 600000);
        CHECK_NEAR(toneHz(x, s.sampleRateHz()), expectedHz(7015000, 7013450), 2.5);
    }
    CHECK(s.centerFrequencyHz() == 7013450.0);
    // Out of range: refused with the range in the sentence, still tuned where it was.
    CHECK(!s.setCenterFrequencyHz(60.0e6));
    CHECK(contains(s.lastError(), "MHz"));
    CHECK(s.centerFrequencyHz() == 7013450.0);
    CHECK(!s.setCenterFrequencyHz(-5.0));

    // A rate change while running: stop, SetDDC1, start; a 32-bit type this time; the tone is still found.
    CHECK(s.setSampleRateHz(384000.0));
    CHECK(s.sampleRateHz() == 384000.0);
    CHECK(g_fx.get(kDdcType) == 3);
    CHECK(s.running());
    {
        collect(s, 50000);
        const auto x = collect(s, 900000);
        CHECK(x.size() == 900000);
        CHECK_NEAR(toneHz(x, s.sampleRateHz()), expectedHz(7015000, 7013450), 3.5);
        CHECK_NEAR(meanMag(x), 0.5, 0.03);
    }
    // A rate nobody offers goes to the nearest one that is offered.
    CHECK(s.setSampleRateHz(200000.0));
    CHECK(s.sampleRateHz() == 192000.0);
    CHECK(!s.setSampleRateHz(0.0));
    CHECK(!s.setSampleRateHz(-3.0));
    CHECK(!s.setSampleRateHz(std::nan("")));
    // The preamplifier is the one gain.
    {
        const auto g = s.gains();
        CHECK(g.size() == 1 && g[0].name == "Preamp");
        CHECK(s.setGainDb("Preamp", 1.0));
        CHECK(g_fx.get(kPreamp) == 1 && s.gainDb("Preamp") == 1.0);
        CHECK(s.setGainDb("Preamp", 0.0));
        CHECK(g_fx.get(kPreamp) == 0);
        CHECK(!s.setGainDb("LNA", 3.0));
        CHECK(!s.autoGainSupported());
        CHECK(s.antennas() == std::vector<std::string>{"RF"});
    }
    const auto st = s.stats();
    CHECK(st.callbacks > 100);
    CHECK(st.samplePairs > 1500000);
    CHECK(st.badCallbacks == 0);
    CHECK(st.pairsDropped == 0);
    s.stop();
    CHECK(!s.running());
    CHECK(g_fx.get(kStops) >= 1);
    // THE ONE-THREAD RULE: every call the library saw came from one thread, never two at once.
    CHECK(g_fx.get(kThreads) == 1);
    CHECK(g_fx.get(kOverlap) == 1);
    s.closeDevice();
    CHECK(!s.isOpen());
    CHECK(g_fx.get(kOpens) == 1 && g_fx.get(kCloses) == 1);
    CHECK(g_fx.get(kPower) == 0);  // closing powers the receiver down
    CHECK(g_fx.get(kCallbackSet) == 0);
    g_fx.reset();
}

void testSecondChannelAndRate() {
    g_fx.reset();
    G39DdcSource s;
    CHECK(s.open(args("channel=1,rate=48000")));
    CHECK(s.sampleRateHz() == 48000.0);
    CHECK(s.start());
    CHECK(s.setCenterFrequencyHz(7015000.0 - 3000.0));
    collect(s, 10000);
    const auto x = collect(s, 200000);
    CHECK(x.size() == 200000);
    CHECK_NEAR(toneHz(x, 48000.0), expectedHz(7015000, 7012000), 4.0);
    s.closeDevice();
    g_fx.reset();
}

void testConcurrentCallers() {
    g_fx.reset();
    G39DdcSource s;
    CHECK(s.open(args()));
    CHECK(s.start());
    std::atomic<bool> go{true};
    std::atomic<int> failures{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t) {
        th.emplace_back([&, t] {
            for (int i = 0; i < 150; ++i) {
                bool ok = true;
                switch ((i + t) % 3) {
                    case 0: ok = s.setCenterFrequencyHz(7000000.0 + 1000.0 * ((i * 7 + t) % 50)); break;
                    case 1: ok = s.setGainDb("Preamp", (i % 2) ? 1.0 : 0.0); break;
                    default: ok = s.sampleRateHz() > 0; break;
                }
                if (!ok) { ++failures; }
            }
        });
    }
    std::thread reader([&] {
        std::complex<float> x[2048];
        while (go.load()) { s.read(x, 2048); }
    });
    for (auto& t : th) { t.join(); }
    go.store(false);
    reader.join();
    CHECK(failures.load() == 0);
    CHECK(g_fx.get(kThreads) == 1);
    CHECK(g_fx.get(kOverlap) == 1);
    CHECK(g_fx.get(kCalls) > 400);
    s.closeDevice();
    g_fx.reset();
}

void testStartStopAndReopen() {
    g_fx.reset();
    g_fx.set(kSpeed, 200);
    G39DdcSource s;
    CHECK(s.open(args()));
    for (int i = 0; i < 40; ++i) {
        CHECK(s.start());
        std::complex<float> x[512];
        std::size_t got = 0;
        for (int tries = 0; tries < 200 && got == 0; ++tries) { got = s.read(x, 512); }
        CHECK(got > 0);
        s.stop();
        CHECK(!s.running());
    }
    CHECK(g_fx.get(kStarts) == 40);
    s.closeDevice();
    for (int i = 0; i < 30; ++i) {
        G39DdcSource t;
        CHECK(t.open(args()));
        CHECK(t.start());
        t.closeDevice();  // close while running
        CHECK(!t.isOpen());
    }
    CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    CHECK(g_fx.get(kOpens) == 31);
    g_fx.reset();
}

void testStreamFaults() {
    // A stream that goes quiet after three buffers is a fault within the silence bound, and the sentence says so.
    {
        g_fx.reset();
        g_fx.set(kSilentAfter, 3);
        G39DdcSource s;
        CHECK(s.open(args("silence=600")));
        CHECK(s.start());
        const auto t0 = std::chrono::steady_clock::now();
        std::complex<float> x[4096];
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) { s.read(x, 4096); }
        CHECK(s.faulted());
        CHECK(s.deviceDead());
        CHECK(contains(s.lastError(), "stopped delivering"));
        CHECK(s.faultedWhile() == "streaming");
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
        CHECK(s.read(x, 4096) == 0);  // a dead receiver delivers nothing more
        s.closeDevice();
        g_fx.reset();
    }
    // A receiver that is unplugged: the library says so (IsDeviceConnected zero with no error) and the sentence says so.
    {
        g_fx.reset();
        g_fx.set(kSilentAfter, 2);
        G39DdcSource s;
        CHECK(s.open(args("silence=30000")));
        CHECK(s.start());
        g_fx.set(kMisbehave, mDisconnected);
        const auto t0 = std::chrono::steady_clock::now();
        std::complex<float> x[4096];
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) { s.read(x, 4096); }
        CHECK(s.faulted());
        CHECK(contains(s.lastError(), "disconnected"));
        s.closeDevice();
        g_fx.reset();
    }
    // Callbacks with impossible arguments are dropped and counted; they never become samples; a stream of nothing else is silent.
    for (const std::int64_t bad : {mBadChannel, mBadBits, mBadCount}) {
        g_fx.reset();
        g_fx.set(kMisbehave, bad);
        G39DdcSource s;
        CHECK(s.open(args("silence=500")));
        CHECK(s.start());
        const auto t0 = std::chrono::steady_clock::now();
        std::complex<float> x[4096];
        std::size_t total = 0;
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) { total += s.read(x, 4096); }
        CHECK(total == 0);
        CHECK(s.stats().badCallbacks > 5);
        CHECK(s.stats().samplePairs == 0);
        CHECK(s.faulted());
        s.closeDevice();
        g_fx.reset();
    }
    // The library refuses to start: a sentence with the Windows error, and the source is still usable once the cause is gone.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartFails);
        G39DdcSource s;
        CHECK(s.open(args()));
        CHECK(!s.start());
        CHECK(!s.running());
        CHECK(contains(s.lastError(), "would not start streaming"));
        CHECK(contains(s.lastError(), "Windows error"));
        CHECK(!s.deviceDead());
        g_fx.set(kMisbehave, 0);
        CHECK(s.start());
        CHECK(collect(s, 1000).size() == 1000);
        s.closeDevice();
        g_fx.reset();
    }
}

void testContainment() {
    // A crash INSIDE the library's code: the bridge reports it and the process carries on.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartCrashes);
        G39DdcSource s;
        CHECK(s.open(args()));
        CHECK(!s.start());
        CHECK(s.deviceDead());
        CHECK(contains(s.lastError(), "crashed"));
        CHECK(s.faultedWhile() == "starting the stream");
        CHECK(!s.running());
        CHECK(!s.setCenterFrequencyHz(7e6));  // nothing more is sent to a library that has crashed
        s.closeDevice();
        g_fx.reset();
        G39DdcSource again;  // the process is alive and a fresh source opens
        CHECK(again.open(args()));
        again.closeDevice();
    }
    // A call that never returns (3 s in the stand-in; the bound here is 300 ms): the source gives up, says so, and closes within a bound.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartHangs);
        G39DdcSource s;
        CHECK(s.open(args("timeout=300")));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.start());
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500));
        CHECK(s.deviceDead());
        CHECK(contains(s.lastError(), "did not answer"));
        CHECK(contains(s.lastError(), "restart"));
        CHECK(s.faultedWhile() == "starting the stream");
        const auto t1 = std::chrono::steady_clock::now();
        s.closeDevice();
        CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::seconds(4));
        CHECK(!s.isOpen());
        CHECK(s.deviceDead());
    }
}

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Nothing in the new files writes to the console or the error stream (the host captures both into its log and its reports), and no sentence is built
// from a full serial number.
void testSinks() {
    const std::string root = EOL_SRC_DIR;
    for (const char* f : {"src/source/g39ddc_source.cpp", "src/source/g39ddc_source.hpp", "src/source/g39ddc_api.hpp", "src/source/vendor_lib.cpp",
                          "src/source/vendor_lib.hpp"}) {
        const std::string t = readText(root + "/" + f);
        CHECK(!t.empty());
        CHECK(!contains(t, "stderr"));
        CHECK(!contains(t, "stdout"));
        CHECK(!contains(t, "std::cerr"));
        CHECK(!contains(t, "std::cout"));
        CHECK(!contains(t, "std::clog"));
        CHECK(!contains(t, " printf("));
        CHECK(!contains(t, "puts("));
        CHECK(!contains(t, "OutputDebugString"));
        CHECK(!contains(t, "settings_set"));
    }
}

}  // namespace

int main() {
    if (!g_fx.load()) {
        std::printf("FAIL cannot load the stand-in library\n");
        return 1;
    }
    testPlanTune();
    testNothingAtStart();
    testOpenRefusals();
    testHappyPath();
    testSecondChannelAndRate();
    testConcurrentCallers();
    testStartStopAndReopen();
    testStreamFaults();
    testSinks();
    testContainment();  // last: it leaves a library call in flight on purpose
    return testSummary("test_g39ddc_source");
}
