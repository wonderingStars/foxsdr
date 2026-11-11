// The test seam's made-up radios - see workspace_testseam.hpp. TEST ONLY.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "workspace_testseam.hpp"

#include "core/app_commands.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace foxsdr::slinthost {

int testRadiosFromEnv(const char* value, bool bounded) {
    if (!bounded || value == nullptr || value[0] == '\0') { return 0; }
    const int n = std::atoi(value);
    return n >= 2 && n <= kMaxRadios ? n : 0;
}

FoxReceiverState& SeamRadios::state(int slot, const FoxReceiverState& real) {
    const std::size_t i = static_cast<std::size_t>(std::clamp(slot, 0, kMaxRadios - 1));
    if (slot <= 0) { states_[0] = real; return states_[0]; }
    if (!made_[i]) {
        FoxReceiverState s = real;
        static const std::uint32_t modes[4] = {FOXAPI_DEMOD_NFM, FOXAPI_DEMOD_WFM, FOXAPI_DEMOD_AM, FOXAPI_DEMOD_USB};
        std::snprintf(s.deviceName, sizeof(s.deviceName), "Test radio %d", slot + 1);
        s.centreHz = 100e6 + 1e6 * static_cast<double>(slot);
        s.vfoOffsetHz = 0;
        s.tunedHz = s.centreHz;
        s.demodMode = modes[i % 4];
        s.sampleRateHz = 2.4e6;
        s.bandwidthHz = 12500;
        s.volume = 0.5 + 0.05 * static_cast<double>(slot);
        s.squelchDb = -60;
        s.dbMin = -110;
        s.dbMax = 0;
        s.faultMessage[0] = '\0';
        s.audioMutedBy[0] = '\0';
        s.flags &= ~(FOXAPI_RX_FAULTED | FOXAPI_RX_MUTED | FOXAPI_RX_DECODER_ACTIVE | FOXAPI_RX_DEVICE_AGC);
        if (slot == 2) { s.flags &= ~FOXAPI_RX_RUNNING; } else { s.flags |= FOXAPI_RX_RUNNING; }   // radio 3 is the stopped one
        states_[i] = s;
        made_[i] = true;
    }
    return states_[i];
}

bool SeamRadios::apply(int slot, const FoxCommand& c, const FoxReceiverState& real) {
    if (!fake(slot)) { return false; }
    FoxReceiverState& s = state(slot, real);
    const auto retune = [&](double hz) {
        s.tunedHz = hz;
        if (std::fabs(hz - s.centreHz) > s.sampleRateHz * 0.45) { s.centreHz = hz; }   // the baseband follows a tune that leaves it
        s.vfoOffsetHz = s.tunedHz - s.centreHz;
    };
    switch (c.op) {
        case FOXAPI_OP_SET_FREQUENCY:
        case FOXAPP_OP_VFO_TO_ABSOLUTE: retune(c.num[0]); return true;
        case FOXAPI_OP_SET_VFO_OFFSET: s.vfoOffsetHz = c.num[0]; s.tunedHz = s.centreHz + s.vfoOffsetHz; return true;
        case FOXAPI_OP_STEP_TUNE: retune(s.tunedHz + static_cast<double>(c.ival[0]) * c.num[0]); return true;
        case FOXAPI_OP_RUN:
            if (c.ival[0] != 0) { s.flags |= FOXAPI_RX_RUNNING; } else { s.flags &= ~FOXAPI_RX_RUNNING; }
            return true;
        default: return false;
    }
}

int32_t SeamRadios::spectrum(int slot, std::uint64_t sinceSeq, FoxSpectrumInfo& info, float* bins, std::uint32_t cap, const FoxReceiverState& real) {
    const std::size_t i = static_cast<std::size_t>(std::clamp(slot, 0, kMaxRadios - 1));
    const FoxReceiverState& s = state(slot, real);
    constexpr std::uint32_t kBins = 1024;
    const std::uint32_t n = std::min(cap, kBins);
    ++seq_[i];
    if (seq_[i] <= sinceSeq) { seq_[i] = sinceSeq + 1; }
    std::uint32_t lcg = static_cast<std::uint32_t>(seq_[i] * 2654435761u + static_cast<std::uint32_t>(slot) * 40503u);
    const double toneAt = toneFraction(slot) * kBins;
    for (std::uint32_t b = 0; b < n; ++b) {
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = static_cast<float>((lcg >> 16) & 0xff) / 255.0f * 6.0f - 3.0f;
        const double d = (static_cast<double>(b) - toneAt) / 3.0;
        const float tone = static_cast<float>(52.0 * std::exp(-d * d));
        bins[b] = -95.0f + noise + tone;
    }
    info.structSize = sizeof(info);
    info.binCount = kBins;
    info.seq = seq_[i];
    info.centreHz = s.centreHz;
    info.spanHz = s.sampleRateHz;
    info.unixMs = 0;
    info.copied = n;
    info.flags = 0;
    return FOXAPI_OK;
}

}  // namespace foxsdr::slinthost
