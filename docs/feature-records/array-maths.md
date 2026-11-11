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
