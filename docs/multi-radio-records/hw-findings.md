# Hardware findings lane (2026-11-17): fixes for what the real B200 and RTL-SDR showed

Branch `feat/hw-findings`, cut from `860fbc65` (`engine/multi-radio`), worktree `D:/foxsdr-wt/en-tuning`, build `D:/foxsdr-build/en-tuning`,
scratch `D:/foxsdr-build/hwf-scratch/`. Source: `D:/foxsdr-build/hw2-scratch/RESULTS.md`. Nothing pushed. Receive only on both radios.
Error-path pin at the start: 410 (`kPendingPin`, tests/test_error_paths.cpp).

## Item 1: RTL-SDR analogue IF bandwidth - HIDDEN

What the setting programs (src/source/tuner_r82xx.cpp `setBandwidthHz`, the same formulas as the reference driver): for a requested width
below 2.43 MHz it writes register 0x0a (bit 4 clear), 0x0b (bits 6 and 5 = high-pass sections not added, low nibble = 15 - table index
of a low-pass corner 350..1700 kHz) and chooses an IF of 2.3 MHz - realBw/2 (+ the added high-pass sections). The code's own model is
a passband of realBw (350 kHz for 300 kHz) centred on that IF.

Measured on the real dongle (R820T, serial 00000001) with a scratch probe that opens `RtlSdrSource` directly
(`hwf-scratch/probe.cpp`, FM band, 98.0 MHz, 2.4 MS/s, 4.8 MS recorded per run, counts and levels only, 10th-percentile envelope per 100 kHz of a
4096-point averaged spectrum, `an.py`):

| setting | envelope (dB) at -1100 / -800 / -500 / -400 / -300 / 0 / +1100 kHz |
|---|---|
| automatic (IF 1.815 MHz) | 15.8 / 20.4 / 21.2 / 21.2 / 21.7 / 25.0 / 18.1 |
| 300 kHz (IF 2.125 MHz) | 5.2 / -6.2 / 4.9 / 14.0 / 18.8 / 23.8 / 16.0 |
| 1000 kHz (IF 1.7 MHz) | -5.7 / 14.1 / 21.2 / 20.9 / 21.7 / 25.0 / 16.8 |

Reproduces the check's finding: 300 kHz is one-sided (lower side falls about 26 dB below -450 kHz, upper side untouched to +1.15 MHz).

Why (shown, not assumed):
1. Held at an EQUAL IF (the IF forced to 2.125 MHz with a scratch-only override), the settings 300, 500, 700 and 1000 kHz gave the SAME
   envelope within 1.5 dB at every 100 kHz step (e500/e700/e1000 against 300 kHz: -500 kHz 5.7/5.5/5.2 against 4.9, -400 kHz 14.6/14.5/14.3
   against 14.0). The low-pass nibble that is supposed to set the width has no measurable effect.
2. What moves the edge is the IF position only: with the 300 kHz registers and the IF shifted by -800, -400, +400, +800 kHz the edge moved
   by about the same amount (edge at +0.9 MHz, -0.85, -0.05, +0.3 MHz against -0.43 at no shift). Mapped in absolute IF terms the passband
   is about 0.45 to 2.55 MHz (2.1 MHz wide, centred near 1.5 MHz), whatever width was typed.
3. The register writes do arrive (readback of the status registers showed the tuner answering; the shadow showed 0x8f for automatic,
   0xe6 for 300 kHz, 0xeb for 1000 kHz). The set-up the driver performs at open (src/source/tuner_r82xx.cpp `setTvStandard`) switches on the
   channel-filter extension (register 0x1e bits 6:5) and sets the widest extension (0x0f bit 7 clear), which is the reference driver's
   television configuration and is what keeps the 2.4 MS/s window inside the filter; behind it the width nibble does nothing measurable.

So the IF is not "not centred": no IF placement makes a 300 kHz-wide passband, because no width is applied. It cannot be made true by a
small correct change (it would need the channel-filter extension off, which changes the whole tuner configuration of every sample
rate, and would have to be calibrated on R820T, R828D and the Blog V4 separately). The control is therefore HIDDEN for this release:
`foxsdr::slints5b::ifBandwidthControlOffered()` (src/slint/s5b_logic.hpp) is `false`, `S5Rows::buildS5b` draws nothing for it, and
`test_slint_s5b_host` holds that no `b.ifbw*` row exists whatever the engine's `ifBandwidth` fact says. "Automatic" is exactly as before
(the driver, the engine option `if_bandwidth` and `tests/test_rtlsdr_ifbw` are untouched: 50 checks, 0 failed). The shared label
"Bandwidth (kHz)" is now carried by the driver bandwidth row (SoapySDR) and the tune row only; they stay (a rename would add a key to all
33 catalogues for a label that is not wrong on its own).
Break-it: `ifBandwidthControlOffered()` set to `true` -> `test_slint_s5b_host` 84 checks, 9 failed; restored, 0 failed.
What a true version needs: the filter extension and the width nibble characterised per tuner by a spectrum sweep like the one above,
then a table of (register set, IF) pairs verified to give a passband of the stated width centred on the tuned frequency.

## Item 2: automatic ppm - HIDDEN

How it measures (src/core/ppm_auto.hpp, src/engine/engine_mpx.cpp): the FM stereo pilot PLL's residual frequency is read every 100 ms while
the pilot is locked and its level is at least 0.5; ppm = -mean(df) / 19 kHz x 1e6. The pilot is therefore taken as exactly 19 kHz.
Tolerance: the header says "the standard allows the pilot +-2 Hz (105 ppm)" and cites NO standard by name; the figure matches ITU-R
BS.450 as remembered, but that is not checked here (no outside request is allowed in this lane): UNVERIFIED against the document. The
reported uncertainty is the statistical one only (scatter of the readings), which cannot see a station whose pilot is off.
Real measurements (hardware check, `D:/foxsdr-build/hw2-scratch/RESULTS.md` section 3, 30 s, 256 readings): station A, dongle +18.91 +-0.81,
+17.06 +-0.78, +15.97 +-0.84 ppm in three runs (spread 2.9 ppm, three times the stated uncertainty); the B200 on the same station +17.08 +-0.15;
station B, dongle -3.91 +-6.5 and B200 -2.39 +-0.66. The two radios agree with each other on each station (and carriers compared directly
the day before agreed within 0.8 ppm) but the number changes by about 20 ppm from station to station: it follows the station's pilot,
not the radio. Nothing in the code or the measurements shows it is right as an absolute correction, so applying it ("Use this
correction") would mis-tune the user.
Done: the Automatic ppm panel is not drawn (`foxsdr::slints5::ppmAutoControlOffered()` is `false`, src/slint/s5_logic.hpp; `S5Rows::build`
skips `buildPpm`). The manual ppm setting is untouched. The engine command `FOXAPP_OP_PPM_MEASURE`, its facts and `test_engine_api_ppmauto`
stay. Tests: `test_slint_s5_host` new block (no `m.*` row, no "Automatic ppm" / "Measure the crystal error" / "Use this correction" label
whatever the engine's facts say, a stale press sends nothing); the old row-logic block is kept and now runs with the seam
`S5Rows::setPpmAutoOffered(true)`; `tests/test_engine_api_s5.cpp` testC sets the same seam; drive script
`src/slint/tests/drive/s5-sources.txt` no longer clicks the key. Break-it: rule set `true` -> `test_slint_s5_host` 105 checks, 5 failed; restored, 0 failed.
What a sound method needs: a reference that is not a broadcast station - a frequency standard (GPS-disciplined source, or a carrier whose
tolerance is known to be a fraction of a ppm), or an average over many stations with a stated rejection of outliers and a result that
carries the SPREAD between stations rather than only the PLL's scatter; the test must include two stations with different pilot errors.
EXPECTATION CHANGED (listed in the final report): `src/slint/tests/test_slint_s5_host.cpp` line 151 `r.find(v, "m.start") != nullptr` is now `== nullptr` (the panel is not drawn).
