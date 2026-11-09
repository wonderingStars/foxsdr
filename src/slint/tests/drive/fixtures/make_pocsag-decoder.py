#!/usr/bin/env python3
"""pocsag-decoder.wav - a POCSAG page as an NFM signal (docs 6.50): 1200 bps, +/-4.5 kHz deviation, address 1234567,
function 3 (alphanumeric), text "FOXSDR FIXTURE". Receiver: centre 153.350 MHz (carrier at centre + 17.75 kHz, where the receiver's VFO sits), mode NFM, bandwidth 12500.

    python -I make_pocsag-decoder.py       writes pocsag-decoder.wav beside this script

The encoder mirrors foxsdr-plugins-dev/src/pocsag/test_pocsag.cpp (encodePage / render): preamble of alternating bits,
batches of a sync codeword + 16 codewords, BCH(31,21) + even parity, 7-bit LSB-first text. Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000          # I/Q sample rate (S/s)
BAUD = 1200
DEV_HZ = 4500.0
# The receiver is tuned centre + 17.75 kHz (the engine's offset tuning on a 48 kS/s source: 0.37 of the rate), so the carrier sits there.
OFFSET_HZ = 17750.0
SECONDS = 4.0
SYNC = 0x7CD215D8
IDLE = 0x7A89C197
POLY = 0x769


def popcount(v):
    return bin(v & 0xFFFFFFFF).count("1")


def bch_remainder(w31):
    r = w31 & 0x7FFFFFFF
    for i in range(30, 9, -1):
        if r & (1 << i):
            r ^= POLY << (i - 10)
    return r & 0x3FF


def bch_encode(info21):
    info = info21 & 0x1FFFFF
    cw = (info << 11) | (bch_remainder(info << 10) << 1)
    return cw | (popcount(cw) & 1)


def encode_page(ric, func, text, preamble_bits=576):
    mbits = []
    for ch in text:
        c = ord(ch) & 0x7F
        mbits += [(c >> k) & 1 for k in range(7)]          # LSB first
    while len(mbits) % 20:
        mbits.append(0)
    frame = ric & 7
    slots = []
    while len(slots) % 16 != 2 * frame:
        slots.append(IDLE)
    slots.append(bch_encode(((ric >> 3) & 0x3FFFF) << 2 | (func & 3)))
    for i in range(0, len(mbits), 20):
        d = 0
        for k in range(20):
            d = (d << 1) | mbits[i + k]
        slots.append(bch_encode((1 << 20) | d))
    while len(slots) % 16:
        slots.append(IDLE)
    bits = [1 if i % 2 == 0 else 0 for i in range(preamble_bits)]
    for b in range(len(slots) // 16):
        for w in [SYNC] + slots[b * 16:(b + 1) * 16]:
            bits += [(w >> i) & 1 for i in range(31, -1, -1)]
    return bits


def main():
    bits = encode_page(1234567, 3, "FOXSDR FIXTURE")
    n = int(RATE * SECONDS)
    lead = int(0.3 * RATE)
    spb = RATE / BAUD
    a = 1.0 - math.exp(-2 * math.pi * 1.5 * BAUD / RATE)   # the test's transmit filter
    lp = 0.0
    phase = 0.0
    k_dev = 2 * math.pi * DEV_HZ / RATE
    frames = bytearray()
    body = int(len(bits) * spb)
    for i in range(n):
        lvl = 0.0
        if lead <= i < lead + body:
            bi = int((i - lead) / spb)
            lvl = 1.0 if bits[bi] else -1.0
        lp += a * (lvl - lp)
        phase += k_dev * lp
        tp = phase + 2 * math.pi * OFFSET_HZ * i / RATE
        frames += struct.pack("<hh", int(0.5 * 32767 * math.cos(tp)), int(0.5 * 32767 * math.sin(tp)))
    out = Path(__file__).with_name("pocsag-decoder.wav")
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print(f"wrote {out} ({n} frames, {RATE} S/s, {len(bits)} bits)")


if __name__ == "__main__":
    main()
