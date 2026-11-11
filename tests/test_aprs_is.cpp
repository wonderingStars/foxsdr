// Tests for net/aprs_is.hpp: the APRS-IS client against STAND-IN SERVERS on loopback only - nothing here contacts any real server, and every
// callsign is an obviously invented test value (N0CALL style). The pure pieces (callsign, passcode, login line, logresp, back-off, the gate
// rules) are tested on their own; the client is tested for what it writes, what it hands on, keep-alive, reconnect with back-off, stop, and
// a hostile server.
//
// The passcodes used are arbitrary digit strings typed here; no passcode is computed anywhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/aprs_is.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::msSince;

namespace {

bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto t0 = nettest::Clock::now();
    while (!pred()) {
        if (msSince(t0) > ms) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

AprsIsConfig cfgFor(const nettest::StandInServer& srv) {
    AprsIsConfig c;
    c.host = "127.0.0.1";
    c.port = srv.port();
    c.callsign = "N0CALL";
    c.softwareName = "FoxSDR";
    c.softwareVersion = "1.2.3";
    c.loginWaitMs = 1000;
    c.backoffInitialMs = 100;
    c.backoffMaxMs = 400;
    c.idleTimeoutMs = 5000;
    return c;
}

// ---- pure pieces -----------------------------------------------------------------------------------------------------------------

void testCallsigns() {
    const char* good[] = {"N0CALL", "n0call", "N0CALL-5", "N0CALL-15", "AB1CD-1", "ABC", "ABCDEFGHI", "ABCDEF-12", "W1AW-Z"};
    for (const char* g : good) {
        std::string c = g;
        CHECK(aprsNormaliseCallsign(c));
        CHECK(c == [&] { std::string u = g; for (char& ch : u) { if (ch >= 'a' && ch <= 'z') { ch = static_cast<char>(ch - 32); } } return u; }());
    }
    const char* bad[] = {"",         "AB",       "N0CALL-0",   "N0CALL-00", "N0CALL-",   "N0CALL-123", "N0CALL-1-2", "ABCDEFGHIJ", "N0CALL 1",
                         "N0,CALL",  "N0CALL\r", "N0CALL\n",   "-5",        "AB-5",      "N0CALL*",    "N0CALL>1",   "N0:CALL",   "ABCDEF12",  "12345678",
                         "ABCDEFAB", "N0CALL-1,"};
    for (const char* b : bad) {
        std::string c = b;
        CHECK(!aprsNormaliseCallsign(c));
    }
    // exactly eight characters of 0-9 / A-F are refused (they would look like a server-generated address); eight with a G are fine
    std::string hex8 = "DEADBEEF";
    CHECK(!aprsNormaliseCallsign(hex8));
    std::string near8 = "DEADBEEG";
    CHECK(aprsNormaliseCallsign(near8));
    std::string nine = "DEADBEEF1";
    CHECK(aprsNormaliseCallsign(nine));
}

void testPasscodes() {
    for (const char* g : {"-1", "0", "7", "12345", "32767", "00001"}) { CHECK(aprsPasscodeWellFormed(g)); }
    for (const char* b : {"", "-2", "-", "32768", "123456", "12a45", " 123", "123 ", "+123", "1.5", "-0", "0x1F"}) { CHECK(!aprsPasscodeWellFormed(b)); }
}

void testLoginLine() {
    AprsIsConfig c;
    c.callsign = "n0call-5";
    c.softwareName = "FoxSDR";
    c.softwareVersion = "0.99.109";
    std::string line, err;
    // receive-only: pass -1 whatever the configuration holds
    c.passcode = "12345";
    CHECK(aprsBuildLogin(c, line, err));
    CHECK(line == "user N0CALL-5 pass -1 vers FoxSDR 0.99.109\r\n");
    c.filter = "r/33/-96/25";
    CHECK(aprsBuildLogin(c, line, err));
    CHECK(line == "user N0CALL-5 pass -1 vers FoxSDR 0.99.109 filter r/33/-96/25\r\n");
    // the documentation's own receive-only example, rebuilt field by field
    AprsIsConfig d;
    d.callsign = "N0CALL-TS";
    d.softwareName = "testsoftware";
    d.softwareVersion = "1.0_05";
    d.filter = "r/33.25/-96.5/50";
    CHECK(aprsBuildLogin(d, line, err));
    CHECK(line == "user N0CALL-TS pass -1 vers testsoftware 1.0_05 filter r/33.25/-96.5/50\r\n");
    // gateway: the user's passcode is used
    c.gateway = true;
    CHECK(aprsBuildLogin(c, line, err));
    CHECK(line == "user N0CALL-5 pass 12345 vers FoxSDR 0.99.109 filter r/33/-96/25\r\n");
    CHECK(aprsRedactedLogin(line) == "user N0CALL-5 pass *** vers FoxSDR 0.99.109 filter r/33/-96/25\r\n");
    CHECK(aprsRedactedLogin(line).find("12345") == std::string::npos);
    CHECK(aprsRedactedLogin("garbage") == "garbage");
    // gateway without a usable passcode is refused, in words, and the passcode text is not in the message
    c.passcode = "";
    CHECK(!aprsBuildLogin(c, line, err) && err.find("passcode") != std::string::npos);
    c.passcode = "-1";
    CHECK(!aprsBuildLogin(c, line, err));
    c.passcode = "99999999";
    CHECK(!aprsBuildLogin(c, line, err) && err.find("99999999") == std::string::npos);
    // refusals
    c = AprsIsConfig();
    c.callsign = "N0CALL";
    CHECK(aprsBuildLogin(c, line, err));
    c.callsign = "N0CALL-0";
    CHECK(!aprsBuildLogin(c, line, err));
    c.callsign = "N0CALL";
    c.softwareName = "Fox SDR";
    CHECK(!aprsBuildLogin(c, line, err));
    c.softwareName = "";
    CHECK(!aprsBuildLogin(c, line, err));
    c.softwareName = "FoxSDR";
    c.softwareVersion = "1 2";
    CHECK(!aprsBuildLogin(c, line, err));
    c.softwareVersion = "1";
    c.filter = "r/1/2/3\r\n#injected";
    CHECK(!aprsBuildLogin(c, line, err));
    c.filter = std::string("r/1/2/3") + std::string(1, '\0') + "x";
    CHECK(!aprsBuildLogin(c, line, err));
    c.filter = "r/\x01/2";
    CHECK(!aprsBuildLogin(c, line, err));
    c.filter = std::string(201, 'x');
    CHECK(!aprsBuildLogin(c, line, err));
    c.filter = std::string(200, 'x');
    CHECK(aprsBuildLogin(c, line, err));
    CHECK(line.size() <= kAprsMaxLineBytes);
}

void testClassifyAndLogresp() {
    CHECK(aprsClassify("") == AprsLine::Empty);
    CHECK(aprsClassify("# javAPRSSrvr 4.3.0") == AprsLine::Comment);
    CHECK(aprsClassify("#") == AprsLine::Comment);
    CHECK(aprsClassify("N0CALL>APRS:>status") == AprsLine::Packet);
    CHECK(aprsClassify(" # not a comment (leading space)") == AprsLine::Packet);
    auto lr = [](const char* s) { return aprsParseLogresp(s); };
    CHECK(lr("# logresp N0CALL verified, server T2TEST").isLogresp && lr("# logresp N0CALL verified, server T2TEST").verified);
    CHECK(lr("# logresp N0CALL unverified, server T2TEST").isLogresp && !lr("# logresp N0CALL unverified, server T2TEST").verified);
    CHECK(lr("# LOGRESP n0call VERIFIED, server X").verified);
    CHECK(!lr("# javAPRSSrvr 4.3.0b12").isLogresp);
    CHECK(!lr("# verified").isLogresp);                       // the word alone is not an answer
    CHECK(!lr("N0CALL>APRS:logresp verified").isLogresp);     // a packet that mentions it is not a comment
    CHECK(!lr("# logresp N0CALL notverified, server").verified);
    CHECK(lr("# logresp N0CALL").isLogresp && !lr("# logresp N0CALL").verified);
}

void testBackoff() {
    CHECK(aprsNextBackoffMs(0, 1000, 60000) == 1000);
    CHECK(aprsNextBackoffMs(1, 1000, 60000) == 2000);
    CHECK(aprsNextBackoffMs(2, 1000, 60000) == 4000);
    CHECK(aprsNextBackoffMs(5, 1000, 60000) == 32000);
    CHECK(aprsNextBackoffMs(6, 1000, 60000) == 60000);
    CHECK(aprsNextBackoffMs(7, 1000, 60000) == 60000);
    CHECK(aprsNextBackoffMs(1000000, 1000, 60000) == 60000);  // no overflow, no loop of a million doublings
    CHECK(aprsNextBackoffMs(-3, 1000, 60000) == 1000);
    CHECK(aprsNextBackoffMs(3, 0, 10) == 8);                  // initial floors at 1
    CHECK(aprsNextBackoffMs(3, 500, 100) == 500);             // max below initial: initial
    int prev = 0;
    for (int a = 0; a < 40; ++a) {
        const int v = aprsNextBackoffMs(a, 1000, 60000);
        CHECK(v >= prev && v <= 60000);
        prev = v;
    }
}

// ---- the gate rules ----------------------------------------------------------------------------------------------------------------

GateDecision gate(const std::string& tnc2, bool ctl = true, bool passall = false) {
    GateInput in;
    in.tnc2 = tnc2;
    in.controlPidOk = ctl;
    in.tncPassAll = passall;
    return aprsGate(in, "N0GATE-10");
}

void testGateRules() {
    // the control case: a plain position report is gated, path unchanged except ",qAO,IGATE" appended to the header
    GateDecision d = gate("N0CALL-9>APRS,WIDE1-1*,WIDE2-1:!4903.50N/07201.75W-Test");
    CHECK(d.reason == GateReason::Gated);
    CHECK(d.line == "N0CALL-9>APRS,WIDE1-1*,WIDE2-1,qAO,N0GATE-10:!4903.50N/07201.75W-Test");
    CHECK(d.line.find("qAR") == std::string::npos);
    // FoxSDR never transmits: a receive-only IGate uses qAO, and qAR is never produced
    d = gate("N0CALL-9>APRS,WIDE1-1*:>status");
    CHECK(d.reason == GateReason::Gated && d.line == "N0CALL-9>APRS,WIDE1-1*,qAO,N0GATE-10:>status");
    // no path at all
    d = gate("N0CALL>APRS:>hi");
    CHECK(d.reason == GateReason::Gated && d.line == "N0CALL>APRS,qAO,N0GATE-10:>hi");
    // the information field is carried untouched, even when it holds colons, commas and non-ASCII bytes
    d = gate("N0CALL>APRS:=:a,b:c\xE2\x80\x94" "d");
    CHECK(d.reason == GateReason::Gated && d.line == "N0CALL>APRS,qAO,N0GATE-10:=:a,b:c\xE2\x80\x94" "d");

    // RULE 1: no control 0x03 / PID 0xF0
    CHECK(gate("N0CALL>APRS:>hi", false).reason == GateReason::NotUiFrame);
    // RULE 2: TNC PASSALL on
    CHECK(gate("N0CALL>APRS:>hi", true, true).reason == GateReason::PassAll);
    // RULE 5: TCPIP, TCPXX, NOGATE, RFONLY in the header (as a path element, with or without the used-mark, any case)
    for (const char* e : {"TCPIP", "TCPIP*", "TCPXX", "TCPXX*", "NOGATE", "RFONLY", "nogate", "tcpip*"}) {
        const std::string pkt = std::string("N0CALL>APRS,WIDE1-1,") + e + ":>hi";
        CHECK(gate(pkt).reason == GateReason::ForbiddenPathElement);
    }
    CHECK(gate("N0CALL>TCPIP:>hi").reason == GateReason::Malformed || gate("N0CALL>TCPIP:>hi").reason == GateReason::ForbiddenPathElement);
    CHECK(gate("N0CALL>APRS,TCPIP,WIDE1-1:>hi").reason == GateReason::ForbiddenPathElement);  // first of several
    CHECK(gate("N0CALL>APRS,WIDE1-1,TCPIPX:>hi").reason == GateReason::Gated);               // a different name is not TCPIP
    // RULES 9 and 10: q constructs and the I construct never appear on RF
    for (const char* e : {"qAR", "qAO", "qAC", "qAX", "qAo", "I", "qAZ"}) {
        const std::string pkt = std::string("N0CALL>APRS,WIDE1-1,") + e + ",N0XYZ:>hi";
        CHECK(gate(pkt).reason == GateReason::QConstructOnRf);
    }
    // RULE 4: generic queries
    CHECK(gate("N0CALL>APRS:?APRS?").reason == GateReason::GenericQuery);
    CHECK(gate("N0CALL>APRS:?IGATE?").reason == GateReason::GenericQuery);
    CHECK(gate("N0CALL>APRS:!?not a query (position)").reason == GateReason::Gated);  // '?' elsewhere is data
    // RULE 3: third-party with TCPIP / TCPXX in the third-party header
    CHECK(gate("N0RF>APRS,WIDE1-1:}N0INT>APRS,TCPIP,N0GATE*:>from the internet").reason == GateReason::ThirdPartyInternet);
    CHECK(gate("N0RF>APRS:}N0INT>APRS,TCPXX*:>x").reason == GateReason::ThirdPartyInternet);
    CHECK(gate("N0RF>APRS:}N0INT>APRS,tcpip*:>x").reason == GateReason::ThirdPartyInternet);
    // RULE 6: third party WITHOUT them: the RF header and the '}' are removed, the inner packet is what goes out (with the append)
    d = gate("N0RF>APRS,WIDE1-1*:}N0INT>APRS,WIDE2-1:>inner status");
    CHECK(d.reason == GateReason::Gated);
    CHECK(d.line == "N0INT>APRS,WIDE2-1,qAO,N0GATE-10:>inner status");
    CHECK(d.line.find("N0RF") == std::string::npos && d.line.find('}') == std::string::npos);
    // third party: a NOGATE/RFONLY or q construct in the inner header is refused too; a generic query inside is refused; nesting is refused
    CHECK(gate("N0RF>APRS:}N0INT>APRS,NOGATE:>x").reason == GateReason::ForbiddenPathElement);
    CHECK(gate("N0RF>APRS:}N0INT>APRS,qAR,N0X:>x").reason == GateReason::QConstructOnRf);
    CHECK(gate("N0RF>APRS:}N0INT>APRS:?APRS?").reason == GateReason::GenericQuery);
    CHECK(gate("N0RF>APRS:}N0A>APRS:}N0B>APRS:>x").reason == GateReason::NestedThirdParty);
    CHECK(gate("N0RF>APRS:}garbage without a header").reason == GateReason::Malformed);
    CHECK(gate("N0RF>APRS:}").reason == GateReason::Malformed);
    // RULE 7: the path is not modified: only the one append; the header text before it is byte-identical
    d = gate("N0CALL-9>APRS,WIDE1-1*,WIDE2-1:!x");
    CHECK(d.line.compare(0, 31, "N0CALL-9>APRS,WIDE1-1*,WIDE2-1,") == 0);
    // malformed input: empty, no colon, no '>', empty data, control characters, bad elements, too many digipeaters, injection attempts
    for (const char* m : {"", "N0CALL>APRS", "N0CALL>APRS:", "N0CALL:>hi", ">APRS:>hi", "N0CALL>:>hi", "N0CALL>APRS,:>hi", "N0 CALL>APRS:>hi",
                          "N0CALL>APRS,WIDE1-1,WIDE2-1,A,B,C,D,E,F,G:>hi", "N0CALL>APRS,TOOLONGELEMENT:>hi"}) {
        CHECK(gate(m).reason == GateReason::Malformed);
    }
    CHECK(gate("N0CALL>APRS:>hi\r\nN0EVIL>APRS:>second packet").reason == GateReason::Malformed);
    CHECK(gate("N0CALL>APRS:>hi\n").reason == GateReason::Malformed);
    CHECK(gate(std::string("N0CALL>APRS:>h") + std::string(1, '\0') + "i").reason == GateReason::Malformed);
    // length: the result (with the append) must fit 510 bytes
    const std::string longData(400, 'x');
    CHECK(gate("N0CALL>APRS:>" + longData).reason == GateReason::Gated);
    const std::string tooLong(500, 'x');
    CHECK(gate("N0CALL>APRS:>" + tooLong).reason == GateReason::TooLong);
    // exact boundary: a gated line of 510 bytes is allowed, 511 is not
    const std::string base = "N0CALL>APRS,qAO,N0GATE-10:>";
    const std::string fit(510 - base.size(), 'y');
    CHECK(gate("N0CALL>APRS:>" + fit).reason == GateReason::Gated);
    CHECK(gate("N0CALL>APRS:>" + fit + "z").reason == GateReason::TooLong);
    CHECK(gate("N0CALL>APRS:>" + fit).line.size() == 510);
    // every reason has a name
    for (int r = 0; r <= static_cast<int>(GateReason::QueueFull); ++r) { CHECK(std::string(aprsGateReasonName(static_cast<GateReason>(r))) != "?"); }
}

// ---- the client against stand-in servers ---------------------------------------------------------------------------------------------

void testConfigRefusals() {
    nettest::StandInServer srv;
    CHECK(srv.start());
    std::string err;
    {
        AprsIsConfig c = cfgFor(srv);
        c.host = "";
        CHECK(!AprsIsClient(c).start(err) && err.find("no APRS-IS server") != std::string::npos);
        c = cfgFor(srv);
        c.callsign = "N0CALL-0";
        CHECK(!AprsIsClient(c).start(err));
        c = cfgFor(srv);
        c.gateway = true;  // without a passcode
        CHECK(!AprsIsClient(c).start(err) && err.find("passcode") != std::string::npos);
        c = cfgFor(srv);
        c.port = 0;
        CHECK(!AprsIsClient(c).start(err));
        c = cfgFor(srv);
        c.backoffMaxMs = 5;
        CHECK(!AprsIsClient(c).start(err));
        AprsIsClient ok(cfgFor(srv));
        CHECK(ok.start(err));
        CHECK(!ok.start(err) && err == "already running");
    }
    CHECK(srv.accepted() <= 1);  // none of the refused configurations touched the network
}

void testReceiveOnlyLoginAndPackets() {
    nettest::StandInServer srv;
    srv.setGreeting("# javAPRSSrvr 4.3.0b12 (stand-in)\r\n");
    CHECK(srv.start());
    AprsIsConfig c = cfgFor(srv);
    c.filter = "r/33/-96/25";
    c.passcode = "54321";  // typed in, but the gateway is off: it must never be sent
    std::mutex m;
    std::vector<std::string> got;
    AprsIsClient cl(c);
    cl.setPacketHandler([&](const std::string& l) {
        std::lock_guard<std::mutex> g(m);
        got.push_back(l);
    });
    std::string err;
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return srv.received(0).find("\r\n") != std::string::npos; }, 3000));
    CHECK(srv.received(0) == "user N0CALL pass -1 vers FoxSDR 1.2.3 filter r/33/-96/25\r\n");
    CHECK(srv.received(0).find("54321") == std::string::npos);
    CHECK(cl.redactedLogin().find("54321") == std::string::npos);
    // the server answers, then sends comments (keep-alives) and packets, split across segments and with LF-only endings
    srv.sendTo(0, "# logresp N0CALL unverified, server T2STAND\r\n");
    srv.sendTo(0, "N0AAA>APRS,TCPIP*:>first\r\n# keep-alive\r\nN0BBB>APRS:>sec");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    srv.sendTo(0, "ond\nN0CCC>APRS:>third\r\n\r\n");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> g(m); return got.size() == 3; }, 3000));
    {
        std::lock_guard<std::mutex> g(m);
        CHECK(got.size() == 3);
        if (got.size() == 3) {
            CHECK(got[0] == "N0AAA>APRS,TCPIP*:>first");
            CHECK(got[1] == "N0BBB>APRS:>second");
            CHECK(got[2] == "N0CCC>APRS:>third");
        }
    }
    const AprsStats s = cl.stats();
    CHECK(s.packetsReceived == 3 && s.commentLines == 3 && s.connects == 1);  // identification, logresp, keep-alive
    // RECEIVE-ONLY: the login line is the only thing this client ever wrote
    CHECK(s.bytesWritten == srv.received(0).size());
    GateInput gi;
    gi.tnc2 = "N0CALL>APRS:>hi";
    CHECK(cl.forward(gi) == GateReason::GatewayOff);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(srv.received(0) == "user N0CALL pass -1 vers FoxSDR 1.2.3 filter r/33/-96/25\r\n");
    CHECK(cl.stats().gateRefused == 1 && cl.stats().packetsForwarded == 0);
    cl.stop();
    CHECK(cl.state() == AprsState::Stopped);
}

void testLoginWaitsForTheServerLine() {
    nettest::StandInServer srv;  // no greeting: the client waits loginWaitMs and then logs in anyway
    CHECK(srv.start());
    AprsIsConfig c = cfgFor(srv);
    c.loginWaitMs = 400;
    AprsIsClient cl(c);
    std::string err;
    const auto t0 = nettest::Clock::now();
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return srv.accepted() == 1; }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(srv.received(0).empty());  // still waiting for the identification line
    CHECK(waitFor([&] { return !srv.received(0).empty(); }, 2000));
    CHECK(msSince(t0) >= 380.0);
    // with a greeting the login follows at once
    nettest::StandInServer srv2;
    srv2.setGreeting("# hello\r\n");
    CHECK(srv2.start());
    AprsIsConfig c2 = cfgFor(srv2);
    c2.loginWaitMs = 4000;
    AprsIsClient cl2(c2);
    const auto t1 = nettest::Clock::now();
    CHECK(cl2.start(err));
    CHECK(waitFor([&] { return !srv2.received(0).empty(); }, 3000));
    CHECK(msSince(t1) < 1500.0);
}

void testKeepAliveAndIdleReconnect() {
    nettest::StandInServer srv;
    srv.setGreeting("# hello\r\n");
    CHECK(srv.start());
    AprsIsConfig c = cfgFor(srv);
    c.idleTimeoutMs = 400;
    AprsIsClient cl(c);
    std::string err;
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return srv.accepted() == 1 && !srv.received(0).empty(); }, 3000));
    // # lines every 100 ms for 1.5 s (three times the idle timeout): the connection is kept, no reconnect
    for (int i = 0; i < 15; ++i) {
        srv.sendTo(0, "# keep-alive " + std::to_string(i) + "\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(srv.accepted() == 1);
    CHECK(cl.stats().idleReconnects == 0 && cl.stats().connects == 1);
    CHECK(cl.stats().commentLines >= 15);
    // now the server goes silent: after the idle timeout the client drops the session and reconnects
    CHECK(waitFor([&] { return srv.accepted() >= 2; }, 4000));
    CHECK(cl.stats().idleReconnects >= 1);
    CHECK(waitFor([&] { return srv.received(1).find("user N0CALL") == 0; }, 3000));  // and logs in again on the new connection
}

void testReconnectAfterDropWithBackoff() {
    nettest::StandInServer srv;
    srv.setGreeting("# hello\r\n");
    CHECK(srv.start());
    AprsIsConfig c = cfgFor(srv);
    c.backoffInitialMs = 150;
    c.backoffMaxMs = 600;
    AprsIsClient cl(c);
    std::string err;
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return !srv.received(0).empty(); }, 3000));
    // the server drops the first connection, then refuses the next four (accepts and closes at once): the gaps between accepts follow the
    // back-off 150, 300, 600, 600, 600 ms (each failed session counts as a failure; none reached "online")
    srv.refuseNext(4);
    srv.dropConn(0);
    CHECK(waitFor([&] { return srv.accepted() >= 6; }, 12000));
    const std::vector<double> t = srv.acceptTimesMs();
    CHECK(t.size() >= 6);
    if (t.size() >= 6) {
        const double gap1 = t[1] - t[0], gap2 = t[2] - t[1], gap3 = t[3] - t[2], gap4 = t[4] - t[3], gap5 = t[5] - t[4];
        std::printf("reconnect gaps (ms): %.0f %.0f %.0f %.0f %.0f (back-off 150/300/600/600/600)\n", gap1, gap2, gap3, gap4, gap5);
        CHECK(gap1 >= 130 && gap1 < 450);
        CHECK(gap2 >= 270 && gap2 < 700);
        CHECK(gap3 >= 570 && gap3 < 1100);
        CHECK(gap4 >= 570 && gap4 < 1100);  // capped at 600, not 1200
        CHECK(gap5 >= 570 && gap5 < 1100);
    }
    // the sixth connection (accepted normally) gets a login
    CHECK(waitFor([&] { return srv.received(1).find("user N0CALL") == 0; }, 5000));
    CHECK(cl.stats().failures >= 5);
}

void testBackoffResetsAfterAStableSession() {
    nettest::StandInServer srv;
    srv.setGreeting("# hello\r\n");
    CHECK(srv.start());
    AprsIsConfig c = cfgFor(srv);
    c.backoffInitialMs = 100;
    c.backoffMaxMs = 800;
    c.stableMs = 300;
    srv.refuseNext(2);  // two failures: the next wait would be 400 ms
    AprsIsClient cl(c);
    std::string err;
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return srv.accepted() >= 3 && !srv.received(0).empty(); }, 5000));
    srv.sendTo(0, "# logresp N0CALL verified, server T2STAND\r\n");
    CHECK(waitFor([&] { return cl.state() == AprsState::Online; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // longer than stableMs
    srv.dropConn(0);
    CHECK(waitFor([&] { return srv.accepted() >= 4; }, 5000));
    const std::vector<double> t = srv.acceptTimesMs();
    CHECK(t.size() >= 4);
    if (t.size() >= 4) {
        const double sinceStable = t[3] - t[2];
        std::printf("reconnect after a stable session: %.0f ms after the connection began (600 ms online + wait; reset gives ~100, no reset 400)\n", sinceStable);
        CHECK(sinceStable < 600 + 330);  // reset: 100 ms wait. Without the reset it would be 400 ms (> 1000)
    }
}

void testStopWhileBackingOffAndConnecting() {
    // nothing listens: connect is refused, the client backs off; stop() must not wait for a back-off or a connect timeout
    AprsIsConfig c;
    c.host = "127.0.0.1";
    c.port = 1;  // closed on this machine (nothing of ours listens there)
    c.callsign = "N0CALL";
    c.backoffInitialMs = 5000;
    c.backoffMaxMs = 60000;
    c.connectTimeoutMs = 5000;
    double worst = 0;
    for (int i = 0; i < 4; ++i) {
        AprsIsClient cl(c);
        std::string err;
        CHECK(cl.start(err));
        std::this_thread::sleep_for(std::chrono::milliseconds(150 + 100 * i));
        const auto t0 = nettest::Clock::now();
        cl.stop();
        worst = std::max(worst, msSince(t0));
        CHECK(cl.state() == AprsState::Stopped);
        CHECK(cl.stats().connects == 0);
    }
    std::printf("stop() while connecting / backing off: worst of 4 = %.1f ms\n", worst);
    CHECK(worst < 500.0);
    {
        // a refused connection is reported in words (Windows retries a refused SYN for a second or two before giving up)
        AprsIsClient cl(c);
        std::string err;
        CHECK(cl.start(err));
        CHECK(waitFor([&] { return !cl.lastError().empty(); }, 8000));
        CHECK(cl.lastError().find("did not accept") != std::string::npos || cl.lastError().find("could not") != std::string::npos);
        CHECK(waitFor([&] { return cl.state() == AprsState::BackingOff; }, 3000));
        CHECK(cl.stats().failures >= 1 && cl.stats().connects == 0);
    }
    // stop while a session is up and the server is silent
    nettest::StandInServer srv;
    srv.setGreeting("# hello\r\n");
    CHECK(srv.start());
    AprsIsClient cl(cfgFor(srv));
    std::string err;
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return !srv.received(0).empty(); }, 3000));
    const auto t0 = nettest::Clock::now();
    cl.stop();
    CHECK(msSince(t0) < 500.0);
    cl.stop();  // idempotent
}

void testHostileServer() {
    nettest::StandInServer srv;
    srv.setGreeting("# hello\r\n");
    CHECK(srv.start());
    AprsIsConfig c = cfgFor(srv);
    c.backoffInitialMs = 100;
    c.backoffMaxMs = 200;
    std::mutex m;
    std::vector<std::string> got;
    AprsIsClient cl(c);
    cl.setPacketHandler([&](const std::string& l) {
        std::lock_guard<std::mutex> g(m);
        got.push_back(l);
    });
    std::string err;
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return !srv.received(0).empty(); }, 3000));
    // 1. a complete line far over 512 bytes is skipped, not handed on; the next line still arrives
    srv.sendTo(0, "N0LONG>APRS:>" + std::string(600, 'x') + "\r\nN0OK>APRS:>fine\r\n");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> g(m); return !got.empty(); }, 3000));
    {
        std::lock_guard<std::mutex> g(m);
        CHECK(got.size() == 1 && got[0] == "N0OK>APRS:>fine");
    }
    CHECK(cl.stats().longLinesSkipped == 1);
    // 2. binary junk and NULs inside a line are passed as text without trouble; an empty line is ignored
    srv.sendTo(0, std::string("N0BIN>APRS:>a") + std::string(1, '\0') + "\x01\xFF" + "b\r\n\r\n");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> g(m); return got.size() == 2; }, 3000));
    // 3. a megabyte with no line break: the session is dropped (memory is bounded) and the client reconnects
    srv.sendTo(0, std::string(1 << 20, 'z'));
    CHECK(waitFor([&] { return cl.stats().hostileDrops >= 1; }, 5000));
    CHECK(cl.lastError().find("far longer") != std::string::npos);
    CHECK(waitFor([&] { return srv.accepted() >= 2 && srv.received(1).find("user N0CALL") == 0; }, 5000));
    // 4. the server floods packets: all are delivered in order, nothing is dropped
    const int before = static_cast<int>([&] { std::lock_guard<std::mutex> g(m); return got.size(); }());
    std::string flood;
    for (int i = 0; i < 2000; ++i) { flood += "N0FLD>APRS:>" + std::to_string(i) + "\r\n"; }
    srv.sendTo(1, flood);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> g(m); return static_cast<int>(got.size()) >= before + 2000; }, 8000));
    {
        std::lock_guard<std::mutex> g(m);
        bool inOrder = static_cast<int>(got.size()) == before + 2000;
        for (int i = 0; inOrder && i < 2000; ++i) { inOrder = got[static_cast<std::size_t>(before + i)] == "N0FLD>APRS:>" + std::to_string(i); }
        CHECK(inOrder);
    }
    // 5. a server that accepts and says nothing at all: login after the wait, no crash, still stoppable
    nettest::StandInServer mute;
    CHECK(mute.start());
    AprsIsConfig mc = cfgFor(mute);
    mc.loginWaitMs = 200;
    AprsIsClient mcl(mc);
    CHECK(mcl.start(err));
    CHECK(waitFor([&] { return !mute.received(0).empty(); }, 3000));
    mcl.stop();
}

void testGatewayMode() {
    nettest::StandInServer srv;
    srv.setGreeting("# hello\r\n");
    CHECK(srv.start());
    AprsIsConfig c = cfgFor(srv);
    c.gateway = true;
    c.passcode = "13579";  // an arbitrary number typed for this test
    c.callsign = "n0gate-10";
    c.filter = "r/33/-96/25";
    AprsIsClient cl(c);
    std::string err;
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return !srv.received(0).empty(); }, 3000));
    CHECK(srv.received(0) == "user N0GATE-10 pass 13579 vers FoxSDR 1.2.3 filter r/33/-96/25\r\n");
    GateInput gi;
    gi.tnc2 = "N0CALL-9>APRS,WIDE1-1*:>status";
    // not yet answered: refused, nothing queued
    CHECK(cl.forward(gi) == GateReason::NotOnline);
    // the server answers "unverified": still refused (an unverified login must not gate)
    srv.sendTo(0, "# logresp N0GATE-10 unverified, server T2STAND\r\n");
    CHECK(waitFor([&] { return cl.state() == AprsState::Online; }, 3000));
    CHECK(cl.forward(gi) == GateReason::NotVerified);
    CHECK(cl.stats().packetsForwarded == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(srv.received(0).find("N0CALL-9") == std::string::npos);
    cl.stop();

    // verified login: packets are gated with ,qAO,N0GATE-10, one per line, CRLF
    nettest::StandInServer srv2;
    srv2.setGreeting("# hello\r\n");
    CHECK(srv2.start());
    AprsIsConfig c2 = c;
    c2.port = srv2.port();
    AprsIsClient cl2(c2);
    CHECK(cl2.start(err));
    CHECK(waitFor([&] { return !srv2.received(0).empty(); }, 3000));
    srv2.sendTo(0, "# logresp N0GATE-10 verified, server T2STAND\r\n");
    CHECK(waitFor([&] { return cl2.state() == AprsState::Online; }, 3000));
    CHECK(cl2.forward(gi) == GateReason::Gated);
    GateInput tp;
    tp.tnc2 = "N0RF>APRS,WIDE1-1*:}N0INT>APRS,WIDE2-1:>third party";
    CHECK(cl2.forward(tp) == GateReason::Gated);
    // every never-gate rule at the client's door: refused, counted, never on the wire
    GateInput bad;
    bad.tnc2 = "N0CALL>APRS,TCPIP*:>x";
    CHECK(cl2.forward(bad) == GateReason::ForbiddenPathElement);
    bad.tnc2 = "N0CALL>APRS:?APRS?";
    CHECK(cl2.forward(bad) == GateReason::GenericQuery);
    bad.tnc2 = "N0CALL>APRS:>x";
    bad.tncPassAll = true;
    CHECK(cl2.forward(bad) == GateReason::PassAll);
    bad.tncPassAll = false;
    bad.controlPidOk = false;
    CHECK(cl2.forward(bad) == GateReason::NotUiFrame);
    bad.controlPidOk = true;
    bad.tnc2 = "N0RF>APRS:}N0INT>APRS,TCPXX*:>x";
    CHECK(cl2.forward(bad) == GateReason::ThirdPartyInternet);
    const std::string want = "N0CALL-9>APRS,WIDE1-1*,qAO,N0GATE-10:>status\r\nN0INT>APRS,WIDE2-1,qAO,N0GATE-10:>third party\r\n";
    CHECK(waitFor([&] { return srv2.received(0).size() >= std::string("user N0GATE-10 pass 13579 vers FoxSDR 1.2.3 filter r/33/-96/25\r\n").size() + want.size(); }, 3000));
    const std::string all = srv2.received(0);
    CHECK(all == "user N0GATE-10 pass 13579 vers FoxSDR 1.2.3 filter r/33/-96/25\r\n" + want);
    CHECK(cl2.stats().packetsForwarded == 2 && cl2.stats().gateRefused == 5);
    // the passcode is in the login on the wire and nowhere the client reports
    CHECK(cl2.redactedLogin().find("13579") == std::string::npos);
    CHECK(cl2.lastError().find("13579") == std::string::npos);

    // qAO and never qAR
    nettest::StandInServer srv3;
    srv3.setGreeting("# hello\r\n");
    CHECK(srv3.start());
    AprsIsConfig c3 = c;
    c3.port = srv3.port();
    AprsIsClient cl3(c3);
    CHECK(cl3.start(err));
    CHECK(waitFor([&] { return !srv3.received(0).empty(); }, 3000));
    srv3.sendTo(0, "# logresp N0GATE-10 verified, server T2STAND\r\n");
    CHECK(waitFor([&] { return cl3.state() == AprsState::Online; }, 3000));
    CHECK(cl3.forward(gi) == GateReason::Gated);
    CHECK(waitFor([&] { return srv3.received(0).find(",qAO,N0GATE-10:") != std::string::npos; }, 3000));
    CHECK(srv3.received(0).find(",qAR,") == std::string::npos);
}

void testForwardQueueBound() {
    // a verified gateway whose server never reads: forward() never blocks, and the queue is bounded (QueueFull is reported)
    nettest::StandInServer srv;
    srv.setGreeting("# hello\r\n");
    CHECK(srv.start());
    AprsIsConfig c = cfgFor(srv);
    c.gateway = true;
    c.passcode = "24680";
    c.outQueueBytes = 4096;
    AprsIsClient cl(c);
    std::string err;
    CHECK(cl.start(err));
    CHECK(waitFor([&] { return !srv.received(0).empty(); }, 3000));
    srv.sendTo(0, "# logresp N0CALL verified, server T2STAND\r\n");
    CHECK(waitFor([&] { return cl.state() == AprsState::Online; }, 3000));
    GateInput gi;
    gi.tnc2 = "N0CALL-9>APRS:>" + std::string(200, 'q');
    int queued = 0, full = 0;
    const auto t0 = nettest::Clock::now();
    for (int i = 0; i < 2000; ++i) {
        const GateReason r = cl.forward(gi);
        if (r == GateReason::Gated) { ++queued; }
        if (r == GateReason::QueueFull) { ++full; }
    }
    CHECK(msSince(t0) < 1000.0);
    CHECK(queued > 0 && queued + full >= 1);
    std::printf("forward(): %d queued, %d QueueFull of 2000 calls in %.1f ms\n", queued, full, msSince(t0));
}

}  // namespace

int main() {
    testCallsigns();
    testPasscodes();
    testLoginLine();
    testClassifyAndLogresp();
    testBackoff();
    testGateRules();
    testConfigRefusals();
    testReceiveOnlyLoginAndPackets();
    testLoginWaitsForTheServerLine();
    testKeepAliveAndIdleReconnect();
    testReconnectAfterDropWithBackoff();
    testBackoffResetsAfterAStableSession();
    testStopWhileBackingOffAndConnecting();
    testHostileServer();
    testGatewayMode();
    testForwardQueueBound();
    return testSummary("test_aprs_is");
}
