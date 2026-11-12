// kiwisdr_protocol.hpp - the KiwiSDR audio-stream wire format as pure functions:
// builders for the text commands a client sends, and a bounds-checked parser
// for the binary frames the server sends back. No sockets, no threads, no
// httplib - kiwisdr_client.hpp is the connection that uses this.
//
// PROVENANCE (CONTRIBUTING.md, clean room). The ONLY protocol source for this
// file is the outline in the pre-release facts report (section 3, "KiwiSDR,
// WebSDR, OpenWebRX"): a WebSocket on the path /<timestamp>/SND; text commands
// SET auth t=kiwi p=<pw>, SET ident_user=<name>, SET mod= low_cut= high_cut=
// freq=, SET compression=, SET AR OK in= out=, SET keepalive about once a
// second; binary frames that start with a 3-byte tag MSG / SND / W/F / EXT; an
// SND header of flags (1 byte), sequence (4 bytes, little-endian) and S-meter
// (2 bytes, big-endian) with RSSI = 0.1 * smeter - 127 dBm; audio that is
// 16-bit, IMA ADPCM when flag 0x10 is set and PCM otherwise; and the server
// messages too_busy, badp (codes 1-7), down and redirect. No source code of
// kiwiclient, kiwirecorder, the KiwiSDR server or any other program that talks
// to a KiwiSDR was read, fetched or searched for. IMA ADPCM is the public
// standard algorithm (the 89-entry step table and the 16-entry index table
// are the standard's), implemented here from that standard.
//
// WHERE THE OUTLINE RUNS OUT, THIS FILE DOES NOT GUESS SILENTLY: every
// decision beyond the outline sits behind ONE named constant or function in
// namespace assumed below, and docs/feature-records/kiwi-client.md lists each
// under ASSUMPTIONS and, as a question, under PROTOCOL QUESTIONS. One live session
// (2026-11-11, record section "Live test 2026-11-11") has since confirmed or
// contradicted most of them; each constant below says which.
//
// BOUNDS. Every length that arrives is checked before it is used. A frame
// longer than kMaxFrameBytes is refused before anything is decoded or
// allocated; the number of samples a frame can yield is derived from the bytes
// actually present (never from a field the sender chose), and is therefore at
// most 2 * kMaxFrameBytes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cascade::source::kiwi {

// --- limits ----------------------------------------------------------------

// The longest frame (tag included) the parser looks at. The audio frames the
// outline describes are a few hundred to a few thousand bytes; 64 KiB is far
// above that and far below anything that could hurt. A frame above it is
// ParseError::TooLarge.
constexpr std::size_t kMaxFrameBytes = 64 * 1024;

// The longest MSG text that is parsed; a longer text is cut here (ParsedFrame::messageTruncated).
constexpr std::size_t kMaxMessageTextBytes = 4096;

// The longest redirect target, password and user name the builders accept.
constexpr std::size_t kMaxRedirectTargetChars = 512;
constexpr std::size_t kMaxPasswordChars = 128;
constexpr std::size_t kMaxIdentChars = 64;

// The tag and the SND header, from the outline: "SND" + flags(1) + sequence(4)
// + smeter(2).
constexpr std::size_t kTagBytes = 3;
constexpr std::size_t kSndHeaderBytes = kTagBytes + 1 + 4 + 2;
constexpr std::uint8_t kFlagAdpcm = 0x10;

// --- the decisions the outline does not make (ASSUMPTIONS) -------------------
namespace assumed {

// The audio rate the server streams at, in Hz. NOT in the outline; the
// earlier design notes say 12 kHz. Sent in AR OK and reported to the caller.
// CONFIRMED LIVE: the receiver announced sample_rate=12000.000000 and audio_rate=12000.
constexpr int kAudioRateHz = 12000;

// SET AR OK in=<kArOkIn> out=<kArOkOut>: the outline names the command and
// its two fields but not their meaning. in = the rate the server sends, out =
// the rate this client plays at; no resampling is done, so both are the same.
constexpr int kArOkIn = kAudioRateHz;
constexpr int kArOkOut = kAudioRateHz;

// freq= is sent in kilohertz (decimal, three places = 1 Hz resolution). The
// outline does not give the unit. CONFIRMED LIVE: kHz (77.500 reached the 77.5 kHz station).
constexpr double kFreqUnitHz = 1000.0;

// 16-bit PCM samples are big-endian on the wire (the outline names only the
// S-meter's byte order; the S-meter being big-endian is the only hint).
constexpr bool kPcmBigEndian = true;

// IMA ADPCM: the low nibble of each byte is the earlier sample (the usual WAV
// packing), the decoder starts at predictor 0 / index 0 on a new connection
// and its state CARRIES OVER from one frame to the next. CONFIRMED LIVE (no frame header,
// 120 payload bytes = 240 samples): the decoded audio carries the station's 1 Hz keying.
constexpr bool kAdpcmLowNibbleFirst = true;

// badp=0 means "password accepted"; the outline names codes 1-7 as the
// refusals. 0 is therefore not a refusal. CONFIRMED LIVE for 0 (the receiver answered badp=0
// to an empty password); codes 1-7 not seen.
constexpr int kBadpAcceptedCode = 0;

// A MSG frame's text may be preceded by one separator byte after the tag.
constexpr char kMsgSeparator = ' ';

// Percent-encode a command value (password, user name): every byte outside
// [A-Za-z0-9-._~] becomes %XX. Assumes the server decodes it as a URI
// component; a plain alphanumeric value is unaffected either way.
std::string encodeValue(const std::string& raw);

// The redirect target as shown to the user: control characters replaced, cut at
// kMaxRedirectTargetChars. NOT decoded (the outline does not say whether it is
// encoded).
std::string sanitiseRedirectTarget(const std::string& raw);

}  // namespace assumed

// --- command builders -------------------------------------------------------
//
// Each returns the text of ONE WebSocket text frame. An argument that cannot
// be put in a command safely (a mode with a space in it, a password above the
// limit, the I/Q mode this client does not decode) returns an EMPTY string and
// nothing is sent.

// An empty password sends "p=" with nothing after it (assumption: the outline
// does not say what a server expecting no password wants).
std::string buildAuth(const std::string& password);
std::string buildIdentUser(const std::string& name);
// mode: lower-case letters and digits, 1 to 8 of them. "iq" is refused (I/Q
// mode is not built). lowCutHz / highCutHz: signed passband edges in Hz
// relative to the carrier. frequencyHz: the dial frequency; sent in kilohertz.
std::string buildTune(const std::string& mode, int lowCutHz, int highCutHz, double frequencyHz);
std::string buildCompression(bool adpcm);
std::string buildAudioRate(int in, int out);
std::string buildKeepAlive();

// The WebSocket path: "/<timestamp>/SND".
std::string buildSoundPath(std::uint64_t timestamp);

// --- frames -----------------------------------------------------------------

enum class FrameType { Msg, Snd, Waterfall, Ext };

enum class ParseError {
    None,
    TooShort,    // fewer bytes than the tag or the header
    TooLarge,    // above kMaxFrameBytes
    UnknownTag,  // not MSG / SND / W/F / EXT (a client may ignore it)
    BadPayload,  // a PCM payload with an odd number of bytes
};

// IMA ADPCM decoder state; one per connection, carried across frames.
struct AdpcmState {
    std::int32_t predictor = 0;
    std::int32_t index = 0;
};

// Decodes `count` bytes of IMA ADPCM (two samples per byte) onto `out`
// (appended), updating `state`. The standard algorithm; the nibble order is
// assumed::kAdpcmLowNibbleFirst.
void decodeAdpcm(const std::uint8_t* data, std::size_t count, AdpcmState& state,
                 std::vector<std::int16_t>& out);

// The key=value words of a MSG text, tolerant: words are separated by any
// whitespace or NUL, a word with no '=' is a key with an empty value, an
// empty key is dropped, and nothing is decoded.
struct KeyValue {
    std::string key;
    std::string value;
    bool hasValue = false;
};
std::vector<KeyValue> parseKeyValues(const std::string& text);

enum class MessageKind { Info, TooBusy, BadPassword, Down, Redirect };

struct ServerMessage {
    MessageKind kind = MessageKind::Info;
    int code = 0;                 // too_busy / badp value; -1 when it was not a number
    std::string redirectTarget;   // sanitised, for Redirect
    std::vector<KeyValue> words;  // every word, for logging
};

struct SndFrame {
    std::uint8_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint16_t smeterRaw = 0;
    double rssiDbm = 0.0;  // 0.1 * smeterRaw - 127
    bool adpcm = false;
    std::vector<std::int16_t> samples;  // reused by the caller between frames
};

struct ParsedFrame {
    FrameType type = FrameType::Msg;
    ServerMessage message;         // Msg
    SndFrame snd;                  // Snd
    std::size_t payloadBytes = 0;  // everything after the tag, for W/F and EXT
    bool messageTruncated = false;  // Msg: the text was longer than kMaxMessageTextBytes; only the front was parsed
};

// Parses one whole binary frame. On any error `out` is left unspecified and
// nothing was read outside [data, data+len). `adpcm` is advanced only by an
// SND frame with the ADPCM flag that parsed without error. `out.snd.samples`
// is cleared and refilled (its capacity is kept between calls).
ParseError parseFrame(const std::uint8_t* data, std::size_t len, AdpcmState& adpcm,
                      ParsedFrame& out);

// RSSI in dBm from the raw S-meter word.
inline double rssiDbm(std::uint16_t smeterRaw) {
    return 0.1 * static_cast<double>(smeterRaw) - 127.0;
}

const char* parseErrorText(ParseError e);

}  // namespace cascade::source::kiwi
