// CAT control: the rigctld-compatible command layer (parse, decide, format).
//
// WHY THIS EXISTS. Logging software, digital-mode applications and antenna
// rotator controllers all speak to a radio the same way: they open a TCP
// connection to Hamlib's `rigctld` and exchange short text commands. A
// receiver that speaks that protocol drops into an existing station's software
// without anyone writing an integration; one that does not is an island.
//
// NOTHING FROM HAMLIB IS USED HERE. Hamlib is GPL and is neither linked nor
// read; this is written from the published rigctld(1) protocol description,
// the same way every decoder in this project is written from its own
// specification. What is reproduced is a wire format, which is what makes
// interoperability possible at all.
//
// THE ONE DETAIL THAT MATTERS MOST, because getting it backwards desynchronises
// every client after the first command: in the default protocol a SUCCESSFUL
// GET RETURNS ONLY ITS VALUES, one per line, and NO "RPRT 0". A SET returns
// "RPRT 0" and nothing else. A FAILURE of either returns "RPRT <negative>".
// (rigctld(1); confirmed against an independent description of the daemon's
// behaviour before this was written.)
//
// This file is deliberately free of sockets so the whole protocol can be
// tested as a pure function: line in, reply out, plus an optional control
// request for the GUI thread. cat_server.cpp is the thin transport around it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_CAT_PROTOCOL_HPP
#define CASCADE_NET_CAT_PROTOCOL_HPP

#include <string>

#include "net/web_control.hpp"
#include "net/web_server.hpp"

namespace cascade::net {

// Hamlib error codes, by their documented names. Only the ones this server can
// actually produce are listed: inventing a code we never return would suggest
// a behaviour that does not exist.
inline constexpr int kCatOk = 0;
inline constexpr int kCatEinval = -1;   // a parameter was malformed or out of range
inline constexpr int kCatEnimpl = -4;   // the command is understood but not implemented
inline constexpr int kCatEnavail = -11; // the function does not exist on this radio

// What one request line produced.
struct CatResult {
    // Exactly the bytes to send back, already newline-terminated. Never empty
    // for a request that warrants a reply.
    std::string reply;
    // The client asked to close (`q`/`Q`). The transport should hang up.
    bool quit = false;
    // Set requests are QUEUED, never applied here: this runs on a network
    // thread and the pipeline belongs to the GUI thread. Same discipline as
    // the browser's controls.
    bool hasControl = false;
    ControlRequest control;
};

// Where a requested absolute frequency should land.
//
// Inside the band already being received, only the VFO moves — no retune, so
// no gap in the waterfall and no hardware settling. Outside it, the centre
// moves and the offset returns to zero, which is what a person does by hand.
struct TunePlan {
    bool retune = false;    // true when the source centre must change
    double centerHz = 0.0;
    double vfoOffsetHz = 0.0;
};

// `usableFraction` keeps the VFO away from the very edge of the band, where a
// channel is half outside the sampled spectrum. 0.90 leaves 5% at each end.
TunePlan planTune(double requestedHz, double currentCenterHz, double sampleRateHz,
                  double usableFraction = 0.90);

// The Hamlib mode token for one of ours, and back. Hamlib has no name for the
// RAW passthrough, so it maps to nothing and `catModeFromToken` will not
// produce it: a client cannot select a mode that is not a demodulator.
const char* catModeToken(cascade::dsp::DemodMode m);
bool catModeFromToken(const std::string& token, cascade::dsp::DemodMode& out);

// Executes one request line against a snapshot of the receiver.
//
// `line` may carry a trailing CR and/or LF and any amount of surrounding
// whitespace; a blank line produces an empty reply and no control request.
CatResult executeCatLine(const std::string& line, const RadioStatus& status);

// The `\dump_state` payload. Split out because it is long, fixed, and worth
// reading on its own: it is how a client learns what this radio can do.
std::string catDumpState(const RadioStatus& status);

}  // namespace cascade::net

#endif  // CASCADE_NET_CAT_PROTOCOL_HPP
