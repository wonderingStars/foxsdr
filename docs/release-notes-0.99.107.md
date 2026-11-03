# FoxSDR 0.99.107 — Windows (Slint beta 3)

This is the third beta of the new FoxSDR window, built with Slint, and it fixes
the two things testers hit first in 0.99.106. The map now redraws on its own: in
0.99.106 the map pane never asked for a fresh frame until the window was
resized, so aircraft appeared late or not at all while the decoder was in fact
tracking them. And the Display section's "Aircraft icons" and "Trail width"
sliders now take effect on the map, bounded as the classic window bounds them
(icons 16 to 96 px, a trail no wider than the icon's wingspan); the saved trail
style is applied too.

The patch page is in the new window as well: the band with START, STOP, ALL
OFF and the part keys, the canvas with its nodes, wires and faces, the inspector
and the PATCH PRESETS page. The classic window, still installed as "FoxSDR
(classic window)", is now needed only for the scope modes and the Transmit page.

This is still a beta and is held back from the stable channel. Copies set to
"Stable releases" stay on 0.99.104. Copies set to "Every release (beta)" get
0.99.107.

Known issues:
- The patch band is tall in the new window (the 43 decoder keys wrap into many
  rows), which leaves the canvas short; a cap or a fold is coming.
- No trail-style control is drawn yet; the saved style applies.
- Chinese, Japanese and Korean glyphs are missing in the new window; the
  classic window draws them.
- In the longer languages, a row that is longer than the window is clipped at
  the right.
- A key you rebind is saved, but the new chord is not yet answered.
- The interface size and the theme are saved but not applied by the new window.
- Each bank has small gaps: the bookmarks' Rename key, an airband import with an
  empty box, some dead keys that are not greyed, and Esc not leaving a plugin's
  page.
- There is no Linux build of this beta. Linux stays at 0.99.103. Nine Linux CI
  tests fail on this branch and are not fixed yet.
- It is not in the Microsoft Store.

Verification:
- The map fix: the painter tests failed first (every trail read 2 px, the glyph
  14 px at every size) and pass now, 754 and 230 checks; live captures at 96 px
  and 86 px against the defaults.
- The patch page: family tests 481 checks and host tests 1,281 checks, failing
  first and green five times; five bounded live runs placed and wired parts, saved
  a preset and restored it in the next run.
- The Slint label: 40 of 40 tests passed; the catalogue test: 230,936 checks.
- Not run again for this beta: the headless and GUI labels (the 0.99.106 gate of
  docs 6.43 stands; nothing under the engine changed since) and the installer
  proof (the installer script is unchanged since 0.99.105's proof). The first
  live pointer drive of the new window is recorded in docs 6.46.

---

**SHA-256 of `foxsdr-setup-0.99.107.exe`:**
`TBD`
