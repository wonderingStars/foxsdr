// aircraft_out.hpp - aircraft data OUT, two formats, on top of the shared TCP fan-out (net/tcp_fanout.hpp).
//
//   (a) BEAST BINARY (port 30005 by convention): the framing most aggregator feeders and map programs read. 0x1A starts a frame, the type
//       byte is '1' (Mode A/C, 2 data bytes), '2' (Mode S short, 7) or '3' (Mode S long, 14), then a 48-bit big-endian timestamp of a
//       12 MHz counter, one signal byte, then the data; every 0x1A after the marker (in the timestamp, signal or data) is sent doubled.
//   (b) SBS / BASESTATION TEXT (port 30003 by convention): one CSV line per message, "MSG," then 21 fields, 22 in all, CRLF-terminated.
//
// INPUT IS A PLAIN STRUCT so the wiring lane can feed it from the ADS-B plugin without this file knowing the plugin: AircraftFrame
// (raw bytes, time, level) for Beast; SbsMessage (decoded fields) for SBS. Both are allocation-free to fill and to encode, and the
// encoders are pure functions (tested against hand-built bytes and lines), so the producer thread's whole cost is an encode into a stack
// buffer and one wait-free publish().
//
// SOURCES. Beast: the format description at https://github.com/firestuff/adsb-tools/blob/master/protocols/beast.md (read 2026-11-11):
// marker, type bytes, 6-byte big-endian 12 MHz timestamp, signal byte, the 0x1a -> 0x1a 0x1a rule, and its worked example frame, which is
// reproduced byte for byte by a test. SBS: the field order and the per-type fields are the BaseStation "socket data" format; the primary
// description page (woodair.net/sbs/article/barebones42_socket_data.htm) could NOT be fetched in this session, so the field positions are
// UNVERIFIED against it (they agree with the secondary descriptions that could be read: 22 fields at most, field 2 the transmission type,
// MSG1 filling only the first eleven, the callsign in MSG1 only, the vertical rate in MSG4 only, the squawk in MSG6 only; the eight
// transmission types are ES identification, surface position, airborne position, airborne velocity, surveillance altitude, surveillance
// identity, air-to-air, all-call reply). The date and time text layout, the -1/0 flags and CRLF are from memory: UNVERIFIED. No program
// source was read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_AIRCRAFT_OUT_HPP
#define CASCADE_NET_AIRCRAFT_OUT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "net/tcp_fanout.hpp"

namespace cascade::net {

inline constexpr std::uint16_t kBeastPort = 30005;
inline constexpr std::uint16_t kSbsPort = 30003;
// The longest Beast frame on the wire: marker + type + (6 timestamp + 1 signal + 14 data) bytes, every one of which could be 0x1A.
inline constexpr std::size_t kBeastMaxFrameBytes = 2 + 2 * (6 + 1 + 14);
// The longest SBS line this encoder writes (22 fields, CRLF).
inline constexpr std::size_t kSbsMaxLineBytes = 192;

// ---- Beast -----------------------------------------------------------------------------------------------------------------

struct AircraftFrame {
    std::uint8_t raw[14] = {};          // the received message: 2 (Mode A/C), 7 (Mode S short) or 14 (Mode S long) bytes
    std::size_t rawLen = 0;
    std::uint64_t timestamp12MHz = 0;   // ticks of a 12 MHz counter; the low 48 bits go on the wire. 0 = "no timestamp"
    std::uint8_t level = 0;             // the signal byte as sent (0xFF = "no level" in some feeders' convention)
};

// Encodes one frame into `out` (capacity `cap`, at least kBeastMaxFrameBytes). Returns the length, or 0 when rawLen is not 2, 7 or 14 or
// `cap` is too small. No allocation.
std::size_t beastEncode(const AircraftFrame& f, std::uint8_t* out, std::size_t cap) noexcept;

// The 12 MHz counter value for a time in seconds since an arbitrary start (the receiver's sample clock); wraps at 2^48 like the wire field.
std::uint64_t beastTimestampFromSeconds(double seconds) noexcept;
// The signal byte for a linear amplitude (0..1, full scale = 1): amplitude x 255, clamped. (The format page gives 0x1a as "about 10 %",
// i.e. a linear scale.) Non-finite reads as 0.
std::uint8_t beastSignalFromAmplitude(double amplitude01) noexcept;

// ---- SBS -------------------------------------------------------------------------------------------------------------------

struct SbsTime {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0, millisecond = 0;  // UTC
};
// Calendar time from Unix milliseconds (UTC). Pure arithmetic, no locale, no allocation.
SbsTime sbsTimeFromUnixMs(std::int64_t unixMs) noexcept;

struct SbsMessage {
    int transmissionType = 3;           // 1..8: 1 identification, 2 surface position, 3 airborne position, 4 velocity, 5 surveillance altitude,
                                        //       6 surveillance identity (squawk), 7 air-to-air, 8 all-call reply
    char hexIdent[7] = {};              // the six-digit ICAO address, upper-case hex, NUL-terminated (anything else refuses the line)
    int sessionId = 1, aircraftId = 1, flightId = 1;
    SbsTime generated;                  // logged is written equal to generated
    char callsign[9] = {};              // MSG1: up to eight characters of A-Z 0-9 space - _ ; anything else becomes '?'
    std::optional<int> altitudeFt;
    std::optional<int> groundSpeedKt;
    std::optional<double> trackDeg;
    std::optional<double> latitude, longitude;   // degrees; out of range or non-finite -> the field is left empty
    std::optional<int> verticalRateFpm;
    std::optional<int> squawk;                   // the four digits as a decimal number (7700); outside 0..7777 or with a digit above 7 -> empty
    std::optional<bool> alert, emergency, spi, onGround;
};

// Writes one CRLF-terminated line. Returns its length, or 0 when the message is refused (transmission type outside 1..8, hexIdent not six
// upper-case hex digits) or `cap` is too small. Fields the type does not carry are written empty, whatever the struct holds. No allocation.
std::size_t sbsEncode(const SbsMessage& m, char* out, std::size_t cap) noexcept;

// ---- servers ---------------------------------------------------------------------------------------------------------------

// A Beast server: submit() is wait-free. Bind policy and everything else of TcpFanout applies (127.0.0.1 unless told otherwise).
class BeastOut {
public:
    explicit BeastOut(FanoutConfig cfg = defaultConfig(kBeastPort)) : fan_(cfg) {}
    bool start(std::string& error) { return fan_.start(error); }
    void stop() { fan_.stop(); }
    // PRODUCER THREAD ONLY. False when the frame is invalid, the server is not running, or the ring is full.
    bool submit(const AircraftFrame& f) noexcept;
    std::uint16_t port() const { return fan_.port(); }
    FanoutStats stats() const { return fan_.stats(); }
    static FanoutConfig defaultConfig(std::uint16_t port) {
        FanoutConfig c;
        c.port = port;
        c.clientQueueBytes = 64 * 1024;
        c.ringBytes = 256 * 1024;
        return c;
    }

private:
    TcpFanout fan_;
};

class SbsOut {
public:
    explicit SbsOut(FanoutConfig cfg = BeastOut::defaultConfig(kSbsPort)) : fan_(cfg) {}
    bool start(std::string& error) { return fan_.start(error); }
    void stop() { fan_.stop(); }
    bool submit(const SbsMessage& m) noexcept;
    std::uint16_t port() const { return fan_.port(); }
    FanoutStats stats() const { return fan_.stats(); }

private:
    TcpFanout fan_;
};

}  // namespace cascade::net

#endif
