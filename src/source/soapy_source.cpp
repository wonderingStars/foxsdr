// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include "core/diag_log.hpp"
#include "source/soapy_enum_proc.hpp"
#include "source/soapy_modules.hpp"
#include "source/vendor_guard.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Modules.hpp>
#include <SoapySDR/Version.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#ifdef _WIN32
// For the LoadLibrary probe in runtimeAvailable(). Included AFTER the SoapySDR
// headers: windows.h defines macros (min/max are suppressed by NOMINMAX in the
// build flags, but others remain) that have historically collided with library
// headers, so it goes last where it can only affect our own code.
#include <windows.h>
#endif

namespace cascade::source {

namespace {

// cascade is a single-VFO receiver front end: it always streams RX channel 0.
// Multi-channel devices (B210 etc.) still work â€” they just expose one chain.
constexpr std::size_t kChannel = 0;

// read() blocking bound. Was 100 ms; 20 ms since devMutex_ serialised the
// vendor stack: a control call (a retune from the GUI, a stop) now waits
// behind at most one bounded readStream(), so this bound IS the worst-case
// control latency. 20 ms is still far above what a healthy device needs to
// have samples ready (the pipeline reads 10 ms chunks; measured B200 read
// p50 is ~4 ms per 8192 samples) and comfortably inside the IqSource
// self-paced contract's "<= ~100 ms".
constexpr long kReadTimeoutUs = 20000;

// How long a GUI-thread control call will wait for the driver lock before it
// gives up and abandons the device. Generously longer than the 20 ms read
// quantum it normally queues behind - a healthy driver never comes close - and
// short enough that a user who has just clicked Stop does not think the
// application has died. See SoapySource::stop().
constexpr std::chrono::milliseconds kControlLockWait{1500};

const char* const kNoDeviceName = "SoapySDR: (no device)";

// Process-wide count of open SoapySource devices, for the in-process
// enumeration gate (see anyDeviceOpen() in the header). Maintained solely by
// clearDeviceStateLocked()/open() via the per-instance counted_ flag.
std::atomic<int> s_openDevices{0};

// A stream error the device can plausibly come back from unaided. Overflow is
// the everyday one â€” the host fell behind, samples were dropped, the next read
// is fine again â€” and a corrupt packet likewise costs one buffer, not the
// radio. Every other negative code (SOAPY_SDR_STREAM_ERROR above all, which is
// what drivers return once the USB endpoint has gone) needs a reopen, so it
// counts toward the fault threshold instead.
bool transientStreamError(int ret) {
    return ret == SOAPY_SDR_OVERFLOW || ret == SOAPY_SDR_CORRUPTION;
}

// Hard errors are COUNTED, not latched on the first one: drivers do emit a
// lone spurious code around a retune, and faulting a live radio over that
// would be a worse bug than the one this fixes. Ten in a row costs at most a
// second at the 100 ms read bound before the pipeline is told.
constexpr int kMaxConsecutiveErrors = 10;

// Turns a block a driver just delivered into a block the DSP chain can be
// trusted with, returning whether anything had to be replaced.
//
// WHY HERE, IN THE HOT PATH. A NaN or an infinity from a driver â€” a
// half-initialised buffer, the CF32 converter fed a malformed packet, a device
// coming apart on the bus â€” is not recoverable further down: it latches the
// AGC gain, the squelch/S-meter EMA and the noise reducer's spectrum, and the
// receiver goes silent while the spectrum display stays alive. This is the
// hardware path's entry point, the counterpart of the sanitise
// source/iq_file_source does for a file.
//
// WHY IT IS AFFORDABLE, measured rather than assumed. A per-sample isfinite
// test over every sample was the design previously declined for this path, and
// it costs 26 us per 10 ms block at 2 Msps (0.26% of the real time that block
// represents; the pipeline reads kChunkSec = 10 ms at a time). What runs here
// instead is ONE float sum over the block: NaN and either infinity poison a
// sum they enter, and radio samples are bounded well inside float range â€” a
// 61.44 Msps block of 614400 samples of |x| <= 1 sums to at most ~1.2e6
// against a 3.4e38 ceiling â€” so a non-finite SUM means a non-finite SAMPLE,
// exactly, with no false positives to explain away. That pass costs 14 us per
// block, 0.14% of the block's real time, and because both the work and the
// budget scale with the sample count that fraction is the same at every rate.
// The per-sample scrub, three times the price of the detection, runs ONLY on
// a block that failed it.
bool sanitizeNonFinite(std::complex<float>* dst, std::size_t n) {
    // std::complex<float> is specified to be layout-compatible with an array
    // of two floats, so a block of them is 2n contiguous floats.
    float* p = reinterpret_cast<float*>(dst);
    const std::size_t floats = 2 * n;
    float sum = 0.0f;
    for (std::size_t i = 0; i < floats; ++i) {
        sum += p[i];
    }
    if (std::isfinite(sum)) {
        return false;
    }
    for (std::size_t i = 0; i < floats; ++i) {
        if (!std::isfinite(p[i])) {
            p[i] = 0.0f;  // silence, the only value that cannot mislead
        }
    }
    return true;
}

// what(): can legitimately be empty for some exception types; the GUI
// contract is "nonempty lastError on failure", so guarantee a floor here.
std::string describe(const std::exception& e, const char* context) {
    std::string msg = e.what();
    if (msg.empty()) {
        msg = "unknown error";
    }
    return std::string(context) + ": " + msg;
}

// EVERY CALL THAT CROSSES INTO A VENDOR MODULE GOES THROUGH HERE.
//
// callGuardingVendorFaults takes a plain noexcept function pointer on purpose
// (see source/vendor_guard.hpp: its own frame must hold nothing that needs C++
// unwinding, or MSVC rejects the __try with C2712). That makes it awkward to
// call directly from a method that has locals, so this trampoline packs a
// stateless lambda's address into the ctx pointer and hands over a captureless
// forwarder. The body still runs on the CALLING thread, inside the __try, which
// is the whole point - a structured exception is delivered on the thread that
// raised it.
//
// The body must not throw: the forwarder is noexcept, so an escaping C++
// exception is a terminate(). Every body below therefore keeps the try/catch
// that was already there INSIDE itself and reports through captured state.
// Message building (which allocates) is deliberately left OUTSIDE the guarded
// body wherever it can be, so a fault path does the least work possible.
template <class Body>
bool guardedVendorCall(const Body& body) noexcept {
    return callGuardingVendorFaults(
        [](void* p) noexcept { (*static_cast<const Body*>(p))(); },
        const_cast<void*>(static_cast<const void*>(&body)));
}

}  // namespace

SoapySource::~SoapySource() {
    closeDevice();
}

bool SoapySource::runtimeAvailable() {
#ifdef _WIN32
    // SoapySDR.dll is DELAY-LOADED (see the root CMakeLists): the process
    // starts even when it is absent, and the DLL is only pulled in on the
    // first call into it. That is what lets a broken or partial install show
    // a readable message instead of Windows' "The code execution cannot
    // proceed because SoapySDR.dll was not found" modal at launch, which the
    // user cannot act on and which kills the app before any of our code runs.
    //
    // Probing with LoadLibrary rather than calling a Soapy symbol matters: a
    // failed delay-load raises a structured exception that a C++ catch does
    // not handle, so we must never reach one. Every entry point below checks
    // here first. The handle is intentionally leaked - the module stays
    // loaded for the process lifetime anyway once anything uses it.
    static const bool available = []() {
        if (::LoadLibraryA("SoapySDR.dll") == nullptr) { return false; }
        // THE MODULE SEARCH PATH, fixed here because this is the first moment
        // it can be: the library is loaded (so its ABI version can be asked
        // for) and no module has been loaded yet (the search path is read once,
        // when the first enumeration triggers loading). See soapy_modules.hpp
        // for what was wrong and why the application found no hardware at all
        // when installed.
        //
        // Guarded like every other crossing. A fault here still answers TRUE:
        // SoapySDR.dll did load, so the runtime IS available - only the search
        // path may be incomplete, and that presents as "no devices found" with
        // a diagnostic, not as a missing runtime.
        const bool completed = guardedVendorCall([]() noexcept {
            try {
                ensureVendorModulesVisible(SoapySDR::getABIVersion());
            } catch (...) {
            }
        });
        if (!completed) {
            core::diagWarnf(
                "soapy: fixing the vendor module search path faulted (code "
                "0x%08X) - hardware may not be found",
                static_cast<unsigned>(vendorGuardLastFaultCode()));
        }
        return true;
    }();
    return available;
#else
    // POSIX links it normally, but the module search path still has to cover
    // wherever the distribution put the vendor modules.
    static const bool ready = []() {
        ensureVendorModulesVisible(SoapySDR::getABIVersion());
        return true;
    }();
    (void)ready;
    return true;
#endif
}

std::vector<std::string> SoapySource::moduleSearchPaths() {
    if (!runtimeAvailable()) { return {}; }
    std::vector<std::string> out;
    const bool completed = guardedVendorCall([&out]() noexcept {
        try {
            out = SoapySDR::listSearchPaths();
        } catch (...) {
            out.clear();
        }
    });
    if (!completed) {
        out.clear();
        core::diagWarnf("soapy: listSearchPaths faulted (code 0x%08X)",
                        static_cast<unsigned>(vendorGuardLastFaultCode()));
    }
    return out;
}

std::vector<std::string> SoapySource::loadedModules() {
    if (!runtimeAvailable()) { return {}; }
    std::vector<std::string> out;
    const bool completed = guardedVendorCall([&out]() noexcept {
        try {
            out = SoapySDR::listModules();
        } catch (...) {
            out.clear();
        }
    });
    if (!completed) {
        out.clear();
        core::diagWarnf("soapy: listModules faulted (code 0x%08X)",
                        static_cast<unsigned>(vendorGuardLastFaultCode()));
    }
    return out;
}

std::vector<VendorRoot> SoapySource::vendorInstalls() {
    if (!runtimeAvailable()) { return {}; }
    // getABIVersion() is the only part of this that crosses into SoapySDR.dll;
    // ensureVendorModulesVisible is our own code, and the guard's module-scoped
    // filter declines a fault raised there on purpose (vendor_guard.hpp rule 1)
    // - so wrapping the pair costs nothing and still covers the crossing.
    std::vector<VendorRoot> out;
    const bool completed = guardedVendorCall([&out]() noexcept {
        try {
            out = ensureVendorModulesVisible(SoapySDR::getABIVersion());
        } catch (...) {
            out.clear();
        }
    });
    if (!completed) {
        out.clear();
        core::diagWarnf("soapy: vendor install scan faulted (code 0x%08X)",
                        static_cast<unsigned>(vendorGuardLastFaultCode()));
    }
    return out;
}

std::vector<SoapyDeviceInfo> SoapySource::enumerate() {
    // THE WALK HAPPENS SOMEWHERE ELSE. See source/soapy_enum_proc.hpp: the
    // fault this path actually reproduces lands on a thread UHD spawned for
    // itself, and the only containment for that is a process boundary. The
    // outcome is deliberately discarded here - this signature promises a list
    // and nothing more - and every one of the four failure modes has already
    // written its own reason to the diagnostics log by the time this returns.
    return enumerateIsolated().devices;
}

bool SoapySource::anyDeviceOpen() {
    return s_openDevices.load(std::memory_order_relaxed) > 0;
}

std::vector<SoapyDeviceInfo> SoapySource::enumerateInProcess() {
    std::vector<SoapyDeviceInfo> out;
    if (!runtimeAvailable()) { return out; }  // no runtime: no devices, no crash

    // NEVER WHILE A RADIO IS OPEN (adjudicated fix #1 for the 0.62.0 field
    // crashes). The vendor walk opens and closes the very dongle a stream may
    // be using, through this process's libusb â€” the exact lifecycle overlap
    // fingerprinted as the corrupting event behind the freed-and-reused-lock
    // faults. The child-process scan never reaches this gate (a fresh process
    // has no device open); only the in-parent fallback does, and an empty
    // list with a logged reason is a far better answer there than a scan that
    // can corrupt the streaming device's driver state.
    if (anyDeviceOpen()) {
        core::diagWarnf(
            "soapy: in-process device scan refused while a radio is open - "
            "close the source and rescan");
        return out;
    }

    // THE ENTIRE VENDOR WALK RUNS INSIDE THE STRUCTURED-EXCEPTION GUARD.
    //
    // SoapySDR::Device::enumerate() loads every vendor module on the machine
    // and calls its find function, each of which scans the USB bus through
    // its own libusb. The catch (...) below is still here and still needed -
    // a module that THROWS is a real case - but under /EHsc it is blind to a
    // module that FAULTS.
    //
    // READ source/vendor_guard.hpp BEFORE TRUSTING THIS. The guard covers a
    // fault raised on THIS thread, inside this call. It does not cover the
    // libusb access violation this machine actually reproduces, which lands
    // on a discovery thread UHD spawned for itself - which is why this
    // function is the CHILD's job and not the application's.
    struct Walk {
        std::vector<SoapyDeviceInfo>* out;
    } walk{&out};

    const bool completed = callGuardingVendorFaults(
        [](void* p) noexcept {
            auto* w = static_cast<Walk*>(p);
            try {
                for (const SoapySDR::Kwargs& kw : SoapySDR::Device::enumerate()) {
                    SoapyDeviceInfo info;
                    // Modules put a display string under "label"; fall back to
                    // the driver key so the menu never shows a blank row.
                    const auto label = kw.find("label");
                    if (label != kw.end() && !label->second.empty()) {
                        info.label = label->second;
                    } else {
                        const auto driver = kw.find("driver");
                        info.label = (driver != kw.end() && !driver->second.empty())
                                         ? driver->second
                                         : "unknown device";
                    }
                    // The FULL kwargs, not just the driver key: serial/index
                    // keys are what pick the same physical unit out of several
                    // identical ones.
                    info.args = SoapySDR::KwargsToString(kw);
                    w->out->push_back(std::move(info));
                }
            } catch (...) {
                // "Never throws; empty on none/no modules": a probe failure in
                // some broken vendor module must not take the Source menu down
                // with it.
                w->out->clear();
            }
        },
        &walk);

    if (!completed) {
        // Partial results from a walk that faulted mid-list are not offered to
        // the user: a row built from half-written kwargs is a device that
        // cannot be opened. Empty plus a logged reason is the honest answer.
        out.clear();
        core::diagWarnf(
            "soapy: enumerate faulted inside a vendor module (code 0x%08X) - "
            "no devices listed; a driver install on this machine is faulty",
            static_cast<unsigned>(vendorGuardLastFaultCode()));
    }
    return out;
}

bool SoapySource::open(const std::string& args) {
    // One lock for the whole open, including the release of whatever was open
    // before â€” no other thread may be inside the driver while a device is
    // being made or unmade.
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    // Reopen semantics: whatever was open before must be fully released
    // first, or the old device handle (and its USB claim) would leak.
    stopLocked();
    teardownLocked();
    if (!runtimeAvailable()) {
        setError(
            "SoapySDR runtime not found (SoapySDR.dll). Reinstall, or use the "
            "signal generator / IQ file sources, which need no hardware.");
        return false;
    }
    // THE WHOLE DEVICE INTERROGATION IS ONE GUARDED CALL, not just
    // Device::make. Every line of it is a call into the vendor module - make,
    // getNumChannels, setupStream, the rate/frequency readback and the two key
    // strings - and every one of them reaches the same libusb surface that
    // faults. Guarding make alone would leave six unguarded crossings behind
    // it, which is the shape of defect this whole change exists to end.
    //
    // Outcome, not exception, crosses the boundary: the body records what
    // happened into `o` and the message is BUILT OUT HERE, where allocating is
    // safe and where a fault has already been ruled out.
    struct Open {
        SoapySource* self;
        const std::string* args;
        enum class Result { Ok, NullDevice, NoRxChannel, NullStream, Threw } result = Result::Ok;
        std::string threw;
        // Readbacks land here, not in the members: the mirrors are atomics
        // and name_ needs errorMutex_, and no lock may be taken inside the
        // structured-exception-guarded body (a fault would abandon it).
        // Committed after the guard returns, on the Ok path only.
        double rateHz = 0.0;
        double freqHz = 0.0;
        std::string label;
    } o{this, &args};

    const bool completed = guardedVendorCall([&o]() noexcept {
        SoapySource* s = o.self;
        try {
            s->dev_ = SoapySDR::Device::make(*o.args);
            if (s->dev_ == nullptr) {
                // make() normally throws on failure, but a null return is
                // legal API-wise and must not turn into a null deref at
                // setupStream.
                o.result = Open::Result::NullDevice;
                return;
            }
            if (s->dev_->getNumChannels(SOAPY_SDR_RX) < 1) {
                o.result = Open::Result::NoRxChannel;
                return;
            }
            // CF32 everywhere: the DSP chain is complex<float>, and every Soapy
            // module provides CF32 via the built-in converter registry even
            // when the wire format is CS16/CS8, so no per-driver format logic.
            s->stream_ = s->dev_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32,
                                              std::vector<std::size_t>{kChannel});
            if (s->stream_ == nullptr) {
                o.result = Open::Result::NullStream;
                return;
            }
            // Cache the device's ACTUAL state, not assumptions: a fresh device
            // boots at whatever rate/frequency its driver defaults to, and the
            // display must agree with the hardware from the first frame.
            o.rateHz = s->dev_->getSampleRate(SOAPY_SDR_RX, kChannel);
            o.freqHz = s->dev_->getFrequency(SOAPY_SDR_RX, kChannel);

            o.label = s->dev_->getHardwareKey();
            if (o.label.empty()) {
                o.label = s->dev_->getDriverKey();
            }
            if (o.label.empty()) {
                o.label = "device";
            }
            o.result = Open::Result::Ok;
        } catch (const std::exception& e) {
            o.result = Open::Result::Threw;
            o.threw = describe(e, "open failed");
        } catch (...) {
            o.result = Open::Result::Threw;
            o.threw = "open failed: non-standard exception";
        }
    });

    if (!completed) {
        // A FAULT MID-OPEN. dev_ may hold a half-made handle and stream_ a
        // half-made stream, both belonging to a module that just raised an
        // access violation. They are ABANDONED rather than unmade - see the
        // dead-device policy above teardown() - and open() fails, which the
        // Source panel already renders from lastError().
        noteVendorFault("opening the device");
        abandonDeviceLocked();
        return false;
    }
    switch (o.result) {
        case Open::Result::Ok:
            // Commit the readbacks the guarded body parked in `o` â€” here,
            // where taking errorMutex_ for name_ is safe.
            sampleRateHz_.store(o.rateHz, std::memory_order_relaxed);
            centerFrequencyHz_.store(o.freqHz, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(errorMutex_);
                name_ = "SoapySDR: " + o.label;
            }
            openMirror_.store(true, std::memory_order_relaxed);
            counted_ = true;
            s_openDevices.fetch_add(1, std::memory_order_relaxed);
            clearError();
            return true;
        case Open::Result::NullDevice:
            setError("SoapySDR::Device::make returned no device for args: " + args);
            teardownLocked();
            return false;
        case Open::Result::NoRxChannel:
            setError("device has no RX channels (args: " + args + ")");
            teardownLocked();
            return false;
        case Open::Result::NullStream:
            setError("setupStream(RX, CF32) returned no stream (args: " + args + ")");
            teardownLocked();
            return false;
        case Open::Result::Threw:
        default:
            setError(o.threw + " (args: " + args + ")");
            teardownLocked();
            return false;
    }
}

void SoapySource::closeDevice() {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    stopLocked();      // deactivate first; harmless no-op when not running
    teardownLocked();  // then release stream + device
    // lastError_ is deliberately NOT cleared: closeDevice() runs right after
    // failures, and the reason must still be readable afterwards.
}

bool SoapySource::deviceDead() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return deviceDead_;
}

void SoapySource::noteVendorFault(const char* what) noexcept {
    const unsigned code = static_cast<unsigned>(vendorGuardLastFaultCode());
    core::diagWarnf(
        "soapy: %s faulted inside the device driver (code 0x%08X) - absorbed, "
        "the device is now abandoned and will not be called again",
        what, code);
    std::lock_guard<std::mutex> lk(errorMutex_);
    // The LATCHES first, because they allocate nothing and they are what the
    // pipeline and every later call actually act on. A message we could not
    // build must not turn a survived fault back into a dead process.
    faulted_ = true;
    deviceDead_ = true;
    try {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "%s faulted inside the device driver (code 0x%08X). The "
                      "radio has been released without further driver calls; "
                      "reconnect it and pick the source again, or restart "
                      "FoxSDR.",
                      what, code);
        lastError_ = buf;
    } catch (...) {
    }
}

void SoapySource::clearDeviceStateLocked() noexcept {
    if (counted_) {
        s_openDevices.fetch_sub(1, std::memory_order_relaxed);
        counted_ = false;
    }
    dev_ = nullptr;
    stream_ = nullptr;
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        name_ = kNoDeviceName;
    }
}

void SoapySource::abandonDeviceLocked() noexcept {
    // No driver call of any kind. The fault latches and lastError_ are left
    // exactly as they are: they are the only record of why this happened.
    clearDeviceStateLocked();
}

// THE DEAD-DEVICE POLICY, which is the one genuinely contestable decision in
// this change, so here is the argument.
//
// Once a vendor call has raised an access violation, that module is in a state
// it no longer guarantees: it may hold its own locks and its own allocator may
// be torn (vendor_guard.hpp says exactly this about the enumeration trade).
// Calling BACK into it has three possible outcomes - it works, it faults again,
// or it BLOCKS FOREVER on a lock the faulting thread was holding. The guard
// covers the second. It cannot cover the third, and a hang on the GUI thread is
// strictly worse than a crash, because the user cannot even restart from it.
//
// So a device that has faulted once is never called again. teardown() drops the
// handle without closeStream and without unmake. That LEAKS the device object
// and its USB claim for the lifetime of the process, and the message says so:
// the radio is unusable until FoxSDR is restarted. That is the honest cost, and
// it buys the two things that matter more - the session survives (recordings,
// plugins, every other window), and switching to another source still works.
//
// The alternative, one more guarded unmake, was rejected on the hang: crash [3]
// of the three uploaded from 0.62.0 is a fault raised INSIDE teardown, so this
// is exactly the path where the module is provably mid-collapse.
void SoapySource::teardownLocked() noexcept {
    if (dev_ != nullptr) {
        if (deviceDead()) {
            core::diagWarnf(
                "soapy: releasing a device whose driver already faulted - the "
                "handle is abandoned deliberately, no closeStream/unmake");
        } else {
            if (stream_ != nullptr) {
                const bool completed = guardedVendorCall([this]() noexcept {
                    try {
                        dev_->closeStream(stream_);
                    } catch (...) {
                        // Teardown must complete regardless; the handle is dead
                        // either way and unmake below still releases the device.
                    }
                });
                if (!completed) { noteVendorFault("closing the device stream"); }
            }
            // Re-checked, not cached: closeStream may have just killed it.
            if (!deviceDead()) {
                const bool completed = guardedVendorCall([this]() noexcept {
                    try {
                        SoapySDR::Device::unmake(dev_);
                    } catch (...) {
                    }
                });
                if (!completed) { noteVendorFault("releasing the device"); }
            }
        }
    }
    clearDeviceStateLocked();
    // The FAULT state goes with the stream that raised it â€” there is nothing
    // left to be faulted about. lastError_ deliberately does NOT: teardown
    // runs immediately after the failures whose reason the GUI still shows
    // (see closeDevice), and clearing it here would erase every one of them â€”
    // including the vendor-fault message noteVendorFault just wrote.
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        faulted_ = false;
        deviceDead_ = false;
        consecutiveErrors_ = 0;
    }
}

void SoapySource::setError(std::string msg) {
    std::lock_guard<std::mutex> lk(errorMutex_);
    lastError_ = std::move(msg);
}

void SoapySource::clearError() {
    std::lock_guard<std::mutex> lk(errorMutex_);
    lastError_.clear();
    faulted_ = false;
    consecutiveErrors_ = 0;
}

bool SoapySource::faulted() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    // deviceDead_ is ORed in rather than relied on to have set faulted_,
    // because clearError() clears faulted_ and does not clear the dead latch.
    // A dead device that reported itself healthy for one poll would let the
    // pipeline keep reading a handle nobody is allowed to touch.
    return faulted_ || deviceDead_;
}

const char* SoapySource::lastError() const {
    // The returned pointer outlives the lock, so it cannot point into
    // lastError_ â€” the source thread rewrites that string. A thread_local
    // snapshot gives each calling thread its own backing buffer, which is what
    // makes this safe for the two callers that genuinely differ: the GUI, and
    // the pipeline's source loop reading the fault message.
    static thread_local std::string snapshot;
    std::lock_guard<std::mutex> lk(errorMutex_);
    snapshot = lastError_;
    return snapshot.c_str();
}

const char* SoapySource::name() const {
    // Same snapshot scheme as lastError(): open() rewrites name_ on whatever
    // thread it runs on (a worker, for the Source menu path), and the GUI
    // draws the previous name in the same instant. A pointer into name_
    // itself would dangle mid-frame.
    static thread_local std::string snapshot;
    std::lock_guard<std::mutex> lk(errorMutex_);
    snapshot = name_;
    return snapshot.c_str();
}

bool SoapySource::start() {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr || stream_ == nullptr) {
        // Documented before-open behavior: refuse with a reason, no crash.
        setError("start() called with no device open");
        return false;
    }
    if (running_.load(std::memory_order_relaxed)) {
        return true;  // idempotent per the IqSource contract
    }
    if (deviceDead()) {
        // lastError() already carries the fault's own message, which is far
        // more use than "the device is dead" written over the top of it.
        return false;
    }

    // CRASH [1] OF THE THREE UPLOADED FROM 0.62.0 lands here: the user pressed
    // Play and activateStream faulted through SoapySDR -> rtlsdrSupport ->
    // rtlsdr -> libusb. On our own call frame, so the guard sees it.
    struct Activate {
        SoapySource* self;
        int ret = 0;
        bool threw = false;
        std::string message;
    } a{this};

    const bool completed = guardedVendorCall([&a]() noexcept {
        try {
            a.ret = a.self->dev_->activateStream(a.self->stream_);
        } catch (const std::exception& e) {
            a.threw = true;
            a.message = describe(e, "activateStream failed");
        } catch (...) {
            a.threw = true;
            a.message = "activateStream failed: non-standard exception";
        }
    });
    if (!completed) {
        // FALSE, and faulted() is now true. Pipeline::start ignores this return
        // by design ("a failed start is not fatal to the pipeline") and spawns
        // the source thread anyway; that thread's first read() returns 0 on the
        // fault latch and its faulted() poll turns this into the pipeline fault
        // the Source panel already renders as "Device stopped: ...". So the
        // user sees the driver crash instead of the application disappearing,
        // through the path that already existed for an unplugged radio.
        noteVendorFault("starting the stream");
        return false;
    }
    if (a.threw) {
        setError(std::move(a.message));
        return false;
    }
    if (a.ret != 0) {
        // errToStr is a pure code->string lookup: no device handle, no
        // hardware, nothing a vendor module can reach. Deliberately outside
        // the guarded body, where allocating a message is safe.
        setError(std::string("activateStream failed: ") + SoapySDR::errToStr(a.ret));
        return false;
    }
    running_.store(true, std::memory_order_relaxed);
    // Clears the fault too: a stream that just activated is a new one, and
    // leaving the previous stream's fault latched would have the pipeline
    // stop the moment it started.
    clearError();
    return true;
}

void SoapySource::stop() {
    // Serialised like every driver entry: a stop no longer interrupts a
    // parked read cross-thread (the SoapySDR stream contract forbids that
    // concurrent use) â€” it waits out at most one kReadTimeoutUs read quantum,
    // then deactivates with the stream unowned.
    //
    // BOUNDED, BECAUSE THAT "AT MOST ONE READ QUANTUM" IS A PROMISE THE DRIVER
    // MAKES AND NOT ONE THIS CODE CAN KEEP. The bound holds only while
    // readStream honours the 20 ms timeout it is given. A vendor module that
    // simply never returns makes every waiter wait for ever, and this waiter
    // is the GUI thread: Pipeline::stop() calls it straight out of the
    // toolbar. Field report 4214EAE4 (0.64.0) is that hang, captured on a
    // Mirics device after the user switched sources twice â€” the interface
    // frozen on this exact lock_guard, which is worse than a crash because
    // the user cannot even restart from it.
    //
    // So the wait is bounded. Losing the race costs a device left activated
    // in a driver that is not answering â€” which is the same abandonment the
    // dead-device policy above already performs deliberately, for the same
    // reason â€” and the user is told, instead of the window going white.
    std::unique_lock<std::timed_mutex> devLk(devMutex_, kControlLockWait);
    if (!devLk.owns_lock()) {
        // running_ is atomic and is what the read loop actually tests, so
        // clearing it here still stops the source as soon as the driver
        // returns, whenever that is.
        running_.store(false, std::memory_order_relaxed);
        core::diagWarnf(
            "soapy: the device driver did not answer within %lld ms of a stop - "
            "abandoning it rather than freezing the interface; restart FoxSDR to "
            "use this radio again",
            static_cast<long long>(kControlLockWait.count()));
        {
            std::lock_guard<std::mutex> lk(errorMutex_);
            deviceDead_ = true;
            faulted_ = true;
            try {
                lastError_ =
                    "the radio's driver stopped responding and has been abandoned. "
                    "Restart FoxSDR to use it again.";
            } catch (...) {
            }
        }
        return;
    }
    stopLocked();
}

void SoapySource::stopLocked() {
    if (!running_.load(std::memory_order_relaxed)) {
        return;  // idempotent, and a safe no-op before open
    }
    // Mark stopped before touching the device: even if deactivation faults,
    // this object's state machine must land in "stopped". That ordering is
    // what makes a fault below safe â€” running_ is already correct when it
    // arrives, so nothing has to be repaired on the way out.
    running_.store(false, std::memory_order_relaxed);
    if (deviceDead()) {
        return;  // never call a driver that has already faulted
    }
    struct Deactivate {
        SoapySource* self;
        int ret = 0;
        bool threw = false;
        std::string message;
    } d{this};

    const bool completed = guardedVendorCall([&d]() noexcept {
        try {
            d.ret = d.self->dev_->deactivateStream(d.self->stream_);
        } catch (const std::exception& e) {
            d.threw = true;
            d.message = describe(e, "deactivateStream failed");
        } catch (...) {
            d.threw = true;
            d.message = "deactivateStream failed: non-standard exception";
        }
    });
    if (!completed) {
        // stop() is void, so the only reporting channel is lastError() â€” and
        // now also faulted(), which the pipeline polls. The teardown that
        // usually follows will see deviceDead() and let the handle go without
        // touching the driver again.
        noteVendorFault("stopping the stream");
        return;
    }
    if (d.threw) {
        setError(std::move(d.message));
        return;
    }
    if (d.ret != 0) {
        setError(std::string("deactivateStream failed: ") + SoapySDR::errToStr(d.ret));
    }
}

std::size_t SoapySource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) {
        return 0;
    }
    {
        // Already faulted: do not touch the driver again. Some drivers block
        // for the full timeout on every call against a handle whose device
        // has gone, which would make the pipeline's shutdown crawl. The source
        // loop polls faulted() and leaves; this is the belt to that braces.
        // Checked BEFORE devMutex_ so a faulted spin never contends the lock.
        std::lock_guard<std::mutex> lk(errorMutex_);
        if (faulted_) { return 0; }
    }
    // The lock is held across the bounded readStream â€” that is the whole
    // serialisation contract (see the header): while samples are being read,
    // no control call can enter the driver, and while a control call is in
    // the driver, this thread waits here instead of entering beside it.
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr || stream_ == nullptr) {
        // Before open (or after a teardown that won the lock first) there is
        // nothing to wait on: return the retry signal immediately.
        return 0;
    }
    // NOT ROUTED THROUGH THE VENDOR GUARD, and that is a decision rather than
    // an omission. vendor_guard.hpp states the limit of the trade in terms:
    // absorbing a fault is right for a QUERY made on the user's behalf, and it
    // "is NOT a licence to wrap the streaming path the same way - a fault in a
    // running stream means the device is gone, and the pipeline has a real
    // fault path for it". Three further reasons hold here specifically:
    // readStream runs ~10 times a second forever, so absorbing would mean
    // calling back into a torn module in a hot loop; every absorbed fault
    // writes a crash report (guard rule 2), so a faulting stream would produce
    // a report storm rather than one report; and none of the three signatures
    // uploaded from 0.62.0 is in this path - all three are control calls.
    void* buffs[1] = {dst};
    int flags = 0;
    long long timeNs = 0;
    try {
        const int ret = dev_->readStream(stream_, buffs, n, flags, timeNs, kReadTimeoutUs);
        if (ret > 0) {
            const std::size_t got = static_cast<std::size_t>(ret);
            const bool scrubbed = sanitizeNonFinite(dst, got);
            std::lock_guard<std::mutex> lk(errorMutex_);
            consecutiveErrors_ = 0;  // samples arrived: the device is alive
            if (scrubbed) {
                // Classified like an overflow: recorded so the GUI can show
                // it, and NOT counted toward the fault threshold. The device
                // answered â€” a block of it was unusable, which is a reason to
                // silence that block, not to stop the radio.
                lastError_ =
                    "device delivered non-finite samples; that block was "
                    "replaced with silence";
            }
            return got;
        }
        // A timeout is the bounded block expiring with nothing ready â€” the
        // contract's normal "retry" answer, not a failure. Deliberately NOT
        // recorded: an idle device would otherwise leave a permanent
        // "readStream failed" in the GUI. (A literal 0 is not a documented
        // return, but it means no samples either way, so it lands here.)
        if (ret == SOAPY_SDR_TIMEOUT || ret == 0) {
            return 0;
        }
        {
            std::lock_guard<std::mutex> lk(errorMutex_);
            lastError_ = std::string("readStream failed: ") + SoapySDR::errToStr(ret);
            if (transientStreamError(ret)) {
                consecutiveErrors_ = 0;
            } else if (++consecutiveErrors_ >= kMaxConsecutiveErrors) {
                faulted_ = true;
            }
        }
        return 0;
    } catch (const std::exception& e) {
        // A driver that THROWS out of a stream read has lost the device: this
        // is what a USB SDR being unplugged mid-capture does, and it is
        // exactly the case that used to be swallowed here. Reporting it as a
        // bare 0 made it indistinguishable from an idle radio, so the pipeline
        // retried forever and the user watched a frozen spectrum.
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = describe(e, "readStream failed");
        faulted_ = true;
        return 0;
    } catch (...) {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = "readStream failed: non-standard exception";
        faulted_ = true;
        return 0;
    }
}

bool SoapySource::setSampleRateHz(double hz) {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr) {
        setError("setSampleRateHz() called with no device open");
        return false;
    }
    if (!(hz > 0.0)) {  // negated compare so NaN also lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    if (deviceDead()) { return false; }

    struct Rate {
        SoapySource* self;
        double want;
        double got = 0.0;
        bool threw = false;
        std::string message;
    } r{this, hz};

    const bool completed = guardedVendorCall([&r]() noexcept {
        try {
            r.self->dev_->setSampleRate(SOAPY_SDR_RX, kChannel, r.want);
            // Readback, not echo: drivers coerce to the nearest supported rate
            // and the DSP chain must follow the hardware's real clock.
            r.got = r.self->dev_->getSampleRate(SOAPY_SDR_RX, kChannel);
        } catch (const std::exception& e) {
            r.threw = true;
            r.message = describe(e, "setSampleRate failed");
        } catch (...) {
            r.threw = true;
            r.message = "setSampleRate failed: non-standard exception";
        }
    });
    if (!completed) {
        // sampleRateHz_ is NOT updated. Same reasoning as the retune below: a
        // rate we never read back is a rate we do not know, and the DSP chain
        // running at a number the hardware may not be using is worse than it
        // running at the last one that was actually confirmed.
        noteVendorFault("setting the sample rate");
        return false;
    }
    if (r.threw) {
        setError(std::move(r.message));
        return false;
    }
    sampleRateHz_.store(r.got, std::memory_order_relaxed);
    return true;
}

bool SoapySource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr) {
        setError("setCenterFrequencyHz() called with no device open");
        return false;
    }
    if (deviceDead()) { return false; }

    // CRASH [2] OF THE THREE, and the most frequent of them: three reports from
    // one user, RTL-SDR Blog V4, who simply changed frequency.
    struct Tune {
        SoapySource* self;
        double want;
        double got = 0.0;
        bool threw = false;
        std::string message;
    } t{this, hz};

    const bool completed = guardedVendorCall([&t]() noexcept {
        try {
            t.self->dev_->setFrequency(SOAPY_SDR_RX, kChannel, t.want);
            // Same readback rationale as the sample rate: the synthesizer lands
            // where its step size allows, and the display tracks the hardware.
            t.got = t.self->dev_->getFrequency(SOAPY_SDR_RX, kChannel);
        } catch (const std::exception& e) {
            t.threw = true;
            t.message = describe(e, "setFrequency failed");
        } catch (...) {
            t.threw = true;
            t.message = "setFrequency failed: non-standard exception";
        }
    });
    if (!completed) {
        // centerFrequencyHz_ IS DELIBERATELY LEFT ALONE. The requested value is
        // not written in, and neither is a readback that never completed: after
        // a fault the synthesiser's real state is unknown, and the least wrong
        // number to show is the last one the hardware actually confirmed.
        // AppWindow::retuneSourceHz already reads back from the source rather
        // than trusting what it asked for (it feeds pluginRunner_.retune from
        // centerFrequencyHz()), so the whole UI keeps agreeing with itself, and
        // faulted() puts "Device stopped: ..." on screen next to it - which is
        // what stops the readout being quietly wrong.
        noteVendorFault("retuning the device");
        return false;
    }
    if (t.threw) {
        setError(std::move(t.message));
        return false;
    }
    centerFrequencyHz_.store(t.got, std::memory_order_relaxed);
    return true;
}

std::vector<std::string> SoapySource::listGainNames() {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr) {
        return {};  // no device, no gain stages â€” not an error
    }
    if (deviceDead()) { return {}; }
    std::vector<std::string> out;
    const bool completed = guardedVendorCall([this, &out]() noexcept {
        try {
            out = dev_->listGains(SOAPY_SDR_RX, kChannel);
        } catch (...) {
            out.clear();
        }
    });
    if (!completed) {
        out.clear();
        noteVendorFault("listing the gain stages");
    }
    return out;
}

bool SoapySource::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr) {
        setError("setGainDb() called with no device open");
        return false;
    }
    if (deviceDead()) { return false; }

    struct Gain {
        SoapySource* self;
        const std::string* name;
        double db;
        bool threw = false;
        std::string message;
    } g{this, &name, db};

    const bool completed = guardedVendorCall([&g]() noexcept {
        try {
            g.self->dev_->setGain(SOAPY_SDR_RX, kChannel, *g.name, g.db);
        } catch (const std::exception& e) {
            g.threw = true;
            g.message = describe(e, "setGain failed");
        } catch (...) {
            g.threw = true;
            g.message = "setGain failed: non-standard exception";
        }
    });
    if (!completed) {
        // False, and the Source panel's gain slider stops agreeing with a
        // device it can no longer reach â€” which is the truth.
        noteVendorFault("setting the gain");
        return false;
    }
    if (g.threw) {
        setError(std::move(g.message));
        return false;
    }
    return true;
}

bool SoapySource::setAutoGain(bool on) {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr) {
        setError("setAutoGain() called with no device open");
        return false;
    }
    if (deviceDead()) { return false; }

    struct Agc {
        SoapySource* self;
        bool on;
        bool hasMode = true;
        bool threw = false;
        std::string message;
    } a{this, on};

    const bool completed = guardedVendorCall([&a]() noexcept {
        try {
            if (!a.self->dev_->hasGainMode(SOAPY_SDR_RX, kChannel)) {
                a.hasMode = false;
                return;
            }
            a.self->dev_->setGainMode(SOAPY_SDR_RX, kChannel, a.on);
        } catch (const std::exception& e) {
            a.threw = true;
            a.message = describe(e, "setGainMode failed");
        } catch (...) {
            a.threw = true;
            a.message = "setGainMode failed: non-standard exception";
        }
    });
    if (!completed) {
        noteVendorFault("setting the gain mode");
        return false;
    }
    if (a.threw) {
        setError(std::move(a.message));
        return false;
    }
    if (!a.hasMode) {
        // Distinct from a driver fault: the hardware simply has no AGC, and
        // the GUI uses this to grey out the checkbox.
        setError("device has no automatic gain mode");
        return false;
    }
    return true;
}

std::vector<std::string> SoapySource::listAntennas() {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr) {
        return {};  // no device, no ports â€” not an error
    }
    if (deviceDead()) { return {}; }

    struct Ports {
        SoapySource* self;
        std::vector<std::string> out;
        bool threw = false;
        std::string message;
    } p{this};

    const bool completed = guardedVendorCall([&p]() noexcept {
        try {
            p.out = p.self->dev_->listAntennas(SOAPY_SDR_RX, kChannel);
        } catch (const std::exception& e) {
            p.threw = true;
            p.message = describe(e, "listAntennas failed");
        } catch (...) {
            p.threw = true;
            p.message = "listAntennas failed: non-standard exception";
        }
    });
    if (!completed) {
        noteVendorFault("listing the antenna ports");
        return {};
    }
    if (p.threw) {
        setError(std::move(p.message));
        return {};
    }
    return std::move(p.out);
}

bool SoapySource::setAntenna(const std::string& name) {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr) {
        setError("setAntenna() called with no device open");
        return false;
    }
    if (deviceDead()) { return false; }

    struct Port {
        SoapySource* self;
        const std::string* name;
        bool known = false;
        bool threw = false;
        std::string message;
    } p{this, &name};

    const bool completed = guardedVendorCall([&p]() noexcept {
        try {
            // Checked against the driver's own list first. Soapy drivers vary
            // in what they do with an unknown antenna name - some throw, some
            // ignore it silently - and a silently ignored port change is
            // precisely the failure this whole method exists to end.
            const std::vector<std::string> avail =
                p.self->dev_->listAntennas(SOAPY_SDR_RX, kChannel);
            if (std::find(avail.begin(), avail.end(), *p.name) == avail.end()) {
                return;  // known stays false
            }
            p.known = true;
            p.self->dev_->setAntenna(SOAPY_SDR_RX, kChannel, *p.name);
        } catch (const std::exception& e) {
            p.threw = true;
            p.message = describe(e, "setAntenna failed");
        } catch (...) {
            p.threw = true;
            p.message = "setAntenna failed: non-standard exception";
        }
    });
    if (!completed) {
        noteVendorFault("selecting the antenna port");
        return false;
    }
    if (p.threw) {
        setError(std::move(p.message));
        return false;
    }
    if (!p.known) {
        setError("device has no RX antenna called \"" + name + "\"");
        return false;
    }
    return true;
}

std::string SoapySource::antenna() {
    std::lock_guard<std::timed_mutex> devLk(devMutex_);
    if (dev_ == nullptr) {
        return {};
    }
    if (deviceDead()) { return {}; }

    struct Which {
        SoapySource* self;
        std::string out;
        bool threw = false;
        std::string message;
    } w{this};

    const bool completed = guardedVendorCall([&w]() noexcept {
        try {
            w.out = w.self->dev_->getAntenna(SOAPY_SDR_RX, kChannel);
        } catch (const std::exception& e) {
            w.threw = true;
            w.message = describe(e, "getAntenna failed");
        } catch (...) {
            w.threw = true;
            w.message = "getAntenna failed: non-standard exception";
        }
    });
    if (!completed) {
        noteVendorFault("reading the antenna port");
        return {};
    }
    if (w.threw) {
        setError(std::move(w.message));
        return {};
    }
    return std::move(w.out);
}

}  // namespace cascade::source
