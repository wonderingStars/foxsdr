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
#include <cstdint>
#include <memory>
#include <cstdio>
#include <string>

#include "core/radio_events.hpp"
#include "source/device_source.hpp"
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
    // ONE PLAIN SENTENCE, which rate is in use and why (hardware check 2026-11-17: a saved 5 MS/s on an RTL-SDR became 3.2 MS/s and the line only said
    // "not the 5 MS/s asked"): the radio cannot run at the rate asked, so it runs at the nearest one it can.
    std::snprintf(buf, sizeof(buf), "%sthis radio cannot run at %.6g MS/s, so it runs at %.6g MS/s",
                  kRateCoercedPrefix, requestedHz / 1e6, landedHz / 1e6);
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

// `explicitRequest` (0.99.44 repair): true for a caller asking for this rate
// DELIBERATELY - a plugin preset, or one the user (or a browser/API request)
// picked by hand - false for a generic default an open asks for or a
// remembered setting being restored. It reaches IqSource::setSampleRateHzExplicit
// instead of the plain setSampleRateHz; every source but AirspySource treats
// the two identically (see iq_source.hpp).
//
// A RATE THE RADIO CANNOT DO IS REPORTED (hardware check 2026-11-17): a call that succeeded but landed on another rate is a setting the radio did not take, so it
// is queued as the same refused-setting event a typed out-of-range value is (control `rate`, refusedClass `range`), unless the driver already said so itself on this
// thread. `why` is the contract's word for what was being applied ("open", "restore", "user", "auto"); null leaves the thread's own scope (default "user"). The text
// carries no number: the rate is not part of what is reported.
inline RateSetOutcome applySourceRate(cascade::source::IqSource& src, double requestedHz,
                                      const std::string& currentError,
                                      bool explicitRequest = false, const char* why = nullptr) {
    const std::string errBefore = src.lastError();
    const std::uint64_t refusalsBefore = cascade::core::radioreport::refusalSequence();
    RateSetOutcome out;
    // A RATE THE RADIO REFUSES IS A REPORT, whoever asked and whenever (open, saved radio, plugin preset, the API, a patch page's radio): the driver says it itself at the
    // place it notices where it is wired, and this watch says it here, once, where it did not (core/radio_events.hpp RefusalWatch). A rate the driver coerced on a call that
    // succeeded is not a refusal and is not said.
    cascade::core::radioreport::RefusalWatch watch;
    out.ok = explicitRequest ? src.setSampleRateHzExplicit(requestedHz)
                             : src.setSampleRateHz(requestedHz);
    const std::string errAfter = src.lastError();
    if (!out.ok) { watch.noteRefusalIfSilent(&src, nullptr, cascade::core::radioreport::Control::Rate, errAfter); }
    out.landedHz = src.sampleRateHz();
    // A LIVE CHANGE THAT WORKED IS JOURNALLED (field data 2026-11-19: ten rate changes on one install were visible only through the next stream window). The open and
    // the restore are said by the open's own settings event (`why` given), so only a caller with no `why` - a person or a plugin preset - says it here.
    if (out.ok && why == nullptr) {
        if (const cascade::source::DeviceSource* dev = dynamic_cast<const cascade::source::DeviceSource*>(&src)) {
            cascade::core::radioreport::noteRateApplied(&src, dev->driverKey(), out.landedHz);
        }
    }
    if (out.ok && rateLandedElsewhere(requestedHz, out.landedHz)) {
        if (const cascade::source::DeviceSource* dev = dynamic_cast<const cascade::source::DeviceSource*>(&src)) {
            std::unique_ptr<cascade::core::radioreport::ControlWhyScope> scope;
            if (why != nullptr) { scope = std::make_unique<cascade::core::radioreport::ControlWhyScope>(why); }
            cascade::core::radioreport::noteRefusalIfSilentSince(refusalsBefore, &src, dev->driverKey(), cascade::core::radioreport::Control::Rate,
                                                                 "the radio cannot run at the sample rate asked; it runs at the nearest rate it can", /*userRange=*/true);
        }
    }
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
