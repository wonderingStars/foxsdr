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
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
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
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        if (g_wedgeDeactivate.load(std::memory_order_relaxed)) { wedgeUntilReleased(); }
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

        const auto t0 = std::chrono::steady_clock::now();
        src.closeDevice();
        const long long closeMs = msSince(t0);
        std::printf("  closeDevice() returned after %lld ms\n", closeMs);
        CHECK(closeMs < 3000);
        CHECK(g_inWedgedCall.load(std::memory_order_acquire));
        CHECK(SoapySource::driverCallsAbandoned() == abandonedBefore + 1);
        CHECK(src.deviceDead());
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

    return testSummary("test_soapy_source");
}
