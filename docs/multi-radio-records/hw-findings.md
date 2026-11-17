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
