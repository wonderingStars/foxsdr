// The meter that replaced FRAME TIME on the tuning bar.
//
// The owner, 2026-09-21: "can you make frame time meter in to a volume meter
// instead". What is asserted here is every decision behind the needle - what
// it measures, how silence and mute read, where the scale sits, and the
// ballistics - because a meter that is wrong is worse than no meter: it is
// read as fact at a glance and never questioned.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/volume_meter.hpp"

#include "test_check.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

int main() {
    using namespace cascade::gui;

    // --- WHAT IT MEASURES: the peak of the block -------------------------
    {
        const std::vector<float> block{0.0f, 0.25f, -0.5f, 0.1f};
        CHECK(audioPeak(block.data(), block.size()) == 0.5f);  // sign does not matter
        const std::vector<float> quiet{0.0f, 0.0f, 0.0f};
        CHECK(audioPeak(quiet.data(), quiet.size()) == 0.0f);
        // Degenerate input answers zero rather than reading past the end.
        CHECK(audioPeak(nullptr, 4) == 0.0f);
        CHECK(audioPeak(block.data(), 0) == 0.0f);
        // A NaN IN THE AUDIO IS IGNORED, and the good samples still read.
        // (This check first asserted 0 - that one bad sample should blank the
        // meter - which is the wrong behaviour: a dropout in the stream must
        // not hide the programme either side of it. Every comparison here is
        // ordered so a NaN loses, and the result is the peak of the rest.)
        const std::vector<float> bad{0.3f, std::numeric_limits<float>::quiet_NaN()};
        CHECK(audioPeak(bad.data(), bad.size()) == 0.3f);
        const std::vector<float> allBad{std::numeric_limits<float>::quiet_NaN()};
        CHECK(audioPeak(allBad.data(), allBad.size()) == 0.0f);
    }

    // --- WHAT REACHES THE SPEAKERS ---------------------------------------
    //
    // The tap is taken BELOW the plugin audio and the hard mute but ABOVE the
    // volume control, so the meter has to apply the volume itself. A meter
    // that ignored the knob would sit half way up a silent set.
    {
        CHECK(audibleAmplitude(0.8f, 1.0f, false) == 0.8f);
        CHECK(audibleAmplitude(0.8f, 0.5f, false) == 0.4f);
        // MUTE READS AS SILENCE, whatever the signal is doing underneath.
        CHECK(audibleAmplitude(0.8f, 1.0f, true) == 0.0f);
        // Volume at zero is silence too - the same reading, for the same
        // reason: nothing is coming out.
        CHECK(audibleAmplitude(0.8f, 0.0f, false) == 0.0f);
        CHECK(audibleAmplitude(0.0f, 1.0f, false) == 0.0f);
    }

    // --- THE SCALE IS dB --------------------------------------------------
    //
    // Full scale pins, the floor sits on the stop, and half the arc is the
    // midpoint of the dB range rather than of the amplitude - which is the
    // whole reason for a dB scale: -30 dB is a perfectly ordinary listening
    // level and a linear needle would put it at 3% of the arc.
    {
        CHECK(meterFraction(1.0f) == 1.0f);
        CHECK(meterFraction(0.0f) == 0.0f);
        CHECK(meterFraction(2.0f) == 1.0f);   // above full scale pins
        CHECK(meterFraction(-1.0f) == 0.0f);  // nonsense reads as silence
        CHECK(meterFraction(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
        // -60 dB is the floor; -30 dB is half way; -6 dB is 0.9.
        CHECK(std::fabs(meterFraction(0.001f) - 0.0f) < 0.001f);
        CHECK(std::fabs(meterFraction(0.0316228f) - 0.5f) < 0.005f);
        CHECK(std::fabs(meterFraction(0.5011872f) - 0.9f) < 0.005f);
        // Below the floor is on the stop, not negative.
        CHECK(meterFraction(0.0001f) == 0.0f);
    }

    // --- THE BALLISTICS ---------------------------------------------------
    //
    // Instant rise so a transient is never under-read; exponential fall so a
    // syllable can be seen. A meter that followed the audio exactly would be
    // an unreadable blur.
    {
        CHECK(meterBallistics(0.2f, 0.9f, 1.0f / 60.0f) == 0.9f);  // rise is immediate
        const float fallen = meterBallistics(1.0f, 0.0f, kVolumeFallS);
        // One time constant: down to 1/e of the way, not to the target.
        CHECK(fallen > 0.3f && fallen < 0.4f);
        // Several time constants later it is effectively there.
        float v = 1.0f;
        for (int i = 0; i < 300; ++i) { v = meterBallistics(v, 0.0f, 1.0f / 60.0f); }
        CHECK(v < 0.01f);
        // A frame time that is not a frame time leaves the needle alone
        // rather than teleporting it: that happens on the first frame and
        // after a stall, and a jump would read as a signal that was not there.
        CHECK(meterBallistics(0.5f, 0.0f, 0.0f) == 0.5f);
        CHECK(meterBallistics(0.5f, 0.0f, -1.0f) == 0.5f);
        CHECK(meterBallistics(0.5f, 0.0f, 5.0f) == 0.5f);
    }

    // --- WHAT IS PRINTED UNDER THE FACE -----------------------------------
    {
        char buf[32];
        formatVolumeText(buf, sizeof(buf), 1.0f, true);
        CHECK(std::string(buf) == "0.0 dB");
        formatVolumeText(buf, sizeof(buf), 0.5011872f, true);
        CHECK(std::string(buf) == "-6.0 dB");
        // SILENCE PRINTS THE FLOOR, not "-inf" and not "0": the first is
        // arithmetic leaking onto the panel and the second reads as full
        // scale to anybody glancing at it.
        formatVolumeText(buf, sizeof(buf), 0.0f, true);
        CHECK(std::string(buf) == "-60 dB");
        formatVolumeText(buf, sizeof(buf), 0.0000001f, true);
        CHECK(std::string(buf) == "-60 dB");
        // No audio at all is not a level of any kind.
        formatVolumeText(buf, sizeof(buf), 0.5f, false);
        CHECK(std::string(buf) == "--");
        // Degenerate output buffers are refused rather than written past.
        formatVolumeText(nullptr, 8, 0.5f, true);
        formatVolumeText(buf, 0, 0.5f, true);
    }

    return testSummary("test_volume_meter");
}
