// soapy_log_bridge.hpp - the SoapySDR logger, delivered into our own log.
//
// WHY. A field crash in 0.88.0 reached the crash store with a log tail that
// said nothing about the radio: the driver had been complaining for a minute
// before the fault, and every word of it went to SoapySDR's DEFAULT log
// handler, which prints to stderr - somewhere no report ever looks. SoapyRTLSDR
// and SoapyUHD log through SoapySDR::log(); this registers a handler that
// writes those lines into the diagnostic ring and file, prefixed "soapy: ",
// so the tail a report carries contains what the driver said.
//
// TWO HALVES, ON PURPOSE. SoapyLogBridge is pure: given a level, a message
// and a clock it decides what to log, and it can be exercised with no
// SoapySDR runtime present (tests/test_soapy_log_bridge.cpp does). The
// registration - the only line that touches SoapySDR - is
// installSoapyLogBridge(), and it may only be called once the runtime is
// known to load: cascade.exe delay-loads SoapySDR.dll, so a call before
// SoapySource::runtimeAvailable() has answered is a structured exception on
// a machine without the DLL, not a C++ one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_SOAPY_LOG_BRIDGE_HPP
#define CASCADE_SOURCE_SOAPY_LOG_BRIDGE_HPP

#include "core/diag_log.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source {

enum class SoapyLogAction { Drop, Info, Warn };

// The level map. `level` is a SoapySDRLogLevel value: 1 FATAL, 2 CRITICAL,
// 3 ERROR, 4 WARNING, 5 NOTICE, 6 INFO, 7 DEBUG, 8 TRACE, 9 SSI (the "O" and
// "U" stream indicators). FATAL..WARNING become warnings in our log,
// NOTICE/INFO/SSI become info lines, and DEBUG/TRACE are dropped unless the
// caller wants them (FOXSDR_SOAPY_DEBUG=1). Pure.
SoapyLogAction classifySoapyLogLevel(int level, bool debugWanted);

// "1", "true", "yes", "on" (any case) mean wanted; anything else, including
// unset, means not. Pure.
bool soapyDebugWanted(const char* envValue);

struct SoapyLogEmit {
    const char* level;  // "info" or "warn", the DiagLog level word
    std::string text;   // the whole line, prefix included
};

// The pure half of the handler: classification, the "soapy: " prefix, the
// vendor scrub (serials stripped, frequencies masked - see
// core::scrubVendorLine) and the per-second limit with its "N more lines
// suppressed" report. feed() returns the lines to write, in order: zero, one,
// or the suppression report followed by the line.
class SoapyLogBridge {
public:
    explicit SoapyLogBridge(bool debugWanted,
                            unsigned perSecond = core::LineRateLimiter::kDefaultPerSecond)
        : debugWanted_(debugWanted), limiter_(perSecond) {}

    std::vector<SoapyLogEmit> feed(int level, const char* message, std::uint64_t nowMs);

private:
    bool debugWanted_;
    core::LineRateLimiter limiter_;
};

// Registers the handler with SoapySDR. Reads FOXSDR_SOAPY_DEBUG, and when it
// is set also lowers SoapySDR's own threshold to TRACE (its default is INFO,
// so debug lines would otherwise never reach any handler). Safe to call more
// than once; every call after the first returns having done nothing.
//
// MUST FOLLOW SoapySource::runtimeAvailable() returning true - see the file
// header. The natural call site is inside that function, right after the
// LoadLibrary probe succeeds.
void installSoapyLogBridge();
bool soapyLogBridgeInstalled();

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_SOAPY_LOG_BRIDGE_HPP
