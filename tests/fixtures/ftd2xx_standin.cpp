// ftd2xx_standin.cpp - a STAND-IN ftd2xx.dll for tests/test_sdriq_d2xx.cpp. Written from FTDI's published "D2XX Programmer's Guide" (the same document
// the bridge's declarations come from, src/source/ftd2xx_api.hpp), so it can show that the bridge calls what the guide says in the order the guide says;
// it cannot show that the guide was read rightly, and it is not FTDI's code.
//
// It lists a configurable set of FTDI devices (one of them an "SDR-IQ"), and behind the SDR-IQ it plays a small radio: it answers the interface
// specification's name / serial / status / range / frequency / gain / rate requests, runs and stops on the receiver-state message, and while running
// hands the reader one Data Item 0 block (a tone with noise, a static error and a sample-clock error) whenever its queue is empty. It records every
// call and can misbehave on cue. Control: eol_set(key, value) / eol_get(key) / eol_reset(), exported by plain name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "source/ftd2xx_api.hpp"
#include "source/sdriq_protocol.hpp"

namespace ft = cascade::source::ftd2xx;
namespace as = cascade::source::ascp;
namespace sq = cascade::source::sdriq;

namespace {

enum Key : int {
    kMisbehave = 1,
    kNumDevices = 2,      // devices listed (default 1)
    kSdrIqIndex = 3,      // which of them is the SDR-IQ (default 0; -1 none)
    kLostAfterBlocks = 4, // GetQueueStatus fails (I/O error: the USB cable was pulled) after this many data blocks (0 = never)
    kToneHzX10 = 5,
    kPpmX10 = 6,
    kOpens = 16,
    kCloses = 17,
    kReads = 18,
    kWrites = 19,
    kZeroTimeouts = 20,   // SetTimeouts calls with a read timeout of 0 (which D2XX reads as "wait forever")
    kLastReadTimeout = 21,
    kWritesToOther = 22,  // bytes written to a device that is not the SDR-IQ
    kForbiddenItems = 23, // Set messages for any item other than state / frequency / RF gain / IF gain / output rate
    kBlocks = 24,
    kRunning = 25,
    kBytesWritten = 26,
    kCalls = 27,
    kOpenHandles = 28,
    kMaxRead = 29,        // largest dwBytesToRead seen
};
enum : std::int64_t {
    mOverrun = 1 << 0,        // ListDevices writes 48 bytes past the buffer it was given
    mOpenFails = 1 << 1,      // FT_OpenEx returns FT_DEVICE_NOT_OPENED
    mReadCrashes = 1 << 2,    // FT_Read faults
    mReadHangs = 1 << 3,      // FT_Read does not return for 30 s
    mReadLies = 1 << 4,       // FT_Read reports more bytes than it was asked for (and writes none)
    mReadFails = 1 << 5,      // FT_Read returns FT_IO_ERROR
    mWriteFails = 1 << 6,     // FT_Write returns FT_IO_ERROR
    mWriteZero = 1 << 7,      // FT_Write returns FT_OK and writes nothing
    mSilent = 1 << 8,         // the radio answers nothing
    mSetTimeoutsFails = 1 << 9,
    mListFails = 1 << 10,     // FT_ListDevices returns FT_OTHER_ERROR
    mNoSerialList = 1 << 11,  // FT_ListDevices for a serial string returns FT_DEVICE_NOT_FOUND
    mUnterminated = 1 << 12,  // the strings come back without a terminator
    mWriteShort = 1 << 13,    // FT_Write takes at most 3 bytes per call
    mBadName = 1 << 14,       // the radio calls itself something else
};

std::mutex g_mu;
std::int64_t g_v[64] = {};
std::deque<std::uint8_t> g_out;
as::StreamAssembler g_asm;
bool g_running = false;
std::uint32_t g_rateCode = sq::kPowerUpRate;
std::uint64_t g_freq = 0;
int g_rf = 0, g_if = 0;
double g_phase = 0.0;
std::mt19937 g_rng{4242};
std::uint32_t g_readTimeoutMs = 0;
void* const kHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5D1A0001));
bool g_open = false;
bool g_openIsSdrIq = false;

void resetLocked() {
    std::memset(g_v, 0, sizeof g_v);
    g_v[kNumDevices] = 1;
    g_v[kSdrIqIndex] = 0;
    g_v[kToneHzX10] = 123456;
    g_v[kPpmX10] = 15000;  // 1500 ppm
    g_out.clear();
    g_asm = as::StreamAssembler();
    g_running = false;
    g_rateCode = sq::kPowerUpRate;
    g_freq = 0;
    g_rf = g_if = 0;
    g_phase = 0;
    g_open = false;
    g_openIsSdrIq = false;
}

std::string serialOf(int i) { return i == g_v[kSdrIqIndex] ? "MT123456" : "FT0ABC" + std::to_string(i); }
std::string descriptionOf(int i) { return i == g_v[kSdrIqIndex] ? "SDR-IQ" : "USB Serial Converter"; }

void queue(const std::vector<std::uint8_t>& b) { g_out.insert(g_out.end(), b.begin(), b.end()); }
void reply(as::MsgType t, std::uint16_t item, const std::vector<std::uint8_t>& body) { queue(as::encodeMessage(t, item, body)); }
std::vector<std::uint8_t> cstr(const std::string& s) {
    std::vector<std::uint8_t> b(s.begin(), s.end());
    b.push_back(0);
    return b;
}

void handle(const as::Message& m) {
    if (g_v[kMisbehave] & mSilent) { return; }
    using Bytes = std::vector<std::uint8_t>;
    if (m.type == as::MsgType::Request) {
        switch (m.item) {
            case sq::ci::Name: reply(as::MsgType::Set, m.item, cstr((g_v[kMisbehave] & mBadName) ? "NetSDR" : "SDR-IQ")); return;
            case sq::ci::Serial: reply(as::MsgType::Set, m.item, cstr("MT123456")); return;
            case sq::ci::Status: reply(as::MsgType::Set, m.item, Bytes{static_cast<std::uint8_t>(sq::kStatusIdle)}); return;
            case sq::ci::RxFrequency: {
                Bytes b{0x00};
                as::putFrequency(b, g_freq);
                reply(as::MsgType::Set, m.item, b);
                return;
            }
            case sq::ci::RfGain: reply(as::MsgType::Set, m.item, Bytes{0x00, static_cast<std::uint8_t>(static_cast<std::int8_t>(g_rf))}); return;
            default: queue(Bytes{0x02, 0x00}); return;
        }
    }
    if (m.type == as::MsgType::RequestRange && m.item == sq::ci::RxFrequency) {
        Bytes b{0x00};
        as::putFrequency(b, 0);
        as::putFrequency(b, 30000000);
        reply(as::MsgType::RequestRange, m.item, b);
        return;
    }
    if (m.type == as::MsgType::Set) {
        const bool allowed = m.item == sq::ci::RxState || m.item == sq::ci::RxFrequency || m.item == sq::ci::RfGain || m.item == sq::ci::IfGain ||
                             m.item == sq::ci::OutSampleRate;
        if (!allowed) { ++g_v[kForbiddenItems]; }
        switch (m.item) {
            case sq::ci::RxState:
                if (m.body.size() >= 2 && m.body[0] == sq::kChannel) {
                    g_running = m.body[1] == sq::kRunRun;
                    g_v[kRunning] = g_running ? 1 : 0;
                    reply(as::MsgType::Set, m.item, m.body);
                } else {
                    queue(Bytes{0x02, 0x00});
                }
                return;
            case sq::ci::OutSampleRate: {
                if (m.body.size() >= 5 && !g_running) {
                    const std::uint32_t c = static_cast<std::uint32_t>(as::getLe(m.body.data() + 1, 4));
                    for (const sq::RateCode& r : sq::kRates) {
                        if (r.code == c) { g_rateCode = c; }
                    }
                }
                Bytes b{0x00};
                as::putLe(b, g_rateCode, 4);
                reply(as::MsgType::Set, m.item, b);
                return;
            }
            case sq::ci::RxFrequency:
                if (m.body.size() >= 6) { g_freq = as::getFrequency(m.body.data() + 1); }
                reply(as::MsgType::Set, m.item, m.body);
                return;
            case sq::ci::RfGain:
                if (m.body.size() >= 2) { g_rf = static_cast<std::int8_t>(m.body[1]); }
                reply(as::MsgType::Set, m.item, m.body);
                return;
            case sq::ci::IfGain:
                if (m.body.size() >= 2) { g_if = m.body[1]; }
                reply(as::MsgType::Set, m.item, m.body);
                return;
            default: queue(Bytes{0x02, 0x00}); return;
        }
    }
    queue(Bytes{0x02, 0x00});
}

void makeBlock() {
    const double nominal = sq::rateHzFor(g_rateCode);
    const double fs = nominal * (1.0 + static_cast<double>(g_v[kPpmX10]) / 10.0 * 1e-6);
    const double w = 2.0 * 3.14159265358979323846 * (static_cast<double>(g_v[kToneHzX10]) / 10.0) / fs;
    std::normal_distribution<double> nd(0.0, 0.02);
    std::vector<std::uint8_t> b{0x00, 0x80};
    for (std::size_t i = 0; i < sq::kDataBlockPairs; ++i) {
        const double I = 0.5 * std::cos(g_phase) + nd(g_rng);
        const double Q = 0.5 * std::sin(g_phase) + nd(g_rng);
        g_phase += w;
        if (g_phase > 6.283185307179586) { g_phase -= 6.283185307179586; }
        auto put = [&](double v) {
            const long s = std::clamp(std::lround(v * 32768.0), -32768L, 32767L);
            as::putLe(b, static_cast<std::uint16_t>(static_cast<std::int16_t>(s)), 2);
        };
        put(I);
        put(Q);
    }
    queue(b);
    ++g_v[kBlocks];
}

}  // namespace

#define EXPORT extern "C" __declspec(dllexport)

EXPORT void eol_reset() {
    std::lock_guard<std::mutex> lk(g_mu);
    resetLocked();
}
EXPORT void eol_set(int key, std::int64_t value) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (key >= 0 && key < 64) { g_v[key] = value; }
}
EXPORT std::int64_t eol_get(int key) {
    std::lock_guard<std::mutex> lk(g_mu);
    return key >= 0 && key < 64 ? g_v[key] : 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        std::lock_guard<std::mutex> lk(g_mu);
        resetLocked();
    }
    return TRUE;
}

EXPORT ft::Status __stdcall FT_ListDevices(void* a1, void* a2, std::uint32_t flags) {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_v[kCalls];
    if (g_v[kMisbehave] & mListFails) { return ft::kOtherError; }
    if (flags & ft::kListNumberOnly) {
        if (a1 == nullptr) { return ft::kInvalidParameter; }
        *static_cast<std::uint32_t*>(a1) = static_cast<std::uint32_t>(g_v[kNumDevices]);
        return ft::kOk;
    }
    if (flags & ft::kListByIndex) {
        const int i = static_cast<int>(reinterpret_cast<std::uintptr_t>(a1));
        if (i < 0 || i >= g_v[kNumDevices] || a2 == nullptr) { return ft::kDeviceNotFound; }
        const bool wantDesc = (flags & ft::kOpenByDescription) != 0;
        if (!wantDesc && (g_v[kMisbehave] & mNoSerialList)) { return ft::kDeviceNotFound; }
        const std::string s = wantDesc ? descriptionOf(i) : serialOf(i);
        char* out = static_cast<char*>(a2);
        if (g_v[kMisbehave] & mUnterminated) {
            std::memset(out, 'A', 256);
            return ft::kOk;
        }
        std::memcpy(out, s.c_str(), s.size() + 1);
        if (g_v[kMisbehave] & mOverrun) { std::memset(out + 256, 0x11, 48); }
        return ft::kOk;
    }
    return ft::kInvalidParameter;
}

EXPORT ft::Status __stdcall FT_OpenEx(void* a1, std::uint32_t flags, ft::Handle* h) {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_v[kCalls];
    if (g_v[kMisbehave] & mOpenFails) { return ft::kDeviceNotOpened; }
    if (h == nullptr || a1 == nullptr || flags != ft::kOpenBySerialNumber) { return ft::kInvalidParameter; }
    const std::string want(static_cast<const char*>(a1));
    for (int i = 0; i < g_v[kNumDevices]; ++i) {
        if (serialOf(i) == want) {
            if (g_open) { return ft::kDeviceNotOpened; }
            g_open = true;
            g_openIsSdrIq = i == g_v[kSdrIqIndex];
            *h = kHandle;
            ++g_v[kOpens];
            ++g_v[kOpenHandles];
            return ft::kOk;
        }
    }
    return ft::kDeviceNotFound;
}

EXPORT ft::Status __stdcall FT_Close(ft::Handle h) {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_v[kCalls];
    if (h != kHandle || !g_open) { return ft::kInvalidHandle; }
    g_open = false;
    g_running = false;
    g_v[kRunning] = 0;
    ++g_v[kCloses];
    --g_v[kOpenHandles];
    return ft::kOk;
}

EXPORT ft::Status __stdcall FT_SetTimeouts(ft::Handle h, std::uint32_t readMs, std::uint32_t) {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_v[kCalls];
    if (h != kHandle || !g_open) { return ft::kInvalidHandle; }
    if (g_v[kMisbehave] & mSetTimeoutsFails) { return ft::kIoError; }
    if (readMs == 0) { ++g_v[kZeroTimeouts]; }
    g_readTimeoutMs = readMs;
    g_v[kLastReadTimeout] = readMs;
    return ft::kOk;
}

EXPORT ft::Status __stdcall FT_Purge(ft::Handle h, std::uint32_t mask) {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_v[kCalls];
    if (h != kHandle || !g_open) { return ft::kInvalidHandle; }
    if (mask & ft::kPurgeRx) { g_out.clear(); }
    return ft::kOk;
}

EXPORT ft::Status __stdcall FT_GetQueueStatus(ft::Handle h, std::uint32_t* n) {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_v[kCalls];
    if (h != kHandle || !g_open || n == nullptr) { return ft::kInvalidHandle; }
    if (g_v[kLostAfterBlocks] != 0 && g_v[kBlocks] >= g_v[kLostAfterBlocks]) { return ft::kIoError; }  // the cable was pulled
    *n = static_cast<std::uint32_t>(g_out.size());
    return ft::kOk;
}

EXPORT ft::Status __stdcall FT_Write(ft::Handle h, void* buf, std::uint32_t n, std::uint32_t* written) {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_v[kCalls];
    if (h != kHandle || !g_open || buf == nullptr || written == nullptr) { return ft::kInvalidHandle; }
    if (g_v[kMisbehave] & mWriteFails) { *written = 0; return ft::kIoError; }
    if (g_v[kMisbehave] & mWriteZero) { *written = 0; return ft::kOk; }
    std::uint32_t take = n;
    if (g_v[kMisbehave] & mWriteShort) { take = std::min<std::uint32_t>(n, 3); }
    ++g_v[kWrites];
    g_v[kBytesWritten] += take;
    if (!g_openIsSdrIq) {
        g_v[kWritesToOther] += take;
        *written = take;
        return ft::kOk;
    }
    std::vector<as::Message> msgs;
    g_asm.feed(static_cast<const std::uint8_t*>(buf), take, msgs);
    for (const as::Message& m : msgs) { handle(m); }
    *written = take;
    return ft::kOk;
}

EXPORT ft::Status __stdcall FT_Read(ft::Handle h, void* buf, std::uint32_t want, std::uint32_t* got) {
    std::uint32_t timeout = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_v[kCalls];
        ++g_v[kReads];
        if (h != kHandle || !g_open || buf == nullptr || got == nullptr) { return ft::kInvalidHandle; }
        g_v[kMaxRead] = std::max<std::int64_t>(g_v[kMaxRead], want);
        if (g_v[kMisbehave] & mReadFails) { *got = 0; return ft::kIoError; }
        timeout = g_readTimeoutMs;
    }
    if (g_v[kMisbehave] & mReadCrashes) {
        volatile int* p = nullptr;
        *p = 1;
    }
    if (g_v[kMisbehave] & mReadHangs) { std::this_thread::sleep_for(std::chrono::seconds(30)); }
    if (g_v[kMisbehave] & mReadLies) { *got = want + 100; return ft::kOk; }
    std::unique_lock<std::mutex> lk(g_mu);
    if (g_out.empty() && g_running && g_openIsSdrIq) {
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // about 1 MS/s: faster than the radio
        lk.lock();
        if (g_out.empty() && g_running) { makeBlock(); }
    }
    if (g_out.empty()) {
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::uint32_t>(timeout, 20)));  // the timeout (shortened)
        lk.lock();
    }
    std::uint32_t n = 0;
    auto* dst = static_cast<std::uint8_t*>(buf);
    while (n < want && !g_out.empty()) {
        dst[n++] = g_out.front();
        g_out.pop_front();
    }
    *got = n;
    return ft::kOk;
}
