// THE TEXT OF THE DSP THREAD'S TIME ACCOUNTING (2026-11-23, the ringdrop measurement; docs/DIAGNOSTICS.md "Where the time goes").
//
// Pure functions from numbers to a log line's text, so the widest form of each can be held against the log's line (a cut line is masked whole
// by the upload scrub) without running an engine. Fixed words and numbers only: no frequency, path, name or user text. The sample rate is a
// figure of the program's own chain, not a tuning.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace cascade::core {

// The text of a line is at most this many characters (DiagLog::kLineBytes 192 - NUL - the 18-character stamp - one spare). The caller adds
// its " [radio N]" tag AFTER the budget is taken, so the budgets below leave room for it.
constexpr std::size_t kDspLineTextBudget = 172;
constexpr std::size_t kDspRadioTagBudget = 11;   // " [radio 8]" is 10

// The phases' share of the BUSY time, as "name:ms,name:ms" - only the phases over 1% of the busy time, the largest first, and as many as fit
// (`maxChars`). Phase 0 is the ring read and the idle sleep: it is not work and is neither listed nor counted in the busy time. `ns` holds
// `count` phase totals for the minute and `names` their words. "" when no phase is over 1%.
inline std::string dspPhaseMsText(const std::uint64_t* ns, const char* const* names, unsigned count, std::size_t maxChars) {
    std::uint64_t busy = 0;
    for (unsigned i = 1; i < count; ++i) { busy += ns[i]; }
    if (busy == 0) { return std::string(); }
    bool used[32] = {};
    std::string out;
    for (unsigned round = 1; round < count && round < 32; ++round) {
        unsigned best = 0;
        for (unsigned i = 1; i < count && i < 32; ++i) {
            if (!used[i] && (best == 0 || ns[i] > ns[best])) { best = i; }
        }
        if (best == 0) { break; }
        used[best] = true;
        if (ns[best] * 100u <= busy) { break; }   // not over 1%: nor is any that follows
        char piece[48];
        const unsigned long long ms = static_cast<unsigned long long>(std::min<std::uint64_t>(ns[best] / 1000000u, 9999999999ULL));
        std::snprintf(piece, sizeof(piece), "%s%s:%llu", out.empty() ? "" : ",", names[best], ms);
        if (out.size() + std::strlen(piece) > maxChars) { break; }
        out += piece;
    }
    return out;
}

// The two largest phases' share of the work, as "name:percent,name:percent" ("none" when there was no work).
inline std::string dspTopPhasesText(const std::uint64_t* ns, const char* const* names, unsigned count) {
    std::uint64_t busy = 0;
    for (unsigned i = 1; i < count; ++i) { busy += ns[i]; }
    if (busy == 0) { return "none"; }
    bool used[32] = {};
    std::string out;
    for (unsigned round = 0; round < 2; ++round) {
        unsigned best = 0;
        for (unsigned i = 1; i < count && i < 32; ++i) {
            if (!used[i] && (best == 0 || ns[i] > ns[best])) { best = i; }
        }
        if (best == 0 || ns[best] == 0) { break; }
        used[best] = true;
        char piece[48];
        std::snprintf(piece, sizeof(piece), "%s%s:%u", out.empty() ? "" : ",", names[best], static_cast<unsigned>((ns[best] * 100u + busy / 2u) / busy));
        out += piece;
    }
    return out.empty() ? std::string("none") : out;
}

// THE BASELINE LINE: how much margin the machine has, in every session, whether or not it ever drops.
//   dsp baseline: rate=<S/s> block=<samples> budget_us=<the block's duration> cost_us=<mean busy per block> headroom=<budget/cost> top=<phase>:<%>,...
// `blocks` and `busyNs` are over the last few seconds. With no block in that window cost and headroom say "unknown".
inline std::string dspBaselineText(double rateHz, std::size_t blockSamples, std::uint64_t blocks, std::uint64_t busyNs, const std::string& top) {
    char line[200];
    const double budgetUs = rateHz > 0.0 ? 1.0e6 * static_cast<double>(blockSamples) / rateHz : 0.0;
    if (blocks == 0 || busyNs == 0 || budgetUs <= 0.0) {
        std::snprintf(line, sizeof(line), "dsp baseline: rate=%.0f block=%zu budget_us=%.0f cost_us=unknown headroom=unknown top=%s", rateHz,
                      blockSamples, budgetUs, top.c_str());
        return line;
    }
    const double costUs = static_cast<double>(busyNs) / 1000.0 / static_cast<double>(blocks);
    const double headroom = costUs > 0.0 ? budgetUs / costUs : 0.0;
    std::snprintf(line, sizeof(line), "dsp baseline: rate=%.0f block=%zu budget_us=%.0f cost_us=%.1f headroom=%.1f top=%s", rateHz, blockSamples,
                  budgetUs, costUs, headroom, top.c_str());
    return line;
}

}  // namespace cascade::core
