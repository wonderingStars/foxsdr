// rig_cat.hpp - CAT for a hardware transceiver, in both directions (survey row R17): a FOLLOWER that
// reads an external rig's frequency and mode over a byte stream, and an EMULATOR that answers the same
// commands from FoxSDR's own frequency and mode so a logging program can read and set them. Parsing
// and framing are pure functions; the byte stream is injected; no thread is started anywhere here.
//
// STAND-ALONE, called by nothing yet. No real serial port is opened by any test.
//
// DIALECTS BUILT: Kenwood-style text commands and Yaesu "newcat". NOT BUILT: Icom CI-V. Icom's own CI-V
// reference guides (https://www.icomjapan.com/support/manual/4486/ for the IC-7300MK2, /2271/ for the
// IC-7300) are offered only behind an "AGREE" button whose terms prohibit reproduction, distribution and
// transfer of the manuals; accepting terms is not this lane's to do, the third-party mirrors found
// (manualslib, manualshive) refused the fetch (HTTP 403, a bot-protection page), and a CI-V detail the
// allowed sources do not give is not guessed. Icom CI-V stays UNBUILT until a document is opened freely.
//
// DOCUMENTS OPENED (read 2026-11-11; both are PDFs of the makers' own manuals, read from third-party
// hosts because the makers' own download pages were not reachable by a plain link; their terms are NOT
// VERIFIED beyond what the documents say: Kenwood's text says it "does not support or warrantee this
// documentation in any way"; no licence is stated):
//  * Kenwood, "PC CONTROL COMMAND REFERENCE FOR THE TS-590S TRANSCEIVER", October/01/2010 (28 pages),
//    https://www.passion-radio.fr/fiches/kenwood/ts-590s/ts_590_pc_command_e.pdf. Read: command layout
//    (two letters, parameters, ";" terminator), FA (set / read / answer, 11 digits in Hz: "FA00007000000;"
//    sets 7 MHz), MD (1 LSB, 2 USB, 3 CW, 4 FM, 5 AM, 6 FSK, 7 CW-R, 9 FSK-R; 0 and 8 "None (setting
//    failure)"), the error messages "?;" (syntax), "E;" (communication error), "O;" (receive data sent
//    but processing not completed) and the not-executed message. The IF status layout's field positions
//    were NOT readable from the extracted text and IF is not used. This is the TS-590S's table: other
//    Kenwood models may differ (the TS-590SG already does for some commands, per a search result).
//  * Yaesu, "CAT Operation Reference Manual" for the FT-991A, https://rigpix.com/yaesu/ft991a_cat_manual.pdf
//    (20 pages). Read: "FA" with a nine-digit parameter in Hz ("FA014250000;" sets 14.250000 MHz; the
//    FT-991A's range 000030000 to 470000000), "MD" P1 (0: MAIN RX) then P2 (1 LSB, 2 USB, 3 CW-U, 4 FM,
//    5 AM, 6 RTTY-LSB, 7 CW-L, 8 DATA-LSB, 9 RTTY-USB, A DATA-FM, B FM-N, C DATA-USB, D AM-N, E C4FM),
//    set "MD0P2;", read "MD0;", answer "MD0P2;"; commands are two letters, either case.
// Nothing from Hamlib, flrig or any other program was read. The numbers above are facts about wire formats.
//
// WHAT IS UNPROVEN: everything on a real rig. Both dialects' replies to a malformed or unsupported command,
// timing and the inter-command gap a real rig needs, serial parameters (the documents say the baud rate is
// chosen in the rig's menu; none is assumed: the caller gives it), and VFO B / split / memory behaviour,
// none of which is implemented.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "source/iiod_client.hpp"

namespace cascade::source::rigcat {

enum class Dialect { Kenwood, Yaesu };
const char* dialectName(Dialect d);
bool parseDialect(const std::string& text, Dialect& d);  // "kenwood", "yaesu" (any case)

// The rig's own modes, as the documents list them.
enum class RigMode {
    Unknown, Lsb, Usb, Cw, CwReverse, Fm, Am, Fsk, FskReverse,   // Kenwood's set
    RttyLsb, RttyUsb, DataLsb, DataUsb, DataFm, FmNarrow, AmNarrow, C4fm   // Yaesu's extras
};
const char* rigModeName(RigMode m);

// The rig mode a command digit names; Unknown for one the dialect does not list.
RigMode modeFromCode(Dialect d, char code);
// The code character for a mode in a dialect; 0 when the dialect has no such mode.
char modeToCode(Dialect d, RigMode m);

// FoxSDR's mode names are dsp::modeName's: NFM WFM AM DSB USB LSB CW RAW. A rig mode maps to the
// receiver mode that demodulates it (data and RTTY modes to the sideband they sit on, reverse CW to
// CW, narrow FM to NFM); "" when none fits (C4FM, Unknown). The reverse: USB, LSB, CW, AM, NFM, and
// WFM (approximated by the rig's FM) map; DSB and RAW and anything else give Unknown.
std::string foxModeForRigMode(RigMode m);
RigMode rigModeForFoxMode(const std::string& foxMode);

// --- commands -----------------------------------------------------------------------------

std::string buildReadFrequency(Dialect d);                              // "FA;"
std::string buildReadMode(Dialect d);                                   // "MD;" or "MD0;"
// "FA00007000000;" (Kenwood, 11 digits) or "FA014250000;" (Yaesu, 9 digits). Empty string when the
// frequency is negative, not finite or does not fit the dialect's digits.
std::string buildSetFrequency(Dialect d, double hz);
// "MD2;" or "MD02;". Empty when the dialect has no code for the mode.
std::string buildSetMode(Dialect d, RigMode m);

// --- what a rig (or this emulator) says --------------------------------------------------

struct RigState {
    bool frequencyKnown = false;
    double frequencyHz = 0.0;
    bool modeKnown = false;
    RigMode mode = RigMode::Unknown;
};

enum class AnswerKind { Frequency, Mode, ErrorSyntax, ErrorComm, ErrorBusy, Other, Malformed };

struct Answer {
    AnswerKind kind = AnswerKind::Malformed;
    double frequencyHz = 0.0;
    RigMode mode = RigMode::Unknown;
};

// One complete ";"-terminated message (the ";" included). Frequency answers must have exactly the
// dialect's digit count and only digits; mode answers exactly their parameters ("MD2;" / "MD02;").
// Other well-formed messages (FB..., IF..., ID...) are AnswerKind::Other, not errors. A mode code
// the dialect does not list gives Mode with RigMode::Unknown.
Answer parseAnswer(Dialect d, const std::string& message);

// Splits a byte stream into ";"-terminated messages. Bytes before a message are returned with it
// (a rig may send a leading line feed). A message longer than kMaxMessage is dropped and counted,
// and the parser resynchronises at the next ";". Never throws.
class MessageSplitter {
public:
    static constexpr std::size_t kMaxMessage = 256;
    void feed(const std::uint8_t* data, std::size_t n);
    bool next(std::string& out);
    std::uint64_t dropped() const { return dropped_; }

private:
    std::string buf_;
    std::vector<std::string> ready_;
    std::size_t pos_ = 0;
    bool skipping_ = false;
    std::uint64_t dropped_ = 0;
};

// --- the byte stream -----------------------------------------------------------------------

// A bounded byte pipe: the only thing the follower and the emulator's driver need.
class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual bool write(const void* data, std::size_t n) = 0;
    // Waits up to `timeout` for at least one byte; returns the count (0 on timeout). False when the
    // stream is closed or failed (lastError() says why).
    virtual bool read(void* data, std::size_t max, std::size_t& got, std::chrono::milliseconds timeout) = 0;
    virtual void close() = 0;
    virtual const char* lastError() const = 0;
};

// A ByteStream over a TCP connection (a rig behind a serial-to-network bridge, or the stand-in of the
// tests). `host`/`port` as iiod::connectTcp; null with `error` on failure.
std::unique_ptr<ByteStream> connectTcpByteStream(const std::string& host, std::uint16_t port,
                                                 std::string& error);

// A ByteStream over a serial port (Windows: CreateFile on "\\.\COMn", 8 data bits, no parity, the
// stop bits given). NEVER OPENED BY ANY TEST: UNPROVEN on a real port. Off Windows it answers an
// error. `port` is "COM3" or "\\.\COM12".
struct SerialSettings {
    int baud = 0;  // required: no default is assumed
    int stopBits = 1;
};
std::unique_ptr<ByteStream> openSerialByteStream(const std::string& port, const SerialSettings& s,
                                                 std::string& error);

// --- the follower --------------------------------------------------------------------------

class RigFollower {
public:
    RigFollower(Dialect d, ByteStream& stream) : dialect_(d), stream_(stream) {}

    // Asks for the frequency and the mode and waits for both answers, bounded by `timeout` in total.
    // Unsolicited answers that arrive meanwhile (a rig with auto-information on) update the state
    // too. Returns true when BOTH were answered. A rig error message ("?;", "E;", "O;") is counted
    // and ends the wait early with false. A closed stream is false with lastError().
    bool poll(std::chrono::milliseconds timeout);

    // Takes bytes that arrived by another route (a reader the caller runs), updating the state.
    void consume(const std::uint8_t* data, std::size_t n);

    const RigState& state() const { return state_; }
    std::uint64_t errorsFromRig() const { return rigErrors_; }
    std::uint64_t malformed() const { return malformed_; }
    std::uint64_t droppedBytes() const { return splitter_.dropped(); }
    const std::string& lastError() const { return error_; }

private:
    void apply(const std::string& message);
    Dialect dialect_;
    ByteStream& stream_;
    MessageSplitter splitter_;
    RigState state_;
    std::uint64_t rigErrors_ = 0, malformed_ = 0;
    bool gotFrequency_ = false, gotMode_ = false, sawError_ = false;
    std::string error_;
};

// --- the emulator ---------------------------------------------------------------------------

// What the emulator answers from and applies to: FoxSDR. The callbacks are the wiring lane's.
struct EmulatorHost {
    std::function<double()> frequencyHz;                  // the receiver's tuned frequency
    std::function<std::string()> foxMode;                 // its mode by dsp::modeName's words
    std::function<bool(double)> setFrequencyHz;           // false: refused
    std::function<bool(const std::string&)> setFoxMode;   // false: refused
};

class RigEmulator {
public:
    RigEmulator(Dialect d, EmulatorHost host) : dialect_(d), host_(std::move(host)) {}

    // Feeds bytes from the logging program and returns the bytes to send back, in order. Implemented:
    // FA read and set, MD read and set. Everything else well-formed, and everything malformed, is
    // answered "?;" (the documents' syntax-error message); a set the host refuses is answered "?;"
    // too. A read answers in the dialect's own digit count (a frequency the digits cannot hold is
    // answered "?;" rather than truncated).
    std::string feed(const std::uint8_t* data, std::size_t n);

    std::uint64_t commandsHandled() const { return handled_; }
    std::uint64_t commandsRefused() const { return refused_; }

private:
    std::string handle(const std::string& cmd);
    Dialect dialect_;
    EmulatorHost host_;
    MessageSplitter splitter_;
    std::uint64_t handled_ = 0, refused_ = 0;
};

}  // namespace cascade::source::rigcat
