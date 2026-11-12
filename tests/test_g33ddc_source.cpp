// test_g33ddc_source.cpp - the WiNRADiO G33DDC run-time bridge (source/g33ddc_source), proven against a STAND-IN G33DDCAPI.dll that this project builds
// from the maker's published document (tests/fixtures/g33ddc_standin.cpp).
//
// What it can and cannot prove is as in test_g39ddc_source.cpp: it proves what the bridge does with a library that behaves as the document says and
// with ones that do not (no library, not a library, the wrong library, a device list that overruns its buffer or comes in another layout, no
// receiver, a refused call, a silent stream, a disconnect, bad callback arguments, a crash inside the library, a call that never returns); it cannot
// prove the document was read rightly, and nothing here has touched a real receiver (UNVERIFIED ON HARDWARE). Generated signals carry noise, a static
// frequency error and a sample-clock error of 1500 ppm; the tone is found where the clock error puts it.
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
#include "source/g33ddc_source.hpp"
#include "source/vendor_lib.hpp"
#include "test_check.hpp"

using cascade::source::G33DdcSource;
namespace vl = cascade::source::vendorlib;

namespace {

enum Key : int {
    kMisbehave = 1, kToneHz = 2, kSpeed = 3, kSilentAfter = 4, kPpmX1000 = 5, kF = 10, kDdcType = 12, kPower = 13, kStarts = 14, kStops = 15,
    kOpens = 16, kCloses = 17, kThreads = 18, kOverlap = 19, kPreamp = 20, kCallbackSet = 21, kCalls = 22, kNoiseX1000 = 23,
};
enum : std::int64_t {
    mNoDevices = 1 << 0, mOpenFails = 1 << 1, mStartFails = 1 << 2, mStartHangs = 1 << 3, mStartCrashes = 1 << 4, mOverrun = 1 << 5,
    mDisconnected = 1 << 6, mBadBits = 1 << 8, mBadCount = 1 << 9, mTuneFails = 1 << 10, mSetDdcFails = 1 << 11, mBadRecord = 1 << 12,
};

struct Fx {
    vl::Library lib;
    void (*set)(int, std::int64_t) = nullptr;
    std::int64_t (*get)(int) = nullptr;
    void (*reset)() = nullptr;
    std::string dir;
    bool load() {
        dir = eol::u8(eol::fixtureDir());
        if (!lib.open(dir + "\\G33DDCAPI.dll").ok()) { return false; }
        set = reinterpret_cast<void (*)(int, std::int64_t)>(lib.symbol("eol_set"));
        get = reinterpret_cast<std::int64_t (*)(int)>(lib.symbol("eol_get"));
        reset = reinterpret_cast<void (*)()>(lib.symbol("eol_reset"));
        return set != nullptr && get != nullptr && reset != nullptr;
    }
};
Fx g_fx;

std::string args(const std::string& extra = "") { return "lib=" + g_fx.dir + (extra.empty() ? "" : "," + extra); }

std::vector<std::complex<float>> collect(G33DdcSource& s, std::size_t n, int timeoutMs = 8000) {
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

void testNothingAtStart() {
    const std::uint64_t loads = vl::Library::silentLoadCount();
    {
        G33DdcSource s;
        CHECK(!s.isOpen() && !s.running() && !s.deviceDead());
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
    const auto dir = eol::scratch("g33ref");
    const auto fx = eol::fixtureDir();
    // No library in the folder; the G39DDC's library in the folder is not the G33DDC's.
    {
        G33DdcSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "not-found");
        CHECK(contains(s.lastError(), "G33DDCAPI.dll"));
        CHECK(!contains(s.lastError(), "eol-g33ref"));
        std::filesystem::copy_file(fx / "G39DDCAPI.dll", dir / "G39DDCAPI.dll", std::filesystem::copy_options::overwrite_existing);
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "not-found");
        std::filesystem::remove(dir / "G39DDCAPI.dll");
    }
    {
        eol::writeFile(dir / "G33DDCAPI.dll", std::vector<std::uint8_t>(3000, 'x'));
        G33DdcSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "not-an-image");
    }
    {
        std::filesystem::copy_file(fx / "eol_partial.dll", dir / "G33DDCAPI.dll", std::filesystem::copy_options::overwrite_existing);
        G33DdcSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "symbol-missing");
        CHECK(contains(s.lastError(), "OpenDevice"));
        CHECK(contains(s.lastError(), "StartDDC1"));
    }
    {
        std::filesystem::copy_file(fx / "eol_needs_dep.dll", dir / "G33DDCAPI.dll", std::filesystem::copy_options::overwrite_existing);
        G33DdcSource s;
        CHECK(!s.open("lib=" + eol::u8(dir)));
        CHECK(s.openVerdictWord() == "load-failed");
        CHECK(contains(s.lastError(), "126"));
    }
    for (const char* bad : {"rate=abc", "rate=5", "timeout=1", "silence=0", "timeout=99999999999"}) {
        G33DdcSource s;
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
        {mNoDevices, "", "no-device", "no WiNRADiO G33DDC receiver is connected"},
        {mOverrun, "", "no-device", "wrote past the end of the device list"},
        {mBadRecord, "", "no-device", "layout FoxSDR does not recognise"},
        {mOpenFails, "", "open-failed", "could not be opened"},
        {mSetDdcFails, "", "open-failed", "refused the sample rate"},
        {0, "serial=NOPE", "no-device", "serial number you gave"},
    };
    for (const Case& c : cases) {
        g_fx.reset();
        g_fx.set(kMisbehave, c.mis);
        G33DdcSource s;
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
    // A receiver whose tuning range is unknown to the document: a refusal of the first tuning is not an open failure; later ones are refused with a sentence.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mTuneFails);
        G33DdcSource s;
        CHECK(s.open(args()));
        CHECK(s.isOpen());
        CHECK(std::string(s.lastError()).empty());
        CHECK(!s.setCenterFrequencyHz(7e6));
        CHECK(contains(s.lastError(), "refused that frequency"));
        s.closeDevice();
    }
    g_fx.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testHappyPath() {
    g_fx.reset();
    G33DdcSource s;
    CHECK(s.open(args()));
    CHECK(std::string(s.driverKey()) == "g33ddc");
    CHECK((s.supportedSampleRatesHz() == std::vector<double>{48000, 96000, 192000, 384000, 768000}));
    CHECK(s.sampleRateHz() == 192000.0);
    double lo = 0, hi = 0;
    CHECK(!s.frequencyRangeHz(lo, hi));  // the document gives none
    CHECK(s.serialMasked() == "****0001");
    CHECK(g_fx.get(kPower) == 1);
    CHECK(g_fx.get(kCallbackSet) == 1);
    CHECK(s.centerFrequencyHz() == 10.0e6);
    CHECK(g_fx.get(kF) == 10000000);
    std::complex<float> junk[64];
    CHECK(s.read(junk, 64) == 0);
    CHECK(s.start());
    CHECK(s.running());
    CHECK(s.start());
    CHECK(s.setCenterFrequencyHz(7010000.0));
    CHECK(g_fx.get(kF) == 7010000);
    {
        collect(s, 20000);
        const auto x = collect(s, 600000);
        CHECK(x.size() == 600000);
        CHECK_NEAR(toneHz(x, s.sampleRateHz()), expectedHz(7015000, 7010000), 2.5);
        CHECK_NEAR(meanMag(x), 0.5, 0.03);
        CHECK(std::fabs(toneHz(x, s.sampleRateHz()) - (7015000 + 12.3 - 7010000)) > 4.0);
    }
    CHECK(s.setCenterFrequencyHz(7013450.0));
    CHECK(g_fx.get(kF) == 7013450);
    {
        collect(s, 30000);
        const auto x = collect(s, 600000);
        CHECK_NEAR(toneHz(x, s.sampleRateHz()), expectedHz(7015000, 7013450), 2.5);
    }
    CHECK(s.centerFrequencyHz() == 7013450.0);
    // Refused by the library: the sentence says so and the centre stays.
    CHECK(!s.setCenterFrequencyHz(60.0e6));
    CHECK(contains(s.lastError(), "refused that frequency"));
    CHECK(s.centerFrequencyHz() == 7013450.0);
    CHECK(!s.setCenterFrequencyHz(-5.0));
    CHECK(!s.setCenterFrequencyHz(5.0e9));
    CHECK(!s.setCenterFrequencyHz(std::nan("")));
    // A rate change while running; a 32-bit type.
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
    CHECK(s.setSampleRateHz(200000.0));
    CHECK(s.sampleRateHz() == 192000.0);
    CHECK(!s.setSampleRateHz(0.0));
    CHECK(!s.setSampleRateHz(-3.0));
    {
        const auto g = s.gains();
        CHECK(g.size() == 1 && g[0].name == "Preamp");
        CHECK(s.setGainDb("Preamp", 1.0));
        CHECK(g_fx.get(kPreamp) == 1 && s.gainDb("Preamp") == 1.0);
        CHECK(s.setGainDb("Preamp", 0.0));
        CHECK(g_fx.get(kPreamp) == 0);
        CHECK(!s.setGainDb("LNA", 3.0));
    }
    const auto st = s.stats();
    CHECK(st.callbacks > 100 && st.samplePairs > 1500000 && st.badCallbacks == 0 && st.pairsDropped == 0);
    s.stop();
    CHECK(!s.running());
    CHECK(g_fx.get(kThreads) == 1);  // THE ONE-THREAD RULE
    CHECK(g_fx.get(kOverlap) == 1);
    s.closeDevice();
    CHECK(!s.isOpen());
    CHECK(g_fx.get(kOpens) == 1 && g_fx.get(kCloses) == 1);
    CHECK(g_fx.get(kPower) == 0);
    CHECK(g_fx.get(kCallbackSet) == 0);
    g_fx.reset();
}

void testConcurrentCallers() {
    g_fx.reset();
    G33DdcSource s;
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
    G33DdcSource s;
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
        G33DdcSource t;
        CHECK(t.open(args()));
        CHECK(t.start());
        t.closeDevice();
    }
    CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    CHECK(g_fx.get(kOpens) == 31);
    g_fx.reset();
}

void testStreamFaults() {
    {
        g_fx.reset();
        g_fx.set(kSilentAfter, 3);
        G33DdcSource s;
        CHECK(s.open(args("silence=600")));
        CHECK(s.start());
        const auto t0 = std::chrono::steady_clock::now();
        std::complex<float> x[4096];
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) { s.read(x, 4096); }
        CHECK(s.faulted() && s.deviceDead());
        CHECK(contains(s.lastError(), "stopped delivering"));
        CHECK(s.faultedWhile() == "streaming");
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
        CHECK(s.read(x, 4096) == 0);
        s.closeDevice();
        g_fx.reset();
    }
    {
        g_fx.reset();
        g_fx.set(kSilentAfter, 2);
        G33DdcSource s;
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
    for (const std::int64_t bad : {mBadBits, mBadCount}) {
        g_fx.reset();
        g_fx.set(kMisbehave, bad);
        G33DdcSource s;
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
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartFails);
        G33DdcSource s;
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
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartCrashes);
        G33DdcSource s;
        CHECK(s.open(args()));
        CHECK(!s.start());
        CHECK(s.deviceDead());
        CHECK(contains(s.lastError(), "crashed"));
        CHECK(s.faultedWhile() == "starting the stream");
        CHECK(!s.running());
        CHECK(!s.setCenterFrequencyHz(7e6));
        s.closeDevice();
        g_fx.reset();
        G33DdcSource again;
        CHECK(again.open(args()));
        again.closeDevice();
    }
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mStartHangs);
        G33DdcSource s;
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

// Nothing in the new files writes to the console or the error stream (the host captures both into its log and its reports).
void testSinks() {
    const std::string root = EOL_SRC_DIR;
    for (const char* f : {"src/source/g33ddc_source.cpp", "src/source/g33ddc_source.hpp", "src/source/g33ddc_api.hpp"}) {
        const std::string t = readText(root + "/" + f);
        CHECK(!t.empty());
        CHECK(!contains(t, "stderr"));
        CHECK(!contains(t, "stdout"));
        CHECK(!contains(t, "std::cerr"));
        CHECK(!contains(t, "std::cout"));
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
    testNothingAtStart();
    testOpenRefusals();
    testHappyPath();
    testConcurrentCallers();
    testStartStopAndReopen();
    testStreamFaults();
    testSinks();
    testContainment();  // last: it leaves a library call in flight on purpose
    return testSummary("test_g33ddc_source");
}
