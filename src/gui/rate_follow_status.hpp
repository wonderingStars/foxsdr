// rate_follow_status.hpp - what the device panel's red line says after the DSP
// chain has tried to follow the radio's sample rate.
//
// WHY THIS EXISTS. A refused rate writes "DSP rate-follow refused ..." into the
// panel's error line, and until 0.99.30 nothing ever took it away: pick a rate
// the chain cannot serve, go back to one it can, and the chain was running
// happily at the new rate under a red line saying it had refused. A beta
// tester's screenshot showed exactly that - the combo back on 2.048 MS/s and
// the refusal of 2.56 MS/s still on screen.
//
// THE RULE. A refusal says so. An accepted rate clears a refusal - and ONLY a
// refusal: the same line carries device errors (a failed gain or bias-tee
// write, a radio that could not be reopened), and a rate change says nothing
// about those, so they are left exactly as they were.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include "source/iq_source.hpp"

namespace cascade::gui {

// Every refusal line starts with this, and nothing else on that line does.
inline constexpr const char kRateFollowRefusedPrefix[] = "DSP rate-follow refused";

// The panel's error line after a rate-follow attempt. `current` is what the
// line says now; `accepted` is Pipeline::setInputRateHz's answer for
// `requestedHz`; `chainHz` is the rate the chain is running at afterwards.
inline std::string sourceErrorAfterRateFollow(const std::string& current, bool accepted,
                                              double requestedHz, double chainHz) {
    if (!accepted) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %.0f S/s; chain stays at %.0f",
                      kRateFollowRefusedPrefix, requestedHz, chainHz);
        return buf;
    }
    if (current.rfind(kRateFollowRefusedPrefix, 0) == 0) { return std::string(); }
    return current;
}

// --- a rate the radio COERCED on a call that succeeded -----------------------
//
// WHY THIS EXISTS (bug hunt 2026-09-24). Some drivers do not refuse a rate
// they cannot run; they run the nearest one they can and say why through
// lastError() on a call that RETURNS TRUE - the RX888 above the ADC's Nyquist
// (its VHF tuner's IF allows 8 MS/s at most), the Pluto above its board's
// maximum. Every call site read lastError() only on a false return, so the
// reason never reached the panel, and the Rate combo was pointed at the entry
// asked for rather than the rate the radio ran at. An RX888 tuned from HF into
// VHF at more than 8 MS/s narrowed its rate inside setCenterFrequencyHz, and
// the retune path never told the DSP chain to follow.
//
// THE RULE, the same shape as the refusal line above: a coercion says so, in
// the driver's words when it gave any on this call and in plain numbers when
// it did not; a later rate that lands where asked takes a coercion line away -
// and ONLY a coercion line.

// Every coercion line starts with this, and nothing else on that line does.
inline constexpr const char kRateCoercedPrefix[] = "Sample rate coerced: ";

// Whether a radio runs at a materially different rate from the one asked:
// more than 1 Hz AND more than 1 ppm apart, so a driver rounding to its own
// clock grid is not a coercion. A NaN either side counts as elsewhere.
inline bool rateLandedElsewhere(double requestedHz, double landedHz) {
    const double tol = std::fmax(1.0, std::fabs(requestedHz) * 1e-6);
    return !(std::fabs(landedHz - requestedHz) <= tol);
}

// The panel's error line after a rate change the source ACCEPTED. `current` is
// what the line says now; `errBefore`/`errAfter` are the source's lastError()
// on either side of the call - a sentence that appeared during it is the
// driver's reason, and one that did not is somebody else's (a stale failure,
// or the same coercion asked twice) and is not passed off as the reason.
inline std::string sourceErrorAfterRateSet(const std::string& current, double requestedHz,
                                           double landedHz, const std::string& errBefore,
                                           const std::string& errAfter) {
    if (!rateLandedElsewhere(requestedHz, landedHz)) {
        if (current.rfind(kRateCoercedPrefix, 0) == 0) { return std::string(); }
        return current;
    }
    if (!errAfter.empty() && errAfter != errBefore) { return kRateCoercedPrefix + errAfter; }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%sthe radio runs at %.6g MS/s, not the %.6g MS/s asked",
                  kRateCoercedPrefix, landedHz / 1e6, requestedHz / 1e6);
    return buf;
}

// Every setSampleRateHz the GUI makes goes through here. `ok` is the call's
// answer; `landedHz` is the READBACK - what the Rate combo must point at;
// `sourceError` is the panel's line afterwards: the driver's reason on a
// refusal (as always), the coercion rule above on success.
struct RateSetOutcome {
    bool ok = false;
    double landedHz = 0.0;
    std::string sourceError;
};

inline RateSetOutcome applySourceRate(cascade::source::IqSource& src, double requestedHz,
                                      const std::string& currentError) {
    const std::string errBefore = src.lastError();
    RateSetOutcome out;
    out.ok = src.setSampleRateHz(requestedHz);
    const std::string errAfter = src.lastError();
    out.landedHz = src.sampleRateHz();
    out.sourceError = out.ok ? sourceErrorAfterRateSet(currentError, requestedHz, out.landedHz,
                                                       errBefore, errAfter)
                             : errAfter;
    return out;
}

// After a retune the source APPLIED: did the tune move the sample rate (an
// RX888 crossing from HF into VHF narrows to 8 MS/s), and if so what the line
// says. `rateMoved` true means the caller must point the Rate combo at
// `rateHz` and make the DSP chain follow it. A retune that left the rate alone
// leaves the line alone.
struct RetuneRateOutcome {
    bool rateMoved = false;
    double rateHz = 0.0;
    std::string sourceError;
};

inline RetuneRateOutcome rateAfterRetune(const cascade::source::IqSource& src, double rateBeforeHz,
                                         const std::string& errBefore,
                                         const std::string& currentError) {
    RetuneRateOutcome out;
    out.rateHz = src.sampleRateHz();
    out.rateMoved = rateLandedElsewhere(rateBeforeHz, out.rateHz);
    out.sourceError = out.rateMoved ? sourceErrorAfterRateSet(currentError, rateBeforeHz,
                                                              out.rateHz, errBefore,
                                                              src.lastError())
                                    : currentError;
    return out;
}

}  // namespace cascade::gui
