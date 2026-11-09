#!/usr/bin/env python3
"""epirb-decoder.wav - a 406 MHz distress-beacon burst for the EPIRB decoder (docs 6.50).

A long (144-bit) Standard Location Protocol burst: country 232 (United Kingdom), EPIRB Serial Location protocol, type approval 123,
serial 4567, position 53.55556 N 1.71444 W, on COSPAS-SARSAT channel B (406.025 MHz = the file's centre). 1.2 s of noise, 160 ms of
unmodulated carrier, 144 bits of biphase-L at 400 bps with +-1.1 rad phase modulation (150-250 us edges), BCH-1 and BCH-2, then a tail;
the file is 2.02 s and loops. 250 kS/s I/Q PCM16 (the plugin's preferred rate), light seeded noise. The decoder is an I/Q decoder: it
sees the file before the receiver's channel, so the receiver's tune does not matter.
Mirrors foxsdr-plugins-dev/src/epirb/test_epirb.cpp (setField, appendBch, standardLocationMessage, synth).

    python -I make_epirb-decoder.py    writes epirb-decoder.wav beside this script. Standard library only."""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 250000
LEAD_S, CARRIER_S, TAIL_S = 1.2, 0.160, 0.30
NBITS = 144
NOISE = 0.05
PHASE_PEAK = 1.1
PHI_C = 0.7


def set_field(b, first, last, value):
    n = last - first + 1
    for i in range(n):
        b[first - 1 + i] = (value >> (n - 1 - i)) & 1


def remainder(bits, n, gen, par):
    top = 1 << par
    reg = 0
    for i in range(n):
        reg = (reg << 1) | (bits[i] & 1)
        if reg & top:
            reg ^= gen
    return reg & (top - 1)


def append_bch(b):
    g1 = int("1001101101100111100011", 2)
    blk = [b[25 - 1 + i] for i in range(61)] + [0] * 21
    set_field(b, 86, 106, remainder(blk, 82, g1, 21))
    g2 = int("1010100111001", 2)
    blk2 = [b[107 - 1 + i] for i in range(26)] + [0] * 12
    set_field(b, 133, 144, remainder(blk2, 38, g2, 12))


def standard_location_message():
    b = [0] * 144
    for i in range(15):
        b[i] = 1
    for i, c in enumerate("000101111"):
        b[16 - 1 + i] = int(c)
    set_field(b, 25, 25, 1)
    set_field(b, 26, 26, 0)
    set_field(b, 27, 36, 232)
    set_field(b, 37, 40, 0x6)
    set_field(b, 41, 50, 123)
    set_field(b, 51, 64, 4567)
    set_field(b, 65, 65, 0)
    set_field(b, 66, 74, 214)
    set_field(b, 75, 75, 1)
    set_field(b, 76, 85, 7)
    set_field(b, 107, 109, 0x6)
    set_field(b, 110, 110, 1)
    set_field(b, 111, 111, 1)
    set_field(b, 112, 112, 1)
    set_field(b, 113, 113, 1)
    set_field(b, 114, 118, 3)
    set_field(b, 119, 122, 5)
    set_field(b, 123, 123, 0)
    set_field(b, 124, 128, 2)
    set_field(b, 129, 132, 2)
    append_bch(b)
    return b


def main():
    out = Path(__file__).with_name("epirb-decoder.wav")
    msg = standard_location_message()
    bit_sec = 1.0 / 400.0
    data_sec = NBITS * bit_sec
    total = LEAD_S + CARRIER_S + data_sec + TAIL_S
    n = int(total * RATE)
    lead = int(LEAD_S * RATE)
    carr_end = lead + int(CARRIER_S * RATE)
    data_end = carr_end + int(data_sec * RATE)
    theta = [0.0] * n
    for k in range(carr_end, data_end):
        t = (k - carr_end) / RATE
        bit = int(t / bit_sec)
        if bit >= NBITS:
            break
        frac = t / bit_sec - bit
        one = msg[bit]
        first_half = frac < 0.5
        theta[k] = (1.0 if first_half else -1.0) * PHASE_PEAK if one else (-1.0 if first_half else 1.0) * PHASE_PEAK
    L = max(1, int(0.0002 * RATE))
    if L > 1:
        sm = [0.0] * n
        acc = 0.0
        for k in range(n):
            acc += theta[k]
            if k >= L:
                acc -= theta[k - L]
            sm[k] = acc / L
        theta = sm
    ramp = int(0.003 * RATE)
    rng = random.Random(406025)
    raw = bytearray()
    for k in range(n):
        env = 0.0
        if lead <= k < data_end + ramp:
            if k < lead + ramp:
                env = (k - lead) / ramp
            elif k >= data_end:
                env = 1.0 - (k - data_end) / ramp
            else:
                env = 1.0
        ph = theta[k] + PHI_C
        i = 0.5 * env * math.cos(ph) + NOISE * rng.gauss(0.0, 1.0)
        q = 0.5 * env * math.sin(ph) + NOISE * rng.gauss(0.0, 1.0)
        raw += struct.pack("<hh", max(-32768, min(32767, int(i * 32767))), max(-32768, min(32767, int(q * 32767))))
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(raw))
    print(f"wrote {out} ({n} frames, {RATE} S/s, {total:.2f} s)")


if __name__ == "__main__":
    main()
