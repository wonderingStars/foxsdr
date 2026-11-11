// The KiwiSDR wire format: command builders and the bounds-checked frame
// parser. See kiwisdr_protocol.hpp for the provenance (outline only, clean
// room) and for what is assumed beyond it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/kiwisdr_protocol.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cascade::source::kiwi {

namespace {

// The IMA ADPCM tables, as the public standard gives them.
constexpr int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,
    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
constexpr int kIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

inline std::int16_t decodeNibble(unsigned nibble, AdpcmState& st) {
    const int step = kStepTable[st.index];
    int diff = step >> 3;
    if (nibble & 1u) { diff += step >> 2; }
    if (nibble & 2u) { diff += step >> 1; }
    if (nibble & 4u) { diff += step; }
    int p = st.predictor + ((nibble & 8u) ? -diff : diff);
    if (p > 32767) { p = 32767; }
    if (p < -32768) { p = -32768; }
    st.predictor = p;
    int idx = st.index + kIndexTable[nibble & 7u];
    if (idx < 0) { idx = 0; }
    if (idx > 88) { idx = 88; }
    st.index = idx;
    return static_cast<std::int16_t>(p);
}

bool isUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

bool modeIsSafe(const std::string& m) {
    if (m.empty() || m.size() > 8) { return false; }
    for (char ch : m) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        if (!ok) { return false; }
    }
    return m != "iq";
}

// A whole decimal number, 0..999999, as an int; -1 for anything else.
int parseSmallInt(const std::string& s) {
    if (s.empty() || s.size() > 6) { return -1; }
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') { return -1; }
        v = v * 10 + (c - '0');
    }
    return v;
}

}  // namespace

namespace assumed {

std::string encodeValue(const std::string& raw) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 3);
    for (char ch : raw) {
        const auto c = static_cast<unsigned char>(ch);
        if (isUnreserved(c)) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string sanitiseRedirectTarget(const std::string& raw) {
    std::string out;
    const std::size_t n = raw.size() < kMaxRedirectTargetChars ? raw.size() : kMaxRedirectTargetChars;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = static_cast<unsigned char>(raw[i]);
        out.push_back((c < 0x20 || c == 0x7f) ? '?' : raw[i]);
    }
    return out;
}

}  // namespace assumed

// --- builders ----------------------------------------------------------------

std::string buildAuth(const std::string& password) {
    if (password.size() > kMaxPasswordChars) { return std::string(); }
    return "SET auth t=kiwi p=" + assumed::encodeValue(password);
}

std::string buildIdentUser(const std::string& name) {
    if (name.empty() || name.size() > kMaxIdentChars) { return std::string(); }
    return "SET ident_user=" + assumed::encodeValue(name);
}

std::string buildTune(const std::string& mode, int lowCutHz, int highCutHz, double frequencyHz) {
    if (!modeIsSafe(mode)) { return std::string(); }
    constexpr int kMaxCutHz = 100000;
    if (lowCutHz < -kMaxCutHz || lowCutHz > kMaxCutHz || highCutHz < -kMaxCutHz ||
        highCutHz > kMaxCutHz) {
        return std::string();
    }
    if (!std::isfinite(frequencyHz) || frequencyHz < 0.0 || frequencyHz > 1.0e9) {
        return std::string();
    }
    // Integer arithmetic, not %f: the C locale of the host program must not be
    // able to turn the decimal point into a comma.
    const long long hz = std::llround(frequencyHz);
    const long long unit = static_cast<long long>(assumed::kFreqUnitHz);
    const long long whole = hz / unit;
    const long long frac = hz % unit;
    char buf[160];
    std::snprintf(buf, sizeof buf, "SET mod=%s low_cut=%d high_cut=%d freq=%lld.%03lld", mode.c_str(),
                  lowCutHz, highCutHz, whole, frac);
    return buf;
}

std::string buildCompression(bool adpcm) {
    return adpcm ? "SET compression=1" : "SET compression=0";
}

std::string buildAudioRate(int in, int out) {
    if (in <= 0 || out <= 0 || in > 1000000 || out > 1000000) { return std::string(); }
    char buf[64];
    std::snprintf(buf, sizeof buf, "SET AR OK in=%d out=%d", in, out);
    return buf;
}

std::string buildKeepAlive() { return "SET keepalive"; }

std::string buildSoundPath(std::uint64_t timestamp) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "/%llu/SND", static_cast<unsigned long long>(timestamp));
    return buf;
}

// --- audio -------------------------------------------------------------------

void decodeAdpcm(const std::uint8_t* data, std::size_t count, AdpcmState& state,
                 std::vector<std::int16_t>& out) {
    if (state.index < 0) { state.index = 0; }
    if (state.index > 88) { state.index = 88; }
    if (state.predictor > 32767) { state.predictor = 32767; }
    if (state.predictor < -32768) { state.predictor = -32768; }
    out.reserve(out.size() + count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned lo = data[i] & 15u;
        const unsigned hi = (data[i] >> 4) & 15u;
        if (assumed::kAdpcmLowNibbleFirst) {
            out.push_back(decodeNibble(lo, state));
            out.push_back(decodeNibble(hi, state));
        } else {
            out.push_back(decodeNibble(hi, state));
            out.push_back(decodeNibble(lo, state));
        }
    }
}

// --- messages ------------------------------------------------------------------

std::vector<KeyValue> parseKeyValues(const std::string& text) {
    std::vector<KeyValue> words;
    std::size_t i = 0;
    const std::size_t n = text.size();
    auto isSep = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
    };
    while (i < n) {
        while (i < n && isSep(text[i])) { ++i; }
        std::size_t j = i;
        while (j < n && !isSep(text[j])) { ++j; }
        if (j > i) {
            KeyValue kv;
            const std::string word = text.substr(i, j - i);
            const std::size_t eq = word.find('=');
            if (eq == std::string::npos) {
                kv.key = word;
            } else {
                kv.key = word.substr(0, eq);
                kv.value = word.substr(eq + 1);
                kv.hasValue = true;
            }
            if (!kv.key.empty()) { words.push_back(std::move(kv)); }
        }
        i = j;
    }
    return words;
}

namespace {

ServerMessage classify(std::vector<KeyValue> words) {
    ServerMessage m;
    for (const KeyValue& kv : words) {
        if (kv.key == "badp") {
            const int code = parseSmallInt(kv.value);
            if (code == assumed::kBadpAcceptedCode) { continue; }
            m.kind = MessageKind::BadPassword;
            m.code = code;  // -1 when it was not a number
            break;
        }
        if (kv.key == "too_busy") {
            m.kind = MessageKind::TooBusy;
            m.code = parseSmallInt(kv.value);
            break;
        }
        if (kv.key == "down") {
            m.kind = MessageKind::Down;
            m.code = parseSmallInt(kv.value);
            break;
        }
        if (kv.key == "redirect") {
            m.kind = MessageKind::Redirect;
            m.redirectTarget = assumed::sanitiseRedirectTarget(kv.value);
            break;
        }
    }
    m.words = std::move(words);
    return m;
}

}  // namespace

// --- frames ----------------------------------------------------------------------

ParseError parseFrame(const std::uint8_t* data, std::size_t len, AdpcmState& adpcm,
                      ParsedFrame& out) {
    // The size test comes first: nothing below allocates or decodes anything
    // for a frame above the limit.
    if (len > kMaxFrameBytes) { return ParseError::TooLarge; }
    if (len < kTagBytes || data == nullptr) { return ParseError::TooShort; }

    if (std::memcmp(data, "MSG", kTagBytes) == 0) {
        std::size_t off = kTagBytes;
        if (off < len && static_cast<char>(data[off]) == assumed::kMsgSeparator) { ++off; }
        const std::size_t textLen = len - off;
        if (textLen > kMaxMessageTextBytes) { return ParseError::TooLarge; }
        const std::string text(reinterpret_cast<const char*>(data) + off, textLen);
        out.type = FrameType::Msg;
        out.payloadBytes = len - kTagBytes;
        out.message = classify(parseKeyValues(text));
        return ParseError::None;
    }
    if (std::memcmp(data, "SND", kTagBytes) == 0) {
        if (len < kSndHeaderBytes) { return ParseError::TooShort; }
        const std::uint8_t flags = data[3];
        const std::uint32_t seq = static_cast<std::uint32_t>(data[4]) |
                                  (static_cast<std::uint32_t>(data[5]) << 8) |
                                  (static_cast<std::uint32_t>(data[6]) << 16) |
                                  (static_cast<std::uint32_t>(data[7]) << 24);
        const std::uint16_t sm = static_cast<std::uint16_t>((data[8] << 8) | data[9]);
        const std::uint8_t* payload = data + kSndHeaderBytes;
        const std::size_t payloadLen = len - kSndHeaderBytes;
        const bool isAdpcm = (flags & kFlagAdpcm) != 0;
        if (!isAdpcm && (payloadLen & 1u) != 0) { return ParseError::BadPayload; }

        out.type = FrameType::Snd;
        out.payloadBytes = payloadLen;
        out.snd.flags = flags;
        out.snd.sequence = seq;
        out.snd.smeterRaw = sm;
        out.snd.rssiDbm = rssiDbm(sm);
        out.snd.adpcm = isAdpcm;
        out.snd.samples.clear();
        if (isAdpcm) {
            decodeAdpcm(payload, payloadLen, adpcm, out.snd.samples);
        } else {
            out.snd.samples.reserve(payloadLen / 2);
            for (std::size_t i = 0; i + 1 < payloadLen; i += 2) {
                const unsigned a = payload[i];
                const unsigned b = payload[i + 1];
                const unsigned u = assumed::kPcmBigEndian ? ((a << 8) | b) : ((b << 8) | a);
                out.snd.samples.push_back(static_cast<std::int16_t>(static_cast<std::uint16_t>(u)));
            }
        }
        return ParseError::None;
    }
    if (std::memcmp(data, "W/F", kTagBytes) == 0) {
        out.type = FrameType::Waterfall;
        out.payloadBytes = len - kTagBytes;
        return ParseError::None;
    }
    if (std::memcmp(data, "EXT", kTagBytes) == 0) {
        out.type = FrameType::Ext;
        out.payloadBytes = len - kTagBytes;
        return ParseError::None;
    }
    return ParseError::UnknownTag;
}

const char* parseErrorText(ParseError e) {
    switch (e) {
        case ParseError::None: return "no error";
        case ParseError::TooShort: return "frame shorter than its header";
        case ParseError::TooLarge: return "frame larger than the limit";
        case ParseError::UnknownTag: return "unknown frame tag";
        case ParseError::BadPayload: return "audio payload with a half sample";
    }
    return "unknown error";
}

}  // namespace cascade::source::kiwi
