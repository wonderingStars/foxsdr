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
// Threading: ONE THREAD IN THE VENDOR STACK, EVER. Every call that enters the
// driver — open/closeDevice, start/stop, read(), every setter and query —
// takes devMutex_ for the duration of the driver call, so a GUI-thread retune
// can never run concurrently with the source thread parked inside
// readStream(), and teardown can never overlap either. This replaced the old
// contract ("setters are control-thread calls; read() runs on the source
// thread; members plain, not atomic, on purpose"), which sent two threads
// into the vendor module at once by design: SoapySDR makes no cross-thread
// guarantee for a device (its own setupStream doc: "The returned stream is
// not required to have internal locking, and may not be used concurrently
// from multiple threads"), UHD merely happened to survive it, and the three
// 0.62.0 field crashes were adjudicated to a libusb-state use-after-free
// consumed by exactly these unserialised control calls.
//
// What that costs: a control call can wait behind one bounded readStream(),
// so the read timeout is 20 ms (kReadTimeoutUs) — a setter waits at most one
// read quantum. stop() no longer interrupts a parked read cross-thread (the
// SoapySDR stream contract forbids that concurrent use anyway); it waits the
// same bounded quantum, then deactivates with the stream unowned.
//
// Lock-free mirrors: running_/sampleRateHz_/centerFrequencyHz_ are atomics
// and isOpen() reads an atomic mirror, so the per-frame GUI readouts never
// block behind a read in flight. Lock order is devMutex_ -> errorMutex_,
// never the reverse; no lock is ever taken inside a structured-exception
// guarded body (a fault would abandon it).
//
// The ERROR SLOT keeps its own lock: read() records failures from the source
// thread at the same time as the GUI reads lastError() and the source loop
// polls faulted() — a std::string written on one thread and read on another
// is UB, not a stale value. Those members (lastError_, faulted_, deviceDead_,
// consecutiveErrors_, and now name_) live under errorMutex_ and are reached
// only through setError/clearError/lastError/faulted/deviceDead/name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
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
// device's own rate, so read() blocks up to ~20 ms and returns 0 as the
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

    // Atomic mirror of "dev_ != nullptr", so per-frame GUI checks never block
    // behind a driver call holding devMutex_.
    bool isOpen() const { return openMirror_.load(std::memory_order_relaxed); }

    // True while ANY SoapySource in this process holds an open device. The
    // in-process enumeration fallback checks this and refuses to run: walking
    // vendor find() routines opens and closes the very dongle a stream may be
    // using, through the same in-process libusb whose corrupted lock state the
    // 0.62.0 field crashes fingerprinted. The child-process scan is unaffected
    // (a fresh process never has a device open).
    static bool anyDeviceOpen();

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

    bool running() const override { return running_.load(std::memory_order_relaxed); }

    // Hardware delivers on its own clock; the pipeline loop must not pace
    // this source (IqSource pacing contract).
    bool selfPaced() const override { return true; }

    // Actual device readback captured after open/setSampleRateHz — the
    // driver may coerce a requested rate, and the DSP chain must run at the
    // rate the hardware really uses, not the one that was asked for.
    // 0.0 until a device is open.
    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // Forwards to the device, then re-reads the ACTUAL rate into
    // sampleRateHz(). False (with lastError) if there is no device, the
    // rate is not positive, or the driver refused it.
    bool setSampleRateHz(double hz) override;

    // Same actual-readback scheme as the sample rate. 0.0 until open.
    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }
    bool setCenterFrequencyHz(double hz) override;

    // readStream with a 20 ms timeout (kReadTimeoutUs — short so a control
    // call waiting on devMutex_ behind a parked read never stalls the GUI
    // noticeably). Returns the sample count delivered; 0 on timeout — the
    // contract-compliant "retry" signal for a self-paced source — and 0
    // immediately (no block) when no stream is open, so a pipeline pointed at
    // an unopened source spins safely instead of crashing.
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
    // ALSO set by an absorbed VENDOR FAULT on any control call — start, stop,
    // a retune, a gain or antenna change, the open itself. Those used to kill
    // the process outright (three signatures uploaded from shipped 0.62.0);
    // they now come back here, which is what turns a driver access violation
    // into the same visible "Device stopped: ..." the user already gets when a
    // radio is unplugged, instead of the application vanishing.
    //
    // The pipeline's source loop polls this and stops with the message, which
    // is what turns a pulled radio into "Device stopped: ..." instead of a
    // frozen spectrum that looks like a hang.
    bool faulted() const override;

    // "SoapySDR: <label>" once open, "SoapySDR: (no device)" otherwise.
    // Snapshot-backed like lastError(): open() on a worker thread rewrites
    // name_ while the GUI draws it, so the pointer must not alias the member.
    const char* name() const override;

    // The pointer is backed by a per-THREAD snapshot taken under errorMutex_,
    // so it stays valid until the calling thread asks again — long enough for
    // every caller here, all of which copy it straight into a std::string.
    // It cannot point at lastError_ itself: that would hand out a buffer the
    // source thread rewrites.
    const char* lastError() const override;

    // True once a vendor call has raised a structured exception that the guard
    // absorbed (see the DEAD DEVICE POLICY note in the .cpp). The device is
    // then never called again: every setter refuses, and teardown releases the
    // handles WITHOUT calling back into the driver. Exposed so the GUI and the
    // tests can tell "this driver crashed" from "the driver said no".
    bool deviceDead() const;

private:
    // The *Locked helpers assume devMutex_ is HELD by the caller — they exist
    // so open()/closeDevice() can run stop-then-teardown under one lock
    // without recursing into it. Public entry points take the lock, privates
    // never do.
    void stopLocked();

    // Releases whatever half-built state exists, swallowing every Soapy
    // exception: teardown runs inside catch blocks and destructors, where a
    // second throw would terminate the process.
    //
    // Every call it makes into the driver is guarded, and it makes NONE at all
    // once deviceDead() - the handles are dropped and the driver is left
    // alone. Whatever happens, the object lands in the same closed state:
    // dev_ and stream_ null, not running, rate and frequency zero, name back
    // to "(no device)". lastError() survives; the fault latches do not.
    void teardownLocked() noexcept;

    // Drops the device and stream handles WITHOUT calling into the driver.
    // The abandonment half of the dead-device policy, used by teardown and by
    // an open() that faulted with a half-built device on its hands.
    void abandonDeviceLocked() noexcept;

    // Resets the mirrors and the open-device count on any path that lets go
    // of dev_ — the one place the bookkeeping lives so teardown and
    // abandonment cannot disagree about it.
    void clearDeviceStateLocked() noexcept;

    // Records an absorbed vendor fault: the message the GUI shows, the fault
    // latch the pipeline's source loop polls, and the dead-device latch.
    // noexcept because teardown and the destructor call it.
    void noteVendorFault(const char* what) noexcept;

    // The only writers of the error slot. Every failure path goes through
    // setError so no site can forget the lock; clearError also resets the
    // fault state, which is why open()/start() call it on success.
    //
    // clearError deliberately does NOT clear the dead-device latch: only
    // teardown does, because only teardown has actually let go of the handle.
    void setError(std::string msg);
    void clearError();

    // Serialises every entry into the vendor driver — see the file header.
    // Touched only by public entry points; held across the driver call.
    mutable std::mutex devMutex_;

    // dev_ and stream_ are read and written ONLY under devMutex_.
    SoapySDR::Device* dev_ = nullptr;
    SoapySDR::Stream* stream_ = nullptr;

    // Lock-free mirrors for the per-frame GUI readouts (see the file header).
    // Written under devMutex_ at the points the driver state actually changed.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};

    // Whether THIS source is counted in the process-wide open-device count —
    // set exactly at the successful-open commit, cleared exactly once on the
    // release paths, so a half-made device that gets abandoned mid-open can
    // never unbalance the count.
    bool counted_ = false;

    // See the error-slot note in the file header: everything below is written
    // from the source thread and read from the control thread. name_ moved in
    // here because open() now runs on a worker thread while the GUI draws it.
    mutable std::mutex errorMutex_;
    std::string name_ = "SoapySDR: (no device)";
    std::string lastError_;
    bool faulted_ = false;
    bool deviceDead_ = false;
    int consecutiveErrors_ = 0;
};

}  // namespace cascade::source
