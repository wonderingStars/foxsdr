// fobos_standin.cpp - a STAND-IN Fobos SDR library for tests/test_fobos_source.cpp. Written from the same reading of the maker's public header as the bridge
// (src/source/fobos_api.hpp), so it can show that the bridge calls what the header says in the order the header says; it cannot show that the header was read
// rightly, and it is not the maker's code.
//
// One board, a list of sample rates, a tuned frequency, LNA and VGA settings, and a stream that calls the registered callback with a tone (noise, a static
// frequency error and a sample-clock error included), either on the calling thread until cancelled (the default) or on a thread of its own with the call
// returning at once. It exports the four calls that change the device (firmware write and read, reset, output pins) ONLY to count them: the test proves the
// count stays zero. It misbehaves on cue. Control: eol_set(key, value) / eol_get(key), exported by plain name.
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

#include "source/fobos_api.hpp"

using namespace cascade::source::fobos;

namespace {

enum Key : int {
    kMisbehave = 1,
    kToneHz = 2,
    kSpeed = 3,
    kSilentAfter = 4,       // buffers delivered before the stream goes quiet (0 = never)
    kPpmX1000 = 5,
    kUnplugAfter = 6,       // buffers delivered before the stream call returns an error (0 = never)
    kFreq = 10,
    kRate = 11,
    kLna = 12,
    kVga = 13,
    kDirect = 14,
    kClock = 15,
    kStarts = 16,
    kCancels = 17,
    kOpens = 18,
    kCloses = 19,
    kThreads = 20,
    kOverlap = 21,
    kCalls = 22,
    kNoiseX1000 = 23,
    kForbiddenCalls = 24,        // the total of calls to the four device-changing functions: must stay 0
    kLenPairs = 25,         // 1: the stream length counts complex samples (the buffer holds twice as many floats)
    kLateCallbacks = 26,    // callbacks made after the cancel call returned
    kStreamLen = 27,        // the length the stream was asked for
    kBufCount = 28,
    kListed = 29,           // 1 when the radio list was asked for
    kNonBlocking = 30,      // 1: the stream call returns at once and streams on a thread of its own
};
enum Misbehave : std::int64_t {
    mNoDevices = 1 << 0,
    mOpenFails = 1 << 1,
    mStartFails = 1 << 2,
    mStartHangs = 1 << 3,    // the stream call never returns and cancel does nothing
    mStartCrashes = 1 << 4,
    mListOverrun = 1 << 5,   // the radio list is written 64 bytes past the end
    mInfoOverrun = 1 << 6,   // the board info is written past the documented size
    mRatesOverrun = 1 << 7,
    mBadLength = 1 << 8,     // the callback length is far larger than asked
    mZeroLength = 1 << 9,
    mOddLength = 1 << 10,
    mNullBuffer = 1 << 11,
    mTuneFails = 1 << 12,
    mRateFails = 1 << 13,
    mGainFails = 1 << 14,
    mLateAfterCancel = 1 << 15,
    mCancelHangs = 1 << 16,  // cancel never returns
    mRatesNone = 1 << 17,
    mCountHuge = 1 << 18,
    mTuneCrashes = 1 << 19,
    mSerialsEmpty = 1 << 20,
};

std::mutex g_m;
std::int64_t g_v[64] = {};
std::set<DWORD> g_threads;
std::atomic<int> g_inCall{0};
std::atomic<int> g_maxIn{0};
std::atomic<bool> g_run{false};
std::atomic<bool> g_loopExited{true};
std::thread& gt() {
    static std::thread* t = new std::thread;   // never destroyed: a thread still running at exit must not make the DLL's static destruction call std::terminate
    return *t;
}
bool g_inited = false;

const double kRates[] = {1.0e6, 2.0e6, 5.0e6, 10.0e6, 25.0e6, 50.0e6};

void init() {
    if (g_inited) { return; }
    g_inited = true;
    g_v[kToneHz] = 7015000;
    g_v[kSpeed] = 20;
    g_v[kPpmX1000] = 1500000;
    g_v[kNoiseX1000] = 20;
    g_v[kRate] = 2000000;
}

struct Enter {
    explicit Enter(bool control = true) {
        std::lock_guard<std::mutex> lk(g_m);
        init();
        if (control) { g_threads.insert(::GetCurrentThreadId()); }   // the streaming call runs on a thread of its own and is not a control call
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

Device* const kHandle = reinterpret_cast<Device*>(static_cast<std::uintptr_t>(0x4f0b05));

// Returns 0 when it ended because it was cancelled, a negative code when the board "went away".
int streamLoop(StreamCallback cb, void* ctx, std::uint32_t bufCount, std::uint32_t length) {
    (void)bufCount;
    std::uint64_t n = 0;
    double phase = 0.0;
    std::uint32_t lcg = 12345;
    const auto rnd = [&]() {
        lcg = lcg * 1664525u + 1013904223u;
        return (static_cast<double>(lcg >> 8) / 8388608.0) - 1.0;
    };
    int ret = 0;
    auto due = std::chrono::steady_clock::now();
    while (g_run.load()) {
        std::int64_t tone, f, rate, speed, ppm, silentAfter, unplugAfter, noise, pairs;
        {
            std::lock_guard<std::mutex> lk(g_m);
            tone = g_v[kToneHz];
            f = g_v[kFreq];
            rate = g_v[kRate];
            speed = g_v[kSpeed];
            ppm = g_v[kPpmX1000];
            silentAfter = g_v[kSilentAfter];
            unplugAfter = g_v[kUnplugAfter];
            noise = g_v[kNoiseX1000];
            pairs = g_v[kLenPairs];
        }
        if (unplugAfter != 0 && static_cast<std::int64_t>(n) >= unplugAfter) {
            ret = kErrUsb;
            break;
        }
        if (silentAfter != 0 && static_cast<std::int64_t>(n) >= silentAfter) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const double fb = static_cast<double>(tone) + 12.3 - static_cast<double>(f);
        const double actualRate = static_cast<double>(rate) * (1.0 + static_cast<double>(ppm) / 1e9);
        const double step = 2.0 * 3.14159265358979323846 * fb / actualRate;
        const std::uint32_t floats = pairs != 0 ? length * 2u : length;
        std::vector<float> buf(floats);
        const double amp = 0.5, na = static_cast<double>(noise) / 1000.0;
        for (std::uint32_t i = 0; i + 1 < floats; i += 2) {
            buf[i] = static_cast<float>(amp * std::cos(phase) + na * rnd());
            buf[i + 1] = static_cast<float>(amp * std::sin(phase) + na * rnd());
            phase += step;
            if (phase > 6.283185307179586) { phase -= 6.283185307179586; }
            if (phase < -6.283185307179586) { phase += 6.283185307179586; }
        }
        std::uint32_t len = length;
        float* ptr = buf.data();
        if (mis(mBadLength)) { len = length * 64u; }
        if (mis(mZeroLength)) { len = 0; }
        if (mis(mOddLength)) { len = length - 1u; }   // an odd number of floats when lengths count floats
        if (mis(mNullBuffer)) { ptr = nullptr; }
        cb(ptr, len, ctx);
        ++n;
        const double seconds = static_cast<double>(pairs != 0 ? length : length / 2u) / actualRate / static_cast<double>(speed > 0 ? speed : 1);
        due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        std::this_thread::sleep_until(due);
    }
    if (mis(mLateAfterCancel)) {
        // Callbacks that arrive after the stop was asked for (and, when the call is non-blocking, after cancel has returned).
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::vector<float> b(length, 0.25f);
        for (int i = 0; i < 3; ++i) {
            cb(b.data(), length, ctx);
            std::lock_guard<std::mutex> lk(g_m);
            ++g_v[kLateCallbacks];
        }
    }
    return ret;
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
    init();
}

__declspec(dllexport) int __cdecl fobos_rx_get_device_count(void) {
    Enter e;
    if (mis(mNoDevices)) { return 0; }
    if (mis(mCountHuge)) { return 5000; }
    return 1;
}

__declspec(dllexport) int __cdecl fobos_rx_list_devices(char* serials) {
    Enter e;
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_v[kListed] = 1;
    }
    if (serials == nullptr) { return -1; }
    const char* s = mis(mSerialsEmpty) ? "" : "F0B05001";
    std::strcpy(serials, s);
    if (mis(mListOverrun)) { std::memset(serials + kInfoLen + 1, 0x11, 64); }
    return 1;
}

__declspec(dllexport) int __cdecl fobos_rx_open(Device** out, std::uint32_t index) {
    Enter e;
    if (mis(mOpenFails) || out == nullptr || index != 0) { return kErrNoDevice; }
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kOpens];
    *out = kHandle;
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_close(Device* d) {
    Enter e;
    if (d != kHandle) { return kErrNotOpen; }
    if (g_run.load() && !mis(mStartHangs)) {
        g_run.store(false);
        if (gt().joinable()) { gt().join(); }
    }
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kCloses];
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_get_board_info(Device* d, char* hw, char* fw, char* maker, char* product, char* serial) {
    Enter e;
    if (d != kHandle || hw == nullptr || fw == nullptr || maker == nullptr || product == nullptr || serial == nullptr) { return kErrNotOpen; }
    std::strcpy(hw, "3.0.0");
    std::strcpy(fw, "2.4.1");
    std::strcpy(maker, "Standin");
    std::strcpy(product, "Fobos SDR (stand-in)");
    std::strcpy(serial, "F0B05001");
    if (mis(mInfoOverrun)) { std::memset(serial + kInfoLen, 0x22, 32); }
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_set_frequency(Device* d, double hz, double* actual) {
    Enter e;
    if (mis(mTuneCrashes)) {
        volatile int* p = nullptr;
        *p = 1;
    }
    // This stand-in tunes 50 kHz to 6 GHz (the header gives no range; this is the stand-in's own), in 1 kHz steps.
    if (d != kHandle || mis(mTuneFails) || hz < 50000.0 || hz > 6.0e9) { return kErrUnsupported; }
    const double got = std::round(hz / 1000.0) * 1000.0;
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_v[kFreq] = static_cast<std::int64_t>(got);
    }
    if (actual != nullptr) { *actual = got; }
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_set_direct_sampling(Device* d, unsigned int on) {
    Enter e;
    if (d != kHandle) { return kErrNotOpen; }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kDirect] = on;
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_set_lna_gain(Device* d, unsigned int v) {
    Enter e;
    if (d != kHandle || v > 3 || mis(mGainFails)) { return kErrUnsupported; }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kLna] = v;
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_set_vga_gain(Device* d, unsigned int v) {
    Enter e;
    if (d != kHandle || v > 31 || mis(mGainFails)) { return kErrUnsupported; }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kVga] = v;
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_get_samplerates(Device* d, double* values, unsigned int* count) {
    Enter e;
    if (d != kHandle || values == nullptr || count == nullptr) { return kErrNotOpen; }
    if (mis(mRatesNone)) {
        *count = 0;
        return 0;
    }
    const unsigned int n = static_cast<unsigned int>(sizeof kRates / sizeof kRates[0]);
    for (unsigned int i = 0; i < n; ++i) { values[i] = kRates[i]; }
    *count = n;
    if (mis(mRatesOverrun)) {
        for (unsigned int i = n; i < 300; ++i) { values[i] = 1.0e6 + i; }
        *count = 300;
    }
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_set_samplerate(Device* d, double hz, double* actual) {
    Enter e;
    if (d != kHandle || mis(mRateFails) || g_run.load()) { return kErrControl; }
    double best = kRates[0];
    for (const double r : kRates) {
        if (std::fabs(r - hz) < std::fabs(best - hz)) { best = r; }
    }
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_v[kRate] = static_cast<std::int64_t>(best);
    }
    if (actual != nullptr) { *actual = best; }
    return 0;
}

__declspec(dllexport) int __cdecl fobos_rx_set_clk_source(Device* d, int v) {
    Enter e;
    if (d != kHandle) { return kErrNotOpen; }
    std::lock_guard<std::mutex> lk(g_m);
    g_v[kClock] = v;
    return 0;
}

__declspec(dllexport) const char* __cdecl fobos_rx_error_name(int error) {
    switch (error) {
        case 0: return "no error";
        case kErrNoDevice: return "no device";
        case kErrNotOpen: return "device not open";
        case kErrControl: return "control transfer failed";
        case kErrUnsupported: return "unsupported";
        case kErrUsb: return "usb transfer failed";
        default: return "unknown error";
    }
}

__declspec(dllexport) int __cdecl fobos_rx_read_async(Device* d, StreamCallback cb, void* ctx, std::uint32_t bufCount, std::uint32_t length) {
    Enter e(false);
    if (d != kHandle || cb == nullptr) { return kErrNotOpen; }
    if (mis(mStartFails)) { return kErrControl; }
    if (mis(mStartCrashes)) {
        volatile int* p = nullptr;
        *p = 1;
    }
    if (mis(mStartHangs)) {
        // Never returns in a way the bridge can wait for: sleeps until the process ends, and ignores cancel.
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    bool nonBlocking;
    {
        std::lock_guard<std::mutex> lk(g_m);
        ++g_v[kStarts];
        g_v[kStreamLen] = length;
        g_v[kBufCount] = bufCount;
        nonBlocking = g_v[kNonBlocking] != 0;
    }
    if (g_run.load()) { return kErrControl; }
    g_run.store(true);
    g_loopExited.store(false);
    if (nonBlocking) {
        gt() = std::thread([cb, ctx, bufCount, length] {
            streamLoop(cb, ctx, bufCount, length);
            g_loopExited.store(true);
        });
        return 0;
    }
    const int rc = streamLoop(cb, ctx, bufCount, length);
    g_run.store(false);
    g_loopExited.store(true);
    return rc;
}

__declspec(dllexport) int __cdecl fobos_rx_cancel_async(Device* d) {
    Enter e;
    if (d != kHandle) { return kErrNotOpen; }
    {
        std::lock_guard<std::mutex> lk(g_m);
        ++g_v[kCancels];
    }
    if (mis(mCancelHangs)) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (mis(mStartHangs)) { return 0; }
    g_run.store(false);
    if (gt().joinable()) { gt().join(); }
    // Blocking mode: wait for the stream call's own thread to leave the loop, as a real library's cancel does.
    for (int i = 0; i < 2000 && !g_loopExited.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    return 0;
}

// The four calls that change the device. Exported ONLY so the test can count calls to them (the bridge never resolves them).
__declspec(dllexport) int __cdecl fobos_rx_write_firmware(Device*, const char*, int) {
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kForbiddenCalls];
    return 0;
}
__declspec(dllexport) int __cdecl fobos_rx_read_firmware(Device*, const char*, int) {
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kForbiddenCalls];
    return 0;
}
__declspec(dllexport) int __cdecl fobos_rx_reset(Device*) {
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kForbiddenCalls];
    return 0;
}
__declspec(dllexport) int __cdecl fobos_rx_set_user_gpo(Device*, std::uint8_t) {
    std::lock_guard<std::mutex> lk(g_m);
    ++g_v[kForbiddenCalls];
    return 0;
}

}  // extern "C"
