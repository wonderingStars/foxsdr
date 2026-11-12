// test_hackrf_sweep.cpp - HackRF sweep mode (src/source/hackrf_sweep.*) against a STAND-IN HackRF behind
// cascade::usb::UsbDevice. The INIT_SWEEP request bytes and the block header are written out literally from
// hackrf_init_sweep and hackrf.h (BSD-3 headers, read 2026-11-12). What the stand-in does with them is this file's
// own reading, so the tests prove FoxSDR's code is consistent with that reading - not that a HackRF behaves so.
// UNPROVEN on a real HackRF One.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "source/hackrf_protocol.hpp"
#include "source/hackrf_sweep.hpp"
#include "test_check.hpp"

using namespace cascade::source;
namespace hs = cascade::source::hackrf_sweep;
namespace hr = cascade::source::hackrf;
using Bytes = std::vector<std::uint8_t>;
using Cx = std::complex<float>;

namespace {

constexpr double kPi = 3.14159265358979323846;

void putHeader(Bytes& b, std::uint64_t hz) {
    b.push_back(0x7F);
    b.push_back(0x7F);
    for (int i = 0; i < 8; ++i) { b.push_back(static_cast<std::uint8_t>(hz >> (8 * i))); }
}

// One block of `blockBytes`: header, then 8-bit I/Q of a tone at `toneRelHz` from the centre plus noise.
Bytes makeBlock(std::uint64_t centreHz, double toneRelHz, double fs, double amp, double dcOffset, std::mt19937& rng, std::size_t blockBytes = 16384) {
    Bytes b;
    putHeader(b, centreHz);
    if (std::fabs(toneRelHz) > 0.45 * fs) { amp = 0.0; }  // outside the analogue filter: no alias of a far tone
    std::normal_distribution<double> nd(0.0, 0.01);
    const std::size_t total = (blockBytes - 10) / 2;
    for (std::size_t i = 0; i < total; ++i) {
        // the header replaced the first five samples: the tone's phase counts from sample 5
        const double ph = 2.0 * kPi * toneRelHz * static_cast<double>(i + 5) / fs;
        auto q = [](double v) { return static_cast<std::uint8_t>(static_cast<std::int8_t>(std::clamp(std::lround(v * 128.0), -127L, 127L))); };
        b.push_back(q(amp * std::cos(ph) + dcOffset + nd(rng)));
        b.push_back(q(amp * std::sin(ph) + nd(rng)));
    }
    return b;
}

// ---- the stand-in HackRF ----------------------------------------------------------------------------------------------
struct Radio {
    std::mutex mu;
    struct Ctl { bool in; std::uint8_t request; std::uint16_t value, index; Bytes data; };
    std::vector<Ctl> log;
    bool refuseLna = false, failInit = false, failRead = false, noBoard = false, failBegin = false;
    bool sweeping = false, streamBegun = false, streamEnded = false;
    int endOrder = 0, modeOffOrder = 0, orderCounter = 0;
    // sweep state from the init request
    std::uint32_t step = 0, offset = 0, numBytes = 0;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> ranges;
    double fs = 20e6;
    double toneHz = 2437.5e6;
    double dc = 0.0;
    std::mt19937 rng{2024};
    // generator
    std::size_t rangeIdx = 0;
    std::uint64_t nextCentre = 0;
    Bytes pending;
    bool noData = false;
    std::size_t maxChunk = 262144;
    int blocksMade = 0;
    bool injectGarbageOnce = false;

    void restartSweep() {
        rangeIdx = 0;
        nextCentre = ranges.empty() ? 0 : static_cast<std::uint64_t>(ranges[0].first) * 1000000ull + offset;
    }
    void makeMore() {
        // one transfer: 16 blocks (262144 / numBytes) in the sweep order
        const std::size_t blocks = 262144 / numBytes;
        for (std::size_t i = 0; i < blocks; ++i) {
            if (injectGarbageOnce) {  // 777 stray bytes, as after a lost transfer
                injectGarbageOnce = false;
                for (int k = 0; k < 777; ++k) { pending.push_back(static_cast<std::uint8_t>(rng())); }
            }
            const Bytes blk = makeBlock(nextCentre, toneHz - static_cast<double>(nextCentre), fs, 0.5, dc, rng, numBytes);
            pending.insert(pending.end(), blk.begin(), blk.end());
            ++blocksMade;
            nextCentre += step;
            const std::uint64_t stopHz = static_cast<std::uint64_t>(ranges[rangeIdx].second) * 1000000ull;
            if (nextCentre >= stopHz + offset) {
                ++rangeIdx;
                if (rangeIdx >= ranges.size()) { rangeIdx = 0; }
                nextCentre = static_cast<std::uint64_t>(ranges[rangeIdx].first) * 1000000ull + offset;
            }
        }
    }
};

class FakeUsb final : public cascade::usb::UsbDevice {
public:
    explicit FakeUsb(std::shared_ptr<Radio> r) : r_(std::move(r)) {}
    int controlOut(std::uint8_t rt, std::uint8_t req, std::uint16_t value, std::uint16_t index, const std::uint8_t* d, std::size_t len, unsigned) override {
        std::lock_guard<std::mutex> lk(r_->mu);
        if (rt != 0x40) { err_ = "fake: wrong direction"; return -1; }
        r_->log.push_back({false, req, value, index, Bytes(d ? d : nullptr, d ? d + len : nullptr)});
        if (req == hs::kRequestInitSweep) {
            if (r_->failInit) { err_ = "fake: stall"; return -1; }
            const Bytes& b = r_->log.back().data;
            if (b.size() < 9) { return -1; }
            r_->step = static_cast<std::uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24));
            r_->offset = static_cast<std::uint32_t>(b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<std::uint32_t>(b[7]) << 24));
            r_->numBytes = static_cast<std::uint32_t>(value | (static_cast<std::uint32_t>(index) << 16));
            r_->ranges.clear();
            for (std::size_t i = 9; i + 3 < b.size(); i += 4) {
                r_->ranges.push_back({static_cast<std::uint16_t>(b[i] | (b[i + 1] << 8)), static_cast<std::uint16_t>(b[i + 2] | (b[i + 3] << 8))});
            }
        } else if (req == hr::requestByte(hr::VendorRequest::SetTransceiverMode)) {
            if (value == hs::kModeRxSweep) { r_->sweeping = true; r_->restartSweep(); }
            if (value == 0) { r_->sweeping = false; r_->modeOffOrder = ++r_->orderCounter; }
        }
        return static_cast<int>(len);
    }
    int controlIn(std::uint8_t rt, std::uint8_t req, std::uint16_t value, std::uint16_t index, std::uint8_t* d, std::size_t len, unsigned) override {
        std::lock_guard<std::mutex> lk(r_->mu);
        if (rt != 0xC0 || len < 1) { return -1; }
        r_->log.push_back({true, req, value, index, {}});
        if (req == hr::requestByte(hr::VendorRequest::BoardIdRead)) {
            if (r_->noBoard) { err_ = "fake: no answer"; return -1; }
            d[0] = 2;
            return 1;
        }
        if (req == hr::requestByte(hr::VendorRequest::SetLnaGain)) { d[0] = r_->refuseLna ? 0 : 1; return 1; }
        if (req == hr::requestByte(hr::VendorRequest::SetVgaGain)) { d[0] = 1; return 1; }
        return -1;
    }
    bool beginBulkStream(std::uint8_t ep, std::size_t bytes, std::size_t count) override {
        std::lock_guard<std::mutex> lk(r_->mu);
        if (r_->failBegin || ep != hr::kRxEndpoint || bytes != 262144 || count != hr::kTransferCount) { err_ = "fake: bad ring"; return false; }
        r_->streamBegun = true;
        r_->streamEnded = false;
        streaming_ = true;
        return true;
    }
    int readBulk(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        std::unique_lock<std::mutex> lk(r_->mu);
        if (r_->failRead) { return -1; }
        if (!r_->sweeping || r_->noData) {
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeoutMs, 10u)));
            return 0;
        }
        if (r_->pending.size() < 1000) { r_->makeMore(); }
        std::uniform_int_distribution<std::size_t> pick(1, std::min<std::size_t>(r_->maxChunk, r_->pending.size()));
        const std::size_t n = std::min(cap, r_->maxChunk == 262144 ? std::min<std::size_t>(262144, r_->pending.size()) : pick(r_->rng));
        std::copy(r_->pending.begin(), r_->pending.begin() + static_cast<std::ptrdiff_t>(n), dst);
        r_->pending.erase(r_->pending.begin(), r_->pending.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<int>(n);
    }
    void endBulkStream() override {
        std::lock_guard<std::mutex> lk(r_->mu);
        streaming_ = false;
        r_->streamEnded = true;
        r_->endOrder = ++r_->orderCounter;
    }
    bool streaming() const override { return streaming_; }
    bool resetPipe(std::uint8_t) override { return true; }
    const std::string& path() const override { return path_; }
    const std::string& lastError() const override { return err_; }

private:
    std::shared_ptr<Radio> r_;
    bool streaming_ = false;
    std::string path_ = "fake", err_;
};

struct Rig {
    std::shared_ptr<Radio> radio = std::make_shared<Radio>();
    hs::HackRfSweeper sw;
    explicit Rig(bool present = true) {
        auto r = radio;
        std::vector<cascade::usb::UsbDeviceInfo> devs;
        if (present) { devs.push_back({hr::kUsbVid, hr::kPidHackRfOne, "fake", "0000000000000000A", "HackRF One"}); }
        sw.setTransportForTest(devs, [r](const std::string&, std::string&) { return std::make_unique<FakeUsb>(r); });
    }
};

hs::SweepConfig wifiConfig() {
    hs::SweepConfig c;
    c.plan.rangesMHz = {{2400, 2500}};
    c.plan.numBytes = 16384;
    c.plan.stepWidthHz = 10000000;
    c.plan.offsetHz = 5000000;
    c.spectrum.sampleRateHz = 20.0e6;
    c.spectrum.fftSize = 1024;
    c.spectrum.stepHz = 10.0e6;
    return c;
}

// ---- request and plan -------------------------------------------------------------------------------------------------
void testRequest() {
    hs::Plan p;
    p.rangesMHz = {{2400, 2500}, {5150, 5850}};
    hs::InitRequest r;
    std::string err;
    CHECK(hs::validate(p).empty() && hs::buildInitRequest(p, r, err));
    // hackrf_init_sweep: wValue = num_bytes & 0xFFFF, wIndex = num_bytes >> 16, data = step LE32, offset LE32,
    // style, then start/stop LE16 pairs in MHz
    CHECK(r.value == 0x4000 && r.index == 0);
    CHECK(r.data == (Bytes{0x80, 0x96, 0x98, 0x00, 0x40, 0x4B, 0x4C, 0x00, 0x00, 0x60, 0x09, 0xC4, 0x09, 0x1E, 0x14, 0xDA, 0x16}));
    CHECK(r.data.size() == 9 + 4 * 2);
    CHECK(hs::kRequestInitSweep == 26 && hs::kModeRxSweep == 5 && hs::kBytesPerBlock == 16384 && hs::kMaxRanges == 10);
    p.numBytes = 3 * 16384;
    CHECK(hs::buildInitRequest(p, r, err) && r.value == 0xC000 && r.index == 0);
    p.numBytes = 4u * 1024u * 1024u;
    CHECK(hs::buildInitRequest(p, r, err) && r.value == 0 && r.index == 0x40);
    // ten ranges is the most
    hs::Plan q;
    for (int i = 0; i < 10; ++i) { q.rangesMHz.push_back({static_cast<std::uint16_t>(100 + 10 * i), static_cast<std::uint16_t>(105 + 10 * i)}); }
    CHECK(hs::buildInitRequest(q, r, err) && r.data.size() == 9 + 40);
    q.rangesMHz.push_back({500, 600});
    CHECK(!hs::buildInitRequest(q, r, err) && err.find("at most 10") != std::string::npos);
    // refusals
    auto bad = [&](auto mutate, const char* word) {
        hs::Plan x;
        x.rangesMHz = {{2400, 2500}};
        mutate(x);
        const std::string e = hs::validate(x);
        CHECK(!e.empty() && e.find(word) != std::string::npos);
        hs::InitRequest rr;
        std::string ee;
        CHECK(!hs::buildInitRequest(x, rr, ee));
    };
    bad([](hs::Plan& x) { x.rangesMHz.clear(); }, "at least one");
    bad([](hs::Plan& x) { x.rangesMHz = {{2500, 2500}}; }, "end above");
    bad([](hs::Plan& x) { x.rangesMHz = {{2500, 2400}}; }, "end above");
    bad([](hs::Plan& x) { x.numBytes = 0; }, "multiple of 16384");
    bad([](hs::Plan& x) { x.numBytes = 1000; }, "multiple of 16384");
    bad([](hs::Plan& x) { x.numBytes = 16385; }, "multiple of 16384");
    bad([](hs::Plan& x) { x.numBytes = 16384 * 300; }, "4 MiB");
    bad([](hs::Plan& x) { x.stepWidthHz = 999; }, "1 kHz");
    bad([](hs::Plan& x) { x.style = hs::Style::Interleaved; }, "linear");
}

// ---- the splitter -------------------------------------------------------------------------------------------------------
void testSplitter() {
    std::mt19937 rng(5);
    Bytes stream;
    std::vector<std::uint64_t> centres;
    for (int i = 0; i < 40; ++i) {
        const std::uint64_t c = 2405000000ull + static_cast<std::uint64_t>(i) * 10000000ull;
        centres.push_back(c);
        const Bytes b = makeBlock(c, 1.0e6, 20e6, 0.5, 0.0, rng);
        stream.insert(stream.end(), b.begin(), b.end());
    }
    for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{4096}, std::size_t{16383}, std::size_t{16384}, std::size_t{262144}, stream.size()}) {
        hs::BlockSplitter sp(16384, 2000000000ull, 3000000000ull);
        std::vector<hs::Block> blocks;
        for (std::size_t p = 0; p < stream.size(); p += chunk) { sp.feed(stream.data() + p, std::min(chunk, stream.size() - p), blocks); }
        CHECK(blocks.size() == 40 && sp.goodBlocks() == 40 && sp.badBlocks() == 0 && sp.resyncs() == 0);
        for (std::size_t i = 0; i < blocks.size(); ++i) { CHECK(blocks[i].frequencyHz == centres[i] && blocks[i].iq.size() == 8187); }
    }
    // the header's frequency is little endian and the samples are signed 8-bit scaled by 1/128
    {
        Bytes b(16384, 0);
        b[0] = 0x7F; b[1] = 0x7F;
        const std::uint64_t f = 0x0000000123456789ull;
        for (int i = 0; i < 8; ++i) { b[2 + i] = static_cast<std::uint8_t>(f >> (8 * i)); }
        b[10] = 0x80; b[11] = 0x7F; b[12] = 0xFF; b[13] = 0x01;
        hs::BlockSplitter sp;
        std::vector<hs::Block> out;
        sp.feed(b.data(), b.size(), out);
        CHECK(out.size() == 1 && out[0].frequencyHz == f);
        CHECK(out[0].iq[0].real() == -1.0f && out[0].iq[0].imag() == 127.0f / 128.0f && out[0].iq[1].real() == -1.0f / 128.0f && out[0].iq[1].imag() == 1.0f / 128.0f);
    }
    // garbage in front: one bad block, one resync, nothing lost afterwards
    {
        Bytes g(1234);
        for (auto& b : g) { b = static_cast<std::uint8_t>(rng() | 1u); b = b == 0x7F ? 0x55 : b; }
        Bytes s2 = g;
        s2.insert(s2.end(), stream.begin(), stream.begin() + 5 * 16384);
        hs::BlockSplitter sp(16384, 2000000000ull, 3000000000ull);
        std::vector<hs::Block> out;
        sp.feed(s2.data(), s2.size(), out);
        CHECK(out.size() == 5 && sp.badBlocks() >= 1 && sp.resyncs() >= 1 && out[0].frequencyHz == centres[0]);
    }
    // 7F 7F inside sample data with an implausible frequency is not a header
    {
        Bytes s3 = stream;
        s3.insert(s3.begin() + 3000, {0x7F, 0x7F, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
        hs::BlockSplitter sp(16384, 2000000000ull, 3000000000ull);
        std::vector<hs::Block> out;
        sp.feed(s3.data(), s3.size(), out);
        CHECK(out.size() >= 38 && out.size() <= 40);
        for (const auto& b : out) { CHECK(b.frequencyHz >= 2000000000ull && b.frequencyHz <= 3000000000ull); }
        CHECK(sp.badBlocks() >= 1 && sp.resyncs() >= 1);
    }
    // a frequency outside the believable span is refused even with a perfect marker
    {
        hs::BlockSplitter sp(16384, 2000000000ull, 3000000000ull);
        const Bytes far = makeBlock(5800000000ull, 0, 20e6, 0.5, 0.0, rng);
        std::vector<hs::Block> out;
        sp.feed(far.data(), far.size(), out);
        CHECK(out.empty() && sp.badBlocks() == 1);
    }
    // an incomplete block waits; bigger blocks (3 x 16384) work
    {
        hs::BlockSplitter sp(3 * 16384);
        const Bytes big = makeBlock(2405000000ull, 1e6, 20e6, 0.5, 0.0, rng, 3 * 16384);
        std::vector<hs::Block> out;
        sp.feed(big.data(), big.size() - 1, out);
        CHECK(out.empty());
        sp.feed(big.data() + big.size() - 1, 1, out);
        CHECK(out.size() == 1 && out[0].iq.size() == (3 * 16384 - 10) / 2);
    }
    // fuzz: never crashes, every emitted block has a believable frequency, nothing is invented
    int emitted = 0;
    for (int r = 0; r < 1500; ++r) {
        Bytes junk(rng() % 70000);
        for (auto& b : junk) { b = static_cast<std::uint8_t>(rng()); }
        if (r % 2 == 0 && junk.size() > 20000) {
            const Bytes ok = makeBlock(2437000000ull, 0, 20e6, 0.3, 0.0, rng);
            std::copy(ok.begin(), ok.end(), junk.begin() + static_cast<std::ptrdiff_t>(rng() % 3000));
        }
        hs::BlockSplitter sp(16384, 2000000000ull, 3000000000ull);
        std::vector<hs::Block> out;
        std::size_t p = 0;
        while (p < junk.size()) { const std::size_t n = std::min<std::size_t>(1 + rng() % 9000, junk.size() - p); sp.feed(junk.data() + p, n, out); p += n; }
        for (const auto& b : out) { CHECK(b.frequencyHz >= 2000000000ull && b.frequencyHz <= 3000000000ull && b.iq.size() == 8187); }
        emitted += static_cast<int>(out.size());
    }
    CHECK(emitted > 300);
    std::printf("hackrf sweep splitter fuzz: %d blocks recovered from noise streams\n", emitted);
}

// ---- the spectrum -------------------------------------------------------------------------------------------------------
std::size_t peakIndex(const std::vector<float>& v) { return static_cast<std::size_t>(std::max_element(v.begin(), v.end()) - v.begin()); }

void testSpectrum() {
    std::mt19937 rng(11);
    hs::SpectrumConfig c;
    c.sampleRateHz = 20e6;
    c.fftSize = 1024;
    c.stepHz = 10e6;
    const double binHz = 20e6 / 1024.0;
    hs::BlockSplitter sp;
    std::vector<hs::Block> bl;
    const double toneRel = 64.0 * binHz;  // exactly on a bin: +1.25 MHz
    Bytes raw = makeBlock(2437000000ull, toneRel, 20e6, 0.5, 0.0, rng);
    sp.feed(raw.data(), raw.size(), bl);
    CHECK(bl.size() == 1);
    cascade::core::SweepLine line;
    std::string err;
    CHECK(hs::blockSpectrum(bl[0], c, line, err));
    CHECK(line.points() == 512 && cascade::core::sweepLineProblem(line).empty());
    CHECK(std::fabs(line.startHz - (2437000000.0 - 5e6)) < 1.0 && std::fabs(line.stopHz - (2437000000.0 + 5e6 - binHz)) < 1.0);
    const std::size_t pk = peakIndex(line.db);
    CHECK(std::fabs(cascade::core::sweepPointHz(line, pk) - (2437000000.0 + toneRel)) < binHz);
    CHECK(std::fabs(line.db[pk] - (-6.02f)) < 1.5f);  // amplitude 0.5 = -6 dBFS
    // the floor is far below the tone
    std::vector<float> sorted = line.db;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted[sorted.size() / 2] < line.db[pk] - 40.0f);
    // a DC offset makes a spike at the centre; removeDcSpike hides it
    Bytes rawDc = makeBlock(2437000000ull, toneRel, 20e6, 0.05, 0.3, rng);
    bl.clear();
    hs::BlockSplitter sp2;
    sp2.feed(rawDc.data(), rawDc.size(), bl);
    cascade::core::SweepLine withDc, noDc;
    c.removeDcSpike = false;
    CHECK(hs::blockSpectrum(bl[0], c, withDc, err));
    c.removeDcSpike = true;
    CHECK(hs::blockSpectrum(bl[0], c, noDc, err));
    CHECK(withDc.db[256] > noDc.db[256] + 20.0f);
    CHECK(noDc.db[255] == noDc.db[256] && noDc.db[257] == noDc.db[256]);
    // refusals
    hs::SpectrumConfig bad = c;
    bad.fftSize = 1000;
    CHECK(!hs::blockSpectrum(bl[0], bad, line, err) && err.find("power of two") != std::string::npos);
    bad = c; bad.fftSize = 16;
    CHECK(!hs::blockSpectrum(bl[0], bad, line, err));
    bad = c; bad.fftSize = 131072;
    CHECK(!hs::blockSpectrum(bl[0], bad, line, err));
    bad = c; bad.sampleRateHz = 0.0;
    CHECK(!hs::blockSpectrum(bl[0], bad, line, err));
    bad = c; bad.sampleRateHz = std::nan("");
    CHECK(!hs::blockSpectrum(bl[0], bad, line, err));
    bad = c; bad.stepHz = 30e6;
    CHECK(!hs::blockSpectrum(bl[0], bad, line, err) && err.find("step") != std::string::npos);
    bad = c; bad.stepHz = 0.0;
    CHECK(!hs::blockSpectrum(bl[0], bad, line, err));
    bad = c; bad.stepHz = binHz;  // one bin: "narrower than two bins"
    CHECK(!hs::blockSpectrum(bl[0], bad, line, err));
    hs::Block tiny;
    tiny.frequencyHz = 1;
    tiny.iq.assign(100, Cx(0, 0));
    CHECK(!hs::blockSpectrum(tiny, c, line, err) && err.find("fewer samples") != std::string::npos);
}

void testAssembler() {
    std::mt19937 rng(21);
    hs::Plan plan;
    plan.rangesMHz = {{2400, 2500}};
    hs::SpectrumConfig cfg;
    cfg.sampleRateHz = 20e6;
    cfg.fftSize = 1024;
    cfg.stepHz = 10e6;
    hs::SweepAssembler as(plan, cfg);
    std::vector<cascade::core::SweepLine> out;
    const double binHz = 20e6 / 1024.0;
    auto addPass = [&](double tone) {
        for (int k = 0; k < 10; ++k) {
            const std::uint64_t centre = 2405000000ull + static_cast<std::uint64_t>(k) * 10000000ull;
            Bytes raw = makeBlock(centre, tone - static_cast<double>(centre), 20e6, 0.5, 0.0, rng);
            hs::BlockSplitter sp;
            std::vector<hs::Block> bl;
            sp.feed(raw.data(), raw.size(), bl);
            as.add(bl[0], 100.0 + k, out);
        }
    };
    const double tone = 2400e6 + 2800.0 * binHz;  // on a bin of the line's grid, in block 5
    addPass(tone);
    CHECK(out.empty());  // the pass completes when the next one begins
    addPass(tone);
    CHECK(out.size() == 1 && as.passes() == 1);
    if (out.size() != 1) { return; }  // recorded above
    const cascade::core::SweepLine& l = out[0];
    CHECK(cascade::core::sweepLineProblem(l).empty());
    CHECK(l.startHz == 2400e6 && l.points() == static_cast<std::size_t>(std::floor(100e6 / binHz)) + 1);
    std::size_t nan = 0;
    for (float v : l.db) { if (std::isnan(v)) { ++nan; } }
    CHECK(nan <= 1);  // the last point (2500 MHz exactly) belongs to no block
    const std::size_t pk = peakIndex(l.db);
    CHECK(std::fabs(cascade::core::sweepPointHz(l, pk) - tone) < binHz);
    CHECK(std::fabs(l.db[pk] - (-6.02f)) < 1.5f);
    // a block outside every range is counted, not placed
    {
        Bytes raw = makeBlock(2800000000ull, 0, 20e6, 0.5, 0.0, rng);
        hs::BlockSplitter sp;
        std::vector<hs::Block> bl;
        sp.feed(raw.data(), raw.size(), bl);
        const std::uint64_t before = as.unusedBlocks();
        as.add(bl[0], 200.0, out);
        CHECK(as.unusedBlocks() == before + 1);
    }
}

// ---- the sweeper -------------------------------------------------------------------------------------------------------------
void testSweeperProgram() {
    Rig rig;
    hs::HackRfSweeper& s = rig.sw;
    CHECK(!s.isOpen() && !s.start() && !s.configure(wifiConfig()));
    CHECK(s.open("") && s.isOpen());
    CHECK(!s.start() && std::string(s.lastError()).find("configured") != std::string::npos);
    CHECK(s.configure(wifiConfig()));
    {
        std::lock_guard<std::mutex> lk(rig.radio->mu);
        // order: board id, rate, filter, LNA, VGA, amp, INIT_SWEEP (26)
        std::vector<std::uint8_t> order;
        for (const auto& c : rig.radio->log) { order.push_back(c.request); }
        CHECK(order == (std::vector<std::uint8_t>{14, 6, 7, 19, 20, 17, 26}));
        if (order.size() != 7) { return; }  // recorded above
        const auto& init = rig.radio->log.back();
        CHECK(!init.in && init.value == 0x4000 && init.index == 0);
        CHECK(init.data == (Bytes{0x80, 0x96, 0x98, 0x00, 0x40, 0x4B, 0x4C, 0x00, 0x00, 0x60, 0x09, 0xC4, 0x09}));
        const auto& lna = rig.radio->log[3];
        CHECK(lna.in && lna.index == 16 && lna.value == 0);  // 16 dB LNA rides in the index word
        const auto& amp = rig.radio->log[5];
        CHECK(!amp.in && amp.value == 0);
        // the sample-rate payload is the receiver driver's: 20 MS/s, divider 1
        const auto& rate = rig.radio->log[1];
        CHECK(rate.data.size() == 8 && rate.data[0] == 0x00 && rate.data[1] == 0x2D && rate.data[2] == 0x31 && rate.data[3] == 0x01 && rate.data[4] == 1);
        // no sweep mode yet
        for (const auto& c : rig.radio->log) { CHECK(!(c.request == 1 && c.value == hs::kModeRxSweep)); }
    }
    CHECK(s.start() && s.streaming());
    {
        std::lock_guard<std::mutex> lk(rig.radio->mu);
        CHECK(rig.radio->log.back().request == 1 && rig.radio->log.back().value == hs::kModeRxSweep);  // after INIT_SWEEP
        CHECK(rig.radio->streamBegun);
    }
    CHECK(!s.configure(wifiConfig()) && std::string(s.lastError()).find("stop") != std::string::npos);
    s.stop();
    {
        std::lock_guard<std::mutex> lk(rig.radio->mu);
        CHECK(rig.radio->streamEnded && rig.radio->modeOffOrder > 0 && rig.radio->modeOffOrder < rig.radio->endOrder);  // radio off, then the ring
        CHECK(!rig.radio->sweeping);
    }
    CHECK(!s.streaming());
    s.close();
    CHECK(!s.isOpen());
    {   // nothing but the known requests was ever sent: no transmit, flash, CPLD or reset request
        std::lock_guard<std::mutex> lk(rig.radio->mu);
        for (const auto& c : rig.radio->log) {
            CHECK(c.request == 14 || c.request == 6 || c.request == 7 || c.request == 19 || c.request == 20 || c.request == 17 ||
                  c.request == 26 || c.request == 1);
        }
    }
}

void testSweeperRefusals() {
    {   // nothing connected, bad selectors
        Rig rig(false);
        CHECK(!rig.sw.open("") && std::string(rig.sw.lastError()).find("no HackRF found") != std::string::npos);
        Rig r2;
        CHECK(!r2.sw.open("serial=nope") && !r2.sw.open("index=5") && !r2.sw.open("index=x"));
        CHECK(r2.sw.open("serial=0000000000000000A") && r2.sw.open("index=0"));
        r2.radio->noBoard = true;
        CHECK(!r2.sw.open("") && !r2.sw.isOpen());
    }
    Rig rig;
    CHECK(rig.sw.open(""));
    auto sent26 = [&]() { std::lock_guard<std::mutex> lk(rig.radio->mu); for (const auto& c : rig.radio->log) { if (c.request == 26) { return true; } } return false; };
    auto refuse = [&](auto mutate, const char* word) {
        hs::SweepConfig c = wifiConfig();
        mutate(c);
        CHECK(!rig.sw.configure(c));
        CHECK(std::string(rig.sw.lastError()).find(word) != std::string::npos);
        CHECK(!sent26());
    };
    refuse([](hs::SweepConfig& c) { c.plan.style = hs::Style::Interleaved; }, "linear");
    refuse([](hs::SweepConfig& c) { c.plan.rangesMHz = {{0, 100}}; }, "1 MHz to 6 GHz");
    refuse([](hs::SweepConfig& c) { c.plan.rangesMHz = {{5000, 7000}}; }, "1 MHz to 6 GHz");
    refuse([](hs::SweepConfig& c) { c.spectrum.sampleRateHz = 25e6; }, "2 to 20");
    refuse([](hs::SweepConfig& c) { c.spectrum.sampleRateHz = 1e6; }, "2 to 20");
    refuse([](hs::SweepConfig& c) { c.spectrum.fftSize = 1000; }, "power of two");
    refuse([](hs::SweepConfig& c) { c.spectrum.fftSize = 8192; }, "larger than the samples");
    refuse([](hs::SweepConfig& c) { c.spectrum.stepHz = 8e6; }, "must equal");
    refuse([](hs::SweepConfig& c) { c.plan.rangesMHz = {{1, 6000}}; c.spectrum.fftSize = 4096; }, "million bins");
    {   // the radio refuses a gain, or stalls the sweep request
        rig.radio->refuseLna = true;
        CHECK(!rig.sw.configure(wifiConfig()) && std::string(rig.sw.lastError()).find("LNA") != std::string::npos && !sent26());
        rig.radio->refuseLna = false;
        rig.radio->failInit = true;
        CHECK(!rig.sw.configure(wifiConfig()) && std::string(rig.sw.lastError()).find("USB API 0x0104") != std::string::npos);
        rig.radio->failInit = false;
        CHECK(rig.sw.configure(wifiConfig()));
        rig.radio->failBegin = true;
        CHECK(!rig.sw.start() && std::string(rig.sw.lastError()).find("transfers") != std::string::npos && !rig.sw.streaming());
        {
            std::lock_guard<std::mutex> lk(rig.radio->mu);
            CHECK(!rig.radio->sweeping);  // switched off again after the failed ring
        }
        rig.radio->failBegin = false;
    }
}

void testSweeperStream() {
    Rig rig;
    rig.radio->toneHz = 2437.5e6;
    hs::HackRfSweeper& s = rig.sw;
    CHECK(s.open("") && s.configure(wifiConfig()) && s.start());
    std::vector<cascade::core::SweepLine> lines;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (lines.size() < 3 && std::chrono::steady_clock::now() < end) { CHECK(s.poll(lines, 100, 1000.0)); }
    CHECK(lines.size() >= 3);
    if (lines.size() < 3) { s.close(); return; }  // recorded above
    const double binHz = 20e6 / 1024.0;
    for (const auto& l : lines) {
        CHECK(cascade::core::sweepLineProblem(l).empty() && l.startHz == 2400e6);
        const std::size_t pk = peakIndex(l.db);
        CHECK(std::fabs(cascade::core::sweepPointHz(l, pk) - 2437.5e6) < 2.0 * binHz);
        CHECK(l.db[pk] > -12.0f && l.db[pk] < 0.0f);
    }
    CHECK(s.badBlocks() == 0 && s.goodBlocks() >= 20 && s.passes() >= 3);
    // lost bytes: the stream recovers, the loss is counted
    {
        std::lock_guard<std::mutex> lk(rig.radio->mu);
        rig.radio->injectGarbageOnce = true;
        rig.radio->maxChunk = 20000;  // small odd chunks from now on
    }
    const std::uint64_t before = s.passes();
    const auto end2 = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (s.passes() < before + 3 && std::chrono::steady_clock::now() < end2) { CHECK(s.poll(lines, 100, 2000.0)); }
    CHECK(s.passes() >= before + 3 && s.resyncs() >= 1 && s.badBlocks() >= 1);
    // no data: poll times out quietly
    {
        std::lock_guard<std::mutex> lk(rig.radio->mu);
        rig.radio->noData = true;
    }
    const std::size_t n = lines.size();
    CHECK(s.poll(lines, 50, 3000.0) && lines.size() == n);
    // the pipe fails
    {
        std::lock_guard<std::mutex> lk(rig.radio->mu);
        rig.radio->failRead = true;
    }
    CHECK(!s.poll(lines, 50, 4000.0) && std::string(s.lastError()).find("pipe failed") != std::string::npos);
    s.close();
    CHECK(!s.poll(lines, 10, 5000.0) && !s.streaming());
    // closing while streaming ends the ring
    Rig rig2;
    CHECK(rig2.sw.open("") && rig2.sw.configure(wifiConfig()) && rig2.sw.start());
    rig2.sw.close();
    {
        std::lock_guard<std::mutex> lk(rig2.radio->mu);
        CHECK(rig2.radio->streamEnded && !rig2.radio->sweeping);
    }
    // two ranges
    Rig rig3;
    hs::SweepConfig two = wifiConfig();
    two.plan.rangesMHz = {{2400, 2420}, {2440, 2460}};
    rig3.radio->toneHz = 2450.0e6;
    CHECK(rig3.sw.open("") && rig3.sw.configure(two) && rig3.sw.start());
    std::vector<cascade::core::SweepLine> l3;
    const auto end3 = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (l3.size() < 4 && std::chrono::steady_clock::now() < end3) { CHECK(rig3.sw.poll(l3, 100, 1.0)); }
    CHECK(l3.size() >= 4);
    if (l3.size() < 4) { rig3.sw.close(); return; }  // the failure above is recorded; nothing below can be read
    CHECK(l3[0].startHz == 2400e6 && l3[1].startHz == 2440e6);
    // the tone is in the second range only
    CHECK(l3[1].db[peakIndex(l3[1].db)] > -12.0f);
    CHECK(l3[0].db[peakIndex(l3[0].db)] < -35.0f);
    rig3.sw.close();
}

}  // namespace

int main() {
    testRequest();
    testSplitter();
    testSpectrum();
    testAssembler();
    testSweeperProgram();
    testSweeperRefusals();
    testSweeperStream();
    return testSummary("test_hackrf_sweep");
}
