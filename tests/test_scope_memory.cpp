// Tests for gui/scope_memory.hpp and the display-mode vocabulary in
// gui/demod_scope.hpp - the demod scope's AVG and PERSIST (0.99.26).
//
// A tester asked for averaging or persistence on the FM multiplex view. What
// can go wrong quietly: an average that depends on the frame rate, one that
// carries an old axis into a new one, a peak hold that drops below the live
// trace, a persistence that never fades, and a mode index from a hand-edited
// config that selects nothing. Each is pinned.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/scope_memory.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "gui/demod_scope.hpp"
#include "test_check.hpp"

using cascade::gui::ScopeDisplay;
using cascade::gui::ScopeMemory;
using cascade::gui::scopeBlendAlpha;
using cascade::gui::scopeMemoryKey;

namespace {
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
}  // namespace

int main() {
    // --- the display vocabulary ---------------------------------------------
    CHECK(cascade::gui::kScopeDisplayCount == 3);
    CHECK(cascade::gui::scopeDisplayFromIndex(0) == ScopeDisplay::Normal);
    CHECK(cascade::gui::scopeDisplayFromIndex(1) == ScopeDisplay::Average);
    CHECK(cascade::gui::scopeDisplayFromIndex(2) == ScopeDisplay::Persist);
    // A hand-edited config: anything outside the range is the live trace,
    // never a mode that does not exist.
    CHECK(cascade::gui::clampScopeDisplay(-1) == 0);
    CHECK(cascade::gui::clampScopeDisplay(3) == 0);
    CHECK(cascade::gui::clampScopeDisplay(1 << 30) == 0);
    CHECK(std::strcmp(cascade::gui::scopeDisplayKey(ScopeDisplay::Average), "AVG") == 0);
    CHECK(std::strcmp(cascade::gui::scopeDisplayKey(ScopeDisplay::Persist), "PERSIST") == 0);
    // Normal by default, so nobody's scope changes until they press a key.
    CHECK(cascade::gui::DemodScopeState{}.display == 0);

    // --- the blend is timed, not counted ------------------------------------
    CHECK(scopeBlendAlpha(0.0, 0.5) == 0.0);
    CHECK(scopeBlendAlpha(-1.0, 0.5) == 0.0);
    CHECK(scopeBlendAlpha(std::nan(""), 0.5) == 0.0);
    CHECK(scopeBlendAlpha(0.1, 0.0) == 1.0);
    // THE SAME AVERAGE AT ANY FRAME RATE. Averaging a constant step for one
    // second in 30 frames and in 144 frames must land in the same place.
    // RED WHEN the blend uses a fixed per-frame weight.
    {
        const double wanted = 1.0 - std::exp(-1.0 / cascade::gui::kScopeAverageTauS);
        for (int fps : {30, 60, 144}) {
            ScopeMemory m;
            const float zero = 0.0f;
            const float one = 1.0f;
            m.average(ScopeMemory::kSpectrum, &zero, 1, 0.0, 7u);
            const float* v = nullptr;
            for (int f = 0; f < fps; ++f) {
                v = m.average(ScopeMemory::kSpectrum, &one, 1, 1.0 / fps, 7u);
            }
            if (!near(v[0], wanted, 1e-4)) {
                std::printf("  (%d fps: average %.5f after 1 s, wanted %.5f)\n", fps, v[0], wanted);
            }
            CHECK(v != nullptr && near(v[0], wanted, 1e-4));
        }
    }

    // --- average: seeded, keyed, NaN-proof ----------------------------------
    {
        ScopeMemory m;
        const float a[3] = {-40.0f, -50.0f, -60.0f};
        const float* v = m.average(ScopeMemory::kSpectrum, a, 3, 0.016, 1u);
        // The first trace IS the average - not a trace growing out of zero.
        CHECK(v[0] == -40.0f && v[1] == -50.0f && v[2] == -60.0f);
        const float b[3] = {-20.0f, std::numeric_limits<float>::quiet_NaN(), -60.0f};
        v = m.average(ScopeMemory::kSpectrum, b, 3, 0.1, 1u);
        CHECK(v[0] > -40.0f && v[0] < -20.0f);  // moved toward the new trace
        CHECK(v[1] == -50.0f);                   // a NaN neither enters nor spoils
        CHECK(v[2] == -60.0f);
        // A NEW AXIS STARTS AGAIN. RED WHEN a key change is ignored - the
        // multiplex would inherit the audio spectrum's shape.
        const float c[3] = {-10.0f, -10.0f, -10.0f};
        v = m.average(ScopeMemory::kSpectrum, c, 3, 0.1, 2u);
        CHECK(v[0] == -10.0f && v[1] == -10.0f && v[2] == -10.0f);
        // ...and so does a new length.
        const float d[2] = {-5.0f, -5.0f};
        v = m.average(ScopeMemory::kSpectrum, d, 2, 0.1, 2u);
        CHECK(v[0] == -5.0f && v[1] == -5.0f);
        CHECK(m.average(ScopeMemory::kSpectrum, nullptr, 2, 0.1, 2u) == nullptr);
    }

    // --- persist on a spectrum: a falling peak, never under the live line ---
    {
        ScopeMemory m;
        const float peak[2] = {-10.0f, -70.0f};
        m.persistPeak(ScopeMemory::kSpectrum, peak, 2, 0.0, 3u);
        const float quiet[2] = {-80.0f, -60.0f};
        const float* h = m.persistPeak(ScopeMemory::kSpectrum, quiet, 2, 0.5, 3u);
        // Half a second at 40 dB/s: the peak has fallen 20 dB, not vanished.
        CHECK(near(h[0], -30.0, 1e-4));
        // Where the live trace is higher than the fallen hold, the hold is
        // the live trace. RED WHEN it is allowed to sit below it.
        CHECK(h[1] == -60.0f);
        // Given long enough it is simply the live trace.
        h = m.persistPeak(ScopeMemory::kSpectrum, quiet, 2, 10.0, 3u);
        CHECK(h[0] == -80.0f && h[1] == -60.0f);
        // No time passed, nothing fell.
        const float up[2] = {0.0f, 0.0f};
        m.persistPeak(ScopeMemory::kSpectrum, up, 2, 0.1, 3u);
        h = m.persistPeak(ScopeMemory::kSpectrum, quiet, 2, 0.0, 3u);
        CHECK(h[0] == 0.0f && h[1] == 0.0f);
    }

    // --- persist on a waveform: an envelope that closes onto the live trace --
    {
        ScopeMemory m;
        const float lo1[2] = {-1.0f, -0.5f};
        const float hi1[2] = {1.0f, 0.5f};
        const float* mlo = nullptr;
        const float* mhi = nullptr;
        CHECK(m.persistEnvelope(ScopeMemory::kAudio, lo1, hi1, 2, 0.0, 8.0f, 9u, &mlo, &mhi));
        const float lo2[2] = {-0.1f, -0.1f};
        const float hi2[2] = {0.1f, 0.1f};
        // A quarter of the persistence time across an 8-unit tube closes the
        // band by 8 * 0.25 = 2 units - past the live trace here, so it stops
        // AT the live trace rather than crossing it.
        CHECK(m.persistEnvelope(ScopeMemory::kAudio, lo2, hi2, 2, cascade::gui::kScopePersistS * 0.25,
                                8.0f, 9u, &mlo, &mhi));
        CHECK(mlo[0] <= lo2[0] && mhi[0] >= hi2[0]);
        // A short step keeps most of the old excursion: 0.1 of the time is 0.8 units.
        ScopeMemory m2;
        m2.persistEnvelope(ScopeMemory::kAudio, lo1, hi1, 2, 0.0, 8.0f, 9u, &mlo, &mhi);
        m2.persistEnvelope(ScopeMemory::kAudio, lo2, hi2, 2, cascade::gui::kScopePersistS * 0.01,
                           8.0f, 9u, &mlo, &mhi);
        CHECK(near(mhi[0], 1.0 - 0.08, 1e-4));
        CHECK(near(mlo[0], -1.0 + 0.08, 1e-4));
        // A new excursion is taken in at once. RED WHEN the memory lags it.
        const float lo3[2] = {-3.0f, -0.1f};
        const float hi3[2] = {3.0f, 0.1f};
        m2.persistEnvelope(ScopeMemory::kAudio, lo3, hi3, 2, 0.01, 8.0f, 9u, &mlo, &mhi);
        CHECK(mlo[0] == -3.0f && mhi[0] == 3.0f);
    }

    // --- persist on the vector display: ghosts that expire ------------------
    {
        ScopeMemory m;
        std::vector<float> iq(10000, 0.5f);
        m.pushVector(iq.data(), iq.data(), iq.size(), 10.0, 5u, 1024);
        CHECK(m.ghosts(10.0).size() == 1u);
        CHECK(m.ghosts(10.0).front().i.size() <= 1024u);  // thinned
        m.pushVector(iq.data(), iq.data(), iq.size(), 10.5, 5u, 1024);
        CHECK(m.ghosts(10.5).size() == 2u);
        // Past the persistence time the old ones are gone. RED WHEN ghosts
        // accumulate forever.
        CHECK(m.ghosts(10.0 + cascade::gui::kScopePersistS + 0.01).size() == 1u);
        CHECK(m.ghosts(20.0).empty());
        // Brightness falls with age from 1 to 0.
        CHECK(ScopeMemory::ghostWeight(0.0) == 1.0f);
        CHECK(near(ScopeMemory::ghostWeight(cascade::gui::kScopePersistS * 0.5), 0.5, 1e-6));
        CHECK(ScopeMemory::ghostWeight(99.0) == 0.0f);
        // A stalled clock cannot grow the list without bound.
        ScopeMemory s;
        for (int k = 0; k < 1000; ++k) { s.pushVector(iq.data(), iq.data(), 16, 1.0, 5u, 16); }
        CHECK(s.ghosts(1.0).size() <= ScopeMemory::kMaxGhosts);
        // A new axis drops the old figures.
        s.pushVector(iq.data(), iq.data(), 16, 1.0, 6u, 16);
        CHECK(s.ghosts(1.0).size() == 1u);
    }

    // --- reset forgets everything -------------------------------------------
    {
        ScopeMemory m;
        const float a[1] = {1.0f};
        const float b[1] = {5.0f};
        m.average(ScopeMemory::kAudio, a, 1, 0.0, 1u);
        m.reset();
        const float* v = m.average(ScopeMemory::kAudio, b, 1, 0.1, 1u);
        CHECK(v[0] == 5.0f);  // seeded afresh, not blended with the old 1.0
    }

    CHECK(scopeMemoryKey(1, 2, 3) != scopeMemoryKey(1, 2, 4));

    return testSummary("test_scope_memory");
}
