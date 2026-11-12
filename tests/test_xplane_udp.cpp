// test_xplane_udp.cpp - the X-Plane UDP receive side (core/xplane_udp.hpp) and its loopback listener (net/xplane_listener.hpp).
// Every datagram is built here from the documented layouts; no simulator is involved. The data indexes used in the bindings below are ARBITRARY test
// numbers: which real DATA index or dataref means latitude is not documented in any page opened (see the header).
//
//   X1  the documented layouts, byte for byte: DATA (36-byte records), RREF (8-byte records).
//   X2  refusals: too short, wrong header, a body that is not whole records, empty body, too long, null.
//   X3  the binding: tracks appear when latitude and longitude are both known; partial updates; several slots; scale/offset (feet to metres).
//   X4  validation: out-of-range, NaN, infinity are dropped; heading wraps; speed and altitude limits.
//   X5  binding errors; a new binding forgets the tracks.
//   X6  bounds: at most N tracks (the least recent goes), age-out at its exact boundary, nothing stored for sources nobody binds.
//   X7  the listener on 127.0.0.1: OFF by default, wider addresses refused, datagrams reach the feed, oversized and garbage are counted, poll is bounded.
//   X8  fuzz: random and mutated datagrams; the track invariants hold.
//   X9  source scan: the listener never sends, has no thread, prints nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "core/xplane_udp.hpp"
#include "hamops_util.hpp"
#include "net/xplane_listener.hpp"
#include "test_check.hpp"

using namespace cascade::core::xplane;
using Bytes = std::vector<std::uint8_t>;

namespace {

void putF(Bytes& b, float f) { std::uint32_t u; std::memcpy(&u, &f, 4); for (int i = 0; i < 4; ++i) { b.push_back(static_cast<std::uint8_t>(u >> (8 * i))); } }
void putI(Bytes& b, std::int32_t v) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * i))); } }

Bytes dataPacket(const std::vector<std::pair<int, std::vector<float>>>& recs) {
    Bytes b = {'D', 'A', 'T', 'A', '*'};
    for (const auto& r : recs) {
        putI(b, r.first);
        for (int k = 0; k < 8; ++k) { putF(b, k < static_cast<int>(r.second.size()) ? r.second[static_cast<std::size_t>(k)] : 0.0f); }
    }
    return b;
}
Bytes rrefPacket(const std::vector<std::pair<int, float>>& recs) {
    Bytes b = {'R', 'R', 'E', 'F', 0};
    for (const auto& r : recs) { putI(b, r.first); putF(b, r.second); }
    return b;
}

Source dsrc(int index, int column, double scale = 1.0, double offset = 0.0) { Source s; s.kind = SrcKind::Data; s.index = index; s.column = column; s.scale = scale; s.offset = offset; return s; }
Source rsrc(int index, double scale = 1.0) { Source s; s.kind = SrcKind::Rref; s.index = index; s.scale = scale; return s; }

SlotBinding ownship() {
    SlotBinding b;
    b.id = "OWN";
    b.lat = dsrc(20, 0);
    b.lon = dsrc(21, 0);
    b.alt = dsrc(20, 2, 0.3048);      // the sim says feet, the track is in metres
    b.heading = dsrc(17, 2);
    b.speed = dsrc(3, 0, 0.514444);   // knots to m/s
    return b;
}

void x1Layouts() {
    // DATA: header "DATA" + one byte, then index 3 and eight floats 1.0, -2.5, 90.0, 0, 0, 0, 0, 0 (hand-written little-endian bytes)
    const Bytes golden = {'D', 'A', 'T', 'A', 0x2A, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x20, 0xC0, 0x00, 0x00, 0xB4, 0x42,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(golden.size() == 5 + 36);
    Packet p = parsePacket(golden.data(), golden.size());
    CHECK(p.kind == PacketKind::Data && p.data.size() == 1 && p.rref.empty());
    CHECK(p.data[0].index == 3 && p.data[0].v[0] == 1.0f && p.data[0].v[1] == -2.5f && p.data[0].v[2] == 90.0f && p.data[0].v[3] == 0.0f);
    // RREF: index 7 and value 51.5
    const Bytes rg = {'R', 'R', 'E', 'F', 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x42};
    p = parsePacket(rg.data(), rg.size());
    CHECK(p.kind == PacketKind::Rref && p.rref.size() == 1 && p.rref[0].index == 7 && p.rref[0].value == 51.5f);
    // several records, negative index, builder agrees with the hand bytes
    CHECK(dataPacket({{3, {1.0f, -2.5f, 90.0f}}}) == [&] { Bytes g = golden; g[4] = '*'; return g; }());
    p = parsePacket(rrefPacket({{1, 1.0f}, {-5, -2.0f}, {2147483647, 3.0f}}).data(), 5 + 24);
    CHECK(p.kind == PacketKind::Rref && p.rref.size() == 3 && p.rref[1].index == -5 && p.rref[2].index == 2147483647);
    // 41 DATA records fit in 1500 bytes
    std::vector<std::pair<int, std::vector<float>>> many;
    for (int i = 0; i < 41; ++i) { many.push_back({i, {static_cast<float>(i)}}); }
    const Bytes b41 = dataPacket(many);
    CHECK(b41.size() == 5 + 41 * 36 && b41.size() <= kMaxDatagram);
    p = parsePacket(b41.data(), b41.size());
    CHECK(p.kind == PacketKind::Data && p.data.size() == 41 && p.data[40].index == 40 && p.data[40].v[0] == 40.0f);
}

void x2Refusals() {
    auto refused = [](const Bytes& b) { const Packet p = parsePacket(b.data(), b.size()); return p.kind == PacketKind::None && !p.error.empty() && p.data.empty() && p.rref.empty(); };
    CHECK(refused({}));
    CHECK(refused({'D', 'A', 'T', 'A'}));
    CHECK(refused({'D', 'A', 'T', 'A', '*'}));                         // no body
    CHECK(refused({'R', 'R', 'E', 'F', 0}));
    CHECK(refused({'X', 'X', 'X', 'X', 0, 1, 2, 3, 4, 5, 6, 7, 8}));
    CHECK(refused({'d', 'a', 't', 'a', 0, 1, 2, 3, 4, 5, 6, 7, 8}));   // case matters
    Bytes odd = dataPacket({{1, {1.0f}}});
    odd.pop_back();
    CHECK(refused(odd));
    odd = rrefPacket({{1, 1.0f}});
    odd.push_back(0);
    CHECK(refused(odd));
    Bytes big = dataPacket({{1, {1.0f}}});
    big.resize(kMaxDatagram + 1);
    CHECK(refused(big));
    CHECK(parsePacket(nullptr, 20).kind == PacketKind::None);
    // exactly 1500 bytes of RREF records is 5 + 8n: 1500-5 = 1495 is not a multiple of 8; 5 + 8*186 = 1493 is
    Bytes r186(5 + 8 * 186, 0);
    std::memcpy(r186.data(), "RREF", 4);
    CHECK(parsePacket(r186.data(), r186.size()).rref.size() == 186);
    Bytes d42 = dataPacket({});
    for (int i = 0; i < 42; ++i) { for (int k = 0; k < 36; ++k) { d42.push_back(0); } }
    CHECK(refused(d42));   // 5 + 42 x 36 = 1517 > 1500
    // NaN and infinity are parsed as values (the feed drops them): the parser does not hide them
    Bytes nanp = rrefPacket({{1, std::numeric_limits<float>::quiet_NaN()}});
    Packet p = parsePacket(nanp.data(), nanp.size());
    CHECK(p.kind == PacketKind::Rref && std::isnan(p.rref[0].value));
}

void x3Tracks() {
    XplaneFeed f;
    std::string err;
    CHECK(f.setBinding({ownship()}, err));
    // latitude alone: no track
    Bytes b = dataPacket({{20, {51.5f}}});
    CHECK(f.feed(b.data(), b.size(), 1000) == 0 && f.trackCount() == 0);
    // longitude arrives (same index/record, other column in a second datagram): now there is one
    b = dataPacket({{20, {51.5f, 0.0f, 10000.0f}}, {21, {-0.25f}}, {17, {0, 0, 450.0f}}, {3, {100.0f}}});
    CHECK(f.feed(b.data(), b.size(), 1001) == 1 && f.trackCount() == 1);
    FlightTrack t = f.tracks()[0];
    CHECK(t.id == "OWN" && t.latDeg == 51.5 && t.lonDeg == -0.25 && t.seenUnix == 1001 && t.updates == 1);
    CHECK(std::fabs(t.altM - 3048.0) < 1e-3 && std::fabs(t.altFeet() - 10000.0) < 1e-2);
    CHECK(std::fabs(t.headingDeg - 90.0) < 1e-9);                       // 450 wraps to 90
    CHECK(std::fabs(t.speedMps - 51.4444) < 1e-3);
    // a datagram that touches only the heading keeps the position and updates the heading and the time
    b = dataPacket({{17, {0, 0, 180.0f}}});
    CHECK(f.feed(b.data(), b.size(), 1002) == 1);
    t = f.tracks()[0];
    CHECK(t.latDeg == 51.5 && t.headingDeg == 180.0 && t.seenUnix == 1002 && t.updates == 2);
    // a datagram that touches nothing bound: no update
    b = dataPacket({{99, {1, 2, 3}}});
    CHECK(f.feed(b.data(), b.size(), 1003) == 0 && f.tracks()[0].seenUnix == 1002);
    // RREF traffic slots
    std::vector<SlotBinding> slots;
    for (int i = 0; i < 3; ++i) {
        SlotBinding s;
        s.id = "AI" + std::to_string(i + 1);
        s.lat = rsrc(100 + 10 * i);
        s.lon = rsrc(101 + 10 * i);
        s.alt = rsrc(102 + 10 * i);
        s.heading = rsrc(103 + 10 * i);
        slots.push_back(s);
    }
    CHECK(f.setBinding(slots, err));
    CHECK(f.trackCount() == 0);   // a new binding forgets the tracks
    const std::uint64_t dgBefore = f.stats().datagrams, updBefore = f.stats().trackUpdates;
    b = rrefPacket({{100, 40.0f}, {101, -3.0f}, {102, 5000.0f}, {103, 270.0f}, {110, -33.9f}, {111, 151.2f}, {120, 10.0f}});
    CHECK(f.feed(b.data(), b.size(), 2000) == 2);   // AI1 and AI2 are complete; AI3 has only a latitude
    CHECK(f.trackCount() == 2);
    b = rrefPacket({{121, 20.0f}});
    CHECK(f.feed(b.data(), b.size(), 2001) == 1 && f.trackCount() == 3);
    auto ts = f.tracks();
    CHECK(ts.size() == 3);
    for (const FlightTrack& x : ts) { CHECK(x.id == "AI1" || x.id == "AI2" || x.id == "AI3"); }
    CHECK(f.stats().datagrams - dgBefore == 2 && f.stats().trackUpdates - updBefore == 3 && f.stats().refused == 0);
    // the same RREF index feeding two slots updates both
    SlotBinding a, c;
    a.id = "A"; a.lat = rsrc(1); a.lon = rsrc(2);
    c.id = "C"; c.lat = rsrc(1); c.lon = rsrc(2);
    CHECK(f.setBinding({a, c}, err));
    b = rrefPacket({{1, 10.0f}, {2, 20.0f}});
    CHECK(f.feed(b.data(), b.size(), 1) == 2 && f.trackCount() == 2);
}

void x4Validation() {
    XplaneFeed f;
    std::string err;
    CHECK(f.setBinding({ownship()}, err));
    auto feedLL = [&](float lat, float lon, std::int64_t t) {
        const Bytes b = dataPacket({{20, {lat}}, {21, {lon}}});
        return f.feed(b.data(), b.size(), t);
    };
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    CHECK(feedLL(91.0f, 0.0f, 1) == 0);
    CHECK(feedLL(-91.0f, 0.0f, 1) == 0);
    CHECK(feedLL(0.0f, 181.0f, 1) == 0);
    CHECK(feedLL(0.0f, -181.0f, 1) == 0);
    CHECK(feedLL(nan, 0.0f, 1) == 0);
    CHECK(feedLL(0.0f, nan, 1) == 0);
    CHECK(feedLL(inf, 0.0f, 1) == 0);
    CHECK(feedLL(0.0f, -inf, 1) == 0);
    CHECK(f.trackCount() == 0);
    CHECK(feedLL(90.0f, 180.0f, 2) == 1);                       // the edges are valid
    CHECK(feedLL(-90.0f, -180.0f, 3) == 1);
    CHECK(f.tracks()[0].latDeg == -90.0 && f.tracks()[0].lonDeg == -180.0);
    // a bad position after a good one does not move the track
    CHECK(feedLL(95.0f, 0.0f, 4) == 0);
    CHECK(f.tracks()[0].latDeg == -90.0 && f.tracks()[0].seenUnix == 3);
    // altitude, heading, speed limits
    Bytes b = dataPacket({{20, {10.0f, 0.0f, 1.0e9f}}, {21, {10.0f}}, {17, {0, 0, -90.0f}}, {3, {-5.0f}}});
    CHECK(f.feed(b.data(), b.size(), 5) == 1);
    FlightTrack t = f.tracks()[0];
    CHECK(t.altM == 0.0);                                      // 1e9 ft is not an altitude: the earlier value (0 m) stays
    CHECK(std::fabs(t.headingDeg - 270.0) < 1e-9);             // -90 wraps to 270
    CHECK(std::isnan(t.speedMps));                              // a negative speed is dropped
    b = dataPacket({{20, {10.0f, 0.0f, nan}}, {21, {10.0f}}, {17, {0, 0, nan}}});
    CHECK(f.feed(b.data(), b.size(), 6) == 1);
    t = f.tracks()[0];
    CHECK(std::fabs(t.headingDeg - 270.0) < 1e-9);             // NaN heading keeps the previous one
    // a scale that overflows to infinity is dropped, not stored
    SlotBinding s = ownship();
    s.alt = dsrc(20, 2, 1.0e300);
    XplaneFeed g;
    CHECK(g.setBinding({s}, err));
    b = dataPacket({{20, {10.0f, 0.0f, 1.0e30f}}, {21, {10.0f}}});
    CHECK(g.feed(b.data(), b.size(), 1) == 1 && std::isnan(g.tracks()[0].altM));
}

void x5BindingErrors() {
    XplaneFeed f;
    std::string err;
    CHECK(f.setBinding({ownship()}, err));
    CHECK(f.setBinding({}, err) && f.trackCount() == 0);          // an empty binding is allowed: nothing is fed
    std::vector<SlotBinding> tooMany(kMaxSlots + 1, ownship());
    for (std::size_t i = 0; i < tooMany.size(); ++i) { tooMany[i].id = "S" + std::to_string(i); }
    CHECK(!f.setBinding(tooMany, err) && !err.empty());
    tooMany.pop_back();
    CHECK(f.setBinding(tooMany, err));
    SlotBinding s = ownship();
    s.lat = dsrc(20, 8);
    CHECK(!f.setBinding({s}, err));
    s = ownship(); s.lon = dsrc(20, -1);
    CHECK(!f.setBinding({s}, err));
    s = ownship(); s.lon = Source();
    CHECK(!f.setBinding({s}, err));
    s = ownship(); s.lat = Source();
    CHECK(!f.setBinding({s}, err));
    s = ownship(); s.id.clear();
    CHECK(!f.setBinding({s}, err));
    s = ownship(); s.id = std::string(25, 'x');
    CHECK(!f.setBinding({s}, err));
    s = ownship(); s.alt = dsrc(20, 2, std::numeric_limits<double>::quiet_NaN());
    CHECK(!f.setBinding({s}, err));
    s = ownship(); s.speed = dsrc(3, 0, 1.0, std::numeric_limits<double>::infinity());
    CHECK(!f.setBinding({s}, err));
    // before any binding nothing is fed and nothing breaks
    XplaneFeed none;
    const Bytes b = dataPacket({{20, {1.0f}}, {21, {2.0f}}});
    CHECK(none.feed(b.data(), b.size(), 1) == 0 && none.trackCount() == 0);
    const Bytes junk = {1, 2, 3};
    CHECK(none.feed(junk.data(), junk.size(), 1) == 0 && none.stats().refused == 1 && none.stats().datagrams == 2);
}

void x6Bounds() {
    XplaneFeed f(4, 100);
    std::string err;
    std::vector<SlotBinding> slots;
    for (int i = 0; i < 10; ++i) {
        SlotBinding s;
        s.id = "T" + std::to_string(i);
        s.lat = rsrc(10 * i);
        s.lon = rsrc(10 * i + 1);
        slots.push_back(s);
    }
    CHECK(f.setBinding(slots, err));
    for (int i = 0; i < 10; ++i) {
        const Bytes b = rrefPacket({{10 * i, 10.0f + static_cast<float>(i)}, {10 * i + 1, 20.0f}});
        CHECK(f.feed(b.data(), b.size(), 1000 + i) == 1);
        CHECK(f.trackCount() <= 4);
    }
    CHECK(f.trackCount() == 4 && f.stats().dropped == 6);
    std::vector<std::string> ids;
    for (const FlightTrack& t : f.tracks()) { ids.push_back(t.id); }
    std::sort(ids.begin(), ids.end());
    CHECK(ids == (std::vector<std::string>{"T6", "T7", "T8", "T9"}));      // the four most recent
    // age-out at the exact boundary: older than 100 s goes
    CHECK(f.ageOut(1106) == 0);          // 1106 - 1006 = 100: kept
    CHECK(f.ageOut(1107) == 1);          // T6 is 101 s old
    CHECK(f.trackCount() == 3);
    CHECK(f.ageOut(5000) == 3 && f.trackCount() == 0);
    CHECK(f.ageOut(0) == 0);
    // a flood of datagrams for sources nobody binds changes nothing
    const std::uint64_t upd = f.stats().trackUpdates;
    for (int i = 0; i < 2000; ++i) {
        const Bytes b = rrefPacket({{100000 + i, 1.0f}});
        f.feed(b.data(), b.size(), 6000);
    }
    CHECK(f.trackCount() == 0 && f.stats().trackUpdates == upd);
}

void x7Listener() {
    using namespace cascade::net;
    XplaneListener L;
    std::string err;
    CHECK(!XplaneListenerConfig().enabled);
    CHECK(!L.start(XplaneListenerConfig(), err) && !err.empty());
    XplaneListenerConfig c;
    c.enabled = true;
    c.host = "0.0.0.0";
    CHECK(!L.start(c, err));
    c.host = "192.168.1.5";
    CHECK(!L.start(c, err));
    c.host = "localhost";
    CHECK(!L.start(c, err));
    c.host = "127.0.0.1";
    c.port = 70000;
    CHECK(!L.start(c, err));
    CHECK(!L.running());
    XplaneFeed feed;
    CHECK(L.poll(feed, 1) == 0);                                     // not running
    c.port = 0;
    CHECK(L.start(c, err) && L.running() && L.port() > 0);
    SlotBinding own = ownship();
    CHECK(feed.setBinding({own}, err));
    const lo::Sock tx = lo::udpOpen(lo::kLoopbackHost, -1);
    CHECK(tx != lo::kBad);
    const Bytes good = dataPacket({{20, {10.0f, 0.0f, 100.0f}}, {21, {20.0f}}});
    CHECK(lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), good.data(), good.size()) == static_cast<int>(good.size()));
    int got = 0;
    for (int i = 0; i < 500 && got == 0; ++i) { got = L.poll(feed, 77); if (got == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); } }
    CHECK(got == 1 && feed.trackCount() == 1 && feed.tracks()[0].seenUnix == 77);
    // garbage and oversized datagrams are counted and do not stop the next good one
    const Bytes garbage = {1, 2, 3, 4, 5, 6, 7};
    lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), garbage.data(), garbage.size());
    Bytes huge(1600, 'D');
    lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), huge.data(), huge.size());
    lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), good.data(), good.size());
    int total = 0;
    for (int i = 0; i < 500 && total < 3; ++i) { total += L.poll(feed, 78); if (total < 3) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); } }
    CHECK(total == 3);
    CHECK(L.oversized() == 1);
    CHECK(feed.stats().refused == 1 && feed.stats().datagrams == 3 && feed.tracks()[0].seenUnix == 78);
    // poll is bounded
    for (int i = 0; i < 10; ++i) { lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), good.data(), good.size()); }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(L.poll(feed, 79, 4) == 4);
    int rest = 0;
    for (int i = 0; i < 100 && rest < 6; ++i) { rest += L.poll(feed, 79, 64); if (rest < 6) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); } }
    CHECK(rest == 6);
    L.stop();
    CHECK(!L.running() && L.poll(feed, 80) == 0);
    lo::closeSock(tx);
    // a port that is taken
    XplaneListener a, b;
    XplaneListenerConfig cc;
    cc.enabled = true;
    CHECK(a.start(cc, err));
    cc.port = a.port();
    CHECK(!b.start(cc, err) && !err.empty());
}

void x8Fuzz() {
    hamops_test::Rng rng(41);
    XplaneFeed f(8, 30);
    std::string err;
    std::vector<SlotBinding> slots;
    slots.push_back(ownship());
    for (int i = 0; i < 6; ++i) {
        SlotBinding s;
        s.id = "AI" + std::to_string(i);
        s.lat = rsrc(10 * i); s.lon = rsrc(10 * i + 1); s.alt = rsrc(10 * i + 2); s.heading = rsrc(10 * i + 3); s.speed = rsrc(10 * i + 4);
        slots.push_back(s);
    }
    CHECK(f.setBinding(slots, err));
    std::int64_t now = 0;
    int updates = 0;
    for (int it = 0; it < 30000; ++it) {
        Bytes b;
        const int mode = static_cast<int>(rng.below(4));
        if (mode == 0) {
            b.resize(rng.below(300));
            for (auto& x : b) { x = static_cast<std::uint8_t>(rng.below(256)); }
        } else if (mode == 1) {
            std::vector<std::pair<int, float>> r;
            const int n = 1 + static_cast<int>(rng.below(8));
            for (int k = 0; k < n; ++k) {
                float v = static_cast<float>(rng.below(2000000)) / 1000.0f - 700.0f;
                if (rng.below(20) == 0) { v = std::numeric_limits<float>::quiet_NaN(); }
                r.push_back({static_cast<int>(rng.below(60)), v});
            }
            b = rrefPacket(r);
        } else if (mode == 2) {
            std::vector<std::pair<int, std::vector<float>>> r;
            const int n = 1 + static_cast<int>(rng.below(4));
            for (int k = 0; k < n; ++k) {
                std::vector<float> v;
                for (int c = 0; c < 8; ++c) { v.push_back(static_cast<float>(rng.below(2000000)) / 1000.0f - 700.0f); }
                static const int idx[4] = {20, 21, 17, 3};
                r.push_back({idx[rng.below(4)], v});
            }
            b = dataPacket(r);
        } else {
            b = rrefPacket({{0, 10.0f}, {1, 20.0f}});
            if (!b.empty()) { b[rng.below(static_cast<std::uint32_t>(b.size()))] = static_cast<std::uint8_t>(rng.below(256)); }
            if (rng.below(2)) { b.resize(rng.below(static_cast<std::uint32_t>(b.size()) + 1)); }
        }
        now += static_cast<std::int64_t>(rng.below(5));
        updates += f.feed(b.data(), b.size(), now);
        if (it % 50 == 0) { f.ageOut(now); }
        CHECK(f.trackCount() <= 8);
        for (const FlightTrack& t : f.tracks()) {
            CHECK(t.latDeg >= -90.0 && t.latDeg <= 90.0 && t.lonDeg >= -180.0 && t.lonDeg <= 180.0);
            CHECK(std::isnan(t.altM) || (t.altM >= -100000.0 && t.altM <= 100000.0));
            CHECK(std::isnan(t.headingDeg) || (t.headingDeg >= 0.0 && t.headingDeg < 360.0));
            CHECK(std::isnan(t.speedMps) || t.speedMps >= 0.0);
        }
    }
    CHECK(updates > 1000);
}

void x9SourceScan() {
    for (const char* file : {"src/core/xplane_udp.hpp", "src/net/xplane_listener.hpp"}) {
        const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile(file));
        CHECK(!src.empty());
        for (const char* bad : {"std::thread", "std::async", "stderr", "stdout", "std::cerr", "std::cout", "settings_set", "INADDR_ANY", "getaddrinfo", "OutputDebugString"}) {
            CHECK(src.find(bad) == std::string::npos);
        }
    }
    // receive only: the listener never sends
    const std::string l = hamops_test::stripCommentsAndStrings(hamops_test::srcFile("src/net/xplane_listener.hpp"));
    CHECK(l.find("udpSendTo") == std::string::npos && l.find("sendto") == std::string::npos && l.find("tcpSend") == std::string::npos);
    CHECK(hamops_test::srcFile("src/net/xplane_listener.hpp").find("bool enabled = false;") != std::string::npos);
}

}  // namespace

int main() {
    x1Layouts();
    x2Refusals();
    x3Tracks();
    x4Validation();
    x5BindingErrors();
    x6Bounds();
    x7Listener();
    x8Fuzz();
    x9SourceScan();
    return testSummary("test_xplane_udp");
}
