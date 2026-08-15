// Tests for dsp/window.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/window.hpp"

#include "test_check.hpp"

using cascade::dsp::WindowType;

int main() {
    // Hann, periodic, n=8: w[i] = 0.5 - 0.5*cos(2*pi*i/8). Known values.
    auto hann = cascade::dsp::makeWindow(WindowType::Hann, 8);
    CHECK_NEAR(hann[0], 0.0, 1e-7);
    CHECK_NEAR(hann[1], 0.1464466, 1e-6);
    CHECK_NEAR(hann[2], 0.5, 1e-7);
    CHECK_NEAR(hann[4], 1.0, 1e-7);
    // Periodic window symmetry: w[i] == w[n-i] for 1 <= i < n.
    for (int i = 1; i < 8; ++i) { CHECK_NEAR(hann[i], hann[8 - i], 1e-7); }

    // The cosine terms of a periodic window sum to zero over a full period, so
    // Hann's coherent gain is exactly its a0 = 0.5 at any size.
    auto hannBig = cascade::dsp::makeWindow(WindowType::Hann, 1024);
    CHECK_NEAR(cascade::dsp::coherentGain(hannBig.data(), hannBig.size()), 0.5, 1e-6);

    auto rect = cascade::dsp::makeWindow(WindowType::Rectangular, 64);
    CHECK_NEAR(cascade::dsp::coherentGain(rect.data(), rect.size()), 1.0, 1e-7);

    // Blackman-Harris a0 term, same full-period argument.
    auto bh = cascade::dsp::makeWindow(WindowType::BlackmanHarris, 512);
    CHECK_NEAR(cascade::dsp::coherentGain(bh.data(), bh.size()), 0.35875, 1e-5);

    // Degenerate sizes must not crash.
    auto one = cascade::dsp::makeWindow(WindowType::Hann, 1);
    CHECK_NEAR(one[0], 1.0, 1e-9);
    cascade::dsp::fillWindow(WindowType::Hann, 0, nullptr);

    return testSummary("test_window");
}
