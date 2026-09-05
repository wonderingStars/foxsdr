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
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <thread>
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
// Multi-channel devices (B210 etc.) still work — they just expose one chain.
constexpr std::size_t kChannel = 0;

// read() blocking bound. Was 100 ms; 20 ms since the device lock serialised the
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

// How long an escape path waits for ONE vendor call to come back before it
// gives up on the call itself - see runAbandonableVendorCall below, and the
// BOUNDING THE LOCK IS ONLY HALF OF IT note in the header.
//
// Same 1.5 s as the lock wait, and for the same reason: a healthy
// deactivateStream/closeStream/unmake is milliseconds (SoapyRTLSDR's cancels
// an async transfer and joins one thread), so this is orders of magnitude of
// headroom, while a user who has just clicked Stop must not be left wondering
// whether the application has died. The two bounds compose: an escape path
// that loses BOTH races - the lock, then the call - is back in the user's
// hands in at most 3 s, because the first abandonment condemns the device and
// every later call on the same path is skipped rather than attempted.
constexpr std::chrono::milliseconds kVendorCallWait{1500};

const char* const kNoDeviceName = "SoapySDR: (no device)";

// The one wording for a driver that stopped answering, wherever we give up on
// it: a lost race for the lock (stop/closeDevice) and a call that never
// returned (runAbandonableVendorCall) are the same event to the person looking
// at the screen, so they must not read as two different faults.
const char* const kAbandonedMessage =
    "the radio's driver stopped responding and has been abandoned. Restart "
    "FoxSDR to use it again.";

// Escape-path vendor calls abandoned since process start. See
// SoapySource::driverCallsAbandoned().
std::atomic<unsigned long long> s_abandonedCalls{0};

// Process-wide count of devices this process still holds open, for the
// in-process enumeration gate (see anyDeviceOpen() in the header). Maintained
// solely by clearDeviceStateLocked()/open() via the per-instance counted_
// flag - and it counts RADIOS, not sources: one the dead-device policy
// abandoned without unmaking is still open, and stays counted for the life of
// the process. clearDeviceStateLocked says why.
std::atomic<int> s_openDevices{0};

// A stream error the device can plausibly come back from unaided. Overflow is
// the everyday one — the host fell behind, samples were dropped, the next read
// is fine again — and a corrupt packet likewise costs one buffer, not the
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
// WHY HERE, IN THE HOT PATH. A NaN or an infinity from a driver — a
// half-initialised buffer, the CF32 converter fed a malformed packet, a device
// coming apart on the bus — is not recoverable further down: it latches the
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
// sum they enter, and radio samples are bounded well inside float range — a
// 61.44 Msps block of 614400 samples of |x| <= 1 sums to at most ~1.2e6
// against a 3.4e38 ceiling — so a non-finite SUM means a non-finite SAMPLE,
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

// --- AN ESCAPE-PATH VENDOR CALL THAT CAN BE ABANDONED ----------------------
//
// THE DEFECT THIS EXISTS FOR, from a 0.70.0 field report. stopLocked() called
// deactivateStream on the GUI thread. For an RTL-SDR that is
// SoapyRTLSDR::deactivateStream: rtlsdr_cancel_async(dev), then
// _rx_async_thread.join(). The join never returned, and because it was made on
// the calling thread there was nothing left to give up - the interface froze
// permanently and the user killed the process. Bounding the WAIT FOR THE LOCK
// (which stop() already did) cannot help when this call is the one holding it.
//
// So the call runs on a worker thread and the caller waits kVendorCallWait for
// it. If it comes back, everything proceeds exactly as before - same guard,
// same return code, same exception text, one thread hop. If it does not, the
// caller stops waiting, condemns the device and returns to the user; the call
// stays parked in the driver for as long as it likes.
//
// WHAT THE ABANDONED CALL IS ALLOWED TO TOUCH is the whole design constraint.
// It outlives the SoapySource - AppWindow makes a fresh source per open and
// drops the old one, and the report's user was switching sources - so it may
// not name `this`, or any member, or any local of the caller's frame. It
// therefore owns everything it touches: one heap object, held by shared_ptr,
// carrying the device link (SoapySource::DeviceLink: the mutex and the two
// handles) and its own result slots and completion latch. The caller keeps a
// reference until it gives up; the worker keeps one until the driver returns,
// which may be never. Whichever ends last frees it.
//
// The caller HOLDS the link's mutex throughout, so the one-thread-in-the-
// vendor-stack contract is unbroken while the call is in flight. Once the call
// is abandoned that mutex is released with the call still running, and what
// keeps the promise from there is the dead-device latch: a condemned device is
// never called again by anything (see abandonWedgedDriverLocked).
struct VendorJob {
    // Kept alive BY VALUE for the worker; the fields the body reads live here.
    std::shared_ptr<SoapySource::DeviceLink> link;

    // A captureless function, so nothing of the caller's frame can be reached
    // through it. Results go into the slots below, which the caller reads only
    // after the latch says the worker is finished with them.
    void (*body)(VendorJob&) noexcept = nullptr;

    int ret = 0;
    bool threw = false;
    std::string message;

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool faulted = false;  // the vendor guard absorbed a structured exception
};

enum class JobOutcome {
    Completed,  // the driver answered; ret/threw/message are readable
    Faulted,    // a structured exception was absorbed inside the driver
    Abandoned,  // it never came back - the caller must condemn the device
};

JobOutcome runAbandonableVendorCall(const std::shared_ptr<VendorJob>& job) noexcept {
    std::thread worker;
    try {
        worker = std::thread([job]() noexcept {
            const bool completed = guardedVendorCall([&job]() noexcept { job->body(*job); });
            {
                std::lock_guard<std::mutex> lk(job->m);
                job->faulted = !completed;
                job->done = true;
            }
            job->cv.notify_all();
            // Nothing else, deliberately: a worker that returns after being
            // abandoned may be doing so during process teardown, and reaching
            // for the logger (or anything else with static lifetime) from here
            // would trade a fixed freeze for a shutdown crash.
        });
    } catch (...) {
        // No thread, so NOTHING RAN. Reported as abandoned rather than run
        // inline: a machine that cannot start a thread is in no state to be
        // handed the call that froze the interface in the first place, and the
        // caller's verdict (condemn, release, tell the user) is survivable
        // where a freeze is not.
        return JobOutcome::Abandoned;
    }
    bool done = false;
    bool faulted = false;
    try {
        std::unique_lock<std::mutex> lk(job->m);
        done = job->cv.wait_for(lk, kVendorCallWait, [&job] { return job->done; });
        faulted = job->faulted;
    } catch (...) {
        done = false;  // an unwaitable latch is treated as a call that hung
    }
    if (!done) {
        worker.detach();
        s_abandonedCalls.fetch_add(1, std::memory_order_relaxed);
        return JobOutcome::Abandoned;
    }
    // Fast: the worker sets done as its last act, so this joins a thread that
    // is already on its way out. It is a real join, not a wait - the OS thread
    // is finished before the caller drops the lock the job's handles live
    // under.
    worker.join();
    return faulted ? JobOutcome::Faulted : JobOutcome::Completed;
}

// The caller-side half: build a job for one call on this link. Null on
// allocation failure, which the call sites treat as an abandoned call for the
// same reason a thread that will not start is treated as one.
std::shared_ptr<VendorJob> makeVendorJob(const std::shared_ptr<SoapySource::DeviceLink>& link,
                                         void (*body)(VendorJob&) noexcept) noexcept {
    try {
        auto job = std::make_shared<VendorJob>();
        job->link = link;
        job->body = body;
        return job;
    } catch (...) {
        return nullptr;
    }
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
    // be using, through this process's libusb — the exact lifecycle overlap
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
    // before - no other thread may be inside the driver while a device is
    // being made or unmade. Bounded like every other entry: a PREVIOUS
    // device's driver can legitimately hold this lock for seconds (a UHD
    // interrogation on the worker thread, or another thread's own slow
    // open()), so a timeout here means "busy", not "broken" - the failure is
    // reported and nothing about the previous device's state is touched,
    // because this call never got far enough to touch any of it.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError(
            "the previous device's driver is busy or not answering; try "
            "again");
        return false;
    }
    // A CONDEMNED LINK IS NOT REOPENABLE, and this gate is what makes the
    // abandonment message true rather than merely hopeful. Once a vendor call
    // has been left running (abandonWedgedDriverLocked), one of this process's
    // threads is still inside that module holding a device this code can never
    // unmake; making a second device through the same link would put a second
    // thread in there beside it, which is the 0.62.0 crash class. It costs
    // nothing in the application: AppWindow::openSoapy builds a FRESH
    // SoapySource for every open, so this only ever refuses an object that has
    // already been condemned - and the words are the ones the user was given
    // when it happened.
    //
    // Checked BEFORE the release below, because teardownLocked() is what
    // clears the fault latches and it must not be reached on this path.
    if (link_->abandoned) {
        setError(kAbandonedMessage);
        return false;
    }
    // Reopen semantics: whatever was open before must be fully released
    // first, or the old device handle (and its USB claim) would leak.
    stopLocked();
    teardownLocked();
    // ...AND THE RELEASE ITSELF CAN CONDEMN THE LINK, which is the reopen a
    // user actually performs: picking a second radio while the first one's
    // deactivateStream is wedged. The gate above cannot see that - the link
    // was healthy when this call started - and without this second check the
    // interrogation below would write a new device handle over the two a
    // wedged call is still reading, which is the race the freeze under it
    // would deserve. Same verdict, same words, no device made.
    if (link_->abandoned) {
        setError(kAbandonedMessage);
        return false;
    }
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
            s->link_->dev = SoapySDR::Device::make(*o.args);
            if (s->link_->dev == nullptr) {
                // make() normally throws on failure, but a null return is
                // legal API-wise and must not turn into a null deref at
                // setupStream.
                o.result = Open::Result::NullDevice;
                return;
            }
            if (s->link_->dev->getNumChannels(SOAPY_SDR_RX) < 1) {
                o.result = Open::Result::NoRxChannel;
                return;
            }
            // CF32 everywhere: the DSP chain is complex<float>, and every Soapy
            // module provides CF32 via the built-in converter registry even
            // when the wire format is CS16/CS8, so no per-driver format logic.
            s->link_->stream = s->link_->dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32,
                                              std::vector<std::size_t>{kChannel});
            if (s->link_->stream == nullptr) {
                o.result = Open::Result::NullStream;
                return;
            }
            // Cache the device's ACTUAL state, not assumptions: a fresh device
            // boots at whatever rate/frequency its driver defaults to, and the
            // display must agree with the hardware from the first frame.
            o.rateHz = s->link_->dev->getSampleRate(SOAPY_SDR_RX, kChannel);
            o.freqHz = s->link_->dev->getFrequency(SOAPY_SDR_RX, kChannel);

            o.label = s->link_->dev->getHardwareKey();
            if (o.label.empty()) {
                o.label = s->link_->dev->getDriverKey();
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
        // A FAULT MID-OPEN. The link may hold a half-made device and a
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
            // Commit the readbacks the guarded body parked in `o` — here,
            // where taking errorMutex_ for name_ is safe.
            sampleRateHz_.store(o.rateHz, std::memory_order_relaxed);
            centerFrequencyHz_.store(o.freqHz, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(errorMutex_);
                name_ = "SoapySDR: " + o.label;
            }
            // CONDEMNED MID-OPEN? stop()/closeDevice() time out against
            // the device lock - which this open has held throughout - and latch
            // the dead flags without it. That is the user pressing Stop
            // during a slow, HEALTHY interrogation. Returning true here
            // would hand back a device that faulted() already reports
            // dead, with clearError() (now latch-aware) leaving the state
            // contradictory forever. The driver just answered a full
            // interrogation, so it is provably not stalled: lift the
            // latch, release the device properly, and fail the open in
            // words that say what happened.
            if (deviceDead()) {
                {
                    std::lock_guard<std::mutex> lk(errorMutex_);
                    deviceDead_ = false;
                    faulted_ = false;
                }
                stopLocked();
                teardownLocked();
                setError(
                    "the device was closed while it was still opening; "
                    "pick the source again to reopen it");
                return false;
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
    // Same bounded wait, and the same escape-path verdict, as stop() below -
    // see the rationale there. On a timeout the link's handles are left
    // completely alone rather than cleared: they are writable ONLY under its
    // mutex, the
    // parked holder is inside the driver dereferencing them right now, and
    // clearing them here without the lock would be a data race on a live
    // handle. This deliberately LEAKS the device - the same documented
    // dead-device policy teardownLocked() applies below when a vendor call
    // itself faults - and the message says so, matching stop()'s wording so
    // the user sees one consistent abandonment reason regardless of which
    // control path gave up.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        running_.store(false, std::memory_order_relaxed);
        core::diagWarnf(
            "soapy: the device driver did not answer within %lld ms of a "
            "close - abandoning it rather than freezing the interface; "
            "restart FoxSDR to use this radio again",
            static_cast<long long>(kControlLockWait.count()));
        std::lock_guard<std::mutex> lk(errorMutex_);
        deviceDead_ = true;
        faulted_ = true;
        try {
            lastError_ = kAbandonedMessage;
        } catch (...) {
        }
        return;
    }
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

void SoapySource::clearDeviceStateLocked(bool deviceReleased) noexcept {
    // THE COUNT FOLLOWS THE RADIO, NOT THIS OBJECT. It answers one question -
    // does anything in this process still hold a device open (anyDeviceOpen(),
    // the gate on the in-process vendor walk) - and this used to answer it
    // with "did a SoapySource stop pointing at one", which are different
    // questions on every path the dead-device policy takes.
    //
    // A faulted or wedged device is dropped WITHOUT closeStream and WITHOUT
    // unmake, deliberately and permanently: the vendor module still owns that
    // radio, and in the wedged case one of our threads is still executing
    // inside it. Decrementing here told the rest of the process that nothing
    // was open at the exact moment that was least true - and what reads this
    // count then walks every vendor module's find(), opening and closing the
    // very dongle a stranded call is still inside, which is the lifecycle
    // overlap the 0.62.0 crashes were adjudicated to. So the decrement is
    // conditional on a release that actually happened, and a radio nobody
    // released stays counted until the process exits. That costs the
    // in-process enumeration FALLBACK for the rest of the session (the normal
    // scan runs in a child and is untouched, so the Source menu still
    // refreshes), and it is the same bargain the user is offered in words:
    // restart FoxSDR to use this radio again.
    if (counted_ && deviceReleased) {
        s_openDevices.fetch_sub(1, std::memory_order_relaxed);
        counted_ = false;
    }
    // THE HANDLES ARE NOT NULLED ON AN ABANDONED LINK, and that is the one
    // asymmetry in this function. A vendor call that never came back is still
    // running, on a thread that is still reading these two pointers out of the
    // link (see runAbandonableVendorCall); writing them from here would be a
    // data race on a live handle, and nulling them would not release anything
    // anyway - nothing may unmake a device a wedged call is still inside. The
    // link is frozen instead, and everything else below still says "closed",
    // which is what the rest of the object and the GUI act on.
    if (!link_->abandoned) {
        link_->dev = nullptr;
        link_->stream = nullptr;
    }
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
    //
    // Nothing was released, so nothing is uncounted: this runs from an open()
    // that faulted with a half-built device in its hands, and that device is
    // being left to the module exactly as teardown leaves a faulted one.
    clearDeviceStateLocked(/*deviceReleased=*/false);
}

unsigned long long SoapySource::driverCallsAbandoned() {
    return s_abandonedCalls.load(std::memory_order_relaxed);
}

int SoapySource::openDeviceCount() {
    return s_openDevices.load(std::memory_order_relaxed);
}

void SoapySource::abandonWedgedDriverLocked(const char* what) noexcept {
    // THE LINK IS CONDEMNED FIRST, before anything that can fail. From this
    // store on, the handles are frozen (clearDeviceStateLocked stops nulling
    // them), the dead latches survive teardown, and open() refuses - so the
    // "restart FoxSDR to use this radio again" below is a promise this object
    // keeps, not a hope. The call it names is still running: nothing here
    // waits for it, and nothing after it will ever touch the driver again.
    link_->abandoned = true;
    // WORD FOR WORD what a lost race for the driver lock says (see stop() and
    // closeDevice()). To the user these are one event - "the radio stopped
    // answering and FoxSDR let it go" - and two wordings for it would read as
    // two different faults with two different remedies.
    core::diagWarnf(
        "soapy: %s did not return within %lld ms - abandoning it rather than "
        "freezing the interface; restart FoxSDR to use this radio again",
        what, static_cast<long long>(kVendorCallWait.count()));
    std::lock_guard<std::mutex> lk(errorMutex_);
    // Latches before the message, as in noteVendorFault: a string we could not
    // allocate must not cost the latches that actually protect the device.
    deviceDead_ = true;
    faulted_ = true;
    try {
        lastError_ = kAbandonedMessage;
    } catch (...) {
    }
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
    // Whether the RADIO went back to the system, which only a completed
    // unmake achieves. Every other way out of this function - the dead-device
    // policy's silent drop, a fault, a call left running - leaves the module
    // holding the device, and the process-wide count has to say so
    // (clearDeviceStateLocked at the bottom, and anyDeviceOpen()).
    bool released = false;
    if (link_->dev != nullptr) {
        if (deviceDead()) {
            core::diagWarnf(
                "soapy: releasing a device whose driver already faulted - the "
                "handle is abandoned deliberately, no closeStream/unmake");
        } else {
            // BOTH OF THESE ARE ESCAPE-PATH CALLS, so both are bounded the way
            // stopLocked()'s deactivateStream is: teardown runs from
            // closeDevice() and from the destructor, on whatever thread is
            // closing the radio - the GUI thread, out of the Source panel -
            // and a closeStream or an unmake that never returns freezes it
            // exactly as thoroughly as the deactivateStream that produced the
            // 0.70.0 report. See runAbandonableVendorCall.
            if (link_->stream != nullptr) {
                const auto job = makeVendorJob(link_, [](VendorJob& j) noexcept {
                    try {
                        j.link->dev->closeStream(j.link->stream);
                    } catch (...) {
                        // Teardown must complete regardless; the handle is dead
                        // either way and unmake below still releases the device.
                    }
                });
                switch (job ? runAbandonableVendorCall(job) : JobOutcome::Abandoned) {
                    case JobOutcome::Completed:
                        break;
                    case JobOutcome::Faulted:
                        noteVendorFault("closing the device stream");
                        break;
                    case JobOutcome::Abandoned:
                        abandonWedgedDriverLocked("closing the device stream");
                        break;
                }
            }
            // Re-checked, not cached: closeStream may have just killed it -
            // by faulting, or by never coming back at all, and an unmake of a
            // device another thread is still inside is the one call this file
            // must never make.
            if (!deviceDead()) {
                const auto job = makeVendorJob(link_, [](VendorJob& j) noexcept {
                    try {
                        SoapySDR::Device::unmake(j.link->dev);
                    } catch (...) {
                    }
                });
                switch (job ? runAbandonableVendorCall(job) : JobOutcome::Abandoned) {
                    case JobOutcome::Completed:
                        // THE ONE PATH THAT GIVES THE RADIO BACK. A fault
                        // inside unmake leaves the module's ownership in a
                        // state it no longer guarantees, and an unmake still
                        // running is not an unmake that finished, so neither
                        // of those may claim it.
                        released = true;
                        break;
                    case JobOutcome::Faulted:
                        noteVendorFault("releasing the device");
                        break;
                    case JobOutcome::Abandoned:
                        abandonWedgedDriverLocked("releasing the device");
                        break;
                }
            }
        }
    }
    clearDeviceStateLocked(released);
    // The FAULT state goes with the stream that raised it — there is nothing
    // left to be faulted about. lastError_ deliberately does NOT: teardown
    // runs immediately after the failures whose reason the GUI still shows
    // (see closeDevice), and clearing it here would erase every one of them —
    // including the vendor-fault message noteVendorFault just wrote.
    //
    // AN ABANDONED LINK IS THE EXCEPTION, and it is what makes the abandonment
    // permanent. A fault leaves a device we have finished with; a wedged call
    // leaves one a thread is still inside, holding handles this process can
    // never reclaim. Clearing the latches there would let the very next
    // start() call activateStream on it, so they stay set for the life of the
    // object - and closeDevice() remains idempotent, because every path it
    // takes from here is a no-op.
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        faulted_ = link_->abandoned;
        deviceDead_ = link_->abandoned;
        consecutiveErrors_ = 0;
    }
}

void SoapySource::setError(std::string msg) {
    std::lock_guard<std::mutex> lk(errorMutex_);
    lastError_ = std::move(msg);
}

void SoapySource::clearError() {
    std::lock_guard<std::mutex> lk(errorMutex_);
    // A CONDEMNED DEVICE STAYS CONDEMNED. The escape paths (stop() /
    // closeDevice() on a driver-lock timeout) latch deviceDead_ WITHOUT
    // holding the device lock, so they can fire while a healthy open() or
    // start() is mid-flight - and that call's success path lands here.
    // Clearing faulted_ then made faulted() read false for exactly the
    // window until something re-checked the dead latch, and erasing
    // lastError_ deleted the one message that tells the user why their
    // radio is gone. Only teardownLocked(), under the device lock, may
    // lift the latch.
    if (deviceDead_) { return; }
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
    // lastError_ — the source thread rewrites that string. A thread_local
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
    // Bounded like every setter/query below: a timeout here can mean nothing
    // worse than contention against a HEALTHY slow open() running on another
    // thread (a UHD interrogation takes seconds), and killing the device for
    // that would be a worse bug than the hang this whole change exists to
    // fix. So this returns the failure value and explains why, but does NOT
    // mark the device dead - only the user's own escape paths (stop(),
    // closeDevice()) may condemn it. Every setter/query in this file below
    // shares this exact reasoning; later ones do not repeat it.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return false;
    }
    if (link_->dev == nullptr || link_->stream == nullptr) {
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
            a.ret = a.self->link_->dev->activateStream(a.self->link_->stream);
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
    // CONDEMNED MID-START? The same race as open()'s commit: an escape
    // path timed out against the lock this call holds and latched the
    // dead flags. The verdict stands here - unlike open(), the user's
    // Stop raced a stream COMING UP, and honouring the stop is the only
    // reading that cannot leave a stream running that faulted() reports
    // dead. Deactivate what was just activated (guarded, driver is
    // answering) and refuse; the abandonment message survives because
    // clearError() is latch-aware.
    if (deviceDead()) {
        // Abandonable like every other deactivateStream in this file, and for
        // the same reason: this one runs on the thread that called start(),
        // which is the GUI thread out of the toolbar, and it is the exact call
        // that froze 0.70.0 (see stopLocked). A stop that had to condemn the
        // device must not be the thing that hangs honouring itself.
        const auto job = makeVendorJob(link_, [](VendorJob& j) noexcept {
            try {
                j.link->dev->deactivateStream(j.link->stream);
            } catch (...) {
            }
        });
        switch (job ? runAbandonableVendorCall(job) : JobOutcome::Abandoned) {
            case JobOutcome::Completed:
                break;
            case JobOutcome::Faulted:
                noteVendorFault("deactivating a condemned stream");
                break;
            case JobOutcome::Abandoned:
                abandonWedgedDriverLocked("deactivating a condemned stream");
                break;
        }
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
    // concurrent use) — it waits out at most one kReadTimeoutUs read quantum,
    // then deactivates with the stream unowned.
    //
    // BOUNDED, BECAUSE THAT "AT MOST ONE READ QUANTUM" IS A PROMISE THE DRIVER
    // MAKES AND NOT ONE THIS CODE CAN KEEP. The bound holds only while
    // readStream honours the 20 ms timeout it is given. A vendor module that
    // simply never returns makes every waiter wait for ever, and this waiter
    // is the GUI thread: Pipeline::stop() calls it straight out of the
    // toolbar. Field report 4214EAE4 (0.64.0) is that hang, captured on a
    // Mirics device after the user switched sources twice — the interface
    // frozen on this exact lock_guard, which is worse than a crash because
    // the user cannot even restart from it.
    //
    // So the wait is bounded. Losing the race costs a device left activated
    // in a driver that is not answering — which is the same abandonment the
    // dead-device policy above already performs deliberately, for the same
    // reason — and the user is told, instead of the window going white.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
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
                lastError_ = kAbandonedMessage;
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
    // what makes a fault below safe — running_ is already correct when it
    // arrives, so nothing has to be repaired on the way out.
    running_.store(false, std::memory_order_relaxed);
    if (deviceDead()) {
        return;  // never call a driver that has already faulted
    }
    // THE 0.70.0 FIELD FREEZE IS THIS CALL. The stack was symbolised to the
    // one _Thrd_join call site in rtlsdrSupport.dll, inside a function that
    // calls rtlsdr_cancel_async, _Thrd_id and _Thrd_join in that order -
    // SoapyRTLSDR::deactivateStream cancelling its async transfer and then
    // joining its own RX thread. That join never returned. Called inline on
    // the GUI thread, as this was, there is nothing left to give up: the
    // interface froze for good and the user killed the process.
    //
    // So it runs where it can be abandoned, and this thread waits
    // kVendorCallWait for it. running_ is already false above, so a call that
    // has to be left behind still leaves the object in "stopped" - the only
    // thing it costs is the device, which is condemned rather than freed.
    const auto job = makeVendorJob(link_, [](VendorJob& j) noexcept {
        try {
            j.ret = j.link->dev->deactivateStream(j.link->stream);
        } catch (const std::exception& e) {
            j.threw = true;
            j.message = describe(e, "deactivateStream failed");
        } catch (...) {
            j.threw = true;
            j.message = "deactivateStream failed: non-standard exception";
        }
    });
    switch (job ? runAbandonableVendorCall(job) : JobOutcome::Abandoned) {
        case JobOutcome::Completed:
            break;
        case JobOutcome::Faulted:
            // stop() is void, so the only reporting channel is lastError() —
            // and now also faulted(), which the pipeline polls. The teardown
            // that usually follows will see deviceDead() and let the handle go
            // without touching the driver again.
            noteVendorFault("stopping the stream");
            return;
        case JobOutcome::Abandoned:
            // Same destination by a different road: the teardown that follows
            // sees deviceDead() and releases the handle without a driver call,
            // which is the only safe thing to do with a device that still has
            // one of our threads inside it.
            abandonWedgedDriverLocked("stopping the stream");
            return;
    }
    if (job->threw) {
        setError(std::move(job->message));
        return;
    }
    if (job->ret != 0) {
        setError(std::string("deactivateStream failed: ") + SoapySDR::errToStr(job->ret));
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
        // Checked BEFORE the device lock so a faulted spin never contends it.
        std::lock_guard<std::mutex> lk(errorMutex_);
        // BOTH latches, not just faulted_. clearError() (a successful
        // start()/open() commit) clears faulted_ and deliberately cannot
        // clear deviceDead_ - so after a stop()-timeout condemned the
        // device while a healthy activate was mid-flight, faulted_ alone
        // reads false here and this gate was the one place that let the
        // "never called again" promise be broken.
        if (faulted_ || deviceDead_) { return 0; }
    }
    // The lock is held across the bounded readStream — that is the whole
    // serialisation contract (see the header): while samples are being read,
    // no control call can enter the driver, and while a control call is in
    // the driver, this thread waits here instead of entering beside it.
    // Bounded like every other entry point, but with the read contract's OWN
    // classification: contention this long means a control call (a retune, a
    // stop) is the one inside the driver right now, which is not this
    // device's fault and not an error at all - the timed-out read just
    // becomes a slightly later "nothing yet, retry" than usual. Nothing is
    // recorded and the device is not marked dead; that verdict belongs only
    // to the user's own escape paths (stop()/closeDevice()).
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        return 0;
    }
    if (link_->dev == nullptr || link_->stream == nullptr) {
        // Before open (or after a teardown that won the lock first) there is
        // nothing to wait on: return the retry signal immediately.
        return 0;
    }
    // CONDEMNED WHILE THIS CALL WAS QUEUED FOR THE LOCK, which the latch test
    // above cannot see: it ran before the wait, and the thread that held the
    // lock may have spent that wait abandoning a wedged driver. Every setter
    // and query in this file re-tests deviceDead() after taking the lock; this
    // was the one entry point that did not, and it relied on the null test
    // above to stand in for it - which an ABANDONED link defeats by design.
    // Its handles are frozen deliberately (a stranded worker is still reading
    // them), so they stay non-null forever, and the read would go straight
    // into a module this process has promised never to call again, on a stream
    // whose deactivate is still parked inside it. 0 is the right answer here:
    // the same "nothing, retry" the source loop already handles, and faulted()
    // - set by every path that condemns a device - is what tells it to stop.
    if (deviceDead()) {
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
        const int ret = link_->dev->readStream(link_->stream, buffs, n, flags, timeNs,
                                               kReadTimeoutUs);
        if (ret > 0) {
            const std::size_t got = static_cast<std::size_t>(ret);
            const bool scrubbed = sanitizeNonFinite(dst, got);
            std::lock_guard<std::mutex> lk(errorMutex_);
            consecutiveErrors_ = 0;  // samples arrived: the device is alive
            if (scrubbed) {
                // Classified like an overflow: recorded so the GUI can show
                // it, and NOT counted toward the fault threshold. The device
                // answered — a block of it was unusable, which is a reason to
                // silence that block, not to stop the radio.
                lastError_ =
                    "device delivered non-finite samples; that block was "
                    "replaced with silence";
            }
            return got;
        }
        // A timeout is the bounded block expiring with nothing ready — the
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
    // Bounded, soft-failure - see start() above.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return false;
    }
    if (link_->dev == nullptr) {
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
            r.self->link_->dev->setSampleRate(SOAPY_SDR_RX, kChannel, r.want);
            // Readback, not echo: drivers coerce to the nearest supported rate
            // and the DSP chain must follow the hardware's real clock.
            r.got = r.self->link_->dev->getSampleRate(SOAPY_SDR_RX, kChannel);
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
    // Bounded, soft-failure - see start() above.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return false;
    }
    if (link_->dev == nullptr) {
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
            t.self->link_->dev->setFrequency(SOAPY_SDR_RX, kChannel, t.want);
            // Same readback rationale as the sample rate: the synthesizer lands
            // where its step size allows, and the display tracks the hardware.
            t.got = t.self->link_->dev->getFrequency(SOAPY_SDR_RX, kChannel);
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
    // Bounded, soft-failure - see start() above.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return {};
    }
    if (link_->dev == nullptr) {
        return {};  // no device, no gain stages - not an error
    }
    if (deviceDead()) { return {}; }
    std::vector<std::string> out;
    const bool completed = guardedVendorCall([this, &out]() noexcept {
        try {
            out = link_->dev->listGains(SOAPY_SDR_RX, kChannel);
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
    // Bounded, soft-failure - see start() above.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return false;
    }
    if (link_->dev == nullptr) {
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
            g.self->link_->dev->setGain(SOAPY_SDR_RX, kChannel, *g.name, g.db);
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
        // device it can no longer reach — which is the truth.
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
    // Bounded, soft-failure - see start() above.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return false;
    }
    if (link_->dev == nullptr) {
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
            if (!a.self->link_->dev->hasGainMode(SOAPY_SDR_RX, kChannel)) {
                a.hasMode = false;
                return;
            }
            a.self->link_->dev->setGainMode(SOAPY_SDR_RX, kChannel, a.on);
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
    // Bounded, soft-failure - see start() above.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return {};
    }
    if (link_->dev == nullptr) {
        return {};  // no device, no ports - not an error
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
            p.out = p.self->link_->dev->listAntennas(SOAPY_SDR_RX, kChannel);
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
    // Bounded, soft-failure - see start() above.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return false;
    }
    if (link_->dev == nullptr) {
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
                p.self->link_->dev->listAntennas(SOAPY_SDR_RX, kChannel);
            if (std::find(avail.begin(), avail.end(), *p.name) == avail.end()) {
                return;  // known stays false
            }
            p.known = true;
            p.self->link_->dev->setAntenna(SOAPY_SDR_RX, kChannel, *p.name);
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
    // Bounded, soft-failure - see start() above.
    std::unique_lock<std::timed_mutex> devLk(link_->mutex, kControlLockWait);
    if (!devLk.owns_lock()) {
        setError("the radio's driver is busy or not answering; try again");
        return {};
    }
    if (link_->dev == nullptr) {
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
            w.out = w.self->link_->dev->getAntenna(SOAPY_SDR_RX, kChannel);
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
