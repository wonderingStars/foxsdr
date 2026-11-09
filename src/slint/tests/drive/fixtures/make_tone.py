#!/usr/bin/env python3
"""tone.wav - the simplest I/Q fixture (docs 6.50): a 1 kHz carrier above the centre, 48 kS/s, 4 s, PCM16 stereo (I, Q).

    python -I make_tone.py            writes tone.wav beside this script

Standard library only: the fixtures are made on any machine without a package install."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
SECONDS = 4
OFFSET_HZ = 1000.0


def main() -> None:
    out = Path(__file__).with_name("tone.wav")
    n = RATE * SECONDS
    frames = bytearray()
    for k in range(n):
        ph = 2.0 * math.pi * OFFSET_HZ * k / RATE
        i = int(0.5 * 32767 * math.cos(ph))
        q = int(0.5 * 32767 * math.sin(ph))
        frames += struct.pack("<hh", i, q)
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print(f"wrote {out} ({n} frames, {RATE} S/s)")


if __name__ == "__main__":
    main()
