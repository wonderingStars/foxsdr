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
#include <functional>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/radio_events.hpp"
#include <filesystem>
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
    std::atomic<int> slowSetRateMs{0};        // setSampleRate takes this long (the settings re-apply)
    std::atomic<int> killCode{SOAPY_SDR_TIMEOUT};   // what a dead stream answers (TIMEOUT, OVERFLOW, CORRUPTION, 0 ...)
    std::atomic<int> deactivateRet{0};        // what deactivateStream answers
    std::atomic<bool> deactivateThrows{false};
    std::atomic<bool> setupNull{false};       // setupStream answers no stream
    std::atomic<bool> setupThrows{false};
    std::atomic<bool> setRateThrows{false};   // setSampleRate refuses (throws)
    std::atomic<int> activateRet{0};          // what activateStream answers
    std::atomic<int> deactivateEntered{0};    // calls that have ENTERED deactivateStream (a hook for tests that wait for a step)
    std::atomic<int> setRateEntered{0};
    std::atomic<bool> setRateFault{false};    // setSampleRate raises a structured exception from "the driver" (the vendor guard absorbs it)
    std::function<void()> onDeactivate;       // run inside deactivateStream / setupStream / setSampleRate (set by a test before the stall begins)
    std::function<void()> onSetup;
    std::function<void()> onSetRate;
    std::atomic<double> rate{2.0e6};
    std::atomic<double> freq{98.0e6};
    std::mutex m;
    std::vector<std::string> makeArgs;       // what make() was given
    void reset() {
        setups = closes = activates = deactivates = rateSets = freqSets = 0;
        killAll = false;
        neverHealthy = false;
        slowDeactivateMs = slowSetupMs = slowSetRateMs = 0;
        killCode = SOAPY_SDR_TIMEOUT;
        deactivateRet = 0;
        activateRet = 0;
        deactivateThrows = setupNull = setupThrows = setRateThrows = false;
        deactivateEntered = setRateEntered = 0;
        setRateFault = false;
        onDeactivate = nullptr;
        onSetup = nullptr;
        onSetRate = nullptr;
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
        if (w_.onSetup) { w_.onSetup(); }
        if (const int ms = w_.slowSetupMs.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
        w_.setups.fetch_add(1);
        if (w_.setupThrows.load()) { throw std::runtime_error("setupStream refused by the fake"); }
        if (w_.setupNull.load()) { return nullptr; }
        return reinterpret_cast<SoapySDR::Stream*>(new StreamState{w_.neverHealthy.load()});
    }
    void closeStream(SoapySDR::Stream* s) override {
        w_.closes.fetch_add(1);
        delete reinterpret_cast<StreamState*>(s);
    }
    int activateStream(SoapySDR::Stream*, const int, const long long, const size_t) override { w_.activates.fetch_add(1); return w_.activateRet.load(); }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        w_.deactivateEntered.fetch_add(1);
        if (w_.onDeactivate) { w_.onDeactivate(); }
        if (const int ms = w_.slowDeactivateMs.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
        w_.deactivates.fetch_add(1);
        if (w_.deactivateThrows.load()) { throw std::runtime_error("deactivateStream refused by the fake"); }
        return w_.deactivateRet.load();
    }
    void setSampleRate(const int, const size_t, const double r) override {
        w_.setRateEntered.fetch_add(1);
        if (w_.onSetRate) { w_.onSetRate(); }
#ifdef _WIN32
        if (w_.setRateFault.load()) { ::RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }   // from KERNELBASE, so the guard's module filter absorbs it
#endif
        if (const int ms = w_.slowSetRateMs.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
        w_.rateSets.fetch_add(1);
        if (w_.setRateThrows.load()) { throw std::runtime_error("sample rate not supported by the fake"); }
        w_.rate = r;
    }
    double getSampleRate(const int, const size_t) const override { return w_.rate.load(); }
    void setFrequency(const int, const size_t, const double f, const SoapySDR::Kwargs&) override { w_.freqSets.fetch_add(1); w_.freq = f; }
    double getFrequency(const int, const size_t) const override { return w_.freq.load(); }
    int readStream(SoapySDR::Stream* s, void* const* buffs, const size_t n, int&, long long&, const long timeoutUs) override {
        StreamState* st = reinterpret_cast<StreamState*>(s);
        if (st->dead || w_.killAll.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(std::min<long>(timeoutUs, 3000)));
            return w_.killCode.load();
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
        // THE YIELD ITSELF (review of fb3b2a89, section 7): the stop arrived during the slow deactivate, so the restart gave the lock back at its next
        // checkpoint and never re-created the stream (700 ms deactivate, then the 1400 ms setup it would otherwise have gone on to). Without the cancel
        // checks the timing above still passes, and only this count says the restart was cut short.
        CHECK(g_uhd.setups == 1);
        CHECK(g_uhd.activates == 1);                        // the start's, and no restart activation after the stop
    }

    // --- H2: THE WINDOW BETWEEN THE END OF A RESTART AND read() LETTING GO OF THE LOCK (review of fb3b2a89, 3.2, and the second wait of lockForEscape) -----
    // The restart's last lines (the log, the fault report) still run under the device lock after the restart returned. A close whose first 1.5 s wait runs
    // out inside that window must still find "restarting" raised and take its second wait, or it condemns a healthy radio. The hook holds the lock
    // deterministically: it ends 1.8 s after the close began, i.e. after the first wait expired and well inside the second.
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_uhd.slowDeactivateMs = 300;
        std::atomic<long long> closeT0Ms{0};   // steady_clock ms at which the close began; 0 = not yet
        std::atomic<bool> hookRan{false};
        src.setAfterRestartHookForTest([&] {
            hookRan = true;
            const auto give = Clock::now() + std::chrono::seconds(10);
            while (closeT0Ms.load() == 0 && Clock::now() < give) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            const auto until = Clock::time_point(std::chrono::milliseconds(closeT0Ms.load())) + std::chrono::milliseconds(1800);
            std::this_thread::sleep_until(until);
        });
        g_uhd.killAll = true;
        std::atomic<bool> stopReader{false};
        std::thread reader([&] {
            std::vector<std::complex<float>> buf(kN);
            while (!stopReader.load()) { (void)src.read(buf.data(), kN); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        });
        const auto wait = Clock::now() + std::chrono::seconds(5);
        while (!src.restartingForTest() && Clock::now() < wait) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(src.restartingForTest());
        closeT0Ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        src.closeDevice();
        stopReader = true;
        reader.join();
        CHECK(hookRan.load());
        CHECK(!src.faulted());                              // not condemned: the second wait took the lock the hook was holding
        CHECK(src.deadReason() != SoapySource::DeadReason::Abandoned);
        CHECK(std::string(src.lastError()).find("restart FoxSDR") == std::string::npos);
        CHECK(std::string(src.lastError()).find("Restart FoxSDR") == std::string::npos);   // (the abandonment sentence is capitalised: H's check above only sees the lower-case ones)
        CHECK(!src.restartingForTest());                    // lowered once the lock was let go
    }

    // --- H3: A CLOSE DURING THE SETTINGS RE-APPLY cuts the restart short before it activates --------------------------------------------------------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_uhd.slowSetRateMs = 1200;                          // the re-apply takes 1.2 s (inside its 1.5 s bound)
        g_uhd.killAll = true;
        std::atomic<bool> stopReader{false};
        std::thread reader([&] {
            std::vector<std::complex<float>> buf(kN);
            while (!stopReader.load()) { (void)src.read(buf.data(), kN); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        });
        const auto wait = Clock::now() + std::chrono::seconds(5);
        while (g_uhd.setRateEntered.load() == 0 && Clock::now() < wait) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(g_uhd.setRateEntered.load() >= 1);             // the restart is inside the re-apply
        const auto t0 = Clock::now();
        src.closeDevice();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        stopReader = true;
        reader.join();
        CHECK(took < 3500);
        CHECK(!src.faulted());
        CHECK(src.deadReason() != SoapySource::DeadReason::Abandoned);
        CHECK(g_uhd.setups == 2);                            // the stream was re-created before the stop came ...
        CHECK(g_uhd.activates == 1);                         // ... and never activated again after it (only the start's activation)
    }

    // --- I: EVERY ANSWER WITHOUT SAMPLES IS SILENCE, AND THE STALL FAULT'S SENTENCE SURVIVES THE READ THAT LATCHED IT (review of fb3b2a89, 3.3) -----------
    for (const int code : {static_cast<int>(SOAPY_SDR_OVERFLOW), static_cast<int>(SOAPY_SDR_CORRUPTION), 0}) {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_uhd.neverHealthy = true;
        g_uhd.killCode = code;
        g_uhd.killAll = true;
        CHECK(readUntilFault(src, std::chrono::seconds(10)));
        CHECK(g_uhd.setups == 1 + SoapySource::kMaxStallRestarts);   // the silence counted: three restarts happened
        const std::string why = src.lastError();
        std::printf("answer %d: %s\n", code, why.c_str());
        CHECK(why.find("restarting its stream 3 times") != std::string::npos);
        CHECK(why.find("power-cycle") != std::string::npos);
        CHECK(why.find("readStream failed") == std::string::npos);   // not written over by the answer that carried the detection
        // PLAY AFTER THE FAULT does not answer "true" and do nothing: it refuses, and the fault's own sentence is still what lastError() says.
        CHECK(!src.start());
        CHECK(std::string(src.lastError()).find("restarting its stream 3 times") != std::string::npos);
    }
    {
        // A HARD ERROR (STREAM_ERROR) is counted into its own fault by kMaxConsecutiveErrors before the stall window can elapse, and says so.
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::seconds(30));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_uhd.killCode = SOAPY_SDR_STREAM_ERROR;
        g_uhd.killAll = true;
        CHECK(readUntilFault(src, std::chrono::seconds(10)));
        CHECK(std::string(src.lastError()).find("readStream failed") != std::string::npos);
        CHECK(g_uhd.setups == 1);
    }

    // --- J: THE RESTART'S STEPS, EACH FAILING (review of fb3b2a89, section 3 "what each failed step leaves") ------------------------------------------
    {
        // J1: a deactivate the driver refuses (or throws out of) does not stop the restart: the stream is rebuilt anyway and the samples come back.
        for (const bool throws : {false, true}) {
            g_uhd.reset();
            SoapySource src;
            src.setStallWindowForTest(std::chrono::milliseconds(150));
            CHECK(src.open(kUhd) && src.start());
            CHECK(readSome(src, std::chrono::seconds(2)) == kN);
            if (throws) { g_uhd.deactivateThrows = true; } else { g_uhd.deactivateRet = SOAPY_SDR_NOT_SUPPORTED; }
            g_uhd.killAll = true;
            std::thread cure(cureOnSecondStream);
            CHECK(readSome(src, std::chrono::seconds(6)) == kN);
            cure.join();
            CHECK(g_uhd.setups == 2);
            CHECK(!src.faulted());
        }
    }
    {
        // J2: the stream cannot be made again (no stream / an exception): the fault says why, no restart is reported, the device is not condemned.
        for (const bool throws : {false, true}) {
            g_uhd.reset();
            SoapySource src;
            src.setStallWindowForTest(std::chrono::milliseconds(100));
            CHECK(src.open(kUhd) && src.start());
            CHECK(readSome(src, std::chrono::seconds(2)) == kN);
            const std::size_t restartedBefore = ringCount("the stream was restarted");
            if (throws) { g_uhd.setupThrows = true; } else { g_uhd.setupNull = true; }
            g_uhd.killAll = true;
            CHECK(readUntilFault(src, std::chrono::seconds(6)));
            const std::string why = src.lastError();
            CHECK(why.find("its stream would not restart") != std::string::npos);
            CHECK(why.find(throws ? "setupStream refused by the fake" : "returned no stream") != std::string::npos);
            CHECK(src.deadReason() == SoapySource::DeadReason::None);
            CHECK(ringCount("the stream was restarted") == restartedBefore);   // StreamRestart is raised with this line, and only when the restart worked
            CHECK(g_uhd.activates == 1);                                       // nothing activated after the failed re-create
            CHECK(g_uhd.rateSets == 0);                                        // and the settings were not put back onto a stream that is not there
        }
    }
    {
        // J3: a setting the driver refuses on the re-apply is logged (which setting, the driver's words) and does not stop the restart.
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(150));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        const std::size_t refusedBefore = ringCount("the driver refused to put back the sample rate: sample rate not supported by the fake");
        g_uhd.setRateThrows = true;
        g_uhd.killAll = true;
        std::thread cure(cureOnSecondStream);
        CHECK(readSome(src, std::chrono::seconds(6)) == kN);
        cure.join();
        CHECK(ringCount("the driver refused to put back the sample rate: sample rate not supported by the fake") == refusedBefore + 1);
        CHECK(g_uhd.freqSets >= 1);                          // the settings after the refused one were still put back
        CHECK(!src.faulted());
    }
    {
        // J4: the activation is refused: the fault says so, and no restart is reported.
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        const std::size_t restartedBefore = ringCount("the stream was restarted");
        g_uhd.activateRet = SOAPY_SDR_NOT_SUPPORTED;
        g_uhd.killAll = true;
        CHECK(readUntilFault(src, std::chrono::seconds(6)));
        CHECK(std::string(src.lastError()).find("activateStream failed") != std::string::npos);
        CHECK(ringCount("the stream was restarted") == restartedBefore);
    }
    // J5: A DEVICE CONDEMNED WHILE A STEP RAN stops the restart at once: after each step the next one is never entered, nothing is activated and no restart is
    // reported (review of fb3b2a89, 3.1). The condemnation is made from inside the step, as a stop that gave up waiting makes it.
    for (int step = 0; step < 3; ++step) {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        const std::size_t restartedBefore = ringCount("the stream was restarted");
        if (step == 0) { g_uhd.onDeactivate = [&src] { src.condemnForTest(); }; }
        if (step == 1) { g_uhd.onSetup = [&src] { src.condemnForTest(); }; }
        if (step == 2) { g_uhd.onSetRate = [&src] { src.condemnForTest(); }; }
        g_uhd.killAll = true;
        CHECK(readUntilFault(src, std::chrono::seconds(6)));
        if (step == 0) { CHECK(g_uhd.setups == 1); }                    // not re-created
        if (step == 1) { CHECK(g_uhd.setRateEntered == 0); }            // settings not put back
        if (step == 2) { CHECK(g_uhd.activates == 1); }                 // not activated again
        CHECK(src.deadReason() == SoapySource::DeadReason::Abandoned);
        CHECK(std::string(src.lastError()).find("Restart FoxSDR") != std::string::npos);   // the condemnation's own sentence, not overwritten
        CHECK(ringCount("the stream was restarted") == restartedBefore);
    }
    // J6: A STEP THAT IS ABANDONED (it does not come back within kVendorCallWait): the restart returns within that bound, the radio is condemned with the
    // abandonment sentence, and nothing after the step is entered. Each of the three jobs.
    for (int step = 0; step < 3; ++step) {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        const void* const streamBefore = src.linkStreamForTest();
        const std::size_t restartedBefore = ringCount("the stream was restarted");
        if (step == 0) { g_uhd.slowDeactivateMs = 2600; }
        if (step == 1) { g_uhd.slowSetupMs = 2600; }
        if (step == 2) { g_uhd.slowSetRateMs = 2600; }
        g_uhd.killAll = true;
        const auto t0 = Clock::now();
        CHECK(readUntilFault(src, std::chrono::seconds(8)));
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        std::printf("abandoned step %d: fault after %lld ms\n", step, static_cast<long long>(took));
        CHECK(took < 2200);                                              // the reader was held for one bounded wait (1.5 s), not for the driver's 2.6 s
        CHECK(src.deadReason() == SoapySource::DeadReason::Abandoned);
        CHECK(std::string(src.lastError()).find("Restart FoxSDR") != std::string::npos);
        CHECK(ringCount("the stream was restarted") == restartedBefore);
        if (step == 0) { CHECK(g_uhd.setups == 1); }
        if (step == 1) { CHECK(g_uhd.rateSets == 0); }
        if (step == 2) { CHECK(g_uhd.activates == 1); }
        std::this_thread::sleep_for(std::chrono::milliseconds(2300));    // the abandoned worker returns now
        // THE ABANDONED RE-CREATE WRITES NOTHING INTO THE FROZEN LINK (review of fb3b2a89, 3.4): its new stream is its own.
        if (step == 1) { CHECK(src.linkStreamForTest() == streamBefore); }   // (step 2's re-create had completed and was published before its re-apply was abandoned)
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

    // --- K: A DRIVER FAULT INSIDE A RESTART STEP IS FILED ONCE, NOT TWICE (review of fb3b2a89, minor item; the old code filed it in noteVendorFault and again in
    // the restart's failure path). LAST: it arms the process-wide crash handler (into a folder of this run's own), after which every absorbed fault writes a report.
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "stall_restart_reports";
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        cascade::core::CrashHandlerConfig cfg;
        cfg.crashDir = dir.string();
        cfg.enabled = true;
        cascade::core::installCrashHandlers(cfg);
        cascade::core::setCrashCaptureEnabled(true, false);

        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open(kUhd) && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        const std::uint64_t before = cascade::core::radioreport::unattributedCount();
        g_uhd.setRateFault = true;                           // the driver faults inside the settings re-apply (absorbed by the vendor guard)
        g_uhd.killAll = true;
        CHECK(readUntilFault(src, std::chrono::seconds(6)));
        CHECK(src.deadReason() == SoapySource::DeadReason::VendorFault);
        const std::uint64_t filed = cascade::core::radioreport::unattributedCount() - before;
        std::printf("device faults filed for one driver fault in a restart step: %llu\n", static_cast<unsigned long long>(filed));
        CHECK(filed == 1);
        std::filesystem::remove_all(dir, ec);
    }

    g_uhd.killAll = false;
    g_stall.killAll = false;
    return testSummary("test_soapy_stall_restart");
}
