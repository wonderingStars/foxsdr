// A single seam pulled out of audio_out_aaudio.cpp so it can be reached from
// a host unit test with no <aaudio/AAudio.h> anywhere in its include chain
// (see that file's header for why the AAudio-touching methods themselves
// cannot be tested off-device).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>

namespace cascade::sink {

// Which raw AAudioStream state counts as "the user is hearing something".
//
// Takes the RAW enum value (aaudio_stream_state_t is itself just int32_t)
// rather than the enum type, so this function — and the decision it makes,
// which is the part worth testing — has no dependency on AAudio.h at all and
// compiles on any host. tests/test_audio_out_aaudio.cpp drives it with the
// literal numbers AAudio.h defines (verified against
// $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/
// aaudio/AAudio.h, API 26+, where the enum is UNINITIALIZED=0, UNKNOWN,
// OPEN, STARTING, STARTED, PAUSING, PAUSED, FLUSHING, FLUSHED, STOPPING,
// STOPPED, CLOSING, CLOSED, DISCONNECTED — a plain sequential enum with one
// explicit "= 0", so STARTING is 3 and STARTED is 4). The real
// AudioOut::streamAlive() (audio_out_aaudio.cpp, __ANDROID__ only) is the
// one call site and static_asserts those two literals against the actual
// AAUDIO_STREAM_STATE_* constants at Android build time, so a future NDK
// that ever renumbered this stable, ABI-frozen enum would fail the arm64
// build loudly rather than silently misclassifying a live stream as dead.
//
// Everything except STARTING/STARTED is "not currently producing audio":
// OPEN (opened but requestStart() not yet in effect — AudioOut::open() only
// returns success after requestStart(), so this should not occur once
// open() has returned, but is dead rather than alive if it somehow does),
// PAUSING/PAUSED, FLUSHING/FLUSHED, STOPPING/STOPPED, CLOSING/CLOSED (all
// states this backend only ever reaches via its OWN close(), at which point
// stream_ is already null and streamAlive() never calls this), DISCONNECTED
// (the state a vanished device leaves behind — exactly the case
// streamAlive() exists to catch, see audio_out.hpp's own comment on it), and
// UNINITIALIZED/UNKNOWN (should never be observed on an opened stream).
bool androidStreamStateIsAlive(std::int32_t rawState);

}  // namespace cascade::sink
