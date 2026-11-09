#!/usr/bin/env python3
"""selcal-decoder.wav - SELCAL fixture (docs 6.50): the code AB-CD (ICAO Annex 10 Table 3-1: Red A 312.6 + Red B 346.7 Hz for 1.0 s, a 0.2 s gap, then
Red C 384.6 + Red D 426.6 Hz for 1.0 s), sent as two simultaneous carriers per pulse on the UPPER sideband the plugin's START preset selects (SELCAL NAT-A
5598 kHz USB). 48 kS/s I/Q PCM16. In USB an audio tone f is a carrier f ABOVE the VFO; the receiver keeps the VFO 22.5 kHz above the file's centre
(48000 / 2 - 3000 / 2, the 3 kHz minimum passband), so the carriers sit at 22500 + f. Expected decode: "SELCAL AB-CD  [A 312.6 B 346.7 Hz, 1.00 s]  gap 0.20 s  [C 384.6 D 426.6 Hz, 1.00 s]".
The engine loops the file (a call every 4 s).

    python -I make_selcal-decoder.py      writes selcal-decoder.wav beside this script

Standard library only. Deterministic."""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 48000
VFO_OFFSET_HZ = 22500.0
PULSES = [((312.6, 346.7), 1.0), ((384.6, 426.6), 1.0)]
GAP_S = 0.2
LEAD_S = 0.6
TAIL_S = 1.2


def main() -> None:
    out = Path(__file__).with_name("selcal-decoder.wav")
    # per-sample list of the active audio tones: [] for silence
    segs = [((), LEAD_S)]
    for i, (tones, sec) in enumerate(PULSES):
        if i:
            segs.append(((), GAP_S))
        segs.append((tones, sec))
    segs.append(((), TAIL_S))
    rnd = random.Random(5)
    frames = bytearray()
    k = 0
    for tones, sec in segs:
        n = int(round(RATE * sec))
        for j in range(n):
            i = rnd.gauss(0, 0.001)
            q = rnd.gauss(0, 0.001)
            if tones:
                # 5 ms raised-cosine edges so the pulse start and end do not splash
                ramp = int(RATE * 0.005)
                e = 1.0
                if j < ramp:
                    e = 0.5 - 0.5 * math.cos(math.pi * j / ramp)
                elif j >= n - ramp:
                    e = 0.5 - 0.5 * math.cos(math.pi * (n - 1 - j) / ramp)
                for t in tones:
                    ph = 2.0 * math.pi * (VFO_OFFSET_HZ + t) * k / RATE
                    i += 0.2 * e * math.cos(ph)
                    q += 0.2 * e * math.sin(ph)
            frames += struct.pack("<hh", int(32767 * i), int(32767 * q))
            k += 1
    with wave.open(str(out), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(frames))
    print(f"wrote {out} ({k} frames, {k / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
