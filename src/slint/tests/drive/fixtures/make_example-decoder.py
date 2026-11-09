#!/usr/bin/env python3
"""example-decoder.wav - a steady note for the Example RMS Reporter (docs 6.50): 192 kS/s I/Q PCM16 stereo, 4 s, one carrier at +93.5 kHz.

The receiver's VFO starts 94.5 kHz above the file's centre (the settings profile's 300 kHz VFO offset, clamped to the span's edge less the
half of the 3 kHz passband: the same clamp that put the VFO at +22.5 kHz in a 48 kS/s file), so a carrier at +93.5 kHz is a 1 kHz note in
LSB (audio = VFO - carrier), steady, with no pointer step needed to move the VFO. A whole number of cycles per table: the loop has no seam.

    python -I make_example-decoder.py      writes example-decoder.wav beside this script

Standard library only. Deterministic."""
import array
import math
import wave
from pathlib import Path

RATE = 192000
CARRIER_HZ = 93500.0
AMP = 0.5
SECONDS = 4


def main() -> None:
    out = Path(__file__).with_name("example-decoder.wav")
    # 93.5 kHz at 192 kS/s repeats every 384 samples (93500 * 384 / 192000 = 187 cycles): one table, repeated.
    period = 384
    one = array.array("h")
    for k in range(period):
        ph = 2.0 * math.pi * CARRIER_HZ * k / RATE
        one.append(int(AMP * 32767 * math.cos(ph)))
        one.append(int(AMP * 32767 * math.sin(ph)))
    n = RATE * SECONDS
    data = one.tobytes() * (n // period)
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)
    print(f"wrote {out} ({n} frames, {RATE} S/s)")


if __name__ == "__main__":
    main()
