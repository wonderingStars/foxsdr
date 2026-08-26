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

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
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
    double getFrequency(const int, const size_t) const override { return 100.0e6; }

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
};

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

    return testSummary("test_soapy_source");
}
