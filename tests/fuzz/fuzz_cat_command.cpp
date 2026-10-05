// Fuzz target: a line a CAT client sends (the Hamlib rigctld protocol).
//
// The CAT server listens on a TCP port, and anything that can reach the port -
// a logger, a digital-mode program, another machine on the network when the
// user has opened it up - can write to it. executeCatLine is the whole of the
// command grammar; the server's own per-connection buffering sits in front of
// it and is bounded separately.
//
// Properties: any line yields an answer or a deliberate silence, never a
// fault; a command that asks the receiver to do something asks for a value
// inside the range every other control surface enforces (web_control.hpp), and
// a finite one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "fuzz_common.hpp"
#include "net/cat_protocol.hpp"

using namespace cascade::net;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    // The radio the client is talking to. A few real-world values plus whatever
    // the input says, so frequency and bandwidth formatting is reached with the
    // awkward ones too.
    RadioStatus status;
    const std::uint8_t pick = in.u8();
    status.running = (pick & 1) != 0;
    status.centerHz = (pick & 2) ? in.rawDouble() : 100.0e6;
    status.vfoOffsetHz = (pick & 4) ? in.rawDouble() : 0.0;
    status.bandwidthHz = (pick & 8) ? in.rawDouble() : 12500.0;
    status.sampleRateHz = (pick & 16) ? in.rawDouble() : 2.4e6;
    static const char* const kModes[] = {"WFM", "NFM", "AM", "USB", "LSB", "CW", "bogus", ""};
    status.mode = kModes[(pick >> 5) % 8];

    const std::string line = in.rest();
    const CatResult r = executeCatLine(line, status);
    (void)catDumpState(status);

    if (r.hasControl) {
        const ControlRequest& c = r.control;
        if (c.centerHz) {
            FUZZ_REQUIRE(std::isfinite(*c.centerHz));
            FUZZ_REQUIRE(*c.centerHz >= kMinCenterHz && *c.centerHz <= kMaxCenterHz);
        }
        if (c.bandwidthHz) {
            FUZZ_REQUIRE(*c.bandwidthHz >= kMinBandwidthHz && *c.bandwidthHz <= kMaxBandwidthHz);
        }
        if (c.vfoOffsetHz) { FUZZ_REQUIRE(std::isfinite(*c.vfoOffsetHz)); }
    }
    return 0;
}
