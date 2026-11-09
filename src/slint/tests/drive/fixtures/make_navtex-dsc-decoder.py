#!/usr/bin/env python3
"""navtex-dsc-decoder.wav - a NAVTEX (SITOR collective B, FEC) message for the NAVTEX & DSC decoder (docs 6.50).

"ZCZC FA01 / FOXSDR FIXTURE 1 / NNNN" (station F, subject A = navigational warning, message 01), 100 Bd, 170 Hz shift (B tone 1785 Hz,
Y tone 1615 Hz in the audio), phasing then the message in DX/RX interleave, 7-unit words, ITA2 letter/figure shifts, idle at the end.
RF I/Q at 48 kS/s (centre 516.300 MHz), upper sideband: an audio tone of f Hz is a carrier at VFO + f. THE RECEIVER'S TUNE: the engine puts the
VFO at centre + rate/2 - bandwidth/2 (24000 - 4000 = +20 kHz for the 8 kHz channel the script sets; the script asserts tunedHz), so the file is
exp(j * phase(t)) of the continuous-phase FSK tone, phase advancing at VFO_HZ + 1785 / 1615 Hz. Mode USB, bandwidth 8000 (a 3 kHz channel
would put the tones past the file's Nyquist, because the VFO sits at the top of the span).
Mirrors foxsdr-plugins-dev/src/navtex/test_navtex.cpp (kTyped, word7, encodeText, navtexMessageText, navtexDx, navtexSlots, render).

    python -I make_navtex-dsc-decoder.py    writes navtex-dsc-decoder.wav beside this script. Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
BAUD = 100.0
B_HZ, Y_HZ = 1785.0, 1615.0
AMP = 0.5
VFO_HZ = 20000.0
LEAD_S, TAIL_S = 0.3, 1.0
FIRST_PHASING, LATER_PHASING, IDLE = 21, 14, 6

# [M625-4] Table 1 combinations 1-32: (7-unit signal, letter, figure); B = 0, Y = 1.
TYPED = [
    ("BBBYYYB", "A", "-"), ("YBYYBBB", "B", "?"), ("BYBBBYY", "C", ":"), ("BBYYBYB", "D", None),
    ("YBBYBYB", "E", "3"), ("BBYBBYY", "F", None), ("BYBYBBY", "G", None), ("BYYBYBB", "H", None),
    ("BYBBYYB", "I", "8"), ("BBBYBYY", "J", None), ("YBBBBYY", "K", "("), ("BYBYYBB", "L", ")"),
    ("BYYBBBY", "M", "."), ("BYYBBYB", "N", ","), ("BYYYBBB", "O", "9"), ("BYBBYBY", "P", "0"),
    ("YBBBYBY", "Q", "1"), ("BYBYBYB", "R", "4"), ("BBYBYYB", "S", "'"), ("YYBYBBB", "T", "5"),
    ("YBBBYYB", "U", "7"), ("YYBBBBY", "V", "="), ("BBBYYBY", "W", "2"), ("YBYBBBY", "X", "/"),
    ("BBYBYBY", "Y", "6"), ("BBYYYBB", "Z", "+"), ("YYYBBBB", "\r", "\r"), ("YYBBYBB", "\n", "\n"),
    ("YBYBBYB", None, None), ("YBBYBBY", None, None), ("YYBBBYB", " ", " "), ("YBYBYBB", None, None),
]
PHASING1, PHASING2 = "BBBBYYY", "YBBYYBB"


def word7(s):
    w = 0
    for ch in s:
        w = (w << 1) | (1 if ch == "Y" else 0)
    return w


def encode_text(s, redundant_shifts=True):
    out = []
    figures = False
    ltrs, figs = word7(TYPED[28][0]), word7(TYPED[29][0])
    for c in s:
        if c in (" ", "\r", "\n"):
            idx = 30 if c == " " else (26 if c == "\r" else 27)
            out.append(word7(TYPED[idx][0]))
            if redundant_shifts and c == " ":
                out.append(figs if figures else ltrs)
            continue
        found, need_fig = -1, False
        for k in range(26):
            if TYPED[k][1] == c:
                found, need_fig = k, False
                break
        if found < 0:
            for k in range(26):
                if TYPED[k][2] == c:
                    found, need_fig = k, True
                    break
        if found < 0:
            raise SystemExit("no code for %r" % c)
        if need_fig != figures:
            out.append(figs if need_fig else ltrs)
            figures = need_fig
        out.append(word7(TYPED[found][0]))
    return out


def message_text(b1, b2, num, body):
    return "ZCZC " + b1 + b2 + num + "\r\n" + body + "\r\nNNNN\r\n\n"


def navtex_dx(messages):
    dx = []
    p2, alpha = word7(PHASING2), word7(PHASING1)
    for m, text in enumerate(messages):
        dx += [p2] * (FIRST_PHASING if m == 0 else LATER_PHASING)
        dx += encode_text(text)
    dx += [alpha] * IDLE
    return dx


def navtex_slots(dx):
    p1, p2 = word7(PHASING1), word7(PHASING2)
    s = [0] * (2 * len(dx))
    for j, d in enumerate(dx):
        s[2 * j] = d
        r = p1
        if j >= 2:
            r = p1 if dx[j - 2] == p2 else dx[j - 2]
        s[2 * j + 1] = r
    return s


def slot_bits(slots):
    bits = []
    for w in slots:
        for k in range(6, -1, -1):
            bits.append((w >> k) & 1)
    return bits


def main():
    out = Path(__file__).with_name("navtex-dsc-decoder.wav")
    msg = message_text("F", "A", "01", "FOXSDR FIXTURE 1")
    bits = slot_bits(navtex_slots(navtex_dx([msg])))
    spb = RATE / BAUD
    body = int(math.ceil(len(bits) * spb))
    lead, tail = int(LEAD_S * RATE), int(TAIL_S * RATE)
    n = lead + body + tail
    raw = bytearray()
    ph = 0.0
    for i in range(n):
        if lead <= i < lead + body:
            k = min(int((i - lead) / spb), len(bits) - 1)
            f = VFO_HZ + (Y_HZ if bits[k] else B_HZ)
            ph += 2.0 * math.pi * f / RATE
            if ph > 2.0 * math.pi:
                ph -= 2.0 * math.pi
            raw += struct.pack("<hh", int(AMP * 32767 * math.cos(ph)), int(AMP * 32767 * math.sin(ph)))
        else:
            raw += struct.pack("<hh", 0, 0)
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(raw))
    print(f"wrote {out} ({n} frames, {RATE} S/s, {n / RATE:.1f} s)")


if __name__ == "__main__":
    main()
