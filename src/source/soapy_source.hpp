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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <string>
#include <vector>

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
    static std::vector<SoapyDeviceInfo> enumerate();

    // False when SoapySDR.dll cannot be loaded (missing or broken install).
    // The DLL is delay-loaded, so the app runs fine without it — only
    // hardware sources are unavailable. Every Soapy entry point checks this
    // first so a missing runtime yields a message, never a crash.
    static bool runtimeAvailable();

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
    // crashing. Stream faults (overflow, device loss) also yield 0 with the
    // fault recorded in lastError(): the caller's retry loop is the right
    // recovery for all of them.
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    // "SoapySDR: <label>" once open, "SoapySDR: (no device)" otherwise.
    const char* name() const override { return name_.c_str(); }

    const char* lastError() const override { return lastError_.c_str(); }

private:
    // Releases whatever half-built state exists, swallowing every Soapy
    // exception: teardown runs inside catch blocks and destructors, where a
    // second throw would terminate the process.
    void teardown() noexcept;

    SoapySDR::Device* dev_ = nullptr;
    SoapySDR::Stream* stream_ = nullptr;
    bool running_ = false;
    double sampleRateHz_ = 0.0;
    double centerFrequencyHz_ = 0.0;
    std::string name_ = "SoapySDR: (no device)";
    std::string lastError_;
};

}  // namespace cascade::source
