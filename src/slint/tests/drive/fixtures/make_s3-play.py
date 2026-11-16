#!/usr/bin/env python3
"""s3-play.wav (and s3-raw.cu8) - a 12 s, 48 kS/s I/Q PCM16 stereo recording of one steady carrier at +5 kHz, for the recording-and-playback drive scripts (screen lane S3).

    python -I make_s3-play.py      writes s3-play.wav beside this script

The same run also writes s3-raw.cu8: a raw 8-bit unsigned I/Q file (no header), 6 s at 256 kS/s, one carrier at +20 kHz, for the drive script s3-raw.

Standard library only. Deterministic."""
import array
import math
import wave
from pathlib import Path

RATE = 48000
SECONDS = 12
CARRIER_HZ = 5000.0
AMP = 0.4


def raw() -> None:
    out = Path(__file__).with_name("s3-raw.cu8")
    rate, n = 256000, 256000 * 6
    b = bytearray(2 * n)
    for i in range(n):
        ph = 2.0 * math.pi * 20000.0 * i / rate
        b[2 * i] = int(127.5 + 100.0 * math.cos(ph))
        b[2 * i + 1] = int(127.5 + 100.0 * math.sin(ph))
    out.write_bytes(bytes(b))


def main() -> None:
    raw()
    out = Path(__file__).with_name("s3-play.wav")
    n = RATE * SECONDS
    buf = array.array("h")
    for i in range(n):
        ph = 2.0 * math.pi * CARRIER_HZ * i / RATE
        buf.append(int(AMP * 32767 * math.cos(ph)))
        buf.append(int(AMP * 32767 * math.sin(ph)))
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(buf.tobytes())


if __name__ == "__main__":
    main()
