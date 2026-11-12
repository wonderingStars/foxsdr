// test_pa_open_list_race.cpp - PortAudio's list of open streams survives streams opened and closed on many threads at once
// (third_party/portaudio/FOXSDR-PATCHES.md, open-stream-list-lock).
//
// THE FAULT. test_engine_api_merge runs 24 engines at once, each with an audio output that opens the default device on a worker.
// PortAudio keeps its open streams in one linked list with no lock (Pa_OpenStream adds to it as its last act, Pa_CloseStream removes
// from it as its first), and AudioOut's guard around that list is a NoWait one - it goes ahead without the lock when the lock is busy,
// which on 24 threads it often is. Two writers on the list lost nodes and left one pointing at a freed stream; the last Pa_Terminate
// then closed it: an access violation in Pa_Terminate (pa_front.c CloseOpenStreams) after 160 to 260 s, in three of three runs under
// load (symbolised: Pa_Terminate+0x64 and Pa_CloseStream+0x48, called from sink::paTerminateShared).
//
// THE RACE ITSELF IS TOO NARROW TO CATCH ON DEMAND (a list change is microseconds inside an open that takes milliseconds: 12 threads
// x 1,440 opens in a quiet run changed nothing), so the test proves the PROPERTY that makes it impossible, deterministically:
//
//   L  the list's lock is taken by hand; an open on another thread then cannot finish (its list change waits) until the lock is
//      let go, and a close cannot either; after the release both complete and the list is as it should be
//
// and also stresses it:
//
// THIS TEST goes round the guard on purpose: the threads call Pa_OpenStream and Pa_CloseStream with NO PaStreamListGuard at all, so
// what is under test is the list's own protection. Before the patch the count of streams the list holds (PaFoxsdr_OpenStreamCount,
// the patch's own read of it) is wrong after the race, or the process dies; after it the count is exactly what is open, at every
// check, and 0 at the end.
//
//   A  baseline streams opened on this thread stay in the list, in order, while the others race: after the race the list holds
//      exactly the baseline streams (a lost node or a resurrected one changes the count)
//   B  every stream the threads opened was closed: closing the baseline leaves the list empty
//   C  the last paTerminateShared, which closes whatever is left, does not fault
//
// A machine with no output device (a CI runner) cannot run this: it says so and counts a skip - never a silent pass.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <portaudio.h>

#include "sink/pa_init.hpp"
#include "test_check.hpp"

extern "C" int PaFoxsdr_OpenStreamCount(void);
extern "C" void PaFoxsdr_OpenStreamListLockForTest(void);
extern "C" void PaFoxsdr_OpenStreamListUnlockForTest(void);

namespace {

constexpr int kThreads = 12;
constexpr int kRounds = 40;     // per thread
constexpr int kPerRound = 3;    // streams held open at once, closed in a different order than opened
constexpr int kBaseline = 3;

PaStream* openOne(const PaStreamParameters& out, double rate) {
    PaStream* s = nullptr;
    // Blocking mode (no callback): nothing runs on a driver thread, so the test is about the list alone.
    const PaError e = Pa_OpenStream(&s, nullptr, &out, rate, paFramesPerBufferUnspecified, paNoFlag, nullptr, nullptr);
    return e == paNoError ? s : nullptr;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_pa_open_list_race\n");
    if (!cascade::sink::paInitializeShared()) {
        std::printf("SKIP: Pa_Initialize failed here\n");
        ++g_checksSkipped;
        return testSummary("test_pa_open_list_race");
    }
    const PaDeviceIndex dev = Pa_GetDefaultOutputDevice();
    const PaDeviceInfo* info = dev == paNoDevice ? nullptr : Pa_GetDeviceInfo(dev);
    if (info == nullptr || info->maxOutputChannels < 1) {
        std::printf("SKIP: no default output device on this machine - the list race cannot be run here\n");
        ++g_checksSkipped;
        cascade::sink::paTerminateShared();
        return testSummary("test_pa_open_list_race");
    }
    PaStreamParameters out{};
    out.device = dev;
    out.channelCount = 1;
    out.sampleFormat = paFloat32;
    out.suggestedLatency = info->defaultLowOutputLatency > 0.0 ? info->defaultLowOutputLatency : 0.05;
    const double rate = info->defaultSampleRate > 0.0 ? info->defaultSampleRate : 48000.0;

    std::vector<PaStream*> baseline;
    for (int i = 0; i < kBaseline; ++i) {
        if (PaStream* s = openOne(out, rate)) baseline.push_back(s);
    }
    std::printf("device \"%s\", %.0f Hz; baseline streams open: %zu; %d threads x %d rounds x %d streams, no guard\n",
                info->name != nullptr ? info->name : "?", rate, baseline.size(), kThreads, kRounds, kPerRound);
    if (baseline.empty()) {
        std::printf("SKIP: the default output device refused every open here (in use?) - the list race cannot be run\n");
        ++g_checksSkipped;
        cascade::sink::paTerminateShared();
        return testSummary("test_pa_open_list_race");
    }
    CHECK(PaFoxsdr_OpenStreamCount() == static_cast<int>(baseline.size()));

    // L: the lock held by hand; an open and a close on other threads must wait for it.
    {
        std::atomic<bool> openDone{false}, closeDone{false};
        std::atomic<PaStream*> opened1{nullptr};
        PaStream* toClose = openOne(out, rate);
        CHECK(toClose != nullptr);
        const int before = PaFoxsdr_OpenStreamCount();
        PaFoxsdr_OpenStreamListLockForTest();
        std::thread opener([&] { opened1.store(openOne(out, rate)); openDone.store(true); });
        std::thread closer([&] { if (toClose != nullptr) Pa_CloseStream(toClose); closeDone.store(true); });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const bool openWaited = !openDone.load();
        const bool closeWaited = !closeDone.load();
        PaFoxsdr_OpenStreamListUnlockForTest();
        opener.join();
        closer.join();
        std::printf("L: with the list lock held 500 ms, the open waited: %d, the close waited: %d\n", openWaited ? 1 : 0, closeWaited ? 1 : 0);
        CHECK(openWaited);
        CHECK(closeWaited);
        CHECK(opened1.load() != nullptr);
        // One stream was added and one removed: the count is where it began (read after the release, so under the lock).
        CHECK(PaFoxsdr_OpenStreamCount() == before);
        if (opened1.load() != nullptr) Pa_CloseStream(opened1.load());
        CHECK(PaFoxsdr_OpenStreamCount() == before - 1);
        // `before` counted the baseline plus toClose: the baseline is still all there.
        CHECK(PaFoxsdr_OpenStreamCount() == static_cast<int>(baseline.size()));
    }

    std::atomic<bool> go{false};
    std::atomic<int> opened{0}, refused{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int r = 0; r < kRounds; ++r) {
                PaStream* held[kPerRound] = {};
                for (int k = 0; k < kPerRound; ++k) {
                    held[k] = openOne(out, rate);
                    if (held[k] != nullptr) ++opened; else ++refused;
                }
                // Close in a rotating order so the removals hit the head, the middle and the tail of the list.
                for (int k = 0; k < kPerRound; ++k) {
                    PaStream*& s = held[(k + t + r) % kPerRound];
                    if (s != nullptr) { Pa_CloseStream(s); s = nullptr; }
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();

    std::printf("opened %d, refused by the device %d\n", opened.load(), refused.load());
    CHECK(opened.load() > kThreads * 4);  // the race really ran: most opens were granted
    // A: only the baseline is left in the list.
    const int left = PaFoxsdr_OpenStreamCount();
    std::printf("list holds %d after the race (expected %zu)\n", left, baseline.size());
    CHECK(left == static_cast<int>(baseline.size()));
    // B: closing the baseline empties it.
    for (PaStream* s : baseline) Pa_CloseStream(s);
    const int after = PaFoxsdr_OpenStreamCount();
    std::printf("list holds %d after closing the baseline (expected 0)\n", after);
    CHECK(after == 0);
    // C: the last terminate, which closes whatever the list still holds, must not fault.
    cascade::sink::paTerminateShared();
    std::printf("terminated\n");
    return testSummary("test_pa_open_list_race");
}
