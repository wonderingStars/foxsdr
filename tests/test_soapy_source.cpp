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
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
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
            std::atomic<bool> reading{false};
            std::thread reader([&] {
                std::vector<std::complex<float>> buf(4096);
                reading.store(true, std::memory_order_relaxed);
                (void)src.read(buf.data(), buf.size());
            });
            while (!reading.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

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

    return testSummary("test_soapy_source");
}
