// radio_report.hpp - the pure half of the radio reports (docs/radio-reports-contract.md, schema 2).
//
// WHAT THIS IS. The owner's rule (2026-11-15): "I want to know every fail of every radio, not an accumulation", sent whether or not the
// Diagnostics switch is on, and a record of everything that happens to every physical radio. This file holds the rules that need no disk, no
// clock and no network: the radio's key, the text scrub that keeps a serial, a path, an address and a frequency out of a sentence, the
// device-argument readers that find a radio's model and serial, and a C++ port of the contract's text rules that every message is held to
// before it is queued. The queue and the sender are radio_report_queue.hpp; the process-wide reporter the engine calls is radio_reporter.hpp.
//
// THE ONE SWITCH. The Worker that accepts the new message kinds must be live BEFORE any build with sending on is given to anyone (contract
// section 11: the Worker is deployed first). The live send of every new kind sits behind kRadioReportsLiveSend, which is TRUE from 0.99.109 (the
// owner's decision of 2026-11-17: this version sends). Each message is written to the queue first and then sent; one that cannot be sent (the Worker
// of before the deploy answers `bad id`, or the service or the site behind it is down) is kept and tried again for up to 14 days. A test, a drive or a
// CI run never sends anything off this computer: core/report_send_guard.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RADIO_REPORT_HPP
#define CASCADE_CORE_RADIO_REPORT_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace cascade::core::radioreport {

// True: every new message kind (radio_fail, the six journal kinds, session_env, first_session, and the beat's `radios` field) is queued on disk and
// then sent. False would queue and send nothing. See docs/multi-radio-records/send-on.md.
inline constexpr bool kRadioReportsLiveSend = true;

// THE BEAT'S `radios` FIELD STAYS OFF (send-on lane, 2026-11-17). The approved PRIVACY.md says the "still running" beat is the install identifier, the version
// and nothing else, and that serial numbers are removed from everything but the radio reports themselves; a beat naming the open radios by model and serial would
// contradict both. The program is held to the text, so the field is not sent. Turning it on needs the owner to approve a sentence first; the plumbing
// (HeartbeatSender::setRadios, the contract's 3.10 and the Worker's reader) is kept and tested.
inline constexpr bool kBeatCarriesRadios = false;

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
    std::string library;  // "" or `^[A-Za-z0-9._+ -]{1,48}$`: for a radio opened through SoapySDR, "SoapySDR <driver key>" (the vendor driver key, kept out of `model`)
};

// The radio's hardware serial from its device arguments: SoapySDR's "driver=uhd, serial=EDR04ZDB2, ..." or a native radio's "serial=00000001".
// Empty for a radio that has none: rtl_tcp and a Pluto by address, an "index=N" row, the sound card, and anything whose value is not shaped
// like a serial. NEVER an address or a device argument string.
std::string serialFromArgs(const std::string& driverKind, const std::string& args);

// The radio a (driver key, device arguments) pair names. `nativeLabel` is the scanned row's label when the caller has one (the bus-reported
// description, "RTL-SDR (serial 00000001)"); empty gives the family's plain name. The model is what the usage report sends (sanitiseDevice for a
// SoapySDR kwargs string, the label without its serial otherwise) and always passes scrubModel, so it never carries an address or a unit name.
//
// A RADIO OPENED THROUGH SOAPYSDR IS NAMED BY ITS OWN MODEL, NOT BY THE DRIVER KEY (hardware check 2026-11-17: a B200 was "uhd"): the device's `product` in the arguments
// (UHD: "B200"; the RTL module: the dongle's product string), else the model learned for this (driver, serial) from a scan row or an open (rememberSoapyModel), else the
// older usage-record text (sanitiseDevice). The vendor driver key goes in `library` ("SoapySDR uhd"). Nothing is probed here.
RadioIdentity identify(const std::string& driverKind, const std::string& args, const std::string& nativeLabel = std::string());
// Tell identify() what a SoapySDR radio is, for the arguments that carry no `product` (a hand-written "driver=uhd,serial=...", an older saved row). `model` is the
// hardware key the opened device reported ("B200") or a scanned row's own model; ignored when the arguments name no serial (two radios of a driver would share it).
void rememberSoapyModel(const std::string& args, const std::string& model);
// The model a scanned row's own arguments say ("product", else the "label" without its serial and bracketed parts); "" when they say none. Pure.
std::string soapyDeviceModel(const std::string& args);
// The sound card: model "sound card" (never the card's name, which is often a person's), no serial.
RadioIdentity soundCardIdentity();

// ---------------------------------------------------------------------------
// The scrub
// ---------------------------------------------------------------------------
// scrubUploadLine first, then the radio's own serial out of the text, then every path (a drive path, \\host\share, \Users\x, /Users/x, /home/x)
// replaced by "<path>", then anything the text rules still refuse is removed (a frequency becomes '#', anything else withholds the sentence:
// "withheld: <code>"), then cut to `maxChars` characters on a character boundary. Never empty (a report needs a reason).
std::string scrubReason(const std::string& raw, const std::string& serial, std::size_t maxChars = 400);

// THE VENDOR'S OWN WORDS for a log line about a failed open: the driver's sentence through the same scrub (the radio's serial out, paths and names masked),
// cut to 160 characters, and never empty ("the driver gave no reason" when it said nothing). The log used to carry only the one-word class.
std::string openFailureWords(const std::string& driverKind, const std::string& args, const std::string& reason);

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

// THE BAND NAME a centre frequency falls in, for the journal's `band` field (contract 3.3): a NAME from the contract's list and never the frequency.
// lf below 300 kHz, mf to 3 MHz, hf to 30 MHz, vhf-low to 87.5 MHz, fm-broadcast to 108 MHz, air-band to 137 MHz, vhf-high to 300 MHz, uhf to 1 GHz,
// l-band to 2 GHz, shf above; "other" for a frequency that is not positive. The edges are the application's own table, written here so the owner can read
// them; the site shows the name and nothing finer.
std::string bandName(double hz);

}  // namespace cascade::core::radioreport

#endif  // CASCADE_CORE_RADIO_REPORT_HPP
