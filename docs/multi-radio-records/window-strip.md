# Window strip lane: the receiver strip folds instead of being cut off (2026-11-17)

Branch `feat/window-strip`, cut from `253a26a5` (feat/window-followups), worktree `D:/foxsdr-wt/en-screen-k`, build `D:/foxsdr-build/en-screen-k` (Release), hidden desktop `FoxSDRStrip`, scratch `D:/foxsdr-build/ws-scratch/`. Nothing pushed.
Real `%APPDATA%/foxsdr/config.json` sha256 before: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.

Finished at the last commit.

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

## Later fixes in this lane (second commit)
- The antenna-power block is 120 px wide; the fold maths used its hint-inflated preferred width and put the key 40 px short of the right edge at 1920 (found by `test_slint_main_screen`). Now 120.
- The separator between the toolbar rows is the full line colour (kept `tab row bottom hairline` at y 171 true).
- Item 4b DONE: the lockout line ("2 locked out; the first ends in 600 s") is its own wrapped line under the Clear key (`tools_bank.slint`), read whole (picture seen). Item 4a: ALREADY BUILT - in German the SYSTEM chips read SCHLICHT, DE, AUS, KEINE, STANDARD, AN (picture `de-system2.png`, 1366 x 1200, seen); `system_logic.cpp` passes every chip word through `tr()`, all 33 catalogues hold OFF, ON, STOCK, NONE. Nothing changed.

## RED before, GREEN after
Tree before the change (`main.slint` at `253a26a5`, rebuilt, exe seen): `window-strip-1366` RED ("Full span" - no control labelled), `window-strip-1280` RED (antenna key right edge 1361 > 1280; "Full span" missing), `window-min-strip` RED (VOLUME, BIAS TEE, Manual, Markers, Band plan, Full span missing at 1024). Log `D:/foxsdr-build/ws-scratch/red1.log`. After: all seven drives pass.

## Expectations changed in `test_slint_main_screen` (1440 x 900 sheet; the toolbar now folds there)
| Check | Before | After |
|---|---|---|
| splitter row | 140 + 32 + 424 = 596 | 140 + 64 + 404 = 608 |
| ribbon along the well top | y 172..180 | y 204..212 |
| narrow 1100 page keys ground box | y 176..212 | y 288..326 (the page bar sits lower; picture seen, keys inside the frame, 3867 px) |

## Pictures (`D:/foxsdr-build/ws-scratch/pics/window-strip-<w>.png`, all opened)
1024 x 768: strip two rows (185 px), toolbar: Band plan and zoom beside the tabs, Trace/Range/Markers on row two; GAIN and SQUELCH captions left out (only N/A AUTO and -120 dB AUTO with their hints); ugly: axis "1001MHz" overlap at the spectrum right end (older). 1280 x 800 and 1366 x 768: strip two rows, row two right half empty; toolbar row two holds Trace only. 1440 x 900: strip ONE row (mode as picker), toolbar two rows. 1920 x 1080: one row everywhere, mode keys shown. Pictures taken before the 120 px antenna fix; the fix only moves the key at 1920.

## TESTS RUN / NOT RUN
Run (Release, hidden desktop FoxSDRStrip): unit slice `^test_slint_*` plus test_i18n, test_error_paths, test_typed_number, test_patch_plan, test_add_all_plan, test_sys_ed25519, test_usage_diag_window, test_window_handover, test_errpath_sweep_a: 128 of 129 green before the two last fixes, the one red `test_slint_main_screen` now green (70 checks, 0 failed). Final commit: test_slint_main_screen, test_slint_host_mustfixa, test_slint_lanec_host, drives window-min-strip, window-strip-1024/1280/1366/1440/1480/1920 and e1-lockout-range all pass (run after the last .slint edit; main_screen after its last test edit). NOT RUN: full headless and GUI gates, the other layout drives of the earlier slice (lanel, lanek, system, tools...), the clip sweep, a real pointer pass. Error-path pin untouched (410). Config hash c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 before and after.

## OPEN ROWS (added)
| Row | Why | Lane kind |
|---|---|---|
| Title bar: "Fox & Schirmyver - Type 71 Mk II" runs under the search box at 1024 | seen by the orchestrator, not fixed | window lane |
| GAIN / SQUELCH captions gone at 1024 (see above) | the lane rule; fix by giving the lanes more room or a shorter readout | window lane |
| English words left in a German window: "no gain stage", "Manual threshold", "SPAN", "FLOOR / CEILING", "lines/s, history" | seen in `de-system2.png` | window lane |
