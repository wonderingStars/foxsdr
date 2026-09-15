// test_audio_in.cpp - the microphone's realtime path, with no microphone.
//
// THERE IS NO MICROPHONE ON THIS BENCH, and there does not need to be. The
// code the realtime audio thread runs is AudioIn::pushBlock, which is static
// and takes its object through a void* for exactly this reason: a test can
// run the callback's own body with a scripted block of audio, on one thread,
// and check every behaviour it has - the ring, the drop, the peak meter, the
// null buffer PortAudio is allowed to hand it - without opening a device.
//
// The parts that DO need a device (open, close, streamAlive) are exercised
// only as far as "they refuse gracefully when there is nothing there", which
// is the state a headless build machine is in and the state a user with no
// microphone is in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "sink/audio_in.hpp"
#include "test_check.hpp"

using cascade::sink::AudioIn;

int main() {
    // =====================================================================
    // 1. THE CALLBACK'S OWN BODY
    // =====================================================================
    {
        AudioIn in;
        CHECK(!in.running());
        CHECK(!in.everOpened());
        CHECK(!in.streamAlive());
        CHECK(in.available() == 0);
        CHECK(in.overruns() == 0);

        std::vector<float> block(480);
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = 0.25f * static_cast<float>(i % 7) - 0.5f;
        }
        CHECK(AudioIn::pushBlock(&in, block.data(), block.size()) == block.size());
        CHECK(in.available() == block.size());

        std::vector<float> got(block.size());
        CHECK(in.read(got.data(), got.size()) == got.size());
        CHECK(got == block);
        CHECK(in.available() == 0);
        // Reading an empty ring is a short read, not a wait: the TX thread
        // pads and carries on rather than stopping to notice the key.
        CHECK(in.read(got.data(), got.size()) == 0);
    }

    // =====================================================================
    // 2. A FULL RING DROPS, AND SAYS SO
    //
    //    Back-pressuring a realtime audio callback takes the whole input
    //    stream down; one overrun is a gap, and a stopped device is silence
    //    for ever.
    // =====================================================================
    {
        AudioIn in;
        std::vector<float> block(4096, 0.1f);
        std::size_t pushed = 0;
        int calls = 0;
        // Push far more than the ring can hold.
        for (int i = 0; i < 20; ++i) {
            pushed += AudioIn::pushBlock(&in, block.data(), block.size());
            ++calls;
        }
        std::printf("pushed %zu of %zu samples in %d callbacks, %llu overruns\n", pushed,
                    block.size() * static_cast<std::size_t>(calls), calls,
                    static_cast<unsigned long long>(in.overruns()));
        CHECK(pushed < block.size() * static_cast<std::size_t>(calls));
        CHECK(in.overruns() > 0);
        // It kept what it could rather than throwing the lot away.
        CHECK(in.available() > 0);

        // drain() empties it - which is what key-down does, so that what was
        // said BEFORE the PTT was pressed never reaches the air.
        in.drain();
        CHECK(in.available() == 0);
    }

    // =====================================================================
    // 3. THE PEAK METER
    //
    //    Read by asking, and CLEARED by asking, so the panel shows the peak
    //    of the interval it is drawing rather than the loudest thing since
    //    the device opened.
    // =====================================================================
    {
        AudioIn in;
        CHECK(in.takePeak() == 0.0f);

        std::vector<float> quiet(256, 0.05f);
        AudioIn::pushBlock(&in, quiet.data(), quiet.size());
        std::vector<float> loud(256, -0.80f);
        AudioIn::pushBlock(&in, loud.data(), loud.size());
        // The LOUDEST of the interval, and its magnitude - a meter that
        // reported the signed value would read zero on a symmetric waveform.
        CHECK_NEAR(in.takePeak(), 0.80f, 1e-6);
        // ...and asking cleared it.
        CHECK(in.takePeak() == 0.0f);

        // THE METER FOLLOWS THE MICROPHONE, NOT THE RING. The ring is filled
        // to bursting first, so nothing more can be accepted; the meter must
        // still move, because a meter that went quiet whenever the ring was
        // full would say the room had gone silent at exactly the moment the
        // transmitter was in trouble.
        std::vector<float> filler(4096, 0.01f);
        for (int i = 0; i < 20; ++i) { AudioIn::pushBlock(&in, filler.data(), filler.size()); }
        in.takePeak();
        std::vector<float> shout(256, 0.95f);
        const std::size_t took = AudioIn::pushBlock(&in, shout.data(), shout.size());
        CHECK(took == 0);  // nothing fitted
        CHECK_NEAR(in.takePeak(), 0.95f, 1e-6);
    }

    // =====================================================================
    // 4. WHAT PORTAUDIO IS ALLOWED TO HAND IT
    //
    //    A null input buffer (a period the device produced nothing for) is
    //    not an error and not an overrun: there is simply nothing to push.
    //    A NaN sample must not pin the meter, because a meter stuck at full
    //    scale is a meter nobody can read.
    // =====================================================================
    {
        AudioIn in;
        CHECK(AudioIn::pushBlock(&in, nullptr, 128) == 0);
        CHECK(in.overruns() == 0);
        CHECK(in.available() == 0);
        CHECK(AudioIn::pushBlock(&in, nullptr, 0) == 0);

        std::vector<float> nasty(64, 0.3f);
        nasty[10] = std::nanf("");
        AudioIn::pushBlock(&in, nasty.data(), nasty.size());
        const float peak = in.takePeak();
        std::printf("a block with a NaN in it read %.4f on the meter\n", peak);
        CHECK(std::isfinite(peak));
        CHECK_NEAR(peak, 0.3f, 1e-6);
    }

    // =====================================================================
    // 5. NO DEVICE IS NOT A CRASH
    //
    //    A headless build machine, and a user with no microphone, are the
    //    same state. Refusing is the answer; the panel says so.
    // =====================================================================
    {
        AudioIn in;
        const std::vector<cascade::sink::AudioDevice> devices = in.listInputDevices();
        std::printf("input devices seen here: %zu\n", devices.size());
        for (const cascade::sink::AudioDevice& d : devices) {
            std::printf("   [%d] %s%s\n", d.index, d.name.c_str(), d.isDefault ? " (default)" : "");
        }
        // An index that cannot exist is refused rather than opened.
        CHECK(!in.open(9999, 48000.0));
        CHECK(!in.running());
        // So is a rate that is not one.
        CHECK(!in.open(-1, 0.0));
        CHECK(!in.open(-1, -48000.0));
        // close() before any open is a no-op, twice.
        in.close();
        in.close();
        CHECK(!in.running());
        CHECK(!in.streamAlive());
    }

    return testSummary("test_audio_in");
}
