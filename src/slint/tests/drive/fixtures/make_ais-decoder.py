#!/usr/bin/env python3
"""ais-decoder.wav - an AIS type 1 position report for the AIS decoder (docs 6.50).

MMSI 235000001 (a UK-range MMSI), 51.5074 N 0.1278 W, 12.3 kt, course 94.2, heading 94, on AIS 1 (161.975 MHz) = -25 kHz from the
162.000 MHz centre, repeated every 0.5 s for 4 s. 192 kS/s (the plugin's requiredRateHz), PCM16 (I, Q), light seeded noise.
Mirrors foxsdr-plugins-dev/src/ais/test_ais.cpp (buildType1, bitsToBytesMsb, CRC-16/X.25, training + 0x7E flags + bit stuffing,
NRZI, BT=0.4 GMSK at 9600 baud, ramp, 192000 / 9600 = 20 samples a symbol).

    python -I make_ais-decoder.py      writes ais-decoder.wav beside this script. Standard library only."""
import math
import random
import struct
import wave
from pathlib import Path

FS = 192000
SPS = 20
SECONDS = 4.0
OFFSET_HZ = -25000.0
AMP = 0.5
SIGMA = 0.05
MMSI = 235000001


def put_u(bits, v, ln):
    for i in range(ln - 1, -1, -1):
        bits.append((v >> i) & 1)


def put_i(bits, v, ln):
    put_u(bits, v & ((1 << ln) - 1), ln)


def build_type1(mmsi, status, lat, lon, sog, cog, hdg):
    b = []
    put_u(b, 1, 6)
    put_u(b, 0, 2)
    put_u(b, mmsi, 30)
    put_u(b, status, 4)
    put_i(b, -128, 8)
    put_u(b, sog, 10)
    put_u(b, 1, 1)
    put_i(b, lon, 28)
    put_i(b, lat, 27)
    put_u(b, cog, 12)
    put_u(b, hdg, 9)
    put_u(b, 42, 6)
    put_u(b, 0, 2)
    put_u(b, 0, 3)
    put_u(b, 0, 1)
    put_u(b, 0, 19)
    return b  # 168 bits


def bits_to_bytes_msb(bits):
    out = bytearray(len(bits) // 8)
    for i, v in enumerate(bits):
        if v:
            out[i // 8] |= 1 << (7 - (i & 7))
    return bytes(out)


def crc_x25(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def wire_frame(payload):
    fcs = crc_x25(payload)
    body = payload + bytes([fcs & 0xFF, fcs >> 8])
    data = []
    for byte in body:
        for b in range(8):
            data.append((byte >> b) & 1)
    bits = [i & 1 for i in range(24)]
    bits += [(0x7E >> i) & 1 for i in range(8)]
    ones = 0
    for bit in data:
        bits.append(bit)
        if bit:
            ones += 1
            if ones == 5:
                bits.append(0)
                ones = 0
        else:
            ones = 0
    bits += [(0x7E >> i) & 1 for i in range(8)]
    bits += [i & 1 for i in range(24)]
    return bits


def nrzi(bits):
    level, out = 1, []
    for b in bits:
        if b == 0:
            level ^= 1
        out.append(level)
    return out


def gmsk_pulse():
    bt = 0.4
    sigma = math.sqrt(math.log(2.0)) / (2.0 * math.pi * bt) * SPS
    ghalf = 2 * SPS
    g = [math.exp(-(k * k) / (2.0 * sigma * sigma)) for k in range(-ghalf, ghalf + 1)]
    p = [0.0] * (len(g) + SPS - 1)
    for i, gv in enumerate(g):
        for j in range(SPS):
            p[i + j] += gv
    s = sum(p)
    return [x / s for x in p]


def modulate_into(iq, start, levels, offset_hz, amp):
    """Continuous-phase GMSK added into iq (a list of floats, I and Q interleaved) at frame `start`."""
    p = gmsk_pulse()
    nsym = len(levels)
    n = nsym * SPS + len(p)
    freq = [0.0] * n
    for k, lv in enumerate(levels):
        a = 1.0 if lv else -1.0
        base = k * SPS
        for j, pv in enumerate(p):
            freq[base + j] += a * pv
    ramp = 4 * SPS
    phase = 0.0
    for i in range(n):
        phase += 0.5 * math.pi * freq[i]
        env = amp
        if i < ramp:
            env *= 0.5 * (1.0 - math.cos(math.pi * i / ramp))
        elif i + ramp > n:
            env *= 0.5 * (1.0 - math.cos(math.pi * (n - i) / ramp))
        out = start + i
        if 2 * out + 1 >= len(iq):
            break
        t = 2.0 * math.pi * offset_hz * out / FS
        si, sq = env * math.cos(phase), env * math.sin(phase)
        iq[2 * out] += si * math.cos(t) - sq * math.sin(t)
        iq[2 * out + 1] += si * math.sin(t) + sq * math.cos(t)


def main():
    out = Path(__file__).with_name("ais-decoder.wav")
    n = int(FS * SECONDS)
    iq = [0.0] * (2 * n)
    msg = build_type1(MMSI, 0, 30904440, -76680, 123, 942, 94)
    levels = nrzi(wire_frame(bits_to_bytes_msb(msg)))
    for rep in range(7):
        modulate_into(iq, 1500 + int(rep * 0.5 * FS), levels, OFFSET_HZ, AMP)
    rng = random.Random(20261109)
    raw = bytearray()
    for v in iq:
        x = int((v + SIGMA * rng.gauss(0.0, 1.0)) * 32767)
        raw += struct.pack("<h", max(-32768, min(32767, x)))
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(bytes(raw))
    print(f"wrote {out} ({n} frames, {FS} S/s)")


if __name__ == "__main__":
    main()
