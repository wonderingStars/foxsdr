// source/extio_protocol.hpp - the wire between FoxSDR and its ExtIO HELPER PROCESS (row R8). Pure: no Windows, no threads, no I/O.
//
// WHY A HELPER PROCESS AND NOT A DLL LOAD. An ExtIO radio driver is a Windows DLL written by a radio's maker (or by a hobbyist) for HDSDR and
// Winrad. It runs arbitrary code, often on its own threads, usually shows its own window, is mostly built for 32-bit Windows, and in practice
// crashes, hangs and floods. This program never loads one. A small separate program (tools/extio_helper) loads the DLL the user chose, calls the
// documented entry points and forwards samples and events over a named pipe; this file is the vocabulary on that pipe. Everything the helper says
// is treated as UNTRUSTED here (the DLL's code runs inside it): every length is bounded, every count checked against what the frame holds.
//
// THE INTERFACE IS FROM THE PUBLISHED SPECIFICATION: "Winrad - specifications for the external I/O DLL", Alberto I2PHD, read as text from
// https://www.i2phd.org/code/Winrad_Extio.pdf (the page https://www.i2phd.org/winrad/ links it; it carries no licence or copyright terms, so it is
// used as a description of a wire/ABI interface and nothing in it is copied beyond names, signatures and status numbers). No header file of any
// vendor or of HDSDR was opened. The entry points, the HW type codes (3 = 16-bit, 5 = 24-bit, 6 = 32-bit integer, 7 = 32-bit float, little endian,
// 4 = samples through the sound card), the callback `void (*)(int cnt, int status, float IQoffs, void* IQdata)` and the status numbers 100 to 115
// are that document's. StartHW's return value is the number of I/Q pairs per callback ("at least 512, or an integer multiple of that value").
//
// FRAME: 12 bytes, little endian: u32 magic "FXIO", u8 type, u8 zero, u16 zero, u32 payload length; then the payload.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_EXTIO_PROTOCOL_HPP
#define CASCADE_SOURCE_EXTIO_PROTOCOL_HPP

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace cascade::source::extio {

inline constexpr std::uint32_t kMagic = 0x4F495846u;           // "FXIO" as little-endian bytes F X I O
inline constexpr std::size_t kHeaderBytes = 12;
inline constexpr std::uint32_t kMaxPayload = 4u * 1024u * 1024u;
inline constexpr std::uint32_t kMaxPairsPerCallback = 256u * 1024u;   // one callback's I/Q pairs; 2 MiB of float pairs
inline constexpr std::uint32_t kMinPairs = 512;                       // the specification's floor for StartHW's return

enum class Msg : std::uint8_t {
    // application to helper
    Init = 1,      // load the DLL, InitHW. payload: none
    Open = 2,      // OpenHW. payload: none
    Start = 3,     // StartHW(freq). payload: i32 freqHz
    Stop = 4,      // StopHW
    Close = 5,     // CloseHW
    SetLo = 6,     // SetHWLO(freq). payload: i32 freqHz
    SetMode = 7,   // ModeChanged(char). payload: u8 mode letter
    Quit = 8,      // unload and exit
    ShowGui = 9,   // ShowGUI (the DLL's own window, if it has one)
    HideGui = 10,  // HideGUI
    // helper to application
    Reply = 0x80,  // payload: u8 command, i32 result, then command-specific
    Data = 0x81,   // payload: u32 pairs, f32 iqOffset, then pairs x (f32 i, f32 q)
    Status = 0x82, // payload: i32 status, i32 value (what the helper read back for it)
};

// The facts a refusal can carry. Closed words, written to the log and to the engine's refusal facts; never a path.
inline constexpr const char* kFactNotChosen = "extio-not-chosen";
inline constexpr const char* kFactHelperMissing = "extio-helper-missing";
inline constexpr const char* kFactHelperStart = "extio-helper-start-failed";
inline constexpr const char* kFactHelperExited = "extio-helper-exited";
inline constexpr const char* kFactTimeout = "extio-timeout";
inline constexpr const char* kFactProtocol = "extio-protocol";
inline constexpr const char* kFactDllRefused = "extio-dll-refused";
inline constexpr const char* kFactUnsupportedType = "extio-unsupported-type";
inline constexpr const char* kFactBadSize = "extio-bad-size";
inline constexpr const char* kFactFlood = "extio-flood";
inline constexpr const char* kFactWrongBitness = "extio-wrong-bitness";
inline constexpr const char* kFactSilent = "extio-silent";

// The warning every screen that lets a person choose a DLL must show (English; no translation in this lane).
inline constexpr const char* kExtioWarning =
    "An ExtIO driver is a program made by the radio's maker. It runs in a separate helper process with YOUR rights: it can read and change anything "
    "you can. Choose only a driver you trust.";

// A status number of this program's own (outside the specification's 100 to 115): the helper's count of callbacks it had to drop because the
// pipe could not keep up, or that carried nonsense. value = the running total.
inline constexpr std::int32_t kStatusDropped = 9001;
inline constexpr std::int32_t kStatusBadCallbacks = 9002;

struct Header {
    Msg type{};
    std::uint32_t length = 0;
};

inline void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) { b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
}
inline std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline void putStr(std::vector<std::uint8_t>& b, const std::string& s) {
    const std::size_t n = s.size() > 255 ? 255 : s.size();
    b.push_back(static_cast<std::uint8_t>(n));
    b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

// A whole frame: header and payload.
inline std::vector<std::uint8_t> frame(Msg type, const std::vector<std::uint8_t>& payload = {}) {
    std::vector<std::uint8_t> b;
    b.reserve(kHeaderBytes + payload.size());
    put32(b, kMagic);
    b.push_back(static_cast<std::uint8_t>(type));
    b.push_back(0);
    b.push_back(0);
    b.push_back(0);
    put32(b, static_cast<std::uint32_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

// Reassembles frames from whatever sized pieces the pipe delivers. A violation is permanent: the stream cannot be resynchronised once a
// length or magic is wrong, and a helper that sends one is not trusted any further.
class FrameReader {
public:
    // Feed bytes; each complete frame is handed to `on(type, payload, n)`. Returns false (and error() says why) on a violation.
    template <class F>
    bool feed(const std::uint8_t* data, std::size_t n, F&& on) {
        if (!error_.empty()) { return false; }
        buf_.insert(buf_.end(), data, data + n);
        std::size_t at = 0;
        while (buf_.size() - at >= kHeaderBytes) {
            const std::uint8_t* h = buf_.data() + at;
            if (get32(h) != kMagic) { error_ = "bad magic"; return false; }
            if (h[5] != 0 || h[6] != 0 || h[7] != 0) { error_ = "reserved bytes not zero"; return false; }
            const std::uint32_t len = get32(h + 8);
            if (len > kMaxPayload) { error_ = "payload too large"; return false; }
            if (buf_.size() - at < kHeaderBytes + len) { break; }
            on(static_cast<Msg>(h[4]), h + kHeaderBytes, static_cast<std::size_t>(len));
            at += kHeaderBytes + len;
        }
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(at));
        return true;
    }
    const std::string& error() const { return error_; }
    std::size_t buffered() const { return buf_.size(); }

private:
    std::vector<std::uint8_t> buf_;
    std::string error_;
};

// ---- what StartHW's return value may be ------------------------------------------------------------------------------------------------------
enum class StartVerdict : std::uint8_t { Ok, Error, TooFew, TooMany, NotMultiple };
inline StartVerdict judgeStartPairs(std::int64_t pairs) {
    if (pairs < 0) { return StartVerdict::Error; }
    if (pairs < static_cast<std::int64_t>(kMinPairs)) { return StartVerdict::TooFew; }
    if (pairs > static_cast<std::int64_t>(kMaxPairsPerCallback)) { return StartVerdict::TooMany; }
    if (pairs % kMinPairs != 0) { return StartVerdict::NotMultiple; }
    return StartVerdict::Ok;
}

// ---- samples ---------------------------------------------------------------------------------------------------------------------------------
// Bytes per I or Q value for an ExtIO HW type; 0 for a type that does not return samples through the callback (4: the sound card) or is unknown.
inline std::size_t bytesPerValue(std::uint32_t hwType) {
    switch (hwType) {
        case 3: return 2;
        case 5: return 3;
        case 6: return 4;
        case 7: return 4;
        default: return 0;
    }
}

// `pairs` interleaved I/Q pairs of the given HW type (little endian) to float32 pairs in [-1, 1). A float source's NaN or infinity becomes 0
// and out-of-range floats are clamped, so nothing downstream ever sees a non-finite sample. Returns false for a type without samples.
inline bool convertSamples(std::uint32_t hwType, const std::uint8_t* src, std::size_t pairs, float* out) {
    const std::size_t bpv = bytesPerValue(hwType);
    if (bpv == 0) { return false; }
    for (std::size_t i = 0; i < pairs * 2; ++i) {
        const std::uint8_t* p = src + i * bpv;
        float v = 0.0f;
        switch (hwType) {
            case 3: v = static_cast<float>(static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)))) / 32768.0f; break;
            case 5: {
                std::int32_t x = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                                           (static_cast<std::uint32_t>(p[2]) << 16));
                if ((x & 0x800000) != 0) { x -= 0x1000000; }
                v = static_cast<float>(x) / 8388608.0f;
                break;
            }
            case 6: v = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(get32(p))) / 2147483648.0); break;
            case 7: {
                const std::uint32_t u = get32(p);
                float f;
                std::memcpy(&f, &u, 4);
                v = std::isfinite(f) ? (f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f)) : 0.0f;
                break;
            }
            default: break;
        }
        out[i] = v;
    }
    return true;
}

// ---- payload codecs (the application side reads helper payloads, so these are the parsers of untrusted bytes) -----------------------------------
struct InitReply {
    std::int32_t result = 0;       // 1: InitHW returned true
    std::uint32_t hwType = 0;
    std::uint32_t exports = 0;     // bit n set: optional entry n exists (see kExport*)
    std::uint32_t win32Error = 0;  // GetLastError() when the DLL did not load
    std::string name, model;
    std::string missing;           // names of mandatory entry points the DLL lacks, comma separated
};
inline constexpr std::uint32_t kExportGetHWLO = 1u, kExportGetHWSR = 2u, kExportGetTune = 4u, kExportGetMode = 8u, kExportShowGui = 16u;

inline std::vector<std::uint8_t> encodeInitReply(const InitReply& r) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(Msg::Init));
    put32(b, static_cast<std::uint32_t>(r.result));
    put32(b, r.hwType);
    put32(b, r.exports);
    put32(b, r.win32Error);
    putStr(b, r.name);
    putStr(b, r.model);
    putStr(b, r.missing);
    return b;
}
// The payload after the command byte and result, i.e. from `hwType` on; `p` points at the whole reply payload.
inline bool decodeInitReply(const std::uint8_t* p, std::size_t n, InitReply& out) {
    if (n < 1 + 4 * 4 || p[0] != static_cast<std::uint8_t>(Msg::Init)) { return false; }
    out.result = static_cast<std::int32_t>(get32(p + 1));
    out.hwType = get32(p + 5);
    out.exports = get32(p + 9);
    out.win32Error = get32(p + 13);
    std::size_t at = 17;
    std::string* fields[3] = {&out.name, &out.model, &out.missing};
    for (std::string* f : fields) {
        if (at >= n) { return false; }
        const std::size_t len = p[at++];
        if (at + len > n) { return false; }
        f->assign(reinterpret_cast<const char*>(p + at), len);
        at += len;
    }
    return at == n;
}

// A Reply for any other command: u8 command, i32 result, u32 extra (StartHW: the sample rate GetHWSR gave, or 0).
inline std::vector<std::uint8_t> encodeReply(Msg cmd, std::int32_t result, std::uint32_t extra = 0) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(cmd));
    put32(b, static_cast<std::uint32_t>(result));
    put32(b, extra);
    return b;
}
struct SimpleReply {
    Msg cmd{};
    std::int32_t result = 0;
    std::uint32_t extra = 0;
};
inline bool decodeSimpleReply(const std::uint8_t* p, std::size_t n, SimpleReply& out) {
    if (n != 9) { return false; }
    out.cmd = static_cast<Msg>(p[0]);
    out.result = static_cast<std::int32_t>(get32(p + 1));
    out.extra = get32(p + 5);
    return true;
}

inline std::vector<std::uint8_t> encodeData(const std::complex<float>* iq, std::uint32_t pairs, float iqOffset) {
    std::vector<std::uint8_t> b;
    b.reserve(8 + static_cast<std::size_t>(pairs) * 8);
    put32(b, pairs);
    std::uint32_t o;
    std::memcpy(&o, &iqOffset, 4);
    put32(b, o);
    const std::size_t at = b.size();
    b.resize(at + static_cast<std::size_t>(pairs) * 8);
    std::memcpy(b.data() + at, iq, static_cast<std::size_t>(pairs) * 8);
    return b;
}
// The pairs of a Data payload, bounds-checked: false when the count disagrees with the payload size or exceeds the cap. `iq` points into `p`.
inline bool decodeData(const std::uint8_t* p, std::size_t n, std::uint32_t& pairs, float& iqOffset, const std::uint8_t*& iq) {
    if (n < 8) { return false; }
    pairs = get32(p);
    if (pairs > kMaxPairsPerCallback) { return false; }
    if (n != 8 + static_cast<std::size_t>(pairs) * 8) { return false; }
    const std::uint32_t o = get32(p + 4);
    std::memcpy(&iqOffset, &o, 4);
    iq = p + 8;
    return true;
}

struct StatusEvent {
    std::int32_t status = 0;
    std::int32_t value = 0;
};
inline bool decodeStatus(const std::uint8_t* p, std::size_t n, StatusEvent& out) {
    if (n != 8) { return false; }
    out.status = static_cast<std::int32_t>(get32(p));
    out.value = static_cast<std::int32_t>(get32(p + 4));
    return true;
}
inline std::vector<std::uint8_t> encodeStatus(std::int32_t status, std::int32_t value) {
    std::vector<std::uint8_t> b;
    put32(b, static_cast<std::uint32_t>(status));
    put32(b, static_cast<std::uint32_t>(value));
    return b;
}

}  // namespace cascade::source::extio

#endif  // CASCADE_SOURCE_EXTIO_PROTOCOL_HPP
