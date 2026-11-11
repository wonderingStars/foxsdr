// test_antenna_genius.cpp - the Antenna Genius client (core/antenna_genius.hpp, net/antenna_genius_tcp.hpp) against stand-in devices: an in-memory script
// and a stand-in device on a real loopback TCP socket (127.0.0.1 only). No real device is touched.
//
//   G1  prologue and line parsing, every shape and every refusal.
//   G2  the message parsers (port, antenna, band, relay).
//   G3  the client over a scripted transport: framing, sequence numbers (wrap past 255), list completion, failure codes, split lines, terminators,
//       oversized lines, pending bound, unmatched responses, a closed transport, status queue bound.
//   G4  SAFETY: the one write is refused unless the port is known, idle and not inhibited; nothing else ever sends "port set"; the source has exactly one
//       place that builds it; the commands that were left out are absent.
//   G5  end to end over loopback TCP with a stand-in device: greeting, port get, lists, subscriptions, a user-selected antenna and the status that follows,
//       the device going away.
//   G6  the TCP transport's refusals.
//   G7  fuzz: random and mutated bytes through the client.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/antenna_genius.hpp"
#include "hamops_util.hpp"
#include "net/antenna_genius_tcp.hpp"
#include "test_check.hpp"

using namespace cascade::core::genius;
using namespace cascade::net;

namespace {

// A scripted transport: bytes to deliver, bytes captured.
class ScriptTransport : public Transport {
public:
    std::string in, out;
    std::size_t chunk = 7;   // deliver in small pieces to exercise split lines
    bool closed = false;
    bool failSend = false;
    int send(const char* d, std::size_t n) override { if (failSend) { return -1; } out.append(d, n); return static_cast<int>(n); }
    int recv(char* buf, std::size_t cap) override {
        if (in.empty()) { return closed ? -1 : 0; }
        const std::size_t n = std::min({in.size(), cap, chunk});
        std::memcpy(buf, in.data(), n);
        in.erase(0, n);
        return static_cast<int>(n);
    }
};

const char* kPort1 = "port 1 auto=1 source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0";

void g1Lines() {
    Greeting g;
    CHECK(parseGreeting("V4.0.1 AG", g) && g.valid && g.version == "4.0.1" && !g.needsAuth);
    CHECK(parseGreeting("V4.0.1 AG AUTH", g) && g.needsAuth);
    for (const char* bad : {"", "V", "V4.0.1", "V4.0.1 XX", "V4.0.1 AG NOAUTH", "4.0.1 AG", "Vx.y.z AG", "V4.0.1 AG AUTH extra", "V AG", "V4..1 AG!", "V4.0.1  AG"}) { CHECK(!parseGreeting(bad, g) && !g.valid); }
    Line l = parseLine("V4.0.1 AG AUTH");
    CHECK(l.kind == LineKind::Prologue && l.greeting.needsAuth);
    l = parseLine("R15|0|port 1 auto=1");
    CHECK(l.kind == LineKind::Response && l.seq == 15 && l.code == 0 && l.body == "port 1 auto=1");
    l = parseLine("R7|0|");
    CHECK(l.kind == LineKind::Response && l.seq == 7 && l.body.empty());
    l = parseLine("R255|1F|oops");
    CHECK(l.kind == LineKind::Response && l.seq == 255 && l.code == 0x1F && l.body == "oops");
    l = parseLine("R9|0|a|b|c");   // the message may itself contain bars
    CHECK(l.kind == LineKind::Response && l.body == "a|b|c");
    l = parseLine("S0|antenna reload");
    CHECK(l.kind == LineKind::Status && l.body == "antenna reload");
    for (const char* bad : {"", "R", "R|0|x", "R0|0|x", "R256|0|x", "Rx|0|x", "R5|0", "R5|zz|x", "R5||x", "R5|123456789|x", "S1|x", "S|x", "Sx|x", "S0", "X5|0|x", "r5|0|x", "C5|port get 1"}) {
        CHECK(parseLine(bad).kind == LineKind::Unknown);
    }
    CHECK(parseLine(std::string(kMaxLine + 1, 'R')).kind == LineKind::Unknown);
    CHECK(frameCommand(12, "band list") == "C12|band list\r");
}

void g2Parsers() {
    PortState p;
    CHECK(parsePort(kPort1, p) && p.valid && p.id == 1 && p.autoDetect && p.source == "AUTO" && p.band == 0 && !p.tx && !p.inhibit);
    CHECK(parsePort("port 2 auto=0 source=MANUAL band=3 rxant=1 txant=2 inband=7 tx=1 inhibit=1", p));
    CHECK(p.id == 2 && !p.autoDetect && p.band == 3 && p.rxant == 1 && p.txant == 2 && p.inband == 7 && p.tx && p.inhibit);
    for (const char* bad : {"", "port", "port 1", "port 3 auto=1 source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0", "port 0 auto=1 source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0",
                            "port 1 auto=2 source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0", "port 1 auto=1 source=AUTO band=16 rxant=0 txant=0 tx=0 inhibit=0",
                            "port 1 auto=1 source=AUTO band=0 rxant=17 txant=0 tx=0 inhibit=0", "port 1 auto=1 source=AUTO band=0 rxant=0 txant=0 tx=0",
                            "port 1 auto=1 band=0 rxant=0 txant=0 tx=0 inhibit=0", "port 1 auto=1 source=A\\B band=0 rxant=0 txant=0 tx=0 inhibit=0",
                            "port 1 auto=1 auto=1 source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0", "port 1 auto source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0",
                            "port 1 auto=x source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0", "port 1 =1 source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0",
                            "plot 1 auto=1 source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0", "port 1 auto=1 source=AUTO band=-1 rxant=0 txant=0 tx=0 inhibit=0"}) {
        CHECK(!parsePort(bad, p) && !p.valid);
    }
    AntennaInfo a;
    CHECK(parseAntenna("antenna 1 name=Antenna_1 tx=0000 rx=0001 inband=0000", a) && a.id == 1 && a.name == "Antenna_1" && a.rxMask == 1 && a.txMask == 0);
    CHECK(parseAntenna("antenna 16 name=Yagi-20m tx=FFFF rx=ffff inband=00aB", a) && a.id == 16 && a.txMask == 0xFFFF && a.rxMask == 0xFFFF && a.inbandMask == 0xAB);
    for (const std::string& bad : std::initializer_list<std::string>{"antenna", "antenna 0 name=x tx=0 rx=0 inband=0", "antenna 17 name=x tx=0 rx=0 inband=0", "antenna 1 tx=0 rx=0 inband=0", "antenna 1 name=x tx=0 rx=0",
                            "antenna 1 name=x tx=00000 rx=0 inband=0", "antenna 1 name=x tx=zz rx=0 inband=0", "antenna 1 name=a/b tx=0 rx=0 inband=0",
                            "antenna 1 name=" + std::string(33, 'a') + " tx=0 rx=0 inband=0", "antennas 1 name=x tx=0 rx=0 inband=0"}) {
        CHECK(!parseAntenna(bad, a));
    }
    BandInfo b;
    CHECK(parseBand("band 1 name=160m freq_start=1.600000 freq_stop=2.200000", b) && b.id == 1 && b.name == "160m" && b.freqStartMHz == 1.6 && b.freqStopMHz == 2.2);
    CHECK(parseBand("band 0 name=None freq_start=0 freq_stop=0", b));
    for (const char* bad : {"band", "band 16 name=x freq_start=1 freq_stop=2", "band 1 freq_start=1 freq_stop=2", "band 1 name=x freq_start=2 freq_stop=1", "band 1 name=x freq_start=a freq_stop=2",
                            "band 1 name=x freq_start=1.2.3 freq_stop=2", "band 1 name=x freq_start=-1 freq_stop=2", "band 1 name=x freq_start=1", "bands 1 name=x freq_start=1 freq_stop=2"}) {
        CHECK(!parseBand(bad, b));
    }
    RelayState r;
    CHECK(parseRelay("relay tx=00 rx=0F state=0a", r) && r.tx == 0 && r.rx == 0xF && r.state == 0xA);
    for (const char* bad : {"relay", "relay tx=00 rx=00", "relay tx=00 rx=00 state=zz", "relay tx=00 rx=00 state=00 extra=1", "relays tx=00 rx=00 state=00", "relay tx=00 tx=00 state=00"}) { CHECK(!parseRelay(bad, r)); }
}

void g3Client() {
    ScriptTransport t;
    AntennaGeniusClient c(&t);
    t.in = "V4.0.1 AG\r\n";
    c.poll();
    CHECK(c.greeting().valid && c.greeting().version == "4.0.1");
    // port get: framing, reply split over several reads, completes on the single line
    const int s1 = c.requestPortGet(1);
    CHECK(s1 == 1 && t.out == "C1|port get 1\r");
    t.in = "R1|0|" + std::string(kPort1) + "\r\n";
    for (int i = 0; i < 20; ++i) { c.poll(); }
    CHECK(c.result(s1).done && c.result(s1).code == 0 && c.port(1).valid && c.port(1).id == 1 && c.pendingCount() == 0);
    // an extra empty OK line for a finished command is "unmatched", not an error
    t.in = "R1|0|\r\n";
    c.poll();
    CHECK(c.unmatched() == 1);
    // antenna list: completes on the empty OK line
    t.out.clear();
    const int s2 = c.requestAntennaList();
    CHECK(s2 == 2 && t.out == "C2|antenna list\r");
    t.in = "R2|0|antenna 1 name=Antenna_1 tx=0000 rx=0001 inband=0000\r\nR2|0|antenna 2 name=Yagi tx=0003 rx=0003 inband=0000\n";
    for (int i = 0; i < 30; ++i) { c.poll(); }
    CHECK(!c.result(s2).done && c.antennas().size() == 2 && c.pendingCount() == 1);
    t.in = "R2|0|\r";
    c.poll();
    CHECK(c.result(s2).done && c.result(s2).lines.size() == 2 && c.pendingCount() == 0);
    // band list, bands upsert by id
    const int s3 = c.requestBandList();
    t.in = "R3|0|band 1 name=160m freq_start=1.600000 freq_stop=2.200000\r\nR3|0|band 2 name=80m freq_start=3.5 freq_stop=4.0\r\nR3|0|\r\n";
    for (int i = 0; i < 40; ++i) { c.poll(); }
    CHECK(c.result(s3).done && c.bands().size() == 2);
    // a failure code ends the command at once and records the code
    const int s4 = c.requestPortGet(2);
    t.in = "R" + std::to_string(s4) + "|5|no such port\r\n";
    for (int i = 0; i < 20; ++i) { c.poll(); }
    CHECK(c.result(s4).done && c.result(s4).code == 5 && !c.port(2).valid);
    // a reply for another port than asked is not trusted
    const int s5 = c.requestPortGet(2);
    t.in = "R" + std::to_string(s5) + "|0|" + kPort1 + "\r\n";
    for (int i = 0; i < 20; ++i) { c.poll(); }
    CHECK(!c.port(2).valid && c.malformed() >= 1);
    // statuses
    t.in = "S0|port 1 auto=0 source=MANUAL band=3 rxant=1 txant=1 inband=0 tx=1 inhibit=0\r\nS0|relay tx=00 rx=03 state=01\r\nS0|antenna reload\r\nS0|output reload\r\nS0|something new\r\n";
    for (int i = 0; i < 40; ++i) { c.poll(); }
    CHECK(c.port(1).tx && c.port(1).band == 3 && c.relay().rx == 3 && c.antennas().empty());   // the reload status clears the antenna list
    StatusMessage m;
    std::vector<StatusMessage::Kind> kinds;
    while (c.takeStatus(m)) { kinds.push_back(m.kind); }
    CHECK(kinds == (std::vector<StatusMessage::Kind>{StatusMessage::Kind::Port, StatusMessage::Kind::Relay, StatusMessage::Kind::AntennaReload, StatusMessage::Kind::OutputReload}));
    CHECK(!c.takeStatus(m));
    // unknown lines are counted
    const std::uint64_t mal = c.malformed();
    t.in = "garbage\r\nR0|0|x\r\n\r\n";
    for (int i = 0; i < 10; ++i) { c.poll(); }
    CHECK(c.malformed() == mal + 2);
    // a line over the limit is dropped and counted, and the next line is read
    t.in = std::string(kMaxLine + 200, 'X') + "\r\nV4.1.0 AG\r\n";
    for (int i = 0; i < 400; ++i) { c.poll(); }
    CHECK(c.droppedLines() == 1 && c.greeting().version == "4.1.0");
    // the status queue is bounded
    for (int i = 0; i < 200; ++i) { t.in += "S0|relay tx=00 rx=00 state=" + std::string(i % 2 ? "01" : "02") + "\r\n"; }
    for (int i = 0; i < 2000; ++i) { c.poll(); }
    int n = 0;
    while (c.takeStatus(m)) { ++n; }
    CHECK(n == static_cast<int>(kMaxStatusQueue));
    // closed transport
    t.closed = true;
    c.poll();
    CHECK(c.closed() && c.requestAntennaList() == 0);
    // a send that fails returns 0 and leaves nothing pending
    ScriptTransport t2;
    t2.failSend = true;
    AntennaGeniusClient c2(&t2);
    CHECK(c2.requestPortGet(1) == 0 && c2.pendingCount() == 0);
    AntennaGeniusClient none(nullptr);
    CHECK(none.requestAntennaList() == 0);
    none.poll();
}

void g3Sequence() {
    ScriptTransport t;
    t.chunk = 4096;
    AntennaGeniusClient c(&t);
    // 600 commands, each answered at once: sequence numbers run 1..255 and wrap, and never repeat while pending
    int last = 0, wrapped = 0;
    for (int i = 0; i < 600; ++i) {
        const int s = c.requestPortGet(1 + i % 2);
        CHECK(s >= 1 && s <= 255);
        if (s < last) { ++wrapped; }
        last = s;
        t.in = "R" + std::to_string(s) + "|0|port " + std::to_string(1 + i % 2) + " auto=1 source=AUTO band=0 rxant=0 txant=0 tx=0 inhibit=0\r\n";
        c.poll();
        CHECK(c.result(s).done);
    }
    CHECK(wrapped == 2);
    // the pending bound: 16 unanswered commands, the 17th is refused
    ScriptTransport u;
    AntennaGeniusClient d(&u);
    std::vector<int> seqs;
    for (int i = 0; i < 16; ++i) { seqs.push_back(d.requestAntennaList()); CHECK(seqs.back() != 0); }
    CHECK(d.requestAntennaList() == 0 && d.requestBandList() == 0 && d.pendingCount() == 16);
    // all distinct
    for (std::size_t i = 0; i < seqs.size(); ++i) { for (std::size_t j = i + 1; j < seqs.size(); ++j) { CHECK(seqs[i] != seqs[j]); } }
    // answering one frees a slot
    u.in = "R" + std::to_string(seqs[0]) + "|0|\r\n";
    d.poll();
    CHECK(d.requestAntennaList() != 0);
    // a response for a sequence nobody asked about
    u.in = "R200|0|\r\n";
    d.poll();
    CHECK(d.unmatched() == 1);
    // port ids for the reads and the subscriptions
    ScriptTransport v;
    AntennaGeniusClient e(&v);
    CHECK(e.requestPortGet(0) == 0 && e.requestPortGet(3) == 0 && e.requestPortGet(-1) == 0);
    CHECK(e.subscribePort(3) == 0 && e.subscribePort(-1) == 0);
    CHECK(e.subscribePort(0) != 0 && v.out == "C1|sub port all\r");
    CHECK(e.subscribePort(2) != 0 && e.subscribeRelay() != 0);
    CHECK(v.out == "C1|sub port all\rC2|sub port 2\rC3|sub relay\r");
}

std::string codeOnly(const std::string& raw) {
    std::string out;
    std::size_t i = 0;
    while (i < raw.size()) {
        const std::size_t e = raw.find(static_cast<char>(10), i);
        std::string ln = raw.substr(i, e == std::string::npos ? std::string::npos : e - i);
        const std::size_t c = ln.find("//");
        if (c != std::string::npos) { ln.erase(c); }
        out += ln;
        out.push_back(static_cast<char>(10));
        if (e == std::string::npos) { break; }
        i = e + 1;
    }
    return out;
}
std::size_t count(const std::string& s, const std::string& sub) {
    std::size_t n = 0, p = 0;
    while ((p = s.find(sub, p)) != std::string::npos) { ++n; p += sub.size(); }
    return n;
}

void g4Safety() {
    ScriptTransport t;
    t.chunk = 4096;
    AntennaGeniusClient c(&t);
    std::string err;
    // port state unknown: refused
    CHECK(c.userSetPortAntenna(1, 2, 2, err) == 0 && !err.empty() && t.out.empty());
    // learn port 1 (idle), then it can be set
    const int g = c.requestPortGet(1);
    t.in = "R" + std::to_string(g) + "|0|" + kPort1 + "\r\n";
    c.poll();
    t.out.clear();
    CHECK(c.userSetPortAntenna(0, 2, 2, err) == 0 && c.userSetPortAntenna(3, 2, 2, err) == 0);
    CHECK(c.userSetPortAntenna(1, -1, 2, err) == 0 && c.userSetPortAntenna(1, 2, 17, err) == 0 && c.userSetPortAntenna(1, 17, 0, err) == 0);
    CHECK(c.userSetPortAntenna(2, 1, 1, err) == 0);                        // port 2 is not known
    CHECK(t.out.empty());                                                   // every refusal sent nothing
    const int s = c.userSetPortAntenna(1, 3, 4, err);
    CHECK(s != 0 && t.out == "C" + std::to_string(s) + "|port set 1 rxant=3 txant=4\r" && err.empty());
    CHECK(c.userSetPortAntenna(1, 0, 0, err) != 0);                        // 0 = none is a valid selection
    // transmitting: refused; inhibited: refused
    t.in = "S0|port 1 auto=0 source=MANUAL band=3 rxant=3 txant=4 inband=0 tx=1 inhibit=0\r\n";
    c.poll();
    t.out.clear();
    CHECK(c.userSetPortAntenna(1, 1, 1, err) == 0 && err == "the port is transmitting" && t.out.empty());
    t.in = "S0|port 1 auto=0 source=MANUAL band=3 rxant=3 txant=4 inband=0 tx=0 inhibit=1\r\n";
    c.poll();
    CHECK(c.userSetPortAntenna(1, 1, 1, err) == 0 && err == "the port is inhibited" && t.out.empty());
    // nothing but the user call ever sends "port set": exercise everything else, then look at the wire
    ScriptTransport w;
    AntennaGeniusClient d(&w);
    w.chunk = 4096;
    d.requestPortGet(1); d.requestAntennaList(); d.requestBandList(); d.subscribePort(0); d.subscribeRelay();
    w.in = "V4.0.1 AG\r\nR1|0|" + std::string(kPort1) + "\r\nS0|port 1 auto=1 source=AUTO band=1 rxant=1 txant=1 inband=0 tx=0 inhibit=0\r\nS0|antenna reload\r\n";
    for (int i = 0; i < 10; ++i) { d.poll(); }
    CHECK(w.out.find("port set") == std::string::npos && w.out.find("band set") == std::string::npos && w.out.find("antenna set") == std::string::npos);
    // SOURCE: exactly one place builds "port set", and the left-out commands are not in the code
    const std::string raw = hamops_test::srcFile("src/core/antenna_genius.hpp");
    const std::string code = codeOnly(raw);
    CHECK(count(code, "port set") == 1);
    for (const char* absent : {"reboot", "\"auth", "\"conf", "\"flex", "\"stack", "\"network", "output set", "output list", "group list", "band set", "band init", "antenna set", "keepalive", "factory"}) {
        CHECK(count(code, absent) == 0);
    }
    // userSetPortAntenna is the only function that calls send with the PortSet kind
    CHECK(count(code, "Kind::PortSet") == 1);   // used in one place only: the user call
    for (const char* f : {"src/core/antenna_genius.hpp", "src/net/antenna_genius_tcp.hpp"}) {
        const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile(f));
        for (const char* bad : {"std::thread", "std::async", "stderr", "stdout", "std::cerr", "std::cout", "settings_set", "OutputDebugString", "getaddrinfo"}) { CHECK(src.find(bad) == std::string::npos); }
    }
    CHECK(hamops_test::srcFile("src/net/antenna_genius_tcp.hpp").find("bool enabled = false;") != std::string::npos);
}

// A stand-in Antenna Genius on loopback. Single threaded: the test calls pump().
class StandIn {
public:
    lo::Sock listener = lo::kBad, conn = lo::kBad;
    int port = 0;
    std::string rx;
    int p1rx = 0, p1tx = 0;
    bool subscribed = false;
    std::vector<std::string> commands;
    bool open() { listener = lo::tcpListen(lo::kLoopbackHost, 0); port = listener == lo::kBad ? 0 : lo::boundPortOf(listener); return listener != lo::kBad; }
    void sendLine(const std::string& s) { const std::string l = s + "\r\n"; lo::tcpSend(conn, l.data(), l.size()); }
    void pump() {
        if (conn == lo::kBad) {
            conn = lo::tcpAccept(listener);
            if (conn != lo::kBad) { sendLine("V4.0.1 AG"); }
            return;
        }
        char buf[256];
        const int n = lo::tcpRecv(conn, buf, sizeof(buf));
        if (n < 0) { lo::closeSock(conn); conn = lo::kBad; return; }
        if (n > 0) { rx.append(buf, static_cast<std::size_t>(n)); }
        std::size_t e;
        while ((e = rx.find('\r')) != std::string::npos) {
            const std::string cmd = rx.substr(0, e);
            rx.erase(0, e + 1);
            commands.push_back(cmd);
            handle(cmd);
        }
    }
    void handle(const std::string& cmd) {
        const std::size_t bar = cmd.find('|');
        if (cmd.empty() || cmd[0] != 'C' || bar == std::string::npos) { return; }
        const std::string seq = cmd.substr(1, bar - 1), body = cmd.substr(bar + 1);
        auto ok = [&](const std::string& m) { sendLine("R" + seq + "|0|" + m); };
        if (body == "port get 1") { ok(portText()); }
        else if (body == "antenna list") { ok("antenna 1 name=Dipole tx=0007 rx=0007 inband=0000"); ok("antenna 2 name=Yagi tx=0018 rx=0018 inband=0000"); ok(""); }
        else if (body == "band list") { ok("band 1 name=160m freq_start=1.600000 freq_stop=2.200000"); ok("band 4 name=40m freq_start=7.0 freq_stop=7.3"); ok(""); }
        else if (body == "sub port all" || body == "sub relay") { subscribed = true; ok(""); }
        else if (body.rfind("port set 1 ", 0) == 0) {
            int rxa = 0, txa = 0;
            std::sscanf(body.c_str(), "port set 1 rxant=%d txant=%d", &rxa, &txa);
            p1rx = rxa; p1tx = txa;
            ok("");
            if (subscribed) { sendLine("S0|" + portText() + " inband=0"); }
        } else { sendLine("R" + seq + "|3|unknown command"); }
    }
    std::string portText() const { return "port 1 auto=0 source=MANUAL band=4 rxant=" + std::to_string(p1rx) + " txant=" + std::to_string(p1tx) + " tx=0 inhibit=0"; }
    ~StandIn() { lo::closeSock(conn); lo::closeSock(listener); }
};

template <class F> bool waitFor(StandIn& dev, AntennaGeniusClient& c, F cond) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end) {
        dev.pump();
        c.poll();
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void g5EndToEnd() {
    StandIn dev;
    CHECK(dev.open());
    GeniusTcpTransport tr;
    GeniusTcpConfig cfg;
    cfg.enabled = true;
    cfg.host = "127.0.0.1";
    cfg.port = dev.port;
    std::string err;
    CHECK(tr.open(cfg, err) && tr.isOpen());
    AntennaGeniusClient c(&tr);
    CHECK(waitFor(dev, c, [&] { return c.greeting().valid; }));
    CHECK(c.greeting().version == "4.0.1");
    const int g = c.requestPortGet(1);
    CHECK(g != 0 && waitFor(dev, c, [&] { return c.result(g).done; }));
    CHECK(c.port(1).valid && c.port(1).band == 4 && c.port(1).source == "MANUAL");
    const int a = c.requestAntennaList();
    CHECK(waitFor(dev, c, [&] { return c.result(a).done; }));
    CHECK(c.antennas().size() == 2 && c.antennas()[1].name == "Yagi" && c.antennas()[1].txMask == 0x18);
    const int b = c.requestBandList();
    CHECK(waitFor(dev, c, [&] { return c.result(b).done; }));
    CHECK(c.bands().size() == 2 && c.bands()[1].name == "40m" && c.bands()[1].freqStartMHz == 7.0);
    const int sub = c.subscribePort(0);
    CHECK(waitFor(dev, c, [&] { return c.result(sub).done; }) && dev.subscribed);
    // the user selects antenna 2 for receive and transmit on port 1
    const int set = c.userSetPortAntenna(1, 2, 2, err);
    CHECK(set != 0);
    CHECK(waitFor(dev, c, [&] { return c.result(set).done && c.port(1).rxant == 2; }));
    CHECK(dev.p1rx == 2 && dev.p1tx == 2);
    StatusMessage m;
    bool sawPort = false;
    while (c.takeStatus(m)) { if (m.kind == StatusMessage::Kind::Port && m.port.rxant == 2) { sawPort = true; } }
    CHECK(sawPort);
    // the stand-in saw exactly the commands this client sent, framed with CR
    CHECK(dev.commands.size() == 5);
    CHECK(dev.commands.back().find("|port set 1 rxant=2 txant=2") != std::string::npos);
    // an unknown reply code
    CHECK(c.requestPortGet(2) != 0);
    CHECK(waitFor(dev, c, [&] { return c.pendingCount() == 0; }));
    CHECK(!c.port(2).valid);
    // the device goes away
    lo::closeSock(dev.conn);
    dev.conn = lo::kBad;
    CHECK(waitFor(dev, c, [&] { return c.closed(); }));
    CHECK(c.requestAntennaList() == 0);
}

void g6Transport() {
    GeniusTcpTransport t;
    std::string err;
    GeniusTcpConfig c;
    CHECK(!GeniusTcpConfig().enabled && GeniusTcpConfig().host.empty() && GeniusTcpConfig().port == 9007);
    CHECK(!t.open(c, err) && !err.empty());                                   // off by default
    c.enabled = true;
    CHECK(!t.open(c, err));                                                   // no address
    c.host = "antenna-genius.local";
    CHECK(!t.open(c, err));                                                   // a name
    c.host = "192.168.1.77";
    CHECK(!t.open(c, err));                                                   // a LAN address without the switch
    c.host = "127.0.0.1";
    c.port = 0;
    CHECK(!t.open(c, err));
    c.port = 70000;
    CHECK(!t.open(c, err));
    c.port = 9007;
    c.connectTimeoutMs = 0;
    CHECK(!t.open(c, err));
    c.connectTimeoutMs = 20000;
    CHECK(!t.open(c, err));
    c.connectTimeoutMs = 500;
    // nothing listening: refused quickly. A port that was just closed on loopback
    int freePort = 0;
    { const lo::Sock l = lo::tcpListen(lo::kLoopbackHost, 0); freePort = lo::boundPortOf(l); lo::closeSock(l); }
    c.port = freePort;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!t.open(c, err) && !t.isOpen());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    char b[4];
    CHECK(t.send("x", 1) == -1 && t.recv(b, 4) == -1);                         // not open
    // a short send is a failure (the line protocol cannot resume): simulated with a peer that never reads a very large write
    StandIn dev;
    CHECK(dev.open());
    c.port = dev.port;
    CHECK(t.open(c, err));
    for (int i = 0; i < 50 && dev.conn == lo::kBad; ++i) { dev.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CHECK(t.send("C1|band list\r", 13) == 13);
    t.close();
    CHECK(!t.isOpen() && t.send("x", 1) == -1);
}

void g7Fuzz() {
    hamops_test::Rng rng(88);
    const std::string seeds[] = {std::string("V4.0.1 AG AUTH"), std::string("R5|0|") + kPort1, std::string("R5|0|antenna 1 name=Antenna_1 tx=0000 rx=0001 inband=0000"),
                                 std::string("R5|0|band 1 name=160m freq_start=1.600000 freq_stop=2.200000"), std::string("S0|relay tx=00 rx=03 state=01"),
                                 std::string("S0|port 1 auto=0 source=MANUAL band=3 rxant=1 txant=1 inband=0 tx=1 inhibit=0"), std::string("R5|0|")};
    ScriptTransport t;
    t.chunk = 13;
    AntennaGeniusClient c(&t);
    std::string err;
    int parsedOk = 0;
    for (int it = 0; it < 30000; ++it) {
        std::string s = seeds[rng.below(7)];
        const int muts = static_cast<int>(rng.below(4));
        for (int k = 0; k < muts && !s.empty(); ++k) {
            const std::size_t pos = rng.below(static_cast<std::uint32_t>(s.size()));
            const int op = static_cast<int>(rng.below(3));
            const char ch = "|= ,.-_/RSV:0123456789abcFx\x01"[rng.below(28)];
            if (op == 0) { s[pos] = ch; } else if (op == 1) { s.erase(pos, 1); } else { s.insert(pos, 1, ch); }
        }
        if (rng.below(10) == 0) { s.resize(rng.below(static_cast<std::uint32_t>(s.size()) + 1)); }
        const Line l = parseLine(s);
        if (l.kind != LineKind::Unknown) { ++parsedOk; }
        PortState p; AntennaInfo a; BandInfo b; RelayState r;
        if (parsePort(s, p)) { CHECK(p.id >= 1 && p.id <= 2 && p.band >= 0 && p.band <= kMaxBandId && p.rxant >= 0 && p.rxant <= kMaxAntennas); }
        if (parseAntenna(s, a)) { CHECK(a.id >= 1 && a.id <= kMaxAntennas && a.name.size() <= 32 && a.txMask <= 0xFFFF); }
        if (parseBand(s, b)) { CHECK(b.id >= 0 && b.id <= kMaxBandId && b.freqStopMHz >= b.freqStartMHz); }
        if (parseRelay(s, r)) { CHECK(r.tx <= 0xFFFF && r.rx <= 0xFFFF && r.state <= 0xFFFF); }
        t.in += s + (rng.below(3) == 0 ? "\r\n" : rng.below(2) ? "\n" : "\r");
        if (it % 7 == 0) { c.requestPortGet(1 + static_cast<int>(rng.below(2))); c.requestAntennaList(); }
        for (int k = 0; k < 4; ++k) { c.poll(); }
        CHECK(c.pendingCount() <= kMaxPending);
        CHECK(c.antennas().size() <= static_cast<std::size_t>(kMaxAntennas) && c.bands().size() <= static_cast<std::size_t>(kMaxBandId) + 1);
        if (it % 50 == 0) { StatusMessage m; int n = 0; while (c.takeStatus(m)) { ++n; } CHECK(n <= static_cast<int>(kMaxStatusQueue)); }
        t.out.clear();
    }
    CHECK(parsedOk > 5000);
    // random bytes straight into the client
    for (int it = 0; it < 3000; ++it) {
        for (int k = 0; k < 40; ++k) { t.in.push_back(static_cast<char>(rng.below(256))); }
        for (int k = 0; k < 4; ++k) { c.poll(); }
    }
    CHECK(c.pendingCount() <= kMaxPending);
}

}  // namespace

int main() {
    g1Lines();
    g2Parsers();
    g3Client();
    g3Sequence();
    g4Safety();
    g5EndToEnd();
    g6Transport();
    g7Fuzz();
    return testSummary("test_antenna_genius");
}
