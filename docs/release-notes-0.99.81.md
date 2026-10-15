# FoxSDR 0.99.81

Release date: 2026-10-15.

**SHA-256 of `foxsdr-setup-0.99.81.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.81-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.81-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.81-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.81-linux-arm64.tar.gz`:**
`TBD`

## Fixes

**IBOC dashboard programme rows respond to clicks.** With IBOC Radio 0.1.2, clicking the HD label, selection-arrow area or programme name selects and saves that programme. Earlier host versions showed an image without delivering mouse input, so fixing the separate settings-panel table did not repair these dashboard clicks. The host now supplies an optional image-input capability, using native image coordinates even when the dashboard is scaled. Old image plugins retain their existing ABI and display behavior. A press must begin and end inside the image; right clicks, outside clicks and drags into or out of the image do not select a programme. Both FoxSDR 0.99.81 and IBOC Radio 0.1.2 are required.

**DAB decoder muting preserves decoded sound.** The default data-decoder preset previously muted the finished audio after decoded DAB PCM had replaced the receiver audio. Automatic decoder muting now suppresses the demodulated carrier before decoded programme audio takes over. The user's Mute control and transmit mute still silence the finished output. Saved automatic-mute settings and overrides retain their meaning, and diagnostics stop describing a decoded programme as muted once it supplies audio.

## Verification before release

The reported IBOC dashboard interaction was reproduced on FoxSDR 0.99.80 with IBOC 0.1.1, then exercised in 16 bounded real application runs using isolated profiles. HD2 selection worked from all three reported positions, HD1 could be selected again, and invalid gestures did not change selection. The real configuration fingerprint was preserved. The IBOC suite passed 27/27; removing callback delivery made both regressions fail.

The audio regression measured a 1 kHz amplitude of 0.0000 before the fix and 0.4995 afterwards. The actual AppWindow default preset policy with a loaded test module measured 0.5008; restoring the old hard-mute combination returned 0.0000 and failed the regression. User mute and automatic carrier suppression without programme PCM remained digital zero. The affected host suites passed 34/34. Detailed logs are under ignored `build/iboc-click-e2e/`, `build/iboc-click-*-test.log`, `build/dab-mute-*-test.log` and `build/plugin-package-test.log`.

Live broadcast reception and Linux desktop interaction remain unverified. Release packaging, the full suite and CI results are recorded after execution.
