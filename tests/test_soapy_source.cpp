// Tests for source/soapy_source.hpp — the no-hardware surface.
//
// This machine may have the SoapySDR core library with ZERO vendor modules
// installed (vcpkg ships only the core), and no SDR attached even if a
// module were present. Every check below therefore targets behavior that
// must hold with nothing plugged in.
//
// ENUMERATION RUNS IN A CHILD PROCESS as of 0.62.1, and this file has to point
// at the helper explicitly - see the first block of main() for why, and for
// what the second-scan assertion had to become once a scan could legitimately
// come back empty. Everything else here (open, teardown, the no-device
// surface, the non-finite device) is still in-process and unchanged.
//
// The checks:
//   - enumerate() completes and is well-formed (any count, including 0);
//   - a bogus open() fails GRACEFULLY: false, nonempty lastError, no
//     half-open device state left behind;
//   - the whole IqSource surface is safe before open (documented no-ops);
//   - teardown is idempotent and the object leaks nothing observable
//     across 100 construct/destruct cycles.
//
// The graceful-failure block is the mutation target: live-stream behavior
// cannot be tested here, so open()'s failure path carries the suite's
// entire weight. It asserts open()==false AND lastError nonempty AND that
// start()/read() after the failed open still behave as "no device" — a
// mutant that swallows the failure and returns true trips all three.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/telemetry.hpp"
#include "source/soapy_enum_proc.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <utility>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test_check.hpp"

using cascade::source::SoapyDeviceInfo;
using cascade::source::SoapySource;

namespace {

// The application binary, which is NOT beside the test binaries: tests build
// into build/tests/Release, cascade.exe into build/Release. Empty if neither
// candidate exists, which the caller asserts against.
std::string findCascadeExe() {
#ifdef _WIN32
    std::wstring buf(1024, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) { return std::string(); }
    buf.resize(n);
    const std::filesystem::path dir = std::filesystem::path(buf).parent_path();
    const std::filesystem::path candidates[] = {
        dir / "cascade.exe",
        dir.parent_path().parent_path() / "Release" / "cascade.exe",
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c, ec)) { return c.string(); }
    }
#else
    // /proc/self/exe names this running binary regardless of how it was
    // invoked - see soapy_enum_proc.cpp's own enumerateHelperPath() for the
    // same reasoning. The single-config Ninja layout puts this test one
    // directory below the app (build/tests/ vs build/), not the two levels
    // the MSVC multi-config candidate above accounts for.
    char linkBuf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", linkBuf, sizeof(linkBuf) - 1);
    if (n > 0) {
        const std::filesystem::path self(std::string(linkBuf, static_cast<std::size_t>(n)));
        const std::filesystem::path candidate = self.parent_path().parent_path() / "cascade";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) { return candidate.string(); }
    }
#endif
    return std::string();
}

void setEnumHelper(const std::string& path) {
#ifdef _WIN32
    ::_putenv_s("CASCADE_ENUM_HELPER", path.c_str());
#else
    ::setenv("CASCADE_ENUM_HELPER", path.c_str(), 1);
#endif
}

// --- A registered SoapySDR device that hands back non-finite samples ---------
//
// Live-stream behaviour is otherwise untestable here (no radio, and vendor
// modules may not even be installed), but SoapySDR's device registry is a
// public in-process API: a factory registered from this file is found by
// SoapySDR::Device::make exactly like a driver module's, so SoapySource can be
// opened, started and read for real, through its own driver path, with no test
// seam added to the production class.
//
// Blocks ALTERNATE poisoned/clean. The clean one is what proves the guard is a
// filter and not a blanket rewrite: its samples must come back bit-exact.

constexpr float kCleanI = 0.25f;
constexpr float kCleanQ = -0.5f;

bool poisonedIndex(std::size_t i) { return (i % 7) == 0; }

class NonFiniteDevice : public SoapySDR::Device {
public:
    std::string getDriverKey() const override { return "fakenonfinite"; }
    std::string getHardwareKey() const override { return "fake non-finite source"; }
    size_t getNumChannels(const int) const override { return 1; }

    SoapySDR::Stream* setupStream(const int, const std::string&,
                                  const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        // The handle is opaque to SoapySource; any non-null value will do.
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long,
                       const size_t) override {
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        return 0;
    }
    double getSampleRate(const int, const size_t) const override { return 2.4e6; }
    // Tunable, so the serialisation block below can hammer retunes against
    // concurrent reads. Starts where the old fixed readback sat, so every
    // earlier block sees exactly the behaviour it always did. Deliberately a
    // PLAIN double touched by both the setter and readStream's caller:
    // SoapySource::devMutex_ is what makes that safe, which is the contract
    // under test.
    void setFrequency(const int, const size_t, const double frequency,
                      const SoapySDR::Kwargs&) override {
        freq_ = frequency;
    }
    double getFrequency(const int, const size_t) const override { return freq_; }

    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems,
                   int&, long long&, const long) override {
        float* p = static_cast<float*>(buffs[0]);
        const bool poison = (block_++ % 2) == 0;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        for (size_t i = 0; i < numElems; ++i) {
            p[2 * i] = kCleanI;
            p[2 * i + 1] = kCleanQ;
            if (poison && poisonedIndex(i)) {
                // NaN, +Inf and -Inf in turn: a sum-based detector that only
                // caught one of the three would pass every other block.
                const int which = static_cast<int>((i / 7) % 3);
                p[2 * i] = (which == 0) ? nan : ((which == 1) ? inf : -inf);
                p[2 * i + 1] = (which == 2) ? nan : -inf;
            }
        }
        return static_cast<int>(numElems);
    }

private:
    unsigned block_ = 0;
    double freq_ = 100.0e6;
};

// ---------------------------------------------------------------------------
// A DRIVER THAT IGNORES ITS READ TIMEOUT, which is the whole of field report
// 4214EAE4 (0.64.0, a Mirics device) reduced to something reproducible.
//
// SoapySource holds devMutex_ across readStream on purpose: with a bounded
// read, that bound IS the worst-case latency of a control call queued behind
// it. The bound belongs to the DRIVER, though, not to this code - readStream
// is handed 20 ms and a vendor module is free to ignore it. When one does, a
// GUI-thread stop() waiting on that mutex waits for ever, and the interface
// freezes with no way out but the task manager.
//
// This device sleeps far past its timeout so stop() must give up rather than
// wait. Red-green: with the bounded acquisition removed, stop() blocks for the
// whole sleep and the elapsed-time assertion below fails.
// ---------------------------------------------------------------------------
// See StallingDevice::readStream: true exactly while a caller is inside the
// stalled driver call (and therefore holding SoapySource's devMutex_).
std::atomic<bool> g_stallInRead{false};

class StallingDevice : public SoapySDR::Device {
public:
    std::string getDriverKey() const override { return "fakestall"; }
    std::string getHardwareKey() const override { return "fake stalling source"; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&,
                                  const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long,
                       const size_t) override {
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        return 0;
    }
    double getSampleRate(const int, const size_t) const override { return 2.4e6; }
    void setFrequency(const int, const size_t, const double f,
                      const SoapySDR::Kwargs&) override {
        freq_ = f;
    }
    double getFrequency(const int, const size_t) const override { return freq_; }

    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems,
                   int&, long long&, const long) override {
        // The rendezvous flag the TEST waits on. "The reader thread has
        // started" is not the fact the test needs - it needs "the reader
        // is inside the driver holding devMutex_", and only this function
        // can attest to that. A flag set on the thread's first line plus a
        // fixed sleep was the first version, and it loses to a single
        // >250 ms preemption in the gap before read() takes the lock: the
        // setter under test then acquires a FREE mutex, succeeds, and the
        // test fails red on perfectly correct code.
        g_stallInRead.store(true, std::memory_order_release);
        // The timeout argument is deliberately ignored - that is the fault
        // being imitated.
        std::this_thread::sleep_for(std::chrono::milliseconds(6000));
        float* p = static_cast<float*>(buffs[0]);
        for (size_t i = 0; i < 2 * numElems; ++i) { p[i] = 0.0f; }
        return static_cast<int>(numElems);
    }

private:
    double freq_ = 100.0e6;
};

SoapySDR::KwargsList findStall(const SoapySDR::Kwargs& args) {
    const auto driver = args.find("driver");
    if (driver != args.end() && driver->second != "fakestall") { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = "fakestall";
    k["label"] = "fake stalling source";
    return SoapySDR::KwargsList{k};
}

SoapySDR::Device* makeStall(const SoapySDR::Kwargs&) { return new StallingDevice(); }

// A device that answers reads from a script: a positive count delivers that
// many zero samples, anything else is returned to the caller as the driver's
// answer. Past the end of the script it delivers a full block, so a test that
// over-reads still sees a live radio rather than a hang.
std::vector<int> g_healthScript;
std::size_t g_healthAt = 0;
class HealthDevice : public SoapySDR::Device {
public:
    std::string getDriverKey() const override { return "fakehealth"; }
    std::string getHardwareKey() const override { return "fake scripted source"; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&,
                                  const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long,
                       const size_t) override {
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        return 0;
    }
    double getSampleRate(const int, const size_t) const override { return 2.4e6; }
    void setFrequency(const int, const size_t, const double f,
                      const SoapySDR::Kwargs&) override {
        freq_ = f;
    }
    double getFrequency(const int, const size_t) const override { return freq_; }
    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems,
                   int&, long long&, const long) override {
        int answer = static_cast<int>(numElems);
        if (g_healthAt < g_healthScript.size()) { answer = g_healthScript[g_healthAt++]; }
        if (answer > 0) {
            const size_t n = std::min(static_cast<size_t>(answer), numElems);
            float* p = static_cast<float*>(buffs[0]);
            for (size_t i = 0; i < 2 * n; ++i) { p[i] = 0.0f; }
            return static_cast<int>(n);
        }
        return answer;
    }

private:
    double freq_ = 100.0e6;
};
SoapySDR::KwargsList findHealth(const SoapySDR::Kwargs& args) {
    const auto driver = args.find("driver");
    if (driver != args.end() && driver->second != "fakehealth") { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = "fakehealth";
    k["label"] = "fake scripted source";
    return SoapySDR::KwargsList{k};
}
SoapySDR::Device* makeHealth(const SoapySDR::Kwargs&) { return new HealthDevice(); }

// ---------------------------------------------------------------------------
// A DRIVER WHOSE ESCAPE-PATH CALLS NEVER COME BACK, which is the 0.70.0 field
// freeze reduced to something reproducible.
//
// The stalling device above wedges a READ, and what that proves is that a
// control call gives up waiting for the LOCK. This one wedges the calls the
// escape paths make themselves - deactivateStream out of stop(), closeStream
// out of closeDevice() - which is the half the lock bound cannot cover: the
// caller has the lock, and it is the driver that will not return. The field
// report's stack symbolises to the single _Thrd_join call site in
// rtlsdrSupport.dll, inside the function that calls rtlsdr_cancel_async,
// _Thrd_id and _Thrd_join in that order: SoapyRTLSDR::deactivateStream joining
// its own async RX thread, on the GUI thread, for ever.
//
// The wedge has a HARD 20 s CAP so a regression fails with a named assertion
// instead of hanging the suite to its 120 s timeout - far past the 1.5 s bound
// under test, far inside the suite's limit. Red-green: raising the bound in
// soapy_source.cpp past that cap (the mutation that deletes the fix) makes
// stop() wait the full 20 s and the elapsed-time, dead-latch and
// abandoned-count checks below all fail.
// ---------------------------------------------------------------------------
std::mutex g_wedgeMutex;
std::condition_variable g_wedgeCv;
bool g_wedgeReleased = false;               // guarded by g_wedgeMutex

std::atomic<bool> g_wedgeDeactivate{false};  // set before open(), not during
std::atomic<bool> g_wedgeCloseStream{false};
// THE LATE ANSWERS (0.99.59). SoapySDRPlay3's setFrequency on a live stream
// calls sdrplay_api_Update and then waits up to 500 x sleep_for(1 ms) for the
// service's callback; its activateStream is sdrplay_api_Init. With the SDRplay
// API service restarted under a live stream, both took seconds on the GUI
// thread and then RETURNED (hang reports 6F550354218029F0, 9B804643C56308CF,
// 0.99.56) - Init with sdrplay_api_AlreadyInitialised, which SoapySDRPlay
// answers as SOAPY_SDR_NOT_SUPPORTED. These wedge the same way as the two
// above, and the activation answers g_activateRet once let go.
std::atomic<bool> g_wedgeSetFrequency{false};
std::atomic<bool> g_wedgeActivate{false};
std::atomic<int> g_activateRet{0};
std::atomic<bool> g_inWedgedCall{false};     // the driver really was entered
std::atomic<int> g_wedgedCallsReturned{0};
std::atomic<int> g_closeStreamCalls{0};
std::atomic<int> g_deviceDestroyed{0};       // unmake() would delete the fake
// Every entry into the fake's readStream. This is what tells a read() that
// gave up at the door from one that went into a driver it had been told never
// to touch again - a timing or return-value assertion cannot, because a
// wedged-then-abandoned device still answers a read perfectly happily.
std::atomic<int> g_readStreamCalls{0};
// One per open() that reached the device interrogation. Counted on the DEVICE
// rather than on the factory because SoapySDR::Device::make keys a cache on
// the resolved kwargs and hands back the same instance for args that resolve
// alike - so a factory counter says nothing about how many opens got through.
std::atomic<int> g_setupStreamCalls{0};

void resetWedge() {
    {
        std::lock_guard<std::mutex> lk(g_wedgeMutex);
        g_wedgeReleased = false;
    }
    g_wedgeDeactivate.store(false, std::memory_order_relaxed);
    g_wedgeCloseStream.store(false, std::memory_order_relaxed);
    g_wedgeSetFrequency.store(false, std::memory_order_relaxed);
    g_wedgeActivate.store(false, std::memory_order_relaxed);
    g_activateRet.store(0, std::memory_order_relaxed);
    g_inWedgedCall.store(false, std::memory_order_relaxed);
    g_wedgedCallsReturned.store(0, std::memory_order_relaxed);
    g_closeStreamCalls.store(0, std::memory_order_relaxed);
    g_deviceDestroyed.store(0, std::memory_order_relaxed);
    g_setupStreamCalls.store(0, std::memory_order_relaxed);
    g_readStreamCalls.store(0, std::memory_order_relaxed);
}

void releaseWedge() {
    {
        std::lock_guard<std::mutex> lk(g_wedgeMutex);
        g_wedgeReleased = true;
    }
    g_wedgeCv.notify_all();
}

// Blocks inside the vendor call until the test lets go (or the cap expires).
// The counter is bumped only AFTER every test-owned object has been released,
// so a worker the test has stopped waiting for touches nothing of this file's
// once the count the test polls has moved.
void wedgeUntilReleased() {
    g_inWedgedCall.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(g_wedgeMutex);
        g_wedgeCv.wait_for(lk, std::chrono::seconds(20), [] { return g_wedgeReleased; });
    }
    g_wedgedCallsReturned.fetch_add(1, std::memory_order_release);
}

class WedgingDevice : public SoapySDR::Device {
public:
    ~WedgingDevice() override { g_deviceDestroyed.fetch_add(1, std::memory_order_release); }
    std::string getDriverKey() const override { return "fakewedge"; }
    std::string getHardwareKey() const override { return "fake wedging source"; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&,
                                  const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        g_setupStreamCalls.fetch_add(1, std::memory_order_release);
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {
        g_closeStreamCalls.fetch_add(1, std::memory_order_release);
        if (g_wedgeCloseStream.load(std::memory_order_relaxed)) { wedgeUntilReleased(); }
    }
    int activateStream(SoapySDR::Stream*, const int, const long long,
                       const size_t) override {
        if (g_wedgeActivate.load(std::memory_order_relaxed)) {
            // Read BEFORE the wedge's return is counted: once the count the
            // test polls has moved, nothing of this file may be touched.
            const int ret = g_activateRet.load(std::memory_order_relaxed);
            wedgeUntilReleased();
            return ret;
        }
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        if (g_wedgeDeactivate.load(std::memory_order_relaxed)) { wedgeUntilReleased(); }
        return 0;
    }
    double getSampleRate(const int, const size_t) const override { return 2.4e6; }
    void setFrequency(const int, const size_t, const double f,
                      const SoapySDR::Kwargs&) override {
        // The frequency is programmed first, as SoapySDRPlay3 writes rfHz
        // before it asks the service and waits.
        freq_ = f;
        if (g_wedgeSetFrequency.load(std::memory_order_relaxed)) { wedgeUntilReleased(); }
    }
    double getFrequency(const int, const size_t) const override { return freq_; }
    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems,
                   int&, long long&, const long) override {
        g_readStreamCalls.fetch_add(1, std::memory_order_release);
        float* p = static_cast<float*>(buffs[0]);
        for (size_t i = 0; i < 2 * numElems; ++i) { p[i] = 0.0f; }
        return static_cast<int>(numElems);
    }

private:
    double freq_ = 100.0e6;
};

SoapySDR::KwargsList findWedge(const SoapySDR::Kwargs& args) {
    const auto driver = args.find("driver");
    if (driver != args.end() && driver->second != "fakewedge") { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = "fakewedge";
    k["label"] = "fake wedging source";
    return SoapySDR::KwargsList{k};
}

SoapySDR::Device* makeWedge(const SoapySDR::Kwargs&) { return new WedgingDevice(); }

// ---------------------------------------------------------------------------
// A DRIVER THAT LISTS WHATEVER IT IS NAMED IN, which is SoapyRedPitaya's find
// function reduced to its one rule (pothosware/SoapyRedPitaya, findSoapyRedPitaya):
// asked with driver=<its own name> it hands the caller's kwargs straight back
// ("TODO perform a test connection to validate device presence"); asked with
// empty kwargs - the whole-bus walk - it finds nothing. Hang report
// 40002A91C26F3C07 (0.99.58) is a user picking the row that rule made, during
// a scan beside an open LimeSDR, and freezing the interface in its connect.
// ---------------------------------------------------------------------------
std::atomic<int> g_echoFinds{0};  // the find function really was asked

SoapySDR::KwargsList findEcho(const SoapySDR::Kwargs& args) {
    g_echoFinds.fetch_add(1, std::memory_order_relaxed);
    const auto driver = args.find("driver");
    if (driver != args.end() && driver->second == "fakeecho") {
        return SoapySDR::KwargsList{args};
    }
    return {};
}

SoapySDR::Device* makeEcho(const SoapySDR::Kwargs&) { return new WedgingDevice(); }

// True once the abandoned driver call has come back out of the vendor module,
// or false if it never does. Bounded so a broken release cannot hang the suite.
bool waitForWedgedReturn(int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (g_wedgedCallsReturned.load(std::memory_order_acquire) < expected) {
        if (std::chrono::steady_clock::now() > deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

SoapySDR::KwargsList findNonFinite(const SoapySDR::Kwargs& args) {
    const auto driver = args.find("driver");
    if (driver != args.end() && driver->second != "fakenonfinite") { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = "fakenonfinite";
    k["label"] = "fake non-finite source";
    return SoapySDR::KwargsList{k};
}

SoapySDR::Device* makeNonFinite(const SoapySDR::Kwargs&) {
    return new NonFiniteDevice();  // owned by SoapySDR, released by unmake()
}

// ---------------------------------------------------------------------------
// A DRIVER THAT RECORDS THE ORDER OF ITS STREAM AND RATE CALLS, which is the
// 0.88.0 field crash (signature 235E46B5D39DED8D, an RTL-SDR NESDR SMArt v5)
// reduced to the one fact that decides it.
//
// The user's radio was streaming at 2 MS/s; the ADS-B preset asked for
// 2.4 MS/s, and SoapySource set it on the running device. SoapyRTLSDR's
// setSampleRate then reprograms the dongle while its own async reader thread
// is inside rtlsdr_read_async with USB transfers in flight, and 45 s later
// that thread died in ntdll on a lock libusb had already freed. No frame of
// ours was on the stack, so nothing of ours could absorb it. The driver's
// deactivateStream joins that thread and activateStream starts a fresh one,
// so a rate set BETWEEN them lands on a quiescent device - which is what this
// fake asserts SoapySource now does, by recording what it was told and in
// what order.
//
// The fake keeps a running flag of its own and reads it back through the
// stream: setSampleRate while active is recorded as "setSampleRate(LIVE)" so
// the order assertion fails on the exact fault rather than on a count, and
// readStream delivers nothing while inactive so a stream that was never
// reactivated is caught by the read that follows the change, not by trust.
// ---------------------------------------------------------------------------
std::vector<std::string> g_rateCalls;   // every stream/rate call, in order
bool g_rateRejectNext = false;          // setSampleRate throws once, as
                                        // SoapyRTLSDR does for a rate the
                                        // dongle cannot make
bool g_rateRefuseActivate = false;      // activateStream answers an error
                                        // code instead of 0

class RateOrderDevice : public SoapySDR::Device {
public:
    std::string getDriverKey() const override { return "fakerateorder"; }
    std::string getHardwareKey() const override { return "fake rate-order source"; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&,
                                  const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long,
                       const size_t) override {
        g_rateCalls.push_back(active_ ? "activateStream(ALREADY)" : "activateStream");
        if (g_rateRefuseActivate) {
            g_rateRefuseActivate = false;
            return SOAPY_SDR_STREAM_ERROR;
        }
        active_ = true;
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        g_rateCalls.push_back(active_ ? "deactivateStream" : "deactivateStream(IDLE)");
        active_ = false;
        return 0;
    }
    double getSampleRate(const int, const size_t) const override { return rate_; }
    void setSampleRate(const int, const size_t, const double hz) override {
        g_rateCalls.push_back(active_ ? "setSampleRate(LIVE)" : "setSampleRate");
        if (g_rateRejectNext) {
            g_rateRejectNext = false;
            throw std::runtime_error(
                "setSampleRate failed: RTL-SDR does not support this sample rate");
        }
        // COERCED, not echoed, the way a real tuner lands on its nearest
        // clock division: whole kilohertz. The readback assertion below is
        // then a readback and not an echo of the request.
        rate_ = std::round(hz / 1000.0) * 1000.0;
    }
    void setFrequency(const int, const size_t, const double f,
                      const SoapySDR::Kwargs&) override {
        freq_ = f;
    }
    double getFrequency(const int, const size_t) const override { return freq_; }
    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems,
                   int&, long long&, const long) override {
        if (!active_) { return SOAPY_SDR_TIMEOUT; }  // nothing flows when stopped
        float* p = static_cast<float*>(buffs[0]);
        for (size_t i = 0; i < 2 * numElems; ++i) { p[i] = 0.5f; }
        return static_cast<int>(numElems);
    }

private:
    bool active_ = false;
    double rate_ = 2.0e6;   // the field radio's rate at the moment of the crash
    double freq_ = 100.0e6;
};

// ---------------------------------------------------------------------------
// A DRIVER WITH A REAL SHAPE: named gain stages with DIFFERENT ranges, two
// antenna ports, and a list of sample rates.
//
// This is what SoapySource has to answer the Source panel with now that the
// panel is written against DeviceSource. Everything below it used to be
// hard-wired in the GUI - every gain slider 0..60 dB whatever the stage,
// every Rate combo 1/2/4/8 MS/s whatever the radio - and because SoapySDR
// clamps silently, a panel that asked for 60 dB of a 16 dB mixer got 16 and
// said 60. The ranges here are deliberately unequal and deliberately include
// a NEGATIVE minimum (an RTL-SDR's VGA starts at -4.7 dB), because a
// fallback-to-0..60 bug passes against any range that happens to fit inside
// it.
// ---------------------------------------------------------------------------
std::string g_shapeAntenna = "RX2";
std::vector<std::pair<std::string, double>> g_shapeGains;  // what was SET, in order

class ShapeDevice : public SoapySDR::Device {
public:
    std::string getDriverKey() const override { return "fakeshape"; }
    std::string getHardwareKey() const override { return "fake shaped source"; }
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
    double getSampleRate(const int, const size_t) const override { return rate_; }
    void setSampleRate(const int, const size_t, const double hz) override { rate_ = hz; }
    std::vector<double> listSampleRates(const int, const size_t) const override {
        return {250000.0, 1024000.0, 2048000.0, 2400000.0, 3200000.0};
    }
    std::vector<std::string> listGains(const int, const size_t) const override {
        return {"LNA", "MIXER", "VGA"};
    }
    SoapySDR::Range getGainRange(const int, const size_t,
                                 const std::string& name) const override {
        if (name == "LNA") { return SoapySDR::Range(0.0, 33.5, 0.1); }
        if (name == "MIXER") { return SoapySDR::Range(0.0, 16.1, 0.1); }
        return SoapySDR::Range(-4.7, 40.8, 0.1);
    }
    void setGain(const int, const size_t, const std::string& name, const double db) override {
        g_shapeGains.push_back({name, db});
        // COERCED, exactly as a real driver coerces: clamped into range and
        // then rounded to a whole decibel. gainDb() must report THIS, not the
        // request - the readback promise the antenna combo has kept since it
        // was added, now extended to the gains.
        const SoapySDR::Range r = getGainRange(0, 0, name);
        double v = db < r.minimum() ? r.minimum() : (db > r.maximum() ? r.maximum() : db);
        v = static_cast<double>(static_cast<long long>(v));
        gains_[name] = v;
    }
    double getGain(const int, const size_t, const std::string& name) const override {
        const auto it = gains_.find(name);
        return it == gains_.end() ? 0.0 : it->second;
    }
    bool hasGainMode(const int, const size_t) const override { return true; }
    void setGainMode(const int, const size_t, const bool on) override { agc_ = on; }
    bool getGainMode(const int, const size_t) const override { return agc_; }
    std::vector<std::string> listAntennas(const int, const size_t) const override {
        return {"TX/RX", "RX2"};
    }
    void setAntenna(const int, const size_t, const std::string& name) override {
        g_shapeAntenna = name;
    }
    std::string getAntenna(const int, const size_t) const override { return g_shapeAntenna; }
    int readStream(SoapySDR::Stream*, void* const*, const size_t, int&, long long&,
                   const long) override {
        return 0;
    }

private:
    double rate_ = 2.0e6;
    bool agc_ = false;
    mutable std::map<std::string, double> gains_;
};
SoapySDR::KwargsList findShape(const SoapySDR::Kwargs& args) {
    const auto driver = args.find("driver");
    if (driver != args.end() && driver->second != "fakeshape") { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = "fakeshape";
    k["label"] = "fake shaped source";
    return SoapySDR::KwargsList{k};
}
SoapySDR::Device* makeShape(const SoapySDR::Kwargs&) { return new ShapeDevice(); }

SoapySDR::KwargsList findRateOrder(const SoapySDR::Kwargs& args) {
    const auto driver = args.find("driver");
    if (driver != args.end() && driver->second != "fakerateorder") { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = "fakerateorder";
    k["label"] = "fake rate-order source";
    return SoapySDR::KwargsList{k};
}

SoapySDR::Device* makeRateOrder(const SoapySDR::Kwargs&) { return new RateOrderDevice(); }

// A STAND-IN FOR SOAPYAUDIO'S "audio" DRIVER that counts every time it is asked
// - to find or to make anything. The real one lists sound cards through
// RtAudio, whose ASIO back end loads every ASIO driver on the machine; on the
// 2026-10-01 field machine one of them faulted (crash reports
// 4138700E14D784C6, 3C2F1A0F27A8FD35).
std::atomic<int> g_audioAsked{0};

SoapySDR::KwargsList findAudioTrap(const SoapySDR::Kwargs&) {
    ++g_audioAsked;
    return {};
}

SoapySDR::Device* makeAudioTrap(const SoapySDR::Kwargs&) {
    ++g_audioAsked;
    return nullptr;
}

// True when the NEWEST line in the diagnostics ring carries `text`. The ring
// is what a crash report is flushed from, so "newest" is exactly the line the
// next report of this shape would show last before the fault.
bool lastDiagLine(const char* text) {
    const std::vector<std::string> ring = cascade::core::DiagLog::instance().ringSnapshot();
    if (ring.empty()) {
        std::printf("  diag ring is empty\n");
        return false;
    }
    const bool hit = ring.back().find(text) != std::string::npos;
    if (!hit) { std::printf("  newest diag line: \"%s\"\n", ring.back().c_str()); }
    return hit;
}

// True when ANY line in the diagnostics ring carries `text` - for a line that
// is written on another thread (a late answer reaped by a read) and may be
// followed by others before the test looks.
bool diagRingHas(const char* text) {
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(text) != std::string::npos) { return true; }
    }
    return false;
}

// Reads until `done` holds or `ms` has passed, as the pipeline's source loop
// would - a late answer is reaped by the read loop when nothing else asks.
template <class Pred>
bool readUntil(SoapySource& src, Pred done, long long ms) {
    std::vector<std::complex<float>> buf(256);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) { return false; }
        (void)src.read(buf.data(), buf.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// The recorded sequence as one line, for the failure printout.
std::string joinCalls(const std::vector<std::string>& calls) {
    std::string out;
    for (const std::string& c : calls) {
        if (!out.empty()) { out += ", "; }
        out += c;
    }
    return out.empty() ? std::string("(none)") : out;
}

}  // namespace

int main() {
    // --- enumerate(): completes, well-formed, repeatable ---------------------
    //
    // POINTED AT THE REAL HELPER FIRST, and that is what stops this block
    // killing its own process.
    //
    // SoapySource::enumerate() now walks the bus in a child (see
    // source/soapy_enum_proc.hpp), and this machine's libusb fault - 0xC0000005
    // about one enumeration in twenty with a B200 attached - used to land in
    // THIS process: test_soapy_source.exe died outright in 1 run of 40. The
    // helper it would resolve by default is cascade.exe beside the running
    // executable, and test binaries build into build/tests/Release while
    // cascade.exe builds into build/Release, so without this the call would
    // fall back to walking the bus in-process and inherit the fault again.
    // Asserted rather than skipped: a build tree that has moved must fail here
    // loudly, not quietly revert to the crashy path.
    {
        const std::string helper = findCascadeExe();
        CHECK(!helper.empty());
        setEnumHelper(helper);
    }
    {
        const std::vector<SoapyDeviceInfo> devices = SoapySource::enumerate();
        std::printf("soapy devices: %zu\n", devices.size());
        for (const SoapyDeviceInfo& d : devices) {
            std::printf("  label=\"%s\" args=\"%s\"\n", d.label.c_str(),
                        d.args.c_str());
            // Every row the Source menu would show must be displayable and
            // reopenable; vacuous when the list is empty (the normal
            // no-modules answer), asserted for real when hardware exists.
            CHECK(!d.label.empty());
            CHECK(!d.args.empty());
        }
        // A second scan must not crash or throw either (the menu Refresh
        // path), and it must reach the helper rather than failing to start it.
        //
        // WHAT THIS NO LONGER ASSERTS, and why removing it is a fix rather
        // than a weakening: it used to require again.size() == devices.size().
        // That is an assertion about the hardware's mood, not about this code.
        // UHD's device discovery runs over UDP and this bench logs
        // "Device discovery error: receive_from ... forcibly closed" on most
        // runs, so two scans seconds apart can legitimately see different
        // populations within one process - and now that a scan runs in a
        // child, one of the two can also come back empty because its child hit
        // the libusb fault, which is the fix working exactly as designed. What
        // is asserted instead is everything that IS about this code: the
        // helper was startable, its answer parsed, and every row is usable.
        const cascade::source::EnumResult again = cascade::source::enumerateIsolated();
        std::printf("soapy rescan: outcome=%s devices=%zu\n",
                    cascade::source::enumOutcomeName(again.outcome), again.devices.size());
        CHECK(again.outcome != cascade::source::EnumOutcome::SpawnFailed);
        CHECK(again.outcome != cascade::source::EnumOutcome::Malformed);
        CHECK(!again.fellBackInProcess);
        for (const SoapyDeviceInfo& d : again.devices) {
            CHECK(!d.label.empty());
            CHECK(!d.args.empty());
        }
    }

    // --- entire surface is safe before open (documented no-op returns) ------
    {
        SoapySource src;
        CHECK(src.selfPaced());              // hardware family, by contract
        CHECK(!src.isOpen());
        CHECK(!src.running());
        CHECK(src.sampleRateHz() == 0.0);    // documented "until open" value
        CHECK(src.centerFrequencyHz() == 0.0);
        CHECK(src.name() != nullptr);
        CHECK(std::strcmp(src.name(), "SoapySDR: (no device)") == 0);
        CHECK(src.lastError() != nullptr);   // never nullptr, empty when none
        CHECK(std::strlen(src.lastError()) == 0);
        CHECK(!src.faulted());               // nothing to fault on yet

        // read() with no stream: immediate 0 (retry signal), dst untouched.
        std::complex<float> buf[8] = {};
        buf[0] = {123.0f, -123.0f};  // sentinel proves no write happened
        CHECK(src.read(buf, 8) == 0u);
        // ...and that 0 must NOT be a fault. The pipeline stops the whole
        // source on faulted(), so a no-device read that raised it would turn
        // "nothing plugged in" into a hard stop with no way back.
        CHECK(!src.faulted());
        CHECK(buf[0] == std::complex<float>(123.0f, -123.0f));
        CHECK(src.read(nullptr, 8) == 0u);   // defensive null: no crash
        CHECK(src.read(buf, 0) == 0u);

        // start() refuses with a reason; stop() is a silent no-op.
        CHECK(!src.start());
        CHECK(std::strlen(src.lastError()) > 0);
        CHECK(!src.running());
        src.stop();
        CHECK(!src.running());

        // Setters cannot reach a device: false, and the cached readbacks
        // stay at their no-device values.
        CHECK(!src.setSampleRateHz(2.4e6));
        CHECK(src.sampleRateHz() == 0.0);
        CHECK(!src.setCenterFrequencyHz(100.0e6));
        CHECK(src.centerFrequencyHz() == 0.0);

        // Gain hooks: empty/false, never a throw.
        CHECK(src.listGainNames().empty());
        CHECK(!src.setGainDb("PGA", 10.0));
        CHECK(!src.setAutoGain(true));

        // closeDevice() before any open, twice: idempotent teardown.
        src.closeDevice();
        CHECK(!src.isOpen());
        src.closeDevice();
        CHECK(!src.isOpen());
        CHECK(!src.running());
    }

    // --- bogus open(): graceful failure, no half-open state (mutant target) --
    {
        SoapySource src;
        const bool opened = src.open("driver=definitely_not_real_xyz");
        std::printf("bogus open: %s, lastError=\"%s\"\n",
                    opened ? "true" : "false", src.lastError());
        CHECK(!opened);                            // the lie a mutant would tell
        CHECK(std::strlen(src.lastError()) > 0);   // reason must be readable
        CHECK(!src.isOpen());
        CHECK(!src.running());
        CHECK(std::strcmp(src.name(), "SoapySDR: (no device)") == 0);
        CHECK(src.sampleRateHz() == 0.0);

        // If open() lied (returned true with no device), the object would
        // now be driven like a live source — these calls must still behave
        // as "no device", not crash on a null handle.
        CHECK(!src.start());
        CHECK(std::strlen(src.lastError()) > 0);
        CHECK(!src.running());
        std::complex<float> buf[16] = {};
        CHECK(src.read(buf, 16) == 0u);
        CHECK(!src.setSampleRateHz(1.0e6));
        // A failed OPEN is not a stream fault: there is no stream. faulted()
        // reports only a device lost mid-capture, which is what makes it safe
        // for the pipeline to treat as "stop everything".
        CHECK(!src.faulted());

        // The failure reason must survive the teardown that follows it —
        // the GUI shows lastError() after closing the half-open attempt.
        src.closeDevice();
        CHECK(std::strlen(src.lastError()) > 0);

        // A second bogus open exercises the close-then-reopen path.
        CHECK(!src.open("driver=definitely_not_real_xyz, serial=0000"));
        CHECK(std::strlen(src.lastError()) > 0);
        CHECK(!src.isOpen());

        // Destructor of a failed-open instance runs at scope exit — must be
        // clean (covered again in bulk below).
    }

    // --- a SoapySDR sound card is never opened (2026-10-04) ------------------
    //
    // The device scan never asks SoapySDR's "audio" driver
    // (source/soapy_enum_proc.cpp, neverAsked), and the application drops every
    // driver=audio row from the Source list and refuses a saved one on open
    // (gui/app_window.cpp, isAudioDriver) - but a PATCH radio names its device
    // by args from a saved patch and opens it here directly. Device::make runs
    // the named driver's find function, so a patch from a build that listed
    // sound cards would have asked the ASIO-walking driver in the application's
    // own process. open() refuses it, in words that say what to use instead,
    // before the driver is asked anything. Sound cards are the "Sound card"
    // source.
    {
        g_audioAsked = 0;
        SoapySDR::Registry reg("audio", &findAudioTrap, &makeAudioTrap, SOAPY_SDR_ABI_VERSION);
        SoapySource src;
        const bool opened = src.open("driver=audio,device_id=0");
        std::printf("audio open: %s, asked=%d, lastError=\"%s\"\n", opened ? "true" : "false",
                    g_audioAsked.load(), src.lastError());
        CHECK(!opened);
        CHECK(g_audioAsked.load() == 0);  // the driver was not asked to find or make
        CHECK(std::strstr(src.lastError(), "sound card") != nullptr);
        CHECK(!src.isOpen());
        // The same refusal however the key is spelled and spaced.
        CHECK(!src.open(" Driver = Audio , device_id=1"));
        CHECK(g_audioAsked.load() == 0);
        // A driver that merely begins with the name is not the sound card's.
        CHECK(!src.open("driver=audiofoo"));
        CHECK(std::strstr(src.lastError(), "sound card") == nullptr);
    }

    // --- 100x construct/destruct: nothing observable leaks or crashes -------
    {
        // No open() in this loop, so no device/module state accumulates; a
        // leak of any per-instance Soapy resource (log handlers, registry
        // entries) or a teardown crash would surface across the cycles.
        for (int i = 0; i < 100; ++i) {
            SoapySource src;
            CHECK(src.selfPaced());
            CHECK(!src.running());
            if ((i % 3) == 0) {
                src.closeDevice();  // teardown-before-open every 3rd cycle
            }
            if ((i % 7) == 0) {
                src.stop();         // and a stray stop() every 7th
            }
        }
        // Survival to here IS the assertion; mark it so the check count
        // reflects that the loop ran.
        CHECK(true);

        // enumerate() still works after the churn (no global state broken).
        const std::vector<SoapyDeviceInfo> devices = SoapySource::enumerate();
        std::printf("soapy devices after churn: %zu\n", devices.size());
    }

    // --- one driver asked alone lists what the whole bus would list ---------
    //
    // The per-driver child (--driver=<name>: the scan beside an open radio,
    // and the sweep after a whole-bus death) used to ask
    // Device::enumerate("driver=<name>"), and SoapySDR hands that kwarg to the
    // find function itself. A module that reads it as "the user named me"
    // then lists a device it never probed - the phantom "redpitaya" row of
    // hang report 40002A91C26F3C07. Red-green: restoring the driver-keyed ask
    // in enumerateInProcess(driver) makes the fakeecho row reappear here.
    // Above the first abandonment on purpose: see the note on the open-device
    // count block below.
    {
        std::printf("--- one driver asked alone: no phantom rows ---\n");
        SoapySDR::Registry echo("fakeecho", &findEcho, &makeEcho, SOAPY_SDR_ABI_VERSION);
        SoapySDR::Registry wedge("fakewedge", &findWedge, &makeWedge, SOAPY_SDR_ABI_VERSION);
        CHECK(!SoapySource::anyDeviceOpen());  // or the walk is refused and proves nothing

        g_echoFinds.store(0, std::memory_order_relaxed);
        const std::vector<SoapyDeviceInfo> alone = SoapySource::enumerateInProcess("fakeecho");
        std::printf("  fakeecho asked alone: %zu row(s)%s%s\n", alone.size(),
                    alone.empty() ? "" : ", first args=", alone.empty() ? "" : alone.front().args.c_str());
        // It really was asked - an empty list from a walk that never reached
        // the module would pass the next check for the wrong reason.
        CHECK(g_echoFinds.load(std::memory_order_relaxed) == 1);
        CHECK(alone.empty());

        // The whole-bus walk agrees: nothing from fakeecho there either.
        bool echoOnBus = false;
        for (const SoapyDeviceInfo& d : SoapySource::enumerateInProcess()) {
            if (d.args.find("fakeecho") != std::string::npos) { echoOnBus = true; }
        }
        CHECK(!echoOnBus);

        // HAPPY PATH: a driver that finds a device with empty kwargs still
        // lists it when asked alone, under its own driver key - the sweep
        // after a whole-bus death must still find the radios.
        const std::vector<SoapyDeviceInfo> real = SoapySource::enumerateInProcess("fakewedge");
        std::vector<std::string> realArgs;
        for (const SoapyDeviceInfo& d : real) { realArgs.push_back(d.args + " | " + d.label); }
        CHECK(realArgs == (std::vector<std::string>{
                              "driver=fakewedge, label=fake wedging source | fake wedging source"}));

        // A name no module registered lists nothing and does not throw.
        CHECK(SoapySource::enumerateInProcess("no-such-driver").empty());
    }

    // --- a device that delivers non-finite samples ---------------------------
    //
    // The entry point of the HARDWARE path, the counterpart of the sanitise
    // source/iq_file_source does for the file path. A driver can deliver NaN
    // or an infinity — a half-initialised buffer, a converter fed a malformed
    // packet, a device coming apart on the USB bus — and one such sample
    // latches the AGC gain, the squelch EMA and the noise reducer's spectrum
    // downstream, which is heard as the receiver going silent while the
    // spectrum display stays alive.
    {
        // Registered for this block only, so the enumerate() assertions above
        // and any later test see the machine's real device population.
        SoapySDR::Registry reg("fakenonfinite", &findNonFinite, &makeNonFinite,
                               SOAPY_SDR_ABI_VERSION);
        SoapySource src;
        const bool opened = src.open("driver=fakenonfinite");
        std::printf("fake device open: %s (%s)\n", opened ? "true" : "false",
                    src.name());
        CHECK(opened);
        if (opened) {
            // The device under test really is the fake, not something the
            // machine happens to have plugged in.
            CHECK(std::strcmp(src.name(), "SoapySDR: fake non-finite source") == 0);
            CHECK(src.start());

            constexpr std::size_t kN = 512;
            std::vector<std::complex<float>> buf(kN);
            CHECK(src.read(buf.data(), kN) == kN);

            std::size_t nonFinite = 0;
            std::size_t wrongSilence = 0;
            std::size_t cleanMangled = 0;
            for (std::size_t i = 0; i < kN; ++i) {
                if (!std::isfinite(buf[i].real()) || !std::isfinite(buf[i].imag())) {
                    ++nonFinite;
                } else if (poisonedIndex(i)) {
                    // Scrubbed to true silence, not to some other number.
                    if (buf[i] != std::complex<float>(0.0f, 0.0f)) { ++wrongSilence; }
                } else if (buf[i] != std::complex<float>(kCleanI, kCleanQ)) {
                    ++cleanMangled;
                }
            }
            std::printf("poisoned block: nonFinite=%zu wrongSilence=%zu "
                        "cleanMangled=%zu lastError=\"%s\"\n",
                        nonFinite, wrongSilence, cleanMangled, src.lastError());
            CHECK(nonFinite == 0u);      // nothing non-finite leaves read()
            CHECK(wrongSilence == 0u);   // and what was non-finite is silence
            CHECK(cleanMangled == 0u);   // neighbours in the same block untouched

            // Reported, because a radio delivering NaN is worth seeing in the
            // GUI — but NOT faulted: the samples arrived, the device is alive,
            // and stopping the source would be a worse answer than silencing
            // one block.
            CHECK(std::strlen(src.lastError()) > 0);
            CHECK(!src.faulted());
            CHECK(src.running());

            // The next block is clean, and must come back BIT-EXACT: the
            // guard is a filter on non-finite values, not a rewrite of the
            // stream.
            std::vector<std::complex<float>> clean(kN);
            CHECK(src.read(clean.data(), kN) == kN);
            std::size_t cleanMismatch = 0;
            for (std::size_t i = 0; i < kN; ++i) {
                if (clean[i] != std::complex<float>(kCleanI, kCleanQ)) {
                    ++cleanMismatch;
                }
            }
            CHECK(cleanMismatch == 0u);
            CHECK(!src.faulted());

            src.stop();
            src.closeDevice();
            CHECK(!src.isOpen());
        }
    }

    // --- Serialisation: one thread in the vendor stack, ever -----------------
    //
    // The three 0.62.0 field crashes were adjudicated to unserialised entry
    // into the vendor driver: a GUI-thread retune concurrent with the source
    // thread inside readStream(), plus in-process enumeration touching the
    // open dongle. This block drives exactly those pairs against the fake
    // driver: a reader thread hammers read() while this thread hammers
    // retunes, queries and readouts. A regression to unserialised access is a
    // data race on the fake's plain `freq_`/`block_` members; a lock bug is a
    // deadlock, which the suite's 120 s timeout converts into a failure.
    {
        SoapySDR::Registry reg("fakenonfinite", &findNonFinite, &makeNonFinite,
                               SOAPY_SDR_ABI_VERSION);
        SoapySource src;
        CHECK(!SoapySource::anyDeviceOpen());
        CHECK(src.open("driver=fakenonfinite"));
        CHECK(SoapySource::anyDeviceOpen());

        // THE ENUMERATION GATE, while the device is open: the in-process walk
        // would list the fake driver registered just above (it does, two
        // dozen lines down) — an empty answer here can only mean the gate
        // refused the walk, which is adjudicated fix #1: never enumerate
        // in-process while a radio is open.
        CHECK(SoapySource::enumerateInProcess().empty());

        CHECK(src.start());
        std::atomic<bool> stopFlag{false};
        std::atomic<std::size_t> reads{0};
        std::thread reader([&src, &stopFlag, &reads]() {
            std::vector<std::complex<float>> buf(256);
            while (!stopFlag.load(std::memory_order_relaxed)) {
                if (src.read(buf.data(), buf.size()) > 0) {
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        // The fake's readStream returns instantly, so 300 control calls can
        // finish inside the reader thread's own startup latency — wait for
        // the first read so the two loops genuinely overlap. A read() that
        // never returns (a lock bug) parks this spin until the suite's 120 s
        // timeout converts it into a failure, which is the liveness check.
        while (reads.load(std::memory_order_relaxed) == 0u) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 300; ++i) {
            CHECK(src.setCenterFrequencyHz(100.0e6 + i * 1.0e3));
            (void)src.listGainNames();
            (void)src.centerFrequencyHz();  // lock-free mirror: must not block
            (void)src.name();
        }
        // The reader must still make progress after the control burst — a
        // starved or wedged read side would sit at whatever count it reached.
        const std::size_t before = reads.load(std::memory_order_relaxed);
        while (reads.load(std::memory_order_relaxed) <= before) {
            std::this_thread::yield();
        }
        stopFlag.store(true, std::memory_order_relaxed);
        reader.join();
        std::printf("serialisation: %zu reads alongside 300 retunes\n",
                    reads.load());
        CHECK(reads.load() > before);
        CHECK(!src.faulted());
        CHECK(src.running());
        // The retune landed: the mirror follows the device readback.
        CHECK_NEAR(src.centerFrequencyHz(), 100.0e6 + 299 * 1.0e3, 1.0);

        // stop() now waits out at most one bounded read instead of
        // interrupting it cross-thread (the SoapySDR stream contract forbids
        // concurrent stream use) — this must return, not deadlock.
        src.stop();
        CHECK(!src.running());
        src.closeDevice();
        CHECK(!src.isOpen());
        CHECK(!SoapySource::anyDeviceOpen());

        // Gate released with the device: the same walk now lists the fake.
        const std::vector<SoapyDeviceInfo> after = SoapySource::enumerateInProcess();
        bool sawFake = false;
        for (const SoapyDeviceInfo& d : after) {
            if (d.args.find("fakenonfinite") != std::string::npos) { sawFake = true; }
        }
        CHECK(sawFake);
    }

    // --- a sample-rate change on a live stream happens on a QUIESCENT one ---
    //
    // THE 0.88.0 FIELD CRASH (235E46B5D39DED8D, NESDR SMArt v5): the ADS-B
    // preset set 2.4 MS/s on a radio streaming at 2 MS/s, and SoapyRTLSDR's
    // reader thread died 45 s later on a freed libusb lock - see the fake's
    // comment for the mechanism. The fix is an ORDER: deactivate, set, activate,
    // under the one device lock, and that order is what this block pins.
    // Red-green: with the restart removed (the rate set on the live stream, as
    // 0.88.0 did) the recorded sequence is a single "setSampleRate(LIVE)" and
    // the first order CHECK below fails.
    {
        std::printf("--- sample rate change on a live stream ---\n");
        SoapySDR::Registry reg("fakerateorder", &findRateOrder, &makeRateOrder,
                               SOAPY_SDR_ABI_VERSION);
        SoapySource src;
        CHECK(src.open("driver=fakerateorder"));
        CHECK(src.sampleRateHz() == 2.0e6);  // the readback at open

        // (a) RUNNING: deactivate, set, activate - exactly, and in that order.
        CHECK(src.start());
        CHECK(src.running());
        g_rateCalls.clear();
        // 2,400,400 asked, 2,400,000 read back: proves the mirror follows the
        // device's coerced answer and not the request.
        const bool changed = src.setSampleRateHz(2.4004e6);
        std::printf("  running change: %s, calls=[%s], lastError=\"%s\"\n",
                    changed ? "true" : "false", joinCalls(g_rateCalls).c_str(),
                    src.lastError());
        CHECK(changed);
        CHECK(g_rateCalls == (std::vector<std::string>{
                                 "deactivateStream", "setSampleRate", "activateStream"}));
        CHECK(src.running());
        CHECK(src.sampleRateHz() == 2.4e6);
        CHECK(!src.faulted());
        // THE LOG LINE THE FIELD REPORT DID NOT HAVE. Its ring showed the
        // restore, then the ADS-B window opening, then a minute of the sound
        // path faltering - and nothing saying the radio's clock had been
        // changed under a live stream in between. Pinned here so the next
        // report of this shape names the transition and how long it took.
        CHECK(lastDiagLine("source: sample rate 2000000 -> 2400000 S/s (stream restarted, "));

        // (e) ...and samples still flow afterwards. The fake answers a read
        // only while ITS stream is active, so a change that forgot the
        // reactivate (or reactivated before it set the rate) comes back empty
        // here rather than being taken on trust.
        std::vector<std::complex<float>> buf(256);
        CHECK(src.read(buf.data(), buf.size()) == buf.size());
        CHECK(buf[0] == std::complex<float>(0.5f, 0.5f));

        // (c) THE SAME RATE AGAIN, while running: no driver call at all. A
        // preset re-applied at the rate the radio already has must not cost a
        // stream restart, and the fake would record one if it happened.
        g_rateCalls.clear();
        CHECK(src.setSampleRateHz(2.4e6));
        CHECK(g_rateCalls.empty());
        CHECK(src.running());
        CHECK(src.sampleRateHz() == 2.4e6);

        // (d) A REJECTED RATE LEAVES THE RADIO RUNNING AT THE OLD ONE. The
        // driver throws the way SoapyRTLSDR does for a rate the dongle cannot
        // make; the user asked for a preset, not for silence, so the stream
        // is put back at 2.4 MS/s and the message says so.
        g_rateCalls.clear();
        g_rateRejectNext = true;
        const bool rejected = src.setSampleRateHz(3.2e6);
        std::printf("  rejected change: %s, calls=[%s], lastError=\"%s\"\n",
                    rejected ? "true" : "false", joinCalls(g_rateCalls).c_str(),
                    src.lastError());
        CHECK(!rejected);
        CHECK(g_rateCalls == (std::vector<std::string>{
                                 "deactivateStream", "setSampleRate", "activateStream"}));
        CHECK(src.running());
        CHECK(src.sampleRateHz() == 2.4e6);            // unchanged
        CHECK(!src.faulted());                         // a refusal, not a fault
        CHECK(std::strstr(src.lastError(), "does not support") != nullptr);
        CHECK(std::strstr(src.lastError(), "still running at 2400000 S/s") != nullptr);
        CHECK(src.read(buf.data(), buf.size()) == buf.size());  // and it really is

        // (f) THE RATE TOOK BUT THE STREAM WOULD NOT COME BACK: running() must
        // say stopped, the mirror must carry the rate the device now has (the
        // DSP chain runs at the device's clock, not at the last one that
        // worked), and the message must say which.
        g_rateCalls.clear();
        g_rateRefuseActivate = true;
        const bool halfway = src.setSampleRateHz(1.024e6);
        std::printf("  activate refused: %s, calls=[%s], lastError=\"%s\"\n",
                    halfway ? "true" : "false", joinCalls(g_rateCalls).c_str(),
                    src.lastError());
        CHECK(!halfway);
        CHECK(g_rateCalls == (std::vector<std::string>{
                                 "deactivateStream", "setSampleRate", "activateStream"}));
        CHECK(!src.running());
        CHECK(src.sampleRateHz() == 1.024e6);
        CHECK(!src.faulted());
        CHECK(std::strstr(src.lastError(), "could not be restarted") != nullptr);
        CHECK(std::strstr(src.lastError(), "it is stopped") != nullptr);
        // The next Play brings it back: the object really is in "stopped",
        // not in some third state start() refuses.
        g_rateCalls.clear();
        CHECK(src.start());
        CHECK(g_rateCalls == (std::vector<std::string>{"activateStream"}));
        CHECK(src.running());
        CHECK(src.read(buf.data(), buf.size()) == buf.size());
        src.stop();
        CHECK(!src.running());

        // (b) STOPPED: the rate call alone. There is no stream to quiesce, and
        // an activate here would start a radio the user has stopped.
        g_rateCalls.clear();
        CHECK(src.setSampleRateHz(1.0e6));
        CHECK(g_rateCalls == (std::vector<std::string>{"setSampleRate"}));
        CHECK(!src.running());
        CHECK(src.sampleRateHz() == 1.0e6);
        CHECK(lastDiagLine("source: sample rate 1024000 -> 1000000 S/s (stream idle)"));

        // (c) again, stopped: same rate, no call.
        g_rateCalls.clear();
        CHECK(src.setSampleRateHz(1.0e6));
        CHECK(g_rateCalls.empty());
        CHECK(!src.running());

        // (d) again, stopped: a rejection is just a rejection - nothing is
        // activated to "restore" a stream that was not running.
        g_rateCalls.clear();
        g_rateRejectNext = true;
        CHECK(!src.setSampleRateHz(3.2e6));
        CHECK(g_rateCalls == (std::vector<std::string>{"setSampleRate"}));
        CHECK(!src.running());
        CHECK(src.sampleRateHz() == 1.0e6);

        src.closeDevice();
        CHECK(!src.isOpen());
        CHECK(!SoapySource::anyDeviceOpen());
    }

    // --- OPT-IN REAL-HARDWARE SOAK: CASCADE_TEST_B200_SOAK=1 -----------------
    //
    // The adjudicated experiment for the 0.62.0 field crashes, adapted to the
    // fixed architecture: stream a real B200 while the control thread retunes
    // continuously AND device scans run — the exact overlap that shipped
    // builds performed unserialised. Needs a B200 attached and SoapyUHD
    // installed, so it is opt-in by environment variable like the live blocks
    // in test_plugin_repo/test_plugin_host, and skipped silently otherwise.
    if (const char* soak = std::getenv("CASCADE_TEST_B200_SOAK");
        soak != nullptr && soak[0] == '1') {
        std::printf("B200 soak: starting (opt-in)\n");
        SoapySource src;
        const bool opened = src.open("driver=uhd");
        std::printf("B200 soak: open=%s (%s)\n", opened ? "true" : "false",
                    src.lastError());
        CHECK(opened);
        if (opened) {
            CHECK(src.setSampleRateHz(2.0e6));
            CHECK(src.start());
            std::atomic<bool> stopFlag{false};
            std::atomic<std::size_t> reads{0};
            std::thread reader([&src, &stopFlag, &reads]() {
                std::vector<std::complex<float>> buf(8192);
                while (!stopFlag.load(std::memory_order_relaxed)) {
                    if (src.read(buf.data(), buf.size()) > 0) {
                        reads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
            // 30 seconds of retune bursts + scans against the live stream.
            // The scans take the normal enumerate() path (child process); the
            // in-process walk is gated and must answer empty while open.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(30);
            int tunes = 0;
            int scans = 0;
            while (std::chrono::steady_clock::now() < deadline) {
                for (int i = 0; i < 25; ++i) {
                    const double hz = 88.0e6 + (tunes % 400) * 50.0e3;
                    if (!src.setCenterFrequencyHz(hz)) { break; }
                    ++tunes;
                }
                CHECK(SoapySource::enumerateInProcess().empty());  // gate holds
                (void)SoapySource::enumerate();  // child-process scan
                ++scans;
                if (src.faulted()) { break; }
            }
            stopFlag.store(true, std::memory_order_relaxed);
            reader.join();
            std::printf(
                "B200 soak: %d tunes, %d scans, %zu reads, faulted=%s "
                "lastError=\"%s\"\n",
                tunes, scans, reads.load(), src.faulted() ? "true" : "false",
                src.lastError());
            CHECK(!src.faulted());
            CHECK(reads.load() > 0u);
            // The child-process scans dominate the wall clock (~5 s each), so
            // the bound is on "many tunes happened against a live stream",
            // not on throughput: 2 full outer loops is the floor.
            CHECK(tunes >= 50);
            src.stop();
            src.closeDevice();
        }
    }


    // --- a driver that will not return must not freeze the interface -------
    {
        std::printf("--- stalling driver: stop() must give up, not hang ---\n");
        SoapySDR::Registry reg("fakestall", &findStall, &makeStall,
                               SOAPY_SDR_ABI_VERSION);
        SoapySource src;
        if (!src.open("driver=fakestall")) {
            std::printf("  (fake stalling device would not open: %s)\n", src.lastError());
        } else {
            CHECK(src.start());
            // A reader thread parked inside the stalling readStream, holding
            // devMutex_ - which is exactly the state the field hang was in.
            g_stallInRead.store(false, std::memory_order_relaxed);
            std::thread reader([&] {
                std::vector<std::complex<float>> buf(4096);
                (void)src.read(buf.data(), buf.size());
            });
            // Rendezvous on the DRIVER, not the thread: g_stallInRead is
            // set from inside readStream itself, so when it reads true the
            // reader provably holds devMutex_ and every timing assertion
            // below starts from the contended state it claims to measure.
            const auto parkStart = std::chrono::steady_clock::now();
            while (!g_stallInRead.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (std::chrono::steady_clock::now() - parkStart >
                    std::chrono::seconds(5)) {
                    break;  // let the CHECKs below name the failure
                }
            }
            CHECK(g_stallInRead.load(std::memory_order_acquire));

            // A SETTER against the same parked reader first: a SOFT failure,
            // not an escape path. This is the case the lane design turns on -
            // a setter can time out against a perfectly healthy slow call
            // too (this stall, or a real UHD open() taking seconds on another
            // thread), so it must report the failure and give up the lock
            // wait, but it must NOT condemn a device it has no evidence is
            // actually broken. Only stop()/closeDevice() get to do that.
            const auto tuneStart = std::chrono::steady_clock::now();
            const bool tuned = src.setCenterFrequencyHz(200.0e6);
            const auto tuneElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tuneStart);
            std::printf("  setCenterFrequencyHz() returned %s after %lld ms\n",
                        tuned ? "true" : "false",
                        static_cast<long long>(tuneElapsed.count()));
            // The driver is still 6 s from returning, so this must be the
            // bounded give-up, not a lucky fast path.
            CHECK(!tuned);
            CHECK(tuneElapsed < std::chrono::milliseconds(4000));
            CHECK(!src.faulted());     // soft failure: NOT condemned
            CHECK(!src.deviceDead());
            CHECK(src.deadReason() == SoapySource::DeadReason::None);
            CHECK(std::strlen(src.lastError()) > 0);

            // NOW the escape path, against the SAME still-parked reader (it
            // has ~4.25 s left of its 6 s sleep at this point): stop() must
            // also give up rather than hang, and THIS timeout is the one
            // permitted to mark the device dead.
            const auto t0 = std::chrono::steady_clock::now();
            src.stop();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0);
            std::printf("  stop() returned after %lld ms\n",
                        static_cast<long long>(elapsed.count()));
            // The driver sleeps 6 s. Anything near that means stop() waited it
            // out, which is the freeze this guards against.
            CHECK(elapsed < std::chrono::milliseconds(4000));
            // And it must SAY so rather than pretending the stop worked.
            CHECK(src.faulted());
            CHECK(src.deviceDead());
            CHECK(std::strlen(src.lastError()) > 0);
            // AND SAY WHICH KIND OF DEAD. The reader is inside the driver
            // right now, holding the lock stop() could not win: this is an
            // ABANDONMENT, and the automatic reopen (0.90.1) must never fire
            // on it - a second thread in that module beside the parked one is
            // the 0.62.0 crash class. RED WHEN the lock-timeout path records
            // no reason (None) or the fault one.
            CHECK(src.deadReason() == SoapySource::DeadReason::Abandoned);
            CHECK(src.faultedWhile().empty());
            reader.join();
            src.closeDevice();
        }
    }

    // --- a driver call that never returns must not freeze the interface ----
    //
    // THE 0.70.0 FIELD FREEZE. stop() already gave up waiting for the driver
    // LOCK (the block above); it then called deactivateStream on the GUI
    // thread and waited for that for ever. Bounding the lock cannot help when
    // the call holding it is the one that hung.
    {
        std::printf("--- wedged deactivateStream: stop() must abandon it ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        g_wedgeDeactivate.store(true, std::memory_order_relaxed);
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();
        // THE USAGE RECORD (0.99.65): the window's poll reads this very counter and counts a rise as
        // `recovered.vendorcall` (core/health_events.hpp, RecoveryWatch). The real trigger: this wedged
        // deactivateStream, abandoned by the real escape path.
        auto healthLedger = cascade::core::health::globalLedger();
        healthLedger->reset();
        healthLedger->arm("", cascade::core::newInstallId(), false);
        cascade::core::health::RecoveryWatch healthWatch;
        cascade::core::health::RecoveryReadings healthReadings;
        healthReadings.vendorCallsAbandoned = abandonedBefore;
        healthWatch.poll(healthReadings);                     // the baseline

        // On the heap, so the source can be DESTROYED while the driver call is
        // still parked inside the fake - the case that turns this fix into a
        // use-after-free if the abandoned call kept a pointer to the source.
        auto src = std::make_unique<SoapySource>();
        CHECK(src->open("driver=fakewedge"));
        CHECK(src->start());

        const auto t0 = std::chrono::steady_clock::now();
        src->stop();
        const long long stopMs = msSince(t0);
        std::printf("  stop() returned after %lld ms (driver still inside)\n", stopMs);
        healthReadings.vendorCallsAbandoned = SoapySource::driverCallsAbandoned();
        healthWatch.poll(healthReadings);
        CHECK(healthLedger->counts() ==
              (cascade::core::health::Counts{{"recovered.vendorcall", 1}}));
        healthLedger->reset();
        // The wedge holds for 20 s. Anything near that is stop() waiting it
        // out, which is the freeze itself.
        CHECK(stopMs < 3000);
        // ...and it really did enter the driver: a stop that never got there
        // would also be fast, and would prove nothing.
        CHECK(g_inWedgedCall.load(std::memory_order_acquire));
        // The abandonment is COUNTED, so this cannot pass on a driver that
        // merely happened to answer quickly (the mutation that deletes the
        // bound leaves every timing check to luck otherwise).
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);
        CHECK(g_wedgedCallsReturned.load(std::memory_order_acquire) == 0);

        // The verdict the user sees, and it must be the escape-path one.
        CHECK(!src->running());
        CHECK(src->faulted());
        CHECK(src->deviceDead());
        // A call left running inside the module is an ABANDONMENT, not a
        // fault - the distinction the automatic reopen (0.90.1) turns on.
        // RED WHEN abandonWedgedDriverLocked records the fault reason.
        CHECK(src->deadReason() == SoapySource::DeadReason::Abandoned);
        CHECK(src->faultedWhile().empty());
        std::printf("  lastError=\"%s\"\n", src->lastError());
        CHECK(std::strstr(src->lastError(), "abandoned") != nullptr);
        CHECK(std::strstr(src->lastError(), "Restart FoxSDR") != nullptr);

        // THE LOCK IS NOT LEFT HELD. A call that takes it must come straight
        // back, not wait out the 1.5 s control-lock bound - if the abandoned
        // call had kept the lock, every later control call would pay for it.
        const auto t1 = std::chrono::steady_clock::now();
        const std::vector<std::string> gains = src->listGainNames();
        const long long lockMs = msSince(t1);
        std::printf("  a control call after the abandonment took %lld ms\n", lockMs);
        CHECK(gains.empty());
        CHECK(lockMs < 500);

        // NO FURTHER DRIVER CALLS, ever: the device a thread is still inside
        // is not closed, not unmade, and not reopenable. That is what makes
        // "restart FoxSDR to use this radio again" true rather than hopeful.
        CHECK(!src->open("driver=fakewedge, serial=reopen"));
        CHECK(std::strstr(src->lastError(), "abandoned") != nullptr);
        CHECK(src->deviceDead());
        // The refused reopen passed through teardownLocked, which clears
        // the latch for a faulted device and keeps it for an abandoned one -
        // the reason must survive that the same way. RED WHEN teardown
        // resets the reason unconditionally.
        CHECK(src->deadReason() == SoapySource::DeadReason::Abandoned);
        CHECK(g_closeStreamCalls.load(std::memory_order_acquire) == 0);
        CHECK(g_deviceDestroyed.load(std::memory_order_acquire) == 0);

        // DESTROYED WHILE THE CALL IS STILL BLOCKED. The abandoned call owns
        // the mutex and the handles through a shared_ptr rather than through
        // the source, so this must be survivable; a call that had captured the
        // source would now be reading freed memory.
        src.reset();
        CHECK(g_wedgedCallsReturned.load(std::memory_order_acquire) == 0);
        CHECK(g_deviceDestroyed.load(std::memory_order_acquire) == 0);

        // Now let the driver go and watch the abandoned call come back out of
        // it, after the object that started it has ceased to exist.
        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        std::printf("  the abandoned call returned after the source was destroyed\n");
    }

    // --- a reopen whose RELEASE is what wedges ------------------------------
    //
    // The switch the field report's user actually performed: pick another
    // radio while the current one's deactivateStream is wedged. open() has to
    // release what is open before it makes anything, so the abandonment
    // happens INSIDE this call - and the interrogation that follows must not
    // then write a fresh device handle over the two the wedged call is still
    // holding.
    {
        std::printf("--- reopen while the release wedges: no second device ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        g_wedgeDeactivate.store(true, std::memory_order_relaxed);
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();

        SoapySource src;
        CHECK(src.open("driver=fakewedge, serial=first"));
        CHECK(src.start());
        CHECK(g_setupStreamCalls.load(std::memory_order_acquire) == 1);

        const auto t0 = std::chrono::steady_clock::now();
        const bool reopened = src.open("driver=fakewedge, serial=second");
        const long long reopenMs = msSince(t0);
        std::printf("  open() returned %s after %lld ms\n", reopened ? "true" : "false",
                    reopenMs);
        CHECK(!reopened);
        CHECK(reopenMs < 3000);
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);
        // THE SECOND DEVICE WAS NEVER SET UP. Interrogating one would have
        // written a fresh device and stream over the two the abandoned call is
        // still reading, and left the first radio's claim held by handles
        // nothing points at any more.
        CHECK(g_setupStreamCalls.load(std::memory_order_acquire) == 1);
        CHECK(src.deviceDead());
        CHECK(!src.isOpen());
        CHECK(std::strstr(src.lastError(), "abandoned") != nullptr);

        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        std::printf("  the abandoned release returned\n");
    }

    // --- the same, for the teardown half: closeStream ----------------------
    //
    // stopLocked() is not the only escape-path call into the driver.
    // closeDevice() and ~SoapySource() run closeStream and unmake, on the same
    // GUI thread, and a wedge there freezes the interface just as completely.
    {
        std::printf("--- wedged closeStream: closeDevice() must abandon it ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        g_wedgeCloseStream.store(true, std::memory_order_relaxed);
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();

        SoapySource src;
        // Different args from the block above: SoapySDR::Device::make keys its
        // device table on them, and the first block's device was deliberately
        // never unmade.
        CHECK(src.open("driver=fakewedge, serial=teardown"));
        CHECK(src.start());
        // Only closeStream is wedged here, so the stop must be ORDINARY - that
        // is what proves the next check is measuring the teardown call and not
        // a leftover of the previous block's.
        src.stop();
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());
        CHECK(src.deadReason() == SoapySource::DeadReason::None);

        const auto t0 = std::chrono::steady_clock::now();
        src.closeDevice();
        const long long closeMs = msSince(t0);
        std::printf("  closeDevice() returned after %lld ms\n", closeMs);
        CHECK(closeMs < 3000);
        CHECK(g_inWedgedCall.load(std::memory_order_acquire));
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);
        CHECK(src.deviceDead());
        CHECK(src.deadReason() == SoapySource::DeadReason::Abandoned);
        CHECK(!src.isOpen());
        CHECK(std::strstr(src.lastError(), "abandoned") != nullptr);

        // AND UNMAKE WAS NOT ATTEMPTED. Deleting a device object while one of
        // this process's threads is still executing inside it would be the
        // worst possible answer to a wedged closeStream, and the abandoned
        // count says only one call was ever left behind.
        CHECK(g_deviceDestroyed.load(std::memory_order_acquire) == 0);

        // Idempotent, and still no driver calls: a second close costs nothing
        // and abandons nothing new.
        src.closeDevice();
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);
        CHECK(g_closeStreamCalls.load(std::memory_order_acquire) == 1);
        CHECK(g_deviceDestroyed.load(std::memory_order_acquire) == 0);

        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        std::printf("  the abandoned teardown call returned\n");
    }

    // --- a wedged activateStream must not freeze the interface -------------
    //
    // HANG REPORT 40002A91C26F3C07 (0.99.58), symbolised: AppWindow::run ->
    // drawUi -> pollSourceAsync -> finishDeviceOpen -> Pipeline::setSource ->
    // SoapySource::start -> activateLocked -> RedPitaya.dll -> WS2_32, on the
    // GUI thread. SoapyRedPitaya's activateStream connects to its default
    // 192.168.1.100:1001 and select()s five seconds per socket; start() made
    // that call inline and the window froze. The escape paths were already
    // abandonable; the call that brings a stream UP was not. Red-green:
    // making start() call the driver inline again leaves it inside the 20 s
    // wedge, and the elapsed-time check below fails.
    {
        std::printf("--- wedged activateStream: start() must abandon it ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        g_wedgeActivate.store(true, std::memory_order_relaxed);
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();

        // On the heap, destroyed while the call is still parked in the driver,
        // as in the deactivate block: the abandoned activate must own what it
        // touches rather than reach back into the source.
        auto src = std::make_unique<SoapySource>();
        // Args of its own: SoapySDR::Device::make caches devices by kwargs, and
        // the blocks above left theirs deliberately un-made.
        CHECK(src->open("driver=fakewedge, serial=activate"));
        // The late call's grace, short enough to wait out here (see below).
        src->setLateCallGraceForTest(std::chrono::milliseconds(2500));

        const auto t0 = std::chrono::steady_clock::now();
        const bool started = src->start();
        const long long startMs = msSince(t0);
        std::printf("  start() returned %s after %lld ms (driver still inside)\n",
                    started ? "true" : "false", startMs);
        // Inside the hang watchdog's 5000 ms frame threshold, which is what
        // the field report tripped; the wedge itself holds for 20 s.
        CHECK(startMs < 4500);
        CHECK(g_inWedgedCall.load(std::memory_order_acquire));
        CHECK(g_wedgedCallsReturned.load(std::memory_order_acquire) == 0);

        // MERGED WITH THE SDRPLAY LATE-CALL FIX (hang report 9B804643C56308CF,
        // where sdrplay_api_Init answered five seconds late and the answer was
        // needed): an activate not back within kVendorCallWait is PENDING,
        // not condemned. So start() answers true with nothing abandoned yet
        // (this block's first version expected false and an abandonment at
        // 3 s; that rule could not tell a slow SDRplay from a dead Red Pitaya).
        CHECK(started);
        CHECK(!src->deviceDead());
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore);
        // The lock is not left held behind the parked call...
        const auto t1 = std::chrono::steady_clock::now();
        (void)src->listGainNames();
        CHECK(msSince(t1) < 500);
        // ...and no read reaches readStream beside it.
        std::complex<float> buf[64];
        CHECK(src->read(buf, 64) == 0u);
        CHECK(g_readStreamCalls.load(std::memory_order_acquire) == 0);

        // A RED PITAYA THAT NEVER ANSWERS is given up by the read loop once the
        // late call's grace has run (kLateCallGrace, 20 s; shortened above so
        // the test can wait it out), exactly as an escape path gives up a
        // wedged call.
        const auto t2 = std::chrono::steady_clock::now();
        CHECK(readUntil(*src, [&] { return src->deviceDead(); }, 6000));
        std::printf("  given up %lld ms after start() returned\n", msSince(t2));
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);
        CHECK(g_wedgedCallsReturned.load(std::memory_order_acquire) == 0);

        // Refused, condemned, and said in the escape paths' own words.
        CHECK(!src->running());
        CHECK(src->faulted());
        CHECK(src->deviceDead());
        CHECK(src->deadReason() == SoapySource::DeadReason::Abandoned);
        std::printf("  lastError=\"%s\"\n", src->lastError());
        CHECK(std::strstr(src->lastError(), "abandoned") != nullptr);

        // The lock is still not held behind the parked call...
        const auto t3 = std::chrono::steady_clock::now();
        (void)src->listGainNames();
        CHECK(msSince(t3) < 500);
        // ...and nothing calls into the driver again: a second start refuses
        // without entering it, and no read reaches readStream.
        CHECK(!src->start());
        CHECK(src->read(buf, 64) == 0u);
        CHECK(g_readStreamCalls.load(std::memory_order_acquire) == 0);
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);

        src.reset();
        CHECK(g_wedgedCallsReturned.load(std::memory_order_acquire) == 0);
        CHECK(g_deviceDestroyed.load(std::memory_order_acquire) == 0);
        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        std::printf("  the abandoned activate returned after the source was destroyed\n");
    }

    // --- a retune the driver answers LATE: no freeze, and the answer kept ---
    //
    // HANG REPORT 6F550354218029F0 (0.99.56, SDRplay RSP1A through
    // SoapySDRPlay in the patch page). The SDRplay API service was restarted
    // under the live stream; the next retune went into SoapySDRPlay3's
    // setFrequency on the GUI thread - sdrplay_api_Update, then up to 500
    // sleeps waiting for a callback that no longer came - and the window froze
    // until it gave up ("RF center frequency update timeout.", then "gui thread
    // recovered after a stall"). The call DID return. So the bound must not
    // condemn the radio the way an escape path does: the caller stops waiting,
    // the answer is listened for, and when it comes the radio is usable again.
    {
        std::printf("--- a late retune: bounded, not condemned, answer recovered ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();

        const int openBefore = SoapySource::openDeviceCount();
        SoapySource src;
        CHECK(src.open("driver=fakewedge, serial=lateretune"));
        CHECK(src.start());
        g_wedgeSetFrequency.store(true, std::memory_order_relaxed);

        const auto t0 = std::chrono::steady_clock::now();
        const bool tuned = src.setCenterFrequencyHz(101.5e6);
        const long long tuneMs = msSince(t0);
        std::printf("  setCenterFrequencyHz() returned %s after %lld ms (driver still inside)\n",
                    tuned ? "true" : "false", tuneMs);
        // The wedge holds for 20 s; the unfixed retune waits all of it on the
        // calling thread, which is the frozen window.
        CHECK(tuneMs < 3000);
        CHECK(g_inWedgedCall.load(std::memory_order_acquire));
        CHECK(!tuned);
        // NOT CONDEMNED: a driver that is slow to answer is not a dead radio.
        CHECK(!src.deviceDead());
        CHECK(!src.faulted());
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore);
        std::printf("  lastError=\"%s\"\n", src.lastError());
        CHECK(std::strstr(src.lastError(), "not answered") != nullptr);

        // HELD, WITHOUT WAITING: the call is still inside the driver, so
        // nothing else may enter it - and asking must not cost the caller
        // the lock bound either, or every frame pays it.
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(!src.setCenterFrequencyHz(102.0e6));
        const long long heldMs = msSince(t1);
        std::printf("  a second retune while the first is out took %lld ms\n", heldMs);
        CHECK(heldMs < 500);
        const int readsBefore = g_readStreamCalls.load(std::memory_order_acquire);
        std::vector<std::complex<float>> buf(256);
        CHECK(src.read(buf.data(), buf.size()) == 0u);
        CHECK(g_readStreamCalls.load(std::memory_order_acquire) == readsBefore);

        // The service answers. Nothing but the read loop asks, as when the
        // user touches nothing.
        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        CHECK(readUntil(src, [&] { return src.centerFrequencyHz() == 101.5e6; }, 3000));
        std::printf("  centre after the late answer: %.0f Hz\n", src.centerFrequencyHz());
        CHECK(src.centerFrequencyHz() == 101.5e6);
        CHECK(!src.deviceDead());
        CHECK(!src.faulted());
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore);
        CHECK(diagRingHas("retuning the device answered after"));
        // ...and the "not answered yet" sentence goes with the wait it described.
        CHECK(std::strstr(src.lastError(), "not answered") == nullptr);

        // USABLE AGAIN: an ordinary retune, samples, and an ordinary close
        // that gives the radio back.
        g_wedgeSetFrequency.store(false, std::memory_order_relaxed);
        CHECK(src.setCenterFrequencyHz(102.0e6));
        CHECK(src.centerFrequencyHz() == 102.0e6);
        CHECK(src.read(buf.data(), buf.size()) == buf.size());
        src.closeDevice();
        CHECK(g_closeStreamCalls.load(std::memory_order_acquire) == 1);
        // RELEASED, by the one path that counts a radio given back (a
        // completed unmake). Not g_deviceDestroyed: every fakewedge open in
        // this file resolves to the same kwargs, so SoapySDR's make cache
        // hands back one instance that earlier blocks deliberately leaked.
        CHECK(SoapySource::openDeviceCount() == openBefore);
    }

    // --- a stream start the driver answers LATE, with a refusal ------------
    //
    // HANG REPORT 9B804643C56308CF, two minutes after the one above in the
    // same session. The patch's START opened the RSP again and called
    // activateStream - sdrplay_api_Init - on the GUI thread; it answered
    // sdrplay_api_AlreadyInitialised five seconds later, and the teardown of
    // the radio that would not start then spent its own bounded wait, past the
    // watchdog's threshold. A start that is not answered in the caller's
    // budget is pending, not failed: the stream reports itself started, reads
    // nothing, and the late refusal arrives as the fault it is.
    {
        std::printf("--- a late stream start answered with a refusal ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        g_wedgeActivate.store(true, std::memory_order_relaxed);
        g_activateRet.store(SOAPY_SDR_NOT_SUPPORTED, std::memory_order_relaxed);
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();

        const int openBefore = SoapySource::openDeviceCount();
        SoapySource src;
        CHECK(src.open("driver=fakewedge, serial=lateactivate"));
        const auto t0 = std::chrono::steady_clock::now();
        const bool started = src.start();
        const long long startMs = msSince(t0);
        std::printf("  start() returned %s after %lld ms (driver still inside)\n",
                    started ? "true" : "false", startMs);
        CHECK(startMs < 3000);
        CHECK(g_inWedgedCall.load(std::memory_order_acquire));
        CHECK(started);
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore);
        std::vector<std::complex<float>> buf(256);
        CHECK(src.read(buf.data(), buf.size()) == 0u);
        CHECK(g_readStreamCalls.load(std::memory_order_acquire) == 0);

        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        CHECK(readUntil(src, [&] { return src.faulted(); }, 3000));
        std::printf("  after the late refusal: faulted=%d running=%d lastError=\"%s\"\n",
                    src.faulted() ? 1 : 0, src.running() ? 1 : 0, src.lastError());
        CHECK(src.faulted());
        CHECK(!src.running());
        // The driver ANSWERED - a refusal, not a fault and not a wedge.
        CHECK(!src.deviceDead());
        CHECK(src.deadReason() == SoapySource::DeadReason::None);
        CHECK(std::strstr(src.lastError(), "activateStream failed") != nullptr);
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore);
        CHECK(g_readStreamCalls.load(std::memory_order_acquire) == 0);

        // The radio is RELEASED by an ordinary close, at once.
        const auto t1 = std::chrono::steady_clock::now();
        src.closeDevice();
        const long long closeMs = msSince(t1);
        std::printf("  closeDevice() after the refusal took %lld ms\n", closeMs);
        CHECK(closeMs < 500);
        CHECK(g_closeStreamCalls.load(std::memory_order_acquire) == 1);
        CHECK(SoapySource::openDeviceCount() == openBefore);
    }

    // --- a sample-rate restart the driver answers LATE ---------------------
    //
    // A live rate change deactivates, sets the rate and ACTIVATES again - the
    // same activateStream as start(), through the same activateLocked, so it
    // has the same late-call rule: the GUI thread is given back inside
    // kVendorCallWait, the radio is not condemned, and the answer is taken
    // by the read loop. Red-green: an inline activate in the restart leaves
    // setSampleRateHz() inside the 20 s wedge and the elapsed-time check fails.
    {
        std::printf("--- a late sample-rate restart: bounded, answer taken ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();
        const int openBefore = SoapySource::openDeviceCount();
        SoapySource src;
        CHECK(src.open("driver=fakewedge, serial=laterate"));
        CHECK(src.start());
        g_wedgeActivate.store(true, std::memory_order_relaxed);

        const auto t0 = std::chrono::steady_clock::now();
        const bool changed = src.setSampleRateHz(2.0e6);
        const long long rateMs = msSince(t0);
        std::printf("  setSampleRateHz() returned %s after %lld ms (driver still inside)\n",
                    changed ? "true" : "false", rateMs);
        CHECK(rateMs < 3000);
        CHECK(g_inWedgedCall.load(std::memory_order_acquire));
        CHECK(changed);
        CHECK(src.running());
        CHECK(!src.deviceDead());
        CHECK(!src.faulted());
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore);
        const int readsBefore = g_readStreamCalls.load(std::memory_order_acquire);
        std::vector<std::complex<float>> buf(256);
        CHECK(src.read(buf.data(), buf.size()) == 0u);
        CHECK(g_readStreamCalls.load(std::memory_order_acquire) == readsBefore);

        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        CHECK(readUntil(src, [&] {
            return diagRingHas("restarting the stream after a sample-rate change answered after");
        }, 3000));
        CHECK(!src.deviceDead());
        CHECK(!src.faulted());
        CHECK(src.running());
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore);
        g_wedgeActivate.store(false, std::memory_order_relaxed);
        CHECK(src.read(buf.data(), buf.size()) == buf.size());
        src.closeDevice();
        CHECK(SoapySource::openDeviceCount() == openBefore);
    }

    // --- a late call that never answers is given up after the grace -------
    //
    // "Later" is not "never" without a limit: past the grace the call is
    // treated exactly as an escape path treats a wedged one - counted,
    // condemned, and the radio left alone for good.
    {
        std::printf("--- a late retune past its grace: given up, condemned ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();
        SoapySource src;
        src.setLateCallGraceForTest(std::chrono::milliseconds(2500));
        CHECK(src.open("driver=fakewedge, serial=lategrace"));
        CHECK(src.start());
        g_wedgeSetFrequency.store(true, std::memory_order_relaxed);
        CHECK(!src.setCenterFrequencyHz(103.0e6));
        CHECK(!src.deviceDead());
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(readUntil(src, [&] { return src.deviceDead(); }, 6000));
        const long long gaveUpMs = msSince(t0);
        std::printf("  given up %lld ms after the caller stopped waiting\n", gaveUpMs);
        // Not before the grace: the caller's own 1.5 s wait is inside it.
        CHECK(gaveUpMs >= 500);
        CHECK(src.deviceDead());
        CHECK(src.deadReason() == SoapySource::DeadReason::Abandoned);
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);
        CHECK(std::strstr(src.lastError(), "abandoned") != nullptr);
        CHECK(src.centerFrequencyHz() != 103.0e6);
        // The condemned radio is never called again: a close is immediate and
        // does not reach closeStream.
        const auto t1 = std::chrono::steady_clock::now();
        src.closeDevice();
        CHECK(msSince(t1) < 500);
        CHECK(g_closeStreamCalls.load(std::memory_order_acquire) == 0);
        releaseWedge();
        CHECK(waitForWedgedReturn(1));
    }

    // --- a stop while a late call is out gives it up at once ---------------
    //
    // Stop and close are the user's escape paths and their cost is budgeted
    // (tests/test_shutdown_budget.cpp): a late call is never waited for there.
    {
        std::printf("--- stop() with a late retune out: no extra wait ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        const unsigned long long abandonedBefore = SoapySource::driverCallsAbandoned();
        SoapySource src;
        CHECK(src.open("driver=fakewedge, serial=latestop"));
        CHECK(src.start());
        g_wedgeSetFrequency.store(true, std::memory_order_relaxed);
        CHECK(!src.setCenterFrequencyHz(104.0e6));
        CHECK(!src.deviceDead());
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const long long stopMs = msSince(t0);
        std::printf("  stop() returned after %lld ms\n", stopMs);
        CHECK(stopMs < 500);
        CHECK(!src.running());
        CHECK(src.deviceDead());
        CHECK(src.deadReason() == SoapySource::DeadReason::Abandoned);
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);
        src.closeDevice();
        CHECK(g_closeStreamCalls.load(std::memory_order_acquire) == 0);
        releaseWedge();
        CHECK(waitForWedgedReturn(1));
    }

    // --- an abandoned radio must go on being COUNTED as open ---------------
    //
    // The process-wide open-device count is not bookkeeping about this class;
    // it is the answer to "may a vendor walk run right now" (anyDeviceOpen(),
    // read by the in-process enumeration fallback). Abandoning a wedged driver
    // deliberately does NOT close its stream and does NOT unmake it - a thread
    // of ours is still inside the module - so the radio is still open, in the
    // strongest sense the word has anywhere in this file, and the count said
    // zero at exactly that moment. A walk let through by that zero would open
    // and close the very dongle the stranded call is in, which is adjudicated
    // fix #1 for the 0.62.0 crashes performed against the worst possible
    // device.
    //
    // NOTE FOR ANYONE ADDING A BLOCK AFTER THIS ONE: from here to the end of
    // the process the gate is closed, by design. Any test needing an
    // in-process walk belongs above the first block that abandons a device.
    {
        std::printf("--- the open-device count after an abandonment ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        g_wedgeDeactivate.store(true, std::memory_order_relaxed);
        // A DELTA, not an absolute: earlier blocks in this file condemn
        // devices of their own and (correctly) leave them counted, so what
        // this block owns is the difference it makes.
        const int openBefore = SoapySource::openDeviceCount();
        {
            SoapySource src;
            CHECK(src.open("driver=fakewedge, serial=counted"));
            CHECK(SoapySource::openDeviceCount() == openBefore + 1);
            CHECK(src.start());
            src.stop();  // deactivateStream wedges: abandoned, device condemned
            CHECK(src.deviceDead());
            src.closeDevice();
            // PROVABLY STILL OPEN, and not by inference: the policy skipped
            // both calls that could have given the radio back.
            CHECK(g_closeStreamCalls.load(std::memory_order_acquire) == 0);
            CHECK(g_deviceDestroyed.load(std::memory_order_acquire) == 0);
            std::printf("  after abandonment + close: count=%d (was %d)\n",
                        SoapySource::openDeviceCount(), openBefore);
            CHECK(SoapySource::openDeviceCount() == openBefore + 1);
        }
        // Destroying the source releases nothing either - its destructor runs
        // the same closeDevice() - so the count must not move there.
        CHECK(g_deviceDestroyed.load(std::memory_order_acquire) == 0);
        CHECK(SoapySource::openDeviceCount() == openBefore + 1);
        CHECK(SoapySource::anyDeviceOpen());
        // AND THE GATE IS WHAT THE COUNT IS FOR. The walk would list
        // "fakewedge" if it ran (the registry above is live, and the same walk
        // lists this file's other fake earlier in this suite), so an empty
        // answer here is the gate holding against a module one of our threads
        // is still parked inside.
        CHECK(SoapySource::enumerateInProcess().empty());
        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        std::printf("  the abandoned call returned; the radio stays counted\n");
    }

    // --- condemned WHILE a read was queued for the driver lock --------------
    //
    // read() is the one entry point whose dead-latch test happens before it
    // takes the lock, and that test is stale by the time the lock is won: the
    // thread holding it can spend that wait abandoning a wedged driver. The
    // null-handle test read() then performs cannot stand in for a fresh latch
    // test, because an abandoned link keeps its handles on purpose - a
    // stranded worker is still reading them - so they never become null. The
    // read would go into the module on a stream whose deactivateStream is
    // still parked inside it.
    //
    // The reader is fired PART WAY through the wedge so that both halves of
    // the state are real: it passes the pre-lock test (nothing is condemned
    // yet) and is still parked on the mutex when stop() condemns the link and
    // releases it. Both are asserted below rather than assumed.
    {
        std::printf("--- condemned while a read waited for the lock ---\n");
        SoapySDR::Registry reg("fakewedge", &findWedge, &makeWedge,
                               SOAPY_SDR_ABI_VERSION);
        resetWedge();
        g_wedgeDeactivate.store(true, std::memory_order_relaxed);

        SoapySource src;
        CHECK(src.open("driver=fakewedge, serial=postlock"));
        CHECK(src.start());

        // The fake ANSWERS a read when it is called - without this, "the
        // driver was not entered" below would also be true of a device that
        // never delivers anything, and the block would prove nothing.
        std::vector<std::complex<float>> warm(64);
        CHECK(src.read(warm.data(), warm.size()) == warm.size());
        CHECK(g_readStreamCalls.load(std::memory_order_acquire) == 1);

        std::atomic<long long> readMs{-1};
        std::atomic<std::size_t> readGot{warm.size()};  // not 0, so a read that
                                                        // never ran cannot pass
        std::thread reader([&] {
            // Rendezvous on the DRIVER: g_inWedgedCall is set from inside the
            // wedged deactivateStream, so stop() provably holds the lock from
            // here. 900 ms of its 1500 ms bound is then spent before the read
            // starts, which leaves the read ~600 ms parked on the mutex out of
            // its own 1500 ms budget.
            while (!g_inWedgedCall.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(900));
            std::vector<std::complex<float>> buf(256);
            const auto r0 = std::chrono::steady_clock::now();
            const std::size_t got = src.read(buf.data(), buf.size());
            const long long ms = msSince(r0);
            readGot.store(got, std::memory_order_relaxed);
            readMs.store(ms, std::memory_order_release);
        });

        src.stop();  // holds the lock ~1.5 s, condemns the link, then releases
        reader.join();
        std::printf("  read() returned %zu after %lld ms; readStream calls=%d\n",
                    readGot.load(std::memory_order_relaxed),
                    readMs.load(std::memory_order_acquire),
                    g_readStreamCalls.load(std::memory_order_acquire));
        CHECK(src.deviceDead());
        // IT REALLY PARKED ON THE MUTEX. A read that returned at once either
        // never entered (nothing to test) or saw the latch before the wait
        // (the pre-lock test, which was never the hole) - so without this the
        // assertion below could pass on a race that did not reproduce.
        CHECK(readMs.load(std::memory_order_acquire) >= 200);
        // ...and it came back out without entering the driver.
        CHECK(readGot.load(std::memory_order_relaxed) == 0u);
        CHECK(g_readStreamCalls.load(std::memory_order_acquire) == 1);

        releaseWedge();
        CHECK(waitForWedgedReturn(1));
        std::printf("  the abandoned deactivate returned; the read never went in\n");
    }

    // ----------------------------------------------------------------
    // THE STREAM-HEALTH LINE. The 0.88.0 field crash arrived with a log that
    // said nothing about the radio for the minute before the driver died;
    // the read loop now tallies what the driver answers and writes one line
    // a minute. A scripted fake answers samples, timeouts, an overflow and a
    // hard error in a known order; the line must classify each one exactly,
    // and the loop must write it on its own once the window has passed.
    {
        std::printf("-- stream health line --\n");
        SoapySDR::Registry reg("fakehealth", &findHealth, &makeHealth,
                               SOAPY_SDR_ABI_VERSION);
        SoapySource src;
        CHECK(src.open("driver=fakehealth"));
        CHECK(src.start());
        std::vector<std::complex<float>> buf(64);
        // samples, timeout, timeout, overflow, samples, hard error, samples
        g_healthScript = {64, SOAPY_SDR_TIMEOUT, SOAPY_SDR_TIMEOUT, SOAPY_SDR_OVERFLOW,
                          64, SOAPY_SDR_STREAM_ERROR, 64};
        g_healthAt = 0;
        for (std::size_t i = 0; i < g_healthScript.size(); ++i) {
            (void)src.read(buf.data(), buf.size());
        }
        const std::string line = src.streamHealthLine();
        std::printf("  %s\n", line.c_str());
        CHECK(line.find("source: stream health - reads 7, with samples 3, timeouts 2, "
                        "overflows 1, errors 1, longest gap ") != std::string::npos);
        CHECK(line.find(", 192 samples in ") != std::string::npos);
        // The window starts again: nothing to say until the next read.
        CHECK(src.streamHealthLine().empty());

        // THE LOOP WRITES IT ITSELF once the window has passed - shortened
        // here so the test does not wait a minute. A window with a timeout in
        // it is not nominal, so it must be written whatever came before.
        // The line is written BY THE READ THAT CROSSES THE WINDOW, so it
        // carries that read and the ones before it: two reads, one timeout.
        src.setStreamHealthWindowForTest(std::chrono::milliseconds(40));
        g_healthScript = {64, SOAPY_SDR_TIMEOUT, 64};
        g_healthAt = 0;
        (void)src.read(buf.data(), buf.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        (void)src.read(buf.data(), buf.size());
        (void)src.read(buf.data(), buf.size());
        CHECK(lastDiagLine("source: stream health - reads 2, with samples 1, timeouts 1, "));

        // AND A NOMINAL MINUTE AFTER THAT IS NOT WRITTEN: the log must not
        // carry a line a minute for a radio that is simply working. The count
        // of lines written is the honest probe - the newest line alone cannot
        // tell "not written" from "written again".
        const std::uint64_t before = cascade::core::DiagLog::instance().linesWritten();
        g_healthScript = {64, 64, 64};
        g_healthAt = 0;
        (void)src.read(buf.data(), buf.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        (void)src.read(buf.data(), buf.size());
        (void)src.read(buf.data(), buf.size());
        CHECK(cascade::core::DiagLog::instance().linesWritten() == before);
        src.stop();
    }

    // -----------------------------------------------------------------------
    // THE DeviceSource SURFACE. SoapySource implements the interface the
    // Source panel is written against from 0.91.0, so an RTL-SDR opened
    // natively and one opened through a vendor module are the same panel.
    // What this block proves is that a Soapy device answers every one of
    // those questions with the DRIVER'S OWN numbers.
    // -----------------------------------------------------------------------
    {
        std::printf("--- DeviceSource surface ---\n");

        // BEFORE OPEN: every accessor is safe and says "nothing", which is
        // the state the panel draws for the generator and the IQ file.
        {
            SoapySource src;
            CHECK(src.gains().empty());
            CHECK(src.antennas().empty());
            CHECK(src.antenna().empty());
            CHECK(src.supportedSampleRatesHz().empty());
            CHECK(!src.autoGainSupported());
            CHECK(!src.autoGain());
            CHECK(src.gainDb("LNA") == 0.0);
            CHECK(src.driverKey() != nullptr);
            CHECK(std::strcmp(src.driverKey(), "soapy") == 0);
        }

        SoapySDR::Registry reg("fakeshape", &findShape, &makeShape, SOAPY_SDR_ABI_VERSION);
        g_shapeAntenna = "RX2";
        g_shapeGains.clear();
        SoapySource src;
        CHECK(src.open("driver=fakeshape"));

        // THE DRIVER KEY comes out of the ARGS, lower-cased, because the
        // prefer-native rule has to ask it about devices it has not opened.
        CHECK(std::strcmp(src.driverKey(), "fakeshape") == 0);

        // THE GAIN STAGES, WITH THEIR REAL RANGES. Three stages, three
        // DIFFERENT ranges, one of them starting below zero. RED WHEN gains()
        // falls back to a fixed span: the -4.7 and the 16.1 are what a 0..60
        // default cannot produce.
        const std::vector<cascade::source::GainInfo> g = src.gains();
        CHECK(g.size() == 3u);
        if (g.size() == 3u) {
            CHECK(g[0].name == "LNA");
            CHECK(g[0].minDb == 0.0);
            CHECK(g[0].maxDb == 33.5);
            CHECK(g[1].name == "MIXER");
            CHECK(g[1].maxDb == 16.1);
            CHECK(g[2].name == "VGA");
            CHECK(g[2].minDb == -4.7);
            CHECK(g[2].maxDb == 40.8);
            CHECK(g[2].stepDb > 0.0);  // never zero: a slider needs a step
            // AND ALL OF THEM DECIBELS. SoapySDR's getGainRange is documented
            // in dB and every vendor module reports it so; a Soapy device is
            // never the steps case, whatever driver key it carries.
            for (const cascade::source::GainInfo& one : g) {
                CHECK(one.unit == cascade::source::GainUnit::Decibels);
            }
        }
        // The names still come out of listGainNames() too - nothing the GUI
        // used before was taken away.
        CHECK(src.listGainNames() == (std::vector<std::string>{"LNA", "MIXER", "VGA"}));

        // GAIN IS REPORTED AS THE DRIVER COERCED IT, NOT AS ASKED. 40 dB into
        // a 16.1 dB mixer comes back as 16, and 21.7 into the LNA comes back
        // as 21. RED WHEN setGainDb stops reading the value back: gainDb()
        // then answers 40 and 21.7, and the panel claims gain the radio never
        // had.
        CHECK(src.setGainDb("MIXER", 40.0));
        CHECK(src.gainDb("MIXER") == 16.0);
        CHECK(src.setGainDb("LNA", 21.7));
        CHECK(src.gainDb("LNA") == 21.0);
        CHECK(src.setGainDb("VGA", -30.0));
        CHECK(src.gainDb("VGA") == -4.0);  // clamped to -4.7, then truncated
        // An unknown stage changes nothing and reads back zero.
        CHECK(src.gainDb("NOSUCH") == 0.0);

        // AGC: supported here, and autoGain() follows what was actually SET.
        CHECK(src.autoGainSupported());
        CHECK(!src.autoGain());
        CHECK(src.setAutoGain(true));
        CHECK(src.autoGain());
        CHECK(src.setAutoGain(false));
        CHECK(!src.autoGain());

        // ANTENNAS: the list, and a selection reported from the driver's own
        // readback rather than from the request.
        CHECK(src.antennas() == (std::vector<std::string>{"TX/RX", "RX2"}));
        CHECK(src.antenna() == "RX2");  // the device's boot port, read at open
        CHECK(src.setAntenna("TX/RX"));
        CHECK(src.antenna() == "TX/RX");
        CHECK(src.antennaReadback() == "TX/RX");
        // A port this device does not have is refused and changes nothing.
        CHECK(!src.setAntenna("RX9"));
        CHECK(src.antenna() == "TX/RX");

        // THE RATE LIST IS THE DRIVER'S OWN, ascending. RED WHEN it falls
        // back to the fixed 1/2/4/8 MS/s table: 2.4 MS/s - the rate ADS-B
        // needs - is in this list and in no fixed one.
        const std::vector<double> rates = src.supportedSampleRatesHz();
        CHECK(rates.size() == 5u);
        if (rates.size() == 5u) {
            CHECK(rates[0] == 250000.0);
            CHECK(rates[3] == 2400000.0);
            CHECK(rates[4] == 3200000.0);
        }
        for (std::size_t i = 1; i < rates.size(); ++i) { CHECK(rates[i] > rates[i - 1]); }

        // CLOSING FORGETS ALL OF IT. A stale gain range or antenna list
        // surviving a close would be drawn by the panel against whatever is
        // opened next.
        src.closeDevice();
        CHECK(src.gains().empty());
        CHECK(src.antennas().empty());
        CHECK(src.antenna().empty());
        CHECK(src.supportedSampleRatesHz().empty());
        CHECK(!src.autoGainSupported());
        CHECK(std::strcmp(src.driverKey(), "soapy") == 0);
    }

    // A DRIVER THAT ANSWERS NO RATE LIST AT ALL still gives the panel
    // something to show. UHD is exactly this: a B200 takes any rate its
    // master clock divides to, so listSampleRates comes back empty, and a
    // Rate control with no rows in it would be a regression on every device
    // that has always shown 1/2/4/8.
    {
        SoapySDR::Registry reg("fakehealth", &findHealth, &makeHealth, SOAPY_SDR_ABI_VERSION);
        SoapySource src;
        CHECK(src.open("driver=fakehealth"));
        const std::vector<double> rates = src.supportedSampleRatesHz();
        std::printf("no-list driver falls back to %zu rates\n", rates.size());
        CHECK(rates == (std::vector<double>{1.0e6, 2.0e6, 4.0e6, 8.0e6}));
        // ...and a driver with no gains and no antennas is not an error
        // either: the panel simply draws no sliders.
        CHECK(src.gains().empty());
        src.closeDevice();
    }

    return testSummary("test_soapy_source");
}
