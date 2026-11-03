# FoxSDR 0.99.106 — Windows (Slint beta 2)

This is the second beta of the new FoxSDR window, built with Slint. The four
rail banks now open in the new window. SIGNAL covers choosing and setting up a
radio with every kind of device, the Radio-setup page, the audio filters, the
sinks, the recorder and the frequency correction. TOOLS covers the bookmarks,
the scanner, airband, the markers window and the receiver position. EXTEND
covers web access, CAT control and the plugin store page. SYSTEM covers every
settings section: display, language and country, updates, serial ports and GPS,
key bindings, diagnostics, usage reporting and beta tester, with the
feature-request and bug-report pages.

The previous window stays installed as "FoxSDR (classic window)" in the Start
menu, but it is now needed only for the patch page, the scope modes and the
Transmit page. This beta also fixes two faults of 0.99.105: the Radio section's
rows were drawn under the second row of every rail bank, and the toolbar's
Markers count read 4096 because the list reader did not stop at the count.

This is still a beta and is held back from the stable channel. Copies set to
"Stable releases" stay on 0.99.104. Copies set to "Every release (beta)" get
0.99.106.

Known issues:
- Chinese, Japanese and Korean glyphs are missing in the new window. The
  embedded fonts have no CJK coverage. The classic window draws them.
- In the longer languages, a row that is longer than the window is clipped at
  the right.
- The window is about 7 MB larger than the classic one, because of the
  catalogues it carries.
- Words the engine sends stay English.
- A key you rebind is saved, but the new chord is not yet answered.
- The interface size and the theme are saved but not applied by the new window.
- Each bank has small gaps: the bookmarks' Rename key, an airband import with an
  empty box, some dead keys that are not greyed, and Esc not leaving a plugin's
  page.
- There is no Linux build of this beta. Linux stays at 0.99.103. Nine Linux CI
  tests fail on this branch and are not fixed yet.
- It is not in the Microsoft Store.

Verification:
- The Slint label: 36 of 36 tests passed.
- Each bank has its own tests, each shown failing first and with mutants caught:
  SIGNAL 315 and 337 checks, SYSTEM 323 and 767, TOOLS 371 and 520, EXTEND 265
  and 390. None failed.
- The catalogue test: 230,639 checks over 2,324 keys in 33 languages, none
  failed.
- Every bank was captured live in the real window on a scratch profile.
- The tick list accounts for every control of every inventory: 1,451 present,
  198 relocated and 314 deferred with reasons, of 1,963.
- The headless and GUI labels and the installer are run by the release gate
  (6.43). The installer proof of 6.38.5 stands for the unchanged installer
  script.

---

**SHA-256 of `foxsdr-setup-0.99.106.exe`:**
`TBD`
