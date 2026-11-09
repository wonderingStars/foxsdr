#!/usr/bin/env python3
"""cw-decoder.wav - Morse fixture (docs 6.50): "CQ CQ DE FOXSDR" at 20 WPM keyed on a carrier 30 Hz above the VFO (the plugin START preset tunes CW 14.060 MHz, the VFO sits 22.5 kHz above the file's centre; CW mode adds the 700 Hz note),
48 kS/s I/Q PCM16, the message sent twice with a word gap between and a pause at the end (the engine loops the file).

    python -I make_cw-decoder.py      writes cw-decoder.wav beside this script

Standard library only. Deterministic (fixed seed for the faint noise floor)."""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 48000
VFO_OFFSET_HZ = 22500.0  # the receiver keeps the VFO 22.5 kHz above the source centre on a 48 kS/s file (centreHz 14037500, tunedHz 14060000 after the START preset)
NOTE_HZ = VFO_OFFSET_HZ + 30.0  # CW mode (the start preset: CW) puts a carrier 30 Hz off the VFO at 700 + 30 Hz; exactly at the VFO would be 700 Hz but a DC blocker is in the chain
WPM = 20
MESSAGE = "CQ CQ DE FOXSDR"
MORSE = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.", "H": "....", "I": "..", "J": ".---",
    "K": "-.-", "L": ".-..", "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.", "S": "...", "T": "-",
    "U": "..-", "V": "...-", "W": ".--", "X": "-..-", "Y": "-.--", "Z": "--..",
}


def key_elements(text):
    """[(on, dots)] for the text: dot 1, dash 3, 1 between elements, 3 between letters, 7 between words."""
    out = []
    for wi, word in enumerate(text.split(" ")):
        if wi:
            out.append((False, 7))
        for li, ch in enumerate(word):
            if li:
                out.append((False, 3))
            for ei, el in enumerate(MORSE[ch]):
                if ei:
                    out.append((False, 1))
                out.append((True, 1 if el == "." else 3))
    return out


def main() -> None:
    out = Path(__file__).with_name("cw-decoder.wav")
    dot = int(RATE * 1.2 / WPM)
    env = []
    env += [0.0] * int(RATE * 0.6)
    for rep in range(2):
        if rep:
            env += [0.0] * (7 * dot)
        for on, dots in key_elements(MESSAGE):
            n = dots * dot
            ramp = int(RATE * 0.005)
            seg = [1.0] * n if on else [0.0] * n
            if on:
                for k in range(ramp):
                    w = 0.5 - 0.5 * math.cos(math.pi * k / ramp)
                    seg[k] = w
                    seg[n - 1 - k] = w
            env += seg
    env += [0.0] * int(RATE * 1.4)
    rnd = random.Random(1)
    frames = bytearray()
    w = 2.0 * math.pi * NOTE_HZ / RATE
    for k, e in enumerate(env):
        ph = w * k
        a = 0.5 * e
        i = a * math.cos(ph) + rnd.gauss(0, 0.002)
        q = a * math.sin(ph) + rnd.gauss(0, 0.002)
        frames += struct.pack("<hh", int(32767 * i), int(32767 * q))
    with wave.open(str(out), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(frames))
    print(f"wrote {out} ({len(env)} frames, {len(env) / RATE:.1f} s, {RATE} S/s)")


if __name__ == "__main__":
    main()
