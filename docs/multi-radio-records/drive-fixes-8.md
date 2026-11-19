# Drive fixes 8: the drag of the tuned band follows the pointer (2026-11-19)

Branch `fix/drive-8`, cut from `fix/drive-7` at `d4fbfa23`. Found by the fast-pointer pass that followed drive fixes 7 (a real pointer moved at about 50 moves a second and at full rate over every drag-type control of the new window; results `D:/foxsdr-build/drive-final/results-fastpointer.csv`).

## The fault

Dragging the tuned band (the orange pass band) along the spectrum moved it only part of the pointer's way: a 200 pixel drag is 0.334 MHz in a 2 MHz picture, and at about 50 moves a second the band moved about 57 percent of that in six drags of six; at full rate three drags of six were exact and three moved about half.

Cause: each pointer move reports how far the pointer went since the last move, and the host asked the engine for "the tuned frequency in the state plus this move" (`main.cpp` `on_band_dragged`, `workspace_logic.cpp` `PaneGestures::bandDrag`). The state is read back from the engine once a tick (33 ms). Every move of a tick therefore started from the same number, and only the last move of each tick counted. A real mouse sends many moves a tick.

It is the same family as drive fixes 7 (a size worked out from a value that is late), and it was missed by the mouse drive for the same reason: the drive placed the band with a click, and where it dragged it did so in a few slow steps.

## The change

- `workspace_logic.hpp/.cpp`, `bandDragTargetHz`: the drag is added up by the host: the tuned frequency when the drag began plus everything the pointer has moved since. A move more than `kBandDragGapTicks` (3 ticks, about 100 ms) after the last one is a new drag and starts from the tuned frequency the engine reports then.
- `radio_view.hpp`: the three numbers of a drag, per radio.
- `main.cpp` (one radio) and `PaneGestures::bandDrag` (a pane of several) both use it.

The engine still clamps the offset to the baseband, as before. A pointer that goes on past the edge of the baseband has to come back the same way before the band moves again within one drag; after a pause of more than the gap the next move starts from the edge.

## Proof

`test_slint_workspace_host`: a new block (ten moves inside one tick with the state never refreshed ask for ten hundredths of the picture, not one; the drag goes on over later ticks and back; the other radio's drag is its own; a pause longer than the gap starts from the engine's number; exactly the gap does not; a move that is not a number adds nothing; a tick counter that runs backwards starts a new drag): 148 checks, 0 failed. Break-it: with the old arithmetic put back, 5 of the new checks fail (lines 367, 371, 373, 383, 386); restored, 0 fail.

The real-pointer proof on the built window is recorded in the merge record (it was run after this commit).

## Not done

- The pass band's EDGE drag (the width) was checked by the same pass and is right at both rates: it is worked out from the press, not from the state. Not changed.
- The picture's pan is a host-side view and is not read back from the engine: not affected, not changed.
