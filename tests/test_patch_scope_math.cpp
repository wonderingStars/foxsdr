// Tests for gui/patch_scope_math.hpp - what a Spectrum node shows.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/patch_scope_math.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#include "test_check.hpp"

using cascade::gui::patch::autoRange;
using cascade::gui::patch::binsToColumns;
using cascade::gui::patch::DbRange;
using cascade::gui::patch::markerX;
using cascade::gui::patch::normalise;
using cascade::gui::patch::ScopeHistory;

namespace {

constexpr double kRate = 2000000.0;
constexpr std::size_t kN = 1024;

// A flat -120 dB floor with one bin at -30 dB, at the bin nearest `offsetHz`.
std::vector<float> oneCarrier(double offsetHz, std::size_t& binOut) {
    std::vector<float> v(kN, -120.0f);
    const double binHz = kRate / static_cast<double>(kN);
    binOut = static_cast<std::size_t>(std::floor((offsetHz + 0.5 * kRate) / binHz));
    v[binOut] = -30.0f;
    return v;
}

int loudestColumn(const std::vector<float>& c) {
    int best = 0;
    for (int i = 1; i < static_cast<int>(c.size()); ++i) {
        if (c[static_cast<std::size_t>(i)] > c[static_cast<std::size_t>(best)]) { best = i; }
    }
    return best;
}

}  // namespace

int main() {
    // [1] THE WHOLE BAND, SQUEEZED: a single-bin carrier survives 1024 bins
    // into 64 columns at full strength, in the column where it belongs. An
    // averaging resample would report it ~12 dB down.
    {
        std::size_t bin = 0;
        const auto bins = oneCarrier(300000.0, bin);
        std::vector<float> cols;
        CHECK(binsToColumns(bins, kRate, -0.5 * kRate, 0.5 * kRate, 64, -140.0f, cols));
        CHECK(cols.size() == 64u);
        const int c = loudestColumn(cols);
        CHECK(cols[static_cast<std::size_t>(c)] == -30.0f);
        // +300 kHz across -1..+1 MHz is 65% of the way: the carrier's bin
        // centre, 299805 Hz, falls in column floor(1299805 / 31250) = 41.
        CHECK(c == 41);
        // Everything else is the floor, not the fill value.
        int floorCols = 0;
        for (const float x : cols) { floorCols += (x == -120.0f); }
        CHECK(floorCols == 63);
    }

    // [2] A CHANNEL'S SLICE, ZOOMED IN: a 48 kHz span around +297 kHz is far
    // narrower than 64 bins, so columns are narrower than a bin and take the
    // bin under them. The +300 kHz carrier lands 3 kHz right of centre.
    {
        std::size_t bin = 0;
        const auto bins = oneCarrier(300000.0, bin);
        std::vector<float> cols;
        CHECK(binsToColumns(bins, kRate, 297000.0 - 24000.0, 297000.0 + 24000.0, 96, -140.0f,
                            cols));
        const int c = loudestColumn(cols);
        CHECK(cols[static_cast<std::size_t>(c)] == -30.0f);
        // The carrier's BIN spans ~1953 Hz, so several narrow columns share
        // it; its first column must sit within one bin of +3 kHz.
        const double colHz = 48000.0 / 96.0;
        const double firstHz = -24000.0 + colHz * c;
        CHECK(std::fabs(firstHz - 3000.0) <= kRate / static_cast<double>(kN) + colHz);
        // And no column is left at the fill value - every one found a bin.
        for (const float x : cols) { CHECK(x != -140.0f); }
    }

    // [3] A span running past the capture's edge leaves those columns at the
    // floor it was given, rather than reading outside the bins.
    {
        std::vector<float> bins(kN, -100.0f);
        std::vector<float> cols;
        CHECK(binsToColumns(bins, kRate, 0.9e6, 1.3e6, 40, -150.0f, cols));
        CHECK(cols.front() == -100.0f);      // still inside the capture
        CHECK(cols.back() == -150.0f);       // past +1 MHz: nothing to read
    }

    // [4] Nonsense in, false out, nothing written.
    {
        std::vector<float> cols{1.0f};
        CHECK(!binsToColumns({}, kRate, -1.0, 1.0, 10, -1.0f, cols));
        CHECK(cols.empty());
        std::vector<float> bins(kN, -1.0f);
        CHECK(!binsToColumns(bins, 0.0, -1.0, 1.0, 10, -1.0f, cols));
        CHECK(!binsToColumns(bins, kRate, 1.0, -1.0, 10, -1.0f, cols));
        CHECK(!binsToColumns(bins, kRate, -1.0, 1.0, 0, -1.0f, cols));
    }

    // [5] Markers: a channel's offset maps across the span, and one outside it
    // is not drawn.
    {
        CHECK(std::fabs(markerX(0.0, -1e6, 1e6, 400.0f) - 200.0f) < 1e-3f);
        CHECK(std::fabs(markerX(300000.0, -1e6, 1e6, 400.0f) - 260.0f) < 1e-3f);
        CHECK(markerX(1.2e6, -1e6, 1e6, 400.0f) < 0.0f);
        CHECK(markerX(-1.2e6, -1e6, 1e6, 400.0f) < 0.0f);
    }

    // [6] The range sits on the noise and the loudest bin, and never squeezes
    // a quiet band into the whole colour map.
    {
        std::vector<float> cols(100, -110.0f);
        cols[40] = -30.0f;
        const DbRange r = autoRange(cols);
        CHECK(r.lo == -110.0f);
        CHECK(r.hi == -30.0f);
        CHECK(normalise(-30.0f, r) == 1.0f);
        CHECK(normalise(-110.0f, r) == 0.0f);
        CHECK(normalise(-200.0f, r) == 0.0f);
        const DbRange flat = autoRange(std::vector<float>(50, -100.0f));
        CHECK(flat.hi - flat.lo == 30.0f);
    }

    // [7] The waterfall's memory: newest first, bounded, and a change of
    // shape starts it afresh.
    {
        ScopeHistory h;
        h.reshape(4, 3, -1.0, 1.0);
        CHECK(h.row(0) == nullptr);
        h.push({0.1f, 0.1f, 0.1f, 0.1f});
        h.push({0.2f, 0.2f, 0.2f, 0.2f});
        CHECK(h.filled() == 2);
        CHECK(h.row(0) != nullptr && h.row(0)[0] == 0.2f);
        CHECK(h.row(1) != nullptr && h.row(1)[0] == 0.1f);
        CHECK(h.row(2) == nullptr);
        h.push({0.3f, 0.3f, 0.3f, 0.3f});
        h.push({0.4f, 0.4f, 0.4f, 0.4f});   // the oldest (0.1) falls off
        CHECK(h.filled() == 3);
        CHECK(h.row(0)[0] == 0.4f);
        CHECK(h.row(2)[0] == 0.2f);
        h.push({0.5f, 0.5f});                // wrong width: ignored
        CHECK(h.row(0)[0] == 0.4f);
        h.reshape(4, 3, -1.0, 1.0);          // same shape: kept
        CHECK(h.filled() == 3);
        h.reshape(4, 3, -2.0, 2.0);          // new span: afresh
        CHECK(h.filled() == 0);
        CHECK(h.row(0) == nullptr);
    }

    return testSummary("test_patch_scope_math");
}
