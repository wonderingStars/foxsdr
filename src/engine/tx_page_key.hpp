// tx_page_key.hpp - what the Transmit page asks the transmitter for this frame
// (engine extraction stage 3a: the engine applies it once a frame, from
// Engine::pumpTransmitter, just before Transmitter::tick). Moved verbatim from
// gui/transmit_page.hpp, which includes it, so every caller still finds it
// where it was.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_TX_PAGE_KEY_HPP
#define CASCADE_ENGINE_TX_PAGE_KEY_HPP

#include <cstdint>

namespace cascade::gui {

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

// --- the request, as the front end hands it over ------------------------------
//
// A LATEST-VALUE SLOT (engine/stage3b-pre, docs/engine-stage3.md OPEN 7): the
// front end writes it once a frame (Engine::submitTransmitPageKey), the control
// side reads the newest one when it pumps (Engine::pumpTransmitter). Once the
// pump runs on a thread of its own it may see two frames' requests as one, or
// the same request twice - so nothing in here may be an EDGE that one reading
// could lose or two could double:
//   - pageLive and pttHeld are LEVELS - the newest wins, as it should;
//   - the LATCH press is a COUNT of every press ever made, never "pressed this
//     frame". The control side acts on the presses since the count it last saw
//     (an odd number toggles the latch, an even number leaves it), so a press
//     is neither lost between two reads nor applied twice by one;
//   - frameSeq numbers the front end's frames, so a reader can tell a new
//     request from a repeat.
// KEY-UP DOES NOT WAIT FOR THE PUMP: the page closing, or the PTT let go, is
// applied by submitTransmitPageKey on the front end's own thread, at once.
// Only key-DOWN (a PTT pressed, a LATCH press) goes through the control side.
struct TxPageRequest {
    bool pageLive = false;
    bool pttHeld = false;
    std::uint32_t latchPressCount = 0;
    std::uint64_t frameSeq = 0;
};

}  // namespace cascade::gui

#endif  // CASCADE_ENGINE_TX_PAGE_KEY_HPP
