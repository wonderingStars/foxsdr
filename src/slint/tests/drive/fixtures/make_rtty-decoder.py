#!/usr/bin/env python3
"""rtty-decoder.wav - RTTY fixture (docs 6.50): "FOXSDR FIXTURE 1" + CR LF, 45.45 Bd Baudot (ITA2, start bit, 5 data bits LSB first, 1.5 stop bits),
170 Hz shift, as FSK on the LOWER sideband the plugin's START preset selects (RTTY 14.080 MHz: LSB, 3000 Hz). 48 kS/s I/Q PCM16.

The audio tone pair is the plugin's own (rtty_demod.h): mark 2125 Hz, space 2295 Hz (centre 2210 Hz). In LSB an audio tone f is a carrier f BELOW the
VFO, and the receiver keeps the VFO 22.5 kHz above the file's centre (centreHz 14057500 / tunedHz 14080000 after the preset), so the carriers sit at
22500 - f: mark is the higher radio frequency, as on the air. The message is sent four times (the engine loops the file, the decoder starts part-way in).

    python -I make_rtty-decoder.py    writes rtty-decoder.wav beside this script

Standard library only. Deterministic."""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 48000
VFO_OFFSET_HZ = 22500.0
MARK_HZ = 2125.0
SPACE_HZ = 2295.0
BAUD = 45.45
MESSAGE = "FOXSDR FIXTURE 1"
REPEATS = 4

LETTERS = ['\0', 'E', '\0', 'A', '\0', 'S', 'I', 'U', '\0', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
           'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', '\0', 'M', 'X', 'V', '\0']
FIGURES = ['\0', '3', '\0', '-', '\0', "'", '8', '7', '\0', '\x02', '4', '\x03', ',', '\x01', ':', '(',
           '5', '+', ')', '2', '\x01', '6', '0', '1', '9', '?', '\x01', '\0', '.', '/', '=', '\0']
CODE_LF, CODE_SPACE, CODE_CR, CODE_FIGS, CODE_LTRS = 0x02, 0x04, 0x08, 0x1B, 0x1F


def encode(text):
    out = []
    figures = False
    for c in text:
        if c == " ":
            out.append(CODE_SPACE)
            figures = False
            continue
        if c in LETTERS and c != "\0":
            code, need = LETTERS.index(c), False
        else:
            code, need = FIGURES.index(c), True
        if need != figures:
            out.append(CODE_FIGS if need else CODE_LTRS)
            figures = need
        out.append(code)
    return out


def main() -> None:
    out = Path(__file__).with_name("rtty-decoder.wav")
    codes = []
    for _ in range(REPEATS):
        codes += [CODE_LTRS, CODE_LTRS] + encode(MESSAGE) + [CODE_CR, CODE_LF, CODE_LTRS, CODE_LTRS]
    spb = RATE / BAUD
    # (mark?, bits) runs, idle mark first and last as a resting line sends
    runs = [(True, 20.0)]
    for c in codes:
        runs.append((False, 1.0))
        for b in range(5):
            runs.append((bool((c >> b) & 1), 1.0))
        runs.append((True, 1.5))
    runs.append((True, 20.0))
    rnd = random.Random(7)
    frames = bytearray()
    phase = 0.0
    carry = 0.0
    for mark, bits in runs:
        carry += spb * bits
        n = int(carry + 0.5)
        carry -= n
        f = VFO_OFFSET_HZ - (MARK_HZ if mark else SPACE_HZ)
        dphi = 2.0 * math.pi * f / RATE
        for _ in range(n):
            i = 0.4 * math.cos(phase) + rnd.gauss(0, 0.002)
            q = 0.4 * math.sin(phase) + rnd.gauss(0, 0.002)
            frames += struct.pack("<hh", int(32767 * i), int(32767 * q))
            phase += dphi
        phase %= 2.0 * math.pi
    nframes = len(frames) // 4
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print(f"wrote {out} ({nframes} frames, {nframes / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
