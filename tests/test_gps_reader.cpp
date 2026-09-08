// test_gps_reader.cpp - the GPS reader against a scripted byte source.
//
// There is no GPS and no COM port on this bench, so the reader is driven by a
// fake ByteSource that hands over chunks on a schedule: a sentence in three
// fragments, a run of "no fix" sentences, a receiver honestly reporting (0,0),
// a corrupted line, a source that dies, a source that never says anything.
// Every rule in gps_reader.hpp is pinned here, and each check was taken red by
// breaking the behaviour it names before this file was accepted.
//
// The last block is the privacy check and is not optional: the diagnostic
// log rides inside uploaded crash reports, and PRIVACY.md promises a position
// is never sent. After a fix at a distinctive latitude the whole log ring is
// searched for the coordinate digits, in decimal and in NMEA form, and for
// the start of any sentence. One hit fails the suite.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/gps_reader.hpp"

#include "core/diag_log.hpp"
#include "core/nmea.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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

using cascade::core::ByteSource;
using cascade::core::DiagLog;
using cascade::core::GpsReader;
using cascade::core::NmeaFix;
using cascade::core::NmeaSentence;
using Clock = std::chrono::steady_clock;

namespace {

// Everything a test wants to know about a fake after the reader has taken
// ownership of it: whether it has been destroyed (the port-closed proxy) and
// how many reads it answered. Shared, because the fake itself is gone by the
// time the question is asked.
struct Probe {
    std::atomic<bool> destroyed{false};
    std::atomic<int> reads{0};
};

// A scripted ByteSource. Each step waits `delayMs` and then delivers its
// chunk; after the script the source either reports nothing (0) every
// `idleMs` or, when `dieAtEnd`, reports itself gone (-1) once and forever.
class FakeSource final : public ByteSource {
public:
    struct Step {
        std::string chunk;
        int delayMs = 0;
    };

    FakeSource(std::shared_ptr<Probe> probe, std::vector<Step> steps, int idleMs, bool dieAtEnd)
        : probe_(std::move(probe)), steps_(std::move(steps)), idleMs_(idleMs), dieAtEnd_(dieAtEnd) {}
    // The destructor is SLOW on purpose. The header promises the source is
    // destroyed (the port closed) before a terminal state is published; a
    // reader that published first and closed afterwards would leave a gap of
    // nanoseconds, which a 2 ms poll would almost never land in. Twenty
    // milliseconds of closing makes the wrong order observable every run.
    ~FakeSource() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        probe_->destroyed = true;
    }

    int read(char* buf, std::size_t cap) override {
        ++probe_->reads;
        if (next_ < steps_.size()) {
            const Step& s = steps_[next_++];
            std::this_thread::sleep_for(std::chrono::milliseconds(s.delayMs));
            const std::size_t n = s.chunk.size() < cap ? s.chunk.size() : cap;
            std::memcpy(buf, s.chunk.data(), n);
            return static_cast<int>(n);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(idleMs_));
        return dieAtEnd_ ? -1 : 0;
    }

private:
    std::shared_ptr<Probe> probe_;
    std::vector<Step> steps_;
    std::size_t next_ = 0;
    int idleMs_;
    bool dieAtEnd_;
};

std::string framed(const char* payload) { return cascade::core::nmeaFrame(payload) + "\r\n"; }

// 51.50735 N, 0.12776 W: 51 deg 30.4410 min, 0 deg 7.6656 min.
const char* kFixPayload = "GPGGA,123519.00,5130.4410,N,00007.6656,W,1,08,1.1,35.0,M,46.9,M,,";
const char* kNoFixPayload = "GPGGA,123519.00,,,,,0,00,,,M,,M,,";
const char* kOriginPayload = "GPGGA,123519.00,0000.0000,N,00000.0000,E,1,05,2.0,0.0,M,0.0,M,,";
const char* kBadLatPayload = "GPGGA,123519.00,9530.0000,N,00007.6656,W,1,08,1.1,35.0,M,46.9,M,,";
const char* kRmcVoidPayload = "GPRMC,123519.00,V,,,,,,,080926,,,N";
// 53.79648 N, 1.54785 W: 53 deg 47.7888 min, 1 deg 32.8710 min.
const char* kRmcFixPayload = "GPRMC,123519.00,A,5347.7888,N,00132.8710,W,0.0,0.0,080926,,,A";

GpsReader::Options opts(double timeoutS = 30.0) {
    GpsReader::Options o;
    o.port = "FAKE1";
    o.baud = 9600;
    o.timeoutS = timeoutS;
    return o;
}

// A source with nothing scripted: silent (0 every idleMs) or dead (-1).
const std::vector<FakeSource::Step> kNoSteps;

bool terminal(GpsReader::State s) { return s != GpsReader::State::Listening; }

// Polls status() from THIS thread while the worker runs - which is also the
// "snapshot readable from another thread at any time" check, taken a few
// hundred times per test - until the state is terminal or `limitMs` passes.
// Records whether the source was already destroyed at the first sight of the
// terminal state, because that ordering is a promise of the header.
struct Outcome {
    GpsReader::Status status;
    bool destroyedAtTerminal = false;
    double waitedMs = 0.0;
};

Outcome waitForEnd(GpsReader& r, const Probe& probe, int limitMs) {
    const auto t0 = Clock::now();
    Outcome o;
    for (;;) {
        o.status = r.status();
        o.waitedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (terminal(o.status.state)) {
            o.destroyedAtTerminal = probe.destroyed.load();
            return o;
        }
        if (o.waitedMs > limitMs) { return o; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

int main() {
    // The ring is searched at the end for anything a position could leak
    // through; start it empty so every line examined is this file's.
    DiagLog::instance().resetForTest();
    DiagLog::instance().configure(std::string(), false);

    // --- a fix in three fragments -> Fixed, correct, source closed first ---
    {
        const std::string s = framed(kFixPayload);
        auto probe = std::make_shared<Probe>();
        std::vector<FakeSource::Step> steps = {
            {s.substr(0, 20), 10}, {s.substr(20, 25), 10}, {s.substr(45), 10}};
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, steps, 50, false), opts());
        CHECK(r.listening());
        const Outcome o = waitForEnd(r, *probe, 3000);
        CHECK(o.status.state == GpsReader::State::Fixed);
        CHECK(o.destroyedAtTerminal);
        CHECK(!r.listening());
        CHECK(o.status.port == "FAKE1");
        CHECK(o.status.baud == 9600);
        CHECK(o.status.bytes == s.size());
        CHECK(o.status.nmea.sentences == 1u);
        CHECK(o.status.nmea.positionSentences == 1u);
        CHECK(o.status.nmea.validFixes == 1u);
        CHECK(o.status.satellites == 8);
        CHECK_NEAR(o.status.hdop, 1.1, 1e-9);
        CHECK(o.status.fixQuality == 1);
        CHECK(o.status.elapsedS > 0.0);
        CHECK(o.status.error.empty());

        // The fix is handed over exactly once, and the state stays Fixed.
        NmeaFix fix;
        CHECK(r.takeFix(fix));
        CHECK_NEAR(fix.latDeg, 51.50735, 1e-6);
        CHECK_NEAR(fix.lonDeg, -0.12776, 1e-6);
        CHECK(fix.sentence == NmeaSentence::GGA);
        CHECK(fix.valid);
        NmeaFix again;
        CHECK(!r.takeFix(again));
        CHECK(r.status().state == GpsReader::State::Fixed);

        // The thread stopped at the fix: the third fragment was the last
        // read, and nothing asked the source for more afterwards.
        r.stop();
        CHECK(probe->reads.load() == 3);
        CHECK(probe->destroyed.load());

        // elapsedS is frozen at the terminal state.
        const double e1 = r.status().elapsedS;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK_NEAR(r.status().elapsedS, e1, 1e-12);

        // clearResult forgets the terminal state.
        r.clearResult();
        CHECK(r.status().state == GpsReader::State::Idle);
        CHECK(cascade::core::gpsStatusLine(r.status()).empty());
    }

    // --- no fix for a while, then a fix; counters; status mid-listen -------
    {
        auto probe = std::make_shared<Probe>();
        std::vector<FakeSource::Step> steps;
        for (int i = 0; i < 4; ++i) { steps.push_back({framed(kNoFixPayload), 20}); }
        // Two sentences in one chunk, plus a sentence the reader must ignore.
        steps.push_back({framed("GPVTG,054.7,T,034.4,M,005.5,N,010.2,K") + framed(kFixPayload), 20});
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, steps, 50, false), opts());

        // Seen while still listening: no-fix sentences counted, no fix.
        bool sawListeningWithSentences = false;
        for (int i = 0; i < 400; ++i) {
            const GpsReader::Status st = r.status();
            if (st.state == GpsReader::State::Listening && st.nmea.sentences >= 1
                && st.nmea.validFixes == 0) {
                sawListeningWithSentences = true;
                CHECK(st.satellites == 0);
                CHECK(st.fixQuality == 0);
                CHECK(st.elapsedS > 0.0);
                const std::string line = cascade::core::gpsStatusLine(st);
                CHECK(contains(line, "Listening on FAKE1 at 9600"));
                CHECK(contains(line, "no fix yet"));
                break;
            }
            if (terminal(st.state)) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(sawListeningWithSentences);

        const Outcome o = waitForEnd(r, *probe, 3000);
        CHECK(o.status.state == GpsReader::State::Fixed);
        CHECK(o.status.nmea.sentences == 6u);
        CHECK(o.status.nmea.positionSentences == 5u);
        CHECK(o.status.nmea.validFixes == 1u);
        CHECK(o.status.nmea.ignored == 1u);
        CHECK(o.status.nmea.checksumFailures == 0u);
        CHECK(o.status.satellites == 8);
        NmeaFix fix;
        CHECK(r.takeFix(fix));
        CHECK_NEAR(fix.latDeg, 51.50735, 1e-6);
        CHECK_NEAR(fix.lonDeg, -0.12776, 1e-6);
        const std::string line = cascade::core::gpsStatusLine(o.status);
        CHECK(line == "Fix: 8 satellites, HDOP 1.1 - position set");
    }

    // --- a valid fix at (0,0) is waited out; the next real fix is taken ----
    {
        auto probe = std::make_shared<Probe>();
        std::vector<FakeSource::Step> steps = {
            {framed(kOriginPayload), 10}, {framed(kOriginPayload), 10}, {framed(kFixPayload), 150}};
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, steps, 50, false), opts());

        // After the origin sentences the reader is still listening, with the
        // receiver's own "valid" counted but not believed.
        bool sawOriginRefused = false;
        for (int i = 0; i < 400; ++i) {
            const GpsReader::Status st = r.status();
            if (st.nmea.validFixes >= 2 && st.state == GpsReader::State::Listening) {
                sawOriginRefused = true;
                break;
            }
            if (terminal(st.state)) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(sawOriginRefused);

        const Outcome o = waitForEnd(r, *probe, 3000);
        CHECK(o.status.state == GpsReader::State::Fixed);
        CHECK(o.status.nmea.validFixes == 3u);
        NmeaFix fix;
        CHECK(r.takeFix(fix));
        CHECK_NEAR(fix.latDeg, 51.50735, 1e-6);
        CHECK_NEAR(fix.lonDeg, -0.12776, 1e-6);
        CHECK(!(fix.latDeg == 0.0 && fix.lonDeg == 0.0));
    }

    // --- an out-of-range latitude is refused whole, then a real fix --------
    {
        auto probe = std::make_shared<Probe>();
        std::vector<FakeSource::Step> steps = {{framed(kBadLatPayload), 10}, {framed(kFixPayload), 10}};
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, steps, 50, false), opts());
        const Outcome o = waitForEnd(r, *probe, 3000);
        CHECK(o.status.state == GpsReader::State::Fixed);
        CHECK(o.status.nmea.malformed == 1u);
        CHECK(o.status.nmea.validFixes == 1u);
        NmeaFix fix;
        CHECK(r.takeFix(fix));
        CHECK_NEAR(fix.latDeg, 51.50735, 1e-6);
    }

    // --- RMC void then RMC active -> the fix is the second one -------------
    {
        auto probe = std::make_shared<Probe>();
        std::vector<FakeSource::Step> steps = {{framed(kRmcVoidPayload), 10}, {framed(kRmcFixPayload), 10}};
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, steps, 50, false), opts());
        const Outcome o = waitForEnd(r, *probe, 3000);
        CHECK(o.status.state == GpsReader::State::Fixed);
        CHECK(o.status.nmea.positionSentences == 2u);
        CHECK(o.status.nmea.validFixes == 1u);
        CHECK(o.status.satellites == -1);  // no GGA was ever seen
        NmeaFix fix;
        CHECK(r.takeFix(fix));
        CHECK(fix.sentence == NmeaSentence::RMC);
        CHECK_NEAR(fix.latDeg, 53.79648, 1e-6);
        CHECK_NEAR(fix.lonDeg, -1.54785, 1e-6);
        CHECK(cascade::core::gpsStatusLine(o.status) == "Fix from RMC - position set");
    }

    // --- checksum failures are counted and never parsed --------------------
    {
        std::string bad = framed(kFixPayload);
        // Flip the last hex digit of the checksum (before the CRLF).
        const std::size_t last = bad.size() - 3;
        bad[last] = (bad[last] == '0') ? '1' : '0';
        const std::string none = std::string("$") + kFixPayload + "\r\n";
        auto probe = std::make_shared<Probe>();
        std::vector<FakeSource::Step> steps = {{bad, 10}, {none, 10}, {framed(kNoFixPayload), 10}};
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, steps, 50, false), opts(0.5));
        const Outcome o = waitForEnd(r, *probe, 3000);
        CHECK(o.status.state == GpsReader::State::TimedOut);
        CHECK(o.status.nmea.checksumFailures == 2u);
        CHECK(o.status.nmea.sentences == 1u);
        CHECK(o.status.nmea.validFixes == 0u);
        CHECK(o.status.bytes == bad.size() + none.size() + framed(kNoFixPayload).size());
        NmeaFix fix;
        CHECK(!r.takeFix(fix));
    }

    // --- timeout: a silent source -> TimedOut with elapsed recorded --------
    {
        auto probe = std::make_shared<Probe>();
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, kNoSteps, 20, false), opts(0.3));
        const Outcome o = waitForEnd(r, *probe, 1500);
        CHECK(o.status.state == GpsReader::State::TimedOut);
        CHECK(o.destroyedAtTerminal);
        CHECK(o.status.elapsedS >= 0.3);
        CHECK(o.status.elapsedS < 1.0);
        CHECK(o.waitedMs < 1000.0);
        CHECK(!r.listening());
        CHECK(o.status.bytes == 0u);
        // Not one byte arrived: the line says THAT, not "go outdoors" - a
        // silent port is the wrong port or a receiver that is off, and sky
        // has nothing to do with it.
        const std::string line = cascade::core::gpsStatusLine(o.status);
        CHECK(contains(line, "Nothing arrived from FAKE1 in 0 s"));
        CHECK(!contains(line, "outdoors"));
        NmeaFix fix;
        CHECK(!r.takeFix(fix));
    }

    // --- a source that dies -> Failed with an error ------------------------
    {
        auto probe = std::make_shared<Probe>();
        std::vector<FakeSource::Step> steps = {{framed(kNoFixPayload), 10}};
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, steps, 10, true), opts());
        const Outcome o = waitForEnd(r, *probe, 3000);
        CHECK(o.status.state == GpsReader::State::Failed);
        CHECK(o.destroyedAtTerminal);
        CHECK(!o.status.error.empty());
        CHECK(o.status.nmea.sentences == 1u);
        const std::string line = cascade::core::gpsStatusLine(o.status);
        CHECK(contains(line, "FAKE1"));
        CHECK(contains(line, "stopped answering"));
        NmeaFix fix;
        CHECK(!r.takeFix(fix));
    }

    // --- stop() while the source blocks returns promptly -> Idle -----------
    {
        auto probe = std::make_shared<Probe>();
        GpsReader r;
        r.start(std::make_unique<FakeSource>(probe, kNoSteps, 100, false), opts());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(r.listening());
        const auto t0 = Clock::now();
        r.stop();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        CHECK(ms < 500.0);
        CHECK(!r.listening());
        CHECK(probe->destroyed.load());
        const GpsReader::Status st = r.status();
        CHECK(st.state == GpsReader::State::Idle);
        CHECK(st.elapsedS >= 0.15);
        CHECK(cascade::core::gpsStatusLine(st).empty());
        NmeaFix fix;
        CHECK(!r.takeFix(fix));
        // stop() on an idle reader is a no-op, twice.
        r.stop();
        r.stop();
        CHECK(r.status().state == GpsReader::State::Idle);
    }

    // --- the destructor joins a live listen ---------------------------------
    {
        auto probe = std::make_shared<Probe>();
        const auto t0 = Clock::now();
        {
            GpsReader r;
            r.start(std::make_unique<FakeSource>(probe, kNoSteps, 100, false), opts());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        CHECK(ms < 600.0);
        CHECK(probe->destroyed.load());
    }

    // --- start() twice: the second listen replaces the first ---------------
    {
        auto p1 = std::make_shared<Probe>();
        auto p2 = std::make_shared<Probe>();
        GpsReader r;
        r.start(std::make_unique<FakeSource>(p1, kNoSteps, 50, false), opts());
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        GpsReader::Options second = opts();
        second.port = "FAKE2";
        second.baud = 4800;
        r.start(std::make_unique<FakeSource>(p2, std::vector<FakeSource::Step>{{framed(kFixPayload), 10}}, 50, false), second);
        CHECK(p1->destroyed.load());  // joined and released before the second began
        CHECK(r.listening());
        const Outcome o = waitForEnd(r, *p2, 3000);
        CHECK(o.status.state == GpsReader::State::Fixed);
        CHECK(o.status.port == "FAKE2");
        CHECK(o.status.baud == 4800);
        CHECK(p2->destroyed.load());
        // clearResult() is a no-op while listening: start a third, try it.
        auto p3 = std::make_shared<Probe>();
        r.start(std::make_unique<FakeSource>(p3, kNoSteps, 50, false), opts());
        r.clearResult();
        CHECK(r.status().state == GpsReader::State::Listening);
        r.stop();
    }

    // --- the real-port start() refuses an empty name and a bad baud --------
    {
        GpsReader r;
        GpsReader::Options o;
        o.port.clear();
        o.baud = 9600;
        r.start(o);
        GpsReader::Status st = r.status();
        CHECK(st.state == GpsReader::State::Failed);
        CHECK(!r.listening());
        CHECK(contains(st.error, "no port chosen"));

        o.port = "COM3";
        o.baud = 1200;
        r.start(o);
        st = r.status();
        CHECK(st.state == GpsReader::State::Failed);
        CHECK(st.port == "COM3");
        CHECK(contains(cascade::core::gpsStatusLine(st), "COM3 could not be opened"));
        CHECK(contains(st.error, "baud"));
    }

    // --- gpsStatusLine: the wording table, one row per state ---------------
    {
        GpsReader::Status s;
        s.port = "COM3";
        s.baud = 9600;
        CHECK(cascade::core::gpsStatusLine(s).empty());

        s.state = GpsReader::State::Listening;
        s.elapsedS = 12.2;
        s.nmea.sentences = 4;
        CHECK(cascade::core::gpsStatusLine(s)
              == "Listening on COM3 at 9600 - 4 sentences, no fix yet (12 s)");

        s.satellites = 6;
        s.hdop = 2.4;
        CHECK(cascade::core::gpsStatusLine(s)
              == "Listening on COM3 at 9600 - 6 satellites, HDOP 2.4, no fix yet (12 s)");

        s.hdop = NmeaFix::kAbsent;
        CHECK(cascade::core::gpsStatusLine(s)
              == "Listening on COM3 at 9600 - 6 satellites, no fix yet (12 s)");

        GpsReader::Status g;
        g.port = "COM3";
        g.baud = 4800;
        g.state = GpsReader::State::Listening;
        g.elapsedS = 12.0;
        g.bytes = 240;
        CHECK(cascade::core::gpsStatusLine(g)
              == "Listening on COM3 at 4800 - nothing readable yet, is the baud right? (12 s)");
        g.bytes = 100;  // too few bytes to call it yet
        CHECK(cascade::core::gpsStatusLine(g)
              == "Listening on COM3 at 4800 - 0 sentences, no fix yet (12 s)");

        s.state = GpsReader::State::Fixed;
        s.satellites = 8;
        s.hdop = 1.1;
        CHECK(cascade::core::gpsStatusLine(s) == "Fix: 8 satellites, HDOP 1.1 - position set");
        s.satellites = -1;
        s.fix.sentence = NmeaSentence::GLL;
        CHECK(cascade::core::gpsStatusLine(s) == "Fix from GLL - position set");

        s.state = GpsReader::State::Failed;
        s.error = "could not be opened: The system cannot find the file specified.";
        CHECK(cascade::core::gpsStatusLine(s)
              == "COM3 could not be opened: The system cannot find the file specified.");

        // TimedOut says WHICH of three different things happened, because the
        // advice for each is the opposite of the advice for the others: a
        // port that never spoke (wrong port, receiver off), a receiver that
        // spoke NMEA and simply could not see the sky (right port, right
        // baud - go outdoors), or bytes that never verified (wrong baud or
        // the wrong protocol). The counts that tell them apart are on the
        // status; the user must not have to read the log to get the right
        // half of the advice.
        GpsReader::Status t;
        t.port = "COM3";
        t.baud = 9600;
        t.state = GpsReader::State::TimedOut;
        t.elapsedS = 60.2;
        CHECK(cascade::core::gpsStatusLine(t)
              == "Nothing arrived from COM3 in 60 s - is this the GPS's port, and is the receiver "
                 "switched on?");
        t.bytes = 3000;
        t.nmea.sentences = 58;
        t.satellites = 3;
        CHECK(cascade::core::gpsStatusLine(t)
              == "No fix within 60 s (58 sentences, 3 satellites) - the receiver is answering but "
                 "cannot see enough sky; try again outdoors.");
        t.satellites = -1;  // RMC-only receiver: no satellite count to give
        CHECK(cascade::core::gpsStatusLine(t)
              == "No fix within 60 s (58 sentences) - the receiver is answering but cannot see "
                 "enough sky; try again outdoors.");
        t.nmea.sentences = 0;
        t.satellites = -1;
        CHECK(cascade::core::gpsStatusLine(t)
              == "Nothing readable in 60 s (3000 bytes) - is the receiver set to NMEA at this "
                 "baud?");
        // The old one-size line is gone from every branch.
        t.bytes = 0;
        CHECK(!contains(cascade::core::gpsStatusLine(t), "outdoors and set to NMEA"));
    }

    // --- stop() while the worker is parked inside a driver's open -----------
    //
    // A Bluetooth SPP puck that is switched off keeps CreateFileW in the
    // RFCOMM connect for tens of seconds, and a wedged USB driver for as long
    // as it likes. The first build joined through that: the Stop key froze
    // the frame loop, closing the window hung the process, and past five
    // seconds the hang watchdog filed the wait as a hang. The reader must
    // return inside kOpenAbandonWait, read Idle, and survive being destroyed
    // while the driver still has the worker - and the worker must still
    // close the port when the driver finally answers. The "driver" here is
    // an opener that sleeps through any cancellation for 2.5 s.
    {
        auto probe = std::make_shared<Probe>();
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto answered = std::make_shared<std::atomic<bool>>(false);
        double stopMs = 0.0;
        {
            GpsReader r;
            r.setOpenerForTest([=](const std::string&, int, std::string&) -> std::unique_ptr<ByteSource> {
                *entered = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2500));
                *answered = true;
                return std::make_unique<FakeSource>(probe, kNoSteps, 20, false);
            });
            GpsReader::Options o = opts();
            o.port = "COM250";
            r.start(o);
            for (int i = 0; i < 200 && !entered->load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            CHECK(entered->load());
            CHECK(r.listening());
            CHECK(r.status().state == GpsReader::State::Listening);
            CHECK(contains(cascade::core::gpsStatusLine(r.status()), "Listening on COM250"));
            const auto t0 = Clock::now();
            r.stop();
            stopMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            std::printf("stop() with the open parked: %.0f ms\n", stopMs);
            CHECK(stopMs < GpsReader::kOpenAbandonWait.count() + 700.0);
            // It waited the bound before giving up - a driver that answers
            // late but inside it is joined, not abandoned.
            CHECK(stopMs >= GpsReader::kOpenAbandonWait.count() * 0.9);
            CHECK(!r.listening());
            CHECK(r.status().state == GpsReader::State::Idle);
            CHECK(cascade::core::gpsStatusLine(r.status()).empty());
            CHECK(!answered->load());  // the driver has NOT answered yet
            // A new listen can start at once, on the same reader, while the
            // abandoned one is still in the driver: it gets its own block.
            auto p2 = std::make_shared<Probe>();
            r.start(std::make_unique<FakeSource>(p2, std::vector<FakeSource::Step>{{framed(kFixPayload), 10}}, 50, false), opts());
            const Outcome o2 = waitForEnd(r, *p2, 3000);
            CHECK(o2.status.state == GpsReader::State::Fixed);
            CHECK(o2.status.port == "FAKE1");
            // The reader is destroyed HERE, with the abandoned worker still
            // inside its "driver".
        }
        CHECK(!answered->load());
        for (int i = 0; i < 800 && !probe->destroyed.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(answered->load());
        // The port opened after the stop was closed without one read.
        CHECK(probe->destroyed.load());
        CHECK(probe->reads.load() == 0);
        bool abandonedLine = false;
        bool stoppedLine = false;
        for (const std::string& l : DiagLog::instance().ringSnapshot()) {
            if (contains(l, "is still being opened by its driver")) { abandonedLine = true; }
            if (contains(l, "gps: stopped by user after")) { stoppedLine = true; }
        }
        CHECK(abandonedLine);
        CHECK(stoppedLine);
    }

#if defined(_WIN32)
    // --- a driver that honours cancellation is cancelled, not waited for ----
    //
    // stop() asks the OS to abandon the worker's synchronous call before it
    // waits at all, so a driver that supports it hands the open back with
    // ERROR_OPERATION_ABORTED at once and the Stop key is instant. The
    // "driver" is a synchronous ReadFile on an anonymous pipe nobody writes
    // to - the cheapest cancellable blocking call this bench has. A valve
    // writes the pipe after three seconds regardless, so a stop() that did
    // NOT cancel fails this block by timing rather than hanging it.
    {
        HANDLE rd = nullptr;
        HANDLE wr = nullptr;
        CHECK(::CreatePipe(&rd, &wr, nullptr, 0) != 0);
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto returned = std::make_shared<std::atomic<bool>>(false);
        auto lastError = std::make_shared<std::atomic<long>>(-1);
        std::atomic<bool> skipValve{false};
        std::thread valve([&] {
            for (int i = 0; i < 60 && !skipValve.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!skipValve.load()) {
                char c = 'x';
                DWORD n = 0;
                ::WriteFile(wr, &c, 1, &n, nullptr);
            }
        });
        {
            GpsReader r;
            r.setOpenerForTest([=](const std::string&, int, std::string& error) -> std::unique_ptr<ByteSource> {
                *entered = true;
                char b[4];
                DWORD n = 0;
                if (::ReadFile(rd, b, sizeof b, &n, nullptr)) { *lastError = 0; }
                else { *lastError = static_cast<long>(::GetLastError()); }
                *returned = true;
                error = "COM250: the open was cancelled";
                return nullptr;
            });
            GpsReader::Options o = opts();
            o.port = "COM250";
            r.start(o);
            for (int i = 0; i < 200 && !entered->load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            CHECK(entered->load());
            // Let the worker actually reach the blocking call.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK(r.listening());
            const auto t0 = Clock::now();
            r.stop();
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            std::printf("stop() with a cancellable open: %.0f ms, open saw error %ld\n", ms,
                        lastError->load());
            CHECK(ms < 500.0);
            CHECK(returned->load());
            CHECK(lastError->load() == static_cast<long>(ERROR_OPERATION_ABORTED));
            // Stopped, not failed: the open did not fail, it was told to stop.
            CHECK(!r.listening());
            CHECK(r.status().state == GpsReader::State::Idle);
            CHECK(r.status().error.empty());
        }
        skipValve = true;
        valve.join();
        ::CloseHandle(rd);
        ::CloseHandle(wr);
        bool line = false;
        for (const std::string& l : DiagLog::instance().ringSnapshot()) {
            if (contains(l, "gps: stopped by user while opening")) { line = true; }
        }
        CHECK(line);
    }
#endif

    // --- PRIVACY: a typed file path never reaches the log ----------------------
    //
    // The port field takes any printable text and the port layer opens a
    // path exactly as it opens a port, so "\\.\C:\Users\alice\gps.nmea" is a
    // valid thing to start the reader with - and PRIVACY.md promises the log
    // never carries a file path. Every line that names the port must go
    // through loggableSerialPortName: the listening line, the Failed line,
    // the refusal in start(), and the abandon line above.
    {
        const std::string typed = "\\\\.\\C:\\Users\\alice\\gps.nmea";
        GpsReader::Options o = opts();
        o.port = typed;
        {
            auto probe = std::make_shared<Probe>();
            GpsReader r;
            r.start(std::make_unique<FakeSource>(probe, kNoSteps, 20, false), o);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            CHECK(contains(cascade::core::gpsStatusLine(r.status()), "alice"));  // on screen: fine
            r.stop();
            CHECK(r.status().state == GpsReader::State::Idle);
        }
        {
            auto probe = std::make_shared<Probe>();
            GpsReader r;
            r.start(std::make_unique<FakeSource>(probe, kNoSteps, 10, true), o);
            waitForEnd(r, *probe, 3000);
            CHECK(r.status().state == GpsReader::State::Failed);
            CHECK(contains(cascade::core::gpsStatusLine(r.status()), "alice"));  // on screen: fine
        }
        {
            GpsReader r;
            GpsReader::Options bad = o;
            bad.baud = 1200;
            r.start(bad);
            CHECK(r.status().state == GpsReader::State::Failed);
        }
        int placeholders = 0;
        int leaks = 0;
        for (const std::string& l : DiagLog::instance().ringSnapshot()) {
            if (contains(l, "(a typed device path, 27 chars)")) { ++placeholders; }
            if (contains(l, "alice") || contains(l, "gps.nmea") || contains(l, "C:\\Users")) {
                ++leaks;
                std::printf("  PATH LEAK: %s\n", l.c_str());
            }
        }
        CHECK(placeholders >= 3);  // listening, stopped-answering, bad-baud refusal
        CHECK(leaks == 0);
    }

    // --- PRIVACY: nothing logged carries a position or a sentence ----------
    //
    // Every block above has logged through the reader; the fixes were at
    // 51.50735 N 0.12776 W and 53.79648 N 1.54785 W. Not one digit of either
    // may be in the ring, in decimal or in the NMEA ddmm form, and no line
    // may carry sentence text. The Fixed lines must exist (so an empty log
    // cannot pass) and be counts only.
    {
        const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();
        CHECK(ring.size() >= 4u);
        int fixLines = 0;
        int listeningLines = 0;
        int leaks = 0;
        const char* forbidden[] = {"51.50", "0.127", "5130.4", "0007.66", "53.79", "1.547",
                                   "5347.7", "0132.87", "$G", "GPGGA", "GPRMC", "GPVTG"};
        for (const std::string& line : ring) {
            if (contains(line, "gps:") && contains(line, "fix after")) { ++fixLines; }
            if (contains(line, "gps: listening on")) { ++listeningLines; }
            // Search the MESSAGE, not the "hh:mm:ss.mmm level " prefix the
            // log puts in front of it: a wall clock reading 12:49:30.127
            // once matched the "0.127" needle and reported a leak that was
            // the time of day.
            std::string body = line;
            const std::size_t sp1 = line.find(' ');
            const std::size_t sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
            if (sp2 != std::string::npos) { body = line.substr(sp2 + 1); }
            for (const char* f : forbidden) {
                if (contains(body, f)) {
                    ++leaks;
                    std::printf("  LEAK: %s\n", line.c_str());
                }
            }
        }
        CHECK(fixLines >= 5);
        CHECK(listeningLines >= 5);
        CHECK(leaks == 0);

        // The status line is shown, not logged, but it is held to the same
        // rule: a Fixed status built from a real fix formats no coordinate.
        GpsReader::Status s;
        s.state = GpsReader::State::Fixed;
        s.fix.latDeg = 51.50735;
        s.fix.lonDeg = -0.12776;
        s.fix.altitudeM = 35.0;
        s.satellites = 8;
        s.hdop = 1.1;
        const std::string line = cascade::core::gpsStatusLine(s);
        CHECK(!contains(line, "51.5"));
        CHECK(!contains(line, "0.127"));
        CHECK(!contains(line, "35"));
    }

    return testSummary("test_gps_reader");
}
