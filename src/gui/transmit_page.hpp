// transmit_page.hpp - the TRANSMIT page's arithmetic, with no ImGui in it.
//
// The page itself is drawn in app_window.cpp like every other; what lives
// here is everything about it that can be WRONG in a way a screenshot would
// not show - which frequency the transmitter should be on, where the power
// control sits in the board's own span, and what the lamp is entitled to say.
// tests/test_transmit_page.cpp pins all three without a GL context.
//
// WHY THE LICENCE SENTENCE IS A CONSTANT IN A HEADER. It is on the page, in
// the README and in nothing else, and it has to be the same words in both. It
// is also the one piece of text in this product that is not about the product:
// see kTxLicenceNotice for what it says and why it says it plainly rather than
// as a warning nobody reads.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_TRANSMIT_PAGE_HPP
#define CASCADE_GUI_TRANSMIT_PAGE_HPP

#include <cstddef>
#include <cstdio>

#include "core/i18n.hpp"  // FOX_TR_NOOP: txLampText is drawn through tr()
#include "core/tx_audio.hpp"  // TxArming::Refusal: the sentences below name its reasons

namespace cascade::gui {

// THE SENTENCE ON THE PAGE.
//
// Said once, plainly, where the controls are, and not repeated as a dialog
// anybody has to dismiss - a warning that has to be clicked past is a warning
// that gets clicked past. It states the two facts an operator needs and
// nothing else: that this puts RF out of a connector and that whether they
// may is a question about them and their country, not about this software.
//
// THE POWER FIGURE IS THE BOARD'S PUBLISHED ONE. Analog Devices give an
// ADALM-Pluto's transmit output as roughly +7 dBm at full scale - about five
// milliwatts, which is a useful number to have in front of somebody deciding
// whether to connect an antenna or an attenuator. It is "about" because it
// varies with frequency across the AD9361's range and this product has never
// measured one.
inline constexpr const char* kTxLicenceNotice =
    "This transmits. An ADALM-Pluto puts out about +7 dBm - roughly five milliwatts - and "
    "whether you may radiate it on the frequency you have set is your responsibility and your "
    "country's licensing authority's, not this software's.";

// --- the frequency -----------------------------------------------------------

// Which frequency the transmitter should be on this frame.
//
// NOT SPLIT is the default and is the answer almost everybody wants: the
// transmitter follows the receiver, so tuning the dial moves both and a reply
// goes out where the call came in. SPLIT unlinks them, which is what a
// repeater, a DX pile-up and a satellite all need - and which is also the
// state where somebody transmits somewhere they are not listening, so the
// page lights the key while it is on.
inline double txFrequencyHz(bool split, double splitHz, double receiverHz) {
    return split ? splitHz : receiverHz;
}

// --- the power ---------------------------------------------------------------
//
// THE CONTROL IS AN ATTENUATION AND ITS "MORE" DIRECTION IS DOWN, which is the
// single most dangerous thing about this page: on an AD9361 0 dB is FULL
// OUTPUT and -89.75 dB is as quiet as the part goes. Everything below turns
// that into a number a person reads as power, so that nothing on the panel has
// to be read backwards - but the value handed to the radio is always the
// board's own decibels, and source/tx_sink.hpp says why no conversion is done
// down there.

// Where `db` sits in the board's published span, as 0 (quietest) to 1 (full
// output) - which is the direction a slider moves and a bar fills. Returns 0
// for a degenerate span rather than dividing by it.
inline float txPowerFraction(double db, double quietDb, double loudDb) {
    const double span = loudDb - quietDb;
    if (!(span > 0.0)) { return 0.0f; }
    double t = (db - quietDb) / span;
    // Negated comparisons so a number that is not one lands on the QUIET end
    // rather than passing every ordering test and being drawn as full.
    if (!(t > 0.0)) { t = 0.0; }
    if (t > 1.0) { t = 1.0; }
    return static_cast<float>(t);
}

// The inverse: a slider position back into the board's decibels.
inline double txPowerFromFraction(float t, double quietDb, double loudDb) {
    double f = static_cast<double>(t);
    if (!(f > 0.0)) { f = 0.0; }
    if (f > 1.0) { f = 1.0; }
    return quietDb + (loudDb - quietDb) * f;
}

// What the readout says. "-20.0 dB" for anything in the span, and the word
// QUIET at the bottom end - because "-89.8 dB" is a number an operator has to
// think about and "QUIET" is not, and the bottom end is where a transmitter
// should be whenever nobody has deliberately turned it up.
void formatTxPower(char* out, std::size_t cap, double db, double quietDb);

// --- the lamp ----------------------------------------------------------------
//
// FOUR STATES, and the distinction that matters is between the middle two: a
// page with no radio on it and a page with a radio that is not keyed look
// identical unless something says which. The first is a setup problem and the
// second is a working transmitter.
enum class TxLamp : int { NoRadio = 0, Ready, Transmitting, Fault };

inline TxLamp txLampState(bool haveRadio, bool faulted, bool transmitting) {
    // FAULT OUTRANKS TRANSMITTING, deliberately. A sink that has faulted has
    // already stopped; showing a transmit lamp over it would be the panel
    // claiming RF that is not there, which is the one direction this readout
    // must never be wrong in.
    if (faulted) { return TxLamp::Fault; }
    if (!haveRadio) { return TxLamp::NoRadio; }
    return transmitting ? TxLamp::Transmitting : TxLamp::Ready;
}

// The word on the glass. Short, because it is lettered beside the key.
inline const char* txLampText(TxLamp l) {
    switch (l) {
        case TxLamp::NoRadio: return FOX_TR_NOOP("NO RADIO");
        case TxLamp::Ready: return FOX_TR_NOOP("READY");
        case TxLamp::Transmitting: return FOX_TR_NOOP("ON AIR");
        case TxLamp::Fault: return FOX_TR_NOOP("FAULT");
    }
    return "";
}

// --- the key -----------------------------------------------------------------
//
// WHAT THE PAGE ASKS THE TRANSMITTER FOR THIS FRAME, applied once a frame in
// the frame loop just before Transmitter::tick() - never from inside the page,
// because a page that is closed or rolled up is not drawn and so could not
// then release anything. Until 0.99.35 the propagation lived in the page body:
// closing or rolling up the page with the LATCH on left the radio keyed for up
// to a minute with no control on screen, and a PTT held at that moment stayed
// held indefinitely.
//
// THE TRANSMITTER OWNS THE LATCH; THE PAGE ONLY SENDS PRESSES. It clears the
// latch itself - its one-minute failsafe, a fault, the frozen-window handle, a
// radio that would not key - and the page used to write its OWN remembered
// value back over that every frame, which re-latched (and so re-keyed) the
// radio the frame after each of those released it.
struct TxPageKey {
    bool latched = false;
    bool pttHeld = false;
};

// pageLive:           the page was drawn this frame with its controls on
//                     screen - open AND not rolled up.
// transmitterLatched: Transmitter::latched() as it stands now.
// latchPressed:       the LATCH key was clicked this frame.
// pttHeld:            something held the PTT down this frame.
inline TxPageKey txPageKey(bool pageLive, bool transmitterLatched, bool latchPressed,
                           bool pttHeld) {
    // No page on screen, no key: nobody can see the lamp or reach LATCH.
    if (!pageLive) { return TxPageKey{}; }
    TxPageKey k;
    k.latched = latchPressed ? !transmitterLatched : transmitterLatched;
    k.pttHeld = pttHeld;
    return k;
}

// --- the audio meter ---------------------------------------------------------

// A peak in [0, 1] as the fraction of the meter to fill, on a decibel scale
// over `floorDb` - because a linear audio meter spends nine tenths of its
// travel in the top 20 dB and reads as dead for normal speech.
float txMeterFraction(float peak01, double floorDb = -50.0);

// --- PLUGINS THAT TRANSMIT (0.99.74, CASCADE_CAP_TX_AUDIO) ---------------------
//
// The page's half of the plugin key: the sentences the operator reads while a
// plugin is allowed to transmit, and the one the Fitted modules plate prints. The
// rule itself (what has to be true for a plugin to key) is core/tx_audio.hpp's
// pluginMayKey(); nothing here decides anything, it only SAYS - which is why a wrong
// sentence here is a test failure and not a safety failure, and why every one of
// them is a constant a test can read.

// Why ARM was refused, in the words the page prints under the keys. The first two are
// the ones an operator can fix; the third cannot happen from the page's own keys and
// is here so the function is total.
inline const char* txArmRefusalText(core::TxArming::Refusal r) {
    switch (r) {
        case core::TxArming::Refusal::None: return "";
        case core::TxArming::Refusal::NoRadio:
            return FOX_TR_NOOP("Not armed: no transmitter is open. Open the radio above first.");
        case core::TxArming::Refusal::NoGrant:
            return FOX_TR_NOOP("Not armed: no plugin has been granted TRANSMIT. Grant one on its "
                               "page in Fitted modules first.");
        case core::TxArming::Refusal::BadMinutes:
            return FOX_TR_NOOP("Not armed: that is not one of the offered times.");
    }
    return "";
}

// "4:32" for 272 seconds, "60:00" for an hour. Whole seconds, minutes unbounded
// (the longest window is 60 minutes, so it never needs an hour column).
inline void formatTxArmCountdown(char* out, std::size_t cap, int secondsLeft) {
    if (out == nullptr || cap == 0) { return; }
    if (secondsLeft < 0) { secondsLeft = 0; }
    std::snprintf(out, cap, "%d:%02d", secondsLeft / 60, secondsLeft % 60);
}

// What one plugin's plate says about its transmit state, most important first. The
// order is the order of precedence: a plugin that has stopped for good is not
// "waiting", one that holds the transmitter is not merely "armed".
enum class TxPlateState : int {
    NotAsked = 0,      // it has never wanted the transmitter: nothing to say
    NotGranted,        // it has asked and has no grant
    GrantedDisarmed,   // granted, and the operator has not armed the page
    Armed,             // granted and armed: it will be keyed when it asks
    OnAir,             // it holds the transmitter and the radio is keyed for it
    Waiting,           // granted and wanting the key, but another plugin holds it
    Stopped,           // its pull() reported a permanent error
};

inline TxPlateState txPlateState(bool asked, bool granted, bool armed, bool onAir, bool waiting,
                                 bool stopped) {
    if (stopped) { return TxPlateState::Stopped; }
    if (onAir) { return TxPlateState::OnAir; }
    if (waiting) { return TxPlateState::Waiting; }
    if (granted) { return armed ? TxPlateState::Armed : TxPlateState::GrantedDisarmed; }
    return asked ? TxPlateState::NotGranted : TxPlateState::NotAsked;
}

inline const char* txPlateText(TxPlateState s) {
    switch (s) {
        case TxPlateState::NotAsked: return "";
        case TxPlateState::NotGranted:
            return FOX_TR_NOOP("It has asked to transmit and is not allowed to.");
        case TxPlateState::GrantedDisarmed:
            return FOX_TR_NOOP("Allowed to transmit, but you have not armed plugins on the "
                               "TRANSMIT page, so it cannot.");
        case TxPlateState::Armed:
            return FOX_TR_NOOP("ARMED: it will be keyed when it asks, until the window closes.");
        case TxPlateState::OnAir:
            return FOX_TR_NOOP("ON AIR: this module has the transmitter.");
        case TxPlateState::Waiting:
            return FOX_TR_NOOP("Another plugin holds the transmitter, so this one waits.");
        case TxPlateState::Stopped:
            return FOX_TR_NOOP("Its transmit audio reported a permanent error and has been "
                               "stopped; stop and start the module to try again.");
    }
    return "";
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TRANSMIT_PAGE_HPP
