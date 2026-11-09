#!/usr/bin/env python3
"""ert-decoder.wav - one Itron ERT SCM meter packet as raw I/Q (docs 6.50): on-off keyed Manchester at 32768 chips/s, the
96-bit SCM of meter id 41234567, type 5 (electric), consumption 1234567, tamper phy1,enc2, a 50 kHz carrier offset.
Receiver: centre 912.600 MHz, mode RAW, 2.4 MS/s (the I/Q decoder's preferred rate).

    python -I make_ert-decoder.py          writes ert-decoder.wav beside this script

Mirrors foxsdr-plugins-dev/src/ert/test_ert.cpp (buildScm, bchCheckBits, chipsOf, addBurst): sync bit 1 + preamble 0xF2A60,
BCH(255,239)-shortened check with P(X)=1+X+X^5+X^6+X^8+X^9+X^10+X^11+X^13+X^14+X^16 over the whole frame (skip 0),
Manchester (bit 1 = high chip then low), a low chip is no carrier. Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 2400000
CHIP_RATE = 32768.0
OFFSET_HZ = 50000.0              # 48 samples a cycle at 2.4 MS/s: a table, not a sin() per sample
AMPLITUDE = 0.5
START_SEC = 0.010
SECONDS = 0.200
GEN_EXP = (16, 14, 13, 11, 10, 9, 8, 6, 5, 1, 0)


def put_bits(bits, start, length, v):
    for i in range(length):
        bits[start + i] = (v >> (length - 1 - i)) & 1


def bch_check_bits(info):
    g = [0] * 17
    for e in GEN_EXP:
        g[16 - e] = 1
    c = list(info) + [0] * 16
    for i in range(len(info)):
        if c[i]:
            for j in range(17):
                c[i + j] ^= g[j]
    return c[len(info):]


def build_scm(ident, typ, consumption, phys, enc):
    bits = [0] * 96
    put_bits(bits, 0, 1, 1)
    put_bits(bits, 1, 20, 0xF2A60)
    put_bits(bits, 21, 2, (ident >> 24) & 3)
    put_bits(bits, 23, 1, 0)
    put_bits(bits, 24, 2, phys & 3)
    put_bits(bits, 26, 4, typ & 15)
    put_bits(bits, 30, 2, enc & 3)
    put_bits(bits, 32, 24, consumption & 0xFFFFFF)
    put_bits(bits, 56, 24, ident & 0xFFFFFF)
    bits[80:96] = bch_check_bits(bits[:80])
    return bits


def main():
    bits = build_scm(41234567 & 0x3FFFFFF, 5, 1234567, 1, 2)
    chips = []
    for b in bits:
        chips += [b, b ^ 1]
    n = int(RATE * SECONDS)
    chip_len = RATE / CHIP_RATE
    start = START_SEC * RATE
    on = bytearray(n)
    for k, c in enumerate(chips):
        if c:
            for s in range(math.ceil(start + chip_len * k), math.ceil(start + chip_len * (k + 1))):
                if 0 <= s < n:
                    on[s] = 1
    cyc = int(round(RATE / OFFSET_HZ))
    tab = [struct.pack("<hh", int(AMPLITUDE * 32767 * math.cos(2 * math.pi * i / cyc)),
                       int(AMPLITUDE * 32767 * math.sin(2 * math.pi * i / cyc))) for i in range(cyc)]
    zero = struct.pack("<hh", 0, 0)
    out = bytearray()
    for i in range(n):
        out += tab[i % cyc] if on[i] else zero
    path = Path(__file__).with_name("ert-decoder.wav")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(out))
    print(f"wrote {path} ({n} frames, {RATE} S/s)")


if __name__ == "__main__":
    main()
