// extio_standin.cpp - STAND-IN ExtIO drivers for tests/test_extio.cpp. Written from the published ExtIO specification's prose only (see
// source/extio_protocol.hpp); no vendor DLL, header or code is involved. One source, built once per behaviour (STANDIN_MODE), each into its own
// DLL: a tone source, and the ways a real driver fails - a crash, a hang, a flood, nonsense block sizes, a bad callback pointer, silence, a
// DLL that lacks entry points, a refused InitHW, a sound-card driver, non-finite float samples, status callbacks.
//
// These DLLs MISBEHAVE ON PURPOSE. They are built into a directory of their own and only ever loaded by the helper process under test.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#ifndef STANDIN_MODE
#define STANDIN_MODE 0
#endif

// the modes are macros, not an enum: the preprocessor compares them
#define kTone 0 
#define kCrashCallback 1 // crashes (null write) on its own thread after a few blocks
#define kCrashSetLo 2 // crashes inside SetHWLO
#define kHangStart 3 // StartHW never returns
#define kHangSetLo 4 // SetHWLO never returns
#define kFlood 5 // calls the callback in a tight loop, unpaced
#define kSizeOdd 6 // StartHW returns 700
#define kSizeHuge 7 // StartHW returns 0x7fffffff
#define kSizeNegative 8 // StartHW returns -3
#define kSizeZero 9 // StartHW returns 0
#define kBadPointer 10 // the first callbacks carry a wild pointer, then good data
#define kMissingExports 11 // exports InitHW only
#define kInitFalse 12 // InitHW returns false
#define kSoundCard 13 // HW type 4
#define kSilent 14 // StartHW is fine, the callback is never called
#define kFloatNonFinite 15 // float32 samples with NaN and infinity in them
#define kStatus 16 // changes its sample rate and LO and says so through status callbacks
#define kBadMode 17 // InitHW type 99

namespace {
using CallbackFn = void (*)(int, int, float, void*);
CallbackFn g_cb = nullptr;
std::atomic<bool> g_run{false};
std::thread g_thread;
std::atomic<long> g_lo{7050000};
std::atomic<long> g_rate{192000};
constexpr long kMinLo = 100000, kMaxLo = 2000000000;
constexpr int kPairs = 512;

void produce() {
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.01f);
    double phase = 0.0;
    const double rate = 192000.0 * (1.0 + 40e-6);   // the "crystal" is 40 ppm off, as real ones are
    const double toneHz = 12000.0;                  // a tone 12 kHz above the centre
    std::vector<std::int16_t> s16(kPairs * 2);
    std::vector<float> f32(kPairs * 2);
    auto next = std::chrono::steady_clock::now();
    int block = 0;
    while (g_run.load()) {
        for (int i = 0; i < kPairs; ++i) {
            const float a = 0.5f;
            const float re = a * static_cast<float>(std::cos(phase)) + noise(rng);
            const float im = a * static_cast<float>(std::sin(phase)) + noise(rng);
            phase += 2.0 * 3.14159265358979323846 * toneHz / rate;
            s16[static_cast<std::size_t>(i) * 2] = static_cast<std::int16_t>(re * 32767.0f);
            s16[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::int16_t>(im * 32767.0f);
            f32[static_cast<std::size_t>(i) * 2] = re;
            f32[static_cast<std::size_t>(i) * 2 + 1] = im;
        }
        if (phase > 1e6) { phase = std::fmod(phase, 2.0 * 3.14159265358979323846); }
        ++block;
        if (g_cb != nullptr) {
#if STANDIN_MODE == kCrashCallback
            if (block == 4) { *reinterpret_cast<volatile int*>(0) = 1; }
#endif
#if STANDIN_MODE == kBadPointer
            if (block <= 10) { g_cb(kPairs, 0, 0.0f, reinterpret_cast<void*>(0x10)); goto pace; }
#endif
#if STANDIN_MODE == kFloatNonFinite
            f32[10] = std::nanf("");
            f32[11] = INFINITY;
            f32[12] = -INFINITY;
            f32[13] = 5.0f;   // out of range: clamped
            g_cb(kPairs, 0, 0.0f, f32.data());
#elif STANDIN_MODE == kStatus
            if (block == 20) {
                g_rate = 96000;
                g_cb(-1, 100, 0.0f, nullptr);
            }
            if (block == 30) {
                g_lo = 14100000;
                g_cb(-1, 101, 0.0f, nullptr);
            }
            g_cb(kPairs, 0, 0.0f, s16.data());
#else
            g_cb(kPairs, 0, 0.0f, s16.data());
#endif
        }
#if STANDIN_MODE == kBadPointer
    pace:
#endif
#if STANDIN_MODE != kFlood
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kPairs / rate));
        std::this_thread::sleep_until(next);
#endif
    }
}

void stopThread() {
    g_run = false;
    if (g_thread.joinable()) { g_thread.join(); }
}
}  // namespace

#define EXPORT extern "C" __declspec(dllexport)

EXPORT bool __stdcall InitHW(char* name, char* model, int& type) {
#if STANDIN_MODE == kInitFalse
    std::strcpy(name, "StandIn");
    std::strcpy(model, "fails");
    type = 3;
    return false;
#else
    std::strcpy(name, "StandIn Radio");
    std::strcpy(model, "SN-TEST-0001");
#if STANDIN_MODE == kSoundCard
    type = 4;
#elif STANDIN_MODE == kFloatNonFinite
    type = 7;
#elif STANDIN_MODE == kBadMode
    type = 99;
#else
    type = 3;
#endif
    return true;
#endif
}

#if STANDIN_MODE != kMissingExports
EXPORT bool __stdcall OpenHW(void) { return true; }

EXPORT int __stdcall StartHW(long freq) {
    g_lo = freq;
#if STANDIN_MODE == kHangStart
    Sleep(INFINITE);
#elif STANDIN_MODE == kSizeOdd
    return 700;
#elif STANDIN_MODE == kSizeHuge
    return 0x7fffffff;
#elif STANDIN_MODE == kSizeNegative
    return -3;
#elif STANDIN_MODE == kSizeZero
    return 0;
#elif STANDIN_MODE == kSilent
    return kPairs;
#endif
#if STANDIN_MODE == kFlood
    g_rate = 48000;
#endif
    stopThread();
    g_run = true;
    g_thread = std::thread(produce);
    return kPairs;
}

EXPORT void __stdcall StopHW(void) { stopThread(); }
EXPORT void __stdcall CloseHW(void) { stopThread(); }

EXPORT int __stdcall SetHWLO(long lo) {
#if STANDIN_MODE == kCrashSetLo
    *reinterpret_cast<volatile int*>(0) = 1;
#elif STANDIN_MODE == kHangSetLo
    Sleep(INFINITE);
#endif
    if (lo < kMinLo) { return -static_cast<int>(kMinLo); }
    if (lo > kMaxLo) { return static_cast<int>(kMaxLo); }
    g_lo = lo;
    return 0;
}
EXPORT long __stdcall GetHWLO(void) { return g_lo.load(); }
EXPORT long __stdcall GetHWSR(void) { return g_rate.load(); }
EXPORT int __stdcall GetStatus(void) { return 0; }
EXPORT void __stdcall SetCallback(void (*cb)(int, int, float, void*)) { g_cb = cb; }
#endif
