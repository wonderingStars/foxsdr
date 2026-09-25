// pa_init.cpp - see pa_init.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/pa_init.hpp"

namespace cascade::sink {

#if defined(CASCADE_ANDROID)

// No PortAudio in the Android build (see soundcard_source.cpp); nothing there
// asks for it, and an honest refusal is what anything that did would get.
bool paInitializeShared() { return false; }
void paTerminateShared() {}

}  // namespace cascade::sink

#else  // !CASCADE_ANDROID

}  // namespace cascade::sink

#include <portaudio.h>

#include <mutex>

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

}  // namespace cascade::sink

#endif  // CASCADE_ANDROID
