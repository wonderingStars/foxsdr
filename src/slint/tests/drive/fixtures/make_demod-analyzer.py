#!/usr/bin/env python3
"""demod-analyzer.wav - a QPSK carrier for the Demod Analyzer (docs 6.50): the plugin works out BPSK / QPSK / 8PSK / 16-QAM and measures it, so
the fixture is a digitally modulated signal, not a tone (a tone would only give "no digital modulation recognised").

480 kS/s I/Q PCM16 stereo, 0.8 s (38,400 symbols, looped by the engine): QPSK at 48 kBd (10 samples a symbol), root-raised-cosine 0.35, random
bits from a fixed seed, centred on +190 kHz from the file's centre = where the receiver's VFO starts (the settings profile's 300 kHz VFO offset
clamped to the span's edge less half the passband: 240 kHz - 100 kHz / 2) so the analyser's VFO channel (100 kHz wide) holds it; a faint
noise floor (sigma 0.003 per channel) under a signal of about 0.35 rms.
The carrier's period (19/48 cycles a sample) divides 48 samples, so the shift is one 48-entry table.

    python -I make_demod-analyzer.py      writes demod-analyzer.wav beside this script

Standard library only. Deterministic (seed 7)."""
import array
import math
import random
import wave
from pathlib import Path

RATE = 480000
BAUD = 48000
SPS = RATE // BAUD          # 10
ROLLOFF = 0.35
SPAN = 8                    # symbols each side of the peak: 17 symbols of filter
SYMBOLS = 38400
CARRIER_HZ = 190000.0
AMP = 0.5                   # peak of one rail before the carrier
SIGMA = 0.003


def rrc(n_taps_each_side: int) -> list:
    """Root-raised-cosine impulse response at SPS samples a symbol, unit symbol period, normalised to unit energy per symbol."""
    beta = ROLLOFF
    out = []
    for i in range(-n_taps_each_side, n_taps_each_side + 1):
        t = i / SPS
        if i == 0:
            v = 1.0 - beta + 4.0 * beta / math.pi
        elif abs(abs(4.0 * beta * t) - 1.0) < 1e-9:
            v = (beta / math.sqrt(2.0)) * ((1.0 + 2.0 / math.pi) * math.sin(math.pi / (4.0 * beta)) +
                                           (1.0 - 2.0 / math.pi) * math.cos(math.pi / (4.0 * beta)))
        else:
            v = (math.sin(math.pi * t * (1.0 - beta)) + 4.0 * beta * t * math.cos(math.pi * t * (1.0 + beta))) / \
                (math.pi * t * (1.0 - (4.0 * beta * t) ** 2))
        out.append(v)
    energy = sum(x * x for x in out) / SPS
    return [x / math.sqrt(energy) for x in out]


def main() -> None:
    out = Path(__file__).with_name("demod-analyzer.wav")
    rnd = random.Random(7)
    taps = rrc(SPAN * SPS)
    half = SPAN * SPS
    n = SYMBOLS * SPS
    ii = [0.0] * (n + 2 * half)
    qq = [0.0] * (n + 2 * half)
    scale = 0.3
    for k in range(SYMBOLS):
        a = scale if rnd.getrandbits(1) else -scale
        b = scale if rnd.getrandbits(1) else -scale
        base = k * SPS
        for j, h in enumerate(taps):
            ii[base + j] += a * h
            qq[base + j] += b * h
    period = 48
    cs = [math.cos(2.0 * math.pi * CARRIER_HZ * m / RATE) for m in range(period)]
    sn = [math.sin(2.0 * math.pi * CARRIER_HZ * m / RATE) for m in range(period)]
    data = array.array("h")
    peak = 0.0
    for m in range(n):
        bi = ii[m + half]
        bq = qq[m + half]
        c = cs[m % period]
        s = sn[m % period]
        re = bi * c - bq * s + rnd.gauss(0, SIGMA)
        im = bi * s + bq * c + rnd.gauss(0, SIGMA)
        peak = max(peak, abs(re), abs(im))
        data.append(int(32767 * max(-0.98, min(0.98, re))))
        data.append(int(32767 * max(-0.98, min(0.98, im))))
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())
    print(f"wrote {out} ({n} frames, {RATE} S/s, peak {peak:.2f})")


if __name__ == "__main__":
    main()
