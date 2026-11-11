// Tests for net/aircraft_out.hpp: the Beast binary and SBS text encoders against hand-built bytes and lines, a round trip through parsers
// written here independently of the encoders, and the two servers over real loopback sockets.
//
// The Beast expectations come from the format page cited in aircraft_out.hpp (its worked example is reproduced byte for byte). The SBS
// expectations are hand-built from the 22-field layout; that layout is UNVERIFIED against the primary page (see the header).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/aircraft_out.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::Client;

namespace {

using Bytes = std::vector<std::uint8_t>;

// ---- an independent Beast parser (a streaming state machine; shares nothing with the encoder) --------------------------------------
struct Parsed {
    char type = 0;
    std::uint64_t ts = 0;
    std::uint8_t sig = 0;
    Bytes data;
};

struct BeastParser {
    enum State { Idle, Type, Body, Esc } st = Idle;
    char type = 0;
    std::size_t need = 0;
    Bytes body;
    std::vector<Parsed> out;
    std::size_t garbage = 0;

    static std::size_t dataLen(char t) { return t == '1' ? 2 : (t == '2' ? 7 : 14); }
    void startFrame(std::uint8_t t) {
        if (t == '1' || t == '2' || t == '3') {
            type = static_cast<char>(t);
            need = 7 + dataLen(type);
            body.clear();
            st = Body;
        } else {
            st = Idle;
        }
    }
    void finish() {
        Parsed p;
        p.type = type;
        for (int i = 0; i < 6; ++i) { p.ts = (p.ts << 8) | body[static_cast<std::size_t>(i)]; }
        p.sig = body[6];
        p.data.assign(body.begin() + 7, body.end());
        out.push_back(std::move(p));
        st = Idle;
    }
    void feed(const Bytes& b) {
        for (std::uint8_t c : b) {
            switch (st) {
                case Idle:
                    if (c == 0x1A) { st = Type; } else { ++garbage; }
                    break;
                case Type:
                    startFrame(c);
                    break;
                case Body:
                    if (c == 0x1A) { st = Esc; } else {
                        body.push_back(c);
                        if (body.size() == need) { finish(); }
                    }
                    break;
                case Esc:
                    if (c == 0x1A) {
                        body.push_back(0x1A);
                        st = Body;
                        if (body.size() == need) { finish(); }
                    } else {
                        startFrame(c);  // a lone marker: the frame was cut short and a new one starts
                    }
                    break;
            }
        }
    }
};

Bytes enc(const AircraftFrame& f) {
    std::uint8_t buf[kBeastMaxFrameBytes];
    const std::size_t n = beastEncode(f, buf, sizeof(buf));
    return Bytes(buf, buf + n);
}

AircraftFrame frame(std::initializer_list<std::uint8_t> raw, std::uint64_t ts, std::uint8_t level) {
    AircraftFrame f;
    f.rawLen = raw.size();
    std::size_t i = 0;
    for (std::uint8_t b : raw) { f.raw[i++] = b; }
    f.timestamp12MHz = ts;
    f.level = level;
    return f;
}

void testBeastHandBuilt() {
    // THE FORMAT PAGE'S WORKED EXAMPLE, byte for byte: Mode S short, timestamp 9063047285610 (08 3e 27 b6 cb 6a), signal 0x1a (doubled),
    // data 00 a1 84 1a c3 b3 1d (the 0x1a doubled).
    const AircraftFrame ex = frame({0x00, 0xa1, 0x84, 0x1a, 0xc3, 0xb3, 0x1d}, 9063047285610ull, 0x1a);
    const Bytes want = {0x1a, 0x32, 0x08, 0x3e, 0x27, 0xb6, 0xcb, 0x6a, 0x1a, 0x1a, 0x00, 0xa1, 0x84, 0x1a, 0x1a, 0xc3, 0xb3, 0x1d};
    CHECK(enc(ex) == want);

    // Mode A/C: '1', two data bytes
    const Bytes ac = {0x1a, 0x31, 0, 0, 0, 0, 0, 1, 0x80, 0x12, 0x34};
    CHECK(enc(frame({0x12, 0x34}, 1, 0x80)) == ac);

    // Mode S long: '3', fourteen bytes (a DF17 identification message)
    const Bytes lg = {0x1a, 0x33, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0x2C, 0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98};
    CHECK(enc(frame({0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98}, 0x0123456789ABull, 0x2C)) == lg);
    CHECK(lg.size() == 23);

    // escaping in every part at once: timestamp bytes, signal and data all 0x1a -> 2 + 2 x 21 = 44 bytes
    AircraftFrame all;
    all.rawLen = 14;
    std::memset(all.raw, 0x1a, 14);
    all.timestamp12MHz = 0x1a1a1a1a1a1aull;
    all.level = 0x1a;
    const Bytes e = enc(all);
    CHECK(e.size() == kBeastMaxFrameBytes && e.size() == 44);
    CHECK(e[0] == 0x1a && e[1] == '3');
    for (std::size_t i = 2; i < e.size(); ++i) { CHECK(e[i] == 0x1a); }  // nothing but doubled markers after the header
    // the marker is the only unpaired 0x1a: walking the escaped part in pairs never splits a pair
    // timestamp above 48 bits is cut to 48 (the wire has six bytes)
    CHECK(enc(frame({1, 2}, 0xFFFF000000000002ull, 0)) == (Bytes{0x1a, 0x31, 0, 0, 0, 0, 0, 2, 0, 1, 2}));
}

void testBeastRefusals() {
    std::uint8_t buf[kBeastMaxFrameBytes];
    AircraftFrame f;
    for (std::size_t len : {0u, 1u, 3u, 6u, 8u, 13u, 15u, 100u}) {
        f.rawLen = len;
        CHECK(beastEncode(f, buf, sizeof(buf)) == 0);  // only 2, 7 and 14 are frames
    }
    f.rawLen = 7;
    CHECK(beastEncode(f, buf, kBeastMaxFrameBytes - 1) == 0);  // a buffer that could not hold the worst case is refused, never overrun
    CHECK(beastEncode(f, buf, 0) == 0);
    CHECK(beastEncode(f, buf, kBeastMaxFrameBytes) == 16);
}

void testBeastHelpers() {
    CHECK(beastTimestampFromSeconds(0.0) == 0);
    CHECK(beastTimestampFromSeconds(1.0) == 12000000ull);
    CHECK(beastTimestampFromSeconds(0.5) == 6000000ull);
    CHECK(beastTimestampFromSeconds(-1.0) == 0);
    CHECK(beastTimestampFromSeconds(std::nan("")) == 0);
    CHECK(beastTimestampFromSeconds(INFINITY) == 0);
    // wraps at 2^48 ticks = 23456248.0592 s
    const double wrap = 281474976710656.0 / 12.0e6;
    const std::uint64_t t = beastTimestampFromSeconds(wrap + 2.0);
    CHECK(t >= 23999000ull && t <= 24001000ull);
    CHECK(t < (1ull << 48));
    CHECK(beastSignalFromAmplitude(0.1) == 26);  // the format page: 0x1a is "about 10 %"
    CHECK(beastSignalFromAmplitude(0.99) == 252);  // 0.99 x 255 = 252.45: pins the scale (256 would give 253)
    CHECK(beastSignalFromAmplitude(0.5) == 128);
    CHECK(beastSignalFromAmplitude(0.0) == 0);
    CHECK(beastSignalFromAmplitude(1.0) == 255);
    CHECK(beastSignalFromAmplitude(7.0) == 255);
    CHECK(beastSignalFromAmplitude(-0.3) == 0);
    CHECK(beastSignalFromAmplitude(std::nan("")) == 0);
}

void testBeastRoundTrip() {
    std::mt19937_64 rng(12345);
    std::vector<AircraftFrame> frames;
    Bytes stream;
    const std::size_t lens[3] = {2, 7, 14};
    for (int i = 0; i < 20000; ++i) {
        AircraftFrame f;
        f.rawLen = lens[rng() % 3];
        const bool heavy = (rng() % 4) == 0;  // a quarter of the frames are full of 0x1a
        for (std::size_t k = 0; k < f.rawLen; ++k) { f.raw[k] = (heavy && rng() % 2) ? 0x1a : static_cast<std::uint8_t>(rng()); }
        f.timestamp12MHz = (heavy && rng() % 2) ? 0x1a1a1a1a1a1aull : (rng() & 0xFFFFFFFFFFFFull);
        f.level = (heavy && rng() % 2) ? 0x1a : static_cast<std::uint8_t>(rng());
        frames.push_back(f);
        const Bytes e = enc(f);
        CHECK(!e.empty());
        stream.insert(stream.end(), e.begin(), e.end());
    }
    BeastParser p;
    // feed in odd-sized chunks, as TCP would
    for (std::size_t off = 0; off < stream.size();) {
        const std::size_t n = std::min<std::size_t>(1 + rng() % 97, stream.size() - off);
        p.feed(Bytes(stream.begin() + static_cast<std::ptrdiff_t>(off), stream.begin() + static_cast<std::ptrdiff_t>(off + n)));
        off += n;
    }
    CHECK(p.out.size() == frames.size());
    std::size_t bad = 0;
    for (std::size_t i = 0; i < frames.size() && i < p.out.size(); ++i) {
        const AircraftFrame& f = frames[i];
        const Parsed& q = p.out[i];
        const char t = f.rawLen == 2 ? '1' : (f.rawLen == 7 ? '2' : '3');
        if (q.type != t || q.ts != f.timestamp12MHz || q.sig != f.level || q.data.size() != f.rawLen ||
            std::memcmp(q.data.data(), f.raw, f.rawLen) != 0) {
            ++bad;
        }
    }
    CHECK(bad == 0);
    CHECK(p.garbage == 0);
    std::printf("beast round trip: %zu frames, %zu stream bytes, %zu mismatches\n", frames.size(), stream.size(), bad);

    // resync: a stream cut mid-frame followed by a whole frame is recovered by the parser (the encoder side can't produce the cut; the
    // cut stands for a client that connected mid-frame)
    const Bytes whole = enc(frame({1, 2, 3, 4, 5, 6, 7}, 77, 5));
    Bytes cut = {0x07, 0x1a, 0x1a, 0x03};  // the tail of a frame, with an escaped 0x1a in it
    cut.insert(cut.end(), whole.begin(), whole.end());
    BeastParser r;
    r.feed(cut);
    CHECK(r.out.size() == 1 && r.out[0].ts == 77 && r.out[0].data.size() == 7);
}

// ---- an independent SBS parser ---------------------------------------------------------------------------------------------------
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) {
        if (c == ',') { f.push_back(cur); cur.clear(); } else { cur.push_back(c); }
    }
    f.push_back(cur);
    return f;
}

std::string sbs(const SbsMessage& m) {
    char buf[kSbsMaxLineBytes];
    const std::size_t n = sbsEncode(m, buf, sizeof(buf));
    return std::string(buf, n);
}

SbsMessage base(int type) {
    SbsMessage m;
    m.transmissionType = type;
    std::strcpy(m.hexIdent, "4840D6");
    m.generated = {2026, 11, 11, 16, 25, 3, 123};
    return m;
}

// The expected line written out field by field, in the layout's order, so the position of every value is visible in the test.
std::string expectLine(const char* type, const char* callsign, const char* alt, const char* gs, const char* trk, const char* lat, const char* lon,
                       const char* vr, const char* sqk, const char* alert, const char* emerg, const char* spi, const char* gnd,
                       const char* sess = "1", const char* acid = "1", const char* flight = "1") {
    const std::vector<std::string> f = {"MSG",      type,  sess,  acid, "4840D6", flight,  "2026/11/11", "16:25:03.123",
                                        "2026/11/11", "16:25:03.123", callsign, alt, gs, trk, lat, lon, vr, sqk, alert, emerg, spi, gnd};
    std::string out;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (i) { out += ','; }
        out += f[i];
    }
    return out + "\r\n";
}

void testSbsHandBuilt() {
    {
        SbsMessage m = base(1);
        std::strcpy(m.callsign, "KLM1023");
        m.altitudeFt = 38000;  // not carried by MSG1: must not appear
        CHECK(sbs(m) == expectLine("1", "KLM1023", "", "", "", "", "", "", "", "", "", "", ""));
    }
    {
        SbsMessage m = base(2);
        m.altitudeFt = 0;
        m.groundSpeedKt = 12;
        m.trackDeg = 271.4;
        m.latitude = 52.30861;
        m.longitude = 4.76389;
        m.onGround = true;
        CHECK(sbs(m) == expectLine("2", "", "0", "12", "271", "52.30861", "4.76389", "", "", "", "", "", "-1"));
    }
    {
        SbsMessage m = base(3);
        m.altitudeFt = 38000;
        m.latitude = 52.12345;
        m.longitude = -1.54321;
        CHECK(sbs(m) == expectLine("3", "", "38000", "", "", "52.12345", "-1.54321", "", "", "0", "0", "0", "0"));
        m.alert = true;
        m.spi = true;
        CHECK(sbs(m) == expectLine("3", "", "38000", "", "", "52.12345", "-1.54321", "", "", "-1", "0", "-1", "0"));
    }
    {
        SbsMessage m = base(4);
        m.groundSpeedKt = 451;
        m.trackDeg = 89.6;
        m.verticalRateFpm = -1280;
        m.altitudeFt = 111;  // not carried
        CHECK(sbs(m) == expectLine("4", "", "", "451", "90", "", "", "-1280", "", "", "", "", ""));
    }
    {
        SbsMessage m = base(5);
        m.altitudeFt = 36000;
        CHECK(sbs(m) == expectLine("5", "", "36000", "", "", "", "", "", "", "0", "", "0", "0"));
    }
    {
        SbsMessage m = base(6);
        m.altitudeFt = 12500;
        m.squawk = 7700;
        m.emergency = true;
        CHECK(sbs(m) == expectLine("6", "", "12500", "", "", "", "", "", "7700", "0", "-1", "0", "0"));
    }
    {
        SbsMessage m = base(7);
        m.altitudeFt = 3000;
        m.onGround = false;
        CHECK(sbs(m) == expectLine("7", "", "3000", "", "", "", "", "", "", "", "", "", "0"));
    }
    {
        SbsMessage m = base(8);
        m.onGround = true;
        CHECK(sbs(m) == expectLine("8", "", "", "", "", "", "", "", "", "", "", "", "-1"));
    }
    {
        // custom ids; lead zeros of a squawk kept; negative numbers
        SbsMessage m = base(6);
        m.sessionId = 5;
        m.aircraftId = 22;
        m.flightId = 7;
        m.altitudeFt = -200;
        m.squawk = 1;
        CHECK(sbs(m) == expectLine("6", "", "-200", "", "", "", "", "", "0001", "0", "0", "0", "0", "5", "22", "7"));
    }
    // one complete line spelled out as text, as the documents show them
    {
        SbsMessage m = base(3);
        m.altitudeFt = 38000;
        m.latitude = 52.12345;
        m.longitude = -1.54321;
        CHECK(sbs(m) == "MSG,3,1,1,4840D6,1,2026/11/11,16:25:03.123,2026/11/11,16:25:03.123,,38000,,,52.12345,-1.54321,,,0,0,0,0\r\n");
    }
}

void testSbsFieldRules() {
    // refusals
    char buf[kSbsMaxLineBytes];
    SbsMessage m = base(3);
    m.transmissionType = 0;
    CHECK(sbsEncode(m, buf, sizeof(buf)) == 0);
    m.transmissionType = 9;
    CHECK(sbsEncode(m, buf, sizeof(buf)) == 0);
    m = base(3);
    std::strcpy(m.hexIdent, "4840d6");  // lower case
    CHECK(sbsEncode(m, buf, sizeof(buf)) == 0);
    std::strcpy(m.hexIdent, "4840D");
    CHECK(sbsEncode(m, buf, sizeof(buf)) == 0);
    std::strcpy(m.hexIdent, "4840D6X");
    CHECK(sbsEncode(m, buf, sizeof(buf)) == 0);
    std::strcpy(m.hexIdent, "4840G6");
    CHECK(sbsEncode(m, buf, sizeof(buf)) == 0);
    std::memcpy(m.hexIdent, "4840D6X", 7);  // no terminator
    CHECK(sbsEncode(m, buf, sizeof(buf)) == 0);
    m = base(3);
    CHECK(sbsEncode(m, buf, 20) == 0);  // too small: refused, not truncated
    const std::size_t full = sbsEncode(m, buf, sizeof(buf));
    CHECK(full > 0);
    CHECK(sbsEncode(m, buf, full) == full);
    CHECK(sbsEncode(m, buf, full - 1) == 0);

    // hostile callsign: commas, CR, LF, NUL-free control characters cannot add a field or a line
    SbsMessage c = base(1);
    std::memcpy(c.callsign, "AB,C\r\nD\x01", 8);
    const std::string line = sbs(c);
    const std::vector<std::string> f = splitCsv(line.substr(0, line.size() - 2));
    CHECK(f.size() == 22);
    CHECK(f[10] == "AB?C??D?");
    CHECK(line.find('\n') == line.size() - 1 && line.find('\r') == line.size() - 2);
    // a lower-case callsign is not silently altered into something else: it is flagged
    SbsMessage lc = base(1);
    std::strcpy(lc.callsign, "ab1");
    CHECK(splitCsv(sbs(lc))[10] == "??1");

    // position: both or neither; out of range and non-finite are left empty
    SbsMessage p = base(3);
    p.latitude = 91.0;
    p.longitude = 0.0;
    CHECK(splitCsv(sbs(p))[14] == "" && splitCsv(sbs(p))[15] == "");
    p.latitude = 10.0;
    p.longitude = 180.5;
    CHECK(splitCsv(sbs(p))[14] == "" && splitCsv(sbs(p))[15] == "");
    p.longitude = std::nan("");
    CHECK(splitCsv(sbs(p))[14] == "" && splitCsv(sbs(p))[15] == "");
    p.longitude = INFINITY;
    CHECK(splitCsv(sbs(p))[15] == "");
    p.latitude = 90.0;
    p.longitude = -180.0;
    CHECK(splitCsv(sbs(p))[14] == "90.00000" && splitCsv(sbs(p))[15] == "-180.00000");
    p.latitude = -0.000001;  // would round to -0.00000
    p.longitude = 0.000004;
    CHECK(splitCsv(sbs(p))[14] == "0.00000" && splitCsv(sbs(p))[15] == "0.00000");
    p.latitude = 0.999996;  // rounds up through the integer part
    p.longitude = -0.000005;
    CHECK(splitCsv(sbs(p))[14] == "1.00000");
    p.latitude = 10.0;
    p.longitude.reset();  // only one of the pair given
    CHECK(splitCsv(sbs(p))[14] == "" && splitCsv(sbs(p))[15] == "");

    // squawk: not a valid transponder code -> empty
    SbsMessage q = base(6);
    for (int bad : {-1, 8, 78, 7800, 7778, 8000, 10000, 1900}) {
        q.squawk = bad;
        CHECK(splitCsv(sbs(q))[17] == "");
    }
    for (int good : {0, 7, 77, 700, 7777, 1200, 4321}) {
        q.squawk = good;
        CHECK(splitCsv(sbs(q))[17].size() == 4);
    }

    // track: wraps and rounds; non-finite empty; ground speed negative empty
    SbsMessage t = base(4);
    t.trackDeg = 359.6;
    CHECK(splitCsv(sbs(t))[13] == "0");
    t.trackDeg = -10.0;
    CHECK(splitCsv(sbs(t))[13] == "350");
    t.trackDeg = 720.0;
    CHECK(splitCsv(sbs(t))[13] == "0");
    t.trackDeg = std::nan("");
    CHECK(splitCsv(sbs(t))[13] == "");
    t.groundSpeedKt = -5;
    CHECK(splitCsv(sbs(t))[12] == "");
    t.groundSpeedKt = 0;
    CHECK(splitCsv(sbs(t))[12] == "0");

    // an out-of-range clock is clamped, never printed with an extra digit that would shift a field
    SbsMessage d = base(8);
    d.generated = {123456, 99, 99, 99, 99, 99, 99999};
    const std::vector<std::string> g = splitCsv(sbs(d));
    CHECK(g.size() == 22);
    CHECK(g[6] == "9999/12/31" && g[7] == "23:59:60.999");
}

void testSbsTime() {
    auto s = [](std::int64_t ms) {
        const SbsTime t = sbsTimeFromUnixMs(ms);
        char b[64];
        std::snprintf(b, sizeof(b), "%04d/%02d/%02d %02d:%02d:%02d.%03d", t.year, t.month, t.day, t.hour, t.minute, t.second, t.millisecond);
        return std::string(b);
    };
    CHECK(s(0) == "1970/01/01 00:00:00.000");
    CHECK(s(-1) == "1969/12/31 23:59:59.999");
    CHECK(s(1762878303123ll) == "2025/11/11 16:25:03.123");
    CHECK(s(1709164800000ll) == "2024/02/29 00:00:00.000");  // a leap day
    CHECK(s(951782400000ll) == "2000/02/29 00:00:00.000");   // the 400-year leap day
    CHECK(s(4102444799999ll) == "2099/12/31 23:59:59.999");
    CHECK(s(-86400000ll * 365 * 100) .substr(0, 4) == "1870");
    // every day over 80 years: consecutive days increase by exactly one calendar day (monotone, valid ranges)
    int prevDays = -1;
    for (std::int64_t day = -1000; day < 29000; ++day) {
        const SbsTime t = sbsTimeFromUnixMs(day * 86400000ll + 43200123);
        CHECK(t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 && t.hour == 12 && t.minute == 0 && t.second == 0 && t.millisecond == 123);
        const int days = t.year * 372 + (t.month - 1) * 31 + t.day;
        CHECK(days > prevDays);
        prevDays = days;
    }
}

// A field-by-field independent check of random messages: every carried field reads back as what was put in, every other is empty.
void testSbsRoundTrip() {
    std::mt19937 rng(777);
    const unsigned carries[9] = {0, 1, 2 | 4 | 8 | 16 | 1024, 2 | 16 | 128 | 256 | 512 | 1024, 4 | 8 | 32, 2 | 128 | 512 | 1024, 2 | 64 | 128 | 256 | 512 | 1024, 2 | 1024, 1024};
    int checked = 0;
    for (int i = 0; i < 5000; ++i) {
        SbsMessage m = base(1 + static_cast<int>(rng() % 8));
        std::snprintf(m.hexIdent, sizeof(m.hexIdent), "%06X", static_cast<unsigned>(rng() & 0xFFFFFF));
        std::snprintf(m.callsign, sizeof(m.callsign), "N%05d", static_cast<int>(rng() % 100000));
        m.generated = sbsTimeFromUnixMs(static_cast<std::int64_t>(rng() % 4000000000u) * 1000 + static_cast<std::int64_t>(rng() % 1000));
        m.altitudeFt = static_cast<int>(rng() % 60000) - 1000;
        m.groundSpeedKt = static_cast<int>(rng() % 600);
        m.trackDeg = static_cast<double>(rng() % 3600) / 10.0;
        m.latitude = (static_cast<double>(rng() % 1800000) / 10000.0) - 90.0 + 0.00001;
        m.longitude = (static_cast<double>(rng() % 3600000) / 10000.0) - 180.0 + 0.00001;
        m.verticalRateFpm = static_cast<int>(rng() % 6000) - 3000;
        m.squawk = static_cast<int>(rng() % 8) * 1000 + static_cast<int>(rng() % 8) * 100 + static_cast<int>(rng() % 8) * 10 + static_cast<int>(rng() % 8);
        m.alert = rng() & 1;
        m.emergency = rng() & 1;
        m.spi = rng() & 1;
        m.onGround = rng() & 1;
        const std::string line = sbs(m);
        CHECK(line.size() > 20 && line.substr(line.size() - 2) == "\r\n");
        const std::vector<std::string> f = splitCsv(line.substr(0, line.size() - 2));
        CHECK(f.size() == 22);
        if (f.size() != 22) { continue; }
        const unsigned has = carries[m.transmissionType];
        CHECK(f[0] == "MSG" && f[1] == std::to_string(m.transmissionType) && f[4] == m.hexIdent);
        CHECK(f[6] == f[8] && f[7] == f[9]);
        CHECK((f[10] != "") == ((has & 1) != 0));
        CHECK((f[11] != "") == ((has & 2) != 0));
        CHECK((f[12] != "") == ((has & 4) != 0));
        CHECK((f[13] != "") == ((has & 8) != 0));
        CHECK((f[14] != "" && f[15] != "") == ((has & 16) != 0));
        CHECK((f[16] != "") == ((has & 32) != 0));
        CHECK((f[17] != "") == ((has & 64) != 0));
        CHECK((f[18] != "") == ((has & 128) != 0));
        CHECK((f[19] != "") == ((has & 256) != 0));
        CHECK((f[20] != "") == ((has & 512) != 0));
        CHECK((f[21] != "") == ((has & 1024) != 0));
        if (has & 2) { CHECK(std::stoi(f[11]) == *m.altitudeFt); }
        if (has & 4) { CHECK(std::stoi(f[12]) == *m.groundSpeedKt); }
        if (has & 8) { CHECK(std::stoi(f[13]) == static_cast<int>(std::llround(*m.trackDeg)) % 360); }
        if (has & 16) {
            CHECK(std::fabs(std::stod(f[14]) - *m.latitude) <= 0.000005 + 1e-9);
            CHECK(std::fabs(std::stod(f[15]) - *m.longitude) <= 0.000005 + 1e-9);
        }
        if (has & 32) { CHECK(std::stoi(f[16]) == *m.verticalRateFpm); }
        if (has & 64) { CHECK(std::stoi(f[17]) == *m.squawk); }
        if (has & 128) { CHECK(f[18] == (*m.alert ? "-1" : "0")); }
        if (has & 1024) { CHECK(f[21] == (*m.onGround ? "-1" : "0")); }
        ++checked;
    }
    CHECK(checked == 5000);
}

// ---- the servers over real sockets -----------------------------------------------------------------------------------------------
void testBeastServer() {
    FanoutConfig c = BeastOut::defaultConfig(0);
    BeastOut out(c);
    std::string err;
    CHECK(out.start(err));
    Client a, b;
    CHECK(a.connect(out.port()));
    CHECK(b.connect(out.port()));
    for (int spins = 0; out.stats().clients < 2 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    Bytes expect;
    std::mt19937 rng(5);
    for (int i = 0; i < 300; ++i) {
        AircraftFrame f = frame({0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98},
                                static_cast<std::uint64_t>(i) * 1000 + 0x1a, static_cast<std::uint8_t>(rng()));
        CHECK(out.submit(f));
        const Bytes e = enc(f);
        expect.insert(expect.end(), e.begin(), e.end());
    }
    AircraftFrame bad;
    bad.rawLen = 5;
    CHECK(!out.submit(bad));  // an invalid frame is refused, nothing is sent
    CHECK(a.waitBytes(expect.size(), 3000));
    CHECK(b.waitBytes(expect.size(), 3000));
    CHECK(a.got == expect);  // byte-exact on the wire, both clients
    CHECK(b.got == expect);
    BeastParser p;
    p.feed(a.got);
    CHECK(p.out.size() == 300);
    // a client that sends control bytes (Beast clients send settings) is ignored without effect
    a.sendBytes("\x1a" "1C", 3);
    CHECK(out.submit(frame({1, 2}, 9, 9)));
    CHECK(a.waitBytes(expect.size() + 11, 2000));
    out.stop();
    CHECK(!out.submit(frame({1, 2}, 9, 9)));
}

void testSbsServer() {
    SbsOut out(BeastOut::defaultConfig(0));
    std::string err;
    CHECK(out.start(err));
    Client a;
    CHECK(a.connect(out.port()));
    for (int spins = 0; out.stats().clients < 1 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    std::string expect;
    for (int i = 0; i < 100; ++i) {
        SbsMessage m = base(3);
        m.altitudeFt = 30000 + i;
        m.latitude = 50.0 + i * 0.001;
        m.longitude = -1.0;
        CHECK(out.submit(m));
        expect += sbs(m);
    }
    SbsMessage bad = base(3);
    std::strcpy(bad.hexIdent, "nothex");
    CHECK(!out.submit(bad));
    CHECK(a.waitBytes(expect.size(), 3000));
    CHECK(std::string(a.got.begin(), a.got.end()) == expect);
}

}  // namespace

int main() {
    testBeastHandBuilt();
    testBeastRefusals();
    testBeastHelpers();
    testBeastRoundTrip();
    testSbsHandBuilt();
    testSbsFieldRules();
    testSbsTime();
    testSbsRoundTrip();
    testBeastServer();
    testSbsServer();
    return testSummary("test_aircraft_out");
}
