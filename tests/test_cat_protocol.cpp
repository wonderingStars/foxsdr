// Tests for net/cat_protocol.hpp — the rigctld-compatible command layer.
//
// The response SHAPE is what these mostly assert, because that is what breaks
// interoperability: a get that returns an extra "RPRT 0" line desynchronises
// every client after the first command, and the failure looks like the radio
// answering the previous question. So the exact bytes are pinned, not merely
// the values inside them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/cat_protocol.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include "test_check.hpp"

using cascade::dsp::DemodMode;
using cascade::net::catDumpState;
using cascade::net::catModeFromToken;
using cascade::net::catModeToken;
using cascade::net::CatResult;
using cascade::net::executeCatLine;
using cascade::net::planTune;
using cascade::net::RadioStatus;

namespace {

RadioStatus baseStatus() {
    RadioStatus s;
    s.running = true;
    s.centerHz = 145'000'000.0;
    s.sampleRateHz = 2'400'000.0;
    s.vfoOffsetHz = 0.0;
    s.bandwidthHz = 12'500.0;
    s.mode = "NFM";
    return s;
}

CatResult run(const std::string& line, const RadioStatus& s) {
    return executeCatLine(line, s);
}

void testGetFrequency() {
    RadioStatus s = baseStatus();
    s.vfoOffsetHz = 500'000.0;

    const CatResult r = run("f", s);
    // The tuned frequency is centre PLUS offset, and the reply is the value
    // alone. No RPRT on a successful get.
    CHECK(r.reply == "145500000\n");
    CHECK(!r.quit);
    CHECK(!r.hasControl);

    // The long form is the same command.
    CHECK(run("\\get_freq", s).reply == "145500000\n");
    // Trailing CRLF from a networked client must not change the answer.
    CHECK(run("f\r\n", s).reply == "145500000\n");
    CHECK(run("  f  ", s).reply == "145500000\n");
}

void testSetFrequencyInsideBandMovesOnlyTheVfo() {
    const RadioStatus s = baseStatus();  // 145.0 MHz centre, 2.4 MS/s

    const CatResult r = run("F 145800000", s);
    CHECK(r.reply == "RPRT 0\n");  // a set answers only this
    CHECK(r.hasControl);
    // 800 kHz away, well inside the usable half-band, so the source is left
    // alone and only the VFO moves. A retune here would blank the waterfall
    // and make the hardware settle for no reason.
    CHECK(!r.control.centerHz.has_value());
    CHECK(r.control.vfoOffsetHz.has_value());
    if (r.control.vfoOffsetHz.has_value()) {
        CHECK(std::fabs(*r.control.vfoOffsetHz - 800'000.0) < 1e-6);
    }
}

void testSetFrequencyOutsideBandRetunes() {
    const RadioStatus s = baseStatus();

    const CatResult r = run("F 433000000", s);
    CHECK(r.reply == "RPRT 0\n");
    CHECK(r.hasControl);
    // Far outside the sampled band: the centre must move, and the offset must
    // return to zero rather than keeping a stale value from the old centre.
    CHECK(r.control.centerHz.has_value());
    if (r.control.centerHz.has_value()) {
        CHECK(std::fabs(*r.control.centerHz - 433'000'000.0) < 1e-6);
    }
    CHECK(r.control.vfoOffsetHz.has_value());
    if (r.control.vfoOffsetHz.has_value()) {
        CHECK(std::fabs(*r.control.vfoOffsetHz) < 1e-9);
    }
}

void testTunePlanBoundary() {
    // The edge itself: with a 2.4 MHz span and 90% usable, the VFO may sit up
    // to 1.08 MHz from centre. Just inside stays; just outside retunes. The
    // boundary is asserted from both sides so a change to the fraction cannot
    // pass unnoticed.
    const double centre = 100'000'000.0;
    const double rate = 2'400'000.0;
    const double limit = 0.5 * rate * 0.90;  // 1.08 MHz

    const auto inside = planTune(centre + limit - 1.0, centre, rate);
    CHECK(!inside.retune);
    CHECK(std::fabs(inside.vfoOffsetHz - (limit - 1.0)) < 1e-6);

    const auto outside = planTune(centre + limit + 1.0, centre, rate);
    CHECK(outside.retune);
    CHECK(std::fabs(outside.centerHz - (centre + limit + 1.0)) < 1e-6);
    CHECK(std::fabs(outside.vfoOffsetHz) < 1e-9);

    // Negative offsets behave symmetrically.
    const auto below = planTune(centre - limit + 1.0, centre, rate);
    CHECK(!below.retune);
    CHECK(below.vfoOffsetHz < 0.0);

    // With no sample rate there is no band to stay inside, so retuning is the
    // only honest plan rather than parking the VFO at a meaningless offset.
    const auto noRate = planTune(50'000'000.0, centre, 0.0);
    CHECK(noRate.retune);
    CHECK(std::fabs(noRate.centerHz - 50'000'000.0) < 1e-6);
}

void testSetFrequencyRejectsRubbish() {
    const RadioStatus s = baseStatus();
    // Each of these would be read as a frequency by a lenient parser, and each
    // would tune the radio somewhere the client never asked for.
    const char* bad[] = {"F", "F abc", "F 14250000abc", "F -1", "F ", "F 1e"};
    for (const char* line : bad) {
        const CatResult r = run(line, s);
        CHECK(r.reply == "RPRT -1\n");
        CHECK(!r.hasControl);  // and nothing was queued
    }
}

void testGetMode() {
    RadioStatus s = baseStatus();
    s.mode = "USB";
    s.bandwidthHz = 2400.0;

    const CatResult r = run("m", s);
    // Two values, each on its own line, and no RPRT.
    CHECK(r.reply == "USB\n2400\n");

    s.mode = "NFM";
    s.bandwidthHz = 12500.0;
    // Ours is NFM; Hamlib calls narrow FM simply "FM".
    CHECK(run("m", s).reply == "FM\n12500\n");

    s.mode = "WFM";
    s.bandwidthHz = 230000.0;
    CHECK(run("m", s).reply == "WFM\n230000\n");
}

void testSetMode() {
    const RadioStatus s = baseStatus();

    const CatResult r = run("M USB 2400", s);
    CHECK(r.reply == "RPRT 0\n");
    CHECK(r.hasControl);
    CHECK(r.control.mode.has_value());
    if (r.control.mode.has_value()) CHECK(*r.control.mode == DemodMode::USB);
    CHECK(r.control.bandwidthHz.has_value());
    if (r.control.bandwidthHz.has_value()) {
        CHECK(std::fabs(*r.control.bandwidthHz - 2400.0) < 1e-6);
    }

    // A passband of 0 means "the mode's default" in this protocol. Passing it
    // through would set a zero-width filter and silence the radio.
    const CatResult zero = run("M CW 0", s);
    CHECK(zero.reply == "RPRT 0\n");
    CHECK(zero.control.mode.has_value());
    CHECK(!zero.control.bandwidthHz.has_value());

    // Omitted entirely: same meaning.
    const CatResult none = run("M AM", s);
    CHECK(none.reply == "RPRT 0\n");
    CHECK(none.control.mode.has_value());
    CHECK(!none.control.bandwidthHz.has_value());

    // An unknown mode changes nothing.
    const CatResult bad = run("M PKTUSB 2400", s);
    CHECK(bad.reply == "RPRT -1\n");
    CHECK(!bad.hasControl);
}

void testModeTokenRoundTrip() {
    // Every demodulator must survive a round trip through the wire tokens,
    // except RAW which is deliberately unnameable.
    const DemodMode all[] = {DemodMode::NFM, DemodMode::WFM, DemodMode::AM,
                             DemodMode::DSB, DemodMode::USB, DemodMode::LSB,
                             DemodMode::CW};
    for (DemodMode m : all) {
        const char* token = catModeToken(m);
        CHECK(token != nullptr && *token != '\0');
        DemodMode back = DemodMode::RAW;
        CHECK(catModeFromToken(token, back));
        CHECK(back == m);
    }
    // RAW has no token, so a client cannot ask for it.
    CHECK(std::string(catModeToken(DemodMode::RAW)).empty());
    DemodMode ignored = DemodMode::AM;
    CHECK(!catModeFromToken("RAW", ignored));

    // Case and the common aliases.
    DemodMode m = DemodMode::AM;
    CHECK(catModeFromToken("usb", m) && m == DemodMode::USB);
    CHECK(catModeFromToken("CWR", m) && m == DemodMode::CW);
    CHECK(catModeFromToken("NFM", m) && m == DemodMode::NFM);
    CHECK(!catModeFromToken("", m));
    CHECK(!catModeFromToken("NOSUCH", m));
}

void testVfoAndSplit() {
    const RadioStatus s = baseStatus();

    CHECK(run("v", s).reply == "VFOA\n");
    CHECK(run("V VFOA", s).reply == "RPRT 0\n");
    CHECK(run("V currVFO", s).reply == "RPRT 0\n");
    // There is no second VFO to select, and saying so beats pretending.
    CHECK(run("V VFOB", s).reply == "RPRT -11\n");

    CHECK(run("s", s).reply == "0\nVFOA\n");
    CHECK(run("S 0 VFOA", s).reply == "RPRT 0\n");
    CHECK(run("S 1 VFOB", s).reply == "RPRT -11\n");
    CHECK(run("S x", s).reply == "RPRT -1\n");
}

void testPttIsAlwaysReceive() {
    const RadioStatus s = baseStatus();

    CHECK(run("t", s).reply == "0\n");
    // Being told to stop transmitting is agreeable.
    CHECK(run("T 0", s).reply == "RPRT 0\n");
    // Being told to START is refused, because there is no transmitter. A
    // client that believed otherwise would wait for a signal that never goes
    // out; the error is what tells it immediately.
    CHECK(run("T 1", s).reply == "RPRT -11\n");
    CHECK(run("T", s).reply == "RPRT -1\n");
}

void testQuit() {
    const RadioStatus s = baseStatus();
    const CatResult r = run("q", s);
    CHECK(r.quit);
    CHECK(r.reply.empty());  // nothing to say; the socket just closes
    CHECK(run("Q", s).quit);
    CHECK(run("\\quit", s).quit);
}

void testUnknownCommands() {
    const RadioStatus s = baseStatus();
    // Understood-but-unimplemented and outright unknown both answer, so a
    // client is never left waiting on a line that will not come.
    CHECK(run("Z", s).reply == "RPRT -4\n");
    CHECK(run("\\no_such_command", s).reply == "RPRT -4\n");
    CHECK(run("\\set_rit 100", s).reply == "RPRT -4\n");

    // A blank line is not an error: it produces no reply at all, which is what
    // keeps a client's newline handling from provoking spurious RPRTs.
    const CatResult blank = run("", s);
    CHECK(blank.reply.empty());
    CHECK(!blank.quit);
    CHECK(!blank.hasControl);
    CHECK(run("   \r\n", s).reply.empty());
}

void testChkVfoAndPowerState() {
    RadioStatus s = baseStatus();
    CHECK(run("\\chk_vfo", s).reply == "CHKVFO 0\n");

    CHECK(run("\\get_powerstat", s).reply == "1\n");
    s.running = false;
    CHECK(run("\\get_powerstat", s).reply == "0\n");
}

void testDumpState() {
    const RadioStatus s = baseStatus();
    const CatResult r = run("\\dump_state", s);
    const std::string& d = r.reply;
    CHECK(!d.empty());
    CHECK(d.back() == '\n');

    // The protocol version leads.
    CHECK(d.rfind("1\n", 0) == 0);

    // THE LINE THAT MATTERS: the TX range list must be empty, because that is
    // how this protocol states "cannot transmit". If a TX range ever appeared
    // here, clients would offer to key a receiver.
    CHECK(d.find("0 0 0 0 0 0 0\n") != std::string::npos);

    // An RX range must be advertised, or a client will refuse every frequency.
    CHECK(d.find("0.000000 4000000000.000000") != std::string::npos);

    // Every line of the block is either numeric/hex data or empty; a stray
    // word here would be parsed as a number by a client and become garbage.
    std::size_t at = 0;
    int lines = 0;
    while (at < d.size()) {
        const std::size_t nl = d.find('\n', at);
        const std::string line = d.substr(at, nl - at);
        at = nl == std::string::npos ? d.size() : nl + 1;
        ++lines;
        for (char c : line) {
            const bool allowed = (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
                                 c == '-' || c == 'x' || (c >= 'a' && c <= 'f');
            if (!allowed) {
                std::printf("FAIL dump_state line has unexpected character: %s\n",
                            line.c_str());
            }
            CHECK(allowed);
        }
    }
    CHECK(lines > 10);
}

void testSetsNeverApplyDirectly() {
    // Every set must arrive as a QUEUED request rather than a mutation: this
    // code runs on a network thread and the pipeline belongs to the GUI
    // thread. The property is that a set carries a control and a get never
    // does.
    const RadioStatus s = baseStatus();
    const char* sets[] = {"F 145000000", "M USB 2400", "V VFOA", "S 0 VFOA", "T 0"};
    for (const char* line : sets) {
        const CatResult r = run(line, s);
        CHECK(r.reply == "RPRT 0\n");
    }
    const char* gets[] = {"f", "m", "v", "s", "t", "\\chk_vfo", "\\dump_state"};
    for (const char* line : gets) {
        const CatResult r = run(line, s);
        CHECK(!r.hasControl);
        // and a successful get NEVER ends with an RPRT line
        CHECK(r.reply.find("RPRT") == std::string::npos);
    }
}

}  // namespace

int main() {
    testGetFrequency();
    testSetFrequencyInsideBandMovesOnlyTheVfo();
    testSetFrequencyOutsideBandRetunes();
    testTunePlanBoundary();
    testSetFrequencyRejectsRubbish();
    testGetMode();
    testSetMode();
    testModeTokenRoundTrip();
    testVfoAndSplit();
    testPttIsAlwaysReceive();
    testQuit();
    testUnknownCommands();
    testChkVfoAndPowerState();
    testDumpState();
    testSetsNeverApplyDirectly();

    return testSummary("test_cat_protocol");
}
