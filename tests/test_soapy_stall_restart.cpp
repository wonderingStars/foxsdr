// A SoapySDR stream that goes silent is restarted (UHD only), a bounded number of times, and then said to be dead in words that are true.
//
// Field, 20 and 22 Nov 2026 (the owner's B200 on 0.99.112): twice the stream was shown at ZERO samples per second for minutes and hours with the radio still
// open and no fault, restart or message. The likely cause was a STOPPED receiver counted as silence (the radio journal), and the stall did not reproduce on
// the real board; the detector is the net under the one driver with that evidence. Every other driver keeps 0.99.112's behaviour (wait), with one log line.
//
// The fakes are registered SoapySDR modules (the registry tests/test_soapy_read_bound.cpp uses), so SoapySource::read, start, stop, close, setSampleRateHz
// and open run for real against them. Nothing here touches a radio.
//
//   uhd         registered under the driver key the real B200 uses; streams that can be killed from outside (everything then times out) and made slow;
//               every make() it is handed is recorded.
//   faketstall  the same device under a driver key that is NOT uhd.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "test_check.hpp"

namespace {

using cascade::source::SoapySource;
using Clock = std::chrono::steady_clock;

struct World {
    std::atomic<int> setups{0};
    std::atomic<int> closes{0};
    std::atomic<int> activates{0};
    std::atomic<int> deactivates{0};
    std::atomic<int> rateSets{0};
    std::atomic<int> freqSets{0};
    std::atomic<bool> killAll{false};        // every stream that exists now stops delivering
    std::atomic<bool> neverHealthy{false};   // a stream made from now on is dead from birth
    std::atomic<int> slowDeactivateMs{0};
    std::atomic<int> slowSetupMs{0};
    std::atomic<double> rate{2.0e6};
    std::atomic<double> freq{98.0e6};
    std::mutex m;
    std::vector<std::string> makeArgs;       // what make() was given
    void reset() {
        setups = closes = activates = deactivates = rateSets = freqSets = 0;
        killAll = false;
        neverHealthy = false;
        slowDeactivateMs = slowSetupMs = 0;
        rate = 2.0e6;
        freq = 98.0e6;
        std::lock_guard<std::mutex> lk(m);
        makeArgs.clear();
    }
};
World g_stall;   // the "faketstall" driver (not uhd)
World g_uhd;     // the "uhd" driver

struct StreamState {
    bool dead;
};

class FakeDevice : public SoapySDR::Device {
public:
    FakeDevice(World& w, std::string key) : w_(w), key_(std::move(key)) {}
    std::string getDriverKey() const override { return key_; }
    std::string getHardwareKey() const override { return "fake " + key_; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&, const std::vector<size_t>&, const SoapySDR::Kwargs&) override {
        if (const int ms = w_.slowSetupMs.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
        w_.setups.fetch_add(1);
        return reinterpret_cast<SoapySDR::Stream*>(new StreamState{w_.neverHealthy.load()});
    }
    void closeStream(SoapySDR::Stream* s) override {
        w_.closes.fetch_add(1);
        delete reinterpret_cast<StreamState*>(s);
    }
    int activateStream(SoapySDR::Stream*, const int, const long long, const size_t) override { w_.activates.fetch_add(1); return 0; }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        if (const int ms = w_.slowDeactivateMs.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
        w_.deactivates.fetch_add(1);
        return 0;
    }
    void setSampleRate(const int, const size_t, const double r) override { w_.rateSets.fetch_add(1); w_.rate = r; }
    double getSampleRate(const int, const size_t) const override { return w_.rate.load(); }
    void setFrequency(const int, const size_t, const double f, const SoapySDR::Kwargs&) override { w_.freqSets.fetch_add(1); w_.freq = f; }
    double getFrequency(const int, const size_t) const override { return w_.freq.load(); }
    int readStream(SoapySDR::Stream* s, void* const* buffs, const size_t n, int&, long long&, const long timeoutUs) override {
        StreamState* st = reinterpret_cast<StreamState*>(s);
        if (st->dead || w_.killAll.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(std::min<long>(timeoutUs, 3000)));
            return SOAPY_SDR_TIMEOUT;
        }
        float* p = static_cast<float*>(buffs[0]);
        for (size_t i = 0; i < n; ++i) { p[2 * i] = 0.25f; p[2 * i + 1] = -0.5f; }
        return static_cast<int>(n);
    }
private:
    World& w_;
    std::string key_;
};

SoapySDR::KwargsList findNone(const SoapySDR::Kwargs&) { return {}; }
SoapySDR::Device* makeIn(World& w, const char* key, const SoapySDR::Kwargs& a) {
    {
        std::lock_guard<std::mutex> lk(w.m);
        w.makeArgs.push_back(SoapySDR::KwargsToString(a));
    }
    return new FakeDevice(w, key);
}
SoapySDR::Device* makeStall(const SoapySDR::Kwargs& a) { return makeIn(g_stall, "faketstall", a); }
SoapySDR::Device* makeUhd(const SoapySDR::Kwargs& a) { return makeIn(g_uhd, "uhd", a); }
SoapySDR::KwargsList findStall(const SoapySDR::Kwargs&) { return {}; }

constexpr std::size_t kN = 1024;
const char* const kUhd = "driver=uhd, serial=ABC123";

// Reads until samples arrive, or the time is up. Returns the samples got (0 = none in the time).
std::size_t readSome(SoapySource& src, std::chrono::milliseconds budget) {
    std::vector<std::complex<float>> buf(kN);
    const auto end = Clock::now() + budget;
    while (Clock::now() < end) {
        const std::size_t got = src.read(buf.data(), kN);
        if (got > 0) { return got; }
        if (src.faulted()) { return 0; }
    }
    return 0;
}

bool readUntilFault(SoapySource& src, std::chrono::milliseconds budget) {
    std::vector<std::complex<float>> buf(kN);
    const auto end = Clock::now() + budget;
    while (Clock::now() < end) {
        (void)src.read(buf.data(), kN);
        if (src.faulted()) { return true; }
    }
    return false;
}

void readFor(SoapySource& src, std::chrono::milliseconds t) {
    std::vector<std::complex<float>> buf(kN);
    const auto end = Clock::now() + t;
    while (Clock::now() < end) { (void)src.read(buf.data(), kN); }
}

std::size_t ringCount(const char* needle) {
    std::size_t n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

SoapySDR::Kwargs lastMake(World& w) {
    std::lock_guard<std::mutex> lk(w.m);
    return SoapySDR::KwargsFromString(w.makeArgs.empty() ? std::string() : w.makeArgs.back());
}

void cureOnSecondStream() {
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (g_uhd.setups.load() < 2 && Clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    g_uhd.killAll = false;
}

}  // namespace

int main() {
    SoapySDR::Registry regStall("faketstall", &findStall, &makeStall, SOAPY_SDR_ABI_VERSION);
    SoapySDR::Registry regUhd("uhd", &findNone, &makeUhd, SOAPY_SDR_ABI_VERSION);

    // --- A: a UHD stream that goes silent while running is restarted, and the samples come back ------------------------------------------------------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(150));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        CHECK(g_uhd.setups == 1);
        const int activatesBefore = g_uhd.activates.load();
        g_uhd.killAll = true;                          // the stall; a restart cures it (the fake lifts the kill when the second stream is made)
        std::thread cure(cureOnSecondStream);
        const std::size_t got = readSome(src, std::chrono::seconds(5));
        cure.join();
        CHECK(got == kN);                              // samples flow again, without anyone touching the radio
        CHECK(!src.faulted());
        CHECK(g_uhd.setups == 2);                      // the stream was re-created ...
        CHECK(g_uhd.closes >= 1);
        CHECK(g_uhd.activates > activatesBefore);      // ... and activated again
        CHECK(g_uhd.rateSets >= 1);                    // the settings were put back
        CHECK(g_uhd.freqSets >= 1);
        CHECK(src.stallRestartsForTest() == 1);
        CHECK(src.lastRestartMsForTest() >= 0);
    }

    // --- B: a stream that never comes back is restarted three times and then the radio is said to be dead, truthfully -----------------------------------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_uhd.neverHealthy = true;
        g_uhd.killAll = true;
        CHECK(readUntilFault(src, std::chrono::seconds(10)));
        CHECK(src.faulted());
        CHECK(g_uhd.setups == 1 + SoapySource::kMaxStallRestarts);   // open + exactly three restarts, no more
        const std::string why = src.lastError();
        CHECK(why.find("restarting its stream 3 times") != std::string::npos);
        CHECK(why.find("power-cycle") != std::string::npos);
        CHECK(why.find("unplug") == std::string::npos);              // never "unplugged" when the device never said it was gone
        CHECK(why.find("disconnect") == std::string::npos);
        std::vector<std::complex<float>> buf(kN);
        CHECK(src.read(buf.data(), kN) == 0);
    }

    // --- B2: a steady stream is never restarted, and a quiet gap shorter than the window is not a stall -------------------------------------------------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(400));
        CHECK(src.open(kUhd) && src.start());
        for (int i = 0; i < 50; ++i) { CHECK(readSome(src, std::chrono::seconds(2)) == kN); }
        g_uhd.killAll = true;
        readFor(src, std::chrono::milliseconds(120));
        g_uhd.killAll = false;
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        CHECK(g_uhd.setups == 1);
        CHECK(src.stallRestartsForTest() == 0);
        CHECK(!src.faulted());
    }

    // --- C: a live rate change keeps the stream delivering at the new rate, with the stream made ONCE and no restart (0.99.112's behaviour, kept) ---------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::seconds(60));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        for (const double hz : {2.4e6, 1.0e6, 2.0e6}) {   // the owner's sequence
            CHECK(src.setSampleRateHz(hz));
            CHECK(src.sampleRateHz() == hz);
            CHECK(readSome(src, std::chrono::milliseconds(500)) == kN);
            CHECK(readSome(src, std::chrono::milliseconds(500)) == kN);
        }
        CHECK(g_uhd.setups == 1);
        CHECK(g_uhd.deactivates == 3 && g_uhd.activates == 4);
        CHECK(src.stallRestartsForTest() == 0);
        CHECK(!src.faulted());
    }

    // --- C3: A STOPPED RECEIVER IS NOT A STALL: reads that stop for longer than the window, then start again, cost no restart and no fault -------------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(150));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_uhd.killAll = true;
        std::vector<std::complex<float>> buf(kN);
        readFor(src, std::chrono::milliseconds(80));     // a silent run, shorter than the window
        src.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(src.start());
        (void)src.read(buf.data(), kN);                   // still silent for one read after the start
        g_uhd.killAll = false;
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        CHECK(g_uhd.setups == 1);
        CHECK(src.stallRestartsForTest() == 0);
        CHECK(!src.faulted());
    }

    // --- F: SILENCE IS TIME SPENT READING, NOT THE GAP BETWEEN TWO SILENT READS (a sleep, a late retune, a long lock hold) --------------------------------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(300));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_uhd.killAll = true;
        std::vector<std::complex<float>> buf(kN);
        (void)src.read(buf.data(), kN);                    // one silent read
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // four windows with no reads at all
        (void)src.read(buf.data(), kN);                    // and one more: adds one capped step (250 ms), not 1200 ms
        CHECK(g_uhd.setups == 1);
        CHECK(src.stallRestartsForTest() == 0);
        CHECK(!src.faulted());
        // ...while back-to-back silent reads for the window do restart it: the kill is left on until the second stream is made
        std::thread cure(cureOnSecondStream);
        CHECK(readSome(src, std::chrono::seconds(5)) == kN);
        cure.join();
        CHECK(g_uhd.setups == 2);
    }

    // --- G: A STREAM THIS OBJECT SAYS IS STOPPED IS NEVER RESTARTED (an inactive stream answers TIMEOUT by the SoapySDR contract) ----------------------
    {
        g_uhd.reset();
        g_uhd.killAll = true;
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(150));
        CHECK(src.open(kUhd));                              // opened, never started: running() is false
        readFor(src, std::chrono::milliseconds(600));       // four windows of silent reads
        CHECK(g_uhd.setups == 1);
        CHECK(g_uhd.activates == 0);                        // nothing activated a stream the object says is stopped
        CHECK(src.stallRestartsForTest() == 0);
        CHECK(!src.faulted());
    }

    // --- E: ANOTHER DRIVER KEEPS 0.99.112's BEHAVIOUR: waits, one log line, no restart, no fault --------------------------------------------------------
    {
        g_stall.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(150));
        CHECK(src.open("driver=faketstall") && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        const std::size_t linesBefore = ringCount("not restarted: only a UHD stream is");
        g_stall.killAll = true;
        readFor(src, std::chrono::milliseconds(900));       // far past the window
        CHECK(g_stall.setups == 1);                         // no restart
        CHECK(!src.faulted());                              // no fault
        CHECK(src.stallRestartsForTest() == 0);
        CHECK(ringCount("not restarted: only a UHD stream is") == linesBefore + 1);   // ONE line for the whole silence
        g_stall.killAll = false;
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);   // and it resumed by itself
    }

    // --- H: CLOSE DURING A RESTART returns promptly and does not condemn the radio ("restart FoxSDR") -------------------------------------------------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_uhd.slowDeactivateMs = 700;                       // a restart now takes about 2.1 seconds: longer than stop/close's 1.5 s lock wait
        g_uhd.slowSetupMs = 1400;
        g_uhd.killAll = true;
        std::atomic<bool> stopReader{false};
        std::thread reader([&] {
            std::vector<std::complex<float>> buf(kN);
            while (!stopReader.load()) {
                (void)src.read(buf.data(), kN);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));   // the source loop's own pacing between reads
            }
        });
        const auto wait = Clock::now() + std::chrono::seconds(5);
        while (!src.restartingForTest() && Clock::now() < wait) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK(src.restartingForTest());                     // the restart is in flight
        const auto t0 = Clock::now();
        src.closeDevice();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        stopReader = true;
        reader.join();
        CHECK(took < 3500);
        CHECK(took < 4500);                                 // a stop during a restart is inside the hang watchdog's 5 s (kControlLockWait + kRestartGiveBack + kVendorCallWait = 4.5 s at most)
        CHECK(!src.faulted());
        CHECK(std::string(src.lastError()).find("restart FoxSDR") == std::string::npos);
        CHECK(src.deadReason() != SoapySource::DeadReason::Abandoned);
    }

    // --- D: the saved row's volatile display keys are not handed to a UHD open; everything else is ---------------------------------------------------------
    {
        g_uhd.reset();
        {
            SoapySource src;   // a USB board
            CHECK(src.open("driver=uhd, label=B200 (usb 1-7), name=, product=B200, serial=ABC123, type=b200"));
            const SoapySDR::Kwargs seen = lastMake(g_uhd);
            CHECK(seen.size() == 3);
            CHECK(seen.count("driver") == 1 && seen.at("driver") == "uhd");
            CHECK(seen.count("serial") == 1 && seen.at("serial") == "ABC123");
            CHECK(seen.count("type") == 1 && seen.at("type") == "b200");
            CHECK(seen.count("label") == 0 && seen.count("name") == 0 && seen.count("product") == 0);
        }
        {
            SoapySource src;   // a NETWORK USRP: its address and a key the user typed are kept (without addr UHD depends on UDP broadcast discovery)
            CHECK(src.open("driver=uhd, addr=192.168.10.2, label=X300 (192.168.10.2), name=lab, product=X310, serial=XYZ789, type=x300, master_clock_rate=200e6"));
            const SoapySDR::Kwargs seen = lastMake(g_uhd);
            CHECK(seen.count("addr") == 1 && seen.at("addr") == "192.168.10.2");
            CHECK(seen.count("master_clock_rate") == 1 && seen.at("master_clock_rate") == "200e6");
            CHECK(seen.count("serial") == 1 && seen.count("type") == 1);
            CHECK(seen.count("label") == 0 && seen.count("name") == 0 && seen.count("product") == 0);
        }
        {
            SoapySource src;   // no serial: nothing to pick the board by, so the row goes through unchanged
            CHECK(src.open("driver=uhd, label=B200, type=b200"));
            const SoapySDR::Kwargs seen = lastMake(g_uhd);
            CHECK(seen.count("label") == 1 && seen.at("label") == "B200");
        }
        g_stall.reset();
        {
            SoapySource src;   // another driver is never reduced
            CHECK(src.open("driver=faketstall, label=keep me, serial=XYZ"));
            const SoapySDR::Kwargs seen = lastMake(g_stall);
            CHECK(seen.count("label") == 1 && seen.at("label") == "keep me");
        }
    }

    g_uhd.killAll = false;
    g_stall.killAll = false;
    return testSummary("test_soapy_stall_restart");
}
