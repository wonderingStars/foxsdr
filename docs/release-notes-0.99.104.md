# FoxSDR 0.99.104 — Windows

Map pages open inside the main window, including a previous session's oversized
or off-main-window rectangle. Manual drag-out remains available. Closing and
reopening a map returns it to the safe default. Decoder and map data stay active.

This removes the automatic second OpenGL context from opening a map. Earlier
field reports record failure to allocate that context. The tester's later
visible freeze has not been reproduced here, so this mitigates the verified
context-failure path; its precise relationship to the later freeze is unverified.

Verification:
- Original map-opening regression: 21 checks, 5 failed on unchanged 0.99.103.
- Closing/reopening initially exposed a 50-pixel width/height shrink; the
  dimension regression failed both equality checks before its correction.
- Final build: successful; final map/window group 8/8 passed. The new runtime
  regression passed all 38 checks, including saved off-main geometry, a small
  main window, explicit native tear-off, recovery after a refused context,
  close/reopen containment, and preservation of the outer window dimensions.
- The broader pre-sizing-correction build passed all 60 GUI tests. Headless
  tests passed 347/348; test_rtlsdr_live failed on the local RTL dongle's exact
  1880-sample repeating pattern. An unchanged older executable reproduced
  the same 67-check/3-failure result. No radio code was changed.
- GUI tests ran on a separate hidden kernel desktop with isolated settings.
  Actual map captures were inspected; no test processes remained afterward.
- The tester's Intel freeze remains unverified on this NVIDIA RTX 4090 host.
Windows is released first. Linux packages remain at 0.99.103 until their builds
and verification finish; there are no Linux 0.99.104 downloads in this release.

**SHA-256 of `foxsdr-setup-0.99.104.exe`:**
`57a7ce3c8c4ce7dae18f66f1c5334ac2eefe58186cf80f18d7d42dc666986a41`
