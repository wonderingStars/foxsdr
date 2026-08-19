// Tests for sink/audio_out.hpp.
//
// Two layers, matching the module's design:
//   1. pullBlock() headless — the exact code the PortAudio callback runs
//      (ring pull, volume scaling, starvation zero-fill + underrun count) is
//      exercised without any device, against expectations computed in-test.
//   2. The real machine surface — device enumeration and an actual
//      open/write/close round trip on the system default output device.
//      This machine has audio, so failures are asserted loudly (with the
//      PortAudio error text), never silently skipped.
//
// Volume checks use exact float equality on purpose: the test computes
// sample * volume with the same single multiplication the implementation
// performs, so the results must be bit-identical — a tolerance would only
// mask a wrong-operation bug.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/audio_out.hpp"

#include <portaudio.h>  // failure diagnostics + cross-checking isDefault

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "test_check.hpp"

using cascade::sink::AudioDevice;
using cascade::sink::AudioOut;

namespace {

// Fixed-seed LCG (Numerical Recipes constants) — deterministic sample data
// in [-1, 1), no <random>.
std::uint32_t g_lcg = 0x13572468u;
float nextSample() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return static_cast<float>(g_lcg >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// On an open(-1, ...) failure, re-derive the reason straight from PortAudio
// so the ctest log shows WHY (no default device? init failure? open error?).
void printOpenFailureDiagnostics(double sampleRateHz) {
    const PaError initErr = Pa_Initialize();
    if (initErr != paNoError) {
        std::printf("diag: Pa_Initialize failed: %s\n", Pa_GetErrorText(initErr));
        return;
    }
    const PaDeviceIndex def = Pa_GetDefaultOutputDevice();
    if (def == paNoDevice) {
        std::printf("diag: Pa_GetDefaultOutputDevice() == paNoDevice "
                    "(no default output device on this machine)\n");
    } else {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(def);
        PaStreamParameters out{};
        out.device = def;
        out.channelCount = 1;
        out.sampleFormat = paFloat32;
        out.suggestedLatency =
            info != nullptr ? info->defaultLowOutputLatency : 0.0;
        PaStream* s = nullptr;
        const PaError err = Pa_OpenStream(&s, nullptr, &out, sampleRateHz, 0,
                                          paNoFlag, nullptr, nullptr);
        if (err == paNoError) {
            Pa_CloseStream(s);
            std::printf("diag: default device %d (%s) opens blocking-mode fine "
                        "— failure is in AudioOut::open itself\n",
                        static_cast<int>(def),
                        info != nullptr && info->name != nullptr ? info->name : "?");
        } else {
            std::printf("diag: Pa_OpenStream on default device %d failed: %s\n",
                        static_cast<int>(def), Pa_GetErrorText(err));
        }
    }
    Pa_Terminate();
}

}  // namespace

int main() {
    // --- pullBlock: exact volume scaling and FIFO order, headless -----------
    {
        AudioOut ao;
        float src[64];
        for (float& s : src) { s = nextSample(); }
        CHECK(ao.write(src, 64) == 64u);

        // Two pulls at different volumes: the volume must be read per call
        // (a slider move mid-stream affects the very next callback), and the
        // ring must hand back the same order it was fed.
        float dst[32];
        ao.setVolume(0.5f);
        CHECK(AudioOut::pullBlock(&ao, dst, 32) == 32u);
        for (int i = 0; i < 32; ++i) { CHECK(dst[i] == src[i] * 0.5f); }

        ao.setVolume(0.3f);  // non-dyadic: still bit-exact, same single multiply
        CHECK(AudioOut::pullBlock(&ao, dst, 32) == 32u);
        for (int i = 0; i < 32; ++i) { CHECK(dst[i] == src[32 + i] * 0.3f); }

        CHECK(ao.underruns() == 0u);  // both pulls were fully served
    }

    // --- pullBlock: unity and zero volume are exact end points --------------
    {
        AudioOut ao;
        float src[16];
        for (float& s : src) { s = nextSample(); }

        CHECK(ao.write(src, 16) == 16u);
        ao.setVolume(1.0f);
        float dst[16];
        CHECK(AudioOut::pullBlock(&ao, dst, 16) == 16u);
        for (int i = 0; i < 16; ++i) { CHECK(dst[i] == src[i]); }  // pass-through

        CHECK(ao.write(src, 16) == 16u);
        ao.setVolume(0.0f);
        CHECK(AudioOut::pullBlock(&ao, dst, 16) == 16u);
        for (int i = 0; i < 16; ++i) { CHECK(dst[i] == 0.0f); }  // muted
    }

    // --- setVolume clamps to [0, 1] (and NaN mutes rather than poisons) -----
    {
        AudioOut ao;
        const float s = 0.625f;  // dyadic: survives any [0,1] multiply exactly
        float d = 0.0f;

        ao.setVolume(2.0f);  // above range → clamps to unity
        CHECK(ao.write(&s, 1) == 1u);
        CHECK(AudioOut::pullBlock(&ao, &d, 1) == 1u);
        CHECK(d == s);

        ao.setVolume(-3.0f);  // below range → clamps to silence
        CHECK(ao.write(&s, 1) == 1u);
        CHECK(AudioOut::pullBlock(&ao, &d, 1) == 1u);
        CHECK(d == 0.0f);

        ao.setVolume(std::nanf(""));  // NaN → silence, not NaN output
        CHECK(ao.write(&s, 1) == 1u);
        CHECK(AudioOut::pullBlock(&ao, &d, 1) == 1u);
        CHECK(d == 0.0f);
    }

    // --- pullBlock: starvation zero-fills and counts one event --------------
    {
        AudioOut ao;
        ao.setVolume(1.0f);
        float dst[64];
        for (float& d : dst) { d = 123.0f; }  // sentinel = stale device buffer

        // Empty ring: nothing pulled, EVERY sample silenced, one underrun.
        CHECK(AudioOut::pullBlock(&ao, dst, 64) == 0u);
        for (int i = 0; i < 64; ++i) { CHECK(dst[i] == 0.0f); }
        CHECK(ao.underruns() == 1u);

        // Each starved callback counts exactly once (event count, not
        // missing-sample count).
        CHECK(AudioOut::pullBlock(&ao, dst, 64) == 0u);
        CHECK(AudioOut::pullBlock(&ao, dst, 64) == 0u);
        CHECK(ao.underruns() == 3u);

        // A zero-length request cannot starve: no bump.
        CHECK(AudioOut::pullBlock(&ao, dst, 0) == 0u);
        CHECK(ao.underruns() == 3u);
    }

    // --- pullBlock: partial fill boundary ------------------------------------
    {
        AudioOut ao;
        ao.setVolume(0.5f);
        float src[10];
        for (float& s : src) { s = nextSample(); }
        CHECK(ao.write(src, 10) == 10u);

        float dst[16];
        for (float& d : dst) { d = 123.0f; }
        // Ask for 16, ring holds 10: the 10 real samples arrive scaled, the
        // 6-sample tail is silence, and the event counts as ONE underrun.
        CHECK(AudioOut::pullBlock(&ao, dst, 16) == 10u);
        for (int i = 0; i < 10; ++i) { CHECK(dst[i] == src[i] * 0.5f); }
        for (int i = 10; i < 16; ++i) { CHECK(dst[i] == 0.0f); }
        CHECK(ao.underruns() == 1u);

        // Exact fill afterwards: no underrun.
        CHECK(ao.write(src, 8) == 8u);
        CHECK(AudioOut::pullBlock(&ao, dst, 8) == 8u);
        CHECK(ao.underruns() == 1u);
    }

    // --- write() is bounded (non-blocking) and loses nothing it accepted -----
    {
        AudioOut ao;
        ao.setVolume(1.0f);
        // Feed an integer staircase until the ring refuses a full chunk. All
        // values stay below 2^24 so their float representation is exact.
        std::vector<float> chunk(1024);
        std::size_t totalWritten = 0;
        bool sawPartial = false;
        for (int iter = 0; iter < 1000000 && !sawPartial; ++iter) {
            for (std::size_t i = 0; i < chunk.size(); ++i) {
                chunk[i] = static_cast<float>(totalWritten + i);
            }
            const std::size_t acc = ao.write(chunk.data(), chunk.size());
            totalWritten += acc;
            if (acc < chunk.size()) { sawPartial = true; }
        }
        CHECK(sawPartial);        // a ring, not an unbounded queue
        CHECK(totalWritten > 0u);
        CHECK(totalWritten < (std::size_t{1} << 24));  // float-exact staircase

        // Drain in a co-prime chunk size: every accepted sample must come
        // back exactly once, in order.
        float dst[997];
        std::size_t totalRead = 0;
        bool orderOk = true;
        for (int iter = 0; iter < 1000000; ++iter) {
            const std::size_t got = AudioOut::pullBlock(&ao, dst, 997);
            if (got == 0u) { break; }
            for (std::size_t i = 0; i < got; ++i) {
                if (dst[i] != static_cast<float>(totalRead + i)) { orderOk = false; }
            }
            totalRead += got;
        }
        CHECK(orderOk);
        CHECK(totalRead == totalWritten);  // conservation: no loss, no invention
    }

    // --- listOutputDevices: sane on this machine -----------------------------
    {
        AudioOut ao;
        const std::vector<AudioDevice> devices = ao.listOutputDevices();
        std::printf("output devices: %zu\n", devices.size());
        for (const AudioDevice& d : devices) {
            std::printf("  [%d] %s%s\n", d.index, d.name.c_str(),
                        d.isDefault ? "  (default)" : "");
        }
        CHECK(!devices.empty());  // this machine has audio hardware

        std::size_t namedCount = 0;
        std::size_t defaultCount = 0;
        for (const AudioDevice& d : devices) {
            CHECK(d.index >= 0);
            if (!d.name.empty()) { ++namedCount; }
            if (d.isDefault) { ++defaultCount; }
        }
        CHECK(namedCount >= 1u);
        // The default flag must mark exactly the device PortAudio reports.
        CHECK(defaultCount == 1u);
    }

    // --- Real default device: open → write 0.25 s → close --------------------
    {
        AudioOut ao;
        const bool ok = ao.open(-1, 48000.0);
        CHECK(ok);  // machine has audio: a failure here is a real failure
        if (!ok) {
            printOpenFailureDiagnostics(48000.0);
        } else {
            CHECK(ao.running());

            const std::uint64_t before = ao.underruns();
            // 0.25 s of silence at 48 kHz; the ring (32k+) is empty, so the
            // whole block must be accepted in one call.
            std::vector<float> zeros(12000, 0.0f);
            CHECK(ao.write(zeros.data(), zeros.size()) == zeros.size());

            // Proof the REAL callback is pulling: once the device drains the
            // ring it starves and the underrun counter must advance past the
            // pre-write snapshot. Single-threaded poll, deadline-bounded at
            // 15 s (well under the 120 s ctest cap); normal machines take
            // ~0.3 s. No second thread exists, so no abort flag is needed.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(15);
            bool callbackRan = false;
            while (std::chrono::steady_clock::now() < deadline) {
                if (ao.underruns() > before) {
                    callbackRan = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK(callbackRan);

            ao.close();
            CHECK(!ao.running());
            ao.close();  // double close is safe
            CHECK(!ao.running());
        }
    }

    // --- close-without-open, and open() failure paths ------------------------
    {
        AudioOut ao;
        ao.close();  // never opened: must be a no-op, not a crash
        CHECK(!ao.running());
        CHECK(!ao.open(1 << 20, 48000.0));  // absurd device index refused
        CHECK(!ao.running());
        CHECK(!ao.open(-1, -48000.0));      // nonsense sample rate refused
        CHECK(!ao.running());
        CHECK(ao.channels() == 1);          // default layout until an open wins
        CHECK(!ao.open(-1, 48000.0, 3));    // only 1 and 2 are layouts
        CHECK(!ao.open(-1, 48000.0, 0));
        CHECK(ao.channels() == 1);          // a refused open changes nothing
    }

    // --- Stereo: real two-channel open, then the callback core headless -----
    // The device is opened for real (this machine has audio) and CLOSED
    // again before pullBlock is exercised: channels() survives the close by
    // contract, so the callback path can be measured deterministically with
    // no PortAudio thread racing the ring for samples.
    {
        AudioOut ao;
        const bool ok = ao.open(-1, 48000.0, 2);
        CHECK(ok);
        if (!ok) {
            printOpenFailureDiagnostics(48000.0);
        } else {
            CHECK(ao.running());
            CHECK(ao.channels() == 2);
            ao.close();
            CHECK(!ao.running());
            CHECK(ao.channels() == 2);  // layout of the ring, not of the stream

            // Baseline, because the open above was REAL: between open and
            // close the device callback ran against a still-empty ring and
            // legitimately counted starvation. Windows happened not to fire a
            // callback in that window and Linux does, which made an absolute
            // count a property of the audio backend rather than of the code
            // under test. What this block actually asserts is pullBlock's own
            // starvation accounting, so it measures the DELTA from here.
            const std::uint64_t base = ao.underruns();

            // writeStereo takes FRAMES and pullBlock takes FRAMES; the
            // samples in between are interleaved L,R and must come back in
            // exactly that order with one volume applied to both channels.
            float frames[64];  // 32 frames
            for (int i = 0; i < 32; ++i) {
                frames[2 * i] = nextSample();
                frames[2 * i + 1] = nextSample();
            }
            CHECK(ao.writeStereo(frames, 32) == 32u);

            ao.setVolume(0.5f);
            float dst[64];
            for (float& d : dst) { d = 123.0f; }
            // 16 frames requested -> 32 samples pulled.
            CHECK(AudioOut::pullBlock(&ao, dst, 16) == 32u);
            for (int i = 0; i < 32; ++i) { CHECK(dst[i] == frames[i] * 0.5f); }
            CHECK(ao.underruns() - base == 0u);

            // Starvation covers BOTH channels of every unfilled frame and
            // still counts one event: 16 frames left in the ring, 24 asked
            // for, so 32 samples arrive and the remaining 16 are silence.
            for (float& d : dst) { d = 123.0f; }
            CHECK(AudioOut::pullBlock(&ao, dst, 24) == 32u);
            for (int i = 0; i < 32; ++i) { CHECK(dst[i] == frames[32 + i] * 0.5f); }
            for (int i = 32; i < 48; ++i) { CHECK(dst[i] == 0.0f); }
            CHECK(ao.underruns() - base == 1u);

            // The channel pair is never split: with an odd number of samples
            // of room left, writeStereo must refuse the straddling frame
            // rather than shift every later sample by one (which would swap
            // L and R for the rest of the session).
            AudioOut fill;
            CHECK(fill.open(-1, 48000.0, 2));
            fill.close();
            std::vector<float> block(4096, 0.25f);
            std::size_t framesIn = 0;
            for (int iter = 0; iter < 100000; ++iter) {
                const std::size_t took = fill.writeStereo(block.data(), 2048);
                framesIn += took;
                if (took < 2048u) { break; }
            }
            CHECK(framesIn > 0u);
            // Every accepted frame is whole, so the drain sees an even sample
            // count and each frame reads back as the pair it was written as.
            std::vector<float> out(4096, 0.0f);
            std::size_t framesOut = 0;
            bool pairsOk = true;
            fill.setVolume(1.0f);
            for (int iter = 0; iter < 100000; ++iter) {
                const std::size_t got = AudioOut::pullBlock(&fill, out.data(), 2048);
                if (got == 0u) { break; }
                if (got % 2u != 0u) { pairsOk = false; }
                framesOut += got / 2u;
            }
            CHECK(pairsOk);
            CHECK(framesOut == framesIn);  // conservation, in whole frames
        }
    }

    return testSummary("test_audio_out");
}
