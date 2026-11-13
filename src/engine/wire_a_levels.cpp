// wire_a_levels.cpp - WIRING LANE wire-a, second pass: the level store of every open radio (LEVEL_STORE; see the operation's comment in core/wire_a_ops.hpp).
//
// HOW. The ring is core::wfhistory::WaterfallHistory (a ring of one-byte rows with a time, centre and span each), one per radio, 14400 rows deep (an hour at four rows a second), fed from the same tap as the display chain
// (WireA::displayFrame, for every radio that publishes a frame, shown or not). A ring is made at a radio's first frame after the store is switched on and freed when it is switched off or the radio goes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "core/app_commands.hpp"
#include "core/waterfall_history.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace wh = cascade::core::wfhistory;

namespace {
constexpr std::size_t kLevelBins = 1024;      // the widest row stored
constexpr std::size_t kLevelRows = 14400;     // an hour at one row every 250 ms
constexpr double kLevelPeriodS = 0.25;
}  // namespace

struct WireA::Lv {
    std::unique_ptr<wh::WaterfallHistory> hist;
    std::size_t bins = 0;
    double nextS = 0.0;
    std::uint64_t segments = 0;
    double lastCentreHz = 0.0, lastSpanHz = 0.0;
    std::int64_t firstMs = 0, lastMs = 0;
    std::uint64_t pushed = 0;
};

void WireA::levelFeed(std::size_t slot, const std::vector<float>& frame, double centreHz, double spanHz, double nowS) {
    if (!levelOn_ || slot >= lv_.size() || frame.empty()) { return; }
    std::shared_ptr<Lv>& lv = lv_[slot];
    if (lv && nowS < lv->nextS) { return; }
    // at most kLevelBins bins: a wider frame is max-pooled, so a narrow peak survives the thinning
    const std::size_t n = frame.size();
    const std::size_t m = std::min(n, kLevelBins);
    if (!lv) { lv = std::make_shared<Lv>(); }
    lv->nextS = nowS + kLevelPeriodS;
    if (!lv->hist || lv->bins != m) {
        wh::Config cfg;
        cfg.bins = m;
        cfg.maxLines = kLevelRows;
        cfg.maxBytes = levelCapMb_ * 1024u * 1024u;
        if (!lv->hist) { lv->hist = std::make_unique<wh::WaterfallHistory>(cfg); } else { lv->hist->reset(cfg); }
        lv->bins = m;
        lv->segments = 0;
        lv->pushed = 0;
    }
    if (!lv->hist->valid()) { return; }
    const float* row = frame.data();
    std::vector<float> pooled;
    if (n > m) {
        pooled.resize(m);
        for (std::size_t i = 0; i < m; ++i) {
            const std::size_t a = i * n / m, b = std::max(a + 1, (i + 1) * n / m);
            float v = frame[a];
            for (std::size_t k = a + 1; k < b; ++k) { v = std::max(v, frame[k]); }
            pooled[i] = v;
        }
        row = pooled.data();
    }
    const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (lv->pushed == 0 || std::fabs(centreHz - lv->lastCentreHz) >= 1.0 || std::fabs(spanHz - lv->lastSpanHz) >= 1.0) { ++lv->segments; }
    lv->lastCentreHz = centreHz;
    lv->lastSpanHz = spanHz;
    if (lv->pushed == 0) { lv->firstMs = ms; }
    lv->lastMs = ms;
    (void)lv->hist->push(row, ms, centreHz, spanHz);
    ++lv->pushed;
}

std::size_t WireA::levelCopy(std::size_t slot, std::size_t rows, std::vector<std::uint8_t>& out, std::size_t& bins, std::vector<wh::LineMeta>* meta) {
    bins = 0;
    out.clear();
    if (slot >= lv_.size()) { return 0; }
    std::shared_ptr<Lv> lv = lv_[slot];
    if (!lv || !lv->hist) { return 0; }
    bins = lv->hist->bins();
    if (bins == 0 || rows == 0) { return 0; }
    out.resize(rows * bins);
    if (meta != nullptr) { meta->assign(rows, wh::LineMeta()); }
    const std::size_t got = lv->hist->copyViewU8(rows, out.data(), meta != nullptr ? meta->data() : nullptr);
    out.resize(got * bins);
    if (meta != nullptr) { meta->resize(got); }
    return got;
}

WireA::Result WireA::levelOp(const FoxCommand& c, Result res) {
    switch (c.ival[0]) {
        case 0:
            if (!levelOn_) { return refused(res, FOXAPI_NO_CHANGE, "the level store is off already"); }
            levelOn_ = false;
            for (auto& l : lv_) { l.reset(); }
            break;
        case 1: {
            if (!std::isfinite(c.num[0]) || c.num[0] < 0.0 || c.num[0] > 64.0) { return refused(res, FOXAPI_OUT_OF_RANGE, "the cap per radio is 1 to 64 MiB (0 = 16)"); }
            const std::size_t mb = c.num[0] < 1.0 ? 16u : static_cast<std::size_t>(c.num[0]);
            if (levelOn_ && mb == levelCapMb_) { return refused(res, FOXAPI_NO_CHANGE, "the level store is on at that cap already"); }
            levelCapMb_ = mb;
            levelOn_ = true;
            for (auto& l : lv_) { l.reset(); }   // (rings made afterwards take the new cap)
            res.applied[0] = static_cast<double>(levelCapMb_);
            break;
        }
        case 2: {
            bool any = false;
            for (auto& l : lv_) { any = any || l != nullptr; l.reset(); }
            if (!any) { return refused(res, FOXAPI_NO_CHANGE, "there is nothing stored"); }
            break;
        }
        case 3: {
            const std::size_t slot = e_.scopedSlot();
            if (slot >= lv_.size()) { return refused(res, FOXAPI_NO_DEVICE, "there is no radio with that number"); }
            if (!lv_[slot] || !lv_[slot]->hist) { return refused(res, FOXAPI_NO_CHANGE, "that radio has no level store yet"); }
            res.applied[0] = static_cast<double>(lv_[slot]->hist->bytesUsed());
            levelStats_[slot] = true;
            break;
        }
        default:
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 off, 1 on, 2 clear or 3 read");
    }
    bump();
    return res;
}

void WireA::levelFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("levels.on", levelOn_ ? "1" : "0");
    fact("levels.capMb", std::to_string(levelCapMb_));
    for (std::size_t slot = 0; slot < lv_.size(); ++slot) {
        const std::shared_ptr<Lv>& lv = lv_[slot];
        if (!levelStats_[slot] || !lv || !lv->hist) { continue; }
        const std::string k = "levels." + std::to_string(slot + 1) + ".";
        const wh::State st = lv->hist->state();
        fact(k + "rows", std::to_string(st.stored));
        fact(k + "seconds", std::to_string(static_cast<long long>((lv->lastMs - lv->firstMs) / 1000)));
        fact(k + "bytes", std::to_string(lv->hist->bytesUsed()));
        fact(k + "bins", std::to_string(lv->bins));
        fact(k + "segments", std::to_string(lv->segments));
    }
}

}  // namespace cascade::engine
