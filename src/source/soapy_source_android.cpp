// soapy_source_android.cpp - SoapySource on Android: THE HONEST REFUSAL.
//
// WHY THIS FILE EXISTS RATHER THAN AN #ifdef IN app_window.cpp. SoapySDR's
// whole premise is a vendor module loaded at runtime from the machine's own
// library path - SoapyUHD for a B200, SoapyRTLSDR for a dongle - and that ABI
// does not exist on Android: there is no system SoapySDR, no module directory,
// and a phone reaches a radio over USB host mode with an fd handed down from
// Java, which is what src/usb's usbfs transport and the native drivers
// (rtlsdr, hackrf, airspy, ...) already do. So find_package(SoapySDR) is not
// even attempted for this ABI and soapy_source.cpp is excluded from the build
// (see the root CMakeLists).
//
// The interface still references the class, though, and in a dozen places
// that have nothing to do with opening a device: the Radio page lists
// enumerate()'s rows beside the native drivers', the Settings page reports
// moduleSearchPaths() and vendorInstalls() so a user can see WHY no radio was
// found, and the teardown asks anyDeviceOpen(). Putting an #ifdef at each of
// those sites would be a dozen branches in the largest file in the tree, each
// one a place for a later slice to get the two platforms out of step.
//
// THE SAME SHAPE THE AUDIO SINK ALREADY USES, therefore: one class, two
// implementations, exactly one compiled (audio_out.cpp is PortAudio,
// audio_out_aaudio.cpp is AAudio). Every entry point below answers the way
// soapy_source.cpp itself answers on a desktop with the core library present
// and ZERO vendor modules - a configuration the header calls "fully
// supported": enumerate() returns nothing, open() fails with a reason, and
// nothing treats "no hardware" as an error to crash over. The only difference
// is that here the reason is permanent, and it SAYS SO, in the one line a user
// will see on the Radio page.
//
// WHAT THIS IS NOT. It is not a claim that a phone cannot have a radio. The
// native drivers work on Android - that is what the usbfs adopted-fd slice was
// for - and they appear on the Radio page from src/source's own enumeration,
// not through this class. What is missing is SoapySDR, and only SoapySDR.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include "core/diag_log.hpp"

namespace cascade::source {
namespace {

// THE ONE LINE A USER SEES, and it is worth getting right: it has to say what
// is unavailable, that it is not a fault, and where a radio does come from on
// this platform - otherwise "no devices" reads as "this app cannot see my
// dongle", which is the wrong conclusion.
constexpr const char* kUnavailable =
    "SoapySDR is not available on Android - radios are reached through USB host "
    "mode by the built-in drivers instead";

}  // namespace

SoapySource::~SoapySource() = default;

// -- Enumeration and the runtime ------------------------------------------

std::vector<SoapyDeviceInfo> SoapySource::enumerate() { return {}; }
std::vector<SoapyDeviceInfo> SoapySource::enumerateInProcess() { return {}; }
bool SoapySource::runtimeAvailable() { return false; }

// Empty rather than a fabricated path: the Settings page prints these under
// "where FoxSDR looked", and a directory nothing ever searched would be a lie
// that sends a user looking in it.
std::vector<std::string> SoapySource::moduleSearchPaths() { return {}; }
std::vector<std::string> SoapySource::loadedModules() { return {}; }
std::vector<VendorRoot> SoapySource::vendorInstalls() { return {}; }

// -- Opening ---------------------------------------------------------------

bool SoapySource::open(const std::string& args) {
    (void)args;
    // Logged once per attempt rather than silently refused: a user who
    // selected a row and got nothing deserves the reason in the log the
    // diagnostics page can show them.
    // Logged rather than stored: setError() lives in soapy_source.cpp, which
    // this build does not compile, and lastError() below answers with the
    // same sentence unconditionally - there is no other error this class can
    // have on this platform.
    cascade::core::diagWarnf("soapy: %s", kUnavailable);
    return false;
}

void SoapySource::closeDevice() {}

const char* SoapySource::driverKey() const { return ""; }

bool SoapySource::anyDeviceOpen() { return false; }
int SoapySource::openDeviceCount() { return 0; }

// -- Gain, antennas, rates: nothing is open, so nothing has any ------------

std::vector<std::string> SoapySource::listGainNames() { return {}; }
std::vector<GainInfo> SoapySource::gains() const { return {}; }

bool SoapySource::setGainDb(const std::string& name, double db) {
    (void)name;
    (void)db;
    return false;
}

double SoapySource::gainDb(const std::string& name) const {
    (void)name;
    return 0.0;
}

bool SoapySource::autoGainSupported() const { return false; }

bool SoapySource::setAutoGain(bool on) {
    (void)on;
    return false;
}

bool SoapySource::autoGain() const { return false; }

std::vector<std::string> SoapySource::listAntennas() { return {}; }
std::vector<std::string> SoapySource::antennas() const { return {}; }

bool SoapySource::setAntenna(const std::string& name) {
    (void)name;
    return false;
}

std::string SoapySource::antenna() const { return {}; }
std::string SoapySource::antennaReadback() { return {}; }
std::vector<double> SoapySource::supportedSampleRatesHz() const { return {}; }

bool SoapySource::setSampleRateHz(double hz) {
    (void)hz;
    return false;
}

bool SoapySource::setCenterFrequencyHz(double hz) {
    (void)hz;
    return false;
}

bool SoapySource::frequencyRangeHz(double& loHz, double& hiHz) const {
    loHz = 0.0;
    hiHz = 0.0;
    return false;
}

// -- Streaming -------------------------------------------------------------

bool SoapySource::start() { return false; }
void SoapySource::stop() {}

std::size_t SoapySource::read(std::complex<float>* dst, std::size_t n) {
    (void)dst;
    (void)n;
    return 0;
}

// -- State -----------------------------------------------------------------

// NOT faulted and NOT dead, deliberately. Both words mean "something went
// wrong with a radio that was working", and the interface treats them that
// way - deviceDead() puts up the "restart FoxSDR to use this radio again"
// banner. Nothing went wrong here; the feature is simply not on this
// platform, which is what open()'s refusal already said.
bool SoapySource::faulted() const { return false; }
bool SoapySource::deviceDead() const { return false; }
SoapySource::DeadReason SoapySource::deadReason() const { return DeadReason::None; }
std::string SoapySource::faultedWhile() const { return {}; }

const char* SoapySource::name() const { return "SoapySDR (unavailable)"; }

const char* SoapySource::lastError() const { return kUnavailable; }

unsigned long long SoapySource::driverCallsAbandoned() { return 0ull; }

}  // namespace cascade::source
