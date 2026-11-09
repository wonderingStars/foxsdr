#!/usr/bin/env python3
"""adsb-decoder.wav - Mode S DF17 extended squitter for the ADS-B decoder (docs 6.50).

ICAO 0x4D2025: an aircraft identification (TC 4, callsign FOXSDR1) and an even/odd airborne position pair
(TC 11, 35000 ft, near 51.5 N 0.1 W), repeated every 0.5 s for 4 s. 2.4 MS/s, centre 1090 MHz, PCM16 (I, Q).
The encoder mirrors foxsdr-plugins-dev/src/adsb/test_adsb.cpp (buildIdent, buildPosition, cprEncode, modulateAtRate with the
box-aperture sample model, bursts off the sample grid): PPM at 1 Mbit/s, 8 us preamble, CRC-24 (poly 0xFFF409).

    python -I make_adsb-decoder.py     writes adsb-decoder.wav beside this script. Standard library only."""
import math
import struct
import wave
from pathlib import Path

RATE = 2400000
SECONDS = 4.0
ICAO = 0x4D2025
CALLSIGN = "FOXSDR1"
LAT, LON, ALT_FT = 51.5, -0.1, 35000
AMP = 0.5 * 32767
PHASE = 0.7


def crc24(data):
    """Remainder of (data << 24) / G(x), G = 0xFFF409 (what adsb::crc24 computes over the data bytes)."""
    r = 0
    for b in data:
        r ^= b << 16
        for _ in range(8):
            r <<= 1
            if r & 0x1000000:
                r ^= 0x1FFF409
    return r & 0xFFFFFF


def set_bits(m, start, ln, val):
    for i in range(ln):
        bit = start + i
        v = (val >> (ln - 1 - i)) & 1
        mask = 0x80 >> (bit & 7)
        if v:
            m[bit >> 3] |= mask
        else:
            m[bit >> 3] &= ~mask & 0xFF


def append_parity(m):
    p = crc24(bytes(m[:11]))
    m[11], m[12], m[13] = (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF


def new_df17(icao):
    m = bytearray(14)
    set_bits(m, 0, 5, 17)
    set_bits(m, 5, 3, 5)
    set_bits(m, 8, 24, icao)
    return m


def ident_code(c):
    if "A" <= c <= "Z":
        return ord(c) - 65 + 1
    if "0" <= c <= "9":
        return ord(c) - 48 + 48
    return 32


def build_ident(icao, callsign):
    m = new_df17(icao)
    set_bits(m, 32, 5, 4)
    set_bits(m, 37, 3, 0)
    padded = callsign[:8].ljust(8)
    for i in range(8):
        set_bits(m, 40 + 6 * i, 6, ident_code(padded[i]))
    append_parity(m)
    return m


def encode_ac12(feet):
    n = (feet + 1000) // 25
    nn = n & 0x7FF
    return ((nn >> 4) << 5) | 0x10 | (nn & 0x0F)


def cpr_nl(lat):
    a = abs(lat)
    if a >= 87.0:
        return 1
    c = math.cos(math.pi / 30.0)
    cl = math.cos(a * math.pi / 180.0)
    if cl <= 0.0:
        return 1
    x = 1.0 - (1.0 - c) / (cl * cl)
    x = max(-1.0, min(1.0, x))
    denom = math.acos(x)
    if denom <= 0.0:
        return 59
    return max(1, min(59, int(math.floor(2.0 * math.pi / denom))))


def cpr_encode(lat, lon, odd):
    d17 = 131072.0
    i = 1 if odd else 0
    dlat = 360.0 / (4 * 15 - i)
    ml = math.fmod(lat, dlat)
    if ml < 0.0:
        ml += dlat
    yz = int(math.floor(d17 * ml / dlat + 0.5)) & 0x1FFFF
    rlat = dlat * (yz / d17 + math.floor(lat / dlat))
    nl = max(1, cpr_nl(rlat) - i)
    dlon = 360.0 / nl
    mo = math.fmod(lon, dlon)
    if mo < 0.0:
        mo += dlon
    xz = int(math.floor(d17 * mo / dlon + 0.5)) & 0x1FFFF
    return yz, xz


def build_position(icao, lat, lon, alt_ft, odd):
    m = new_df17(icao)
    yz, xz = cpr_encode(lat, lon, odd)
    set_bits(m, 32, 5, 11)
    set_bits(m, 37, 2, 0)
    set_bits(m, 39, 1, 0)
    set_bits(m, 40, 12, encode_ac12(alt_ft))
    set_bits(m, 52, 1, 0)
    set_bits(m, 53, 1, 1 if odd else 0)
    set_bits(m, 54, 17, yz)
    set_bits(m, 71, 17, xz)
    append_parity(m)
    return m


def slot_high(m, s):
    if s < 16:
        return s in (0, 2, 7, 9)
    k = (s - 16) // 2
    first = ((s - 16) % 2) == 0
    bit = (m[k >> 3] >> (7 - (k & 7))) & 1
    return first if bit else not first


def burst(m, frac):
    """Box-aperture samples (energy per sample period) of one frame: list of (I, Q) ints, starting 2 samples early."""
    nslots = 16 + 2 * 112
    sps = RATE / 2.0e6
    high = [slot_high(m, s) for s in range(nslots)]
    nsamp = int(math.ceil(nslots * sps)) + 2
    cp, sp = math.cos(PHASE), math.sin(PHASE)
    out = []
    for i in range(-2, nsamp):
        t0 = (i + frac) / sps
        t1 = (i + 1 + frac) / sps
        acc = 0.0
        s = int(math.floor(t0))
        while s < t1:
            if 0 <= s < nslots and high[s]:
                acc += min(t1, s + 1) - max(t0, s)
            s += 1
        a = AMP * acc / (t1 - t0)
        out.append((int(a * cp), int(a * sp)))
    return out


def main():
    out = Path(__file__).with_name("adsb-decoder.wav")
    n = int(RATE * SECONDS)
    buf = bytearray(4 * n)
    ident = build_ident(ICAO, CALLSIGN)
    even = build_position(ICAO, LAT, LON, ALT_FT, False)
    odd = build_position(ICAO, LAT, LON, ALT_FT, True)
    assert crc24(bytes(ident)) == 0 and crc24(bytes(even)) == 0 and crc24(bytes(odd)) == 0
    frames = 0
    for rep in range(8):
        t = 0.10 + 0.5 * rep
        start = int(t * RATE)
        for k, (m, off) in enumerate(((ident, 0), (even, 3000), (odd, 6000))):
            frac = (0.3 * (rep + k)) % 1.0
            pos = start + off * 2
            for j, (i_, q_) in enumerate(burst(m, frac)):
                p = (pos + j) * 4
                if 0 <= p < len(buf) - 3:
                    struct.pack_into("<hh", buf, p, i_, q_)
            frames += 1
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(buf))
    print(f"wrote {out} ({n} frames, {RATE} S/s, {frames} Mode S frames)")


if __name__ == "__main__":
    main()
