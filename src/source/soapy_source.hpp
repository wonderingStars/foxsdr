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
// so the read timeout is 20 ms (kReadTimeoutUs) — under a healthy driver a
// setter waits at most one read quantum. stop() no longer interrupts a
// parked read cross-thread (the SoapySDR stream contract forbids that
// concurrent use anyway); it waits the same bounded quantum, then
// deactivates with the stream unowned.
//
// EVERY devMutex_ ACQUISITION IS ITSELF BOUNDED (kControlLockWait, 1.5 s),
// not only stop()'s. kReadTimeoutUs is the bound the DRIVER promises, not one
// this code can enforce — field report 4214EAE4 proved a vendor readStream
// can simply ignore it and freeze every waiter forever, and a hang on the GUI
// thread is worse than a crash, because the user cannot even restart from it.
//
// BOUNDING THE LOCK IS ONLY HALF OF IT, and the missing half was a second
// field freeze on 0.70.0. Winning the lock and then calling a driver that
// never returns freezes the interface exactly as thoroughly as losing it: the
// symbolised report resolves to the one _Thrd_join call site in
// rtlsdrSupport.dll, inside SoapyRTLSDR::deactivateStream, which does
// rtlsdr_cancel_async and then joins its own async RX thread — a join that
// never came back, on the GUI thread, out of stopLocked(). So the VENDOR CALL
// on each escape path is bounded too, not just the wait for the lock: the
// calls in stopLocked() (deactivateStream) and teardownLocked() (closeStream,
// unmake) run on a worker that can be abandoned, and the caller waits
// kVendorCallWait for it. On a timeout the device is condemned exactly as a
// lock timeout condemns it, with the same words, and the call is left running.
// See DeviceLink below for what makes leaving it running safe.
//
// The verdict on losing the LOCK race differs by what the call is FOR, not
// just by whether it timed out: read() treats it as an ordinary retry (the source
// thread is not the injured party), a setter/query reports a soft failure
// without touching the device's health (it can lose this race against a
// perfectly healthy slow open()), and only the user's own escape paths —
// stop() and closeDevice() — condemn the device as dead. See the .cpp for the
// per-site reasoning; open()'s own timeout is the odd one out, since there it
// is the PREVIOUS device's driver holding the lock and this call never
// touched anything of its own to clean up.
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
#include <chrono>
#include <memory>
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
    //
    // A DEVICE THIS PROCESS DELIBERATELY NEVER RELEASED KEEPS THIS TRUE FOR
    // THE LIFE OF THE PROCESS, and that is the point rather than a leak in the
    // bookkeeping. The dead-device policy (see the .cpp) drops a faulted or
    // wedged device's handle WITHOUT closeStream or unmake, so the module
    // still owns that radio - and in the wedged case one of our own threads is
    // still executing inside it. Those are precisely the conditions under
    // which a vendor walk must not run, so the count follows the RADIO and not
    // this object's state: it goes down only when a device was actually
    // unmade. The user is told the same thing in words ("restart FoxSDR to use
    // this radio again"); this is that promise expressed as state.
    static bool anyDeviceOpen();

    // The number behind anyDeviceOpen(), exposed for the same reason
    // driverCallsAbandoned() is: a boolean cannot tell "the radio this test
    // abandoned is still held" from "some other device happens to be open", so
    // a test asserting the count did not lie would pass against the bug it
    // names. This is a delta a test can pin.
    static int openDeviceCount();

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
    // an unopened source spins safely instead of crashing. Also 0, after
    // waiting up to kControlLockWait, if devMutex_ itself cannot be acquired
    // (a control call is the one inside the driver right now) — the same
    // retry signal, not an error, because the source thread is not who is
    // blocking whom here.
    //
    // And 0 once more if the device was condemned WHILE this call was waiting
    // for the lock. The latches are tested before the lock as well, but that
    // test is stale by the time the lock is won: the thread that held it may
    // have spent the wait abandoning a wedged driver, and an abandoned link
    // keeps its handles (nothing may null them), so the null-handle test that
    // used to catch a torn-down device catches nothing here. Every setter and
    // query re-tests deviceDead() after taking the lock; this does too.
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

    // Escape-path vendor calls this PROCESS has abandoned because they did not
    // return within kVendorCallWait — deactivateStream, closeStream, unmake.
    // 0 on every healthy path.
    //
    // Counted for the same reason Pipeline counts abandoned source threads: a
    // timing assertion alone cannot tell an abandonment from a driver that
    // happened to answer quickly, so a test that only measures elapsed time
    // still passes when the bound is deleted. This number is the difference.
    static unsigned long long driverCallsAbandoned();

    // THE DEVICE LINK: the mutex that serialises entry into the vendor driver,
    // and the two handles that entry uses, in one object owned by a
    // shared_ptr.
    //
    // It is a separate object because AN ABANDONED DRIVER CALL OUTLIVES THE
    // SOURCE THAT STARTED IT. When an escape-path call does not come back
    // inside kVendorCallWait, it is left running on its worker thread — still
    // inside the vendor module, still using these two handles — and the
    // SoapySource can be destroyed a millisecond later (AppWindow::openSoapy
    // makes a fresh one per open and drops the old one; the field freeze this
    // exists for is a user switching sources). A worker that captured `this`
    // would then be reading freed memory, which is a worse defect than the
    // freeze it was added to prevent.
    //
    // So the worker captures a COPY OF THE SHARED POINTER instead, by value,
    // and the link outlives whichever of the two ends first. Nothing ever
    // unmakes an abandoned device, so the handles it names leak for the life
    // of the process — deliberately, and documented, exactly like the zombie
    // source thread Pipeline::quiesceSourceThreadLocked leaks one level up.
    //
    // Public only because the worker that runs a call is a free function in
    // the .cpp; nothing outside this class has any business touching one.
    struct DeviceLink {
        // Serialises every entry into the vendor driver (see the file header).
        // A TIMED mutex, so every entry point can give up rather than freeze.
        std::timed_mutex mutex;

        // Read and written ONLY under `mutex`, and — once a call using them
        // has been abandoned — read WITHOUT it by the worker still inside the
        // driver. That is safe because `abandoned` is the point after which
        // this object's fields are frozen: nothing may write them again.
        SoapySDR::Device* dev = nullptr;
        SoapySDR::Stream* stream = nullptr;

        // Set once, under `mutex`, when a call on these handles was abandoned.
        // From then on the device is condemned permanently: the dead-device
        // latches survive teardown, the handles are never nulled (a worker may
        // still be reading them) and never touched, and open() refuses. That
        // is what makes "restart FoxSDR to use this radio again" a promise
        // this object actually keeps rather than a hopeful message.
        bool abandoned = false;
    };

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
    //
    // deviceReleased says whether the DRIVER let go of the radio (an unmake
    // that actually completed), which is a different question from whether
    // this object stopped pointing at it. Only the first decrements the
    // process-wide count: see anyDeviceOpen() for why a device the
    // dead-device policy abandoned has to keep being counted.
    void clearDeviceStateLocked(bool deviceReleased) noexcept;

    // Records an absorbed vendor fault: the message the GUI shows, the fault
    // latch the pipeline's source loop polls, and the dead-device latch.
    // noexcept because teardown and the destructor call it.
    void noteVendorFault(const char* what) noexcept;

    // Records a vendor call that never came back: marks the link abandoned
    // (permanently — see DeviceLink::abandoned), condemns the device with the
    // same words a driver-lock timeout uses, and logs. The call itself is
    // still running when this returns; that is the point.
    //
    // Caller holds the link's mutex, like every other *Locked helper.
    void abandonWedgedDriverLocked(const char* what) noexcept;

    // The only writers of the error slot. Every failure path goes through
    // setError so no site can forget the lock; clearError also resets the
    // fault state, which is why open()/start() call it on success.
    //
    // clearError deliberately does NOT clear the dead-device latch: only
    // teardown does, because only teardown has actually let go of the handle.
    void setError(std::string msg);
    void clearError();

    // The mutex that serialises every entry into the vendor driver, and the
    // handles that entry uses — see DeviceLink above for why they live behind
    // a shared_ptr instead of being members here, and the file header for the
    // serialisation contract itself. Every acquisition in the .cpp uses the
    // shared kControlLockWait bound; what happens on a lost race differs by
    // call site, and is explained there.
    //
    // NEVER REASSIGNED, hence const: an abandoned worker holds its own copy of
    // this pointer, so a source that swapped in a fresh link could be inside a
    // new device while a wedged call is still inside the old one. One link per
    // SoapySource, for the whole life of the object; a condemned one stays
    // condemned and AppWindow makes a new SoapySource for the next open.
    const std::shared_ptr<DeviceLink> link_ = std::make_shared<DeviceLink>();

    // Lock-free mirrors for the per-frame GUI readouts (see the file header).
    // Written under devMutex_ at the points the driver state actually changed.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};

    // Whether THIS source is counted in the process-wide open-device count —
    // set exactly at the successful-open commit, cleared exactly once on the
    // path that actually RELEASED the device, so a half-made device that gets
    // abandoned mid-open can never unbalance the count and one this process
    // never unmade goes on being counted (see anyDeviceOpen()).
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
