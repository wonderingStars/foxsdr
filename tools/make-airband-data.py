"""Build FoxSDR's airport frequency table and embed it in the binary.

WHAT IT MAKES. resources/airband/airband.tsv - every airport in the world that
publishes a VHF airband frequency, with its frequencies - and
src/core/airband_assets.hpp, the same bytes as a C++ array (the loader in
core/airband_data.cpp parses them at first use). tests/test_airband_data.cpp
compares the two byte for byte, so a table rebuilt without re-running this
script fails the suite rather than shipping the old one.

WHERE THE DATA COMES FROM, both public domain:
  - OurAirports (https://ourairports.com/data/): airports.csv and
    airport-frequencies.csv, "released to the Public Domain". Worldwide, but
    thin for big airports: one row per service, so Chicago O'Hare has nine.
  - The FAA's 28-day NASR subscription, FRQ.csv from the "Frequency Data
    (FRQ)" CSV group (https://www.faa.gov/air_traffic/flight_info/aeronav/
    aero_data/NASR_Subscription/). A work of the US Government. Complete: every
    tower, ground, clearance, approach and departure frequency with its
    sector, so O'Hare has twenty-five on VHF. Where the FAA lists an airport,
    its list REPLACES OurAirports'.

WHY COMPILED IN, NOT DOWNLOADED. A lookup then works offline, sends nothing
anywhere (PRIVACY.md needs no new line), and cannot break when a third-party
URL moves. The FAA cycle is 28 days and FoxSDR releases far more often than
that, so re-running this before a release keeps it current.

THE FORMAT, UTF-8, LF, tab-separated, '#' lines are comments:
  A <ident> <iata> <faa local id> <iso country> <lat> <lon> <name>
  F <channel kHz> <type> <description>
Each F line belongs to the A line above it. <channel kHz> is the frequency AS
PUBLISHED, which on an 8.33 kHz raster is a channel NAME rather than a
frequency (118.010 is 118.00833 MHz); core::airbandChannelHz() turns it into
the frequency the radio is tuned to.

KEPT: 108.000-136.990 MHz (VHF airband; ATIS is sometimes on a VOR between
108 and 118). DROPPED: navigation aids themselves (NDB, VOR, ILS rows), UHF
military frequencies, and airports left with no frequency at all.

Run from the repository root:
    py -3.14 tools/make-airband-data.py <airports.csv> <airport-frequencies.csv> <FRQ.csv>
"""
import csv
import datetime
import io
import os
import sys

OUT_TSV = os.path.join('resources', 'airband', 'airband.tsv')
OUT_HPP = os.path.join('src', 'core', 'airband_assets.hpp')

LO_KHZ = 108000
HI_KHZ = 136990

NAV_TYPES = {'VOR', 'ILS', 'NDB', 'DME', 'VORTAC', 'TACAN', 'LOC', 'GS', 'NAV', 'VOT'}


def khz_of(text):
    """'121.6' -> 121600; None for anything that is not a frequency in MHz."""
    t = (text or '').strip()
    if not t:
        return None
    try:
        mhz = float(t)
    except ValueError:
        return None
    khz = int(round(mhz * 1000.0))
    if khz < LO_KHZ or khz > HI_KHZ:
        return None
    return khz


def clean(text):
    """One line of plain text: no tabs, no newlines, runs of spaces folded."""
    t = (text or '').replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')
    return ' '.join(t.split())


def faa_type(use, facility_type):
    """A short service name, the same vocabulary OurAirports uses."""
    u = use.upper()
    if facility_type == 'ASOS_AWOS':
        return 'AWOS'
    if facility_type == 'RCAG':
        return 'CNTR'
    if u.startswith('LCL'):
        return 'TWR'
    if u.startswith('GND'):
        return 'GND'
    if u.startswith('CD'):
        return 'CLD'
    if u.startswith('APCH') or u.startswith('APP'):
        return 'APP'
    if u.startswith('DEP') or u.endswith(' DP'):
        return 'DEP'
    if 'ATIS' in u:
        return 'ATIS'
    if u.startswith('CLASS B'):
        return 'CLASS B'
    if u.startswith('CLASS C'):
        return 'CLASS C'
    if u.startswith('EMERG'):
        return 'EMERG'
    if u.startswith('UNICOM'):
        return 'UNIC'
    if u.startswith('CTAF'):
        return 'CTAF'
    if u.startswith('PRM'):
        return 'PRM'
    if 'ADZY' in u:
        return 'ADV'
    if u.startswith('RDO') or 'RADIO' in u:
        return 'RDO'
    return clean(use)[:12] or 'MISC'


def read_csv(path):
    with open(path, newline='', encoding='utf-8-sig') as f:
        return list(csv.DictReader(f))


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    airports_csv, freqs_csv, faa_csv = argv[1], argv[2], argv[3]

    airports = {}          # OurAirports id -> row
    by_local_us = {}       # FAA location id -> OurAirports id (US and territories)
    for row in read_csv(airports_csv):
        if row.get('type') == 'closed':
            continue
        airports[row['id']] = row
        local = (row.get('local_code') or '').strip()
        if local and row.get('iso_country') in ('US', 'PR', 'GU', 'VI', 'AS', 'MP'):
            # Several OurAirports rows can share a stale local code; the one
            # with an ICAO-style ident (K..., P..., T...) is the live airport.
            prev = by_local_us.get(local)
            if prev is None or (len(row['ident']) == 4 and len(airports[prev]['ident']) != 4):
                by_local_us[local] = row['id']

    freqs = {}             # OurAirports id -> {khz: [type, [descriptions]]}

    def add(aid, khz, typ, desc):
        slot = freqs.setdefault(aid, {})
        entry = slot.get(khz)
        if entry is None:
            slot[khz] = [typ, [desc] if desc else []]
        elif desc and desc not in entry[1]:
            entry[1].append(desc)

    # --- the FAA's lists, by the airport each frequency SERVES -------------
    faa_airports = set()
    for row in read_csv(faa_csv):
        if row.get('FACILITY_TYPE') == 'NAVAID':
            continue
        if (row.get('SERVICED_SITE_TYPE') or '') not in ('AIRPORT', 'ASOS', 'AWOS', 'HELIPORT',
                                                       'SEAPLANE BASE', 'GLIDERPORT',
                                                       'BALLOONPORT', 'ULTRALIGHT', ''):
            continue
        khz = khz_of(row.get('FREQ'))
        if khz is None:
            continue
        aid = by_local_us.get((row.get('SERVICED_FACILITY') or '').strip())
        if aid is None:
            continue
        use = clean(row.get('FREQ_USE'))
        sector = clean(row.get('SECTORIZATION'))
        desc = clean((sector + ' ' + use) if sector else use)
        add(aid, khz, faa_type(use, row.get('FACILITY_TYPE') or ''), desc)
        faa_airports.add(aid)

    # --- OurAirports for everywhere the FAA does not cover ------------------
    for row in read_csv(freqs_csv):
        aid = row.get('airport_ref')
        if aid not in airports or aid in faa_airports:
            continue
        typ = clean(row.get('type')).upper()
        if typ in NAV_TYPES:
            continue
        khz = khz_of(row.get('frequency_mhz'))
        if khz is None:
            continue
        desc = clean(row.get('description'))
        add(aid, khz, typ or 'MISC', '' if desc.upper() == typ else desc)

    # --- write ---------------------------------------------------------------
    out = io.StringIO(newline='')
    out.write('# FoxSDR airband table, built %s by tools/make-airband-data.py.\n'
              % datetime.date.today().isoformat())
    out.write('# Sources (public domain): OurAirports (ourairports.com/data) and the FAA 28-day\n')
    out.write('# NASR subscription, FRQ.csv (faa.gov). FAA lists replace OurAirports\' for US airports.\n')
    out.write('# A ident iata local country lat lon name / F channel-kHz type description\n')
    count_a = count_f = 0
    for aid in sorted(freqs, key=lambda a: airports[a]['ident']):
        a = airports[aid]
        try:
            lat = float(a['latitude_deg'])
            lon = float(a['longitude_deg'])
        except (TypeError, ValueError):
            continue
        out.write('A\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%s\n' % (
            clean(a['ident']), clean(a.get('iata_code')), clean(a.get('local_code')),
            clean(a.get('iso_country')), lat, lon, clean(a.get('name'))))
        count_a += 1
        for khz in sorted(freqs[aid]):
            typ, descs = freqs[aid][khz]
            out.write('F\t%d\t%s\t%s\n' % (khz, clean(typ), clean('; '.join(descs))[:160]))
            count_f += 1
    data = out.getvalue().encode('utf-8')

    os.makedirs(os.path.dirname(OUT_TSV), exist_ok=True)
    with open(OUT_TSV, 'wb') as f:
        f.write(data)

    lines = []
    for i in range(0, len(data), 24):
        lines.append(','.join('0x%02x' % b for b in data[i:i + 24]) + ',')
    hpp = (
        '// airband_assets.hpp - GENERATED by tools/make-airband-data.py. Do not edit by hand.\n'
        '//\n'
        '// resources/airband/airband.tsv, byte for byte: the airport frequency table the\n'
        '// AIRBAND section looks airports up in. Re-run the generator after rebuilding\n'
        '// the table; tests/test_airband_data.cpp fails when these bytes and the file\n'
        '// disagree. INCLUDE FROM EXACTLY ONE TRANSLATION UNIT (core/airband_data.cpp).\n'
        '//\n'
        '// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0\n'
        '#ifndef CASCADE_CORE_AIRBAND_ASSETS_HPP\n'
        '#define CASCADE_CORE_AIRBAND_ASSETS_HPP\n'
        '\n'
        'namespace cascade::core::airbanddata {\n'
        '\n'
        '// %d airports, %d frequencies, %d bytes\n'
        'inline constexpr unsigned int kTableLen = %du;\n'
        'inline constexpr unsigned char kTable[] = {\n'
        '%s\n'
        '};\n'
        '\n'
        '}  // namespace cascade::core::airbanddata\n'
        '\n'
        '#endif  // CASCADE_CORE_AIRBAND_ASSETS_HPP\n'
    ) % (count_a, count_f, len(data), len(data), '\n'.join(lines))
    with open(OUT_HPP, 'w', newline='\n', encoding='ascii') as f:
        f.write(hpp)

    print('%s: %d airports, %d frequencies, %d bytes' % (OUT_TSV, count_a, count_f, len(data)))
    print('%s written' % OUT_HPP)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
