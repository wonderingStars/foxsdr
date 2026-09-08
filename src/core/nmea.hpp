// nmea.hpp - NMEA 0183 position sentences, parsed as a PURE function of text.
//
// A GPS receiver on a serial port talks NMEA 0183: one ASCII sentence per
// line, "$GPGGA,...*47", at a few sentences a second. This file turns those
// lines into a receiver position. It owns no port, no thread and no clock;
// serial_port.hpp moves the bytes and gps_reader.hpp runs the loop, and both
// are kept out of here so that every rule below is table-testable against a
// literal string with no hardware in the room - which matters more than usual
// for this feature, because the bench has NO GPS. A beta tester asked for it
// and will be the first person to run it against a real receiver. Every
// sentence shape the parser accepts or refuses is therefore pinned by a test
// that reads like the NMEA specification, and the parser is written to that
// specification rather than to the one receiver we happen not to own.
//
// WHAT IS PARSED. GGA (fix data), RMC (recommended minimum) and GLL
// (geographic position), from ANY talker: GP (GPS), GN (multi-constellation),
// GL (GLONASS), GA (Galileo), GB and BD (BeiDou), QZ (QZSS). Modern receivers
// emit $GN... once they use more than one constellation, and a parser that
// accepted only $GP would see a u-blox 8 as "no fix" all day. Every other
// sentence (GSV, GSA, VTG, ZDA, proprietary $PMTK...) is counted and ignored:
// it carries no position, and a talker we have never heard of is not an
// error, just not our business.
//
// THE CHECKSUM IS MANDATORY. "$...*hh" where hh is the XOR of every byte
// between '$' and '*', in hex. A sentence with no '*hh' is refused, not
// tolerated, and so is one whose sum is wrong: on a serial line at 9600 baud
// with a loose cable, a corrupted digit in a latitude is a receiver placed in
// the next county, and the checksum is the only thing standing between that
// corruption and applyReceiverPosition. The specification makes the checksum
// optional for some sentences; for a position that will be persisted and
// measured from, it is not optional here.
//
// EMPTY FIELDS MEAN NO FIX, NEVER 0,0. A receiver that has not yet found the
// sky sends "$GPGGA,123519,,,,,0,00,,,M,,M,,*hh" - the position fields empty
// and quality 0. Parsing those empties as 0.0 would produce exactly the pair
// (0 N, 0 E) that receiverPositionAcceptable exists to refuse (a user's scope
// was once found measuring from the Gulf of Guinea). The parser reports an
// absent coordinate as NaN and valid == false, and the fix is valid ONLY when
// the sentence itself declares a MEASURED one: GGA quality 1 to 5 (GPS,
// differential, PPS, RTK fixed, RTK float), RMC status 'A', GLL status 'A'.
// A position with status 'V' (void) is a receiver's dead-reckoned guess and
// is reported with valid == false so the reader waits for better - and so is
// GGA quality 6, which is the SAME receiver state under the other sentence's
// name ("estimated"): a dead-reckoning receiver carried indoors keeps
// emitting quality 6 with a drifting position and no satellites, and the
// first build took the first of those as the antenna's fixed position.
// Quality 7 (manual input) and 8 (simulator) are not measurements either.
// The quality field is still reported verbatim so the status line can say
// what the receiver is doing.
//
// COORDINATES. NMEA writes latitude as ddmm.mmmm and longitude as dddmm.mmmm,
// each followed by a hemisphere letter in the NEXT field. Degrees are the
// integer part before the last two digits of the integer minutes; minutes
// (with their fraction) divide by 60; S and W negate. "4807.038,N" is
// 48 deg 07.038 min = 48.1173 N. A field with fewer than three digits before
// the point, a non-digit, or a hemisphere letter other than N/S/E/W is
// malformed and the sentence is refused whole - half a position is worse than
// none.
//
// THE LINE ASSEMBLER takes bytes in whatever chunks a port hands over -
// fragments of a sentence, several sentences at once, CR, LF or CRLF endings,
// binary noise before the first '$' (a receiver mid-sentence at open, or a
// device set to a binary protocol at the wrong baud) - and yields complete
// sentences without their line ending. Two bounds keep it honest: a candidate
// line that reaches kMaxSentenceBytes with no line ending is dropped and
// counted (a stream with no line endings, such as the wrong baud producing
// framing garbage, would otherwise grow one "line" forever), and the buffer
// never holds more than kBufferBytes, so no input pattern can make it
// allocate without limit. The bound is NOT the specification's 82: real
// receivers exceed that (see kMaxSentenceBytes), and the bound exists to
// stop garbage growing, not to enforce a standard the receivers do not keep.
//
// THE CHECKSUM TRAILER may be followed by padding. "*hh" is the end of the
// sentence, but a logger replay, a serial adapter or a receiver that pads
// its lines can leave a space, a tab or a NUL between the two hex digits
// and the CR/LF. Those bytes are outside the checksum and carry nothing, so
// they are tolerated; anything else after the digits (a third digit, a
// comma, another field) is not a trailer and the sentence is refused.
// Without this a stream that is entirely readable counts every line as
// NoChecksum and tells the user to check the baud rate.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_NMEA_HPP
#define CASCADE_CORE_NMEA_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace cascade::core {

// Which position sentence a fix came from. Kept on the fix so the GUI and the
// log can say "GGA" rather than "a sentence", and so a test can assert that a
// GLL was parsed as a GLL.
enum class NmeaSentence { GGA, RMC, GLL };

// One parsed position sentence.
//
// `valid` is the receiver's OWN claim of a measured fix (GGA quality 1..5,
// RMC/GLL status 'A') AND both coordinates present. It is deliberately NOT the
// application's acceptance rule: a receiver may honestly report a valid fix
// at (0,0) on a test bench, and whether the application will take a position
// is decided once, by receiverPositionAcceptable, in the reader - not here.
struct NmeaFix {
    static constexpr double kAbsent = std::numeric_limits<double>::quiet_NaN();

    NmeaSentence sentence = NmeaSentence::GGA;
    std::string talker;         // "GP", "GN", ... exactly as sent, two letters

    bool valid = false;         // the sentence declared a fix and carried a position
    double latDeg = kAbsent;    // signed decimal degrees, N positive; NaN when absent
    double lonDeg = kAbsent;    // signed decimal degrees, E positive; NaN when absent

    // GGA only. Altitude above mean sea level in metres; NaN when absent or
    // when the sentence is not a GGA. Persisted nowhere and never logged - it
    // is shown to the user as a sign the receiver is doing real work.
    double altitudeM = kAbsent;

    // GGA: the quality field verbatim (0 = no fix, 1 = GPS, 2 = DGPS, 3 =
    // PPS, 4/5 = RTK, 6 = estimated, 7 = manual, 8 = simulator). Only 1..5
    // make `valid` true; the rest are reported here so the status line can
    // still say what the receiver is doing. RMC and GLL have no such field
    // and report -1.
    int fixQuality = -1;

    // GGA: satellites in use; -1 when the field is empty or the sentence is
    // not a GGA. Shown on the status line so a user staring at "no fix yet"
    // can see the count climbing, which is the difference between "wait" and
    // "move the antenna".
    int satellites = -1;

    // GGA: horizontal dilution of precision; NaN when absent. 1.0 is
    // excellent, 5.0 is a window sill, 20 is a fix not worth having.
    double hdop = kAbsent;

    // UTC time of the fix as seconds since midnight (hhmmss.ss decoded), NaN
    // when the field is empty. All three sentences carry it.
    double utcSecondsOfDay = kAbsent;
};

// What parseNmeaSentence decided about one line.
enum class NmeaStatus {
    Ok,             // a GGA, RMC or GLL was understood; read `fix` (valid may still be false)
    Ignored,        // checksum fine, but not a position sentence (GSV, VTG, proprietary...)
    NoChecksum,     // no "*hh" trailer - refused, see the header comment
    BadChecksum,    // "*hh" present and wrong - line corrupted in transit
    Malformed,      // checksum fine, but the fields are not what the sentence type promises
};

// Running totals for the GUI's status line and the log. Everything here is a
// count; nothing here can identify a place.
struct NmeaSummary {
    std::uint64_t sentences = 0;          // lines whose checksum verified (Ok + Ignored + Malformed)
    std::uint64_t positionSentences = 0;  // of those, GGA/RMC/GLL that parsed (Ok)
    std::uint64_t validFixes = 0;         // of those, with valid == true
    std::uint64_t checksumFailures = 0;   // NoChecksum + BadChecksum
    std::uint64_t malformed = 0;
    std::uint64_t ignored = 0;

    // Folds one parse result in. Written once here so the reader and any
    // future replay tool count the same way.
    void record(NmeaStatus status, const NmeaFix& fix);
};

// --- checksum ------------------------------------------------------------------

// The XOR of every byte of `payload`, which is the text BETWEEN '$' and '*'
// (neither included). Exposed so a test - or a simulator - can frame its own
// sentences with nmeaFrame() below instead of hand-computing hex.
std::uint8_t nmeaChecksum(std::string_view payload);

// "$" + payload + "*" + two uppercase hex digits. No line ending: the caller
// chooses CR, LF or CRLF, because the assembler must be shown all three.
std::string nmeaFrame(std::string_view payload);

// True only for a sentence of the form "$...*hh" whose hh matches. Leading
// '$' required; a trailing CR/LF is tolerated (the assembler strips them, but
// a test feeding raw lines should not have to). Case of the hex is not
// significant: receivers emit uppercase, but the specification does not say
// they must.
bool nmeaChecksumValid(std::string_view sentence);

// --- coordinates ---------------------------------------------------------------

// "ddmm.mmmm" (latitude) or "dddmm.mmmm" (longitude) plus its hemisphere
// letter to signed decimal degrees. Returns false, leaving `outDeg` untouched,
// for an empty field, a field with fewer than three digits before the point,
// any non-digit where a digit belongs, minutes >= 60, a result beyond 90/180,
// or a hemisphere letter that is not N/S (latitude) or E/W (longitude).
// `isLongitude` selects the letter set and the range; it does not change the
// digit rule, because receivers pad longitude to three degree digits but the
// parser must not depend on a receiver's formatting for correctness.
bool nmeaParseCoordinate(std::string_view field, char hemisphere, bool isLongitude,
                         double& outDeg);

// --- sentences -----------------------------------------------------------------

// One complete sentence (as the assembler yields it: starts with '$', no line
// ending, though a trailing CR/LF is tolerated) to a fix.
//
// The talker is the two characters after '$'; the sentence type is the next
// three. GGA, RMC and GLL from any talker fill `fix` and return Ok. `fix` is
// fully assigned on Ok and left untouched on anything else, so a caller that
// keeps the last good fix is not handed a half-written one by a bad line.
//
// Field rules, in the order they are checked:
//   GGA  $ttGGA,hhmmss.ss,lat,N,lon,E,quality,sats,hdop,alt,M,geoid,M,age,ref
//        quality 1..5 AND lat/lon present => valid; 0, 6 (estimated), 7
//        (manual), 8 (simulator) => valid false. Empty lat/lon with any
//        quality => valid false, coordinates NaN (a receiver still searching).
//   RMC  $ttRMC,hhmmss.ss,A,lat,N,lon,E,speed,course,ddmmyy,var,E,mode
//        status 'A' AND lat/lon present => valid; 'V' => valid false.
//   GLL  $ttGLL,lat,N,lon,E,hhmmss.ss,A,mode
//        status 'A' AND lat/lon present => valid; 'V' or absent => false.
// Trailing fields a newer receiver adds (RMC navigational status, GGA
// extensions) are ignored; too FEW fields to reach the status/quality field
// is Malformed. A coordinate field that is present but unparseable is
// Malformed - the whole sentence, not one half of it.
NmeaStatus parseNmeaSentence(std::string_view sentence, NmeaFix& fix);

// --- line assembly -------------------------------------------------------------

// Bytes in, sentences out. See the header comment for what it tolerates.
//
// The buffer holds at most kBufferBytes; a candidate line that reaches
// kMaxSentenceBytes without a line ending is discarded, `overlongDropped`
// counts it, and scanning resumes at the next '$'. Bytes before the first '$'
// (or between a dropped line and the next '$') are discarded and counted in
// `noiseBytes`. Neither counter is a fault by itself: one burst of either at
// open is normal (the port was opened mid-sentence). A stream that is ALL
// noise is the wrong baud rate, and the status line says so from the counts.
class NmeaLineAssembler {
public:
    // NMEA 0183 caps a sentence at 82 characters including '$' and CRLF -
    // and the receivers a beta tester is likely to own do not keep to it. A
    // u-blox ZED-F9P or M9 in high-precision mode writes seven decimals of
    // minutes and its interface description says that mode cannot be
    // combined with "Limit82"; its GGA is 84 characters framed. gpsd records
    // a Trimble BX-960 emitting a 91-character GGA and sets its own limit to
    // 130. The first build cut at 82, so a receiver set to GGA-only (the
    // usual RTK configuration) never yielded a line, the reader timed out,
    // and the status line blamed the baud rate. 128 takes every real
    // sentence with room to spare, keeps the buffer bound below, and is
    // still short enough that a wrong-baud stream is cut every few dozen
    // bytes rather than never. The bound exists to stop garbage growing,
    // not to enforce the standard.
    static constexpr std::size_t kMaxSentenceBytes = 128;
    // Room for a full sentence plus the fragment of the next one, and no
    // more. The bound is what makes an endless line ending-free stream cost
    // a fixed 256 bytes instead of the heap.
    static constexpr std::size_t kBufferBytes = 256;

    // Appends every sentence completed by this chunk to `outLines`, in order,
    // each starting with '$' and stripped of its CR/LF. A chunk may complete
    // zero, one or several. `outLines` is never cleared here, so a caller can
    // accumulate across reads.
    void feed(const char* bytes, std::size_t count, std::vector<std::string>& outLines);

    // Forgets any partial line. The reader calls this when a port is
    // (re)opened so the tail of one session cannot prefix the first sentence
    // of the next.
    void reset();

    std::uint64_t overlongDropped() const { return overlongDropped_; }
    std::uint64_t noiseBytes() const { return noiseBytes_; }

private:
    std::string pending_;     // bytes of the sentence in progress, '$' first
    bool inSentence_ = false; // a '$' has been seen and not yet ended
    std::uint64_t overlongDropped_ = 0;
    std::uint64_t noiseBytes_ = 0;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_NMEA_HPP
