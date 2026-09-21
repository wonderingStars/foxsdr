// first_run_decoders.hpp - what a phone should be doing the first time it runs.
//
// THE REPORT (Android tester, through the owner, 2026-09-21): "After starting
// up and tuning to an FM (3m) broadcast station, all the decoders were active,
// so I had to turn them off first... I'm not sure if that's intended
// behavior." In the same list: the audio stuttered until the sample rate was
// dropped to 1.92 MS/s.
//
// It was not intended, and the two are related. The Android package BUNDLES
// its decoders - fourteen of them - and a fitted decoder that nobody has
// stopped is fed. On a desktop that is a handful of percent of one core each
// and nobody notices; on a phone it is the whole audio budget, and the first
// thing a new user meets is stuttering FM and a screen full of decoders they
// did not ask for.
//
// So on a phone, the FIRST run starts with every bundled decoder stopped. Not
// removed, not hidden: stopped, in the list, one key away - and pressing any
// decoder's preset starts it, because applyPluginPreset already clears the
// stop for the module it is starting.
//
// ONLY THE FIRST RUN. After that the stop list is the user's, and a later
// launch must not undo what they chose; the file's own presence is what says
// which of the two this is.
//
// DESKTOP IS NOT TOUCHED: decoders there are installed one at a time by
// somebody who wanted them, and silently stopping them on first run would be
// answering a question nobody asked.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FIRST_RUN_DECODERS_HPP
#define CASCADE_CORE_FIRST_RUN_DECODERS_HPP

#include <string>
#include <vector>

namespace cascade::core {

// The module keys to start stopped. Empty on every run but a phone's first.
inline std::vector<std::string> decodersStoppedOnFirstRun(
    const std::vector<std::string>& decoderKeys, bool onAndroid, bool firstRun) {
    if (!onAndroid || !firstRun) { return {}; }
    return decoderKeys;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_FIRST_RUN_DECODERS_HPP
