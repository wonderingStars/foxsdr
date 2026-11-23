// A SoapySDR stream that goes silent is restarted, a bounded number of times, and then said to be dead in words that are true.
//
// Field, 20 and 22 Nov 2026 (the owner's B200 on 0.99.112): twice the stream went to ZERO samples per second and stayed there - 18 minutes, then 237
// minutes - with the radio still shown as open and no fault, restart or message. The second came straight after the sample rate was changed on a live
// stream (2.4, 1.0, 2.0 MS/s). The 0.99.110 restart covers the RTL-SDR, HackRF and Airspy readers; the SoapySDR source had nothing.
//
// The fakes are registered SoapySDR modules (the registry tests/test_soapy_read_bound.cpp uses), so SoapySource::read, start, setSampleRateHz and open run
// for real against them. Nothing here touches a radio.
//
//   faketstall  streams that can be killed from outside (everything they are asked then times out), healthy again after a restart; or never healthy again.
//   uhd         the same device registered under the driver key the real B200 uses, whose stream is bound to the sample rate it was made at: a stream made
//               before a rate change times out for ever after it (the field behaviour, modelled), and every make() it is handed is recorded.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    std::atomic<double> rate{2.0e6};
    std::atomic<double> freq{98.0e6};
    std::mutex m;
    std::vector<std::string> makeArgs;       // what make() was given
    void reset() {
        setups = closes = activates = deactivates = rateSets = freqSets = 0;
        killAll = false;
        neverHealthy = false;
        rate = 2.0e6;
        freq = 98.0e6;
        std::lock_guard<std::mutex> lk(m);
        makeArgs.clear();
    }
};
World g_stall;   // the "faketstall" driver
World g_uhd;     // the "uhd" driver

struct StreamState {
    double madeAtRate;
    bool dead;
    bool boundToRate;
};

class FakeDevice : public SoapySDR::Device {
public:
    FakeDevice(World& w, std::string key, bool boundToRate) : w_(w), key_(std::move(key)), bound_(boundToRate) {}
    std::string getDriverKey() const override { return key_; }
    std::string getHardwareKey() const override { return "fake " + key_; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&, const std::vector<size_t>&, const SoapySDR::Kwargs&) override {
        w_.setups.fetch_add(1);
        return reinterpret_cast<SoapySDR::Stream*>(new StreamState{w_.rate.load(), w_.neverHealthy.load(), bound_});
    }
    void closeStream(SoapySDR::Stream* s) override {
        w_.closes.fetch_add(1);
        delete reinterpret_cast<StreamState*>(s);
    }
    int activateStream(SoapySDR::Stream*, const int, const long long, const size_t) override { w_.activates.fetch_add(1); return 0; }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override { w_.deactivates.fetch_add(1); return 0; }
    void setSampleRate(const int, const size_t, const double r) override { w_.rateSets.fetch_add(1); w_.rate = r; }
    double getSampleRate(const int, const size_t) const override { return w_.rate.load(); }
    void setFrequency(const int, const size_t, const double f, const SoapySDR::Kwargs&) override { w_.freqSets.fetch_add(1); w_.freq = f; }
    double getFrequency(const int, const size_t) const override { return w_.freq.load(); }
    int readStream(SoapySDR::Stream* s, void* const* buffs, const size_t n, int&, long long&, const long timeoutUs) override {
        StreamState* st = reinterpret_cast<StreamState*>(s);
        const bool silent = st->dead || w_.killAll.load() || (st->boundToRate && st->madeAtRate != w_.rate.load());
        if (silent) {
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
    bool bound_;
};

SoapySDR::KwargsList findStall(const SoapySDR::Kwargs& a) {
    const auto d = a.find("driver");
    if (d != a.end() && d->second != "faketstall") { return {}; }
    return {SoapySDR::Kwargs{{"driver", "faketstall"}, {"label", "fake stall"}}};
}
SoapySDR::Device* makeStall(const SoapySDR::Kwargs&) { return new FakeDevice(g_stall, "faketstall", false); }

SoapySDR::KwargsList findUhd(const SoapySDR::Kwargs&) { return {}; }
SoapySDR::Device* makeUhd(const SoapySDR::Kwargs& a) {
    {
        std::lock_guard<std::mutex> lk(g_uhd.m);
        g_uhd.makeArgs.push_back(SoapySDR::KwargsToString(a));
    }
    return new FakeDevice(g_uhd, "uhd", true);
}

constexpr std::size_t kN = 1024;

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

// Reads until the source faults or the time is up.
bool readUntilFault(SoapySource& src, std::chrono::milliseconds budget) {
    std::vector<std::complex<float>> buf(kN);
    const auto end = Clock::now() + budget;
    while (Clock::now() < end) {
        (void)src.read(buf.data(), kN);
        if (src.faulted()) { return true; }
    }
    return false;
}

}  // namespace

int main() {
    SoapySDR::Registry regStall("faketstall", &findStall, &makeStall, SOAPY_SDR_ABI_VERSION);
    SoapySDR::Registry regUhd("uhd", &findUhd, &makeUhd, SOAPY_SDR_ABI_VERSION);

    // --- A: a stream that goes silent while running is restarted, and the samples come back -----------------------------------------------------------
    {
        g_stall.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(150));
        CHECK(src.open("driver=faketstall") && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        CHECK(g_stall.setups == 1);
        const int activatesBefore = g_stall.activates.load();
        g_stall.killAll = true;                        // the stall
        // The restart makes a new stream; the fake's kill switch is lifted by the first make after it, as a restart cures it.
        std::thread cure([] {
            const auto end = Clock::now() + std::chrono::seconds(5);
            while (g_stall.setups.load() < 2 && Clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            g_stall.killAll = false;
        });
        const std::size_t got = readSome(src, std::chrono::seconds(5));
        cure.join();
        CHECK(got == kN);                              // samples flow again, without anyone touching the radio
        CHECK(!src.faulted());
        CHECK(g_stall.setups == 2);                    // the stream was re-created ...
        CHECK(g_stall.closes >= 1);
        CHECK(g_stall.activates > activatesBefore);    // ... and activated again
        CHECK(g_stall.rateSets >= 1);                  // the settings were put back
        CHECK(g_stall.freqSets >= 1);
        CHECK(src.stallRestartsForTest() == 1);
    }

    // --- B: a stream that never comes back is restarted three times and then the radio is said to be dead, truthfully ------------------------------------
    {
        g_stall.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(100));
        CHECK(src.open("driver=faketstall") && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        g_stall.neverHealthy = true;                   // every stream made from now on is dead from birth
        g_stall.killAll = true;                        // and the one running
        CHECK(readUntilFault(src, std::chrono::seconds(10)));
        CHECK(src.faulted());
        CHECK(g_stall.setups == 1 + SoapySource::kMaxStallRestarts);   // open + exactly three restarts, no more
        const std::string why = src.lastError();
        CHECK(why.find("restarting its stream 3 times") != std::string::npos);
        CHECK(why.find("power-cycle") != std::string::npos);
        // never "unplugged" when the device never said it was gone
        CHECK(why.find("unplug") == std::string::npos);
        CHECK(why.find("disconnect") == std::string::npos);
        // and it is not read again once faulted
        std::vector<std::complex<float>> buf(kN);
        CHECK(src.read(buf.data(), kN) == 0);
    }

    // --- B2: a steady stream is never restarted, and a quiet gap shorter than the window is not a stall -----------------------------------------------
    {
        g_stall.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::milliseconds(400));
        CHECK(src.open("driver=faketstall") && src.start());
        for (int i = 0; i < 50; ++i) { CHECK(readSome(src, std::chrono::seconds(2)) == kN); }
        g_stall.killAll = true;
        std::vector<std::complex<float>> buf(kN);
        const auto end = Clock::now() + std::chrono::milliseconds(120);
        while (Clock::now() < end) { (void)src.read(buf.data(), kN); }
        g_stall.killAll = false;
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        CHECK(g_stall.setups == 1);
        CHECK(src.stallRestartsForTest() == 0);
        CHECK(!src.faulted());
    }

    // --- C: a live rate change on the B200's driver leaves the stream delivering at the new rate -------------------------------------------------------
    {
        g_uhd.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::seconds(60));   // the stall detector must NOT be what saves this: the rate change itself must
        CHECK(src.open("driver=uhd, serial=ABC123") && src.start());
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        for (const double hz : {2.4e6, 1.0e6, 2.0e6}) {   // the owner's sequence
            CHECK(src.setSampleRateHz(hz));
            CHECK(src.sampleRateHz() == hz);
            CHECK(readSome(src, std::chrono::milliseconds(500)) == kN);   // delivering at the new rate, straight away
            CHECK(readSome(src, std::chrono::milliseconds(500)) == kN);
        }
        CHECK(g_uhd.setups == 1 + 3);                  // the stream was re-created for each new rate
        CHECK(!src.faulted());
        CHECK(src.stallRestartsForTest() == 0);
    }

    // --- C2: another driver's stream is not re-created for a rate change (it never needed it) --------------------------------------------------------
    {
        g_stall.reset();
        SoapySource src;
        src.setStallWindowForTest(std::chrono::seconds(60));
        CHECK(src.open("driver=faketstall") && src.start());
        CHECK(src.setSampleRateHz(1.0e6));
        CHECK(readSome(src, std::chrono::seconds(2)) == kN);
        CHECK(g_stall.setups == 1);
    }

    // --- D: the saved row's volatile keys are not handed to a UHD open ---------------------------------------------------------------------------------
    {
        g_uhd.reset();
        {
            SoapySource src;
            CHECK(src.open("driver=uhd, label=B200 (usb 1-7), name=, product=B200, serial=ABC123, type=b200"));
            std::lock_guard<std::mutex> lk(g_uhd.m);
            CHECK(g_uhd.makeArgs.size() == 1);
            const SoapySDR::Kwargs seen = SoapySDR::KwargsFromString(g_uhd.makeArgs.empty() ? std::string() : g_uhd.makeArgs[0]);
            CHECK(seen.size() == 3);
            CHECK(seen.count("driver") == 1 && seen.at("driver") == "uhd");
            CHECK(seen.count("serial") == 1 && seen.at("serial") == "ABC123");
            CHECK(seen.count("type") == 1 && seen.at("type") == "b200");
            CHECK(seen.count("label") == 0 && seen.count("name") == 0 && seen.count("product") == 0);
        }
        g_uhd.reset();
        {
            // no serial: nothing to pick the board by, so the row goes through unchanged
            SoapySource src;
            CHECK(src.open("driver=uhd, label=B200, type=b200"));
            std::lock_guard<std::mutex> lk(g_uhd.m);
            const SoapySDR::Kwargs seen = SoapySDR::KwargsFromString(g_uhd.makeArgs.empty() ? std::string() : g_uhd.makeArgs[0]);
            CHECK(seen.count("label") == 1);
        }
        g_stall.reset();
        {
            // another driver is never reduced
            SoapySource src;
            CHECK(src.open("driver=faketstall, label=keep me, serial=XYZ"));
        }
    }

    g_stall.killAll = false;
    return testSummary("test_soapy_stall_restart");
}
