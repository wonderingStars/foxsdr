#!/usr/bin/env python3
"""aprs-decoder.wav - two APRS packets from FOXSDR-1 as an NFM signal (docs 6.50): AFSK 1200 baud Bell 202 (1200 / 2200 Hz),
NRZI, HDLC bit stuffing, CRC-16/X.25, +/-3 kHz deviation. Receiver: centre 144.800 MHz (carrier at centre + 17.75 kHz, where the receiver's VFO sits), mode NFM, bandwidth 12500.
  1. FOXSDR-1>APRS  status    ">FOXSDR FIXTURE 1"
  2. FOXSDR-1>APRS  position  "!5130.44N/00007.67W>FOXSDR FIXTURE 1"   (puts a station on the map: the track source)

    python -I make_aprs-decoder.py         writes aprs-decoder.wav beside this script

Mirrors foxsdr-plugins-dev/src/aprs/test_aprs.cpp (fcs_x25, build_frame, BitStream, modulate). Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
DEV_HZ = 3000.0
# The receiver is tuned centre + 17.75 kHz (the engine's offset tuning on a 48 kS/s source: 0.37 of the rate), so the carrier sits there.
OFFSET_HZ = 17750.0
SECONDS = 6.0


def rev(v, n):
    return int(format(v, "0%db" % n)[::-1], 2)


def fcs_x25(data):
    crc = 0xFFFF
    for b in data:
        crc ^= rev(b, 8) << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return rev(crc, 16) ^ 0xFFFF


def address(call, flags, last):
    base, _, ssid = call.partition("-")
    out = [ord(base[i] if i < len(base) else " ") << 1 for i in range(6)]
    b = flags | ((int(ssid or 0) & 0xF) << 1) | (1 if last else 0)
    return out + [b]


def build_frame(src, dst, path, info):
    f = address(dst, 0xE0, False) + address(src, 0x60, not path)
    for i, p in enumerate(path):
        f += address(p, 0x60, i + 1 == len(path))
    f += [0x03, 0xF0] + [ord(c) for c in info]
    crc = fcs_x25(f)
    return f + [crc & 0xFF, crc >> 8]


class Bits:
    def __init__(self):
        self.bits = []
        self.ones = 0

    def flag(self):
        self.bits += [(0x7E >> i) & 1 for i in range(8)]
        self.ones = 0

    def data(self, frame):
        for byte in frame:
            for i in range(8):
                bit = (byte >> i) & 1
                self.bits.append(bit)
                if bit:
                    self.ones += 1
                    if self.ones == 5:
                        self.bits.append(0)
                        self.ones = 0
                else:
                    self.ones = 0


def main():
    frames = [build_frame("FOXSDR-1", "APRS", ["WIDE1-1"], ">FOXSDR FIXTURE 1"),
              build_frame("FOXSDR-1", "APRS", ["WIDE1-1"], "!5130.44N/00007.67W>FOXSDR FIXTURE 1")]
    bs = Bits()
    for _ in range(40):
        bs.flag()
    for f in frames:
        bs.data(f)
        bs.flag()
        for _ in range(12):
            bs.flag()
    for _ in range(3):
        bs.flag()
    spb = RATE / 1200.0
    n = int(RATE * SECONDS)
    lead = int(0.3 * RATE)
    body = int(len(bs.bits) * spb)
    mark = True
    aphase = 0.0
    phase = 0.0
    kdev = 2 * math.pi * DEV_HZ / RATE
    out = bytearray()
    for i in range(n):
        s = 0.0
        if lead <= i < lead + body:
            k = int((i - lead) / spb)
            if k != getattr(main, "_k", -1):
                main._k = k
                if bs.bits[k] == 0:          # NRZI: a zero is a transition
                    mark = not mark
            f = 1200.0 if mark else 2200.0
            aphase = (aphase + 2 * math.pi * f / RATE) % (2 * math.pi)
            s = math.sin(aphase)
        phase += kdev * s
        tp = phase + 2*math.pi*OFFSET_HZ*i/RATE
        out += struct.pack("<hh", int(0.5 * 32767 * math.cos(tp)), int(0.5 * 32767 * math.sin(tp)))
    path = Path(__file__).with_name("aprs-decoder.wav")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(out))
    print(f"wrote {path} ({n} frames, {RATE} S/s, {len(bs.bits)} bits)")


if __name__ == "__main__":
    main()
