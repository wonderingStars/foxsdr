// sweep_stitch.cpp - see sweep_stitch.hpp for the method and its stated limits.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sweep_stitch.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace cascade::core {

namespace {

StitchResult refuse(std::string why) {
    StitchResult r;
    r.ok = false;
    r.error = std::move(why);
    return r;
}

double medianOf(std::vector<double>& v) {
    const std::size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2), v.end());
    double m = v[n / 2];
    if (n % 2 == 0) {
        const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2));
        m = 0.5 * (m + lo);
    }
    return m;
}

struct Placed {
    long long cellLo = 0;            // first cell the kept bins reach
    std::vector<float> value;        // per cell from cellLo; NaN = none
    double centerHz = 0.0;
};

}  // namespace

StitchResult stitchPass(const std::vector<SpectrumCapture>& caps, const StitchConfig& cfg) {
    if (caps.empty()) { return refuse("no captures"); }
    if (caps.size() > cfg.maxCaptures) { return refuse("too many captures"); }
    if (!(cfg.trimFraction >= 0.0 && cfg.trimFraction < 0.45)) { return refuse("trim fraction out of range"); }
    std::vector<double> bws;
    for (const SpectrumCapture& c : caps) {
        if (!std::isfinite(c.centerHz) || c.centerHz < 0.0 || c.centerHz > kSweepMaxFrequencyHz) {
            return refuse("capture centre out of range");
        }
        if (!std::isfinite(c.rateHz) || !(c.rateHz > 0.0) || c.rateHz > kSweepMaxFrequencyHz) {
            return refuse("capture rate out of range");
        }
        if (c.db.size() < 16 || c.db.size() > kSweepMaxPointsPerLine) { return refuse("capture needs 16 to 1048576 bins"); }
        if (!std::isfinite(c.timeS)) { return refuse("capture time is not a number"); }
        for (float v : c.db) {
            if (!std::isnan(v) && (!std::isfinite(v) || v < kSweepMinLevelDb || v > kSweepMaxLevelDb)) {
                return refuse("capture level out of range");
            }
        }
        bws.push_back(c.rateHz / static_cast<double>(c.db.size()));
    }
    std::vector<double> bwSorted = bws;
    const double bw = medianOf(bwSorted);
    for (double b : bws) {
        if (std::fabs(b / bw - 1.0) > cfg.maxBinWidthMismatch) { return refuse("captures' bin widths disagree"); }
    }

    // ascending centre order (stable)
    std::vector<std::size_t> order(caps.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return caps[a].centerHz < caps[b].centerHz; });

    // kept bin range and the grid origin
    std::vector<std::size_t> lo(caps.size()), hi(caps.size());
    double origin = std::numeric_limits<double>::infinity();
    double top = -std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k < caps.size(); ++k) {
        const std::size_t n = caps[k].db.size();
        lo[k] = static_cast<std::size_t>(std::ceil(cfg.trimFraction * static_cast<double>(n)));
        hi[k] = n - lo[k];
        if (hi[k] < lo[k] + 4) { return refuse("trim leaves fewer than 4 bins"); }
        const double half = static_cast<double>(n / 2);
        const double f0 = caps[k].centerHz + (static_cast<double>(lo[k]) - half) * bws[k];
        const double f1 = caps[k].centerHz + (static_cast<double>(hi[k] - 1) - half) * bws[k];
        origin = std::min(origin, f0);
        top = std::max(top, f1);
    }
    if (!(origin >= 0.0) && origin > -1e-3) { origin = 0.0; }
    if (origin < 0.0) { return refuse("capture reaches below 0 Hz"); }
    const double cellsD = std::floor((top - origin) / bw + 0.5) + 1.0;
    if (!(cellsD >= 1.0) || cellsD > static_cast<double>(cfg.maxCells)) { return refuse("wide trace would exceed the cell cap"); }
    const std::size_t cells = static_cast<std::size_t>(cellsD);

    // put each capture's kept bins on the grid
    std::vector<Placed> placed(caps.size());
    for (std::size_t k = 0; k < caps.size(); ++k) {
        const std::size_t n = caps[k].db.size();
        const double half = static_cast<double>(n / 2);
        Placed& p = placed[k];
        p.centerHz = caps[k].centerHz;
        const auto cellOf = [&](std::size_t i) {
            const double f = caps[k].centerHz + (static_cast<double>(i) - half) * bws[k];
            return static_cast<long long>(std::floor((f - origin) / bw + 0.5));
        };
        p.cellLo = std::max<long long>(0, cellOf(lo[k]));
        const long long cellHi = std::min<long long>(static_cast<long long>(cells) - 1, cellOf(hi[k] - 1));
        p.value.assign(static_cast<std::size_t>(std::max<long long>(0, cellHi - p.cellLo + 1)), std::nanf(""));
        for (std::size_t i = lo[k]; i < hi[k]; ++i) {
            const long long c = cellOf(i) - p.cellLo;
            if (c >= 0 && c < static_cast<long long>(p.value.size()) && !std::isnan(caps[k].db[i])) {
                p.value[static_cast<std::size_t>(c)] = caps[k].db[i];
            }
        }
    }

    // level alignment along the chain
    StitchResult res;
    res.binHz = bw;
    res.correctionDb.assign(caps.size(), 0.0);
    std::vector<double> corr(caps.size(), 0.0);   // by sorted position
    for (std::size_t s = 1; s < order.size(); ++s) {
        const Placed& a = placed[order[s - 1]];
        const Placed& b = placed[order[s]];
        StitchSeam seam;
        seam.lowerCenterHz = a.centerHz;
        seam.upperCenterHz = b.centerHz;
        std::vector<double> d;
        const long long from = std::max(a.cellLo, b.cellLo);
        const long long to = std::min(a.cellLo + static_cast<long long>(a.value.size()),
                                      b.cellLo + static_cast<long long>(b.value.size()));
        for (long long c = from; c < to; ++c) {
            const float va = a.value[static_cast<std::size_t>(c - a.cellLo)];
            const float vb = b.value[static_cast<std::size_t>(c - b.cellLo)];
            if (!std::isnan(va) && !std::isnan(vb)) {
                d.push_back(static_cast<double>(va) + corr[s - 1] - static_cast<double>(vb));
            }
        }
        seam.overlapCells = d.size();
        if (d.size() >= cfg.minOverlapBins) {
            std::vector<double> tmp = d;
            const double med = medianOf(tmp);
            for (double& x : tmp) { x = std::fabs(x - med); }
            const double mad = medianOf(tmp);
            const double sigma = std::max(1.4826 * mad, 0.1);
            double sum = 0.0;
            std::size_t used = 0;
            for (double x : d) {
                if (std::fabs(x - med) <= cfg.outlierMads * sigma) { sum += x; ++used; }
            }
            if (used >= cfg.minOverlapBins) {
                // d already carries the lower capture's correction, so its mean IS the upper
                // capture's total correction against the reference.
                const double total = sum / static_cast<double>(used);
                if (std::fabs(total) > cfg.maxOffsetDb) { return refuse("level offset beyond the stated maximum"); }
                seam.usedCells = used;
                seam.offsetDb = total - corr[s - 1];
                seam.aligned = true;
                corr[s] = total;
            } else {
                corr[s] = corr[s - 1];
            }
        } else {
            corr[s] = corr[s - 1];
        }
        res.seams.push_back(seam);
    }
    for (std::size_t s = 0; s < order.size(); ++s) { res.correctionDb[order[s]] = corr[s]; }

    // fill: each cell from the capture whose centre is nearest
    res.line.startHz = origin;
    res.line.stopHz = origin + static_cast<double>(cells - 1) * bw;
    res.line.db.assign(cells, std::nanf(""));
    std::vector<double> bestDist(cells, std::numeric_limits<double>::infinity());
    for (std::size_t s = 0; s < order.size(); ++s) {
        const Placed& p = placed[order[s]];
        for (std::size_t i = 0; i < p.value.size(); ++i) {
            const float v = p.value[i];
            if (std::isnan(v)) { continue; }
            const std::size_t cell = static_cast<std::size_t>(p.cellLo) + i;
            const double dist = std::fabs(origin + static_cast<double>(cell) * bw - p.centerHz);
            if (dist < bestDist[cell]) {
                bestDist[cell] = dist;
                res.line.db[cell] = static_cast<float>(static_cast<double>(v) + corr[s]);
            }
        }
    }
    double t = 0.0;
    for (const SpectrumCapture& c : caps) { t += c.timeS; }
    res.line.timeS = t / static_cast<double>(caps.size());
    res.ok = true;
    return res;
}

// ------------------------------------------------------------------ heatmap

bool SweepHeatmap::configure(const HeatmapConfig& cfg, std::string* why) {
    auto fail = [&](const char* w) { if (why != nullptr) { *why = w; } return false; };
    if (!std::isfinite(cfg.startHz) || !std::isfinite(cfg.stopHz) || !(cfg.startHz >= 0.0) ||
        !(cfg.stopHz > cfg.startHz) || cfg.stopHz > kSweepMaxFrequencyHz) {
        return fail("frequency range unusable");
    }
    if (cfg.cols < 1 || cfg.cols > kHeatmapMaxCols) { return fail("columns out of range"); }
    if (cfg.maxRows < 2 || cfg.maxRows > kHeatmapMaxRows) { return fail("rows out of range"); }
    if (cfg.cols * cfg.maxRows > kHeatmapMaxCells) { return fail("too many cells"); }
    cfg_ = cfg;
    colHz_ = (cfg.stopHz - cfg.startHz) / static_cast<double>(cfg.cols);
    rowCount_ = 0;
    passesPerRow_ = 1;
    passes_ = 0;
    value_.assign(cfg.cols * cfg.maxRows, std::nanf(""));
    count_.assign(cfg.cols * cfg.maxRows, 0);
    rowPasses_.assign(cfg.maxRows, 0);
    rowTimeSum_.assign(cfg.maxRows, 0.0);
    configured_ = true;
    return true;
}

void SweepHeatmap::compact() {
    const std::size_t cols = cfg_.cols;
    const std::size_t newRows = (rowCount_ + 1) / 2;
    for (std::size_t r = 0; r < newRows; ++r) {
        const std::size_t a = 2 * r;
        const std::size_t b = 2 * r + 1;
        if (b >= rowCount_) {
            if (a != r) {   // lone last row moves up
                std::copy(value_.begin() + static_cast<std::ptrdiff_t>(a * cols),
                          value_.begin() + static_cast<std::ptrdiff_t>((a + 1) * cols),
                          value_.begin() + static_cast<std::ptrdiff_t>(r * cols));
                std::copy(count_.begin() + static_cast<std::ptrdiff_t>(a * cols),
                          count_.begin() + static_cast<std::ptrdiff_t>((a + 1) * cols),
                          count_.begin() + static_cast<std::ptrdiff_t>(r * cols));
                rowPasses_[r] = rowPasses_[a];
                rowTimeSum_[r] = rowTimeSum_[a];
            }
            continue;
        }
        for (std::size_t c = 0; c < cols; ++c) {
            const float va = value_[a * cols + c];
            const float vb = value_[b * cols + c];
            const std::uint32_t na = count_[a * cols + c];
            const std::uint32_t nb = count_[b * cols + c];
            float v = std::nanf("");
            if (na > 0 && nb > 0) {
                v = cfg_.timeMode == Decimate::Max
                        ? std::max(va, vb)
                        : static_cast<float>((static_cast<double>(va) * na + static_cast<double>(vb) * nb) /
                                             (static_cast<double>(na) + nb));
            } else if (na > 0) { v = va; }
            else if (nb > 0) { v = vb; }
            value_[r * cols + c] = v;
            count_[r * cols + c] = na + nb;
        }
        rowPasses_[r] = rowPasses_[a] + rowPasses_[b];
        rowTimeSum_[r] = rowTimeSum_[a] + rowTimeSum_[b];
    }
    for (std::size_t r = newRows; r < rowCount_; ++r) {
        std::fill(value_.begin() + static_cast<std::ptrdiff_t>(r * cols),
                  value_.begin() + static_cast<std::ptrdiff_t>((r + 1) * cols), std::nanf(""));
        std::fill(count_.begin() + static_cast<std::ptrdiff_t>(r * cols),
                  count_.begin() + static_cast<std::ptrdiff_t>((r + 1) * cols), 0u);
        rowPasses_[r] = 0;
        rowTimeSum_[r] = 0.0;
    }
    rowCount_ = newRows;
    passesPerRow_ *= 2;
}

bool SweepHeatmap::addLine(const SweepLine& line, std::string* why) {
    auto fail = [&](const char* w) { if (why != nullptr) { *why = w; } return false; };
    if (!configured_) { return fail("not configured"); }
    if (!sweepLineProblem(line).empty()) { return fail("line refused"); }
    if (line.stopHz < cfg_.startHz || line.startHz > cfg_.stopHz) { return fail("line outside the range"); }

    // this line's value per column
    const std::size_t cols = cfg_.cols;
    std::vector<double> acc(cols, 0.0);
    std::vector<std::uint32_t> n(cols, 0);
    for (std::size_t i = 0; i < line.db.size(); ++i) {
        const float v = line.db[i];
        if (std::isnan(v)) { continue; }
        const long long c = columnAtFrequency(sweepPointHz(line, i));
        if (c < 0) { continue; }
        const std::size_t cc = static_cast<std::size_t>(c);
        if (n[cc] == 0) { acc[cc] = v; }
        else if (cfg_.freqMode == Decimate::Max) { acc[cc] = std::max(acc[cc], static_cast<double>(v)); }
        else { acc[cc] += v; }
        ++n[cc];
    }
    if (cfg_.freqMode == Decimate::Mean) {
        for (std::size_t c = 0; c < cols; ++c) { if (n[c] > 0) { acc[c] /= static_cast<double>(n[c]); } }
    }

    // which row: the last one while it has room, else a new one (compacting first if full)
    std::size_t row;
    if (rowCount_ > 0 && rowPasses_[rowCount_ - 1] < passesPerRow_) {
        row = rowCount_ - 1;
    } else {
        if (rowCount_ == cfg_.maxRows) { compact(); }
        // after a compaction the last row may have room again
        if (rowCount_ > 0 && rowPasses_[rowCount_ - 1] < passesPerRow_) { row = rowCount_ - 1; }
        else { row = rowCount_++; }
    }
    for (std::size_t c = 0; c < cols; ++c) {
        if (n[c] == 0) { continue; }
        float& cell = value_[row * cols + c];
        std::uint32_t& cnt = count_[row * cols + c];
        const float nv = static_cast<float>(acc[c]);
        if (cnt == 0) { cell = nv; }
        else if (cfg_.timeMode == Decimate::Max) { cell = std::max(cell, nv); }
        else { cell = static_cast<float>((static_cast<double>(cell) * cnt + nv) / (static_cast<double>(cnt) + 1.0)); }
        ++cnt;
    }
    ++rowPasses_[row];
    rowTimeSum_[row] += line.timeS;
    ++passes_;
    return true;
}

float SweepHeatmap::at(std::size_t row, std::size_t col) const {
    if (!configured_ || row >= rowCount_ || col >= cfg_.cols) { return std::nanf(""); }
    if (count_[row * cfg_.cols + col] == 0) { return std::nanf(""); }
    return value_[row * cfg_.cols + col];
}

double SweepHeatmap::rowTimeS(std::size_t row) const {
    if (!configured_ || row >= rowCount_ || rowPasses_[row] == 0) { return 0.0; }
    return rowTimeSum_[row] / static_cast<double>(rowPasses_[row]);
}

std::size_t SweepHeatmap::memoryBytes() const {
    return value_.size() * sizeof(float) + count_.size() * sizeof(std::uint32_t) +
           rowPasses_.size() * sizeof(std::uint32_t) + rowTimeSum_.size() * sizeof(double);
}

double SweepHeatmap::frequencyAtColumn(std::size_t col) const {
    if (!configured_) { return 0.0; }
    if (col >= cfg_.cols) { col = cfg_.cols - 1; }
    return cfg_.startHz + (static_cast<double>(col) + 0.5) * colHz_;
}

long long SweepHeatmap::columnAtFrequency(double hz) const {
    if (!configured_ || !std::isfinite(hz) || hz < cfg_.startHz || hz > cfg_.stopHz) { return -1; }
    long long c = static_cast<long long>(std::floor((hz - cfg_.startHz) / colHz_));
    if (c >= static_cast<long long>(cfg_.cols)) { c = static_cast<long long>(cfg_.cols) - 1; }
    return c;
}

double SweepHeatmap::frequencyAtFraction(double x01) const {
    if (!configured_) { return 0.0; }
    if (!(x01 > 0.0)) { x01 = 0.0; }
    if (x01 > 1.0) { x01 = 1.0; }
    return cfg_.startHz + x01 * (cfg_.stopHz - cfg_.startHz);
}

}  // namespace cascade::core
