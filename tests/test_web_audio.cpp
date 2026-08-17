// Tests for the browser audio ring (net/web_audio.hpp).
//
// The interesting behaviour is what happens to a reader that falls behind,
// because that is the case a live stream actually meets — a backgrounded tab,
// a slow link — and the case a ring written naively corrupts silently by
// handing back a mixture of new and stale samples.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <vector>

#include "net/web_audio.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

// A ramp whose value identifies its absolute position, so a test can tell
// exactly which samples came back rather than only how many.
std::vector<float> ramp(std::uint64_t from, std::size_t n) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<float>(from + i);
    }
    return v;
}

bool isRampFrom(const std::vector<float>& v, std::size_t n, std::uint64_t from) {
    for (std::size_t i = 0; i < n; ++i) {
        if (v[i] != static_cast<float>(from + i)) {
            return false;
        }
    }
    return true;
}

void testEmptyRing() {
    AudioRing ring(100);
    CHECK(ring.written() == 0);
    CHECK(ring.available() == 0);
    CHECK(ring.capacity() == 100);

    std::vector<float> out(10, -1.0f);
    std::uint64_t cursor = 0;
    std::uint64_t dropped = 99;
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 0);
    CHECK(cursor == 0);
    CHECK(dropped == 0);
}

void testSequentialReads() {
    AudioRing ring(100);
    const std::vector<float> a = ramp(0, 10);
    ring.write(a.data(), a.size());
    CHECK(ring.written() == 10);
    CHECK(ring.available() == 10);

    std::vector<float> out(64, -1.0f);
    std::uint64_t cursor = 0;
    std::uint64_t dropped = 0;
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 10);
    CHECK(dropped == 0);
    CHECK(cursor == 10);
    CHECK(isRampFrom(out, 10, 0));

    // Nothing new yet.
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 0);
    CHECK(cursor == 10);

    // More arrives; the reader continues exactly where it stopped, with no
    // repeat and no gap.
    const std::vector<float> b = ramp(10, 5);
    ring.write(b.data(), b.size());
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 5);
    CHECK(cursor == 15);
    CHECK(dropped == 0);
    CHECK(isRampFrom(out, 5, 10));
}

void testPartialReadRespectsMax() {
    AudioRing ring(100);
    const std::vector<float> a = ramp(0, 50);
    ring.write(a.data(), a.size());

    std::vector<float> out(8, -1.0f);
    std::uint64_t cursor = 0;
    std::uint64_t dropped = 0;
    // A reader with a small buffer gets its buffer's worth and keeps its place.
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 8);
    CHECK(cursor == 8);
    CHECK(isRampFrom(out, 8, 0));
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 8);
    CHECK(cursor == 16);
    CHECK(isRampFrom(out, 8, 8));
}

void testWrapAround() {
    // Write well past the capacity so the data physically wraps, and confirm a
    // reader kept up throughout still sees one unbroken ramp.
    AudioRing ring(64);
    std::uint64_t cursor = 0;
    std::uint64_t dropped = 0;
    std::uint64_t expect = 0;
    std::vector<float> out(32, -1.0f);

    for (int round = 0; round < 20; ++round) {
        const std::vector<float> chunk = ramp(expect + 0, 10);
        // Each chunk continues the global ramp.
        std::vector<float> c(10);
        for (std::size_t i = 0; i < 10; ++i) {
            c[i] = static_cast<float>(expect + i);
        }
        (void)chunk;
        ring.write(c.data(), c.size());

        const std::size_t got = ring.read(cursor, out.data(), out.size(), dropped);
        CHECK(got == 10);
        CHECK(dropped == 0);
        CHECK(isRampFrom(out, got, expect));
        expect += got;
    }
    CHECK(cursor == expect);
    CHECK(ring.written() == expect);
}

void testOverrunResyncsAndReports() {
    AudioRing ring(64);
    std::uint64_t cursor = 0;
    std::uint64_t dropped = 0;

    // The reader takes nothing while 200 samples go by: its cursor at 0 is
    // long gone.
    const std::vector<float> a = ramp(0, 200);
    ring.write(a.data(), a.size());
    CHECK(ring.written() == 200);
    CHECK(ring.available() == 64);

    std::vector<float> out(64, -1.0f);
    const std::size_t got = ring.read(cursor, out.data(), out.size(), dropped);
    CHECK(got == 64);
    // 200 written, 64 held, so the oldest surviving sample is 136 and the
    // reader lost exactly that many.
    CHECK(dropped == 136);
    CHECK(cursor == 200);
    CHECK(isRampFrom(out, got, 136));

    // Having resynced, it is up to date and continues cleanly.
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 0);
    CHECK(dropped == 0);
    const std::vector<float> b = ramp(200, 5);
    ring.write(b.data(), b.size());
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 5);
    CHECK(dropped == 0);
    CHECK(isRampFrom(out, 5, 200));
}

void testWriteLargerThanCapacity() {
    AudioRing ring(16);
    const std::vector<float> a = ramp(0, 100);
    ring.write(a.data(), a.size());
    CHECK(ring.written() == 100);
    CHECK(ring.available() == 16);

    std::vector<float> out(16, -1.0f);
    std::uint64_t cursor = 0;
    std::uint64_t dropped = 0;
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 16);
    // Only the newest 16 survive: 84..99.
    CHECK(isRampFrom(out, 16, 84));
    CHECK(dropped == 84);
    CHECK(cursor == 100);
}

void testIndependentCursors() {
    // Two browsers listening must not disturb each other.
    AudioRing ring(100);
    const std::vector<float> a = ramp(0, 30);
    ring.write(a.data(), a.size());

    std::uint64_t c1 = 0;
    std::uint64_t c2 = 0;
    std::uint64_t dropped = 0;
    std::vector<float> out(30, -1.0f);

    CHECK(ring.read(c1, out.data(), 10, dropped) == 10);
    CHECK(isRampFrom(out, 10, 0));
    CHECK(c1 == 10);
    CHECK(c2 == 0);  // untouched

    CHECK(ring.read(c2, out.data(), 30, dropped) == 30);
    CHECK(isRampFrom(out, 30, 0));  // the second reader still gets everything
    CHECK(c2 == 30);

    CHECK(ring.read(c1, out.data(), 30, dropped) == 20);
    CHECK(isRampFrom(out, 20, 10));
}

void testResetInvalidatesCursors() {
    AudioRing ring(100);
    const std::vector<float> a = ramp(0, 50);
    ring.write(a.data(), a.size());
    std::uint64_t cursor = 0;
    std::uint64_t dropped = 0;
    std::vector<float> out(64, -1.0f);
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 50);
    CHECK(cursor == 50);

    ring.reset();
    CHECK(ring.written() == 0);
    CHECK(ring.available() == 0);

    // A cursor from before the reset now points past the end. It must not read
    // uninitialised memory; it resyncs and reports nothing available.
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 0);
    CHECK(cursor == 0);
}

void testNewReaderStartsAtLive() {
    // The documented way to join without replaying the buffer.
    AudioRing ring(100);
    const std::vector<float> a = ramp(0, 80);
    ring.write(a.data(), a.size());

    std::uint64_t cursor = ring.written();
    std::uint64_t dropped = 0;
    std::vector<float> out(64, -1.0f);
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 0);
    CHECK(dropped == 0);

    const std::vector<float> b = ramp(80, 7);
    ring.write(b.data(), b.size());
    CHECK(ring.read(cursor, out.data(), out.size(), dropped) == 7);
    CHECK(isRampFrom(out, 7, 80));
}

void testPcm16Conversion() {
    // Endianness, rounding and the two saturation ends, against values chosen
    // so the expected bytes can be written down rather than computed by the
    // same code under test.
    const float in[] = {0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 0.5f, -0.5f};
    std::vector<std::uint8_t> out(2 * (sizeof(in) / sizeof(in[0])), 0xAA);
    floatToPcm16le(in, sizeof(in) / sizeof(in[0]), out.data());

    auto sampleAt = [&out](std::size_t i) {
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(out[2 * i]) |
                                         (static_cast<std::uint16_t>(out[2 * i + 1]) << 8));
    };

    CHECK(sampleAt(0) == 0);
    // Full scale positive must be +32767, NOT +32768 wrapping to -32768.
    CHECK(sampleAt(1) == 32767);
    CHECK(sampleAt(2) == -32767);
    // Beyond full scale clips rather than wrapping — a wrap would turn the
    // loudest sample into its opposite polarity, which is an audible click.
    CHECK(sampleAt(3) == 32767);
    CHECK(sampleAt(4) == -32767);
    CHECK(sampleAt(5) == 16384);   // lround(0.5 * 32767) = 16384
    CHECK(sampleAt(6) == -16384);

    // Little-endian byte order, checked explicitly on a value whose two bytes
    // differ.
    const float one[] = {0.5f};
    std::uint8_t two[2] = {0, 0};
    floatToPcm16le(one, 1, two);
    CHECK(two[0] == 0x00);
    CHECK(two[1] == 0x40);  // 16384 = 0x4000

    // NaN must be silenced, not turned into an arbitrary 16-bit pattern.
    const float nan[] = {std::nanf("")};
    std::uint8_t nanOut[2] = {0xAA, 0xAA};
    floatToPcm16le(nan, 1, nanOut);
    CHECK(nanOut[0] == 0);
    CHECK(nanOut[1] == 0);
}

}  // namespace

int main() {
    testEmptyRing();
    testSequentialReads();
    testPartialReadRespectsMax();
    testWrapAround();
    testOverrunResyncsAndReports();
    testWriteLargerThanCapacity();
    testIndependentCursors();
    testResetInvalidatesCursors();
    testNewReaderStartsAtLive();
    testPcm16Conversion();
    return testSummary("test_web_audio");
}
