// Tests for source/soapy_source.hpp — the no-hardware surface.
//
// This machine may have the SoapySDR core library with ZERO vendor modules
// installed (vcpkg ships only the core), and no SDR attached even if a
// module were present. Every check below therefore targets behavior that
// must hold with nothing plugged in:
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
// SPDX-License-Identifier: MIT
#include "source/soapy_source.hpp"

#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_check.hpp"

using cascade::source::SoapyDeviceInfo;
using cascade::source::SoapySource;

int main() {
    // --- enumerate(): completes, well-formed, repeatable ---------------------
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
        // A second scan must not crash or throw either (menu refresh path),
        // and on a static machine the population is stable within one run.
        const std::vector<SoapyDeviceInfo> again = SoapySource::enumerate();
        CHECK(again.size() == devices.size());
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

        // read() with no stream: immediate 0 (retry signal), dst untouched.
        std::complex<float> buf[8] = {};
        buf[0] = {123.0f, -123.0f};  // sentinel proves no write happened
        CHECK(src.read(buf, 8) == 0u);
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

    return testSummary("test_soapy_source");
}
