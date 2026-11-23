// WHEN THE DSP THREAD SHEDS DISPLAY WORK (2026-11-23, the ringdrop measurement; docs/DIAGNOSTICS.md "Shedding display work").
//
// A DSP thread that runs below real time used to do ALL its work for every block and let the source's ring overflow: the radio's samples were
// dropped while the screen's spectrum was still being computed for every block. When the thread is behind, the work whose only consumer is the
// screen is skipped first; nothing a decoder, the recorder, the audio path or a plugin's I/Q tap needs is ever skipped.
//
// BEHIND means the ring holds more than half its capacity at the start of a block; the thread goes back to full work when it holds less than a
// quarter. Two thresholds, so a ring that hovers around one of them does not flap: between a quarter and a half the state does not change.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>

namespace cascade::core {

// The state for the next block, from the current state and the ring's fill (samples waiting) against its capacity. Integer arithmetic, no
// allocation, no lock: one call a block.
inline bool dspShedDecision(bool shedding, std::size_t fill, std::size_t capacity) {
    if (capacity == 0) { return false; }
    if (!shedding) { return fill > capacity / 2; }   // MORE than half (exactly half is not behind)
    return !(fill < capacity / 4);                   // back to full work only UNDER a quarter
}

// While shedding, the spectrum is still computed for one block in this many, so everything that reads a frame (the window, the scanner, the
// stitcher, tune-to-peak, the health watch) keeps getting fresh ones - fewer of them - and not the same stale one for as long as the machine is
// behind. The zoom and the other display-only steps are skipped outright.
constexpr unsigned kDspShedSpectrumEvery = 8;

}  // namespace cascade::core
