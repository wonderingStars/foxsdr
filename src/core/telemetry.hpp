// telemetry.hpp - anonymous, opt-in usage reporting.
//
// WHAT THIS IS FOR. Product decisions that are currently guesses: whether
// anyone runs this on Linux, whether people update, which decoders justify
// further work, which radios to prioritise, and how often it falls over. All
// of those are answerable from aggregate counts, and none of them need to know
// anything about a person.
//
// WHAT IT DELIBERATELY DOES NOT COLLECT, and these are design constraints
// rather than an oversight:
//
//   - NO frequencies. What somebody listens to is the most sensitive thing
//     this application knows, it is a criminal matter in some jurisdictions
//     (see the POCSAG and Inmarsat-C notices), and it would tell us nothing
//     we could act on.
//   - NO decoded content, ever. Uploading intercepted pager or satellite
//     traffic would make the user the discloser and us the recipient of it.
//   - NO position, of the receiver or of anything it hears.
//   - NO IP address or derived location is recorded. A request necessarily
//     carries an IP to the edge; nothing stores it.
//   - NO device serial numbers. The SDR MODEL is useful ("B200"); the serial
//     is a unique hardware identifier and is stripped - see sanitiseDevice().
//
// CONSENT. Reporting is OFF until the user turns it on, and the install id
// below is generated only at that moment. Storing an identifier on someone's
// machine for analytics requires consent under PECR regulation 6, and an
// opt-out default would not be consent. Turning it off again deletes the id.
//
// WHEN IT SENDS. At STARTUP, reporting the session that has already finished,
// never at exit. A network call on the shutdown path can hang the application
// while the user is trying to close it; and a report written to disk at exit
// would be lost by the one event most worth counting, a crash. Instead the
// session summary is journalled locally, and a clean exit is recorded - so a
// startup that finds the previous session unfinished knows it crashed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TELEMETRY_HPP
#define CASCADE_CORE_TELEMETRY_HPP

#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace cascade::core {

// One session's worth of counters, accumulated in memory and journalled to
// the config at exit. Everything here is a count or a coarse label.
struct TelemetrySession {
    std::uint64_t seconds = 0;                  // how long the session ran
    std::map<std::string, std::uint64_t> modeSeconds;  // "WFM" -> seconds
    std::vector<std::string> panels;            // panels opened, deduplicated
    std::vector<std::string> plugins;           // "name version", installed
    std::string sdrModel;                       // "B200"; serial stripped
};

// What is actually transmitted. Kept as a struct so a test can assert on the
// exact set of fields - the privacy promise is only as good as the payload.
struct TelemetryReport {
    std::string installId;
    std::string appVersion;
    std::string os;         // "Windows 10.0.22631"
    std::string arch;       // "x64"
    std::uint64_t launches = 0;
    std::uint64_t crashes = 0;
    TelemetrySession session;

    // Compact JSON, exactly the fields above and nothing else.
    std::string toJson() const;
};

// A device string as SoapySDR reports it, reduced to something that
// identifies the MODEL and nothing else.
//
// "driver=uhd, label=B200 EDR04ZDB2, product=B200, serial=EDR04ZDB2, type=b200"
//   -> "uhd b200"
//
// The serial is a unique per-unit identifier - it would let two reports be
// tied to the same physical radio, which is precisely what anonymity means
// not doing. Anything not on the allowed key list is dropped, so a driver
// that invents a new key cannot leak it by default.
std::string sanitiseDevice(const std::string& soapyArgs);

// A fresh random install id: 32 lowercase hex characters from the system CSPRNG.
// Random, never derived from anything about the machine or the user - a
// hash of a MAC address or a hostname would be a fingerprint wearing a
// disguise. Empty on failure, which suppresses reporting rather than
// falling back to something guessable.
std::string newInstallId();

// True when `id` is exactly the shape newInstallId produces. Used to refuse a
// hand-edited config that tried to put something meaningful in the field.
bool validInstallId(const std::string& id);

// "Windows 10.0.22631" - the OS and its build, no machine or user name.
std::string osDescription();
std::string archDescription();

// WHERE REPORTS GO. Empty in this build, which DISABLES reporting entirely:
// with nowhere to send, the setting is unavailable rather than silently
// collecting. Set it to the deployed Cloudflare Worker (a route on the
// project's own domain rather than a workers.dev address, so the endpoint can
// move without orphaning binaries already installed).
//
// FOXSDR_TELEMETRY_URL overrides it, which is how the tests point at a local
// stub without a network.
std::string telemetryEndpoint();

// Posts one report, on a thread of its own, and forgets about it.
//
// Fire-and-forget, but NOT detached: the destructor joins, because a detached
// thread writing into a WinHTTP handle while the process tears down is how a
// clean exit turns into a crash on exit - and a crash on exit would be
// counted, by this very feature, as a crash. Transfers are bounded by short
// timeouts so the join cannot hold shutdown open.
//
// Every failure is silent. A usage counter that interrupted somebody's
// listening to complain it could not reach a server would be worse than
// having no usage counter.
class TelemetryReporter {
public:
    TelemetryReporter() = default;
    ~TelemetryReporter();

    TelemetryReporter(const TelemetryReporter&) = delete;
    TelemetryReporter& operator=(const TelemetryReporter&) = delete;

    // No-op when `url` or `json` is empty, or when a send is already running.
    void send(const std::string& url, const std::string& json);

    bool busy() const;

private:
    std::thread thread_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_TELEMETRY_HPP
