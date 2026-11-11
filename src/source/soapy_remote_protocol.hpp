// soapy_remote_protocol.hpp - the SoapyRemote wire format as plain, testable functions: the
// RPC message codec and the stream datagram header. No sockets here.
//
// PROVENANCE (CONTRIBUTING.md, clean room; this lane's rule: read a project only if its
// licence is Boost, Apache-2.0, MIT, BSD or public domain). SoapyRemote's licence was opened
// first: https://github.com/pothosware/SoapyRemote, file LICENSE_1_0.txt is the Boost Software
// License 1.0 (first line "Boost Software License - Version 1.0 - August 17th, 2003"), and
// the README says "Use, modification and distribution is subject to the Boost Software
// License, Version 1.0", and every source file carries "SPDX-License-Identifier: BSL-1.0".
// The format below was taken from that project's own sources, read on 2026-11-11 at master:
// common/SoapyRemoteDefs.hpp (the numbers), common/SoapyRPCPacker.cpp and
// SoapyRPCUnpacker.cpp (the message layout), common/SoapyStreamEndpoint.cpp (the datagram
// header and flow control), client/Settings.cpp, Streaming.cpp, Registration.cpp (the
// order of the calls and their arguments). The words and the code here are this project's
// own; the numbers are facts about a wire format. No GPL/LGPL source was consulted. No
// SoapySDR library was linked or downloaded.
//
// THE FORMAT, all integers big-endian.
//   message  = u32 "SRPC" (0x53525043) | u32 version | u32 length | payload | u32 "CPRS"
//              (0x43505253). length counts the whole message: header, payload and trailer.
//              The version this reading saw is 0x000400.
//   payload  = a sequence of TYPED values: one type byte (SoapyRemoteTypes) then its body.
//     CHAR 0: 1 byte      BOOL 1: 1 byte (0/1)    INT32 2: 4 bytes    INT64 3: 8 bytes
//     FLOAT64 4: INT32 exponent then INT64 mantissa (each itself typed): value =
//                ldexp(mantissa, exponent - 53) (frexp split, 53 = DBL_MANT_DIG)
//     STRING 6: INT32 length (typed) then the bytes
//     RANGE 7: FLOAT64 min, FLOAT64 max, and (when the sender's version is >= 0x000400)
//                FLOAT64 step
//     RANGE_LIST 8, STRING_LIST 9, FLOAT64_LIST 10, KWARGS_LIST 12, ARG_INFO_LIST 18,
//     SIZE_LIST 16 (each element an INT32): INT32 count then the elements, each typed.
//     KWARGS 11: INT32 count, then key STRING, value STRING pairs.
//     EXCEPTION 13: STRING message.   VOID 14: nothing.   CALL 15: followed by a typed INT32
//                call number.
//   request  = CALL, then the arguments. reply = the result values, or VOID, or EXCEPTION.
//
//   stream datagram (24 bytes of header then samples): u32 bytes (the whole datagram incl.
//   this header) | u32 sequence | u32 elems (an element count, or a negative error code)
//   | i32 flags | i64 time (ns). The receiver acknowledges with a header-only datagram:
//   bytes = 24, sequence = the next sequence expected, elems = the most datagrams it
//   allows in flight, flags 0, time 0.
//
// WHAT THE READING DID NOT GIVE, and is therefore not built: a CS12 (packed) stream, the log
// forwarding socket, hardware time and multi-channel streams (the elements of channel k sit
// at header + k*buffSize*elemSize, the part of this that was understood, but only channel 0
// is used).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "source/iiod_client.hpp"

namespace cascade::source::soapyremote {

constexpr std::uint32_t kHeaderWord = 0x53525043u;   // "SRPC"
constexpr std::uint32_t kTrailerWord = 0x43505253u;  // "CPRS"
constexpr std::uint32_t kRpcVersion = 0x000400u;
constexpr std::size_t kMessageHeaderBytes = 12;
constexpr std::size_t kMessageTrailerBytes = 4;
// A reply longer than this is refused before anything is allocated (the length field is the
// peer's word, not ours).
constexpr std::size_t kMaxMessageBytes = 1u << 20;
constexpr std::uint16_t kDefaultPort = 55132;
constexpr std::size_t kDatagramHeaderBytes = 24;
constexpr int kDirectionTx = 0;
constexpr int kDirectionRx = 1;

enum Type : unsigned char {
    T_CHAR = 0, T_BOOL = 1, T_INT32 = 2, T_INT64 = 3, T_FLOAT64 = 4, T_COMPLEX128 = 5,
    T_STRING = 6, T_RANGE = 7, T_RANGE_LIST = 8, T_STRING_LIST = 9, T_FLOAT64_LIST = 10,
    T_KWARGS = 11, T_KWARGS_LIST = 12, T_EXCEPTION = 13, T_VOID = 14, T_CALL = 15,
    T_SIZE_LIST = 16, T_ARG_INFO = 17, T_ARG_INFO_LIST = 18
};

enum Call : int {
    C_FIND = 0, C_MAKE = 1, C_UNMAKE = 2, C_HANGUP = 3,
    C_GET_DRIVER_KEY = 100, C_GET_HARDWARE_KEY = 101,
    C_SETUP_STREAM = 300, C_CLOSE_STREAM = 301, C_ACTIVATE_STREAM = 302,
    C_DEACTIVATE_STREAM = 303, C_GET_NATIVE_STREAM_FORMAT = 305,
    C_LIST_ANTENNAS = 500, C_SET_ANTENNA = 501, C_GET_ANTENNA = 502,
    C_HAS_FREQUENCY_CORRECTION = 503, C_SET_FREQUENCY_CORRECTION = 504,
    C_LIST_GAINS = 700, C_SET_GAIN_MODE = 701, C_SET_GAIN_ELEMENT = 704,
    C_GET_GAIN_ELEMENT = 706, C_GET_GAIN_RANGE_ELEMENT = 708, C_HAS_GAIN_MODE = 709,
    C_SET_FREQUENCY = 800, C_GET_FREQUENCY_RANGE = 805,
    C_SET_SAMPLE_RATE = 900, C_GET_SAMPLE_RATE = 901, C_LIST_SAMPLE_RATES = 902,
    C_GET_SAMPLE_RATE_RANGE = 907, C_SET_BANDWIDTH = 903
};

struct Range {
    double minimum = 0.0, maximum = 0.0, step = 0.0;
};

// --- building a message ------------------------------------------------------------------

class Packer {
public:
    Packer() { bytes_.resize(kMessageHeaderBytes); }

    Packer& call(int c) { type(T_CALL); return i32(c); }
    Packer& chr(char v) { type(T_CHAR); bytes_.push_back(static_cast<unsigned char>(v)); return *this; }
    Packer& boolean(bool v) { type(T_BOOL); bytes_.push_back(v ? 1 : 0); return *this; }
    Packer& i32(int v) { type(T_INT32); i32raw(v); return *this; }
    Packer& i64(long long v) { type(T_INT64); u64raw(static_cast<std::uint64_t>(v)); return *this; }
    Packer& f64(double v) {
        type(T_FLOAT64);
        int exp = 0;
        const double x = std::frexp(v, &exp);
        const long long man = static_cast<long long>(std::ldexp(x, 53));
        i32(exp);
        i64(man);
        return *this;
    }
    Packer& str(const std::string& s) {
        type(T_STRING);
        i32(static_cast<int>(s.size()));
        bytes_.insert(bytes_.end(), s.begin(), s.end());
        return *this;
    }
    Packer& kwargs(const std::map<std::string, std::string>& m) {
        type(T_KWARGS);
        i32(static_cast<int>(m.size()));
        for (const auto& kv : m) { str(kv.first); str(kv.second); }
        return *this;
    }
    Packer& sizeList(const std::vector<int>& v) {
        type(T_SIZE_LIST);
        i32(static_cast<int>(v.size()));
        for (const int x : v) { i32(x); }
        return *this;
    }

    // The finished message: header and trailer filled in.
    std::vector<std::uint8_t> finish() const {
        std::vector<std::uint8_t> out = bytes_;
        out.push_back(static_cast<std::uint8_t>(kTrailerWord >> 24));
        out.push_back(static_cast<std::uint8_t>(kTrailerWord >> 16));
        out.push_back(static_cast<std::uint8_t>(kTrailerWord >> 8));
        out.push_back(static_cast<std::uint8_t>(kTrailerWord));
        put32(out.data() + 0, kHeaderWord);
        put32(out.data() + 4, kRpcVersion);
        put32(out.data() + 8, static_cast<std::uint32_t>(out.size()));
        return out;
    }

    static void put32(std::uint8_t* p, std::uint32_t v) {
        p[0] = static_cast<std::uint8_t>(v >> 24);
        p[1] = static_cast<std::uint8_t>(v >> 16);
        p[2] = static_cast<std::uint8_t>(v >> 8);
        p[3] = static_cast<std::uint8_t>(v);
    }

private:
    void type(Type t) { bytes_.push_back(static_cast<unsigned char>(t)); }
    void i32raw(int v) {
        const std::uint32_t u = static_cast<std::uint32_t>(v);
        for (int s = 24; s >= 0; s -= 8) { bytes_.push_back(static_cast<unsigned char>(u >> s)); }
    }
    void u64raw(std::uint64_t u) {
        for (int s = 56; s >= 0; s -= 8) { bytes_.push_back(static_cast<unsigned char>(u >> s)); }
    }
    std::vector<std::uint8_t> bytes_;
};

// --- reading a message -------------------------------------------------------------------

// Throws std::runtime_error with a sentence for every malformed input: no read ever goes past
// the payload, and no length is trusted before it is checked.
class Unpacker {
public:
    // `payload` is what lies between the 12-byte header and the 4-byte trailer;
    // `remoteVersion` is the header's version word.
    Unpacker(std::vector<std::uint8_t> payload, std::uint32_t remoteVersion)
        : p_(std::move(payload)), version_(remoteVersion) {
        // A reply that is a VOID is consumed; an EXCEPTION throws, as the peer's own reader does.
        if (!p_.empty() && p_[0] == T_VOID) { off_ = 1; }
        else if (!p_.empty() && p_[0] == T_EXCEPTION) {
            off_ = 1;
            const std::string m = str();
            throw std::runtime_error("RemoteError: " + m);
        }
    }

    bool done() const { return off_ == p_.size(); }
    std::uint32_t version() const { return version_; }

    int i32() {
        expect(T_INT32);
        return static_cast<int>(raw32());
    }
    long long i64() {
        expect(T_INT64);
        std::uint64_t v = 0;
        need(8);
        for (int k = 0; k < 8; ++k) { v = (v << 8) | p_[off_++]; }
        return static_cast<long long>(v);
    }
    bool boolean() {
        expect(T_BOOL);
        need(1);
        return p_[off_++] != 0;
    }
    char chr() {
        expect(T_CHAR);
        need(1);
        return static_cast<char>(p_[off_++]);
    }
    double f64() {
        expect(T_FLOAT64);
        const int exp = i32();
        const long long man = i64();
        return std::ldexp(static_cast<double>(man), exp - 53);
    }
    std::string str() {
        expect(T_STRING);
        const int n = i32();
        if (n < 0) { throw std::runtime_error("negative string length"); }
        need(static_cast<std::size_t>(n));
        std::string s(reinterpret_cast<const char*>(p_.data() + off_), static_cast<std::size_t>(n));
        off_ += static_cast<std::size_t>(n);
        return s;
    }
    std::vector<std::string> stringList() {
        expect(T_STRING_LIST);
        const int n = count(1);
        std::vector<std::string> v;
        for (int k = 0; k < n; ++k) { v.push_back(str()); }
        return v;
    }
    std::vector<double> f64List() {
        expect(T_FLOAT64_LIST);
        const int n = count(1);
        std::vector<double> v;
        for (int k = 0; k < n; ++k) { v.push_back(f64()); }
        return v;
    }
    Range range() {
        expect(T_RANGE);
        Range r;
        r.minimum = f64();
        r.maximum = f64();
        if (version_ >= kRpcVersion) { r.step = f64(); }
        return r;
    }
    std::vector<Range> rangeList() {
        expect(T_RANGE_LIST);
        const int n = count(1);
        std::vector<Range> v;
        for (int k = 0; k < n; ++k) { v.push_back(range()); }
        return v;
    }
    std::map<std::string, std::string> kwargs() {
        expect(T_KWARGS);
        const int n = count(2);
        std::map<std::string, std::string> m;
        for (int k = 0; k < n; ++k) {
            std::string key = str();
            m[key] = str();
        }
        return m;
    }

private:
    void need(std::size_t n) const {
        if (n > p_.size() - off_) { throw std::runtime_error("the reply is shorter than it says"); }
    }
    void expect(Type t) {
        need(1);
        if (p_[off_++] != t) { throw std::runtime_error("the reply holds a value of the wrong type"); }
    }
    std::uint32_t raw32() {
        need(4);
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) { v = (v << 8) | p_[off_++]; }
        return v;
    }
    // An element count is checked against the bytes left (every element takes at least
    // `minBytesEach`), so a lying count cannot make a loop run for ever or allocate.
    int count(std::size_t minBytesEach) {
        const int n = i32();
        if (n < 0 || static_cast<std::size_t>(n) > (p_.size() - off_) / minBytesEach) {
            throw std::runtime_error("an element count larger than the reply");
        }
        return n;
    }
    std::vector<std::uint8_t> p_;
    std::size_t off_ = 0;
    std::uint32_t version_;
};

// --- moving whole messages over a transport ----------------------------------------------

inline bool sendMessage(iiod::Transport& t, const std::vector<std::uint8_t>& msg) {
    return t.sendAll(msg.data(), msg.size());
}

// Reads one message. False with `error` set for: a transport failure or timeout, a wrong
// header word, a length that cannot hold a header, trailer and one byte, a length over
// kMaxMessageBytes, or a wrong trailer word. On success `payload` and `version` are set.
inline bool recvMessage(iiod::Transport& t, std::vector<std::uint8_t>& payload,
                        std::uint32_t& version, std::string& error) {
    std::uint8_t h[kMessageHeaderBytes];
    if (!t.recvAll(h, sizeof(h))) { error = t.lastError(); return false; }
    const auto rd = [](const std::uint8_t* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    };
    if (rd(h) != kHeaderWord) { error = "the server did not answer in the SoapyRemote format"; return false; }
    version = rd(h + 4);
    const std::uint32_t length = rd(h + 8);
    if (length <= kMessageHeaderBytes + kMessageTrailerBytes) {
        error = "a reply with an impossible length";
        return false;
    }
    if (length > kMaxMessageBytes) { error = "a reply longer than the limit"; return false; }
    std::vector<std::uint8_t> rest(length - kMessageHeaderBytes);
    if (!t.recvAll(rest.data(), rest.size())) { error = t.lastError(); return false; }
    if (rd(rest.data() + rest.size() - kMessageTrailerBytes) != kTrailerWord) {
        error = "a reply with a wrong trailer";
        return false;
    }
    rest.resize(rest.size() - kMessageTrailerBytes);
    payload = std::move(rest);
    return true;
}

// --- the stream datagram header ---------------------------------------------------------

struct DatagramHeader {
    std::uint32_t bytes = 0;
    std::uint32_t sequence = 0;
    std::int32_t elems = 0;
    std::int32_t flags = 0;
    std::int64_t time = 0;
};

inline void encodeDatagramHeader(const DatagramHeader& h, std::uint8_t* out) {
    Packer::put32(out + 0, h.bytes);
    Packer::put32(out + 4, h.sequence);
    Packer::put32(out + 8, static_cast<std::uint32_t>(h.elems));
    Packer::put32(out + 12, static_cast<std::uint32_t>(h.flags));
    const std::uint64_t t = static_cast<std::uint64_t>(h.time);
    Packer::put32(out + 16, static_cast<std::uint32_t>(t >> 32));
    Packer::put32(out + 20, static_cast<std::uint32_t>(t));
}

inline DatagramHeader decodeDatagramHeader(const std::uint8_t* p) {
    const auto rd = [](const std::uint8_t* q) {
        return (static_cast<std::uint32_t>(q[0]) << 24) | (static_cast<std::uint32_t>(q[1]) << 16) |
               (static_cast<std::uint32_t>(q[2]) << 8) | q[3];
    };
    DatagramHeader h;
    h.bytes = rd(p);
    h.sequence = rd(p + 4);
    h.elems = static_cast<std::int32_t>(rd(p + 8));
    h.flags = static_cast<std::int32_t>(rd(p + 12));
    h.time = static_cast<std::int64_t>((static_cast<std::uint64_t>(rd(p + 16)) << 32) | rd(p + 20));
    return h;
}

// What a received datagram holds, checked. `have` is the bytes actually received (the whole
// datagram for UDP, header plus payload for TCP). Result: Ok (elems samples follow the
// header), Status (elems < 0: an error code, no samples), or one of the refusals.
enum class DatagramVerdict { Ok, Status, TooShort, LengthLiesBeyondData, ElemsBeyondLength, BadElemSize };

inline DatagramVerdict checkDatagram(const DatagramHeader& h, std::size_t have, std::size_t elemBytes) {
    if (have < kDatagramHeaderBytes) { return DatagramVerdict::TooShort; }
    if (h.bytes < kDatagramHeaderBytes || h.bytes > have) { return DatagramVerdict::LengthLiesBeyondData; }
    if (h.elems < 0) { return DatagramVerdict::Status; }
    if (elemBytes == 0) { return DatagramVerdict::BadElemSize; }
    const std::size_t payload = h.bytes - kDatagramHeaderBytes;
    if (static_cast<std::uint64_t>(h.elems) * elemBytes > payload) { return DatagramVerdict::ElemsBeyondLength; }
    return DatagramVerdict::Ok;
}

// The stream formats this reading understood, with the bytes one complex element takes.
// 0 for a format that is not supported (CS12 and anything else).
inline std::size_t elementBytes(const std::string& fmt) {
    if (fmt == "CS8" || fmt == "CU8") { return 2; }
    if (fmt == "CS16") { return 4; }
    if (fmt == "CF32") { return 8; }
    return 0;
}

}  // namespace cascade::source::soapyremote
