// The I/Q TUNNEL (src/slint/tunnel3d_logic.hpp and tunnel3d_compositor.hpp, docs 6.64) against synthetic baseband: the TIME DEPTH table and what a tap can fill, the samples to take, the axis
// range and the number formats, and the painted tunnel - a QPSK-like burst of four clusters draws every sample, time colours run from cyan at the oldest to magenta at the newest, Level
// colours differ, the inset shows the four dots in its corner, the words are the box's, a shorter depth draws fewer points, an empty feed draws the box and nothing else.
// No Slint, no engine, no window.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"
#include "tunnel3d_compositor.hpp"
#include "tunnel3d_logic.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace foxsdr::slintview3d;
namespace s3 = foxsdr::scene3d;

namespace {

struct Burst {
    std::vector<float> i, q;
};

// n samples of a QPSK-like signal: four clusters at (+-a, +-a) with a small deterministic scatter
Burst qpsk(std::size_t n, float a = 0.7f, float scatter = 0.06f) {
    Burst b;
    std::uint32_t s = 12345u;
    const auto rnd = [&] { s = s * 1664525u + 1013904223u; return (static_cast<float>(s >> 9) / 8388608.0f - 0.5f) * 2.0f; };
    for (std::size_t k = 0; k < n; ++k) {
        const int sym = static_cast<int>((k / 24) % 4);
        b.i.push_back((sym & 1 ? a : -a) + scatter * rnd());
        b.q.push_back((sym & 2 ? a : -a) + scatter * rnd());
    }
    return b;
}

TunnelInput input(const Burst& b, double rate = 48000.0) {
    TunnelInput in;
    in.i = b.i.data();
    in.q = b.q.data();
    in.n = b.i.size();
    in.rateHz = rate;
    in.camera = tunnelDefaultCamera();
    return in;
}

std::size_t countWhere(const std::vector<s3::Rgba>& px, bool (*pred)(const s3::Rgba&)) {
    std::size_t n = 0;
    for (const s3::Rgba& p : px) { n += (p.a > 150 && pred(p)) ? 1 : 0; }
    return n;
}
bool cyan(const s3::Rgba& p) { return p.r < 90 && p.g > 190 && p.b > 220; }          // the time ramp's oldest end
bool magenta(const s3::Rgba& p) { return p.r > 215 && p.g < 70 && p.b > 190; }       // its newest end
bool yellowish(const s3::Rgba& p) { return p.r > 200 && p.g > 200 && p.b < 90; }     // the level ramp's top
std::size_t opaque(const std::vector<s3::Rgba>& px) { std::size_t n = 0; for (const auto& p : px) { n += p.a > 0 ? 1 : 0; } return n; }

const s3::Label3D* find(const TunnelResult& r, const std::string& text) {
    for (const s3::Label3D& l : r.labels) { if (l.text == text) { return &l; } }
    return nullptr;
}

void testLogic() {
    CHECK(kTunnelDepthsMs.size() == 16u);
    CHECK(kTunnelDepthsMs[static_cast<std::size_t>(kTunnelDefaultDepth)] == 40.0);   // the design's 40 ms
    for (std::size_t k = 1; k < kTunnelDepthsMs.size(); ++k) { CHECK(kTunnelDepthsMs[k] > kTunnelDepthsMs[k - 1]); }
    CHECK_NEAR(availableMs(4800, 48000.0), 100.0, 1e-9);
    CHECK(availableMs(4800, 0.0) == 0.0);
    CHECK_NEAR(effectiveDepthMs(8, 100.0, 48000.0), 40.0, 1e-9);
    CHECK_NEAR(effectiveDepthMs(15, 100.0, 48000.0), 100.0, 1e-9);        // a tap that holds 100 ms cannot show 250
    CHECK_NEAR(effectiveDepthMs(0, 100.0, 48000.0), 2.0, 1e-9);
    CHECK(effectiveDepthMs(0, 0.0, 48000.0) >= 1000.0 / 48000.0 - 1e-12);  // never under one sample
    CHECK_NEAR(effectiveDepthMs(-5, 100.0, 48000.0), 2.0, 1e-9);
    CHECK_NEAR(effectiveDepthMs(99, 1000.0, 48000.0), 250.0, 1e-9);
    CHECK(samplesFor(40.0, 48000.0, 4800) == 1920u);
    CHECK(samplesFor(1000.0, 48000.0, 4800) == 4800u);                     // no more than the tap holds
    CHECK(samplesFor(0.001, 48000.0, 4800) == 2u);                         // at least two
    CHECK(samplesFor(40.0, 0.0, 300) == 300u);                             // no rate: all of it
    CHECK(samplesFor(40.0, 48000.0, 0) == 0u);
    CHECK_NEAR(niceAxis(0.7), 1.0, 1e-12);
    CHECK_NEAR(niceAxis(0.19), 0.2, 1e-12);
    CHECK_NEAR(niceAxis(0.2), 0.25, 1e-12);                                // 0.2 * 1.05 does not fit in 0.2
    CHECK_NEAR(niceAxis(1.0), 2.0, 1e-12);
    CHECK_NEAR(niceAxis(0.0), 1.0, 1e-12);
    CHECK_NEAR(niceAxis(-3.0), 1.0, 1e-12);
    CHECK_NEAR(niceAxis(std::nan("")), 1.0, 1e-12);
    CHECK_NEAR(niceAxis(40.0), 42.0, 1e-9);                                // beyond the table: the next whole number up
    CHECK(formatAxis(-0.5, 1.0) == "-0.50");
    CHECK(formatAxis(-0.0, 1.0) == "0.00");
    CHECK(formatAxis(2.5, 5.0) == "2.5");
    CHECK(formatMs(40.0, 40.0) == "40");
    CHECK(formatMs(1.5, 6.0) == "1.5");
    CHECK(formatMs(10.0, 40.0) == "10");
    CHECK(depthIndexOfSlider(0.0f) == 0 && depthIndexOfSlider(1.0f) == 15 && depthIndexOfSlider(0.5f) >= 7 && depthIndexOfSlider(0.5f) <= 8);
    CHECK(depthIndexOfSlider(-1.0f) == 0 && depthIndexOfSlider(7.0f) == 15);
    for (int k = 0; k < 16; ++k) { CHECK(depthIndexOfSlider(sliderOfDepthIndex(k)) == k); }
}

void testPaint() {
    s3::Scene scene;
    const Burst b = qpsk(1920);
    std::vector<s3::Rgba> px(480 * 320);

    // the tunnel: every sample is a point, the axis fits the burst (0.7 + scatter), the span is the samples' time
    TunnelInput in = input(b);
    const TunnelResult r = paintTunnel(scene, px.data(), 480, 320, in);
    CHECK(r.points == 1920u);
    CHECK_NEAR(r.axis, 1.0, 1e-12);
    CHECK_NEAR(r.spanMs, 40.0, 1e-9);
    CHECK(opaque(px) > 3000);
    // the words: five ticks per axis and three captions; time ticks 0 .. 40 ms, I and Q -1.00 .. 1.00
    CHECK(r.labels.size() == 18u);
    CHECK(find(r, "0") != nullptr && find(r, "10") != nullptr && find(r, "20") != nullptr && find(r, "30") != nullptr && find(r, "40") != nullptr);
    CHECK(find(r, "-1.00") != nullptr && find(r, "-0.50") != nullptr && find(r, "0.00") != nullptr && find(r, "0.50") != nullptr && find(r, "1.00") != nullptr);
    CHECK(find(r, "time (ms)") != nullptr && find(r, "I") != nullptr && find(r, "Q") != nullptr);
    // time colours: cyan at the start of the burst, magenta at its end
    CHECK(countWhere(px, cyan) > 20);
    CHECK(countWhere(px, magenta) > 20);
    // level colours are the other ramp: neither end of the time ramp, the top of the viridis one
    TunnelInput lv = in;
    lv.colourBy = 1;
    std::vector<s3::Rgba> pl(480 * 320);
    paintTunnel(scene, pl.data(), 480, 320, lv);
    CHECK(countWhere(pl, cyan) == 0u);
    CHECK(countWhere(pl, magenta) == 0u);
    CHECK(countWhere(pl, yellowish) + countWhere(pl, [](const s3::Rgba& p) { return p.g > 150 && p.r < 140 && p.b < 160; }) > 20);

    // the inset: a square in the corner holding the four clusters as four groups of dots
    CHECK(r.insetSize >= 72 && r.insetSize <= 150);
    int quadrants[4] = {0, 0, 0, 0};
    for (int y = 0; y < r.insetSize; ++y) {
        for (int x = 0; x < r.insetSize; ++x) {
            const s3::Rgba& p = px[static_cast<std::size_t>(r.insetY + y) * 480 + static_cast<std::size_t>(r.insetX + x)];
            if (p.a > 200 && (cyan(p) || magenta(p) || (p.b > 150 && p.g < 160))) { ++quadrants[(y < r.insetSize / 2 ? 0 : 2) + (x < r.insetSize / 2 ? 0 : 1)]; }
        }
    }
    for (int k = 0; k < 4; ++k) { CHECK(quadrants[k] > 10); }
    TunnelInput noInset = in;
    noInset.inset = false;
    std::vector<s3::Rgba> pn(480 * 320);
    const TunnelResult rn = paintTunnel(scene, pn.data(), 480, 320, noInset);
    CHECK(rn.insetSize == 0);
    CHECK(opaque(pn) < opaque(px));
    // a picture too small for an inset draws none
    std::vector<s3::Rgba> tiny(120 * 90);
    CHECK(paintTunnel(scene, tiny.data(), 120, 90, in).insetSize == 0);

    // TIME DEPTH: a shorter span is fewer samples, and the time axis says so
    const std::size_t take = samplesFor(10.0, 48000.0, b.i.size());
    CHECK(take == 480u);
    TunnelInput shortIn = input(b);
    shortIn.i = b.i.data() + (b.i.size() - take);
    shortIn.q = b.q.data() + (b.q.size() - take);
    shortIn.n = take;
    std::vector<s3::Rgba> ps(480 * 320);
    const TunnelResult rs = paintTunnel(scene, ps.data(), 480, 320, shortIn);
    CHECK(rs.points == 480u);
    CHECK_NEAR(rs.spanMs, 10.0, 1e-9);
    CHECK(find(rs, "10") != nullptr && find(rs, "2.5") != nullptr && find(rs, "7.5") != nullptr);

    // a weak signal gets a tighter axis, so it still fills the tunnel
    const Burst weak = qpsk(960, 0.1f, 0.01f);
    std::vector<s3::Rgba> pw(480 * 320);
    const TunnelResult rw = paintTunnel(scene, pw.data(), 480, 320, input(weak));
    CHECK_NEAR(rw.axis, 0.2, 1e-12);
    CHECK(find(rw, "0.20") != nullptr && find(rw, "-0.20") != nullptr);

    // an empty feed: the box, its rings and its words, no points, no inset dots, no crash; a null feed too
    TunnelInput none = input(b);
    none.n = 0;
    std::vector<s3::Rgba> pe(480 * 320);
    const TunnelResult re = paintTunnel(scene, pe.data(), 480, 320, none);
    CHECK(re.points == 0u);
    CHECK(opaque(pe) > 500);
    CHECK(re.labels.size() == 18u);
    CHECK_NEAR(re.axis, 1.0, 1e-12);
    TunnelInput nul;
    nul.camera = tunnelDefaultCamera();
    CHECK(paintTunnel(scene, pe.data(), 480, 320, nul).points == 0u);
    // no rate: the time axis has no milliseconds to say, and nothing divides by it
    TunnelInput norate = input(b, 0.0);
    CHECK(paintTunnel(scene, pe.data(), 480, 320, norate).spanMs == 0.0);

    // determinism, and the camera moves it
    std::vector<s3::Rgba> d1(480 * 320), d2(480 * 320), d3(480 * 320);
    paintTunnel(scene, d1.data(), 480, 320, in);
    paintTunnel(scene, d2.data(), 480, 320, in);
    CHECK(std::memcmp(d1.data(), d2.data(), d1.size() * sizeof(s3::Rgba)) == 0);
    TunnelInput turned = in;
    turned.camera.azimuthDeg = 40.0f;
    paintTunnel(scene, d3.data(), 480, 320, turned);
    CHECK(std::memcmp(d1.data(), d3.data(), d1.size() * sizeof(s3::Rgba)) != 0);
    // a sample beyond the axis is clamped to it, not drawn outside the tunnel
    Burst wild = qpsk(100);
    wild.i[50] = 1000.0f;
    std::vector<s3::Rgba> pwld(480 * 320);
    CHECK(paintTunnel(scene, pwld.data(), 480, 320, input(wild)).points == 100u);
    CHECK(tunnelRamp(0).at(0.0f).g > 200 && tunnelRamp(0).at(1.0f).r > 240);
    CHECK(&tunnelRamp(0) != &tunnelRamp(1));
}

}  // namespace

int main() {
    testLogic();
    testPaint();
    return testSummary("test_slint_host_tunnel3d");
}
