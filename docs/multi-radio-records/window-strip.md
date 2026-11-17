# Window strip lane: the receiver strip folds instead of being cut off (2026-11-17)

Branch `feat/window-strip`, cut from `253a26a5` (feat/window-followups), worktree `D:/foxsdr-wt/en-screen-k`, build `D:/foxsdr-build/en-screen-k` (Release), hidden desktop `FoxSDRStrip`, scratch `D:/foxsdr-build/ws-scratch/`. Nothing pushed.
Real `%APPDATA%/foxsdr/config.json` sha256 before: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.

This record is written at the first commit and finished at the last (see the end for what ran).

## Design (item 1)
The one-row strip was a `HorizontalLayout`; a layout cannot wrap, so the blocks are now placed by hand inside the strip's clipped rectangle (`sb` in `ui/main.slint`), each block at its own preferred width, as the layout did.
- ROW ONE (the user's first reach): the tuned frequency, the mode, then the gain / squelch / volume lanes. ROW TWO: the STOP key with its four lamps, and the antenna-power (BIAS TEE) key. Nothing is hidden: every control is in the window at every width.
- The strip folds exactly when the one row of blocks (lanes at 396 px, the width they need to keep their captions) is wider than the window (`sb.two-rows`, from the blocks' own preferred widths, so a language with longer words folds earlier instead of cutting).
- The spectrum toolbar had the same fault (the tab row held tabs and toolbar on one 32 px row; it was cut at 1366 and 1440). It is five groups (Trace, Range, Markers, Band plan, zoom). Row one keeps the tabs and as many of the LAST groups as fit beside them (zoom and band plan are the ones used most); the earlier groups drop to a second 32 px row, right-aligned (`tbr`, `gx/gy`). With room, it is the one row it was.
- Why this and not a "more" drawer: a drawer hides controls behind a click and has to say what is inside it; the fold hides nothing and costs only height, which a laptop has more of than width.
- Cost in height when folded: the strip 104 -> 185 px (+81: row two is the taller of the STOP key, 64, and the antenna block, 73, plus 8) and the tab row 32 -> 64 px (+32). One-row widths cost nothing.
- Measured: single-row strip needs about 1404 px (32 + 4 x 29 + the five blocks), so the strip is one row at 1440 and wider and two rows at 1366, 1280 and 1024. The toolbar is one row only when the centre column (window - 64 rail - 296 panel) fits the tabs (384 px) plus the toolbar (776 px), about 1470 px of window; at 1920 it is one row.

## Minimum window (item 2)
`kMinWindowW` 1480 -> 1024, `kMinWindowH` 560 -> 640 (`host_logic.hpp`). Height measured with the drive runner at 1024 wide: the window layout's own minimum is 626 px (FOLD, the last rail key, ends at 598 at 560 and 600 high and sits against the status bar at 640 and 680); 640 is the smallest round height at which the folded strip, both tab rows, a spectrum with a plot and a waterfall, and the rail down to FOLD all fit. At 1024 x 640 the spectrum plot is about 90 px tall: alive, and honest about being small.

## Expectations changed (both values)
| Test / drive | Before | After |
|---|---|---|
| `test_slint_host_mustfixa::theSmallestWindowIsTheMeasuredOne` | `kMinWindowW = 1480.0f`, `kMinWindowH = 560.0f` | `1024.0f`, `640.0f`; plus the fold markers in `main.slint` and the seven drives |
| drive `window-min-strip` | `size=1480x560`; "Report a bug / dislike", "Bias tee off ...", "Full span", "Layouts" `right <= 1480`; "FOLD" `bottom <= 560` | `size=1024x640`; every strip control, the toolbar, "Layouts", "Report a bug / dislike" inside the window (left >= 0, right <= 1024, bottom <= 640), "FOLD" `bottom <= 612` |
| new drives `window-strip-1024`, `-1280`, `-1366`, `-1440`, `-1480`, `-1920` | (none) | the same list at 1024 x 768, 1280 x 800, 1366 x 768, 1440 x 900, 1480 x 800, 1920 x 1080 |

## OPEN ROWS FOR THE NEXT LANE
| Row | Why | Lane kind |
|---|---|---|
| Real pointer pass over the folded strip and toolbar at 1024, 1366 and 1280 | no real pointer here | mouse drive |
| Windows snap or maximise at a size below the new minimum (below 1024 x 640) | only the drag edges are held; the OS minimum is not set | window lane |
| Lane captions (GAIN, SQUELCH) are left out at 1024 by the lane's own rule (the AUTO key and the readout take the room); the tooltip and accessible name carry them | pre-existing lane rule, not changed | window lane |
| Row two at 1366 leaves its right half empty | the fold is by need, not balance | window lane |
