// patch_scope_math.hpp - what a Spectrum node on the patch canvas shows, as
// arithmetic. No ImGui.
//
// A Spectrum node draws a live trace and a scrolling waterfall of whatever it
// is wired to: the RADIO (the whole capture, every channel marked) or a
// CHANNEL (that channel's slice). Both come out of the wideband spectrum the
// pipeline already publishes - SpectrumFrame::dbBins, fftshifted, bin 0 at
// -rate/2 - so the node and the main waterfall are the same measurement and
// cannot disagree.
//
// WHY THE COLUMNS ARE MAX-HOLD. A node is a few hundred pixels wide and the
// FFT is 1024 or more bins, so each column covers several bins. Averaging
// them would dilute a narrow carrier - one bin of -30 dB among seven of -120
// averages to about -39 dB and fades into the floor at small sizes - which is
// exactly the signal someone is looking for. Taking the loudest bin keeps it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PATCH_SCOPE_MATH_HPP
#define CASCADE_GUI_PATCH_SCOPE_MATH_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace cascade::gui::patch {

// Resamples an fftshifted spectrum onto `columns` columns covering the
// offsets [loHz, hiHz] from the capture's centre. `rateHz` is the capture
// rate, so bin k sits at -rate/2 + (k + 0.5) * rate / N. Each column takes the
// loudest bin whose centre falls inside it; a column narrower than a bin (a
// zoomed-in channel) takes the bin containing its middle. Columns outside the
// capture are left at `floorDb`. Returns false and leaves `out` empty on
// nonsense input.
inline bool binsToColumns(const std::vector<float>& dbBins, double rateHz, double loHz,
                          double hiHz, int columns, float floorDb, std::vector<float>& out) {
    out.clear();
    const std::size_t n = dbBins.size();
    if (n == 0 || !(rateHz > 0.0) || !(hiHz > loHz) || columns <= 0) { return false; }
    out.assign(static_cast<std::size_t>(columns), floorDb);
    const double binHz = rateHz / static_cast<double>(n);
    const double colHz = (hiHz - loHz) / static_cast<double>(columns);
    for (int c = 0; c < columns; ++c) {
        const double f0 = loHz + colHz * static_cast<double>(c);
        const double f1 = f0 + colHz;
        // Bins whose CENTRE lies in [f0, f1).
        const double k0d = std::ceil((f0 + 0.5 * rateHz) / binHz - 0.5);
        const double k1d = std::ceil((f1 + 0.5 * rateHz) / binHz - 0.5) - 1.0;
        float best = floorDb;
        bool any = false;
        for (double kd = std::max(0.0, k0d); kd <= k1d && kd < static_cast<double>(n); kd += 1.0) {
            const float v = dbBins[static_cast<std::size_t>(kd)];
            if (!any || v > best) { best = v; }
            any = true;
        }
        if (!any) {
            // Narrower than a bin: the bin under the column's middle.
            const double mid = 0.5 * (f0 + f1);
            const double kd = std::floor((mid + 0.5 * rateHz) / binHz);
            if (kd >= 0.0 && kd < static_cast<double>(n)) {
                best = dbBins[static_cast<std::size_t>(kd)];
            }
        }
        out[static_cast<std::size_t>(c)] = best;
    }
    return true;
}

// Where an offset lands across a span `width` pixels wide, or a negative
// value when it is outside the span (the caller then draws no marker).
inline float markerX(double offsetHz, double loHz, double hiHz, float width) {
    if (!(hiHz > loHz) || offsetHz < loHz || offsetHz > hiHz) { return -1.0f; }
    return static_cast<float>((offsetHz - loHz) / (hiHz - loHz)) * width;
}

// The dB range a node's trace and waterfall are scaled to: the floor is the
// 20th-percentile bin (the noise, whatever the receiver's gain), the ceiling
// the loudest bin, and never less than 30 dB between them - a flat, quiet band
// stretched to fill the colour map would paint noise as signal.
struct DbRange {
    float lo = -120.0f;
    float hi = -40.0f;
};

inline DbRange autoRange(const std::vector<float>& cols) {
    DbRange r;
    if (cols.empty()) { return r; }
    std::vector<float> s(cols);
    const std::size_t k = s.size() / 5;
    std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(k), s.end());
    r.lo = s[k];
    r.hi = *std::max_element(cols.begin(), cols.end());
    if (r.hi < r.lo + 30.0f) { r.hi = r.lo + 30.0f; }
    return r;
}

inline float normalise(float db, const DbRange& r) {
    const float t = (db - r.lo) / (r.hi - r.lo);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// A waterfall's memory: `rows` rows of `columns` normalised values, newest
// FIRST when read by row(0). Fixed size, allocated once; a change of shape
// (the node was resized, or rewired to a different span) starts it afresh,
// because rows of a different width or frequency span cannot be stacked.
class ScopeHistory {
public:
    void reshape(int columns, int rows, double loHz, double hiHz) {
        if (columns == columns_ && rows == rows_ && loHz == lo_ && hiHz == hi_) { return; }
        columns_ = std::max(0, columns);
        rows_ = std::max(0, rows);
        lo_ = loHz;
        hi_ = hiHz;
        data_.assign(static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_), 0.0f);
        head_ = 0;
        filled_ = 0;
    }

    void push(const std::vector<float>& norm01) {
        if (columns_ <= 0 || rows_ <= 0 || norm01.size() != static_cast<std::size_t>(columns_)) {
            return;
        }
        head_ = (head_ + rows_ - 1) % rows_;
        std::copy(norm01.begin(), norm01.end(),
                  data_.begin() + static_cast<std::ptrdiff_t>(head_) * columns_);
        if (filled_ < rows_) { ++filled_; }
    }

    // Row `age` (0 = newest), or nullptr when that many rows have not arrived.
    const float* row(int age) const {
        if (age < 0 || age >= filled_) { return nullptr; }
        const int r = (head_ + age) % rows_;
        return data_.data() + static_cast<std::ptrdiff_t>(r) * columns_;
    }

    int columns() const { return columns_; }
    int rows() const { return rows_; }
    int filled() const { return filled_; }

private:
    int columns_ = 0;
    int rows_ = 0;
    double lo_ = 0.0;
    double hi_ = 0.0;
    std::vector<float> data_;
    int head_ = 0;
    int filled_ = 0;
};

}  // namespace cascade::gui::patch

#endif  // CASCADE_GUI_PATCH_SCOPE_MATH_HPP
