// THE PIPELINE STEERS TO THE SINK'S OWN TARGET (0.99.73).
//
// tests/test_drift_matcher.cpp proves the matcher follows the target it is HANDED,
// and tests/test_audio_out.cpp that the sink's lead and target are runtime values.
// What neither can show is the join between them: that Pipeline::processAudioBlock
// hands the matcher the sink's AudioOut::targetFrames() on every block, and not the
// old fixed 160 ms. A pipeline that forgot would pass both of those and bleed every
// deepened buffer back down to 160 ms within a minute or two - the whole feature
// undone with nothing red.
//
// THE MEASUREMENT. A real pipeline on the built-in generator plays to this
// machine's real output at the default 120 ms lead. Once the sink is playing, its
// lead is raised to 960 ms WHILE IT PLAYS (the same call the window's once-a-minute
// poll makes), which moves the target to 1000 ms while the ring holds about 160 ms.
// The matcher's correction is then pinned at its +5000 ppm cap within a second or two
// (a lead 840 ms short, 50 ppm per ms, capped). Against the old fixed target the
// ring is exactly where the matcher wants it and the correction stays near zero - a
// difference of thousands of ppm, not a timing margin.
//
// An output that never plays is a SKIP, printed, never a pass. GitHub's ALSA
// `null` device can even prime briefly, then stop draining: both Linux jobs
// saw +3/+4 ppm instead of +5000 ppm. The workflow names that fixture
// explicitly, so this real-speaker integration test skips on that fixture.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

#include "sink/audio_out.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

int main() {
    const char* nullAudio = std::getenv("FOXSDR_CI_NULL_AUDIO");
    if (nullAudio != nullptr && std::string_view(nullAudio) == "1") {
        std::printf("SKIP the pipeline's target: CI uses ALSA null, which cannot sustain playback\n");
        ++g_checksSkipped;
        return testSummary("test_pipeline_audio_lead");
    }

    Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.audioEnabled = true;
    Pipeline p(cfg);
    cascade::sink::AudioOut& out = p.audio();
    if (!out.everOpened()) {
        std::printf("SKIP the pipeline's target: no output device on this machine\n");
        ++g_checksSkipped;
        return testSummary("test_pipeline_audio_lead");
    }

    p.start();
    // PLAYING: the callback has primed (the default 120 ms of audio is queued).
    const auto t0 = steady_clock::now();
    while (!out.primed() && steady_clock::now() - t0 < seconds(20)) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    if (!out.primed()) {
        std::printf("SKIP the pipeline's target: the output opened and never primed in 20 s "
                    "(host api \"%s\", %llu priming callbacks)\n",
                    out.openedHostApi().c_str(),
                    static_cast<unsigned long long>(out.primingCallbacks()));
        ++g_checksSkipped;
        p.stop();
        return testSummary("test_pipeline_audio_lead");
    }

    // Playing at the default: the ring is still climbing from its 120 ms prime to the
    // 160 ms target, so the matcher asks for a push of at most ~2000 ppm (40 ms at 50
    // ppm per ms) plus the clock mismatch it has learned (capped at 1000 ppm). Printed,
    // not asserted: it is the baseline the next number is read against.
    std::this_thread::sleep_for(seconds(3));
    const double before = p.driftCorrectionPpm();
    std::printf("pipeline: correction at the default lead %+.0f ppm, ring %zu frames\n", before,
                out.ringFrames());

    // THE RAISE, while it plays: the target moves to 1000 ms and the ring holds ~160.
    // Against the real target the error is some 840 ms and the correction sits at its
    // +5000 ppm cap; against the old fixed 160 ms it could not exceed ~3000 ppm.
    out.setLeadFrames(cascade::sink::audioLeadFrames(960));
    CHECK(out.targetFrames() == 48000u);
    double after = 0.0;
    const auto t1 = steady_clock::now();
    while (steady_clock::now() - t1 < seconds(10)) {
        std::this_thread::sleep_for(milliseconds(100));
        after = p.driftCorrectionPpm();
        if (after > 4500.0) { break; }
    }
    std::printf("pipeline: correction after the lead was raised to 960 ms %+.0f ppm, ring %zu frames\n",
                after, out.ringFrames());
    CHECK(after > 4500.0);                       // steering UP to the new target
    CHECK(out.primed() || !out.running());       // the raise did not stop what was playing
    p.stop();

    return testSummary("test_pipeline_audio_lead");
}
