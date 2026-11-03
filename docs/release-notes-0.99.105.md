# FoxSDR 0.99.105 — Windows (Slint beta)

This is the new FoxSDR window, built with Slint. It has the Workbench look of
the redesign and every control of the main screen. All 43 catalogue plugins run
through the plugin kit: decoder output, pictures, panels, instruments, maps,
settings forms and presets. The 33 languages are bundled into the window.

The previous window stays installed as "FoxSDR (classic window)" in the Start
menu. Use it for the plugin store, bookmarks, scanner, airband and recorder
pages until those are ported.

This is a beta and is held back from the stable channel. Copies set to "Stable
releases" stay on 0.99.104. Copies set to "Every release (beta)" get 0.99.105.

Known issues:
- Chinese, Japanese and Korean glyphs are missing in the new window. The
  embedded fonts have no CJK coverage. The classic window draws them.
- In the longer languages, a row that is longer than the window is clipped at
  the right.
- The window is about 7 MB larger than the classic one, because of the
  catalogues it carries.
- Words the engine sends stay English.
- There is no Linux build of this beta. Linux stays at 0.99.103.
- It is not in the Microsoft Store.

Verification:
- The whole tree was built in the release configuration.
- Headless tests: 662 of 662 passed.
- GUI tests, on the hidden desktop: 68 of 70 passed. One was a load flake that
  passed when run alone. The other fails on the base tree too, on this machine.
- The Slint tests: 30 of 30 passed.
- The catalogue test: 225,590 checks, none failed.
- The installer was built and installed per user into a scratch directory. The
  shortcuts, the uninstall entry and the foxsdr: handler were inventoried and
  all point at the new window. The installed window was run in French from the
  bundled translations. It was then uninstalled with nothing left behind.
- The owner's 0.99.104 was reinstalled afterwards. His settings file was
  byte-identical throughout.

---

**SHA-256 of `foxsdr-setup-0.99.105.exe`:**
`TBD`
