// workspace_testseam.hpp - TEST ONLY: made-up radios 2 to N for the hidden-desktop drive harness (docs/multi-radio-records/screen-s3.md).
//
// Engine stage 4 (radios 2 to 8 live) is not in this tree: the engine has one radio and refuses a command aimed at radio 2. To show, drive and screenshot the workspace with several radios
// anyway, a BOUNDED run (a test's `--frames N` run, never a session) can name FOXSDR_SLINT_TEST_RADIOS=<2..8>: the host then answers for radios 2 to N from this class instead of the engine -
// each with its own made-up state and its own tone in its spectrum, commands aimed at it applied to that state, and RADIO_FOCUS accepted for them. Radio 1 is always the real one.
// The environment variable is read only when the run is bounded (the same gate as FOXSDR_SLINT_VIEW and the other test hooks), and nothing in the window reaches it: a shipped session
// never has fake radios. When stage 4 is merged the host's reads go to the engine for every radio and this seam is the part to delete (or keep as a test fixture).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstdint>

#include "foxsdr_api.h"
#include "radio_view.hpp"

namespace foxsdr::slinthost {

// The number of made-up radios the environment asks for: 0 (off) unless the run is bounded and the value is 2..kMaxRadios.
int testRadiosFromEnv(const char* value, bool bounded);

class SeamRadios {
public:
    explicit SeamRadios(int count) : count_(count < 1 ? 1 : count > kMaxRadios ? kMaxRadios : count) {}
    int count() const { return count_; }
    std::uint32_t mask() const { return (1u << count_) - 1u; }
    bool fake(int slot) const { return slot >= 1 && slot < count_; }

    // The radio's state: slot 0 is the engine's `real` state untouched; a made-up radio's is made from it on first use (its own device name, centre, tuned frequency, mode, running) and kept.
    FoxReceiverState& state(int slot, const FoxReceiverState& real);
    // A command aimed at a made-up radio changes ITS state (SET_FREQUENCY, VFO_TO_ABSOLUTE, SET_VFO_OFFSET, STEP_TUNE, RUN). True if the op is one it knows.
    bool apply(int slot, const FoxCommand& c, const FoxReceiverState& real);
    // A made-up radio's spectrum frame: 1024 bins, a noise floor and ONE tone whose place is the radio's own (toneFraction); a new frame each call.
    int32_t spectrum(int slot, std::uint64_t sinceSeq, FoxSpectrumInfo& info, float* bins, std::uint32_t cap, const FoxReceiverState& real);
    static double toneFraction(int slot) { return 0.15 + 0.09 * slot; }    // of the span, from its low edge
    // The engine's answer to RADIO_FOCUS for a radio (1-based), as the real engine gives it: OK for a radio there is, NO_DEVICE otherwise.
    int32_t focus(std::uint32_t radio) const { return radio >= 1 && static_cast<int>(radio) <= count_ ? FOXAPI_OK : FOXAPI_NO_DEVICE; }

private:
    int count_;
    std::array<FoxReceiverState, kMaxRadios> states_{};
    std::array<bool, kMaxRadios> made_{};
    std::array<std::uint64_t, kMaxRadios> seq_{};
};

}  // namespace foxsdr::slinthost
