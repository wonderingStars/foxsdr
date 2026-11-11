// test_extio.cpp - the ExtIO helper process (row R8): source/extio_protocol.hpp, source/extio_client.{hpp,cpp}, tools/extio_helper, and the
// stand-in drivers of tests/fixtures/extio_standin.cpp. Arguments: the helper executable and the directory of the stand-in DLLs.
//
//   [A] the wire: frames, a reader fed one byte at a time, violations, every sample format by hand, StartHW's sizes, the payload decoders; fuzz.
//   [B] the DLL's bitness from its PE header.
//   [C] real helper processes against real (stand-in) DLLs: off until chosen; a tone (spectrum, rate, retune, range answers); status callbacks;
//       a crash on the driver's thread and inside a call; a hang in StartHW and in SetHWLO; a flood; odd, huge, negative and zero block sizes;
//       a wild callback pointer; a driver without entry points, one that refuses InitHW, one for the sound card, an unknown sample format;
//       silence; non-finite floats; a 32-bit DLL with no 32-bit helper; not a DLL; no helper; a helper that cannot start; the engine-facing
//       ExtioSource; retuning while streaming.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "source/extio_client.hpp"
#include "source/extio_protocol.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace ex = cascade::source::extio;
using cascade::source::ExtioClient;
using cascade::source::ExtioOptions;
using cascade::source::ExtioSource;
using Clock = std::chrono::steady_clock;

namespace {

std::string g_helper;
fs::path g_dir;

std::string path(const char* name) { return (g_dir / (std::string("ExtIO_") + name + ".dll")).string(); }

ExtioOptions opts(const char* standin, int cmdMs = 5000, int initMs = 15000) {
    ExtioOptions o;
    o.dllPath = path(standin);
    o.helperPath64 = g_helper;
    o.commandTimeoutMs = cmdMs;
    o.initTimeoutMs = initMs;
    return o;
}

// ---- [A] --------------------------------------------------------------------------------------------------------------------------------
void testWire() {
    {   // a frame, whole and fed one byte at a time
        std::vector<std::uint8_t> payload{1, 2, 3, 4, 5};
        const auto f = ex::frame(ex::Msg::Start, payload);
        CHECK(f.size() == ex::kHeaderBytes + 5);
        CHECK(f[0] == 'F' && f[1] == 'X' && f[2] == 'I' && f[3] == 'O');
        int got = 0;
        ex::FrameReader r;
        for (std::uint8_t b : f) {
            CHECK(r.feed(&b, 1, [&](ex::Msg t, const std::uint8_t* p, std::size_t n) {
                ++got;
                CHECK(t == ex::Msg::Start && n == 5 && p[4] == 5);
            }));
        }
        CHECK(got == 1 && r.buffered() == 0);
    }
    {   // two frames in one read, and an empty payload
        auto a = ex::frame(ex::Msg::Open);
        const auto b = ex::frame(ex::Msg::Stop);
        a.insert(a.end(), b.begin(), b.end());
        int got = 0;
        ex::FrameReader r;
        CHECK(r.feed(a.data(), a.size(), [&](ex::Msg, const std::uint8_t*, std::size_t n) { ++got; CHECK(n == 0); }));
        CHECK(got == 2);
    }
    {   // violations are permanent
        auto bad = ex::frame(ex::Msg::Open);
        bad[0] = 'X';
        ex::FrameReader r;
        CHECK(!r.feed(bad.data(), bad.size(), [](ex::Msg, const std::uint8_t*, std::size_t) {}));
        CHECK(!r.error().empty());
        const auto good = ex::frame(ex::Msg::Open);
        CHECK(!r.feed(good.data(), good.size(), [](ex::Msg, const std::uint8_t*, std::size_t) {}));   // stays failed
        auto big = ex::frame(ex::Msg::Data);
        big[8] = 0xFF; big[9] = 0xFF; big[10] = 0xFF; big[11] = 0x7F;   // 2 GiB
        ex::FrameReader r2;
        CHECK(!r2.feed(big.data(), big.size(), [](ex::Msg, const std::uint8_t*, std::size_t) {}));
        auto rsv = ex::frame(ex::Msg::Open);
        rsv[6] = 1;
        ex::FrameReader r3;
        CHECK(!r3.feed(rsv.data(), rsv.size(), [](ex::Msg, const std::uint8_t*, std::size_t) {}));
        // the largest allowed payload is accepted (and waits for the rest)
        auto edge = ex::frame(ex::Msg::Data);
        const std::uint32_t lim = ex::kMaxPayload;
        std::memcpy(&edge[8], &lim, 4);
        ex::FrameReader r4;
        CHECK(r4.feed(edge.data(), edge.size(), [](ex::Msg, const std::uint8_t*, std::size_t) {}));
    }
    {   // StartHW sizes, by the specification's rule
        using V = ex::StartVerdict;
        CHECK(ex::judgeStartPairs(512) == V::Ok && ex::judgeStartPairs(1024) == V::Ok && ex::judgeStartPairs(512 * 64) == V::Ok);
        CHECK(ex::judgeStartPairs(0) == V::TooFew && ex::judgeStartPairs(511) == V::TooFew);
        CHECK(ex::judgeStartPairs(-1) == V::Error && ex::judgeStartPairs(INT32_MIN) == V::Error);
        CHECK(ex::judgeStartPairs(700) == V::NotMultiple && ex::judgeStartPairs(513) == V::NotMultiple);
        CHECK(ex::judgeStartPairs(ex::kMaxPairsPerCallback) == V::Ok);
        CHECK(ex::judgeStartPairs(ex::kMaxPairsPerCallback + 512) == V::TooMany && ex::judgeStartPairs(0x7fffffff) == V::TooMany);
    }
    {   // every sample format, hand-computed
        float out[4];
        const std::uint8_t s16[] = {0x00, 0x40, 0x00, 0xC0, 0xFF, 0x7F, 0x00, 0x80};   // 16384, -16384, 32767, -32768
        CHECK(ex::convertSamples(3, s16, 2, out));
        CHECK_NEAR(out[0], 0.5, 1e-7); CHECK_NEAR(out[1], -0.5, 1e-7); CHECK_NEAR(out[2], 32767.0 / 32768.0, 1e-7); CHECK_NEAR(out[3], -1.0, 1e-7);
        const std::uint8_t s24[] = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80};   // 4194304, -4194304, 8388607, -8388608
        CHECK(ex::convertSamples(5, s24, 2, out));
        CHECK_NEAR(out[0], 0.5, 1e-7); CHECK_NEAR(out[1], -0.5, 1e-7); CHECK_NEAR(out[2], 8388607.0 / 8388608.0, 1e-7); CHECK_NEAR(out[3], -1.0, 1e-7);
        const std::uint8_t s32[] = {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x80};
        CHECK(ex::convertSamples(6, s32, 2, out));
        CHECK_NEAR(out[0], 0.5, 1e-7); CHECK_NEAR(out[1], -0.5, 1e-6); CHECK_NEAR(out[3], -1.0, 1e-7);
        const float f[] = {0.25f, -0.75f, std::nanf(""), INFINITY};
        std::uint8_t fb[16];
        std::memcpy(fb, f, 16);
        CHECK(ex::convertSamples(7, fb, 2, out));
        CHECK_NEAR(out[0], 0.25, 1e-7); CHECK_NEAR(out[1], -0.75, 1e-7); CHECK(out[2] == 0.0f); CHECK(out[3] == 0.0f);   // NaN and infinity both to 0
        CHECK(!ex::convertSamples(4, s16, 1, out) && !ex::convertSamples(0, s16, 1, out) && !ex::convertSamples(99, s16, 1, out));
        CHECK(ex::bytesPerValue(3) == 2 && ex::bytesPerValue(5) == 3 && ex::bytesPerValue(6) == 4 && ex::bytesPerValue(7) == 4 && ex::bytesPerValue(4) == 0);
    }
    {   // the payload decoders
        ex::InitReply r;
        r.result = 1; r.hwType = 3; r.exports = 7; r.win32Error = 0; r.name = "Radio"; r.model = "M1"; r.missing = "";
        const auto b = ex::encodeInitReply(r);
        ex::InitReply d;
        CHECK(ex::decodeInitReply(b.data(), b.size(), d) && d.result == 1 && d.hwType == 3 && d.exports == 7 && d.name == "Radio" && d.model == "M1");
        for (std::size_t cut = 0; cut < b.size(); ++cut) { ex::InitReply x; CHECK(!ex::decodeInitReply(b.data(), cut, x)); }   // every truncation refused
        auto extra = b;
        extra.push_back(0);
        ex::InitReply y;
        CHECK(!ex::decodeInitReply(extra.data(), extra.size(), y));   // trailing bytes refused
        const auto sr = ex::encodeReply(ex::Msg::Start, -5, 192000);
        ex::SimpleReply s;
        CHECK(ex::decodeSimpleReply(sr.data(), sr.size(), s) && s.cmd == ex::Msg::Start && s.result == -5 && s.extra == 192000);
        CHECK(!ex::decodeSimpleReply(sr.data(), sr.size() - 1, s));
        std::vector<std::complex<float>> iq(512, {0.5f, -0.5f});
        const auto dd = ex::encodeData(iq.data(), 512, 0.25f);
        std::uint32_t pairs = 0;
        float off = 0;
        const std::uint8_t* p = nullptr;
        CHECK(ex::decodeData(dd.data(), dd.size(), pairs, off, p) && pairs == 512 && off == 0.25f);
        CHECK(!ex::decodeData(dd.data(), dd.size() - 1, pairs, off, p));
        auto lie = dd;
        lie[0] = 0xFF; lie[1] = 0xFF; lie[2] = 0xFF; lie[3] = 0xFF;   // a count that promises 34 GB
        CHECK(!ex::decodeData(lie.data(), lie.size(), pairs, off, p));
        ex::StatusEvent ev;
        const auto st = ex::encodeStatus(100, 96000);
        CHECK(ex::decodeStatus(st.data(), st.size(), ev) && ev.status == 100 && ev.value == 96000 && !ex::decodeStatus(st.data(), 7, ev));
    }
    {   // FUZZ: random bytes and mutated good frames through the reader and every decoder; counts must stay inside their buffers
        std::mt19937 rng(2468);
        std::vector<std::complex<float>> iq(512, {0.1f, 0.2f});
        std::vector<std::vector<std::uint8_t>> seeds = {ex::frame(ex::Msg::Data, ex::encodeData(iq.data(), 512, 0.0f)),
                                                        ex::frame(ex::Msg::Reply, ex::encodeInitReply(ex::InitReply{1, 3, 1, 0, "a", "b", "c"})),
                                                        ex::frame(ex::Msg::Status, ex::encodeStatus(100, 5)), ex::frame(ex::Msg::Reply, ex::encodeReply(ex::Msg::Start, 512, 1))};
        for (int i = 0; i < 30000; ++i) {
            std::vector<std::uint8_t> b = seeds[rng() % seeds.size()];
            if (i % 5 == 0) { b.resize(rng() % 300); for (auto& x : b) { x = static_cast<std::uint8_t>(rng()); } }
            else { for (int f = 0, n = 1 + static_cast<int>(rng() % 6); f < n; ++f) { b[rng() % b.size()] = static_cast<std::uint8_t>(rng()); } }
            ex::FrameReader r;
            std::size_t at = 0;
            bool ok = true;
            while (at < b.size() && ok) {
                const std::size_t piece = 1 + rng() % 40;
                const std::size_t take = std::min(piece, b.size() - at);
                ok = r.feed(b.data() + at, take, [](ex::Msg, const std::uint8_t* p, std::size_t n) {
                    std::uint32_t pairs; float off; const std::uint8_t* iq2;
                    if (ex::decodeData(p, n, pairs, off, iq2)) { volatile std::uint8_t sink = iq2[pairs ? pairs * 8 - 1 : 0]; (void)sink; }
                    ex::InitReply ir; ex::decodeInitReply(p, n, ir);
                    ex::SimpleReply sr2; ex::decodeSimpleReply(p, n, sr2);
                    ex::StatusEvent se; ex::decodeStatus(p, n, se);
                });
                at += take;
            }
        }
        CHECK(true);
    }
}

// ---- [B] --------------------------------------------------------------------------------------------------------------------------------
std::string writeFile(const char* name, const std::vector<std::uint8_t>& bytes) {
    const fs::path p = fs::temp_directory_path() / (std::string("foxsdr-test-extio-") + name);
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return p.string();
}
std::vector<std::uint8_t> pe(std::uint16_t machine) {
    std::vector<std::uint8_t> b(0x200, 0);
    b[0] = 'M'; b[1] = 'Z'; b[0x3C] = 0x80;
    std::memcpy(&b[0x80], "PE\0\0", 4);
    b[0x84] = static_cast<std::uint8_t>(machine); b[0x85] = static_cast<std::uint8_t>(machine >> 8);
    return b;
}

void testMachine() {
    CHECK(ExtioClient::peMachine(writeFile("m64.dll", pe(0x8664))) == 0x8664);
    CHECK(ExtioClient::peMachine(writeFile("m32.dll", pe(0x14C))) == 0x14C);
    CHECK(ExtioClient::peMachine(writeFile("marm.dll", pe(0xAA64))) == 0xAA64);
    CHECK(ExtioClient::peMachine(writeFile("text.dll", std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'})) == 0);
    auto badOff = pe(0x8664);
    badOff[0x3C] = 0xFF; badOff[0x3D] = 0xFF;   // the PE header offset points off the end
    CHECK(ExtioClient::peMachine(writeFile("badoff.dll", badOff)) == 0);
    auto noSig = pe(0x8664);
    noSig[0x80] = 'X';
    CHECK(ExtioClient::peMachine(writeFile("nosig.dll", noSig)) == 0);
    CHECK(ExtioClient::peMachine((fs::temp_directory_path() / "foxsdr-test-extio-none.dll").string()) == 0);
    CHECK(ExtioClient::peMachine("") == 0);
    std::mt19937 rng(5);
    for (int i = 0; i < 300; ++i) {
        auto b = pe(0x8664);
        for (int f = 0; f < 6; ++f) { b[rng() % b.size()] = static_cast<std::uint8_t>(rng()); }
        b.resize(rng() % b.size());
        (void)ExtioClient::peMachine(writeFile("fz.dll", b));
    }
    CHECK(true);
}

// ---- [C] --------------------------------------------------------------------------------------------------------------------------------
// Pairs from the client until `want` have arrived or `ms` have passed (or it faulted).
std::vector<std::complex<float>> collect(ExtioClient& c, std::size_t want, int ms) {
    std::vector<std::complex<float>> all;
    std::vector<std::complex<float>> buf(4096);
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (all.size() < want && Clock::now() < end && !c.faulted()) {
        const std::size_t n = c.read(buf.data(), buf.size());
        all.insert(all.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
        if (n == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    }
    return all;
}
// Wait until the client has faulted (reading is what notices silence and floods) or `ms` pass.
bool waitFault(ExtioClient& c, int ms) {
    std::vector<std::complex<float>> buf(4096);
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        c.read(buf.data(), buf.size());
        if (c.faulted()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return c.faulted();
}
// Goertzel power at `hz` for complex input sampled at `rate`.
double tonePower(const std::vector<std::complex<float>>& x, double hz, double rate) {
    std::complex<double> acc = 0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double ph = -2.0 * 3.14159265358979323846 * hz * static_cast<double>(n) / rate;
        acc += std::complex<double>(x[n]) * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return std::norm(acc) / (static_cast<double>(x.size()) * static_cast<double>(x.size()));
}
bool hasFact(const ExtioClient& c, const char* code) {
    for (const auto& f : c.facts()) { if (f.code == code) { return true; } }
    return false;
}

void testOffUntilChosen() {
    ExtioOptions o;
    o.helperPath64 = g_helper;
    ExtioClient c(o);
    CHECK(!c.open());
    CHECK(c.fact().code == ex::kFactNotChosen);
    CHECK(!c.helperAlive());
    CHECK(!c.start(7.0e6));
    ExtioSource s(o);
    CHECK(!s.start() && std::string(s.lastError()).find("no ExtIO driver") != std::string::npos);
    CHECK(std::string(ex::kExtioWarning).find("YOUR rights") != std::string::npos);
}

void testTone() {
    ExtioClient c(opts("tone"));
    CHECK(c.open());
    CHECK(c.isOpen() && c.helperAlive());
    CHECK(c.hwType() == 3 && c.driverName() == "StandIn Radio");
    CHECK(c.start(7050000.0));
    CHECK(c.running());
    CHECK_NEAR(c.sampleRateHz(), 192000.0, 0.5);
    const auto t0 = Clock::now();
    const auto x = collect(c, 96000, 15000);
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK(x.size() >= 96000);
    if (x.size() >= 19200) {
        std::vector<std::complex<float>> win(x.begin() + 4800, x.begin() + 4800 + 19200);
        const double rate = 192000.0 * (1.0 + 40e-6);
        const double atTone = tonePower(win, 12000.0, rate);
        const double mirror = tonePower(win, -12000.0, rate);
        const double off = tonePower(win, 31000.0, rate);
        CHECK_NEAR(std::sqrt(atTone), 0.5, 0.03);      // amplitude 0.5 survives the 16-bit path
        CHECK(mirror < atTone * 1e-3 && off < atTone * 1e-3);   // the right sign, and nothing else
        for (const auto& s : win) { CHECK(std::abs(s.real()) <= 1.0f && std::abs(s.imag()) <= 1.0f); }
    }
    CHECK(secs > 0.2);   // paced by the driver, not instant
    CHECK(c.stats().pairsReceived >= 96000 && c.stats().pairsOverrun == 0);
    // range answers from SetHWLO
    CHECK(c.setCenterHz(14100000.0) == 0);
    CHECK(c.setCenterHz(50000.0) == -100000);          // below the minimum: the minimum, negated
    CHECK(c.setCenterHz(2.1e9 - 1.0) == 2000000000);   // above the maximum: the maximum
    CHECK(!c.faulted());
    c.stop();
    CHECK(!c.running());
    CHECK(c.read(nullptr, 0) == 0);
    c.close();
    CHECK(!c.helperAlive());
    CHECK(c.facts().empty());
}

void testStatus() {
    ExtioClient c(opts("status"));
    CHECK(c.start(7050000.0));
    CHECK_NEAR(c.sampleRateHz(), 192000.0, 0.5);
    collect(c, 512 * 40, 10000);   // 40 blocks: past both status callbacks
    const auto ev = c.takeStatusEvents();
    bool sr = false, lo = false;
    for (const auto& e : ev) {
        if (e.status == 100 && e.value == 96000) { sr = true; }
        if (e.status == 101 && e.value == 14100000) { lo = true; }
    }
    CHECK(sr && lo);
    CHECK_NEAR(c.sampleRateHz(), 96000.0, 0.5);   // status 100: the helper asked GetHWSR and told us
    CHECK(c.takeStatusEvents().empty());
    c.close();
}

void testCrashes() {
    {   // a crash on the driver's own thread, a few blocks in
        ExtioClient c(opts("crashcb"));
        CHECK(c.start(7.0e6));
        CHECK(waitFault(c, 10000));
        std::printf("crashcb fact: %s | %s%c", c.fact().code.c_str(), c.fact().text.c_str(), '\n');
        CHECK(c.fact().code == ex::kFactHelperExited);
        CHECK(!c.helperAlive());
        CHECK(c.helperExitCode() == 0xC0000005u);   // the access violation, named by the exit code
        CHECK(c.fact().text.find("0xC0000005") != std::string::npos);
        CHECK(!c.running() && c.read(nullptr, 0) == 0);
        c.close();   // after a crash: does not hang or throw
    }
    {   // a crash inside a call
        ExtioClient c(opts("crashlo"));
        CHECK(c.start(7.0e6));
        CHECK(c.setCenterHz(7.1e6) == INT32_MIN);
        CHECK(c.fact().code == ex::kFactHelperExited && c.faulted());
        CHECK(!c.start(7.0e6));
    }
}

void testHangs() {
    {   // StartHW never returns
        ExtioClient c(opts("hangstart", 5000, 2500));
        CHECK(c.open());
        const auto t0 = Clock::now();
        CHECK(!c.start(7.0e6));
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        CHECK(secs > 2.0 && secs < 8.0);
        CHECK(c.fact().code == ex::kFactTimeout);
        CHECK(c.fact().text.find("start") != std::string::npos);
        CHECK(!c.helperAlive());   // the hung helper was killed
    }
    {   // SetHWLO never returns
        ExtioClient c(opts("hanglo", 800));
        CHECK(c.start(7.0e6));
        CHECK(c.setCenterHz(7.1e6) == INT32_MIN);
        CHECK(c.fact().code == ex::kFactTimeout && !c.helperAlive());
        c.close();
    }
}

void testFlood() {
    ExtioClient c(opts("flood"));
    CHECK(c.start(7.0e6));
    CHECK_NEAR(c.sampleRateHz(), 48000.0, 0.5);
    CHECK(waitFault(c, 15000));
    CHECK(c.fact().code == ex::kFactFlood);
    const auto st = c.stats();
    CHECK(st.pairsReceived > 48000);   // far more than the 48 kS/s it claimed in the second it was watched
    CHECK(!c.helperAlive() || c.faulted());
    c.close();
    CHECK(!c.helperAlive());
}

void testSizes() {
    struct Row { const char* name; const char* code; };
    const Row rows[] = {{"sizeodd", ex::kFactBadSize}, {"sizehuge", ex::kFactBadSize}, {"sizezero", ex::kFactBadSize}, {"sizeneg", ex::kFactDllRefused}};
    for (const Row& r : rows) {
        ExtioClient c(opts(r.name));
        CHECK(c.open());
        CHECK(!c.start(7.0e6));
        CHECK(c.fact().code == r.code);
        CHECK(!c.running());
        c.close();
    }
    ExtioClient odd(opts("sizeodd"));
    CHECK(!odd.start(7.0e6));
    CHECK(odd.fact().text.find("700") != std::string::npos);   // the number it gave is in the sentence
}

void testBadPointer() {
    ExtioClient c(opts("badptr"));
    CHECK(c.start(7.0e6));
    const auto x = collect(c, 512 * 8, 10000);
    CHECK(x.size() >= 512 * 8);   // the good blocks after the ten wild ones arrive
    CHECK(!c.faulted());
    // the helper reports its count of unreadable callbacks once a second
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (c.stats().helperBadCallbacks == 0 && Clock::now() < end) { collect(c, 512, 200); }
    CHECK(c.stats().helperBadCallbacks >= 10);
    CHECK(c.helperAlive());
    c.close();
}

void testRefusals() {
    {
        ExtioClient c(opts("noexports"));
        CHECK(!c.open());
        CHECK(c.fact().code == ex::kFactDllRefused && c.fact().text.find("StartHW") != std::string::npos && c.fact().text.find("SetCallback") != std::string::npos);
        CHECK(!c.helperAlive());
    }
    {
        ExtioClient c(opts("initfalse"));
        CHECK(!c.open());
        CHECK(c.fact().code == ex::kFactDllRefused && !c.helperAlive());
    }
    {
        ExtioClient c(opts("soundcard"));
        CHECK(!c.open());
        CHECK(c.fact().code == ex::kFactUnsupportedType && c.fact().text.find("sound card") != std::string::npos && !c.helperAlive());
    }
    {
        ExtioClient c(opts("badtype"));
        CHECK(!c.open());
        CHECK(c.fact().code == ex::kFactUnsupportedType && !c.helperAlive());
    }
    {   // silence after a good start
        ExtioOptions o = opts("silent");
        o.silenceMs = 700;
        ExtioClient c(o);
        CHECK(c.start(7.0e6));
        CHECK(waitFault(c, 8000));
        CHECK(c.fact().code == ex::kFactSilent);
        c.close();
        CHECK(!c.helperAlive());
    }
}

void testFloatNonFinite() {
    ExtioClient c(opts("floatbad"));
    CHECK(c.start(7.0e6));
    CHECK(c.hwType() == 7);
    const auto x = collect(c, 512 * 6, 10000);
    CHECK(x.size() >= 512 * 6);
    bool allFinite = true;
    for (const auto& s : x) {
        if (!std::isfinite(s.real()) || !std::isfinite(s.imag()) || std::abs(s.real()) > 1.0f || std::abs(s.imag()) > 1.0f) { allFinite = false; }
    }
    CHECK(allFinite);
    // the NaN at pair 5 (index 10) arrived as exactly zero, and the out-of-range 5.0 at index 13 as exactly 1
    CHECK(x.size() > 7 && x[5].real() == 0.0f && x[5].imag() == 0.0f);
    CHECK(x.size() > 7 && x[6].imag() == 1.0f);
    c.close();
}

void testBeforeTheHelper() {
    {   // a 32-bit DLL and no 32-bit helper installed
        ExtioOptions o;
        o.dllPath = writeFile("fake32.dll", pe(0x14C));
        o.helperPath64 = g_helper;
        ExtioClient c(o);
        CHECK(!c.open() && c.fact().code == ex::kFactHelperMissing && c.fact().text.find("32-bit") != std::string::npos);
        CHECK(!c.helperAlive());
    }
    {   // not a DLL at all
        ExtioOptions o;
        o.dllPath = writeFile("notdll.dll", std::vector<std::uint8_t>{'n', 'o', 'p', 'e'});
        o.helperPath64 = g_helper;
        ExtioClient c(o);
        CHECK(!c.open() && c.fact().code == ex::kFactDllRefused);
        ExtioOptions o2 = o;
        o2.dllPath = (fs::temp_directory_path() / "foxsdr-test-extio-absent.dll").string();
        ExtioClient c2(o2);
        CHECK(!c2.open() && c2.fact().code == ex::kFactDllRefused);
    }
    {   // the helper is missing, or is not a program
        ExtioOptions o = opts("tone");
        o.helperPath64 = (fs::temp_directory_path() / "foxsdr-test-extio-nohelper.exe").string();
        ExtioClient c(o);
        CHECK(!c.open() && c.fact().code == ex::kFactHelperMissing);
        ExtioOptions o2 = opts("tone");
        o2.helperPath64 = writeFile("notprogram.exe", std::vector<std::uint8_t>{'n', 'o'});
        ExtioClient c2(o2);
        CHECK(!c2.open() && c2.fact().code == ex::kFactHelperStart);
    }
    {   // no fact carries a path
        ExtioOptions o = opts("noexports");
        ExtioClient c(o);
        c.open();
        for (const auto& f : c.facts()) { CHECK(f.text.find(g_dir.string()) == std::string::npos && f.text.find("ExtIO_noexports") == std::string::npos); }
    }
}

void testSource() {
    ExtioSource s(opts("tone"));
    CHECK(s.selfPaced());
    CHECK(s.setCenterFrequencyHz(7050000.0) && s.centerFrequencyHz() == 7050000.0);   // before start: remembered
    CHECK(s.start());
    CHECK(s.running() && std::string(s.name()) == "ExtIO: StandIn Radio");
    CHECK_NEAR(s.sampleRateHz(), 192000.0, 0.5);
    CHECK(!s.setSampleRateHz(48000.0) && std::string(s.lastError()).find("driver's own window") != std::string::npos);
    CHECK(s.setCenterFrequencyHz(14100000.0));
    CHECK(!s.setCenterFrequencyHz(10000.0) && std::string(s.lastError()).find("lowest") != std::string::npos);
    CHECK(!s.setCenterFrequencyHz(2.1e9 - 1) && std::string(s.lastError()).find("highest") != std::string::npos);
    std::vector<std::complex<float>> buf(2048);
    std::size_t got = 0;
    for (int i = 0; i < 400 && got < 2048; ++i) { got += s.read(buf.data(), buf.size()); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK(got >= 2048);
    CHECK(!s.faulted());
    s.stop();
    CHECK(!s.running());
    // a second start after stop works (the driver restarts)
    CHECK(s.start());
    s.stop();
    // a faulted source says why
    ExtioSource bad(opts("crashlo"));
    CHECK(bad.start());
    CHECK(!bad.setCenterFrequencyHz(7.1e6));
    CHECK(bad.faulted() && !std::string(bad.lastError()).empty());
}

void testRetuneWhileStreaming() {
    ExtioClient c(opts("tone"));
    CHECK(c.start(7.0e6));
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> got{0};
    std::thread reader([&] {
        std::vector<std::complex<float>> buf(1024);
        while (!stop.load()) {
            const std::size_t n = c.read(buf.data(), buf.size());
            got += n;
            if (n == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        }
    });
    int ok = 0;
    for (int i = 0; i < 60; ++i) { ok += c.setCenterHz(7.0e6 + i * 1000.0) == 0 ? 1 : 0; }
    for (int i = 0; i < 500 && got.load() == 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    stop = true;
    reader.join();
    CHECK(ok == 60);
    CHECK(got.load() > 0 && !c.faulted());
    c.close();
}

// OPTIONAL (arguments 3 and 4): a 32-bit helper and a 32-bit stand-in tone driver built without a .DEF file, so it exports the DECORATED stdcall names
// (_InitHW@12 ...) the specification's note on Visual C++ describes. Skipped (and said so) when they are not given.
void testThirtyTwoBit(const std::string& helper32, const std::string& dll32) {
    ExtioOptions o;
    o.dllPath = dll32;
    o.helperPath64 = g_helper;
    o.helperPath32 = helper32;
    CHECK(ExtioClient::peMachine(dll32) == 0x14C);
    ExtioClient c(o);
    CHECK(c.open());
    CHECK(c.start(7050000.0));
    const auto x = collect(c, 24000, 15000);
    CHECK(x.size() >= 24000);
    if (x.size() >= 24000) {
        std::vector<std::complex<float>> win(x.begin() + 4800, x.begin() + 4800 + 19200);
        const double rate = 192000.0 * (1.0 + 40e-6);
        CHECK_NEAR(std::sqrt(tonePower(win, 12000.0, rate)), 0.5, 0.03);
    }
    CHECK(c.setCenterHz(14100000.0) == 0);
    c.close();
    CHECK(!c.helperAlive());
    CHECK(c.facts().empty());
}

}  // namespace

int main(int argc, char** argv) {
    testWire();
    testMachine();
#if defined(_WIN32)
    if (argc < 3) {
        std::printf("usage: test_extio <extio helper exe> <stand-in directory>\n");
        return 2;
    }
    g_helper = argv[1];
    g_dir = argv[2];
    testOffUntilChosen();
    testTone();
    testStatus();
    testCrashes();
    testHangs();
    testFlood();
    testSizes();
    testBadPointer();
    testRefusals();
    testFloatNonFinite();
    testBeforeTheHelper();
    testSource();
    testRetuneWhileStreaming();
    if (argc >= 5) { testThirtyTwoBit(argv[3], argv[4]); }
    else { std::printf("32-bit helper and driver not given: the 32-bit run was SKIPPED%c", '\n'); }
#else
    (void)argc; (void)argv;
    ExtioOptions o;
    o.dllPath = "x";
    ExtioClient c(o);
    CHECK(!c.open() && c.fact().code == ex::kFactHelperMissing);
#endif
    return testSummary("test_extio");
}
