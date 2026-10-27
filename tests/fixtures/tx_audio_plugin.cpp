// A REAL plugin module for tests/test_tx_audio_app.cpp: a decoder that declares
// CASCADE_CAP_TX_AUDIO (0.99.74, host API level 2) and asks for the transmitter
// whenever the user's settings say so.
//
// WHAT IT IS FOR. The keying rule in core/tx_audio.hpp is only as good as the proof
// that a real module, loaded the way a downloaded one is, cannot get past any of its
// conditions. This is that module: it is always willing to transmit and does nothing
// to be allowed to, so every refusal the application makes is the application's.
//
// WHAT IT DOES.
//   * A DECODER (audio, any rate) that decodes nothing - the handle TX_AUDIO rides on.
//   * HOST_CLIENT + SETTINGS_UI: one BOOL field, "send" ("Send a 1 kHz tone"), default
//     off. A worker thread, started in create() and joined in destroy(), reads it with
//     settings_get every few milliseconds (settings_get refuses the real-time thread,
//     which is where active() runs, so the flag crosses that boundary the way a real
//     modem's would: through a variable) - the settings flag the test turns on and off.
//   * TX_AUDIO: active() answers the flag; pull() hands back a 1 kHz tone at half
//     scale, continuous across calls, for as many frames as it is asked for.
//
// WHAT IT COUNTS, for the test to read through the exported tx_fixture_counts(): how
// many times the host called active(), how many times it called pull(), and how many
// frames pull() handed out. "pull() reached the ring only while keyed" and "an ungranted
// plugin is never pulled from" are then read off the module, not off what the
// application says it did.
//
// Built by tests/CMakeLists.txt as a MODULE, exactly as a third-party plugin is built.
// It includes ONLY plugin_abi.h (and the standard library), as every plugin must.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>

#include "plugin_abi.h"

namespace {

// THE HOST TABLE, PUBLISHED TO THE WORKER THREAD (stub campaign B13, found by the ThreadSanitizer leg): attach() runs on the host's thread and the worker below is
// started by create(), and nothing about that order is promised to the worker - a plain pointer here was a data race on the host's own bridge (the table it points into is
// built just before attach() is called), which ThreadSanitizer reported at the worker's first settings_get. A release store here and an acquire load there is the
// ordering a real modem's worker needs too.
std::atomic<const CascadeHostApi*> g_host{nullptr};

void hostAttach(const CascadeHostApi* h) { g_host.store(h, std::memory_order_release); }

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &hostAttach,
};

const CascadeSettingSpec kSpecs[] = {
    {static_cast<uint32_t>(sizeof(CascadeSettingSpec)), "send", "Send a 1 kHz tone",
     CASCADE_SETTING_BOOL, 0u, "", ""},
};

const CascadeSettingsUiApi kSettingsUi = {
    static_cast<uint32_t>(sizeof(CascadeSettingsUiApi)),
    static_cast<uint32_t>(sizeof(kSpecs) / sizeof(kSpecs[0])),
    kSpecs,
};

// The counters live outside the instance so the exported function can read them after
// the instance is gone, and so a second instance (a rescan) does not reset them.
std::atomic<unsigned long long> g_actives{0};
std::atomic<unsigned long long> g_pulls{0};
std::atomic<unsigned long long> g_frames{0};

struct Instance {
    std::atomic<int> wantSend{0};
    std::atomic<bool> stop{false};
    std::thread worker;
    double phase = 0.0;
};

// The worker: the ONLY place settings_get is called, because the real-time thread may
// not call it. A few milliseconds is far inside the test's waits.
void workerMain(Instance* s) {
    while (!s->stop.load(std::memory_order_relaxed)) {
        int v = 0;
        const CascadeHostApi* host = g_host.load(std::memory_order_acquire);
        if (host != nullptr && CASCADE_HOST_HAS(host, settings_get)) {
            char buf[16] = {};
            const int32_t rc = host->settings_get(host->ctx, "send", buf, sizeof(buf));
            v = (rc >= 0 && buf[0] == '1') ? 1 : 0;
        }
        s->wantSend.store(v, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
}

void* create(uint32_t) {
    Instance* s = new (std::nothrow) Instance();
    if (s == nullptr) { return nullptr; }
    s->worker = std::thread(workerMain, s);
    return s;
}

void process(void*, const float*, size_t) {}

int32_t pollText(void*, char*, size_t) { return 0; }

void destroy(void* handle) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr) { return; }
    s->stop.store(true, std::memory_order_relaxed);
    if (s->worker.joinable()) { s->worker.join(); }
    delete s;
}

const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)), 0u, &create, &process, &pollText, &destroy,
};

// --- the transmit audio ------------------------------------------------------

int32_t txActive(void* handle) {
    Instance* s = static_cast<Instance*>(handle);
    g_actives.fetch_add(1, std::memory_order_relaxed);
    return (s != nullptr && s->wantSend.load(std::memory_order_relaxed) != 0) ? 1 : 0;
}

int32_t txPull(void* handle, float* mono, size_t frames) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || mono == nullptr) { return -1; }
    g_pulls.fetch_add(1, std::memory_order_relaxed);
    const double step = 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
    for (size_t i = 0; i < frames; ++i) {
        mono[i] = static_cast<float>(0.5 * std::sin(s->phase));
        s->phase += step;
        if (s->phase > 6.283185307179586) { s->phase -= 6.283185307179586; }
    }
    g_frames.fetch_add(frames, std::memory_order_relaxed);
    return static_cast<int32_t>(frames);
}

const CascadeTxAudioApi kTxAudio = {
    static_cast<uint32_t>(sizeof(CascadeTxAudioApi)), 48000u, 1u, 0u, &txPull, &txActive,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
    {CASCADE_CAP_SETTINGS_UI, static_cast<uint32_t>(sizeof(CascadeSettingsUiApi)), &kSettingsUi},
    {CASCADE_CAP_TX_AUDIO, static_cast<uint32_t>(sizeof(CascadeTxAudioApi)), &kTxAudio},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "TX Audio Fixture",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_DECODER | CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_SETTINGS_UI | CASCADE_CAP_TX_AUDIO,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}

// For the test only, found with GetProcAddress/dlsym on the module the host mapped. It is
// not part of the plugin ABI and a plugin does not need one.
extern "C" CASCADE_PLUGIN_EXPORT void tx_fixture_counts(unsigned long long* actives,
                                                       unsigned long long* pulls,
                                                       unsigned long long* frames) {
    if (actives != nullptr) { *actives = g_actives.load(std::memory_order_relaxed); }
    if (pulls != nullptr) { *pulls = g_pulls.load(std::memory_order_relaxed); }
    if (frames != nullptr) { *frames = g_frames.load(std::memory_order_relaxed); }
}
