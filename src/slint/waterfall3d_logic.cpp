// waterfall3d_logic.cpp - see waterfall3d_logic.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "waterfall3d_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace foxsdr::slintview3d {

void WaterfallHistory::clear() {
    rows_.clear();
    haveKey_ = false;
}

void WaterfallHistory::push(std::int64_t tMs, const float* bins, std::size_t n, std::uint64_t windowKey) {
    if (bins == nullptr || n == 0) { return; }
    if (!haveKey_ || key_ != windowKey) {
        rows_.clear();
        key_ = windowKey;
        haveKey_ = true;
    }
    // a clock that went backwards (it never does on a steady clock) would unsort the rows: such a frame starts the history again
    if (!rows_.empty() && tMs < rows_.back().t) { rows_.clear(); }
    Row r;
    r.t = tMs;
    const std::size_t cols = static_cast<std::size_t>(kWf3dStoreCols);
    for (std::size_t c = 0; c < cols; ++c) {
        // the bins of column c: [c * n / cols, (c + 1) * n / cols), at least one; the column's level is their maximum (a narrow carrier must not vanish in the downsampling)
        const std::size_t a = c * n / cols;
        const std::size_t b = std::max(a + 1, (c + 1) * n / cols);
        float m = -std::numeric_limits<float>::infinity();
        for (std::size_t i = a; i < b && i < n; ++i) { m = std::max(m, bins[i]); }
        r.v[c] = std::isfinite(m) ? m : -300.0f;
    }
    rows_.push_back(r);
    const std::int64_t horizon = tMs - static_cast<std::int64_t>(kWf3dMaxAgeS * 1000.0);
    while (!rows_.empty() && (rows_.front().t < horizon || rows_.size() > kWf3dMaxRows)) { rows_.pop_front(); }
}

double WaterfallHistory::spanSeconds(std::int64_t nowMs) const {
    if (rows_.empty()) { return 0.0; }
    return std::max(0.0, static_cast<double>(nowMs - rows_.front().t) / 1000.0);
}

std::size_t WaterfallHistory::grid(std::int64_t nowMs, double depthS, int rows, int cols, std::vector<float>& out) const {
    rows = std::max(2, rows);
    cols = std::max(2, cols);
    out.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols), std::numeric_limits<float>::quiet_NaN());
    if (rows_.empty() || !(depthS > 0.0)) { return 0; }
    std::size_t have = 0;
    // walk the output rows from the newest (age 0) to the oldest, keeping the stored index monotone: one pass
    std::size_t idx = rows_.size();   // one past the candidate
    for (int r = 0; r < rows; ++r) {
        const double age = depthS * static_cast<double>(r) / static_cast<double>(rows - 1);
        const std::int64_t at = nowMs - static_cast<std::int64_t>(age * 1000.0);
        // the latest stored row at or before `at`
        while (idx > 0 && rows_[idx - 1].t > at) { --idx; }
        if (idx == 0) { break; }                       // nothing that old: this and every older row is a hole
        const Row& row = rows_[idx - 1];
        // a stored row more than two output rows' worth of time before `at` is stale (a gap in the reception), not the picture of that moment
        const double rowStepMs = depthS * 1000.0 / static_cast<double>(rows - 1);
        if (static_cast<double>(at - row.t) > 2.0 * rowStepMs + 1500.0) { continue; }
        float* dst = out.data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(cols);
        for (int c = 0; c < cols; ++c) {
            const int a = c * kWf3dStoreCols / cols;
            const int b = std::max(a + 1, (c + 1) * kWf3dStoreCols / cols);
            float m = row.v[static_cast<std::size_t>(a)];
            for (int k = a + 1; k < b && k < kWf3dStoreCols; ++k) { m = std::max(m, row.v[static_cast<std::size_t>(k)]); }
            dst[c] = m;
        }
        ++have;
    }
    return have;
}

void gridSizeFor(int pixelW, int pixelH, int& rows, int& cols) {
    cols = std::clamp(pixelW / 8, 48, kWf3dStoreCols);
    rows = std::clamp(pixelH / 6, 24, 96);
}

std::string formatMHz(double hz, double stepHz) {
    const double step = std::fabs(stepHz);
    int decimals = 3;
    if (step > 0.0) {
        if (step >= 1000.0) { decimals = 3; }
        else if (step >= 100.0) { decimals = 4; }
        else if (step >= 10.0) { decimals = 5; }
        else { decimals = 6; }
        if (step >= 100000.0) { decimals = 2; }
        if (step >= 1000000.0) { decimals = 1; }
    }
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", decimals, hz / 1e6);
    return b;
}

std::string formatAge(double seconds, double stepS) {
    char b[32];
    if (stepS < 10.0 && stepS > 0.0 && std::fabs(seconds - std::round(seconds)) > 1e-6) { std::snprintf(b, sizeof(b), "%.1f", seconds); }
    else { std::snprintf(b, sizeof(b), "%.0f", seconds); }
    return b;
}

std::string formatDb(double db) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.0f", db);
    return b;
}

}  // namespace foxsdr::slintview3d
