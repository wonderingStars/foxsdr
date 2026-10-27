// deck_parts.hpp - the top bar's KNOB ROW and what it keeps in the window (stub campaign B12, RAIL-B; master 0.99.74).
//
// The deck (drawToolbar) carries three dials since 0.99.74 - GAIN, SQUELCH, VOLUME, in the order the signal goes through the system - with a latched AUTO
// key under each of the first two. The drawing and what the dials do are AppWindow members in app_window_deck.cpp, kept out of app_window.cpp because that file
// is at the object-file limit; this header is the two small things a second translation unit shares with it:
//   - the engraved caption helpers (barEngrave, barTrackedWidth), which drawToolbar uses for MASTER / TUNED and the knob row uses for GAIN / SQUELCH / VOLUME;
//   - DeckKnobState, the window's half of the dials: where the hand has put them, and the paced SET_GAIN that follows. The ENGINE holds everything else - the
//     radio's gain, the switch of automatic gain, the threshold and AUTOMATIC squelch (engine/engine_squelch_auto.cpp) - and the window only asks.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_DECK_PARTS_HPP
#define CASCADE_GUI_DECK_PARTS_HPP

#include <cstdint>

#include <imgui.h>

namespace cascade::gui {

// LETTER-SPACING, WHICH DEAR IMGUI HAS NOT, and which is most of what separates an engraved legend from a word in a label. Stepped a UTF-8 character at a time,
// because the captions are translated; for ASCII that is one byte.
float barTrackedWidth(ImFont* f, float px, const char* text, float track);

// A caption CUT INTO THE BRASS: the lit lower lip of the cut first, then the cut itself. `trackFrac` is the letter-spacing as a fraction of the size. 0.24 is the
// deck's; the three dials' captions close it up when the longest of them, in the language it is read in, will not fit its cell at the engraving's own spacing.
void barEngrave(ImDrawList* dl, ImVec2 at, float px, const char* text, bool centred, float trackFrac = 0.24f);

// THE GAIN KNOB'S PACE: the radio is asked at most this often while the dial is held, and the final value once more on release; a step the radio refuses (an
// answer of FAILED: RtlSdrSource::setGainDb answers "the radio is busy" when its control lock is held, and says so by failing, not by waiting) is tried again at
// this pace up to kDeckGainRetries times, then let go and the dial shows the radio's own readback.
inline constexpr double kDeckGainPaceS = 0.05;
inline constexpr int kDeckGainRetries = 8;

// What the knob row keeps between frames. The dial is read from the engine every frame it is not being turned; while the hand is on it (and for a few frames
// after, until the engine's figure has caught up) it shows where the hand put it.
struct DeckKnobState {
    // --- GAIN: the radio's first stage -----------------------------------------------------------------------------------------------
    bool gainHeld = false;               // the hand was on the dial last frame
    bool gainDirty = false;              // a target not yet asked of the radio
    float gainTargetDb = 0.0f;           // where the hand has put it (the stage's own unit)
    double gainSetS = -1.0e9;            // when the radio was last asked (ImGui time)
    int gainRetries = 0;                 // refusals since the last accepted ask
    std::uint64_t gainTicket = 0;        // the SET_GAIN in flight (0: none); its answer is judged in deckOnAnswer
    float gainSentDb = 0.0f;             // ...and what it asked for
    float gainReadDb = 0.0f;             // what the radio gave back for the last accepted ask (the SET_GAIN's answer)...
    int gainReadHold = 0;                // ...shown until the GAINS list has it, for at most this many frames
    // --- SQUELCH ---------------------------------------------------------------------------------------------------------------------
    float squelchEdit = -50.0f;          // where the hand put the dial, shown until the engine's figure has caught up
    int squelchHold = 0;                 // frames left of showing squelchEdit after the hand last moved it
    bool onAirbandLast = false;          // the dial was the Airband monitor's last frame (the hold does not carry across)
    bool airbandDirty = false;           // the monitor's new threshold is waiting for the hand to let go (a block rebuild restarts its channels)
};

// Frames a dial keeps showing the hand's figure after the last move while the engine's catches up (a command is applied on the next pass and read back the
// frame after).
inline constexpr int kDeckSquelchHoldFrames = 30;

}  // namespace cascade::gui

#endif  // CASCADE_GUI_DECK_PARTS_HPP
