#!/usr/bin/env python3
"""weather-decoder.wav - 433.92 MHz Oregon Scientific fixture (docs 6.50): complex baseband at 250 kS/s (the plugin's preferred rate, centre 433.92 MHz,
receiver mode RAW), OOK bursts on a carrier 25 kHz off the centre under faint noise and a tuner-style DC leak:
  1. a version 3.0 THGR810 message, id F824, channel 2, rolling code 6B, 21.7 C, 55 %RH, battery ok (test_weather.cpp's round-trip message);
  2. the protocol description's own worked example 1 as a version 2.1 transmission, sent twice 10.9 ms apart as the sensors do
     (THGR122NX id 1D20, channel 3, rolling code 85, -8.4 C, 28 %RH, battery low).
Six seconds of noise lead the first burst so the decoder, started by hand after the window settles, is already running (the engine loops the file).

    python -I make_weather-decoder.py      writes weather-decoder.wav beside this script

Standard library only. Deterministic (fixed seed). Framing, Manchester coding, pulse shortening and checksum follow test_weather.cpp's modulator."""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 250000
BIT_RATE = 1024.0
AMP = 0.30
SIGMA = 0.02
DC = (0.10, -0.06)
OFFSET_HZ = 25000.0
LEAD_S = 6.0
GAP_S = 1.6


def msg_temp_hum(id_, ch, rolling, flags, c, hum):
    n = [0] * 17
    n[0:4] = [(id_ >> 12) & 15, (id_ >> 8) & 15, (id_ >> 4) & 15, id_ & 15]
    n[4] = ch
    n[5] = (rolling >> 4) & 15
    n[6] = rolling & 15
    n[7] = flags
    tenths = int(math.floor(abs(c) * 10.0 + 0.5))
    n[8], n[9], n[10] = tenths % 10, (tenths // 10) % 10, (tenths // 100) % 10
    n[11] = 8 if c < 0 else 0
    n[12], n[13], n[14] = hum % 10, (hum // 10) % 10, 0
    s = sum(n[:15]) & 0xFF
    n[15], n[16] = s & 15, (s >> 4) & 15
    return n


def from_hex(h):
    return [int(ch, 16) for ch in h]


def transmitted_bits(nibbles, v30):
    src = [1] * (24 if v30 else 16) + [0, 1, 0, 1]
    for nib in nibbles:
        src += [(nib >> i) & 1 for i in range(4)]
    src += [0] * 8
    if v30:
        return src
    out = []
    for b in src:  # version 2.1: every bit twice, the inverted copy first
        out += [0 if b else 1, b]
    return out


def runs_of(bits, shorten_us):
    half = 1.0 / (2.0 * BIT_RATE)
    chips = []
    for b in bits:  # Manchester, normal polarity: the bit value is the state before the mid-clock transition
        chips += [b, 0 if b else 1]
    runs, i = [], 0
    while i < len(chips):
        if not chips[i]:
            i += 1
            continue
        j = i
        while j < len(chips) and chips[j]:
            j += 1
        t0, t1 = i * half, j * half - shorten_us * 1e-6
        if t1 > t0:
            runs.append((t0, t1))
        i = j
    return runs


def main() -> None:
    out = Path(__file__).with_name("weather-decoder.wav")
    rnd = random.Random(20260908)
    # a list of (start second, runs, duration) bursts
    m1 = msg_temp_hum(0xF824, 2, 0x6B, 1, 21.7, 55)
    r1 = runs_of(transmitted_bits(m1, True), 138.0)
    m2 = from_hex("1D20485C480882835")
    r2 = runs_of(transmitted_bits(m2, False), 93.0)
    bursts = []
    t = LEAD_S
    d1 = r1[-1][1] + 0.002
    bursts.append((t, r1, d1))
    t += d1 + GAP_S
    d2 = r2[-1][1] + 0.002
    bursts.append((t, r2, d2))
    t += d2 + 0.0109
    bursts.append((t, r2, d2))
    total_s = t + d2 + GAP_S
    n = int(total_s * RATE)
    amp = [0.0] * n
    for start, runs, _ in bursts:
        s0 = int(start * RATE)
        for t0, t1 in runs:
            a = s0 + t0 * RATE
            b = s0 + t1 * RATE
            ia, ib = int(math.ceil(a)), int(math.floor(b))
            for k in range(ia, ib):
                amp[k] = AMP
            if ia > a and ia - 1 < n:  # fractional edges, so a pulse is not quantised to whole samples
                amp[ia - 1] = max(amp[ia - 1], AMP * (ia - a))
            if b > ib and ib < n:
                amp[ib] = max(amp[ib], AMP * (b - ib))
    frames = bytearray()
    pack = struct.Struct("<hh").pack
    w = 2.0 * math.pi * OFFSET_HZ / RATE
    for k in range(n):
        a = amp[k]
        ph = 0.9 + w * k
        i = a * math.cos(ph) + DC[0] + SIGMA * rnd.gauss(0, 1)
        q = a * math.sin(ph) + DC[1] + SIGMA * rnd.gauss(0, 1)
        frames += pack(max(-32768, min(32767, int(32767 * i))), max(-32768, min(32767, int(32767 * q))))
    with wave.open(str(out), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(frames))
    print(f"wrote {out} ({n} frames, {n / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
