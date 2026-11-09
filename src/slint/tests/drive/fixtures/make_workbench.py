#!/usr/bin/env python3
"""workbench.wav - one NOAA weather channel on the air, a 1 kHz FM note (docs 6.50): the fixture of the Decoder Workbench.

240 kS/s I/Q PCM16 stereo, 4 s. The workbench's own NOAA weather preset retunes the receiver to 162.388 MHz (centre of the band at this
rate: seen in a driven run) and the file holds a carrier 62 kHz above the centre = 162.450 MHz = channel WX-3, frequency-modulated by a 1 kHz
note at 3 kHz deviation, amplitude 0.3. The panel then shows WX-3 with the note's level and the other in-range channels with the level of
a discriminator fed nothing. There is no SAME message in it, so no channel's message count moves (a tone: the panel and the routing line
are the proof). The carrier (31/120 cycles a sample) and the note (period 240 samples) both fit a 240-sample table, repeated with no seam.

    python -I make_workbench.py       writes workbench.wav beside this script

Standard library only. Deterministic."""
import array
import math
import wave
from pathlib import Path

RATE = 240000
OFFSET_HZ = 62000.0
NOTE_HZ = 1000.0
DEVIATION_HZ = 3000.0
AMP = 0.3
SECONDS = 4


def main() -> None:
    out = Path(__file__).with_name("workbench.wav")
    period = 240  # one note
    beta = DEVIATION_HZ / NOTE_HZ
    one = array.array("h")
    for k in range(period):
        ph = 2.0 * math.pi * (OFFSET_HZ * k / RATE) + beta * math.sin(2.0 * math.pi * NOTE_HZ * k / RATE)
        one.append(int(AMP * 32767 * math.cos(ph)))
        one.append(int(AMP * 32767 * math.sin(ph)))
    n = RATE * SECONDS
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(one.tobytes() * (n // period))
    print(f"wrote {out} ({n} frames, {RATE} S/s)")


if __name__ == "__main__":
    main()
