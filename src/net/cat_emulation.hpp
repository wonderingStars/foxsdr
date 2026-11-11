// net/cat_emulation.hpp - the application answers as ONE KENWOOD and ONE YAESU radio (row N2), receive only, so logging and digital-mode programs
// that know only those radios can follow and set the receiver's frequency and mode and read its signal strength.
//
// WHY THESE TWO MODELS. The Kenwood TS-480 and the Yaesu FT-891 are chosen for their published, complete, short ASCII command references (a
// semicolon-terminated two-letter command, parameters of fixed width) and because the two families (Kenwood-style and Yaesu-style ASCII CAT) are
// the two that logging software and digital-mode programs implement first. This is a judgement, not a measured popularity: no client list was
// consulted (UNVERIFIED that these are the single most-supported models of each maker). A TS-480 client also works against many programs' generic
// "Kenwood" setting; an FT-891 client against the "Yaesu FT-891" entry.
//
// SOURCES (the makers' own documents, opened 2026-11-11; each carries only the maker's "for the user's convenience" notice, no licence, so the
// command formats are used as facts about a wire protocol and the code and words are this project's):
//   * Kenwood, "PC CONTROL COMMAND REFERENCE FOR THE TS-480HX/SAT TRANSCEIVER" (2003/11/28), read from a copy at
//     https://www.radioamatore.info/attachments/059_TS-480_pc_control_command.pdf (kenwood.com refused the automated fetch of its own page).
//   * Yaesu, "FT-891 CAT Operation Reference Book", read from a copy at
//     https://www.rigreference.com/storage/manuals/yaesu/FT-891 CAT Reference Book.pdf--5a6a4c01735456.71645851.pdf (the book itself names
//     yaesu.com as the maker's site).
//   * OmniRig (research only, see the record): https://www.dxatlas.com/omnirig/ and the MIT-licensed https://github.com/VE3NEA/OmniRig
//     (OmniRig.ridl).
//
// WHAT IS ANSWERED. Both dialects: read and set the frequency (FA, FB), the mode (MD), read the signal strength (SM), the identity (ID), the
// information string (IF), power state (PS reads 1 while the receiver runs), auto-information (AI: accepted and ignored, reads 0). Kenwood adds the
// receive VFO select (FR: VFO A or B; FT reads the same and a set is accepted and ignored). Yaesu adds VFO swap and copy (SV, AB, BA) and split off
// (ST0). Everything else is "?;" (not understood), the answer the real radios give.
//
// RECEIVE ONLY. Every transmit command is refused or ignored and counted in `transmitRefused`: Kenwood TX, KY (CW keying), AC (tuner start) and SR
// (reset) answer "E;" (the radio's own "not executed" message); PS0 (power off) answers "E;" and the receiver is not stopped. Yaesu TX1 and TX2,
// KY, ST1, ST2 and PS0 answer "?;"; TX0 (transmit off) and RX-type no-ops are accepted silently. Nothing transmits and nothing is keyed.
//
// WHAT IS CHOSEN WHERE THE DOCUMENTS DO NOT SAY (all UNVERIFIED against a real client):
//   * The S-meter: the receiver gives a 0..1 level; Kenwood answers round(level * 20) (the document's 0000 to 0020), Yaesu round(level * 255) (000 to 255).
//   * Modes the receiver has no equivalent of (FSK, RTTY, C4FM, reversed CW) are refused; WFM reads as FM, DSB as AM, RAW as USB.
//   * Yaesu frequencies are 9 digits, so 999999999 Hz is the most it can say; it accepts 30000 Hz and up where the real radio accepts to 56 MHz.
//   * A set that the radio would answer with nothing, this answers with nothing (Kenwood and Yaesu sets have no answer).
//
// TRANSPORTS. The protocol (CatEmulator) is pure: bytes in, bytes out. serveCatStream runs it over any CatStream; CatSerialService gives it a serial
// port the user names (core/serial_port.hpp), CatTcpService a TCP port (127.0.0.1 unless the user asks for all interfaces). Both are OFF until the
// user starts them. A real port pair needs a virtual-COM driver that is not installed on the build machine: serial operation on a real port is
// UNVERIFIED; it is tested through a named pipe opened by the same SerialPort class, and its refusal path is tested on real (absent) ports.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_CAT_EMULATION_HPP
#define CASCADE_NET_CAT_EMULATION_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cascade::core { class SerialPort; }

namespace cascade::net {

enum class CatDialect : std::uint8_t { KenwoodTS480, YaesuFT891 };
const char* catDialectWord(CatDialect d);   // "kenwood-ts480", "yaesu-ft891"

// What the emulation reads and writes of the receiver. The engine fills these with its real calls (each must be safe from any thread). Any may be
// null: a missing one makes its command answer as the radio does when it cannot (read: the last value or 0; set: refused).
struct RigBackend {
    std::function<double()> frequencyHz;                    // the tuned frequency
    std::function<bool(double)> setFrequencyHz;             // false: outside what the receiver can tune
    std::function<std::string()> mode;                      // the receiver's mode name: NFM WFM AM DSB USB CW LSB RAW
    std::function<bool(const std::string&)> setMode;
    std::function<double()> signalFraction;                 // 0..1
    std::function<bool()> running;                          // the receiver is running
};

// The two VFOs, shared by every connection of one service. The receive VFO is the one the receiver is tuned to; the other is remembered only.
struct RigVfos {
    std::mutex m;
    double freq[2] = {0.0, 0.0};
    int rx = 0;   // 0: VFO A, 1: VFO B
};

struct CatEmuStats {
    std::uint64_t commands = 0;
    std::uint64_t answered = 0;
    std::uint64_t notUnderstood = 0;   // "?;"
    std::uint64_t transmitRefused = 0;
    std::uint64_t notExecuted = 0;     // refused for another reason (power off, reset, a mode the receiver has no equivalent of)
    std::uint64_t setsApplied = 0;
};

class CatEmulator {
public:
    CatEmulator(CatDialect dialect, RigBackend backend, std::shared_ptr<RigVfos> vfos = nullptr);
    // Bytes from the client. Returns the bytes to send back (possibly none). Partial commands are kept for the next call; a command with no
    // terminator in 64 bytes is dropped and answered "?;".
    std::string feed(const char* data, std::size_t n);
    std::string feed(const std::string& s) { return feed(s.data(), s.size()); }
    // One command without its ';'.
    std::string handle(const std::string& command);
    CatEmuStats stats() const { return stats_; }
    CatDialect dialect() const { return dialect_; }

    static constexpr std::size_t kMaxCommandBytes = 64;

private:
    std::string kenwood(const std::string& cmd);
    std::string yaesu(const std::string& cmd);
    double activeFrequency();
    bool tuneActive(double hz);
    char kenwoodModeDigit();
    char yaesuModeChar();
    CatDialect dialect_;
    RigBackend rig_;
    std::shared_ptr<RigVfos> vfos_;
    std::string partial_;
    bool dropping_ = false;   // an over-long command: everything up to its ';' is skipped
    CatEmuStats stats_;
};

// ---- transports --------------------------------------------------------------------------------------------------------------------------
// Bytes to and from a client. read: bytes placed in buf (1..cap), 0 when the bounded wait expired, negative when the stream is finished.
class CatStream {
public:
    virtual ~CatStream() = default;
    virtual int read(char* buf, std::size_t cap) = 0;
    virtual int write(const char* data, std::size_t len) = 0;   // bytes written, negative on failure
};

// Runs `emu` over `stream` until the stream ends or `stop` is set. Returns the number of commands served.
std::uint64_t serveCatStream(CatStream& stream, CatEmulator& emu, const std::atomic<bool>& stop);

struct CatEmuFact {
    std::string code;   // "cat-port-refused", "cat-tcp-refused", "cat-already-running"
    std::string text;   // one sentence; a serial port's NAME may appear (COM3), never a path
};

class CatSerialService {
public:
    CatSerialService(CatDialect dialect, RigBackend backend);
    ~CatSerialService();
    // Opens the port the user named at `baud` and starts answering. False with the fact set when the port cannot be opened.
    bool start(const std::string& portName, int baud, CatEmuFact& fact);
    void stop();
    bool running() const { return running_.load(); }
    CatEmuStats stats() const;

private:
    CatDialect dialect_;
    RigBackend backend_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> commands_{0};
    std::shared_ptr<RigVfos> vfos_;
    std::unique_ptr<core::SerialPort> port_;
    std::mutex statsMu_;
    CatEmuStats last_;
};

class CatTcpService {
public:
    static constexpr int kMaxClients = 4;
    CatTcpService(CatDialect dialect, RigBackend backend);
    ~CatTcpService();
    // `bindAll` false: 127.0.0.1 only. Port 0 asks the OS for a free one (boundPort() says which).
    bool start(std::uint16_t port, bool bindAll, CatEmuFact& fact);
    void stop();   // hard: no thread of this service runs when it returns
    bool running() const { return running_.load(); }
    std::uint16_t boundPort() const { return port_.load(); }
    int clients() const { return clients_.load(); }
    std::uint64_t commandsServed() const { return commands_.load(); }

private:
    void acceptLoop();
    CatDialect dialect_;
    RigBackend backend_;
    std::shared_ptr<RigVfos> vfos_;
    std::thread acceptor_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> port_{0};
    std::atomic<int> clients_{0};
    std::atomic<std::uint64_t> commands_{0};
    std::intptr_t listener_ = -1;
    std::mutex mu_;
    struct ClientRec;
    std::vector<std::unique_ptr<ClientRec>> recs_;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_CAT_EMULATION_HPP
