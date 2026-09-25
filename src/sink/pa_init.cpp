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

}  // namespace

PaStreamListGuard::PaStreamListGuard(Mode mode) {
    if (mode == NoWait) {
        held_ = streamListMutex().try_lock();
        return;
    }
    held_ = streamListMutex().try_lock_for(kStreamListWaitMs);
    if (!held_) {
        // Whoever has it has had it for kStreamListWaitMs: a close inside a
        // host API that has stopped answering, which is past its list work.
        gStreamListWaitsAbandoned.fetch_add(1, std::memory_order_relaxed);
        cascade::core::diagWarnf(
            "audio: PortAudio's stream list was held for more than %lld ms (a sound card close "
            "that has stopped answering?); going ahead without it",
            static_cast<long long>(kStreamListWaitMs.count()));
    }
}

PaStreamListGuard::~PaStreamListGuard() {
    if (held_) { streamListMutex().unlock(); }
}

std::uint64_t paStreamListWaitsAbandoned() {
    return gStreamListWaitsAbandoned.load(std::memory_order_relaxed);
}

}  // namespace cascade::sink

#endif  // CASCADE_ANDROID
