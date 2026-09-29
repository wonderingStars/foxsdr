// The arithmetic behind the four bench meter faces (GitHub/user request: "is
// it possible to customise the VU meter, choosing between 3 or 4 different
// VU meters?"). Every style reads the SAME gui::meterFraction /
// gui::meterBallistics figure - see gui/meter_styles_math.hpp's own header
// for why - so nothing here re-derives the reading; it only exercises the
// peak-hold catch, the LED ladder's segment/zone math, the style vocabulary,
// and (Peak style's numeric readout) the shared dB formatter already proven
// in tests/test_volume_meter.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/meter_styles_math.hpp"
#include "gui/volume_meter.hpp"

#include "test_check.hpp"

#include <cstring>
#include <limits>
#include <string>

int main() {
    using namespace cascade::gui;

    // --- THE STYLE VOCABULARY ----------------------------------------------
    {
        CHECK(meterStyleFromName("classic") == MeterStyle::Classic);
        CHECK(meterStyleFromName("needle") == MeterStyle::Needle);
        CHECK(meterStyleFromName("led") == MeterStyle::LedLadder);
        CHECK(meterStyleFromName("peak") == MeterStyle::Peak);
        // AN UNKNOWN STYLE INDEX/NAME FALLS BACK TO CLASSIC, never a refusal:
        // the config file is user-editable and a typo must leave the meter
        // looking like itself (the tunerDisplayStyle rule).
        for (const char* bad : {"", "bogus", "Needle", "NEEDLE ", "led "}) {
            CHECK(meterStyleFromName(bad) == MeterStyle::Classic);
        }
        // Round trip: every name the picker offers survives its own style.
        for (int k = 0; k < kMeterStyleCount; ++k) {
            const MeterStyle st = meterStyleFromName(kMeterStyleNames[k]);
            CHECK(std::string(meterStyleName(st)) == kMeterStyleNames[k]);
        }
        CHECK(std::string(meterStyleName(MeterStyle::Classic)) == "classic");
    }

    // --- PEAK HOLD: RISES INSTANTLY ------------------------------------------
    {
        MeterPeakHold h;
        h.update(0.4f, 1.0f / 60.0f);
        CHECK(h.peak == 0.4f);
        // A higher reading on the very next frame is caught immediately, not
        // ramped up to - the same "a transient is never under-read" rule
        // meterBallistics' rise half already follows.
        h.update(0.9f, 1.0f / 60.0f);
        CHECK(h.peak == 0.9f);
        // A lower reading does NOT pull the held peak down on the same frame:
        // the catch is what makes a peak meter useful at all.
        h.update(0.1f, 1.0f / 60.0f);
        CHECK(h.peak < 0.9f);       // it did start decaying...
        CHECK(h.peak > 0.1f);       // ...but is nowhere near the new reading yet
    }

    // --- PEAK HOLD: DECAYS AT THE STATED RATE --------------------------------
    {
        MeterPeakHold h;
        h.update(1.0f, 1.0f / 60.0f);
        CHECK(h.peak == 1.0f);
        // One whole second below any new peak: the peak must have fallen by
        // exactly kMeterPeakHoldFallPerS, not an exponential fraction of it -
        // a peak-hold catch is a LINEAR ramp, unlike the needle's own fall.
        float t = 0.0f;
        const float dt = 1.0f / 100.0f;
        for (int i = 0; i < 100; ++i) { h.update(0.0f, dt); t += dt; }
        CHECK(std::fabs(t - 1.0f) < 0.001f);
        CHECK(std::fabs(h.peak - (1.0f - kMeterPeakHoldFallPerS)) < 0.01f);
        // It never falls below the LIVE reading, however long it is given:
        // a peak of nothing is not a peak.
        for (int i = 0; i < 10000; ++i) { h.update(0.2f, 1.0f / 60.0f); }
        CHECK(h.peak == 0.2f);
        // It never falls below zero either, from a peak that started at zero.
        MeterPeakHold z;
        for (int i = 0; i < 10000; ++i) { z.update(0.0f, 1.0f / 60.0f); }
        CHECK(z.peak == 0.0f);
    }

    // --- PEAK HOLD: THE meterBallistics STALL RULE ---------------------------
    //
    // A frame time that is not a frame time (the first frame, a stall) leaves
    // the held peak exactly where it is rather than teleporting it downward -
    // the same guard meterBallistics carries for the needle.
    {
        MeterPeakHold h;
        h.update(0.7f, 1.0f / 60.0f);
        h.update(0.0f, 0.0f);
        CHECK(h.peak == 0.7f);
        h.update(0.0f, -1.0f);
        CHECK(h.peak == 0.7f);
        h.update(0.0f, 5.0f);
        CHECK(h.peak == 0.7f);
    }

    // --- THE LED LADDER: SEGMENT COUNT ---------------------------------------
    {
        CHECK(meterLedLitCount(0.0f) == 0);
        CHECK(meterLedLitCount(-1.0f) == 0);                          // nonsense reads as silence
        CHECK(meterLedLitCount(std::numeric_limits<float>::quiet_NaN()) == 0);
        CHECK(meterLedLitCount(1.0f) == kMeterLedSegments);
        CHECK(meterLedLitCount(2.0f) == kMeterLedSegments);            // above full scale pins
        // Rounded, not floored: sitting exactly on a boundary lights that
        // segment rather than leaving it dark by one ULP.
        CHECK(meterLedLitCount(0.5f) == kMeterLedSegments / 2);
        // Every count from 0 to kMeterLedSegments is reachable and monotonic.
        int prev = -1;
        for (int i = 0; i <= 100; ++i) {
            const int lit = meterLedLitCount(static_cast<float>(i) / 100.0f);
            CHECK(lit >= prev);
            CHECK(lit >= 0 && lit <= kMeterLedSegments);
            prev = lit;
        }
    }

    // --- THE LED LADDER: COLOUR ZONE PER SEGMENT -----------------------------
    {
        // The bottom of a twelve-segment ladder is green, the caution band is
        // amber and the top two segments - "the top of the travel should be
        // uncomfortable to sit on", drawBenchMeter's own rule for its tick
        // ladder - are red.
        CHECK(meterLedZone(0, kMeterLedSegments) == MeterLedZone::Green);
        CHECK(meterLedZone(6, kMeterLedSegments) == MeterLedZone::Green);
        CHECK(meterLedZone(7, kMeterLedSegments) == MeterLedZone::Amber);
        CHECK(meterLedZone(9, kMeterLedSegments) == MeterLedZone::Amber);
        CHECK(meterLedZone(10, kMeterLedSegments) == MeterLedZone::Red);
        CHECK(meterLedZone(11, kMeterLedSegments) == MeterLedZone::Red);
        // Every segment of a full ladder is classified into exactly one zone.
        for (int i = 0; i < kMeterLedSegments; ++i) {
            const MeterLedZone z = meterLedZone(i, kMeterLedSegments);
            CHECK(z == MeterLedZone::Green || z == MeterLedZone::Amber ||
                 z == MeterLedZone::Red);
        }
    }

    // --- THE PEAK METER'S NUMERIC READOUT ------------------------------------
    //
    // Peak style draws no needle of its own, so its dB figure is the shared
    // formatter every other style's value line already uses - proven fully in
    // tests/test_volume_meter.cpp, exercised here for the specific case the
    // style exists to show: silence prints the floor, not "-inf".
    {
        char buf[32];
        formatVolumeText(buf, sizeof(buf), 0.0f, true);
        CHECK(std::string(buf) == "-60 dB");
        formatVolumeText(buf, sizeof(buf), 1.0f, true);
        CHECK(std::string(buf) == "0.0 dB");
        formatVolumeText(buf, sizeof(buf), 0.5f, false);
        CHECK(std::string(buf) == "--");
    }

    return testSummary("test_meter_styles_math");
}
