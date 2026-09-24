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

#include <cstdio>
#include <string>

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

}  // namespace cascade::gui
