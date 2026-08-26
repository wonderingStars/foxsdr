// Fault injection into SoapySource's CONTROL PATH, one block per crash
// signature real users uploaded from the shipped 0.62.0.
//
// WHAT THIS FILE IS EVIDENCE FOR. Five reports, three signatures, at least two
// people, two RTL-SDR models, two Windows 11 builds, all in 24 hours, all the
// same shape: cascade frame -> SoapySDR.dll -> rtlsdrSupport.dll -> rtlsdr.dll
// -> libusb-1.0.dll -> ntdll.dll, and the process dies.
//
//   [1] B63B14A9BA45C175  the user pressed PLAY        SoapySource::start
//   [2] 235E46B5D39DED8D  the user CHANGED FREQUENCY   SoapySource::setCenterFrequencyHz
//   [3] 7496A711C2D58EE2  the user SWITCHED SOURCE     SoapySource::teardown
//
// Unlike the B200 enumeration fault (a thread UHD spawns for itself, which no
// in-process guard can reach - see source/vendor_guard.hpp), every one of these
// is raised on OUR OWN CALL FRAME. A structured exception is delivered on the
// thread that raised it, so a __try/__except around the call catches it, and
// that is what these three blocks prove.
//
// HOW THE FAULT IS MADE, and why it is a real one rather than a mimed one.
// There is no RTL-SDR on the machine that runs this suite, so the users' exact
// fault cannot be reproduced and is not attempted. Instead SoapySDR's device
// registry - a public in-process API, already used this way by
// tests/test_soapy_source.cpp - registers a driver whose Device methods call
// ntdll's exported memset through a null pointer on demand. That raises a
// GENUINE 0xC0000005 whose faulting instruction lives inside ntdll.dll, i.e.
// foreign code reached from our thread through SoapySDR's own dispatch: the
// same shape as libusb, and the shape the guard's module-scoped filter needs
// before it will absorb anything. A store written inside this file would fault
// at an address in the TEST's image, which the guard deliberately refuses to
// absorb, and the process would die - so the choice of ntdll is load-bearing.
//
// WHY EACH BLOCK FAILS IF ITS GUARD IS REMOVED. Nothing here is a mock: the
// faulting call really is dev_->activateStream / dev_->setFrequency /
// dev_->closeStream, dispatched virtually out of the production method. Take
// the __try off that one call site and this executable does not report a
// failed CHECK, it dies with 0xC0000005 and ctest records the crash. Verified
// by doing exactly that to each of the three, one at a time.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include "source/vendor_guard.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "test_check.hpp"

using cascade::source::SoapySource;
using cascade::source::vendorGuardCallCount;
using cascade::source::vendorGuardFaultCount;
using cascade::source::vendorGuardLastFaultCode;

namespace {

#ifdef _WIN32

// Which driver entry point faults on its next call. One at a time, because a
// block that armed two would not be able to say which one the guard caught.
enum class FaultAt {
    None,
    ActivateStream,
    DeactivateStream,
    SetFrequency,
    SetSampleRate,
    CloseStream,
    SetGain,
    SetAntenna,
};

FaultAt g_faultAt = FaultAt::None;

// Every driver call the fake serves, counted. This is how "a dead device is
// never called again" is asserted: not by inspecting a flag, but by proving
// the driver's own entry points stopped being entered.
long g_driverCalls = 0;

// Devices actually DESTROYED, i.e. reached through SoapySDR::Device::unmake.
// The abandonment half of the dead-device policy is asserted against this: a
// device that faulted must NOT be unmade, so this must not move.
long g_devicesDestroyed = 0;

// ntdll's memset, called with a null destination. The faulting instruction is
// then inside ntdll.dll - foreign to the guard's image, which is what the
// module-scoped filter requires. NON-const so the optimiser cannot fold the
// call away and turn this test into nothing.
using MemsetFn = void*(__cdecl*)(void*, int, std::size_t);
MemsetFn g_ntdllMemset = nullptr;

void faultNowInForeignModule() {
    g_ntdllMemset(nullptr, 0, 64);  // genuine 0xC0000005, raised inside ntdll
}

// Fires (once) if this entry point is the armed one. Disarms itself so a
// method called again after the fault - which must not happen, and is asserted
// - would be visible as a call rather than a second crash.
void maybeFault(FaultAt site) {
    ++g_driverCalls;
    if (g_faultAt != site) { return; }
    g_faultAt = FaultAt::None;
    faultNowInForeignModule();
}

constexpr double kBootRateHz = 2.048e6;
constexpr double kBootFreqHz = 100.1e6;

class FaultyDevice : public SoapySDR::Device {
public:
    ~FaultyDevice() override { ++g_devicesDestroyed; }

    std::string getDriverKey() const override { return "faultydriver"; }
    std::string getHardwareKey() const override { return "faulty test device"; }
    size_t getNumChannels(const int) const override { return 1; }

    SoapySDR::Stream* setupStream(const int, const std::string&,
                                  const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        // Opaque to SoapySource; any non-null value will do.
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {
        maybeFault(FaultAt::CloseStream);
    }
    int activateStream(SoapySDR::Stream*, const int, const long long,
                       const size_t) override {
        maybeFault(FaultAt::ActivateStream);
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
        maybeFault(FaultAt::DeactivateStream);
        return 0;
    }

    double getSampleRate(const int, const size_t) const override { return rate_; }
    void setSampleRate(const int, const size_t, const double hz) override {
        maybeFault(FaultAt::SetSampleRate);
        rate_ = hz;
    }
    double getFrequency(const int, const size_t) const override { return freq_; }
    void setFrequency(const int, const size_t, const double hz,
                      const SoapySDR::Kwargs&) override {
        maybeFault(FaultAt::SetFrequency);
        freq_ = hz;
    }

    std::vector<std::string> listGains(const int, const size_t) const override {
        return {"TUNER"};
    }
    void setGain(const int, const size_t, const std::string&,
                 const double db) override {
        maybeFault(FaultAt::SetGain);
        gain_ = db;
    }
    bool hasGainMode(const int, const size_t) const override { return true; }
    void setGainMode(const int, const size_t, const bool on) override {
        agc_ = on;
    }

    std::vector<std::string> listAntennas(const int, const size_t) const override {
        return {"RX", "RX2"};
    }
    void setAntenna(const int, const size_t, const std::string& name) override {
        maybeFault(FaultAt::SetAntenna);
        antenna_ = name;
    }
    std::string getAntenna(const int, const size_t) const override {
        return antenna_;
    }

    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems,
                   int&, long long&, const long) override {
        float* p = static_cast<float*>(buffs[0]);
        for (size_t i = 0; i < 2 * numElems; ++i) { p[i] = 0.125f; }
        return static_cast<int>(numElems);
    }

private:
    double rate_ = kBootRateHz;
    double freq_ = kBootFreqHz;
    double gain_ = 0.0;
    bool agc_ = false;
    std::string antenna_ = "RX";
};

SoapySDR::KwargsList findFaulty(const SoapySDR::Kwargs& args) {
    const auto driver = args.find("driver");
    if (driver == args.end() || driver->second != "faultydriver") { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = "faultydriver";
    k["label"] = "faulty test device";
    // ECHOED BACK, and this matters: SoapySDR caches made devices by their
    // resolved args, so without a distinct instance key every block below
    // would be handed the SAME device - including the one a previous block
    // deliberately abandoned without unmaking.
    const auto instance = args.find("instance");
    k["instance"] = (instance != args.end()) ? instance->second : "0";
    return SoapySDR::KwargsList{k};
}

SoapySDR::Device* makeFaulty(const SoapySDR::Kwargs&) {
    return new FaultyDevice();  // owned by SoapySDR, released by unmake()
}

// A device opened and confirmed healthy, so that every fault below is injected
// into a KNOWN-GOOD device rather than into an open that might have failed for
// its own reasons.
bool openHealthy(SoapySource& src, const char* instance) {
    g_faultAt = FaultAt::None;
    const bool ok = src.open(std::string("driver=faultydriver, instance=") + instance);
    CHECK(ok);
    CHECK(src.isOpen());
    CHECK(!src.faulted());
    CHECK(!src.deviceDead());
    CHECK(src.centerFrequencyHz() == kBootFreqHz);
    CHECK(src.sampleRateHz() == kBootRateHz);
    return ok;
}

#endif  // _WIN32

}  // namespace

int main() {
#ifndef _WIN32
    // The guard is a counted passthrough off Windows (no recoverable SIGSEGV),
    // so there is no absorbed fault to inject. Asserted rather than skipped so
    // this file never silently tests nothing on a platform.
    {
        SoapySource src;
        CHECK(!src.deviceDead());
        CHECK(!src.faulted());
    }
    return testSummary("test_soapy_vendor_guard");
#else
    {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        CHECK(ntdll != nullptr);
        g_ntdllMemset = reinterpret_cast<MemsetFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "memset")));
        // Asserted, not skipped: if this export ever moves, every block below
        // must fail loudly rather than quietly injecting nothing.
        CHECK(g_ntdllMemset != nullptr);
    }
    SoapySDR::Registry reg("faultydriver", &findFaulty, &makeFaulty,
                           SOAPY_SDR_ABI_VERSION);

    // --- THE HEALTHY PATH STILL WORKS ---------------------------------------
    //
    // First, because a guard that broke ordinary operation would make every
    // fault block below meaningless: they would be proving that a receiver
    // nobody can use does not crash. Every guarded control call is exercised
    // and its effect READ BACK from the driver.
    {
        SoapySource src;
        if (openHealthy(src, "healthy")) {
            const std::uint64_t calls = vendorGuardCallCount();

            CHECK(src.start());
            CHECK(src.running());

            CHECK(src.setCenterFrequencyHz(101.5e6));
            CHECK(src.centerFrequencyHz() == 101.5e6);  // readback, not echo
            CHECK(src.setSampleRateHz(1.024e6));
            CHECK(src.sampleRateHz() == 1.024e6);

            const std::vector<std::string> gains = src.listGainNames();
            CHECK(gains == (std::vector<std::string>{"TUNER"}));
            CHECK(src.setGainDb("TUNER", 28.0));
            CHECK(src.setAutoGain(true));

            const std::vector<std::string> ports = src.listAntennas();
            CHECK(ports == (std::vector<std::string>{"RX", "RX2"}));
            CHECK(src.setAntenna("RX2"));
            CHECK(src.antenna() == "RX2");
            CHECK(!src.setAntenna("NOT_A_PORT"));  // still refused, not faulted
            CHECK(!src.deviceDead());

            // Samples still flow through the unguarded streaming path.
            std::complex<float> buf[64] = {};
            CHECK(src.read(buf, 64) == 64u);

            src.stop();
            CHECK(!src.running());

            // EVERY ONE OF THOSE WENT THROUGH THE GUARD. Deleting the guard
            // from any of them drops this count, so the wiring is a test
            // failure rather than a silent return to the unguarded behaviour.
            CHECK(vendorGuardCallCount() >= calls + 10);
            CHECK(!src.faulted());

            const long destroyed = g_devicesDestroyed;
            src.closeDevice();
            CHECK(!src.isOpen());
            // A device that never faulted IS unmade - the abandonment policy
            // must not have leaked into the ordinary path.
            CHECK(g_devicesDestroyed == destroyed + 1);
        }
    }

    // --- [1] THE USER PRESSED PLAY: a fault inside activateStream ------------
    {
        SoapySource src;
        if (openHealthy(src, "play")) {
            const std::uint64_t faults = vendorGuardFaultCount();
            g_faultAt = FaultAt::ActivateStream;

            const bool started = src.start();

            // REACHING THIS LINE AT ALL is half the assertion: with no guard
            // on activateStream the process is already gone.
            CHECK(!started);                    // the caller learns it failed
            CHECK(!src.running());              // and the state machine agrees
            CHECK(vendorGuardFaultCount() == faults + 1);
            CHECK(vendorGuardLastFaultCode() == 0xC0000005u);
            CHECK(src.faulted());               // Pipeline's source loop polls this
            CHECK(src.deviceDead());
            CHECK(std::strlen(src.lastError()) > 0);
            CHECK(std::strstr(src.lastError(), "0xC0000005") != nullptr);

            // A DEAD DEVICE IS NEVER CALLED AGAIN. Not asserted through a flag
            // but through the driver's own entry points: none of these may
            // reach it.
            const long callsBefore = g_driverCalls;
            CHECK(!src.start());
            CHECK(!src.setCenterFrequencyHz(99.0e6));
            CHECK(!src.setSampleRateHz(1.0e6));
            CHECK(!src.setGainDb("TUNER", 10.0));
            CHECK(!src.setAutoGain(false));
            CHECK(!src.setAntenna("RX2"));
            CHECK(src.listGainNames().empty());
            CHECK(src.listAntennas().empty());
            CHECK(src.antenna().empty());
            src.stop();
            CHECK(g_driverCalls == callsBefore);

            // ...and it is released WITHOUT closeStream or unmake.
            const long destroyed = g_devicesDestroyed;
            src.closeDevice();
            CHECK(g_driverCalls == callsBefore);
            CHECK(g_devicesDestroyed == destroyed);  // deliberately leaked
            CHECK(!src.isOpen());
            CHECK(!src.running());
            CHECK(std::strcmp(src.name(), "SoapySDR: (no device)") == 0);
            // The reason survives the close - the Source panel shows it after
            // the half-open attempt has been cleaned up.
            CHECK(std::strlen(src.lastError()) > 0);
        }
    }

    // --- [2] THE USER CHANGED FREQUENCY: a fault inside setFrequency ---------
    {
        SoapySource src;
        if (openHealthy(src, "tune")) {
            CHECK(src.start());
            CHECK(src.setCenterFrequencyHz(103.0e6));
            CHECK(src.centerFrequencyHz() == 103.0e6);

            const std::uint64_t faults = vendorGuardFaultCount();
            g_faultAt = FaultAt::SetFrequency;

            const bool tuned = src.setCenterFrequencyHz(107.9e6);

            CHECK(!tuned);  // the caller learns the radio did not move
            CHECK(vendorGuardFaultCount() == faults + 1);
            CHECK(vendorGuardLastFaultCode() == 0xC0000005u);
            // THE READOUT MUST NOT ADVERTISE A FREQUENCY THE RADIO IS NOT ON.
            // The requested value is not written in and neither is a readback
            // that never completed: what stands is the last value the hardware
            // actually confirmed. AppWindow::retuneSourceHz feeds the plugin
            // decoders from centerFrequencyHz(), so this is what keeps the
            // whole UI agreeing with itself after a failed tune.
            CHECK(src.centerFrequencyHz() == 103.0e6);
            CHECK(src.faulted());
            CHECK(src.deviceDead());
            CHECK(std::strstr(src.lastError(), "0xC0000005") != nullptr);

            src.closeDevice();
            CHECK(!src.isOpen());
        }
    }

    // --- [3] THE USER SWITCHED SOURCE: a fault during DESTRUCTION ------------
    //
    // The hard one. teardown() runs from ~SoapySource, reached through
    // Pipeline::setSource, so a fault here must leave no half-destroyed object,
    // must not double-free, and must not throw out of a destructor.
    {
        const std::uint64_t faults = vendorGuardFaultCount();
        const long destroyed = g_devicesDestroyed;
        {
            SoapySource src;
            if (openHealthy(src, "swap")) {
                CHECK(src.start());
                g_faultAt = FaultAt::CloseStream;

                src.closeDevice();  // stop() then teardown(): the crash [3] path

                CHECK(vendorGuardFaultCount() == faults + 1);
                CHECK(vendorGuardLastFaultCode() == 0xC0000005u);

                // FULLY torn down, not half. Every one of these is what the
                // object promises after teardown, whatever happened inside it.
                CHECK(!src.isOpen());
                CHECK(!src.running());
                CHECK(src.sampleRateHz() == 0.0);
                CHECK(src.centerFrequencyHz() == 0.0);
                CHECK(std::strcmp(src.name(), "SoapySDR: (no device)") == 0);
                // teardown clears the fault latches - there is no device left
                // to be faulted about - while lastError keeps the reason.
                CHECK(!src.faulted());
                CHECK(!src.deviceDead());
                CHECK(std::strstr(src.lastError(), "0xC0000005") != nullptr);

                // unmake was NOT called: the handle is abandoned on purpose,
                // because the module that just raised an access violation may
                // hold its own locks, and a hang is worse than a leak.
                CHECK(g_devicesDestroyed == destroyed);

                // A second close, and then the destructor, must be quiet
                // no-ops rather than a double free.
                const long calls = g_driverCalls;
                src.closeDevice();
                CHECK(g_driverCalls == calls);
                CHECK(g_devicesDestroyed == destroyed);
            }
            // ~SoapySource runs here.
        }
        // The destructor of an abandoned device neither faulted nor unmade.
        CHECK(g_devicesDestroyed == destroyed);
    }

    // --- THE APPLICATION IS STILL USABLE AFTERWARDS --------------------------
    //
    // Three absorbed access violations ago. Surviving one fault and then being
    // unable to open a radio would look identical to the fix working, right up
    // until the user tries again - which is exactly what they do next.
    {
        SoapySource src;
        if (openHealthy(src, "again")) {
            CHECK(src.start());
            CHECK(src.running());
            CHECK(src.setCenterFrequencyHz(88.5e6));
            CHECK(src.centerFrequencyHz() == 88.5e6);
            std::complex<float> buf[32] = {};
            CHECK(src.read(buf, 32) == 32u);
            CHECK(!src.faulted());
            CHECK(!src.deviceDead());
            const long destroyed = g_devicesDestroyed;
            src.closeDevice();
            CHECK(g_devicesDestroyed == destroyed + 1);
        }
    }

    // --- the remaining guarded control calls, faulted one at a time ----------
    //
    // stop(), the sample rate, the gain and the antenna are not among the three
    // uploaded signatures, but they reach the same libusb surface through the
    // same dispatch, so they get the same proof rather than the same argument.
    {
        struct Case {
            const char* instance;
            FaultAt site;
        };
        const Case cases[] = {
            {"stop", FaultAt::DeactivateStream},
            {"rate", FaultAt::SetSampleRate},
            {"gain", FaultAt::SetGain},
            {"ant", FaultAt::SetAntenna},
        };
        for (const Case& c : cases) {
            SoapySource src;
            if (!openHealthy(src, c.instance)) { continue; }
            CHECK(src.start());
            const std::uint64_t faults = vendorGuardFaultCount();
            g_faultAt = c.site;

            switch (c.site) {
                case FaultAt::DeactivateStream:
                    src.stop();  // void: reports through faulted()/lastError()
                    // running_ is cleared BEFORE the driver is touched, so a
                    // fault on the way out still lands in "stopped".
                    CHECK(!src.running());
                    break;
                case FaultAt::SetSampleRate:
                    CHECK(!src.setSampleRateHz(1.8e6));
                    CHECK(src.sampleRateHz() == kBootRateHz);  // unchanged
                    break;
                case FaultAt::SetGain:
                    CHECK(!src.setGainDb("TUNER", 44.0));
                    break;
                case FaultAt::SetAntenna:
                    CHECK(!src.setAntenna("RX2"));
                    break;
                default:
                    CHECK(false);  // unreachable; a new case must be handled
                    break;
            }

            std::printf("faulted %s: alive, fault absorbed\n", c.instance);
            CHECK(vendorGuardFaultCount() == faults + 1);
            CHECK(vendorGuardLastFaultCode() == 0xC0000005u);
            CHECK(src.faulted());
            CHECK(src.deviceDead());
            CHECK(std::strstr(src.lastError(), "0xC0000005") != nullptr);

            const long destroyed = g_devicesDestroyed;
            src.closeDevice();
            CHECK(g_devicesDestroyed == destroyed);  // abandoned, as designed
        }
    }

    return testSummary("test_soapy_vendor_guard");
#endif
}
