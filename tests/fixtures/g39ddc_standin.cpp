// g39ddc_standin.cpp - a STAND-IN G39DDCAPI.dll for tests/test_g39ddc_source.cpp. Written from the same published document as the bridge
// (src/source/g39ddc_api.hpp), so it can show that the bridge calls what the document says in the order the document says; it cannot show that the
// document was read rightly, and it is not WiNRADiO's code.
//
// It behaves like a receiver: one device, a list of DDC types, a front end with a tuning step and a window, and a streaming thread that calls the
// registered DDC1 callback with a tone (noise, a static frequency error and a sample-clock error included). It also records which thread every
// API call arrived on, so the test can prove the one-thread rule, and it can misbehave on cue (see the keys below) so the bridge's refusals and
// containment can be exercised. NOTHING here is WiNRADiO's.
//
// Control: eol_set(key, value) / eol_get(key), exported by plain name.
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

#include "source/g39ddc_api.hpp"

using namespace cascade::source::g39ddc;

namespace {

enum Key : int {
    kMisbehave = 1,   // bit mask, see below
    kToneHz = 2,      // absolute tone frequency (default 7015000)
    kSpeed = 3,       // streaming speed-up factor (default 20)
    kSilentAfter = 4, // after this many buffers the stream goes quiet (0 = never)
    kPpmX1000 = 5,    // sample-clock error in ppm * 1000 (default 1500000 = 1500 ppm)
    kFe = 10,         // last SetFrontEndFrequency
    kDdc1 = 11,       // last SetDDC1Frequency
    kDdcType = 12,    // last SetDDC1 type index
    kPower = 13,
    kStarts = 14,     // StartDDC1 calls
    kStops = 15,
    kOpens = 16,
    kCloses = 17,
    kThreads = 18,    // number of distinct threads that made API calls
    kOverlap = 19,    // max simultaneous API calls seen
    kPreamp = 20,
    kCallbackSet = 21,  // 1 when a callback is registered
    kCalls = 22,        // total API calls
    kNoiseX1000 = 23,   // noise amplitude * 1000 (default 20)
};
enum Misbehave : std::int64_t {
    kNoDevices = 1 << 0,
    kOpenFails = 1 << 1,
    kStartFails = 1 << 2,
    kStartHangs = 1 << 3,
    kStartCrashes = 1 << 4,
    kBigRecord = 1 << 5,      // reports a 316-byte device record
    kDisconnected = 1 << 6,   // IsDeviceConnected: 0 with no error
    kBadChannel = 1 << 7,     // the callback is called with channel 1 - ours
    kBadBits = 1 << 8,        // the callback says 24 bits
    kBadCount = 1 << 9,       // the callback says 0 samples
    kTuneFails = 1 << 10,
    kSetDdcFails = 1 << 11,
};

std::mutex g_m;
std::int64_t g_v[64] = {};
std::set<DWORD> g_threads;
std::atomic<int> g_inCall{0};
std::atomic<int> g_maxIn{0};
std::atomic<bool> g_run{false};
// Never destroyed: a stream thread still running when the process exits must not make the DLL's static destruction call std::terminate.
std::thread& gt() {
    static std::thread* t = new std::thread;
    return *t;
}
Callbacks g_cb;
std::uintptr_t g_user = 0;
std::uint32_t g_ch = 0;
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
    g_v[kDdcType] = 0;
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
        std::int64_t type, tone, fe, d1, speed, ppm, silentAfter, noise;
        Callbacks cb;
        std::uintptr_t user;
        std::uint32_t ch;
        {
            std::lock_guard<std::mutex> lk(g_m);
            type = g_v[kDdcType];
            tone = g_v[kToneHz];
            fe = g_v[kFe];
            d1 = g_v[kDdc1];
            speed = g_v[kSpeed];
            ppm = g_v[kPpmX1000];
            silentAfter = g_v[kSilentAfter];
            noise = g_v[kNoiseX1000];
            cb = g_cb;
            user = g_user;
            ch = g_ch;
        }
        const DdcT t = kTypes[type];
        const std::uint32_t N = 4096;
        if (silentAfter != 0 && static_cast<std::int64_t>(n) >= silentAfter) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // The tone as the DDC sees it: absolute frequency minus (front end + DDC1 offset), plus a static 12.3 Hz error; the sample clock runs
        // fast by `ppm`, so the phase step per sample is for the faster clock.
        const double fb = static_cast<double>(tone) + 12.3 - static_cast<double>(fe + d1);
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
        std::uint32_t bits = t.bits, count = N, chan = ch;
        if (mis(kBadBits)) { bits = 24; }
        if (mis(kBadCount)) { count = 0; }
        if (mis(kBadChannel)) { chan = ch ^ 1u; }
        if (cb.ddc1StreamCallback != nullptr) {
            cb.ddc1StreamCallback(chan, t.bits == 16 ? static_cast<const void*>(buf16.data()) : static_cast<const void*>(buf32.data()), count, bits, user);
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

__declspec(dllexport) Bool __stdcall GetDeviceList(DeviceInfo* list, std::uint32_t bufferSize, std::uint32_t* count, std::uint32_t* itemSize) {
    Enter e;
    if (count == nullptr || itemSize == nullptr) { return 0; }
    const bool none = mis(kNoDevices);
    *itemSize = mis(kBigRecord) ? static_cast<std::uint32_t>(sizeof(DeviceInfo) + 1) : static_cast<std::uint32_t>(sizeof(DeviceInfo));
    *count = none ? 0 : 1;
    if (list != nullptr && !none) {
        if (bufferSize < sizeof(DeviceInfo)) { return 0; }
        std::memset(&list[0], 0, sizeof(DeviceInfo));
        std::memcpy(list[0].serialNumber, "ST000001", 9);
        list[0].channelCount = 2;
    }
    return 1;
}

__declspec(dllexport) Handle __stdcall OpenDevice(const char* serial) {
    Enter e;
    if (mis(kOpenFails) || serial == nullptr || std::strcmp(serial, "ST000001") != 0) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return -1;
    }
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kOpens];
    return 7;
}

__declspec(dllexport) Bool __stdcall CloseDevice(Handle h) {
    Enter e;
    if (h != 7) { return 0; }
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
    if (h != 7) { return 0; }
    if (mis(kDisconnected)) {
        ::SetLastError(ERROR_SUCCESS);
        return 0;
    }
    return 1;
}

__declspec(dllexport) Bool __stdcall GetDeviceInfo(Handle h, DeviceInfo* info, std::uint32_t len) {
    Enter e;
    if (h != 7 || info == nullptr || len < sizeof(DeviceInfo)) { return 0; }
    std::memset(info, 0, sizeof *info);
    std::memcpy(info->serialNumber, "ST000001", 9);
    info->channelCount = 2;
    info->frontEndMinFrequency = 100000;
    info->frontEndMaxFrequency = 30000000;
    info->frontEndMaxHFFrequency = 30000000;
    info->frontEndFrequencyStep = 10000;
    info->frontEndWindowWidth = 2000000;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetPower(Handle h, Bool p) {
    Enter e;
    if (h != 7) { return 0; }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kPower] = p ? 1 : 0;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetPreamplifier(Handle h, Bool p) {
    Enter e;
    if (h != 7) { return 0; }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kPreamp] = p ? 1 : 0;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetFrontEndFrequency(Handle h, std::uint64_t f) {
    Enter e;
    if (h != 7 || mis(kTuneFails)) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    // The document: a multiple of the step unless it equals the minimum, and inside the range.
    if (f < 100000 || f > 30000000 || (f != 100000 && f % 10000 != 0)) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kFe] = static_cast<std::int64_t>(f);
    return 1;
}

__declspec(dllexport) Bool __stdcall GetDDCInfo(std::uint32_t i, DdcInfo* info) {
    Enter e;
    if (info == nullptr || i >= 7) { return 0; }
    info->sampleRate = kTypes[i].rate;
    info->bandwidth = kTypes[i].bw;
    info->bitsPerSample = kTypes[i].bits;
    return 1;
}

__declspec(dllexport) Bool __stdcall GetDDC1Count(Handle h, std::uint32_t ch, std::uint32_t* count) {
    Enter e;
    if (h != 7 || ch > 1 || count == nullptr) { return 0; }
    *count = 7;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetDDC1(Handle h, std::uint32_t ch, std::uint32_t idx) {
    Enter e;
    // The document: fails while DDC1 streaming runs.
    if (h != 7 || ch > 1 || idx >= 7 || g_run.load() || mis(kSetDdcFails)) {
        ::SetLastError(ERROR_BUSY);
        return 0;
    }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kDdcType] = idx;
    g_ch = ch;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetDDC1Frequency(Handle h, std::uint32_t ch, std::int32_t f) {
    Enter e;
    if (h != 7 || ch > 1) { return 0; }
    // The document: within +-(window/2 - bandwidth/2) of the front end.
    std::lock_guard<std::mutex> lk(g_m);
    const std::int64_t lim = 1000000 - static_cast<std::int64_t>(kTypes[g_v[kDdcType]].bw) / 2;
    if (std::llabs(static_cast<std::int64_t>(f)) > lim) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    g_v[kDdc1] = f;
    return 1;
}

__declspec(dllexport) Bool __stdcall SetCallbacks(Handle h, const Callbacks* cb, std::uintptr_t user) {
    Enter e;
    if (h != 7) { return 0; }
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

__declspec(dllexport) Bool __stdcall StartDDC1(Handle h, std::uint32_t ch, std::uint32_t n) {
    Enter e;
    if (h != 7 || ch > 1 || n == 0 || n % 64 != 0 || g_v[kPower] == 0) {
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
        g_ch = ch;
    }
    g_run.store(true);
    gt() = std::thread(streamLoop);
    return 1;
}

__declspec(dllexport) Bool __stdcall StopDDC1(Handle h, std::uint32_t ch) {
    Enter e;
    (void)ch;
    if (h != 7) { return 0; }
    if (g_run.load()) {
        g_run.store(false);
        if (gt().joinable()) { gt().join(); }
    }
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kStops];
    return 1;
}

}  // extern "C"
