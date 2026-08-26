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

// read() blocking bound, per the IqSource self-paced contract ("<= ~100 ms"):
// long enough that a healthy device always has samples within it, short
// enough that stop requests never wait noticeably on a dead stream.
constexpr long kReadTimeoutUs = 100000;

const char* const kNoDeviceName = "SoapySDR: (no device)";

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
        ensureVendorModulesVisible(SoapySDR::getABIVersion());
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
    try {
        return SoapySDR::listSearchPaths();
    } catch (...) {
        return {};
    }
}

std::vector<std::string> SoapySource::loadedModules() {
    if (!runtimeAvailable()) { return {}; }
    try {
        return SoapySDR::listModules();
    } catch (...) {
        return {};
    }
}

std::vector<VendorRoot> SoapySource::vendorInstalls() {
    if (!runtimeAvailable()) { return {}; }
    return ensureVendorModulesVisible(SoapySDR::getABIVersion());
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

std::vector<SoapyDeviceInfo> SoapySource::enumerateInProcess() {
    std::vector<SoapyDeviceInfo> out;
    if (!runtimeAvailable()) { return out; }  // no runtime: no devices, no crash

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
    // Reopen semantics: whatever was open before must be fully released
    // first, or the old device handle (and its USB claim) would leak.
    closeDevice();
    if (!runtimeAvailable()) {
        setError(
            "SoapySDR runtime not found (SoapySDR.dll). Reinstall, or use the "
            "signal generator / IQ file sources, which need no hardware.");
        return false;
    }
    try {
        dev_ = SoapySDR::Device::make(args);
        if (dev_ == nullptr) {
            // make() normally throws on failure, but a null return is legal
            // API-wise and must not turn into a null deref at setupStream.
            setError("SoapySDR::Device::make returned no device for args: " + args);
            return false;
        }
        if (dev_->getNumChannels(SOAPY_SDR_RX) < 1) {
            setError("device has no RX channels (args: " + args + ")");
            teardown();
            return false;
        }
        // CF32 everywhere: the DSP chain is complex<float>, and every Soapy
        // module provides CF32 via the built-in converter registry even when
        // the wire format is CS16/CS8, so no per-driver format logic here.
        stream_ = dev_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32,
                                    std::vector<std::size_t>{kChannel});
        if (stream_ == nullptr) {
            setError("setupStream(RX, CF32) returned no stream (args: " + args + ")");
            teardown();
            return false;
        }
        // Cache the device's ACTUAL state, not assumptions: a fresh device
        // boots at whatever rate/frequency its driver defaults to, and the
        // display must agree with the hardware from the first frame.
        sampleRateHz_ = dev_->getSampleRate(SOAPY_SDR_RX, kChannel);
        centerFrequencyHz_ = dev_->getFrequency(SOAPY_SDR_RX, kChannel);

        std::string label = dev_->getHardwareKey();
        if (label.empty()) {
            label = dev_->getDriverKey();
        }
        if (label.empty()) {
            label = "device";
        }
        name_ = "SoapySDR: " + label;
        clearError();
        return true;
    } catch (const std::exception& e) {
        setError(describe(e, "open failed"));
        teardown();
        return false;
    } catch (...) {
        setError("open failed: non-standard exception (args: " + args + ")");
        teardown();
        return false;
    }
}

void SoapySource::closeDevice() {
    stop();      // deactivate first; harmless no-op when not running
    teardown();  // then release stream + device
    // lastError_ is deliberately NOT cleared: closeDevice() runs right after
    // failures, and the reason must still be readable afterwards.
}

void SoapySource::teardown() noexcept {
    if (dev_ != nullptr) {
        if (stream_ != nullptr) {
            try {
                dev_->closeStream(stream_);
            } catch (...) {
                // Teardown must complete regardless; the handle is dead
                // either way and unmake below still releases the device.
            }
        }
        try {
            SoapySDR::Device::unmake(dev_);
        } catch (...) {
        }
    }
    dev_ = nullptr;
    stream_ = nullptr;
    running_ = false;
    sampleRateHz_ = 0.0;
    centerFrequencyHz_ = 0.0;
    name_ = kNoDeviceName;
    // The FAULT state goes with the stream that raised it — there is nothing
    // left to be faulted about. lastError_ deliberately does NOT: teardown
    // runs immediately after the failures whose reason the GUI still shows
    // (see closeDevice), and clearing it here would erase every one of them.
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        faulted_ = false;
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
    return faulted_;
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

bool SoapySource::start() {
    if (dev_ == nullptr || stream_ == nullptr) {
        // Documented before-open behavior: refuse with a reason, no crash.
        setError("start() called with no device open");
        return false;
    }
    if (running_) {
        return true;  // idempotent per the IqSource contract
    }
    try {
        const int ret = dev_->activateStream(stream_);
        if (ret != 0) {
            setError(std::string("activateStream failed: ") + SoapySDR::errToStr(ret));
            return false;
        }
        running_ = true;
        // Clears the fault too: a stream that just activated is a new one, and
        // leaving the previous stream's fault latched would have the pipeline
        // stop the moment it started.
        clearError();
        return true;
    } catch (const std::exception& e) {
        setError(describe(e, "activateStream failed"));
        return false;
    } catch (...) {
        setError("activateStream failed: non-standard exception");
        return false;
    }
}

void SoapySource::stop() {
    if (!running_) {
        return;  // idempotent, and a safe no-op before open
    }
    // Mark stopped before touching the device: even if deactivation faults,
    // this object's state machine must land in "stopped".
    running_ = false;
    try {
        const int ret = dev_->deactivateStream(stream_);
        if (ret != 0) {
            // stop() is void, so the only reporting channel is lastError().
            setError(std::string("deactivateStream failed: ") + SoapySDR::errToStr(ret));
        }
    } catch (const std::exception& e) {
        setError(describe(e, "deactivateStream failed"));
    } catch (...) {
        setError("deactivateStream failed: non-standard exception");
    }
}

std::size_t SoapySource::read(std::complex<float>* dst, std::size_t n) {
    if (dev_ == nullptr || stream_ == nullptr || dst == nullptr || n == 0) {
        // Before open there is nothing to wait on: return the retry signal
        // immediately rather than burning the 100 ms timeout on nothing.
        return 0;
    }
    {
        // Already faulted: do not touch the driver again. Some drivers block
        // for the full timeout on every call against a handle whose device
        // has gone, which would make the pipeline's shutdown crawl. The source
        // loop polls faulted() and leaves; this is the belt to that braces.
        std::lock_guard<std::mutex> lk(errorMutex_);
        if (faulted_) { return 0; }
    }
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
    if (dev_ == nullptr) {
        setError("setSampleRateHz() called with no device open");
        return false;
    }
    if (!(hz > 0.0)) {  // negated compare so NaN also lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    try {
        dev_->setSampleRate(SOAPY_SDR_RX, kChannel, hz);
        // Readback, not echo: drivers coerce to the nearest supported rate
        // and the DSP chain must follow the hardware's real clock.
        sampleRateHz_ = dev_->getSampleRate(SOAPY_SDR_RX, kChannel);
        return true;
    } catch (const std::exception& e) {
        setError(describe(e, "setSampleRate failed"));
        return false;
    } catch (...) {
        setError("setSampleRate failed: non-standard exception");
        return false;
    }
}

bool SoapySource::setCenterFrequencyHz(double hz) {
    if (dev_ == nullptr) {
        setError("setCenterFrequencyHz() called with no device open");
        return false;
    }
    try {
        dev_->setFrequency(SOAPY_SDR_RX, kChannel, hz);
        // Same readback rationale as the sample rate: the synthesizer lands
        // where its step size allows, and the display tracks the hardware.
        centerFrequencyHz_ = dev_->getFrequency(SOAPY_SDR_RX, kChannel);
        return true;
    } catch (const std::exception& e) {
        setError(describe(e, "setFrequency failed"));
        return false;
    } catch (...) {
        setError("setFrequency failed: non-standard exception");
        return false;
    }
}

std::vector<std::string> SoapySource::listGainNames() {
    if (dev_ == nullptr) {
        return {};  // no device, no gain stages — not an error
    }
    try {
        return dev_->listGains(SOAPY_SDR_RX, kChannel);
    } catch (...) {
        return {};
    }
}

bool SoapySource::setGainDb(const std::string& name, double db) {
    if (dev_ == nullptr) {
        setError("setGainDb() called with no device open");
        return false;
    }
    try {
        dev_->setGain(SOAPY_SDR_RX, kChannel, name, db);
        return true;
    } catch (const std::exception& e) {
        setError(describe(e, "setGain failed"));
        return false;
    } catch (...) {
        setError("setGain failed: non-standard exception");
        return false;
    }
}

bool SoapySource::setAutoGain(bool on) {
    if (dev_ == nullptr) {
        setError("setAutoGain() called with no device open");
        return false;
    }
    try {
        if (!dev_->hasGainMode(SOAPY_SDR_RX, kChannel)) {
            // Distinct from a driver fault: the hardware simply has no AGC,
            // and the GUI uses this to grey out the checkbox.
            setError("device has no automatic gain mode");
            return false;
        }
        dev_->setGainMode(SOAPY_SDR_RX, kChannel, on);
        return true;
    } catch (const std::exception& e) {
        setError(describe(e, "setGainMode failed"));
        return false;
    } catch (...) {
        setError("setGainMode failed: non-standard exception");
        return false;
    }
}

std::vector<std::string> SoapySource::listAntennas() {
    if (dev_ == nullptr) {
        return {};  // no device, no ports — not an error
    }
    try {
        return dev_->listAntennas(SOAPY_SDR_RX, kChannel);
    } catch (const std::exception& e) {
        setError(describe(e, "listAntennas failed"));
        return {};
    } catch (...) {
        setError("listAntennas failed: non-standard exception");
        return {};
    }
}

bool SoapySource::setAntenna(const std::string& name) {
    if (dev_ == nullptr) {
        setError("setAntenna() called with no device open");
        return false;
    }
    try {
        // Checked against the driver's own list first. Soapy drivers vary in
        // what they do with an unknown antenna name - some throw, some ignore
        // it silently - and a silently ignored port change is precisely the
        // failure this whole method exists to end.
        const std::vector<std::string> avail = dev_->listAntennas(SOAPY_SDR_RX, kChannel);
        if (std::find(avail.begin(), avail.end(), name) == avail.end()) {
            setError("device has no RX antenna called \"" + name + "\"");
            return false;
        }
        dev_->setAntenna(SOAPY_SDR_RX, kChannel, name);
        return true;
    } catch (const std::exception& e) {
        setError(describe(e, "setAntenna failed"));
        return false;
    } catch (...) {
        setError("setAntenna failed: non-standard exception");
        return false;
    }
}

std::string SoapySource::antenna() {
    if (dev_ == nullptr) {
        return {};
    }
    try {
        return dev_->getAntenna(SOAPY_SDR_RX, kChannel);
    } catch (const std::exception& e) {
        setError(describe(e, "getAntenna failed"));
        return {};
    } catch (...) {
        setError("getAntenna failed: non-standard exception");
        return {};
    }
}

}  // namespace cascade::source
