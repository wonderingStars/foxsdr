// pa_init.hpp - the ONE place the process initialises and terminates PortAudio.
//
// WHY ONE PLACE. PortAudio counts Pa_Initialize/Pa_Terminate pairs in a plain
// int (pa_front.c, initializationCount_), with no lock of its own, and the
// first Pa_Initialize also builds every host API's device list. The audio
// output, the audio input and every sound card source initialise it, from the
// GUI thread and from workers alike; two of those at once is a race on that
// count. So every one of them goes through here, and here takes one lock.
//
// WHAT THE LOCK IS NOT. It is held for Pa_Initialize and Pa_Terminate and for
// nothing else - never across an open, a close, an abort or an enumeration.
// That is deliberate: a sound card that has been pulled out can make a close
// or an abort sit inside the host API indefinitely, and a lock such a call
// could be holding is a lock the GUI thread must never wait on (the fault
// found in review of the sound card source, where one lock covered both).
// A Pa_Terminate that is not the last one only decrements the count; the last
// one closes every stream still open, which is why a sound card's closer
// thread keeps its own initialisation until its close has returned - a close
// that never returns keeps PortAudio initialised, rather than having the
// stream torn down under it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::sink {

// Pa_Initialize() under the process-wide PortAudio init lock. True when it
// succeeded; every true must be paired with exactly one paTerminateShared().
bool paInitializeShared();

// Pa_Terminate() under the same lock. Call only after a successful
// paInitializeShared().
void paTerminateShared();

}  // namespace cascade::sink
