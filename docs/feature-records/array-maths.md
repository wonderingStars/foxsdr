# Array maths lane (feat/array-maths): OIRT stereo, two-channel combiner, direction-finding maths, W12/W18/N26 pieces

Branch `feat/array-maths` from b9208fba. New files only; no engine, pipeline, op-table, settings or Slint wiring.
CMake edits: none (src/dsp/*.cpp and tests/test_*.cpp are globbed with CONFIGURE_DEPENDS; re-run `cmake .` to pick up new files).
Every figure below is from generated signals only; no radio, no real OIRT signal, no coherent multi-channel capture was available.

## Part 1: OIRT polar-modulation FM stereo (D3)

Files: `src/dsp/oirt_stereo.hpp`, `src/dsp/oirt_stereo.cpp`, `tests/test_oirt_stereo.cpp`.

Source (opened): Recommendation ITU-R BS.450-3 (2001, editorial amendments Oct 2018), Annex 1, section 2.1 "Polar-modulation system",
clauses 2.1.2.1 to 2.1.2.5, https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.450-3-200111-S!!PDF-E.pdf (text extracted locally with pdftotext).
A search result also shows the same figures in BS.450-4 (2019), which I did not read in full (its PDF is a scan; the text layer is not extractable by the fetcher).
What the clauses give and the block uses: M=(A+B)/2, S=(A-B)/2, both 50 us pre-emphasised; S amplitude-modulates 31.25 kHz; the sub-carrier is reduced 14 dB and the
modulation components are shaped by K(f)=(1+j6.4f)/(5+j6.4f) (f in kHz); levels M 80 %, S 80 %, reduced carrier 20 % of maximum multiplex; positive multiplex = positive deviation.
Decoder: quadrature PLL on the 20 % carrier, product detection, restoration of the suppressed level by K^-1=(5+s tau)/(1+s tau) (tau=6.4/(2 pi 1000) s, bilinear at the
composite rate), identical 15 kHz linear-phase low-pass on M and envelope, A=(M+S)/0.8, B=(M-S)/0.8, one-pole 50 us de-emphasis per channel, 20 ms stereo gate.
Lamp: carrier magnitude above 30 % of nominal and quadrature below half of in-phase, held 40 ms.

NOT given by the clause and therefore not guessed: the absolute phase of the sub-carrier (not needed, the carrier is the reference and the in-phase envelope means A > B);
the exact definition of the reference for the "80 %" S level (I took unreduced carrier = 1 so that 20 % is the reduced carrier; separation does not depend on it);
that A is left and B is right (the clause says "left-hand signal, A" and "right-hand signal, B").
Not exercised: real OIRT transmitters (levels may differ from nominal), the 50 kHz deviation variant (caller normalises by deviation).
The decoder uses the nominal levels; it does not estimate them.

Measured (generated multiplex, fs 200 kHz, output 50 kHz, carrier phase 0.7 rad, 0.5 s warm-up):
- left-only 1 kHz: output level 0.4002 against 0.4000 expected (0.00 dB)
- separation: L-only 1 kHz 59.9 dB, R-only 1 kHz 59.9 dB, 100 Hz 40.0 dB (limited by the 2 Hz offset high-pass), 5 kHz 69.6 dB, 10 kHz 69.1 dB, 1 kHz with the carrier 10 Hz off nominal 59.9 dB
- pins: >55 dB at 1/5/10 kHz and +10 Hz offset, >35 dB at 100 Hz
- mono transmission (no sub-carrier, loud tones plus noise): lamp never lit, L==R bit-identical
- stereo transmitter with A==B: lamp on, L and R level within 0.01
- lamp drops within 150 ms when the sub-carrier disappears
- block-split invariance bit-identical
- CPU: 10 s of 200 kS/s composite in 0.457 s on this desk = 4.6 % of one core
Break-it: with the K^-1 restoration bypassed (test hook setRestoreLevelForTest(false)) the 100 Hz separation falls from 40.0 dB to 3.7 dB and the test (`broken < sepL100 - 15`) would fail if the restoration were not doing the work.
Test: `ctest -C Release -R test_oirt_stereo` -> result line in the "Gates" section below.

Hand-off: engine calls `OirtStereo(compositeRate, decimation)` with the WFM discriminator output scaled so +-1 = maximum deviation, then `process()`; shows `stereoLamp()`;
needs a band/mode switch (OIRT mode only for 65.8-74 MHz, the B200 tunes from 70 MHz), optional force-mono and de-emphasis us settings. Output rate = composite/decimation.

## Part 2: two-channel combiner (F17)

Files: `src/dsp/two_channel_combiner.hpp`, `.cpp`, `tests/test_two_channel_combiner.cpp`.

Method (opened): minimum-variance distortionless response (Capon) beamformer, min w^H R w subject to w^H v = 1, w = R^-1 v / (v^H R^-1 v);
S. Loyka, ELG5132 Smart Antennas, Lecture 7 "Optimum Beamforming: Basic Concepts", Univ. of Ottawa, 26-Jan-13, slides 5-6, eqs (7.8)-(7.14),
https://www.site.uottawa.ca/~sloyka/elg5132/Lec_7_ELG5132.pdf (cites H.L. Van Trees, Optimum Array Processing, Wiley 2002; I did not open Van Trees).
The same lecture states the R = sigma^2 I case is the matched filter (maximum-ratio combining). For two channels every matrix is 2x2: inverse by adjugate/determinant,
dominant eigenvector in closed form ([b, lambda-a], lambda = (a+d)/2 + sqrt(((a-d)/2)^2 + |b|^2)). A second source (Iowa State EE524 lecture 11) was refused by the site; a TU Delft slide set was unreachable. Neither is relied on.
Modes: Manual (y = x0 + amp e^{j phase} x1), MaxRatio (steering given, or the dominant eigenvector of the adapted covariance), Capon (steering given; covariance adapted, 1e-4 diagonal loading),
ReferenceNull (blind: y = x0 - (r01/r11) x1, the Wiener / sidelobe-canceller solution; no wanted-signal knowledge). `solveFrom(Cov2)` solves once from an external covariance (a capture of the interferer alone) and freezes.

Measured (generated complex Gaussian wanted, interferer and noise, wanted coupling 0.8 at +0.5 rad, interferer coupling 0.9 at -1.2 rad):
- Manual weight g1 = -1/ri: interferer rejection 149.0 dB (float limit), wanted gain 3.05 dB = analytic |1 - rw/ri|^2 3.05 dB. With wanted and interferer from the SAME coupling the null removes the wanted too (-148.9 dB): stated limit.
- MRC, auto steering from the covariance: SNR gain 3.00 dB at |rho| = 1 (theory 3.01), 0.96 dB at |rho| = 0.5 (theory 0.97); wanted unchanged (0.01 dB).
- Capon, exact steering, interferer 20 dB over noise, wanted 10 dB over noise: interferer rejection 45.6 dB, wanted gain -0.000 dB (distortionless), noise gain 0.42 dB, SINR -10.0 -> 9.6 dB (noise-limited).
- Capon with NO steering given steers on the dominant source: on an interferer-dominated scene the wanted gain was -3.4 dB, i.e. it nulls the wrong signal. Steering must be supplied (from geometry, a calibration, or a wanted-only capture).
- ReferenceNull, interferer 30 dB over wanted: rejection 57.3 dB, wanted gain +3.04 dB; equal-power wanted and interferer: rejection only 3.0 dB (it cannot tell them apart).
- Coherence / slow phase drift between the channels (interferer-only scene, rejection = channel-0 power / output power), drift in cycles per sample:
  0: frozen 46.5, adaptive tau 2048 46.5, tau 256 46.5 dB; 1e-5: frozen -5.1, 17.9, 35.3 dB; 1e-4: frozen -3.0, 2.0, 15.7 dB; 1e-3: -3.0, 0.0, 1.2 dB; 1e-2: -3.0, 0.0, -0.2 dB.
  Reading: weights frozen at t = 0 lose the null within a fraction of one drift cycle and then cost 3 dB; an adaptive tracker holds a null only while the phase moves little within its time constant
  (drift x tau well under 0.1 cycle). Two radios on separate clocks (a B200 and an RTL-SDR) drift far faster than this and stay unusable for nulling; a shared-clock coherent pair is required (F17 item 1).
- CPU: adaptive Capon path 4.8 ns per sample pair (about 1 % of one core at 2 MS/s per channel).
Break-it: sign of the reference-null weight and the conjugate in the Capon weight deliberately broken: 9 of 25 checks went red (iRej, wanted gain, SINR, rejection, three drift pins), restored: 0 of 25 failed.
Pins: manual rejection > 90 dB and gain within 0.05 dB of analytic; MRC within 0.25 dB; Capon rejection > 18 dB, |wanted gain| < 0.05 dB, SINR out > 9 dB; ReferenceNull > 25 dB; drift pins as listed in the test.
UNPROVEN: everything is on generated signals; no coherent two-channel radio on this desk (B200 has one RX chain per the Ettus page cited in the re-check).

Hand-off: engine feeds two sample-aligned complex streams (same rate, same clock) to `TwoChannelCombiner::process`; settings needed: mode, manual amplitude/phase,
steering ratio (amplitude and phase), forgetting time constant, freeze, optional "capture interferer-only covariance then freeze" button using `covariance()` and `solveFrom()`.

## Part 3: direction-finding maths (Q8a)

Files: `src/dsp/doa_maths.hpp`, `.cpp`, `tests/test_doa_maths.cpp`. Maths only, no hardware code.

Built: sample covariance (N = 1..8 coherent channels), Hermitian eigen-decomposition by cyclic Jacobi written here (phase rotation to make the pivot real, then the real Jacobi rotation; no third-party library),
UCA and ULA steering vectors, Bartlett and MUSIC spectra, peak picking (wrap-around for the circle, parabolic refinement, peak-to-median figure and a 0..1 confidence heuristic 1 - 10^(-dB/20)),
`countAboveNoise` (a heuristic eigenvalue-ratio source count, not an information criterion), and a common-source calibration (`calibrateFromCommonSource` returns per-channel complex correction, phase in degrees, gain in dB, and a dominance figure; `applyCalibration` forms K R K^H).
Conventions are in the header (UCA element 0 on +x, bearing counter-clockwise from +x; ULA bearing from the axis, grid 0..180 degrees).

Sources opened: Wikipedia "MUSIC (algorithm)" (steps, pseudo-spectrum, source count must be known; cites Schmidt, IEEE Trans. AP-34, March 1986, pp. 276-280, which I did not open);
Werner, Gardill, Hutter, arXiv:2502.08592v1 (Feb 2025, CC BY-NC-ND 4.0), eq. (1) plane-wave model for an element at (x_m, y_m) and eq. (16) MUSIC spectrum (text extracted with pdftotext);
Loyka ELG5132 lecture 7 (same as Part 2) for the Bartlett spectrum as the matched-beamformer output. The calibration method is my own derivation (R = P c c^H + noise, dominant eigenvector is c); no free source describing a calibration was found, and the Werner paper only mentions a reference antenna in its abstract.
Not opened and not used: krakensdr_doa, pyArgus, gr-doa, Heimdall (GPL) or any other SDR program.

Measured (generated plane waves, complex Gaussian sources and noise, 1000 snapshots, 40 random bearings per cell, RMS bearing error in degrees; grid 0.5 degree with parabolic refinement):
```
 single source   SNR dB   UCA5 MUSIC  UCA5 Bartlett  UCA8 MUSIC  ULA4 MUSIC
                  -10        1.69        1.67          0.77        1.61
                    0        0.25        0.25          0.19        0.34
                   10        0.12        0.10          0.11        0.12
                   20        0.10        0.02          0.09        0.09
```
(UCA5 radius 0.425 lambda, UCA8 0.65 lambda, ULA4 spacing 0.5 lambda, 145 MHz.) The 0.1 degree floor at high SNR is the grid and the parabolic refinement, not noise.
Two uncorrelated sources at 40.3 and 110.7 degrees (off-grid), 30 trials, MUSIC with 2 sources: UCA5 0.82 / 0.23 / 0.14 degrees and UCA8 0.35 / 0.14 / 0.14 degrees at -5 / 5 / 20 dB, all 30 trials resolved; Bartlett on UCA5 at 20 dB 3.06 degrees (broad lobes);
ULA4 60.3/110.7 at 20 dB 0.13 degrees; UCA5 MUSIC two sources 30 degrees apart 0.15 degrees, 12 degrees apart 0.18 degrees (20 dB, still resolved).
Confidence: strong signal 0.987 (38.1 dB over the spectrum median), noise only 0.226 (2.2 dB). Jacobi: residual < 1e-11, orthonormality < 1e-12, N = 1..8, multiplicity-7 eigenvalue, zero matrix, empty input.
Calibration (5 channels, injected random gains of +-2 dB and random phases, common source with 20 dB receiver SNR, 4096 snapshots): worst phase error 0.041 degrees, worst gain error 0.031 dB; bearing RMS error with the injected offsets 100.32 degrees uncalibrated against 0.11 degrees calibrated.
CPU, N = 8: covariance of 4096 snapshots 0.29 ms, Jacobi 0.022 ms (5 sweeps), MUSIC on 720 bearings 0.31 ms.
Ambiguity of a linear array, stated and tested: bearing is measured from the axis and theta and -theta have identical steering vectors (front/back mirror; mismatch 0), so the scan covers 0..180 degrees only and a linear array alone cannot give a full-circle bearing;
with spacing = 1 lambda, bearings with cos(theta') = cos(theta) + 1 are indistinguishable (grating lobe, mismatch 2.8e-16), hence spacing <= lambda/2. A circular array has no such ambiguity but its element spacing must stay <= lambda/2 too.
Break-it: (a) MUSIC using the signal instead of the noise subspace turned 19 of 105 checks red; (b) inverting the calibration factor (ei/ref instead of ref/ei) turned 4 red (phase, gain, calibrated bearing, improvement); both restored: 105/105 pass.
Pins: see the test (UCA5 MUSIC < 1 degree at 20 dB, < 3 at 0 dB, < 15 at -10 dB; two-source < 1 degree at 20 dB with >= 28 of 30 resolved; calibration < 1 degree and < 0.1 dB).
UNPROVEN: generated signals only; no coherent multi-channel radio on this desk; real arrays have mutual coupling, element pattern and position errors, correlated (multipath) sources that break MUSIC, and amplitude/phase drift between calibrations, none modelled.
What hardware would need what (from the re-check section): a KrakenSDR-class device is five RTL-SDR tuners on one clock with a switchable common noise source; its coherent I/Q is served by Heimdall (GPL-3.0, clean-room barrier), so the capture link stays blocked until a permissively licensed description exists or the owner supplies a device to capture.
The maths here needs only: N sample-aligned complex streams, array type and radius or spacing, frequency, and a capture with the noise source on for calibration.

Hand-off: engine/capture lane produces N aligned streams; call `sampleCovariance` per block (about 4096 snapshots), `hermitianEigen`, then `musicSpectrum`/`bartlettSpectrum` and `pickPeaks`; settings: array type, radius/spacing, centre frequency, source count, calibration run (noise source on) and stored per-channel factors applied with `applyCalibration` (or applied to the samples before the covariance).

## Part 4: W12 driver allow-list, W18 WebSDR link-out, N26 phone web side

All new files; nothing downloaded or installed; no existing file edited.

### W12 driver allow-list (`src/core/driver_policy.hpp/.cpp`, `tests/test_driver_policy.cpp`)
Data: one reviewed row per radio family (action, whether the source was opened, source, note) plus the pure lookup `decide(vid, pid, interfaceNumber)` over the existing `radiosetup::knownDevices()` ids.
InstallWinUsb only for the RTL-SDR family and only for interface 0 (never the composite parent -1, never interface 1); NoInstallNeeded for Airspy; NotAllowed for everything else and for every unknown id.
Read (opened 2026-11-11): https://airspy.com/quickstart/ ("Airspy is a plug-and-play device and does not require any particular driver installation on Windows Vista, 7, 8, 8.1 and 10"; manual route = the vendor's WinUSB Compatibility Driver .inf; Zadig not mentioned);
https://hackrf.readthedocs.io/en/latest/installing_hackrf_software.html (no Windows driver, WinUSB or Zadig step; libusb only as a build dependency). A web search found only community advice for HackRF (Zadig rarely needed, "downgrade WCID driver"): not authoritative, so HackRF stays NotAllowed.
RX888, AOR and Mirics: pages not opened, USB ids NOT VERIFIED, no rows added (1df7:2500 is already named an SDRplay RSP1 in core/radio_setup.cpp and is also the Mirics reference id). Airspy HF+ and LimeSDR rows are NotAllowed with evidenceVerified = false (no Windows driver page opened).
Test (`ctest -R test_driver_policy`): 338 checks, 0 failed. Includes: every known family has a row and every row names a real family; no duplicate rows; an InstallWinUsb row must be evidence-verified; for every id in `rtlSdrUsbIds()` interface 0 installs and interface 1 / parent do not;
for EVERY known device the policy installs exactly when the shipped `winusb::targetAllowed` accepts the same device (cross-check against the existing installer); fake ids (0000:0000, ffff:ffff, 1234:5678, 0bda:0000, 04b4:00f1) refused on every interface.
Break-it: dropping the interface-0 condition turned the interface 1 / parent checks red; restored.
Hand-off: the installer (`core/winusb_install.cpp`) should call `driverpolicy::decide` before offering the key, and a radio-setup finding for Airspy can say "no driver install needed" using the row; adding a family means adding a row with a source. UNPROVEN: nothing was installed; no HackRF, Airspy or other radio driver behaviour was observed.

### W18 WebSDR link-out (`src/core/websdr_link.hpp/.cpp`, `tests/test_websdr_link.cpp`)
Pure `buildLink(baseUrl, frequencyKHz, mode)` -> "scheme://host[:port]/path?tune=<kHz>[am]". Source (opened): https://www.websdr.org/faq.html: deep link "?tune=" appended to the site address, example http://websdr.ewi.utwente.nl:8901/?tune=198am (198 kHz, AM); the FAQ has no terms for linking, nothing on automated clients, and says the author takes action against framing his WebSDR (so open it in the system browser, never embed).
Documented: the shape and the "am" suffix. NOT VERIFIED: usb/lsb/cw/fm suffixes and fractional kHz: any non-AM mode yields the frequency alone and `modeSuffixDocumented = false` so the UI can warn. Frequency printed to at most three decimals. Refused: non-http(s), no host, user:password or any "@", bad ports, spaces/quotes/angle brackets, non-finite or out-of-range frequency (0 < f <= 3,000,000 kHz), non-alphanumeric mode. Query and fragment on the base are dropped.
Test: 76 checks, 0 failed (the FAQ example reproduced byte for byte). Break-it: removing the "@" refusal turned 6 checks red; restored. An audio client is still blocked on a written reply from the operator (contact address NOT VERIFIED; via websdr.org).
Hand-off: a "Open in browser" key on a WebSDR list row calls `buildLink` and hands the URL to the OS shell; needs the list of WebSDR addresses from the owner's directory work.

### N26 phone web side (`src/net/web_phone.hpp/.cpp`, `tests/test_web_phone.cpp`)
Built: the manifest JSON, `<head>` tags, an additive phone stylesheet (44 CSS px targets, safe-area padding, tables scroll inside themselves, no literal colours because the web UI's tokens govern), `cspWithManifest`, and checkers `originInstallable` and `manifestInstallable`.
Sources (opened 2026-11-11): MDN "Making PWAs installable" (manifest linked from every page; Chromium needs name or short_name, icons with 192 and 512 px, start_url, display or display_override, prefer_related_applications false/absent; https or localhost/127.0.0.1 with or without port; a service worker is NOT required; Firefox does not install PWAs; Android installs only from Chrome with Google Mobile Services or Samsung Internet; iOS 16.4+ via the Share menu);
MDN CSP manifest-src (falls back to default-src); W3C WCAG 2.2 Understanding SC 2.5.8 (24 by 24 CSS px; the page also describes 2.5.5 with 44 by 44); MDN env() (safe-area-inset-top/right/bottom/left; viewport-fit=cover is NOT on that page: general knowledge, NOT VERIFIED).
Finding that matters for wiring: the web UI's Content-Security-Policy is `default-src 'none'; ...` and has no manifest-src, so a manifest would be BLOCKED; `cspWithManifest` appends `manifest-src 'self'`.
Test: 47 checks, 0 failed: shipped manifest passes the checker; removing start_url, display, icons, name+short_name, the 512 icon or setting prefer_related_applications true each fails it with the right message (the checker's own break-it); origin rule cases; CSP helper idempotent and exact; CSS braces balanced, inside `@media (max-width: 600px)`, no url(), @import, #, !important. Break-it on the product code: disabling the CSP helper turned 3 checks red; restored.
Not "tested the way existing web tests are" (those drive web_server.cpp over a real loopback listener): the pieces are not served yet; a test that serves them needs the patch below.
PATCH DESCRIPTION for web_server.cpp (file owned by other work, not edited here):
1. In `svr.Get("/")` (around line 3379) keep the page as is but (a) replace the existing viewport `<meta>` in `kIndexHtml` with `phone::headTags()` content (viewport now has viewport-fit=cover, plus theme-color and manifest link); (b) set the CSP through `phone::cspWithManifest(...)`.
2. Add `svr.Get(phone::kManifestPath, ...)` returning `phone::manifestJson()` as `application/manifest+json`, as an ungated static route exactly like `/app.css` and the fonts (verified: those routes at lines 3379-3421 carry no session check; the manifest holds nothing private and a browser fetches it without credentials by default, so it must not be gated).
3. Add `svr.Get(phone::kIcon192Path ...)` and `kIcon512Path`: 192 and 512 px PNGs, which do not exist yet (resources/icon has a 256 px PNG; both must be made from generate_icon.py and embedded like the fonts).
4. In the `/app.css` route append `phone::phoneCss()` to the joined stylesheet (the existing join is `kAppCss1 + kAppCss2 + kAppCss3` near line 884).
5. HTTPS: installability needs https (or localhost): see N13 (TLS, owner's word still needed). Over plain http on a LAN address the page works in a phone browser but cannot be installed (`originInstallable` says so).
Native Android/iOS builds stay with U2/U3 and are not part of this lane.
Hand-off settings: none required; optionally show the `originInstallable` problem text on the web settings page.

## Gates (build D:/foxsdr-build/feat-maths, Release, Slint off, softgl off)

Own tests: `test_oirt_stereo` 26 checks, `test_two_channel_combiner` 25, `test_doa_maths` 105, `test_driver_policy` 338, `test_websdr_link` 76, `test_web_phone` 47: all 0 failed (run directly and via ctest).
Whole set: `ctest -C Release -L "headless|slint" -j4 --timeout 300` on the full build (690 tests): "97% tests passed, 22 tests failed out of 690".
Red in that run and what they are: the known false failures test_airband_data, test_band_plan, test_converter_call_sites, test_engine_api_transmit, test_page_resize_id, test_png_decode, test_window_failed_app (7);
load flakes that passed when re-run alone once: test_audio_open, test_bookmark_save_async, test_bounded_hooks, test_engine_main_smoke, test_engine_service_keep, test_failure_files, test_gui_file_jobs, test_iq_open_async, test_link_request_poll, test_plugin_hostile, test_plugin_unload_handshake, test_patch_dest_async_stepped, test_plugin_store_window_free, test_stop_ends_recordings_free (the last failed once with 1 check in a second concurrent run, passed alone twice);
test_pass_control (on the known load-flake list) failed again alone, on its latency checks only (`longestStartGap <= 36`, `kicksDropped*10 <= nKicks`, `kickedPasses >= ...`, 4 of 709 checks) while other lanes were building on the machine: a timing measurement, not related to new files; not re-proved green.
Existing files edited by this lane: none (no CMake edits either; sources and tests are globbed). Records: this file only.
