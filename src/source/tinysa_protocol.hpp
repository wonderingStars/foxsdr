// tinysa_protocol.hpp - the tinySA / tinySA Ultra "scanraw" sweep exchange as
// numbers and pure functions, plus a sweep source that talks to an ABSTRACT
// serial link. Nothing in this file can open a port.
//
// THE ONE SOURCE. The maker's own USB-interface page, https://tinysa.org/wiki/
// pmwiki.php?n=Main.USBInterface (opened 2026-11-11; the page states no licence
// and sits on the maker's wiki, not inside a GPL repository). Everything
// below is taken from these entries of it, quoted exactly where a number
// matters:
//   scanraw: "usage: scanraw {start(Hz)} {stop(Hz)} [points] [option]"
//            "The measured data is the level in dBm and is send as '{'
//            ('x' LSB MSB)*points '}', the 16 bit value least significant
//            byte first."
//            "The option, when present, can be either 0,1,2 or 3 being the sum
//            of 1=unbuffered and 2=continuous"
//            "To get the dBm level from the 16 bit data, divide by 32 and
//            subtract 128 for the tinySA and 174 for the tinySA Ultra."
// The tinySA firmware is GPL and was not read. Nothing from the community
// tools that wrap the device was read either.
//
// WHAT THE PAGE DOES NOT SAY (so this file does not guess at it, and each is
// UNVERIFIED ON HARDWARE): the command terminator (kCommandTerminator is a
// choice), whether the device echoes the command or prints a prompt before the
// '{' (the parser discards every byte before the first '{', whatever it is),
// the format of the "version" and "info" replies (so the model is NAMED BY THE
// CALLER, never read from the device), and a limit on scanraw points (this
// file caps at core::kSweepMaxPointsPerLine). There is no checksum in the
// frame: its integrity checks are the framing bytes and the exact point count
// asked for.
//
// THERE IS NO tinySA ON THE BENCH. Everything is proven against frames written
// in the tests from the sentence above (tests/test_sweep_tinysa.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/sweep_line.hpp"

namespace cascade::source::tinysa {

enum class Model { Unknown, TinySa, TinySaUltra };

// dBm = raw / 32 - offset (the page: "divide by 32 and subtract 128 for the
// tinySA and 174 for the tinySA Ultra"). Unknown has no offset: a decode with it
// is refused.
double levelOffsetDb(Model m);

// The choice described above; the page documents none.
inline constexpr const char* kCommandTerminator = "\r\n";

// The scanraw option word: 1 = unbuffered, 2 = continuous (the page: "the sum
// of 1=unbuffered and 2=continuous"). This file never asks for continuous.
inline constexpr int kOptionUnbuffered = 1;

// The command line for one sweep, or an empty string and a reason. start/stop
// are whole Hz, start < stop, points in [2, kSweepMaxPointsPerLine], option in
// 0..3.
std::string scanrawCommand(double startHz, double stopHz, std::size_t points, int option,
                           std::string* whyNot = nullptr);

enum class FrameStatus {
    NeedMore,    // the frame is not finished
    Done,        // a whole frame of the asked number of points
    BadFraming,  // a byte that is neither 'x' (start of a point) nor '}' where one must be
    BadLength,   // '}' before the asked number of points, or a further 'x' after them
    Truncated    // finish() called with a frame open
};

// Incremental parser for one scanraw reply. Feed whatever the port delivered, in
// any chunking. Bytes before the first '{' are discarded (echo, prompt - the
// page does not describe them). After Done or an error the parser stays in that
// state until reset().
class ScanrawParser {
public:
    explicit ScanrawParser(std::size_t expectedPoints);
    FrameStatus feed(const std::uint8_t* data, std::size_t n);
    // The input ended (timeout / port closed): Truncated when a frame is open or
    // nothing was seen, otherwise the terminal state already reached.
    FrameStatus finish();
    void reset();
    FrameStatus status() const { return status_; }
    std::size_t pointsSeen() const { return raw_.size(); }
    std::size_t discardedBytes() const { return discarded_; }
    // The raw 16-bit values of a Done frame.
    const std::vector<std::uint16_t>& raw() const { return raw_; }

private:
    enum class St { Seek, PointOrEnd, Lsb, Msb, Finished };
    std::size_t expected_;
    St st_ = St::Seek;
    FrameStatus status_ = FrameStatus::NeedMore;
    std::vector<std::uint16_t> raw_;
    std::uint8_t lsb_ = 0;
    std::size_t discarded_ = 0;
};

// The highest level the decode accepts, dBm. Our own sanity bound (no tinySA
// input takes more, and a wrong model or garbage decodes far above it), NOT a
// device specification.
inline constexpr double kPlausibleMaxDb = 30.0;

// Raw values -> a sweep line. Refuses (empty result, reason) an unknown model,
// a size that is not what was asked, a level above kPlausibleMaxDb.
bool decodeScanraw(const std::vector<std::uint16_t>& raw, Model model, double startHz,
                   double stopHz, core::SweepLine& out, std::string* whyNot = nullptr);

// --- the link and the source ------------------------------------------------

// What the source needs of a serial port. A fake in the tests; WinSerialLink
// (serial_port_win.hpp) for a real one.
class SerialLink {
public:
    virtual ~SerialLink() = default;
    // Write all of it; false on failure.
    virtual bool write(const std::uint8_t* data, std::size_t n) = 0;
    // Read up to cap bytes, waiting at most the link's own timeout. 0 = nothing
    // arrived in that time; -1 = the link failed or closed.
    virtual long read(std::uint8_t* buf, std::size_t cap) = 0;
};

struct SweepRequest {
    double startHz = 0.0;
    double stopHz = 0.0;
    std::size_t points = 0;
    Model model = Model::Unknown;
    double timeS = 0.0;   // stamped on the line (the caller's clock)
    int maxIdleReads = 20; // consecutive empty reads before the sweep is called lost
};

enum class SweepResult { Ok, BadRequest, WriteFailed, LinkFailed, TimedOut, Malformed, Refused };

// One sweep: write the command, read until the frame is whole, decode.
// Never blocks longer than maxIdleReads empty reads of the link.
SweepResult sweepOnce(SerialLink& link, const SweepRequest& req, core::SweepLine& out,
                      std::string* why = nullptr);

}  // namespace cascade::source::tinysa
