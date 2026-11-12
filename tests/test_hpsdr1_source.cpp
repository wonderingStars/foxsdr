// test_hpsdr1_source.cpp - the openHPSDR protocol-1 frame layer and Hpsdr1Source (Hermes-class radios and
// the Red Pitaya), proven against a STAND-IN radio on loopback UDP.
//
// WHAT THIS CAN AND CANNOT PROVE. The frame layout is from "HPSDR - USB Data Protocol" V1.60
// (src/source/hpsdr1_frames.hpp has the provenance). The stand-in writes its frames from that same
// document with its OWN index arithmetic (explicit loops, literal addresses) rather than the client's
// helpers; the literal-byte tests print the document's own example values. It cannot show the document
// right or complete. UNPROVEN: any real HPSDR radio or Red Pitaya; whether a board needs the command
// frames repeated at a different rate; the 24-bit full scale.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/hpsdr1_frames.hpp"
#include "source/hpsdr1_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::Hpsdr1Profile;
using cascade::source::Hpsdr1Source;
using cascade::source::NetSampleSource;
namespace h1 = cascade::source::hpsdr1;
namespace hp = cascade::source::hpsdr;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool pollRead(Sock s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{};
    tv.tv_usec = ms * 1000;
#if defined(_WIN32)
    return ::select(0, &rf, nullptr, nullptr, &tv) > 0;
#else
    return ::select(s + 1, &rf, nullptr, nullptr, &tv) > 0;
#endif
}

// --- the stand-in radio -------------------------------------------------------------------

enum class Quirk { None, DropEvery8, Silent, OverloadEvery3, BadFrames, NoSyncInSecondFrame };

class Radio {
public:
    explicit Radio(Quirk q = Quirk::None, std::uint8_t board = 0x01) : quirk_(q), board_(board) {}
    ~Radio() { stop(); }
    bool start() {
        if (!sock_.open()) { return false; }
        run_.store(true);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        run_.store(false);
        if (th_.joinable()) { th_.join(); }
        stopStream();
    }
    std::string args(const std::string& extra = "") const {
        return "hpsdr1=127.0.0.1:" + std::to_string(sock_.port()) + (extra.empty() ? "" : "," + extra);
    }
    // What the client has told the radio (decoded here with this file's own arithmetic).
    struct Seen {
        int speedBits = -1;
        int receiversMinus1 = -1;
        std::uint32_t rx1 = 0, tx = 0;
        int attenRaw = -1;
        bool duplex = false;
    };
    Seen seen() {
        std::lock_guard<std::mutex> lk(m_);
        return seen_;
    }
    std::atomic<int> starts{0}, stops{0}, discoveries{0}, commandPackets{0}, badCommands{0}, moxSeen{0}, audioNonZero{0}, driveNonZero{0};
    std::atomic<int> commandsBeforeFirstStart{0};
    std::atomic<int> sentPackets{0};
    std::atomic<double> toneHz{9000.0}, ppm{300.0}, noise{0.01};
    std::atomic<bool> streaming{false};

private:
    Bytes reply(std::uint8_t status) const {
        Bytes r(60, 0);
        r[0] = 0xEF; r[1] = 0xFE; r[2] = status;
        const std::uint8_t mac[6] = {0x00, 0x1c, 0xc0, 0xa2, 0x13, 0x77};
        for (int i = 0; i < 6; ++i) { r[3 + i] = mac[i]; }
        r[9] = 32;
        r[10] = board_;
        return r;
    }
    void loop() {
        Bytes d;
        std::uint16_t from = 0;
        while (run_.load()) {
            if (!pollRead(sock_.sock(), 5)) { continue; }
            if (!sock_.recvFrom(run_, d, from)) { continue; }
            if (d.size() == 63 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x02) {
                ++discoveries;
                if (quirk_ == Quirk::Silent && false) { continue; }
                const Bytes j = reply(streaming.load() ? 3 : 2);
                sock_.sendTo(from, j.data(), j.size());
            } else if (d.size() == 64 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x04) {
                if (d[3] & 1) { ++starts; startStream(from); } else { ++stops; stopStream(); }
            } else if (d.size() == 1032 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x01 && d[3] == 0x02) {
                ++commandPackets;
                if (starts.load() == 0) { ++commandsBeforeFirstStart; }
                for (int fi = 0; fi < 2; ++fi) { parseCommandFrame(&d[8 + 512 * fi]); }
            } else {
                ++badCommands;
            }
        }
    }
    void parseCommandFrame(const std::uint8_t* f) {
        if (f[0] != 0x7F || f[1] != 0x7F || f[2] != 0x7F) { ++badCommands; return; }
        if (f[3] & 1) { ++moxSeen; }
        for (int i = 8; i < 512; ++i) { if (f[i] != 0) { ++audioNonZero; break; } }
        const int addr = f[3] >> 1;
        const std::uint32_t word = (static_cast<std::uint32_t>(f[4]) << 24) | (static_cast<std::uint32_t>(f[5]) << 16) | (static_cast<std::uint32_t>(f[6]) << 8) | f[7];
        std::lock_guard<std::mutex> lk(m_);
        if (addr == 0) {
            seen_.speedBits = f[4] & 3;
            seen_.receiversMinus1 = (f[7] >> 3) & 7;
            seen_.duplex = (f[7] & 4) != 0;
        } else if (addr == 1) {
            seen_.tx = word;
        } else if (addr == 2) {
            seen_.rx1 = word;
        } else if (addr == 9) {
            if (f[4] != 0) { ++driveNonZero; }
        } else if (addr == 10) {
            seen_.attenRaw = f[7] & 0x3F;
        }
    }
    void startStream(std::uint16_t to) {
        stopStream();
        streamRun_.store(true);
        streaming.store(true);
        stream_ = std::thread([this, to] {
            std::uint32_t seq = 0;
            double phase = 0.0;
            int count = 0, statusRot = 0;
            Rng rng(4242);
            while (streamRun_.load()) {
                for (int burst = 0; burst < 6 && streamRun_.load(); ++burst) {
                    Seen sn = seen();
                    const int rate = sn.speedBits == 3 ? 384000 : sn.speedBits == 2 ? 192000 : sn.speedBits == 1 ? 96000 : 48000;
                    const double fs = rate * (1.0 + ppm.load() * 1e-6);
                    const double dphi = 2.0 * kPi * toneHz.load() / fs;
                    Bytes p = {0xEF, 0xFE, 0x01, 0x06, static_cast<std::uint8_t>(seq >> 24), static_cast<std::uint8_t>(seq >> 16),
                               static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq)};
                    p.resize(1032, 0);
                    for (int fi = 0; fi < 2; ++fi) {
                        std::uint8_t* f = &p[8 + 512 * fi];
                        f[0] = f[1] = f[2] = 0x7F;
                        const int addrs[4] = {0, 1, 4, 2};
                        const int a = addrs[statusRot++ & 3];
                        f[3] = static_cast<std::uint8_t>(a << 3);
                        if (a == 0) {
                            const bool ov = quirk_ == Quirk::OverloadEvery3 && (count + fi) % 3 == 0;
                            f[4] = ov ? 1 : 0;
                            f[7] = 0x2A;  // software version
                        }
                        // one receiver: 63 sets of I(3) Q(3) mic(2), big-endian 24-bit
                        for (int k = 0; k < 63; ++k) {
                            std::uint8_t* b = f + 8 + 8 * k;
                            auto nz = [&]() { return (static_cast<double>(rng.next() & 0xFFFF) / 65535.0 - 0.5) * 3.4 * noise.load(); };
                            const double re = 0.5 * std::cos(phase) + nz();
                            const double im = 0.5 * std::sin(phase) + nz();
                            phase += dphi;
                            if (phase > 2 * kPi) { phase -= 2 * kPi; }
                            const std::int32_t I = static_cast<std::int32_t>(std::lround(re * 8388607.0)), Q = static_cast<std::int32_t>(std::lround(im * 8388607.0));
                            b[0] = static_cast<std::uint8_t>(I >> 16); b[1] = static_cast<std::uint8_t>(I >> 8); b[2] = static_cast<std::uint8_t>(I);
                            b[3] = static_cast<std::uint8_t>(Q >> 16); b[4] = static_cast<std::uint8_t>(Q >> 8); b[5] = static_cast<std::uint8_t>(Q);
                            b[6] = static_cast<std::uint8_t>(rng.next());  // a microphone sample that must never reach the output
                            b[7] = static_cast<std::uint8_t>(rng.next());
                        }
                    }
                    if (quirk_ == Quirk::BadFrames && count % 5 == 2) { p[8] = 0x00; }                    // first frame loses its sync
                    if (quirk_ == Quirk::NoSyncInSecondFrame && count % 5 == 2) { p[8 + 512 + 1] = 0x00; }
                    ++count;
                    if (quirk_ == Quirk::DropEvery8 && count % 8 == 0) { ++seq; continue; }
                    if (quirk_ != Quirk::Silent) { sock_.sendTo(to, p.data(), p.size()); ++sentPackets; }
                    ++seq;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    void stopStream() {
        streamRun_.store(false);
        std::lock_guard<std::mutex> l(sm_);
        if (stream_.joinable()) { stream_.join(); }
        streaming.store(false);
    }

    Quirk quirk_;
    std::uint8_t board_;
    UdpSock sock_;
    std::atomic<bool> run_{false}, streamRun_{false};
    std::thread th_, stream_;
    std::mutex m_, sm_;
    Seen seen_;
};

std::vector<std::complex<float>> readN(Hpsdr1Source& s, std::size_t n, int ms = 6000) {
    std::vector<std::complex<float>> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::vector<std::complex<float>> tmp(2048);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t g = s.read(tmp.data(), tmp.size());
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(g));
        if (s.faulted()) { break; }
    }
    return out;
}

double toneHz(const std::vector<std::complex<float>>& v, double fs) {
    std::complex<double> acc(0, 0);
    for (std::size_t i = 1; i < v.size(); ++i) { acc += std::complex<double>(v[i]) * std::conj(std::complex<double>(v[i - 1])); }
    return std::arg(acc) * fs / (2.0 * kPi);
}

// --- tests -----------------------------------------------------------------------------

void testDocumentTables() {
    // The document's padding table equals the left-over of 504 / (6n + 2).
    const int table[9] = {0, 0, 0, 4, 10, 24, 10, 20, 4};
    for (int n = 1; n <= 8; ++n) {
        CHECK(h1::kPadding[n] == table[n]);
        CHECK(504 - h1::setsPerFrame(n) * (6 * n + 2) == table[n]);
    }
    CHECK(h1::setsPerFrame(1) == 63 && h1::setsPerFrame(2) == 36 && h1::setsPerFrame(3) == 25 && h1::setsPerFrame(8) == 10);
    CHECK(h1::setsPerFrame(0) == 0 && h1::setsPerFrame(9) == 0 && h1::setsPerFrame(-1) == 0);
    CHECK(h1::speedCodeForRate(48000) == 0 && h1::speedCodeForRate(384000) == 3 && h1::speedCodeForRate(44100) == -1);
    CHECK(h1::rateForSpeedCode(2) == 192000 && h1::rateForSpeedCode(9) == 0);
}

void testCommandFrames() {
    h1::CommandState s;
    s.speedCode = 2;
    s.receivers = 1;
    s.rxHz[0] = 7050000;  // 0x006B9310
    s.attenDb = 20;
    s.attenEnable = true;
    // address 0: sync, C0 = 0 (MOX never), C1 = speed 10b, C3 = no preamp, C4 = 1 receiver + duplex bit
    {
        const auto f = h1::buildCommandFrame(s, 0);
        const std::uint8_t want[8] = {0x7F, 0x7F, 0x7F, 0x00, 0x02, 0x00, 0x00, 0x04};
        CHECK(std::memcmp(f.data(), want, 8) == 0);
        for (int i = 8; i < 512; ++i) { CHECK(f[i] == 0); }
    }
    // receiver 1 frequency: C0 = 0b0000010x = 0x04, C1..C4 = 00 6B 93 10
    {
        const auto f = h1::buildCommandFrame(s, 2);
        const std::uint8_t want[8] = {0x7F, 0x7F, 0x7F, 0x04, 0x00, 0x6B, 0x93, 0x10};
        CHECK(std::memcmp(f.data(), want, 8) == 0);
    }
    // transmitter frequency (address 1, C0 = 0x02) holds receiver 1's
    {
        const auto f = h1::buildCommandFrame(s, 1);
        const std::uint8_t want[8] = {0x7F, 0x7F, 0x7F, 0x02, 0x00, 0x6B, 0x93, 0x10};
        CHECK(std::memcmp(f.data(), want, 8) == 0);
    }
    // attenuator 20 dB with the enable bit: address 10 -> C0 = 0x14, C4 = 0x20 | 20
    {
        const auto f = h1::buildCommandFrame(s, 10);
        const std::uint8_t want[8] = {0x7F, 0x7F, 0x7F, 0x14, 0x00, 0x00, 0x00, 0x34};
        CHECK(std::memcmp(f.data(), want, 8) == 0);
    }
    // drive is zero, always; MOX is never set on any address
    for (int a = 0; a < 128; ++a) {
        const auto f = h1::buildCommandFrame(s, a);
        CHECK((f[3] & 1) == 0);
        if (a == 9) { CHECK(f[4] == 0); }
    }
    // eight receivers: C4[5:3] = 7, and receivers 2..7 have their own addresses; the document lists none for 8
    s.receivers = 8;
    s.rxHz[1] = 14200000;
    {
        const auto f = h1::buildCommandFrame(s, 0);
        CHECK(((f[7] >> 3) & 7) == 7 && (f[7] & 4) != 0);
        const auto g = h1::buildCommandFrame(s, 3);
        CHECK(g[3] == 0x06 && g[4] == 0x00 && g[5] == 0xD8 && g[6] == 0xAC && g[7] == 0xC0);  // 14200000 = 0x00D8ACC0
    }
    const std::vector<int> addrs = h1::refreshAddresses(s);
    CHECK(addrs.size() == 2 + 7 + 2);  // general, tx, seven receivers, drive, attenuator
    s.receivers = 1;
    CHECK(h1::refreshAddresses(s) == (std::vector<int>{0, 1, 2, 9, 10}));
    // a frame for an address nobody defined is all zero after the header
    const auto z = h1::buildCommandFrame(s, 100);
    CHECK(z[4] == 0 && z[5] == 0 && z[6] == 0 && z[7] == 0 && z[3] == 200);
}

// An independent reference: pull sample (set k, receiver r) out of a frame with explicit arithmetic.
std::complex<double> refSample(const Bytes& f, int n, int k, int r) {
    const int base = 8 + k * (6 * n + 2) + 6 * r;
    auto s24 = [&](int o) {
        long v = (static_cast<long>(f[o]) << 16) | (static_cast<long>(f[o + 1]) << 8) | f[o + 2];
        if (v >= 0x800000) { v -= 0x1000000; }
        return static_cast<double>(v) / 8388608.0;
    };
    return {s24(base), s24(base + 3)};
}

void testDecode() {
    Rng rng(555);
    // literal: one receiver, first set I = 0x7FFFFF, Q = 0x800000, second I = 1, Q = -1
    {
        Bytes f(512, 0);
        f[0] = f[1] = f[2] = 0x7F;
        f[3] = 0x20; f[4] = 0x01; f[7] = 0x2A;
        const std::uint8_t px[16] = {0x7F, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x12, 0x34, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0x56, 0x78};
        std::memcpy(&f[8], px, 16);
        std::vector<std::vector<std::complex<float>>> out;
        h1::FrameStatus st;
        CHECK(h1::decodeRxFrame(f.data(), f.size(), 1, out, st));
        CHECK(out.size() == 1 && out[0].size() == 63);
        CHECK_NEAR(out[0][0].real(), 8388607.0 / 8388608.0, 1e-7);
        CHECK_NEAR(out[0][0].imag(), -1.0, 1e-7);
        CHECK_NEAR(out[0][1].real(), 1.0 / 8388608.0, 1e-9);
        CHECK_NEAR(out[0][1].imag(), -1.0 / 8388608.0, 1e-9);
        CHECK(st.address() == 4 && st.c1 == 1 && st.c4 == 0x2A && !st.adcOverload());
    }
    // overflow bit and version only on address 0
    {
        Bytes f(512, 0);
        f[0] = f[1] = f[2] = 0x7F;
        f[3] = 0x00; f[4] = 0x01; f[7] = 0x33;
        std::vector<std::vector<std::complex<float>>> out;
        h1::FrameStatus st;
        CHECK(h1::decodeRxFrame(f.data(), f.size(), 1, out, st) && st.adcOverload() && st.boardSoftwareVersion() == 0x33);
        f[3] = 0x08;  // address 1
        CHECK(h1::decodeRxFrame(f.data(), f.size(), 1, out, st) && !st.adcOverload() && st.boardSoftwareVersion() == 0);
    }
    // every receiver count against the reference extraction
    for (int n = 1; n <= 8; ++n) {
        Bytes f(512);
        for (auto& b : f) { b = static_cast<std::uint8_t>(rng.next()); }
        f[0] = f[1] = f[2] = 0x7F;
        std::vector<std::vector<std::complex<float>>> out;
        h1::FrameStatus st;
        CHECK(h1::decodeRxFrame(f.data(), f.size(), n, out, st));
        CHECK(out.size() == static_cast<std::size_t>(n));
        const int sets = h1::setsPerFrame(n);
        for (int r = 0; r < n; ++r) {
            CHECK(out[static_cast<std::size_t>(r)].size() == static_cast<std::size_t>(sets));
            for (int k = 0; k < sets; ++k) {
                const auto want = refSample(f, n, k, r);
                CHECK_NEAR(out[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)].real(), want.real(), 1e-6);
                CHECK_NEAR(out[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)].imag(), want.imag(), 1e-6);
            }
        }
        // the padding at the end is not read: the last used byte is 8 + sets*(6n+2) - 1
        CHECK(8 + sets * (6 * n + 2) + h1::kPadding[n] == 512);
    }
    // refusals append nothing
    {
        Bytes f(512, 0);
        f[0] = f[1] = f[2] = 0x7F;
        std::vector<std::vector<std::complex<float>>> out;
        h1::FrameStatus st;
        CHECK(!h1::decodeRxFrame(f.data(), 511, 1, out, st) && !h1::decodeRxFrame(f.data(), 513, 1, out, st));
        CHECK(!h1::decodeRxFrame(f.data(), 512, 0, out, st) && !h1::decodeRxFrame(f.data(), 512, 9, out, st));
        for (int i = 0; i < 3; ++i) {
            Bytes g = f;
            g[static_cast<std::size_t>(i)] = 0x7E;
            CHECK(!h1::decodeRxFrame(g.data(), 512, 1, out, st));
        }
        CHECK(out.empty() || out[0].empty());
    }
}

void testFuzz() {
    Rng rng(0xBEEF);
    for (int i = 0; i < 20000; ++i) {
        Bytes f(512);
        for (auto& b : f) { b = static_cast<std::uint8_t>(rng.next()); }
        if (i % 4 != 0) { f[0] = f[1] = f[2] = 0x7F; }
        const int n = static_cast<int>(rng.next() % 11) - 1;
        const std::size_t len = (i % 16 == 0) ? rng.next() % 600 : 512;
        std::vector<std::vector<std::complex<float>>> out;
        h1::FrameStatus st;
        const bool ok = h1::decodeRxFrame(f.data(), std::min<std::size_t>(len, f.size()), n, out, st);
        if (ok) {
            CHECK(n >= 1 && n <= 8 && len == 512 && out.size() == static_cast<std::size_t>(n));
            for (auto& v : out) { for (auto& c : v) { CHECK(c.real() >= -1.0f && c.real() < 1.0f && c.imag() >= -1.0f && c.imag() < 1.0f); } }
        }
        h1::CommandState cs;
        cs.speedCode = static_cast<int>(rng.next());
        cs.receivers = static_cast<int>(rng.next() % 12) - 2;
        cs.attenDb = static_cast<std::uint8_t>(rng.next());
        cs.rxHz[0] = rng.next();
        const auto cf = h1::buildCommandFrame(cs, static_cast<int>(rng.next()));
        CHECK(cf[0] == 0x7F && (cf[3] & 1) == 0);  // sync, and never MOX, whatever the state
    }
}

void testOpenAndStream(const char* profile, int rate, bool generic) {
    Radio r;
    CHECK(r.start());
    r.ppm = 700.0;  // a 9 kHz tone moves ~6.3 Hz
    Hpsdr1Source s;
    CHECK(std::string(s.driverKey()) == "hpsdr1");
    CHECK(s.open(r.args(std::string("profile=") + profile + ",rate=" + std::to_string(rate) + ",timeout=3000")));
    CHECK(s.isOpen() && s.profile() == (generic ? Hpsdr1Profile::Generic : Hpsdr1Profile::RedPitaya));
    CHECK(s.sampleRateHz() == rate);
    const auto rates = s.supportedSampleRatesHz();
    CHECK(rates.size() == (generic ? 4u : 3u));
    double lo = -1, hi = -1;
    CHECK(s.frequencyRangeHz(lo, hi) == !generic);
    if (!generic) { CHECK(lo == 0.0 && hi == 61440000.0); }
    // tuning
    CHECK(s.setCenterFrequencyHz(7050000.0) && s.centerFrequencyHz() == 7050000.0);
    CHECK(!s.setCenterFrequencyHz(-1.0) && !s.setCenterFrequencyHz(std::nan("")) && !s.setCenterFrequencyHz(5.0e9));
    if (!generic) { CHECK(!s.setCenterFrequencyHz(61440001.0) && s.setCenterFrequencyHz(61440000.0) && s.setCenterFrequencyHz(7050000.0)); }
    // gain
    if (generic) {
        const auto gs = s.gains();
        CHECK(gs.size() == 1 && gs[0].name == "Attenuator" && gs[0].minDb == 0 && gs[0].maxDb == 31);
        CHECK(s.setGainDb("Attenuator", 20.0) && s.gainDb("Attenuator") == 20.0);
        CHECK(!s.setGainDb("Attenuator", 32.0) && !s.setGainDb("Attenuator", -1.0) && !s.setGainDb("Attenuator", 2.5) && !s.setGainDb("Other", 3.0));
        CHECK(s.gainDb("Attenuator") == 20.0);
    } else {
        CHECK(s.gains().empty() && !s.setGainDb("Attenuator", 3.0));
    }
    CHECK(!s.setSampleRateHz(44100.0) && !s.setSampleRateHz(0.0) && !s.setSampleRateHz(std::nan("")));
    CHECK(s.setSampleRateHz(rate));
    CHECK(s.start() && s.running());
    // the settings reached the radio before data was asked for, and match what was set
    for (int w = 0; w < 300 && r.commandsBeforeFirstStart.load() < 1; ++w) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(r.commandsBeforeFirstStart.load() >= 1);
    for (int i = 0; i < 100 && r.sentPackets.load() < 4; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const Radio::Seen sn = r.seen();
    CHECK(sn.speedBits == h1::speedCodeForRate(rate) && sn.receiversMinus1 == 0 && sn.duplex);
    CHECK(sn.rx1 == 7050000 && sn.tx == 7050000);
    if (generic) { CHECK(sn.attenRaw == (0x20 | 20)); }
    const auto got = readN(s, 30000);
    CHECK(got.size() >= 30000);
    const double expect = r.toneHz.load() / (1.0 + r.ppm.load() * 1e-6);
    CHECK_NEAR(toneHz(got, rate), expect, 2.0 + rate * 1.5e-5);
    if (rate <= 96000) { CHECK(std::fabs(toneHz(got, rate) - r.toneHz.load()) > 3.0); }  // the clock error is in the data
    double mag = 0;
    for (auto& v : got) { mag += std::abs(v); }
    CHECK_NEAR(mag / static_cast<double>(got.size()), 0.5, 0.03);
    CHECK(s.packetsLost() == 0 && s.badPackets() == 0 && s.overloadFrames() == 0);
    // retuning while running reaches the radio in a refresh
    CHECK(s.setCenterFrequencyHz(14200000.0));
    for (int i = 0; i < 200 && r.seen().rx1 != 14200000; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(r.seen().rx1 == 14200000 && r.seen().tx == 14200000);
    // the command frames keep coming about every 100 ms
    const int before = r.commandPackets.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(r.commandPackets.load() - before >= 3);
    CHECK(s.commandPacketsSent() >= 3);
    s.stop();
    CHECK(!s.running());
    for (int i = 0; i < 100 && r.stops.load() < 1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(r.stops.load() == 1);
    // this client never keys, never sends audio, never sets drive
    CHECK(r.moxSeen.load() == 0 && r.audioNonZero.load() == 0 && r.driveNonZero.load() == 0 && r.badCommands.load() == 0);
    s.closeDevice();
    CHECK(!s.isOpen());
}

void testRefusals() {
    {
        Hpsdr1Source s;
        CHECK(!s.open("hpsdr1=127.0.0.1:0") && !s.open("hpsdr1=127.0.0.1:70000") && !s.open("hpsdr1=[::1"));
        CHECK(!s.open("hpsdr1=127.0.0.1:1,timeout=2") && !s.open("hpsdr1=127.0.0.1:1,profile=hermeslite"));
        CHECK(!s.open("hpsdr1=127.0.0.1:1,rate=44100") && !s.open("hpsdr1=127.0.0.1:1,profile=redpitaya,rate=384000"));
        CHECK(!s.open("hpsdr1=127.0.0.1:1,rate=abc"));
        CHECK(!s.start() && !s.setCenterFrequencyHz(7e6) && !s.setSampleRateHz(48000) && !s.setGainDb("Attenuator", 3));
        CHECK(s.gains().empty() && s.supportedSampleRatesHz().empty());
    }
    {   // nobody answers
        Hpsdr1Source s;
        CHECK(!s.open("hpsdr1=127.0.0.1:" + std::to_string(deadPort()) + ",timeout=300") && !s.isOpen());
    }
    {   // MAC filter
        Radio r;
        CHECK(r.start());
        Hpsdr1Source s;
        CHECK(!s.open(r.args("timeout=3000,mac=de:ad:be:ef:00:01")));
        CHECK(s.open(r.args("timeout=3000,mac=00:1C:C0:A2:13:77")) && s.isOpen());
        s.closeDevice();
    }
    {   // a radio that goes silent after Start
        Radio r(Quirk::Silent);
        CHECK(r.start());
        Hpsdr1Source s;
        CHECK(s.open(r.args("timeout=1000")));
        CHECK(s.start());
        const auto t0 = std::chrono::steady_clock::now();
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.faulted() && s.faultedWhile().find("stopped sending") != std::string::npos);
        s.closeDevice();
    }
}

void testStreamFlaws() {
    {   // lost packets are counted, the stream goes on
        Radio r(Quirk::DropEvery8);
        CHECK(r.start());
        Hpsdr1Source s;
        CHECK(s.open(r.args("timeout=3000")));
        CHECK(s.start());
        CHECK(readN(s, 20000).size() >= 20000);
        CHECK(s.packetsLost() >= 3 && !s.faulted());
        s.closeDevice();
    }
    {   // frames without sync are counted and skipped, the good ones still come
        Radio r(Quirk::BadFrames);
        CHECK(r.start());
        Hpsdr1Source s;
        CHECK(s.open(r.args("timeout=3000")));
        CHECK(s.start());
        CHECK(readN(s, 20000).size() >= 20000);
        CHECK(s.badPackets() >= 3 && !s.faulted());
        s.closeDevice();
    }
    {   // a missing sync in the second frame only costs that frame
        Radio r(Quirk::NoSyncInSecondFrame);
        CHECK(r.start());
        Hpsdr1Source s;
        CHECK(s.open(r.args("timeout=3000")));
        CHECK(s.start());
        CHECK(readN(s, 20000).size() >= 20000);
        CHECK(s.badPackets() >= 3);
        s.closeDevice();
    }
    {   // the overflow flag is counted
        Radio r(Quirk::OverloadEvery3);
        CHECK(r.start());
        Hpsdr1Source s;
        CHECK(s.open(r.args("timeout=3000")));
        CHECK(s.start());
        CHECK(readN(s, 20000).size() >= 20000);
        CHECK(s.overloadFrames() >= 3 && !s.faulted());
        s.closeDevice();
    }
}

void testCycles() {
    Radio r;
    CHECK(r.start());
    const auto abandoned = NetSampleSource::readersAbandoned();
    Hpsdr1Source s;
    CHECK(s.open(r.args("timeout=3000")));
    CHECK(s.setCenterFrequencyHz(10.0e6));
    for (int i = 0; i < 50; ++i) {
        const bool st = s.start();
        CHECK(st);
        if (!st) {
            std::printf("cycle %d start failed: %s | faulted=%d sent=%d starts=%d stops=%d recv=%llu bad=%llu\n", i, s.lastError(), (int)s.faulted(),
                        r.sentPackets.load(), r.starts.load(), r.stops.load(), (unsigned long long)s.samplesReceived(), (unsigned long long)s.badPackets());
            break;
        }
        // keep the stand-in in step: it handles one datagram at a time and joins its stream thread on each
        // Start and Stop, so a client that cycles in microseconds would otherwise outrun it by many commands
        for (int w = 0; w < 600 && r.starts.load() < i + 1; ++w) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        if (i % 10 == 0) { CHECK(readN(s, 1000).size() >= 1000); }
        s.stop();
        for (int w = 0; w < 600 && r.stops.load() < i + 1; ++w) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    for (int i = 0; i < 300 && (r.starts.load() < 50 || r.stops.load() < 50); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(r.starts.load() == 50 && r.stops.load() >= 50);
    s.closeDevice();
    for (int i = 0; i < 50; ++i) {
        Hpsdr1Source t;
        const bool op = t.open(r.args("timeout=3000"));
        CHECK(op);
        if (!op) { break; }
        if (i % 5 == 0) { CHECK(t.start()); }
        const auto t0 = std::chrono::steady_clock::now();
        t.closeDevice();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000));
    }
    CHECK(NetSampleSource::readersAbandoned() == abandoned);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testDocumentTables();
    testCommandFrames();
    testDecode();
    testFuzz();
    testOpenAndStream("generic", 192000, true);
    testOpenAndStream("generic", 48000, true);
    testOpenAndStream("generic", 384000, true);
    testOpenAndStream("redpitaya", 96000, false);
    testOpenAndStream("redpitaya", 192000, false);
    testRefusals();
    testStreamFlaws();
    testCycles();
    return testSummary("test_hpsdr1_source");
}
