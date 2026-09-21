// Fourteen decoders running before the user has asked for one.
//
// THE REPORT (Android tester, through the owner, 2026-09-21): "After starting
// up and tuning to an FM (3m) broadcast station, all the decoders were active,
// so I had to turn them off first", alongside stuttering FM audio that only
// cleared at a lower sample rate. The Android package bundles its decoders, so
// a first run fed every one of them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/first_run_decoders.hpp"

#include "test_check.hpp"

#include <string>
#include <vector>

int main() {
    using cascade::core::decodersStoppedOnFirstRun;

    const std::vector<std::string> fitted{"acars.so", "pocsag.so", "flex.so", "adsb.so"};

    // A PHONE'S FIRST RUN: every bundled decoder starts stopped, in the list,
    // one key away - and a preset press starts the one that was pressed.
    CHECK(decodersStoppedOnFirstRun(fitted, /*onAndroid=*/true, /*firstRun=*/true) == fitted);

    // EVERY LATER RUN IS THE USER'S. The stop list is theirs by then, and a
    // launch that re-stopped everything would undo what they chose - which is
    // the same complaint in a different coat.
    CHECK(decodersStoppedOnFirstRun(fitted, true, false).empty());

    // DESKTOP IS NOT TOUCHED, first run or not: a decoder there was installed
    // by somebody who wanted it.
    CHECK(decodersStoppedOnFirstRun(fitted, false, true).empty());
    CHECK(decodersStoppedOnFirstRun(fitted, false, false).empty());

    // Nothing fitted is not a special case.
    CHECK(decodersStoppedOnFirstRun({}, true, true).empty());

    return testSummary("test_first_run_decoders");
}
