#!/usr/bin/env python3
"""flex-decoder.wav - one FLEX page as an NFM signal (docs 6.50): 1600 bps 2-level FSK (+/-4.8 kHz), capcode 1234567 (short
address), alphanumeric message "FOXSDR FIXTURE". Receiver: centre 931.937500 MHz (the first preset) (carrier at centre + 17.75 kHz, where the
receiver's VFO sits), mode NFM, bandwidth 12500.

    python -I make_flex-decoder.py         writes flex-decoder.wav beside this script

The encoder mirrors foxsdr-plugins-dev/src/flex/test_flex.cpp (WordBuild, FrameBuild, StreamWrite, putBiw1, putShortAddress,
putAlphaPage, buildStream, modulate): bit sync 1, sync A / B / inverted A, frame information word, sync C, eleven interleaved
blocks of eight BCH(31,21) words, with the 4-bit information checksum. Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
BAUD = 1600
DEV_HZ = 4800.0
# The receiver is tuned centre + 17.75 kHz (the engine's offset tuning on a 48 kS/s source: 0.37 of the rate), so the carrier sits there.
OFFSET_HZ = 17750.0
POLY = 0x769
SYNC_A_1600x2 = 0x78F35939
SYNC_B = 0x5555
SYNC_C = 0xED84
WORDS_PER_BLOCK = 8
BLOCKS = 11
WORDS = WORDS_PER_BLOCK * BLOCKS
VEC_ALPHA = 5


def popcount(v):
    return bin(v & 0xFFFFFFFF).count("1")


def bch_remainder(w31):
    r = w31 & 0x7FFFFFFF
    for i in range(30, 9, -1):
        if r & (1 << i):
            r ^= POLY << (i - 10)
    return r & 0x3FF


def bch_encode(info21):
    info = info21 & 0x1FFFFF
    cw = (info << 11) | (bch_remainder(info << 10) << 1)
    return cw | (popcount(cw) & 1)


class Word:
    """One word, addressed by TRANSMITTED BIT NUMBER 1..21 (the spec's figures)."""

    def __init__(self):
        self.b = [0] * 22

    def put(self, pos, v, n):
        for k in range(n):
            j = pos + k
            if 1 <= j <= 21:
                self.b[j] = (v >> k) & 1

    def field(self, pos, n):
        return sum(self.b[pos + k] << k for k in range(n) if pos + k <= 21)

    def info(self):
        return sum(self.b[j] << (21 - j) for j in range(1, 22))

    def word(self):
        return bch_encode(self.info())

    def seal(self):                       # section 3.5.1: checksum over bits 5..21
        s = self.field(5, 4) + self.field(9, 4) + self.field(13, 4) + self.field(17, 4) + self.b[21]
        self.put(1, (~s) & 0xF, 4)


class Frame:
    def __init__(self):
        self.wb = [Word() for _ in range(WORDS)]
        self.used = [False] * WORDS

    def word(self, i):
        if self.used[i]:
            return self.wb[i].word()
        return 0x00000000 if (i & 1) else 0xFFFFFFFF      # idle block: alternating all-ones / all-zeros words


def put_biw1(f, addr_start, vec_start):
    w = f.wb[0]
    f.used[0] = True
    w.put(5, 0, 4)
    w.put(9, addr_start - 1, 2)
    w.put(11, vec_start, 6)
    w.put(17, 0, 2)
    w.put(19, 0, 3)
    w.seal()


def put_short_address(f, at, address):
    f.used[at] = True
    f.wb[at].put(1, address, 21)


def put_alpha_page(f, vec_at, msg_at, text):
    bits = 7 * (len(text) + 2)
    text_words = (bits + 20) // 21
    n = 1 + text_words
    v = f.wb[vec_at]
    f.used[vec_at] = True
    v.put(5, VEC_ALPHA, 3)
    v.put(8, msg_at, 7)
    v.put(15, n, 7)
    v.seal()
    f.used[msg_at] = True
    f.wb[msg_at].put(11, 0, 1)            # C0: not continued
    f.wb[msg_at].put(12, 3, 2)            # fragment 11: first fragment, signature present
    # a continuous LSB-first stream across the information bits of the following words
    state = {"word": msg_at + 1, "pos": 1}

    def put(val, count):
        for k in range(count):
            if state["word"] >= WORDS:
                return
            f.used[state["word"]] = True
            f.wb[state["word"]].b[state["pos"]] = (val >> k) & 1
            state["pos"] += 1
            if state["pos"] > 21:
                state["pos"] = 1
                state["word"] += 1

    put(0x55, 7)                          # signature
    for ch in text:
        put(ord(ch), 7)
    put(0x03, 7)                          # ETX
    while state["word"] < msg_at + n:
        f.used[state["word"]] = True
        state["word"] += 1


def msb(bits, v, n):
    bits += [(v >> i) & 1 for i in range(n - 1, -1, -1)]


def build_stream(f, cycle, frame_no, preamble=512):
    bits = [0 if i & 1 else 1 for i in range(preamble)]
    msb(bits, 0xAAAAAAAA, 32)
    msb(bits, SYNC_A_1600x2, 32)
    msb(bits, SYNC_B, 16)
    msb(bits, (~SYNC_A_1600x2) & 0xFFFFFFFF, 32)
    fi = Word()
    fi.put(5, cycle, 4)
    fi.put(9, frame_no, 7)
    fi.put(16, 0, 1)
    fi.put(17, 0, 1)
    fi.put(18, 0, 4)
    fi.seal()
    msb(bits, fi.word(), 32)
    msb(bits, 0xA, 4)
    msb(bits, SYNC_C, 16)
    msb(bits, 0x5, 4)
    msb(bits, (~SYNC_C) & 0xFFFF, 16)
    for block in range(BLOCKS):                # eleven interleaved blocks of eight words
        words = [f.word(block * WORDS_PER_BLOCK + i) for i in range(WORDS_PER_BLOCK)]
        for j in range(1, 33):
            for i in range(WORDS_PER_BLOCK):
                bits.append((words[i] >> (32 - j)) & 1)
    bits += [0 if i & 1 else 1 for i in range(256)]
    return bits


def main():
    f = Frame()
    put_biw1(f, 1, 2)                          # addresses at word 1, vectors at word 2
    put_short_address(f, 1, 1234567 + 32768)   # capcode 1234567
    put_alpha_page(f, 2, 3, "FOXSDR FIXTURE")
    bits = build_stream(f, 3, 60)
    spb = RATE / BAUD
    lead = int(0.3 * RATE)
    body = int(len(bits) * spb)
    n = lead + body + int(0.7 * RATE)
    a = 1.0 - math.exp(-2 * math.pi * 3200.0 / RATE)       # the test's 3.2 kHz transmit low-pass
    lp = 0.0
    phase = 0.0
    kdev = 2 * math.pi * DEV_HZ / RATE
    out = bytearray()
    for i in range(n):
        lvl = 0.0
        if lead <= i < lead + body:
            lvl = 1.0 if bits[int((i - lead) / spb)] else -1.0
        lp += a * (lvl - lp)
        phase += kdev * lp
        tp = phase + 2 * math.pi * OFFSET_HZ * i / RATE
        out += struct.pack("<hh", int(0.5 * 32767 * math.cos(tp)), int(0.5 * 32767 * math.sin(tp)))
    path = Path(__file__).with_name("flex-decoder.wav")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(out))
    print(f"wrote {path} ({n} frames, {RATE} S/s, {len(bits)} bits)")


if __name__ == "__main__":
    main()
