// sdrconnect_protocol.hpp - the SDRconnect WebSocket API's messages as plain functions: building
// the JSON requests, reading the JSON pushes and replies, decoding the binary frames. No sockets.
//
// PROVENANCE. SDRplay's own document "SDRconnect WebSocket API" (https://api.sdrplay.com/docs/
// SDRconnect_WebSocket_API.pdf, 6 pages, author shown in the PDF metadata as Andy Carpenter,
// version 1.0.3, fetched and read in full on 2026-11-11; linked from
// https://sdrplay.com/websocket-api, which states no licence, only that the API "allows
// applications to be written"; SDRplay states no licence or terms for the document itself,
// copyright footer only: NOT VERIFIED beyond that). Nothing but that document was used; the
// C# and JavaScript examples it links were not opened.
//
// WHAT THE DOCUMENT SAYS (and this file implements):
//   * Text messages are single JSON objects with the string fields event_type, property, value
//     and (optional) device ("primary" or "secondary"; the Primary is assumed when absent).
//   * From SDRconnect: event_type "property_changed" and "get_property_response".
//   * To SDRconnect: set_property, get_property, iq_stream_enable, audio_stream_enable,
//     spectrum_enable, device_stream_enable, selected_device (index), selected_device_serial,
//     selected_device_name, start_recording, stop_recording, apply_device_profile,
//     set_primary_device_enable, set_secondary_device_enable. Values are strings
//     ("true"/"false", decimal numbers).
//   * Properties include device_center_frequency (Uint64 Hz), device_sample_rate (Double Hz),
//     device_vfo_frequency, lna_state, lna_state_min, lna_state_max, filter_bandwidth,
//     demodulator, started, overload, valid_antennas (comma list), active_antenna,
//     valid_devices (comma list), active_device, api_version (string).
//   * Binary messages start with a 2-byte little-endian payload type: 1 audio (signed 16-bit
//     stereo, 48 kHz), 2 signed 16-bit interleaved IQ, 3 unsigned 8-bit spectrum bins (these
//     three for the Primary device), 4/5/6 the same for the Secondary device.
//
// WHAT THE DOCUMENT DOES NOT SAY (NOT VERIFIED, not guessed): the TCP port and the WebSocket
// path (the re-check found port 5454 in a search snippet only; the source asks for it and
// defaults to / with the port in the address), the sample rate of the IQ stream (this client
// assumes it equals device_sample_rate and says so), whether an IQ sample is scaled to the
// full 16-bit range (it is divided by 32768), the byte order of the IQ pairs (I first, as
// "IQIQ" says), and the "network mode" suffix values beyond the three names it lists. The
// re-check note of the same date recorded the type numbers as 0/1/2; the document as read
// here (page 6 table) numbers them 1/2/3; this file follows the document read in full and
// keeps the numbers in ONE place (kBinary*) so a real server that disagrees is a one-line fix.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::source::sdrconnect {

constexpr std::uint16_t kBinaryAudioPrimary = 1;
constexpr std::uint16_t kBinaryIqPrimary = 2;
constexpr std::uint16_t kBinarySpectrumPrimary = 3;
constexpr std::uint16_t kBinaryAudioSecondary = 4;
constexpr std::uint16_t kBinaryIqSecondary = 5;
constexpr std::uint16_t kBinarySpectrumSecondary = 6;

struct Envelope {
    std::string eventType;
    std::string property;
    std::string value;
    std::string device;  // empty when absent
};

// A request as JSON text. `device` is omitted when empty.
inline std::string buildMessage(const std::string& eventType, const std::string& property,
                                const std::string& value, const std::string& device = std::string()) {
    nlohmann::json j;
    j["event_type"] = eventType;
    j["property"] = property;
    if (!device.empty()) { j["device"] = device; }
    j["value"] = value;
    return j.dump();
}

// Reads one text message. False (and `out` untouched) for anything that is not a JSON object
// with string event_type and string-or-number value; extra fields are ignored; a missing
// property or device is empty. Numbers and booleans in "value" are accepted and rendered as
// text, since a real server may not quote them (the document's examples do).
inline bool parseMessage(const std::string& text, Envelope& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return false; }
    const auto et = j.find("event_type");
    if (et == j.end() || !et->is_string()) { return false; }
    Envelope e;
    e.eventType = et->get<std::string>();
    const auto prop = j.find("property");
    if (prop != j.end() && prop->is_string()) { e.property = prop->get<std::string>(); }
    const auto dev = j.find("device");
    if (dev != j.end() && dev->is_string()) { e.device = dev->get<std::string>(); }
    const auto val = j.find("value");
    if (val != j.end()) {
        if (val->is_string()) { e.value = val->get<std::string>(); }
        else if (val->is_number() || val->is_boolean()) { e.value = val->dump(); }
        else { return false; }
    }
    out = std::move(e);
    return true;
}

enum class BinaryKind { Audio, Iq, Spectrum, Unknown, TooShort };

struct BinaryFrame {
    BinaryKind kind = BinaryKind::TooShort;
    bool secondary = false;
    std::size_t payloadOffset = 0;  // bytes before the payload
};

inline BinaryFrame classifyBinary(const std::uint8_t* d, std::size_t n) {
    BinaryFrame f;
    if (n < 2) { return f; }
    const std::uint16_t t = static_cast<std::uint16_t>(d[0] | (d[1] << 8));
    f.payloadOffset = 2;
    switch (t) {
        case kBinaryAudioPrimary: f.kind = BinaryKind::Audio; break;
        case kBinaryIqPrimary: f.kind = BinaryKind::Iq; break;
        case kBinarySpectrumPrimary: f.kind = BinaryKind::Spectrum; break;
        case kBinaryAudioSecondary: f.kind = BinaryKind::Audio; f.secondary = true; break;
        case kBinaryIqSecondary: f.kind = BinaryKind::Iq; f.secondary = true; break;
        case kBinarySpectrumSecondary: f.kind = BinaryKind::Spectrum; f.secondary = true; break;
        default: f.kind = BinaryKind::Unknown; break;
    }
    return f;
}

// Signed little-endian 16-bit I,Q pairs to complex float (divided by 32768). A trailing partial
// pair (fewer than 4 bytes) is dropped and not counted. Returns the pairs produced.
inline std::size_t decodeIq16(const std::uint8_t* d, std::size_t n, std::vector<std::complex<float>>& out) {
    const std::size_t pairs = n / 4;
    out.resize(pairs);
    for (std::size_t i = 0; i < pairs; ++i) {
        const std::int16_t a = static_cast<std::int16_t>(d[4 * i] | (d[4 * i + 1] << 8));
        const std::int16_t b = static_cast<std::int16_t>(d[4 * i + 2] | (d[4 * i + 3] << 8));
        out[i] = {static_cast<float>(a) * (1.0f / 32768.0f), static_cast<float>(b) * (1.0f / 32768.0f)};
    }
    return pairs;
}

// The comma-delimited lists (valid_antennas, valid_devices): trimmed, empty items dropped.
inline std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        std::size_t e = s.find(',', pos);
        if (e == std::string::npos) { e = s.size(); }
        std::string item = s.substr(pos, e - pos);
        std::size_t a = 0, b = item.size();
        while (a < b && (item[a] == ' ' || item[a] == '\t')) { ++a; }
        while (b > a && (item[b - 1] == ' ' || item[b - 1] == '\t')) { --b; }
        item = item.substr(a, b - a);
        if (!item.empty()) { out.push_back(item); }
        pos = e + 1;
    }
    return out;
}

}  // namespace cascade::source::sdrconnect
