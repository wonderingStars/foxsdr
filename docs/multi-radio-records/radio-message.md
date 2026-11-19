# A message for one radio (0.99.110)

Lane `radio-message`, branch `feat/radio-message`, cut from the release commit `03ac57b7` on 2026-11-19. LOCAL ONLY: nothing pushed, deployed or published. The version number is not bumped (the release lane does that). The site half is in the private site repository, branch `feat/radio-message`, `radiomessage.go` and PART 14 of `HANDOVER-daylight.md`.

The owner's request, 2026-11-19: "in the next build i want to be able to send messages to a specfici radio it should show in a popup window on the app".

## The design as built

The application never sends its radios anywhere. The update check it already makes (`GET /api/update`, once per launch) is answered with a NEW list, `radioMessages`, beside the existing `messages`:

```
{"id":"rm3e5f623e20","date":"2026-11-19","text":"...","salt":"<32 lower-case hex>","hash":"<64 lower-case hex>"}
hash = lower-case hex SHA-256 over the ASCII salt followed by the UTF-8 radio key
radio key = radioreport::radioKey(): "<model slug>:<SERIAL upper-cased>", e.g. "nesdr-smart-v5:00A1B2C3"
```

The application hashes the radio keys it has itself found or opened on this PC and shows an entry only when one matches. A copy without that radio learns that some message exists and nothing else. The hash is a lock, not a secret (a key is a model and a serial, so someone holding a guess can test it); it stops the answer from naming radios.

Where each piece lives:

| Piece | File |
|---|---|
| the keyed hash, the sanitiser, the known-radio registry, the popup rule | `src/core/radio_message.{hpp,cpp}` (`radioMessageHash`, `sanitiseRadioMessageText`, `radiomessage::noteRadio`, `radioMessagePopupFor`) |
| the reader of `radioMessages` (bounded, entry by entry, sanitised on receipt) | `src/core/updater.cpp` `parseUpdateManifest`; the field `UpdateInfo::radioMessages` |
| the radios found or opened | `src/core/radio_reporter.cpp` `Reporter::event`: `radio_found` and `radio_open` call `radiomessage::noteRadio`, before the reporter's own gate. Nothing leaves the PC. A radio with no serial is not recorded (it cannot be addressed). |
| the popup's model, Seen, the later-in-session path | `src/slint/host_duties.{hpp,cpp}`: `radioMessageFor`, `HostDuties::takeRadioMessageChange / radioMessage / radioMessageSeen`; `poll()` re-works the popup when `radiomessage::generation()` changed |
| the popup itself and the queue | `src/slint/ui/main.slint`: `radio-message-*` properties, `modal-busy`, `radio-message-visible`, the last `Sheet` before the drop area; `src/slint/main.cpp` `applyRadioMessage` |
| Seen | the SAME list as the team messages, `AppConfig::messagesSeen` (settings file, saved at once), so no new settings key; after a successful check the list keeps the ids either list still names (`seenAfterCheck`, three-argument form) |
| strings | `resources/lang/*.json` (33), `src/core/lang_assets.hpp` regenerated |

## What the user sees

A modal sheet over the window, last in the stack:

- Title: **A message from the FoxSDR team**
- Line 1 (bold): **For your radio: NESDR Smart v5, serial ending B2C3** (the model and the LAST FOUR characters of the serial; never more)
- Line 2: the date as published (`2026-11-19`)
- The text, word-wrapped, plain
- One key: **Seen**

Only Seen closes it (a click on the backdrop does nothing). It is shown when the radio is found at start-up, or later in the session when the radio is plugged in (the registry's generation changes and the next `poll()` works it out again). It is shown once per message id: Seen is saved at once into `messagesSeen`.

THE QUEUE. `modal-busy` in `main.slint` is the OR of every other sheet and dialog the window can show (the mute question, the Legacy hand-over question, the unclean-exit offer, the link offer, Layouts, Add-a-radio, the group-removal question, Radio setup, the recorder's import dialog, the SDRplay dialog, the report sheet, the markers sheet, the bias-tee question, About, the shortcuts, the Recordings sheet, the lane K sheet, the Radio matrix, the help). The popup is `radio-message-shown && !modal-busy`: it waits and appears when the last one closes. Proved in `test_slint_main_screen`.

PLAIN TEXT ONLY. The text is a Slint `Text` element's string: nothing is interpreted and no link is clickable. On receipt (`sanitiseRadioMessageText`): invalid UTF-8, C0 and C1 controls, DEL, the zero-width and direction controls, the line and paragraph separators, the byte-order mark and the specials block are dropped; a tab becomes a space; a run of blank lines becomes one; the ends are trimmed; at most 600 characters, cut on a character boundary. Markup (`<b>`, `<script>`, `&amp;`, `**x**`) is shown literally.

NOT SHOWN when the Updates tick is off (no check was made, and `updateSettingChanged` drops what arrived earlier) or for a Microsoft Store copy (it never calls the endpoint). No acknowledgement is sent: the application sends nothing new.

## The Legacy window

Not built. The existing team-message card there (`drawTeamMessages`) could carry a message cheaply only by turning a matched radio message into a synthetic `TeamMessage` when the check lands, but it would not name the radio, would not follow a radio plugged in later, and the Legacy window prunes `messagesSeen` to its own list, forgetting these ids. New window only.

One consequence worth knowing: a person who uses the Legacy window after pressing Seen in the new one has the radio message's id forgotten by the Legacy prune (it keeps only ids of `update_.messages`), so the popup is shown once more the next time the new window starts. Harmless; not fixed.

## What old versions do (0.99.109 and earlier)

They never read the key `radioMessages`: the reader takes the keys it knows by name and ignores the rest. Evidence:

1. `git show 03ac57b7:src/core/updater.cpp | grep -c radioMessages` is 0 (the release's reader and header).
2. The site does not send the list to a caller older than 0.99.110 (`radioMessagesReader`; `TestTheUpdateAnswerCarriesTheListOnlyForACopyThatReadsItAndNeverTheRadioKey`).
3. The 0.99.109 reader itself (byte for byte, `git show 03ac57b7:src/core/updater.{hpp,cpp}`) was compiled and run on a real answer of the new site code that CARRIES the list: see "Old-reader probe" below.
4. The Go test `TestAnOldCopyThatReadsTheAnswerSeesTheSameWithAndWithoutTheNewListAndNeverItsText` ports the same reader and compares.

## Tests

- `tests/test_radio_message.cpp` (new; headless): the contract fixture, hash match and non-match, text sanitising (control characters, over-long, invalid UTF-8, markup literal), the reader's bounds and per-entry drops, an old reader's fields unchanged, the registry fed by the reporter's real funnel, the popup rule (once per id, oldest first, a radio found later, model and last four only, the Updates tick off, a Store copy).
- `src/slint/tests/test_slint_duties.cpp`: the popup through the duties (answer first, radio plugged in later, Seen saved at once, a start that has seen it, switched off); `seenAfterCheck`.
- `src/slint/tests/test_slint_main_screen.cpp`: the popup is drawn, queued behind each sheet in turn (About, the mute question, the bias-tee question) and comes when the last closes; Seen reaches the host; the window is as before when it is gone.
- `tests/fixtures/radio-message-contract.json`: the SAME FILE as the site's `testdata/radio-message-contract.json` (sha-256 `d8859c190bf863b1247dda8525a3760767b85e4bcfb479f256ae6b0fd27740ba`). The vectors were made by Python's `hashlib`, a third implementation; the C++ and the Go code both recompute them.

Results are in the lane report (the commit message carries them).

## Old-reader probe

See the commit message for the run (the probe sources are not kept in the tree: `D:/foxsdr-build/radio-message/oldprobe`).

## Candidate privacy sentence (for the owner; the privacy page is NOT edited)

"When FoxSDR checks for updates it asks foxsdr.com one question, which version it is, and sends nothing about your radios. The answer can include a short message from the FoxSDR team meant for one particular radio. The message is not addressed by the radio's serial number: it is locked with a one-way scrambled code that your copy of FoxSDR tries against the radios it has itself found on your computer, and shows the message only if one matches. The site is never told which radios you have, and never told whether you saw the message. If you switch the update check off, no message arrives."

## Not built, not proved

- Nothing was seen end to end against a live site. The site's answer was produced by its own handlers in tests and run through the real 0.99.109 reader and the new reader; no real FoxSDR window fetched it.
- The popup was not seen by a person. The Slint sheet was rendered in the headless render harness and its visibility and queueing asserted; a screenshot is written by `test_slint_main_screen` (`main-screen-radio-message.png`) but was not looked at by a human here.
- The popup's keyboard behaviour (Enter or Escape for Seen) is the sheet's: neither is wired.
- The translations of the two new strings (`A message from the FoxSDR team`, `For your radio: %s, serial ending %s`) are machine-made in all 33 catalogues (the catalogues are flagged `machine`), unchecked by a speaker.
- The Legacy window shows nothing (above).
- The hash is a lock, not a secret (above).
- Radios with a stock or empty serial (`00000001`, all zeros, none) cannot be addressed one by one; the site refuses them.

## Results (final, 2026-11-19)

All built with MSBUILDDISABLENODEREUSE=1, Release, in D:/foxsdr-build/radio-message; foxsdr-slint.exe links (exit 0). Run: test_radio_message 67/0 failed (break-it: hash match forced true gives 10 failed); test_slint_duties 447/0; test_slint_main_screen 84/0; test_slint_house_rules 376/0 (0 untranslated literals); test_i18n 486259/0; test_updater 384/0; test_wire_c_updater 30/0; test_config 3945/0; test_radio_report 215/0; test_update_old_prompt 72/0. Site: go vet clean; go test -timeout 40m -count=1 ./... ok (1147 s). The whole ctest suite was not run, nor any gui-labelled test on the hidden desktop (the main-screen test renders headless in the harness).
