// test_hl2.cpp - the Hermes-Lite 2 extensions to the openHPSDR protocol 1 (src/source/hl2_protocol.hpp, the
// profile=hl2 of Hpsdr1Source), proven against a STAND-IN radio on loopback UDP. The stand-in is written from the
// project's wiki page "Protocol" (read as facts only; the repository has no licence and its gateware is GPL: nothing
// of the repository was used). It proves FoxSDR's code is consistent with that page, not that a Hermes-Lite 2
// behaves so. UNPROVEN on a real Hermes-Lite 2. Everything that writes to the radio's own hardware (I2C, SPI, the
// EEPROM, bias, clock, synchronisation words) is NOT built, and testNothingForbiddenIsSent proves none of those
// addresses is ever sent.
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
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "source/hl2_protocol.hpp"
#include "source/hpsdr1_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::Hpsdr1Profile;
using cascade::source::Hpsdr1Source;
namespace hp = cascade::source::hpsdr;
namespace hl = cascade::source::hl2;

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

// A discovery reply laid out as the wiki's table (the Address column is the byte offset).
Bytes hl2Reply(std::uint8_t status, std::uint8_t board = 0x06, std::size_t len = 60) {
    Bytes r(60, 0);
    r[0] = 0xEF;
    r[1] = 0xFE;
    r[2] = status;
    const std::uint8_t mac[6] = {0x00, 0x1c, 0xc0, 0xa2, 0x13, 0x77};
    for (int i = 0; i < 6; ++i) { r[3 + i] = mac[i]; }
    r[0x09] = 73;           // gateware major
    r[0x0A] = board;        // board id
    r[0x13] = 4;            // number of hardware receivers
    r[0x14] = 0x40 | 0x05;  // 16-bit wide band, build board 5
    r[0x15] = 2;            // gateware minor
    r[0x1B] = 0x40 | 0x02;  // PTT, ADC clip count 2
    r[0x1C] = 0x0A; r[0x1D] = 0xBC;  // temperature 0xABC
    r[0x1E] = 0x01; r[0x1F] = 0x23;  // forward power 0x123
    r[0x20] = 0x04; r[0x21] = 0x56;  // reverse power 0x456
    r[0x22] = 0x07; r[0x23] = 0x89;  // bias current 0x789
    r.resize(len);
    return r;
}

// --- the stand-in radio ------------------------------------------------------------------------------------------------
class Radio {
public:
    explicit Radio(std::uint8_t board = 0x06, std::size_t replyLen = 60) : board_(board), replyLen_(replyLen) {}
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
    struct Seen {
        std::set<int> addresses;  // every command address received
        std::vector<int> startBytes, stopBytes;
        std::uint32_t rx1 = 0;
        int speedBits = -1;
        int lnaByte = -1;  // C4 of address 10
        int moxFrames = 0, drive = 0, packets = 0, badFrames = 0, discoveries = 0;
        bool nonZeroPayload = false;
    };
    Seen seen() {
        std::lock_guard<std::mutex> lk(m_);
        return seen_;
    }
    std::atomic<bool> overload{false};
    std::atomic<std::uint8_t> fw{0x46};
    std::atomic<double> toneHz{9000.0};
    std::atomic<bool> streaming{false};

private:
    void loop() {
        Bytes d;
        std::uint16_t from = 0;
        while (run_.load()) {
            if (!pollRead(sock_.sock(), 5)) { continue; }
            if (!sock_.recvFrom(run_, d, from)) { continue; }
            if (d.size() == 63 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x02) {
                {
                    std::lock_guard<std::mutex> lk(m_);
                    ++seen_.discoveries;
                }
                const Bytes j = hl2Reply(streaming.load() ? 3 : 2, board_, replyLen_);
                sock_.sendTo(from, j.data(), j.size());
            } else if (d.size() == 64 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x04) {
                const int cmd = d[3];
                bool start = false;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    if (cmd & 1) { seen_.startBytes.push_back(cmd); start = true; } else { seen_.stopBytes.push_back(cmd); }
                }
                if (start) { startStream(from); } else { stopStream(); }
            } else if (d.size() == 1032 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x01 && d[3] == 0x02) {
                std::lock_guard<std::mutex> lk(m_);
                ++seen_.packets;
                for (int fi = 0; fi < 2; ++fi) { parseFrame(&d[8 + 512 * fi]); }
            }
        }
    }
    void parseFrame(const std::uint8_t* f) {
        if (f[0] != 0x7F || f[1] != 0x7F || f[2] != 0x7F) { ++seen_.badFrames; return; }
        if (f[3] & 1) { ++seen_.moxFrames; }
        for (int i = 8; i < 512; ++i) { if (f[i] != 0) { seen_.nonZeroPayload = true; break; } }
        const int addr = (f[3] >> 1) & 0x3F;
        seen_.addresses.insert(addr);
        const std::uint32_t word = (static_cast<std::uint32_t>(f[4]) << 24) | (static_cast<std::uint32_t>(f[5]) << 16) | (static_cast<std::uint32_t>(f[6]) << 8) | f[7];
        if (addr == 0) { seen_.speedBits = f[4] & 3; }
        else if (addr == 2) { seen_.rx1 = word; }
        else if (addr == 9) { if (f[4] != 0) { ++seen_.drive; } }
        else if (addr == 10) { seen_.lnaByte = f[7]; }
    }
    void startStream(std::uint16_t to) {
        stopStream();
        streamRun_.store(true);
        streaming.store(true);
        stream_ = std::thread([this, to] {
            std::uint32_t seq = 0;
            double phase = 0.0;
            int rot = 0;
            Rng rng(99);
            while (streamRun_.load()) {
                for (int burst = 0; burst < 6 && streamRun_.load(); ++burst) {
                    int bits;
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        bits = seen_.speedBits;
                    }
                    const int rate = bits == 3 ? 384000 : bits == 2 ? 192000 : bits == 1 ? 96000 : 48000;
                    const double dphi = 2.0 * kPi * toneHz.load() / rate;
                    Bytes p = {0xEF, 0xFE, 0x01, 0x06, static_cast<std::uint8_t>(seq >> 24), static_cast<std::uint8_t>(seq >> 16),
                               static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq)};
                    p.resize(1032, 0);
                    for (int fi = 0; fi < 2; ++fi) {
                        std::uint8_t* f = &p[8 + 512 * fi];
                        f[0] = f[1] = f[2] = 0x7F;
                        const int a = rot++ % 3;  // the three status addresses the wiki defines
                        f[3] = static_cast<std::uint8_t>(a << 3);
                        if (a == 0) {
                            f[4] = overload.load() ? 0x01 : 0x00;
                            f[6] = 0x42;  // FIFO MSBs
                            f[7] = fw.load();
                        } else if (a == 1) {
                            f[4] = 0x0A; f[5] = 0xBC; f[6] = 0x01; f[7] = 0x23;
                        } else {
                            f[4] = 0x04; f[5] = 0x56; f[6] = 0x07; f[7] = 0x89;
                        }
                        for (int k = 0; k < 63; ++k) {
                            std::uint8_t* b = f + 8 + 8 * k;
                            auto nz = [&]() { return (static_cast<double>(rng.next() & 0xFFFF) / 65535.0 - 0.5) * 0.02; };
                            const double re = 0.5 * std::cos(phase) + nz();
                            const double im = 0.5 * std::sin(phase) + nz();
                            phase += dphi;
                            if (phase > 2 * kPi) { phase -= 2 * kPi; }
                            const std::int32_t I = static_cast<std::int32_t>(std::lround(re * 8388607.0));
                            const std::int32_t Q = static_cast<std::int32_t>(std::lround(im * 8388607.0));
                            b[0] = static_cast<std::uint8_t>(I >> 16); b[1] = static_cast<std::uint8_t>(I >> 8); b[2] = static_cast<std::uint8_t>(I);
                            b[3] = static_cast<std::uint8_t>(Q >> 16); b[4] = static_cast<std::uint8_t>(Q >> 8); b[5] = static_cast<std::uint8_t>(Q);
                        }
                    }
                    sock_.sendTo(to, p.data(), p.size());
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

    std::uint8_t board_;
    std::size_t replyLen_;
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

double toneHzOf(const std::vector<std::complex<float>>& v, double fs) {
    std::complex<double> acc(0, 0);
    for (std::size_t i = 1; i < v.size(); ++i) { acc += std::complex<double>(v[i]) * std::conj(std::complex<double>(v[i - 1])); }
    return std::arg(acc) * fs / (2.0 * kPi);
}

bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// --- protocol literals --------------------------------------------------------------------------------------------------
void testProtocol() {
    CHECK(hl::kBoardId == 0x06 && hl::kDiscoveryBytes == 60);
    // C0 = RQST(7) | ADDR[5:0](6:1) | MOX(0): the MOX bit is never part of c0For
    CHECK(hl::c0For(0, false) == 0x00 && hl::c0For(0x0A, false) == 0x14 && hl::c0For(0x0A, true) == 0x94 && hl::c0For(0x3F, false) == 0x7E);
    CHECK(hl::c0For(0x40, false) == 0x00);  // seven bits do not fit: masked, never reaching the RQST bit
    // receivers 1..7 at 0x02..0x08, 8..12 at 0x12..0x16
    const int want[13] = {-1, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x12, 0x13, 0x14, 0x15, 0x16};
    for (int n = 1; n <= 12; ++n) { CHECK(hl::rxNcoAddress(n) == want[n]); }
    CHECK(hl::rxNcoAddress(0) == -1 && hl::rxNcoAddress(13) == -1 && hl::rxNcoAddress(-1) == -1 && hl::kMaxReceivers == 12);
    // the direct LNA: bit 6 set, code 0 = -12 dB, code 60 = +48 dB
    CHECK(hl::lnaDirectByte(-12) == 0x40 && hl::lnaDirectByte(0) == 0x4C && hl::lnaDirectByte(20) == 0x60 && hl::lnaDirectByte(48) == 0x7C);
    CHECK(hl::lnaDbValid(-12) && hl::lnaDbValid(48) && !hl::lnaDbValid(-13) && !hl::lnaDbValid(49));
    CHECK((hl::lnaDirectByte(10) & 0x40) != 0 && (hl::lnaHermesByte(10, true) & 0x40) == 0 && hl::lnaHermesByte(10, true) == 0x2A && hl::lnaHermesByte(40, false) == 0x08);
    // start and stop: bit 0 radio, bit 1 wide band never, bit 7 watchdog off
    CHECK(hl::startCommand(false) == 0x01 && hl::startCommand(true) == 0x81 && hl::stopCommand(false) == 0x00 && hl::stopCommand(true) == 0x80);
    CHECK((hl::startCommand(true) & 0x02) == 0 && (hl::startCommand(false) & 0x02) == 0);
    // the discovery reply
    const Bytes good = hl2Reply(0x02);
    const hl::Info i = hl::parseDiscovery(good.data(), good.size());
    CHECK(i.valid && i.gatewareMajor == 73 && i.gatewareMinor == 2 && i.hardwareReceivers == 4 && i.wideband16Bit && i.buildBoard == 5);
    CHECK(!i.externalCwKey && i.ptt && i.adcClipCount == 2 && i.temperature == 0xABC && i.forwardPower == 0x123 && i.reversePower == 0x456 && i.biasCurrent == 0x789);
    {
        Bytes b = good;
        b[0x14] = 0x00 | 0x03;  // 12-bit sign extended
        b[0x1B] = 0x80;
        const hl::Info j = hl::parseDiscovery(b.data(), b.size());
        CHECK(j.valid && !j.wideband16Bit && j.buildBoard == 3 && j.externalCwKey && !j.ptt && j.adcClipCount == 0);
    }
    // refusals: too short, wrong magic, wrong status, another board
    for (std::size_t n : {std::size_t{0}, std::size_t{10}, std::size_t{11}, std::size_t{59}}) { CHECK(!hl::parseDiscovery(good.data(), n).valid); }
    CHECK(!hl::parseDiscovery(nullptr, 60).valid);
    for (int pos = 0; pos < 3; ++pos) {
        Bytes b = good;
        b[static_cast<std::size_t>(pos)] ^= 0x10;
        CHECK(!hl::parseDiscovery(b.data(), b.size()).valid);
    }
    for (std::uint8_t board : {0x00, 0x01, 0x02, 0x04, 0x05, 0x07, 0xFF}) {
        const Bytes b = hl2Reply(0x02, board);
        CHECK(!hl::parseDiscovery(b.data(), b.size()).valid);
    }
    // the status words in the C&C bytes (ACK == 0)
    {
        const hl::Telemetry t = hl::decodeStatus(0x00, 0x03, 0x00, 0xC5, 0x46);  // address 0: overload, tx bit, flags 3, fifo 0x45, fw 0x46
        CHECK(t.valid && t.radioAddress == 0 && !t.ack && t.adcOverload && t.txInhibitBit && t.fifoFlags == 3 && t.txFifoMsb == 0x45 && t.firmwareVersion == 0x46);
        const hl::Telemetry t0 = hl::decodeStatus(0x00, 0x00, 0x00, 0x00, 0x10);
        CHECK(t0.valid && !t0.adcOverload && !t0.txInhibitBit && t0.fifoFlags == 0 && t0.firmwareVersion == 0x10);
        const hl::Telemetry t1 = hl::decodeStatus(0x08, 0x12, 0x34, 0x56, 0x78);  // address 1
        CHECK(t1.valid && t1.radioAddress == 1 && t1.temperature == 0x1234 && t1.forwardPower == 0x5678);
        const hl::Telemetry t2 = hl::decodeStatus(0x10, 0x9A, 0xBC, 0xDE, 0xF0);  // address 2
        CHECK(t2.valid && t2.radioAddress == 2 && t2.reversePower == 0x9ABC && t2.current == 0xDEF0);
        for (int a = 3; a < 16; ++a) { CHECK(!hl::decodeStatus(static_cast<std::uint8_t>(a << 3), 1, 2, 3, 4).valid); }
        CHECK(!hl::decodeStatus(0x80, 1, 2, 3, 4).valid && hl::decodeStatus(0x80, 1, 2, 3, 4).ack);  // an answer to a request this client never makes
        CHECK(!hl::decodeStatus(0x88, 1, 2, 3, 4).valid);
    }
    // every C0..C4 combination decodes without surprises (exhaustive over C0, sampled over the data bytes)
    int validCount = 0;
    for (int c0 = 0; c0 < 256; ++c0) {
        for (int k = 0; k < 16; ++k) {
            const hl::Telemetry t = hl::decodeStatus(static_cast<std::uint8_t>(c0), static_cast<std::uint8_t>(k * 17), static_cast<std::uint8_t>(k * 5), 0xFF, static_cast<std::uint8_t>(k));
            if (t.valid) { ++validCount; CHECK(!t.ack && t.radioAddress <= 2); }
        }
    }
    CHECK(validCount == 3 * 8 * 16);  // 8 values of C0 bits 2..0 per address, ACK clear
}

// --- the source -------------------------------------------------------------------------------------------------------------
void testHl2Source() {
    Radio radio;
    CHECK(radio.start());
    Hpsdr1Source s;
    CHECK(s.open(radio.args("profile=hl2,rate=96000")));
    CHECK(s.profile() == Hpsdr1Profile::HermesLite2 && std::string(s.name()).find("HPSDR") != std::string::npos);
    const hl::Info info = s.hl2Info();
    CHECK(info.valid && info.gatewareMajor == 73 && info.hardwareReceivers == 4 && info.wideband16Bit && info.gatewareMinor == 2);
    // gains: one, "LNA gain", -12..+48 dB in whole dB, default +20 dB
    const auto g = s.gains();
    CHECK(g.size() == 1 && g[0].name == "LNA gain" && g[0].minDb == -12.0 && g[0].maxDb == 48.0 && g[0].stepDb == 1.0);
    CHECK(s.gainDb("LNA gain") == 20.0 && s.gainDb("Attenuator") == 0.0);
    CHECK(!s.setGainDb("Attenuator", 5.0) && std::string(s.lastError()).find("LNA gain") != std::string::npos);
    for (double bad : {-13.0, 49.0, 1.5, -12.5, std::nan(""), HUGE_VAL}) { CHECK(!s.setGainDb("LNA gain", bad)); }
    CHECK(s.setGainDb("LNA gain", -12.0) && s.gainDb("LNA gain") == -12.0 && s.setGainDb("LNA gain", 48.0) && s.gainDb("LNA gain") == 48.0);
    CHECK(s.setGainDb("LNA gain", 30.0));
    // frequency and rates
    double lo = -1, hi = -1;
    CHECK(s.frequencyRangeHz(lo, hi) && lo == 0.0 && hi == 38400000.0);
    CHECK(!s.setCenterFrequencyHz(38400001.0) && std::string(s.lastError()).find("38.4 MHz") != std::string::npos);
    CHECK(!s.setCenterFrequencyHz(-1.0) && !s.setCenterFrequencyHz(std::nan("")));
    CHECK(s.setCenterFrequencyHz(7100000.0) && s.centerFrequencyHz() == 7100000.0);
    const auto rates = s.supportedSampleRatesHz();
    CHECK(rates.size() == 4 && rates.back() == 384000.0);
    CHECK(s.sampleRateHz() == 96000.0);
    // streaming
    CHECK(s.start() && s.running());
    CHECK(waitFor([&] { return radio.seen().packets >= 3; }, 4000));
    const std::vector<std::complex<float>> x = readN(s, 40000);
    CHECK(x.size() >= 40000);
    CHECK(std::fabs(toneHzOf(x, 96000.0) - 9000.0) < 60.0);
    {
        const Radio::Seen sn = radio.seen();
        CHECK(sn.startBytes.size() == 1 && sn.startBytes[0] == 0x01);     // watchdog left ON by default
        CHECK(sn.lnaByte == (0x40 | (30 + 12)));                           // the direct LNA word: bit 6 + (30 dB + 12)
        CHECK(sn.speedBits == 1 && sn.rx1 == 7100000u);
        CHECK(sn.moxFrames == 0 && sn.drive == 0 && !sn.nonZeroPayload && sn.badFrames == 0);
    }
    // telemetry from the status frames, raw counts
    CHECK(waitFor([&] { const auto t = s.hl2Telemetry(); return t.valid && t.firmwareVersion == 0x46 && t.temperature == 0x0ABC && t.reversePower == 0x0456; }, 4000));
    {
        const hl::Telemetry t = s.hl2Telemetry();
        CHECK(t.forwardPower == 0x0123 && t.current == 0x0789 && !t.adcOverload && t.txFifoMsb == 0x42);
    }
    radio.overload.store(true);
    CHECK(waitFor([&] { return s.hl2Telemetry().adcOverload && s.overloadFrames() > 0; }, 4000));
    radio.overload.store(false);
    // live gain change while running reaches the radio
    CHECK(s.setGainDb("LNA gain", 0.0));
    CHECK(waitFor([&] { return radio.seen().lnaByte == 0x4C; }, 4000));
    s.stop();
    CHECK(waitFor([&] { return !radio.seen().stopBytes.empty(); }, 4000));
    CHECK(radio.seen().stopBytes.back() == 0x00);
    s.closeDevice();
    radio.stop();
}

void testWatchdogOff() {
    Radio radio;
    CHECK(radio.start());
    Hpsdr1Source s;
    CHECK(s.open(radio.args("profile=hl2,watchdog=off")));
    CHECK(s.start());
    CHECK(waitFor([&] { return !radio.seen().startBytes.empty(); }, 4000));
    s.stop();
    CHECK(waitFor([&] { return !radio.seen().stopBytes.empty(); }, 4000));
    const Radio::Seen sn = radio.seen();
    CHECK(sn.startBytes[0] == 0x81 && sn.stopBytes.back() == 0x80);
    CHECK((sn.startBytes[0] & 0x02) == 0);  // the wide-band stream is never asked for
    s.closeDevice();
    radio.stop();
}

void testRefusals() {
    {   // a Hermes answering to profile=hl2
        Radio radio(0x01);
        CHECK(radio.start());
        Hpsdr1Source s;
        CHECK(!s.open(radio.args("profile=hl2")) && std::string(s.lastError()).find("not a Hermes-Lite 2") != std::string::npos);
        CHECK(!s.isOpen() && !s.hl2Info().valid);
        radio.stop();
    }
    {   // a Hermes-Lite 2 whose reply carries no extension
        Radio radio(0x06, 20);
        CHECK(radio.start());
        Hpsdr1Source s;
        CHECK(!s.open(radio.args("profile=hl2")) && std::string(s.lastError()).find("too short") != std::string::npos);
        radio.stop();
    }
    {   // watchdog= outside the hl2 profile, and bad values
        Radio radio;
        CHECK(radio.start());
        Hpsdr1Source s;
        CHECK(!s.open(radio.args("watchdog=off")) && std::string(s.lastError()).find("hl2 profile") != std::string::npos);
        CHECK(!s.open(radio.args("profile=hl2,watchdog=maybe")) && std::string(s.lastError()).find("on or off") != std::string::npos);
        CHECK(!s.open(radio.args("profile=nope")) && std::string(s.lastError()).find("hl2") != std::string::npos);
        radio.stop();
    }
    {   // the generic profile on the same radio is unchanged: the Hermes attenuator word, not the direct LNA
        Radio radio;
        CHECK(radio.start());
        Hpsdr1Source s;
        CHECK(s.open(radio.args("")) && s.profile() == Hpsdr1Profile::Generic && !s.hl2Info().valid);
        CHECK(s.gains().size() == 1 && s.gains()[0].name == "Attenuator");
        CHECK(s.setGainDb("Attenuator", 10.0));
        CHECK(s.start());
        CHECK(waitFor([&] { const auto q = radio.seen(); return q.lnaByte >= 0 && !q.startBytes.empty(); }, 4000));
        const Radio::Seen sn = radio.seen();
        CHECK(sn.startBytes.size() == 1 && sn.startBytes[0] == 0x01);
        CHECK(sn.lnaByte == (0x20 | 10) && (sn.lnaByte & 0x40) == 0);
        CHECK(!s.hl2Telemetry().valid);
        double lo, hi;
        CHECK(!s.frequencyRangeHz(lo, hi));
        s.closeDevice();
        radio.stop();
    }
}

void testNothingForbiddenIsSent() {
    Radio radio;
    CHECK(radio.start());
    Hpsdr1Source s;
    CHECK(s.open(radio.args("profile=hl2")));
    CHECK(s.setCenterFrequencyHz(14074000.0) && s.setGainDb("LNA gain", 25.0));
    CHECK(s.start());
    CHECK(waitFor([&] { return radio.seen().packets >= 30; }, 6000));
    s.stop();
    s.closeDevice();
    const Radio::Seen sn = radio.seen();
    // only the receive-side addresses: general, TX NCO (equal to RX1), RX1 NCO, drive (always 0), LNA
    const std::set<int> allowed = {0, 1, 2, 9, 10};
    for (int a : sn.addresses) { CHECK(allowed.count(a) == 1); }
    for (int bad : {0x0B, 0x0E, 0x0F, 0x10, 0x12, 0x17, 0x2B, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3F}) { CHECK(sn.addresses.count(bad) == 0); }
    CHECK(sn.moxFrames == 0 && sn.drive == 0 && !sn.nonZeroPayload && sn.badFrames == 0);
    CHECK(!sn.startBytes.empty());
    for (int b : sn.startBytes) { CHECK((b & 0x02) == 0); }  // never the wide-band stream
    radio.stop();
}

void testCycles() {
    Radio radio;
    CHECK(radio.start());
    Hpsdr1Source s;
    CHECK(s.open(radio.args("profile=hl2")));
    for (int i = 0; i < 15; ++i) {
        CHECK(s.start());
        s.stop();
        CHECK(!s.running());
    }
    s.closeDevice();
    for (int i = 0; i < 15; ++i) {
        Hpsdr1Source t;
        CHECK(t.open(radio.args("profile=hl2")));
        t.closeDevice();
        CHECK(!t.isOpen());
    }
    CHECK(Hpsdr1Source::readersAbandoned() == 0);
    radio.stop();
}

}  // namespace

int main() {
    testProtocol();
    testHl2Source();
    testWatchdogOff();
    testRefusals();
    testNothingForbiddenIsSent();
    testCycles();
    return testSummary("test_hl2");
}
