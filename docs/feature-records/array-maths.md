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
