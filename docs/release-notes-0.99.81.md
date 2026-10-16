# FoxSDR 0.99.81

Release date: 2026-10-16.

**IBOC dashboard programme rows respond to clicks.** With IBOC Radio 0.1.2, clicking the HD label, selection-arrow area or programme name selects and saves that programme. Both FoxSDR 0.99.81 and IBOC Radio 0.1.2 are required. The entire programme row works when the dashboard is scaled, and invalid clicks and drags do not select a programme.

**DAB decoder muting preserves decoded sound.** Automatic decoder muting now suppresses the receiver's demodulated carrier before decoded programme audio takes over. The user's Mute control and transmit mute still silence the finished output. Saved automatic-mute settings and overrides retain their meaning.

---

**SHA-256 of `foxsdr-setup-0.99.81.exe`:**
`58510f7320320e5b7c314c88e5ee0b2aab3951e8348de5496ae6903759586c5c`

**SHA-256 of `FoxSDR-0.99.81-x86_64.AppImage`:**
`ad15fbe9aa88d23de6bc4876a564ed9024f09a01cdb4a8474d18e789ca774ac1`

**SHA-256 of `foxsdr-0.99.81-linux-x64.tar.gz`:**
`d882909b30f0708d6a866f87c80d178ec2cd766806f0fe1038d5fb0520464608`

**SHA-256 of `FoxSDR-0.99.81-aarch64.AppImage`:**
`153fd0b15dc819752358daf430adecd2a80d8048dc947330ea5fd3fa318ac408`

**SHA-256 of `foxsdr-0.99.81-linux-arm64.tar.gz`:**
`f4c5829131ec6ab57b6e5ed7afa962693308865d82651c9f3b75f397e614d3b8`

## Fixes

**IBOC dashboard programme rows respond to clicks.** With IBOC Radio 0.1.2, clicking the HD label, selection-arrow area or programme name selects and saves that programme. Earlier host versions showed an image without delivering mouse input, so fixing the separate settings-panel table did not repair these dashboard clicks. The host now supplies an optional image-input capability, using native image coordinates even when the dashboard is scaled. Old image plugins retain their existing ABI and display behavior. A press must begin and end inside the image; right clicks, outside clicks and drags into or out of the image do not select a programme. Both FoxSDR 0.99.81 and IBOC Radio 0.1.2 are required.

**DAB decoder muting preserves decoded sound.** The default data-decoder preset previously muted the finished audio after decoded DAB PCM had replaced the receiver audio. Automatic decoder muting now suppresses the demodulated carrier before decoded programme audio takes over. The user's Mute control and transmit mute still silence the finished output. Saved automatic-mute settings and overrides retain their meaning, and diagnostics stop describing a decoded programme as muted once it supplies audio.

## Verification before release

The reported IBOC dashboard interaction was reproduced on FoxSDR 0.99.80 with IBOC 0.1.1, then exercised in 16 bounded real application runs using isolated profiles. HD2 selection worked from all three reported positions, HD1 could be selected again, and invalid gestures did not change selection. The real configuration fingerprint was preserved. The IBOC suite passed 27/27; removing callback delivery made both regressions fail.

The audio regression measured a 1 kHz amplitude of 0.0000 before the fix and 0.4995 afterwards. The actual AppWindow default preset policy with a loaded test module measured 0.5008; restoring the old hard-mute combination returned 0.0000 and failed the regression. User mute and automatic carrier suppression without programme PCM remained digital zero. The affected host suites passed 34/34. Detailed logs are under ignored `build/iboc-click-e2e/`, `build/iboc-click-*-test.log`, `build/dab-mute-*-test.log` and `build/plugin-package-test.log`.

Live broadcast reception and Linux desktop interaction remain unverified. The completed packaging, full-suite and CI checks are recorded below.

The first complete Windows build and both Linux CI builds caught an older transmit-audio ABI assertion that still pinned the pre-image-input known-capability mask (`0x00007FFF`). The assertion now pins the current documented mask (`0x0000FFFF`) and the image-input bit (`0x00008000`) explicitly; the transmit-audio bit and all transmit safety checks remain unchanged. This corrects the stale test expectation rather than changing runtime behavior.

## Release verification, 2026-10-16

Published [v0.99.81](https://github.com/wonderingStars/foxsdr/releases/tag/v0.99.81) from source commit `7fea69c16003e0dcbe7a7ac4906f8a56deed1e0a`. All three jobs in [CI run 38829886391](https://github.com/wonderingStars/foxsdr/actions/runs/38829886391) passed: Linux x64, Linux ARM64 and Windows. The final complete Windows run reported **100% tests passed, 0 tests failed out of 376**, in 2,450.79 seconds. An earlier full run was deliberately interrupted for the native click investigation and was not counted as passing. The affected suites' earlier 34/34 result was additional coverage, not a substitute for this complete run.

Both clean Windows release build passes exited 0. The second pass relinked the executable; that final frozen payload has SHA-256 `5149e9be42f07fb5eee1086ff6d5a533e8d0660e6492c31b1c2c5b317b2416e6`. Its installer and MSIX passed their payload, identity and version checks. The complete all-target build also exited 0 and subsequently relinked the local development executable from the same clean source commit. The installer test independently verified the frozen executable's exact hash, `--version`, `--selftest` and a 60-frame run, all with exit 0. It then verified removal of installed files, registrations and the protocol handler while retaining user-installed plugins. Only retained plugins and a stale 0.99.80 registration existed before this test; the stale registration was preserved as a local verification snapshot. The current user configuration's hash, size and modification time remained unchanged.

Native Windows mouse input also selected HD2 in the actual IBOC dashboard using a real KUT recording, libnrsc5 and the published IBOC 0.1.2 DLL. The root verification checked point ownership with `WindowFromPoint`, used the main viewport with `FOXSDR_INPUT_SCRIPT` absent, and confirmed saved `program="1"`. The process exited 0 after 50,235 frames with the owner profile unchanged. The tester independently confirmed switching working twice. The scripted-input hook discards native input, so scripted and native interaction checks were treated separately. A native click in a detached OS viewport was not verified. The native result is retained locally under `build/iboc-native-real-decoder/native81-public12/native-result.json`.

All five GitHub asset names, sizes and SHA-256 digests matched the frozen local files before publication as latest. The annotated release tag and master branch were pushed to GitHub and the NAS using explicit refs. Windows and both Linux symbol archives were hash-verified on the NAS, with three index rows appended; all three symbol maps were hash-verified on the site host. The ARM64 symbols download was resumed after interruption, with the already successful CI jobs and downloaded payloads reused. The telemetry Worker was unchanged since v0.99.80 and did not require deployment.

Microsoft Store submission 22 contains the validated package 1.99.81.0 and updated English release notes, retains the mandatory-update setting, and is in certification with publication scheduled as soon as certification passes. The Partner Center submission identifier is `1152921505702144192`. The public Store catalogue served 0.99.80 at submission time; Store approval and public availability of 0.99.81 remain pending.

The suite retains its existing hardware and live-service skips. Live antenna reception and Linux desktop interaction remain unverified. Detailed local release logs and the frozen files are retained under `foxsdr-build/verified-7fea69c/release-0.99.81/`; full Windows suite evidence is in `build/release81-7fea69c-full-test-rerun.log`.
