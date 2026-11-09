# FoxSDR 0.99.108 — Windows (Slint beta 4)

This is the fourth beta of the new FoxSDR window, built with Slint. It fixes a
tester's report: tuning from 7 MHz to 3.5 MHz left the picture and the mode
behind. Now a band switch moves the picture and the mode. The waterfall follows
a retune that lands outside it (the same rule in both windows), and the engine
applies the band plan's mode on every switch path: a typed frequency, a click
to tune, a bookmark and a remote command. It is a new Radio setting, "Follow the
band plan's mode when retuning", on by default. A step, a drag, the scanner or
a plugin preset never changes the mode, and a mode you set by hand stays until
the next band change.

The default World band plan now has a 40 m row (7000 to 7200 kHz) and an 80 m
row (3500 to 3800 kHz), the segments every ITU Region allocates to amateurs, so
the mode can follow on the default plan.

The VOR plugin's bearing was 45 degrees out. The cause was the AM demodulator's
DC blocker, whose corner is now 2 Hz instead of 30 Hz.

Five gaps in the new window are closed: SYSTEM > Display has a Trail style box;
the Bookmarks panel's two spectrum options are saved and applied (the names
stack on the spectrum); a bookmark saved with an empty name is named after its
frequency; the Tune step has one label; and the patch band's decoder rows are
capped at two, with a fold key for the rest. A settings save now keeps the file
it replaces as a dated copy, the newest five.

Behind the scenes, this beta was tested with 118 scripted drives of the new
window, 24 synthetic I/Q recordings decoded in the live window, and five
minutes of real 1090 MHz ADS-B on a B200 compared frame by frame with the
classic window.

This is still a beta and is held back from the stable channel. Copies set to
"Stable releases" stay on 0.99.104. Copies set to "Every release (beta)" get
0.99.108.

Known issues:
- Chinese, Japanese and Korean glyphs are missing in the new window; the
  classic window draws them.
- In the longer languages, a row that is longer than the window is clipped at
  the right.
- A key you rebind is saved, but the new chord is not yet answered.
- The interface size and the theme are saved but not applied by the new window.
- Each bank has small gaps: the bookmarks' Rename key, an airband import with an
  empty box, some dead keys that are not greyed, and Esc not leaving a plugin's
  page.
- The scope modes and the Transmit page still need the classic window, which
  stays installed as "FoxSDR (classic window)".
- There is no Linux build of this beta. Linux stays at 0.99.103. Nine Linux CI
  tests fail on this branch and are not fixed yet.
- It is not in the Microsoft Store.

Verification:
- The gate on this tree: the headless label 665 of 665, the Slint label 42 of
  42, and the GUI label 192 of 192 on the hidden desktop (docs 6.54 and 6.55).
- The airband drive passed 10 of 10 repeats. The band-plan test failed first on
  the missing rows and passes now.
- Not run again for this beta: the installer proof (the installer script is
  unchanged since 0.99.105's proof).

---

**SHA-256 of `foxsdr-setup-0.99.108.exe`:**
`TBD`
