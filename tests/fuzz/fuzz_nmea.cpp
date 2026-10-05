// Fuzz target: the GPS receiver's NMEA 0183 stream.
//
// Bytes arrive from a serial port in whatever chunks the driver delivers them,
// from a device the user plugged in - a real GPS, a clone, a different gadget
// on the same port number, or noise from a wrong baud rate. The line assembler
// finds sentences in that stream and parseNmeaSentence reads them; both are our
// code and both run on a worker thread for as long as the port is open.
//
// The input is fed to the assembler in chunks whose sizes it also dictates (so
// a sentence is split at every possible place), each assembled line is parsed,
// and the whole input is parsed once as a single sentence.
//
// Properties: a position the parser calls valid is a real position (latitude
// within +-90, longitude within +-180); the assembler never hands out a line
// longer than its documented bound; the summary counters add up.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/nmea.hpp"
#include "fuzz_common.hpp"

using namespace cascade::core;

namespace {

void checkFix(const NmeaFix& fix) {
    if (!fix.valid) { return; }
    FUZZ_REQUIRE(std::isfinite(fix.latDeg) && std::fabs(fix.latDeg) <= 90.0);
    FUZZ_REQUIRE(std::isfinite(fix.lonDeg) && std::fabs(fix.lonDeg) <= 180.0);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    const std::size_t chunk = 1 + in.u8() % 40;
    const std::string stream = in.rest();

    NmeaSummary summary;
    NmeaLineAssembler assembler;
    std::vector<std::string> lines;
    for (std::size_t at = 0; at < stream.size(); at += chunk) {
        const std::size_t n = std::min(chunk, stream.size() - at);
        assembler.feed(stream.data() + at, n, lines);
    }
    for (const std::string& line : lines) {
        FUZZ_REQUIRE(line.size() <= NmeaLineAssembler::kMaxSentenceBytes + 8);
        NmeaFix fix;
        const NmeaStatus status = parseNmeaSentence(line, fix);
        summary.record(status, fix);
        if (status == NmeaStatus::Ok) { checkFix(fix); }
    }
    FUZZ_REQUIRE(summary.positionSentences <= summary.sentences);
    FUZZ_REQUIRE(summary.validFixes <= summary.positionSentences);

    NmeaFix whole;
    if (parseNmeaSentence(stream, whole) == NmeaStatus::Ok) { checkFix(whole); }

    double deg = 0.0;
    const std::size_t split = stream.size() / 2;
    (void)nmeaParseCoordinate(std::string_view(stream).substr(0, split),
                              split < stream.size() ? stream[split] : 'N', (chunk & 1) != 0, deg);
    (void)nmeaChecksumValid(stream);
    return 0;
}
