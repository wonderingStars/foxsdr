#!/usr/bin/env python3
"""tones3.wav - three carriers, one after another, above a weak noise floor (docs 6.50): the fixture of Nearby Signal Catch and Survey Engine.

480 kS/s I/Q PCM16 stereo (I, Q), 13 s, looped by the engine:
    1 s silence | 1 s carrier A -150 kHz (amp 0.30) | 3 s silence | 1 s carrier B +50 kHz (amp 0.25) | 3 s silence |
    1 s carrier C +200 kHz (amp 0.20) | 3 s silence
Offsets are from the centre the script tunes (433 MHz for Nearby Signal Catch, 145 MHz for Survey Engine). One carrier at a time
because Nearby Signal Catch holds ONE hit while its signal is up and starts the next only after the 2 s hang has run out: the 3 s gaps
let each carrier be its own hit. Survey Engine averages a whole dwell, so all three show in its table.
The carriers' periods (16, 48 and 12 samples) divide the 1 s noise table, so every second of the file is built from a pre-computed
block: the file is made in a couple of seconds.

    python -I make_tones3.py          writes tones3.wav beside this script

Standard library only. Deterministic (fixed seed for the noise floor, sigma 0.002 per channel)."""
import array
import math
import random
import wave
from pathlib import Path

RATE = 480000
SIGMA = 0.002
# (offset Hz, amplitude)
CARRIERS = [(-150000.0, 0.30), (50000.0, 0.25), (200000.0, 0.20)]
# the schedule in seconds: None = silence
SCHEDULE = [None, 0, None, None, None, 1, None, None, None, 2, None, None, None]


def block(noise_i, noise_q, carrier):
    """One second of I/Q, interleaved int16, noise plus (optionally) a carrier."""
    out = array.array("h", bytes(4 * RATE))
    if carrier is None:
        for k in range(RATE):
            out[2 * k] = int(32767 * noise_i[k])
            out[2 * k + 1] = int(32767 * noise_q[k])
        return out
    off, amp = carrier
    # the carrier's period in samples: RATE / gcd(RATE, |off|)
    p = RATE // math.gcd(RATE, int(abs(off)))
    ci = [amp * math.cos(2.0 * math.pi * off * k / RATE) for k in range(p)]
    cq = [amp * math.sin(2.0 * math.pi * off * k / RATE) for k in range(p)]
    for k in range(RATE):
        out[2 * k] = int(32767 * (ci[k % p] + noise_i[k]))
        out[2 * k + 1] = int(32767 * (cq[k % p] + noise_q[k]))
    return out


def main() -> None:
    out = Path(__file__).with_name("tones3.wav")
    rnd = random.Random(3)
    noise_i = [rnd.gauss(0, SIGMA) for _ in range(RATE)]
    noise_q = [rnd.gauss(0, SIGMA) for _ in range(RATE)]
    quiet = block(noise_i, noise_q, None)
    blocks = [block(noise_i, noise_q, c) for c in CARRIERS]
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        for step in SCHEDULE:
            data = quiet if step is None else blocks[step]
            w.writeframes(data.tobytes())
    print(f"wrote {out} ({len(SCHEDULE)} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
