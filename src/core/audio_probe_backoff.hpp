// THE AUDIO WATCHDOG'S RETRY SCHEDULE (2026-11-23, review of the ringdrop branch, section 5): how long the engine waits between its probes of
// the sound output, as a pure rule so a test can pin every step.
//
// The watchdog asks once per interval whether the output stream is alive. While it is alive the interval is one second, so a death is noticed
// within a second. Each probe that finds the stream dead (and so starts a re-open) doubles the interval, 1, 2, 4, up to a CEILING OF FIVE
// SECONDS: a device that keeps refusing is not asked once a second for ever, but a device that comes back (a headset reconnecting, a monitor's
// audio waking) is found again within five seconds, not thirty. The interval goes back to one second the moment a re-open SUCCEEDS (and when a
// probe finds the stream alive), so the NEXT death is noticed at once and not after a leftover long wait.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>

namespace cascade::core {

struct AudioProbeBackoff {
    static constexpr double kMinIntervalS = 1.0;
    static constexpr double kMaxIntervalS = 5.0;

    double intervalS = kMinIntervalS;

    // A probe found the stream dead and starts a re-open: wait longer before the next probe, to the ceiling.
    void onProbeDead() { intervalS = std::min(kMaxIntervalS, intervalS * 2.0); }
    // A probe found the stream alive, or a re-open succeeded: the next death is noticed within a second again.
    void onAlive() { intervalS = kMinIntervalS; }
};

}  // namespace cascade::core
