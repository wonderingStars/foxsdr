// xplane_udp.hpp - flight-simulator traffic as a map source (hamops lane): the receive side of X-Plane's UDP data output as a pure parser and a
// bounded track store with the shape of other aircraft tracks (id, latitude, longitude, altitude, heading, speed, time).
//
// WHAT IS DOCUMENTED, AND WHAT IS NOT (opened 2026-11-11):
//   * RREF reply: https://xppython3.readthedocs.io/en/latest/development/udp/rref.html - header "RREF" + 1 pad byte, then one or more 8-byte records,
//     each a little-endian int32 index (the one the requester chose) and a little-endian float32 value; the total length is 5 + 8 x N.
//   * DATA: https://xppython3.readthedocs.io/en/latest/development/udp/data.html and https://nuclearprojects.com/xplane/xplaneref.html - the 5-byte
//     prologue "DATA" + 1 byte, then records of an int32 index (the item's position in the Data Output screen) and eight float32 values (36 bytes),
//     little-endian, several records to a datagram.
//   * Laminar's own 10.40 release notes (https://www.x-plane.com/kb/x-plane-10-40-release-notes/) confirm the RREF / RPOS commands and UDP port 49000
//     but carry no packet layout; the layouts live in a file in X-Plane's install ("Exchanging Data with X-Plane") that is not readable here.
//   * NOT documented in any page opened: which DATA index is latitude/longitude/altitude/heading (the pages say only that the index is the Data
//     Output screen's list position) and the dataref names of an aircraft's position. So THE BINDING IS DATA, NOT CODE: the user (or the wiring lane,
//     once it has read the dataref list on a real installation) says which source feeds each track field. No default index or name is built in.
//   * ForeFlight's XGPS/XATT/XTRAFFIC simulator protocol (a ready-made position/traffic format) could not be opened (HTTP 403): not built.
//   * MSFS: its SimConnect needs the Microsoft SDK, a download: not allowed, not built.
//
// BOUNDS: a datagram of at most 1500 bytes, 41 DATA records or 187 RREF records; at most 64 slots; at most 64 tracks (the least recently seen goes);
// tracks older than maxAgeSec are removed by ageOut(). Latitude must be within +-90, longitude within +-180, altitude finite and within +-100000 m;
// a value that is not finite or out of range is dropped, never stored.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace cascade::core::xplane {

inline constexpr std::size_t kMaxDatagram = 1500;
inline constexpr std::size_t kDataRecordBytes = 36;
inline constexpr std::size_t kRrefRecordBytes = 8;
inline constexpr std::size_t kMaxSlots = 64;

enum class PacketKind { None, Data, Rref };

struct DataRecord { std::int32_t index = 0; float v[8] = {}; };
struct RrefRecord { std::int32_t index = 0; float value = 0; };

struct Packet {
    PacketKind kind = PacketKind::None;
    std::vector<DataRecord> data;
    std::vector<RrefRecord> rref;
    std::string error;   // a sentence when kind == None
};

inline float readF32(const std::uint8_t* p) {
    std::uint32_t u = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline std::int32_t readI32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24));
}

// Never reads outside [data, data+len). A datagram that does not fit either layout exactly is refused with a sentence.
inline Packet parsePacket(const std::uint8_t* data, std::size_t len) {
    Packet p;
    if (data == nullptr || len < 5) { p.error = "too short to carry a header"; return p; }
    if (len > kMaxDatagram) { p.error = "longer than 1500 bytes"; return p; }
    const std::size_t body = len - 5;
    if (std::memcmp(data, "DATA", 4) == 0) {
        if (body == 0 || body % kDataRecordBytes != 0) { p.error = "a DATA body is not a whole number of 36-byte records"; return p; }
        for (std::size_t o = 5; o + kDataRecordBytes <= len; o += kDataRecordBytes) {
            DataRecord r;
            r.index = readI32(data + o);
            for (int k = 0; k < 8; ++k) { r.v[k] = readF32(data + o + 4 + 4 * k); }
            p.data.push_back(r);
        }
        p.kind = PacketKind::Data;
        return p;
    }
    if (std::memcmp(data, "RREF", 4) == 0) {
        if (body == 0 || body % kRrefRecordBytes != 0) { p.error = "an RREF body is not a whole number of 8-byte records"; return p; }
        for (std::size_t o = 5; o + kRrefRecordBytes <= len; o += kRrefRecordBytes) {
            RrefRecord r;
            r.index = readI32(data + o);
            r.value = readF32(data + o + 4);
            p.rref.push_back(r);
        }
        p.kind = PacketKind::Rref;
        return p;
    }
    p.error = "the header is neither DATA nor RREF";
    return p;
}

// ---- binding ---------------------------------------------------------------------------------------------------------------------------

enum class SrcKind { None, Data, Rref };

struct Source {
    SrcKind kind = SrcKind::None;
    int index = 0;      // DATA: the item index; RREF: the index the requester chose
    int column = 0;     // DATA only: 0..7
    double scale = 1.0; // value x scale + offset -> degrees / metres / metres per second
    double offset = 0.0;
};

struct SlotBinding {
    std::string id;     // what the map shows for this aircraft (the user's label; not read from the sim)
    Source lat, lon, alt, heading, speed;
};

struct FlightTrack {
    std::string id;
    double latDeg = 0, lonDeg = 0;
    double altM = std::nan("");
    double headingDeg = std::nan("");    // 0..360
    double speedMps = std::nan("");
    std::int64_t seenUnix = 0;
    std::uint64_t updates = 0;
    double altFeet() const { return altM * 3.280839895; }
};

struct FeedStats {
    std::uint64_t datagrams = 0, refused = 0, records = 0, dropped = 0, trackUpdates = 0;
};

class XplaneFeed {
public:
    explicit XplaneFeed(std::size_t maxTracks = 64, std::int64_t maxAgeSec = 60) : maxTracks_(std::max<std::size_t>(1, std::min<std::size_t>(maxTracks, 1024))), maxAge_(std::max<std::int64_t>(1, maxAgeSec)) {}

    // Replaces the binding (and forgets the tracks). False and a sentence when it has too many slots or a slot has an unusable source.
    bool setBinding(const std::vector<SlotBinding>& slots, std::string& error) {
        if (slots.size() > kMaxSlots) { error = "more than 64 slots"; return false; }
        for (const SlotBinding& s : slots) {
            for (const Source* src : {&s.lat, &s.lon, &s.alt, &s.heading, &s.speed}) {
                if (src->kind == SrcKind::None) { continue; }
                if (src->column < 0 || src->column > 7) { error = "a DATA column must be 0..7"; return false; }
                if (!std::isfinite(src->scale) || !std::isfinite(src->offset)) { error = "a scale or offset is not a number"; return false; }
            }
            if (s.lat.kind == SrcKind::None || s.lon.kind == SrcKind::None) { error = "every slot needs a latitude and a longitude source"; return false; }
            if (s.id.empty() || s.id.size() > 24) { error = "a slot id must be 1 to 24 characters"; return false; }
        }
        slots_ = slots;
        values_.clear();
        tracks_.clear();
        error.clear();
        return true;
    }

    // One datagram. Returns the number of track updates it caused.
    int feed(const std::uint8_t* data, std::size_t len, std::int64_t nowUnix) {
        ++stats_.datagrams;
        const Packet p = parsePacket(data, len);
        if (p.kind == PacketKind::None) { ++stats_.refused; return 0; }
        std::vector<bool> touched(slots_.size(), false);
        auto note = [&](SrcKind k, int index, int column, float v) {
            ++stats_.records;
            bool bound = false;
            for (std::size_t si = 0; si < slots_.size(); ++si) {
                for (const Source* s : {&slots_[si].lat, &slots_[si].lon, &slots_[si].alt, &slots_[si].heading, &slots_[si].speed}) {
                    if (s->kind == k && s->index == index && (k == SrcKind::Rref || s->column == column)) { touched[si] = true; bound = true; }
                }
            }
            // only values some slot reads are kept: the store cannot grow from what a sender invents
            if (bound) { values_[Key{k == SrcKind::Data ? 0 : 1, index, k == SrcKind::Data ? column : 0}] = v; }
        };
        if (p.kind == PacketKind::Data) {
            for (const DataRecord& r : p.data) { for (int c = 0; c < 8; ++c) { note(SrcKind::Data, r.index, c, r.v[c]); } }
        } else {
            for (const RrefRecord& r : p.rref) { note(SrcKind::Rref, r.index, 0, r.value); }
        }
        int updates = 0;
        for (std::size_t si = 0; si < slots_.size(); ++si) {
            if (touched[si] && publish(slots_[si], nowUnix)) { ++updates; }
        }
        stats_.trackUpdates += static_cast<std::uint64_t>(updates);
        return updates;
    }

    // Removes tracks not updated for maxAgeSec; returns how many went.
    std::size_t ageOut(std::int64_t nowUnix) {
        const std::size_t before = tracks_.size();
        for (auto it = tracks_.begin(); it != tracks_.end();) {
            if (nowUnix - it->second.seenUnix > maxAge_) { it = tracks_.erase(it); } else { ++it; }
        }
        return before - tracks_.size();
    }

    std::vector<FlightTrack> tracks() const {
        std::vector<FlightTrack> v;
        for (const auto& kv : tracks_) { v.push_back(kv.second); }
        return v;
    }
    std::size_t trackCount() const { return tracks_.size(); }
    const FeedStats& stats() const { return stats_; }

private:
    using Key = std::tuple<int, int, int>;

    bool value(const Source& s, double& out) const {
        if (s.kind == SrcKind::None) { return false; }
        const auto it = values_.find(Key{s.kind == SrcKind::Data ? 0 : 1, s.index, s.kind == SrcKind::Data ? s.column : 0});
        if (it == values_.end() || !std::isfinite(it->second)) { return false; }
        out = static_cast<double>(it->second) * s.scale + s.offset;
        return std::isfinite(out);
    }

    bool publish(const SlotBinding& b, std::int64_t now) {
        double lat = 0, lon = 0;
        if (!value(b.lat, lat) || !value(b.lon, lon)) { return false; }
        if (!(lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0)) { return false; }
        FlightTrack t;
        const auto prev = tracks_.find(b.id);
        if (prev != tracks_.end()) { t = prev->second; }
        t.id = b.id;
        t.latDeg = lat;
        t.lonDeg = lon;
        double v = 0;
        if (value(b.alt, v) && v >= -100000.0 && v <= 100000.0) { t.altM = v; }
        if (value(b.heading, v)) { v = std::fmod(v, 360.0); if (v < 0) { v += 360.0; } t.headingDeg = v; }
        if (value(b.speed, v) && v >= 0.0 && v <= 1.0e5) { t.speedMps = v; }
        t.seenUnix = now;
        ++t.updates;
        if (prev == tracks_.end() && tracks_.size() >= maxTracks_) {
            auto oldest = std::min_element(tracks_.begin(), tracks_.end(), [](const auto& a, const auto& c) { return a.second.seenUnix < c.second.seenUnix; });
            tracks_.erase(oldest);
            ++stats_.dropped;
        }
        tracks_[b.id] = t;
        return true;
    }

    std::size_t maxTracks_;
    std::int64_t maxAge_;
    std::vector<SlotBinding> slots_;
    std::map<Key, float> values_;
    std::map<std::string, FlightTrack> tracks_;
    FeedStats stats_;
};

}  // namespace cascade::core::xplane
