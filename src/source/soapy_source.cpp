// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.hpp>

#include <algorithm>
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
    static const bool available = (::LoadLibraryA("SoapySDR.dll") != nullptr);
    return available;
#else
    return true;  // POSIX builds link it normally
#endif
}

std::vector<SoapyDeviceInfo> SoapySource::enumerate() {
    std::vector<SoapyDeviceInfo> out;
    if (!runtimeAvailable()) { return out; }  // no runtime: no devices, no crash
    try {
        for (const SoapySDR::Kwargs& kw : SoapySDR::Device::enumerate()) {
            SoapyDeviceInfo info;
            // Modules put a display string under "label"; fall back to the
            // driver key so the menu never shows a blank row.
            const auto label = kw.find("label");
            if (label != kw.end() && !label->second.empty()) {
                info.label = label->second;
            } else {
                const auto driver = kw.find("driver");
                info.label = (driver != kw.end() && !driver->second.empty())
                                 ? driver->second
                                 : "unknown device";
            }
            // The FULL kwargs, not just the driver key: serial/index keys are
            // what pick the same physical unit out of several identical ones.
            info.args = SoapySDR::KwargsToString(kw);
            out.push_back(std::move(info));
        }
    } catch (...) {
        // "Never throws; empty on none/no modules": a probe failure in some
        // broken vendor module must not take the Source menu down with it.
        out.clear();
    }
    return out;
}

bool SoapySource::open(const std::string& args) {
    // Reopen semantics: whatever was open before must be fully released
    // first, or the old device handle (and its USB claim) would leak.
    closeDevice();
    if (!runtimeAvailable()) {
        lastError_ =
            "SoapySDR runtime not found (SoapySDR.dll). Reinstall, or use the "
            "signal generator / IQ file sources, which need no hardware.";
        return false;
    }
    try {
        dev_ = SoapySDR::Device::make(args);
        if (dev_ == nullptr) {
            // make() normally throws on failure, but a null return is legal
            // API-wise and must not turn into a null deref at setupStream.
            lastError_ = "SoapySDR::Device::make returned no device for args: " + args;
            return false;
        }
        if (dev_->getNumChannels(SOAPY_SDR_RX) < 1) {
            lastError_ = "device has no RX channels (args: " + args + ")";
            teardown();
            return false;
        }
        // CF32 everywhere: the DSP chain is complex<float>, and every Soapy
        // module provides CF32 via the built-in converter registry even when
        // the wire format is CS16/CS8, so no per-driver format logic here.
        stream_ = dev_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32,
                                    std::vector<std::size_t>{kChannel});
        if (stream_ == nullptr) {
            lastError_ = "setupStream(RX, CF32) returned no stream (args: " + args + ")";
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
        lastError_.clear();
        return true;
    } catch (const std::exception& e) {
        lastError_ = describe(e, "open failed");
        teardown();
        return false;
    } catch (...) {
        lastError_ = "open failed: non-standard exception (args: " + args + ")";
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
}

bool SoapySource::start() {
    if (dev_ == nullptr || stream_ == nullptr) {
        // Documented before-open behavior: refuse with a reason, no crash.
        lastError_ = "start() called with no device open";
        return false;
    }
    if (running_) {
        return true;  // idempotent per the IqSource contract
    }
    try {
        const int ret = dev_->activateStream(stream_);
        if (ret != 0) {
            lastError_ = std::string("activateStream failed: ") + SoapySDR::errToStr(ret);
            return false;
        }
        running_ = true;
        lastError_.clear();
        return true;
    } catch (const std::exception& e) {
        lastError_ = describe(e, "activateStream failed");
        return false;
    } catch (...) {
        lastError_ = "activateStream failed: non-standard exception";
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
            lastError_ = std::string("deactivateStream failed: ") + SoapySDR::errToStr(ret);
        }
    } catch (const std::exception& e) {
        lastError_ = describe(e, "deactivateStream failed");
    } catch (...) {
        lastError_ = "deactivateStream failed: non-standard exception";
    }
}

std::size_t SoapySource::read(std::complex<float>* dst, std::size_t n) {
    if (dev_ == nullptr || stream_ == nullptr || dst == nullptr || n == 0) {
        // Before open there is nothing to wait on: return the retry signal
        // immediately rather than burning the 100 ms timeout on nothing.
        return 0;
    }
    void* buffs[1] = {dst};
    int flags = 0;
    long long timeNs = 0;
    try {
        const int ret = dev_->readStream(stream_, buffs, n, flags, timeNs, kReadTimeoutUs);
        if (ret > 0) {
            return static_cast<std::size_t>(ret);
        }
        if (ret != SOAPY_SDR_TIMEOUT) {
            // Overflow, device loss, etc.: record it, but still answer with
            // the retry signal — transient faults (overflow) self-heal on
            // the next call, and fatal ones keep returning 0 until the
            // control thread notices and closes the device.
            lastError_ = std::string("readStream failed: ") + SoapySDR::errToStr(ret);
        }
        return 0;
    } catch (const std::exception& e) {
        lastError_ = describe(e, "readStream failed");
        return 0;
    } catch (...) {
        lastError_ = "readStream failed: non-standard exception";
        return 0;
    }
}

bool SoapySource::setSampleRateHz(double hz) {
    if (dev_ == nullptr) {
        lastError_ = "setSampleRateHz() called with no device open";
        return false;
    }
    if (!(hz > 0.0)) {  // negated compare so NaN also lands here
        lastError_ = "setSampleRateHz() requires a positive rate";
        return false;
    }
    try {
        dev_->setSampleRate(SOAPY_SDR_RX, kChannel, hz);
        // Readback, not echo: drivers coerce to the nearest supported rate
        // and the DSP chain must follow the hardware's real clock.
        sampleRateHz_ = dev_->getSampleRate(SOAPY_SDR_RX, kChannel);
        return true;
    } catch (const std::exception& e) {
        lastError_ = describe(e, "setSampleRate failed");
        return false;
    } catch (...) {
        lastError_ = "setSampleRate failed: non-standard exception";
        return false;
    }
}

bool SoapySource::setCenterFrequencyHz(double hz) {
    if (dev_ == nullptr) {
        lastError_ = "setCenterFrequencyHz() called with no device open";
        return false;
    }
    try {
        dev_->setFrequency(SOAPY_SDR_RX, kChannel, hz);
        // Same readback rationale as the sample rate: the synthesizer lands
        // where its step size allows, and the display tracks the hardware.
        centerFrequencyHz_ = dev_->getFrequency(SOAPY_SDR_RX, kChannel);
        return true;
    } catch (const std::exception& e) {
        lastError_ = describe(e, "setFrequency failed");
        return false;
    } catch (...) {
        lastError_ = "setFrequency failed: non-standard exception";
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
        lastError_ = "setGainDb() called with no device open";
        return false;
    }
    try {
        dev_->setGain(SOAPY_SDR_RX, kChannel, name, db);
        return true;
    } catch (const std::exception& e) {
        lastError_ = describe(e, "setGain failed");
        return false;
    } catch (...) {
        lastError_ = "setGain failed: non-standard exception";
        return false;
    }
}

bool SoapySource::setAutoGain(bool on) {
    if (dev_ == nullptr) {
        lastError_ = "setAutoGain() called with no device open";
        return false;
    }
    try {
        if (!dev_->hasGainMode(SOAPY_SDR_RX, kChannel)) {
            // Distinct from a driver fault: the hardware simply has no AGC,
            // and the GUI uses this to grey out the checkbox.
            lastError_ = "device has no automatic gain mode";
            return false;
        }
        dev_->setGainMode(SOAPY_SDR_RX, kChannel, on);
        return true;
    } catch (const std::exception& e) {
        lastError_ = describe(e, "setGainMode failed");
        return false;
    } catch (...) {
        lastError_ = "setGainMode failed: non-standard exception";
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
        lastError_ = describe(e, "listAntennas failed");
        return {};
    } catch (...) {
        lastError_ = "listAntennas failed: non-standard exception";
        return {};
    }
}

bool SoapySource::setAntenna(const std::string& name) {
    if (dev_ == nullptr) {
        lastError_ = "setAntenna() called with no device open";
        return false;
    }
    try {
        // Checked against the driver's own list first. Soapy drivers vary in
        // what they do with an unknown antenna name - some throw, some ignore
        // it silently - and a silently ignored port change is precisely the
        // failure this whole method exists to end.
        const std::vector<std::string> avail = dev_->listAntennas(SOAPY_SDR_RX, kChannel);
        if (std::find(avail.begin(), avail.end(), name) == avail.end()) {
            lastError_ = "device has no RX antenna called \"" + name + "\"";
            return false;
        }
        dev_->setAntenna(SOAPY_SDR_RX, kChannel, name);
        return true;
    } catch (const std::exception& e) {
        lastError_ = describe(e, "setAntenna failed");
        return false;
    } catch (...) {
        lastError_ = "setAntenna failed: non-standard exception";
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
        lastError_ = describe(e, "getAntenna failed");
        return {};
    } catch (...) {
        lastError_ = "getAntenna failed: non-standard exception";
        return {};
    }
}

}  // namespace cascade::source
