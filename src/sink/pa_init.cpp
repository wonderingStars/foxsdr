// pa_init.cpp - see pa_init.hpp: the PortAudio init count, and the lock on
// PortAudio's list of open streams.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/pa_init.hpp"

namespace cascade::sink {

#if defined(CASCADE_ANDROID)

// No PortAudio in the Android build (see soundcard_source.cpp); nothing there
// asks for it, and an honest refusal is what anything that did would get.
bool paInitializeShared() { return false; }
void paTerminateShared() {}
PaStreamListGuard::PaStreamListGuard(Mode /*mode*/) {}
PaStreamListGuard::~PaStreamListGuard() {}
std::uint64_t paStreamListWaitsAbandoned() { return 0; }

}  // namespace cascade::sink

#else  // !CASCADE_ANDROID

}  // namespace cascade::sink

#include <portaudio.h>

#include <atomic>
#include <mutex>

#include "core/diag_log.hpp"

namespace cascade::sink {

namespace {

// Allocated once and never destroyed: a sound card's closer thread can still
// be finishing (and so terminating its PortAudio) while the process runs its
// static destructors, and a destroyed mutex is not something it may touch.
std::mutex& initMutex() {
    static std::mutex* const m = new std::mutex;
    return *m;
}

}  // namespace

bool paInitializeShared() {
    std::lock_guard<std::mutex> lk(initMutex());
    return Pa_Initialize() == paNoError;
}

void paTerminateShared() {
    std::lock_guard<std::mutex> lk(initMutex());
    Pa_Terminate();
}

namespace {

// The stream-list lock (see the header). A timed mutex, so a waiter can give
// up; allocated once and never destroyed, like the init lock, because a
// closer thread can still be inside a close while static destructors run.
std::timed_mutex& streamListMutex() {
    static std::timed_mutex* const m = new std::timed_mutex;
    return *m;
}

std::atomic<std::uint64_t> gStreamListWaitsAbandoned{0};

// When the CURRENT holder took the lock (steady-clock milliseconds), 0 while
// nobody holds it. What a waiter judges "has stopped" by.
std::atomic<std::int64_t> gHeldSinceMs{0};

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// How often a waiter looks at the holder's age between tries.
constexpr std::chrono::milliseconds kStreamListSliceMs{10};

}  // namespace

PaStreamListGuard::PaStreamListGuard(Mode mode) {
    if (mode == NoWait) {
        held_ = streamListMutex().try_lock();
        if (held_) { gHeldSinceMs.store(steadyMs() | 1, std::memory_order_relaxed); }
        return;
    }
    // THE HOLDER'S AGE, NOT THE WAITER'S WAIT. Going ahead without the lock is
    // safe only past a holder that has kept it for kStreamListWaitMs - a close
    // inside a host API that has stopped answering, which is past its list
    // work. Until this measured the waiter's own wait (one try_lock_for), and
    // a waiter behind several HEALTHY holders in turn gave up while one of
    // them was still inside its list change: two unlocked writers on
    // PortAudio's list (tests/test_pa_stream_list_guard.cpp; the eight-thread
    // probe in test_soundcard_source failing under a parallel load on
    // Windows). So a waiter tries in short slices and, between them, gives up
    // only when whoever holds the lock now has held it that long - or when it
    // has waited kStreamListStarveMs in all, so no queue keeps it for ever.
    const std::int64_t startMs = steadyMs();
    for (;;) {
        if (streamListMutex().try_lock_for(kStreamListSliceMs)) {
            held_ = true;
            gHeldSinceMs.store(steadyMs() | 1, std::memory_order_relaxed);
            return;
        }
        const std::int64_t now = steadyMs();
        const std::int64_t since = gHeldSinceMs.load(std::memory_order_relaxed);
        const bool holderStopped = since != 0 && now - since >= kStreamListWaitMs.count();
        const bool starved = now - startMs >= kStreamListStarveMs.count();
        if (!holderStopped && !starved) { continue; }
        gStreamListWaitsAbandoned.fetch_add(1, std::memory_order_relaxed);
        if (holderStopped) {
            cascade::core::diagWarnf(
                "audio: PortAudio's stream list was held for more than %lld ms (a sound card close "
                "that has stopped answering?); going ahead without it",
                static_cast<long long>(kStreamListWaitMs.count()));
        } else {
            cascade::core::diagWarnf(
                "audio: waited %lld ms for PortAudio's stream list behind other opens and closes; "
                "going ahead without it",
                static_cast<long long>(kStreamListStarveMs.count()));
        }
        return;
    }
}

PaStreamListGuard::~PaStreamListGuard() {
    if (held_) {
        gHeldSinceMs.store(0, std::memory_order_relaxed);
        streamListMutex().unlock();
    }
}

std::uint64_t paStreamListWaitsAbandoned() {
    return gStreamListWaitsAbandoned.load(std::memory_order_relaxed);
}

}  // namespace cascade::sink

#endif  // CASCADE_ANDROID
