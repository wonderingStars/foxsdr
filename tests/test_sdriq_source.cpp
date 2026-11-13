// test_sdriq_source.cpp - the RFspace SDR-IQ message layer and source (src/source/sdriq_protocol.hpp,
// sdriq_source.{hpp,cpp}) against a STAND-IN radio written here from RFspace/MoeTronix's "SDR-IQ Interface
// Specification" Ver. 1.04. The stand-in sits behind SdriqLink; nothing touches USB or FTDI. UNPROVEN on a real
// SDR-IQ. Byte strings in testLiterals are the ones printed in the document (section numbers in comments).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#if defined(_WIN32)
#include <windows.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "source/sdriq_source.hpp"
#include "test_check.hpp"

using namespace cascade::source;
namespace as = cascade::source::ascp;
namespace sq = cascade::source::sdriq;
using Bytes = std::vector<std::uint8_t>;
using Cx = std::complex<float>;

namespace {

// ---- the stand-in radio -----------------------------------------------------------------------------------
struct FakeState {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::uint8_t> out;  // bytes the radio has queued for the host
    bool intr = false, closed = false;
    bool running = false;
    std::uint32_t rateCode = sq::kPowerUpRate;
    std::uint64_t freq = 0;
    int rfGain = 0, ifGain = 0;
    // configuration
    std::string name = "SDR-IQ", serial = "MT123456";
    bool silent = false;       // answers nothing
    bool nakFreq = false;      // NAK for frequency Set
    bool bootMode = false;     // status says boot loader idle
    bool noData = false;       // runs but sends no I/Q
    bool failRead = false;     // read() fails
    bool junkAfterStart = false;
    double toneHz = 12345.6, ppm = 0.0, noise = 0.02;
    std::uint64_t maxFreq = 30000000;
    // log
    struct Rx { as::MsgType type; std::uint16_t item; Bytes params; };
    std::vector<Rx> log;
    std::uint64_t blocksSent = 0;
    std::uint64_t nonIdle = 0;
    // generator
    std::mt19937 rng{12345};
    double phase = 0.0;
    as::StreamAssembler asmb;

    void queue(const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
    void reply(as::MsgType type, std::uint16_t item, const Bytes& body) {
        // the radio's header types: response 0, unsolicited 1, range response 2
        queue(as::encodeMessage(type, item, body));
    }
    Bytes cstr(const std::string& s) { Bytes b(s.begin(), s.end()); b.push_back(0); return b; }

    void handle(const as::Message& m) {
        log.push_back({m.type, m.item, m.body});
        if (silent) { return; }
        if (m.type == as::MsgType::Request) {
            switch (m.item) {
                case sq::ci::Name: reply(as::MsgType::Set, m.item, cstr(name)); return;
                case sq::ci::Serial: reply(as::MsgType::Set, m.item, cstr(serial)); return;
                case sq::ci::Status: reply(as::MsgType::Set, m.item, Bytes{static_cast<std::uint8_t>(bootMode ? sq::kStatusBootIdle : sq::kStatusIdle)}); return;
                case sq::ci::RxFrequency: {
                    Bytes b{0x00};
                    as::putFrequency(b, freq);
                    reply(as::MsgType::Set, m.item, b);
                    return;
                }
                case sq::ci::RfGain:
                    reply(as::MsgType::Set, m.item, Bytes{0x00, static_cast<std::uint8_t>(static_cast<std::int8_t>(rfGain))});
                    return;
                default: queue(Bytes{0x02, 0x00}); return;  // NAK
            }
        }
        if (m.type == as::MsgType::RequestRange && m.item == sq::ci::RxFrequency) {
            Bytes b{0x00};
            as::putFrequency(b, 0);
            as::putFrequency(b, maxFreq);
            reply(as::MsgType::RequestRange, m.item, b);
            return;
        }
        if (m.type == as::MsgType::Set) {
            switch (m.item) {
                case sq::ci::RxState:
                    if (m.body.size() >= 2 && m.body[0] == sq::kChannel) {
                        running = m.body[1] == sq::kRunRun;
                        reply(as::MsgType::Set, m.item, m.body);
                        if (running && junkAfterStart) { queue(Bytes{0x01, 0x00}); }
                    } else {
                        queue(Bytes{0x02, 0x00});
                    }
                    return;
                case sq::ci::OutSampleRate: {
                    // "any other values will be ignored": the reply carries the value now in force
                    if (m.body.size() >= 5 && !running) {
                        const std::uint32_t c = static_cast<std::uint32_t>(as::getLe(m.body.data() + 1, 4));
                        for (const sq::RateCode& r : sq::kRates) {
                            if (r.code == c) { rateCode = c; }
                        }
                    }
                    Bytes b{0x00};
                    as::putLe(b, rateCode, 4);
                    reply(as::MsgType::Set, m.item, b);
                    return;
                }
                case sq::ci::RxFrequency:
                    if (nakFreq) { queue(Bytes{0x02, 0x00}); return; }
                    if (m.body.size() >= 6) { freq = as::getFrequency(m.body.data() + 1); }
                    reply(as::MsgType::Set, m.item, m.body);
                    return;
                case sq::ci::RfGain:
                    if (m.body.size() >= 2) { rfGain = static_cast<std::int8_t>(m.body[1]); }
                    reply(as::MsgType::Set, m.item, m.body);
                    return;
                case sq::ci::IfGain:
                    if (m.body.size() >= 2) { ifGain = m.body[1]; }
                    reply(as::MsgType::Set, m.item, m.body);
                    return;
                default: queue(Bytes{0x02, 0x00}); return;
            }
        }
        queue(Bytes{0x02, 0x00});
    }

    // one Data Item 0 block of tone + noise at the radio's TRUE rate (nominal x (1 + ppm))
    void makeBlock() {
        const double nominal = sq::rateHzFor(rateCode);
        const double fs = nominal * (1.0 + ppm * 1e-6);
        const double w = 2.0 * 3.14159265358979323846 * toneHz / fs;
        std::normal_distribution<double> nd(0.0, noise);
        Bytes b{0x00, 0x80};
        for (std::size_t i = 0; i < sq::kDataBlockPairs; ++i) {
            const double I = 0.5 * std::cos(phase) + nd(rng);
            const double Q = 0.5 * std::sin(phase) + nd(rng);
            phase += w;
            if (phase > 6.283185307179586) { phase -= 6.283185307179586; }
            auto put = [&](double v) {
                const long s = std::clamp(std::lround(v * 32768.0), -32768L, 32767L);
                as::putLe(b, static_cast<std::uint16_t>(static_cast<std::int16_t>(s)), 2);
            };
            put(I);
            put(Q);
        }
        queue(b);
        ++blocksSent;
    }
};

class FakeLink : public SdriqLink {
public:
    explicit FakeLink(std::shared_ptr<FakeState> s) : s_(std::move(s)) {}
    bool write(const std::uint8_t* d, std::size_t n, std::string& error) override {
        std::lock_guard<std::mutex> lk(s_->mu);
        if (s_->closed) { error = "closed"; return false; }
        std::vector<as::Message> msgs;
        s_->asmb.feed(d, n, msgs);
        for (const as::Message& m : msgs) { s_->handle(m); }
        s_->cv.notify_all();
        return true;
    }
    int read(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        std::unique_lock<std::mutex> lk(s_->mu);
        if (s_->failRead) { return -1; }
        if (s_->out.empty() && s_->running && !s_->noData) {
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));  // about 1 MS/s: faster than the radio
            lk.lock();
            if (s_->out.empty() && s_->running) { s_->makeBlock(); }
        }
        if (s_->out.empty()) {
            s_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !s_->out.empty() || s_->intr || s_->closed; });
            if (s_->intr) { s_->intr = false; return 0; }
            if (s_->out.empty()) { return 0; }
        }
        std::size_t n = 0;
        while (n < cap && !s_->out.empty()) {
            dst[n++] = s_->out.front();
            s_->out.pop_front();
        }
        return static_cast<int>(n);
    }
    void interrupt() override {
        std::lock_guard<std::mutex> lk(s_->mu);
        s_->intr = true;
        s_->cv.notify_all();
    }
    void close() override {
        std::lock_guard<std::mutex> lk(s_->mu);
        s_->closed = true;
        s_->cv.notify_all();
    }

private:
    std::shared_ptr<FakeState> s_;
};

SdriqOpener openerFor(const std::shared_ptr<FakeState>& st, std::string* wantedSerial = nullptr) {
    return [st, wantedSerial](const std::string& serial, std::string&) -> std::unique_ptr<SdriqLink> {
        if (wantedSerial) { *wantedSerial = serial; }
        return std::make_unique<FakeLink>(st);
    };
}

// reads until n samples or the deadline; returns what was read
std::vector<Cx> readSome(SdriqSource& s, std::size_t n, int ms) {
    std::vector<Cx> v;
    std::vector<Cx> tmp(4096);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (v.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t got = s.read(tmp.data(), tmp.size());
        v.insert(v.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return v;
}

// the tone frequency by the average lag-1 phase step, at the NOMINAL rate
double toneHzOf(const std::vector<Cx>& x, double fsNominal) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t i = 1; i < x.size(); ++i) {
        acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1]));
    }
    return std::arg(acc) * fsNominal / (2.0 * 3.14159265358979323846);
}

bool logHas(FakeState& st, as::MsgType type, std::uint16_t item) {
    std::lock_guard<std::mutex> lk(st.mu);
    for (const auto& r : st.log) {
        if (r.type == type && r.item == item) { return true; }
    }
    return false;
}

// ---- the document's literal examples ---------------------------------------------------------------------------
void testLiterals() {
    // 5.1.1 name request [04][20][01][00] and the response "SDR-IQ"
    CHECK(as::encodeMessage(as::MsgType::Request, sq::ci::Name) == (Bytes{0x04, 0x20, 0x01, 0x00}));
    // 5.2.1 start, contiguous, channel 0x81 [08][00][18][00][81][02][00][01]; stop [08][00][18][00][81][01][00][00]
    CHECK(as::encodeMessage(as::MsgType::Set, sq::ci::RxState, sq::runParams(true)) == (Bytes{0x08, 0x00, 0x18, 0x00, 0x81, 0x02, 0x00, 0x01}));
    const Bytes stop = as::encodeMessage(as::MsgType::Set, sq::ci::RxState, sq::runParams(false));
    CHECK(stop.size() == 8 && stop[4] == 0x81 && stop[5] == 0x01 && stop[6] == 0x00);  // blocks byte ignored when idle
    // 5.2.2 14.010 MHz [0A][00][20][00][00][90][C6][D5][00][00]
    CHECK(as::encodeMessage(as::MsgType::Set, sq::ci::RxFrequency, sq::frequencyParams(14010000)) ==
          (Bytes{0x0A, 0x00, 0x20, 0x00, 0x00, 0x90, 0xC6, 0xD5, 0x00, 0x00}));
    // 5.2.2 range request [05][40][20][00][00]
    CHECK(as::encodeMessage(as::MsgType::RequestRange, sq::ci::RxFrequency, {0x00}) == (Bytes{0x05, 0x40, 0x20, 0x00, 0x00}));
    // 5.2.4 55556 Hz [09][00][B8][00][00][04][D9][00][00]
    CHECK(as::encodeMessage(as::MsgType::Set, sq::ci::OutSampleRate, sq::rateParams(55556)) == (Bytes{0x09, 0x00, 0xB8, 0x00, 0x00, 0x04, 0xD9, 0x00, 0x00}));
    // 5.2.5 fixed gain -20 dB [06][00][38][00][00][EC]
    CHECK(as::encodeMessage(as::MsgType::Set, sq::ci::RfGain, sq::rfGainParams(-20)) == (Bytes{0x06, 0x00, 0x38, 0x00, 0x00, 0xEC}));
    // 5.2.6 IF gain 12 dB [06][00][40][00][00][0C]
    CHECK(as::encodeMessage(as::MsgType::Set, sq::ci::IfGain, sq::ifGainParams(12)) == (Bytes{0x06, 0x00, 0x40, 0x00, 0x00, 0x0C}));
    // 3.2 the data ACK [03][60][02]
    CHECK(as::encodeDataAck(2) == (Bytes{0x03, 0x60, 0x02}));
    // the rate codes against their divisors (5.2.4)
    for (const sq::RateCode& r : sq::kRates) {
        CHECK(std::fabs(sq::kAdClockHz / r.divisor - r.code) < 1.0);
        std::uint32_t c = 0;
        CHECK(sq::rateCodeFor(r.code, c) && c == r.code);
    }
    std::uint32_t c = 0;
    CHECK(!sq::rateCodeFor(196077.0, c) && !sq::rateCodeFor(0.0, c) && !sq::rateCodeFor(-1.0, c) && !sq::rateCodeFor(250000.0, c));
    // responses fed byte by byte: name, an unsolicited idle (5.2.1), the range answer (5.2.2: 10 bytes, no count)
    as::StreamAssembler a;
    std::vector<as::Message> m;
    const Bytes name{0x0B, 0x00, 0x01, 0x00, 'S', 'D', 'R', '-', 'I', 'Q', 0x00};
    for (std::uint8_t b : name) { a.feed(&b, 1, m); }
    CHECK(m.size() == 1 && m[0].item == sq::ci::Name);
    std::string str;
    CHECK(sq::parseCString(m[0].body, str) && str == "SDR-IQ");
    m.clear();
    const Bytes idle{0x08, 0x20, 0x18, 0x00, 0x81, 0x01, 0x02, 0x00};
    a.feed(idle.data(), idle.size(), m);
    CHECK(m.size() == 1 && m[0].type == as::MsgType::Request && m[0].item == sq::ci::RxState);
    m.clear();
    const Bytes range{0x0F, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xC3, 0xC9, 0x01, 0x00};
    a.feed(range.data(), range.size(), m);
    CHECK(m.size() == 1 && m[0].type == as::MsgType::RequestRange);
    as::FreqRange fr;
    CHECK(sq::parseRange(m[0].body, fr) && fr.minHz == 0 && fr.maxHz == 30000000);
    for (std::size_t cut = 0; cut < m[0].body.size(); ++cut) {
        CHECK(!sq::parseRange(Bytes(m[0].body.begin(), m[0].body.begin() + static_cast<std::ptrdiff_t>(cut)), fr));
    }
    // a data block header is 00 80 and the assembler reads it as 8194 bytes
    Bytes blk(sq::kDataBlockBytes, 0);
    blk[0] = 0x00;
    blk[1] = 0x80;
    m.clear();
    a.feed(blk.data(), blk.size(), m);
    CHECK(m.size() == 1 && m[0].type == as::MsgType::Data0 && m[0].body.size() == 8192);
}

void testDecodeBlock() {
    Bytes body(8192, 0);
    // I1 = 32767, Q1 = -32768, I2 = -1, Q2 = 1 (little endian)
    body[0] = 0xFF; body[1] = 0x7F; body[2] = 0x00; body[3] = 0x80;
    body[4] = 0xFF; body[5] = 0xFF; body[6] = 0x01; body[7] = 0x00;
    std::vector<Cx> out;
    CHECK(sq::decodeBlock(body, out) && out.size() == 2048);
    CHECK(out[0].real() == 32767.0f / 32768.0f && out[0].imag() == -1.0f);
    CHECK(out[1].real() == -1.0f / 32768.0f && out[1].imag() == 1.0f / 32768.0f);
    out.clear();
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{4}, std::size_t{8191}, std::size_t{8193}, std::size_t{16384}}) {
        CHECK(!sq::decodeBlock(Bytes(n, 0), out) && out.empty());
    }
}

void testCStringAndRange() {
    std::string s;
    CHECK(!sq::parseCString({}, s));
    CHECK(!sq::parseCString({'A', 'B'}, s));                       // no terminator
    CHECK(!sq::parseCString({0x00}, s));                           // empty
    CHECK(!sq::parseCString({'A', 0x07, 0x00}, s));                // control character
    CHECK(!sq::parseCString({'A', 0xC3, 0x00}, s));                // non-ASCII
    CHECK(sq::parseCString({'A', 'B', 0x00, 'Z'}, s) && s == "AB"); // stops at the terminator
    Bytes long33(33, 'x');
    long33.push_back(0);
    CHECK(!sq::parseCString(long33, s));
    as::FreqRange r;
    CHECK(!sq::parseRange({}, r));
    Bytes inverted{0x00, 0x10, 0, 0, 0, 0, 0x01, 0, 0, 0, 0};  // min 16, max 1
    CHECK(!sq::parseRange(inverted, r));
}

void testFuzzBytes() {
    std::mt19937 rng(777);
    int blocks = 0, errs = 0;
    for (int round = 0; round < 3000; ++round) {
        as::StreamAssembler a;
        std::vector<as::Message> m;
        Bytes junk;
        const unsigned kind = round % 3;
        if (kind == 0) {  // a data block header followed by random bytes: decodes
            junk = Bytes{0x00, 0x80};
            junk.resize(sq::kDataBlockBytes + rng() % 300);
            for (std::size_t i = 2; i < junk.size(); ++i) { junk[i] = static_cast<std::uint8_t>(rng()); }
        } else if (kind == 1) {  // an impossible length first
            junk = Bytes{0x01, 0x00};
            junk.resize(2 + rng() % 500);
            for (std::size_t i = 2; i < junk.size(); ++i) { junk[i] = static_cast<std::uint8_t>(rng()); }
        } else {
            junk.resize(rng() % 40000);
            for (auto& b : junk) { b = static_cast<std::uint8_t>(rng()); }
        }
        std::size_t pos = 0;
        while (pos < junk.size()) {
            const std::size_t step = 1 + rng() % 700;
            const std::size_t n = std::min(step, junk.size() - pos);
            a.feed(junk.data() + pos, n, m);
            pos += n;
        }
        for (const as::Message& msg : m) {
            std::vector<Cx> out;
            if (msg.type == as::MsgType::Data0 && sq::decodeBlock(msg.body, out)) { ++blocks; CHECK(out.size() == 2048); }
        }
        errs += a.error().empty() ? 0 : 1;
        std::string s;
        as::FreqRange fr;
        (void)sq::parseCString(junk, s);
        (void)sq::parseRange(junk, fr);
    }
    CHECK(errs > 900 && blocks > 900);
    std::printf("sdriq fuzz: %d blocks decoded, %d streams refused of 3000\n", blocks, errs);
}

// ---- the source against the stand-in ---------------------------------------------------------------------------
void testOpenAndIdentify() {
    auto st = std::make_shared<FakeState>();
    st->freq = 7100000;
    st->rfGain = -10;
    std::string asked = "unset";
    SdriqSource s;
    s.setOpener(openerFor(st, &asked));
    CHECK(!s.isOpen());
    CHECK(s.open("sdriq=MT123456"));
    CHECK(asked == "MT123456");
    CHECK(s.isOpen() && s.serialNumber() == "MT123456");
    CHECK(std::string(s.name()).find("SDR-IQ") != std::string::npos);
    CHECK(s.centerFrequencyHz() == 7100000.0);
    CHECK(s.gainDb("RF gain") == -10.0 && s.gainDb("IF gain") == 0.0);
    double lo = -1, hi = -1;
    CHECK(s.frequencyRangeHz(lo, hi) && lo == 0.0 && hi == 30000000.0);
    CHECK(s.sampleRateHz() > 196078.0 && s.sampleRateHz() < 196079.0);
    const auto g = s.gains();
    CHECK(g.size() == 2 && g[0].name == "RF gain" && g[0].minDb == -30.0 && g[0].maxDb == 0.0 && g[0].stepDb == 10.0 &&
          g[1].name == "IF gain" && g[1].maxDb == 24.0 && g[1].stepDb == 6.0);
    const auto rates = s.supportedSampleRatesHz();
    CHECK(rates.size() == 7 && rates.front() < 8139.0 && rates.back() > 196078.0 && rates.back() < 196079.0);
    // nothing but identification was sent: no Set of any kind
    {
        std::lock_guard<std::mutex> lk(st->mu);
        for (const auto& r : st->log) { CHECK(r.type != as::MsgType::Set); }
    }
    s.closeDevice();
    CHECK(!s.isOpen());
}

void testOpenRefusals() {
    {   // the production opener looks for FTDI's D2XX library the user installed (PATH only); with no PATH it says so, naming FTDI and D2XX
        // (radio-bridges lane: the link itself is tested in test_sdriq_d2xx.cpp). PATH is emptied so the test never touches a real driver.
#if defined(_WIN32)
        wchar_t old[32768];
        const DWORD oldLen = ::GetEnvironmentVariableW(L"PATH", old, 32768);
        ::SetEnvironmentVariableW(L"PATH", L"");
#endif
        SdriqSource s;
        CHECK(!s.open(""));
        const std::string e = s.lastError();
        CHECK(e.find("FTDI") != std::string::npos && e.find("D2XX") != std::string::npos && !s.isOpen());
#if defined(_WIN32)
        if (oldLen > 0 && oldLen < 32768) { ::SetEnvironmentVariableW(L"PATH", old); }
#endif
    }
    {   // an opener that finds no radio says so
        SdriqSource s;
        s.setOpener([](const std::string&, std::string& err) -> std::unique_ptr<SdriqLink> { err = "no SDR-IQ is connected"; return nullptr; });
        CHECK(!s.open("sdriq=ABC") && std::string(s.lastError()) == "no SDR-IQ is connected");
        s.setOpener([](const std::string&, std::string&) -> std::unique_ptr<SdriqLink> { return nullptr; });
        CHECK(!s.open("") && std::string(s.lastError()).find("no SDR-IQ") != std::string::npos);
    }
    {   // wrong name, including the SDR-14 sibling and an empty and a non-printable name
        for (const char* nm : {"SDR-14", "NetSDR", "sdr-iq", "SDR-IQ2"}) {
            auto st = std::make_shared<FakeState>();
            st->name = nm;
            SdriqSource s;
            s.setOpener(openerFor(st));
            CHECK(!s.open(""));
            CHECK(std::string(s.lastError()).find("not an SDR-IQ") != std::string::npos);
            CHECK(!s.isOpen() && !s.faulted());
        }
        auto st = std::make_shared<FakeState>();
        st->name = std::string("SDR\x01IQ");
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(!s.open("") && std::string(s.lastError()).find("not a name") != std::string::npos);
    }
    {   // a radio that answers nothing: fails within the timeout, not forever
        auto st = std::make_shared<FakeState>();
        st->silent = true;
        SdriqSource s;
        s.setOpener(openerFor(st));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.open("timeout=300"));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms >= 250 && ms < 3000);
        CHECK(std::string(s.lastError()).find("did not answer") != std::string::npos);
    }
    {   // the firmware boot loader: refused with a sentence
        auto st = std::make_shared<FakeState>();
        st->bootMode = true;
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(!s.open("") && std::string(s.lastError()).find("boot mode") != std::string::npos);
        std::lock_guard<std::mutex> lk(st->mu);
        for (const auto& r : st->log) { CHECK(r.type != as::MsgType::Set); }
    }
    {   // another serial number than asked for
        auto st = std::make_shared<FakeState>();
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(!s.open("sdriq=OTHER") && std::string(s.lastError()).find("not the one asked for") != std::string::npos);
    }
    {   // bad arguments
        auto st = std::make_shared<FakeState>();
        SdriqSource s;
        s.setOpener(openerFor(st));
        for (const char* a : {"timeout=5", "timeout=x", "timeout=99999999", "sdriq=\x01"}) { CHECK(!s.open(a)); }
        CHECK(!s.open(std::string("sdriq=") + std::string(40, 'A')));
        CHECK(s.open(""));
    }
    {   // junk on the line: an impossible length is a fault, not a hang
        auto st = std::make_shared<FakeState>();
        {
            std::lock_guard<std::mutex> lk(st->mu);
            st->queue(Bytes{0x01, 0x00});  // length 1 is impossible
        }
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(!s.open("timeout=500"));
        CHECK(!s.isOpen());
    }
}

void testTuneRateGain() {
    auto st = std::make_shared<FakeState>();
    SdriqSource s;
    s.setOpener(openerFor(st));
    CHECK(s.open(""));
    CHECK(s.setCenterFrequencyHz(14010000.0) && s.centerFrequencyHz() == 14010000.0);
    {
        std::lock_guard<std::mutex> lk(st->mu);
        CHECK(st->freq == 14010000);
    }
    CHECK(s.setCenterFrequencyHz(0.0) && s.setCenterFrequencyHz(30000000.0));
    CHECK(!s.setCenterFrequencyHz(30000001.0) && std::string(s.lastError()).find("outside") != std::string::npos);
    CHECK(!s.setCenterFrequencyHz(-1.0) && !s.setCenterFrequencyHz(std::nan("")) && !s.setCenterFrequencyHz(HUGE_VAL));
    CHECK(s.centerFrequencyHz() == 30000000.0);
    // rates: the seven, by code or by exact value; anything else refused
    for (const sq::RateCode& r : sq::kRates) { CHECK(s.setSampleRateHz(r.code)); }
    CHECK(s.setSampleRateHz(66666667.0 / 1200.0) && std::fabs(s.sampleRateHz() - 55555.56) < 0.01);
    for (double bad : {0.0, 48000.0, 196077.0, 250000.0, -1.0, std::nan("")}) { CHECK(!s.setSampleRateHz(bad)); }
    CHECK(std::string(s.lastError()).find("8138") != std::string::npos);
    // gains
    for (int g : {0, -10, -20, -30}) { CHECK(s.setGainDb("RF gain", g) && s.gainDb("RF gain") == g); }
    for (double bad : {5.0, -5.0, -40.0, 10.0, -9.9, std::nan("")}) { CHECK(!s.setGainDb("RF gain", bad)); }
    for (int g : {0, 6, 12, 18, 24}) { CHECK(s.setGainDb("IF gain", g) && s.gainDb("IF gain") == g); }
    for (double bad : {1.0, 30.0, -6.0, 7.0}) { CHECK(!s.setGainDb("IF gain", bad)); }
    CHECK(!s.setGainDb("LNA", 0.0));
    CHECK(!s.autoGainSupported() && !s.setAutoGain(true) && s.antennas().empty() && !s.setAntenna("x"));
    // a NAK on tuning is reported, not believed
    {
        std::lock_guard<std::mutex> lk(st->mu);
        st->nakFreq = true;
    }
    CHECK(!s.setCenterFrequencyHz(5000000.0) && std::string(s.lastError()).find("does not implement") != std::string::npos);
    // closed: every call says so
    s.closeDevice();
    CHECK(!s.setCenterFrequencyHz(1.0e6) && !s.setSampleRateHz(55556) && !s.setGainDb("RF gain", 0.0) && !s.start());
    // nothing forbidden was ever sent: no firmware update (0x0300..0x0302), serial port (0x0200/1), security code
    for (int itemI : {0x0300, 0x0301, 0x0302, 0x0200, 0x0201, 0x000B, 0x00B0}) {
        const std::uint16_t item = static_cast<std::uint16_t>(itemI);
        CHECK(!logHas(*st, as::MsgType::Set, item));
        CHECK(!logHas(*st, as::MsgType::Request, item));
    }
}

void testStreamingTone() {
    for (const sq::RateCode& rc : {sq::kRates[6], sq::kRates[3], sq::kRates[0]}) {
        auto st = std::make_shared<FakeState>();
        st->toneHz = 3000.0;
        st->ppm = -300.0;  // the radio's clock is 300 ppm off
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(s.open(""));
        CHECK(s.setSampleRateHz(rc.code));
        CHECK(s.setGainDb("RF gain", -20.0) && s.setGainDb("IF gain", 12.0));
        CHECK(s.start() && s.running());
        const std::vector<Cx> x = readSome(s, 20000, 8000);
        CHECK(x.size() >= 20000);
        const double fs = sq::rateHzFor(rc.code);
        const double tone = std::min(3000.0, fs / 5.0);
        {
            std::lock_guard<std::mutex> lk(st->mu);
            st->toneHz = tone;
        }
        // let the new tone through, then measure
        (void)readSome(s, 8192, 3000);
        const std::vector<Cx> y = readSome(s, 20000, 8000);
        CHECK(y.size() >= 20000);
        // the stand-in's true rate is nominal x (1 - 300 ppm); measured at the nominal rate the tone is high by that
        const double want = tone / (1.0 - 300e-6);
        const double got = toneHzOf(y, fs);
        CHECK(std::fabs(got - want) < 0.02 * tone + 1.0);
        CHECK(got > 0.0);  // a positive frequency stays positive: I and Q are not swapped
        // the tone is there and the amplitude is not mangled (0.5 of full scale)
        double p = 0;
        for (const Cx& c : y) { p += std::norm(c); }
        p /= static_cast<double>(y.size());
        CHECK(p > 0.2 && p < 0.35);
        // the radio saw the gains and the rate before the run bit
        {
            std::lock_guard<std::mutex> lk(st->mu);
            CHECK(st->rateCode == rc.code && st->rfGain == -20 && st->ifGain == 12 && st->running);
            std::size_t iRate = 0, iRun = 0;
            for (std::size_t i = 0; i < st->log.size(); ++i) {
                if (st->log[i].type == as::MsgType::Set && st->log[i].item == sq::ci::OutSampleRate) { iRate = i; }
                if (st->log[i].type == as::MsgType::Set && st->log[i].item == sq::ci::RxState) { iRun = i; }
            }
            CHECK(iRate != 0 && iRun > iRate);
        }
        CHECK(!s.setSampleRateHz(55556) && std::string(s.lastError()).find("stop") != std::string::npos);
        // retune and regain while running
        CHECK(s.setCenterFrequencyHz(9500000.0) && s.setGainDb("RF gain", -30.0));
        s.stop();
        CHECK(!s.running());
        {
            std::lock_guard<std::mutex> lk(st->mu);
            CHECK(!st->running && st->freq == 9500000);
        }
        CHECK(s.dataBlocks() > 10 && s.otherBlocks() == 0 && s.samplesReceived() > 0);
        s.closeDevice();
    }
}

void testFaults() {
    {   // the radio stops sending while running
        auto st = std::make_shared<FakeState>();
        st->noData = true;
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(s.open("timeout=300"));
        CHECK(s.start());
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (!s.faulted() && std::chrono::steady_clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.faulted() && s.faultedWhile().find("stopped sending") != std::string::npos);
        s.closeDevice();
    }
    {   // the link fails (the cable is pulled)
        auto st = std::make_shared<FakeState>();
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(s.open(""));
        CHECK(s.start());
        {
            std::lock_guard<std::mutex> lk(st->mu);
            st->failRead = true;
            st->cv.notify_all();
        }
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!s.faulted() && std::chrono::steady_clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.faulted() && s.faultedWhile().find("connection failed") != std::string::npos);
        CHECK(!s.start());
        s.closeDevice();
    }
    {   // junk after the start: a malformed message faults the source
        auto st = std::make_shared<FakeState>();
        st->junkAfterStart = true;
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(s.open(""));
        s.start();
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!s.faulted() && std::chrono::steady_clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.faulted() && s.faultedWhile().find("malformed") != std::string::npos);
        s.closeDevice();
    }
    {   // a rate the radio ignores (it replies with the old value): start() says so
        auto st = std::make_shared<FakeState>();
        SdriqSource s;
        s.setOpener(openerFor(st));
        CHECK(s.open(""));
        CHECK(s.setSampleRateHz(55556));
        {
            std::lock_guard<std::mutex> lk(st->mu);
            st->running = true;  // the stand-in ignores a rate change while running, as the document says
        }
        CHECK(!s.start() && std::string(s.lastError()).find("did not accept that sample rate") != std::string::npos);
        s.closeDevice();
    }
}

void testOverloadAndOtherItems() {
    auto st = std::make_shared<FakeState>();
    SdriqSource s;
    s.setOpener(openerFor(st));
    CHECK(s.open(""));
    CHECK(s.start());
    {
        std::lock_guard<std::mutex> lk(st->mu);
        st->queue(Bytes{0x05, 0x20, 0x05, 0x00, 0x20});  // unsolicited status: A/D overload
        st->queue(Bytes{0x05, 0x20, 0x05, 0x00, 0x20});
        st->queue(Bytes{0x05, 0x20, 0x05, 0x00, 0x0C});  // busy: not an overload
        Bytes d1{0x06, 0xA0, 0x01, 0x02, 0x03, 0x04};    // a data item 1 block (6 bytes): counted, not used
        st->queue(d1);
        Bytes wrong{0x00, 0x80};                         // an 8194-byte block cannot be short: build a 100-byte data-0
        wrong = Bytes{0x64, 0x80};
        wrong.resize(100, 0);
        st->queue(wrong);
        st->cv.notify_all();
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while ((s.overloadReports() < 2 || s.otherBlocks() < 2) && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(s.overloadReports() == 2);
    CHECK(s.otherBlocks() == 2);
    CHECK(!s.faulted());
    s.closeDevice();
}

void testCycles() {
    auto st = std::make_shared<FakeState>();
    SdriqSource s;
    s.setOpener(openerFor(st));
    CHECK(s.open(""));
    for (int i = 0; i < 50; ++i) {
        CHECK(s.start());
        if (i % 10 == 0) { CHECK(readSome(s, 2048, 4000).size() >= 2048); }
        s.stop();
        CHECK(!s.running());
    }
    s.closeDevice();
    for (int i = 0; i < 50; ++i) {
        auto st2 = std::make_shared<FakeState>();
        SdriqSource s2;
        s2.setOpener(openerFor(st2));
        CHECK(s2.open(""));
        s2.closeDevice();
        CHECK(!s2.isOpen());
    }
    CHECK(NetSampleSource::readersAbandoned() == 0);
}

}  // namespace

int main() {
    testLiterals();
    testDecodeBlock();
    testCStringAndRange();
    testFuzzBytes();
    testOpenAndIdentify();
    testOpenRefusals();
    testTuneRateGain();
    testStreamingTone();
    testFaults();
    testOverloadAndOtherItems();
    testCycles();
    return testSummary("test_sdriq_source");
}
