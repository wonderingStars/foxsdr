// gps_reader.hpp - one read of the receiver's position from a GPS on a
// serial port.
//
// A beta tester asked for this in one sentence: "if you have a GPS receiver
// you can pull the location straight from there instead of typing it in."
// That is the whole feature. The reader opens the port, listens until the
// receiver says where it is, hands that ONE position to the application, and
// closes the port. It is not a tracker: the antenna does not move between
// launches, the position is persisted exactly as a typed one is, and a port
// held open for the life of the session would be a port the user's other
// software could not use.
//
// OWNERSHIP AND THREADS. The reader owns a std::thread. start() creates it;
// it opens the port (on the worker, so a slow or wedged driver never stalls a
// frame), reads, assembles, parses, and exits on the first ACCEPTABLE fix, on
// the timeout, on a dead source, or on stop(). The destructor calls stop(),
// which joins - an object that could be destroyed with its thread alive is
// how the satellites plugin died 23 times (a joinable std::thread destroyed
// with an instance alive; see MEMORY.md), and that lesson is applied here
// before the first crash report rather than after.
//
// HOW LONG stop() TAKES, in two cases that must be kept apart. While the
// worker is READING, it checks the stop flag before every read and a read is
// bounded by the source (SerialPort::kReadTimeoutMs, 200 ms), so the join
// returns inside that plus scheduling; that is why ByteSource insists on a
// SHORT read bound. While the worker is still OPENING the port there is no
// such bound: CreateFileW on a Bluetooth SPP port whose puck is switched off
// sits in the RFCOMM connect attempt for tens of seconds, and a wedged USB
// driver can sit in its create for as long as it likes. A stop() that joined
// through that would freeze the frame loop with it - the Stop key doing
// nothing, the window not closing - and past five seconds the hang watchdog
// would write the wait up as a hang of this application's making. So stop()
// first asks the OS to abandon the open (CancelSynchronousIo on Windows:
// drivers that honour it fail the create with ERROR_OPERATION_ABORTED at
// once), then waits kOpenAbandonWait for the worker to finish, and if it has
// not, DETACHES it. That is safe, and it is the reason the worker never
// touches the GpsReader object: everything it writes lives in a block the
// worker holds by shared_ptr, so a reader destroyed while an abandoned open
// is still in the driver leaves the worker writing into a block only it can
// still see. The abandoned worker closes the port when the driver finally
// answers; the reader reads Idle from a fresh block meanwhile. The wait is
// one of the bounded waits the shutdown budget counts
// (tests/test_shutdown_budget.cpp discovers it by name).
//
// WHAT "ACCEPTABLE" MEANS, AND WHY IT IS DECIDED HERE. A fix is taken when
// the sentence declares one (NmeaFix::valid) AND the pair passes
// cascade::gui::receiverPositionAcceptable - finite, on the globe, and not
// (0,0). The reader includes gui/scope_view.hpp for that predicate exactly as
// config.cpp does, so there is one rule at every door; a receiver that
// honestly reports (0,0) on a test bench is waited out like a receiver with no
// fix. The GUI then applies the fix through AppWindow::applyReceiverPosition,
// which runs the same predicate again. That is not belt and braces for its
// own sake: applyReceiverPosition is the ONLY path that also moves the map
// pages, the scope and the coverage accumulator, and a GPS fix that bypassed
// it would set a position the rest of the application did not know about.
//
// THE STATUS SNAPSHOT is what the GUI draws at frame rate. It is copied out
// under a mutex - a struct of counters, an enum, two short strings and the
// fix - because the alternative, a handful of atomics read at different
// instants, would let a status line say "listening" beside a fix that had
// already arrived. The GUI takes the fix ONCE, through takeFix(): the status
// keeps reporting Fixed for as long as the reader is left alone, but the
// position is applied exactly one time, so a frame loop polling every 16 ms
// cannot re-apply it (and reset the coverage map) sixty times a second.
//
// PRIVACY - THIS IS NOT NEGOTIABLE. The diagnostic log rides inside crash
// reports that are uploaded, and PRIVACY.md promises that a position is never
// sent. So this file logs port names, baud rates, sentence counts, satellite
// counts, HDOP, elapsed time and error text, and NEVER a latitude, a
// longitude, an altitude or the text of a sentence. Not at debug level, not
// truncated, not "just the first digits". tests/test_gps_reader.cpp feeds a
// fix and then searches the log ring for the coordinate digits and for "$G";
// a line that carried either fails the suite.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_GPS_READER_HPP
#define CASCADE_CORE_GPS_READER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/nmea.hpp"
#include "core/serial_port.hpp"

namespace cascade::core {

class GpsReader {
public:
    // How long to wait for a fix before giving up. A cold receiver indoors
    // needs 30-60 s to see enough satellites; a warm one outdoors answers in
    // a few seconds. 60 s is generous for the first and costs nothing for
    // the second, and the user can press Stop at any moment.
    static constexpr double kDefaultTimeoutS = 60.0;

    enum class State {
        Idle,       // never started, or stop() completed without a fix
        Listening,  // port open (or opening), waiting for an acceptable fix
        Fixed,      // an acceptable fix arrived; the port is already closed
        Failed,     // the port could not be opened, or the source died; see error
        TimedOut,   // the timeout passed with no acceptable fix; the port is closed
    };

    struct Options {
        std::string port;   // "COM3", "/dev/ttyUSB0", or a device path (test seam)
        int baud = kDefaultGpsBaud;
        double timeoutS = kDefaultTimeoutS;
    };

    // Everything the GUI draws, taken as one consistent copy.
    struct Status {
        State state = State::Idle;
        std::string port;                     // as given to start()
        int baud = 0;
        double elapsedS = 0.0;                // since start(); frozen at the terminal state
        std::uint64_t bytes = 0;              // raw bytes received
        NmeaSummary nmea;                     // sentences, checksum failures, fixes...
        std::uint64_t overlongDropped = 0;    // from the assembler: the wrong-baud tell
        std::uint64_t noiseBytes = 0;
        int satellites = -1;                  // from the latest GGA, -1 until one is seen
        double hdop = NmeaFix::kAbsent;       // from the latest GGA
        int fixQuality = -1;                  // from the latest GGA
        std::string error;                    // Failed: why. Empty otherwise.
        // The accepted fix. Meaningful only when state == Fixed; a copy of
        // the sentence that ended the listen, so the GUI can show satellites
        // and HDOP beside "position set".
        NmeaFix fix;
    };

    // How the worker gets its ByteSource when start(const Options&) was
    // called: a function of the port name and baud that returns an open
    // source, or nullptr with `error` filled. The product's opener is a
    // SerialPort; setOpenerForTest() swaps in one that blocks, or fails, or
    // records what it was asked, so that stop() during a slow open can be
    // exercised on a bench with no port that could be slow.
    using Opener = std::function<std::unique_ptr<ByteSource>(const std::string& port, int baud,
                                                             std::string& error)>;

    GpsReader();
    ~GpsReader();  // stop() and join (or abandon, see the header)
    GpsReader(const GpsReader&) = delete;
    GpsReader& operator=(const GpsReader&) = delete;

    // Opens `opts.port` at `opts.baud` on the worker thread and listens.
    // Returns at once: an open failure surfaces as State::Failed with
    // `error` set, not as a return value, so the GUI has ONE place to look.
    // Calling start() while a read is in progress stops that read first
    // (join included), so at most one thread ever exists. A baud outside
    // kSerialBaudRates or an empty port name goes straight to Failed.
    void start(const Options& opts);

    // The test seam, and the reason ByteSource exists: the same loop over a
    // caller-supplied source. `opts.port` and `opts.baud` are used for the
    // status text only; nothing is opened. The reader owns `source` from
    // here and destroys it on the worker after the loop ends, so a test's
    // fake must be safe to delete from a thread other than the one that made
    // it (a plain object is).
    void start(std::unique_ptr<ByteSource> source, const Options& opts);

    // Ends the listen and JOINS - or, when the worker is still inside the
    // port driver's open after kOpenAbandonWait, abandons it (the header
    // says why that is safe). Bounded by the source's read timeout while
    // reading and by kOpenAbandonWait while opening. The state afterwards is
    // whatever the worker reached: a fix that landed before the stop is kept
    // (Fixed), a listen cut short returns to Idle, and an abandoned open
    // reads Idle. Safe to call when nothing is running, and from the
    // destructor.
    void stop();

    // The bound on stop() while the worker is parked in the driver's open.
    // Long against any open that is going to succeed (a USB CDC port opens
    // in milliseconds), short against the five-second hang threshold and
    // the shutdown budget - both of which count it.
    static constexpr std::chrono::milliseconds kOpenAbandonWait{1000};

    // Test seam: the opener used by start(const Options&) instead of a
    // SerialPort. Takes effect for the next start(); an empty function
    // restores the SerialPort. Not for the product.
    void setOpenerForTest(Opener opener);

    // True while the worker thread exists and has not reached a terminal
    // state. The GUI shows its Stop key against this.
    bool listening() const;

    // A consistent copy. Cheap enough for every frame: one mutex, a few
    // hundred bytes.
    Status status() const;

    // The accepted fix, ONCE. Returns true and fills `out` on the first call
    // after a fix is accepted; false on every later call and whenever there
    // is no fix. The state stays Fixed either way (the status line keeps
    // saying so); only the hand-over is single-shot.
    bool takeFix(NmeaFix& out);

    // Forgets a terminal result so the status line reads Idle again, e.g.
    // when the user changes the port. No-op while listening (stop() first).
    void clearResult();

private:
    // EVERYTHING THE WORKER TOUCHES, and nothing else. Held by shared_ptr
    // from both the reader and the worker, so a worker abandoned inside a
    // driver's open (see stop()) can outlive the reader and still have
    // somewhere to write its ending. The reader replaces its own pointer
    // with a fresh block when it abandons one, so the abandoned worker's
    // late "could not be opened" never reaches the screen after the user
    // pressed Stop. Defined in the .cpp; the header need not know its shape.
    struct Shared;

    // Both public start() overloads end here. A NULL `source` means "open
    // one on the worker through opener_ (a SerialPort unless a test swapped
    // it)" - the open cost and its failure both live off the GUI thread that
    // way; a non-null source is the test seam's, used as is. Stops any
    // previous listen (join or abandon included), resets the status, records
    // the start time, and starts worker_ on run().
    void launch(std::unique_ptr<ByteSource> source, Options opts);

    // The loop the worker runs, STATIC on purpose: it may only reach the
    // block it was handed, never the GpsReader, because the reader may be
    // gone by the time an abandoned open returns. Opens the port when handed
    // none, then reads, assembles, parses, and updates the shared status
    // under the block's mutex after every read. Exits on: an acceptable fix,
    // elapsed >= timeoutS, a negative read (source gone), or the stop flag.
    // Destroys `source` (closing the port) BEFORE publishing a terminal
    // state, and marks the block finished LAST.
    static void run(std::shared_ptr<Shared> sh, std::unique_ptr<ByteSource> source, Options opts,
                    Opener opener);

    std::thread worker_;
    std::shared_ptr<Shared> shared_;
    Opener opener_;  // empty: open a SerialPort
};

// PURE: the status line the GUI prints, so the wording is table-tested once
// rather than composed in three panels. Examples, one per state:
//   Idle       ""  (the GUI draws nothing)
//   Listening  "Listening on COM3 at 9600 - 4 sentences, no fix yet (12 s)"
//              "Listening on COM3 at 9600 - 6 satellites, HDOP 2.4, no fix yet (12 s)"
//              "Listening on COM3 at 9600 - nothing readable yet, is the baud right? (12 s)"
//                (bytes > 0 and no sentence has verified after ~200 bytes)
//   Fixed      "Fix: 8 satellites, HDOP 1.1 - position set"
//              "Fix from RMC - position set"  (no GGA seen, so no counts)
//   Failed     "COM3 could not be opened: <error>"
//   TimedOut   "Nothing arrived from COM3 in 60 s - is this the GPS's port, and is the receiver switched on?"
//                (not one byte: wrong port, or the receiver is off)
//              "No fix within 60 s (58 sentences, 3 satellites) - the receiver is answering but cannot see enough sky; try again outdoors."
//                (sentences verified: port and baud are right; the count is omitted without a GGA)
//              "Nothing readable in 60 s (3000 bytes) - is the receiver set to NMEA at this baud?"
//                (bytes, none of which verified: the rate or the protocol)
// Never a coordinate: this string is shown on screen only, but a function
// that could format one would be one copy-paste away from the log.
std::string gpsStatusLine(const GpsReader::Status& s);

}  // namespace cascade::core

#endif  // CASCADE_CORE_GPS_READER_HPP
