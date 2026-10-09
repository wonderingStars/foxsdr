// THE POLICY THAT DEEPENS THE AUDIO BUFFER (0.99.73), as a table.
//
// sink::nextAudioLead(underrunsLastMinute, currentLeadMs, configuredMs) is pure:
// it says what the lead should be when a minute closes. Every transition is
// here, with the numbers written out, because the whole value of the feature is
// in the boundaries - the third starved callback and not the second, the ceiling
// that is never passed, the lead that never comes back down by itself, and the
// fixed setting that never moves. The application of the answer (the card, the
// log line, the config, the health count) is tests/test_audio_lead_app.cpp; the
// lead as the sink's callback honours it is tests/test_audio_out.cpp.
//
// THE FIELD REPORT (12CF, a Store user on 0.99.64, RSP1A at 2.048 MS/s, thirty
// plugins): 110 to 126 starved callbacks every minute at a 120 ms lead. The first
// row of the stepping sequence below is that report's minute.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/audio_out.hpp"

#include <climits>
#include <cstdint>
#include <cstdio>

#include "test_check.hpp"

using cascade::sink::AudioOut;
using cascade::sink::audioLeadFrames;
using cascade::sink::audioLeadMs;
using cascade::sink::kAudioLeadStarvedPerMinute;
using cascade::sink::kAudioLeadStepsMs;
using cascade::sink::nextAudioLead;
using cascade::sink::validAudioBufferSetting;

namespace {

struct Row {
    std::uint64_t starved;  // starved callbacks in the minute that closed
    int current;            // the lead in force, ms
    int configured;         // the setting: 0 automatic
    int want;               // the lead afterwards
    const char* why;
};

}  // namespace

int main() {
    // The constants the table is written against.
    CHECK(kAudioLeadStarvedPerMinute == 3);
    CHECK(kAudioLeadStepsMs[0] == 120 && kAudioLeadStepsMs[1] == 240 &&
          kAudioLeadStepsMs[2] == 480 && kAudioLeadStepsMs[3] == 960);

    const Row table[] = {
        // --- AUTOMATIC: a minute of three or more steps the lead up ONE rung ---
        {3, 120, 0, 240, "the third starved callback is the first step"},
        {3, 240, 0, 480, "240 -> 480"},
        {3, 480, 0, 960, "480 -> 960"},
        {126, 120, 0, 240, "the 12CF minute: 126 starved, still one rung, not a jump to the top"},
        {110, 240, 0, 480, "110 starved at 240: one rung"},
        {1000000, 480, 0, 960, "a huge count is still one rung"},
        // --- below the threshold: nothing -----------------------------------------
        {2, 120, 0, 120, "two starved callbacks is a stutter, not a machine that fell behind"},
        {1, 120, 0, 120, "one"},
        {0, 120, 0, 120, "none at the default"},
        {2, 240, 0, 240, "two at a raised lead"},
        // --- NEVER DOWN: a quiet minute at a raised lead does nothing ---------------
        {0, 240, 0, 240, "a quiet minute at 240 stays at 240"},
        {0, 480, 0, 480, "a quiet minute at 480 stays at 480"},
        {0, 960, 0, 960, "a quiet minute at 960 stays at 960"},
        {1, 960, 0, 960, "a nearly quiet minute at the top"},
        // --- THE CEILING: never above 960 ms -----------------------------------------
        {3, 960, 0, 960, "at the ceiling a bad minute changes nothing"},
        {5000, 960, 0, 960, "however bad"},
        {3, 1200, 0, 1200, "a lead above the ceiling (it cannot be set) is left as it is, not lowered"},
        // --- a lead that is not on the ladder takes the next rung above it ---------------
        {3, 300, 0, 480, "300 is between rungs: the rung above"},
        {3, 100, 0, 120, "below the first rung: the first"},
        {3, 959, 0, 960, "one under the top: the top"},
        // --- A FIXED SETTING NEVER STEPS ---------------------------------------------------
        {50, 120, 120, 120, "fixed 120 does not climb however bad the minute"},
        {50, 240, 240, 240, "fixed 240"},
        {3, 480, 480, 480, "fixed 480 at the threshold"},
        {5000, 960, 960, 960, "fixed 960"},
        {50, 120, 480, 120, "a fixed setting never moves the lead on its own, even to its own value"},
    };
    for (const Row& r : table) {
        const int got = nextAudioLead(r.starved, r.current, r.configured);
        if (got != r.want) {
            std::printf("FAIL nextAudioLead(%llu, %d, %d) = %d, wanted %d (%s)\n",
                        static_cast<unsigned long long>(r.starved), r.current, r.configured, got,
                        r.want, r.why);
        }
        CHECK(got == r.want);
    }

    // The two-argument form is automatic.
    CHECK(nextAudioLead(3, 120) == 240);
    CHECK(nextAudioLead(2, 120) == 120);

    // THE SEQUENCE: minute after minute of a computer that cannot keep up. One
    // rung a minute, then it holds at the top. Written as the person lives it.
    {
        int lead = 120;
        const int expect[] = {240, 480, 960, 960, 960};
        for (int minute = 0; minute < 5; ++minute) {
            lead = nextAudioLead(120, lead, 0);
            CHECK(lead == expect[minute]);
        }
    }
    // ...and a computer that recovers does not give the lead back.
    {
        int lead = 120;
        lead = nextAudioLead(10, lead, 0);   // 240
        lead = nextAudioLead(0, lead, 0);
        lead = nextAudioLead(0, lead, 0);
        CHECK(lead == 240);
    }

    // The settings the config may hold: 0 and the four rungs, and nothing else.
    for (const int ok : {0, 120, 240, 480, 960}) {
        if (!validAudioBufferSetting(ok)) { std::printf("FAIL %d should be a setting\n", ok); }
        CHECK(validAudioBufferSetting(ok));
    }
    for (const int bad : {-1, 1, 60, 100, 119, 121, 239, 300, 481, 961, 1000, 1920, 5000, INT_MIN, INT_MAX}) {
        if (validAudioBufferSetting(bad)) { std::printf("FAIL %d should not be a setting\n", bad); }
        CHECK(!validAudioBufferSetting(bad));
    }

    // Milliseconds and frames at the sink's 48 kHz, and the ladder against the
    // limits the sink holds.
    CHECK(audioLeadFrames(120) == 5760u);
    CHECK(audioLeadFrames(120) == AudioOut::kPrimeFrames);
    CHECK(audioLeadFrames(240) == 11520u);
    CHECK(audioLeadFrames(480) == 23040u);
    CHECK(audioLeadFrames(960) == 46080u);
    CHECK(audioLeadFrames(960) == AudioOut::kMaxLeadFrames);
    for (const int ms : kAudioLeadStepsMs) { CHECK(audioLeadMs(audioLeadFrames(ms)) == ms); }
    CHECK(AudioOut::kTargetMarginFrames == 1920u);   // 40 ms: today's 120 -> 160 relation
    CHECK(AudioOut::kPrimeFrames + AudioOut::kTargetMarginFrames == 7680u);   // the old fixed target

    return testSummary("test_audio_lead");
}
