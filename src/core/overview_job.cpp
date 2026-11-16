// overview_job.cpp - see overview_job.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/overview_job.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>

#include "source/iq_readers.hpp"

namespace cascade::core::overview {

namespace {

namespace iqf = cascade::source::iqfile;

// In-place iterative radix-2 FFT (n a power of two).
void fft(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) { j ^= bit; }
        j ^= bit;
        if (i < j) { std::swap(a[i], a[j]); }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const std::complex<float> wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// dark blue -> blue -> cyan -> yellow -> white for 0..1
void colour(float t, std::uint8_t* px) {
    t = std::clamp(t, 0.0f, 1.0f);
    float r, g, b;
    if (t < 0.25f) { const float u = t / 0.25f; r = 0; g = 0; b = 0.15f + 0.65f * u; }
    else if (t < 0.5f) { const float u = (t - 0.25f) / 0.25f; r = 0; g = u * 0.9f; b = 0.8f; }
    else if (t < 0.75f) { const float u = (t - 0.5f) / 0.25f; r = u; g = 0.9f + 0.1f * u; b = 0.8f * (1.0f - u); }
    else { const float u = (t - 0.75f) / 0.25f; r = 1.0f; g = 1.0f; b = u; }
    px[0] = static_cast<std::uint8_t>(r * 255.0f);
    px[1] = static_cast<std::uint8_t>(g * 255.0f);
    px[2] = static_cast<std::uint8_t>(b * 255.0f);
    px[3] = 255;
}

}  // namespace

bool compute(const Request& req, const std::atomic<bool>& cancel, std::atomic<double>& progress, Image& out, std::string& err, bool& cancelled) {
    cancelled = false;
    progress.store(0.0);
    const int n = req.fft;
    if (n < 512 || n > 4096 || (n & (n - 1)) != 0) { err = "the transform size must be 512, 1024, 2048 or 4096"; return false; }
    if (req.rows < 50 || req.rows > 2000) { err = "rows must be 50 to 2000"; return false; }
    iqf::RawParams params;
    std::unique_ptr<iqf::IqFileReader> rd;
    if (!req.rawFormat.empty()) {
        iqf::Layout layout;
        if (req.rawFormat == "cu8") { layout.kind = iqf::Kind::U8; }
        else if (req.rawFormat == "cs16") { layout.kind = iqf::Kind::S16; }
        else if (req.rawFormat == "cf32") { layout.kind = iqf::Kind::F32; }
        else { err = "unknown raw sample format"; return false; }
        params.sampleRateHz = req.rawRateHz;
        params.centerFrequencyHz = req.rawCentreHz;
        params.hasCenter = req.rawCentreHz > 0.0;
        rd = iqf::openRaw(req.path, layout, params, &err);
    } else {
        rd = iqf::openAuto(req.path, params, &err);
    }
    if (!rd) { if (err.empty()) { err = "the file could not be opened"; } return false; }
    const std::uint64_t total = rd->info().totalSamples;
    const double rate = rd->info().sampleRateHz;
    if (total < static_cast<std::uint64_t>(n)) { err = "the file is shorter than one block of the transform"; return false; }
    if (!(rate > 0.0)) { err = "the file does not say its sample rate"; return false; }
    const int rows = static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint64_t>(req.rows), total / static_cast<std::uint64_t>(n)));
    const int width = std::min(n, 1024);
    std::vector<float> win(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) { win[static_cast<std::size_t>(i)] = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * 3.14159265358979323846 * i / (n - 1))); }
    std::vector<float> db(static_cast<std::size_t>(rows) * static_cast<std::size_t>(width), 0.0f);
    std::vector<std::complex<float>> buf(static_cast<std::size_t>(n)), spec(static_cast<std::size_t>(n));
    std::vector<double> acc(static_cast<std::size_t>(n));
    const std::uint64_t piece = total / static_cast<std::uint64_t>(rows);
    for (int r = 0; r < rows; ++r) {
        if (cancel.load()) { cancelled = true; err = "cancelled"; return false; }
        const std::uint64_t from = static_cast<std::uint64_t>(r) * piece;
        const std::uint64_t blocks = std::max<std::uint64_t>(1, std::min<std::uint64_t>(4, piece / static_cast<std::uint64_t>(n)));
        std::fill(acc.begin(), acc.end(), 0.0);
        int used = 0;
        for (std::uint64_t j = 0; j < blocks; ++j) {
            const std::uint64_t room = piece >= static_cast<std::uint64_t>(n) ? piece - static_cast<std::uint64_t>(n) : 0;
            const std::uint64_t at = from + (blocks > 1 ? room * j / (blocks - 1) : 0);
            if (!rd->seek(at) || rd->read(buf.data(), static_cast<std::size_t>(n)) != static_cast<std::size_t>(n)) { continue; }
            for (int i = 0; i < n; ++i) { spec[static_cast<std::size_t>(i)] = buf[static_cast<std::size_t>(i)] * win[static_cast<std::size_t>(i)]; }
            fft(spec);
            for (int i = 0; i < n; ++i) { acc[static_cast<std::size_t>(i)] += static_cast<double>(std::norm(spec[static_cast<std::size_t>(i)])); }
            ++used;
        }
        if (used == 0) { err = "the file could not be read to its end (it may have been cut short or removed)"; return false; }
        // fftshift: column 0 is -rate/2; bins joined by their strongest when the transform is wider than the picture
        const int group = n / width;
        for (int c = 0; c < width; ++c) {
            double best = 0.0;
            for (int g = 0; g < group; ++g) {
                const int bin = (c * group + g + n / 2) % n;
                best = std::max(best, acc[static_cast<std::size_t>(bin)] / used);
            }
            db[static_cast<std::size_t>(r) * static_cast<std::size_t>(width) + static_cast<std::size_t>(c)] = 10.0f * std::log10(static_cast<float>(best / (static_cast<double>(n) * n)) + 1e-20f);
        }
        progress.store(static_cast<double>(r + 1) / rows);
    }
    std::vector<float> sorted(db);
    std::sort(sorted.begin(), sorted.end());
    const float floorDb = sorted[sorted.size() / 2];
    const float peakDb = std::max(sorted.back(), floorDb + 1.0f);
    out.width = width;
    out.height = rows;
    out.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(rows) * 4u, 0);
    for (std::size_t i = 0; i < db.size(); ++i) { colour((db[i] - floorDb) / (peakDb - floorDb), out.rgba.data() + 4 * i); }
    out.rateHz = rate;
    out.centreHz = rd->info().centerFrequencyHz;
    out.durationS = static_cast<double>(total) / rate;
    out.floorDb = floorDb;
    out.peakDb = peakDb;
    return true;
}

Job::~Job() { reap(); }

void Job::reap() {
    cancel_.store(true);
    if (th_.joinable()) { th_.join(); }
}

bool Job::start(const Request& req) {
    if (running_.load()) { return false; }
    if (th_.joinable()) { th_.join(); }
    cancel_.store(false);
    ready_.store(false);
    running_.store(true);
    progress_.store(0.0);
    th_ = std::thread([this, req] {
        const auto t0 = std::chrono::steady_clock::now();
        Result r;
        bool cancelled = false;
        try {
            r.ok = compute(req, cancel_, progress_, r.image, r.error, cancelled);
        } catch (...) {
            r.ok = false;
            r.error = "the picture could not be made";
        }
        r.cancelled = cancelled;
        r.elapsedS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        result_ = std::move(r);
        ready_.store(true);
        running_.store(false);
    });
    return true;
}

void Job::cancel() { cancel_.store(true); }

bool Job::take(Result& out) {
    if (!ready_.load()) { return false; }
    ready_.store(false);
    out = std::move(result_);
    return true;
}

}  // namespace cascade::core::overview
