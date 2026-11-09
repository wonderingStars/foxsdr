#!/usr/bin/env python3
"""sstv-decoder.wav - SSTV fixture (docs 6.50): Martin M1 (VIS 44): the VIS header, then the first LINES scan lines of a colour-bar picture,
an audio-frequency-modulated signal (1200 Hz sync, 1500 Hz black .. 2300 Hz white) sent as a carrier 1.5-2.3 kHz above the centre (USB: the
audio is the offset). 48 kS/s I/Q PCM16. Seven seconds of silence lead the signal so the decoder, started by hand after the window settles,
is already running when the header arrives (the engine loops the file).

    python -I make_sstv-decoder.py      writes sstv-decoder.wav beside this script

Standard library only. Timings from sstv_modes.h (Martin: sync 4.862 ms, gaps 0.572 ms, scans 146.432 ms) and test_sstv.cpp's Encoder."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
LINES = 40
LEAD_S = 7.0
OFFSET_HZ = 3000.0  # the VFO sits this far above the file centre (the 42 kHz passband the header asks for leaves 3 kHz of room): see fixture-sstv-decoder.txt
SYNC, GAP, SCAN = 4.862, 0.572, 146.432
WIDTH = 320
# colour bars: white yellow cyan green magenta red blue black
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def hz_of(level):
    return 1500.0 + level * 800.0 / 255.0


def main() -> None:
    out = Path(__file__).with_name("sstv-decoder.wav")
    segs = []  # (hz, ms)

    def tone(hz, ms):
        segs.append((hz, ms))

    # VIS header for Martin M1 = 44
    vis = 44
    tone(1900.0, 300.0)
    tone(1200.0, 10.0)
    tone(1900.0, 300.0)
    tone(1200.0, 30.0)
    ones = 0
    for b in range(7):
        bit = (vis >> b) & 1
        ones += bit
        tone(1100.0 if bit else 1300.0, 30.0)
    tone(1100.0 if (ones & 1) else 1300.0, 30.0)
    tone(1200.0, 30.0)

    def scan(channel):
        # one scan of WIDTH pixels, each pixel SCAN/WIDTH ms, from the bar for that pixel
        px = SCAN / WIDTH
        for x in range(WIDTH):
            tone(hz_of(BARS[x * len(BARS) // WIDTH][channel]), px)

    for _ in range(LINES):
        tone(1200.0, SYNC)
        tone(1500.0, GAP)
        scan(1)  # green
        tone(1500.0, GAP)
        scan(2)  # blue
        tone(1500.0, GAP)
        scan(0)  # red
        tone(1500.0, GAP)
    tone(1500.0, 300.0)

    # Render with exact running time (no per-tone rounding drift), phase-continuous.
    lead = int(RATE * LEAD_S)
    frames = bytearray(b"\x00\x00\x00\x00" * lead)
    elapsed = 0.0
    n = 0
    phase = 0.0
    amp = 0.5 * 32767.0
    pack = struct.Struct("<hh").pack
    for hz, ms in segs:
        elapsed += ms * RATE / 1000.0
        want = int(elapsed + 0.5)
        dp = 2.0 * math.pi * (hz + OFFSET_HZ) / RATE
        while n < want:
            frames += pack(int(amp * math.cos(phase)), int(amp * math.sin(phase)))
            phase += dp
            if phase > 2.0 * math.pi:
                phase -= 2.0 * math.pi
            n += 1
    tail = int(RATE * 1.0)
    frames += b"\x00\x00\x00\x00" * tail
    with wave.open(str(out), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(frames))
    total = lead + n + tail
    print(f"wrote {out} ({total} frames, {total / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
