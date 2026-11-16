// radio_report.hpp - the pure half of the radio reports (docs/radio-reports-contract.md, schema 2).
//
// WHAT THIS IS. The owner's rule (2026-11-15): "I want to know every fail of every radio, not an accumulation", sent whether or not the
// Diagnostics switch is on, and a record of everything that happens to every physical radio. This file holds the rules that need no disk, no
// clock and no network: the radio's key, the text scrub that keeps a serial, a path, an address and a frequency out of a sentence, the
// device-argument readers that find a radio's model and serial, and a C++ port of the contract's text rules that every message is held to
// before it is queued. The queue and the sender are radio_report_queue.hpp; the process-wide reporter the engine calls is radio_reporter.hpp.
//
// THE ONE SWITCH. The Worker that accepts the new message kinds is not deployed, and it must be live BEFORE any app sends them
// (contract section 11). So the live send of every new kind sits behind kRadioReportsLiveSend, which is false in this build: everything is
// queued, nothing leaves the machine. The release lane changes that ONE line when the Worker is confirmed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RADIO_REPORT_HPP
#define CASCADE_CORE_RADIO_REPORT_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace cascade::core::radioreport {

// THE ONE LINE THE RELEASE LANE CHANGES. False: every new message kind (radio_fail, the six journal kinds, session_env, first_session, and the
// beat's `radios` field) is queued on disk and NONE is sent. See the record "radio-reports-app.md".
inline constexpr bool kRadioReportsLiveSend = false;

inline constexpr int kSchema = 2;

// ---------------------------------------------------------------------------
// The contract's text rules (section 5), the same four checks in the same order as `textCode` in telemetry-worker/radio-reports.js, held to
// the parity table test-fixtures/radio-text-rules.json by tests/test_radio_report.cpp.
// ---------------------------------------------------------------------------
bool pathInText(const std::string& s);
bool addressInText(const std::string& s);
bool serialInText(const std::string& s, const std::string& ownSerial);
bool frequencyInText(const std::string& s);
// "path-in-text", "address-in-text", "serial-in-text", "frequency-in-text", or "" when the text is acceptable.
std::string textCode(const std::string& s, const std::string& ownSerial);

// ---------------------------------------------------------------------------
// The radio key (contract 1.1)
// ---------------------------------------------------------------------------
std::string modelSlug(const std::string& model);
// "<modelSlug>:<SERIAL upper-cased>", or "<modelSlug>:-" without a serial.
std::string radioKey(const std::string& model, const std::string& serial);

// ---------------------------------------------------------------------------
// Which radio a driver key and its device arguments name
// ---------------------------------------------------------------------------
struct RadioIdentity {
    std::string driver;   // one of the fourteen driver words
    std::string model;    // 1 to 32 characters, unchanged by scrubModel
    std::string serial;   // "" or ^[A-Za-z0-9._-]{1,40}$
    std::string key;      // radioKey(model, serial)
};

// The radio's hardware serial from its device arguments: SoapySDR's "driver=uhd, serial=EDR04ZDB2, ..." or a native radio's "serial=00000001".
// Empty for a radio that has none: rtl_tcp and a Pluto by address, an "index=N" row, the sound card, and anything whose value is not shaped
// like a serial. NEVER an address or a device argument string.
std::string serialFromArgs(const std::string& driverKind, const std::string& args);

// The radio a (driver key, device arguments) pair names. `nativeLabel` is the scanned row's label when the caller has one (the bus-reported
// description, "RTL-SDR (serial 00000001)"); empty gives the family's plain name. The model is what the usage report sends (sanitiseDevice for a
// SoapySDR kwargs string, the label without its serial otherwise) and always passes scrubModel, so it never carries an address or a unit name.
RadioIdentity identify(const std::string& driverKind, const std::string& args, const std::string& nativeLabel = std::string());
// The sound card: model "sound card" (never the card's name, which is often a person's), no serial.
RadioIdentity soundCardIdentity();

// ---------------------------------------------------------------------------
// The scrub
// ---------------------------------------------------------------------------
// scrubUploadLine first, then the radio's own serial out of the text, then every path (a drive path, \\host\share, \Users\x, /Users/x, /home/x)
// replaced by "<path>", then anything the text rules still refuse is removed (a frequency becomes '#', anything else withholds the sentence:
// "withheld: <code>"), then cut to `maxChars` characters on a character boundary. Never empty (a report needs a reason).
std::string scrubReason(const std::string& raw, const std::string& serial, std::size_t maxChars = 400);

// Only the radio lines of the log ring (the first word, after the time stamp and the level word, is a radio prefix of contract 2.1), each
// through the same scrub, in the contract's own shape "HH:MM:SS.mmm source: ..." (the ring's "info"/"warn" level word is dropped: the contract's
// pattern has none), at most `maxLines`, newest last, each at most 240 characters. A line the text rules still refuse is left out.
std::vector<std::string> radioLines(const std::vector<std::string>& ring, const std::string& serial, std::size_t maxLines = 40);

// Free text for any other field the text rules apply to (antenna, usbHost, cpu, gpu, a journal reason): scrubbed and clipped the same way.
std::string scrubText(const std::string& raw, const std::string& serial, std::size_t maxChars);

// Every drive path, UNC path and profile path of `s` replaced by "<path>".
std::string pathsToToken(const std::string& s);

// ---------------------------------------------------------------------------
// Small helpers the messages share
// ---------------------------------------------------------------------------
// `n` random lower-case hex characters (n even, at most 64) from the system CSPRNG; never derived from anything about the machine.
std::string randomHex(std::size_t n);
// `s` cut to at most `maxChars` UTF-8 characters on a character boundary.
std::string clipUtf8(const std::string& s, std::size_t maxChars);
// The number of UTF-8 characters.
std::size_t utf8Length(const std::string& s);
// "a0-b1" style: every character outside ^[A-Za-z0-9._+ -] removed, cut to 48; "" when nothing is left.
std::string versionText(const std::string& s);
// The CPU's brand string without its "@ 3.70GHz" part: a hertz number is refused by the text rules.
std::string cpuWithoutClock(const std::string& brand);

}  // namespace cascade::core::radioreport

#endif  // CASCADE_CORE_RADIO_REPORT_HPP
