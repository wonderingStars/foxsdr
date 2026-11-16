// cat_link.hpp - FoxSDR FOLLOWS A RIG over CAT (screen lane S5b, rows G1-16 CAT panadapter, G3-18a OmniRig, G3-18b CAT over a virtual COM port).
//
// Three ways in, one at a time, OFF by default, and every one is receive-side: FoxSDR reads the rig's frequency and mode and (the person's choice) moves its own centre and mode to match, or answers a logging program that
// reads and sets FoxSDR's frequency and mode.
//   follow    a serial port to a rig (Kenwood text commands or Yaesu newcat, source/rig_cat.hpp): FoxSDR asks the rig for its frequency and mode and follows them
//   emulate   a serial port that is one end of a virtual COM pair: FoxSDR answers like a rig to the logging program on the other end (frequency and mode only)
//   omnirig   OmniRig (a free program the person installs): FoxSDR reads the rig OmniRig shows and follows it
//
// WHAT FOXSDR CAN SEND TO A RIG, the whole list: "FA;" (read the frequency), "MD;" (Kenwood: read the mode) and "MD0;" (Yaesu: read the mode). Nothing else leaves: every write to a rig goes through GatedStream, which
// refuses any message not on that list (so a future bug cannot key a transmitter); to OmniRig nothing at all is written (the port handed to the client refuses both of its writes). The emulator's answers are not commands
// to a rig: they are replies to the logging program, built by rig_cat.hpp from FoxSDR's own frequency and mode, and every transmit command a logging program sends is answered "?;".
//
// The serial port and OmniRig are opened by the link's own thread, never by a test: tests hand in stand-ins through CatLinkSeams. Nothing here is downloaded; when OmniRig is absent the sentence says what is needed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/omnirig_client.hpp"
#include "source/rig_cat.hpp"

struct FoxEngineApi;
struct FoxSession;

namespace cascade::engine::wirec {

// ---------------------------------------------------------------------------------------- what may be sent to a rig
// The complete list of messages the link can write to a rig, in every dialect.
const std::vector<std::string>& catCommandsSent();
// True when `message` (one ";"-terminated CAT message) is on that list for the dialect.
bool isCatReadCommand(cascade::source::rigcat::Dialect d, const std::string& message);

// A byte stream that writes only what isCatReadCommand allows. A write carrying anything else is refused whole (nothing reaches the inner stream), counted and remembered.
class GatedStream final : public cascade::source::rigcat::ByteStream {
public:
    GatedStream(cascade::source::rigcat::ByteStream& inner, cascade::source::rigcat::Dialect d) : inner_(inner), dialect_(d) {}
    bool write(const void* data, std::size_t n) override;
    bool read(void* data, std::size_t max, std::size_t& got, std::chrono::milliseconds timeout) override { return inner_.read(data, max, got, timeout); }
    void close() override { inner_.close(); }
    const char* lastError() const override { return inner_.lastError(); }
    std::uint64_t blocked() const { return blocked_; }
    std::uint64_t written() const { return written_; }
    const std::string& lastBlocked() const { return lastBlocked_; }

private:
    cascade::source::rigcat::ByteStream& inner_;
    cascade::source::rigcat::Dialect dialect_;
    std::uint64_t blocked_ = 0, written_ = 0;
    std::string lastBlocked_;
};

// ---------------------------------------------------------------------------------------- the link
struct CatLinkConfig {
    std::string mode = "off";          // off | follow | emulate | omnirig
    std::string port;                  // "COM3" (follow, emulate)
    std::string dialect = "kenwood";   // kenwood | yaesu
    int baud = 0;                      // 0 = not set: nothing is opened (the rig's menu decides, none is assumed)
    int stopBits = 1;
    int rig = 1;                       // OmniRig's Rig1 or Rig2
    bool followFrequency = true;
    bool followMode = true;
    bool operator==(const CatLinkConfig& o) const {
        return mode == o.mode && port == o.port && dialect == o.dialect && baud == o.baud && stopBits == o.stopBits && rig == o.rig && followFrequency == o.followFrequency && followMode == o.followMode;
    }
};

struct CatLinkSeams {
    const FoxEngineApi* table = nullptr;                 // how the link reaches the engine: the table and ONE internal CAT session (opened by `openSession`, closed by the link)
    std::function<FoxSession*()> openSession;
    // Both are called on the link's own thread. Defaults (empty): rigcat::openSerialByteStream and the Windows COM bridge.
    std::function<std::unique_ptr<cascade::source::rigcat::ByteStream>(const std::string& port, const cascade::source::rigcat::SerialSettings&, std::string& error)> openSerial;
    std::function<std::unique_ptr<cascade::core::rig::OmniRigRigPort>(int rig, std::string& error)> openOmniRig;
    std::chrono::milliseconds pollEvery{250};
};

struct CatLinkStatus {
    std::string kind = "off";    // the mode that is configured
    std::string state = "off";   // off | opening | following | emulating | error
    std::string error;           // a sentence when state is error (or the last thing that went wrong)
    bool rigKnown = false;       // a frequency was read from the rig
    double rigHz = 0.0;
    std::string rigMode;         // the rig's own mode name, "" when unknown
    std::uint64_t polls = 0, answers = 0, rigErrors = 0, applied = 0, refused = 0, blocked = 0, served = 0;
};

class CatLink {
public:
    CatLink() = default;
    ~CatLink() { stop(); }
    CatLink(const CatLink&) = delete;
    CatLink& operator=(const CatLink&) = delete;

    // Stops whatever runs, then (for a mode other than off with what it needs given) starts a thread that opens the port and runs. Returns at once; the outcome is in status().
    void configure(const CatLinkConfig& cfg, const CatLinkSeams& seams);
    void stop();                 // a hard stop: no thread of the link runs after it returns
    bool running() const;
    CatLinkStatus status() const;

    // The sentence that says what OmniRig is and where it comes from (nothing is downloaded by FoxSDR).
    static std::string omniRigNeededSentence();

    struct Run;   // (public so the thread's helpers in cat_link.cpp can name it; nothing of it is reachable from here)

private:
    std::shared_ptr<Run> run_;
    std::thread thread_;
};

// The Windows COM bridge to OmniRig (omnirig_com.cpp): READ-ONLY by construction (the two writes of the port return false without touching COM). Null with the "needed" sentence when OmniRig is not registered on
// this computer; opening it starts the OmniRig program, so it is called only by the link's thread after the person chose OmniRig. Off Windows: null with a sentence.
std::unique_ptr<cascade::core::rig::OmniRigRigPort> openOmniRigCom(int rig, std::string& error);

}  // namespace cascade::engine::wirec
