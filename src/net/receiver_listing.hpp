// receiver_listing.hpp - the public "status.json" of FoxSDR's web page, the document a receiver directory (Receiverbook) reads
// from <base-url>/status.json to list a web receiver (oss-net item 4).
//
// OFF BY DEFAULT, PUBLIC FIELDS ONLY. Nothing is published until the user turns it on AND fills the fields in. The document holds exactly
// what the user typed: a receiver name, a location text, optionally a latitude and longitude, and a list of bands (a name, a centre
// frequency and a sample rate each), plus the program's version and the listener limit (both set by the host, not secrets). It holds no
// operator e-mail address (the format's "admin" field is left out on purpose), no altitude, no serial number, no radio model, no address of
// this machine, no session or user, nothing read from the receiver's live state. A listing that is off answers 404, as if the route did not
// exist. Registering with the directory stays a step the user takes in their own browser (it asks for the page's URL); no key, token or
// password is stored, sent or read here (the directory also has a key handshake for owners who claim a receiver; that is not built, and
// nothing about it was read).
//
// WHERE THE FORMAT COMES FROM (the facts of a document, observed; no source code of the directory or of OpenWebRX was opened):
//   * the public status page of a running OpenWebRX receiver, https://openwebrx.zl3bcj.duckdns.org/status.json, read on 2026-11-12 (a
//     public receiver's own page; its operator's e-mail address, shown there, was not copied anywhere), gave this structure:
//       receiver {name: string, admin: string, gps: {lat: number, lon: number}, asl: number, location: string},
//       max_clients: number, version: string,
//       sdrs: [ {name: string, type: string, profiles: [ {name: string, center_freq: number, sample_rate: number} ]} ];
//   * the OpenWebRX project's API wiki page (documentation, not source), read through the mirror
//     https://github-wiki-see.page/m/jketterl/openwebrx/wiki/API on 2026-11-12, shows the same top-level keys for v0.20.3 (receiver with a
//     callsign-like name, admin, gps, asl and location; max_clients; version; sdrs with profiles of name, center_freq, sample_rate).
//   Both agree on the keys. WHICH FIELDS THE DIRECTORY REQUIRES, whether it wants a "type" it knows, and whether a receiver without "gps"
//   appears on its map are UNVERIFIED: nothing public says so, and the lane did not (and may not) contact the directory. The document is
//   therefore shaped like a real OpenWebRX one, with the fields above, and the user checks the result on the directory's own
//   "report a new receiver" page.
//
// LIMITS (every one tested at its boundary): name 1..64 bytes, location 0..96 bytes (both valid UTF-8, no control characters), latitude
// -90..90 and longitude -180..180 (both or neither), up to 32 bands, a band's name 1..48 bytes, centre frequency 1 Hz .. 1 THz, sample rate
// 1 Hz .. 10 GHz, listener limit 1..1000, version 0..32 printable ASCII characters. An invalid listing is REFUSED, never cut or repaired.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::net {

inline constexpr std::size_t kListingMaxName = 64;
inline constexpr std::size_t kListingMaxLocation = 96;
inline constexpr std::size_t kListingMaxBandName = 48;
inline constexpr std::size_t kListingMaxBands = 32;
inline constexpr std::size_t kListingMaxVersion = 32;
inline constexpr int kListingMaxClients = 1000;

struct ListedBand {
    std::string name;
    double centerHz = 0.0;
    double sampleRateHz = 0.0;
};

struct ReceiverListing {
    bool enabled = false;             // OFF until the user turns it on
    std::string name;                 // the receiver's public name
    std::string location;             // free text, e.g. a town and a region
    bool hasPosition = false;         // latitude and longitude, both typed by the user, or neither
    double latitude = 0.0, longitude = 0.0;
    std::vector<ListedBand> bands;
    int maxClients = 1;               // the listener limit the host publishes
    std::string version;              // set by the host, e.g. "v0.99.109"
};

// Valid UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF), no control character (below 0x20, 0x7F), at most maxBytes.
inline bool listingTextOk(const std::string& s, std::size_t maxBytes) {
    if (s.size() > maxBytes) { return false; }
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) { return false; }
        if (c < 0x80) { ++i; continue; }
        std::size_t need = 0;
        std::uint32_t cp = 0, minCp = 0;
        if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; minCp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; minCp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; minCp = 0x10000; }
        else { return false; }
        if (i + need >= s.size()) { return false; }   // the sequence runs past the end
        for (std::size_t k = 1; k <= need; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { return false; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { return false; }
        i += need + 1;
    }
    return true;
}

// The first thing wrong with a listing, as a sentence, or an empty string when it may be published. A DISABLED listing is always valid (it
// publishes nothing); an enabled one needs a name.
inline std::string validateListing(const ReceiverListing& l) {
    if (!l.enabled) { return {}; }
    if (l.name.empty()) { return "type a name for the receiver"; }
    if (!listingTextOk(l.name, kListingMaxName)) { return "the receiver name must be 1 to 64 bytes of text with no control characters"; }
    if (!listingTextOk(l.location, kListingMaxLocation)) { return "the location must be at most 96 bytes of text with no control characters"; }
    if (l.hasPosition) {
        if (!std::isfinite(l.latitude) || l.latitude < -90.0 || l.latitude > 90.0) { return "the latitude must be from -90 to 90"; }
        if (!std::isfinite(l.longitude) || l.longitude < -180.0 || l.longitude > 180.0) { return "the longitude must be from -180 to 180"; }
    }
    if (l.bands.size() > kListingMaxBands) { return "at most 32 bands can be listed"; }
    for (const ListedBand& b : l.bands) {
        if (b.name.empty() || !listingTextOk(b.name, kListingMaxBandName)) { return "a band name must be 1 to 48 bytes of text with no control characters"; }
        if (!std::isfinite(b.centerHz) || b.centerHz < 1.0 || b.centerHz > 1.0e12) { return "a band's centre frequency must be from 1 Hz to 1 THz"; }
        if (!std::isfinite(b.sampleRateHz) || b.sampleRateHz < 1.0 || b.sampleRateHz > 1.0e10) { return "a band's sample rate must be from 1 Hz to 10 GHz"; }
    }
    if (l.maxClients < 1 || l.maxClients > kListingMaxClients) { return "the listener limit must be from 1 to 1000"; }
    if (l.version.size() > kListingMaxVersion) { return "the version text is too long"; }
    for (char c : l.version) {
        if (c < 0x20 || c > 0x7E) { return "the version text must be printable ASCII"; }
    }
    return {};
}

// The document. An invalid or disabled listing yields an empty string: the caller answers 404.
inline std::string listingJson(const ReceiverListing& l) {
    if (!l.enabled || !validateListing(l).empty()) { return {}; }
    nlohmann::json receiver;
    receiver["name"] = l.name;
    receiver["location"] = l.location;
    if (l.hasPosition) { receiver["gps"] = {{"lat", l.latitude}, {"lon", l.longitude}}; }
    nlohmann::json profiles = nlohmann::json::array();
    for (const ListedBand& b : l.bands) {
        nlohmann::json p;
        p["name"] = b.name;
        p["center_freq"] = static_cast<long long>(std::llround(b.centerHz));
        p["sample_rate"] = static_cast<long long>(std::llround(b.sampleRateHz));
        profiles.push_back(p);
    }
    nlohmann::json sdr;
    sdr["name"] = l.name;
    sdr["type"] = "FoxSDR";
    sdr["profiles"] = profiles;
    nlohmann::json doc;
    doc["receiver"] = receiver;
    doc["max_clients"] = l.maxClients;
    doc["version"] = l.version;
    doc["sdrs"] = nlohmann::json::array({sdr});
    return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace cascade::net
