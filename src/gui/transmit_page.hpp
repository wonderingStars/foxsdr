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

#include "core/i18n.hpp"  // FOX_TR_NOOP: txLampText is drawn through tr()

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

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TRANSMIT_PAGE_HPP
