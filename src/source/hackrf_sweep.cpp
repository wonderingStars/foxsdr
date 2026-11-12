// hackrf_sweep.cpp - see hackrf_sweep.hpp for the sources, the licence of each file read and what is inferred.
// Ported as facts from greatscottgadgets/hackrf host/libhackrf/src/hackrf.c and hackrf.h (BSD 3-clause headers);
// the notice is reproduced in THIRD-PARTY-NOTICES.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hackrf_sweep.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "dsp/fft.hpp"
#include "source/device_source.hpp"
#include "source/hackrf_protocol.hpp"

namespace cascade::source::hackrf_sweep {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool isPowerOfTwo(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

std::uint64_t le64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) { v = (v << 8) | p[i]; }
    return v;
}

}  // namespace

std::string validate(const Plan& p) {
    if (p.rangesMHz.empty()) { return "the sweep needs at least one frequency range"; }
    if (p.rangesMHz.size() > static_cast<std::size_t>(kMaxRanges)) { return "a HackRF sweep takes at most 10 frequency ranges"; }
    for (const auto& r : p.rangesMHz) {
        if (r.second <= r.first) { return "every sweep range must end above where it starts"; }
    }
    if (p.numBytes < kBytesPerBlock || p.numBytes % kBytesPerBlock != 0) {
        return "the bytes per tuning must be a multiple of 16384";
    }
    if (p.numBytes > 4u * 1024u * 1024u) { return "the bytes per tuning are limited to 4 MiB here"; }
    if (p.stepWidthHz < 1000) { return "the sweep step must be at least 1 kHz"; }
    if (p.style != Style::Linear) { return "only the linear sweep style is built (the interleaved style's combination rule is in GPL source that was not read)"; }
    return {};
}

bool buildInitRequest(const Plan& p, InitRequest& out, std::string& error) {
    error = validate(p);
    if (!error.empty()) { return false; }
    out = InitRequest{};
    out.value = static_cast<std::uint16_t>(p.numBytes & 0xFFFFu);
    out.index = static_cast<std::uint16_t>(p.numBytes >> 16);
    auto le32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) { out.data.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    };
    le32(p.stepWidthHz);
    le32(p.offsetHz);
    out.data.push_back(static_cast<std::uint8_t>(p.style));
    for (const auto& r : p.rangesMHz) {
        out.data.push_back(static_cast<std::uint8_t>(r.first));
        out.data.push_back(static_cast<std::uint8_t>(r.first >> 8));
        out.data.push_back(static_cast<std::uint8_t>(r.second));
        out.data.push_back(static_cast<std::uint8_t>(r.second >> 8));
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// the block stream
// ---------------------------------------------------------------------------------------

bool BlockSplitter::plausible(const std::uint8_t* p) const {
    if (p[0] != 0x7F || p[1] != 0x7F) { return false; }
    const std::uint64_t f = le64(p + 2);
    if (maxHz_ > 0) { return f >= minHz_ && f <= maxHz_; }
    return f <= 100000000000ull;
}

void BlockSplitter::feed(const std::uint8_t* d, std::size_t n, std::vector<Block>& out) {
    if (d != nullptr && n > 0) { buf_.insert(buf_.end(), d, d + n); }
    std::size_t pos = 0;
    while (buf_.size() - pos >= blockBytes_) {
        const std::uint8_t* p = buf_.data() + pos;
        if (plausible(p)) {
            Block b;
            b.frequencyHz = le64(p + 2);
            const std::size_t count = (blockBytes_ - kHeaderBytes) / 2;
            b.iq.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const float re = static_cast<float>(static_cast<std::int8_t>(p[kHeaderBytes + 2 * i])) / 128.0f;
                const float im = static_cast<float>(static_cast<std::int8_t>(p[kHeaderBytes + 2 * i + 1])) / 128.0f;
                b.iq.emplace_back(re, im);
            }
            out.push_back(std::move(b));
            ++good_;
            pos += blockBytes_;
            continue;
        }
        ++bad_;
        // scan for the next believable header; keep the last 9 bytes if none is complete yet
        std::size_t q = pos + 1;
        bool found = false;
        for (; q + kHeaderBytes <= buf_.size(); ++q) {
            if (plausible(buf_.data() + q)) { found = true; break; }
        }
        if (found) {
            ++resyncs_;
            pos = q;
        } else {
            pos = buf_.size() >= kHeaderBytes - 1 ? buf_.size() - (kHeaderBytes - 1) : pos;
            break;
        }
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(std::min(pos, buf_.size())));
}

// ---------------------------------------------------------------------------------------
// the spectrum
// ---------------------------------------------------------------------------------------

bool blockSpectrum(const Block& b, const SpectrumConfig& c, core::SweepLine& line, std::string& error) {
    const std::size_t N = c.fftSize;
    if (!isPowerOfTwo(N) || N < 32 || N > 65536 || !dsp::ComplexFFT::isValidSize(N)) {
        error = "the FFT size must be a power of two from 32 to 65536";
        return false;
    }
    if (!(c.sampleRateHz > 0.0) || !std::isfinite(c.sampleRateHz)) { error = "the sample rate must be positive"; return false; }
    if (!(c.stepHz > 0.0) || c.stepHz > c.sampleRateHz || !std::isfinite(c.stepHz)) {
        error = "the step must be positive and not above the sample rate";
        return false;
    }
    if (b.iq.size() < N) { error = "the block has fewer samples than the FFT size"; return false; }
    const double binHz = c.sampleRateHz / static_cast<double>(N);
    const std::size_t segments = b.iq.size() / N;
    static thread_local std::unique_ptr<dsp::ComplexFFT> fft;
    static thread_local std::size_t fftN = 0;
    if (!fft || fftN != N) {
        fft = std::make_unique<dsp::ComplexFFT>(N);
        fftN = N;
    }
    std::vector<float> w(N);
    double sw = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        w[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(N)));
        sw += w[i];
    }
    std::vector<std::complex<float>> in(N), spec(N);
    std::vector<double> power(N, 0.0);
    for (std::size_t s = 0; s < segments; ++s) {
        for (std::size_t i = 0; i < N; ++i) { in[i] = b.iq[s * N + i] * w[i]; }
        fft->forward(in.data(), spec.data());
        for (std::size_t k = 0; k < N; ++k) { power[k] += static_cast<double>(std::norm(spec[k])); }
    }
    const double norm = 1.0 / (static_cast<double>(segments) * sw * sw);
    // fftshift: shifted index j holds unshifted bin (j + N/2) mod N; offset of j = (j - N/2) * binHz
    std::vector<float> shifted(N);
    for (std::size_t j = 0; j < N; ++j) {
        const double p = power[(j + N / 2) % N] * norm;
        shifted[j] = static_cast<float>(10.0 * std::log10(p + 1e-20));
    }
    if (c.removeDcSpike && N >= 8) {
        const std::size_t c0 = N / 2;
        const float mean = 0.5f * (shifted[c0 - 2] + shifted[c0 + 2]);
        shifted[c0 - 1] = shifted[c0] = shifted[c0 + 1] = mean;
    }
    // keep the offsets in [-step/2, +step/2)
    const double half = c.stepHz / 2.0;
    std::vector<float> kept;
    double firstHz = 0.0, lastHz = 0.0;
    for (std::size_t j = 0; j < N; ++j) {
        const double off = (static_cast<double>(j) - static_cast<double>(N / 2)) * binHz;
        if (off < -half - 1e-9 || off >= half - 1e-9) { continue; }
        if (kept.empty()) { firstHz = static_cast<double>(b.frequencyHz) + off; }
        lastHz = static_cast<double>(b.frequencyHz) + off;
        kept.push_back(shifted[j]);
    }
    if (kept.size() < 2) { error = "the step is narrower than two bins"; return false; }
    line = core::SweepLine{};
    line.startHz = firstHz;
    line.stopHz = lastHz;
    line.db = std::move(kept);
    return true;
}

SweepAssembler::SweepAssembler(const Plan& plan, const SpectrumConfig& cfg) : plan_(plan), cfg_(cfg) {
    const double binHz = cfg_.sampleRateHz / static_cast<double>(cfg_.fftSize);
    for (const auto& r : plan_.rangesMHz) {
        core::SweepLine l;
        l.startHz = static_cast<double>(r.first) * 1e6;
        l.stopHz = l.startHz + std::floor((static_cast<double>(r.second) - static_cast<double>(r.first)) * 1e6 / binHz) * binHz;
        const std::size_t points = static_cast<std::size_t>(std::floor((static_cast<double>(r.second) - static_cast<double>(r.first)) * 1e6 / binHz)) + 1;
        l.db.assign(std::min<std::size_t>(points, core::kSweepMaxPointsPerLine), std::numeric_limits<float>::quiet_NaN());
        acc_.push_back(std::move(l));
    }
}

void SweepAssembler::flush(double timeS, std::vector<core::SweepLine>& out) {
    bool data = false;
    for (const core::SweepLine& l : acc_) {
        for (float v : l.db) { if (!std::isnan(v)) { data = true; break; } }
        if (data) { break; }
    }
    if (!data) { return; }
    for (core::SweepLine& l : acc_) {
        bool has = false;
        for (float v : l.db) { if (!std::isnan(v)) { has = true; break; } }
        if (has && l.db.size() >= 2) {
            core::SweepLine copy = l;
            copy.timeS = timeS;
            out.push_back(std::move(copy));
        }
        std::fill(l.db.begin(), l.db.end(), std::numeric_limits<float>::quiet_NaN());
    }
    ++passes_;
}

void SweepAssembler::add(const Block& b, double timeS, std::vector<core::SweepLine>& out) {
    if (any_ && b.frequencyHz <= lastHz_) { flush(timeS, out); }
    any_ = true;
    lastHz_ = b.frequencyHz;
    core::SweepLine line;
    std::string err;
    if (!blockSpectrum(b, cfg_, line, err)) { ++unused_; return; }
    const double binHz = cfg_.sampleRateHz / static_cast<double>(cfg_.fftSize);
    bool placed = false;
    for (core::SweepLine& r : acc_) {
        const double lo = r.startHz;
        for (std::size_t i = 0; i < line.db.size(); ++i) {
            const double f = line.startHz + static_cast<double>(i) * binHz;
            const double idx = std::round((f - lo) / binHz);
            if (idx < 0.0 || idx >= static_cast<double>(r.db.size())) { continue; }
            r.db[static_cast<std::size_t>(idx)] = line.db[i];
            placed = true;
        }
    }
    if (!placed) { ++unused_; }
}

// ---------------------------------------------------------------------------------------
// the driver
// ---------------------------------------------------------------------------------------

HackRfSweeper::~HackRfSweeper() { close(); }

void HackRfSweeper::setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices, UsbOpenFn opener) {
    std::lock_guard<std::mutex> lk(mu_);
    fakeDevices_ = std::move(devices);
    fakeOpener_ = std::move(opener);
    useFake_ = static_cast<bool>(fakeOpener_);
}

bool HackRfSweeper::isOpen() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dev_ != nullptr;
}
bool HackRfSweeper::streaming() const {
    std::lock_guard<std::mutex> lk(mu_);
    return streaming_;
}

bool HackRfSweeper::controlOut(std::uint8_t request, std::uint16_t value, std::uint16_t index, const std::uint8_t* d, std::size_t n, const char* what) {
    if (!dev_) { error_ = std::string("no HackRF is open (") + what + ")"; return false; }
    const int r = dev_->controlOut(cascade::usb::kRequestTypeVendorOut, request, value, index, d, n, hackrf::kControlTimeoutMs);
    if (r < 0 || static_cast<std::size_t>(r) != n) {
        error_ = std::string(what) + " failed: " + dev_->lastError();
        return false;
    }
    return true;
}

bool HackRfSweeper::controlInByte(std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint8_t& reply, const char* what) {
    if (!dev_) { error_ = std::string("no HackRF is open (") + what + ")"; return false; }
    std::uint8_t b = 0;
    const int r = dev_->controlIn(cascade::usb::kRequestTypeVendorIn, request, value, index, &b, 1, hackrf::kControlTimeoutMs);
    if (r != 1) {
        error_ = std::string(what) + " failed: " + dev_->lastError();
        return false;
    }
    reply = b;
    return true;
}

bool HackRfSweeper::open(const std::string& args) {
    close();
    std::lock_guard<std::mutex> lk(mu_);
    error_.clear();
    std::vector<cascade::usb::UsbDeviceInfo> devices;
    if (useFake_) {
        devices = fakeDevices_;
    } else {
        devices = cascade::usb::enumerateWinUsb({{hackrf::kUsbVid, hackrf::kPidHackRfOne}, {hackrf::kUsbVid, hackrf::kPidJawbreaker}, {hackrf::kUsbVid, hackrf::kPidRad1o}});
    }
    std::vector<cascade::usb::UsbDeviceInfo> ours;
    for (const auto& d : devices) {
        if (d.vid == hackrf::kUsbVid && (d.pid == hackrf::kPidHackRfOne || d.pid == hackrf::kPidJawbreaker || d.pid == hackrf::kPidRad1o)) { ours.push_back(d); }
    }
    if (ours.empty()) {
        error_ = std::string("no HackRF found (is it plugged in, and ") + cascade::usb::kBindHint + "?)";
        return false;
    }
    std::size_t pick = 0;
    const std::string serial = argValue(args, "serial");
    const std::string idx = argValue(args, "index");
    if (!serial.empty()) {
        bool found = false;
        for (std::size_t i = 0; i < ours.size(); ++i) {
            if (ours[i].serial == serial) { pick = i; found = true; break; }
        }
        if (!found) { error_ = "no HackRF with that serial number is connected"; return false; }
    } else if (!idx.empty()) {
        char* e = nullptr;
        const long v = std::strtol(idx.c_str(), &e, 10);
        if (e == idx.c_str() || *e != '\0' || v < 0 || static_cast<std::size_t>(v) >= ours.size()) {
            error_ = "the HackRF index is outside the list of connected radios";
            return false;
        }
        pick = static_cast<std::size_t>(v);
    }
    std::string err;
    dev_ = useFake_ ? fakeOpener_(ours[pick].path, err) : cascade::usb::openWinUsb(ours[pick].path, err);
    if (!dev_) {
        error_ = err.empty() ? std::string("the HackRF could not be opened") : err;
        return false;
    }
    std::uint8_t board = 0;
    if (!controlInByte(hackrf::requestByte(hackrf::VendorRequest::BoardIdRead), 0, 0, board, "reading the board id")) {
        dev_.reset();
        return false;
    }
    configured_ = streaming_ = ringBegun_ = false;
    buf_.assign(hackrf::kTransferBufferBytes, 0);
    return true;
}

void HackRfSweeper::close() {
    stop();
    std::lock_guard<std::mutex> lk(mu_);
    dev_.reset();
    configured_ = false;
}

bool HackRfSweeper::configure(const SweepConfig& c) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!dev_) { error_ = "configure() called with no HackRF open"; return false; }
    if (streaming_) { error_ = "stop the sweep before changing it"; return false; }
    InitRequest req;
    std::string err;
    if (!buildInitRequest(c.plan, req, err)) { error_ = err; return false; }
    for (const auto& r : c.plan.rangesMHz) {
        if (static_cast<double>(r.first) * 1e6 < hackrf::kMinFrequencyHz || static_cast<double>(r.second) * 1e6 > hackrf::kMaxFrequencyHz) {
            error_ = "the HackRF sweeps 1 MHz to 6 GHz";
            return false;
        }
    }
    const SpectrumConfig& s = c.spectrum;
    if (!(s.sampleRateHz >= hackrf::kMinSampleRateHz && s.sampleRateHz <= hackrf::kMaxSampleRateHz)) {
        error_ = "the HackRF sample rate is 2 to 20 MS/s";
        return false;
    }
    if (!isPowerOfTwo(s.fftSize) || s.fftSize < 32 || s.fftSize > 65536) { error_ = "the FFT size must be a power of two from 32 to 65536"; return false; }
    if (!(s.stepHz > 0.0) || s.stepHz > s.sampleRateHz) { error_ = "the step must be positive and not above the sample rate"; return false; }
    if (std::fabs(s.stepHz - static_cast<double>(c.plan.stepWidthHz)) > 0.5) { error_ = "the spectrum step must equal the sweep step width"; return false; }
    if ((static_cast<std::size_t>(c.plan.numBytes) - kHeaderBytes) / 2 < s.fftSize) { error_ = "the FFT is larger than the samples of one block"; return false; }
    const double binHz = s.sampleRateHz / static_cast<double>(s.fftSize);
    for (const auto& r : c.plan.rangesMHz) {
        if (std::floor((static_cast<double>(r.second) - static_cast<double>(r.first)) * 1e6 / binHz) + 1.0 > static_cast<double>(core::kSweepMaxPointsPerLine)) {
            error_ = "that range has more than a million bins: use a smaller FFT or a narrower range";
            return false;
        }
    }
    // the receiver's own setup, in its own order: rate and its filter, gains, amplifier; then the sweep
    const hackrf::RateSetting rate = hackrf::computeRate(s.sampleRateHz);
    std::uint8_t payload[hackrf::kRatePayloadBytes];
    hackrf::encodeRate(rate, payload);
    if (!controlOut(hackrf::requestByte(hackrf::VendorRequest::SampleRateSet), 0, 0, payload, sizeof(payload), "setting the sample rate")) { return false; }
    const std::uint32_t bw = hackrf::basebandFilterForRate(rate);
    if (!controlOut(hackrf::requestByte(hackrf::VendorRequest::BasebandFilterBandwidthSet), static_cast<std::uint16_t>(bw & 0xFFFFu),
                    static_cast<std::uint16_t>(bw >> 16), nullptr, 0, "setting the baseband filter")) { return false; }
    std::uint8_t reply = 0;
    if (!controlInByte(hackrf::requestByte(hackrf::VendorRequest::SetLnaGain), 0, static_cast<std::uint16_t>(hackrf::roundLnaGainDb(c.lnaDb)), reply, "setting the LNA gain")) { return false; }
    if (reply == 0) { error_ = "the radio refused that LNA gain"; return false; }
    if (!controlInByte(hackrf::requestByte(hackrf::VendorRequest::SetVgaGain), 0, static_cast<std::uint16_t>(hackrf::roundVgaGainDb(c.vgaDb)), reply, "setting the VGA gain")) { return false; }
    if (reply == 0) { error_ = "the radio refused that VGA gain"; return false; }
    if (!controlOut(hackrf::requestByte(hackrf::VendorRequest::AmpEnable), c.ampOn ? 1 : 0, 0, nullptr, 0, "switching the front-end amplifier")) { return false; }
    if (!controlOut(kRequestInitSweep, req.value, req.index, req.data.data(), req.data.size(), "programming the sweep")) {
        error_ += " (a HackRF whose firmware is older than USB API 0x0104 cannot sweep)";
        return false;
    }
    cfg_ = c;
    cfg_.spectrum.sampleRateHz = static_cast<double>(rate.freqHz) / static_cast<double>(rate.divider);
    std::uint64_t lo = static_cast<std::uint64_t>(c.plan.rangesMHz[0].first), hi = 0;
    for (const auto& r : c.plan.rangesMHz) {
        lo = std::min<std::uint64_t>(lo, r.first);
        hi = std::max<std::uint64_t>(hi, r.second);
    }
    // a believable block centre: inside the swept span widened by a step and the offset
    const std::uint64_t pad = static_cast<std::uint64_t>(c.plan.stepWidthHz) + c.plan.offsetHz + static_cast<std::uint64_t>(s.sampleRateHz);
    splitter_ = std::make_unique<BlockSplitter>(c.plan.numBytes, lo * 1000000ull > pad ? lo * 1000000ull - pad : 0, hi * 1000000ull + pad);
    assembler_ = std::make_unique<SweepAssembler>(cfg_.plan, cfg_.spectrum);
    configured_ = true;
    error_.clear();
    return true;
}

bool HackRfSweeper::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!dev_ || !configured_) { error_ = "start() needs an open, configured HackRF"; return false; }
    if (streaming_) { return true; }
    if (!controlOut(hackrf::requestByte(hackrf::VendorRequest::SetTransceiverMode), kModeRxSweep, 0, nullptr, 0, "switching to sweep mode")) { return false; }
    if (!dev_->beginBulkStream(hackrf::kRxEndpoint, hackrf::kTransferBufferBytes, hackrf::kTransferCount)) {
        error_ = "queueing the sweep transfers failed: " + dev_->lastError();
        dev_->controlOut(cascade::usb::kRequestTypeVendorOut, hackrf::requestByte(hackrf::VendorRequest::SetTransceiverMode),
                         static_cast<std::uint16_t>(hackrf::TransceiverMode::Off), 0, nullptr, 0, hackrf::kControlTimeoutMs);
        return false;
    }
    ringBegun_ = true;
    splitter_->reset();
    streaming_ = true;
    return true;
}

bool HackRfSweeper::poll(std::vector<core::SweepLine>& out, unsigned timeoutMs, double timeS) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!dev_ || !streaming_) { error_ = "poll() called while the sweep is not running"; return false; }
    const int n = dev_->readBulk(buf_.data(), buf_.size(), timeoutMs);
    if (n < 0) {
        error_ = "the HackRF's I/Q pipe failed: " + dev_->lastError();
        return false;
    }
    if (n == 0) { return true; }
    std::vector<Block> blocks;
    splitter_->feed(buf_.data(), static_cast<std::size_t>(n), blocks);
    for (const Block& b : blocks) { assembler_->add(b, timeS, out); }
    return true;
}

void HackRfSweeper::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!dev_ || !streaming_) { return; }
    // the radio first, then the ring (usb_device.hpp: nothing may be inside readBulk when the ring is freed - there is
    // no reader thread here, poll() is called by the owner and is excluded by this mutex)
    dev_->controlOut(cascade::usb::kRequestTypeVendorOut, hackrf::requestByte(hackrf::VendorRequest::SetTransceiverMode),
                     static_cast<std::uint16_t>(hackrf::TransceiverMode::Off), 0, nullptr, 0, hackrf::kControlTimeoutMs);
    if (ringBegun_) { dev_->endBulkStream(); }
    ringBegun_ = false;
    streaming_ = false;
}

std::uint64_t HackRfSweeper::goodBlocks() const {
    std::lock_guard<std::mutex> lk(mu_);
    return splitter_ ? splitter_->goodBlocks() : 0;
}
std::uint64_t HackRfSweeper::badBlocks() const {
    std::lock_guard<std::mutex> lk(mu_);
    return splitter_ ? splitter_->badBlocks() : 0;
}
std::uint64_t HackRfSweeper::resyncs() const {
    std::lock_guard<std::mutex> lk(mu_);
    return splitter_ ? splitter_->resyncs() : 0;
}
std::uint64_t HackRfSweeper::passes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return assembler_ ? assembler_->passes() : 0;
}

}  // namespace cascade::source::hackrf_sweep
