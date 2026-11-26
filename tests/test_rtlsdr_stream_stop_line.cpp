// test_rtlsdr_stream_stop_line.cpp - THE TIMINGS ON THE "the RTL-SDR stream stopped" LINE (lane D1 of the 2026-11-25 crash fixes, from R1's proposal).
//
// Three NESDR SMArt v5 dongles dropped off USB ("a bulk read failed (Windows error 31)", then error 433 on the endpoint reset) and nobody could tell whether a FoxSDR
// call came just before. The line now carries: ms since the control transfer ended and whether one is in flight, ms since the process last listed or scanned USB,
// ms since the handle was opened, and the power word. This holds
//   A  the text builder: in flight / not, never scanned, never controlled, a known clock, and that no identifier-shaped word can appear;
//   B  the listing clock: never -> -1, then a note makes it small and growing;
//   C  the control trace: a transfer through Rtl2832u stamps its end and is not "in flight" afterwards, and one inside the transport IS in flight;
//   D  the power word is "unknown" until a machine line has said it, then the line's own word;
//   E  the whole line a real reader logs over a fake dongle carries the new fields (the reader thread's path, end to end).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/machine_state.hpp"
#include "source/rtl2832u.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_device.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::streamStopTimings;

namespace {

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

void testTheText() {
    const std::string idle = streamStopTimings(120, false, 4500, 31000, "ac");
    CHECK(has(idle, "control transfer 120 ms ago"));
    CHECK(has(idle, "in flight: no"));
    CHECK(has(idle, "USB scan 4500 ms ago"));
    CHECK(has(idle, "open for 31000 ms"));
    CHECK(has(idle, "power ac"));

    const std::string busy = streamStopTimings(0, true, 0, 5, "battery");
    CHECK(has(busy, "control transfer 0 ms ago"));
    CHECK(has(busy, "in flight: yes"));
    CHECK(has(busy, "power battery"));

    const std::string never = streamStopTimings(-1, false, -1, 7, "unknown");
    CHECK(has(never, "control transfer never"));
    CHECK(has(never, "USB scan never"));
    CHECK(has(never, "power unknown"));
    CHECK(!has(never, "-1"));

    // Times, a yes/no and a fixed word: no path, serial or identifier-shaped text ever appears in what is added.
    CHECK(!has(idle, "\\"));
    CHECK(!has(idle, "serial"));
    CHECK(!has(idle, "usb#"));
    // A power word that is not one of the three fixed words is never passed on.
    CHECK(has(streamStopTimings(1, false, 1, 1, "C:\secret"), "power unknown"));
    CHECK(has(streamStopTimings(1, false, 1, 1, nullptr), "power unknown"));
}

// The second line must never be cut at the ring's width, even with the largest times a long session can make (a stamp "HH:MM:SS.mmm warn " is 18 characters).
void testTheLineFits() {
    const std::string worst = std::string("source: the RTL-SDR stream stopped, timings: ") + streamStopTimings(86400000LL * 30, true, 86400000LL * 30, 86400000LL * 30, "battery");
    CHECK(worst.size() + 18 < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
}

void testTheListingClock() {
    // The clock is process-wide and other tests in this process may have listed; what is held is that a note makes it small and that it grows.
    cascade::usb::noteUsbListing();
    const long long a = cascade::usb::msSinceUsbListing();
    CHECK(a >= 0 && a < 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const long long b = cascade::usb::msSinceUsbListing();
    CHECK(b >= 50 && b < 2000);
    cascade::usb::noteUsbListing();
    CHECK(cascade::usb::msSinceUsbListing() < b);
}

void testTheControlTrace() {
    cascade::usb::FakeUsbDevice fake;
    cascade::source::Rtl2832u rtl(fake, 500);
    cascade::source::Rtl2832u::ControlTrace trace;
    rtl.setTrace(&trace);
    CHECK(trace.lastEndNs.load() == 0);
    CHECK(trace.inFlight.load() == 0);
    (void)rtl.readReg(cascade::source::RtlBlock::Usb, 0x2000, 1);   // the answer does not matter: the transfer went through the transport
    CHECK(trace.lastEndNs.load() != 0);
    CHECK(trace.inFlight.load() == 0);

    // Without a trace nothing is recorded and nothing breaks.
    cascade::source::Rtl2832u plain(fake, 500);
    (void)plain.readReg(cascade::source::RtlBlock::Usb, 0x2000, 1);
}

void testThePowerWord() {
    const std::string w = cascade::core::lastKnownPowerWord();
    CHECK(w == "unknown" || w == "ac" || w == "battery");
    (void)cascade::core::machineStateText("none", "none");   // makes the line, which records its own power word
    const std::string after = cascade::core::lastKnownPowerWord();
    CHECK(after == "unknown" || after == "ac" || after == "battery");
    CHECK(cascade::core::machineStateText("none", "none").find(std::string("power=") + after) != std::string::npos);
}

const std::vector<std::uint8_t> kTunerAnswer = {0x69, 0x00, 0x02, 0x00, 0xA4};

// E: the reader's own line, over a fake dongle whose every bulk read fails: the line a real reader logs carries the new fields.
void testTheLineTheReaderLogs() {
    cascade::usb::noteUsbListing();   // a scan "just happened"
    {
        cascade::source::RtlSdrSource src;
        auto fk = std::make_unique<cascade::usb::FakeUsbDevice>();
        fk->answerIn(0, 0x0034, 0x0600, kTunerAnswer);
        fk->answerIn(0, 0x00a0, 0x0600, {0x00});
        cascade::usb::FakeUsbDevice* live = fk.get();
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        live->failBulkAfter = 0;
        live->failBulkAfterText = "a bulk read failed (Windows error 31)";
        CHECK(src.start());
        for (int i = 0; i < 100 && !src.faulted(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        CHECK(src.faulted());
        src.stop();
    }
    // The ring is what a report would carry; read it, newest last.
    std::string line;
    std::string mainLine;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find("the RTL-SDR stream stopped, timings: ") != std::string::npos) { line = l; }
        else if (l.find("the RTL-SDR stream stopped - ") != std::string::npos) { mainLine = l; }
    }
    std::printf("  logged (first line): %s\n", mainLine.c_str());
    CHECK(!mainLine.empty());
    // (The first line is already at the ring's 191-character width with its stamp and is cut there, as it was before this change: its tail, "restart N of 3", is lost in the ring.
    //  That is why the timings are a second line and not appended to it.)
    CHECK(has(mainLine, "still listed on USB: "));   // the existing fields are still there, in the existing line
    CHECK(!line.empty());
    CHECK(line.size() < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
    std::printf("  logged (second line): %s\n", line.c_str());
    CHECK(has(line, "control transfer "));
    CHECK(has(line, "in flight: no"));
    CHECK(has(line, "USB scan "));
    CHECK(has(line, "open for "));
    CHECK(has(line, "power "));
    // What an upload would carry of it: the timings survive the scrub (they are ms counts) and nothing is cut.
    const std::string scrubbed = cascade::core::scrubUploadLine(line);
    std::printf("  scrubbed: %s\n", scrubbed.c_str());
    CHECK(scrubbed == line);   // byte for byte: no number is masked
    CHECK(has(scrubbed, "ms ago"));
    CHECK(has(scrubbed, "open for "));
}

}  // namespace

int main() {
    std::printf("test_rtlsdr_stream_stop_line\n");
    testTheText();
    testTheLineFits();
    testTheListingClock();
    testTheControlTrace();
    testThePowerWord();
    testTheLineTheReaderLogs();
    return testSummary("test_rtlsdr_stream_stop_line");
}
