# Drive fixes 7: a window edge follows the pointer (2026-11-18)

Branch `fix/drive-7`, cut from `engine/multi-radio` at `abce63d0`. One fault, found by the owner on the release candidate the night of the release, with a real mouse on a 5120 x 1440 screen.

## The fault

Dragging the right edge, the bottom edge or the corner of the new window made the window's size jump: in the owner's drag the width swung between the smallest window (1024) and the 16384 pixel limit of the window guard, and the window ended at a size the pointer had not asked for.

Cause, in two parts:

1. `ui/main.slint`'s three edges reported how far the pointer was from where it was pressed, measured from the edge itself, and `main.cpp` asked for "the size the window has now plus that distance". The window takes a frame or more to take a size, so every pointer move that arrived in between added the same distance again: overshoot, then a negative distance and an undershoot.
2. An edge that is dragged quickly is soon behind the pointer; the pointer is then outside the window, where the edge's press is taken away (`pointer-event` cancel) and it hears no more moves. With part 1 repaired alone, a fast drag stopped short (asked +200 px, got +53).

## Why the mouse drive did not find it

The drive's pointer tool moved in a few slow steps and judged the size after the drag had ended. At low rates the size comes out right once the pointer stops; the fault needs moves that arrive faster than the window resizes, which a real mouse sends.

## The change

- `look_logic.hpp/.cpp`, `EdgeDrag`: the size to ask for is a function of the press and of where the pointer is, never of the size the window has at that moment. Any number of moves at one pointer position ask for one size. Clamped to the smallest window at the scale in force and to the guard's limit.
- `gaps_win.hpp/.cpp`, `systemPointer`: the pointer's place on the screen and whether its primary button is held, asked of the system (Windows; false elsewhere).
- `ui/main.slint`: the edges report the press (`resize-drag-began`), the pointer's place in the window (`resize-drag`) and the end (`resize-drag-ended`).
- `main.cpp`: where the system can be asked, a drag follows the system's pointer from the press on, on every move the edge still hears and on every 33 ms tick, until the button is let go; the pointer leaving the window is not the end. A bounded run (the tests, whose pointer is not the system's) and a system that cannot be asked keep to the edge's own moves, with the same `EdgeDrag` arithmetic.

## Proof

Tool: `D:/foxsdr-build/resize-capture/drag-test.ps1` (a real pointer through SetCursorPos and a real button press; it refuses to press unless the window owns the point), at two rates: about 50 moves a second, and as fast as the pointer can be moved (6000 moves in 0.9 s).

Before (the candidate built from `abce63d0`):

| Drag | Pointer travel | Window ended | Right answer |
|---|---|---|---|
| right edge, 50 moves/s | +300 | 1407 (mid-drag samples 101 over, 176 and 303 under) | 1407 |
| right edge, full rate | +200 | 1863 | 1607 |

After (this branch, staged with the helper beside it):

| Drag | Pointer travel | Window ended | Right answer |
|---|---|---|---|
| right edge, 50 moves/s | +300 | 1900 (every sample exact) | 1900 |
| right edge, full rate | +200 | 2100 | 2100 |
| bottom edge, full rate | +150 | 1050 | 1050 |
| corner, full rate, out | +250, +120 | 2350 x 1170 | 2350 x 1170 |
| corner, full rate, in | -600, -250 | 1750 x 920 | 1750 x 920 |
| corner, 50 moves/s, out | +300, +150 | 2050 x 1070 | 2050 x 1070 |
| corner, full rate, far past the smallest window | -1600, -900 | 1024 x 640 | 1024 x 640 (the smallest window) |
| right edge from the smallest window, full rate | +500 | 1524 | 1524 |
| bottom edge, 50 moves/s | +200 | 840 | 840 |

The title-bar drag was checked with the same tool at both rates on both builds: it ends exactly where the pointer does (it was not affected and is not changed).

Unit test: `test_slint_host_look` gains `edgeDrag()` (a thousand moves at one place ask for one size; each edge; the smallest window; the limit; a scale that is not 1; a position that is not a number): 4490 checks, 0 failed.

## Not done

- No test drives a real system pointer in the gates: the gates' pointer is the toolkit's, so they exercise the edge's own moves and the arithmetic, not `systemPointer`. The proof of that path is the table above.
- Linux: the edge's own moves, with the repaired arithmetic; a fast drag there can still stop when the pointer leaves the window. Not tried on Linux.
