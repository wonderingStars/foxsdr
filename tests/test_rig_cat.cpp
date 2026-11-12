// test_rig_cat.cpp - CAT for a hardware transceiver: the Kenwood and Yaesu "newcat" dialects, the
// follower and the emulator, proven against literal strings from the makers' manuals (quoted in
// src/source/rig_cat.hpp), an in-memory byte stream, and a stand-in rig on a real loopback socket.
// No serial port is opened (the refusal tests return before any Windows call). UNPROVEN: a real rig.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/rig_cat.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
namespace rc = cascade::source::rigcat;
using rc::Dialect;

namespace {

std::vector<std::uint8_t> bytes(const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

// An in-memory ByteStream: `incoming` is served by read() in chunks, `written` collects writes.
class ScriptStream : public rc::ByteStream {
public:
    std::string incoming;
    std::size_t chunk = 1000;
    bool closeWhenEmpty = false;
    std::string written;
    bool write(const void* d, std::size_t n) override { written.append(static_cast<const char*>(d), n); return true; }
    bool read(void* d, std::size_t max, std::size_t& got, std::chrono::milliseconds timeout) override {
        got = 0;
        if (incoming.empty()) {
            if (closeWhenEmpty) { err_ = "closed"; return false; }
            std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(timeout, std::chrono::milliseconds(5)));
            return true;
        }
        got = std::min({max, chunk, incoming.size()});
        std::memcpy(d, incoming.data(), got);
        incoming.erase(0, got);
        return true;
    }
    void close() override {}
    const char* lastError() const override { return err_.c_str(); }

private:
    std::string err_;
};

// --- the stand-in rig, with its own literal formats ---------------------------------------

class FakeRig {
public:
    enum class Quirk { None, ErrorOnMode, Silent, Garbage, SplitAnswers, CloseAfterFa, Unsolicited };
    explicit FakeRig(Dialect d) : d_(d) {}
    ~FakeRig() { srv_.stop(); }
    bool start() {
        return srv_.start([this](Sock s, std::size_t, const std::atomic<bool>& run) { serve(s, run); });
    }
    std::uint16_t port() const { return srv_.port(); }
    std::atomic<Quirk> quirk{Quirk::None};
    std::atomic<long long> freq{7074000};
    std::atomic<char> mode{'2'};
    std::atomic<int> commands{0};

private:
    bool put(Sock s, const std::string& m) { return sendExact(s, m.data(), m.size()); }
    void serve(Sock s, const std::atomic<bool>& run) {
        std::string buf;
        if (quirk.load() == Quirk::Garbage) {
            put(s, std::string(600, 'x'));  // longer than any message, no terminator
            put(s, ";");
        }
        while (run.load()) {
            if (!waitRead(s, run)) { return; }
            char c[64];
            const int r = ::recv(s, c, sizeof(c), 0);
            if (r <= 0) { return; }
            buf.append(c, static_cast<std::size_t>(r));
            std::size_t p;
            while ((p = buf.find(';')) != std::string::npos) {
                const std::string cmd = buf.substr(0, p + 1);
                buf.erase(0, p + 1);
                ++commands;
                if (quirk.load() == Quirk::Silent) { continue; }
                char out[64];
                if (cmd == "FA;") {
                    std::snprintf(out, sizeof(out), d_ == Dialect::Kenwood ? "FA%011lld;" : "FA%09lld;", freq.load());
                    if (quirk.load() == Quirk::SplitAnswers) {
                        const std::string a = out;
                        put(s, a.substr(0, 5));
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        put(s, a.substr(5));
                    } else {
                        put(s, out);
                    }
                    if (quirk.load() == Quirk::CloseAfterFa) { return; }
                } else if ((d_ == Dialect::Kenwood && cmd == "MD;") || (d_ == Dialect::Yaesu && cmd == "MD0;")) {
                    if (quirk.load() == Quirk::ErrorOnMode) { put(s, "?;"); continue; }
                    if (quirk.load() == Quirk::Unsolicited) { put(s, d_ == Dialect::Kenwood ? "FA00014074000;" : "FA014074000;"); }
                    put(s, std::string(d_ == Dialect::Kenwood ? "MD" : "MD0") + mode.load() + ";");
                } else if (cmd.compare(0, 2, "FA") == 0 && cmd.size() == (d_ == Dialect::Kenwood ? 14u : 12u)) {
                    freq = std::stoll(cmd.substr(2, cmd.size() - 3));
                } else {
                    put(s, "?;");
                }
            }
        }
    }
    Dialect d_;
    TcpServer srv_;
};

// --- tests ------------------------------------------------------------------------------

void testManualStrings() {
    // Kenwood TS-590S manual: "FA00007000000;" sets VFO A to 7 MHz, "FA;" reads it.
    CHECK(rc::buildSetFrequency(Dialect::Kenwood, 7000000.0) == "FA00007000000;");
    CHECK(rc::buildReadFrequency(Dialect::Kenwood) == "FA;");
    CHECK(rc::buildSetFrequency(Dialect::Kenwood, 14250000.0) == "FA00014250000;");
    CHECK(rc::buildSetFrequency(Dialect::Kenwood, 145500000.4) == "FA00145500000;");  // rounded to Hz
    // Yaesu FT-991A manual: "FA014250000;" sets VFO-A to 14.250000 MHz.
    CHECK(rc::buildSetFrequency(Dialect::Yaesu, 14250000.0) == "FA014250000;");
    CHECK(rc::buildSetFrequency(Dialect::Yaesu, 30000.0) == "FA000030000;");
    CHECK(rc::buildSetFrequency(Dialect::Yaesu, 470000000.0) == "FA470000000;");
    CHECK(rc::buildReadMode(Dialect::Yaesu) == "MD0;" && rc::buildReadMode(Dialect::Kenwood) == "MD;");
    // frequencies the digits cannot hold, or that are not frequencies
    CHECK(rc::buildSetFrequency(Dialect::Yaesu, 1.0e9).empty());
    CHECK(rc::buildSetFrequency(Dialect::Kenwood, 1.0e11).empty());
    CHECK(rc::buildSetFrequency(Dialect::Kenwood, -1.0).empty());
    CHECK(rc::buildSetFrequency(Dialect::Kenwood, std::nan("")).empty());
    CHECK(rc::buildSetFrequency(Dialect::Kenwood, INFINITY).empty());
    // mode tables, every code of each manual
    struct K { char c; rc::RigMode m; };
    const K kenwood[] = {{'1', rc::RigMode::Lsb}, {'2', rc::RigMode::Usb}, {'3', rc::RigMode::Cw}, {'4', rc::RigMode::Fm},
                         {'5', rc::RigMode::Am}, {'6', rc::RigMode::Fsk}, {'7', rc::RigMode::CwReverse}, {'9', rc::RigMode::FskReverse}};
    for (const K& k : kenwood) {
        CHECK(rc::modeFromCode(Dialect::Kenwood, k.c) == k.m);
        CHECK(rc::modeToCode(Dialect::Kenwood, k.m) == k.c);
    }
    CHECK(rc::modeFromCode(Dialect::Kenwood, '0') == rc::RigMode::Unknown);  // "None (setting failure)"
    CHECK(rc::modeFromCode(Dialect::Kenwood, '8') == rc::RigMode::Unknown);
    CHECK(rc::modeFromCode(Dialect::Kenwood, 'A') == rc::RigMode::Unknown);
    const K yaesu[] = {{'1', rc::RigMode::Lsb}, {'2', rc::RigMode::Usb}, {'3', rc::RigMode::Cw}, {'4', rc::RigMode::Fm},
                       {'5', rc::RigMode::Am}, {'6', rc::RigMode::RttyLsb}, {'7', rc::RigMode::CwReverse}, {'8', rc::RigMode::DataLsb},
                       {'9', rc::RigMode::RttyUsb}, {'A', rc::RigMode::DataFm}, {'B', rc::RigMode::FmNarrow}, {'C', rc::RigMode::DataUsb},
                       {'D', rc::RigMode::AmNarrow}, {'E', rc::RigMode::C4fm}};
    for (const K& k : yaesu) {
        CHECK(rc::modeFromCode(Dialect::Yaesu, k.c) == k.m);
        CHECK(rc::modeToCode(Dialect::Yaesu, k.m) == k.c);
    }
    CHECK(rc::modeFromCode(Dialect::Yaesu, '0') == rc::RigMode::Unknown && rc::modeFromCode(Dialect::Yaesu, 'F') == rc::RigMode::Unknown);
    CHECK(rc::buildSetMode(Dialect::Kenwood, rc::RigMode::Usb) == "MD2;");
    CHECK(rc::buildSetMode(Dialect::Yaesu, rc::RigMode::Usb) == "MD02;");
    CHECK(rc::buildSetMode(Dialect::Yaesu, rc::RigMode::DataUsb) == "MD0C;");
    CHECK(rc::buildSetMode(Dialect::Kenwood, rc::RigMode::DataUsb).empty());  // Kenwood's table has none
    // FoxSDR mode names
    CHECK(rc::foxModeForRigMode(rc::RigMode::CwReverse) == "CW" && rc::foxModeForRigMode(rc::RigMode::RttyLsb) == "LSB");
    CHECK(rc::foxModeForRigMode(rc::RigMode::FmNarrow) == "NFM" && rc::foxModeForRigMode(rc::RigMode::C4fm).empty());
    CHECK(rc::rigModeForFoxMode("WFM") == rc::RigMode::Fm && rc::rigModeForFoxMode("DSB") == rc::RigMode::Unknown);
    CHECK(rc::rigModeForFoxMode("RAW") == rc::RigMode::Unknown && rc::rigModeForFoxMode("USB") == rc::RigMode::Usb);
    Dialect d = Dialect::Kenwood;
    CHECK(rc::parseDialect("Yaesu", d) && d == Dialect::Yaesu && rc::parseDialect("KENWOOD", d) && d == Dialect::Kenwood);
    CHECK(!rc::parseDialect("icom", d));  // not built
    CHECK(std::string(rc::dialectName(Dialect::Yaesu)) == "Yaesu");
}

void testParseAnswer() {
    using K = rc::AnswerKind;
    auto a = rc::parseAnswer(Dialect::Kenwood, "FA00007000000;");
    CHECK(a.kind == K::Frequency && a.frequencyHz == 7000000.0);
    a = rc::parseAnswer(Dialect::Yaesu, "FA014250000;");
    CHECK(a.kind == K::Frequency && a.frequencyHz == 14250000.0);
    CHECK(rc::parseAnswer(Dialect::Yaesu, "fa014250000;").kind == K::Frequency);  // "either lower or upper case"
    // digit counts are exact, digits only
    CHECK(rc::parseAnswer(Dialect::Yaesu, "FA00007000000;").kind == K::Malformed);   // Kenwood's width to a Yaesu parser
    CHECK(rc::parseAnswer(Dialect::Kenwood, "FA014250000;").kind == K::Malformed);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "FA0000700000x;").kind == K::Malformed);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "FA;").kind == K::Malformed);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "FA-0007000000;").kind == K::Malformed);
    // modes
    a = rc::parseAnswer(Dialect::Kenwood, "MD2;");
    CHECK(a.kind == K::Mode && a.mode == rc::RigMode::Usb);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "MD0;").mode == rc::RigMode::Unknown);
    a = rc::parseAnswer(Dialect::Yaesu, "MD0C;");
    CHECK(a.kind == K::Mode && a.mode == rc::RigMode::DataUsb);
    CHECK(rc::parseAnswer(Dialect::Yaesu, "MD2;").kind == K::Malformed);   // Kenwood's form to a Yaesu parser
    CHECK(rc::parseAnswer(Dialect::Yaesu, "MD12;").kind == K::Other);      // another receiver's mode
    CHECK(rc::parseAnswer(Dialect::Kenwood, "MD22;").kind == K::Malformed);
    // the manual's error messages
    CHECK(rc::parseAnswer(Dialect::Kenwood, "?;").kind == K::ErrorSyntax);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "E;").kind == K::ErrorComm);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "O;").kind == K::ErrorBusy);
    // well-formed but not ours; and rubbish
    CHECK(rc::parseAnswer(Dialect::Kenwood, "FB00007000000;").kind == K::Other);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "ID021;").kind == K::Other);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "").kind == K::Malformed);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "FA00007000000").kind == K::Malformed);  // no terminator
    CHECK(rc::parseAnswer(Dialect::Kenwood, ";").kind == K::Malformed);
    CHECK(rc::parseAnswer(Dialect::Kenwood, "12;").kind == K::Malformed);
}

void testSplitter() {
    rc::MessageSplitter s;
    std::string m;
    const std::string stream = "FA00007000000;MD2;?;";
    for (const char c : stream) { s.feed(reinterpret_cast<const std::uint8_t*>(&c), 1); }
    CHECK(s.next(m) && m == "FA00007000000;");
    CHECK(s.next(m) && m == "MD2;");
    CHECK(s.next(m) && m == "?;");
    CHECK(!s.next(m));
    // line feeds between messages are not part of one
    const auto lf = bytes("\r\nMD2;\r\nMD3;");
    s.feed(lf.data(), lf.size());
    CHECK(s.next(m) && m == "MD2;" && s.next(m) && m == "MD3;");
    // an overlong run without a terminator is dropped and the parser resynchronises
    const std::string longRun(1000, 'x');
    s.feed(reinterpret_cast<const std::uint8_t*>(longRun.data()), longRun.size());
    const auto tail = bytes(";MD4;");
    s.feed(tail.data(), tail.size());
    CHECK(s.next(m) && m == "MD4;" && !s.next(m));
    CHECK(s.dropped() == 1);
}

void testFollowerScripted() {
    for (const Dialect d : {Dialect::Kenwood, Dialect::Yaesu}) {
        ScriptStream st;
        rc::RigFollower f(d, st);
        st.incoming = d == Dialect::Kenwood ? "FA00007074000;MD2;" : "FA007074000;MD02;";
        st.chunk = 3;  // answers arrive in pieces
        CHECK(f.poll(std::chrono::milliseconds(500)));
        CHECK(st.written == (d == Dialect::Kenwood ? "FA;MD;" : "FA;MD0;"));
        CHECK(f.state().frequencyKnown && f.state().frequencyHz == 7074000.0);
        CHECK(f.state().modeKnown && f.state().mode == rc::RigMode::Usb);
        // unsolicited answers (auto information) update the state without a poll
        const std::string unsolicited = d == Dialect::Kenwood ? "FA00014074000;MD1;" : "FA014074000;MD01;";
        f.consume(reinterpret_cast<const std::uint8_t*>(unsolicited.data()), unsolicited.size());
        CHECK(f.state().frequencyHz == 14074000.0 && f.state().mode == rc::RigMode::Lsb);
    }
    {  // nothing answers: bounded
        ScriptStream st;
        rc::RigFollower f(Dialect::Kenwood, st);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!f.poll(std::chrono::milliseconds(150)));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500));
        CHECK(f.lastError().find("in time") != std::string::npos && !f.state().frequencyKnown);
    }
    {  // a rig error message ends the wait at once
        ScriptStream st;
        st.incoming = "FA00007074000;?;";
        rc::RigFollower f(Dialect::Kenwood, st);
        CHECK(!f.poll(std::chrono::seconds(5)));
        CHECK(f.errorsFromRig() == 1 && f.state().frequencyKnown && !f.state().modeKnown);
    }
    {  // the stream closes
        ScriptStream st;
        st.closeWhenEmpty = true;
        rc::RigFollower f(Dialect::Yaesu, st);
        CHECK(!f.poll(std::chrono::seconds(5)));
        CHECK(f.lastError().find("failed") != std::string::npos);
    }
    {  // only one of the two answers: false, the other stays unknown
        ScriptStream st;
        st.incoming = "MD02;";
        rc::RigFollower f(Dialect::Yaesu, st);
        CHECK(!f.poll(std::chrono::milliseconds(100)));
        CHECK(f.state().modeKnown && !f.state().frequencyKnown);
    }
    {  // malformed and foreign answers are counted, the good ones still land
        ScriptStream st;
        st.incoming = "FA1234;ID021;FA00007074000;zz;MD2;";
        rc::RigFollower f(Dialect::Kenwood, st);
        CHECK(f.poll(std::chrono::milliseconds(500)));
        CHECK(f.malformed() == 1 && f.state().frequencyHz == 7074000.0);
    }
}

void testFollowerOverTcp() {
    for (const Dialect d : {Dialect::Kenwood, Dialect::Yaesu}) {
        FakeRig rig(d);
        CHECK(rig.start());
        std::string err;
        std::unique_ptr<rc::ByteStream> s = rc::connectTcpByteStream("127.0.0.1", rig.port(), err);
        CHECK(s != nullptr);
        if (!s) { continue; }
        rc::RigFollower f(d, *s);
        CHECK(f.poll(std::chrono::milliseconds(2000)));
        CHECK(f.state().frequencyHz == 7074000.0 && f.state().mode == rc::RigMode::Usb);
        rig.freq = 14230000;
        rig.mode = '1';
        CHECK(f.poll(std::chrono::milliseconds(2000)));
        CHECK(f.state().frequencyHz == 14230000.0 && f.state().mode == rc::RigMode::Lsb);
        // a set command in the dialect's own format moves the stand-in
        const std::string set = rc::buildSetFrequency(d, 3573000.0);
        CHECK(s->write(set.data(), set.size()));
        for (int i = 0; i < 100 && rig.freq.load() != 3573000; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(rig.freq.load() == 3573000);
        // answers split across two sends
        rig.quirk = FakeRig::Quirk::SplitAnswers;
        CHECK(f.poll(std::chrono::milliseconds(2000)) && f.state().frequencyHz == 3573000.0);
        // the rig rejects the mode read
        rig.quirk = FakeRig::Quirk::ErrorOnMode;
        CHECK(!f.poll(std::chrono::milliseconds(2000)));
        CHECK(f.errorsFromRig() >= 1);
        // unsolicited extra frequency before the mode answer
        rig.quirk = FakeRig::Quirk::Unsolicited;
        CHECK(f.poll(std::chrono::milliseconds(2000)) && f.state().frequencyHz == 14074000.0);
        // silent rig
        rig.quirk = FakeRig::Quirk::Silent;
        CHECK(!f.poll(std::chrono::milliseconds(300)));
        s->close();
    }
    {  // garbage first: an overlong run is dropped and the follower still works
        FakeRig rig(Dialect::Kenwood);
        rig.quirk = FakeRig::Quirk::Garbage;
        CHECK(rig.start());
        std::string err;
        auto s = rc::connectTcpByteStream("127.0.0.1", rig.port(), err);
        CHECK(s != nullptr);
        rc::RigFollower f(Dialect::Kenwood, *s);
        CHECK(f.poll(std::chrono::milliseconds(2000)));
        CHECK(f.droppedBytes() >= 1);
    }
    {  // the rig hangs up after the first answer
        FakeRig rig(Dialect::Yaesu);
        rig.quirk = FakeRig::Quirk::CloseAfterFa;
        CHECK(rig.start());
        std::string err;
        auto s = rc::connectTcpByteStream("127.0.0.1", rig.port(), err);
        CHECK(s != nullptr);
        rc::RigFollower f(Dialect::Yaesu, *s);
        CHECK(!f.poll(std::chrono::milliseconds(2000)));
        CHECK(f.state().frequencyKnown && f.lastError().find("failed") != std::string::npos);
    }
}

void testConnectionRefusalsAndCycles() {
    std::string err;
    CHECK(rc::connectTcpByteStream("127.0.0.1", deadPort(), err) == nullptr && !err.empty());
    FakeRig rig(Dialect::Kenwood);
    CHECK(rig.start());
    for (int i = 0; i < 50; ++i) {
        auto s = rc::connectTcpByteStream("127.0.0.1", rig.port(), err);
        CHECK(s != nullptr);
        if (!s) { continue; }
        rc::RigFollower f(Dialect::Kenwood, *s);
        if (i % 10 == 0) { CHECK(f.poll(std::chrono::milliseconds(2000))); }
        s->close();
    }
    // serial: only the refusals that return before any Windows call
    rc::SerialSettings bad;
    CHECK(rc::openSerialByteStream("COM3", bad, err) == nullptr && err.find("baud") != std::string::npos);
    rc::SerialSettings ok;
    ok.baud = 9600;
    ok.stopBits = 3;
    CHECK(rc::openSerialByteStream("COM3", ok, err) == nullptr && err.find("stop bits") != std::string::npos);
    ok.stopBits = 1;
    CHECK(rc::openSerialByteStream("COM3 ; evil", ok, err) == nullptr);
}

struct HostState {
    double hz = 7074000.0;
    std::string mode = "USB";
    bool refuse = false;
    int setCalls = 0;
};

rc::EmulatorHost hostFor(HostState& h) {
    rc::EmulatorHost e;
    e.frequencyHz = [&h] { return h.hz; };
    e.foxMode = [&h] { return h.mode; };
    e.setFrequencyHz = [&h](double hz) { ++h.setCalls; if (h.refuse) { return false; } h.hz = hz; return true; };
    e.setFoxMode = [&h](const std::string& m) { ++h.setCalls; if (h.refuse) { return false; } h.mode = m; return true; };
    return e;
}

std::string feed(rc::RigEmulator& e, const std::string& s) {
    return e.feed(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

void testEmulator() {
    {
        HostState h;
        rc::RigEmulator e(Dialect::Kenwood, hostFor(h));
        CHECK(feed(e, "FA;") == "FA00007074000;");
        CHECK(feed(e, "MD;") == "MD2;");
        CHECK(feed(e, "FA00014250000;").empty() && h.hz == 14250000.0);  // a set has no answer
        CHECK(feed(e, "MD1;").empty() && h.mode == "LSB");
        CHECK(feed(e, "FA;MD;") == "FA00014250000;MD1;");              // several in one write
        CHECK(feed(e, "FA0001425").empty());                           // half a command: nothing yet
        CHECK(feed(e, "0000;").empty() && h.hz == 14250000.0);
        CHECK(feed(e, "FA00014250000").empty());
        // refusals: "?;" the manual's syntax error
        CHECK(feed(e, "FA123;") == "?;");                  // wrong digit count
        CHECK(feed(e, "FA0001425000x;") == "?;");
        CHECK(feed(e, "MD0;") != "");                      // read-with-parameter is not a read; refused or set-refused below
        CHECK(feed(e, "MD8;") == "?;");                    // "None (setting failure)"
        CHECK(feed(e, "MD22;") == "?;");
        CHECK(feed(e, "XX;") == "?;");                     // unknown command
        CHECK(feed(e, ";") == "?;");
        CHECK(feed(e, "FB00007000000;") == "?;");          // VFO B is not implemented
        h.mode = "RAW";
        CHECK(feed(e, "MD;") == "?;");                     // no rig mode for it
        h.mode = "WFM";
        CHECK(feed(e, "MD;") == "MD4;");                   // the rig's FM
        h.hz = 1.0e12;
        CHECK(feed(e, "FA;") == "?;");                     // not truncated
        h.refuse = true;
        CHECK(feed(e, "FA00007000000;") == "?;" && feed(e, "MD2;") == "?;");
        CHECK(e.commandsRefused() >= 8 && e.commandsHandled() > e.commandsRefused());
    }
    {
        HostState h;
        rc::RigEmulator e(Dialect::Yaesu, hostFor(h));
        CHECK(feed(e, "FA;") == "FA007074000;");
        CHECK(feed(e, "MD0;") == "MD02;");
        CHECK(feed(e, "fa014250000;").empty() && h.hz == 14250000.0);  // lower case
        CHECK(feed(e, "MD0C;").empty() && h.mode == "USB");            // DATA-USB demodulates as USB
        CHECK(feed(e, "MD0A;").empty() && h.mode == "NFM");            // DATA-FM
        CHECK(feed(e, "MD0E;") == "?;");                               // C4FM: no FoxSDR mode
        CHECK(feed(e, "MD12;") == "?;");                               // the other receiver
        CHECK(feed(e, "MD2;") == "?;");                                // Kenwood's form
        CHECK(feed(e, "FA00014250000;") == "?;");                      // Kenwood's width
        CHECK(feed(e, "IF;") == "?;");
    }
    {  // a host with no callbacks refuses, never crashes
        rc::RigEmulator e(Dialect::Kenwood, rc::EmulatorHost{});
        CHECK(feed(e, "FA;MD;FA00007000000;MD2;") == "?;?;?;?;");
    }
    {  // follower and emulator agree: the follower reads what the emulator says
        HostState h;
        h.hz = 21074000.0;
        h.mode = "CW";
        for (const Dialect d : {Dialect::Kenwood, Dialect::Yaesu}) {
            rc::RigEmulator e(d, hostFor(h));
            ScriptStream st;
            rc::RigFollower f(d, st);
            // the emulator answers what the follower wrote
            st.incoming = e.feed(reinterpret_cast<const std::uint8_t*>(rc::buildReadFrequency(d).data()), 3) +
                          e.feed(reinterpret_cast<const std::uint8_t*>(rc::buildReadMode(d).data()), rc::buildReadMode(d).size());
            CHECK(f.poll(std::chrono::milliseconds(300)));
            CHECK(f.state().frequencyHz == 21074000.0 && f.state().mode == rc::RigMode::Cw);
        }
    }
}

void testFuzz() {
    Rng rng(0x5EED5u);
    HostState h;
    rc::RigEmulator ek(Dialect::Kenwood, hostFor(h)), ey(Dialect::Yaesu, hostFor(h));
    int answers = 0, refusals = 0;
    for (int i = 0; i < 10000; ++i) {
        std::string in;
        const int len = static_cast<int>(rng.next() % 60);
        const int style = static_cast<int>(rng.next() % 4);
        if (style == 1) { in = "FA"; }
        if (style == 2) { in = "MD"; }
        for (int k = 0; k < len; ++k) {
            const std::uint32_t r = rng.next();
            in += (r % 3 == 0) ? static_cast<char>('0' + r % 10) : (r % 11 == 0 ? ';' : static_cast<char>(r >> 8));
        }
        in += ';';
        for (const Dialect d : {Dialect::Kenwood, Dialect::Yaesu}) {
            const rc::Answer a = rc::parseAnswer(d, in);
            if (a.kind == rc::AnswerKind::Frequency) { CHECK(a.frequencyHz >= 0.0); }
            const std::string out = (d == Dialect::Kenwood ? ek : ey).feed(reinterpret_cast<const std::uint8_t*>(in.data()), in.size());
            answers += !out.empty();
            refusals += out.find("?;") != std::string::npos;
        }
        rc::MessageSplitter sp;
        sp.feed(reinterpret_cast<const std::uint8_t*>(in.data()), in.size());
        std::string m;
        while (sp.next(m)) { CHECK(!m.empty() && m.size() <= rc::MessageSplitter::kMaxMessage && m.back() == ';'); }
    }
    CHECK(answers > 1000 && refusals > 1000);
}

}  // namespace

#define STEP(call) do { std::fputs(#call, stderr); std::fputc(10, stderr); std::fflush(stderr); call; } while (0)

int main() {
    STEP(testManualStrings());
    STEP(testParseAnswer());
    STEP(testSplitter());
    STEP(testFollowerScripted());
    STEP(testFollowerOverTcp());
    STEP(testConnectionRefusalsAndCycles());
    STEP(testEmulator());
    STEP(testFuzz());
    return testSummary("test_rig_cat");
}
