// Real-hardware IQ source through SoapySDR: any vendor whose module is
// installed at runtime (SoapyUHD for the B200, SoapyRTLSDR, ...) appears
// here without cascade linking a single GPL driver — that runtime-module
// indirection is what keeps this tree MIT (see PLAN.md legal ground rules).
//
// A machine with the core SoapySDR library but ZERO vendor modules (this
// vcpkg tree, most CI boxes) is a fully supported configuration: enumerate()
// returns an empty list and open() fails gracefully with a reason. Nothing
// in this class treats "no hardware" as an error to crash over.
//
// Threading: matches the IqSource contract — open/closeDevice/start/stop and
// every setter are control-thread calls; read() runs only on the pipeline's
// source thread. Callers must stop the source thread before closeDevice()
// (Pipeline already sequences it that way); members are therefore plain, not
// atomic, on purpose.
//
// The ERROR SLOT is the one exception, and it has to be: read() records
// failures from the source thread at the same time as the GUI reads
// lastError() and the source loop polls faulted() — a std::string written on
// one thread and read on another is UB, not a stale value. Those four members
// (lastError_, faulted_, consecutiveErrors_) live under errorMutex_ and are
// reached only through setError/clearError/lastError/faulted; nothing else in
// the class changed, because nothing else is touched concurrently.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "source/soapy_modules.hpp"

#include "source/iq_source.hpp"

// Forward declarations instead of <SoapySDR/Device.hpp>: the GUI includes
// this header for the Source menu, and it must not inherit the whole Soapy
// API surface (or its Config.h macros) just to name a device pointer.
namespace SoapySDR {
class Device;
class Stream;
}  // namespace SoapySDR

namespace cascade::source {

// One enumerated device, as the Source menu wants it: a human label to show
// and the exact kwargs markup string that reopens that same device later.
struct SoapyDeviceInfo {
    std::string label;  // e.g. "B200 mini", falls back to the driver key
    std::string args;   // "driver=uhd, serial=..." — feed straight to open()
};

// Self-paced hardware source (selfPaced() == true): samples arrive at the
// device's own rate, so read() blocks up to ~100 ms and returns 0 as the
// "nothing yet, retry" signal the IqSource contract defines.
class SoapySource : public IqSource {
public:
    SoapySource() = default;
    ~SoapySource() override;

    // Owns a raw device handle from Device::make(); copying would double-
    // unmake it, so ownership is unique and non-transferable.
    SoapySource(const SoapySource&) = delete;
    SoapySource& operator=(const SoapySource&) = delete;

    // Lists every device the installed runtime modules can see. Never
    // throws: with no modules (or a module that fails to probe) the result
    // is simply empty — that is the normal moduleless-machine answer, not
    // an error.
    //
    // RUNS IN A CHILD PROCESS. The vendor walk loads every SDR driver on the
    // machine and lets each one scan the USB bus, and on this bench that
    // faults about once in twenty scans (0xC0000005 inside libusb, on a
    // thread UHD spawns for itself — which no in-process guard can catch;
    // proved, see source/vendor_guard.hpp). Isolating it is the only thing
    // that works: the child dies, the parent reads an empty list and logs the
    // exit code, and the session survives. source/soapy_enum_proc.hpp has the
    // outcomes, the timeout and the retry; call enumerateIsolated() directly
    // if you need to tell "no devices" from "the probe died".
    static std::vector<SoapyDeviceInfo> enumerate();

    // The walk itself, in THIS process, under the structured-exception guard
    // (source/vendor_guard.hpp). This is what the child process runs, and what
    // enumerate() falls back to when no child can be started at all.
    //
    // Not for general use: it carries the crash exposure described above. It
    // is public because the child helper is a separate translation unit, and
    // named so that a call site reads as the deliberate choice it has to be.
    static std::vector<SoapyDeviceInfo> enumerateInProcess();

    // False when SoapySDR.dll cannot be loaded (missing or broken install).
    // The DLL is delay-loaded, so the app runs fine without it — only
    // hardware sources are unavailable. Every Soapy entry point checks this
    // first so a missing runtime yields a message, never a crash.
    static bool runtimeAvailable();

    // Diagnostics for the "no devices found" case, which used to say nothing
    // at all. A user whose radio is missing needs to know WHERE the
    // application looked and WHAT it loaded; without that, "no devices" is
    // indistinguishable from a broken application, and for one release it
    // genuinely was one.
    //
    // All three are safe with no runtime present: they return empty.
    static std::vector<std::string> moduleSearchPaths();
    static std::vector<std::string> loadedModules();
    static std::vector<VendorRoot> vendorInstalls();

    // Opens the device described by a SoapySDR kwargs markup string (the
    // .args of an enumerate() entry, or hand-written like "driver=rtlsdr")
    // and sets up its RX CF32 stream on channel 0. Any previously open
    // device is torn down first, so reopen is safe. False + lastError() on
    // any failure; no partial state survives a failed open.
    bool open(const std::string& args);

    // Full teardown: deactivate, close stream, release the device handle.
    // Idempotent and safe on a never-opened instance. lastError() is left
    // untouched so a failure reason survives the cleanup that follows it.
    void closeDevice();

    bool isOpen() const { return dev_ != nullptr; }

    // --- gain hooks for the Source panel (GUI wires these later) ----------
    // All are safe with no device open: empty list / false, never a throw.

    // Names of the device's RX amplification stages, RF-first (Soapy's
    // documented ordering), e.g. {"PGA"} on a B200.
    std::vector<std::string> listGainNames();

    // Sets one named gain element in dB. False if there is no device, the
    // name is unknown, or the driver refused the value.
    bool setGainDb(const std::string& name, double db);

    // Hardware AGC on/off. False when there is no device or the driver has
    // no gain mode (hasGainMode() false) — the GUI greys the checkbox then.
    bool setAutoGain(bool on);

    // The device's RX antenna ports, e.g. {"TX/RX", "RX2"} on a B200.
    //
    // This exists because its absence was a silent, complete failure. With no
    // way to choose a port the driver's default is used, which on a USRP B200
    // is RX2 — and an antenna connected to TX/RX then delivers nothing but
    // noise. Measured on this hardware at 1090 MHz: TX/RX gave 37 dB
    // peak-to-noise and decoded aircraft, RX2 gave 21 dB and decoded almost
    // nothing. Samples flow either way, the spectrum looks alive either way,
    // and there was no control and no readout to reveal which port was in use.
    std::vector<std::string> listAntennas();

    // Selects an RX antenna by name. False if there is no device, the name is
    // not one the driver lists, or it refused.
    bool setAntenna(const std::string& name);

    // The port currently selected, as the DRIVER reports it (not a cached copy
    // of what was requested) — an empty string when nothing is open.
    std::string antenna();

    // --- IqSource --------------------------------------------------------

    // Activates the RX stream. False without an open device (that is the
    // documented before-open no-op: no crash, lastError() explains).
    // Idempotent while running.
    bool start() override;

    // Deactivates the stream; idempotent, safe before open. The stream and
    // device stay set up, so start() again is cheap (no retune glitch).
    void stop() override;

    bool running() const override { return running_; }

    // Hardware delivers on its own clock; the pipeline loop must not pace
    // this source (IqSource pacing contract).
    bool selfPaced() const override { return true; }

    // Actual device readback captured after open/setSampleRateHz — the
    // driver may coerce a requested rate, and the DSP chain must run at the
    // rate the hardware really uses, not the one that was asked for.
    // 0.0 until a device is open.
    double sampleRateHz() const override { return sampleRateHz_; }

    // Forwards to the device, then re-reads the ACTUAL rate into
    // sampleRateHz(). False (with lastError) if there is no device, the
    // rate is not positive, or the driver refused it.
    bool setSampleRateHz(double hz) override;

    // Same actual-readback scheme as the sample rate. 0.0 until open.
    double centerFrequencyHz() const override { return centerFrequencyHz_; }
    bool setCenterFrequencyHz(double hz) override;

    // readStream with a 100 ms timeout. Returns the sample count delivered;
    // 0 on timeout — the contract-compliant "retry" signal for a self-paced
    // source — and 0 immediately (no block) when no stream is open, so a
    // pipeline pointed at an unopened source spins safely instead of
    // crashing.
    //
    // Errors are CLASSIFIED rather than uniformly retried, because "retry
    // forever" is the wrong recovery for half of them:
    //   - timeout: not an error at all. Not even recorded — an idle device
    //     would otherwise park a permanent message in the GUI.
    //   - overflow / corruption: the host fell behind or one buffer was
    //     malformed. Recorded, retried, and treated as proof the device is
    //     alive (the fault counter resets).
    //   - any other error code: recorded and counted; kMaxConsecutiveErrors
    //     of them in a row raises faulted().
    //   - a driver EXCEPTION: recorded and faulted() immediately. A driver
    //     that throws out of a stream read has lost the device — that is how
    //     a USB SDR being unplugged presents.
    //
    // A delivered block is also checked for NaN and infinities, which are
    // replaced with silence before it is handed on; that is recorded like an
    // overflow (message, no fault) because the device answered and only the
    // samples were unusable. See sanitizeNonFinite in the .cpp for what the
    // check costs and why it is done per block rather than per sample.
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    // Set by read() when the stream is beyond retrying, per the classification
    // above; lastError() carries the reason. Cleared by a successful open() or
    // start(), and by teardown — a fault describes a live stream, and after a
    // reopen there is a new one. Safe from any thread.
    //
    // The pipeline's source loop polls this and stops with the message, which
    // is what turns a pulled radio into "Device stopped: ..." instead of a
    // frozen spectrum that looks like a hang.
    bool faulted() const override;

    // "SoapySDR: <label>" once open, "SoapySDR: (no device)" otherwise.
    const char* name() const override { return name_.c_str(); }

    // The pointer is backed by a per-THREAD snapshot taken under errorMutex_,
    // so it stays valid until the calling thread asks again — long enough for
    // every caller here, all of which copy it straight into a std::string.
    // It cannot point at lastError_ itself: that would hand out a buffer the
    // source thread rewrites.
    const char* lastError() const override;

private:
    // Releases whatever half-built state exists, swallowing every Soapy
    // exception: teardown runs inside catch blocks and destructors, where a
    // second throw would terminate the process.
    void teardown() noexcept;

    // The only writers of the error slot. Every failure path goes through
    // setError so no site can forget the lock; clearError also resets the
    // fault state, which is why open()/start() call it on success.
    void setError(std::string msg);
    void clearError();

    SoapySDR::Device* dev_ = nullptr;
    SoapySDR::Stream* stream_ = nullptr;
    bool running_ = false;
    double sampleRateHz_ = 0.0;
    double centerFrequencyHz_ = 0.0;
    std::string name_ = "SoapySDR: (no device)";

    // See the error-slot note in the file header: everything below is written
    // from the source thread and read from the control thread.
    mutable std::mutex errorMutex_;
    std::string lastError_;
    bool faulted_ = false;
    int consecutiveErrors_ = 0;
};

}  // namespace cascade::source
