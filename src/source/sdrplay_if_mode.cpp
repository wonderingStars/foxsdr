// sdrplay_if_mode.cpp - see sdrplay_if_mode.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_if_mode.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace cascade::source {

namespace abi = sdrplay_abi;

const char* sdrPlayIfModeName(SdrPlayIfMode m) {
    return m == SdrPlayIfMode::LowIf ? "low-IF" : "zero-IF";
}

bool parseSdrPlayIfMode(const std::string& text, SdrPlayIfMode& mode) {
    std::string t;
    for (const char c : text) { t += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    if (t == "zero" || t == "zero-if" || t == "zeroif") { mode = SdrPlayIfMode::ZeroIf; return true; }
    if (t == "low" || t == "low-if" || t == "lowif") { mode = SdrPlayIfMode::LowIf; return true; }
    return false;
}

namespace {
struct LowRow {
    long long outHz;
    double fsHz;
    unsigned decM;
    abi::IfKHzT ifk;
    abi::BwMHzT bw;
};
// Every row is one of the listed combinations (fs, BW, IF) with decimation a power of two
// (1, 2, 4 ... 32) so that out = fs / decM.
const LowRow kLowRows[] = {
    {62500, 2000000.0, 32, abi::IF_0_450, abi::BW_0_200},
    {125000, 2000000.0, 16, abi::IF_0_450, abi::BW_0_200},
    {250000, 2000000.0, 8, abi::IF_0_450, abi::BW_0_200},
    {500000, 2000000.0, 4, abi::IF_0_450, abi::BW_0_600},
    {1000000, 2000000.0, 2, abi::IF_0_450, abi::BW_0_600},
    {2000000, 2000000.0, 1, abi::IF_0_450, abi::BW_0_600},
    {3000000, 6000000.0, 2, abi::IF_1_620, abi::BW_1_536},
    {4000000, 8000000.0, 2, abi::IF_2_048, abi::BW_1_536},
    {8000000, 8000000.0, 1, abi::IF_2_048, abi::BW_5_000},
};
}  // namespace

bool sdrPlayRatePlanForMode(double outputRateHz, SdrPlayIfMode mode, SdrPlayRatePlan& out) {
    if (mode == SdrPlayIfMode::ZeroIf) { return sdrPlayRatePlan(outputRateHz, out); }
    out = SdrPlayRatePlan{};
    const long long r = static_cast<long long>(std::llround(outputRateHz));
    for (const LowRow& row : kLowRows) {
        if (row.outHz != r) { continue; }
        out.fsHz = row.fsHz;
        out.decM = row.decM;
        out.decEnable = row.decM > 1 ? 1u : 0u;
        out.wideBandSignal = 0;
        out.ifType = row.ifk;
        out.bwType = row.bw;
        return true;
    }
    return false;
}

std::vector<double> sdrPlaySupportedRatesForMode(SdrPlayIfMode mode) {
    if (mode == SdrPlayIfMode::ZeroIf) { return sdrPlaySupportedRatesHz(); }
    std::vector<double> v;
    for (const LowRow& row : kLowRows) { v.push_back(static_cast<double>(row.outHz)); }
    std::sort(v.begin(), v.end());
    return v;
}

LowIfRateVerdict sdrPlayLowIfRateVerdict(const SdrPlayRatePlan& plan, double measuredHz) {
    if (plan.fsHz <= 0.0 || plan.decM == 0 || !(measuredHz > 0.0)) { return LowIfRateVerdict::Other; }
    const double expect = plan.fsHz / plan.decM;
    if (std::fabs(measuredHz - expect) <= 0.02 * expect) { return LowIfRateVerdict::AsPlanned; }
    if (plan.decM != 1 && std::fabs(measuredHz - plan.fsHz) <= 0.02 * plan.fsHz) {
        return LowIfRateVerdict::DeliversAdcRate;
    }
    return LowIfRateVerdict::Other;
}

const char* sdrPlayLowIfRateVerdictSentence(LowIfRateVerdict v) {
    switch (v) {
        case LowIfRateVerdict::AsPlanned: return "The stream runs at the planned rate.";
        case LowIfRateVerdict::DeliversAdcRate:
            return "The service delivers the ADC rate, not the decimated rate: low-IF is not usable on this radio.";
        default: break;
    }
    return "The stream rate matches neither the planned rate nor the ADC rate.";
}

}  // namespace cascade::source
