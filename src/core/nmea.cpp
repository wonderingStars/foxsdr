// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/nmea.hpp"

#include <cmath>
#include <cstdio>

namespace cascade::core {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isUpperLetter(char c) { return c >= 'A' && c <= 'Z'; }

// One hex digit, either case, or -1. Written out rather than reaching for
// strtol so that "*4G" cannot be read as "*4" and pass on a one-digit match.
int hexValue(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return 10 + (c - 'a'); }
    if (c >= 'A' && c <= 'F') { return 10 + (c - 'A'); }
    return -1;
}

std::string_view stripLineEnding(std::string_view s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) { s.remove_suffix(1); }
    return s;
}

// How the checksum trailer was judged. NoChecksum and BadChecksum are the two
// refusals parseNmeaSentence reports separately, because they mean different
// things to the person reading the status line: no trailer at all is a
// receiver (or a baud rate) that is not speaking NMEA, a wrong trailer is a
// cable problem.
enum class Trailer { Valid, Missing, Wrong };

// Splits "$payload*hh" into its payload and judges hh. `payload` is set only
// on Valid. The '*' searched for is the LAST one, so a stray '*' inside the
// payload (never legal, but never say never on a serial line) cannot make a
// good trailer look absent.
Trailer checkTrailer(std::string_view sentence, std::string_view& payload) {
    sentence = stripLineEnding(sentence);
    if (sentence.size() < 4 || sentence.front() != '$') { return Trailer::Missing; }
    const std::size_t star = sentence.rfind('*');
    if (star == std::string_view::npos) { return Trailer::Missing; }
    // Exactly two hex digits after the star. "*4" and "*" are not a checksum.
    if (sentence.size() - star < 3) { return Trailer::Missing; }
    const int hi = hexValue(sentence[star + 1]);
    const int lo = hexValue(sentence[star + 2]);
    if (hi < 0 || lo < 0) { return Trailer::Missing; }
    // Whatever follows the two digits may only be padding - a space, a tab
    // or a NUL that a logger replay or a serial adapter left between the
    // trailer and the line ending (the header says why that is tolerated).
    // "*477" and "*47,x" are not trailers: a third digit or another field
    // means the star was not the end of the sentence.
    for (std::size_t i = star + 3; i < sentence.size(); ++i) {
        const char c = sentence[i];
        if (c != ' ' && c != '\t' && c != '\0' && c != '\r' && c != '\n') { return Trailer::Missing; }
    }
    const std::string_view body = sentence.substr(1, star - 1);
    const std::uint8_t want = static_cast<std::uint8_t>((hi << 4) | lo);
    if (nmeaChecksum(body) != want) { return Trailer::Wrong; }
    payload = body;
    return Trailer::Valid;
}

// Splits on ',' keeping empty fields, because "123519,,,,,0" is the normal
// shape of a receiver with no fix and every one of those empties is a field.
std::vector<std::string_view> splitFields(std::string_view payload) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t comma = payload.find(',', at);
        if (comma == std::string_view::npos) {
            out.push_back(payload.substr(at));
            break;
        }
        out.push_back(payload.substr(at, comma - at));
        at = comma + 1;
    }
    return out;
}

// Reads a field of the form digits[.digits] with an optional leading sign, as
// HDOP, altitude and speed are written. False for anything else, including an
// empty field - the callers treat "absent" and "unreadable" the same way for
// these (NaN), because none of them gates whether a position is taken.
bool parseDecimal(std::string_view f, double& out) {
    if (f.empty()) { return false; }
    std::size_t i = 0;
    bool negative = false;
    if (f[i] == '-' || f[i] == '+') {
        negative = f[i] == '-';
        ++i;
    }
    double value = 0.0;
    std::size_t intDigits = 0;
    for (; i < f.size() && isDigit(f[i]); ++i, ++intDigits) { value = value * 10.0 + (f[i] - '0'); }
    std::size_t fracDigits = 0;
    if (i < f.size() && f[i] == '.') {
        ++i;
        double scale = 0.1;
        for (; i < f.size() && isDigit(f[i]); ++i, ++fracDigits) {
            value += (f[i] - '0') * scale;
            scale *= 0.1;
        }
    }
    if (i != f.size() || (intDigits == 0 && fracDigits == 0)) { return false; }
    out = negative ? -value : value;
    return true;
}

// A field of digits only, as satellites-in-use and fix quality are written.
bool parseUnsigned(std::string_view f, int& out) {
    if (f.empty() || f.size() > 6) { return false; }
    int value = 0;
    for (char c : f) {
        if (!isDigit(c)) { return false; }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

// "hhmmss" or "hhmmss.ss" to seconds since midnight. False for anything that
// is not six digits plus an optional fraction; the callers record NaN and
// keep the sentence, because the time of a fix is decoration here - the
// position is what the user asked for, and refusing a fix over a time field
// would be the parser failing the user to make a point.
bool parseUtc(std::string_view f, double& out) {
    if (f.size() < 6) { return false; }
    for (std::size_t i = 0; i < 6; ++i) {
        if (!isDigit(f[i])) { return false; }
    }
    const int hh = (f[0] - '0') * 10 + (f[1] - '0');
    const int mm = (f[2] - '0') * 10 + (f[3] - '0');
    const int ss = (f[4] - '0') * 10 + (f[5] - '0');
    if (hh > 23 || mm > 59 || ss > 60) { return false; }  // 60: a leap second is a real second
    double frac = 0.0;
    if (f.size() > 6) {
        if (f[6] != '.') { return false; }
        double scale = 0.1;
        for (std::size_t i = 7; i < f.size(); ++i) {
            if (!isDigit(f[i])) { return false; }
            frac += (f[i] - '0') * scale;
            scale *= 0.1;
        }
    }
    out = hh * 3600.0 + mm * 60.0 + ss + frac;
    return true;
}

// The one place a coordinate pair enters a fix. Both empty means "the
// receiver has no position yet": NaN, and the caller decides validity from
// the status field (which will say no fix). One empty and one present is
// Malformed - a receiver never emits that, and half a position must not be
// half-trusted. Both present must both parse, with their hemisphere letters,
// or the sentence is refused whole.
enum class Coords { Present, Absent, Malformed };

Coords readCoordinates(std::string_view lat, std::string_view ns, std::string_view lon,
                       std::string_view ew, NmeaFix& fix) {
    if (lat.empty() && lon.empty()) {
        fix.latDeg = NmeaFix::kAbsent;
        fix.lonDeg = NmeaFix::kAbsent;
        return Coords::Absent;
    }
    if (lat.empty() || lon.empty()) { return Coords::Malformed; }
    if (ns.size() != 1 || ew.size() != 1) { return Coords::Malformed; }
    double latDeg = 0.0;
    double lonDeg = 0.0;
    if (!nmeaParseCoordinate(lat, ns[0], false, latDeg)) { return Coords::Malformed; }
    if (!nmeaParseCoordinate(lon, ew[0], true, lonDeg)) { return Coords::Malformed; }
    fix.latDeg = latDeg;
    fix.lonDeg = lonDeg;
    return Coords::Present;
}

std::string_view fieldAt(const std::vector<std::string_view>& f, std::size_t i) {
    return i < f.size() ? f[i] : std::string_view{};
}

// $ttGGA,hhmmss.ss,lat,N,lon,E,quality,sats,hdop,alt,M,geoid,M,age,ref
NmeaStatus parseGga(const std::vector<std::string_view>& f, NmeaFix& fix) {
    // Through the quality field: that is what decides whether the position
    // fields mean anything, so a sentence cut short of it says nothing.
    if (f.size() < 7) { return NmeaStatus::Malformed; }
    const Coords coords = readCoordinates(f[2], f[3], f[4], f[5], fix);
    if (coords == Coords::Malformed) { return NmeaStatus::Malformed; }
    int quality = 0;  // an empty quality field is a receiver with no fix
    if (!f[6].empty() && !parseUnsigned(f[6], quality)) { return NmeaStatus::Malformed; }
    fix.fixQuality = quality;
    // 1..5 are measurements (GPS, differential, PPS, RTK fixed, RTK float).
    // 6 is dead reckoning - what RMC calls status 'V' - and 7 and 8 are a
    // hand-typed and a simulated position; none of those is where the
    // antenna is, so none of them is a fix here (header comment).
    fix.valid = quality >= 1 && quality <= 5 && coords == Coords::Present;
    int sats = -1;
    fix.satellites = parseUnsigned(fieldAt(f, 7), sats) ? sats : -1;
    double v = 0.0;
    fix.hdop = parseDecimal(fieldAt(f, 8), v) ? v : NmeaFix::kAbsent;
    fix.altitudeM = parseDecimal(fieldAt(f, 9), v) ? v : NmeaFix::kAbsent;
    fix.utcSecondsOfDay = parseUtc(f[1], v) ? v : NmeaFix::kAbsent;
    return NmeaStatus::Ok;
}

// $ttRMC,hhmmss.ss,A,lat,N,lon,E,speed,course,ddmmyy,var,E,mode
NmeaStatus parseRmc(const std::vector<std::string_view>& f, NmeaFix& fix) {
    // Through the longitude hemisphere: the status is at index 2, but a
    // status of 'A' with the position fields missing is not a fix either.
    if (f.size() < 7) { return NmeaStatus::Malformed; }
    const Coords coords = readCoordinates(f[3], f[4], f[5], f[6], fix);
    if (coords == Coords::Malformed) { return NmeaStatus::Malformed; }
    // 'A' is the only word for "valid". 'V' is void; anything else a receiver
    // might invent is treated as void too, because the parser's job is to
    // know when the receiver said yes, not to guess at every way it can say
    // no.
    fix.valid = f[2] == "A" && coords == Coords::Present;
    double v = 0.0;
    fix.utcSecondsOfDay = parseUtc(f[1], v) ? v : NmeaFix::kAbsent;
    return NmeaStatus::Ok;
}

// $ttGLL,lat,N,lon,E,hhmmss.ss,A,mode
NmeaStatus parseGll(const std::vector<std::string_view>& f, NmeaFix& fix) {
    // Through the longitude hemisphere. The status field came later in the
    // standard's life and older receivers omit it; absent is void.
    if (f.size() < 5) { return NmeaStatus::Malformed; }
    const Coords coords = readCoordinates(f[1], f[2], f[3], f[4], fix);
    if (coords == Coords::Malformed) { return NmeaStatus::Malformed; }
    fix.valid = fieldAt(f, 6) == "A" && coords == Coords::Present;
    double v = 0.0;
    fix.utcSecondsOfDay = parseUtc(fieldAt(f, 5), v) ? v : NmeaFix::kAbsent;
    return NmeaStatus::Ok;
}

}  // namespace

// --- checksum ------------------------------------------------------------------

std::uint8_t nmeaChecksum(std::string_view payload) {
    std::uint8_t sum = 0;
    for (char c : payload) { sum = static_cast<std::uint8_t>(sum ^ static_cast<unsigned char>(c)); }
    return sum;
}

std::string nmeaFrame(std::string_view payload) {
    char hex[4];
    std::snprintf(hex, sizeof hex, "%02X", static_cast<unsigned>(nmeaChecksum(payload)));
    std::string out;
    out.reserve(payload.size() + 4);
    out += '$';
    out += payload;
    out += '*';
    out += hex;
    return out;
}

bool nmeaChecksumValid(std::string_view sentence) {
    std::string_view payload;
    return checkTrailer(sentence, payload) == Trailer::Valid;
}

// --- coordinates ---------------------------------------------------------------

bool nmeaParseCoordinate(std::string_view field, char hemisphere, bool isLongitude,
                         double& outDeg) {
    if (field.empty()) { return false; }
    // Integer part: at least three digits, the last two of which are whole
    // minutes. "4807.038" is 48 degrees and 07.038 minutes; "807.038" is 8
    // degrees and the same minutes, and is legal, because the rule is that
    // the minutes need two digits, not that the degrees are padded.
    std::size_t i = 0;
    while (i < field.size() && isDigit(field[i])) { ++i; }
    const std::size_t intDigits = i;
    if (intDigits < 3) { return false; }
    // Fraction of a minute: a point and digits, or nothing at all. The digits
    // are read as an integer over a power of ten and divided ONCE, so that
    // "7.038" is the double nearest 7.038 and not the sum of three rounded
    // terms - the difference is far below a metre, but a test that computes
    // the value by hand should meet the parser at the same double.
    double fraction = 0.0;
    if (i < field.size()) {
        if (field[i] != '.') { return false; }
        ++i;
        double numerator = 0.0;
        double denominator = 1.0;
        std::size_t fracDigits = 0;
        for (; i < field.size(); ++i, ++fracDigits) {
            if (!isDigit(field[i])) { return false; }
            // Beyond twelve places nothing changes at double precision and a
            // pathological field cannot overflow the denominator.
            if (fracDigits < 12) {
                numerator = numerator * 10.0 + (field[i] - '0');
                denominator *= 10.0;
            }
        }
        fraction = numerator / denominator;
    }
    double degrees = 0.0;
    for (std::size_t k = 0; k < intDigits - 2; ++k) { degrees = degrees * 10.0 + (field[k] - '0'); }
    const int wholeMinutes = (field[intDigits - 2] - '0') * 10 + (field[intDigits - 1] - '0');
    if (wholeMinutes >= 60) { return false; }
    const double value = degrees + (wholeMinutes + fraction) / 60.0;
    double signedValue = 0.0;
    if (isLongitude) {
        if (hemisphere == 'E') { signedValue = value; }
        else if (hemisphere == 'W') { signedValue = -value; }
        else { return false; }
        if (value > 180.0) { return false; }
    } else {
        if (hemisphere == 'N') { signedValue = value; }
        else if (hemisphere == 'S') { signedValue = -value; }
        else { return false; }
        if (value > 90.0) { return false; }
    }
    outDeg = signedValue;
    return true;
}

// --- sentences -----------------------------------------------------------------

NmeaStatus parseNmeaSentence(std::string_view sentence, NmeaFix& fix) {
    std::string_view payload;
    switch (checkTrailer(sentence, payload)) {
        case Trailer::Missing: return NmeaStatus::NoChecksum;
        case Trailer::Wrong: return NmeaStatus::BadChecksum;
        case Trailer::Valid: break;
    }
    // The address field: two talker letters and a three-letter type. A
    // payload that does not start that way ("$PMTK001,..." has a five-letter
    // proprietary address; "$*00" has none) is somebody else's sentence, and
    // the checksum having verified means it was at least sent on purpose.
    const std::vector<std::string_view> fields = splitFields(payload);
    const std::string_view address = fields[0];
    if (address.size() != 5) { return NmeaStatus::Ignored; }
    for (char c : address) {
        if (!isUpperLetter(c)) { return NmeaStatus::Ignored; }
    }
    const std::string_view type = address.substr(2);
    NmeaFix parsed;
    parsed.talker = std::string(address.substr(0, 2));
    NmeaStatus status = NmeaStatus::Ignored;
    if (type == "GGA") {
        parsed.sentence = NmeaSentence::GGA;
        status = parseGga(fields, parsed);
    } else if (type == "RMC") {
        parsed.sentence = NmeaSentence::RMC;
        status = parseRmc(fields, parsed);
    } else if (type == "GLL") {
        parsed.sentence = NmeaSentence::GLL;
        status = parseGll(fields, parsed);
    } else {
        return NmeaStatus::Ignored;
    }
    // `fix` is assigned whole, and only now: a Malformed sentence has left
    // nothing behind but the local.
    if (status == NmeaStatus::Ok) { fix = parsed; }
    return status;
}

void NmeaSummary::record(NmeaStatus status, const NmeaFix& fix) {
    switch (status) {
        case NmeaStatus::Ok:
            ++sentences;
            ++positionSentences;
            if (fix.valid) { ++validFixes; }
            break;
        case NmeaStatus::Ignored:
            ++sentences;
            ++ignored;
            break;
        case NmeaStatus::Malformed:
            ++sentences;
            ++malformed;
            break;
        case NmeaStatus::NoChecksum:
        case NmeaStatus::BadChecksum:
            ++checksumFailures;
            break;
    }
}

// --- line assembly -------------------------------------------------------------

void NmeaLineAssembler::feed(const char* bytes, std::size_t count,
                             std::vector<std::string>& outLines) {
    for (std::size_t i = 0; i < count; ++i) {
        const char c = bytes[i];
        if (!inSentence_) {
            if (c == '$') {
                inSentence_ = true;
                pending_.assign(1, '$');
            } else if (c != '\r' && c != '\n') {
                // The line ending of the previous sentence is not noise; a
                // receiver's CRLF arrives one byte at a time like everything
                // else. Anything else before a '$' is.
                ++noiseBytes_;
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            // A lone "$" and a line ending is not a sentence; count the '$'
            // as the noise it was rather than hand the parser an empty line.
            if (pending_.size() > 1) { outLines.push_back(pending_); }
            else { ++noiseBytes_; }
            pending_.clear();
            inSentence_ = false;
            continue;
        }
        if (c == '$') {
            // A new start before the old sentence ended: the old one can
            // never verify, so it was noise, and the new one begins here.
            noiseBytes_ += pending_.size();
            pending_.assign(1, '$');
            continue;
        }
        pending_.push_back(c);
        if (pending_.size() >= kMaxSentenceBytes) {
            // Longer than any sentence the standard allows and still no line
            // ending: this is not a sentence, and waiting for its end would
            // let a wrong-baud stream grow one "line" forever. Drop it,
            // count it, and resume at the next '$'.
            ++overlongDropped_;
            pending_.clear();
            inSentence_ = false;
        }
    }
}

void NmeaLineAssembler::reset() {
    pending_.clear();
    inSentence_ = false;
}

}  // namespace cascade::core
