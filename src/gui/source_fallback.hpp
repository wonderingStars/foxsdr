// source_fallback.hpp - what the receiver says, remembers and shows when a
// radio it recognised ends up replaced by the signal generator (0.99.36).
//
// THE REPORT. A beta tester with an RSP1A, and a second user with an RSPdx -
// two different radios, the second one reachable only through the SDRplay API
// - both wrote "the RSP is recognized initially, but the program then switches
// to an internal source". The owner: "the same bug recurring". Nothing here
// could reproduce it on hardware (there is no RSP on this desk); what the code
// and the stored field logs show is a set of paths that each end on the
// generator, several of them without a word on the screen, and one of them
// making the NEXT launch start on the generator too:
//
//   - The dead-service fault told the user "Reconnect it and pick the source
//     again" (and the driver's own sentence "then open the radio again"). Since
//     0.99.28 an SDRplay session that has been lost refuses every open until
//     FoxSDR restarts, so doing what the screen said closed the dead radio,
//     failed to open it, and left the receiver on the generator.
//   - That failed open then left nothing remembered: the exit save wrote
//     "siggen", so the next start - after the user HAD restarted the service
//     and FoxSDR - came up on the generator as well.
//   - A re-scan while an API radio was open dropped its row (see
//     source::withClaimedSdrPlayRows), so the combo's selection index pointed
//     at whatever row slid into its place.
//
// Each rule the Source section applies is here, pure, so tests/
// test_source_fallback.cpp can hold it to account without a window, a radio
// or an SDRplay install.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

#include "gui/tune_control.hpp"

namespace cascade::gui {

// --- 1. the advice under "Device stopped" -----------------------------------
//
// The Source section prints the driver's own fault sentence and then, for
// every fault, "Reconnect it and pick the source again, or switch to the
// signal generator." That is right for an unplugged dongle and WRONG for a
// driver that has already said picking it again cannot work until FoxSDR is
// restarted - the SDRplay lost-session sentences all end that way. False means
// the generic line must not be shown: the driver's sentence is the whole
// instruction.
inline bool reconnectAdviceApplies(const std::string& faultMessage) {
    return faultMessage.find("restart FoxSDR") == std::string::npos;
}

// --- 2. what a failed switch leaves the config naming ------------------------
//
// The receiver closes the radio it has BEFORE it opens another (the 0.62.0
// field-crash rule), so a failed open leaves the generator running. Before
// 0.99.36 nothing was remembered on that path, and the exit save wrote
// "siggen": one failed attempt - including re-opening an RSP whose service had
// died, as the screen told the user to - was enough for every later launch to
// start on the generator.
//
// `alreadyKept` is what the receiver already remembers (a startup restore that
// failed); `closed` is the radio it closed to make the attempt, built with
// rememberedSourceAfterFailedOpen from the LIVE values it was running with.
// The radio that last WORKED is what is remembered, never the one that just
// failed: a row that cannot open (the native row of an RSP the SDRplay
// service owns, for one) must not become the radio every later start tries.
inline RememberedSource rememberAfterFailedSwitch(const RememberedSource& alreadyKept,
                                                  const RememberedSource& closed) {
    if (alreadyKept.valid()) { return alreadyKept; }
    if (closed.valid()) { return closed; }
    return RememberedSource{};
}

// --- 3. the sentence on the screen ------------------------------------------
//
// Which radio, why, and where the receiver is now. `why` is the driver's own
// reason - which for every SDRplay failure also says what to do - verbatim
// apart from a trailing full stop. What is remembered for the next start is
// NOT repeated here: the Source section already says it, in its own
// translated note, whenever a radio is being remembered.
inline std::string radioNotOpenedSentence(const std::string& wanted, const std::string& why) {
    std::string reason = why;
    while (!reason.empty() && (reason.back() == '.' || reason.back() == ' ')) { reason.pop_back(); }
    std::string s = wanted + " did not open";
    if (!reason.empty()) { s += ": " + reason; }
    s += ". The receiver is running on the signal generator.";
    return s;
}

// --- 4. the combo selection across a re-scan ---------------------------------
//
// The Source combo is one list: the generator, the I/Q file, the native rows,
// then the SoapySDR rows - and the selection is an INDEX into it. A re-scan
// that adds, drops or reorders any native row moves every row after it, and
// the index then names a different radio (the RSP's row gone and the Pluto's
// row slid into its place, or a B200's Soapy row shifted by one). So the
// selection is found again by what it IS.
//
// Each list is index-aligned with the combo; an entry is the row's family
// ("siggen", "file", a native driver key, "soapy") and its args. Rows 0 and 1
// never move. A row that is no longer listed answers -1, which the combo
// already reads as "the live source, by its own name".
struct SourceRowKey {
    std::string kind;
    std::string args;
    bool operator==(const SourceRowKey& o) const { return kind == o.kind && args == o.args; }
};

inline int refindSourceRow(const std::vector<SourceRowKey>& before, int sel,
                           const std::vector<SourceRowKey>& after) {
    if (sel == 0 || sel == 1) { return sel; }
    if (sel < 0 || sel >= static_cast<int>(before.size())) { return -1; }
    const SourceRowKey& was = before[static_cast<std::size_t>(sel)];
    for (std::size_t i = 2; i < after.size(); ++i) {
        if (after[i] == was) { return static_cast<int>(i); }
    }
    return -1;
}

}  // namespace cascade::gui
