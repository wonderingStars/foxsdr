#!/usr/bin/env python3
"""vor-decoder.wav - a VOR composite with a known bearing for the VOR decoder (docs 6.50).

Radial 090, ident "FOX" (1020 Hz, about 7 wpm, from 0.3 s), 8 s, AM on a carrier (centre 113.000 MHz): 30 Hz variable AM
(30 %), a 9960 Hz subcarrier frequency-modulated at 30 Hz with deviation ratio 16 (30 % depth), the ident at 10 %. The file is the RF
I/Q at 48 kS/s (so the AM demodulator's audio is the 48 kHz the plugin asks for). THE RECEIVER'S TUNE: the engine puts the VFO at centre + rate/2 -
bandwidth/2 (24000 - 11000 = +13 kHz for the 22 kHz channel the script sets; the script asserts tunedHz), so the carrier is at +13 kHz:
I + jQ = 0.5 * (1 + composite) * exp(j 2 pi 13000 t).
Mirrors foxsdr-plugins-dev/src/vor/test_vor.cpp synth(): the variable phase LAGS the reference by the bearing.

    python -I make_vor-decoder.py      writes vor-decoder.wav beside this script. Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
SECONDS = 8.0
BEARING_DEG = 90.0
IDENT = "FOX"
IDENT_START = 0.3
CARRIER_HZ = 13000.0
MORSE = {"F": "..-.", "O": "---", "X": "-..-"}


def keying(n):
    keyed = bytearray(n)
    dot = 1.2 / 7.0
    t = IDENT_START
    for ch in IDENT:
        for e in MORSE[ch]:
            ln = dot * 3.0 if e == "-" else dot
            for k in range(int(t * RATE), min(n, int((t + ln) * RATE))):
                keyed[k] = 1
            t += ln + dot
        t += dot * 2.0
    return keyed


def main():
    out = Path(__file__).with_name("vor-decoder.wav")
    n = int(RATE * SECONDS)
    keyed = keying(n)
    theta = math.radians(BEARING_DEG)
    phi_var = -theta
    raw = bytearray()
    two_pi = 2.0 * math.pi
    for k in range(n):
        t = k / RATE
        v = 0.30 * math.cos(two_pi * 30.0 * t + phi_var)
        v += 0.30 * math.cos(two_pi * 9960.0 * t + 16.0 * math.sin(two_pi * 30.0 * t))
        if keyed[k]:
            v += 0.10 * math.sin(two_pi * 1020.0 * t)
        e = 0.5 * (1.0 + v) * 32767
        ph = two_pi * CARRIER_HZ * k / RATE
        raw += struct.pack("<hh", int(e * math.cos(ph)), int(e * math.sin(ph)))
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(raw))
    print(f"wrote {out} ({n} frames, {RATE} S/s)")


if __name__ == "__main__":
    main()
