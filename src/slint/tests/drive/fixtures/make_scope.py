#!/usr/bin/env python3
"""scope.wav - the demod scope's fixture (docs 6.57): an AM carrier at +22.5 kHz of a 48 kS/s I/Q file, 40 % modulated by a 100 Hz tone, 4 s, PCM16 stereo (I, Q).

The engine tunes a 3 kHz-wide channel 22.5 kHz above the file's centre (the span edge less half the passband, docs 6.50), so the carrier lands on the VFO: the AUDIO tap carries the
100 Hz tone (a half period of 5 ms - long enough for the scope's trigger hold-off at 10 ms/DIV), the I/Q tap carries the envelope on the channel's baseband, a circle that breathes.

    python -I make_scope.py            writes scope.wav beside this script

Standard library only: the fixtures are made on any machine without a package install."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
SECONDS = 4
CARRIER_HZ = 22500.0
TONE_HZ = 100.0
DEPTH = 0.4


def main() -> None:
    out = Path(__file__).with_name("scope.wav")
    n = RATE * SECONDS
    frames = bytearray()
    for k in range(n):
        t = k / RATE
        env = 1.0 + DEPTH * math.sin(2.0 * math.pi * TONE_HZ * t)
        ph = 2.0 * math.pi * CARRIER_HZ * t
        i = int(0.45 * 32767 * env / (1.0 + DEPTH) * math.cos(ph))
        q = int(0.45 * 32767 * env / (1.0 + DEPTH) * math.sin(ph))
        frames += struct.pack("<hh", i, q)
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


if __name__ == "__main__":
    main()
