// rfexplorer_protocol.hpp - the RF Explorer spectrum analyser (the "RF Explorer UART API") as a SWEEP source: the few messages of its
// serial interface as numbers and pure functions, an incremental parser of the stream the device sends, and a sweeper that talks to an ABSTRACT serial link
// (the one tinysa_protocol.hpp defines). Nothing in this file can open a port.
//
// IT IS NOT A LIBRARY BRIDGE. The brief of the radio-bridges lane says an RF Explorer is "a serial sweep device rather than a library": the maker's
// libraries are Python, .NET and Arduino code under the LGPL and nothing a native bridge can load, so this is the tinySA pattern (sweep lines through a
// serial link), with a link that can open the device's 500 kbit/s default (rfexplorer_serial_win.hpp).
//
// THE SOURCES, both opened on 2026-11-12:
//   * the maker's own interface page "RF Explorer UART API interface specification" (https://github.com/RFExplorer/RFExplorer-for-.NET/wiki/
//     RF-Explorer-UART-API-interface-specification, text of the wiki read as a DOCUMENT; the page states no licence, only that its content is "believe
//     to be accurate"): the message syntax of Request_Config (#<Size>C0), AnalyzerConfig (#<Size>C2-F: start, end, top, bottom with the field widths),
//     Request_Hold (CH), Disable_DumpScreen (D0), Current_Setup (#C2-M:), Current_Config (#C2-F:, three layouts by field count), Sweep_data ($S),
//     Sweep_data_ext ($s), Sweep_data_large ($z), the screen dump ($D, 128 x 8 bytes) and the data field list (kHz and Hz units, AdBm = -(byte)/2, the model
//     and mode codes). No LGPL source file was opened.
//   * pyRFExplorer (https://github.com/RFExplorer/pyRFExplorer, Unlicense, pyrfe/rfe.py at commit 59de6166, 2013), the maker's own small client under the
//     public-domain licence, for the cross-check that the host speaks 500000 baud, frames a command as '#' + one size byte (command length + 2) + command,
//     and that a $S frame is '$S' + one count byte + that many -dBm*2 bytes + CR LF. Nothing is copied.
//
// WHAT THE DOCUMENTS DISAGREE ABOUT, AND WHAT THIS FILE DOES (all UNVERIFIED on hardware):
//   * the count byte of $S. The 2013 client reads it as the number of points. The maker's page describes <Sample_points> as "increased by one and
//     multiplied by 16" (with the firmware column blank, and word for word the same as for $s). Both readings are accepted and the one that equals the
//     point count of the device's last Current_Config is used; a count that matches neither is a bad frame. $s uses the page's rule, $z the 16-bit
//     big-endian count; both are checked against the same point count. A sweep frame before any Current_Config has been seen is never interpreted.
//   * the end of line: "0x0A, 0x0D, standard CR + LF". A frame must end with one CR and one LF in either order, which also proves the framing.
//   * how a positive amplitude is written in <Amp_Top> ("4 ASCII digits"): not stated, so this file refuses amplitudes above 0 dBm and writes "%04d".
//   * the serial line settings beyond the baud rate (data bits, parity, stop bits, flow control): not stated; 8N1 without flow control is assumed.
//   * whether Current_Config is answered to Request_Config in every firmware (the 2013 client assumes so): connect() waits a bounded time for one.
//
// RECEIVE ONLY. The spectrum analyser has no transmit stage. The signal-generator commands (C3-, C5-, CP1/CP0, tracking C3-K/k), the baud change, shutdown,
// reboot, LCD, wifi mode and every other configuration command of the page are NOT declared in this file: the only things ever written are Request_Config,
// Disable_DumpScreen, AnalyzerConfig (start, end, top, bottom: the span the person asked for) and Request_Hold. A device that answers as a signal
// generator (model 60 or a C3- message) is refused by name.
//
// PRIVACY. A device's serial number and any text it sends are never kept; only the numbers below are. Nothing here writes to the console or a setting.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/sweep_line.hpp"
#include "source/tinysa_protocol.hpp"

namespace cascade::source::rfexplorer {

inline constexpr int kDefaultBaud = 500000;              // the page: "2400bps and 500Kbps"; pyRFExplorer: 500000
inline constexpr std::size_t kMaxCommandBytes = 64;      // the page: "Size is limited to max 64 bytes"
inline constexpr std::size_t kScreenDumpBytes = 128 * 8; // $D
inline constexpr std::size_t kMaxTextLine = 256;
inline constexpr long kMaxFrequencyKHz = 9999999;        // 7 ASCII digits
inline constexpr int kMainModelGenerator = 60;           // "RFEGEN:60"

// ---- what is written ---------------------------------------------------------------------------------------------------------------
std::string requestConfigCommand();        // "#" 0x04 "C0"
std::string holdCommand();                 // "#" 0x04 "CH"  (stops the analyser's data dump)
std::string dumpScreenOffCommand();        // "#" 0x04 "D0"
// "#" 0x20 "C2-F:" start(7) "," end(7) "," top(4) "," bottom(4): 32 bytes. start < end in whole kHz (rounded), both within 1..kMaxFrequencyKHz,
// -500 <= bottom < top <= 0 dBm. Empty with `whyNot` set otherwise.
std::string analyzerConfigCommand(double startHz, double endHz, int topDbm, int bottomDbm, std::string* whyNot = nullptr);

// ---- what is read ------------------------------------------------------------------------------------------------------------------
struct Setup {            // #C2-M:<Main_Model>,<Expansion_Model>,<Firmware_Version>
    int mainModel = -1;
    int expansionModel = -1;
    std::string firmware; // 5 characters, bounded
};
struct Config {           // #C2-F: three layouts by field count (10: <=1.08, 11: 1.09-1.11, 13: 1.12+)
    long startKHz = 0;
    long stepHz = 0;
    int ampTop = 0;
    int ampBottom = 0;
    long points = 0;
    int expansionActive = 0;
    int mode = -1;        // 0 = spectrum analyser
    long minKHz = 0, maxKHz = 0, maxSpanKHz = 0;
    long rbwKHz = -1;     // -1 = not sent
    int fields = 0;
};
// The text AFTER the "#C2-M:" / "#C2-F:" prefix, without the end of line.
bool parseSetup(const std::string& text, Setup& out, std::string* why = nullptr);
bool parseConfig(const std::string& text, Config& out, std::string* why = nullptr);
const char* mainModelName(int code);  // "WSUB3G"... or "unknown"; only used in sentences
bool isAnalyzerModel(int mainModel);  // every code of the page except the generator

// -(byte)/2 dBm
inline float dbmOfByte(std::uint8_t b) { return -static_cast<float>(b) / 2.0f; }

enum class ItemKind { Setup, Config, Sweep };
struct Item {
    ItemKind kind = ItemKind::Sweep;
    Setup setup;
    Config config;
    std::vector<float> dbm;  // Sweep
    char frame = 'S';        // 'S', 's' or 'z'
};

struct DemuxStats {
    std::uint64_t discardedBytes = 0;   // bytes that belong to no frame (echo, noise, a frame of the generator)
    std::uint64_t sweeps = 0;
    std::uint64_t badFrames = 0;        // a sweep header that matched no point count, or a frame without its CR LF
    std::uint64_t badConfigs = 0;       // a #C2- line that did not parse
    std::uint64_t ignoredLines = 0;     // other '#' lines (#Sn, #K, #T:, #C3-...)
    std::uint64_t screenDumps = 0;      // $D frames skipped
    std::uint64_t generatorMessages = 0;// #C3- / #C5- lines: the device is a signal generator
};

// Incremental parser of everything the device sends. Feed bytes in any chunking.
class Demux {
public:
    // Appends the frames completed by `data` to `out`. Never throws; a malformed frame is counted and skipped, the parser resynchronises on the next '#' or '$'.
    void feed(const std::uint8_t* data, std::size_t n, std::vector<Item>& out);
    void reset();
    long knownPoints() const { return points_; }
    const DemuxStats& stats() const { return stats_; }

private:
    std::vector<std::uint8_t> buf_;
    long points_ = 0;  // from the last Current_Config; 0 = none seen
    DemuxStats stats_;
};

// ---- the sweeper -------------------------------------------------------------------------------------------------------------------
struct Request {
    double startHz = 0.0;
    double stopHz = 0.0;
    int topDbm = -10;
    int bottomDbm = -120;
    int maxIdleReads = 30;   // consecutive empty reads before the device is called silent (the link's own timeout each)
};

enum class Result { Ok, BadRequest, WriteFailed, LinkFailed, TimedOut, NotAnalyzer, Refused };
const char* resultWord(Result r);  // "ok", "bad-request", "write-failed", "link-failed", "timed-out", "not-analyzer", "refused"

class Sweeper {
public:
    explicit Sweeper(tinysa::SerialLink& link) : link_(link) {}

    // Asks for the configuration and waits for it. NotAnalyzer when the device does not answer as a spectrum analyser (a generator, another device).
    Result connect(int maxIdleReads, std::string* why = nullptr);
    // Sets the span; checked against the range and span the device reported; waits for the device to report it back. Refused when it does not take it.
    Result configure(const Request& req, std::string* why = nullptr);
    // The next complete sweep as a line (points and frequencies from the device's own configuration). `timeS` stamps it.
    Result nextLine(core::SweepLine& out, double timeS, int maxIdleReads, std::string* why = nullptr);
    // Best effort: tells the analyser to stop its data dump.
    void hold();

    const Config& config() const { return config_; }
    const Setup& setup() const { return setup_; }
    bool haveConfig() const { return haveConfig_; }
    const DemuxStats& stats() const { return demux_.stats(); }

private:
    // Reads until a sweep is waiting (wantSweep) or a configuration newer than `configsBefore` has arrived, or the link is idle too long.
    Result pump(int maxIdleReads, bool wantSweep, std::uint64_t configsBefore, std::string* why);
    void absorb(const std::vector<Item>& items);

    tinysa::SerialLink& link_;
    Demux demux_;
    Config config_;
    Setup setup_;
    bool haveConfig_ = false;
    bool haveSetup_ = false;
    std::uint64_t configsSeen_ = 0;
    std::vector<Item> pending_;  // sweeps received while waiting for something else
};

}  // namespace cascade::source::rfexplorer
