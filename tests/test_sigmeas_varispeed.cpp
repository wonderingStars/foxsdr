// Tests for dsp/sigmeas_varispeed.hpp: varispeed playback (pitch follows speed, reverse, smooth
// speed changes, stop at the ends) measured with the audio block's tone finder.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_varispeed.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include "dsp/sigmeas_audio.hpp"
#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {

std::vector<float> tone(std::size_t n, double fs, double f, double a = 0.5) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(a * std::sin(sg::kTwoPi * f * static_cast<double>(i) / fs));
    }
    return x;
}

double toneOf(const std::vector<float>& y, double fs) {
    const AudioResult r = measureAudio(y.data(), y.size(), fs);
    return r.valid ? r.toneHz : -1.0;
}

}  // namespace

int main() {
    const double fs = 48000.0;
    const std::vector<float> src = tone(96000, fs, 1000.0);

    // --- speed 1 is a copy, exactly ---------------------------------------------------------------------------
    {
        Varispeed v;
        CHECK(v.setSource(src.data(), src.size()));
        std::vector<float> out(5000);
        CHECK(v.read(out.data(), out.size()) == out.size());
        bool same = true;
        for (std::size_t i = 0; i < out.size(); ++i) { if (out[i] != src[i]) { same = false; break; } }
        CHECK(same);
        CHECK_NEAR(v.position(), 5000.0, 1e-9);
        CHECK(!v.atEnd());
    }
    // --- pitch follows speed: 1.5x -> 1500 Hz, 0.5x -> 500 Hz, 2.5x ---------------------------------------------
    for (double s : {1.5, 0.5, 2.5}) {
        Varispeed v;
        v.setSource(src.data(), src.size());
        v.setSpeed(s);
        std::vector<float> out(static_cast<std::size_t>(30000));
        const std::size_t got = v.read(out.data(), out.size());
        CHECK(got == out.size());
        CHECK_NEAR(toneOf(out, fs), 1000.0 * s, 1000.0 * s * 0.001 + 0.2);
        CHECK_NEAR(v.position(), s * 30000.0, 1e-6);
        CHECK_NEAR(v.speed(), s, 1e-12);
    }
    // --- reverse: speed -1 from the last sample returns the source backwards, exactly ------------------------------
    {
        Varispeed v;
        v.setSource(src.data(), src.size());
        CHECK(v.seek(static_cast<double>(src.size() - 1)));
        v.setSpeed(-1.0);
        std::vector<float> out(1000);
        CHECK(v.read(out.data(), out.size()) == 1000);
        bool same = true;
        for (std::size_t i = 0; i < out.size(); ++i) { if (out[i] != src[src.size() - 1 - i]) { same = false; break; } }
        CHECK(same);
        // run off the start: fewer samples than asked, atEnd set
        CHECK(v.seek(10.0));
        std::vector<float> few(100);
        const std::size_t got = v.read(few.data(), few.size());
        CHECK(got == 11);
        CHECK(v.atEnd());
        CHECK(v.read(few.data(), few.size()) == 0);
        CHECK(v.seek(500.0) && !v.atEnd());
    }
    // --- forward run off the end -------------------------------------------------------------------------------------------
    {
        Varispeed v;
        v.setSource(src.data(), src.size());
        v.seek(static_cast<double>(src.size()) - 10.0);
        std::vector<float> out(100);
        const std::size_t got = v.read(out.data(), out.size());
        CHECK(got == 10);
        CHECK(v.atEnd());
    }
    // --- a smooth speed change: 1 -> 2 at 1/1000 per sample; the position is the sum of the speeds -------------------------
    {
        Varispeed v;
        v.setSource(src.data(), src.size());
        CHECK(v.setSlew(0.001));
        CHECK(v.setSpeed(2.0));
        CHECK_NEAR(v.speed(), 1.0, 1e-12);   // not yet: the slew has not run
        std::vector<float> out(1000);
        CHECK(v.read(out.data(), out.size()) == 1000);
        CHECK_NEAR(v.speed(), 2.0, 1e-9);
        double expect = 0.0;
        for (int i = 1; i <= 1000; ++i) { expect += 1.0 + i / 1000.0; }
        CHECK_NEAR(v.position(), expect, 1e-6);
        // and no step in the output at the start of the change (adjacent samples stay close)
        double worst = 0.0;
        for (std::size_t i = 1; i < out.size(); ++i) { worst = std::max(worst, static_cast<double>(std::fabs(out[i] - out[i - 1]))); }
        CHECK(worst < 0.2);
        // slowing down follows too
        v.setSpeed(0.5);
        out.resize(2000);
        v.read(out.data(), out.size());
        CHECK_NEAR(v.speed(), 0.5, 1e-9);
    }
    // --- speed 0 holds the position -------------------------------------------------------------------------------------------
    {
        Varispeed v;
        v.setSource(src.data(), src.size());
        v.seek(100.0);
        v.setSpeed(0.0);
        std::vector<float> out(10);
        CHECK(v.read(out.data(), out.size()) == 10);
        CHECK(out[0] == out[9] && out[0] == src[100]);
        CHECK_NEAR(v.position(), 100.0, 1e-12);
    }
    // --- limits and failure paths ----------------------------------------------------------------------------------------------
    {
        Varispeed v;
        std::vector<float> out(10);
        CHECK(v.read(out.data(), out.size()) == 0);       // no source yet
        CHECK(!v.setSource(nullptr, 100));
        CHECK(!v.setSource(src.data(), 3));
        CHECK(v.setSource(src.data(), src.size()));
        CHECK(v.sourceLength() == src.size());
        CHECK(v.setSpeed(100.0));
        CHECK_NEAR(v.targetSpeed(), Varispeed::kMaxSpeed, 1e-12);
        CHECK(v.setSpeed(-100.0));
        CHECK_NEAR(v.targetSpeed(), -Varispeed::kMaxSpeed, 1e-12);
        CHECK(!v.setSpeed(std::nan("")));
        CHECK(!v.setSpeed(std::numeric_limits<double>::infinity()));
        CHECK_NEAR(v.targetSpeed(), -Varispeed::kMaxSpeed, 1e-12);   // unchanged by the refusals
        CHECK(!v.setSlew(-1.0));
        CHECK(!v.setSlew(std::nan("")));
        CHECK(!v.seek(std::nan("")));
        CHECK(v.seek(-50.0) && v.position() == 0.0);       // clamped
        CHECK(v.seek(1e12) && v.position() == static_cast<double>(src.size() - 1));
        CHECK(v.read(nullptr, 10) == 0);
        CHECK(v.read(out.data(), 0) == 0);
    }
    return testSummary("test_sigmeas_varispeed");
}
