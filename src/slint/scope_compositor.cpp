// scope_compositor.cpp - the scopes' pictures (see scope_compositor.hpp). The demod tube is a port of demod_scope_face.cpp's drawDemodScopeFace (383) with the text left to Slint.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scope_compositor.hpp"

#include <algorithm>
#include <cmath>

#include "gui/demod_scope.hpp"
#include "gui/fm_mpx_plan.hpp"
#include "gui/scope_memory.hpp"

namespace foxsdr::slintscope {

using cascade::gui::ScopeSignal;

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the canvas
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Canvas::fill(Rgba c) { std::fill(px_, px_ + static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), c); }

void Canvas::blend(int x, int y, Rgba c, float cover) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) { return; }
    const int a = static_cast<int>(std::clamp(cover, 0.0f, 1.0f) * static_cast<float>(c.a) + 0.5f);   // 0..255
    if (a <= 0) { return; }
    Rgba& d = px_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
    const int ia = 255 - a;
    d.r = static_cast<std::uint8_t>((d.r * ia + c.r * a + 127) / 255);
    d.g = static_cast<std::uint8_t>((d.g * ia + c.g * a + 127) / 255);
    d.b = static_cast<std::uint8_t>((d.b * ia + c.b * a + 127) / 255);
    d.a = 255;
}

void Canvas::line(float x0, float y0, float x1, float y1, float widthPx, Rgba c) {
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) { return; }
    const float half = widthPx * 0.5f;
    if (x0 == x1 && y0 == y1) { disc(x0, y0, half, c); return; }
    // STEPPED ALONG THE LONGER AXIS: each step covers the few pixels across the stroke, so a long diagonal costs its length, not its bounding box (a PERSIST figure is tens of thousands of strokes)
    const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    const float invLen2 = 1.0f / len2, invLen = std::sqrt(invLen2);
    const int majorMax = steep ? h_ : w_, minorMax = steep ? w_ : h_;
    const float reach = (half + 1.0f) * 1.5f;   // the stroke's half thickness across the minor axis, at its steepest (45 degrees) with a pixel to spare
    const int xa = std::max(0, static_cast<int>(std::floor(x0 - half - 1.0f)));
    const int xb = std::min(majorMax - 1, static_cast<int>(std::ceil(x1 + half + 1.0f)));
    for (int X = xa; X <= xb; ++X) {
        const float xc = static_cast<float>(X) + 0.5f;
        const float t = std::clamp((xc - x0) / dx, 0.0f, 1.0f);
        const float yc = y0 + t * dy;
        const int ya = std::max(0, static_cast<int>(std::floor(yc - reach)));
        const int yb = std::min(minorMax - 1, static_cast<int>(std::ceil(yc + reach)));
        for (int Y = ya; Y <= yb; ++Y) {
            const float px = xc - x0, py = static_cast<float>(Y) + 0.5f - y0;
            const float uraw = (px * dx + py * dy) * invLen2;
            float d;
            if (uraw >= 0.0f && uraw <= 1.0f) {
                d = std::abs(px * dy - py * dx) * invLen;   // across the stroke: no square root
            } else {
                const float u = uraw < 0.0f ? 0.0f : 1.0f;   // past an end: the distance to the end point
                const float ex = px - u * dx, ey = py - u * dy;
                d = std::sqrt(ex * ex + ey * ey);
            }
            const float cover = std::clamp(half + 0.5f - d, 0.0f, 1.0f);
            if (cover > 0.0f) { steep ? blend(Y, X, c, cover) : blend(X, Y, c, cover); }
        }
    }
}

void Canvas::rect(float x0, float y0, float x1, float y1, Rgba c) {
    const int xa = std::max(0, static_cast<int>(std::floor(x0))), xb = std::min(w_, static_cast<int>(std::ceil(x1)));
    const int ya = std::max(0, static_cast<int>(std::floor(y0))), yb = std::min(h_, static_cast<int>(std::ceil(y1)));
    if (xa >= xb || ya >= yb || c.a == 0) { return; }
    const int a = c.a, ia = 255 - a;
    for (int y = ya; y < yb; ++y) {
        Rgba* row = px_ + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_);
        for (int x = xa; x < xb; ++x) {
            Rgba& d = row[x];
            if (a == 255) {
                d = Rgba{c.r, c.g, c.b, 255};
            } else {
                d.r = static_cast<std::uint8_t>((d.r * ia + c.r * a + 127) / 255);
                d.g = static_cast<std::uint8_t>((d.g * ia + c.g * a + 127) / 255);
                d.b = static_cast<std::uint8_t>((d.b * ia + c.b * a + 127) / 255);
                d.a = 255;
            }
        }
    }
}

void Canvas::disc(float cx, float cy, float r, Rgba c) {
    const int xa = std::max(0, static_cast<int>(std::floor(cx - r - 1))), xb = std::min(w_ - 1, static_cast<int>(std::ceil(cx + r + 1)));
    const int ya = std::max(0, static_cast<int>(std::floor(cy - r - 1))), yb = std::min(h_ - 1, static_cast<int>(std::ceil(cy + r + 1)));
    for (int y = ya; y <= yb; ++y) {
        for (int x = xa; x <= xb; ++x) {
            const float d = std::hypot(static_cast<float>(x) + 0.5f - cx, static_cast<float>(y) + 0.5f - cy);
            const float cover = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            if (cover > 0.0f) { blend(x, y, c, cover); }
        }
    }
}

void Canvas::ring(float cx, float cy, float r, float widthPx, Rgba c) {
    const float outer = r + widthPx * 0.5f + 1.0f;
    const int xa = std::max(0, static_cast<int>(std::floor(cx - outer))), xb = std::min(w_ - 1, static_cast<int>(std::ceil(cx + outer)));
    const int ya = std::max(0, static_cast<int>(std::floor(cy - outer))), yb = std::min(h_ - 1, static_cast<int>(std::ceil(cy + outer)));
    for (int y = ya; y <= yb; ++y) {
        for (int x = xa; x <= xb; ++x) {
            const float d = std::abs(std::hypot(static_cast<float>(x) + 0.5f - cx, static_cast<float>(y) + 0.5f - cy) - r);
            const float cover = std::clamp(widthPx * 0.5f + 0.5f - d, 0.0f, 1.0f);
            if (cover > 0.0f) { blend(x, y, c, cover); }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the demod tube
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

constexpr Rgba kGlass{9, 18, 11, 255};
constexpr Rgba kRule{143, 217, 160, 38};
constexpr Rgba kRuleAxis{143, 217, 160, 92};
constexpr Rgba kRuleTick{143, 217, 160, 70};
constexpr Rgba kBeam{180, 255, 195, 235};
constexpr Rgba kBeamGlow{143, 217, 160, 64};
constexpr Rgba kBeamQ{120, 200, 235, 225};
constexpr Rgba kBeamQGlow{120, 200, 235, 56};
constexpr Rgba kBeamQMem{120, 200, 235, 95};
constexpr Rgba kBeamQMemGlow{120, 200, 235, 30};
constexpr Rgba kPersist{143, 217, 160, 105};
constexpr Rgba kPersistGlow{143, 217, 160, 34};
constexpr Rgba kHold{236, 186, 92, 200};
constexpr Rgba kHoldWash{236, 186, 92, 22};
constexpr Rgba kScan{0, 0, 0, 30};
constexpr std::size_t kMaxVectorPoints = 2048;   // the classic thins to 4096; a CPU raster strokes half that (a figure still closes: the figures of a real signal are smooth)
constexpr std::size_t kGhostPointBudget = 40000;   // the classic draws up to 245 000 ghost segments a frame; this bounds the CPU raster (docs 6.57: a departure)

struct Counter {
    std::size_t n = 0;
};

void seg(Canvas& cv, Counter& k, float x0, float y0, float x1, float y1, float wd, Rgba c) {
    cv.line(x0, y0, x1, y1, wd, c);
    ++k.n;
}

void addGraticule(Canvas& cv, Counter& k, float aX, float aY, float bX, float bY) {
    using namespace cascade::gui;
    for (int i = 0; i <= kScopeDivX; ++i) {
        const float x = std::floor(scopeGridLine(i, kScopeDivX, aX, bX));
        cv.rect(x, aY, x + 1.0f, bY, scopeGridIsCentre(i, kScopeDivX) ? kRuleAxis : kRule);
        ++k.n;
    }
    for (int i = 0; i <= kScopeDivY; ++i) {
        const float y = std::floor(scopeGridLine(i, kScopeDivY, aY, bY));
        cv.rect(aX, y, bX, y + 1.0f, scopeGridIsCentre(i, kScopeDivY) ? kRuleAxis : kRule);
        ++k.n;
    }
    // THE MINOR TICKS ON THE TWO AXES, five to a division.
    const float cy = std::floor(scopeGridLine(kScopeDivY / 2, kScopeDivY, aY, bY));
    const float cx = std::floor(scopeGridLine(kScopeDivX / 2, kScopeDivX, aX, bX));
    const float divW = (bX - aX) / static_cast<float>(kScopeDivX), divH = (bY - aY) / static_cast<float>(kScopeDivY);
    for (int d = 0; d < kScopeDivX; ++d) {
        for (int s = 1; s < 5; ++s) {
            const float x = std::floor(aX + divW * (static_cast<float>(d) + static_cast<float>(s) / 5.0f));
            cv.rect(x, cy - 3.0f, x + 1.0f, cy + 4.0f, kRuleTick);
            ++k.n;
        }
    }
    for (int d = 0; d < kScopeDivY; ++d) {
        for (int s = 1; s < 5; ++s) {
            const float y = std::floor(aY + divH * (static_cast<float>(d) + static_cast<float>(s) / 5.0f));
            cv.rect(cx - 3.0f, y, cx + 4.0f, y + 1.0f, kRuleTick);
            ++k.n;
        }
    }
}

// One trace: the envelope as a band with the beam on its edges and the columns joined (demod_scope_face.cpp addTrace).
void addTrace(Canvas& cv, Counter& k, const float* lo, const float* hi, std::size_t cols, float x0, float unitsPerDiv, float yCentre, float divPx, Rgba beam, Rgba glow) {
    if (lo == nullptr || hi == nullptr || cols == 0) { return; }
    float prevY = yCentre;
    for (std::size_t c = 0; c < cols; ++c) {
        const float x = x0 + static_cast<float>(c) + 0.5f;
        const float yHi = cascade::gui::scopeTraceY(hi[c], unitsPerDiv, yCentre, divPx);
        const float yLo = cascade::gui::scopeTraceY(lo[c], unitsPerDiv, yCentre, divPx);
        seg(cv, k, x, yHi - 1.5f, x, yLo + 1.5f, 2.5f, glow);
        seg(cv, k, x, yHi, x, yLo + 1.0f, 1.0f, beam);
        if (c > 0) {
            const float j0 = std::min(prevY, yLo), j1 = std::max(prevY, yHi);
            if (j1 - j0 > 1.0f) { seg(cv, k, x, j0, x, j1, 1.0f, beam); }
        }
        prevY = 0.5f * (yHi + yLo);
    }
}

}  // namespace

std::size_t paintDemodFace(Rgba* px, int w, int h, DemodScope& scope, PaintScratch& scratch) {
    using namespace cascade::gui;
    Canvas cv(px, w, h);
    cv.fill(kGlass);
    Counter k;
    if (w < kDemodMinW || h < kDemodMinH) { return 0; }

    const float aX = 0.0f, aY = 0.0f, bX = static_cast<float>(w), bY = static_cast<float>(h);
    addGraticule(cv, k, aX, aY, bX, bY);

    const DemodFeed& feed = scope.feed();
    const DemodScopeState& state = scope.state;
    const ScopeSignal signal = scope.signalNow();
    const float divPx = (bY - aY) / static_cast<float>(kScopeDivY);
    const float yCentre = scopeGridLine(kScopeDivY / 2, kScopeDivY, aY, bY);
    const std::size_t cols = static_cast<std::size_t>(std::max(1.0f, bX - aX));
    std::vector<float>& lo = scratch.lo;
    std::vector<float>& hi = scratch.hi;
    lo.resize(cols);
    hi.resize(cols);
    const float unitsPerDiv = scopeGainPerDiv(state.gainIndex);
    const bool persist = scopeDisplayFromIndex(state.display) == ScopeDisplay::Persist;
    const float fullRange = unitsPerDiv * static_cast<float>(kScopeDivY);
    ScopeMemory& mem = scope.memory();
    const double dtS = scope.dtS();

    if (feed.live) {
        switch (signal) {
            case ScopeSignal::Audio: {
                if (feed.audio != nullptr && feed.audioCount > 0 && scopeReduce(feed.audio, feed.audioCount, cols, lo.data(), hi.data())) {
                    if (persist) {
                        const float *mLo = nullptr, *mHi = nullptr;
                        if (mem.persistEnvelope(ScopeMemory::kAudio, lo.data(), hi.data(), cols, dtS, fullRange, scopeMemoryKey(1u, cols, static_cast<std::uint64_t>(state.timebase)), &mLo, &mHi)) {
                            addTrace(cv, k, mLo, mHi, cols, aX, unitsPerDiv, yCentre, divPx, kPersist, kPersistGlow);
                        }
                    }
                    addTrace(cv, k, lo.data(), hi.data(), cols, aX, unitsPerDiv, yCentre, divPx, kBeam, kBeamGlow);
                }
                break;
            }
            case ScopeSignal::Spectrum:
            case ScopeSignal::Mpx: {
                if (feed.spectrumDb == nullptr || feed.spectrumBins < 2 || !(feed.spectrumBinHz > 0.0)) { break; }
                if (signal == ScopeSignal::Mpx) {
                    // the multiplex's rules under the beam: a bracket for a band, one rule the full height for a tone (the names are Slint's)
                    for (const MpxMark& m : scope.mpxMarks()) {
                        const Rgba ink = m.optional ? kRule : kRuleAxis;
                        const float x0 = aX + (bX - aX) * static_cast<float>(m.x0), x1 = aX + (bX - aX) * static_cast<float>(m.x1);
                        if (m.tone) {
                            cv.rect(std::floor(x0), aY + 2.0f, std::floor(x0) + 1.0f, bY - 2.0f, ink);
                        } else {
                            const float by = aY + 10.0f;
                            cv.rect(std::floor(x0), by, std::floor(x1) + 1.0f, by + 1.0f, ink);
                            cv.rect(std::floor(x0), by, std::floor(x0) + 1.0f, by + 5.0f, ink);
                            cv.rect(std::floor(x1), by, std::floor(x1) + 1.0f, by + 5.0f, ink);
                        }
                        ++k.n;
                    }
                }
                const std::size_t n = feed.spectrumBins;
                if (persist) {
                    const float* held = mem.persistPeak(ScopeMemory::kSpectrum, feed.spectrumDb, n, dtS, scopeMemoryKey(2u, static_cast<std::uint64_t>(signal), n));
                    if (held != nullptr) {
                        // ONE VALUE PER PIXEL COLUMN FIRST (a wash drawn per bin stacks into a solid block): the highest held bin of each column
                        std::fill(lo.begin(), lo.end(), bY);
                        for (std::size_t i = 0; i < n; ++i) {
                            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
                            std::size_t c = static_cast<std::size_t>(t * static_cast<float>(cols - 1));
                            if (c >= cols) { c = cols - 1; }
                            lo[c] = std::min(lo[c], scopeSpectrumY(held[i], aY, bY));
                        }
                        const float colW = cols > 1 ? (bX - aX) / static_cast<float>(cols - 1) : 0.0f;
                        for (std::size_t c = 0; c < cols; ++c) {
                            const float x = aX + colW * static_cast<float>(c);
                            seg(cv, k, x, lo[c], x, bY, colW + 0.5f, kHoldWash);
                            if (c > 0) { seg(cv, k, x - colW, lo[c - 1], x, lo[c], 1.0f, kHold); }
                        }
                    }
                }
                float prevX = aX, prevY = scopeSpectrumY(feed.spectrumDb[0], aY, bY);
                for (std::size_t i = 1; i < n; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(n - 1);
                    const float x = aX + (bX - aX) * t, y = scopeSpectrumY(feed.spectrumDb[i], aY, bY);
                    // a bin finer than a pixel adds nothing the previous one did not draw
                    if (x - prevX >= 0.5f || i == n - 1 || std::abs(y - prevY) > 1.0f) {
                        seg(cv, k, prevX, prevY, x, y, 3.0f, kBeamGlow);
                        seg(cv, k, prevX, prevY, x, y, 1.0f, kBeam);
                        prevX = x;
                        prevY = y;
                    }
                }
                break;
            }
            case ScopeSignal::Baseband: {
                if (feed.iqI == nullptr || feed.iqQ == nullptr || feed.iqCount == 0) { break; }
                const float qCentre = yCentre + divPx * 2.0f, iCentre = yCentre - divPx * 2.0f;
                const std::uint64_t bbKey = scopeMemoryKey(3u, cols, static_cast<std::uint64_t>(state.timebase));
                if (scopeReduce(feed.iqI, feed.iqCount, cols, lo.data(), hi.data())) {
                    const float *mLo = nullptr, *mHi = nullptr;
                    if (persist && mem.persistEnvelope(ScopeMemory::kI, lo.data(), hi.data(), cols, dtS, fullRange, bbKey, &mLo, &mHi)) {
                        addTrace(cv, k, mLo, mHi, cols, aX, unitsPerDiv, iCentre, divPx, kPersist, kPersistGlow);
                    }
                    addTrace(cv, k, lo.data(), hi.data(), cols, aX, unitsPerDiv, iCentre, divPx, kBeam, kBeamGlow);
                }
                if (scopeReduce(feed.iqQ, feed.iqCount, cols, lo.data(), hi.data())) {
                    const float *mLo = nullptr, *mHi = nullptr;
                    if (persist && mem.persistEnvelope(ScopeMemory::kQ, lo.data(), hi.data(), cols, dtS, fullRange, bbKey, &mLo, &mHi)) {
                        addTrace(cv, k, mLo, mHi, cols, aX, unitsPerDiv, qCentre, divPx, kBeamQMem, kBeamQMemGlow);
                    }
                    addTrace(cv, k, lo.data(), hi.data(), cols, aX, unitsPerDiv, qCentre, divPx, kBeamQ, kBeamQGlow);
                }
                break;
            }
            case ScopeSignal::Vector: {
                if (feed.iqI == nullptr || feed.iqQ == nullptr || feed.iqCount == 0) { break; }
                const float cx = scopeGridLine(kScopeDivX / 2, kScopeDivX, aX, bX);
                if (persist) {
                    // THE PHOSPHOR: an intensity buffer that decays and is refreshed by this frame's figure (the classic's ghosts fade linearly over 1.5 s; this fades by e^-2 a second)
                    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
                    const std::uint64_t key = scopeMemoryKey(4u, static_cast<std::uint64_t>(state.timebase), static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h));
                    if (scratch.phosphor.size() != n || scratch.key != key) {
                        scratch.phosphor.assign(n, 0.0f);
                        scratch.key = key;
                    }
                    scratch.pw = w;
                    scratch.ph = h;
                    scratch.deposit.assign(n, Rgba{0, 0, 0, 255});
                    Canvas dep(scratch.deposit.data(), w, h);
                    Counter unused;
                    const std::size_t dstep = std::max<std::size_t>(1, (feed.iqCount + kMaxVectorPoints / 4 - 1) / (kMaxVectorPoints / 4));
                    float qx = 0.0f, qy = 0.0f;
                    bool have2 = false;
                    for (std::size_t i = 0; i < feed.iqCount; i += dstep) {
                        float x = 0.0f, y = 0.0f;
                        scopeVectorPoint(feed.iqI[i], feed.iqQ[i], unitsPerDiv, cx, yCentre, divPx, x, y);
                        if (have2) { seg(dep, unused, qx, qy, x, y, 1.0f, Rgba{255, 255, 255, 255}); }
                        qx = x;
                        qy = y;
                        have2 = true;
                    }
                    const float decay = std::exp(-2.0f * static_cast<float>(std::clamp(dtS, 0.0, 0.25)));
                    float peak = 0.0f;
                    constexpr float kGhostAlpha = 90.0f;   // the classic's ghost alpha at full weight
                    for (std::size_t i = 0; i < n; ++i) {
                        const float fresh = static_cast<float>(scratch.deposit[i].r) * (1.0f / 255.0f);
                        float v = std::max(scratch.phosphor[i] * decay, fresh);
                        if (v < 0.01f) { scratch.phosphor[i] = 0.0f; continue; }
                        scratch.phosphor[i] = v;
                        peak = std::max(peak, v);
                        const int a = static_cast<int>(v * kGhostAlpha + 0.5f);
                        if (a <= 0) { continue; }
                        const int ia = 255 - a;
                        Rgba& d = px[i];
                        d.r = static_cast<std::uint8_t>((d.r * ia + kBeamGlow.r * a + 127) / 255);
                        d.g = static_cast<std::uint8_t>((d.g * ia + kBeamGlow.g * a + 127) / 255);
                        d.b = static_cast<std::uint8_t>((d.b * ia + kBeamGlow.b * a + 127) / 255);
                    }
                    scratch.ghostPeak = peak;
                    ++k.n;
                }
                const std::size_t step = (feed.iqCount + kMaxVectorPoints - 1) / kMaxVectorPoints;
                float px0 = 0.0f, py0 = 0.0f;
                bool have = false;
                for (std::size_t i = 0; i < feed.iqCount; i += (step > 0 ? step : 1)) {
                    float x = 0.0f, y = 0.0f;
                    scopeVectorPoint(feed.iqI[i], feed.iqQ[i], unitsPerDiv, cx, yCentre, divPx, x, y);
                    if (have) {
                        seg(cv, k, px0, py0, x, y, 2.5f, kBeamGlow);
                        seg(cv, k, px0, py0, x, y, 1.0f, kBeam);
                    }
                    px0 = x;
                    py0 = y;
                    have = true;
                }
                break;
            }
        }
    }

    // the phosphor belongs to a live PERSIST vector figure only
    if (!(feed.live && signal == ScopeSignal::Vector && persist) && !scratch.phosphor.empty()) {
        scratch.phosphor.clear();
        scratch.ghostPeak = 0.0;
    }
    // ONE DARK ROW EVERY THREE PIXELS, the radar tube's own treatment.
    for (float y = aY + 1.5f; y < bY; y += 3.0f) { cv.rect(aX, std::floor(y), bX, std::floor(y) + 1.0f, kScan); }
    return k.n;
}

}  // namespace foxsdr::slintscope
