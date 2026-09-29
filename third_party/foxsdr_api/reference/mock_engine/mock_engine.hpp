// mock_engine.hpp - a headless FoxSDR engine with a synthetic signal source.
//
// WHAT IT IS FOR. Two things, and it is judged by both:
//   1. INTERFACE DEVELOPMENT. Anyone writing an interface plugin needs
//      something behind the API that tunes, draws a moving spectrum and meters
//      audio, without a radio, a DSP chain or the real application.
//   2. THE CONFORMANCE TARGET. It implements the API's RULES exactly as the
//      real engine must - snapshots that never wait, a bounded command queue
//      with results, grants, structSize handling, and above all the transmit
//      key's safety rules - so the tests in tests/ are the checklist the real
//      engine will be held to when it is extracted from the FoxSDR app
//      (docs/ENGINE-EXTRACTION.md).
//
// WHAT IT IS NOT. There is no DSP here. The "spectrum" is generated from a
// fixed list of synthetic stations; "audio" is a tone whose level follows the
// synthetic signal in the channel. No real radio is opened and nothing is ever
// transmitted: the transmitter is a dummy load that only keeps the key state.
//
// THREADS. A SIGNAL thread (stands in for the DSP thread) publishes spectrum
// frames and audio at its own pace and never takes a lock an interface can
// hold. A CONTROL thread drains the command queue, applies commands, runs the
// transmitter's watchdogs and publishes the state snapshot. Interface calls
// never block either of them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>

#include "foxsdr_api.h"

namespace foxsdr::mock {

// The mock engine's table (the same one the module exports as
// foxsdr_engine_query). NULL when (apiMajor, apiMinor) cannot be served.
const FoxEngineApi* engineApi(uint32_t apiMajor, uint32_t apiMinor);

// Engine options understood in FoxEngineParams::options ("k=v;k=v"):
//   bins=N            spectrum bins, 64..16384 (default 2048)
//   fps=F             spectrum frames per second, 1..240 (default 30)
//   centreHz=F        start centre frequency (default 94.5e6)
//   running=0|1       start running (default 1)
//   pttHoldMs=N       } TEST OPTIONS: may only SHORTEN the standard values
//   latchTimeoutMs=N  } (FOXAPI_PTT_HOLD_MS, FOXAPI_LATCH_TIMEOUT_MS,
//   keepaliveMs=N     }  FOXAPI_KEEPALIVE_MS); a longer value is clamped down.
//   controlHz=N       control-thread wake rate, 20..1000 (default 200)

}  // namespace foxsdr::mock
