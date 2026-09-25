// soundcard_portaudio.hpp - the sound card's PortAudio backend, with every
// PortAudio call it makes reachable through one table, so a test can run the
// REAL backend - its locking, its host API filter, its close - against a
// PortAudio that it scripts: an enumeration of host APIs this machine does not
// have, and a close or an abort that never returns.
//
// Not in the Android build, which has no PortAudio (soundcard_source.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#if !defined(CASCADE_ANDROID)

#include <portaudio.h>

#include <memory>

#include "source/soundcard_source.hpp"

namespace cascade::source {

// Every PortAudio entry point the backend uses. The real table initialises and
// terminates through sink/pa_init.hpp (one lock for PortAudio's init count,
// held for nothing else).
struct SoundCardPaApi {
    PaError (*initialize)();
    PaError (*terminate)();
    PaDeviceIndex (*getDeviceCount)();
    PaDeviceIndex (*getDefaultInputDevice)();
    const PaDeviceInfo* (*getDeviceInfo)(PaDeviceIndex);
    const PaHostApiInfo* (*getHostApiInfo)(PaHostApiIndex);
    PaError (*isFormatSupported)(const PaStreamParameters* in, const PaStreamParameters* out,
                                 double rateHz);
    PaError (*openStream)(PaStream** stream, const PaStreamParameters* in,
                          const PaStreamParameters* out, double rateHz, unsigned long framesPerBuffer,
                          PaStreamFlags flags, PaStreamCallback* callback, void* user);
    PaError (*startStream)(PaStream*);
    PaError (*abortStream)(PaStream*);
    PaError (*closeStream)(PaStream*);
    PaError (*isStreamActive)(PaStream*);
    const char* (*getErrorText)(PaError);
};

// The table that calls the real PortAudio.
const SoundCardPaApi& realSoundCardPaApi();

// The backend over any table. `api` must outlive every backend made from it.
std::shared_ptr<SoundCardBackend> makePortAudioSoundCardBackend(const SoundCardPaApi& api);

// WHICH HOST APIs A SOUND CARD IS OFFERED FROM. False for MME, DirectSound,
// WDM-KS and ASIO; true for everything else (WASAPI on Windows, ALSA/JACK/OSS
// on Linux, Core Audio on a Mac). See soundcard_source.hpp, "ONLY HOST APIs
// WHOSE DEVICES KEEP THEIR IDENTITY".
bool soundCardHostApiListed(PaHostApiTypeId type);

}  // namespace cascade::source

#endif  // !CASCADE_ANDROID
