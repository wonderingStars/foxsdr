# Window gaps, lane 3: the plugin store, the fitted-modules list and the plugin pages (2026-11-14)

Branch `feat/window-gaps-3`, cut from `07e717e6` in worktree `D:/foxsdr-wt/en-sigmeas` (clean; the old branch `feat/wire-b` was merged), build folder `D:/foxsdr-build/en-sigmeas` (Slint ON, Release, soft-GL archive from the local copy under `D:/foxsdr-build/mr-dsp/_softgl`, no download), scratch `D:/foxsdr-build/wg3-scratch/`. Nothing is pushed. The real `%APPDATA%/foxsdr/config.json` is not touched by any test of this lane.

Brief: `window-gaps-common.md` and `window-gaps-3.md` (`D:/foxsdr-build/prep/queued-briefs/wave7/`), plus the orchestrator's added item U6 (audit `not-built-audit.md`): the plugin store says what each capability lets a plugin do.

## Design (one renderer for two pages, as in the old window)

The old window draws the catalogue body of a plugin with ONE function (`gui/plugin_store_view.cpp drawModulePageBody`) for the store's page and for a fitted module's page. The new window now does the same: `ui/plugin_body.slint` (new file) holds `PluginBody` (SCREENSHOTS strip with a caption per frame, WHAT IT DOES, WHAT'S NEW, WHAT THIS MODULE REACHES, DETAILS as a grid) and the model structs `PluginBodyModel`, `BodyFrame`, `BodyFact`, `BodyReach`. `StorePage` (extend_bank.slint) and `ModulePage` (fitted.slint) both mount it; the store page puts the maker's notice (BEFORE YOU FIT IT) in the body's `@children` slot between WHAT'S NEW and DETAILS. The logic is `extend_logic.cpp fillBody` and `reachLines`; `extend_publish.cpp mapBody` turns it into the Slint model; a fitted module's body is built from the catalogue entry named by the module's install record (`catalogueKey`) and handed to the kit publisher (`setModuleBodies`).

## The rows

| Row | Verdict | Evidence |
|---|---|---|
| 406 / 1880 catalogue body (SCREENSHOTS, WHAT IT DOES, WHAT'S NEW, DETAILS) on the plugin pages | BUILT | store page and fitted module page both mount `PluginBody`; `test_slint_host_extend` windowGaps3 (module bodies built from the catalogue, `module_pages`), `wg3-store-page` / `wg3-store-plain` drive scripts for the store page. The fitted module page's route is proved by logic tests and a render of the specimen; a drive of a real fitted module is UNVERIFIED (see below). |
| 1845 framed strip, a caption under each frame | BUILT | `PictureStrip` in plugin_body.slint: 16:10 frames 211x132, caption under each, frame words ("loading...", failure reason) inside a frame that has no picture. `wg3-store-page`: `expect "Picture number 1 of the page" label contains ...`. |
| 1846 strip arrows | ALREADY BUILT (publisher) + REBUILT (strip) | the ledger said the arrows did nothing; the publisher already kept the index (`extend_publish.cpp on_store_picture`). The strip now moves one frame a press with an eased slide, the arrows are drawn only when the frames overflow, grey at the ends, the wheel over the strip moves it while it can. `wg3-store-page`: index 0 -> 1 -> 2, the right arrow greys at the last reachable frame, back to 1. |
| 1853 facts as a grid | BUILT | `FactLine` rows: name in a cap column, value beside it; a fact with no source is dim, slanted, on a darker ground with its reason (REPLACED the old window's ruled hatch with that look); a URL or digest is read-only selectable text. `PageFact.hatched/copyable` now reach the window. `wg3-store-page` reads the grid back (`store.facts`, `expect "MAKER: ..."`). |
| 631 legal notice shown before install | ALREADY BUILT in logic, re-proved | the notice is shown while the plugin is not installed (`notice_shown`), GET stays greyed until it is ticked. Proved on a local catalogue with a notice (`wg3-store-page`: notice text, GET greyed, tick, GET live, untick, greyed) and by the unit test for the Pager entry. The catalogue's `legalNotice` reaches the page through `fact.store.<id>.legal` (engine_api_lists.cpp:1065). |
| U6 capability explanations | BUILT | `reachLines` (extend_logic.cpp) gives, for every capability bit, the old window's key and sentence ("Fed complex baseband straight from the receiver.", "GRANTED...", "Sends the host audio to transmit. Nothing goes out unless you grant it the TRANSMIT permission AND arm it..."), drawn under WHAT'S NEW on the store page, so a user reads what a plugin may do before fitting it. Unit test checks the sentences and the outward flag; `wg3-store-page` reads `store.reach`. |
| 2 promise sentences | BUILT (removed) | see "Sentences changed" |
| 3 reading width and no empty frame | BUILT | the store page's plugin column is `min(inner width, 640 px)` and the fitted module page's column `min(width, 672 px)` (640 plus its 16 px padding); a page with no picture shows one sentence and no frame (`wg3-store-plain`). |
| 279 rail chip + lamp | ALREADY BUILT | the DECODE bank's "Plugin store" row: `kit_publish.cpp publishDecodeBank` (chip from `setStoreRail`, lamp green while the page is up, amber while a transfer or updates wait); the words BUSY / IDLE / "n UPD" / OK are `extend_logic.cpp readStoreChip`, tested by `railChip` in test_slint_host_extend. The audit's "always '--'" is `main.cpp:897`, the generic section fallback for bank 1, which the DECODE bank does not draw (`main.slint` shows `DecodeBank` there). `wg3-store-page`: `state store.chip == OK` after the catalogue was read. |
| 280 row tooltip | ALREADY BUILT | the row's tooltip is the old window's sentence (kit_publish.cpp, the `add(0, "store", ...)` call). |
| 391 / 1868 CLEAN UP OLD VERSIONS and its question | BUILT | the Fitted modules page's foot now has its label "CLEAN UP OLD VERSIONS (n)", the first three old copies beside it, and what the last clean-up did; the key opens a question (a panel in the lower right: every old copy listed, "Remove n files" / "Keep them"); only the first sends `FOXAPP_OP_STORE_CLEAN_OLD`; the second sends nothing. The store's UPDATES tab foot (already built) shares the same question state. The old copies are read with the store page closed (`readStore` with `read_store=false` now reads them). Proof: `windowGaps3` (label, lines, question, YES sends exactly one command, NO none, a transfer greys it, nothing old closes it) and `test_slint_fitted` (the specimen's question: its panel, KEEP THEM, REMOVE n FILES). The command is only a unit-test result and a specimen click; no scratch plugins folder was needed and none is touched. |
| 1804 / 1805 store too narrow / too small | BUILT | `StorePage`: width under 320 px or height under 200 px shows the old words "This window is too narrow to lay out the catalogue. Widen it and the plugins come back."; under 40 px, "Too small - drag the window larger." (the old window's 900 px minimum was for its card grid; this page lays out down to 320). |
| 1855 fitted too narrow | BUILT | `FittedModules`: under 240 x 160 px, "Too narrow to draw. Widen the window."; under 40 px, "Too small - drag the window larger." |
| 354 fax chart panel | BUILT | a fax page with a chart shows the picture on the left (PicturePane: the plugin's own picture, status line, press = `FOXAPP_OP_IMAGE_PRESS` through the picture page's own callbacks) and the memory table beside it. UNVERIFIED end to end (needs a fax plugin run; the fixtures fit plugins from the user's folder). |
| 355 NEW hold | BUILT | `kit_logic.hpp NewHold`: lit for 4 s from the first sight of an event, out after, lit again by a new one; `test_slint_host_kit` checks 3999 ms lit, 4000 ms out, a new event lights again, no reading never lights. |
| 426 teleprinter paper strip | BUILT | `kit_publish.cpp fillInstrument` feeds `paper-newest` / `paper-previous` from the last two memory rows' last named column (`paperColumn`, tested) and `InstrumentFace` already drew them; before, nothing fed them. |
| Companion applications shelf | NOT BUILT, as briefed | would mount in `PluginBody`'s `@children` slot on the store page (between WHAT IT MAY DO and DETAILS) or under the Browse grid's foot in `StorePage`. |

## Sentences changed (row 2)

* `kit_publish.cpp`: "Catalogue entry: %s (the store's pictures and description come with a later beta)" removed; the module page now shows the real catalogue body (or nothing for a module the catalogue does not list). The key is gone from all 33 catalogues (`tools/i18n_keys.py --merge`).
* `fitted.slint` header comment and `fitted_specimen.slint` / `extend_specimen.slint` sample words that promised the store: replaced. Searched `later beta`, `a later`, `not yet`, `coming soon`, `arrives with`, `will come` across `src/slint` outside tests: the others found are code comments or belong to other lanes' files (`host_duties.cpp` updates text "a later phase", lane 1/2 territory: NOT edited, named here).

## SHARED FILE EDITS

`ui/main.slint` (import and export of the five new names, 3 lines), `ui/pages.slint` (FittedModel and ModuleModel fields, PageTab `paper-newest/previous`, the fax layout, the callbacks of the module body and the clean-up question), `ui/components.slint` untouched, `main.cpp` (one `store.<what>` state read for the drive scripts), `tests/test_slint_drive_run.cpp` (a header value `fixtures/...` names a file under tests/drive/fixtures), `resources/lang/*.json` (one stale key dropped).

## New engine commands
None. (`FOXAPP_OP_STORE_CLEAN_OLD` and `FOXAPP_OP_STORE_PICTURES_WANT` already existed.)

## New strings
None: every sentence reuses a key the old window already had (reach sentences, "Too small...", "This window is too narrow...", "Too narrow to draw...", "Remove old plugin versions", "No pictures published yet"); `tools/i18n_keys.py`: 3828 keys, 0 missing. One key was dropped (the removed promise).

## HAND-OFF
* A fitted module whose install record names no catalogue key, or whose entry the catalogue does not list, gets no body (the page keeps its own FILE / PLUGIN ABI lines); the old window also drew a DETAILS grid built from the host record for such a module. Not built: it would repeat the lines the page already has.
* Companion applications shelf: see above.

## UNVERIFIED
(to be completed with the gate: see the end of this file)
