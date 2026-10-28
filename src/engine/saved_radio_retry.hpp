// When a saved radio that was merely BUSY at start-up is tried again - the
// pure decision behind Engine::pollSavedRadioRetry, pulled out so it is
// checkable without a window, a pipeline or a dongle (the same reason
// autoReopenDue lives in engine/tune_control.hpp). THE FILE LIVES IN src/engine/ SINCE THE 0.99.97 MERGE (master had it in gui/; the retry runs in the
// Engine now, and the engine library may not include gui/ - the namespace is master's, as tune_control.hpp's is).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::gui {

// --- Trying a busy saved radio again, a few times, once the window is up -----
//
// The 0.99.91 field report (RTL-SDR Blog V4, Windows 11): "the Gain button no
// longer works". The gain dial was fine - there was no radio open. At the
// start-up of BOTH 0.99.91 sessions the restore of the saved radio got "the
// radio is already in use by another program" from the USB transport
// (usb::kInUseError, counted anonymously as radio_fail.rtlsdr.busy), the
// receiver went on running on the signal generator, and nothing ever tried the
// dongle again. The generator looks alive - spectrum, audio, an open squelch -
// so the session read as a radio with a dead gain control.
//
// WHY THE DONGLE WAS BUSY: the in-app updater starts the installer and then
// closes the old window, and the installer relaunches the new build while the
// old process is still letting go of the dongle. The window of overlap is
// seconds, not a session. A radio held by another program for that long is the
// ordinary case for "busy"; a radio held for good (a second copy of FoxSDR,
// SDR# left running) is the same failure, and four patient tries cost nothing.
//
// So a saved radio that failed because it was BUSY - and only then - is tried
// again through the ordinary asynchronous open, 2, 4, 8 and 16 s after the
// failure before it (30 s in all), each delay counted from the last failure,
// not from the start of the session. After the fourth failure the ordinary
// message stands, as it always did. The first delay runs from the start-up
// restore, which is before the first frame, so the first retry is 2 s into the
// frame loop.
//
// The conditions, each of which is load-bearing:
//   - keptRadioValid: something is still remembered for the config
//     (Engine::restoreKeep_). Choosing any other source replaces it, and the
//     retries stop with it.
//   - lastFailureBusy: the last attempt failed because the radio was HELD.
//     An unplugged dongle, a missing driver, a rate the radio refused will not
//     be cured by waiting, and a loop against them is noise in the log and
//     work on the bus for nothing. Their ordinary message stands at once.
//   - attemptsMade < 4: bounded. A radio that is really held stays held.
//   - !deviceOpen: a radio is already running (the user opened one by hand).
//   - !openPending && !scanPending: nothing else may be inside the driver
//     stack, and an open already in flight must not be doubled.
//   - nowSec - lastFailureSec >= the delay for this attempt (index attemptsMade
//     of kSavedRadioRetryDelaysSec). Both in Engine::retryClockS() seconds; the
//     start-up failure is stamped 0.0, so a 0.0 stamp is a real time, not
//     "never".
inline constexpr double kSavedRadioRetryDelaysSec[4] = {2.0, 4.0, 8.0, 16.0};

inline bool savedRadioRetryDue(bool keptRadioValid, bool lastFailureBusy, int attemptsMade,
                               bool deviceOpen, bool openPending, bool scanPending,
                               double nowSec, double lastFailureSec) {
    if (!keptRadioValid || !lastFailureBusy) { return false; }
    if (attemptsMade < 0 || attemptsMade >= 4) { return false; }
    if (deviceOpen || openPending || scanPending) { return false; }
    return nowSec - lastFailureSec >= kSavedRadioRetryDelaysSec[attemptsMade];
}

}  // namespace cascade::gui
