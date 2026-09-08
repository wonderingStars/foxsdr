// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/gps_reader.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/diag_log.hpp"
// The one acceptance rule, exactly as config.cpp reaches it. Not copied.
#include "gui/scope_view.hpp"

namespace cascade::core {

// The block the worker writes and the reader reads (gps_reader.hpp says why
// it is a block of its own). `done` and `finished` are the hand-over stop()
// waits on: the worker sets `done` under the mutex as its very last act, so a
// reader that has seen it can join without waiting on anything.
struct GpsReader::Shared {
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};  // worker alive and not yet terminal
    Status status;                      // the copy status() returns
    bool fixPending = false;            // set with Fixed, cleared by takeFix()
    std::chrono::steady_clock::time_point startedAt{};
};

namespace {

// Seconds since `from`, on the clock the whole reader keeps time by.
double secondsSince(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

// The sentence type spelled for a status line. Kept beside gpsStatusLine
// so the wording of "Fix from RMC" has one home.
const char* sentenceName(NmeaSentence s) {
    switch (s) {
        case NmeaSentence::GGA: return "GGA";
        case NmeaSentence::RMC: return "RMC";
        case NmeaSentence::GLL: return "GLL";
    }
    return "sentence";
}

// A short formatted string; everything reaching it is a count, a name the
// config sanitiser already capped at 64 bytes, or an OS message. Truncating
// rather than trusting, as the scope readouts do.
std::string fmt(const char* f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    const int n = std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < 0) { return std::string(); }
    buf[sizeof buf - 1] = '\0';
    return std::string(buf);
}

// The product's opener: a SerialPort. Returned as the ByteSource it is, or
// nullptr with the port layer's message.
std::unique_ptr<ByteSource> openSerialPort(const std::string& port, int baud, std::string& error) {
    std::unique_ptr<SerialPort> p = std::make_unique<SerialPort>();
    if (!p->open(port, baud, error)) { return nullptr; }
    return p;
}

}  // namespace

GpsReader::GpsReader() : shared_(std::make_shared<Shared>()) {}

GpsReader::~GpsReader() { stop(); }

void GpsReader::setOpenerForTest(Opener opener) { opener_ = std::move(opener); }

void GpsReader::start(const Options& opts) {
    // The two things that can be wrong before any device is asked. Refused
    // here, on the caller, so a status line appears at once instead of after
    // a worker thread was created to discover it - and so a SerialPort is
    // never constructed for a request the port layer would refuse anyway.
    if (opts.port.empty() || !serialBaudSupported(opts.baud)) {
        stop();
        const char* why = opts.port.empty() ? "no port chosen"
                                            : "baud rate is not one the port can be set to";
        diagWarnf("gps: %s could not be opened: %s", loggableSerialPortName(opts.port).c_str(), why);
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->status = Status{};
        shared_->status.state = State::Failed;
        shared_->status.port = opts.port;
        shared_->status.baud = opts.baud;
        shared_->status.error = std::string("could not be opened: ") + why;
        shared_->fixPending = false;
        return;
    }
    launch(nullptr, opts);
}

void GpsReader::start(std::unique_ptr<ByteSource> source, const Options& opts) {
    launch(std::move(source), opts);
}

void GpsReader::launch(std::unique_ptr<ByteSource> source, Options opts) {
    // At most one thread is ever OURS: the previous listen is ended and
    // joined (or abandoned to finish on its own, in which case it holds a
    // block this reader no longer reads) before this one starts, so two
    // workers can never race for one status and a fix from the old port can
    // never be reported against the new one.
    stop();
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->status = Status{};
        shared_->status.state = State::Listening;
        shared_->status.port = opts.port;
        shared_->status.baud = opts.baud;
        shared_->fixPending = false;
        shared_->startedAt = std::chrono::steady_clock::now();
        shared_->done = false;
    }
    shared_->stopping = false;
    shared_->running = true;
    worker_ = std::thread(&GpsReader::run, shared_, std::move(source), std::move(opts), opener_);
}

void GpsReader::run(std::shared_ptr<Shared> sh, std::unique_ptr<ByteSource> source, Options opts,
                    Opener opener) {
    // Marks the block finished on every way out of this function, after the
    // terminal state and the log line, so that stop() never joins a thread
    // that still has a line to write.
    struct Finished {
        Shared& sh;
        ~Finished() {
            {
                std::lock_guard<std::mutex> lock(sh.mutex);
                sh.done = true;
            }
            sh.finished.notify_all();
        }
    } finished{*sh};

    // The port is opened HERE, on the worker, because a wedged USB driver can
    // sit inside CreateFile for seconds and a frame loop cannot wait for it.
    //
    // EVERY LOG LINE NAMES THE PORT BY loggableSerialPortName. The field
    // accepts any printable text and the port layer opens a typed file path
    // exactly as it opens a port, so the name given here can be
    // "\\.\C:\Users\alice\gps.nmea" - and this log rides inside uploaded
    // crash reports that PRIVACY.md promises carry no file path. A port
    // name passes through; anything else is logged as what kind of thing it
    // was and how long. The status line on screen keeps the real name.
    const std::string portForLog = loggableSerialPortName(opts.port);
    if (!source) {
        std::string error;
        source = opener ? opener(opts.port, opts.baud, error)
                        : openSerialPort(opts.port, opts.baud, error);
        if (!source) {
            // The port layer prefixes its message with the name ("COM3: The
            // system cannot find..."); the status line and the log both
            // name the port themselves, so that prefix is taken back off.
            const std::string prefix = opts.port + ": ";
            if (error.compare(0, prefix.size(), prefix) == 0) { error.erase(0, prefix.size()); }
            std::lock_guard<std::mutex> lock(sh->mutex);
            sh->status.elapsedS = secondsSince(sh->startedAt);
            if (sh->stopping.load()) {
                // The open failed BECAUSE stop() cancelled it (or the user
                // stopped while the driver was still deciding): that is a
                // stop, not a fault, and the status must not say "could not
                // be opened" about a port nobody was allowed to finish
                // opening.
                diagLogf("gps: stopped by user while opening %s (%.1f s)", portForLog.c_str(),
                         sh->status.elapsedS);
                sh->status.state = State::Idle;
            } else {
                diagWarnf("gps: %s could not be opened: %s", portForLog.c_str(), error.c_str());
                sh->status.error = "could not be opened: " + error;
                sh->status.state = State::Failed;
            }
            sh->running = false;
            return;
        }
    }
    diagLogf("gps: listening on %s at %d", portForLog.c_str(), opts.baud);

    NmeaLineAssembler assembler;
    assembler.reset();
    std::vector<std::string> lines;
    char buf[512];

    State terminal = State::Idle;
    std::string error;
    NmeaFix accepted;
    bool found = false;

    for (;;) {
        // stop() is checked BEFORE the read, not only after it, so a stop
        // issued while the previous read was returning is honoured without
        // one more bounded wait on the device. It also catches a stop that
        // landed while the open was in progress: the port opened after all,
        // and is closed below without a single read.
        if (sh->stopping.load()) { terminal = State::Idle; break; }

        const int n = source->read(buf, sizeof buf);
        if (n < 0) {
            // Terminal by contract: the device is unplugged, the pipe's server
            // has gone, the descriptor is dead. Reading again would be a bug.
            terminal = State::Failed;
            error = "stopped answering (unplugged, or closed by another program)";
            break;
        }
        if (n > 0) {
            lines.clear();
            assembler.feed(buf, static_cast<std::size_t>(n), lines);

            std::lock_guard<std::mutex> lock(sh->mutex);
            Status& status = sh->status;
            status.bytes += static_cast<std::uint64_t>(n);
            for (const std::string& line : lines) {
                NmeaFix fix;
                const NmeaStatus st = parseNmeaSentence(line, fix);
                status.nmea.record(st, fix);
                if (st != NmeaStatus::Ok) { continue; }
                if (fix.sentence == NmeaSentence::GGA) {
                    // The counts a user watches while waiting: satellites
                    // climbing is "wait", satellites at 0 is "move the antenna".
                    status.satellites = fix.satellites;
                    status.hdop = fix.hdop;
                    status.fixQuality = fix.fixQuality;
                }
                // THE acceptance rule, applied once, here. A receiver's own
                // "valid" is necessary, not sufficient: (0,0) with quality 1
                // is a bench receiver's honest answer and is waited out.
                if (!found && fix.valid
                    && cascade::gui::receiverPositionAcceptable(fix.latDeg, fix.lonDeg)) {
                    accepted = fix;
                    found = true;
                }
            }
            status.overlongDropped = assembler.overlongDropped();
            status.noiseBytes = assembler.noiseBytes();
            if (found) { terminal = State::Fixed; break; }
        }
        if (secondsSince(sh->startedAt) >= opts.timeoutS) {
            terminal = State::TimedOut;
            break;
        }
    }

    // The port is CLOSED before the state is published, so "position set" on
    // the status line always implies "port released" - the user's other GPS
    // software can have it back the instant the fix is shown, and a test can
    // assert the order.
    source.reset();

    std::lock_guard<std::mutex> lock(sh->mutex);
    Status& status = sh->status;
    status.elapsedS = secondsSince(sh->startedAt);
    status.error = error;
    status.state = terminal;
    if (terminal == State::Fixed) {
        status.fix = accepted;
        sh->fixPending = true;
    }
    sh->running = false;

    // Counts, names and durations only. NEVER the fix, never a sentence: this
    // log rides inside uploaded crash reports (see the header).
    const NmeaSummary& s = status.nmea;
    switch (terminal) {
        case State::Fixed:
            diagLogf("gps: fix after %llu sentences in %.1f s (%d satellites, HDOP %.1f)",
                     static_cast<unsigned long long>(s.sentences), status.elapsedS,
                     status.satellites, status.hdop);
            break;
        case State::TimedOut:
            diagLogf("gps: no fix within %.0f s (%llu sentences, %llu checksum failures, "
                     "%llu overlong)",
                     opts.timeoutS, static_cast<unsigned long long>(s.sentences),
                     static_cast<unsigned long long>(s.checksumFailures),
                     static_cast<unsigned long long>(status.overlongDropped));
            break;
        case State::Failed:
            diagWarnf("gps: %s %s after %.1f s (%llu sentences)", portForLog.c_str(),
                      error.c_str(), status.elapsedS,
                      static_cast<unsigned long long>(s.sentences));
            break;
        case State::Idle:
            diagLogf("gps: stopped by user after %.1f s", status.elapsedS);
            break;
        case State::Listening:
            break;
    }
}

void GpsReader::stop() {
    if (!worker_.joinable()) { return; }
    shared_->stopping = true;
#if defined(_WIN32)
    // Ask the OS to abandon whatever synchronous call the worker is inside -
    // which, if it is inside anything, is the driver's open. A driver that
    // honours cancellation fails CreateFileW with ERROR_OPERATION_ABORTED at
    // once and the worker exits through the "stopped while opening" path;
    // one that does not is what the bounded wait below is for. The reads
    // are overlapped and are not affected (they are bounded by their own
    // event wait, and the stop flag is checked before each one).
    ::CancelSynchronousIo(static_cast<HANDLE>(worker_.native_handle()));
#endif
    bool done = false;
    {
        std::unique_lock<std::mutex> lock(shared_->mutex);
        done = shared_->finished.wait_for(lock, kOpenAbandonWait, [&] { return shared_->done; });
    }
    if (done) {
        worker_.join();
        return;
    }
    // Still inside the driver. The worker is abandoned WITH its block (it
    // holds the shared_ptr) and this reader starts a fresh one so it reads
    // Idle from here on; the abandoned worker will see the stop flag the
    // moment the driver answers, close the port if it opened, and finish
    // its block that nobody reads. Said in the log by port name and count,
    // because a port the OS takes seconds to open is a fact worth having in
    // a crash report from a machine we cannot see.
    std::string port;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        port = shared_->status.port;
    }
    diagWarnf("gps: %s is still being opened by its driver after %lld ms; the read is "
              "abandoned and the port will be released when the driver answers",
              loggableSerialPortName(port).c_str(),
              static_cast<long long>(kOpenAbandonWait.count()));
    worker_.detach();
    shared_ = std::make_shared<Shared>();
}

bool GpsReader::listening() const { return shared_->running.load(); }

GpsReader::Status GpsReader::status() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    Status copy = shared_->status;
    // elapsedS is frozen into the block at the terminal transition; while
    // listening it is live, so the status line's "(12 s)" moves.
    if (copy.state == State::Listening) { copy.elapsedS = secondsSince(shared_->startedAt); }
    return copy;
}

bool GpsReader::takeFix(NmeaFix& out) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (!shared_->fixPending) { return false; }
    out = shared_->status.fix;
    shared_->fixPending = false;
    return true;
}

void GpsReader::clearResult() {
    if (shared_->running.load()) { return; }
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->status = Status{};
    shared_->fixPending = false;
}

// The wording table in gps_reader.hpp is the contract; every row is pinned
// in tests/test_gps_reader.cpp. Nothing here formats a coordinate, and the
// Status it reads carries none outside `fix`, which this function does not
// touch beyond its sentence type.
std::string gpsStatusLine(const GpsReader::Status& s) {
    using State = GpsReader::State;
    switch (s.state) {
        case State::Idle:
            return std::string();
        case State::Listening: {
            std::string line = fmt("Listening on %s at %d - ", s.port.c_str(), s.baud);
            // ~200 bytes is two or three sentences' worth at any baud: a
            // stream that long with nothing verifying is framing garbage,
            // and the most likely cause is the wrong rate.
            if (s.bytes >= 200 && s.nmea.sentences == 0) {
                line += "nothing readable yet, is the baud right?";
            } else if (s.satellites >= 0) {
                line += fmt("%d satellites, ", s.satellites);
                if (std::isfinite(s.hdop)) { line += fmt("HDOP %.1f, ", s.hdop); }
                line += "no fix yet";
            } else {
                line += fmt("%llu sentences, no fix yet",
                            static_cast<unsigned long long>(s.nmea.sentences));
            }
            line += fmt(" (%.0f s)", s.elapsedS);
            return line;
        }
        case State::Fixed:
            if (s.satellites >= 0) {
                std::string line = fmt("Fix: %d satellites", s.satellites);
                if (std::isfinite(s.hdop)) { line += fmt(", HDOP %.1f", s.hdop); }
                return line + " - position set";
            }
            return fmt("Fix from %s - position set", sentenceName(s.fix.sentence));
        case State::Failed:
            // `error` is a complete predicate ("could not be opened: ...",
            // "stopped answering ..."), so the port name reads as its subject.
            return s.port + " " + s.error;
        case State::TimedOut:
            // Three different failures, three different pieces of advice,
            // told apart by counts the status already carries (the log line
            // always could; the screen must too, because the person who can
            // act on it is looking at the screen). Not one byte: the port is
            // wrong or the receiver is off, and sky has nothing to do with
            // it. Sentences that verified: the port and the baud are right
            // and the receiver simply cannot see enough satellites. Bytes
            // that never verified: the rate or the protocol.
            if (s.bytes == 0) {
                return fmt("Nothing arrived from %s in %.0f s - is this the GPS's port, and is "
                           "the receiver switched on?",
                           s.port.c_str(), s.elapsedS);
            }
            if (s.nmea.sentences > 0) {
                std::string line = fmt("No fix within %.0f s (%llu sentences", s.elapsedS,
                                       static_cast<unsigned long long>(s.nmea.sentences));
                if (s.satellites >= 0) { line += fmt(", %d satellites", s.satellites); }
                return line + ") - the receiver is answering but cannot see enough sky; try "
                              "again outdoors.";
            }
            return fmt("Nothing readable in %.0f s (%llu bytes) - is the receiver set to NMEA "
                       "at this baud?",
                       s.elapsedS, static_cast<unsigned long long>(s.bytes));
    }
    return std::string();
}

}  // namespace cascade::core
