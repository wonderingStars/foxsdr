// test_fobos_source.cpp - the Fobos SDR run-time bridge (source/fobos_source), proven against a STAND-IN library that this project builds from the maker's
// public header (tests/fixtures/fobos_standin.cpp).
//
// What it can and cannot prove: it proves what the bridge does with a library that behaves as the header says and with ones that do not (no library, not a
// library, the wrong library, a list or rate table that overruns its buffer, no radio, a refused call, a silent stream, an unplug mid-stream, a callback with
// an impossible length or a null buffer, a crash inside the library, a call that never returns, callbacks after the stop); it cannot prove the header was
// read rightly, and nothing here has touched a real radio. STAND-IN PROOF ONLY; REAL RADIO UNVERIFIED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "eol_fixture.hpp"
#include "source/fobos_api.hpp"
#include "source/fobos_source.hpp"
#include "source/vendor_lib.hpp"
#include "test_check.hpp"

using cascade::source::FobosSource;
namespace vl = cascade::source::vendorlib;
namespace fb = cascade::source::fobos;

namespace {

enum Key : int {
    kMisbehave = 1, kToneHz = 2, kSpeed = 3, kSilentAfter = 4, kPpmX1000 = 5, kUnplugAfter = 6, kFreq = 10, kRate = 11, kLna = 12, kVga = 13, kDirect = 14,
    kClock = 15, kStarts = 16, kCancels = 17, kOpens = 18, kCloses = 19, kThreads = 20, kOverlap = 21, kCalls = 22, kNoiseX1000 = 23, kForbiddenCalls = 24,
    kLenPairs = 25, kLateCallbacks = 26, kStreamLen = 27, kBufCount = 28, kListed = 29, kNonBlocking = 30,
};
enum : std::int64_t {
    mNoDevices = 1 << 0, mOpenFails = 1 << 1, mStartFails = 1 << 2, mStartHangs = 1 << 3, mStartCrashes = 1 << 4, mListOverrun = 1 << 5, mInfoOverrun = 1 << 6,
    mRatesOverrun = 1 << 7, mBadLength = 1 << 8, mZeroLength = 1 << 9, mOddLength = 1 << 10, mNullBuffer = 1 << 11, mTuneFails = 1 << 12, mRateFails = 1 << 13,
    mGainFails = 1 << 14, mLateAfterCancel = 1 << 15, mCancelHangs = 1 << 16, mRatesNone = 1 << 17, mCountHuge = 1 << 18, mTuneCrashes = 1 << 19,
    mSerialsEmpty = 1 << 20,
};

struct Fx {
    vl::Library lib;
    void (*set)(int, std::int64_t) = nullptr;
    std::int64_t (*get)(int) = nullptr;
    void (*reset)() = nullptr;
    std::string dir;
    bool load() {
        dir = eol::u8(eol::fixtureDir());
        if (!lib.open(dir + "\\fobos.dll").ok()) { return false; }
        set = reinterpret_cast<void (*)(int, std::int64_t)>(lib.symbol("eol_set"));
        get = reinterpret_cast<std::int64_t (*)(int)>(lib.symbol("eol_get"));
        reset = reinterpret_cast<void (*)()>(lib.symbol("eol_reset"));
        return set != nullptr && get != nullptr && reset != nullptr;
    }
};
Fx g_fx;

std::string args(const std::string& extra = "") { return "lib=" + g_fx.dir + (extra.empty() ? "" : "," + extra); }

std::vector<std::complex<float>> collect(FobosSource& s, std::size_t n, int timeoutMs = 8000) {
    std::vector<std::complex<float>> out;
    std::vector<std::complex<float>> tmp(8192);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t got = s.read(tmp.data(), std::min(tmp.size(), n - out.size()));
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}
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
double expectedHz(double toneAbs, double centre) { return (toneAbs + 12.3 - centre) / (1.0 + 1500e-6); }
bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void testNothingAtStart() {
    const std::uint64_t loads = vl::Library::silentLoadCount();
    {
        FobosSource s;
        CHECK(!s.isOpen() && !s.running() && !s.deviceDead());
        CHECK(std::string(s.lastError()).empty());
        std::complex<float> x[16];
        CHECK(s.read(x, 16) == 0);
        CHECK(!s.start());
        CHECK(!s.setCenterFrequencyHz(100e6));
        CHECK(!s.setSampleRateHz(2e6));
        CHECK(!s.setGainDb("LNA", 1.0));
        s.stop();
        s.closeDevice();
    }
    CHECK(vl::Library::silentLoadCount() == loads);
}

void testOpenRefusals() {
    const auto dir = eol::scratch("fobref");
    const auto fx = eol::fixtureDir();
    // No library in the folder: a plain sentence that says what is needed, that the person installs it, and that nothing is downloaded.
    {
        FobosSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "not-found");
        const std::string e = s.lastError();
        CHECK(contains(e.c_str(), "fobos.dll") && contains(e.c_str(), "libfobos.dll"));
        CHECK(contains(e.c_str(), "you install yourself") && contains(e.c_str(), "does not include or download"));
        CHECK(!contains(e.c_str(), "eol-fobref"));   // never the folder: it holds a user name
        CHECK(!s.isOpen() && !s.deviceDead());
    }
    // The second name is tried: the same stand-in under the other file name is found and opens.
    {
        std::filesystem::copy_file(fx / "fobos.dll", dir / "libfobos.dll", std::filesystem::copy_options::overwrite_existing);
        g_fx.reset();
        FobosSource s;
        CHECK(s.open("lib=" + eol::u8(dir)));
        s.closeDevice();
        std::filesystem::remove(dir / "libfobos.dll");
    }
    {
        eol::writeFile(dir / "fobos.dll", std::vector<std::uint8_t>(3000, 'x'));
        FobosSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "not-an-image");
    }
    // A library that exports one of the calls and not the rest: named, call by call.
    {
        std::filesystem::copy_file(fx / "eol_partial.dll", dir / "fobos.dll", std::filesystem::copy_options::overwrite_existing);
        FobosSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "symbol-missing");
        CHECK(contains(s.lastError(), "fobos_rx_open") && contains(s.lastError(), "fobos_rx_read_async"));
        CHECK(!s.isOpen() && !s.deviceDead());
    }
    {
        std::filesystem::copy_file(fx / "eol_needs_dep.dll", dir / "fobos.dll", std::filesystem::copy_options::overwrite_existing);
        FobosSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "load-failed");
        CHECK(contains(s.lastError(), "126"));
    }
    for (const char* bad : {"rate=abc", "rate=5", "timeout=1", "silence=0", "index=-3", "count=both", "direct=maybe", "clock=sideways"}) {
        FobosSource s;
        CHECK(!s.open(args(bad)));
        CHECK(contains(s.lastError(), "not valid"));
    }
    struct Case {
        std::int64_t mis;
        const char* extra;
        const char* verdict;
        const char* sentence;
    };
    const Case cases[] = {
        {mNoDevices, "", "no-device", "no Fobos SDR is connected"},
        {mCountHuge, "", "no-device", "impossible number of radios"},
        {mListOverrun, "", "no-device", "wrote past the end of the radio list"},
        {mInfoOverrun, "", "no-device", "board information"},
        {mRatesOverrun, "", "no-device", "wrote past the end of the sample-rate list"},
        {mRatesNone, "", "no-device", "no sample rate this build can use"},
        {mSerialsEmpty, "", "no-device", "listed no serial number"},
        {mOpenFails, "", "no-device", "could not be opened"},
        {mRateFails, "", "no-device", "refused the sample rate"},
        {0, "serial=NOPE", "no-device", "serial number you gave"},
        {0, "index=5", "no-device", "no Fobos SDR at that position"},
    };
    for (const Case& c : cases) {
        g_fx.reset();
        g_fx.set(kMisbehave, c.mis);
        FobosSource s;
        CHECK(!s.open(args(c.extra)));
        CHECK(!s.isOpen());
        CHECK(s.openVerdictWord() == c.verdict);
        CHECK(contains(s.lastError(), c.sentence));
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
        CHECK(!s.deviceDead());
        g_fx.set(kMisbehave, 0);
        CHECK(s.open(args()));
        s.closeDevice();
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    // A radio whose tuning range the header does not state: a refusal of the first tuning is not an open failure; later ones are refused with a sentence.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mTuneFails);
        FobosSource s;
        CHECK(s.open(args()));
        CHECK(s.isOpen());
        CHECK(std::string(s.lastError()).empty());
        CHECK(!s.setCenterFrequencyHz(7e6));
        CHECK(contains(s.lastError(), "refused that frequency") && contains(s.lastError(), "unsupported"));
        s.closeDevice();
    }
    // The two optional settings are sent only when asked for, and a refused or missing one is a refusal of the open.
    {
        g_fx.reset();
        FobosSource s;
        CHECK(s.open(args("direct=on,clock=external")));
        CHECK(g_fx.get(kDirect) == 1 && g_fx.get(kClock) == 1);
        s.closeDevice();
        g_fx.reset();
        FobosSource t;
        CHECK(t.open(args()));
        CHECK(g_fx.get(kDirect) == 0 && g_fx.get(kClock) == 0);   // never touched
        t.closeDevice();
    }
    g_fx.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testHappyPath() {
    g_fx.reset();
    FobosSource s;
    CHECK(s.open(args()));
    CHECK(std::string(s.driverKey()) == "fobos");
    CHECK((s.supportedSampleRatesHz() == std::vector<double>{1e6, 2e6, 5e6, 10e6, 25e6, 50e6}));
    CHECK(s.sampleRateHz() == 2e6);
    CHECK(g_fx.get(kRate) == 2000000);
    double lo = 0, hi = 0;
    CHECK(!s.frequencyRangeHz(lo, hi));   // the header gives none
    CHECK(s.serialMasked() == "****5001");
    CHECK(s.centerFrequencyHz() == 100.0e6);
    CHECK(g_fx.get(kFreq) == 100000000);
    std::complex<float> junk[64];
    CHECK(s.read(junk, 64) == 0);
    CHECK(s.start());
    CHECK(s.running());
    CHECK(s.start());
    CHECK(g_fx.get(kStarts) == 1);
    CHECK(g_fx.get(kStreamLen) == 16384 && g_fx.get(kBufCount) == 8);   // what the library was asked for
    CHECK(s.setCenterFrequencyHz(7010000.0));
    CHECK(g_fx.get(kFreq) == 7010000);
    {
        collect(s, 20000);
        const auto x = collect(s, 600000);
        CHECK(x.size() == 600000);
        CHECK_NEAR(toneHz(x, s.sampleRateHz()), expectedHz(7015000, 7010000), 3.0);
        CHECK_NEAR(meanMag(x), 0.5, 0.03);
    }
    // A frequency the library rounds: the centre reported is what the library said it tuned to.
    CHECK(s.setCenterFrequencyHz(7013450.4));
    CHECK(g_fx.get(kFreq) == 7013000);
    CHECK(s.centerFrequencyHz() == 7013000.0);
    CHECK(!s.setCenterFrequencyHz(10.0));   // the library refuses
    CHECK(contains(s.lastError(), "refused that frequency"));
    CHECK(s.centerFrequencyHz() == 7013000.0);
    CHECK(!s.setCenterFrequencyHz(-5.0));
    CHECK(!s.setCenterFrequencyHz(0.0));
    CHECK(!s.setCenterFrequencyHz(std::nan("")));
    CHECK(!s.setCenterFrequencyHz(1e12));
    // A rate change while running: the bridge stops the stream, asks, and starts again.
    CHECK(s.setSampleRateHz(5.2e6));
    CHECK(s.sampleRateHz() == 5e6);
    CHECK(g_fx.get(kRate) == 5000000);
    CHECK(s.running());
    CHECK(g_fx.get(kStarts) == 2);
    {
        collect(s, 50000);
        const auto x = collect(s, 900000);
        CHECK(x.size() == 900000);
        CHECK_NEAR(toneHz(x, s.sampleRateHz()), expectedHz(7015000, 7013000), 4.0);
    }
    CHECK(!s.setSampleRateHz(0.0));
    CHECK(!s.setSampleRateHz(-3.0));
    // Gains: two settings in the library's own steps, clamped to its ranges, read back from the library.
    {
        const auto g = s.gains();
        CHECK(g.size() == 2 && g[0].name == "LNA" && g[1].name == "VGA");
        CHECK(g[0].maxDb == 3.0 && g[1].maxDb == 31.0 && g[0].unit == cascade::source::GainUnit::Steps && g[1].unit == cascade::source::GainUnit::Steps);
        CHECK(s.setGainDb("LNA", 2.0));
        CHECK(g_fx.get(kLna) == 2 && s.gainDb("LNA") == 2.0);
        CHECK(s.setGainDb("VGA", 17.4));
        CHECK(g_fx.get(kVga) == 17 && s.gainDb("VGA") == 17.0);
        CHECK(s.setGainDb("VGA", 99.0));
        CHECK(g_fx.get(kVga) == 31);
        CHECK(s.setGainDb("LNA", -4.0));
        CHECK(g_fx.get(kLna) == 0);
        CHECK(!s.setGainDb("Mixer", 1.0));
        CHECK(contains(s.lastError(), "no gain called"));
        CHECK(!s.setGainDb("VGA", std::nan("")));
        g_fx.set(kMisbehave, mGainFails);
        CHECK(!s.setGainDb("LNA", 1.0));
        CHECK(contains(s.lastError(), "refused that gain"));
        CHECK(s.gainDb("LNA") == 0.0);   // the old value stands
        g_fx.set(kMisbehave, 0);
    }
    CHECK(!s.autoGainSupported() && s.antennas() == std::vector<std::string>{"RF"} && s.antenna() == "RF");
    const auto st = s.stats();
    CHECK(st.callbacks > 20 && st.samplePairs > 1000000 && st.badCallbacks == 0);
    s.stop();
    CHECK(!s.running());
    CHECK(g_fx.get(kCancels) >= 2);
    CHECK(g_fx.get(kThreads) == 1);      // every control call came from ONE thread (the streaming call runs on another, which the stand-in does not count)
    CHECK(g_fx.get(kForbiddenCalls) == 0);    // nothing that changes the device was ever called
    s.closeDevice();
    CHECK(!s.isOpen());
    CHECK(g_fx.get(kOpens) == 1 && g_fx.get(kCloses) == 1);
}

void testStartStopAndReopen() {
    g_fx.reset();
    FobosSource s;
    for (int round = 0; round < 3; ++round) {
        CHECK(s.open(args("timeout=2000")));
        CHECK(s.start());
        CHECK(collect(s, 20000).size() == 20000);
        s.stop();
        CHECK(!s.running());
        CHECK(s.start());
        CHECK(collect(s, 20000).size() == 20000);
        s.closeDevice();
        CHECK(!s.isOpen() && !s.running());
    }
    CHECK(g_fx.get(kOpens) == 3 && g_fx.get(kCloses) == 3 && g_fx.get(kStarts) == 6);
    // A library whose streaming call returns at once and streams on its own thread behaves the same.
    g_fx.reset();
    g_fx.set(kNonBlocking, 1);
    FobosSource n;
    CHECK(n.open(args()));
    CHECK(n.start());
    CHECK(collect(n, 50000).size() == 50000);
    n.stop();
    CHECK(n.start());
    CHECK(collect(n, 50000).size() == 50000);
    n.closeDevice();
    CHECK(g_fx.get(kForbiddenCalls) == 0);
}

void testSampleCount() {
    // The default reads the length as a count of floats (never more than the library gave); a library that counts complex samples shows as half the rate.
    g_fx.reset();
    g_fx.set(kSpeed, 1);
    g_fx.set(kNoiseX1000, 0);
    {
        FobosSource s;
        CHECK(s.open(args("rate=1000000")));
        CHECK(s.start());
        const auto x = collect(s, 3000000, 4500);
        CHECK(x.size() > 1000000);
        CHECK(s.streamNote().empty());   // a stream at the rate asked for says nothing
        CHECK(s.stats().badCallbacks == 0);
        s.closeDevice();
    }
    g_fx.reset();
    g_fx.set(kSpeed, 1);
    g_fx.set(kLenPairs, 1);
    {
        FobosSource s;
        CHECK(s.open(args("rate=1000000")));
        CHECK(s.start());
        const auto x = collect(s, 3000000, 4500);
        CHECK(x.size() > 400000);
        CHECK(contains(s.streamNote().c_str(), "half the rate") && contains(s.streamNote().c_str(), "count=pairs"));
        s.closeDevice();
    }
    // With count=pairs on that library the stream is whole and says nothing.
    g_fx.reset();
    g_fx.set(kSpeed, 1);
    g_fx.set(kLenPairs, 1);
    {
        FobosSource s;
        CHECK(s.open(args("rate=1000000,count=pairs")));
        CHECK(s.start());
        const auto x = collect(s, 3000000, 4500);
        CHECK(x.size() > 1000000);
        CHECK(s.streamNote().empty());
        s.closeDevice();
    }
    g_fx.reset();
}

void testStreamFaults() {
    // Start refused: the sentence carries the library's own words; the radio is not dead and starts later.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartFails);
        FobosSource s;
        CHECK(s.open(args()));
        CHECK(!s.start());
        CHECK(!s.running() && !s.deviceDead());
        CHECK(contains(s.lastError(), "would not start streaming") && contains(s.lastError(), "control transfer failed"));
        g_fx.set(kMisbehave, 0);
        CHECK(s.start());
        CHECK(collect(s, 20000).size() == 20000);
        s.closeDevice();
    }
    // A stream that goes quiet is a fault, named.
    {
        g_fx.reset();
        g_fx.set(kSilentAfter, 5);
        FobosSource s;
        CHECK(s.open(args("silence=400")));
        CHECK(s.start());
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        std::complex<float> tmp[4096];
        while (!s.deviceDead() && std::chrono::steady_clock::now() < end) { s.read(tmp, 4096); }
        CHECK(s.deviceDead() && s.faulted());
        CHECK(contains(s.lastError(), "stopped delivering samples"));
        CHECK(s.faultedWhile() == "streaming");
        s.closeDevice();
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    // An unplug in the middle of the stream: the streaming call returns an error, and the radio is dead at once with the library's words.
    {
        g_fx.reset();
        g_fx.set(kSpeed, 1);
        g_fx.set(kUnplugAfter, 120);   // about a second in: well after the start call has settled
        FobosSource s;
        CHECK(s.open(args()));
        CHECK(s.start());
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        std::complex<float> tmp[4096];
        while (!s.deviceDead() && std::chrono::steady_clock::now() < end) { s.read(tmp, 4096); }
        CHECK(s.deviceDead());
        CHECK(contains(s.lastError(), "stream ended") && contains(s.lastError(), "usb transfer failed"));
        CHECK(s.read(tmp, 16) == 0);
        s.closeDevice();
        CHECK(!s.isOpen());
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    // A callback with a length it was never asked for, no samples, an odd number of floats or a null buffer is dropped and counted, never read.
    for (const std::int64_t mis : {mBadLength, mZeroLength, mOddLength, mNullBuffer}) {
        g_fx.reset();
        g_fx.set(kMisbehave, mis);
        FobosSource s;
        CHECK(s.open(args("silence=60000")));
        CHECK(s.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        std::complex<float> tmp[8192];
        std::size_t got = 0;
        for (int i = 0; i < 20; ++i) { got += s.read(tmp, 8192); }
        const auto st = s.stats();
        CHECK(got == 0);
        CHECK(st.samplePairs == 0);
        CHECK(st.badCallbacks > 5 && st.badCallbacks == st.callbacks);
        CHECK(!s.deviceDead());
        // The same stream, well formed again, carries on.
        g_fx.set(kMisbehave, 0);
        CHECK(collect(s, 20000).size() == 20000);
        s.closeDevice();
    }
    // A flood nobody reads: the ring holds a bounded number of samples, the rest are counted as dropped, memory does not grow.
    {
        g_fx.reset();
        g_fx.set(kSpeed, 100000);
        FobosSource s;
        CHECK(s.open(args("silence=60000")));
        CHECK(s.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const auto st = s.stats();
        CHECK(st.pairsDropped > 0);
        CHECK(st.samplePairs <= (std::uint64_t{1} << 20) + 16384);
        // The ring is full: more arrives and is counted as dropped, while what was kept does not grow.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto st2 = s.stats();
        CHECK(st2.pairsDropped > st.pairsDropped);
        CHECK(st2.samplePairs == st.samplePairs);
        std::vector<std::complex<float>> tmp(65536);
        CHECK(s.read(tmp.data(), tmp.size()) <= tmp.size());
        s.closeDevice();
    }
    // Callbacks that arrive after the stop was asked for write nothing.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mLateAfterCancel);
        FobosSource s;
        CHECK(s.open(args()));
        CHECK(s.start());
        collect(s, 30000);
        s.stop();
        const auto before = s.stats();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto after = s.stats();
        CHECK(g_fx.get(kLateCallbacks) == 3);
        CHECK(before.samplePairs == after.samplePairs);
        CHECK(!s.running());
        s.closeDevice();
    }
    // A crash inside the library while starting the stream is a refusal, not a crash of this process.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartCrashes);
        FobosSource s;
        CHECK(s.open(args()));
        CHECK(!s.start());
        CHECK(s.deviceDead());
        CHECK(contains(s.lastError(), "crashed"));
        s.closeDevice();
    }
    // A crash inside the library on a control call is also a refusal.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mTuneCrashes);
        FobosSource s;
        CHECK(!s.open(args()));   // the first tuning crashed inside the library: caught, the open is refused and says so
        CHECK(!s.isOpen());
        CHECK(contains(s.lastError(), "crashed while tuning"));
        s.closeDevice();
    }
}

void testContainment() {
    // A stream call that never returns and a cancel that does nothing: the silence watchdog names it, closing returns in bounded time, the radio is dead.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartHangs);
        FobosSource s;
        CHECK(s.open(args("timeout=300,silence=300")));
        CHECK(s.start());   // the call is still inside the library: nothing has failed yet
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        std::complex<float> tmp[4096];
        while (!s.deviceDead() && std::chrono::steady_clock::now() < end) { s.read(tmp, 4096); }
        CHECK(s.deviceDead());
        CHECK(contains(s.lastError(), "stopped delivering samples"));
        const auto t1 = std::chrono::steady_clock::now();
        s.closeDevice();
        CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::seconds(4));
        CHECK(!s.isOpen());
        CHECK(s.deviceDead());
    }
    // A cancel call that never returns: bounded, named, dead.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mCancelHangs);
        FobosSource s;
        CHECK(s.open(args("timeout=300")));
        CHECK(s.start());
        collect(s, 10000);
        const auto t0 = std::chrono::steady_clock::now();
        s.stop();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
        CHECK(s.deviceDead());
        CHECK(contains(s.lastError(), "did not answer"));
        const auto t1 = std::chrono::steady_clock::now();
        s.closeDevice();
        CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::seconds(6));
        CHECK(!s.isOpen());
    }
}

// The four calls of the library that change the device are not on the list the bridge resolves, and nothing in the bridge's source names them.
void testReceiveOnly() {
    for (const char* f : fb::kForbidden) {
        for (const char* r : fb::kRequired) { CHECK(std::string(f) != r); }
        for (const char* r : fb::kOptional) { CHECK(std::string(f) != r); }
    }
    CHECK(std::size(fb::kForbidden) == 4);
    const std::string root = EOL_SRC_DIR;
    for (const char* file : {"src/source/fobos_source.cpp", "src/source/fobos_source.hpp"}) {
        const std::string t = readText(root + "/" + file);
        CHECK(!t.empty());
        for (const char* f : fb::kForbidden) { CHECK(!contains(t, f)); }
        CHECK(!contains(t, "firmware_") && !contains(t, "_gpo") && !contains(t, "rx_reset"));
    }
    // The only file that names them is the api header, once each, in kForbidden.
    const std::string api = readText(root + "/src/source/fobos_api.hpp");
    for (const char* f : fb::kForbidden) {
        std::size_t at = 0, n = 0;
        while ((at = api.find(std::string("\"") + f + "\"", at)) != std::string::npos) { ++n; ++at; }
        CHECK(n == 1);
    }
    // Every call a bridge could make through Api is one of the resolved names: count the members against the lists.
    CHECK(std::size(fb::kRequired) == 10 && std::size(fb::kOptional) == 5);
}

// Nothing in the new files writes to the console or the error stream (the host captures both into its log and its reports).
void testSinks() {
    const std::string root = EOL_SRC_DIR;
    for (const char* f : {"src/source/fobos_source.cpp", "src/source/fobos_source.hpp", "src/source/fobos_api.hpp"}) {
        const std::string t = readText(root + "/" + f);
        CHECK(!t.empty());
        CHECK(!contains(t, "stderr"));
        CHECK(!contains(t, "stdout"));
        CHECK(!contains(t, "std::cerr"));
        CHECK(!contains(t, "std::cout"));
        CHECK(!contains(t, " printf("));
        CHECK(!contains(t, "puts("));
        CHECK(!contains(t, "OutputDebugString"));
    }
}

}  // namespace

int main() {
    if (!g_fx.load()) {
        std::printf("FAIL cannot load the stand-in library\n");
        return 1;
    }
    testNothingAtStart();
    testOpenRefusals();
    testHappyPath();
    testStartStopAndReopen();
    testSampleCount();
    testStreamFaults();
    testReceiveOnly();
    testSinks();
    testContainment();   // last: it leaves a library call in flight on purpose
    return testSummary("test_fobos_source");
}
