// test_sweep_tinysa.cpp - the tinySA "scanraw" exchange (source/tinysa_protocol.hpp)
// proven against a FAKE serial link fed frames written here from the one sentence
// of the maker's page the format comes from:
//   '{' ('x' LSB MSB)*points '}'   dBm = raw / 32 - 128 (tinySA) or - 174 (Ultra)
// NO real serial port is opened anywhere in this file (isValidComPortName only
// looks at a string). Not tested on hardware: none is on the desk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "core/sweep_line.hpp"
#include "source/serial_port_win.hpp"
#include "source/tinysa_protocol.hpp"
#include "test_check.hpp"

using namespace cascade::source::tinysa;
using cascade::core::SweepLine;

namespace {

using Bytes = std::vector<std::uint8_t>;

std::uint16_t rawFor(double dbm, Model m) {
    return static_cast<std::uint16_t>(std::lround((dbm + levelOffsetDb(m)) * 32.0));
}

// The frame, written from the page's sentence: '{', then 'x' LSB MSB per point, then '}'.
Bytes makeFrame(const std::vector<std::uint16_t>& raw, const std::string& prefix = {}) {
    Bytes b(prefix.begin(), prefix.end());
    b.push_back('{');
    for (std::uint16_t r : raw) {
        b.push_back('x');
        b.push_back(static_cast<std::uint8_t>(r & 0xFF));
        b.push_back(static_cast<std::uint8_t>(r >> 8));
    }
    b.push_back('}');
    return b;
}

std::vector<std::uint16_t> ramp(std::size_t n, Model m, double fromDbm = -100.0, double stepDb = 0.1) {
    std::vector<std::uint16_t> v;
    for (std::size_t i = 0; i < n; ++i) { v.push_back(rawFor(fromDbm + stepDb * static_cast<double>(i), m)); }
    return v;
}

// A link that records writes and replays canned reads; an empty chunk is a timeout (0).
struct FakeLink : SerialLink {
    Bytes written;
    std::deque<Bytes> reads;
    bool failWrite = false;
    bool failRead = false;
    std::size_t readCalls = 0;
    bool write(const std::uint8_t* d, std::size_t n) override {
        if (failWrite) { return false; }
        written.insert(written.end(), d, d + n);
        return true;
    }
    long read(std::uint8_t* buf, std::size_t cap) override {
        ++readCalls;
        if (failRead) { return -1; }
        if (reads.empty()) { return 0; }
        Bytes c = std::move(reads.front());
        reads.pop_front();
        const std::size_t n = std::min(c.size(), cap);
        std::memcpy(buf, c.data(), n);
        return static_cast<long>(n);
    }
};

void chunked(FakeLink& l, const Bytes& all, std::size_t chunk) {
    for (std::size_t i = 0; i < all.size(); i += chunk) {
        l.reads.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(i),
                             all.begin() + static_cast<std::ptrdiff_t>(std::min(all.size(), i + chunk)));
    }
}

SweepRequest req(std::size_t pts, Model m = Model::TinySaUltra) {
    SweepRequest r;
    r.startHz = 100e6;
    r.stopHz = 120e6;
    r.points = pts;
    r.model = m;
    r.timeS = 12.5;
    return r;
}

}  // namespace

int main() {
    // ---- offsets and the command ----
    CHECK(levelOffsetDb(Model::TinySa) == 128.0);
    CHECK(levelOffsetDb(Model::TinySaUltra) == 174.0);
    CHECK(std::isnan(levelOffsetDb(Model::Unknown)));
    {
        std::string why;
        const std::string c = scanrawCommand(100e6, 120e6, 450, 1, &why);
        CHECK(c == "scanraw 100000000 120000000 450 1\r\n");
        CHECK(scanrawCommand(120e6, 100e6, 450, 1, &why).empty() && why == "start must be below stop");
        CHECK(scanrawCommand(100e6, 100e6, 450, 1, &why).empty());
        CHECK(scanrawCommand(100e6, 120e6, 1, 1, &why).empty() && why == "points out of range");
        CHECK(scanrawCommand(100e6, 120e6, 0, 1, &why).empty());
        CHECK(scanrawCommand(100e6, 120e6, cascade::core::kSweepMaxPointsPerLine + 1, 1, &why).empty());
        CHECK(scanrawCommand(100e6, 120e6, 450, 4, &why).empty() && why == "option must be 0..3");
        CHECK(scanrawCommand(100e6, 120e6, 450, -1, &why).empty());
        CHECK(scanrawCommand(-1.0, 120e6, 450, 1, &why).empty());
        CHECK(scanrawCommand(std::nan(""), 120e6, 450, 1, &why).empty());
        CHECK(scanrawCommand(100e6, 2e12, 450, 1, &why).empty());
        CHECK(scanrawCommand(100.2, 100.4, 450, 1, &why).empty());   // both round to 100 Hz
    }

    // ---- happy path, whole and byte-by-byte, with an echo and a prompt before the '{' ----
    {
        const auto raw = ramp(450, Model::TinySaUltra);
        const std::string echo = "scanraw 100000000 120000000 450 1\r\nch> ";
        const Bytes frame = makeFrame(raw, echo);
        for (std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{64}, frame.size()}) {
            ScanrawParser p(450);
            FrameStatus st = FrameStatus::NeedMore;
            for (std::size_t i = 0; i < frame.size(); i += chunk) {
                st = p.feed(frame.data() + i, std::min(chunk, frame.size() - i));
            }
            CHECK(st == FrameStatus::Done);
            CHECK(p.raw() == raw);
            CHECK(p.discardedBytes() == echo.size());   // every byte of echo and prompt
        }
        SweepLine line;
        std::string why;
        CHECK(decodeScanraw(raw, Model::TinySaUltra, 100e6, 120e6, line, &why));
        CHECK(line.db.size() == 450);
        if (line.db.size() != 450) { return testSummary("test_sweep_tinysa"); }
        CHECK_NEAR(line.db[0], -100.0, 1.0 / 32.0);
        CHECK_NEAR(line.db[449], -100.0 + 0.1 * 449.0, 1.0 / 32.0);
        CHECK(line.startHz == 100e6 && line.stopHz == 120e6);
        // the same raw words on the other model are 46 dB apart (174 - 128)
        SweepLine other;
        CHECK(decodeScanraw(raw, Model::TinySa, 100e6, 120e6, other, &why));
        if (other.db.size() != 450) { return testSummary("test_sweep_tinysa"); }
        CHECK_NEAR(other.db[10] - line.db[10], 46.0, 1e-4);
    }

    // ---- data bytes that look like framing ('{' 0x7B, 'x' 0x78, '}' 0x7D) are data ----
    {
        std::vector<std::uint16_t> raw = {0x7D7D, 0x787B, 0x7B78, 0x0000, 0x7D00, 0x007D, 0x0078, 0x007B};
        const Bytes frame = makeFrame(raw);
        ScanrawParser p(raw.size());
        CHECK(p.feed(frame.data(), frame.size()) == FrameStatus::Done);
        CHECK(p.raw() == raw);
    }

    // ---- refusals ----
    {
        const auto raw = ramp(10, Model::TinySaUltra);
        // fewer points than asked: '}' arrives early
        { ScanrawParser p(11); Bytes f = makeFrame(raw); CHECK(p.feed(f.data(), f.size()) == FrameStatus::BadLength); }
        // more points than asked: an 'x' after the asked number
        { ScanrawParser p(9); Bytes f = makeFrame(raw); CHECK(p.feed(f.data(), f.size()) == FrameStatus::BadLength); }
        // garbage where a point must start
        { ScanrawParser p(10); Bytes f = makeFrame(raw); f[1 + 3 * 4] = 'q'; CHECK(p.feed(f.data(), f.size()) == FrameStatus::BadFraming); }
        // a frame cut off mid-point and with no closing brace
        { ScanrawParser p(10); Bytes f = makeFrame(raw); f.resize(f.size() - 4);
          CHECK(p.feed(f.data(), f.size()) == FrameStatus::NeedMore); CHECK(p.finish() == FrameStatus::Truncated); }
        // nothing at all
        { ScanrawParser p(10); CHECK(p.finish() == FrameStatus::Truncated); }
        // only an echo, never a '{'
        { ScanrawParser p(10); const std::string e = "scanraw 1 2 3\r\nch> "; CHECK(p.feed(reinterpret_cast<const std::uint8_t*>(e.data()), e.size()) == FrameStatus::NeedMore);
          CHECK(p.finish() == FrameStatus::Truncated); }
        // a terminal state sticks until reset
        { ScanrawParser p(10); Bytes f = makeFrame(raw); CHECK(p.feed(f.data(), f.size()) == FrameStatus::Done);
          Bytes more = makeFrame(raw); CHECK(p.feed(more.data(), more.size()) == FrameStatus::Done); CHECK(p.raw().size() == 10);
          p.reset(); CHECK(p.status() == FrameStatus::NeedMore && p.raw().empty());
          CHECK(p.feed(f.data(), f.size()) == FrameStatus::Done); }
        // decode refusals
        SweepLine l; std::string why;
        CHECK(!decodeScanraw(raw, Model::Unknown, 1e6, 2e6, l, &why) && why == "model not named");
        CHECK(!decodeScanraw({5}, Model::TinySa, 1e6, 2e6, l, &why));
        CHECK(!decodeScanraw({}, Model::TinySa, 1e6, 2e6, l, &why));
        CHECK(!decodeScanraw({100, 65535}, Model::TinySa, 1e6, 2e6, l, &why));   // 1920 dBm
        CHECK(!decodeScanraw({100, 6000}, Model::TinySa, 1e6, 2e6, l, &why));     // 6000/32 - 128 = +59.5 dBm, above the bound
        CHECK(decodeScanraw({100, 4000}, Model::TinySa, 1e6, 2e6, l, &why));      // 4000/32 - 128 = -3 dBm
        CHECK(!decodeScanraw({100, 100}, Model::TinySa, 2e6, 1e6, l, &why));      // stop below start
    }

    // ---- sweepOnce through the fake link ----
    {
        FakeLink link;
        chunked(link, makeFrame(ramp(450, Model::TinySaUltra), "ch> "), 61);
        SweepLine out; std::string why;
        CHECK(sweepOnce(link, req(450), out, &why) == SweepResult::Ok);
        CHECK(out.db.size() == 450 && out.timeS == 12.5);
        CHECK(std::string(link.written.begin(), link.written.end()) == "scanraw 100000000 120000000 450 1\r\n");
    }
    {   // silence: times out after the idle reads, not before
        FakeLink link; SweepLine out; std::string why;
        SweepRequest r = req(450); r.maxIdleReads = 5;
        CHECK(sweepOnce(link, r, out, &why) == SweepResult::TimedOut && why == "no reply");
        CHECK(link.readCalls == 6);
        CHECK(out.db.empty());
    }
    {   // cut short: half a frame then silence
        FakeLink link; Bytes f = makeFrame(ramp(450, Model::TinySaUltra)); f.resize(f.size() / 2); chunked(link, f, 100);
        SweepLine out; std::string why;
        CHECK(sweepOnce(link, req(450), out, &why) == SweepResult::TimedOut && why == "reply cut short");
    }
    {   // wrong count, garbage, wrong model, implausible
        SweepLine out; std::string why;
        { FakeLink l; chunked(l, makeFrame(ramp(449, Model::TinySaUltra)), 50); CHECK(sweepOnce(l, req(450), out, &why) == SweepResult::Malformed); }
        { FakeLink l; Bytes f = makeFrame(ramp(450, Model::TinySaUltra)); f[100] = '!'; chunked(l, f, 50); CHECK(sweepOnce(l, req(450), out, &why) == SweepResult::Malformed); }
        { FakeLink l; chunked(l, makeFrame(ramp(450, Model::TinySaUltra)), 50); CHECK(sweepOnce(l, req(450, Model::Unknown), out, &why) == SweepResult::BadRequest); CHECK(l.written.empty()); }
        { FakeLink l; auto raw = ramp(450, Model::TinySaUltra); raw[7] = 65000; chunked(l, makeFrame(raw), 50); CHECK(sweepOnce(l, req(450), out, &why) == SweepResult::Refused); CHECK(out.db.empty()); }
        { FakeLink l; SweepRequest r = req(450); r.stopHz = r.startHz; CHECK(sweepOnce(l, r, out, &why) == SweepResult::BadRequest); CHECK(l.written.empty()); }
        { FakeLink l; l.failWrite = true; CHECK(sweepOnce(l, req(450), out, &why) == SweepResult::WriteFailed); }
        { FakeLink l; l.failRead = true; CHECK(sweepOnce(l, req(450), out, &why) == SweepResult::LinkFailed); }
        { // a device that never stops: endless garbage before any '{' still ends
          struct Endless : SerialLink {
              bool write(const std::uint8_t*, std::size_t) override { return true; }
              long read(std::uint8_t* b, std::size_t c) override { std::memset(b, 'z', c); return 1; }
          } e;
          CHECK(sweepOnce(e, req(450), out, &why) == SweepResult::TimedOut);
        }
    }

    // ---- COM port names: a string check only, no port is touched ----
    CHECK(isValidComPortName("COM3") && isValidComPortName("COM256") && isValidComPortName("\\\\.\\COM12"));
    CHECK(!isValidComPortName("") && !isValidComPortName("COM") && !isValidComPortName("COM0") &&
          !isValidComPortName("COM257") && !isValidComPortName("COM03") && !isValidComPortName("C:\\x") &&
          !isValidComPortName("\\\\.\\PhysicalDrive0") && !isValidComPortName("com3") && !isValidComPortName("COM3 "));
    {
        WinSerialLink wl;   // not opened: every call refuses
        std::uint8_t b = 0;
        CHECK(!wl.isOpen() && !wl.write(&b, 1) && wl.read(&b, 1) == -1);
        CHECK(!wl.open("not a port"));   // refused by the name check before CreateFile
        CHECK(!wl.isOpen());
    }

    // ---- fuzz: random and mutated bytes in random chunks; the parser never faults and its
    //      invariants hold ----
    {
        std::mt19937 rng(0xC0FFEEu);
        const Bytes good = makeFrame(ramp(60, Model::TinySa), "ch> ");
        std::size_t done = 0, bad = 0, trunc = 0;
        for (int it = 0; it < 30000; ++it) {
            Bytes b;
            if (rng() % 3 == 0) {
                b.resize(rng() % 300);
                for (auto& x : b) { x = static_cast<std::uint8_t>(rng()); }
            } else {
                b = good;
                const int flips = static_cast<int>(rng() % 4);
                for (int k = 0; k < flips && !b.empty(); ++k) { b[rng() % b.size()] = static_cast<std::uint8_t>(rng()); }
                if (rng() % 4 == 0 && !b.empty()) { b.resize(rng() % b.size()); }
            }
            const std::size_t expect = (rng() % 3 == 0) ? 60 : (rng() % 200);
            ScanrawParser p(expect);
            std::size_t at = 0;
            while (at < b.size()) {
                const std::size_t n = std::min<std::size_t>(1 + rng() % 40, b.size() - at);
                p.feed(b.data() + at, n);
                at += n;
            }
            const FrameStatus st = p.finish();
            CHECK(p.raw().size() <= expect);
            if (st == FrameStatus::Done) { ++done; CHECK(p.raw().size() == expect); }
            else if (st == FrameStatus::Truncated) { ++trunc; }
            else { ++bad; }
            SweepLine l; std::string why;
            if (st == FrameStatus::Done && expect >= 2 && decodeScanraw(p.raw(), Model::TinySa, 1e6, 2e6, l, &why)) {
                CHECK(cascade::core::sweepLineProblem(l).empty());
            }
        }
        CHECK(done > 1000);   // unmutated frames were exercised
        CHECK(bad > 1000 && trunc > 1000);
        std::printf("fuzz: %zu done, %zu bad, %zu truncated of 30000\n", done, bad, trunc);
    }

    return testSummary("test_sweep_tinysa");
}
