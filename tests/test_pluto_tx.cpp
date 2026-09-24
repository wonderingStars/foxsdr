// test_pluto_tx.cpp - the ADALM-Pluto transmit driver, proven command for
// command against a fake IIOD daemon.
//
// THERE IS NO PLUTO ON THIS BENCH AND NOTHING HERE MAY TRANSMIT. That is not
// a gap to apologise for: it is the reason this file exists in the shape it
// does. The only claim a transmit driver can make without a radio and a
// licence is "these are the bytes I send and this is the order I send them
// in", and the only way to prove it is to put something on the other end of
// the wire that records them. tests/iiod_fake_server.hpp is that something.
//
// WHAT IS ACTUALLY BEING TESTED, and it is mostly not the happy path. A
// transmitter's interesting behaviour is at its edges, because those are the
// ones with somebody else's band on the end:
//
//   - opening a board LEAVES IT QUIET, even though nothing asked it to;
//   - the step that keys it is the LAST step of start(), so every failure
//     before it leaves a silent radio;
//   - stopping SILENCES BEFORE IT TIDIES, in that order;
//   - the destructor does it too, for a caller who forgot;
//   - a refused write anywhere in start() leaves the board quiet;
//   - a board that vanishes mid-transmission faults, stops, and is silenced
//     through whatever connection still works.
//
// Each of those was watched go RED against a deliberately broken driver
// before it was trusted green; the failing lines are in the report for this
// change-set.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "iiod_fake_server.hpp"
#include "source/pluto_tx.hpp"
#include "test_check.hpp"

using cascade::source::clampTxGainDb;
using cascade::source::kTxSamplesPerBuffer;
using cascade::source::PlutoTx;
using cascade::test::FakeTxIiod;
namespace iiod = cascade::source::iiod;

namespace {

// Bounds-safe indexing. A `CHECK(v.size() == n)` followed by `v[i]` is an
// out-of-bounds read in exactly the run that has something to report - the
// harness records a failed check and carries on, so the crash lands instead
// of the message. (mayhem-b200, 2026-08-13.)
std::string at(const std::vector<std::string>& v, std::size_t i) {
    return i < v.size() ? v[i] : std::string("<nothing sent>");
}

bool sameCommand(const char* label, const std::vector<std::string>& got, std::size_t i,
                 const char* want) {
    const std::string have = at(got, i);
    const bool ok = have == want;
    if (!ok) {
        std::printf("     %s[%zu]: got \"%s\", want \"%s\"\n", label, i, have.c_str(), want);
    }
    return ok;
}

void dump(const char* label, const std::vector<std::string>& v) {
    std::printf("     %s sent %zu commands:\n", label, v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::printf("       [%zu] %s\n", i, v[i].c_str());
    }
}

// Where a command first appears, or npos.
std::size_t indexOf(const std::vector<std::string>& v, const char* want) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == want) { return i; }
    }
    return std::string::npos;
}

bool contains(const std::vector<std::string>& v, const char* want) {
    return indexOf(v, want) != std::string::npos;
}

std::string uriFor(std::uint16_t port) {
    return "uri=ip:127.0.0.1:" + std::to_string(static_cast<unsigned>(port));
}

// Waits for a predicate, bounded, so a test that would otherwise hang fails
// instead.
template <typename Fn>
bool waitFor(Fn fn, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}

const char* kQuietGain = "WRITE ad9361-phy OUTPUT voltage0 hardwaregain 11 | -89.750000<NUL>";
const char* kLoOff = "WRITE ad9361-phy OUTPUT altvoltage1 powerdown 2 | 1<NUL>";
const char* kLoOn = "WRITE ad9361-phy OUTPUT altvoltage1 powerdown 2 | 0<NUL>";

}  // namespace

int main() {
    // =====================================================================
    // 1. clampTxGainDb - THE ONE PIECE OF ARITHMETIC WHERE BEING WRONG IS A
    //    LICENCE VIOLATION.
    //
    //    Every receive gain in this product clamps an over-large request to
    //    the maximum. Here the maximum is FULL TRANSMIT POWER, so this one
    //    must not: a request it cannot honour has to land on SILENCE.
    // =====================================================================
    {
        iiod::Range r;
        r.min = -89.75;  // the quietest an AD9361 goes
        r.step = 0.25;
        r.max = 0.0;  // full output

        // In range, untouched.
        CHECK_NEAR(clampTxGainDb(-20.0, r), -20.0, 1e-9);
        CHECK_NEAR(clampTxGainDb(-89.75, r), -89.75, 1e-9);
        CHECK_NEAR(clampTxGainDb(0.0, r), 0.0, 1e-9);

        // Below the quiet end: quiet.
        CHECK_NEAR(clampTxGainDb(-200.0, r), -89.75, 1e-9);

        // ABOVE THE LOUD END: ALSO QUIET. This is the line. A conventional
        // clamp would answer 0.0 here, which is a radio at full output
        // because a config file had a stray number in it.
        CHECK_NEAR(clampTxGainDb(40.0, r), -89.75, 1e-9);
        CHECK_NEAR(clampTxGainDb(1.0, r), -89.75, 1e-9);

        // A number that is not one lands on silence too - it cannot be
        // compared into range, so it must not be written verbatim.
        CHECK_NEAR(clampTxGainDb(std::nan(""), r), -89.75, 1e-9);

        // A board that published the pair the other way round still gets a
        // quiet answer rather than a loud one.
        iiod::Range flipped;
        flipped.min = 0.0;
        flipped.max = -89.75;
        CHECK_NEAR(clampTxGainDb(40.0, flipped), -89.75, 1e-9);
        CHECK_NEAR(clampTxGainDb(-300.0, flipped), -89.75, 1e-9);
    }

    // =====================================================================
    // 2. packSample - THE SAMPLE CONVERSION, AND ITS SATURATION
    //
    //    The inverse of convertSample, driven by the PARSED format. The
    //    saturation half is the one that matters on the air: an overshoot
    //    cast straight to an int16 wraps +1.0 to full-scale NEGATIVE, which
    //    is a full-amplitude discontinuity in the middle of a transmitted
    //    block - a click, and a wide one.
    // =====================================================================
    {
        iiod::SampleFormat f16;
        CHECK(iiod::parseSampleFormat("le:S16/16>>0", f16));
        CHECK(f16.bits == 16 && f16.storageBits == 16 && f16.isSigned && f16.littleEndian);

        // Round trip: everything representable comes back.
        double worst = 0.0;
        for (int q = -32768; q <= 32767; q += 7) {
            const float v = static_cast<float>(q) / 32768.0f;
            std::uint8_t word[2] = {0, 0};
            iiod::packSample(v, f16, word);
            const float back = iiod::convertSample(word, f16);
            worst = std::max(worst, static_cast<double>(std::fabs(back - v)));
        }
        std::printf("packSample/convertSample round trip: worst error %.3e\n", worst);
        CHECK(worst < 1.0 / 32768.0);

        // The two ends, exactly.
        std::uint8_t word[2] = {0, 0};
        iiod::packSample(0.0f, f16, word);
        CHECK(word[0] == 0x00 && word[1] == 0x00);
        iiod::packSample(-1.0f, f16, word);
        CHECK(word[0] == 0x00 && word[1] == 0x80);  // -32768, little-endian

        // SATURATION, NOT WRAP. +1.0 and beyond pin to +32767 (0x7FFF) - a
        // wrapping cast would produce 0x8000, which is the most NEGATIVE
        // sample there is.
        iiod::packSample(1.0f, f16, word);
        CHECK(word[0] == 0xFF && word[1] == 0x7F);
        iiod::packSample(9.0f, f16, word);
        CHECK(word[0] == 0xFF && word[1] == 0x7F);
        iiod::packSample(-9.0f, f16, word);
        CHECK(word[0] == 0x00 && word[1] == 0x80);
        // A number that is not one is transmitted as nothing.
        iiod::packSample(std::nanf(""), f16, word);
        CHECK(word[0] == 0x00 && word[1] == 0x00);

        // The receive side's own format, to prove the conversion follows what
        // was PARSED rather than a hard-coded 16 bits.
        iiod::SampleFormat f12;
        CHECK(iiod::parseSampleFormat("le:S12/16>>0", f12));
        iiod::packSample(1.0f, f12, word);
        const std::uint16_t raw12 =
            static_cast<std::uint16_t>(word[0] | (static_cast<std::uint16_t>(word[1]) << 8));
        CHECK(raw12 == 0x07FF);  // 2047, the largest a 12-bit signed field holds
        CHECK_NEAR(iiod::convertSample(word, f12), 2047.0 / 2048.0, 1e-6);

        // Big-endian, so the byte order is the format's and not the host's.
        iiod::SampleFormat be;
        CHECK(iiod::parseSampleFormat("be:S16/16>>0", be));
        iiod::packSample(-1.0f, be, word);
        CHECK(word[0] == 0x80 && word[1] == 0x00);
    }

    // =====================================================================
    // 3. OPENING A BOARD LEAVES IT QUIET
    //
    //    A Pluto keeps whatever the last program left in it. This fake is
    //    handed over at 0 dB attenuation - full output - with its TX LO
    //    ready, exactly as a board somebody else was transmitting through
    //    would be. Opening it must fix that WITHOUT being asked.
    // =====================================================================
    {
        FakeTxIiod d;
        std::string err;
        CHECK(d.start(err));
        cascade::test::stockTxBoard(d);
        d.setAttr("ad9361-phy/out/altvoltage1/powerdown", "0");  // LO up, as if keyed
        CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "0.000000 dB");

        PlutoTx tx;
        CHECK(tx.open(uriFor(d.port())));
        CHECK(tx.isOpen());
        CHECK(!tx.running());  // NOTHING is transmitting after an open

        const std::vector<std::string> c = d.commands(0);
        if (!(contains(c, kQuietGain) && contains(c, kLoOff))) { dump("control", c); }
        CHECK(contains(c, kQuietGain));
        CHECK(contains(c, kLoOff));
        // And the board actually took them.
        CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "-89.750000");
        CHECK(d.attr("ad9361-phy/out/altvoltage1/powerdown") == "1");
        // THE ORDER IS THE SAFETY PROPERTY: the attenuation goes up FIRST,
        // because it is the control that stops RF leaving the connector and
        // it works whether or not the oscillator is running.
        CHECK(indexOf(c, kQuietGain) < indexOf(c, kLoOff));

        // The power a fresh install starts at is the QUIET end, and it is not
        // read off the board - the board said 0 dB.
        CHECK_NEAR(tx.gainDb(), -89.75, 1e-9);

        // It found the transmit half of the context, not the receive half.
        CHECK(tx.phyDeviceName() == "ad9361-phy");
        CHECK(tx.dacDeviceName() == "cf-ad9361-dds-core-lpc");
        // THE OSCILLATOR IS altvoltage1. altvoltage0 is the RECEIVER's and is
        // the same shape; a driver that picked it would retune the receiver
        // every time the operator moved the transmit frequency.
        CHECK(tx.txLoChannel() == "altvoltage1");
        CHECK(tx.txChannel() == "voltage0");

        // What it read off the board.
        CHECK_NEAR(tx.centerFrequencyHz(), 2.4e9, 1.0);
        CHECK_NEAR(tx.sampleRateHz(), 2500000.0, 1.0);
        double lo = 0.0;
        double hi = 0.0;
        CHECK(tx.gainRangeDb(lo, hi));
        CHECK_NEAR(lo, -89.75, 1e-9);
        CHECK_NEAR(hi, 0.0, 1e-9);

        // Nothing has been transmitted, at all.
        CHECK(d.writeBufCount() == 0);
    }

    // =====================================================================
    // 4. THE LIMITS COME OFF THE BOARD
    //
    //    The same code served two different boards. A driver with a table
    //    passes neither.
    // =====================================================================
    {
        for (int pass = 0; pass < 2; ++pass) {
            FakeTxIiod d;
            std::string err;
            CHECK(d.start(err));
            if (pass == 0) {
                cascade::test::stockTxBoard(d);
            } else {
                cascade::test::unlockedTxBoard(d);
            }
            PlutoTx tx;
            CHECK(tx.open(uriFor(d.port())));
            double lo = 0.0;
            double hi = 0.0;
            CHECK(tx.frequencyRangeHz(lo, hi));
            std::printf("%s board transmits %.3f - %.3f MHz\n",
                        pass == 0 ? "stock AD9363" : "AD9364-unlocked", lo / 1e6, hi / 1e6);
            CHECK_NEAR(lo, pass == 0 ? 325.0e6 : 70.0e6, 1.0);
            CHECK_NEAR(hi, pass == 0 ? 3.8e9 : 6.0e9, 1.0);

            // A frequency outside what the board published is REFUSED with a
            // reason rather than clamped: a transmission that silently landed
            // somewhere else is on somebody's band.
            CHECK(!tx.setCenterFrequencyHz(100.0e6 * (pass == 0 ? 1.0 : 0.1)));
            CHECK(std::string(tx.lastError()).find("outside") != std::string::npos);
            // ...and one inside it is taken.
            CHECK(tx.setCenterFrequencyHz(433.5e6));
            CHECK_NEAR(tx.centerFrequencyHz(), 433.5e6, 1.0);
            CHECK(d.attr("ad9361-phy/out/altvoltage1/frequency") == "433500000");
            // THE RECEIVER'S OSCILLATOR WAS NOT TOUCHED.
            CHECK(d.attr("ad9361-phy/out/altvoltage0/frequency") == "433000000");
        }
    }

    // =====================================================================
    // 5. A BOARD THAT WILL NOT SAY HOW QUIET IT CAN BE IS NOT KEYED
    //
    //    Everything that makes this class safe is written in terms of the
    //    board's own maximum attenuation. Without one there is no number to
    //    write, so the open is refused rather than the safety being skipped.
    // =====================================================================
    {
        FakeTxIiod d;
        std::string err;
        CHECK(d.start(err));
        cascade::test::stockTxBoard(d);
        d.refuse("READ ad9361-phy OUTPUT voltage0 hardwaregain_available", -2);

        PlutoTx tx;
        CHECK(!tx.open(uriFor(d.port())));
        CHECK(!tx.isOpen());
        const std::string why = tx.lastError();
        std::printf("no gain range: \"%s\"\n", why.c_str());
        CHECK(why.find("will not transmit") != std::string::npos);
    }

    // =====================================================================
    // 6. START: THE ORDER, AND THE FACT THAT KEYING IS THE LAST STEP
    // =====================================================================
    {
        FakeTxIiod d;
        std::string err;
        CHECK(d.start(err));
        cascade::test::stockTxBoard(d);

        PlutoTx tx;
        CHECK(tx.open(uriFor(d.port())));
        CHECK(tx.setGainDb(-20.0));
        // A power set while NOT keyed is REMEMBERED, not written: the board
        // is deliberately sitting at maximum attenuation and writing the
        // operator's chosen power into it now would undo exactly that.
        CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "-89.750000");
        CHECK_NEAR(tx.gainDb(), -20.0, 1e-9);

        const std::size_t before = d.commands(0).size();
        CHECK(tx.start());
        CHECK(tx.running());

        const std::vector<std::string> c = d.commands(0);
        std::vector<std::string> during(c.begin() + static_cast<std::ptrdiff_t>(before),
                                        c.end());
        const char* kKey = "WRITE ad9361-phy OUTPUT voltage0 hardwaregain 11 | -20.000000<NUL>";
        const char* kPort = "WRITE ad9361-phy OUTPUT voltage0 rf_port_select 2 | A<NUL>";
        bool ok = true;
        ok &= sameCommand("start", during, 0, kPort);
        // THE BOARD'S OWN TEST TONES OFF. Without this a Pluto transmits its
        // internal DDS and ignores every sample handed to it - which looks
        // exactly like a driver that is writing nothing.
        ok &= sameCommand("start", during, 1,
                          "WRITE cf-ad9361-dds-core-lpc OUTPUT altvoltage0 raw 2 | 0<NUL>");
        ok &= sameCommand("start", during, 2,
                          "WRITE cf-ad9361-dds-core-lpc OUTPUT altvoltage1 raw 2 | 0<NUL>");
        ok &= sameCommand("start", during, 3,
                          "WRITE cf-ad9361-dds-core-lpc OUTPUT altvoltage2 raw 2 | 0<NUL>");
        ok &= sameCommand("start", during, 4,
                          "WRITE cf-ad9361-dds-core-lpc OUTPUT altvoltage3 raw 2 | 0<NUL>");
        ok &= sameCommand("start", during, 5, kLoOn);
        // AND THE KEY IS LAST. Every step before it happens while the board
        // is still at maximum attenuation, which is what makes a failure
        // halfway through start() safe.
        ok &= sameCommand("start", during, 6, kKey);
        if (!ok) { dump("start", during); }
        CHECK(ok);

        // The stream connection is a SECOND connection, and it carries the
        // buffer commands rather than the control ones.
        CHECK(d.connections() >= 2);
        const std::vector<std::string> s = d.commands(1);
        bool sok = true;
        sok &= sameCommand("stream", s, 0, "TIMEOUT 4000");
        // BUFFERS_COUNT BEFORE OPEN (ops.c create_buf_and_blocks reads the
        // count only when it makes the buffer, which OPEN is what triggers);
        // sent afterwards it would succeed and change nothing.
        sok &= sameCommand("stream", s, 1, "SET cf-ad9361-dds-core-lpc BUFFERS_COUNT 2");
        // The mask is computed from the CHANNEL COUNT in the context, not
        // written as a literal: the daemon rejects any other length outright.
        sok &= sameCommand("stream", s, 2, "OPEN cf-ad9361-dds-core-lpc 4096 00000003");
        if (!sok) { dump("stream", s); }
        CHECK(sok);

        tx.stop();
        CHECK(!tx.running());

        // THE START LINE says the rate and the power and NEVER the frequency
        // (0.99.33: it used to print "tx: started - 433.000000 MHz, ...").
        int started = 0;
        for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
            if (l.find("tx: started") == std::string::npos) { continue; }
            ++started;
            std::printf("start line: %s\n", l.c_str());
            CHECK(l.find("MHz") == std::string::npos);
            CHECK(l.find("S/s") != std::string::npos);
            CHECK(l.find("power -20.00 dB") != std::string::npos);
        }
        CHECK(started >= 1);
    }

    // =====================================================================
    // 7. THE SAMPLES THAT GO DOWN THE WIRE
    //
    //    With no radio here, the only way to know the conversion is right is
    //    to read back what was actually sent and decode it with the same
    //    format the board published.
    // =====================================================================
    {
        FakeTxIiod d;
        std::string err;
        CHECK(d.start(err));
        cascade::test::stockTxBoard(d);

        PlutoTx tx;
        CHECK(tx.open(uriFor(d.port())));
        CHECK(tx.setGainDb(-30.0));
        CHECK(tx.start());

        // A ramp on I and its negation on Q, so a swapped pair, a dropped
        // sample or a byte-order mistake all show up as a mismatch at a named
        // index rather than as a count that happens to come out right.
        const std::size_t n = kTxSamplesPerBuffer;
        std::vector<std::complex<float>> want(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float v = -0.9f + 1.8f * static_cast<float>(i) / static_cast<float>(n);
            want[i] = std::complex<float>(v, -v);
        }
        std::size_t wrote = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (wrote < n && std::chrono::steady_clock::now() < deadline) {
            wrote += tx.write(want.data() + wrote, n - wrote);
        }
        CHECK(wrote == n);

        // Wait for enough buffers that ours must have gone out.
        CHECK(waitFor([&d] { return d.writeBufCount() >= 4; }, std::chrono::seconds(3)));
        tx.stop();

        iiod::SampleFormat fmt;
        CHECK(iiod::parseSampleFormat("le:S16/16>>0", fmt));
        const std::vector<std::uint8_t> raw = d.transmitted();
        CHECK(raw.size() % 4 == 0);
        std::vector<std::complex<float>> got(raw.size() / 4);
        for (std::size_t i = 0; i < got.size(); ++i) {
            got[i] = std::complex<float>(iiod::convertSample(&raw[i * 4], fmt),
                                         iiod::convertSample(&raw[i * 4 + 2], fmt));
        }

        // Find our block anywhere in the stream: the writer may have sent a
        // buffer of silence before the first write() landed, so the block is
        // contiguous but not necessarily buffer-aligned.
        const float tol = 1.0f / 16384.0f;
        std::size_t found = got.size();
        for (std::size_t s = 0; s + n <= got.size() && found == got.size(); ++s) {
            if (std::fabs(got[s].real() - want[0].real()) > tol) { continue; }
            bool all = true;
            for (std::size_t i = 0; i < n && all; ++i) {
                if (std::fabs(got[s + i].real() - want[i].real()) > tol ||
                    std::fabs(got[s + i].imag() - want[i].imag()) > tol) {
                    all = false;
                }
            }
            if (all) { found = s; }
        }
        std::printf("transmitted %zu samples in %zu buffers; the written block starts at %zu\n",
                    got.size(), static_cast<std::size_t>(d.writeBufCount()), found);
        CHECK(found < got.size());
        // And nothing was padded into the middle of it - the underrun count
        // says how many buffers had to be filled with silence, and a stream
        // where ours was split by one would not have matched above anyway.
        std::printf("underrun buffers: %llu\n",
                    static_cast<unsigned long long>(tx.underrunBuffers()));
    }

    // =====================================================================
    // 8. STOPPING SILENCES BEFORE IT TIDIES - ON THE WRITER'S CONNECTION
    // =====================================================================
    {
        FakeTxIiod d;
        std::string err;
        CHECK(d.start(err));
        cascade::test::stockTxBoard(d);

        PlutoTx tx;
        CHECK(tx.open(uriFor(d.port())));
        CHECK(tx.setGainDb(-10.0));
        CHECK(tx.start());
        CHECK(waitFor([&d] { return d.writeBufCount() >= 1; }, std::chrono::seconds(3)));
        CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "-10.000000");
        CHECK(d.attr("ad9361-phy/out/altvoltage1/powerdown") == "0");

        const std::size_t streamBefore = d.commands(1).size();
        tx.stop();
        CHECK(!tx.running());

        // The board is quiet again...
        CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "-89.750000");
        CHECK(d.attr("ad9361-phy/out/altvoltage1/powerdown") == "1");

        // ...and it was silenced on the WRITER's connection, as its last act,
        // which is what keeps stop() to one bounded wait instead of a join
        // plus two network round trips.
        const std::vector<std::string> s = d.commands(1);
        std::vector<std::string> after(s.begin() + static_cast<std::ptrdiff_t>(streamBefore),
                                       s.end());
        const std::size_t gain = indexOf(after, kQuietGain);
        const std::size_t off = indexOf(after, kLoOff);
        if (gain == std::string::npos || off == std::string::npos) { dump("stream tail", after); }
        CHECK(gain != std::string::npos);
        CHECK(off != std::string::npos);
        // THE ORDER AGAIN. Powering the oscillator down first would leave the
        // amplifier at whatever gain it had for as long as the second write
        // took.
        CHECK(gain < off);

        // Idempotent, and a second stop sends nothing more.
        const std::size_t quiet = d.commands(1).size();
        tx.stop();
        CHECK(d.commands(1).size() == quiet);
    }

    // =====================================================================
    // 9. A CALLER WHO FORGETS CANNOT LEAVE A RADIO KEYED
    // =====================================================================
    {
        FakeTxIiod d;
        std::string err;
        CHECK(d.start(err));
        cascade::test::stockTxBoard(d);
        {
            PlutoTx tx;
            CHECK(tx.open(uriFor(d.port())));
            CHECK(tx.setGainDb(0.0));  // full output, deliberately
            CHECK(tx.start());
            CHECK(waitFor([&d] { return d.writeBufCount() >= 1; }, std::chrono::seconds(3)));
            CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "0.000000");
            // No stop(). The destructor is the only thing between this board
            // and a radio left transmitting into an empty room.
        }
        CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "-89.750000");
        CHECK(d.attr("ad9361-phy/out/altvoltage1/powerdown") == "1");
    }

    // =====================================================================
    // 10. A REFUSED WRITE IN THE MIDDLE OF START LEAVES THE BOARD QUIET
    //
    //     The DDS shutdown is refused - a plausible firmware difference - so
    //     start() gives up part way through, which is exactly the case the
    //     "key last" ordering exists for.
    // =====================================================================
    {
        FakeTxIiod d;
        std::string err;
        CHECK(d.start(err));
        cascade::test::stockTxBoard(d);
        d.refuse("WRITE cf-ad9361-dds-core-lpc OUTPUT altvoltage2 raw 2", -22);

        PlutoTx tx;
        CHECK(tx.open(uriFor(d.port())));
        CHECK(tx.setGainDb(0.0));
        CHECK(!tx.start());
        CHECK(!tx.running());
        // Nothing was ever transmitted...
        CHECK(d.writeBufCount() == 0);
        // ...and the board is quiet, not left at the power the operator asked
        // for with a half-configured DAC behind it.
        CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "-89.750000");
        CHECK(d.attr("ad9361-phy/out/altvoltage1/powerdown") == "1");
        std::printf("refused DDS write: \"%s\"\n", tx.lastError());
    }

    // =====================================================================
    // 11. A BOARD THAT VANISHES MID-TRANSMISSION
    //
    //     The socket dies after three buffers with the radio keyed. The
    //     driver must fault, stop, and take whatever chance it still has to
    //     silence the board.
    // =====================================================================
    {
        FakeTxIiod d;
        std::string err;
        CHECK(d.start(err));
        cascade::test::stockTxBoard(d);
        d.dieAfterWriteBufs.store(3);

        const unsigned long long abandonedBefore = PlutoTx::writersAbandoned();
        {
            PlutoTx tx;
            CHECK(tx.open(uriFor(d.port())));
            CHECK(tx.setGainDb(-5.0));
            CHECK(tx.start());

            std::vector<std::complex<float>> block(1024, std::complex<float>(0.2f, -0.2f));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!tx.faulted() && std::chrono::steady_clock::now() < deadline) {
                tx.write(block.data(), block.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            CHECK(tx.faulted());
            std::printf("board vanished: \"%s\"\n", tx.lastError());
            // THE CAUSE, NOT ITS CONSEQUENCE. The silencing write fails too
            // on a dead socket, and the first draft of this driver reported
            // THAT - so the message named the quietening rather than the
            // stream that had stopped, which is the less useful half of what
            // happened.
            CHECK(std::string(tx.lastError()).find("transmit buffer") != std::string::npos);

            // write() refuses once the board has gone rather than queueing
            // modulation for a radio that is not there.
            CHECK(tx.write(block.data(), block.size()) == 0);

            const auto t0 = std::chrono::steady_clock::now();
            tx.stop();
            const double stopMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            std::printf("stop() after the board vanished took %.1f ms (bound %lld ms)\n", stopMs,
                        static_cast<long long>(PlutoTx::kWriterJoinWait.count()));
            // The writer noticed the dead socket and came back on its own, so
            // the bounded join is not spent - and nothing was abandoned.
            CHECK(stopMs < static_cast<double>(PlutoTx::kWriterJoinWait.count()));
            CHECK(!tx.running());
        }
        CHECK(PlutoTx::writersAbandoned() == abandonedBefore);
        // The CONTROL connection was never the one that died, so close()'s
        // belt-and-braces quietening got through even though the writer's own
        // attempt could not.
        CHECK(d.attr("ad9361-phy/out/voltage0/hardwaregain") == "-89.750000");
        CHECK(d.attr("ad9361-phy/out/altvoltage1/powerdown") == "1");
    }

    // =====================================================================
    // 12. NOTHING AT ALL AT THE ADDRESS
    // =====================================================================
    {
        PlutoTx tx;
        // Port 9 is the discard service and nothing listens on it here; the
        // connect is bounded by iiod::kConnectWait, so this must come back
        // rather than hang.
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!tx.open("uri=ip:127.0.0.1:9"));
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        std::printf("a refused connection took %.1f ms (bound %lld ms)\n", ms,
                    static_cast<long long>(iiod::kConnectWait.count()));
        CHECK(ms <= static_cast<double>(iiod::kConnectWait.count()) + 500.0);
        CHECK(!tx.isOpen());
        // And keying a sink that never opened is refused rather than crashing.
        CHECK(!tx.start());
        CHECK(!tx.running());
        tx.stop();
    }

    return testSummary("test_pluto_tx");
}
