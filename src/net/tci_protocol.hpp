// net/tci_protocol.hpp - the TCI (Transceiver Control Interface) protocol layer: parse, decide, format. No sockets, no engine.
//
// SOURCE. Written from "TCI Protocol 2.1" (Expert Electronics / "Expert Group" LLC), the PDF in https://github.com/ExpertSDR3/TCI
// ("TCI Protocol 2.1.pdf"), read in full in the session that wrote this file. Its licence is the MIT licence, printed at the front of the
// PDF and in the repository's LICENSE file ("Copyright (c) 2023 Expert Group LLC, Taganrog"); the notice is reproduced in
// docs/THIRD-PARTY.md. What is reproduced here is a wire format and the names of its commands, which is what interoperability needs; no
// other program's source was read. The PDF names no default port (50001 is what the facts report found other programs using).
//
// WHAT IS SPOKEN (a deliberate subset; docs/API.md 6.22 lists every command and what is not answered):
//   initialisation: VFO_LIMITS IF_LIMITS TRX_COUNT CHANNELS_COUNT DEVICE RECEIVE_ONLY MODULATIONS_LIST PROTOCOL READY
//   bidirectional:  START STOP DDS IF VFO MODULATION RX_ENABLE RX_FILTER_BAND SQL_ENABLE SQL_LEVEL
//   receive-only answers: TRX TUNE TX_ENABLE (always false), DRIVE and TUNE_DRIVE (ignored)
//   per-client:     AUDIO_START AUDIO_STOP AUDIO_SAMPLERATE AUDIO_STREAM_SAMPLE_TYPE AUDIO_STREAM_CHANNELS AUDIO_STREAM_SAMPLES
//   binary:         the receiver audio stream (RX_AUDIO_STREAM, the 64-byte header of section 3.4)
// EVERYTHING ELSE IS IGNORED WITHOUT A REPLY, which is what the specification says a server does with a command it does not know
// (section 3.1 and note 5 of 4.7). There is NO TRANSMIT: TRX, TUNE and TX_ENABLE always answer "false", a binary TX_AUDIO_STREAM from a
// client is dropped, CW commands are ignored, and the engine session behind a connection has no transmitter op on its profile at all.
// There is no I/Q stream and no RX_SENSORS (FoxSDR has no calibrated dBm scale, and a made-up number would be a false reading).
//
// RECEIVERS. A TCI receiver number is the INDEX into the radios the engine has, in ascending radio order: TCI receiver 0 is the lowest
// radio. When a radio is added or removed the numbering shifts and the connection says so (TRX_COUNT and the whole state again).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_TCI_PROTOCOL_HPP
#define CASCADE_NET_TCI_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "foxsdr_api.h"

namespace cascade::net {

// ---- limits: every one is a refusal of hostile input, not a promise about real clients ---------------------------------------------
inline constexpr std::size_t kMaxTciTextBytes = 2048;         // one WebSocket text message
inline constexpr std::size_t kMaxTciCommandsPerMessage = 32;
inline constexpr std::size_t kMaxTciArgs = 8;
inline constexpr std::size_t kMaxTciNameBytes = 32;
inline constexpr std::size_t kMaxTciArgBytes = 64;
inline constexpr int kTciHeaderBytes = 64;                    // section 3.4: the Stream header is still 64 bytes

struct TciCommand {
    std::string name;               // lower case
    std::vector<std::string> args;  // empty arguments already discarded (4.7 note 1)
};

struct TciParse {
    std::vector<TciCommand> commands;
    bool oversize = false;  // the message was longer than kMaxTciTextBytes: nothing in it was read
    int dropped = 0;        // commands that were malformed, over-long or past the per-message limit
};

// Splits one text message into commands: `name:arg1,arg2;`, `name;`. The case of letters does not matter (3.1) so names are lowered.
// A malformed command (no name, a name over kMaxTciNameBytes or with characters other than letters, digits and `_`, more than
// kMaxTciArgs arguments, an argument over kMaxTciArgBytes) is dropped and the rest of the message still read.
TciParse parseTciMessage(std::string_view text);

// 4.7 note 2 and 3: an unparseable numeric argument makes the command a read request; a logical is true for "true" or "1" in any case.
bool tciParseInteger(const std::string& s, std::int64_t& out);   // optional leading '-', digits only, at most 15 digits
bool tciParseBool(const std::string& s);

// ---- the engine's side ----------------------------------------------------------------------------------------------------------------
struct TciRadioView {
    bool running = false;
    double centreHz = 0.0;
    double vfoOffsetHz = 0.0;
    double tunedHz = 0.0;
    double sampleRateHz = 0.0;
    double bandwidthHz = 0.0;
    double squelchDb = -200.0;     // -200 = always open
    std::uint32_t demod = 0;       // FOXAPI_DEMOD_*
    double limitLoHz = 0.0;        // the tuner's range when the radio reports one, else 0 / 0
    double limitHiHz = 0.0;
};

// What one connection asks of the engine. The engine implements it over one internal session (engine/server_host.cpp); a test over a
// stub. Called from the connection's own thread only.
class TciBackend {
public:
    virtual ~TciBackend() = default;
    // The radio numbers (1..8) the engine has, ascending.
    virtual std::vector<int> radios() = 0;
    // False when the radio is not there or its figures could not be read this time.
    virtual bool readRadio(int radio, TciRadioView& out) = 0;
    // Hands commands over in order; returns how many NEW tickets the engine gave (each produces one result).
    virtual int submit(const std::vector<FoxCommand>& commands) = 0;
    // Collects the results that are ready; returns how many it took.
    virtual int pollResults() = 0;
    // The radio's finished audio (mono, 48 kHz, float) from `cursor` (0 = join live). Returns frames written (>= 0) or a negative FOXAPI_*.
    virtual int readAudio(int radio, std::uint64_t& cursor, float* out, std::size_t capFrames) = 0;
};

// ---- names -----------------------------------------------------------------------------------------------------------------------
const char* tciModeName(std::uint32_t foxDemod);                // upper case, "" for a mode TCI cannot select (RAW)
bool tciModeFromName(const std::string& name, std::uint32_t& foxDemod);   // case-insensitive

// ---- the audio stream (section 3.4) -----------------------------------------------------------------------------------------------
enum class TciSampleType : std::uint32_t { Int16 = 0, Int24 = 1, Int32 = 2, Float32 = 3 };
inline constexpr std::uint32_t kTciRxAudioStream = 1;   // StreamType::RX_AUDIO_STREAM

// An integer-factor decimator (48 kHz to 24, 12 or 8 kHz; factor 1 passes through): a windowed-sinc low-pass at 0.9 of the new Nyquist.
class TciDecimator {
public:
    explicit TciDecimator(int factor = 1);
    int factor() const { return factor_; }
    void process(const float* in, std::size_t n, std::vector<float>& out);
    void reset();

private:
    int factor_;
    std::vector<float> taps_;
    std::vector<float> hist_;   // the last taps-1 inputs
    int phase_ = 0;
};

// One header + payload, little-endian, exactly the layout of section 3.4. `interleaved` holds frames*channels floats.
std::string tciPackAudio(std::uint32_t receiver, std::uint32_t sampleRate, TciSampleType type, std::uint32_t channels,
                         std::uint32_t count, const float* interleaved, std::size_t frames);

// ---- one connection --------------------------------------------------------------------------------------------------------------
class TciConnection {
public:
    explicit TciConnection(TciBackend& backend) : be_(backend) {}

    // The server's first words: the initialisation commands, READY, then the state (3.1). One entry per command (one text message each).
    std::vector<std::string> begin();
    // A client's text message; the replies owed at once. Accepted sets go to the engine and are echoed by tick().
    std::vector<std::string> onText(std::string_view text);
    // Every ~30 ms (4.7 note 7): collects results, pushes what changed and the echo of what the client set. `nowMs` is any monotone clock.
    std::vector<std::string> tick(std::int64_t nowMs);

    // The radio numbers this client has started the audio stream for.
    std::vector<int> audioRadios() const;
    // Mono 48 kHz audio of `radio` in, finished binary messages out (none until a whole packet has accumulated).
    std::vector<std::string> audioPackets(int radio, const float* mono48k, std::size_t frames);

    struct Counters {
        std::uint64_t commands = 0;      // commands parsed
        std::uint64_t ignored = 0;       // unknown, out of range, or not spoken here
        std::uint64_t oversize = 0;      // messages refused for size
        std::uint64_t dropped = 0;       // malformed commands dropped
        std::uint64_t submitted = 0;     // engine commands handed over
        std::uint64_t txRefused = 0;     // transmit requests answered "false"
    };
    const Counters& counters() const { return c_; }
    int audioRate() const { return rate_; }
    TciSampleType audioType() const { return type_; }
    int audioChannels() const { return channels_; }
    int audioSamples() const;
    std::size_t outstanding() const { return outstanding_; }

private:
    enum Group : unsigned { kDds = 1, kIf = 2, kVfo = 4, kMode = 8, kRun = 16, kSql = 32, kAll = 63 };
    struct AudioStream {
        bool on = false;
        TciDecimator dec;
        std::vector<float> pend;   // mono at the client's rate
        std::uint32_t count = 0;
    };
    void refreshRadios();
    int radioOf(const std::string& arg) const;                 // TCI receiver index -> radio number, 0 when out of range
    int indexOf(int radio) const;
    void emitGroup(int radio, const TciRadioView& v, unsigned groups, std::vector<std::string>& out) const;
    void dispatch(const TciCommand& cmd, std::vector<std::string>& replies, std::vector<FoxCommand>& cmds);
    void force(int radio, unsigned groups) { forced_[radio] |= groups; }
    static std::string num(double v);

    TciBackend& be_;
    std::vector<int> radios_;
    struct Sent {
        long long centre = 0, offset = 0, tuned = 0, bandwidth = 0, squelch = 0, rate = 0;
        std::uint32_t demod = 0;
        bool running = false;
    };
    std::map<int, Sent> last_;
    std::map<int, unsigned> forced_;
    std::map<int, double> sqlLevel_;            // the last enabled squelch level per radio
    std::map<int, AudioStream> audio_;
    bool anyRunningSent_ = false;
    bool anyRunningKnown_ = false;
    long long ifLimitSent_ = -1;
    std::size_t outstanding_ = 0;
    std::int64_t lastSubmitMs_ = 0;
    std::int64_t now_ = 0;
    int rate_ = 48000;
    int channels_ = 2;
    int samples_ = 0;                           // 0 = the default for the rate
    TciSampleType type_ = TciSampleType::Float32;
    Counters c_;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_TCI_PROTOCOL_HPP
