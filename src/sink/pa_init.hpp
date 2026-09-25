// pa_init.hpp - the ONE place the process initialises and terminates PortAudio,
// and the one lock that keeps PortAudio's list of open streams in one piece.
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
// THE STREAM-LIST LOCK. PortAudio also keeps every open stream in a plain
// linked list (pa_front.c, firstOpenStream_), with no lock: Pa_OpenStream adds
// to it as its LAST act, after the host API has opened the device, and
// Pa_CloseStream removes from it as its FIRST act, before the host API closes
// anything (RemoveOpenStream, "be sure to call this _before_ closing the
// stream"). A sound card is closed on a thread of its own, so a close can now
// run while an open does - two unlocked writers on one list. PaStreamListGuard
// is held around every Pa_OpenStream and every Pa_CloseStream in the product
// (a sound card's on its closer thread, or on the worker whose open failed;
// the audio output's and the microphone's wherever they run), so those list
// changes happen one at a time.
//
// AND WHY IT CANNOT HANG ANYBODY. A close that has stopped answering holds the
// guard for as long as it hangs - but by then it is inside the host API's own
// close, which comes AFTER its list bookkeeping. So the guard is a TIMED one:
// a sound card open or close waits for it at most kStreamListWaitMs and then
// goes ahead without it (logged, and counted by paStreamListWaitsAbandoned). A
// holder that has kept it that long is past the list, so going ahead is safe,
// and nothing waits on a hung close for longer than that. Those waits are all
// on workers and closer threads.
//
// THE GUI THREAD NEVER WAITS FOR IT. The audio output's open is not always on
// a worker - the patch page's speaker opens its device from the frame loop,
// and the Pipeline constructor opens the default output on the thread that
// builds it - so AudioOut and AudioIn take the guard with NoWait: they hold it
// if it is free (and a sound card open or close that comes along meanwhile
// waits its turn), and go ahead at once if it is not. Their CLOSES take it the
// same way, NoWait (~AudioOut can run on the GUI thread at exit): a close
// changes the list exactly as an open does, and a close that finds the lock
// held goes ahead rather than make the GUI thread wait.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstdint>

namespace cascade::sink {

// Pa_Initialize() under the process-wide PortAudio init lock. True when it
// succeeded; every true must be paired with exactly one paTerminateShared().
bool paInitializeShared();

// Pa_Terminate() under the same lock. Call only after a successful
// paInitializeShared().
void paTerminateShared();

// The most anyone waits for the stream-list lock before going ahead without
// it. Far longer than any list change takes (microseconds), far shorter than
// a sound card's kCloseWaitMs, so a healthy close still finishes inside the
// wait its caller gives it even while another card's close is hung.
constexpr std::chrono::milliseconds kStreamListWaitMs{250};

// Held for its lifetime around one Pa_OpenStream or one Pa_CloseStream. See
// the file header.
class PaStreamListGuard {
public:
    enum Mode {
        Wait,    // up to kStreamListWaitMs, then without it (workers, closer threads)
        NoWait,  // only if it is free right now (anything that may be the GUI thread)
    };
    explicit PaStreamListGuard(Mode mode = Wait);
    ~PaStreamListGuard();
    PaStreamListGuard(const PaStreamListGuard&) = delete;
    PaStreamListGuard& operator=(const PaStreamListGuard&) = delete;
    // False when the wait ran out and the caller went ahead without it.
    bool held() const { return held_; }

private:
    bool held_ = false;
};

// Waits for the stream-list lock that ran out, process-wide. Monotonic.
std::uint64_t paStreamListWaitsAbandoned();

}  // namespace cascade::sink
