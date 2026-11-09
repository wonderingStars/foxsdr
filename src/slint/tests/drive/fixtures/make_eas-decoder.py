#!/usr/bin/env python3
"""eas-decoder.wav - EAS / SAME fixture (docs 6.50): a Required Weekly Test header, sent three times, then NNNN three times, as the plugin's own test
transmitter builds it (src/eas/test_eas.cpp, 47 CFR 11.31): 16 bytes of 0xAB, then the ASCII, least significant bit first, AFSK at 520.83 baud,
mark 2083.3 Hz / space 1562.5 Hz, continuous phase, a second of silence between copies. The header is
ZCZC-WXR-RWT-048113-048121+0030-2311832-KXYZ/NWS-

The audio frequency-modulates the carrier (NFM, peak deviation 3 kHz) on the VFO: the plugin's START preset is NFM, 16 kHz, NOAA Weather Radio 162.400 MHz,
and the receiver keeps the VFO (96000 / 2 - 16000 / 2) = 40 kHz above the file's centre. 96 kS/s I/Q PCM16, about 11 s (the engine loops the file).
Expected decode: "EAS  Required Weekly Test (RWT)" ... "raw        ZCZC-WXR-RWT-048113-048121+0030-2311832-KXYZ/NWS-".

    python -I make_eas-decoder.py     writes eas-decoder.wav beside this script

Standard library only. Deterministic."""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 96000
VFO_OFFSET_HZ = 40000.0
DEVIATION_HZ = 3000.0
MARK_HZ = 2083.3
SPACE_HZ = 1562.5
BAUD = 520.83
HEADER = "ZCZC-WXR-RWT-048113-048121+0030-2311832-KXYZ/NWS-"


def main() -> None:
    out = Path(__file__).with_name("eas-decoder.wav")
    audio = []
    phase = [0.0]
    boundary = [0.0]

    def silence(sec):
        audio.extend([0.0] * int(sec * RATE))

    def bit(mark):
        boundary[0] += RATE / BAUD
        step = 2.0 * math.pi * (MARK_HZ if mark else SPACE_HZ) / RATE
        while len(audio) < boundary[0]:
            phase[0] += step
            if phase[0] > 2.0 * math.pi:
                phase[0] -= 2.0 * math.pi
            audio.append(0.9 * math.sin(phase[0]))

    def burst(text):
        boundary[0] = float(len(audio))
        data = [0xAB] * 16 + [ord(c) for c in text]
        for b in data:
            for i in range(8):
                bit(bool((b >> i) & 1))

    silence(0.3)
    for _ in range(3):
        burst(HEADER)
        silence(1.0)
    for _ in range(3):
        burst("NNNN")
        silence(1.0)
    rnd = random.Random(11)
    frames = bytearray()
    ph = 0.0
    base = 2.0 * math.pi * VFO_OFFSET_HZ / RATE
    k_dev = 2.0 * math.pi * DEVIATION_HZ / RATE
    for a in audio:
        ph += base + k_dev * a
        i = 0.5 * math.cos(ph) + rnd.gauss(0, 0.001)
        q = 0.5 * math.sin(ph) + rnd.gauss(0, 0.001)
        frames += struct.pack("<hh", int(32767 * i), int(32767 * q))
        if ph > 1.0e6:
            ph %= 2.0 * math.pi
    with wave.open(str(out), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(frames))
    print(f"wrote {out} ({len(audio)} frames, {len(audio) / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
