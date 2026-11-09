#!/usr/bin/env python3
"""wefax-decoder.wav - radiofax fixture (docs 6.50): 6.5 s of silence, 0.5 s white, the 300 Hz start tone (IOC 576, a black/white square wave, edges rounded)
for 3 s, 10 phasing lines (25 ms white + 475 ms black), 12 picture lines of 1810 pixels at 120 lpm (eight grey bars), the 450 Hz stop tone
for 3 s and 1 s of black. The FM audio (black 1500 Hz .. white 2300 Hz) is sent as a carrier above the centre (USB: the audio is the
offset). 48 kS/s I/Q PCM16. The silence leads so the decoder, started by hand after the window settles, hears the start tone.

    python -I make_wefax-decoder.py      writes wefax-decoder.wav beside this script

Standard library only. Timings and tones from wefax_decode.h and test_wefax.cpp's Encoder."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
OFFSET_HZ = 3000.0  # the VFO sits this far above the file centre: see the header of fixture-wefax-decoder.txt
LEAD_S = 6.5
WIDTH = 1810
LINE_MS = 500.0
PHASING_LINES = 10
PICTURE_LINES = 12
BLACK, WHITE = 1500.0, 2300.0


def main() -> None:
    out = Path(__file__).with_name("wefax-decoder.wav")
    segs = []

    def tone(hz, ms):
        segs.append((hz, ms))

    def apt(hz, seconds):
        # a square wave of black/white at `hz` with the edges rounded the way a modulator's baseband filter rounds them
        # (tanh of a sine): the receiver's 2.7 kHz audio filter then does not turn each edge into a click.
        n = int(seconds * RATE + 0.5)
        mid, span = (WHITE + BLACK) / 2.0, (WHITE - BLACK) / 2.0
        for i in range(n):
            tone(mid + span * math.tanh(3.0 * math.sin(2.0 * math.pi * hz * i / RATE)), 1000.0 / RATE)

    tone(WHITE, 500.0)
    apt(300.0, 3.0)
    for _ in range(PHASING_LINES):
        tone(WHITE, LINE_MS * 0.05)
        tone(BLACK, LINE_MS * 0.95)
    px = LINE_MS / WIDTH
    for y in range(PICTURE_LINES):
        for x in range(WIDTH):
            level = (x * 8 // WIDTH) * 255 // 7
            tone(BLACK + (WHITE - BLACK) * level / 255.0, px)
    apt(450.0, 3.0)
    tone(BLACK, 1000.0)

    frames = bytearray(b"\x00\x00\x00\x00" * int(RATE * LEAD_S))
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
    frames += b"\x00\x00\x00\x00" * RATE
    with wave.open(str(out), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(frames))
    total = len(frames) // 4
    print(f"wrote {out} ({total} frames, {total / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
