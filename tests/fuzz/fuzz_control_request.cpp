// Fuzz target: the body of a web-remote control request (POST /api/control).
//
// This is the JSON a browser - or anything that can reach the web server's
// port - sends to retune, change mode, start a recording, install or remove a
// plugin. parseControlRequest turns it into the ControlRequest AppWindow's
// applyControlRequest acts on, and its range checks are the only thing between
// a request and the receiver (the GUI side trusts a parsed request).
//
// Properties: a refused body gives an error and an empty request; an accepted
// one has every number it carries finite and inside the documented bounds.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "fuzz_common.hpp"
#include "net/web_control.hpp"

using namespace cascade::net;

namespace {

bool within(const std::optional<double>& v, double lo, double hi) {
    return !v || (std::isfinite(*v) && *v >= lo && *v <= hi);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string body = fuzz::asString(data, size);

    ControlRequest req;
    std::string error;
    if (!parseControlRequest(body, req, error)) {
        FUZZ_REQUIRE(!error.empty());
        FUZZ_REQUIRE(req.empty());
        return 0;
    }

    FUZZ_REQUIRE(within(req.centerHz, kMinCenterHz, kMaxCenterHz));
    FUZZ_REQUIRE(within(req.vfoOffsetHz, -kMaxVfoOffsetHz, kMaxVfoOffsetHz));
    FUZZ_REQUIRE(within(req.bandwidthHz, kMinBandwidthHz, kMaxBandwidthHz));
    FUZZ_REQUIRE(within(req.squelchDb, kMinSquelchDb, kMaxSquelchDb));
    FUZZ_REQUIRE(within(req.volume, 0.0, 1.0));
    FUZZ_REQUIRE(within(req.dbMin, kMinDisplayDb, kMaxDisplayDb));
    FUZZ_REQUIRE(within(req.dbMax, kMinDisplayDb, kMaxDisplayDb));
    FUZZ_REQUIRE(within(req.nrStrength, 0.0, 1.0));
    FUZZ_REQUIRE(within(req.notchFreqHz, kMinNotchHz, kMaxNotchHz));
    FUZZ_REQUIRE(within(req.notchQ, kMinNotchQ, kMaxNotchQ));
    FUZZ_REQUIRE(within(req.sampleRateHz, 8000.0, 61.44e6));
    FUZZ_REQUIRE(within(req.gainDb, -30.0, 100.0));
    FUZZ_REQUIRE(within(req.scanStartHz, kMinCenterHz, kMaxCenterHz));
    FUZZ_REQUIRE(within(req.scanStopHz, kMinCenterHz, kMaxCenterHz));
    FUZZ_REQUIRE(within(req.scanStepHz, 100.0, 10.0e6));
    return 0;
}
