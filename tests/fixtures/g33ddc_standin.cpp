// g33ddc_standin.cpp - a STAND-IN G33DDCAPI.dll for tests/test_g33ddc_source.cpp. Written from the same published document as the bridge
// (src/source/g33ddc_api.hpp), so it can show that the bridge calls what the document says in the order the document says; it cannot show that the
// document was read rightly, and it is not WiNRADiO's code.
//
// One device, a list of DDC types, an absolute DDC1 frequency, and a streaming thread that calls the registered DDC1 callback with a tone (noise, a
// static frequency error and a sample-clock error included). It records which thread every API call arrived on (the one-thread rule) and can
// misbehave on cue. Control: eol_set(key, value) / eol_get(key), exported by plain name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "source/g33ddc_api.hpp"

using namespace cascade::source::g33ddc;

namespace {

enum Key : int {
    kMisbehave = 1,
    kToneHz = 2,
    kSpeed = 3,
    kSilentAfter = 4,
    kPpmX1000 = 5,
    kF = 10,  // last SetDDC1Frequency
    kDdcType = 12,
    kPower = 13,
    kStarts = 14,
    kStops = 15,
    kOpens = 16,
    kCloses = 17,
    kThreads = 18,
    kOverlap = 19,
    kPreamp = 20,
    kCallbackSet = 21,
    kCalls = 22,
    kNoiseX1000 = 23,
};
enum Misbehave : std::int64_t {
    kNoDevices = 1 << 0,
    kOpenFails = 1 << 1,
    kStartFails = 1 << 2,
    kStartHangs = 1 << 3,
    kStartCrashes = 1 << 4,
    kOverrun = 1 << 5,  // GetDeviceList writes 64 bytes past the buffer it was given
    kDisconnected = 1 << 6,
    kBadBits = 1 << 8,
    kBadCount = 1 << 9,
    kTuneFails = 1 << 10,
    kSetDdcFails = 1 << 11,
    kBadRecord = 1 << 12,  // the channel count of the record is impossible (a different layout)
};

std::mutex g_m;
std::int64_t g_v[64] = {};
std::set<DWORD> g_threads;
std::atomic<int> g_inCall{0};
std::atomic<int> g_maxIn{0};
std::atomic<bool> g_run{false};
// Never destroyed: a stream thread still running when the process exits must not make the DLL static destruction call std::terminate.
std::thread& gt() {
    static std::thread* t = new std::thread;
    return *t;
}
Callbacks g_cb;
std::uintptr_t g_user = 0;
bool g_inited = false;

struct DdcT {
    std::uint32_t rate, bw, bits;
};
const DdcT kTypes[] = {{48000, 38000, 16}, {96000, 76000, 16}, {192000, 150000, 16}, {384000, 300000, 32},
                       {768000, 600000, 32}, {0, 0, 16},      {1000000, 800000, 24}};

void init() {
    if (g_inited) { return; }
    g_inited = true;
    g_v[kToneHz] = 7015000;
    g_v[kSpeed] = 20;
    g_v[kPpmX1000] = 1500000;
    g_v[kNoiseX1000] = 20;
}

struct Enter {
    Enter() {
        std::lock_guard<std::mutex> lk(g_m);
        init();
        g_threads.insert(::GetCurrentThreadId());
        g_v[kThreads] = static_cast<std::int64_t>(g_threads.size());
        ++g_v[kCalls];
        const int now = g_inCall.fetch_add(1) + 1;
        int prev = g_maxIn.load();
        while (now > prev && !g_maxIn.compare_exchange_weak(prev, now)) {}
        g_v[kOverlap] = g_maxIn.load();
    }
    ~Enter() { g_inCall.fetch_sub(1); }
};

bool mis(std::int64_t bit) {
    std::lock_guard<std::mutex> lk(g_m);
    return (g_v[kMisbehave] & bit) != 0;
}

void streamLoop() {
    std::uint64_t n = 0;
    double phase = 0.0;
    std::uint32_t lcg = 12345;
    const auto rnd = [&]() {
        lcg = lcg * 1664525u + 1013904223u;
        return (static_cast<double>(lcg >> 8) / 8388608.0) - 1.0;
    };
    while (g_run.load()) {
        std::int64_t type, tone, f, speed, ppm, silentAfter, noise;
        Callbacks cb;
        std::uintptr_t user;
        {
            std::lock_guard<std::mutex> lk(g_m);
            type = g_v[kDdcType];
            tone = g_v[kToneHz];
            f = g_v[kF];
            speed = g_v[kSpeed];
            ppm = g_v[kPpmX1000];
            silentAfter = g_v[kSilentAfter];
            noise = g_v[kNoiseX1000];
            cb = g_cb;
            user = g_user;
        }
        const DdcT t = kTypes[type];
        const std::uint32_t N = 4096;
        if (silentAfter != 0 && static_cast<std::int64_t>(n) >= silentAfter) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const double fb = static_cast<double>(tone) + 12.3 - static_cast<double>(f);
        const double actualRate = static_cast<double>(t.rate) * (1.0 + static_cast<double>(ppm) / 1e9);
        const double step = 2.0 * 3.14159265358979323846 * fb / actualRate;
        std::vector<std::int32_t> buf32(static_cast<std::size_t>(N) * 2);
        std::vector<std::int16_t> buf16(static_cast<std::size_t>(N) * 2);
        const double amp = 0.5, na = static_cast<double>(noise) / 1000.0;
        for (std::uint32_t i = 0; i < N; ++i) {
            const double I = amp * std::cos(phase) + na * rnd();
            const double Q = amp * std::sin(phase) + na * rnd();
            phase += step;
            if (phase > 6.283185307179586) { phase -= 6.283185307179586; }
            if (phase < -6.283185307179586) { phase += 6.283185307179586; }
            if (t.bits == 16) {
                buf16[2 * i] = static_cast<std::int16_t>(I * 32767.0);
                buf16[2 * i + 1] = static_cast<std::int16_t>(Q * 32767.0);
            } else {
                buf32[2 * i] = static_cast<std::int32_t>(I * 2147483647.0);
                buf32[2 * i + 1] = static_cast<std::int32_t>(Q * 2147483647.0);
            }
        }
        std::uint32_t bits = t.bits, count = N;
        if (mis(kBadBits)) { bits = 24; }
        if (mis(kBadCount)) { count = 0; }
        if (cb.ddc1StreamCallback != nullptr) {
            cb.ddc1StreamCallback(t.bits == 16 ? static_cast<const void*>(buf16.data()) : static_cast<const void*>(buf32.data()), count, bits, user);
        }
        ++n;
        const double seconds = static_cast<double>(N) / actualRate / static_cast<double>(speed > 0 ? speed : 1);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }
}

}  // namespace

extern "C" {

__declspec(dllexport) void eol_set(int key, std::int64_t v) {
    std::lock_guard<std::mutex> lk(g_m);
    init();
    if (key >= 0 && key < 64) { g_v[key] = v; }
}
__declspec(dllexport) std::int64_t eol_get(int key) {
    std::lock_guard<std::mutex> lk(g_m);
    init();
    return key >= 0 && key < 64 ? g_v[key] : 0;
}
__declspec(dllexport) void eol_reset() {
    if (g_run.load()) {
        g_run.store(false);
        if (gt().joinable()) { gt().join(); }
    }
    std::lock_guard<std::mutex> lk(g_m);
    std::memset(g_v, 0, sizeof g_v);
    g_inited = false;
    g_threads.clear();
    g_maxIn.store(0);
    g_cb = Callbacks{};
    init();
}

__declspec(dllexport) std::int32_t __stdcall GetDeviceList(DeviceInfo* list, std::uint32_t bufferSize) {
    Enter e;
    const bool none = mis(kNoDevices);
    if (list == nullptr) {
        if (bufferSize != 0) { return -1; }
        return none ? 0 : 1;
    }
    if (none) { return 0; }
    if (bufferSize < sizeof(DeviceInfo)) { return -1; }
    std::memset(&list[0], 0, sizeof(DeviceInfo));
    std::memcpy(list[0].serialNumber, "ST330001", 9);
    list[0].channelCount = mis(kBadRecord) ? 99 : 3;
    list[0].ddcTypeCount = 7;
    if (mis(kOverrun)) { std::memset(reinterpret_cast<unsigned char*>(list) + bufferSize, 0x11, 64); }
    return 1;
}

__declspec(dllexport) Handle __stdcall OpenDevice(const char* serial) {
    Enter e;
    if (mis(kOpenFails) || serial == nullptr || std::strcmp(serial, "ST330001") != 0) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return -1;
    }
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kOpens];
    return 9;
}

__declspec(dllexport) Bool __stdcall CloseDevice(Handle h) {
    Enter e;
    if (h != 9) { return 0; }
    if (g_run.load()) {
        g_run.store(false);
        if (gt().joinable()) { gt().join(); }
    }
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kCloses];
    return 1;
}

__declspec(dllexport) Bool __stdcall IsDeviceConnected(Handle h) {
    Enter e;
    if (h != 9) { return 0; }
    if (mis(kDisconnected)) {
        ::SetLastError(ERROR_SUCCESS);
        return 0;
    }
    return 1;
}

__declspec(dllexport) Bool __stdcall GetDeviceInfo(Handle h, DeviceInfo* info, std::uint32_t len) {
    Enter e;
    if (h != 9 || info == nullptr || len < sizeof(DeviceInfo)) { return 0; }
    std::memset(info, 0, sizeof *info);
    std::memcpy(info->serialNumber, "ST330001", 9);
    info->channelCount = 3;
    info->ddcTypeCount = 7;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetPower(Handle h, Bool p) {
    Enter e;
    if (h != 9) { return 0; }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kPower] = p ? 1 : 0;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetPreamp(Handle h, Bool p) {
    Enter e;
    if (h != 9) { return 0; }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kPreamp] = p ? 1 : 0;
    return 1;
}

__declspec(dllexport) Bool __stdcall GetDDCInfo(Handle h, std::uint32_t i, DdcInfo* info) {
    Enter e;
    if (h != 9 || info == nullptr || i >= 7) { return 0; }
    info->sampleRate = kTypes[i].rate;
    info->bandwidth = kTypes[i].bw;
    info->bitsPerSample = kTypes[i].bits;
    return 1;
}

__declspec(dllexport) Bool __stdcall GetDDC1Count(Handle h, std::uint32_t* count) {
    Enter e;
    if (h != 9 || count == nullptr) { return 0; }
    *count = 7;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetDDC1(Handle h, std::uint32_t idx) {
    Enter e;
    // The document: fails while DDC1 streaming runs.
    if (h != 9 || idx >= 7 || g_run.load() || mis(kSetDdcFails)) {
        ::SetLastError(ERROR_BUSY);
        return 0;
    }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kDdcType] = idx;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetDDC1Frequency(Handle h, std::uint32_t f) {
    Enter e;
    // This stand-in tunes 100 kHz to 30 MHz (the document gives no range; this is the stand-in own).
    if (h != 9 || mis(kTuneFails) || f < 100000 || f > 30000000) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kF] = f;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetCallbacks(Handle h, const Callbacks* cb, std::uintptr_t user) {
    Enter e;
    if (h != 9) { return 0; }
    std::lock_guard<std::mutex> lk(g_m);
    if (cb == nullptr) {
        g_cb = Callbacks{};
        g_v[kCallbackSet] = 0;
    } else {
        g_cb = *cb;
        g_v[kCallbackSet] = 1;
    }
    g_user = user;
    return 1;
}

__declspec(dllexport) Bool __stdcall StartDDC1(Handle h, std::uint32_t n) {
    Enter e;
    if (h != 9 || n == 0 || n % 64 != 0 || g_v[kPower] == 0) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (mis(kStartFails)) {
        ::SetLastError(ERROR_GEN_FAILURE);
        return 0;
    }
    if (mis(kStartHangs)) { std::this_thread::sleep_for(std::chrono::seconds(3)); }
    if (mis(kStartCrashes)) {
        volatile int* p = nullptr;
        *p = 1;
    }
    if (g_run.load()) { return 1; }
    {
        std::lock_guard<std::mutex> lk(g_m);
        ++g_v[kStarts];
    }
    g_run.store(true);
    gt() = std::thread(streamLoop);
    return 1;
}

__declspec(dllexport) Bool __stdcall StopDDC1(Handle h) {
    Enter e;
    if (h != 9) { return 0; }
    if (g_run.load()) {
        g_run.store(false);
        if (gt().joinable()) { gt().join(); }
    }
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kStops];
    return 1;
}

}  // extern "C"
