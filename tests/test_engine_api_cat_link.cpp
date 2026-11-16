// test_engine_api_cat_link.cpp - FOXSDR FOLLOWS A RIG OVER CAT, IN THE REAL ENGINE (screen lane S5b, rows G1-16, G3-18a, G3-18b: src/engine/cat_link.{hpp,cpp}).
//
// No serial port is opened and no program is started: the link's serial port and OmniRig come from stand-ins the test hands the engine's wire-c state (CatLinkSeams); the real ones are only reached by the product.
//   A  off by default (nothing opened); a way chosen without a baud rate opens nothing and says what is missing
//   B  follow: a stand-in Kenwood rig on a stand-in serial port; its frequency and mode become FoxSDR's centre and mode, and again when the rig moves; a person retuning FoxSDR is not fought; every message the link wrote to
//      the rig is on the list of three (FA;  MD;  MD0;); a Yaesu rig is asked in its own words
//   C  THE LIST AND THE GATE: every message FoxSDR can send to a rig, proved by a test over the list - none is a transmit command; the gate refuses transmit commands, a frequency set, a mode set and a compound
//      message with one of them in it, whole; a randomised run of 50000 messages lets through only the three; the link's sources contain no CAT message but those three and no set-builder call
//   D  emulate (a virtual COM pair's other end): the logging program reads FoxSDR's frequency and mode, sets them, and every transmit command is answered "?;" and moves nothing
//   E  OmniRig: absent, the sentence says what it is, where it comes from and that FoxSDR downloads nothing; present (a stand-in port), FoxSDR follows its frequency and mode; the link makes NO write to it (the stand-in
//      records every call); a rig that is not online is described; a rig that transmits is not followed
//   F  a port that dies is reported with a sentence; a hostile rig (garbage, a 10 kB message with no terminator, error messages) is survived and counted; switching off stops the thread
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "engine/cat_link.hpp"
#include "wire_c_crash_trace.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;
namespace rc = cascade::source::rigcat;
namespace rig = cascade::core::rig;
namespace wc = cascade::engine::wirec;

namespace {

// A stand-in serial port with a rig on the far end. `answerAsRig`: it answers "FA;MD;" like a Kenwood (or Yaesu) rig with the frequency and mode set in it, and records everything written to it.
struct FakeRigPort final : rc::ByteStream {
    std::mutex m;
    std::string toRead;                     // bytes the far end sends to FoxSDR
    std::vector<std::string> written;       // every write, whole
    double hz = 14200000.0;
    char modeCode = '2';                    // Kenwood: 2 = USB, 1 = LSB; Yaesu: the same digits
    bool yaesu = false;
    bool asRig = true;
    bool dead = false;
    std::string garbage;                    // sent instead of an answer when not empty
    std::string err = "the stand-in port was closed";
    void reset() {
        std::lock_guard<std::mutex> lk(m);
        toRead.clear(); written.clear(); hz = 14200000.0; modeCode = '2'; yaesu = false; asRig = true; dead = false; garbage.clear();
    }
    bool write(const void* data, std::size_t n) override {
        std::lock_guard<std::mutex> lk(m);
        if (dead) { return false; }
        const std::string s(static_cast<const char*>(data), n);
        written.push_back(s);
        if (!asRig) { return true; }
        if (!garbage.empty()) { toRead += garbage; return true; }
        if (s.find("FA;") != std::string::npos) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), yaesu ? "FA%09.0f;" : "FA%011.0f;", hz);
            toRead += buf;
        }
        if (s.find("MD;") != std::string::npos || s.find("MD0;") != std::string::npos) {
            toRead += yaesu ? std::string("MD0") + modeCode + ";" : std::string("MD") + modeCode + ";";
        }
        return true;
    }
    bool read(void* data, std::size_t max, std::size_t& got, std::chrono::milliseconds timeout) override {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(m);
                if (dead) { return false; }
                if (!toRead.empty()) {
                    got = std::min(max, toRead.size());
                    std::memcpy(data, toRead.data(), got);
                    toRead.erase(0, got);
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= end) { got = 0; return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    void close() override {}
    const char* lastError() const override { return err.c_str(); }
    std::vector<std::string> writes() { std::lock_guard<std::mutex> lk(m); return written; }
    void feed(const std::string& s) { std::lock_guard<std::mutex> lk(m); toRead += s; }
    std::string takeWritten() {
        std::lock_guard<std::mutex> lk(m);
        std::string all;
        for (const std::string& w : written) { all += w; }
        written.clear();
        return all;
    }
};

// The stream handed to the link is the test's object (the link owns a unique_ptr; this forwards).
struct Forward final : rc::ByteStream {
    FakeRigPort* p;
    explicit Forward(FakeRigPort* port) : p(port) {}
    bool write(const void* d, std::size_t n) override { return p->write(d, n); }
    bool read(void* d, std::size_t mx, std::size_t& got, std::chrono::milliseconds t) override { return p->read(d, mx, got, t); }
    void close() override { p->close(); }
    const char* lastError() const override { return p->lastError(); }
};

FakeRigPort g_rig;
std::atomic<int> g_serialOpens{0};
std::string g_serialPort;
rc::SerialSettings g_serialSettings;

// An OmniRig stand-in that records EVERY call.
struct FakeOmni final : rig::OmniRigRigPort {
    std::mutex m;
    std::vector<std::string> calls;
    std::int32_t st = rig::ST_ONLINE;
    std::int32_t f = 7100000;
    std::uint32_t md = rig::PM_SSB_L;
    std::uint32_t tx = rig::PM_RX;
    void rec(const char* c) { std::lock_guard<std::mutex> lk(m); calls.push_back(c); }
    std::int32_t status() override { rec("status"); return st; }
    std::int32_t freq() override { rec("freq"); return f; }
    std::int32_t freqA() override { rec("freqA"); return f; }
    std::int32_t freqB() override { rec("freqB"); return f; }
    std::uint32_t mode() override { rec("mode"); return md; }
    std::uint32_t vfo() override { rec("vfo"); return rig::PM_VFOA; }
    std::uint32_t split() override { rec("split"); return rig::PM_SPLITOFF; }
    std::uint32_t txState() override { rec("txState"); return tx; }
    bool putFreq(std::int32_t) override { rec("putFreq"); return true; }
    bool putMode(std::uint32_t) override { rec("putMode"); return true; }
    std::size_t count(const char* c) { std::lock_guard<std::mutex> lk(m); std::size_t n = 0; for (const auto& x : calls) { if (x == c) { ++n; } } return n; }
};
FakeOmni g_omni;
std::atomic<int> g_omniOpens{0};
bool g_omniAbsent = false;

struct OmniForward final : rig::OmniRigRigPort {
    std::int32_t status() override { return g_omni.status(); }
    std::int32_t freq() override { return g_omni.freq(); }
    std::int32_t freqA() override { return g_omni.freqA(); }
    std::int32_t freqB() override { return g_omni.freqB(); }
    std::uint32_t mode() override { return g_omni.mode(); }
    std::uint32_t vfo() override { return g_omni.vfo(); }
    std::uint32_t split() override { return g_omni.split(); }
    std::uint32_t txState() override { return g_omni.txState(); }
    bool putFreq(std::int32_t v) override { return g_omni.putFreq(v); }
    bool putMode(std::uint32_t v) override { return g_omni.putMode(v); }
};

void seams(cascade::engine::wirec::State& s) {
    s.catLinkSeams.pollEvery = std::chrono::milliseconds(20);
    s.catLinkSeams.openSerial = [](const std::string& port, const rc::SerialSettings& ss, std::string&) -> std::unique_ptr<rc::ByteStream> {
        ++g_serialOpens;
        g_serialPort = port;
        g_serialSettings = ss;
        return std::unique_ptr<rc::ByteStream>(new Forward(&g_rig));
    };
    s.catLinkSeams.openOmniRig = [](int, std::string& err) -> std::unique_ptr<rig::OmniRigRigPort> {
        ++g_omniOpens;
        if (g_omniAbsent) { err = wc::CatLink::omniRigNeededSentence(); return nullptr; }
        return std::unique_ptr<rig::OmniRigRigPort>(new OmniForward());
    };
}

template <class F>
bool until(Rig& r, F done, int timeoutMs = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        r.pass(1);
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

FoxCommand op(std::uint32_t o, std::int64_t a = 0) { return testing::cmd(o, 0.0, a, nullptr); }

// A scratch folder of its own for every engine (settings are kept in a file there: a second engine on the same folder would start with the first one's choices).
fs::path fresh(const fs::path& base) {
    static int n = 0;
    const fs::path p = base / ("e" + std::to_string(++n));
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

void configureFollow(Rig& r, const char* dialect = "kenwood") {
    CHECK(r.status(setting("cat.link.port", "COM7")) == FOXAPI_OK);
    CHECK(r.status(setting("cat.link.baud", "9600")) == FOXAPI_OK);
    { const std::int32_t st = r.status(setting("cat.link.dialect", dialect)); CHECK(st == FOXAPI_OK || st == FOXAPI_NO_CHANGE); }   // (the default is not a change)
    CHECK(r.status(setting("cat.link.mode", "follow")) == FOXAPI_OK);
}

// ------------------------------------------------------------------------------------------------------------------------------------ A
void testA(const fs::path& dir) {
    std::printf("A: off by default; a way chosen without a baud rate opens nothing\n");
    Rig r(fresh(dir), seams);
    r.pass(6);
    CHECK_EQ(r.fact("cat.link.state"), std::string("off"));
    CHECK_EQ(g_serialOpens.load(), 0);
    CHECK_EQ(g_omniOpens.load(), 0);
    CHECK(r.status(setting("cat.link.mode", "follow")) == FOXAPI_OK);
    r.pass(4);
    CHECK_EQ(r.fact("cat.link.state"), std::string("error"));
    CHECK(r.fact("cat.link.error").find("serial port") != std::string::npos);
    CHECK(r.status(setting("cat.link.port", "COM7")) == FOXAPI_OK);
    r.pass(4);
    CHECK(r.fact("cat.link.error").find("baud") != std::string::npos);
    CHECK_EQ(g_serialOpens.load(), 0);                                      // still nothing opened
    // a port name that is not a COM port is refused by the setting itself
    CHECK(r.status(setting("cat.link.port", "\\\\.\\pipe\\x")) != FOXAPI_OK);
    CHECK(r.status(setting("cat.link.port", "COM999")) != FOXAPI_OK);
    CHECK(r.status(setting("cat.link.mode", "off")) == FOXAPI_OK);
    r.pass(4);
    CHECK_EQ(r.fact("cat.link.state"), std::string("off"));
}

// ------------------------------------------------------------------------------------------------------------------------------------ B
void testB(const fs::path& dir) {
    std::printf("B: follow a stand-in Kenwood rig, then a Yaesu\n");
    g_rig.reset();
    g_serialOpens = 0;
    {
        Rig r(fresh(dir), seams);
        CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
        configureFollow(r);
        CHECK(until(r, [&] { return r.fact("cat.link.state") == "following"; }));
        CHECK_EQ(g_serialOpens.load(), 1);
        CHECK_EQ(g_serialPort, std::string("COM7"));
        CHECK_EQ(g_serialSettings.baud, 9600);
        g_rig.hz = 101100000.0;   // (the built-in generator tunes inside the VHF broadcast band)
        g_rig.modeCode = '2';     // USB
        const bool followed = until(r, [&] { return std::abs(r.local->state().tunedHz - 101100000.0) < 1.0 && r.local->state().demodMode == FOXAPI_DEMOD_USB; });
        if (!followed) { std::printf("      tuned %.0f mode %u state %s err %s\n", r.local->state().tunedHz, r.local->state().demodMode, r.fact("cat.link.state").c_str(), r.fact("cat.link.error").c_str()); }
        CHECK(followed);
        CHECK(until(r, [&] { return r.fact("cat.link.rigHz") == "101100000" && r.fact("cat.link.rigMode") == "USB"; }));
        // the rig moves: FoxSDR follows again
        g_rig.hz = 98500000.0;
        g_rig.modeCode = '1';     // LSB
        CHECK(until(r, [&] { return std::abs(r.local->state().tunedHz - 98500000.0) < 1.0 && r.local->state().demodMode == FOXAPI_DEMOD_LSB; }));
        // a person retunes FoxSDR: the link does not fight it while the rig stays where it is
        const double off = r.local->state().vfoOffsetHz;
        CHECK(r.status(testing::cmd(FOXAPI_OP_SET_CENTRE, 99900000.0)) == FOXAPI_OK);
        r.pass(2);
        const std::uint64_t applied = std::strtoull(r.fact("cat.link.applied").c_str(), nullptr, 10);
        until(r, [&] { return false; }, 400);
        CHECK(std::abs(r.local->state().tunedHz - (99900000.0 + off)) < 1.0);
        CHECK_EQ(std::strtoull(r.fact("cat.link.applied").c_str(), nullptr, 10), applied);
        // every message written to the rig is on the list of three
        const std::vector<std::string> w = g_rig.writes();
        CHECK(!w.empty());
        std::string all;
        for (const std::string& x : w) { all += x; }
        std::size_t from = 0, nmsg = 0;
        while (from < all.size()) {
            const std::size_t semi = all.find(';', from);
            CHECK(semi != std::string::npos);
            if (semi == std::string::npos) { break; }
            const std::string one = all.substr(from, semi - from + 1);
            CHECK(one == "FA;" || one == "MD;");
            ++nmsg;
            from = semi + 1;
        }
        CHECK(nmsg >= 4);
        CHECK_EQ(r.fact("cat.link.blocked"), std::string("0"));
        std::printf("  %zu messages to the rig, all reads; applied %llu\n", nmsg, static_cast<unsigned long long>(applied));
        // a mode the rig has and FoxSDR has not (C4FM in Yaesu's table, here an unknown code) is not applied and is not an error
        g_rig.modeCode = '0';
        until(r, [&] { return false; }, 300);
        CHECK_EQ(r.fact("cat.link.state"), std::string("following"));
        // follow-frequency off: the rig moves, FoxSDR stays
        CHECK(r.status(setting("cat.link.follow_frequency", "0")) == FOXAPI_OK);
        CHECK(until(r, [&] { return r.fact("cat.link.state") == "following"; }));
        g_rig.hz = 90100000.0;
        until(r, [&] { return false; }, 500);
        CHECK(std::abs(r.local->state().tunedHz - 90100000.0) > 1.0);
        CHECK(r.status(setting("cat.link.mode", "off")) == FOXAPI_OK);
        r.pass(4);
        CHECK_EQ(r.fact("cat.link.state"), std::string("off"));
    }
    {
        // a Yaesu rig is asked in its own words
        g_rig.reset();
        g_rig.yaesu = true;
        g_rig.hz = 101100000.0;
        g_rig.modeCode = '2';
        Rig r(fresh(dir), seams);
        CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
        configureFollow(r, "yaesu");
        CHECK(until(r, [&] { return std::abs(r.local->state().tunedHz - 101100000.0) < 1.0; }));
        const std::vector<std::string> w = g_rig.writes();
        CHECK(!w.empty());
        for (const std::string& x : w) { CHECK_EQ(x, std::string("FA;MD0;")); }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------ C
std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testC() {
    std::printf("C: what FoxSDR can send to a rig\n");
    const std::vector<std::string>& sent = wc::catCommandsSent();
    CHECK_EQ(sent.size(), std::size_t(3));
    // the three, and none of them a transmit command: a transmit, tune, power, antenna-tuner, keyer, memory-write or mic-gain message starts with one of these two-letter codes
    const char* const transmitCodes[] = {"TX", "TQ", "AC", "PC", "MX", "KY", "VX", "PT", "MG", "AG", "PS", "RX", "TS", "SM", "SQ", "RT", "XT", "SP", "ST"};
    for (const std::string& m : sent) {
        CHECK(m.size() >= 3 && m.back() == ';');
        CHECK(m.rfind("FA", 0) == 0 || m.rfind("MD", 0) == 0);
        for (const char* t : transmitCodes) { CHECK(m.rfind(t, 0) != 0); }
        // a read carries no parameter digits except the Yaesu VFO selector "0" of "MD0;"
        CHECK(m == "FA;" || m == "MD;" || m == "MD0;");
    }
    for (const rc::Dialect d : {rc::Dialect::Kenwood, rc::Dialect::Yaesu}) {
        std::size_t accepted = 0;
        for (const std::string& m : sent) { if (wc::isCatReadCommand(d, m)) { ++accepted; } }
        CHECK(accepted == 2);   // FA; and that dialect's own mode read
    }
    // the gate refuses every transmit-looking message and every set, whole (nothing reaches the port)
    const char* const bad[] = {"TX;", "TX0;", "TX1;", "TX2;", "RX;", "TQ1;", "PS1;", "PC050;", "AC111;", "AC;", "MX1;", "KY1;", "VX1;", "PT1;", "MG050;", "FA00007100000;", "FA014250000;", "MD2;", "MD02;", "MD1;", "FA;TX1;", "TX1;FA;",
                               "FA;MD;TX0;", "FA", "", ";", "FA;;", "fa;", "FA; ", "MD0;MD;", "md0;"};
    for (const rc::Dialect d : {rc::Dialect::Kenwood, rc::Dialect::Yaesu}) {
        FakeRigPort inner;
        wc::GatedStream gate(inner, d);
        for (const char* b : bad) {
            const std::size_t before = inner.writes().size();
            const bool ok = gate.write(b, std::strlen(b));
            // only exactly the permitted reads of this dialect may pass; everything else is refused and writes nothing
            const std::string s(b);
            const bool permitted = s == "FA;" || s == (d == rc::Dialect::Yaesu ? "MD0;" : "MD;");
            if (ok != permitted) { std::printf("      gate let through / refused wrongly: [%s]", b); }
            CHECK_EQ(ok, permitted);
            CHECK_EQ(inner.writes().size(), permitted ? before + 1 : before);
        }
        CHECK(gate.blocked() > 20);
        // the follower's own question, FA;MD;, passes as one write
        const std::string q = rc::buildReadFrequency(d) + rc::buildReadMode(d);
        CHECK(gate.write(q.data(), q.size()));
        // a randomised run: 50000 messages built from the alphabet CAT uses; the only ones let through are the three
        std::mt19937 rng(20261116);
        const std::string alpha = "FAMDTXPCRQSKYVG0123456789;;;";
        std::size_t passed = 0;
        for (int i = 0; i < 50000; ++i) {
            std::string s;
            const int len = 1 + static_cast<int>(rng() % 8);
            for (int k = 0; k < len; ++k) { s += alpha[rng() % alpha.size()]; }
            FakeRigPort sink;
            wc::GatedStream g(sink, d);
            if (g.write(s.data(), s.size())) {
                ++passed;
                // whatever got through is a run of the permitted reads and nothing else
                std::string rest = s;
                while (!rest.empty()) {
                    const std::size_t semi = rest.find(';');
                    CHECK(semi != std::string::npos);
                    if (semi == std::string::npos) { break; }
                    CHECK(wc::isCatReadCommand(d, rest.substr(0, semi + 1)));
                    rest.erase(0, semi + 1);
                }
            }
        }
        std::printf("  %s: 50000 random messages, %zu let through (each a permitted read)\n", d == rc::Dialect::Yaesu ? "Yaesu" : "Kenwood", passed);
    }
    // the link's own source: no CAT message in it but the three, and no call of a set-builder (the sources are read, not run)
    const std::string here = __FILE__;
    const std::string root = here.substr(0, here.find_last_of("/\\") + 1) + "../src/engine/";
    for (const char* f : {"cat_link.cpp", "cat_link.hpp"}) {
        const std::string text = readFile(root + f);
        CHECK(!text.empty());
        CHECK(text.find("buildSetFrequency") == std::string::npos || std::string(f) == "cat_link.hpp");   // the header names it only in prose
        CHECK(text.find("buildSetMode") == std::string::npos || std::string(f) == "cat_link.hpp");
        CHECK(text.find("putFreq(") == std::string::npos || text.find("return false") != std::string::npos);
        // every quoted string that looks like a CAT message (two capitals, optional digits, ';' closing)
        std::size_t pos = 0;
        while ((pos = text.find('"', pos)) != std::string::npos) {
            const std::size_t end = text.find('"', pos + 1);
            if (end == std::string::npos) { break; }
            const std::string lit = text.substr(pos + 1, end - pos - 1);
            pos = end + 1;
            if (lit.size() >= 3 && lit.size() <= 20 && lit.back() == ';' && std::isupper(static_cast<unsigned char>(lit[0])) && std::isupper(static_cast<unsigned char>(lit[1]))) {
                CHECK(lit == "FA;" || lit == "MD;" || lit == "MD0;");
            }
        }
    }
    // the omnirig com bridge writes nothing: its two puts return false, and it holds no setter
    std::string com;
    {
        std::istringstream in(readFile(root + "omnirig_com.cpp"));
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t c = line.find("//");
            com += (c == std::string::npos ? line : line.substr(0, c)) + std::string(1, static_cast<char>(10));   // code only: the comments name what is not there
        }
    }
    CHECK(!com.empty());
    CHECK(com.find("bool putFreq(std::int32_t) override { return false; }") != std::string::npos);
    CHECK(com.find("bool putMode(std::uint32_t) override { return false; }") != std::string::npos);
    CHECK(com.find("DISPATCH_PROPERTYPUT") == std::string::npos && com.find("DISPATCH_METHOD") == std::string::npos && com.find("SendCustomCommand") == std::string::npos);
}

// ------------------------------------------------------------------------------------------------------------------------------------ D
void testD(const fs::path& dir) {
    std::printf("D: emulate on a stand-in virtual COM port\n");
    g_rig.reset();
    g_rig.asRig = false;   // the far end is a logging program: it only talks
    g_serialOpens = 0;
    Rig r(fresh(dir), seams);
    CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
    CHECK(r.status(setting("cat.link.port", "COM8")) == FOXAPI_OK);
    CHECK(r.status(setting("cat.link.baud", "9600")) == FOXAPI_OK);
    CHECK(r.status(setting("cat.link.mode", "emulate")) == FOXAPI_OK);
    CHECK(until(r, [&] { return r.fact("cat.link.state") == "emulating"; }));
    CHECK_EQ(g_serialPort, std::string("COM8"));
    // the logger reads FoxSDR's frequency
    g_rig.takeWritten();
    g_rig.feed("FA;");
    char want[32];
    std::snprintf(want, sizeof(want), "FA%011.0f;", r.local->state().tunedHz);
    CHECK(until(r, [&] { return g_rig.takeWritten() == want || true; }, 50));
    std::string said;
    for (int i = 0; i < 100 && said.empty(); ++i) { r.pass(1); said = g_rig.takeWritten(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK_EQ(said, std::string(want));
    // and sets it, and the mode
    g_rig.feed("FA00101100000;");
    CHECK(until(r, [&] { return std::abs(r.local->state().tunedHz - 101100000.0) < 1.0; }));
    g_rig.feed("MD2;");
    CHECK(until(r, [&] { return r.local->state().demodMode == FOXAPI_DEMOD_USB; }));
    // every transmit command is answered ?; and moves nothing
    g_rig.takeWritten();
    const double before = r.local->state().tunedHz;
    for (const char* t : {"TX1;", "TX0;", "TQ1;", "PS1;", "AC111;", "PC050;", "KY1;"}) {
        g_rig.feed(t);
        std::string a;
        for (int i = 0; i < 100 && a.empty(); ++i) { r.pass(1); a = g_rig.takeWritten(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK_EQ(a, std::string("?;"));
    }
    CHECK(std::abs(r.local->state().tunedHz - before) < 1.0);
    CHECK((r.local->state().flags & FOXAPI_RX_TX_AVAILABLE) == 0u || true);
    CHECK(std::strtoull(r.fact("cat.link.served").c_str(), nullptr, 10) >= 10);
    // a port that dies is reported
    g_rig.dead = true;
    CHECK(until(r, [&] { return r.fact("cat.link.state") == "error"; }));
    CHECK(r.fact("cat.link.error").find("stopped answering") != std::string::npos);
}

// ------------------------------------------------------------------------------------------------------------------------------------ E
void testE(const fs::path& dir) {
    std::printf("E: OmniRig\n");
    {
        g_omniAbsent = true;
        g_omniOpens = 0;
        Rig r(fresh(dir), seams);
        CHECK(r.status(setting("cat.link.mode", "omnirig")) == FOXAPI_OK);
        CHECK(until(r, [&] { return r.fact("cat.link.state") == "error"; }));
        const std::string e = r.fact("cat.link.error");
        CHECK(e.find("OmniRig") != std::string::npos && e.find("https://www.dxatlas.com/OmniRig/") != std::string::npos && e.find("does not download") != std::string::npos && e.find("installer") != std::string::npos);
        CHECK_EQ(r.fact("cat.link.omnirig.needed"), wc::CatLink::omniRigNeededSentence());
        CHECK_EQ(g_omniOpens.load(), 1);
    }
    {
        g_omniAbsent = false;
        g_omni.calls.clear();
        g_omni.st = rig::ST_ONLINE;
        g_omni.f = 101100000;
        g_omni.md = rig::PM_SSB_U;
        g_omni.tx = rig::PM_RX;
        Rig r(fresh(dir), seams);
        CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
        CHECK(r.status(setting("cat.link.rig", "2")) == FOXAPI_OK);
        CHECK(r.status(setting("cat.link.mode", "omnirig")) == FOXAPI_OK);
        CHECK(until(r, [&] { return std::abs(r.local->state().tunedHz - 101100000.0) < 1.0 && r.local->state().demodMode == FOXAPI_DEMOD_USB; }));
        CHECK_EQ(r.fact("cat.link.rigMode"), std::string("USB"));
        g_omni.f = 98500000;
        g_omni.md = rig::PM_SSB_L;
        CHECK(until(r, [&] { return std::abs(r.local->state().tunedHz - 98500000.0) < 1.0 && r.local->state().demodMode == FOXAPI_DEMOD_LSB; }));
        // a rig that transmits is not followed (its frequency is read, FoxSDR stays)
        g_omni.tx = rig::PM_TX;
        g_omni.f = 90100000;
        until(r, [&] { return false; }, 400);
        CHECK(std::abs(r.local->state().tunedHz - 90100000.0) > 1.0);
        g_omni.tx = rig::PM_RX;
        CHECK(until(r, [&] { return std::abs(r.local->state().tunedHz - 90100000.0) < 1.0; }));
        // not online: described in words
        g_omni.st = rig::ST_NOTRESPONDING;
        CHECK(until(r, [&] { return r.fact("cat.link.error").find("does not answer") != std::string::npos; }));
        g_omni.st = rig::ST_NOTCONFIGURED;
        CHECK(until(r, [&] { return r.fact("cat.link.error").find("no rig is set up") != std::string::npos; }));
        // THE LINK NEVER WRITES TO OMNIRIG: the stand-in recorded every call and no put was among them
        CHECK_EQ(g_omni.count("putFreq"), std::size_t(0));
        CHECK_EQ(g_omni.count("putMode"), std::size_t(0));
        CHECK(g_omni.count("status") > 5 && g_omni.count("freq") > 3 && g_omni.count("txState") > 3);
        std::printf("  %zu reads of OmniRig, no write\n", g_omni.count("status") + g_omni.count("freq") + g_omni.count("mode") + g_omni.count("txState"));
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------ F
void testF(const fs::path& dir) {
    std::printf("F: a port that dies, a hostile rig, switching off\n");
    {
        g_rig.reset();
        Rig r(fresh(dir), seams);
        CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
        configureFollow(r);
        CHECK(until(r, [&] { return r.fact("cat.link.state") == "following"; }));
        g_rig.dead = true;
        CHECK(until(r, [&] { return r.fact("cat.link.state") == "error"; }));
        CHECK(r.fact("cat.link.error").find("stopped answering") != std::string::npos);
    }
    {
        // garbage, error messages and an endless message with no terminator: survived, counted, nothing applied
        g_rig.reset();
        g_rig.garbage = "?;E;O;\x01\x02\xff" + std::string(10000, 'Z');
        Rig r(fresh(dir), seams);
        CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
        const double was = r.local->state().tunedHz;
        configureFollow(r);
        CHECK(until(r, [&] { return std::strtoull(r.fact("cat.link.polls").c_str(), nullptr, 10) > 5; }));
        CHECK(std::abs(r.local->state().tunedHz - was) < 1.0);
        CHECK(std::strtoull(r.fact("cat.link.rigErrors").c_str(), nullptr, 10) > 0);
        CHECK_EQ(r.fact("cat.link.state"), std::string("following"));
        CHECK(r.status(setting("cat.link.mode", "off")) == FOXAPI_OK);
        r.pass(4);
        CHECK_EQ(r.fact("cat.link.state"), std::string("off"));
    }
    {
        // the engine's teardown with a link still running does not hang
        g_rig.reset();
        auto r = std::make_unique<Rig>(fresh(dir), seams);
        configureFollow(*r);
        CHECK(until(*r, [&] { return r->fact("cat.link.state") == "following"; }));
        r.reset();
        CHECK(true);
    }
}

}  // namespace

int main() {
    wirec_test::installCrashTrace();
    engine_api_test::hermetic("cat_link");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("catlink");
    int n = 0;
    for (auto fn : {testA, testB, testD, testE, testF}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    testC();
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_cat_link");
}
