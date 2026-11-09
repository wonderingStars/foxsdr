#!/usr/bin/env python3
"""twotone-decoder.wav - two-tone sequential paging fixture (docs 6.50): a Motorola Quick Call II individual call, cap code 258 = reed 125 (746.8 Hz, 1.0 s)
then reed 128 (879.0 Hz, 3.0 s), no gap (twotone_tables.h, the chart's timings), FM-modulated at 3 kHz peak deviation on a carrier 41.75 kHz above the
file's centre. 96 kS/s I/Q PCM16.

The plugin's START preset is NFM, 12.5 kHz, 154.000 MHz; the receiver keeps the VFO at (rate / 2 - bandwidth / 2) above the file's centre
(96000 / 2 - 12500 / 2 = 41750 Hz), so the carrier sits there. Expected decode: "TWO-TONE  A 746.8 Hz 1.00 s   B 879.0 Hz 3.00 s" and
"Motorola Quick Call II  code 258". The engine loops the file: silence (an unmodulated carrier) before and after the page.

    python -I make_twotone-decoder.py     writes twotone-decoder.wav beside this script

Standard library only. Deterministic."""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 96000
VFO_OFFSET_HZ = 41750.0
DEVIATION_HZ = 3000.0
TONES = [(746.8, 1.0), (879.0, 3.0)]
LEAD_S = 0.6
TAIL_S = 2.4


def main() -> None:
    out = Path(__file__).with_name("twotone-decoder.wav")
    audio = [0.0] * int(RATE * LEAD_S)
    for hz, sec in TONES:
        n = int(RATE * sec)
        w = 2.0 * math.pi * hz / RATE
        audio += [0.9 * math.sin(w * k) for k in range(n)]
    audio += [0.0] * int(RATE * TAIL_S)
    rnd = random.Random(3)
    frames = bytearray()
    phase = 0.0
    base = 2.0 * math.pi * VFO_OFFSET_HZ / RATE
    k_dev = 2.0 * math.pi * DEVIATION_HZ / RATE
    for a in audio:
        phase += base + k_dev * a
        i = 0.5 * math.cos(phase) + rnd.gauss(0, 0.001)
        q = 0.5 * math.sin(phase) + rnd.gauss(0, 0.001)
        frames += struct.pack("<hh", int(32767 * i), int(32767 * q))
        if phase > 1.0e6:
            phase %= 2.0 * math.pi
    phase %= 2.0 * math.pi
    with wave.open(str(out), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(frames))
    print(f"wrote {out} ({len(audio)} frames, {len(audio) / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
