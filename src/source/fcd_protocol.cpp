// fcd_protocol.cpp - the FUNcube Dongle Pro's six-stage IF gain distribution, ported as facts from
// pothosware/SoapyFCDPP (Boost Software License 1.0, Copyright (c) 2018 Albin Stigo; see fcd_protocol.hpp and
// THIRD-PARTY-NOTICES.md). The tables are SoapyFCDPP's fcd.c; the code is rewritten.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/fcd_protocol.hpp"

namespace cascade::source::fcd {

namespace {

// dB per register value, per stage
constexpr int kStage1[] = {-3, 6};
constexpr int kStage2[] = {0, 3, 6, 9};
constexpr int kStage3[] = {0, 3, 6, 9};
constexpr int kStage4[] = {0, 1, 2, 3};
constexpr int kStage5[] = {3, 6, 9, 12, 15};
constexpr int kStage6[] = {3, 6, 9, 12, 15};
const int* const kStageDb[6] = {kStage1, kStage2, kStage3, kStage4, kStage5, kStage6};
constexpr int kStageCount[6] = {2, 4, 4, 4, 5, 5};

// cutoffs: element 0 is how many follow; stage register = the highest j whose cutoff the gain reaches
constexpr int kCut1[] = {1, 45};
constexpr int kCut2[] = {6, 39, 42, 45, 48, 51, 54};
constexpr int kCut3[] = {3, 30, 33, 36};
constexpr int kCut4[] = {0};
constexpr int kCut5[] = {4, 18, 21, 24, 27};
constexpr int kCut6[] = {4, 6, 9, 12, 15};
const int* const kCuts[6] = {kCut1, kCut2, kCut3, kCut4, kCut5, kCut6};

}  // namespace

void proIfStageRegisters(int gainDb, std::uint8_t regs[6]) {
    int previous = 0;
    for (int i = 0; i < 6; ++i) {
        const int* cut = kCuts[i];
        int val = 0;
        for (int j = 1; j <= cut[0]; ++j) {
            if (gainDb >= cut[j]) { val = j; }
        }
        if (i == 1 && previous != 0) { val -= 3; }
        if (val < 0) { val = 0; }
        if (i == 3) { val = gainDb % 3; if (val < 0) { val = 0; } }
        previous = val;
        regs[i] = static_cast<std::uint8_t>(val);
    }
}

int proStageDb(int stage, std::uint8_t reg) {
    if (stage < 0 || stage > 5 || reg >= kStageCount[stage]) { return 0; }
    return kStageDb[stage][reg];
}

int proIfTotalDb(const std::uint8_t regs[6]) {
    int sum = 0;
    for (int i = 0; i < 6; ++i) { sum += proStageDb(i, regs[i]); }
    return sum;
}

}  // namespace cascade::source::fcd
