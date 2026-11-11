// spectrum_extras.cpp - see spectrum_extras.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "spectrum_extras.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "core/colour_maps.hpp"
#include "export_logic.hpp"

namespace foxsdr::slintextras {

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
std::string fmt(const char* f, double a) { char b[96]; std::snprintf(b, sizeof(b), f, a); return b; }
}  // namespace

// ---------------------------------------------------------------- S7
const char* paletteName(int index) {
    static const char* const k[kPaletteCount] = {"Classic", "Deep sea", "Ember", "Slate", "Spectrum", "Blue-yellow safe"};
    return k[std::clamp(index, 0, kPaletteCount - 1)];
}

const std::array<Rgba, 256>& paletteTable(int index) {
    static const std::array<std::array<Rgba, 256>, kPaletteCount> all = [] {
        std::array<std::array<Rgba, 256>, kPaletteCount> a{};
        for (std::size_t i = 0; i < 256; ++i) { a[0][i] = foxsdr::slinthost::heat(static_cast<float>(i) / 255.0f); }
        for (int m = 0; m < cascade::core::colour::kMapCount; ++m) {
            const auto& t = cascade::core::colour::colourMap(static_cast<cascade::core::colour::MapId>(m));
            for (std::size_t i = 0; i < 256; ++i) { a[static_cast<std::size_t>(m + 1)][i] = Rgba{t[i].r, t[i].g, t[i].b, 255}; }
        }
        return a;
    }();
    return all[static_cast<std::size_t>(std::clamp(index, 0, kPaletteCount - 1))];
}

// ---------------------------------------------------------------- S10
double speedLinesPerSecond(int index) {
    static const double k[kSpeedCount] = {0.0, 10.0, 4.0, 2.0, 1.0, 0.5, 0.2, 0.1, 1.0 / 30.0, 1.0 / 60.0};
    return k[std::clamp(index, 0, kSpeedCount - 1)];
}

void RowPacer::setLinesPerSecond(double lps) {
    lps_ = lps > 0.0 ? lps : 0.0;
    acc_.clear();
}

bool RowPacer::feed(const float* bins, std::size_t n, double nowS) {
    if (bins == nullptr || n == 0) { return false; }
    if (lps_ <= 0.0) {
        row_.assign(bins, bins + n);
        ++lines_;
        return true;
    }
    if (acc_.size() != n) { acc_.assign(bins, bins + n); } else { for (std::size_t i = 0; i < n; ++i) { acc_[i] = std::max(acc_[i], bins[i]); } }
    const double interval = 1.0 / lps_;
    if (lastS_ > -1e17 && nowS - lastS_ < interval) { return false; }
    row_ = acc_;
    acc_.clear();
    lastS_ = (lastS_ > -1e17 && nowS - lastS_ < 2.0 * interval) ? lastS_ + interval : nowS;   // keep the cadence while it is kept, restart it after a gap
    ++lines_;
    return true;
}

// ---------------------------------------------------------------- S8
std::string clockText(double utcS, int mode, double localOffsetS, bool seconds) {
    const long long sec = static_cast<long long>(std::floor(utcS + (mode == 2 ? localOffsetS : 0.0)));
    const long long sod = ((sec % 86400) + 86400) % 86400;
    char b[24];
    if (seconds) { std::snprintf(b, sizeof(b), "%02lld:%02lld:%02lld", sod / 3600, (sod / 60) % 60, sod % 60); }
    else { std::snprintf(b, sizeof(b), "%02lld:%02lld", sod / 3600, (sod / 60) % 60); }
    return b;
}

std::vector<Tick> clockTicks(double newestUtcS, double secondsPerRow, double heightPx, double minPitchPx, int mode, double localOffsetS) {
    std::vector<Tick> out;
    if (!(secondsPerRow > 0.0) || !(heightPx > 0.0) || !(minPitchPx > 0.0)) { return out; }
    const double history = heightPx * secondsPerRow;
    const double minStep = minPitchPx * secondsPerRow;
    double step = 0.0;
    for (const double s : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0, 7200.0, 21600.0, 43200.0, 86400.0}) {
        if (s >= minStep) { step = s; break; }
    }
    if (step == 0.0) { step = 86400.0 * std::ceil(minStep / 86400.0); }
    // round clock times in the zone the labels are in (a local stamp falls on a round local minute)
    const double off = mode == 2 ? localOffsetS : 0.0;
    double t = std::floor((newestUtcS + off) / step) * step - off;
    for (int guard = 0; guard < 64 && newestUtcS - t <= history + 1e-9; ++guard, t -= step) {
        out.push_back({(newestUtcS - t) / history, clockText(t, mode, localOffsetS, step < 60.0)});
    }
    return out;
}

// ---------------------------------------------------------------- S22
void ReferenceTrace::save(const float* frame, std::size_t n, double centre, double span) {
    if (frame == nullptr || n == 0) { return; }
    bins.assign(frame, frame + n);
    centreHz = centre;
    spanHz = span;
    set = true;
}

void ReferenceTrace::columns(double viewCentreHz, double viewSpanHz, std::size_t w, std::vector<float>& out) const {
    out.assign(w, kNaN);
    if (!set || bins.empty() || !(spanHz > 0.0) || !(viewSpanHz > 0.0) || w == 0) { return; }
    const double lo = viewCentreHz - viewSpanHz / 2.0, savedLo = centreHz - spanHz / 2.0;
    const double binHz = spanHz / static_cast<double>(bins.size());
    for (std::size_t x = 0; x < w; ++x) {
        // the saved bins this column covers (the strongest, like the trace's own column)
        const double f0 = lo + viewSpanHz * static_cast<double>(x) / static_cast<double>(w), f1 = lo + viewSpanHz * static_cast<double>(x + 1) / static_cast<double>(w);
        const double b0 = (f0 - savedLo) / binHz, b1 = (f1 - savedLo) / binHz;
        if (b1 <= 0.0 || b0 >= static_cast<double>(bins.size())) { continue; }
        const std::size_t i0 = static_cast<std::size_t>(std::max(0.0, std::floor(b0)));
        const std::size_t i1 = std::min(bins.size(), std::max(i0 + 1, static_cast<std::size_t>(std::ceil(b1))));
        float m = -300.0f;
        for (std::size_t i = i0; i < i1; ++i) { m = std::max(m, bins[i]); }
        out[x] = m;
    }
}

// ---------------------------------------------------------------- S20
double medianDb(const float* bins, std::size_t n) {
    if (bins == nullptr || n == 0) { return -300.0; }
    // a stride keeps the cost bounded on a long frame (the median of every k-th bin of a noise floor is the same to a fraction of a dB)
    const std::size_t stride = n > 4096 ? n / 2048 : 1;
    std::vector<float> v;
    v.reserve(n / stride + 1);
    for (std::size_t i = 0; i < n; i += stride) { v.push_back(bins[i]); }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}

std::vector<Peak> findPeaks(const float* bins, std::size_t n, double centreHz, double spanHz, int maxCount, double aboveFloorDb, std::size_t minSepBins) {
    std::vector<Peak> out;
    if (bins == nullptr || n < 3 || maxCount <= 0) { return out; }
    const float floorDb = static_cast<float>(medianDb(bins, n));
    const float need = floorDb + static_cast<float>(aboveFloorDb);
    std::vector<std::size_t> cand;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        if (bins[i] >= need && bins[i] > bins[i - 1] && bins[i] >= bins[i + 1]) { cand.push_back(i); }
    }
    std::sort(cand.begin(), cand.end(), [&](std::size_t a, std::size_t b) { return bins[a] != bins[b] ? bins[a] > bins[b] : a < b; });
    const double binHz = spanHz / static_cast<double>(n);
    for (const std::size_t i : cand) {
        if (static_cast<int>(out.size()) >= maxCount) { break; }
        bool near = false;
        for (const Peak& p : out) { const std::size_t d = p.bin > i ? p.bin - i : i - p.bin; if (d < minSepBins) { near = true; break; } }
        if (near) { continue; }
        const double a = bins[i - 1], b = bins[i], c = bins[i + 1];
        const double den = a - 2.0 * b + c;
        const double delta = den < -1e-9 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
        Peak p;
        p.bin = i;
        p.db = bins[i];
        p.hz = centreHz - spanHz / 2.0 + (static_cast<double>(i) + 0.5 + delta) * binHz;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.bin < b.bin; });
    return out;
}

// ---------------------------------------------------------------- S14 / Q2
SnrReading measureSnr(const float* bins, std::size_t n, std::size_t p0, std::size_t p1) {
    SnrReading r;
    if (bins == nullptr || n == 0) { return r; }
    p1 = std::min(p1, n);
    if (p0 >= p1) { return r; }
    float peak = -300.0f;
    for (std::size_t i = p0; i < p1; ++i) { peak = std::max(peak, bins[i]); }
    const std::size_t outside = n - (p1 - p0);
    double floorDb;
    if (outside >= 16 && outside * 4 >= n) {
        std::vector<float> v;
        v.reserve(outside);
        for (std::size_t i = 0; i < p0; ++i) { v.push_back(bins[i]); }
        for (std::size_t i = p1; i < n; ++i) { v.push_back(bins[i]); }
        floorDb = medianDb(v.data(), v.size());
    } else {
        floorDb = medianDb(bins, n);
    }
    if (peak <= -299.0f) { return r; }
    r.valid = true;
    r.peakDb = peak;
    r.floorDb = floorDb;
    r.snrDb = peak - floorDb;
    return r;
}

double channelPowerDb(const float* bins, std::size_t n, std::size_t b0, std::size_t b1) {
    if (bins == nullptr) { return -200.0; }
    b1 = std::min(b1, n);
    if (b0 >= b1) { return -200.0; }
    double sum = 0.0;
    for (std::size_t i = b0; i < b1; ++i) { sum += std::pow(10.0, static_cast<double>(bins[i]) / 10.0); }
    return sum > 0.0 ? 10.0 * std::log10(sum) : -200.0;
}

double levelAtHz(const float* bins, std::size_t n, double centreHz, double spanHz, double hz) {
    if (bins == nullptr || n == 0 || !(spanHz > 0.0)) { return std::numeric_limits<double>::quiet_NaN(); }
    const double pos = (hz - (centreHz - spanHz / 2.0)) / spanHz * static_cast<double>(n);
    if (pos < 0.0 || pos >= static_cast<double>(n)) { return std::numeric_limits<double>::quiet_NaN(); }
    const std::size_t i = static_cast<std::size_t>(pos);
    float m = bins[i];
    if (i > 0) { m = std::max(m, bins[i - 1]); }
    if (i + 1 < n) { m = std::max(m, bins[i + 1]); }
    return m;
}

// ---------------------------------------------------------------- S21
AbReading abReading(const AbMarkers& ab, const float* bins, std::size_t n, double centreHz, double spanHz) {
    AbReading r;
    if (!ab.aSet || !ab.bSet) { return r; }
    r.both = true;
    r.deltaHz = ab.bHz - ab.aHz;
    r.aDb = levelAtHz(bins, n, centreHz, spanHz, ab.aHz);
    r.bDb = levelAtHz(bins, n, centreHz, spanHz, ab.bHz);
    r.deltaDb = (std::isnan(r.aDb) || std::isnan(r.bDb)) ? std::numeric_limits<double>::quiet_NaN() : r.bDb - r.aDb;
    return r;
}

namespace {
std::string hzText(double hz) {
    const double a = std::fabs(hz);
    if (a >= 1e6) { return fmt("%.4f MHz", hz / 1e6); }
    if (a >= 1e3) { return fmt("%.2f kHz", hz / 1e3); }
    return fmt("%.0f Hz", hz);
}
}  // namespace

std::string abLabelA(const AbMarkers& ab, double levelDb) {
    return "A " + fmt("%.4f MHz", ab.aHz / 1e6) + (std::isnan(levelDb) ? std::string() : "  " + fmt("%.0f dB", levelDb));
}

std::string abLabelB(const AbMarkers& ab, double levelDb, const AbReading& r) {
    std::string s = "B " + fmt("%.4f MHz", ab.bHz / 1e6) + (std::isnan(levelDb) ? std::string() : "  " + fmt("%.0f dB", levelDb));
    if (r.both) {
        s += "  \xCE\x94 " + std::string(r.deltaHz >= 0 ? "+" : "-") + hzText(std::fabs(r.deltaHz));
        if (!std::isnan(r.deltaDb)) { s += std::string(" ") + (r.deltaDb >= 0 ? "+" : "-") + fmt("%.0f dB", std::fabs(r.deltaDb)); }
    }
    return s;
}

// ---------------------------------------------------------------- Q3
TrackReading FreqTracker::update(double nowS, const float* bins, std::size_t n, double centreHz, double spanHz, double lowHz, double highHz, double refHz, double minAboveFloorDb, double windowS) {
    TrackReading r;
    if (bins == nullptr || n < 3 || !(spanHz > 0.0)) { return r; }
    const double binHz = spanHz / static_cast<double>(n), lo = centreHz - spanHz / 2.0;
    const long long i0 = std::max(1LL, static_cast<long long>(std::floor((lowHz - lo) / binHz))), i1 = std::min(static_cast<long long>(n) - 2, static_cast<long long>(std::ceil((highHz - lo) / binHz)));
    if (i0 > i1) { return r; }
    std::size_t best = static_cast<std::size_t>(i0);
    for (long long i = i0; i <= i1; ++i) { if (bins[i] > bins[best]) { best = static_cast<std::size_t>(i); } }
    const double floorDb = medianDb(bins, n);
    if (bins[best] < floorDb + minAboveFloorDb) { return r; }
    const double a = bins[best - 1], b = bins[best], c = bins[best + 1];
    const double den = a - 2.0 * b + c;
    const double delta = den < -1e-9 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    const double hz = lo + (static_cast<double>(best) + 0.5 + delta) * binHz;
    t_.push_back(nowS);
    f_.push_back(hz);
    while (t_.size() > 2 && nowS - t_.front() > windowS) { t_.erase(t_.begin()); f_.erase(f_.begin()); }
    r.valid = true;
    r.hz = hz;
    r.offsetHz = hz - refHz;
    r.peakDb = bins[best];
    if (t_.size() >= 3 && t_.back() - t_.front() >= 1.0) {   // a least-squares line through the window
        double mt = 0, mf = 0;
        for (std::size_t i = 0; i < t_.size(); ++i) { mt += t_[i]; mf += f_[i]; }
        mt /= static_cast<double>(t_.size());
        mf /= static_cast<double>(t_.size());
        double num = 0, den2 = 0;
        for (std::size_t i = 0; i < t_.size(); ++i) { num += (t_[i] - mt) * (f_[i] - mf); den2 += (t_[i] - mt) * (t_[i] - mt); }
        r.driftHzPerS = den2 > 0 ? num / den2 : 0.0;
    }
    return r;
}

// ---------------------------------------------------------------- S15
void SignalHistory::push(const HistorySample& s, double minGapS) {
    if (!v_.empty() && s.utcS - v_.back().utcS < minGapS) { return; }
    v_.push_back(s);
    if (v_.size() > capacity) { v_.erase(v_.begin(), v_.begin() + static_cast<std::ptrdiff_t>(v_.size() - capacity)); }
}

std::string SignalHistory::csv(const std::string& radioLabel) const {
    std::string s = "# FoxSDR signal history (screen lane B)\n# radio: " + radioLabel + "\n# peak_dbfs: the strongest bin in the passband; floor_dbfs: the median bin outside it; snr_db: peak minus floor; power_dbfs: the passband's bins' power summed (relative, not corrected for the window)\nutc,peak_dbfs,floor_dbfs,snr_db,power_dbfs\n";
    for (const HistorySample& h : v_) {
        s += foxsdr::slintexport::isoUtc(static_cast<std::int64_t>(std::llround(h.utcS * 1000.0)));
        s += "," + fmt("%.1f", h.peakDb) + "," + fmt("%.1f", h.floorDb) + "," + fmt("%.1f", h.snrDb) + "," + fmt("%.1f", h.powerDb) + "\n";
    }
    return s;
}

// ---------------------------------------------------------------- the painters
namespace {
void plate(Rgba* px, std::uint32_t w, std::uint32_t h, int x0, int y0, int x1, int y1, Rgba c) {
    for (int y = std::max(0, y0); y < std::min<int>(static_cast<int>(h), y1); ++y) {
        for (int x = std::max(0, x0); x < std::min<int>(static_cast<int>(w), x1); ++x) { px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = c; }
    }
}
}  // namespace

void paintMeasureBox(Rgba* px, std::uint32_t w, std::uint32_t h, const std::vector<std::string>& lines) {
    if (px == nullptr || lines.empty()) { return; }
    const int kScale = w >= 700 ? 2 : 1, kPad = 4, kLineH = 7 * kScale + 4;   // a wide picture gets the face at twice the size
    int widest = 0;
    for (const std::string& l : lines) { widest = std::max(widest, foxsdr::slintexport::labelWidthPx(l, kScale)); }
    const int bw = widest + 2 * kPad, bh = static_cast<int>(lines.size()) * kLineH + 2 * kPad - 2;
    const int x0 = static_cast<int>(w) - bw - 6, y0 = 6;
    if (x0 < 0 || y0 + bh > static_cast<int>(h)) { return; }
    plate(px, w, h, x0, y0, x0 + bw, y0 + bh, Rgba{0x08, 0x0b, 0x10, 215});
    plate(px, w, h, x0, y0, x0 + bw, y0 + 1, Rgba{0x3a, 0x44, 0x55, 255});
    plate(px, w, h, x0, y0 + bh - 1, x0 + bw, y0 + bh, Rgba{0x3a, 0x44, 0x55, 255});
    plate(px, w, h, x0, y0, x0 + 1, y0 + bh, Rgba{0x3a, 0x44, 0x55, 255});
    plate(px, w, h, x0 + bw - 1, y0, x0 + bw, y0 + bh, Rgba{0x3a, 0x44, 0x55, 255});
    int y = y0 + kPad;
    for (const std::string& l : lines) {
        foxsdr::slintexport::drawLabelRgba(reinterpret_cast<std::uint8_t*>(px), w, h, x0 + kPad, y, l, 0xe6, 0xee, 0xf7, 255, kScale);
        y += kLineH;
    }
}

std::size_t paintHistoryGraph(Rgba* px, std::uint32_t w, std::uint32_t h, const SignalHistory& hist, int which, double seconds) {
    if (px == nullptr || !(seconds > 0.0)) { return 0; }
    constexpr int kW = 168, kH = 56, kPad = 4;
    const int x0 = static_cast<int>(w) - kW - 6, y0 = static_cast<int>(h) - kH - 6;
    if (x0 < 0 || y0 < 0) { return 0; }
    plate(px, w, h, x0, y0, x0 + kW, y0 + kH, Rgba{0x08, 0x0b, 0x10, 215});
    plate(px, w, h, x0, y0, x0 + kW, y0 + 1, Rgba{0x3a, 0x44, 0x55, 255});
    plate(px, w, h, x0, y0 + kH - 1, x0 + kW, y0 + kH, Rgba{0x3a, 0x44, 0x55, 255});
    plate(px, w, h, x0, y0, x0 + 1, y0 + kH, Rgba{0x3a, 0x44, 0x55, 255});
    plate(px, w, h, x0 + kW - 1, y0, x0 + kW, y0 + kH, Rgba{0x3a, 0x44, 0x55, 255});
    const auto& v = hist.samples();
    if (v.empty()) { return 0; }
    const double newest = v.back().utcS;
    const auto value = [&](const HistorySample& s) { return static_cast<double>(which == 0 ? s.snrDb : which == 1 ? s.peakDb : s.powerDb); };
    double lo = 1e9, hi = -1e9;
    std::size_t first = v.size();
    for (std::size_t i = v.size(); i-- > 0;) {
        if (newest - v[i].utcS > seconds) { break; }
        first = i;
        lo = std::min(lo, value(v[i]));
        hi = std::max(hi, value(v[i]));
    }
    if (first == v.size()) { return 0; }
    if (hi - lo < 6.0) { const double mid = 0.5 * (hi + lo); lo = mid - 3.0; hi = mid + 3.0; }
    const int gx0 = x0 + kPad, gx1 = x0 + kW - kPad, gy0 = y0 + kPad + 8, gy1 = y0 + kH - kPad;
    std::size_t drawn = 0;
    int px0 = -1, py0 = -1;
    for (std::size_t i = first; i < v.size(); ++i) {
        const int x = gx0 + static_cast<int>(std::lround((1.0 - (newest - v[i].utcS) / seconds) * (gx1 - gx0 - 1)));
        const int y = gy1 - 1 - static_cast<int>(std::lround((value(v[i]) - lo) / (hi - lo) * (gy1 - gy0 - 1)));
        if (px0 >= 0) {   // join the samples with a straight line
            const int steps = std::max(std::abs(x - px0), std::abs(y - py0));
            for (int s = 0; s <= steps; ++s) {
                const int xx = px0 + (steps ? (x - px0) * s / steps : 0), yy = py0 + (steps ? (y - py0) * s / steps : 0);
                if (xx >= 0 && yy >= 0 && xx < static_cast<int>(w) && yy < static_cast<int>(h)) { px[static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx)] = Rgba{0x7a, 0xd7, 0xff, 255}; }
            }
        }
        px0 = x;
        py0 = y;
        ++drawn;
    }
    const char* name = which == 0 ? "SNR DB " : which == 1 ? "PEAK DBFS " : "POWER DBFS ";
    foxsdr::slintexport::drawLabelRgba(reinterpret_cast<std::uint8_t*>(px), w, h, x0 + kPad, y0 + 3, std::string(name) + fmt("%.0f", value(v.back())), 0xe6, 0xee, 0xf7, 255, 1);
    return drawn;
}

}  // namespace foxsdr::slintextras
