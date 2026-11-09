#!/usr/bin/env python3
"""apt-decoder.wav - NOAA APT fixture (docs 6.50): 6.5 s of silence, then 24 APT lines (12 s at 2 lines/s): a 2400 Hz subcarrier amplitude
modulated by the 2080-word line (sync A train, space, a left-to-right ramp in video A, sync B train, a flat field in video B, telemetry
bars; layout from test_apt.cpp), carried by wide-FM-style narrow FM: the baseband is FM modulated 4 kHz above the centre with a 3 kHz peak
deviation (the receiver is NFM). 48 kS/s I/Q PCM16. The silence leads so the decoder, started by hand after the window settles, sees
the whole pass.

    python -I make_apt-decoder.py      writes apt-decoder.wav beside this script

Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
LEAD_S = 6.5
LINES = 24
WORDS = 2080
WORD_RATE = 4160.0
SUB_HZ = 2400.0
DEV_HZ = 3000.0
PEAK = 0.9
OFFSET_HZ = 4000.0  # the VFO sits this far above the file centre (the 40 kHz passband the header asks for leaves 4 kHz of room): see fixture-apt-decoder.txt


def make_line():
    w = [0.0] * WORDS
    n = 0
    n += 4
    for _ in range(7):
        w[n] = 1.0
        w[n + 1] = 1.0
        n += 4
    n += 7
    for _ in range(47):
        w[n] = 1.0
        n += 1
    for k in range(909):
        w[n] = k / 908.0
        n += 1
    for k in range(45):
        w[n] = 0.8 if (k // 9) % 2 else 0.2
        n += 1
    for k in range(39):
        w[n] = 1.0 if k % 5 == 0 else 0.0
        n += 1
    n += 47
    for _ in range(909):
        w[n] = 0.5
        n += 1
    while n < WORDS:
        w[n] = 0.4
        n += 1
    return w


def main() -> None:
    out = Path(__file__).with_name("apt-decoder.wav")
    line = make_line()
    total = int(LINES * WORDS / WORD_RATE * RATE)
    frames = bytearray(b"\x00\x00\x00\x00" * int(RATE * LEAD_S))
    pack = struct.Struct("<hh").pack
    phase = 0.0
    amp = 0.5 * 32767.0
    k = 2.0 * math.pi * DEV_HZ / RATE
    carrier = 2.0 * math.pi * OFFSET_HZ / RATE
    sub = 2.0 * math.pi * SUB_HZ / RATE
    for i in range(total):
        word = int(i * WORD_RATE / RATE)
        v = line[word % WORDS]
        audio = PEAK * (0.1 + 0.8 * v) * math.sin(sub * i)
        phase += carrier + k * audio
        frames += pack(int(amp * math.cos(phase)), int(amp * math.sin(phase)))
    frames += b"\x00\x00\x00\x00" * RATE
    with wave.open(str(out), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(frames))
    n = len(frames) // 4
    print(f"wrote {out} ({n} frames, {n / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
