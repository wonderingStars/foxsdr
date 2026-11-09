#!/usr/bin/env python3
"""acars-decoder.wav - an ACARS uplink block for the ACARS decoder (docs 6.50).

Registration G-FOXS, label AA, block id T (an uplink: no sequence number or flight), text "FOXSDR FIXTURE 1", sent as 2400 bps
MSK (1200 / 2400 Hz tones, differential, continuous phase) on an AM carrier, three bursts in 4 s. The file is the RF I/Q at 48 kS/s (centre 131.550 MHz) so the AM demodulator's audio is the 48 kHz
the plugin asks for. THE RECEIVER'S TUNE: the engine puts the VFO at centre + rate/2 - bandwidth/2 (here 24000 - 4000 = +20 kHz for the 8 kHz
channel the script sets; the script asserts tunedHz), so the carrier is at CARRIER_HZ = +20 kHz: I + jQ = 0.5 * (1 + 0.8 * msk) * exp(j 2 pi 20000 t).
Mirrors foxsdr-plugins-dev/src/acars/test_acars.cpp (makeCodeword, makeFrame, synth, CRC-16/KERMIT, odd parity).

    python -I make_acars-decoder.py    writes acars-decoder.wav beside this script. Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
SECONDS = 4.0
CARRIER_HZ = 20000.0
MODE, ADDRESS, TAK, LABEL, BLOCK_ID = "m", ".G-FOXS", 0x15, "AA", "T"
TEXT = "FOXSDR FIXTURE 1"
STX, ETX, DEL = 0x02, 0x03, 0x7F


def odd_parity(b):
    return bin(b).count("1") & 1 == 1


def with_parity(c):
    v = c & 0x7F
    return v if odd_parity(v) else v | 0x80


def crc16_kermit(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c


def make_codeword():
    b = [with_parity(ord(MODE))]
    for i in range(7):
        b.append(with_parity(ord(ADDRESS[i]) if i < len(ADDRESS) else 32))
    b.append(with_parity(TAK))
    b += [with_parity(ord(LABEL[0])), with_parity(ord(LABEL[1])), with_parity(ord(BLOCK_ID))]
    b.append(with_parity(STX))
    b += [with_parity(ord(c)) for c in TEXT]
    b.append(with_parity(ETX))
    crc = crc16_kermit(b)
    b += [crc & 0xFF, (crc >> 8) & 0xFF]
    return b


def make_frame(cw):
    f = [0xFF] * 16 + [0xAB, 0x2A, 0x16, 0x16, 0x01] + cw + [with_parity(DEL)]
    return f


def msk(frame):
    """Continuous-phase MSK samples: every byte LSB first, a bit that differs from the one before is 1200 Hz, else 2400 Hz."""
    out = []
    phase = 0.0
    prev = 1
    n = RATE // 2400
    for byte in frame:
        for b in range(8):
            bit = (byte >> b) & 1
            f = 1200.0 if bit ^ prev else 2400.0
            prev = bit
            step = 2.0 * math.pi * f / RATE
            for _ in range(n):
                out.append(math.sin(phase))
                phase += step
                if phase > 2.0 * math.pi:
                    phase -= 2.0 * math.pi
    return out


def main():
    out = Path(__file__).with_name("acars-decoder.wav")
    n = int(RATE * SECONDS)
    audio = [0.0] * n
    burst = msk(make_frame(make_codeword()))
    for rep in range(3):
        start = int((0.1 + rep * 1.2) * RATE)
        for k, v in enumerate(burst):
            if start + k < n:
                audio[start + k] = v
    raw = bytearray()
    for k, v in enumerate(audio):
        e = 0.5 * (1.0 + 0.8 * v) * 32767
        ph = 2.0 * math.pi * CARRIER_HZ * k / RATE
        raw += struct.pack("<hh", int(e * math.cos(ph)), int(e * math.sin(ph)))
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(raw))
    print(f"wrote {out} ({n} frames, {RATE} S/s, burst {len(burst) / RATE:.3f} s)")


if __name__ == "__main__":
    main()
