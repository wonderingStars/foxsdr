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
#include <cstdint>

#include "core/i18n.hpp"  // FOX_TR_NOOP: txLampText is drawn through tr()
#include "engine/tx_frequency.hpp"
#include "foxsdr_api.h"

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
//
// txFrequencyHz is in engine/tx_frequency.hpp (included above), where the
// engine's transmitter-follows-the-dial step can reach it.

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
// THE PAGE'S KEY, AS COMMANDS ON THE WINDOW'S ENGINE API SESSION (engine
// stage 4; API 0.2's THE KEY, docs/engine-stage4.md decision 1). The frame
// loop (AppWindow::pumpTransmitKey) calls this once a frame, after the page
// has been drawn - never the page itself, because a page that is closed or
// rolled up is not drawn and so could not then let go of anything (0.99.35).
// It submits what this returns as ONE batch; the engine applies it on its
// next pass, under its rules, and the page draws the key from the state its
// session reads.

// What the page did this frame, and what the state it drew from showed.
struct TxKeyFrame {
    bool pageLive = false;      // drawn with its controls on screen (open, not rolled up)
    bool pttHeld = false;       // the PTT key or the spacebar is down
    bool latchPressed = false;  // the LATCH key was clicked
    int armRequest = -1;        // "Allow remote transmit" clicked: -1 no, 0 off, 1 on
    bool latchedShown = false;  // FOXAPI_RX_TX_LATCHED in the state drawn
    bool latchMine = false;     // ...and FOXAPI_RX_TX_KEY_MINE with it
};

// What the frame loop remembers between frames.
struct TxKeyMemory {
    bool pttSent = false;      // a PTT 1 went out on the last frame
    bool armedByUs = false;    // this window's consent to remote transmit stands
    bool pageWasLive = false;  // the page was live on the last frame
    // A LATCH 1 went out and the state has not yet shown this window's latch
    // (it lands one pass later): a page that goes away first must still end
    // it - A PAGE CLOSE ENDS ANY PENDING PRESS (the Opus review of 47e991e,
    // carried over to the latch protocol).
    bool latchPending = false;
};

inline constexpr std::uint32_t kTxKeyMaxCommands = 6;

// Fills `out` (room for kTxKeyMaxCommands) and returns how many:
//   - the PTT is a HOLD the interface re-asserts: TX_PTT 1 every frame it is
//     held, TX_PTT 0 once, on the frame it is let go;
//   - the LATCH key is EDGE-triggered: a press on a key shown released sends
//     TX_LATCH 0 then TX_LATCH 1, a press on a key shown latched TX_LATCH 0 -
//     so a deliberate press latches whatever ended the last latch, and the
//     key is never sent level (two windows sending levels fight);
//   - the remote-transmit switch sends TX_REMOTE_ARM of what was clicked;
//   - a page that stops being live lets go of what this window holds: its
//     PTT, the latch if it is this window's - or a press of its own that has
//     not landed yet - and its consent.
inline std::uint32_t txKeyCommands(const TxKeyFrame& f, TxKeyMemory& m, FoxCommand* out) {
    std::uint32_t n = 0;
    auto add = [&](std::uint32_t op, long long v) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = op;
        c.ival[0] = v;
        out[n++] = c;
    };
    if (f.latchedShown && f.latchMine) { m.latchPending = false; }  // it landed
    if (f.pageLive) {
        if (f.pttHeld) {
            add(FOXAPI_OP_TX_PTT, 1);
            m.pttSent = true;
        } else if (m.pttSent) {
            add(FOXAPI_OP_TX_PTT, 0);
            m.pttSent = false;
        }
        if (f.latchPressed) {
            add(FOXAPI_OP_TX_LATCH, 0);
            if (!f.latchedShown) {
                add(FOXAPI_OP_TX_LATCH, 1);
                m.latchPending = true;
            } else {
                m.latchPending = false;
            }
        }
        if (f.armRequest >= 0) {
            add(FOXAPI_OP_TX_REMOTE_ARM, f.armRequest);
            m.armedByUs = f.armRequest == 1;
        }
    } else if (m.pageWasLive) {
        if (m.pttSent) { add(FOXAPI_OP_TX_PTT, 0); }
        // This window's latch, or its press not yet landed - unless the state
        // shows ANOTHER window's latch, which this page's release would end.
        const bool othersLatch = f.latchedShown && !f.latchMine;
        if (f.latchMine || (m.latchPending && !othersLatch)) { add(FOXAPI_OP_TX_LATCH, 0); }
        if (m.armedByUs) { add(FOXAPI_OP_TX_REMOTE_ARM, 0); }
        m.pttSent = false;
        m.armedByUs = false;
        m.latchPending = false;
    }
    m.pageWasLive = f.pageLive;
    return n;
}

// --- the audio meter ---------------------------------------------------------

// A peak in [0, 1] as the fraction of the meter to fill, on a decibel scale
// over `floorDb` - because a linear audio meter spends nine tenths of its
// travel in the top 20 dB and reads as dead for normal speech.
float txMeterFraction(float peak01, double floorDb = -50.0);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TRANSMIT_PAGE_HPP
