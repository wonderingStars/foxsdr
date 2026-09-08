// Tests for core/nmea.hpp / nmea.cpp - the pure NMEA 0183 parser and the
// line assembler that feeds it.
//
// There is no GPS on this bench and no COM port on this machine, so every
// sentence here is written from the NMEA 0183 field definitions, not read off
// a receiver. Two kinds of sentence appear. LITERAL ones carry a checksum that
// was verified OUTSIDE the implementation (the reference sentences that the
// published field definitions use as examples, whose "*47" / "*6A" / "*1D"
// trailers are quoted everywhere, plus a few more whose XOR was worked
// independently in Python) - those pin nmeaChecksum itself, because a checksum
// helper that agreed with a parser sharing its bug would prove nothing. FRAMED
// ones go through nmeaFrame(), which the literals have just checked, so the
// rest of the file can vary a field without hand-recomputing a trailer.
//
// Expected coordinates are computed HERE, by hand, from degrees + minutes/60,
// never copied out of the parser: 4807.038 N is 48 + 7.038/60 = 48.1173
// exactly, and the tolerance is 1e-9 so a conversion that divides by 100, or
// by 60 twice, or truncates the fraction, cannot slip through.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/nmea.hpp"

// receiverPositionAcceptable(): the parser deliberately reports a fix at
// 0 N 0 E as valid (the receiver said so), and the application refuses it one
// door later. Both halves are asserted here so the division of labour is
// pinned - see the "fix at exactly 0,0" block.
#include "gui/scope_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "test_check.hpp"

using cascade::core::NmeaFix;
using cascade::core::NmeaLineAssembler;
using cascade::core::NmeaSentence;
using cascade::core::NmeaStatus;
using cascade::core::NmeaSummary;
using cascade::core::nmeaChecksum;
using cascade::core::nmeaChecksumValid;
using cascade::core::nmeaFrame;
using cascade::core::nmeaParseCoordinate;
using cascade::core::parseNmeaSentence;

namespace {

// --- the literal, independently verified sentences ------------------------------

// The reference GGA/RMC/GLL sentences from the published field definitions,
// trailers as published. Munich: 48 deg 07.038 min N, 11 deg 31.000 min E.
const char* const kGgaMunich =
    "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
const char* const kRmcMunich =
    "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
// The GLL reference carries a trailing empty mode field; it is kept because a
// real sentence with a trailing comma must parse as the standard shows it.
const char* const kGllReference = "$GPGLL,4916.45,N,12311.12,W,225444,A,*1D";
// A receiver still searching: empty position fields, quality 0. XOR worked
// independently.
const char* const kGgaNoFix = "$GPGGA,123519,,,,,0,00,,,M,,M,,*6B";
const char* const kRmcVoid = "$GPRMC,,V,,,,,,,,,,N*53";
// A multi-constellation talker at a distinctive place (Leeds), with the
// two-decimal time and three-decimal HDOP a u-blox writes. XOR worked
// independently.
const char* const kGgaLeeds =
    "$GNGGA,092725.00,5347.7888,N,00132.871,W,1,08,1.01,499.6,M,48.0,M,,*69";
// A receiver honestly reporting the origin. XOR worked independently.
const char* const kGllOrigin = "$GPGLL,0000.0000,N,00000.0000,E,120000,A*29";

// Seconds since midnight, by hand.
constexpr double kUtc123519 = 12 * 3600.0 + 35 * 60.0 + 19.0;   // 45319
constexpr double kUtc092725 = 9 * 3600.0 + 27 * 60.0 + 25.0;    // 34045
constexpr double kUtc225444 = 22 * 3600.0 + 54 * 60.0 + 44.0;   // 82484

// A fix filled with values no sentence in this file produces, so "untouched"
// is distinguishable from "assigned the same thing".
NmeaFix sentinelFix() {
    NmeaFix f;
    f.sentence = NmeaSentence::RMC;
    f.talker = "ZZ";
    f.valid = true;
    f.latDeg = 12.345;
    f.lonDeg = 67.89;
    f.altitudeM = 1234.5;
    f.fixQuality = 9;
    f.satellites = 99;
    f.hdop = 9.9;
    f.utcSecondsOfDay = 1.0;
    return f;
}

bool isSentinel(const NmeaFix& f) {
    return f.sentence == NmeaSentence::RMC && f.talker == "ZZ" && f.valid &&
           f.latDeg == 12.345 && f.lonDeg == 67.89 && f.altitudeM == 1234.5 &&
           f.fixQuality == 9 && f.satellites == 99 && f.hdop == 9.9 &&
           f.utcSecondsOfDay == 1.0;
}

void feedString(NmeaLineAssembler& a, const std::string& s, std::vector<std::string>& out) {
    a.feed(s.data(), s.size(), out);
}

}  // namespace

int main() {
    // --- checksum: the helper against hand-verified literals ----------------------
    {
        std::printf("[checksum] hand-verified literals pin the helper\n");
        // Payloads are the text between '$' and '*'.
        CHECK(nmeaChecksum("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,") == 0x47);
        CHECK(nmeaChecksum("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W") == 0x6A);
        CHECK(nmeaChecksum("GPGLL,4916.45,N,12311.12,W,225444,A,") == 0x1D);
        CHECK(nmeaChecksum("GPGGA,123519,,,,,0,00,,,M,,M,,") == 0x6B);
        CHECK(nmeaChecksum("GPRMC,,V,,,,,,,,,,N") == 0x53);
        CHECK(nmeaChecksum("") == 0);
        CHECK(nmeaChecksumValid(kGgaMunich));
        CHECK(nmeaChecksumValid(kRmcMunich));
        CHECK(nmeaChecksumValid(kGllReference));
        CHECK(nmeaChecksumValid(kGgaNoFix));
        CHECK(nmeaChecksumValid(kRmcVoid));
        CHECK(nmeaChecksumValid(kGgaLeeds));
        CHECK(nmeaChecksumValid(kGllOrigin));
        // nmeaFrame reproduces the published trailers byte for byte, uppercase.
        CHECK(nmeaFrame("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,") ==
              std::string(kGgaMunich));
        CHECK(nmeaFrame("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W") ==
              std::string(kRmcMunich));
        CHECK(nmeaFrame("GPGLL,4916.45,N,12311.12,W,225444,A,") == std::string(kGllReference));
        // Round trip: whatever nmeaFrame produces, nmeaChecksumValid accepts.
        CHECK(nmeaChecksumValid(nmeaFrame("GPVTG,054.7,T,034.4,M,005.5,N,010.2,K")));
        CHECK(nmeaChecksumValid(nmeaFrame("PMTK001,314,3")));
    }

    // --- checksum: accept / reject shapes ------------------------------------------
    {
        std::printf("[checksum] accept and reject\n");
        NmeaFix fix;
        // Trailing line endings are tolerated on a raw line.
        CHECK(nmeaChecksumValid(std::string(kGgaMunich) + "\r\n"));
        CHECK(nmeaChecksumValid(std::string(kGgaMunich) + "\n"));
        CHECK(nmeaChecksumValid(std::string(kGgaMunich) + "\r"));
        CHECK(parseNmeaSentence(std::string(kGgaMunich) + "\r\n", fix) == NmeaStatus::Ok);
        // Lowercase hex is the same number.
        CHECK(nmeaChecksumValid("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6a"));
        CHECK(parseNmeaSentence("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6a",
                                fix) == NmeaStatus::Ok);
        // Wrong by one bit: refused as corrupted.
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*46"));
        CHECK(parseNmeaSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*46",
                                fix) == NmeaStatus::BadChecksum);
        // A corrupted digit in the latitude with the original trailer: the
        // whole point of insisting on the checksum.
        CHECK(parseNmeaSentence("$GPGGA,123519,4907.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
                                fix) == NmeaStatus::BadChecksum);
        // No trailer at all.
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"));
        CHECK(parseNmeaSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", fix) ==
              NmeaStatus::NoChecksum);
        // A '*' with no digits, one digit, or a non-hex pair is not a checksum.
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*"));
        CHECK(parseNmeaSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*", fix) ==
              NmeaStatus::NoChecksum);
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4"));
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4G"));
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*477"));
        // No leading '$' is not a sentence, whatever follows.
        CHECK(!nmeaChecksumValid("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
        CHECK(parseNmeaSentence("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", fix) ==
              NmeaStatus::NoChecksum);
        CHECK(!nmeaChecksumValid(""));
        CHECK(!nmeaChecksumValid("$"));
        // An empty payload XORs to 0x00, so "$*00" is an honestly framed
        // nothing: it verifies, and the parser then ignores it as not ours.
        CHECK(nmeaChecksumValid("$*00"));
        CHECK(!nmeaChecksumValid("$*01"));
        CHECK(parseNmeaSentence("$*00", fix) == NmeaStatus::Ignored);
        CHECK(nmeaChecksumValid("$X*58"));  // 'X' is 0x58: the smallest sentence that can verify
    }

    // --- GGA with a fix ---------------------------------------------------------------
    {
        std::printf("[GGA] a fix: every field\n");
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence(kGgaMunich, fix) == NmeaStatus::Ok);
        CHECK(fix.sentence == NmeaSentence::GGA);
        CHECK(fix.talker == "GP");
        CHECK(fix.valid);
        CHECK_NEAR(fix.latDeg, 48.0 + 7.038 / 60.0, 1e-9);
        CHECK_NEAR(fix.latDeg, 48.1173, 1e-9);
        CHECK_NEAR(fix.lonDeg, 11.0 + 31.0 / 60.0, 1e-9);
        CHECK(fix.fixQuality == 1);
        CHECK(fix.satellites == 8);
        CHECK_NEAR(fix.hdop, 0.9, 1e-9);
        CHECK_NEAR(fix.altitudeM, 545.4, 1e-9);
        CHECK_NEAR(fix.utcSecondsOfDay, kUtc123519, 1e-9);

        NmeaFix leeds = sentinelFix();
        CHECK(parseNmeaSentence(kGgaLeeds, leeds) == NmeaStatus::Ok);
        CHECK(leeds.talker == "GN");
        CHECK(leeds.valid);
        CHECK_NEAR(leeds.latDeg, 53.0 + 47.7888 / 60.0, 1e-9);
        CHECK_NEAR(leeds.latDeg, 53.79648, 1e-9);
        CHECK_NEAR(leeds.lonDeg, -(1.0 + 32.871 / 60.0), 1e-9);
        CHECK_NEAR(leeds.lonDeg, -1.54785, 1e-9);
        CHECK(leeds.satellites == 8);
        CHECK_NEAR(leeds.hdop, 1.01, 1e-9);
        CHECK_NEAR(leeds.altitudeM, 499.6, 1e-9);
        CHECK_NEAR(leeds.utcSecondsOfDay, kUtc092725, 1e-9);

        // DGPS quality is still a fix; a fractional time keeps its fraction.
        NmeaFix dgps;
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519.50,4807.038,N,01131.000,E,2,12,0.8,545.4,M,46.9,M,3,0120"),
                                dgps) == NmeaStatus::Ok);
        CHECK(dgps.valid);
        CHECK(dgps.fixQuality == 2);
        CHECK(dgps.satellites == 12);
        CHECK_NEAR(dgps.utcSecondsOfDay, kUtc123519 + 0.5, 1e-9);
    }

    // --- GGA without a fix ------------------------------------------------------------
    {
        std::printf("[GGA] no fix: quality 0 and empty position fields, never 0,0\n");
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence(kGgaNoFix, fix) == NmeaStatus::Ok);
        CHECK(!fix.valid);
        CHECK(std::isnan(fix.latDeg));
        CHECK(std::isnan(fix.lonDeg));
        CHECK(fix.fixQuality == 0);
        CHECK(fix.satellites == 0);
        CHECK(std::isnan(fix.hdop));
        CHECK(std::isnan(fix.altitudeM));
        CHECK_NEAR(fix.utcSecondsOfDay, kUtc123519, 1e-9);
        CHECK(!cascade::gui::receiverPositionAcceptable(fix.latDeg, fix.lonDeg));

        // Position fields present but quality 0: the receiver's last guess.
        // The coordinates are reported, the fix is not valid.
        NmeaFix guess;
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4807.038,N,01131.000,E,0,03,9.9,,M,,M,,"), guess) ==
              NmeaStatus::Ok);
        CHECK(!guess.valid);
        CHECK_NEAR(guess.latDeg, 48.1173, 1e-9);
        CHECK(guess.fixQuality == 0);
        CHECK(guess.satellites == 3);

        // Empty quality field: no fix.
        NmeaFix emptyQuality;
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,,,,,,,,,,M,,M,,"), emptyQuality) == NmeaStatus::Ok);
        CHECK(!emptyQuality.valid);
        CHECK(emptyQuality.fixQuality == 0);
        CHECK(emptyQuality.satellites == -1);
        CHECK(std::isnan(emptyQuality.utcSecondsOfDay));

        // Altitude absent on a valid fix stays NaN (a receiver with a 2-D fix).
        NmeaFix noAlt;
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4807.038,N,01131.000,E,1,04,2.5,,M,,M,,"), noAlt) ==
              NmeaStatus::Ok);
        CHECK(noAlt.valid);
        CHECK(std::isnan(noAlt.altitudeM));
        CHECK_NEAR(noAlt.hdop, 2.5, 1e-9);
        CHECK(noAlt.satellites == 4);
    }

    // --- RMC ------------------------------------------------------------------------
    {
        std::printf("[RMC] status A valid, status V not\n");
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence(kRmcMunich, fix) == NmeaStatus::Ok);
        CHECK(fix.sentence == NmeaSentence::RMC);
        CHECK(fix.talker == "GP");
        CHECK(fix.valid);
        CHECK_NEAR(fix.latDeg, 48.1173, 1e-9);
        CHECK_NEAR(fix.lonDeg, 11.0 + 31.0 / 60.0, 1e-9);
        CHECK_NEAR(fix.utcSecondsOfDay, kUtc123519, 1e-9);
        // RMC has no quality, satellite or altitude fields.
        CHECK(fix.fixQuality == -1);
        CHECK(fix.satellites == -1);
        CHECK(std::isnan(fix.altitudeM));
        CHECK(std::isnan(fix.hdop));

        NmeaFix voidFix = sentinelFix();
        CHECK(parseNmeaSentence(kRmcVoid, voidFix) == NmeaStatus::Ok);
        CHECK(!voidFix.valid);
        CHECK(std::isnan(voidFix.latDeg));
        CHECK(std::isnan(voidFix.lonDeg));
        CHECK(std::isnan(voidFix.utcSecondsOfDay));

        // 'V' with a position present: dead reckoning, reported, not valid.
        NmeaFix dr;
        CHECK(parseNmeaSentence(nmeaFrame("GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W,N"),
                                dr) == NmeaStatus::Ok);
        CHECK(!dr.valid);
        CHECK_NEAR(dr.latDeg, 48.1173, 1e-9);

        // The NMEA 4.1 navigational-status trailing field is ignored.
        NmeaFix v41;
        CHECK(parseNmeaSentence(nmeaFrame("GNRMC,092725.00,A,5347.7888,N,00132.871,W,0.004,77.52,091202,,,A,V"),
                                v41) == NmeaStatus::Ok);
        CHECK(v41.valid);
        CHECK(v41.talker == "GN");
        CHECK_NEAR(v41.lonDeg, -1.54785, 1e-9);
    }

    // --- GLL ------------------------------------------------------------------------
    {
        std::printf("[GLL] reference sentence, status V, status absent\n");
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence(kGllReference, fix) == NmeaStatus::Ok);
        CHECK(fix.sentence == NmeaSentence::GLL);
        CHECK(fix.valid);
        CHECK_NEAR(fix.latDeg, 49.0 + 16.45 / 60.0, 1e-9);
        CHECK_NEAR(fix.lonDeg, -(123.0 + 11.12 / 60.0), 1e-9);
        CHECK_NEAR(fix.utcSecondsOfDay, kUtc225444, 1e-9);
        CHECK(fix.fixQuality == -1);
        CHECK(fix.satellites == -1);
        CHECK(std::isnan(fix.altitudeM));

        NmeaFix voidFix;
        CHECK(parseNmeaSentence(nmeaFrame("GPGLL,4916.45,N,12311.12,W,225444,V,N"), voidFix) == NmeaStatus::Ok);
        CHECK(!voidFix.valid);
        CHECK_NEAR(voidFix.latDeg, 49.0 + 16.45 / 60.0, 1e-9);

        // An old receiver that stops at the time field: status absent is void.
        NmeaFix noStatus;
        CHECK(parseNmeaSentence(nmeaFrame("GPGLL,4916.45,N,12311.12,W,225444"), noStatus) == NmeaStatus::Ok);
        CHECK(!noStatus.valid);

        NmeaFix empty;
        CHECK(parseNmeaSentence(nmeaFrame("GPGLL,,,,,,V,N"), empty) == NmeaStatus::Ok);
        CHECK(!empty.valid);
        CHECK(std::isnan(empty.latDeg));
        CHECK(std::isnan(empty.lonDeg));
    }

    // --- hemispheres ------------------------------------------------------------------
    {
        std::printf("[hemisphere] every sign combination, through the parser and the helper\n");
        // 51 deg 30.4 min, 0 deg 07.66 min: London-ish magnitudes, small
        // enough that a sign error cannot be hidden by a range check.
        const double lat = 51.0 + 30.4 / 60.0;
        const double lon = 0.0 + 7.66 / 60.0;
        struct Case { const char* ns; const char* ew; double latSign; double lonSign; };
        const Case cases[] = {
            {"N", "E", +1.0, +1.0}, {"N", "W", +1.0, -1.0}, {"S", "E", -1.0, +1.0}, {"S", "W", -1.0, -1.0},
        };
        for (const Case& c : cases) {
            const std::string payload = std::string("GPGGA,120000,5130.4000,") + c.ns + ",00007.6600," +
                                        c.ew + ",1,06,1.5,20.0,M,45.0,M,,";
            NmeaFix fix;
            CHECK(parseNmeaSentence(nmeaFrame(payload), fix) == NmeaStatus::Ok);
            CHECK(fix.valid);
            CHECK_NEAR(fix.latDeg, c.latSign * lat, 1e-9);
            CHECK_NEAR(fix.lonDeg, c.lonSign * lon, 1e-9);
            double d = 0.0;
            CHECK(nmeaParseCoordinate("5130.4000", c.ns[0], false, d));
            CHECK_NEAR(d, c.latSign * lat, 1e-9);
            CHECK(nmeaParseCoordinate("00007.6600", c.ew[0], true, d));
            CHECK_NEAR(d, c.lonSign * lon, 1e-9);
        }
        // The wrong letter set for the axis, lowercase, or nothing: refused,
        // and outDeg is left alone.
        double untouched = 12345.0;
        CHECK(!nmeaParseCoordinate("5130.4000", 'E', false, untouched));
        CHECK(!nmeaParseCoordinate("5130.4000", 'W', false, untouched));
        CHECK(!nmeaParseCoordinate("00007.6600", 'N', true, untouched));
        CHECK(!nmeaParseCoordinate("00007.6600", 'S', true, untouched));
        CHECK(!nmeaParseCoordinate("5130.4000", 'n', false, untouched));
        CHECK(!nmeaParseCoordinate("5130.4000", '\0', false, untouched));
        CHECK(!nmeaParseCoordinate("5130.4000", ' ', false, untouched));
        CHECK(untouched == 12345.0);
        // Through the parser: a hemisphere letter that is not N/S/E/W refuses
        // the whole sentence, and the caller's fix is untouched.
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,120000,5130.4000,X,00007.6600,E,1,06,1.5,20.0,M,45.0,M,,"),
                                fix) == NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,120000,5130.4000,N,00007.6600,S,1,06,1.5,20.0,M,45.0,M,,"),
                                fix) == NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,120000,5130.4000,,00007.6600,E,1,06,1.5,20.0,M,45.0,M,,"),
                                fix) == NmeaStatus::Malformed);
        CHECK(isSentinel(fix));
    }

    // --- ddmm.mmmm conversion exactness -----------------------------------------------
    {
        std::printf("[coordinate] ddmm.mmmm conversion against hand-computed values\n");
        double d = 0.0;
        CHECK(nmeaParseCoordinate("4807.038", 'N', false, d));
        CHECK_NEAR(d, 48.1173, 1e-9);                       // 7.038 / 60 = 0.1173 exactly
        CHECK(nmeaParseCoordinate("01131.000", 'E', true, d));
        CHECK_NEAR(d, 11.0 + 31.0 / 60.0, 1e-9);
        CHECK(nmeaParseCoordinate("5347.7888", 'N', false, d));
        CHECK_NEAR(d, 53.79648, 1e-9);                      // 47.7888 / 60 = 0.79648 exactly
        CHECK(nmeaParseCoordinate("00132.871", 'W', true, d));
        CHECK_NEAR(d, -1.54785, 1e-9);                      // 32.871 / 60 = 0.54785 exactly
        CHECK(nmeaParseCoordinate("4916.45", 'N', false, d));
        CHECK_NEAR(d, 49.0 + 16.45 / 60.0, 1e-9);
        CHECK(nmeaParseCoordinate("12311.12", 'W', true, d));
        CHECK_NEAR(d, -(123.0 + 11.12 / 60.0), 1e-9);
        // The extremes of the globe.
        CHECK(nmeaParseCoordinate("0000.0000", 'N', false, d));
        CHECK(d == 0.0);
        CHECK(nmeaParseCoordinate("00000.0000", 'W', true, d));
        CHECK(d == 0.0);
        CHECK(nmeaParseCoordinate("9000.0000", 'S', false, d));
        CHECK_NEAR(d, -90.0, 1e-9);
        CHECK(nmeaParseCoordinate("18000.0000", 'E', true, d));
        CHECK_NEAR(d, 180.0, 1e-9);
        CHECK(nmeaParseCoordinate("8959.9999", 'N', false, d));
        CHECK_NEAR(d, 89.0 + 59.9999 / 60.0, 1e-9);
        CHECK(nmeaParseCoordinate("17959.9999", 'W', true, d));
        CHECK_NEAR(d, -(179.0 + 59.9999 / 60.0), 1e-9);
        // No fraction at all is a whole number of minutes.
        CHECK(nmeaParseCoordinate("4807", 'N', false, d));
        CHECK_NEAR(d, 48.0 + 7.0 / 60.0, 1e-9);
        // Many decimals: a high-precision receiver's field converts as the
        // same digits would by hand.
        CHECK(nmeaParseCoordinate("4807.0380000001", 'N', false, d));
        CHECK_NEAR(d, 48.0 + 7.0380000001 / 60.0, 1e-9);
        // Three degree digits are legal in a latitude field only if the value
        // fits; the digit rule is about the minutes, so "807.038" is 8 deg.
        CHECK(nmeaParseCoordinate("807.038", 'N', false, d));
        CHECK_NEAR(d, 8.0 + 7.038 / 60.0, 1e-9);
        // And a longitude written with two degree digits is legal too.
        CHECK(nmeaParseCoordinate("4807.038", 'E', true, d));
        CHECK_NEAR(d, 48.1173, 1e-9);

        // Refusals, each leaving outDeg alone.
        double untouched = 12345.0;
        CHECK(!nmeaParseCoordinate("", 'N', false, untouched));          // empty
        CHECK(!nmeaParseCoordinate("07.038", 'N', false, untouched));    // two digits before the point
        CHECK(!nmeaParseCoordinate("7.038", 'N', false, untouched));
        CHECK(!nmeaParseCoordinate(".038", 'N', false, untouched));
        CHECK(!nmeaParseCoordinate("4860.000", 'N', false, untouched));  // minutes >= 60
        CHECK(!nmeaParseCoordinate("4899.999", 'N', false, untouched));
        CHECK(!nmeaParseCoordinate("9100.000", 'N', false, untouched));  // beyond 90
        CHECK(!nmeaParseCoordinate("9000.001", 'N', false, untouched));
        CHECK(!nmeaParseCoordinate("18100.000", 'E', true, untouched));  // beyond 180
        CHECK(!nmeaParseCoordinate("18000.001", 'W', true, untouched));
        CHECK(!nmeaParseCoordinate("48O7.038", 'N', false, untouched));  // letter O for zero
        CHECK(!nmeaParseCoordinate("4807.03x", 'N', false, untouched));
        CHECK(!nmeaParseCoordinate("4807,038", 'N', false, untouched));
        CHECK(!nmeaParseCoordinate("-4807.038", 'N', false, untouched)); // sign is the letter's job
        CHECK(!nmeaParseCoordinate(" 4807.038", 'N', false, untouched));
        CHECK(!nmeaParseCoordinate("4807.038 ", 'N', false, untouched));
        CHECK(!nmeaParseCoordinate("4807.0.38", 'N', false, untouched));
        CHECK(untouched == 12345.0);

        // Through the parser, each refusal is Malformed and leaves the fix alone.
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,48O7.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4860.000,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,9100.000,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4807.038,N,18100.000,E,1,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Malformed);
        // Half a position: latitude present, longitude empty.
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4807.038,N,,,1,08,0.9,545.4,M,46.9,M,,"), fix) ==
              NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPRMC,123519,A,,,01131.000,E,022.4,084.4,230394,003.1,W"), fix) ==
              NmeaStatus::Malformed);
        CHECK(isSentinel(fix));
    }

    // --- too few fields ---------------------------------------------------------------
    {
        std::printf("[malformed] a sentence cut short of its status field\n");
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4807.038,N,01131.000,E"), fix) == NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA"), fix) == NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPRMC,123519,A,4807.038,N"), fix) == NmeaStatus::Malformed);
        CHECK(parseNmeaSentence(nmeaFrame("GPGLL,4916.45,N,12311.12"), fix) == NmeaStatus::Malformed);
        // A quality field that is not a number.
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4807.038,N,01131.000,E,A,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Malformed);
        CHECK(isSentinel(fix));
        // The bare minimum for each type still parses: GGA through quality,
        // RMC through the longitude hemisphere, GLL likewise.
        NmeaFix minimal;
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4807.038,N,01131.000,E,1"), minimal) == NmeaStatus::Ok);
        CHECK(minimal.valid);
        CHECK(minimal.satellites == -1);
        CHECK(std::isnan(minimal.hdop));
        CHECK(std::isnan(minimal.altitudeM));
        CHECK(parseNmeaSentence(nmeaFrame("GPRMC,123519,A,4807.038,N,01131.000,E"), minimal) == NmeaStatus::Ok);
        CHECK(minimal.valid);
        CHECK(parseNmeaSentence(nmeaFrame("GPGLL,4916.45,N,12311.12,W"), minimal) == NmeaStatus::Ok);
        CHECK(!minimal.valid);  // no status field: void
    }

    // --- talkers ----------------------------------------------------------------------
    {
        std::printf("[talker] every constellation, and sentences that are not ours\n");
        const char* talkers[] = {"GP", "GN", "GL", "GA", "GB", "BD", "QZ"};
        for (const char* t : talkers) {
            NmeaFix fix;
            const std::string payload = std::string(t) + "GGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
            CHECK(parseNmeaSentence(nmeaFrame(payload), fix) == NmeaStatus::Ok);
            CHECK(fix.talker == t);
            CHECK(fix.valid);
            NmeaFix rmc;
            CHECK(parseNmeaSentence(nmeaFrame(std::string(t) + "RMC,123519,A,4807.038,N,01131.000,E,0.0,0.0,230394,,,A"),
                                    rmc) == NmeaStatus::Ok);
            CHECK(rmc.talker == t);
            NmeaFix gll;
            CHECK(parseNmeaSentence(nmeaFrame(std::string(t) + "GLL,4916.45,N,12311.12,W,225444,A"), gll) ==
                  NmeaStatus::Ok);
            CHECK(gll.talker == t);
        }
        // Verified but not a position sentence: ignored, fix untouched.
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence(nmeaFrame("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00"),
                                fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("GPVTG,054.7,T,034.4,M,005.5,N,010.2,K"), fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1"), fix) ==
              NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("GPZDA,201530.00,04,07,2002,00,00"), fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("PMTK001,314,3"), fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("PUBX,00,081350.00,4717.113210,N,00833.915187,E,546.589,G3,2.1,2.0,0.007,77.52,0.007,,0.92,1.19,0.77,9,0,0"),
                                fix) == NmeaStatus::Ignored);
        // A position type with a talker that is not two capital letters, or
        // an address of the wrong length, is not one of ours either.
        CHECK(parseNmeaSentence(nmeaFrame("gpGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("G1GGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGAX,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("GGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"),
                                fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame(""), fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA"), fix) == NmeaStatus::Malformed);  // ours, but empty
        CHECK(isSentinel(fix));
    }

    // --- the summary ----------------------------------------------------------------
    {
        std::printf("[summary] each status lands in its bucket\n");
        NmeaSummary s;
        NmeaFix fix;
        // Fold in real parse results rather than hand-built statuses, so the
        // buckets are tested against what the parser actually returns.
        auto fold = [&](const std::string& line) {
            NmeaFix parsed;
            const NmeaStatus st = parseNmeaSentence(line, parsed);
            s.record(st, parsed);
            return st;
        };
        CHECK(fold(kGgaMunich) == NmeaStatus::Ok);
        CHECK(fold(kGgaNoFix) == NmeaStatus::Ok);
        CHECK(fold(kRmcMunich) == NmeaStatus::Ok);
        CHECK(fold(nmeaFrame("GPGSV,3,1,11,03,03,111,00")) == NmeaStatus::Ignored);
        CHECK(fold(nmeaFrame("GPVTG,054.7,T")) == NmeaStatus::Ignored);
        CHECK(fold(nmeaFrame("PMTK001,314,3")) == NmeaStatus::Ignored);
        CHECK(fold(nmeaFrame("GPGGA,123519,48O7.038,N,01131.000,E,1")) == NmeaStatus::Malformed);
        CHECK(fold("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*46") == NmeaStatus::BadChecksum);
        CHECK(fold("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,") == NmeaStatus::NoChecksum);
        CHECK(fold("garbage") == NmeaStatus::NoChecksum);
        CHECK(s.sentences == 7);           // 3 Ok + 3 Ignored + 1 Malformed
        CHECK(s.positionSentences == 3);
        CHECK(s.validFixes == 2);          // the no-fix GGA does not count
        CHECK(s.ignored == 3);
        CHECK(s.malformed == 1);
        CHECK(s.checksumFailures == 3);
        // A fresh summary is all zeros.
        const NmeaSummary fresh;
        CHECK(fresh.sentences == 0 && fresh.positionSentences == 0 && fresh.validFixes == 0 &&
              fresh.checksumFailures == 0 && fresh.malformed == 0 && fresh.ignored == 0);
        (void)fix;
    }

    // --- assembler: one byte at a time ---------------------------------------------
    {
        std::printf("[assembler] bytes one at a time, CRLF\n");
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        const std::string stream = std::string(kGgaMunich) + "\r\n" + kRmcMunich + "\r\n";
        for (char c : stream) { a.feed(&c, 1, lines); }
        CHECK(lines.size() == 2);
        CHECK(lines.size() >= 1 && lines[0] == kGgaMunich);
        CHECK(lines.size() >= 2 && lines[1] == kRmcMunich);
        CHECK(a.noiseBytes() == 0);   // the line endings are not noise
        CHECK(a.overlongDropped() == 0);
        NmeaFix fix;
        CHECK(lines.size() >= 2 && parseNmeaSentence(lines[1], fix) == NmeaStatus::Ok);
        CHECK(fix.valid);
    }

    // --- assembler: several sentences in one chunk, every line ending ----------------
    {
        std::printf("[assembler] one chunk, CR, LF and CRLF endings\n");
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        const std::string stream = std::string(kGgaMunich) + "\r" + kRmcMunich + "\n" + kGllReference + "\r\n" +
                                   kGgaNoFix + "\n\n" + kRmcVoid + "\r\r\n";
        feedString(a, stream, lines);
        CHECK(lines.size() == 5);
        CHECK(lines.size() >= 5 && lines[0] == kGgaMunich && lines[1] == kRmcMunich && lines[2] == kGllReference &&
              lines[3] == kGgaNoFix && lines[4] == kRmcVoid);
        CHECK(a.noiseBytes() == 0);
        for (const std::string& l : lines) {
            NmeaFix fix;
            CHECK(parseNmeaSentence(l, fix) == NmeaStatus::Ok);
        }
    }

    // --- assembler: a sentence split across reads ---------------------------------
    {
        std::printf("[assembler] a chunk boundary in the middle of a sentence\n");
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        const std::string whole = std::string(kGgaMunich) + "\r\n";
        // Every possible split point, including inside the checksum and
        // between CR and LF.
        for (std::size_t cut = 0; cut <= whole.size(); ++cut) {
            NmeaLineAssembler b;
            std::vector<std::string> out;
            b.feed(whole.data(), cut, out);
            CHECK(out.empty() || cut >= whole.size() - 1);  // nothing complete before the CR
            b.feed(whole.data() + cut, whole.size() - cut, out);
            CHECK(out.size() == 1);
            CHECK(!out.empty() && out[0] == kGgaMunich);
            CHECK(b.noiseBytes() == 0);
        }
        // And the accumulate-across-reads contract: outLines is appended to.
        a.feed(whole.data(), 10, lines);
        a.feed(whole.data() + 10, whole.size() - 10, lines);
        a.feed(whole.data(), whole.size(), lines);
        CHECK(lines.size() == 2);
    }

    // --- assembler: binary garbage before the first '$' ----------------------------
    {
        std::printf("[assembler] 2 KB of binary garbage before the first sentence\n");
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        std::string garbage;
        // Every byte value except '$' (which would start a sentence) and the
        // two line endings (which are not noise between sentences), so the
        // count is exact.
        for (int i = 0; garbage.size() < 2048; ++i) {
            const char c = static_cast<char>((i * 37 + 11) & 0xFF);
            if (c == '$' || c == '\r' || c == '\n') { continue; }
            garbage.push_back(c);
        }
        CHECK(garbage.size() == 2048);
        feedString(a, garbage, lines);
        CHECK(lines.empty());
        CHECK(a.noiseBytes() == 2048);
        feedString(a, std::string(kGgaLeeds) + "\r\n", lines);
        CHECK(lines.size() == 1);
        CHECK(!lines.empty() && lines[0] == kGgaLeeds);
        CHECK(a.noiseBytes() == 2048);
        NmeaFix fix;
        CHECK(!lines.empty() && parseNmeaSentence(lines[0], fix) == NmeaStatus::Ok);
        CHECK(fix.valid);
        CHECK_NEAR(fix.latDeg, 53.79648, 1e-9);
    }

    // --- assembler: an overlong line --------------------------------------------------
    {
        std::printf("[assembler] a 300-character line is dropped and the next sentence still parses\n");
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        const std::string overlong = "$" + std::string(299, 'A') + "\r\n";
        feedString(a, overlong, lines);
        CHECK(lines.empty());
        CHECK(a.overlongDropped() == 1);
        // The bytes after the drop and before the line ending were noise:
        // 300 - kMaxSentenceBytes of them.
        CHECK(a.noiseBytes() == 300 - NmeaLineAssembler::kMaxSentenceBytes);
        feedString(a, std::string(kRmcMunich) + "\r\n", lines);
        CHECK(lines.size() == 1);
        CHECK(!lines.empty() && lines[0] == kRmcMunich);
        CHECK(a.overlongDropped() == 1);
        NmeaFix fix;
        CHECK(!lines.empty() && parseNmeaSentence(lines[0], fix) == NmeaStatus::Ok);
        CHECK(fix.valid);

        // A line of exactly the maximum legal length (80 characters, then
        // CRLF) is kept; one character more is not a sentence.
        NmeaLineAssembler b;
        std::vector<std::string> kept;
        const std::string legal = "$" + std::string(NmeaLineAssembler::kMaxSentenceBytes - 3, 'B');
        feedString(b, legal + "\r\n", kept);
        CHECK(kept.size() == 1);
        CHECK(!kept.empty() && kept[0] == legal);
        CHECK(b.overlongDropped() == 0);
        const std::string tooLong = "$" + std::string(NmeaLineAssembler::kMaxSentenceBytes - 1, 'C');
        feedString(b, tooLong + "\r\n", kept);
        CHECK(kept.size() == 1);
        CHECK(b.overlongDropped() == 1);
    }

    // --- assembler: the buffer is bounded --------------------------------------------
    {
        std::printf("[assembler] a megabyte with no line ending costs a fixed buffer, not the heap\n");
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        // A '$' every 200 bytes and never a line ending: each start runs to
        // kMaxSentenceBytes and is dropped, the rest of the run is noise, and
        // nothing accumulates. 1,000,000 bytes = 5,000 such runs. The spacing
        // is written against the constant so that raising the bound (it went
        // 82 -> 128 for high-precision receivers) keeps every run overlong;
        // a run SHORTER than the bound would be restarted by the next '$' and
        // counted as noise instead, and the drop this block exists to pin
        // would never happen.
        static_assert(200 > NmeaLineAssembler::kMaxSentenceBytes,
                      "the runs must reach the bound or nothing here is overlong");
        std::string stream;
        stream.reserve(1000000);
        for (int i = 0; i < 5000; ++i) {
            stream.push_back('$');
            stream.append(199, 'Z');
        }
        CHECK(stream.size() == 1000000);
        // Fed in ragged chunks so the drop lands at every offset within a read.
        std::size_t at = 0;
        std::size_t chunk = 1;
        while (at < stream.size()) {
            const std::size_t n = std::min(chunk, stream.size() - at);
            a.feed(stream.data() + at, n, lines);
            at += n;
            chunk = chunk % 977 + 1;
        }
        CHECK(lines.empty());
        CHECK(a.overlongDropped() == 5000);
        CHECK(a.noiseBytes() == 5000u * (200 - NmeaLineAssembler::kMaxSentenceBytes));
        // A megabyte of pure noise with no '$' at all: counted, nothing kept.
        NmeaLineAssembler b;
        const std::string noise(1000000, 'Q');
        feedString(b, noise, lines);
        CHECK(lines.empty());
        CHECK(b.noiseBytes() == 1000000);
        CHECK(b.overlongDropped() == 0);
        // Both still assemble the next real sentence.
        feedString(a, "\r\n" + std::string(kGgaMunich) + "\r\n", lines);
        feedString(b, std::string(kGgaMunich) + "\r\n", lines);
        CHECK(lines.size() == 2);
        CHECK(lines.size() == 2 && lines[0] == kGgaMunich && lines[1] == kGgaMunich);
        // The class holds nothing growable beyond its one bounded line: the
        // bound in the header is a compile-time promise a future edit cannot
        // quietly raise past the buffer.
        static_assert(NmeaLineAssembler::kMaxSentenceBytes <= NmeaLineAssembler::kBufferBytes);
    }

    // --- assembler: restarts, lone '$', reset ---------------------------------------
    {
        std::printf("[assembler] a '$' mid-sentence restarts, a lone '$' is noise, reset forgets\n");
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        // The port was opened mid-sentence: the tail of one sentence, no
        // line ending, then a whole one.
        const std::string tail = "N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
        feedString(a, tail + "\r\n" + std::string(kRmcMunich) + "\r\n", lines);
        CHECK(lines.size() == 1);
        CHECK(!lines.empty() && lines[0] == kRmcMunich);
        CHECK(a.noiseBytes() == tail.size());  // the tail before the first '$', not its CRLF
        // A '$' before the previous sentence ended: the fragment was noise.
        NmeaLineAssembler b;
        std::vector<std::string> out;
        feedString(b, "$GPGGA,1235" + std::string(kGgaMunich) + "\r\n", out);
        CHECK(out.size() == 1);
        CHECK(!out.empty() && out[0] == kGgaMunich);
        CHECK(b.noiseBytes() == 11);
        // "$" then a line ending is not a sentence.
        NmeaLineAssembler c;
        out.clear();
        feedString(c, "$\r\n$\n", out);
        CHECK(out.empty());
        CHECK(c.noiseBytes() == 2);
        // reset() drops a partial line so it cannot prefix the next session.
        // The partial is kept short enough that partial + sentence stays
        // under kMaxSentenceBytes, so the "without reset" twin below fuses
        // into one line rather than being dropped as overlong.
        const std::string partial = "$GPGGA,1";
        // The continuation deliberately carries no '$': a '$' would trigger
        // the restart rule above and hide a reset() that does nothing.
        const std::string continuation = "," + std::string(kGgaMunich).substr(1) + "\r\n";
        NmeaLineAssembler d;
        out.clear();
        feedString(d, partial, out);
        d.reset();
        feedString(d, continuation, out);
        CHECK(out.empty());                              // nothing in progress to complete
        CHECK(d.noiseBytes() == continuation.size() - 2);  // all noise but its CRLF
        feedString(d, std::string(kGgaMunich) + "\r\n", out);
        CHECK(out.size() == 1);
        CHECK(!out.empty() && out[0] == kGgaMunich);
        // Without the reset the same bytes are one fused, unverifiable line -
        // pinned so reset() cannot become a no-op.
        NmeaLineAssembler e;
        out.clear();
        feedString(e, partial, out);
        feedString(e, continuation, out);
        CHECK(out.size() == 1);
        CHECK(!out.empty() && out[0].size() == partial.size() + std::string(kGgaMunich).size());
        NmeaFix fused;
        CHECK(!out.empty() && parseNmeaSentence(out[0], fused) == NmeaStatus::BadChecksum);
        // An empty feed is harmless.
        e.feed(nullptr, 0, out);
        CHECK(out.size() == 1);
    }

    // --- a fix at exactly 0,0 ---------------------------------------------------------
    {
        std::printf("[origin] the parser reports 0 N 0 E honestly; the acceptance rule refuses it\n");
        NmeaFix fix;
        CHECK(parseNmeaSentence(kGllOrigin, fix) == NmeaStatus::Ok);
        CHECK(fix.valid);          // the receiver said so, and the parser does not editorialise
        CHECK(fix.latDeg == 0.0);
        CHECK(fix.lonDeg == 0.0);
        CHECK(!cascade::gui::receiverPositionAcceptable(fix.latDeg, fix.lonDeg));
        // The same door accepts a real place and refuses the absent pair.
        NmeaFix munich;
        CHECK(parseNmeaSentence(kGgaMunich, munich) == NmeaStatus::Ok);
        CHECK(cascade::gui::receiverPositionAcceptable(munich.latDeg, munich.lonDeg));
        NmeaFix none;
        CHECK(parseNmeaSentence(kGgaNoFix, none) == NmeaStatus::Ok);
        CHECK(!cascade::gui::receiverPositionAcceptable(none.latDeg, none.lonDeg));
        // 0 N with a real longitude, and a real latitude with 0 E, are places.
        NmeaFix equator;
        CHECK(parseNmeaSentence(nmeaFrame("GPGLL,0000.0000,N,00930.0000,E,120000,A"), equator) == NmeaStatus::Ok);
        CHECK(cascade::gui::receiverPositionAcceptable(equator.latDeg, equator.lonDeg));
        NmeaFix greenwich;
        CHECK(parseNmeaSentence(nmeaFrame("GPGLL,5128.6600,N,00000.0000,E,120000,A"), greenwich) == NmeaStatus::Ok);
        CHECK(cascade::gui::receiverPositionAcceptable(greenwich.latDeg, greenwich.lonDeg));
    }

    // --- a fix is left untouched by every non-Ok result ------------------------------
    {
        std::printf("[contract] fix untouched unless Ok\n");
        NmeaFix fix = sentinelFix();
        CHECK(parseNmeaSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*46", fix) ==
              NmeaStatus::BadChecksum);
        CHECK(parseNmeaSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", fix) ==
              NmeaStatus::NoChecksum);
        CHECK(parseNmeaSentence(nmeaFrame("GPGSV,3,1,11"), fix) == NmeaStatus::Ignored);
        CHECK(parseNmeaSentence(nmeaFrame("GPGGA,123519,4807.038"), fix) == NmeaStatus::Malformed);
        CHECK(isSentinel(fix));
        // And fully assigned on Ok: nothing of the sentinel survives.
        CHECK(parseNmeaSentence(kGgaNoFix, fix) == NmeaStatus::Ok);
        CHECK(fix.sentence == NmeaSentence::GGA && fix.talker == "GP" && !fix.valid && std::isnan(fix.latDeg) &&
              std::isnan(fix.lonDeg) && std::isnan(fix.altitudeM) && fix.fixQuality == 0 && fix.satellites == 0 &&
              std::isnan(fix.hdop));
    }

    // --- assembler: sentences longer than the 82 the standard names --------------------
    //
    // A u-blox ZED-F9P/M9 in high-precision mode writes seven decimals of
    // minutes (its interface description says the mode cannot be combined
    // with "Limit82"), and gpsd records a Trimble BX-960 emitting a
    // 91-character GGA. A receiver configured for RTK is the one most likely
    // to be set to GGA-only, so an assembler that dropped everything past 82
    // characters would leave such a receiver with no fix and the status line
    // blaming the baud rate - which is what the first build did.
    {
        std::printf("[assembler] a high-precision GGA past 82 characters still assembles and parses\n");
        // 53 deg 47.7888123 min N, 1 deg 32.8712345 min W: 84 characters framed.
        const std::string ublox = nmeaFrame(
            "GNGGA,133554.00,5347.7888123,N,00132.8712345,W,2,12,0.62,123.4,M,47.5,M,1.0,0000");
        CHECK(ublox.size() == 84);
        // Eight decimals and a three-decimal altitude, in the Trimble shape:
        // 91 characters framed, the length gpsd records for the BX-960.
        const std::string trimble = nmeaFrame(
            "GPGGA,133554.00,5347.78881234,N,00132.87123456,W,4,14,0.618,123.456,M,47.512,M,1.0,0000");
        CHECK(trimble.size() == 91);
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        feedString(a, ublox + "\r\n" + trimble + "\r\n", lines);
        CHECK(lines.size() == 2);
        CHECK(a.overlongDropped() == 0);
        CHECK(a.noiseBytes() == 0);
        NmeaFix u;
        CHECK(lines.size() == 2 && parseNmeaSentence(lines[0], u) == NmeaStatus::Ok);
        CHECK(u.valid);
        CHECK_NEAR(u.latDeg, 53.0 + 47.7888123 / 60.0, 1e-9);
        CHECK_NEAR(u.lonDeg, -(1.0 + 32.8712345 / 60.0), 1e-9);
        CHECK(u.satellites == 12);
        NmeaFix t;
        CHECK(lines.size() == 2 && parseNmeaSentence(lines[1], t) == NmeaStatus::Ok);
        CHECK(t.valid);
        CHECK(t.fixQuality == 4);
        CHECK_NEAR(t.latDeg, 53.0 + 47.78881234 / 60.0, 1e-9);
        CHECK_NEAR(t.altitudeM, 123.456, 1e-9);
        // The bound still exists, and still sits inside the buffer: gpsd uses
        // 130 and nothing real is longer, so 128 is enough and the stream of
        // framing garbage a wrong baud produces is still cut there.
        CHECK(NmeaLineAssembler::kMaxSentenceBytes >= 100);  // a 91-character Trimble GGA must assemble
        static_assert(NmeaLineAssembler::kMaxSentenceBytes <= NmeaLineAssembler::kBufferBytes);
    }

    // --- a byte between the trailer and the line ending --------------------------------
    //
    // A logger replay, a serial adapter or a receiver that pads the line can
    // leave a space or a NUL after "*hh". The checksum still covers exactly
    // what it should, so the sentence is readable and must be read; refusing
    // it would count every line as NoChecksum and tell the user to check the
    // baud rate of a stream that was entirely fine.
    {
        std::printf("[checksum] trailing whitespace or NUL after *hh is tolerated; a third digit is not\n");
        NmeaFix fix;
        CHECK(parseNmeaSentence(std::string(kGgaMunich) + " ", fix) == NmeaStatus::Ok);
        CHECK(fix.valid);
        CHECK_NEAR(fix.latDeg, 48.1173, 1e-9);
        CHECK(parseNmeaSentence(std::string(kGgaMunich) + " \r\n", fix) == NmeaStatus::Ok);
        CHECK(parseNmeaSentence(std::string(kGgaMunich) + "\t", fix) == NmeaStatus::Ok);
        CHECK(parseNmeaSentence(std::string(kGgaMunich) + std::string(1, '\0'), fix) == NmeaStatus::Ok);
        CHECK(parseNmeaSentence(std::string(kGgaMunich) + std::string("  \0\r\n", 5), fix) == NmeaStatus::Ok);
        CHECK(nmeaChecksumValid(std::string(kGgaMunich) + " "));
        // Through the assembler, which keeps the padding byte (it is not a
        // line ending), the line still parses.
        NmeaLineAssembler a;
        std::vector<std::string> lines;
        feedString(a, std::string(kGgaMunich) + " \r\n", lines);
        CHECK(lines.size() == 1);
        CHECK(!lines.empty() && parseNmeaSentence(lines[0], fix) == NmeaStatus::Ok);
        // What follows the two digits must be padding, not more digits or
        // another field: "*477" and "*47,x" are not trailers.
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*477"));
        CHECK(parseNmeaSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*477", fix) ==
              NmeaStatus::NoChecksum);
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47,x"));
        CHECK(!nmeaChecksumValid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4 7"));
        // A wrong checksum followed by padding is still wrong, not missing.
        CHECK(parseNmeaSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*46 ", fix) ==
              NmeaStatus::BadChecksum);
    }

    // --- GGA quality: which values are a fix ---------------------------------------
    //
    // 1 (GPS), 2 (DGPS), 3 (PPS), 4 (RTK fixed) and 5 (RTK float) are the
    // receiver measuring where it is. 6 is "estimated" - dead reckoning, the
    // same receiver state RMC reports as status 'V' - and 7 (manual input)
    // and 8 (simulator) are not measurements at all. A dead-reckoning
    // receiver carried indoors keeps emitting quality 6 with a drifting
    // position and no satellites; taking that as the antenna's fixed position
    // would be the very thing the RMC rule refuses, one sentence type over.
    {
        std::printf("[GGA] quality 1-5 are fixes; 6 (estimated), 7 (manual) and 8 (simulator) are not\n");
        for (int q = 1; q <= 5; ++q) {
            char payload[128];
            std::snprintf(payload, sizeof payload,
                          "GNGGA,133554.00,5347.7888,N,00132.8712,W,%d,08,1.1,123.4,M,47.5,M,,", q);
            NmeaFix fix;
            CHECK(parseNmeaSentence(nmeaFrame(payload), fix) == NmeaStatus::Ok);
            CHECK(fix.valid);
            CHECK(fix.fixQuality == q);
        }
        for (int q = 6; q <= 8; ++q) {
            char payload[128];
            std::snprintf(payload, sizeof payload,
                          "GNGGA,133554.00,5347.7888,N,00132.8712,W,%d,00,99.9,123.4,M,47.5,M,,", q);
            NmeaFix fix;
            CHECK(parseNmeaSentence(nmeaFrame(payload), fix) == NmeaStatus::Ok);
            CHECK(!fix.valid);
            // The quality is still reported verbatim, so the status line can
            // say what the receiver is doing; only the claim of a fix is
            // withheld, and the coordinates are still read.
            CHECK(fix.fixQuality == q);
            CHECK_NEAR(fix.latDeg, 53.79648, 1e-9);
        }
        // And the same receiver state through RMC, for the record: void.
        NmeaFix rmc;
        CHECK(parseNmeaSentence(nmeaFrame("GNRMC,133554.00,V,5347.7888,N,00132.8712,W,0.0,0.0,080926,,,E,V"),
                                rmc) == NmeaStatus::Ok);
        CHECK(!rmc.valid);
    }

    return testSummary("test_nmea");
}
