// Tests for sink/audio_out_aaudio.cpp — the AAudio (Android) backend of
// sink/audio_out.hpp's AudioOut.
//
// WHAT RUNS HERE, AND WHY THIS FILE IS LINKED THE WAY IT IS. This desk has no
// Android device or emulator (no KVM under WSL), so nothing that calls a real
// AAudio entry point (AudioOut::open/close/streamAlive/listOutputDevices,
// compiled only under __ANDROID__ — see audio_out_aaudio.cpp) can be
// exercised here. What CAN be, and is: the ring/priming/underrun core those
// methods share with the PortAudio backend (AudioOut::write, writeStereo,
// setVolume, pullBlock, ringFrames, ringCapacityFrames, openedDeviceName),
// which is defined UNCONDITIONALLY in audio_out_aaudio.cpp precisely so it
// compiles here with no AAudio header on the include path at all; the error
// callback's own logic (AudioOut::androidErrorCallback), likewise
// unconditional; and androidStreamStateIsAlive() (audio_out_aaudio.hpp),
// which is the whole of the decision streamAlive() makes from a real
// AAudioStream_getState() result, expressed as a function of a plain int32_t
// so it needs no real stream — a "fake stream" in the same spirit as
// test_audio_out.cpp's headless pullBlock tests need no real PaStream.
//
// This is why tests/CMakeLists.txt gives this ONE test a bespoke link line
// (this file plus src/sink/audio_out_aaudio.cpp directly, no cascade_lib):
// cascade_lib already carries audio_out.cpp's PortAudio-backed
// cascade::sink::AudioOut, and linking both backends' definitions of the
// same class into one binary is a straight ODR violation.
//
// NOT COVERED HERE: pullBlock's STEREO interleaving (channels() can only
// become 2 through a successful open(), which does not exist in this
// translation unit without __ANDROID__) and the two AAudio callback
// TRAMPOLINES themselves (trampolineData/trampolineError — two-line casts
// into pullBlock/androidErrorCallback, which this file tests directly, plus
// a numFrames<=0 guard AAudio's own contract says should never fire). Both
// are provable by inspection against audio_out.cpp's already-tested,
// textually identical mono/stereo pullBlock logic and against
// tests/test_audio_out.cpp's own stereo coverage of it, respectively — see
// audio_out_aaudio.cpp's file header for the full accounting of what is and
// is not exercised, on device or off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/audio_out.hpp"
#include "sink/audio_out_aaudio.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include "test_check.hpp"

using cascade::sink::AudioOut;

namespace {

// Fixed-seed LCG (Numerical Recipes constants), matching test_audio_out.cpp
// exactly so the two suites are visibly testing the same arithmetic against
// two different linked copies of it.
std::uint32_t g_lcg = 0x13572468u;
float nextSample() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return static_cast<float>(g_lcg >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// Cold start -> primed, empty ring, mirroring test_audio_out.cpp's
// primeAndDrain(). Mono only: channels() defaults to 1 and nothing in this
// translation unit (open() is __ANDROID__-only) can change it.
void primeAndDrain(AudioOut& ao) {
    std::vector<float> pad(AudioOut::kPrimeFrames, 0.0f);
    CHECK(ao.write(pad.data(), pad.size()) == pad.size());
    std::vector<float> junk(pad.size());
    CHECK(AudioOut::pullBlock(&ao, junk.data(), AudioOut::kPrimeFrames) == pad.size());
}

}  // namespace

int main() {
    // --- fresh-object invariants ---------------------------------------------
    {
        AudioOut ao;
        CHECK(!ao.everOpened());
        CHECK(ao.channels() == 1);
        CHECK(ao.ringFrames() == 0u);
        CHECK(ao.ringCapacityFrames() == (std::size_t{1} << 15));  // mono: 32768
        CHECK(ao.openedDeviceName().empty());
        CHECK(!ao.androidStreamErrorFlagged());
        // Destructor (close() -> the host-mode closeLocked() stub) must not
        // crash on an object that was never opened.
    }

    // --- pullBlock: exact volume scaling and FIFO order, headless -----------
    {
        AudioOut ao;
        primeAndDrain(ao);
        float src[64];
        for (float& s : src) { s = nextSample(); }
        CHECK(ao.write(src, 64) == 64u);

        float dst[32];
        ao.setVolume(0.5f);
        CHECK(AudioOut::pullBlock(&ao, dst, 32) == 32u);
        for (int i = 0; i < 32; ++i) { CHECK(dst[i] == src[i] * 0.5f); }

        ao.setVolume(0.3f);
        CHECK(AudioOut::pullBlock(&ao, dst, 32) == 32u);
        for (int i = 0; i < 32; ++i) { CHECK(dst[i] == src[32 + i] * 0.3f); }

        CHECK(ao.underruns() == 0u);
    }

    // --- setVolume clamps to [0, 1] (and NaN mutes rather than poisons) -----
    {
        AudioOut ao;
        primeAndDrain(ao);
        const float s = 0.625f;
        float d = 0.0f;

        ao.setVolume(2.0f);
        CHECK(ao.write(&s, 1) == 1u);
        CHECK(AudioOut::pullBlock(&ao, &d, 1) == 1u);
        CHECK(d == s);

        ao.setVolume(-3.0f);
        CHECK(ao.write(&s, 1) == 1u);
        CHECK(AudioOut::pullBlock(&ao, &d, 1) == 1u);
        CHECK(d == 0.0f);

        ao.setVolume(std::nanf(""));
        CHECK(ao.write(&s, 1) == 1u);
        CHECK(AudioOut::pullBlock(&ao, &d, 1) == 1u);
        CHECK(d == 0.0f);
    }

    // --- pullBlock: a starvation counts ONE event and unprimes --------------
    {
        AudioOut ao;
        primeAndDrain(ao);
        ao.setVolume(1.0f);
        float dst[64];
        for (float& d : dst) { d = 123.0f; }

        CHECK(AudioOut::pullBlock(&ao, dst, 64) == 0u);
        for (int i = 0; i < 64; ++i) { CHECK(dst[i] == 0.0f); }
        CHECK(ao.underruns() == 1u);

        CHECK(AudioOut::pullBlock(&ao, dst, 64) == 0u);
        CHECK(AudioOut::pullBlock(&ao, dst, 64) == 0u);
        CHECK(ao.underruns() == 1u);
        CHECK(ao.primingCallbacks() >= 2u);

        CHECK(AudioOut::pullBlock(&ao, dst, 0) == 0u);
        CHECK(ao.underruns() == 1u);
    }

    // --- pullBlock: partial fill boundary, then refilling past the prime ----
    {
        AudioOut ao;
        primeAndDrain(ao);
        ao.setVolume(0.5f);
        float src[10];
        for (float& s : src) { s = nextSample(); }
        CHECK(ao.write(src, 10) == 10u);

        float dst[16];
        for (float& d : dst) { d = 123.0f; }
        CHECK(AudioOut::pullBlock(&ao, dst, 16) == 10u);
        for (int i = 0; i < 10; ++i) { CHECK(dst[i] == src[i] * 0.5f); }
        for (int i = 10; i < 16; ++i) { CHECK(dst[i] == 0.0f); }
        CHECK(ao.underruns() == 1u);

        CHECK(ao.write(src, 8) == 8u);
        CHECK(AudioOut::pullBlock(&ao, dst, 8) == 0u);
        CHECK(ao.underruns() == 1u);

        std::vector<float> pad(AudioOut::kPrimeFrames - 8, 0.0f);
        CHECK(ao.write(pad.data(), pad.size()) == pad.size());
        CHECK(AudioOut::pullBlock(&ao, dst, 8) == 8u);
        for (int i = 0; i < 8; ++i) { CHECK(dst[i] == src[i] * 0.5f); }
        CHECK(ao.underruns() == 1u);
    }

    // --- priming: the full sequence, exact numbers ---------------------------
    {
        AudioOut ao;
        ao.setVolume(1.0f);
        std::vector<float> chunk(1000);
        for (float& s : chunk) { s = nextSample(); }
        CHECK(ao.write(chunk.data(), chunk.size()) == chunk.size());
        CHECK(ao.ringFrames() == 1000u);

        float dst[480];
        for (float& d : dst) { d = 123.0f; }
        const std::uint64_t primingBefore = ao.primingCallbacks();
        CHECK(AudioOut::pullBlock(&ao, dst, 480) == 0u);
        for (float d : dst) { CHECK(d == 0.0f); }
        CHECK(ao.underruns() == 0u);
        CHECK(ao.primingCallbacks() == primingBefore + 1);
        CHECK(ao.ringFrames() == 1000u);

        std::vector<float> more(5000);
        for (float& s : more) { s = nextSample(); }
        CHECK(ao.write(more.data(), more.size()) == more.size());
        CHECK(ao.ringFrames() == 6000u);

        float playDst[200];
        CHECK(AudioOut::pullBlock(&ao, playDst, 200) == 200u);
        for (int i = 0; i < 200; ++i) {
            CHECK(playDst[i] == chunk[static_cast<std::size_t>(i)]);
        }
        CHECK(ao.underruns() == 0u);
        CHECK(ao.ringFrames() == 6000u - 200u);

        std::size_t remaining = 6000 - 200;
        float drainDst[997];
        bool starved = false;
        for (int iter = 0; iter < 100 && !starved; ++iter) {
            const std::size_t got = AudioOut::pullBlock(&ao, drainDst, 997);
            if (got < 997u) { starved = true; }
            remaining -= got;
        }
        CHECK(starved);
        CHECK(remaining == 0u);
        CHECK(ao.underruns() == 1u);
        CHECK(ao.ringFrames() == 0u);
    }

    // --- write() is bounded (non-blocking) and loses nothing it accepted -----
    {
        AudioOut ao;
        ao.setVolume(1.0f);
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
        CHECK(sawPartial);
        CHECK(totalWritten > 0u);
        CHECK(totalWritten < (std::size_t{1} << 24));

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
        CHECK(totalRead == totalWritten);
    }

    // --- writeStereo: whole-frame rounding, with no open() at all -----------
    // writeStereo() never consults channels() (see audio_out_aaudio.cpp) —
    // it only rounds the FRAME request down to the ring's whole-frame free
    // space — so this is fully testable without a real stereo open().
    {
        AudioOut ao;
        std::vector<float> block(4096, 0.25f);
        std::size_t framesIn = 0;
        for (int iter = 0; iter < 100000; ++iter) {
            const std::size_t took = ao.writeStereo(block.data(), 2048);
            framesIn += took;
            if (took < 2048u) { break; }
        }
        CHECK(framesIn > 0u);
        // Every accepted frame must be whole: draining in raw samples must
        // see an even count and pull every accepted sample back exactly
        // once (channels() is still 1 here, so pullBlock reads it as a flat
        // sample stream — the point of this test is writeStereo's own
        // frame-rounding, not pullBlock's stereo interleaving).
        ao.setVolume(1.0f);
        std::vector<float> out(4096, 0.0f);
        std::size_t samplesOut = 0;
        for (int iter = 0; iter < 100000; ++iter) {
            const std::size_t got = AudioOut::pullBlock(&ao, out.data(), 2048);
            if (got == 0u) { break; }
            samplesOut += got;
        }
        CHECK(samplesOut == framesIn * 2u);  // whole frames in, all samples out
    }

    // --- androidErrorCallback: the ONE thing AAudio's error callback may do -
    {
        AudioOut ao;
        CHECK(!ao.androidStreamErrorFlagged());
        AudioOut::androidErrorCallback(&ao);
        CHECK(ao.androidStreamErrorFlagged());
        // Idempotent: a second error (or a stream that keeps erroring before
        // anyone reopens it) must not do anything more exotic than stay set.
        AudioOut::androidErrorCallback(&ao);
        CHECK(ao.androidStreamErrorFlagged());
        // A null self must not crash — trampolineError never passes one (the
        // userData is always `this` from a successful open()), but the
        // static method takes void* and nothing stops a future caller from
        // getting it wrong.
        AudioOut::androidErrorCallback(nullptr);

        // Two independent objects must not share the flag — it is a member,
        // not a global, so a second AudioOut is unaffected by the first's
        // error.
        AudioOut other;
        CHECK(!other.androidStreamErrorFlagged());
    }

    // --- androidStreamStateIsAlive: every AAudio stream state, by number ----
    // The literal values are AAudio.h's own aaudio_stream_state_t constants
    // (verified against $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/
    // linux-x86_64/sysroot/usr/include/aaudio/AAudio.h): UNINITIALIZED=0,
    // UNKNOWN=1, OPEN=2, STARTING=3, STARTED=4, PAUSING=5, PAUSED=6,
    // FLUSHING=7, FLUSHED=8, STOPPING=9, STOPPED=10, CLOSING=11, CLOSED=12,
    // DISCONNECTED=13 — a plain sequential enum with one explicit "= 0".
    // This is the "fake stream": no real AAudioStream is needed to drive the
    // exact decision AudioOut::streamAlive() makes from a real
    // AAudioStream_getState() result, because that decision is this pure
    // function of the raw number.
    {
        using cascade::sink::androidStreamStateIsAlive;
        CHECK(!androidStreamStateIsAlive(0));   // UNINITIALIZED
        CHECK(!androidStreamStateIsAlive(1));   // UNKNOWN
        CHECK(!androidStreamStateIsAlive(2));   // OPEN
        CHECK(androidStreamStateIsAlive(3));    // STARTING - alive
        CHECK(androidStreamStateIsAlive(4));    // STARTED  - alive
        CHECK(!androidStreamStateIsAlive(5));   // PAUSING
        CHECK(!androidStreamStateIsAlive(6));   // PAUSED
        CHECK(!androidStreamStateIsAlive(7));   // FLUSHING
        CHECK(!androidStreamStateIsAlive(8));   // FLUSHED
        CHECK(!androidStreamStateIsAlive(9));   // STOPPING
        CHECK(!androidStreamStateIsAlive(10));  // STOPPED
        CHECK(!androidStreamStateIsAlive(11));  // CLOSING
        CHECK(!androidStreamStateIsAlive(12));  // CLOSED
        CHECK(!androidStreamStateIsAlive(13));  // DISCONNECTED - the case this exists for
        // Out-of-range values (should never happen, but streamAlive() calls
        // this with whatever getState() hands back) must read as dead, not
        // crash or accidentally match.
        CHECK(!androidStreamStateIsAlive(-1));
        CHECK(!androidStreamStateIsAlive(999));
    }

    return testSummary("test_audio_out_aaudio");
}
