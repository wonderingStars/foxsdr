// See soapy_log_bridge.hpp for why the driver's log is brought into ours and
// why the pure half is separate from the registration.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_log_bridge.hpp"

#include <SoapySDR/Logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace cascade::source {

SoapyLogAction classifySoapyLogLevel(int level, bool debugWanted) {
    switch (level) {
        case SOAPY_SDR_FATAL:
        case SOAPY_SDR_CRITICAL:
        case SOAPY_SDR_ERROR:
        case SOAPY_SDR_WARNING: return SoapyLogAction::Warn;
        case SOAPY_SDR_NOTICE:
        case SOAPY_SDR_INFO: return SoapyLogAction::Info;
        // Stream status indicators - UHD's "O" (overflow), "U" (underflow) and
        // "D" (dropped) - are the one-character evidence of a stalled USB
        // delivery, which is precisely what the 0.88.0 report was missing.
        // Kept as info; the limiter bounds how many of them a second can cost.
        case SOAPY_SDR_SSI: return SoapyLogAction::Info;
        case SOAPY_SDR_DEBUG:
        case SOAPY_SDR_TRACE: return debugWanted ? SoapyLogAction::Info : SoapyLogAction::Drop;
        default:
            // A level this build does not know. A future SoapySDR adding one
            // above TRACE is more likely to be chatter than a fault, but an
            // unknown value BELOW warning would be a fault of some kind; treat
            // anything at or above INFO's number as droppable debug and the
            // rest as a warning.
            return (level > SOAPY_SDR_INFO) ? (debugWanted ? SoapyLogAction::Info
                                                           : SoapyLogAction::Drop)
                                            : SoapyLogAction::Warn;
    }
}

bool soapyDebugWanted(const char* envValue) {
    if (envValue == nullptr) { return false; }
    std::string v(envValue);
    for (char& c : v) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::vector<SoapyLogEmit> SoapyLogBridge::feed(int level, const char* message,
                                               std::uint64_t nowMs) {
    std::vector<SoapyLogEmit> out;
    const SoapyLogAction action = classifySoapyLogLevel(level, debugWanted_);
    if (action == SoapyLogAction::Drop) { return out; }

    std::string text = (message != nullptr) ? message : "";
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) { return out; }

    // Dropped lines are counted AFTER classification, so a driver tracing at
    // DEBUG with debug off costs nothing against the budget of the warnings
    // that matter.
    std::uint64_t suppressed = 0;
    const bool admitted = limiter_.admit(nowMs, suppressed);
    if (suppressed != 0) {
        out.push_back({"info", "soapy: " + std::to_string(suppressed) + " more lines suppressed"});
    }
    if (admitted) {
        out.push_back({action == SoapyLogAction::Warn ? "warn" : "info",
                       "soapy: " + core::scrubVendorLine(text)});
    }
    return out;
}

namespace {

std::atomic<bool> g_installed{false};
std::mutex g_bridgeMutex;
SoapyLogBridge* g_bridge = nullptr;

std::uint64_t nowMs() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t).count());
}

// Called by SoapySDR from whichever thread logged - the GUI thread during an
// open, a vendor stream thread during a read - so the bridge's limiter is
// serialised here. The DiagLog write takes its own lock after this one is
// released; nothing in DiagLog calls back into SoapySDR, so the order cannot
// invert.
void handler(const SoapySDRLogLevel level, const char* message) {
    std::vector<SoapyLogEmit> lines;
    {
        std::lock_guard<std::mutex> lk(g_bridgeMutex);
        if (g_bridge == nullptr) { return; }
        lines = g_bridge->feed(static_cast<int>(level), message, nowMs());
    }
    for (const SoapyLogEmit& e : lines) { core::DiagLog::instance().write(e.level, e.text.c_str()); }
}

}  // namespace

void installSoapyLogBridge() {
    if (g_installed.exchange(true)) { return; }
    const bool debug = soapyDebugWanted(std::getenv("FOXSDR_SOAPY_DEBUG"));
    {
        std::lock_guard<std::mutex> lk(g_bridgeMutex);
        g_bridge = new SoapyLogBridge(debug);  // process lifetime, like the handler
    }
    // THE ONE LINE THAT TOUCHES SoapySDR. The caller has established that the
    // runtime loads (see the header); on a machine where it does not, this
    // would be the delay-load fault the whole runtimeAvailable() dance exists
    // to avoid.
    SoapySDR::registerLogHandler(&handler);
    if (debug) { SoapySDR::setLogLevel(SOAPY_SDR_TRACE); }
    core::diagLogf("soapy: log bridge installed - driver messages are recorded%s",
                   debug ? " (debug on)" : "");
}

bool soapyLogBridgeInstalled() { return g_installed.load(); }

}  // namespace cascade::source
