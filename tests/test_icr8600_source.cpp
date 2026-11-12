// test_icr8600_source.cpp - the Icom IC-R8600 [I/Q OUT] protocol layer and source (src/source/icr8600_protocol.*,
// icr8600_source.*) against a STAND-IN radio behind cascade::usb::UsbDevice, written here from the two MIT projects
// named in icr8600_protocol.hpp (ibelinp/ic-r8600-usb-iq, dmitryelj/SoapyICR8600). The byte strings of
// testLiterals are the ones both projects send (SoapyICR8600's FE FE 96 E0 ... FD FF arrays). No firmware of
// Icom's is in this file: the "firmware" below is a made-up sequence with the right shape. UNPROVEN on a real
// IC-R8600, with Icom's real firmware, and on the WinUSB readBulkOnce path (compiled only).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "source/icr8600_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

#if defined(_WIN32)
#include <process.h>
#define EOR_PID _getpid()
#else
#include <unistd.h>
#define EOR_PID getpid()
#endif

using namespace cascade::source;
namespace ic = cascade::source::icr8600;
using Bytes = std::vector<std::uint8_t>;
using Cx = std::complex<float>;

namespace {

// ---- helpers: a made-up firmware in both file formats ------------------------------------------------------------
std::vector<ic::FwRecord> madeUpFirmware() {
    std::vector<ic::FwRecord> r;
    r.push_back({0xE600, {0x01}});
    r.push_back({0x0000, {0x02, 0x07, 0xF3, 0xFF, 0x00, 0x11, 0x22}});
    r.push_back({0x0100, Bytes(64, 0xA5)});
    r.push_back({0xE000, {0x01, 0x02, 0x03}});
    r.push_back({0xE600, {0x00}});
    return r;
}
void putLe32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); } }
Bytes csptRecord(std::uint8_t reqType, std::uint8_t req, std::uint16_t wValue, const Bytes& data) {
    Bytes b{'C', 'S', 'P', 'T'};
    putLe32(b, 0); putLe32(b, 0);                                   // 4..11
    putLe32(b, static_cast<std::uint32_t>(reqType) << 24);          // 12: request type in the top byte
    b.push_back(req); b.push_back(0);                                // 16, 17
    b.push_back(static_cast<std::uint8_t>(wValue)); b.push_back(static_cast<std::uint8_t>(wValue >> 8));  // 18, 19
    putLe32(b, 0); putLe32(b, 0);                                   // 20..27 (wIndex, ...)
    putLe32(b, static_cast<std::uint32_t>(data.size()));            // 28
    b.insert(b.end(), data.begin(), data.end());
    return b;
}
Bytes csptFile(const std::vector<ic::FwRecord>& recs) {
    Bytes f;
    for (const auto& r : recs) { const Bytes x = csptRecord(0x40, 0xA0, r.wValue, r.data); f.insert(f.end(), x.begin(), x.end()); }
    return f;
}
std::string jsonFile(const std::vector<ic::FwRecord>& recs) {
    std::string s = "[";
    for (std::size_t i = 0; i < recs.size(); ++i) {
        char h[16];
        std::string hex;
        for (std::uint8_t b : recs[i].data) { std::snprintf(h, sizeof(h), "%02x", b); hex += h; }
        s += std::string(i ? ", " : "") + "{\"wValue\": " + std::to_string(recs[i].wValue) + ", \"data\": \"" + hex + "\"}";
    }
    return s + "]";
}
Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }
bool same(const std::vector<ic::FwRecord>& a, const std::vector<ic::FwRecord>& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].wValue != b[i].wValue || a[i].data != b[i].data) { return false; }
    }
    return true;
}
std::string writeTemp(const std::string& name, const Bytes& content) {
    const std::string p = "eor-" + std::to_string(EOR_PID) + "-" + name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    return p;
}

// ---- the stand-in radio -----------------------------------------------------------------------------------
struct World {
    std::mutex mu;
    std::condition_variable cv;
    bool loaderPresent = true, streamPresent = false;
    int enumsAfterRelease = -1;       // -1 until the CPU is released
    int appearAfterEnums = 3;         // the 0023 personality shows up on the Nth enumeration after the release
    bool neverReenumerate = false;
    int rejectRecord = -1;            // control transfer number (1-based) the loader refuses
    std::vector<ic::FwRecord> received;
    // CI-V
    std::vector<Bytes> civLog;         // payloads, in order
    bool silentCiv = false, nakAntenna = false, nakFrequency = false, garbageAck = false;
    std::deque<Bytes> acks;
    bool ivOn = false, modeOn = false;
    std::uint8_t rateCode = 0;
    std::uint64_t freq = 0;
    unsigned rfGain = 999, atten = 999, antenna = 99;
    int preamp = -1;
    bool streamBegun = false, streamEnded = false;
    int bulkOnceCalls = 0;
    // I/Q
    double toneHz = 20000.0, ppm = 0.0, noise = 0.003;
    bool noData = false, failRead = false;
    bool dropNextSample = false;      // loses one int16 from the stream (a Q), once
    std::mt19937 rng{4242};
    double phase = 0.0;
    std::uint64_t pairsEmitted = 0;
    Bytes pending;                     // generated, not yet delivered
    std::uint64_t markersEmitted = 0;
    bool startWithQ = false;           // begin the stream with a stray Q sample before the first marker

    static unsigned bcd2(std::uint8_t b) { return (b >> 4) * 10 + (b & 15); }

    void civ(const Bytes& frame) {
        if (frame.size() < 6 || frame[0] != 0xFE || frame[1] != 0xFE || frame[2] != ic::kRadioAddress || frame[3] != ic::kControllerAddress) {
            return;  // not for us: the radio says nothing
        }
        std::size_t end = 4;
        while (end < frame.size() && frame[end] != 0xFD) { ++end; }
        if (end >= frame.size()) { return; }
        Bytes pl(frame.begin() + 4, frame.begin() + static_cast<std::ptrdiff_t>(end));
        if (pl.empty()) { return; }
        civLog.push_back(pl);
        if (silentCiv) { return; }
        bool ok = true;
        if (pl.size() >= 4 && pl[0] == 0x1A && pl[1] == 0x13) {
            if (pl[2] == 0x01 && pl[3] == 0x00) { ivOn = false; }
            else if (pl[2] == 0x00 && pl[3] == 0x01) { modeOn = true; }
            else if (pl[2] == 0x00 && pl[3] == 0x00) { modeOn = false; ivOn = false; }
            else if (pl[2] == 0x02 && pl[3] == 0x01) {}
            else if (pl[2] == 0x01 && pl[3] == 0x01 && pl.size() == 6) { ivOn = modeOn; rateCode = pl[5]; startPhase(); }
            else { ok = false; }
        } else if (pl[0] == 0x05 && pl.size() == 6) {
            if (nakFrequency) { ok = false; }
            else {
                std::uint64_t f = 0, m = 1;
                for (int i = 0; i < 5; ++i) { f += m * (bcd2(pl[1 + i])); m *= 100; }
                freq = f;
            }
        } else if (pl[0] == 0x14 && pl.size() == 4 && pl[1] == 0x02) {
            rfGain = (pl[2] & 15) * 100 + bcd2(pl[3]);
        } else if (pl[0] == 0x11 && pl.size() == 2) {
            atten = bcd2(pl[1]);
        } else if (pl[0] == 0x12 && pl.size() == 2) {
            if (nakAntenna) { ok = false; } else { antenna = pl[1]; }
        } else if (pl[0] == 0x16 && pl.size() == 3 && pl[1] == 0x02) {
            preamp = pl[2];
        } else if (pl[0] == 0x16 && pl.size() == 3 && pl[1] == 0x65) {
        } else {
            ok = false;
        }
        if (garbageAck) { acks.push_back(Bytes{0x00, 0x01, 0x02, 0x03, 0x04, 0x05}); return; }
        acks.push_back(Bytes{0xFE, 0xFE, ic::kControllerAddress, ic::kRadioAddress, static_cast<std::uint8_t>(ok ? 0xFB : 0xFA), 0xFD});
    }

    void startPhase() {
        pending.clear();
        pairsEmitted = 0;
        phase = 0.0;
        if (startWithQ) {
            pending.push_back(0x34); pending.push_back(0x12);  // a stray Q-like sample before the first marker
        }
    }

    std::uint32_t markerEvery() const {
        for (const auto& r : ic::kRates) { if (r.code == rateCode) { return r.markerPairs; } }
        return 1024;
    }
    double rateHz() const {
        for (const auto& r : ic::kRates) { if (r.code == rateCode) { return r.hz; } }
        return 240000.0;
    }

    void put16(Bytes& b, std::int16_t v) { b.push_back(static_cast<std::uint8_t>(v & 0xFF)); b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF)); }

    void generate(std::size_t pairs) {
        const double fs = rateHz() * (1.0 + ppm * 1e-6);
        const double w = 2.0 * 3.14159265358979323846 * toneHz / fs;
        std::normal_distribution<double> nd(0.0, noise);
        const std::uint32_t every = markerEvery();
        for (std::size_t i = 0; i < pairs; ++i) {
            if (pairsEmitted % every == 0) {
                put16(pending, static_cast<std::int16_t>(0x8000));
                put16(pending, static_cast<std::int16_t>(0x8000));
                ++markersEmitted;
            }
            auto q = [&](double v) { const long s = std::clamp(std::lround(v * 32768.0), -32767L, 32767L); return static_cast<std::int16_t>(s); };
            const std::int16_t I = q(0.5 * std::cos(phase) + nd(rng));
            const std::int16_t Q = q(0.5 * std::sin(phase) + nd(rng));
            phase += w;
            if (phase > 6.283185307179586) { phase -= 6.283185307179586; }
            ++pairsEmitted;
            put16(pending, I);
            if (dropNextSample) { dropNextSample = false; continue; }  // the Q is lost
            put16(pending, Q);
        }
    }
};

class FakeUsb : public cascade::usb::UsbDevice {
public:
    FakeUsb(std::shared_ptr<World> w, bool loader, std::string path) : w_(std::move(w)), loader_(loader), path_(std::move(path)) {}
    int controlOut(std::uint8_t rt, std::uint8_t req, std::uint16_t value, std::uint16_t index, const std::uint8_t* d, std::size_t len, unsigned) override {
        std::lock_guard<std::mutex> lk(w_->mu);
        if (!loader_ || rt != 0x40 || req != 0xA0 || index != 0) { err_ = "fake: unexpected control transfer"; return -1; }
        w_->received.push_back({value, Bytes(d, d + len)});
        if (w_->rejectRecord > 0 && static_cast<int>(w_->received.size()) == w_->rejectRecord) { err_ = "fake: stall"; return -1; }
        if (value == 0xE600 && len == 1 && d[0] == 0) {  // release: the device drops off the bus
            w_->loaderPresent = false;
            w_->enumsAfterRelease = 0;
            return -1;  // the final transfer reports an error, as the README says
        }
        return static_cast<int>(len);
    }
    int controlIn(std::uint8_t, std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t*, std::size_t, unsigned) override { return -1; }
    bool beginBulkStream(std::uint8_t ep, std::size_t bytes, std::size_t count) override {
        std::lock_guard<std::mutex> lk(w_->mu);
        if (loader_ || ep != ic::kEpIq || bytes == 0 || count == 0) { err_ = "fake: bad stream"; return false; }
        w_->streamBegun = true;
        w_->streamEnded = false;
        streaming_ = true;
        return true;
    }
    int readBulk(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        std::unique_lock<std::mutex> lk(w_->mu);
        if (w_->failRead) { return -1; }
        if (!w_->ivOn || w_->noData) {
            w_->cv.wait_for(lk, std::chrono::milliseconds(std::min(timeoutMs, 20u)));
            return 0;
        }
        if (w_->pending.size() < 600) { w_->generate(2048); }
        std::uniform_int_distribution<std::size_t> pick(1, std::min<std::size_t>(cap, w_->pending.size()));
        const std::size_t n = std::min(pick(w_->rng), std::min<std::size_t>(w_->pending.size(), 3001));  // odd sizes included
        std::copy(w_->pending.begin(), w_->pending.begin() + static_cast<std::ptrdiff_t>(n), dst);
        w_->pending.erase(w_->pending.begin(), w_->pending.begin() + static_cast<std::ptrdiff_t>(n));
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        return static_cast<int>(n);
    }
    void endBulkStream() override {
        std::lock_guard<std::mutex> lk(w_->mu);
        streaming_ = false;
        w_->streamEnded = true;
    }
    bool streaming() const override { return streaming_; }
    bool resetPipe(std::uint8_t) override { return true; }
    int writeBulk(std::uint8_t ep, const std::uint8_t* d, std::size_t len, unsigned) override {
        std::lock_guard<std::mutex> lk(w_->mu);
        if (ep != ic::kEpCommand || loader_) { err_ = "fake: wrong endpoint"; return -1; }
        w_->civ(Bytes(d, d + len));
        w_->cv.notify_all();
        return static_cast<int>(len);
    }
    int readBulkOnce(std::uint8_t ep, std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        std::unique_lock<std::mutex> lk(w_->mu);
        ++w_->bulkOnceCalls;
        if (ep != ic::kEpAnswer) { err_ = "fake: wrong endpoint"; return -1; }
        if (w_->acks.empty()) {
            w_->cv.wait_for(lk, std::chrono::milliseconds(std::min(timeoutMs, 400u)), [&] { return !w_->acks.empty(); });
        }
        if (w_->acks.empty()) { return 0; }
        const Bytes a = w_->acks.front();
        w_->acks.pop_front();
        const std::size_t n = std::min(cap, a.size());
        std::copy(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(n), dst);
        return static_cast<int>(n);
    }
    const std::string& path() const override { return path_; }
    const std::string& lastError() const override { return err_; }

private:
    std::shared_ptr<World> w_;
    bool loader_;
    std::string path_, err_;
    bool streaming_ = false;
};

void wire(Icr8600Source& s, const std::shared_ptr<World>& w) {
    s.setTransportForTest(
        [w]() {
            std::lock_guard<std::mutex> lk(w->mu);
            std::vector<cascade::usb::UsbDeviceInfo> v;
            if (w->enumsAfterRelease >= 0) {
                ++w->enumsAfterRelease;
                if (!w->neverReenumerate && w->enumsAfterRelease >= w->appearAfterEnums) { w->streamPresent = true; }
            }
            if (w->loaderPresent) { v.push_back({ic::kVid, ic::kPidLoader, "loader", "", "IC-R8600 loader"}); }
            if (w->streamPresent) { v.push_back({ic::kVid, ic::kPidStream, "stream", "", "IC-R8600 I/Q"}); }
            return v;
        },
        [w](const std::string& path, std::string&) -> std::unique_ptr<cascade::usb::UsbDevice> {
            return std::make_unique<FakeUsb>(w, path == "loader", path);
        });
}

std::shared_ptr<World> streamWorld() {
    auto w = std::make_shared<World>();
    w->loaderPresent = false;
    w->streamPresent = true;
    return w;
}

std::vector<Cx> readSome(Icr8600Source& s, std::size_t n, int ms) {
    std::vector<Cx> v;
    std::vector<Cx> tmp(8192);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (v.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t got = s.read(tmp.data(), tmp.size());
        v.insert(v.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return v;
}
double toneHzOf(const std::vector<Cx>& x, double fs) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t i = 1; i < x.size(); ++i) { acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1])); }
    return std::arg(acc) * fs / (2.0 * 3.14159265358979323846);
}
bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// ---- protocol literals ------------------------------------------------------------------------------------------
void testLiterals() {
    // SoapyICR8600 arrays (WinUSBDevice.cpp)
    CHECK(ic::cmdIqModeOn() == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x1A, 0x13, 0x00, 0x01, 0xFD, 0xFF}));
    CHECK(ic::cmdIqModeOff() == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x1A, 0x13, 0x00, 0x00, 0xFD, 0xFF}));
    CHECK(ic::cmdIqOn(0x01) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x1A, 0x13, 0x01, 0x01, 0x00, 0x01, 0xFD, 0xFF}));
    CHECK(ic::cmdIqOn(0x06) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x1A, 0x13, 0x01, 0x01, 0x00, 0x06, 0xFD, 0xFF}));
    CHECK(ic::cmdRfGain(255) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x14, 0x02, 0x02, 0x55, 0xFD, 0xFF}));
    CHECK(ic::cmdRfGain(0) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x14, 0x02, 0x00, 0x00, 0xFD, 0xFF}));
    CHECK(ic::cmdRfGain(100) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x14, 0x02, 0x01, 0x00, 0xFD, 0xFF}));
    CHECK(ic::cmdAttenuator(10) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x11, 0x10, 0xFD, 0xFF}));
    CHECK(ic::cmdAttenuator(30) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x11, 0x30, 0xFD, 0xFF}));
    CHECK(ic::cmdAntenna(1) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x12, 0x01, 0xFD, 0xFF}));
    CHECK(ic::cmdPreamp(true) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x16, 0x02, 0x01, 0xFD}));  // 8 bytes: even, no pad
    CHECK(ic::cmdFrequency(100000000) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFD, 0xFF}));
    CHECK(ic::cmdFrequency(1090000000) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x05, 0x00, 0x00, 0x00, 0x90, 0x10, 0xFD, 0xFF}));
    CHECK(ic::cmdFrequency(14074000) == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x05, 0x00, 0x40, 0x07, 0x14, 0x00, 0xFD, 0xFF}));
    // the README's start-up list
    CHECK(ic::cmdIqOff() == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x1A, 0x13, 0x01, 0x00, 0xFD, 0xFF}));
    CHECK(ic::cmdIqPrepare() == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x1A, 0x13, 0x02, 0x01, 0xFD, 0xFF}));
    CHECK(ic::cmdIpPlusOff() == (Bytes{0xFE, 0xFE, 0x96, 0xE0, 0x16, 0x65, 0x00, 0xFD}));
    // refusals of the builders
    CHECK(ic::cmdRfGain(256).empty() && ic::cmdAttenuator(5).empty() && ic::cmdAttenuator(40).empty() && ic::cmdAntenna(3).empty());
    CHECK(ic::cmdFrequency(10000000000ull).empty() && !ic::cmdFrequency(9999999999ull).empty() && !ic::cmdFrequency(0).empty());
    CHECK(ic::civFrame({}).empty() && ic::civFrame(Bytes(33, 1)).empty());
    // every frame is even in length
    for (unsigned n = 1; n <= 32; ++n) { CHECK(ic::civFrame(Bytes(n, 0x01)).size() % 2 == 0); }
    // BCD
    std::uint8_t b[5];
    CHECK(ic::frequencyBcd(9999999999ull, b) && b[0] == 0x99 && b[4] == 0x99);
    CHECK(ic::frequencyBcd(123456789ull, b) && b[0] == 0x89 && b[1] == 0x67 && b[2] == 0x45 && b[3] == 0x23 && b[4] == 0x01);
    CHECK(!ic::frequencyBcd(10000000000ull, b));
    // rates
    std::uint8_t code = 0;
    for (const auto& r : ic::kRates) { CHECK(ic::rateCodeFor(r.hz, code) && code == r.code); }
    CHECK(!ic::rateCodeFor(250000, code) && !ic::rateCodeFor(0, code) && !ic::rateCodeFor(-1, code) && !ic::rateCodeFor(5120001, code));
    CHECK(ic::kRates[5].code == 0x01 && ic::kRates[5].hz == 5120000 && ic::kRates[0].code == 0x06 && ic::kRates[0].hz == 240000);
}

void testAnswers() {
    const Bytes ok{0xFE, 0xFE, 0xE0, 0x96, 0xFB, 0xFD};
    const Bytes ng{0xFE, 0xFE, 0xE0, 0x96, 0xFA, 0xFD};
    const Bytes okPad{0xFE, 0xFE, 0xE0, 0x96, 0xFB, 0xFD, 0xFF, 0xFF};
    CHECK(ic::parseAnswer(ok.data(), ok.size()) == ic::Answer::Ok);
    CHECK(ic::parseAnswer(ng.data(), ng.size()) == ic::Answer::Refused);
    CHECK(ic::parseAnswer(okPad.data(), okPad.size()) == ic::Answer::Ok);
    std::vector<std::uint8_t> body;
    const Bytes rb{0xFE, 0xFE, 0xE0, 0x96, 0x14, 0x02, 0x02, 0x55, 0xFD};
    CHECK(ic::parseAnswer(rb.data(), rb.size(), &body) == ic::Answer::Other && body == (Bytes{0x14, 0x02, 0x02, 0x55}));
    for (const Bytes& g : {Bytes{}, Bytes{0xFE}, Bytes{0xFE, 0xFE, 0xE0, 0x96, 0xFB}, Bytes{0xFE, 0xFE, 0x96, 0xE0, 0xFB, 0xFD},
                           Bytes{0xFE, 0xFE, 0xE0, 0x96, 0xFD, 0xFD}, Bytes{0x00, 0xFE, 0xE0, 0x96, 0xFB, 0xFD}, Bytes{0xFE, 0xFE, 0xE0, 0x97, 0xFB, 0xFD}}) {
        CHECK(ic::parseAnswer(g.data(), g.size()) == ic::Answer::Garbage);
    }
    CHECK(ic::parseAnswer(nullptr, 10) == ic::Answer::Garbage);
}

// ---- the aligner ------------------------------------------------------------------------------------------
// a stream of pairs with a marker every `every` pairs; I = pair index (as a small int), Q = -index
Bytes streamOf(int pairs, int every, int dropQAt = -1) {
    Bytes b;
    auto put = [&b](std::int16_t v) { b.push_back(static_cast<std::uint8_t>(v & 0xFF)); b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF)); };
    for (int i = 0; i < pairs; ++i) {
        if (i % every == 0) { put(static_cast<std::int16_t>(0x8000)); put(static_cast<std::int16_t>(0x8000)); }
        put(static_cast<std::int16_t>(i + 1));
        if (i == dropQAt) { continue; }
        put(static_cast<std::int16_t>(-(i + 1)));
    }
    return b;
}

void testAligner() {
    const int every = 100;
    const Bytes s = streamOf(1000, every);
    // whole at once
    {
        ic::IqAligner a;
        std::vector<Cx> out;
        a.feed(s.data(), s.size(), out);
        CHECK(out.size() == 1000 && a.pairs() == 1000 && a.markers() == 10 && a.realigned() == 0 && a.locked());
        for (std::size_t i = 0; i < out.size(); ++i) {
            CHECK(out[i].real() == static_cast<float>(i + 1) / 32768.0f && out[i].imag() == -static_cast<float>(i + 1) / 32768.0f);
        }
    }
    // the same result for every chunking, including one byte at a time and odd chunks
    for (std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{101}, std::size_t{4093}}) {
        ic::IqAligner a;
        std::vector<Cx> out;
        for (std::size_t p = 0; p < s.size(); p += chunk) { a.feed(s.data() + p, std::min(chunk, s.size() - p), out); }
        CHECK(out.size() == 1000 && a.markers() == 10 && a.realigned() == 0);
        CHECK(out.front().real() == 1.0f / 32768.0f && out.back().imag() == -1000.0f / 32768.0f);
    }
    // a stream that starts on a Q (a stray sample before the first marker): nothing before the marker is trusted
    {
        Bytes t{0x34, 0x12};
        t.insert(t.end(), s.begin(), s.end());
        ic::IqAligner a;
        std::vector<Cx> out;
        a.feed(t.data(), t.size(), out);
        CHECK(out.size() == 1000 && out.front().real() == 1.0f / 32768.0f && a.realigned() == 0);
    }
    // a lost Q: the dangling I is completed with Q = 0, counted once, and every later pair is right
    {
        const Bytes d = streamOf(1000, every, 250);
        ic::IqAligner a;
        std::vector<Cx> out;
        a.feed(d.data(), d.size(), out);
        // until the next marker the pairs are shifted by one sample: (I251, I252), (Q252, I253), ...; at the marker the
        // dangling Q300 is completed with zero, and everything after is right again
        CHECK(a.realigned() == 1 && out.size() == 1000);
        CHECK(out[249].real() == 250.0f / 32768.0f && out[249].imag() == -250.0f / 32768.0f);
        CHECK(out[250].real() == 251.0f / 32768.0f && out[250].imag() == 252.0f / 32768.0f);
        CHECK(out[299].real() == -300.0f / 32768.0f && out[299].imag() == 0.0f);
        CHECK(out[300].real() == 301.0f / 32768.0f && out[300].imag() == -301.0f / 32768.0f);
        CHECK(out[999].real() == 1000.0f / 32768.0f && out[999].imag() == -1000.0f / 32768.0f);
    }
    // no sample ever equals the marker value, and the empty input does nothing
    {
        ic::IqAligner a;
        std::vector<Cx> out;
        a.feed(nullptr, 0, out);
        CHECK(out.empty() && !a.locked());
    }
    // data before any marker is dropped entirely
    {
        Bytes noMarker;
        for (int i = 0; i < 200; ++i) { noMarker.push_back(static_cast<std::uint8_t>(i)); noMarker.push_back(0x01); }
        ic::IqAligner a;
        std::vector<Cx> out;
        a.feed(noMarker.data(), noMarker.size(), out);
        CHECK(out.empty() && !a.locked());
    }
    // fuzz: random bytes with markers sprinkled in never crash and never emit more pairs than samples/2
    std::mt19937 rng(99);
    for (int r = 0; r < 2000; ++r) {
        Bytes junk(rng() % 3000);
        for (auto& b : junk) { b = static_cast<std::uint8_t>(rng()); }
        for (std::size_t i = 0; i + 4 < junk.size(); i += 1 + rng() % 400) { junk[i] = 0; junk[i + 1] = 0x80; junk[i + 2] = 0; junk[i + 3] = 0x80; }
        ic::IqAligner a;
        std::vector<Cx> out;
        std::size_t p = 0;
        while (p < junk.size()) { const std::size_t n = std::min<std::size_t>(1 + rng() % 500, junk.size() - p); a.feed(junk.data() + p, n, out); p += n; }
        CHECK(out.size() <= junk.size() / 2 + 2);
        for (const Cx& c : out) { CHECK(c.real() > -1.0f && c.imag() > -1.0f); }
    }
}

// ---- the firmware file -------------------------------------------------------------------------------------------
void testFirmware() {
    const auto fw = madeUpFirmware();
    std::vector<ic::FwRecord> got;
    std::string err;
    CHECK(ic::parseFirmware(csptFile(fw), got, err) && same(got, fw) && err.empty());
    CHECK(ic::parseFirmware(bytesOf(jsonFile(fw)), got, err) && same(got, fw));
    CHECK(ic::parseFirmware(bytesOf("  \n\t" + jsonFile(fw)), got, err) && same(got, fw));
    // refusals, each with a sentence and nothing left in `got`
    auto refuse = [&](const Bytes& f, const char* word) {
        got.assign(3, {});
        err.clear();
        const bool ok = ic::parseFirmware(f, got, err);
        CHECK(!ok);
        CHECK(got.empty());
        if (err.find(word) == std::string::npos) { std::printf("expected '%s' in: %s\n", word, err.c_str()); }
        CHECK(err.find(word) != std::string::npos);
    };
    refuse({}, "empty");
    refuse({'C', 'S', 'P'}, "too short");
    refuse(bytesOf("hello, this is not a firmware file"), "neither");
    refuse(bytesOf("[1, 2, 3]"), "{\"wValue\"");
    refuse(bytesOf("[{\"wValue\": 0, \"data\": \"zz\"}]"), "hexadecimal");
    refuse(bytesOf("[{\"wValue\": 0, \"data\": \"abc\"}]"), "impossible");
    refuse(bytesOf("[{\"wValue\": -1, \"data\": \"ab\"}]"), "impossible");
    refuse(bytesOf("[{\"wValue\": 70000, \"data\": \"ab\"}]"), "impossible");
    refuse(bytesOf("[{\"wValue\": 0}]"), "{\"wValue\"");
    refuse(bytesOf("[{\"wValue\": 0, \"data\": \"ab\""), "could not be read");
    refuse(bytesOf("[]"), "too short");
    refuse(bytesOf("[ ]  "), "no records");
    {   // record outside the loadable space
        auto bad = fw;
        bad[1].wValue = 0x7F00;
        refuse(csptFile(bad), "outside");
        bad = fw;
        bad[1].wValue = 0x3FFF;  // 7 bytes starting at 0x3FFF run past the 16 KiB RAM
        refuse(bytesOf(jsonFile(bad)), "outside");
        bad = fw;
        bad[3].wValue = 0xE1FE;  // 3 bytes run past the 512-byte scratch RAM
        refuse(csptFile(bad), "outside");
        bad = fw;
        bad.insert(bad.begin() + 1, ic::FwRecord{0xE601, {0x00}});  // another CPU-control-looking register
        refuse(csptFile(bad), "outside");
        bad = fw;
        bad[2].wValue = 0xFFFF;
        refuse(bytesOf(jsonFile(bad)), "outside");
    }
    {   // framing
        auto b = csptFile(fw);
        b.push_back('X');
        refuse(b, "damaged");
        b = csptFile(fw);
        b.resize(b.size() - 1);
        refuse(b, "runs past");  // the last record's header is intact but its payload is short
    }
    {   // wrong request type / request in a record
        Bytes f = csptRecord(0xC0, 0xA0, 0xE600, {0x01});
        Bytes x = csptRecord(0x40, 0xA0, 0xE600, {0x00});
        f.insert(f.end(), x.begin(), x.end());
        refuse(f, "vendor request 0xA0");
        f = csptRecord(0x40, 0xA3, 0xE600, {0x01});
        x = csptRecord(0x40, 0xA0, 0xE600, {0x00});
        f.insert(f.end(), x.begin(), x.end());
        refuse(f, "vendor request 0xA0");
    }
    {   // payload length lies
        Bytes f = csptRecord(0x40, 0xA0, 0xE600, {0x01});
        f[28] = 0xFF; f[29] = 0xFF; f[30] = 0xFF; f[31] = 0x7F;
        refuse(f, "runs past");
    }
    {   // must begin with the reset hold and end with the release
        auto bad = fw;
        bad.front().data = {0x00};
        refuse(csptFile(bad), "hold");
        bad = fw;
        bad.back().data = {0x01};
        refuse(csptFile(bad), "releasing");
        bad = fw;
        bad.pop_back();
        refuse(csptFile(bad), "releasing");
        bad = fw;
        bad.erase(bad.begin());
        refuse(csptFile(bad), "hold");
    }
    {   // size limits
        std::vector<ic::FwRecord> many = fw;
        many.insert(many.begin() + 1, 300, ic::FwRecord{0x0000, {0x01}});
        refuse(csptFile(many), "more than 256");
        refuse(bytesOf(jsonFile(many)), "more than 256");
        auto big = fw;
        big[1].data.assign(20000, 0x11);
        big[2].data.assign(20000, 0x22);
        big[3].data.assign(20000, 0x33);
        big[3].wValue = 0xE000;
        refuse(csptFile(big), "outside");  // 20000 bytes at 0xE000 does not fit the scratch RAM
        auto total = fw;
        total.insert(total.begin() + 1, 5, ic::FwRecord{0x0000, Bytes(16000, 0x44)});
        refuse(csptFile(total), "64 KiB");
    }
    // fuzz: random bytes, and bit-flipped good files, never crash and never produce records on refusal
    std::mt19937 rng(31337);
    const Bytes good = csptFile(fw);
    const Bytes goodJ = bytesOf(jsonFile(fw));
    int accepted = 0, refused = 0;
    for (int r = 0; r < 4000; ++r) {
        Bytes f = (r % 3 == 0) ? good : (r % 3 == 1 ? goodJ : Bytes(rng() % 600));
        if (r % 3 == 2) { for (auto& b : f) { b = static_cast<std::uint8_t>(rng()); } }
        else { for (int k = 0; k < 1 + static_cast<int>(rng() % 4); ++k) { f[rng() % f.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); } }
        std::vector<ic::FwRecord> o;
        std::string e;
        if (ic::parseFirmware(f, o, e)) {
            ++accepted;
            CHECK(!o.empty() && o.front().wValue == 0xE600 && o.back().wValue == 0xE600);
            for (const auto& rec : o) { CHECK(ic::firmwareAddressOk(rec.wValue, rec.data.size())); }
        } else {
            ++refused;
            CHECK(o.empty() && !e.empty());
        }
    }
    CHECK(accepted > 100 && refused > 1000);
    std::printf("icr8600 firmware fuzz: %d accepted, %d refused of 4000\n", accepted, refused);
}

void testDefaultReadBulkOnce() {
    cascade::usb::FakeUsbDevice f;
    std::uint8_t b[8];
    CHECK(f.readBulkOnce(0x88, b, sizeof(b), 10) < 0);  // a transport without it refuses
}

// ---- the source -------------------------------------------------------------------------------------------------
void testOpenDirect() {
    auto w = streamWorld();
    Icr8600Source s;
    wire(s, w);
    CHECK(s.open(""));
    CHECK(s.isOpen() && std::string(s.name()).find("IC-R8600") != std::string::npos);
    {
        std::lock_guard<std::mutex> lk(w->mu);
        CHECK(w->streamBegun && w->modeOn && !w->ivOn);
        CHECK(w->received.empty());  // no firmware was touched
        CHECK(w->civLog.size() == 2 && w->civLog[0] == (Bytes{0x1A, 0x13, 0x01, 0x00}) && w->civLog[1] == (Bytes{0x1A, 0x13, 0x00, 0x01}));
    }
    CHECK(s.sampleRateHz() == 1920000.0 && s.centerFrequencyHz() == 100000000.0);
    CHECK(s.gainDb("RF gain") == 255.0 && s.gainDb("Attenuator") == 0.0 && s.gainDb("Pre-amp") == 1.0 && s.antenna() == "ANT 1");
    const auto g = s.gains();
    CHECK(g.size() == 3 && g[0].name == "RF gain" && g[0].unit == GainUnit::Steps && g[0].maxDb == 255.0 && g[1].name == "Attenuator" &&
          g[1].minDb == -30.0 && g[2].name == "Pre-amp");
    const auto r = s.supportedSampleRatesHz();
    CHECK(r.size() == 6 && r.front() == 240000.0 && r.back() == 5120000.0);
    CHECK(s.antennas().size() == 3);
    double lo, hi;
    CHECK(!s.frequencyRangeHz(lo, hi));
    s.closeDevice();
    CHECK(!s.isOpen());
    {
        std::lock_guard<std::mutex> lk(w->mu);
        CHECK(w->streamEnded && !w->modeOn);
        CHECK(w->civLog.back() == (Bytes{0x1A, 0x13, 0x00, 0x00}));
    }
}

void testOpenFromLoader() {
    const auto fw = madeUpFirmware();
    const std::string spt = writeTemp("fw.spt", csptFile(fw));
    const std::string js = writeTemp("fw.json", bytesOf(jsonFile(fw)));
    for (const std::string& path : {spt, js}) {
        auto w = std::make_shared<World>();
        Icr8600Source s;
        wire(s, w);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(s.open("firmware=" + path));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 4000);
        {
            std::lock_guard<std::mutex> lk(w->mu);
            CHECK(same(w->received, fw));
            CHECK(w->streamBegun && w->modeOn);
        }
        s.closeDevice();
    }
    std::remove(spt.c_str());
    std::remove(js.c_str());
}

void testOpenRefusals() {
    {   // nothing connected
        auto w = std::make_shared<World>();
        w->loaderPresent = false;
        Icr8600Source s;
        wire(s, w);
        CHECK(!s.open("") && std::string(s.lastError()).find("no IC-R8600 I/Q port") != std::string::npos);
        CHECK(std::string(s.lastError()).find("WinUSB") != std::string::npos || std::string(s.lastError()).find("usbfs") != std::string::npos);
    }
    {   // loader without firmware: tells the user what to do, touches nothing
        auto w = std::make_shared<World>();
        Icr8600Source s;
        wire(s, w);
        CHECK(!s.open(""));
        const std::string e = s.lastError();
        CHECK(e.find("Icom's firmware") != std::string::npos && e.find("USB I/Q Package") != std::string::npos && e.find("does not include or download") != std::string::npos);
        std::lock_guard<std::mutex> lk(w->mu);
        CHECK(w->received.empty());
    }
    {   // a firmware file that is missing or bad: nothing is sent to the device
        auto w = std::make_shared<World>();
        Icr8600Source s;
        wire(s, w);
        CHECK(!s.open("firmware=eor-no-such-file.spt") && std::string(s.lastError()).find("could not be opened") != std::string::npos);
        const std::string bad = writeTemp("bad.spt", bytesOf("not firmware"));
        CHECK(!s.open("firmware=" + bad) && std::string(s.lastError()).find("neither") != std::string::npos);
        std::remove(bad.c_str());
        auto fw = madeUpFirmware();
        fw[1].wValue = 0x9000;
        const std::string evil = writeTemp("evil.spt", csptFile(fw));
        CHECK(!s.open("firmware=" + evil) && std::string(s.lastError()).find("outside") != std::string::npos);
        std::remove(evil.c_str());
        std::lock_guard<std::mutex> lk(w->mu);
        CHECK(w->received.empty());
    }
    {   // the loader refuses a record in the middle
        auto w = std::make_shared<World>();
        w->rejectRecord = 3;
        const std::string spt = writeTemp("fw2.spt", csptFile(madeUpFirmware()));
        Icr8600Source s;
        wire(s, w);
        CHECK(!s.open("firmware=" + spt) && std::string(s.lastError()).find("record 3 of 5") != std::string::npos);
        std::remove(spt.c_str());
    }
    {   // the radio never comes back as 0023
        auto w = std::make_shared<World>();
        w->neverReenumerate = true;
        const std::string spt = writeTemp("fw3.spt", csptFile(madeUpFirmware()));
        Icr8600Source s;
        wire(s, w);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.open("firmware=" + spt) && std::string(s.lastError()).find("did not come back") != std::string::npos);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms >= 4500 && ms < 12000);
        std::remove(spt.c_str());
    }
    {   // a radio that answers no CI-V: bounded failure
        auto w = streamWorld();
        w->silentCiv = true;
        Icr8600Source s;
        wire(s, w);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.open("timeout=300"));
        CHECK(std::string(s.lastError()).find("did not answer") != std::string::npos && std::string(s.lastError()).find("IC-R8600 I/Q port") != std::string::npos);
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 3000);
        CHECK(!s.isOpen());
    }
    {   // answers that are not CI-V acknowledgements
        auto w = streamWorld();
        w->garbageAck = true;
        Icr8600Source s;
        wire(s, w);
        CHECK(!s.open("") && std::string(s.lastError()).find("not an acknowledgement") != std::string::npos);
    }
    {   // bad arguments
        auto w = streamWorld();
        Icr8600Source s;
        wire(s, w);
        for (const char* a : {"timeout=5", "timeout=x", "timeout=20000"}) { CHECK(!s.open(a)); }
        CHECK(s.open("timeout=2000"));
    }
}

void testStartSequenceAndStream() {
    auto w = streamWorld();
    w->toneHz = 20000.0;
    w->ppm = 3000.0;
    Icr8600Source s;
    wire(s, w);
    CHECK(s.open(""));
    CHECK(s.setSampleRateHz(960000.0) && s.setCenterFrequencyHz(14074000.0));
    CHECK(s.setGainDb("RF gain", 200) && s.setGainDb("Attenuator", -10.0) && s.setGainDb("Pre-amp", 0.0) && s.setAntenna("ANT 2"));
    {
        std::lock_guard<std::mutex> lk(w->mu);
        w->civLog.clear();
    }
    CHECK(s.start() && s.running());
    {
        std::lock_guard<std::mutex> lk(w->mu);
        // the README's order: frequency, attenuator, antenna, RF gain, pre-amp, IP+, prepare, ON(rate)
        const std::vector<Bytes> want{{0x05, 0x00, 0x40, 0x07, 0x14, 0x00}, {0x11, 0x10}, {0x12, 0x01}, {0x14, 0x02, 0x02, 0x00},
                                      {0x16, 0x02, 0x00}, {0x16, 0x65, 0x00}, {0x1A, 0x13, 0x02, 0x01}, {0x1A, 0x13, 0x01, 0x01, 0x00, 0x04}};
        CHECK(w->civLog == want);
        CHECK(w->ivOn && w->freq == 14074000 && w->rfGain == 200 && w->atten == 10 && w->antenna == 1 && w->preamp == 0 && w->rateCode == 0x04);
    }
    CHECK(!s.setSampleRateHz(480000.0) && std::string(s.lastError()).find("stop") != std::string::npos);
    const std::vector<Cx> x = readSome(s, 60000, 8000);
    CHECK(x.size() >= 60000);
    // the radio's clock is 3000 ppm fast; at the nominal rate the tone reads 3000 ppm low (60 Hz)
    const double want = 20000.0 / (1.0 + 3000e-6);
    CHECK(std::fabs(toneHzOf(x, 960000.0) - want) < 25.0);
    CHECK(toneHzOf(x, 960000.0) > 0.0);  // I and Q are not swapped
    double p = 0;
    for (const Cx& c : x) { p += std::norm(c); CHECK(c.real() > -1.0f && c.imag() > -1.0f); }
    p /= static_cast<double>(x.size());
    CHECK(p > 0.2 && p < 0.35);
    CHECK(s.syncMarkers() >= 20 && s.realignments() == 0 && s.samplesReceived() > 0);
    // live changes while running
    CHECK(s.setCenterFrequencyHz(1090000000.0) && s.setGainDb("RF gain", 255.0) && s.setGainDb("Pre-amp", 1.0) && s.setGainDb("Attenuator", -30.0));
    {
        std::lock_guard<std::mutex> lk(w->mu);
        CHECK(w->freq == 1090000000ull && w->rfGain == 255 && w->preamp == 1 && w->atten == 30);
    }
    CHECK(!s.setAntenna("ANT 3"));  // above HF
    s.stop();
    CHECK(!s.running());
    {
        std::lock_guard<std::mutex> lk(w->mu);
        CHECK(!w->ivOn && w->civLog.back() == (Bytes{0x1A, 0x13, 0x01, 0x00}) && w->modeOn);
    }
    // a second run: the aligner starts unlocked again
    w->startWithQ = true;
    CHECK(s.setSampleRateHz(240000.0) && s.start());
    const std::vector<Cx> y = readSome(s, 20000, 8000);
    CHECK(y.size() >= 20000);
    CHECK(std::fabs(toneHzOf(y, 240000.0) - 20000.0 / (1.0 + 3000e-6)) < 25.0 && s.realignments() == 0);
    s.closeDevice();
    {
        std::lock_guard<std::mutex> lk(w->mu);
        CHECK(!w->ivOn && !w->modeOn && w->streamEnded);
    }
}

void testSettingsValidation() {
    auto w = streamWorld();
    Icr8600Source s;
    wire(s, w);
    CHECK(!s.setSampleRateHz(240000.0) && !s.setCenterFrequencyHz(1e6) && !s.setGainDb("RF gain", 1.0) && !s.setAntenna("ANT 1") && !s.start());
    CHECK(s.open(""));
    for (double bad : {0.0, 48000.0, 250000.0, -1.0, std::nan(""), 5120001.0}) { CHECK(!s.setSampleRateHz(bad)); }
    for (const auto& r : ic::kRates) { CHECK(s.setSampleRateHz(r.hz) && s.sampleRateHz() == r.hz); }
    for (double bad : {-1.0, std::nan(""), HUGE_VAL, 1e10}) { CHECK(!s.setCenterFrequencyHz(bad)); }
    CHECK(s.setCenterFrequencyHz(0.0) && s.setCenterFrequencyHz(9999999999.0));
    for (double bad : {-1.0, 256.0, 1.5, std::nan("")}) { CHECK(!s.setGainDb("RF gain", bad)); }
    CHECK(s.setGainDb("RF gain", 0.0) && s.setGainDb("RF gain", 255.0));
    for (double bad : {5.0, -5.0, -40.0, 10.0}) { CHECK(!s.setGainDb("Attenuator", bad)); }
    for (double bad : {2.0, -1.0, 0.5}) { CHECK(!s.setGainDb("Pre-amp", bad)); }
    CHECK(!s.setGainDb("LNA", 0.0) && !s.setAntenna("ANT 4") && !s.setAntenna(""));
    CHECK(!s.autoGainSupported() && !s.setAutoGain(true));
    CHECK(s.setCenterFrequencyHz(14000000.0) && s.setAntenna("ANT 3") && s.antenna() == "ANT 3");
    CHECK(s.setCenterFrequencyHz(100000000.0) && !s.setAntenna("ANT 2") && s.setAntenna("ANT 1"));
}

void testRefusedCommands() {
    {   // an optional command the radio refuses (the antenna outside HF) is counted and tolerated
        auto w = streamWorld();
        w->nakAntenna = true;
        Icr8600Source s;
        wire(s, w);
        CHECK(s.open("") && s.start());
        CHECK(s.refusedOptional() >= 1);
        s.closeDevice();
    }
    {   // a mandatory one (tuning) is not
        auto w = streamWorld();
        w->nakFrequency = true;
        Icr8600Source s;
        wire(s, w);
        CHECK(s.open(""));
        CHECK(!s.start() && std::string(s.lastError()).find("refused the command for tuning") != std::string::npos);
        CHECK(!s.running());
        std::lock_guard<std::mutex> lk(w->mu);
        CHECK(!w->ivOn);
    }
}

void testFaults() {
    {   // the radio stops sending I/Q
        auto w = streamWorld();
        Icr8600Source s;
        wire(s, w);
        CHECK(s.open("timeout=300") && s.start());
        {
            std::lock_guard<std::mutex> lk(w->mu);
            w->noData = true;
        }
        CHECK(waitFor([&] { return s.faulted(); }, 8000));
        CHECK(s.faultedWhile().find("stopped sending") != std::string::npos);
        CHECK(!s.start());
        s.closeDevice();
    }
    {   // the bulk pipe fails (cable pulled)
        auto w = streamWorld();
        Icr8600Source s;
        wire(s, w);
        CHECK(s.open("") && s.start());
        {
            std::lock_guard<std::mutex> lk(w->mu);
            w->failRead = true;
        }
        CHECK(waitFor([&] { return s.faulted(); }, 4000));
        CHECK(s.faultedWhile().find("failed") != std::string::npos);
        s.closeDevice();
    }
    {   // lost data: the stream re-locks and the loss is counted
        auto w = streamWorld();
        Icr8600Source s;
        wire(s, w);
        CHECK(s.open("") && s.setSampleRateHz(480000.0) && s.start());
        {
            std::lock_guard<std::mutex> lk(w->mu);
            w->dropNextSample = true;
        }
        const std::vector<Cx> x = readSome(s, 30000, 8000);
        CHECK(x.size() >= 30000);
        CHECK(waitFor([&] { return s.realignments() >= 1; }, 4000));
        CHECK(s.realignments() == 1);
        s.closeDevice();
    }
}

void testCycles() {
    auto w = streamWorld();
    Icr8600Source s;
    wire(s, w);
    CHECK(s.open(""));
    for (int i = 0; i < 40; ++i) {
        CHECK(s.start());
        if (i % 10 == 0) { CHECK(readSome(s, 4000, 4000).size() >= 4000); }
        s.stop();
        CHECK(!s.running());
    }
    s.closeDevice();
    for (int i = 0; i < 40; ++i) {
        auto w2 = streamWorld();
        Icr8600Source s2;
        wire(s2, w2);
        CHECK(s2.open(""));
        s2.closeDevice();
        CHECK(!s2.isOpen());
        std::lock_guard<std::mutex> lk(w2->mu);
        CHECK(w2->streamEnded);
    }
    CHECK(NetSampleSource::readersAbandoned() == 0);
}

}  // namespace

int main() {
    testLiterals();
    testAnswers();
    testAligner();
    testFirmware();
    testDefaultReadBulkOnce();
    testOpenDirect();
    testOpenFromLoader();
    testOpenRefusals();
    testStartSequenceAndStream();
    testSettingsValidation();
    testRefusedCommands();
    testFaults();
    testCycles();
    return testSummary("test_icr8600_source");
}
